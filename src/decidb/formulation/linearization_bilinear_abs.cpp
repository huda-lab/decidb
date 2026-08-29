//===----------------------------------------------------------------------===//
//                         DecidB
//
// src/decidb/formulation/linearization_bilinear_abs.cpp
//
// Bilinear products and ABS lowering. See ilp_linearization.cpp.
//
//===----------------------------------------------------------------------===//
#include "duckdb/decidb/formulation/ilp_linearization.hpp"
#include "duckdb/common/exception.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include "duckdb/decidb/formulation/ilp_linearization_internal.hpp"

namespace duckdb {

using namespace decide_linearize; // NOLINT: internal DECIDE linearization helpers

//===--------------------------------------------------------------------===//
// Bilinear products and ABS
//===--------------------------------------------------------------------===//

void DeriveBilinearAuxiliaryBounds(SolverInput &input, const FormulationBox &box,
                                   const vector<string> &var_names) {
    for (const auto &link : input.bilinear_links) {
        double U = box.upper[link.other_var_idx];
        double L = box.lower[link.other_var_idx];
        if (U >= 1e20) {
            throw InvalidInputException(
                "Bilinear term requires a finite upper bound on variable '%s'. "
                "Add a constraint like '%s <= <bound>' to provide one.",
                var_names[link.other_var_idx], var_names[link.other_var_idx]);
        }
        input.lower_bounds[link.aux_idx] = std::min(0.0, L);
        input.upper_bounds[link.aux_idx] = std::max(0.0, U);
    }
}

void LinearizeBilinear(SolverInput &input, const FormulationBox &box,
                       const vector<string> &var_names) {
    const idx_t num_rows = input.num_rows;

    // For (w = b * x) with b Boolean and x in [L, U] the exact linearization is:
    //   w <= U*b                  (ec1)
    //   w >= x - U*(1-b)          (ec2)
    //   w <= x - L*(1-b)          (ec3, upper corner)
    //   w >= L*b                  (ec4, lower corner)
    // For L >= 0 the lower corner is implied by w's own non-negative bound, and ec3
    // simplifies to the plain structural `w <= x` (w=0 at b=0 is enforced by ec1).
    // We emit exactly those two-plus-one constraints in that case — stage 05 no
    // longer emits the structural `w <= x` (it lives here). For L < 0 we emit the
    // full four corners and widen w's own lower bound so the product can take the
    // negative value of x when b=1.
    for (auto &link : input.bilinear_links) {
        double U = box.upper[link.other_var_idx];
        double L = box.lower[link.other_var_idx];
        D_ASSERT(U < 1e20); // derived and validated before ABS descendants read the aux box

        // ec1: w <= U * b  (i.e., w - U*b <= 0)
        EvaluatedConstraint ec1;
        ec1.variable_indices = {link.aux_idx, link.bool_var_idx};
        ec1.row_coefficients.push_back(CoefficientColumn::MakeScalar(1.0, num_rows));
        ec1.row_coefficients.push_back(CoefficientColumn::MakeScalar(-U, num_rows));
        ec1.rhs_values.AssignScalar(num_rows, 0.0);
        ec1.comparison_type = ExpressionType::COMPARE_LESSTHANOREQUALTO;
        ec1.lhs_is_aggregate = false;
        ec1.kind = ConstraintKind::STRUCTURAL;
        ec1.source_clause_id = link.source_clause_id;
        ec1.removal_group_id = link.removal_group_id;
        input.constraints.push_back(std::move(ec1));

        // ec2: w >= x - U*(1-b) = x - U + U*b
        // Rearranged: w - x + U*b >= -U  →  1*w + (-1)*x + (-U)*b >= -U
        EvaluatedConstraint ec2;
        ec2.variable_indices = {link.aux_idx, link.other_var_idx, link.bool_var_idx};
        ec2.row_coefficients.push_back(CoefficientColumn::MakeScalar(1.0, num_rows));   // +w
        ec2.row_coefficients.push_back(CoefficientColumn::MakeScalar(-1.0, num_rows));  // -x
        ec2.row_coefficients.push_back(CoefficientColumn::MakeScalar(-U, num_rows));    // -U*b
        ec2.rhs_values.AssignScalar(num_rows, -U);
        ec2.comparison_type = ExpressionType::COMPARE_GREATERTHANOREQUALTO;
        ec2.lhs_is_aggregate = false;
        ec2.kind = ConstraintKind::STRUCTURAL;
        ec2.source_clause_id = link.source_clause_id;
        ec2.removal_group_id = link.removal_group_id;
        input.constraints.push_back(std::move(ec2));

        // ec3: upper corner. L >= 0 → plain `w <= x`; L < 0 → `w <= x - L*(1-b)`,
        // i.e. w - x - L*b <= -L.
        EvaluatedConstraint ec3;
        if (L < 0.0) {
            ec3.variable_indices = {link.aux_idx, link.other_var_idx, link.bool_var_idx};
            ec3.row_coefficients.push_back(CoefficientColumn::MakeScalar(1.0, num_rows));   // +w
            ec3.row_coefficients.push_back(CoefficientColumn::MakeScalar(-1.0, num_rows));  // -x
            ec3.row_coefficients.push_back(CoefficientColumn::MakeScalar(-L, num_rows));    // -L*b
            ec3.rhs_values.AssignScalar(num_rows, -L);
        } else {
            ec3.variable_indices = {link.aux_idx, link.other_var_idx};
            ec3.row_coefficients.push_back(CoefficientColumn::MakeScalar(1.0, num_rows));   // +w
            ec3.row_coefficients.push_back(CoefficientColumn::MakeScalar(-1.0, num_rows));  // -x
            ec3.rhs_values.AssignScalar(num_rows, 0.0);
        }
        ec3.comparison_type = ExpressionType::COMPARE_LESSTHANOREQUALTO;
        ec3.lhs_is_aggregate = false;
        ec3.kind = ConstraintKind::STRUCTURAL;
        ec3.source_clause_id = link.source_clause_id;
        ec3.removal_group_id = link.removal_group_id;
        input.constraints.push_back(std::move(ec3));

        // ec4: lower corner `w >= L*b`, only needed when x can be negative. Also
        // widen the aux's own lower bound so w may equal the negative x at b=1.
        if (L < 0.0) {
            // A write to the box, not a read of it: this DECLARES the auxiliary's column
            // (w must be able to hold a negative x), so it belongs on `input` rather than
            // on the derived-constant box above. The two coincide today because `box` is
            // the solved model's own box; they stop coinciding as soon as a second
            // formulation runs against a widened box, and this line must still land on
            // the column the model declares.
            input.lower_bounds[link.aux_idx] = std::min(input.lower_bounds[link.aux_idx], L);
            EvaluatedConstraint ec4;
            ec4.variable_indices = {link.aux_idx, link.bool_var_idx};
            ec4.row_coefficients.push_back(CoefficientColumn::MakeScalar(1.0, num_rows));   // +w
            ec4.row_coefficients.push_back(CoefficientColumn::MakeScalar(-L, num_rows));    // -L*b
            ec4.rhs_values.AssignScalar(num_rows, 0.0);
            ec4.comparison_type = ExpressionType::COMPARE_GREATERTHANOREQUALTO;
            ec4.lhs_is_aggregate = false;
            ec4.kind = ConstraintKind::STRUCTURAL;
            ec4.source_clause_id = link.source_clause_id;
            ec4.removal_group_id = link.removal_group_id;
            input.constraints.push_back(std::move(ec4));
        }
    }
}

namespace {

//! Index of the two tagged lower-bound rows an ABS auxiliary was given by stage 05:
//! C1 (`aux >= inner`, ABS_UB_POS) and C2 (`aux >= -inner`, ABS_UB_NEG). Both phases
//! below start from this map, keyed by the link's AUXILIARY — the one index both the
//! lowering and the native arm have, since only the lowering arm allocates a sign
//! indicator.
struct AbsConstraintPair {
    idx_t c1 = DConstants::INVALID_INDEX;
    idx_t c2 = DConstants::INVALID_INDEX;
};

unordered_map<idx_t, AbsConstraintPair> BuildAbsTagMap(const SolverInput &input) {
    unordered_map<idx_t, AbsConstraintPair> abs_tag_map;
    for (idx_t ci = 0; ci < input.constraints.size(); ci++) {
        auto &ec = input.constraints[ci];
        if (ec.abs_aux_idx == DConstants::INVALID_INDEX) {
            continue;
        }
        if (ec.abs_is_pos_bound) {
            abs_tag_map[ec.abs_aux_idx].c1 = ci;
        } else {
            abs_tag_map[ec.abs_aux_idx].c2 = ci;
        }
    }
    return abs_tag_map;
}

//! The largest value some clause requires `aux` to reach, or 0 when nothing does.
//!
//! Every Big-M and every derived column ceiling in this file is sized from the decision
//! box as the query states it, and that is right for the solve. It is wrong for the
//! diagnosis that follows an infeasible one: the elastic engine repairs an infeasible
//! query by WIDENING a bound, and a ceiling baked in at the old width makes the widened
//! repair unrepresentable — so the engine reports a different, worse edit. It also
//! reports a DIFFERENT edit per backend, because a natively-stated construct bakes in
//! nothing (`x >= -1 AND x <= 1 AND ABS(x) >= 5` was advised to widen the box on Gurobi
//! and to weaken the ABS on HiGHS, one repair worth five times the other).
//!
//! So a clause that asks the auxiliary for more than the box can supply sizes the
//! ceiling itself. Note when that fires: only when the demand EXCEEDS the box, which is
//! exactly when the clause cannot be met as written. A query that solves reaches its own
//! bound by definition, so its ceiling and its Big-M are untouched and no tightness is
//! traded away. A looser M is valid in any case — it only ever slackens the deactivated
//! arm of a disjunction — whereas a too-small one cuts off legal answers.
static double DemandedAuxReach(const SolverInput &input, idx_t aux_idx, idx_t num_rows) {
    double demanded = 0.0;
    // The common shape, and the one B3 was reported against: `ABS(x) >= 5` is a simple
    // `var OP const` comparison over the auxiliary, so stage 05 absorbs it into the
    // auxiliary's own box rather than leaving a row (`AbsorbVariableBounds`). The floor
    // it puts there IS the demand, and without this the ceiling below is set under it —
    // an empty column, which no widening of `x` can repair.
    if (aux_idx < input.lower_bounds.size() && input.lower_bounds[aux_idx] > -1e20) {
        demanded = MaxValue<double>(demanded, input.lower_bounds[aux_idx]);
    }
    for (const auto &ec : input.constraints) {
        if (ec.abs_aux_idx == aux_idx) {
            continue; // the definitional pair: it states what the auxiliary IS, not what was asked of it
        }
        idx_t term = DConstants::INVALID_INDEX;
        for (idx_t t = 0; t < ec.variable_indices.size(); t++) {
            if (ec.variable_indices[t] == aux_idx) {
                term = t;
                break;
            }
        }
        if (term == DConstants::INVALID_INDEX) {
            continue;
        }
        bool pushes_up = ec.comparison_type == ExpressionType::COMPARE_GREATERTHANOREQUALTO ||
                         ec.comparison_type == ExpressionType::COMPARE_GREATERTHAN ||
                         ec.comparison_type == ExpressionType::COMPARE_EQUAL;
        bool pushes_down = ec.comparison_type == ExpressionType::COMPARE_LESSTHANOREQUALTO ||
                           ec.comparison_type == ExpressionType::COMPARE_LESSTHAN ||
                           ec.comparison_type == ExpressionType::COMPARE_EQUAL;
        for (idx_t r = 0; r < num_rows; r++) {
            if (!ec.row_group_ids.empty() && ec.row_group_ids[r] == DConstants::INVALID_INDEX) {
                continue; // row excluded by WHEN/PER: this clause asks nothing here
            }
            double a = ec.row_coefficients[term].Get(r);
            if (std::abs(a) < 1e-15) {
                continue;
            }
            double k = DecideRowEffectiveBound(ec, r);
            if (!std::isfinite(k)) {
                continue;
            }
            // The row reads `a*aux + S <op> k` with `S` the rest of its variable terms.
            // The auxiliary has to cover the worst `S` the box allows, and only an
            // UPWARD demand matters here: this raises a ceiling, and an ABS auxiliary
            // is non-negative and floored by its own definitional pair.
            double lo = 0.0;
            double hi = 0.0;
            DecideRowSignedRange(ec, r, input.lower_bounds, input.upper_bounds, lo, hi, aux_idx);
            if (a > 0.0 && pushes_up && std::isfinite(lo)) {
                demanded = MaxValue<double>(demanded, (k - lo) / a);
            } else if (a < 0.0 && pushes_down && std::isfinite(hi)) {
                demanded = MaxValue<double>(demanded, (k - hi) / a);
            }
        }
    }
    return demanded;
}

} // namespace

void DeriveAbsAuxiliaryBounds(SolverInput &input, const vector<string> &var_names,
                              bool refuse_when_unbounded) {
    if (input.abs_maximize_links.empty()) {
        return;
    }
    const idx_t num_rows = input.num_rows;
    auto abs_tag_map = BuildAbsTagMap(input);

    for (auto &link : input.abs_maximize_links) {
        auto it = abs_tag_map.find(link.aux_idx);
        D_ASSERT(it != abs_tag_map.end() && it->second.c1 != DConstants::INVALID_INDEX &&
                 it->second.c2 != DConstants::INVALID_INDEX);
        const auto &c1 = input.constraints[it->second.c1];

        // M = max over rows of |rhs[r]| + sum_{t: var != aux} |coeff[t][r]| * max(|lb|, |ub|),
        // reusing the shared per-row range helper (skipping the aux term). This
        // upper-bounds |inner| across all rows and variable values.
        bool abs_unbounded = false;
        double M = 0.0;
        for (idx_t r = 0; r < num_rows; r++) {
            // The link's bound carries the constant part of the expression the user
            // wrote inside ABS. An infinity there is a data problem, not a bounding
            // one, and it defeats the native path too — |inf| is still inf — so it is
            // refused whichever path follows.
            double link_rhs = c1.rhs_values.Get(r);
            if (!std::isfinite(link_rhs)) {
                throw InvalidInputException(
                    "The expression inside ABS() evaluates to Infinity at row %llu, "
                    "so DECIDE cannot bound it. Check that column for an overflow, "
                    "or exclude the row with WHERE.",
                    r);
            }
            double row_bound = std::abs(link_rhs) +
                               DecideRowTermRange(c1.variable_indices, c1.row_coefficients, r,
                                                  input.lower_bounds, input.upper_bounds,
                                                  abs_unbounded, link.aux_idx);
            M = std::max(M, row_bound);
        }

        if (abs_unbounded) {
            link.range_unbounded = true;
            if (!refuse_when_unbounded) {
                // The native path is about to hand Gurobi `aux = |t|`, which needs no
                // Big-M and therefore no bound. Leave the auxiliary's box open and
                // carry on: this is exactly the query the capability buys back.
                continue;
            }
            // Locate an offending variable to name in the error.
            idx_t bad = DConstants::INVALID_INDEX;
            for (idx_t t = 0; t < c1.variable_indices.size(); t++) {
                idx_t v = c1.variable_indices[t];
                if (v == DConstants::INVALID_INDEX || v == link.aux_idx) {
                    continue;
                }
                if (input.upper_bounds[v] >= 1e20 || input.lower_bounds[v] <= -1e20) {
                    bad = v;
                    break;
                }
            }
            const string &name = var_names[bad];
            throw InvalidInputException(
                "ABS over decision variable requires a finite bound on '%s' "
                "for the Big-M sign-indicator linearization. Add constraints "
                "'%s >= <lower>' and '%s <= <upper>'. (Triggered by "
                "MAXIMIZE SUM(ABS(...)) or by a hard-direction ABS constraint "
                "such as ABS(...) >= K or ABS(...) = K.)",
                name, name, name);
        }

        // A clause may demand more of the auxiliary than the box can supply, and then
        // the box is not the reach the encoding has to cover: `ABS(x) >= 5` over
        // `x` in [-1, 1] needs `aux` to be ABLE to hold 5, or the only repair the
        // infeasible diagnosis can see is weakening the ABS itself. See
        // `DemandedAuxReach` — this can only widen, and only on a clause that cannot
        // be met as written.
        M = MaxValue<double>(M, DemandedAuxReach(input, link.aux_idx, num_rows));

        link.abs_range = M;
        // The formulation that follows — Big-M rows or a native `aux = |t|` — pins
        // `aux = |inner|`, and M is the largest |inner| any row can reach. So M is a
        // valid upper bound on the auxiliary's column, and the only one anything
        // derives: the pair `aux >= inner`, `aux >= -inner` bounds the auxiliary from
        // BELOW only, which is why it otherwise sits at +infinity. Narrow the box
        // here, before any other linearizer reads it, so an outer MIN/MAX or `<>` over
        // this auxiliary gets a tight Big-M instead of an unbounded column it must
        // refuse. Never widens: an implied bound already tighter than M stands.
        if (M < input.upper_bounds[link.aux_idx]) {
            input.upper_bounds[link.aux_idx] = M;
        }
    }
}

void LinearizeAbsMaximize(SolverInput &input) {
    if (input.abs_maximize_links.empty()) {
        return;
    }
    const idx_t num_rows = input.num_rows;

    // For each link, find the two tagged lower-bound EvaluatedConstraints
    // (C1: aux >= inner tagged ABS_UB_POS, C2: aux >= -inner tagged ABS_UB_NEG) and emit:
    //   C_ub1: derived from C1, add y with coeff +2M, comparison <=, rhs[r] += 2M
    //   C_ub2: derived from C2, add y with coeff -2M, comparison <=, rhs unchanged
    // Together with C1/C2 these force aux = |inner| under MAXIMIZE.
    auto abs_tag_map = BuildAbsTagMap(input);

    // Reserve up-front so the two push_back calls per link cannot reallocate
    // input.constraints. With capacity guaranteed, references to existing C1/C2 stay
    // valid across appends and we don't need defensive copies of their fields.
    input.constraints.reserve(input.constraints.size() + 2 * input.abs_maximize_links.size());

    for (auto &link : input.abs_maximize_links) {
        auto it = abs_tag_map.find(link.aux_idx);
        D_ASSERT(it != abs_tag_map.end() &&
                 it->second.c1 != DConstants::INVALID_INDEX &&
                 it->second.c2 != DConstants::INVALID_INDEX);
        // This arm is the only one that switches the envelope with a binary, so it is
        // the only one stage 05 allocates one for. Reaching here without it would mean
        // the gate routed a lowering the plan had already decided against.
        D_ASSERT(link.y_idx != DConstants::INVALID_INDEX);

        const auto &c1 = input.constraints[it->second.c1];
        const auto &c2 = input.constraints[it->second.c2];

        // M was derived — and the auxiliary's box narrowed to it — by
        // DeriveAbsAuxiliaryBounds, which runs before every linearizer because they all
        // read column boxes. An unbounded range was refused there; reaching here with
        // one would mean the gate routed a lowering it had already declined.
        D_ASSERT(!link.range_unbounded);
        double two_M = 2.0 * link.abs_range;

        auto ShiftRhs = [&](const CoefficientColumn &src, double delta) {
            if (src.IsUniform()) {
                return CoefficientColumn::MakeScalar(src.UniformValue() + delta, num_rows);
            }
            auto out = CoefficientColumn::MakeDense(num_rows, 0.0);
            for (idx_t r = 0; r < num_rows; r++) {
                out.Set(r, src.Get(r) + delta);
            }
            return out;
        };

        // C_ub1: same as C1 but add y_idx with coeff +2M, flip to <=, rhs[r] += 2M
        EvaluatedConstraint ec_ub1;
        ec_ub1.variable_indices = c1.variable_indices;
        ec_ub1.row_coefficients = c1.row_coefficients;
        ec_ub1.variable_indices.push_back(link.y_idx);
        ec_ub1.row_coefficients.push_back(CoefficientColumn::MakeScalar(two_M, num_rows));
        ec_ub1.rhs_values = ShiftRhs(c1.rhs_values, two_M);
        ec_ub1.comparison_type = ExpressionType::COMPARE_LESSTHANOREQUALTO;
        ec_ub1.lhs_is_aggregate = false;
        ec_ub1.row_group_ids = c1.row_group_ids;
        ec_ub1.num_groups = c1.num_groups;
        ec_ub1.group_labels = c1.group_labels;
        ec_ub1.qualifier = c1.qualifier;
        ec_ub1.kind = ConstraintKind::STRUCTURAL;
        ec_ub1.source_clause_id = c1.source_clause_id;
        ec_ub1.repair_group_id = c1.repair_group_id;
        ec_ub1.removal_group_id = c1.removal_group_id;
        input.constraints.push_back(std::move(ec_ub1));

        // C_ub2: same as C2 but add y_idx with coeff -2M, flip to <=, rhs unchanged
        EvaluatedConstraint ec_ub2;
        ec_ub2.variable_indices = c2.variable_indices;
        ec_ub2.row_coefficients = c2.row_coefficients;
        ec_ub2.variable_indices.push_back(link.y_idx);
        ec_ub2.row_coefficients.push_back(CoefficientColumn::MakeScalar(-two_M, num_rows));
        ec_ub2.rhs_values = c2.rhs_values;
        ec_ub2.comparison_type = ExpressionType::COMPARE_LESSTHANOREQUALTO;
        ec_ub2.lhs_is_aggregate = false;
        ec_ub2.row_group_ids = c2.row_group_ids;
        ec_ub2.num_groups = c2.num_groups;
        ec_ub2.group_labels = c2.group_labels;
        ec_ub2.qualifier = c2.qualifier;
        ec_ub2.kind = ConstraintKind::STRUCTURAL;
        ec_ub2.source_clause_id = c2.source_clause_id;
        ec_ub2.repair_group_id = c2.repair_group_id;
        ec_ub2.removal_group_id = c2.removal_group_id;
        input.constraints.push_back(std::move(ec_ub2));
    }
}

void EmitNativeAbs(SolverInput &input, const VarIndexer &indexer) {
    if (input.abs_maximize_links.empty()) {
        return;
    }
    const idx_t num_rows = input.num_rows;
    auto abs_tag_map = BuildAbsTagMap(input);

    for (auto &link : input.abs_maximize_links) {
        auto it = abs_tag_map.find(link.aux_idx);
        D_ASSERT(it != abs_tag_map.end() && it->second.c1 != DConstants::INVALID_INDEX);
        const auto &c1 = input.constraints[it->second.c1];

        // C1 is `aux + sum_t c_t x_t >= k`, the row spelling of `aux >= inner` with
        // inner's constant part moved to the bound. So the expression inside ABS is
        //   inner_r = k_r - sum_{t != aux} c_t[r] * x_t
        // and the argument column below is pinned to exactly that.
        //
        // The general constraint relates two VARIABLES, so the linear half has to be a
        // row either way. That is the whole shape of a native construct here: one free
        // column `t`, one equality row `t = inner`, and one `aux = |t|` record. It
        // replaces the two Big-M rows and the binary sign indicator — which is the
        // point, since those are what needed a finite M.
        //
        // C1 and C2 stay. They are exact (`aux >= inner`, `aux >= -inner`), implied by
        // `aux = |t|`, and they carry the clause provenance the elastic engine reads.
        // Dropping them would be a separate optimization that costs diagnosis.
        for (idx_t r = 0; r < num_rows; r++) {
            if (!c1.row_group_ids.empty() && c1.row_group_ids[r] == DConstants::INVALID_INDEX) {
                continue; // row excluded by WHEN/PER: no ABS to express here
            }
            // `t = inner`, boxed by THIS ROW's own reach — the same per-row walk native
            // MIN/MAX boxes its argument columns with, rather than one table-wide
            // magnitude reused for every row. The wider box is not free: a slack
            // continuous column costs the root LP relaxation (see the note at the top of
            // this file), and `abs_range` is the MAXIMUM over rows, so every row but the
            // extreme one was being given more room than it can reach.
            //
            // The bracket is read off C1 exactly as the link row below states it. C1 is
            // `aux + sum_{v != aux} c_v x_v >= k`, so the column is
            //   inner_r = k_r - sum_{v != aux} c_v x_v
            // and negating the variable walk swaps its two ends. Skipping the auxiliary
            // is what makes this the range of the expression rather than of the row.
            double vars_lo = 0.0;
            double vars_hi = 0.0;
            DecideRowSignedRange(c1, r, input.lower_bounds, input.upper_bounds, vars_lo, vars_hi,
                                 link.aux_idx);
            double k = c1.rhs_values.Get(r);
            // Each end on its own. A row open on one side still contributes its closed
            // side, and a row whose contributors are all bounded gets a real box even
            // when some other row's are not — which `abs_range` could not express, being
            // one number for the whole link. Free only where nothing at all is derivable,
            // which is precisely the query this arm exists to answer and the only case
            // that earns a free column. The bracket carries the constant, as `k` here.
            AuxRange inner_range;
            inner_range.CoverRowSided(k - vars_hi, k - vars_lo, DConstants::INVALID_INDEX);
            // `ABS(x)` is the common shape, and there `t = x`: one column and one
            // equality row per data row to restate a column that already exists. C1
            // spells the argument as `inner = k - sum c_t x_t`, so a renaming is one
            // non-auxiliary term with coefficient -1 and a zero bound. Anything else —
            // `ABS(x - target)`, `ABS(2 * x)` — is a genuine expression, and a general
            // constraint relates columns, so it still earns one.
            idx_t sole_var = DConstants::INVALID_INDEX;
            double sole_coef = 0.0;
            idx_t term_count = 0;
            for (idx_t t = 0; t < c1.variable_indices.size(); t++) {
                idx_t v = c1.variable_indices[t];
                if (v == DConstants::INVALID_INDEX || v == link.aux_idx) {
                    continue;
                }
                double coef = c1.row_coefficients[t].Get(r);
                if (coef == 0.0) {
                    continue;
                }
                term_count++;
                sole_var = v;
                sole_coef = coef;
            }
            bool renaming = (term_count == 1 && sole_coef == -1.0 && k == 0.0);

            idx_t t_idx = DConstants::INVALID_INDEX;
            if (renaming) {
                t_idx = indexer.Get(sole_var, r);
            } else {
                t_idx = AddGlobalContinuousAux(input, indexer, inner_range, 0.0);

                SolverInput::RawConstraint link_row;
                link_row.indices.push_back(static_cast<int>(t_idx));
                link_row.coefficients.push_back(1.0);
                for (idx_t t = 0; t < c1.variable_indices.size(); t++) {
                    idx_t v = c1.variable_indices[t];
                    if (v == DConstants::INVALID_INDEX || v == link.aux_idx) {
                        continue;
                    }
                    double coef = c1.row_coefficients[t].Get(r);
                    if (coef == 0.0) {
                        continue;
                    }
                    // `t - inner = 0` with `inner = k - sum c_t x_t` is `t + sum c_t x_t = k`.
                    link_row.indices.push_back(static_cast<int>(indexer.Get(v, r)));
                    link_row.coefficients.push_back(coef);
                }
                link_row.sense = '=';
                link_row.rhs = c1.rhs_values.Get(r);
                link_row.kind = ConstraintKind::STRUCTURAL;
                link_row.source_clause_id = c1.source_clause_id;
                link_row.repair_group_id = c1.repair_group_id;
                link_row.removal_group_id = c1.removal_group_id;
                input.global_constraints.push_back(std::move(link_row));
            }

            GeneralConstraintSpec gc;
            gc.kind = GeneralConstraintKind::ABS;
            gc.result_column = static_cast<int>(indexer.Get(link.aux_idx, r));
            gc.argument_columns.push_back(static_cast<int>(t_idx));
            gc.source_clause_id = c1.source_clause_id;
            gc.repair_group_id = c1.repair_group_id;
            gc.removal_group_id = c1.removal_group_id;
            input.general_constraints.push_back(std::move(gc));
        }
    }
}

// --- MIN/MAX objective ------------------------------------------------------
// The objective counterpart of the MIN/MAX constraint encoding above. Both build
// the same envelope + indicator shape; they differ in what the auxiliary is pinned
// against (a user bound there, the objective expression here) and in where the rows
// land (`input.constraints` there, `input.global_constraints` here, because the
// auxiliaries live in the flat global block).

} // namespace duckdb
