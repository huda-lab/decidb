#include "duckdb/decidb/ilp_linearization.hpp"
#include "duckdb/common/exception.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace duckdb {

// --- Data-driven Big-M support ---------------------------------------------
// A Big-M linearization toggles a constraint on/off via a binary indicator. The
// constant M must be at least the reachable magnitude of the constraint's
// left-hand expression: too small silently distorts the feasible region (wrong
// answer), too large degrades numerical stability. We therefore derive M from
// the actual variable bounds and per-row coefficient data instead of a fixed
// constant, mirroring the long-standing ABS-maximize path.

double DecideRowTermRange(const vector<idx_t> &variable_indices,
                          const vector<CoefficientColumn> &row_coefficients,
                          idx_t row, const vector<double> &lower_bounds,
                          const vector<double> &upper_bounds,
                          bool &has_unbounded,
                          idx_t skip_idx) {
    double sum = 0.0;
    for (idx_t t = 0; t < variable_indices.size(); t++) {
        idx_t v = variable_indices[t];
        if (v == DConstants::INVALID_INDEX || v == skip_idx) {
            continue;
        }
        double coef = std::abs(row_coefficients[t].Get(row));
        if (coef < 1e-15) {
            continue;
        }
        double lb = lower_bounds[v];
        double ub = upper_bounds[v];
        if (ub >= 1e20 || lb <= -1e20) {
            has_unbounded = true;
            continue;
        }
        sum += coef * std::max(std::abs(lb), std::abs(ub));
    }
    return sum;
}

static double DecideRowFixedLhsOffset(const vector<idx_t> &variable_indices,
                                      const vector<CoefficientColumn> &row_coefficients,
                                      idx_t row) {
    double offset = 0.0;
    for (idx_t t = 0; t < variable_indices.size(); t++) {
        if (variable_indices[t] == DConstants::INVALID_INDEX) {
            offset += row_coefficients[t].Get(row);
        }
    }
    return offset;
}

//! The bound a row actually imposes: its right-hand side less the constant part of
//! its left-hand side. This is the value a Big-M has to dominate, so it is also the
//! value a caller must classify before asking for an M over it.
static double DecideRowEffectiveBound(const EvaluatedConstraint &ec, idx_t row) {
    double rhs = ec.rhs_values.IsUniform() ? ec.rhs_values.UniformValue() : ec.rhs_values.Get(row);
    return rhs - DecideRowFixedLhsOffset(ec.variable_indices, ec.row_coefficients, row);
}

double DecideTightPerRowBigM(const EvaluatedConstraint &ec,
                             const vector<double> &lower_bounds,
                             const vector<double> &upper_bounds,
                             idx_t num_rows) {
    bool has_unbounded = false;
    double M = 0.0;
    for (idx_t r = 0; r < num_rows; r++) {
        if (!ec.row_group_ids.empty() &&
            ec.row_group_ids[r] == DConstants::INVALID_INDEX) {
            continue;
        }
        double rhs = DecideRowEffectiveBound(ec, r);
        // An infinite bound reaches the model unchanged everywhere else — it is
        // simply "no constraint" or "no solution" — but it cannot be linearized:
        // M has to dominate the row's slack, and nothing finite dominates
        // infinity. Refuse it here, in SQL terms, rather than let a non-finite
        // coefficient reach the model validator as an internal error. Callers that
        // can read an infinity by direction (`<>`, MIN/MAX) classify it before
        // getting here, so what still arrives is a bound with no reading at all.
        if (!std::isfinite(rhs)) {
            throw InvalidInputException(
                "DECIDE cannot rewrite this constraint: its bound is %s at "
                "row %llu, and rewriting ABS, MIN, MAX or <> needs a finite bound. "
                "Compare against a finite value instead.",
                std::isnan(rhs) ? "NaN" : "Infinity", r);
        }
        double rhs_mag = std::abs(rhs);
        double range = rhs_mag + DecideRowTermRange(ec.variable_indices, ec.row_coefficients,
                                                    r, lower_bounds, upper_bounds, has_unbounded);
        M = std::max(M, range);
    }
    M += 1.0;
    if (has_unbounded) {
        M = std::max(M, DECIDE_BIGM_FALLBACK);
    }
    return M;
}

//! What an infinite bound means for a hard MIN/MAX constraint.
enum class MinMaxBoundKind : uint8_t {
    LINEARIZE,  //!< finite bound: needs the indicator + Big-M rewrite
    VACUOUS,    //!< every assignment satisfies it: the constraint can be dropped
    NO_SOLUTION //!< no assignment satisfies it: report it as an infeasibility
};

//! A hard MAX reads "some active row has LHS >= K" and a hard MIN "some active row
//! has LHS <= K", so an infinite K is answered by the direction it points, not by
//! the rewrite's ability to encode it: `MAX(x) >= -inf` and `MIN(x) <= +inf` hold
//! for every assignment, `MAX(x) >= +inf` and `MIN(x) <= -inf` for none. Classifying
//! first is what keeps a constraint that constrains nothing from being refused for
//! lacking a finite M — the same order the `<>` rewrite uses. NaN is neither case
//! and stays on the finite path, where the Big-M guard still names it.
static MinMaxBoundKind ClassifyMinMaxBound(double bound, bool is_max_agg) {
    if (!std::isinf(bound)) {
        return MinMaxBoundKind::LINEARIZE;
    }
    bool out_of_reach = (bound > 0.0) == is_max_agg;
    return out_of_reach ? MinMaxBoundKind::NO_SOLUTION : MinMaxBoundKind::VACUOUS;
}

void DecidePropagateImpliedBounds(const vector<EvaluatedConstraint> &constraints,
                                  vector<double> &lower_bounds,
                                  vector<double> &upper_bounds, idx_t num_rows) {
    for (auto &ec : constraints) {
        if (ec.comparison_type != ExpressionType::COMPARE_LESSTHANOREQUALTO &&
            ec.comparison_type != ExpressionType::COMPARE_EQUAL) {
            continue;
        }
        if (!ec.bilinear_terms.empty() || ec.has_quadratic) {
            continue;
        }
        if (ec.minmax_indicator_idx != DConstants::INVALID_INDEX ||
            ec.ne_indicator_idx != DConstants::INVALID_INDEX ||
            ec.abs_y_idx != DConstants::INVALID_INDEX) {
            continue;
        }
        // WHEN-conditional constraints apply to only a subset of rows, but the
        // bound we derive is shared across ALL of a variable's rows. Deriving it
        // from the active subset would wrongly bound the excluded (WHEN-false)
        // rows, which carry no such constraint. Skip any constraint that has
        // excluded rows.
        bool has_excluded = false;
        for (idx_t gid : ec.row_group_ids) {
            if (gid == DConstants::INVALID_INDEX) {
                has_excluded = true;
                break;
            }
        }
        if (has_excluded) {
            continue;
        }
        // Soundness requires every term to be non-negative — both the variable
        // lower bounds (x >= 0) AND the coefficients (a >= 0) — so that dropping
        // the other terms to derive x_t <= K/a_t is valid. Constraints with any
        // negative coefficient (e.g. the IN/ABS rewrites such as x - z1 - 3*z2 = 0)
        // must be skipped: dropping a negative term would wrongly tighten the
        // bound and cut feasible points (or make the model infeasible).
        bool nonneg = true;
        for (idx_t v : ec.variable_indices) {
            if (v != DConstants::INVALID_INDEX && lower_bounds[v] < 0.0) {
                nonneg = false;
                break;
            }
        }
        for (idx_t t = 0; nonneg && t < ec.row_coefficients.size(); t++) {
            const auto &col = ec.row_coefficients[t];
            if (col.IsUniform()) {
                // Scalar column: one value covers every row.
                if (col.UniformValue() < -1e-15) {
                    nonneg = false;
                }
                continue;
            }
            for (idx_t r = 0; r < num_rows; r++) {
                if (!ec.row_group_ids.empty() &&
                    ec.row_group_ids[r] == DConstants::INVALID_INDEX) {
                    continue;
                }
                if (col.Get(r) < -1e-15) {
                    nonneg = false;
                    break;
                }
            }
        }
        if (!nonneg) {
            continue;
        }
        bool uniform_rhs = ec.rhs_values.IsUniform();
        for (idx_t t = 0; t < ec.variable_indices.size(); t++) {
            idx_t v = ec.variable_indices[t];
            if (v == DConstants::INVALID_INDEX) {
                continue;
            }
            // A variable can occupy several additive terms of the SAME constraint
            // (`2*ship + 3*ship <= 10`, or two reducers over the same decision), and
            // every one of them names the same solver column. The implied bound
            // follows from their combined coefficient (10/5 = 2), never from one
            // term's alone (10/3 = 3.33, sound but 1.67x looser) — reading a single
            // term silently drops the others' contribution. So handle each variable
            // once, at its first term, and sum every term that names it. The
            // coefficients are evaluated numbers by now, which is why the addition
            // belongs here and not in canonicalization, where they are still
            // unevaluated expressions over data columns.
            bool already_handled = false;
            for (idx_t prev = 0; prev < t; prev++) {
                if (ec.variable_indices[prev] == v) {
                    already_handled = true;
                    break;
                }
            }
            if (already_handled) {
                continue;
            }
            double bound = 0.0;
            // The shared bound ub_x applies to EVERY row instance of x. A row
            // bounds x only if it is active (not WHEN-excluded) AND x has a
            // non-zero coefficient there. WHEN-excluded rows show up as a zero
            // coefficient (not always as a row_group_ids marker), so if ANY row
            // leaves x_r unconstrained we cannot use this constraint to bound the
            // shared variable — doing so would cap the unconstrained rows.
            // Two benign degenerate edges, both sound and intentionally left as-is:
            //  - num_rows == 0: the loop never runs, every_row_constrained stays
            //    true and bound stays 0, so ub may be pinned to 0 — harmless,
            //    because with no rows there are no instances of x to bind.
            //  - a tiny positive coefficient (a just above 1e-15): K/a is a huge
            //    but still-VALID upper bound; it merely re-inflates M and loosens
            //    the relaxation. Degenerate input, sound, not worth special-casing.
            bool every_row_constrained = true;
            for (idx_t r = 0; r < num_rows; r++) {
                bool excluded = !ec.row_group_ids.empty() &&
                                ec.row_group_ids[r] == DConstants::INVALID_INDEX;
                double a = 0.0;
                for (idx_t t2 = t; t2 < ec.variable_indices.size(); t2++) {
                    if (ec.variable_indices[t2] == v) {
                        a += ec.row_coefficients[t2].Get(r);
                    }
                }
                if (excluded || a <= 1e-15) {
                    every_row_constrained = false;
                    break;
                }
                double k = uniform_rhs ? ec.rhs_values.UniformValue() : ec.rhs_values.Get(r);
                if (k < 0.0) {
                    every_row_constrained = false;
                    break;
                }
                bound = std::max(bound, k / a);
            }
            if (every_row_constrained && bound < upper_bounds[v] && bound >= lower_bounds[v]) {
                upper_bounds[v] = bound;
            }
        }
    }
}

void LinearizeMinMaxIndicators(SolverInput &input) {
    const auto &lower_bounds = input.lower_bounds;
    const auto &upper_bounds = input.upper_bounds;
    const idx_t num_rows = input.num_rows;

    vector<EvaluatedConstraint> new_constraints;
    for (auto &ec : input.constraints) {
        // Skip constraints without a minmax indicator tag
        if (ec.minmax_indicator_idx == DConstants::INVALID_INDEX) {
            new_constraints.push_back(std::move(ec));
            continue;
        }

        idx_t indicator_idx = ec.minmax_indicator_idx;
        bool is_max_agg = (ec.minmax_agg_type == "max");

        // Classify each group's bound before asking for a Big-M over it. The RHS
        // reaching here already holds one value per group: ReduceAggregateRhsPerGroup
        // collapsed a row-varying bound to the tightest one (MAX for `>=`, MIN for
        // `<=`), which is also what settles a group that mixed finite and infinite
        // bounds — the infinity either dominates the reduction or is dominated by a
        // finite value. So a group's verdict is any of its rows' verdicts, and only
        // a linearizable group reaches DecideTightPerRowBigM.
        {
            bool has_groups = !ec.row_group_ids.empty();
            idx_t group_count = has_groups ? MaxValue<idx_t>(ec.num_groups, 1) : 1;
            vector<MinMaxBoundKind> group_kind(group_count, MinMaxBoundKind::LINEARIZE);
            vector<bool> group_active(group_count, false);
            for (idx_t r = 0; r < num_rows; r++) {
                idx_t g = has_groups ? ec.row_group_ids[r] : 0;
                if (g == DConstants::INVALID_INDEX || g >= group_count) {
                    continue;
                }
                group_kind[g] = ClassifyMinMaxBound(DecideRowEffectiveBound(ec, r), is_max_agg);
                group_active[g] = true;
            }
            bool any_vacuous = false, any_no_solution = false, any_linearize = false;
            for (idx_t g = 0; g < group_count; g++) {
                if (!group_active[g]) {
                    continue;
                }
                any_vacuous |= group_kind[g] == MinMaxBoundKind::VACUOUS;
                any_no_solution |= group_kind[g] == MinMaxBoundKind::NO_SOLUTION;
                any_linearize |= group_kind[g] == MinMaxBoundKind::LINEARIZE;
            }
            if (any_vacuous || any_no_solution) {
                // Row-level masking is how a group is excluded, so an ungrouped
                // constraint needs its (implicit) single group materialized first.
                if (!has_groups) {
                    ec.row_group_ids.assign(num_rows, 0);
                    ec.num_groups = 1;
                }
                if (any_no_solution) {
                    // With the bound out of every row's reach there is no disjunction
                    // left to encode — each row fails it on its own — so the group is
                    // emitted as a plain per-row constraint carrying that bound. That
                    // is what every constraint with an unreachable bound already does,
                    // and the solver reports it as an infeasibility naming this clause
                    // rather than a rewrite refusing the query.
                    EvaluatedConstraint ec_no_solution = ec;
                    ec_no_solution.minmax_indicator_idx = DConstants::INVALID_INDEX;
                    ec_no_solution.lhs_is_aggregate = false;
                    for (idx_t r = 0; r < num_rows; r++) {
                        idx_t g = ec_no_solution.row_group_ids[r];
                        if (g != DConstants::INVALID_INDEX &&
                            group_kind[g] != MinMaxBoundKind::NO_SOLUTION) {
                            ec_no_solution.row_group_ids[r] = DConstants::INVALID_INDEX;
                        }
                    }
                    new_constraints.push_back(std::move(ec_no_solution));
                }
                // Drop every group the rewrite has no work for: a vacuous one
                // constrains nothing, and an unsatisfiable one was just emitted.
                for (idx_t r = 0; r < num_rows; r++) {
                    idx_t g = ec.row_group_ids[r];
                    if (g != DConstants::INVALID_INDEX &&
                        group_kind[g] != MinMaxBoundKind::LINEARIZE) {
                        ec.row_group_ids[r] = DConstants::INVALID_INDEX;
                    }
                }
                if (!any_linearize) {
                    continue; // no group left to rewrite
                }
            }
        }

        // Compute Big-M from variable bounds. Skip constant LHS terms
        // (var_idx == INVALID_INDEX) — they have no associated variable
        // bound; their contribution will be folded into the RHS by the
        // per-row constraint emitter.
        double M = DecideTightPerRowBigM(ec, lower_bounds, upper_bounds, num_rows);

        auto BuildShiftedRhs = [&](double shift) {
            if (ec.rhs_values.IsUniform()) {
                return CoefficientColumn::MakeScalar(ec.rhs_values.UniformValue() + shift, num_rows);
            }
            auto col = CoefficientColumn::MakeDense(num_rows, 0.0);
            for (idx_t r = 0; r < num_rows; r++) {
                col.Set(r, ec.rhs_values.Get(r) + shift);
            }
            return col;
        };

        if (is_max_agg) {
            // Hard MAX(expr) >= K: for each row i, expr_i - M*y_i >= K - M
            // This is a per-row constraint (not aggregate)
            EvaluatedConstraint ec_row;
            ec_row.variable_indices = ec.variable_indices;
            ec_row.row_coefficients = ec.row_coefficients;
            // Add indicator variable: -M * y_i (broadcast)
            ec_row.variable_indices.push_back(indicator_idx);
            ec_row.row_coefficients.push_back(CoefficientColumn::MakeScalar(-M, num_rows));
            ec_row.rhs_values = BuildShiftedRhs(-M);
            ec_row.comparison_type = ExpressionType::COMPARE_GREATERTHANOREQUALTO;
            ec_row.lhs_is_aggregate = false; // per-row!
            ec_row.row_group_ids = ec.row_group_ids;
            ec_row.num_groups = ec.num_groups;
            ec_row.group_labels = ec.group_labels;
            ec_row.qualifier = ec.qualifier;
            ec_row.kind = ConstraintKind::USER_MECHANISM;
            new_constraints.push_back(std::move(ec_row));

            // SUM(y) >= 1 (at least one row must satisfy)
            EvaluatedConstraint ec_sum;
            ec_sum.variable_indices = {indicator_idx};
            ec_sum.row_coefficients.push_back(CoefficientColumn::MakeScalar(1.0, num_rows));
            ec_sum.rhs_values.AssignScalar(num_rows, 1.0);
            ec_sum.comparison_type = ExpressionType::COMPARE_GREATERTHANOREQUALTO;
            ec_sum.lhs_is_aggregate = true;
            ec_sum.row_group_ids = ec.row_group_ids;
            ec_sum.num_groups = ec.num_groups;
            ec_sum.group_labels = ec.group_labels;
            ec_sum.qualifier = ec.qualifier;
            ec_sum.kind = ConstraintKind::USER_MECHANISM;
            new_constraints.push_back(std::move(ec_sum));
        } else {
            // MIN(expr) <= K: for each row i, expr_i + M*y_i <= K + M
            EvaluatedConstraint ec_row;
            ec_row.variable_indices = ec.variable_indices;
            ec_row.row_coefficients = ec.row_coefficients;
            // Add indicator variable: +M * y_i (broadcast)
            ec_row.variable_indices.push_back(indicator_idx);
            ec_row.row_coefficients.push_back(CoefficientColumn::MakeScalar(M, num_rows));
            ec_row.rhs_values = BuildShiftedRhs(M);
            ec_row.comparison_type = ExpressionType::COMPARE_LESSTHANOREQUALTO;
            ec_row.lhs_is_aggregate = false;
            ec_row.row_group_ids = ec.row_group_ids;
            ec_row.num_groups = ec.num_groups;
            ec_row.group_labels = ec.group_labels;
            ec_row.qualifier = ec.qualifier;
            ec_row.kind = ConstraintKind::USER_MECHANISM;
            new_constraints.push_back(std::move(ec_row));

            // SUM(y) >= 1
            EvaluatedConstraint ec_sum;
            ec_sum.variable_indices = {indicator_idx};
            ec_sum.row_coefficients.push_back(CoefficientColumn::MakeScalar(1.0, num_rows));
            ec_sum.rhs_values.AssignScalar(num_rows, 1.0);
            ec_sum.comparison_type = ExpressionType::COMPARE_GREATERTHANOREQUALTO;
            ec_sum.lhs_is_aggregate = true;
            ec_sum.row_group_ids = ec.row_group_ids;
            ec_sum.num_groups = ec.num_groups;
            ec_sum.group_labels = ec.group_labels;
            ec_sum.qualifier = ec.qualifier;
            ec_sum.kind = ConstraintKind::USER_MECHANISM;
            new_constraints.push_back(std::move(ec_sum));
        }
    }
    input.constraints = std::move(new_constraints);
}

//===--------------------------------------------------------------------===//
// `<>` disjunctions
//===--------------------------------------------------------------------===//

//! The ±1 band is only semantically exact when the LHS is integer-valued. For a REAL
//! variable or a non-integer coefficient the band `(K-1, K+1)` wrongly excludes
//! feasible continuous points. Mirrors the strict-inequality guard in
//! `ilp_model_builder.cpp::IsEvalConstraintLhsIntegerValued`, and splits its result the
//! same way: only one of the two failures is still the user's to fix here.
//!
//! A REAL decision is knowable from the declared type and is rejected at bind time by
//! `ValidateDecideNoIntegerStepComparisonOnReal`, which names the variable and quotes
//! the clause. Seeing one arrive here means a `<>` reached the model builder without
//! passing that gate — an invariant violation, not a query error. The branch is kept
//! rather than deleted so that a future rewrite introducing a REAL auxiliary inside a
//! `<>` fails loudly instead of silently cutting the band.
//!
//! A fractional coefficient comes from evaluating a data column and is knowable only
//! now, so it stays a user-facing refusal.
enum class NELhsIntegrality : uint8_t { INTEGER, REAL_VARIABLE, FRACTIONAL_COEFFICIENT };

static NELhsIntegrality NELhsIsIntegerValued(const EvaluatedConstraint &ec,
                                             const vector<LogicalType> &variable_types) {
    for (idx_t i = 0; i < ec.variable_indices.size(); i++) {
        idx_t vi = ec.variable_indices[i];
        if (vi == DConstants::INVALID_INDEX) {
            continue;
        }
        const auto &t = variable_types[vi];
        if (t == LogicalType::DOUBLE || t == LogicalType::FLOAT) {
            return NELhsIntegrality::REAL_VARIABLE;
        }
        if (!ec.row_coefficients[i].AllIntegral()) {
            return NELhsIntegrality::FRACTIONAL_COEFFICIENT;
        }
    }
    return NELhsIntegrality::INTEGER;
}

//! Companion check on the RHS. With integer-valued LHS and a non-integer K,
//! `LHS <> K` is a tautology (no integer can equal K). The ±1 Big-M rewrite would
//! emit `LHS <= K-1 ∨ LHS >= K+1`, which on the integer lattice wrongly excludes
//! floor(K) and ceil(K) — both of which the original predicate accepted. Treat such
//! RHS values as a silent drop. An infinite K is the same case for the same reason —
//! no integer equals it — and it must drop rather than reach the Big-M constant,
//! which cannot dominate infinity.
static bool NEIsIntegerValuedRhs(double k) {
    return std::isfinite(k) && std::abs(k - std::round(k)) < 1e-9;
}

//! What a row's reachable range says about the point `<>` excludes.
//!
//! `LHS <> K` is a disjunction only when `K` sits strictly inside the range the LHS can
//! actually reach. When the range lies wholly on one side of `K`, one branch is dead and
//! the surviving branch is a plain inequality — no indicator, no Big-M, and an LP
//! relaxation that is tight instead of empty. The common `SUM(x) <> 0` over nonnegative
//! decisions is exactly this case: it is `SUM(x) >= 1`.
enum class NECollapse : uint8_t {
    DISJUNCTION, //!< K is interior: both branches live, keep the Big-M pair
    ALWAYS_TRUE, //!< K is unreachable: the row excludes nothing, drop it
    LOWER_ONLY,  //!< the range never exceeds K: `<> K` is exactly `<= K-1`
    UPPER_ONLY   //!< the range never falls below K: `<> K` is exactly `>= K+1`
};

//! Signed reachable interval of the variable part of `ec`'s LHS on `row`, as opposed to
//! `DecideRowTermRange`, which returns an unsigned magnitude because a Big-M only has to
//! dominate one. A collapse needs to know which side of `K` the range lies on, so the
//! coefficient sign has to be respected rather than taken through `abs`.
//!
//! An unbounded side yields an infinite endpoint rather than failing outright: the two
//! collapses are one-sided, and `SUM(x) <> 0` over decisions that are merely
//! non-negative — the case worth serving — has a finite floor and no ceiling at all.
//! Comparisons against an infinite endpoint simply never fire, so the classifier needs
//! no separate unbounded branch.
//!
//! `lower_bounds` / `upper_bounds` must be the rigid box: see
//! `SolverInput::rigid_lower_bounds`.
static void DecideRowSignedRange(const EvaluatedConstraint &ec, idx_t row,
                                 const vector<double> &lower_bounds,
                                 const vector<double> &upper_bounds, double &out_lo,
                                 double &out_hi) {
    constexpr double INF = std::numeric_limits<double>::infinity();
    double lo = 0.0;
    double hi = 0.0;
    bool lo_unbounded = false;
    bool hi_unbounded = false;
    for (idx_t t = 0; t < ec.variable_indices.size(); t++) {
        idx_t v = ec.variable_indices[t];
        if (v == DConstants::INVALID_INDEX) {
            continue; // constant term: accounted for by DecideRowEffectiveBound
        }
        double coef = ec.row_coefficients[t].Get(row);
        if (std::abs(coef) < 1e-15) {
            continue;
        }
        // An auxiliary introduced after the rigid box was captured has no rigid entry;
        // treat it as unbounded, which declines the collapse rather than guessing.
        if (v >= lower_bounds.size() || v >= upper_bounds.size()) {
            lo_unbounded = true;
            hi_unbounded = true;
            continue;
        }
        double lb = lower_bounds[v];
        double ub = upper_bounds[v];
        bool lb_unbounded = lb <= -1e20;
        bool ub_unbounded = ub >= 1e20;
        // The low end of a positive term comes from the variable's low end, and from its
        // high end when the coefficient flips the sense.
        bool lo_from_ub = coef < 0.0;
        if (lo_from_ub ? ub_unbounded : lb_unbounded) {
            lo_unbounded = true;
        } else {
            lo += coef * (lo_from_ub ? ub : lb);
        }
        if (lo_from_ub ? lb_unbounded : ub_unbounded) {
            hi_unbounded = true;
        } else {
            hi += coef * (lo_from_ub ? lb : ub);
        }
    }
    out_lo = lo_unbounded ? -INF : lo;
    out_hi = hi_unbounded ? INF : hi;
}

//! Classify one row of a `<>` constraint. Only called once both integrality guards have
//! passed, so the LHS and `K` are both on the integer lattice and a half-unit tolerance
//! cleanly separates "can reach K" from "cannot".
static NECollapse ClassifyNERow(const EvaluatedConstraint &ec, idx_t row,
                                const vector<double> &lower_bounds,
                                const vector<double> &upper_bounds) {
    double lo;
    double hi;
    DecideRowSignedRange(ec, row, lower_bounds, upper_bounds, lo, hi);
    double k = DecideRowEffectiveBound(ec, row);
    if (!std::isfinite(k)) {
        return NECollapse::DISJUNCTION;
    }
    if (hi < k - 0.5 || lo > k + 0.5) {
        return NECollapse::ALWAYS_TRUE;
    }
    if (lo > k - 0.5) {
        return NECollapse::UPPER_ONLY; // lo == K on the lattice: LHS >= K
    }
    if (hi < k + 0.5) {
        return NECollapse::LOWER_ONLY; // hi == K on the lattice: LHS <= K
    }
    return NECollapse::DISJUNCTION;
}

//! The collapse verdict for a whole constraint, or DISJUNCTION if its active rows do not
//! agree on one.
//!
//! Per-row `<>` shares one EvaluatedConstraint across every row, so a per-row verdict
//! would mean splitting it into up to three constraints carrying complementary row masks.
//! Rows disagree only when their bounds or their RHS vary across rows, which is not the
//! shape the collapse exists to serve — a uniform `x <> 0` or `SUM(x) <> 0` yields one
//! verdict for every row. So a mixed constraint keeps the Big-M pair, unsplit.
static NECollapse ClassifyNEConstraint(const EvaluatedConstraint &ec, idx_t num_rows,
                                       const vector<double> &lower_bounds,
                                       const vector<double> &upper_bounds) {
    // A bilinear or quadratic LHS is not captured by variable_indices alone, so its
    // reachable range is not the linear one computed above.
    if (!ec.bilinear_terms.empty() || !ec.quadratic_groups.empty()) {
        return NECollapse::DISJUNCTION;
    }
    bool seen = false;
    NECollapse verdict = NECollapse::DISJUNCTION;
    for (idx_t r = 0; r < num_rows; r++) {
        if (!ec.row_group_ids.empty() && ec.row_group_ids[r] == DConstants::INVALID_INDEX) {
            continue;
        }
        NECollapse row_verdict = ClassifyNERow(ec, r, lower_bounds, upper_bounds);
        if (!seen) {
            verdict = row_verdict;
            seen = true;
        } else if (row_verdict != verdict) {
            return NECollapse::DISJUNCTION;
        }
    }
    return seen ? verdict : NECollapse::DISJUNCTION;
}

void LinearizeNotEqual(SolverInput &input, vector<EvaluatedConstraint> &deferred_aggregate) {
    const idx_t num_rows = input.num_rows;

    vector<EvaluatedConstraint> new_constraints;
    for (auto &ec : input.constraints) {
        if (ec.ne_indicator_idx == DConstants::INVALID_INDEX) {
            new_constraints.push_back(std::move(ec));
            continue;
        }
        if (NELhsIsIntegerValued(ec, input.variable_types) != NELhsIntegrality::INTEGER) {
            throw InternalException(
                "DECIDE: a '<>' whose left-hand side is not integer-valued reached the "
                "model builder. Both halves of that refusal are stated on declared types "
                "at bind time — ValidateDecideNoIntegerStepComparisonOnReal for the "
                "decision, ValidateDecideIntegralComparisonOperands for every other "
                "operand — so arriving here is an invariant violation, not a query error.");
        }
        if (ec.lhs_is_aggregate) {
            // Aggregate NE: defer to after var_indexer is built. Expanded with a
            // single global z per group, whose Big-M comes from that group's SUMMED
            // range — a single per-row bound would be far too small. The per-group
            // integer-RHS check (for AVG <> where the rescaled K*N_g may or may not
            // be integer) lives in the deferred expansion; we don't filter here.
            deferred_aggregate.push_back(ec); // copy before the loop moves on
            continue;                         // not added to new_constraints
        }

        // Per-row NE: expand inline with the row-scoped indicator variable.
        //
        // Integer-valued RHS guard. If RHS is uniform and non-integer, every row's
        // `LHS <> K` is a tautology — drop the whole constraint. If RHS varies per
        // row (e.g. correlated subquery), mask out only the non-integer rows by
        // marking them INVALID_INDEX in row_group_ids so the model builder skips
        // them. The remaining rows still get the real Big-M pair.
        if (ec.rhs_values.IsUniform()) {
            if (!NEIsIntegerValuedRhs(ec.rhs_values.UniformValue())) {
                continue; // drop ec entirely (tautology)
            }
        } else {
            // Build/extend a mask. row_group_ids may be empty (no WHEN/PER); in that
            // case materialize one initialised to group 0 so individual rows can be
            // excluded by setting INVALID_INDEX.
            if (ec.row_group_ids.empty()) {
                ec.row_group_ids.assign(num_rows, 0);
                ec.num_groups = 1;
            }
            idx_t dropped = 0;
            for (idx_t r = 0; r < num_rows; r++) {
                if (ec.row_group_ids[r] == DConstants::INVALID_INDEX) {
                    continue;
                }
                if (!NEIsIntegerValuedRhs(ec.rhs_values.Get(r))) {
                    ec.row_group_ids[r] = DConstants::INVALID_INDEX;
                    dropped++;
                }
            }
            if (dropped == num_rows) {
                continue; // every row is a tautology — drop the constraint
            }
        }

        auto BuildShiftedRhs = [&](double shift) {
            if (ec.rhs_values.IsUniform()) {
                return CoefficientColumn::MakeScalar(ec.rhs_values.UniformValue() + shift, num_rows);
            }
            auto col = CoefficientColumn::MakeDense(num_rows, 0.0);
            for (idx_t r = 0; r < num_rows; r++) {
                col.Set(r, ec.rhs_values.Get(r) + shift);
            }
            return col;
        };

        // Range collapse, before any Big-M is computed. When the LHS cannot reach the
        // far side of K, one disjunct is dead and the constraint is a plain inequality.
        NECollapse collapse = ClassifyNEConstraint(ec, num_rows, input.rigid_lower_bounds,
                                                   input.rigid_upper_bounds);
        if (collapse == NECollapse::ALWAYS_TRUE) {
            continue; // excludes nothing reachable — drop, like the tautology case
        }
        if (collapse != NECollapse::DISJUNCTION) {
            bool lower = collapse == NECollapse::LOWER_ONLY;
            EvaluatedConstraint ec_collapsed;
            ec_collapsed.variable_indices = ec.variable_indices;
            ec_collapsed.row_coefficients = ec.row_coefficients;
            ec_collapsed.rhs_values = BuildShiftedRhs(lower ? -1.0 : 1.0);
            ec_collapsed.comparison_type = lower ? ExpressionType::COMPARE_LESSTHANOREQUALTO
                                                 : ExpressionType::COMPARE_GREATERTHANOREQUALTO;
            ec_collapsed.lhs_is_aggregate = false; // per-row
            ec_collapsed.row_group_ids = ec.row_group_ids;
            ec_collapsed.num_groups = ec.num_groups;
            ec_collapsed.group_labels = ec.group_labels;
            ec_collapsed.qualifier = ec.qualifier;
            ec_collapsed.source_clause_id = ec.source_clause_id;
            ec_collapsed.repair_group_id = ec.repair_group_id;
            ec_collapsed.kind = ConstraintKind::USER_MECHANISM;
            // Keep the `<>` provenance even though the indicator no longer appears in
            // the row: diagnosis must still offer this clause as a remove-only `<>`
            // rather than as a bound the user can nudge, whichever encoding it received.
            // The removal engine falls back to a range-derived M when it finds no
            // indicator coefficient to read one from.
            ec_collapsed.ne_indicator_idx = ec.ne_indicator_idx;
            new_constraints.push_back(std::move(ec_collapsed));
            continue;
        }

        // Tight data-driven per-row Big-M for the inline NE expansion. Computed
        // after the tautology filter: a row this rewrite never emits must not be
        // asked for an M, and an infinite bound is exactly such a row
        // (`LHS <> Infinity` always holds), so it is dropped above rather than
        // refused by the Big-M guard.
        double M = DecideTightPerRowBigM(ec, input.lower_bounds, input.upper_bounds, num_rows);
        idx_t indicator_var_idx = ec.ne_indicator_idx;

        // Build the indicator coefficient column. With no WHEN/PER filter every row
        // gets -M (broadcast scalar). Otherwise only the active rows hold -M and the
        // rest are 0 — stored as SparseMasked instead of Dense to skip the
        // per-excluded-row 0 allocation. ec.row_group_ids is iterated in row order,
        // so the resulting sparse_indices list is already sorted ascending (the
        // SparseMasked invariant).
        CoefficientColumn indicator_coeffs;
        if (ec.row_group_ids.empty()) {
            indicator_coeffs = CoefficientColumn::MakeScalar(-M, num_rows);
        } else {
            vector<idx_t> active_indices;
            active_indices.reserve(num_rows / 8);
            for (idx_t r = 0; r < num_rows; r++) {
                if (ec.row_group_ids[r] != DConstants::INVALID_INDEX) {
                    active_indices.push_back(r);
                }
            }
            indicator_coeffs = CoefficientColumn::MakeSparseMasked(
                num_rows, std::move(active_indices), -M);
        }

        // Constraint 1: x - M*z <= K - 1
        EvaluatedConstraint ec1;
        ec1.variable_indices = ec.variable_indices;
        ec1.row_coefficients = ec.row_coefficients;
        ec1.variable_indices.push_back(indicator_var_idx);
        ec1.row_coefficients.push_back(indicator_coeffs);
        ec1.rhs_values = BuildShiftedRhs(-1.0);
        ec1.comparison_type = ExpressionType::COMPARE_LESSTHANOREQUALTO;
        ec1.lhs_is_aggregate = false; // per-row
        ec1.row_group_ids = ec.row_group_ids;
        ec1.num_groups = ec.num_groups;
        ec1.group_labels = ec.group_labels;
        ec1.qualifier = ec.qualifier;
        ec1.kind = ConstraintKind::USER_MECHANISM;
        // I4: tag this disjunction row with its indicator so the elastic engine can
        // group the pair and offer removal (remove-only `<>`).
        ec1.ne_indicator_idx = indicator_var_idx;
        new_constraints.push_back(std::move(ec1));

        // Constraint 2: x - M*z >= K + 1 - M
        EvaluatedConstraint ec2;
        ec2.variable_indices = ec.variable_indices;
        ec2.row_coefficients = ec.row_coefficients;
        ec2.variable_indices.push_back(indicator_var_idx);
        ec2.row_coefficients.push_back(std::move(indicator_coeffs));
        ec2.rhs_values = BuildShiftedRhs(1.0 - M);
        ec2.comparison_type = ExpressionType::COMPARE_GREATERTHANOREQUALTO;
        ec2.lhs_is_aggregate = false; // per-row
        ec2.row_group_ids = ec.row_group_ids;
        ec2.num_groups = ec.num_groups;
        ec2.group_labels = ec.group_labels;
        ec2.qualifier = ec.qualifier;
        ec2.kind = ConstraintKind::USER_MECHANISM;
        // I4: same indicator as ec1 — both rows form one removable `<>`.
        ec2.ne_indicator_idx = indicator_var_idx;
        new_constraints.push_back(std::move(ec2));
    }
    input.constraints = std::move(new_constraints);
}

void ExpandDeferredAggregateNotEqual(SolverInput &input, const VarIndexer &var_indexer,
                                     vector<EvaluatedConstraint> &deferred_aggregate,
                                     const vector<pair<idx_t, string>> &aux_var_expressions) {
    if (deferred_aggregate.empty()) {
        return;
    }
    const idx_t num_rows = input.num_rows;

    // Reusable scratch for per-group LHS accumulation (replaces a per-group
    // unordered_map<int,double>). The decide-variable flat indices are bounded by
    // var_indexer.global_block_start, so the dense accumulator is sized to that —
    // tighter than total_vars and unaffected by the globals appended as we go.
    SparseCoeffAccumulator accum;
    {
        constexpr idx_t DENSE_CAP = 1u << 20;
        idx_t decide_var_index_span = var_indexer.global_block_start;
        if (decide_var_index_span <= DENSE_CAP) {
            accum.BeginDense(decide_var_index_span);
        } else {
            accum.BeginSparse(num_rows); // hint; per-group merging keeps actual size small
        }
    }

    for (auto &ec : deferred_aggregate) {
        bool has_groups = !ec.row_group_ids.empty();

        // I4 (aggregate `<>`): clause text used to name a dropped aggregate `<>`.
        // Stage 05 recorded "(SUM(x) <> K)" in aux_var_expressions keyed by the
        // indicator decide-var; carry it onto every global z this `ec` allocates so
        // the infeasible removal dial can label the DROP edit.
        string ne_label;
        for (auto &ae : aux_var_expressions) {
            if (ae.first == ec.ne_indicator_idx) {
                ne_label = ae.second;
                break;
            }
        }

        // Build group → rows mapping. For grouped constraints reuse the CSR index
        // already attached to ec; for ungrouped, materialize the trivial
        // single-group CSR locally.
        idx_t num_groups_to_process = 1;
        vector<idx_t> ungrouped_offsets;
        vector<idx_t> ungrouped_flat;
        const vector<idx_t> *offsets_ptr = nullptr;
        const vector<idx_t> *flat_ptr = nullptr;
        if (has_groups) {
            BuildGroupCSR(ec.row_group_ids, ec.num_groups, ec.group_offsets, ec.group_row_ids);
            num_groups_to_process = ec.num_groups;
            offsets_ptr = &ec.group_offsets;
            flat_ptr = &ec.group_row_ids;
        } else {
            ungrouped_offsets = {0, num_rows};
            ungrouped_flat.resize(num_rows);
            for (idx_t r = 0; r < num_rows; r++) {
                ungrouped_flat[r] = r;
            }
            offsets_ptr = &ungrouped_offsets;
            flat_ptr = &ungrouped_flat;
        }
        const auto &offsets = *offsets_ptr;
        const auto &flat_rows = *flat_ptr;

        for (idx_t g = 0; g < num_groups_to_process; g++) {
            idx_t g_begin = offsets[g];
            idx_t g_end = offsets[g + 1];
            if (g_begin == g_end) {
                continue;
            }
            idx_t g_size = g_end - g_begin;

            // Base (unscaled) RHS, read from a row that actually belongs to this
            // group. For AVG(x) <> K we store the original K in rhs_values and
            // multiply by the group size below.
            double rhs = ec.rhs_values.Get(flat_rows[g_begin]);
            if (ec.ne_avg_rhs_scale) {
                rhs *= static_cast<double>(g_size);
            }
            double fixed_offset = SumFixedAggregateLhsOffset(
                ec, &flat_rows, g_begin, g_end, "fixed aggregate <> term");
            rhs -= fixed_offset;

            // Integer-RHS guard: with integer LHS (already enforced by
            // NELhsIsIntegerValued at deferral time) and a non-integer K,
            // `LHS <> K` is a tautology — every integer LHS satisfies it. The ±1
            // Big-M rewrite would wrongly cut floor(K) and ceil(K). Skip the group
            // entirely. For AVG <> with mixed group sizes, some groups may have
            // integer K*N_g and others not — each is handled independently. No
            // global z is allocated for skipped groups, so the model stays clean.
            // The predicate is shared with the per-row path: spelling the negation
            // inline here let an infinite K through, because `inf - round(inf)` is
            // NaN and every comparison against NaN is false — so the group reached
            // the Big-M below and built an infinite coefficient instead of dropping
            // as a tautology.
            if (!NEIsIntegerValuedRhs(rhs)) {
                continue;
            }

            // Range collapse, per group. Same reasoning as the per-row path, over the
            // group's summed interval: each row of a group contributes its own solver
            // column, so the group's reachable range is the sum of its rows' ranges.
            // Unlike the per-row path a mixed verdict costs nothing here, because groups
            // are already emitted independently — each gets the encoding its own range
            // earns.
            NECollapse collapse = NECollapse::DISJUNCTION;
            if (ec.bilinear_terms.empty() && ec.quadratic_groups.empty()) {
                double grp_lo = 0.0;
                double grp_hi = 0.0;
                for (idx_t k = g_begin; k < g_end; k++) {
                    double row_lo;
                    double row_hi;
                    DecideRowSignedRange(ec, flat_rows[k], input.rigid_lower_bounds,
                                         input.rigid_upper_bounds, row_lo, row_hi);
                    grp_lo += row_lo; // -inf is absorbing, and only ever accumulates here
                    grp_hi += row_hi;
                }
                if (grp_hi < rhs - 0.5 || grp_lo > rhs + 0.5) {
                    collapse = NECollapse::ALWAYS_TRUE;
                } else if (grp_lo > rhs - 0.5) {
                    collapse = NECollapse::UPPER_ONLY;
                } else if (grp_hi < rhs + 0.5) {
                    collapse = NECollapse::LOWER_ONLY;
                }
            }
            if (collapse == NECollapse::ALWAYS_TRUE) {
                continue; // this group's aggregate cannot reach K — it excludes nothing
            }

            // Tight per-group Big-M: the aggregate LHS ranges over the SUM of this
            // group's rows, so M must cover the summed magnitude. A single per-row
            // bound is far too small at scale and would silently cap the aggregate.
            // Only the disjunctive encoding needs one.
            double M = 0.0;
            if (collapse == NECollapse::DISJUNCTION) {
                bool grp_unbounded = false;
                double grp_range = 0.0;
                for (idx_t k = g_begin; k < g_end; k++) {
                    grp_range += DecideRowTermRange(ec.variable_indices, ec.row_coefficients,
                                                    flat_rows[k], input.lower_bounds,
                                                    input.upper_bounds, grp_unbounded);
                }
                M = grp_range + std::abs(rhs) + 1.0;
                if (grp_unbounded) {
                    M = std::max(M, DECIDE_BIGM_FALLBACK);
                }
            }

            // Allocate one global binary z for this group. A collapsed group still gets
            // one, unreferenced by any row: it is what carries the clause's label and
            // groups its rows for the remove-only `<>` repair, so allocating it keeps
            // diagnosis identical whichever encoding the group received. The per-row
            // path is in the same position — its indicator is allocated at stage 05,
            // before any range is knowable.
            idx_t z_idx = var_indexer.global_block_start + input.num_global_vars;
            input.num_global_vars++;
            input.global_variable_types.push_back(LogicalType::BOOLEAN);
            input.global_lower_bounds.push_back(0.0);
            input.global_upper_bounds.push_back(1.0);
            input.global_obj_coeffs.push_back(0.0);
            input.global_variable_labels.push_back(ne_label);

            // Accumulate LHS coefficients for active rows in this group.
            for (idx_t term_idx = 0; term_idx < ec.variable_indices.size(); term_idx++) {
                idx_t decide_var_idx = ec.variable_indices[term_idx];
                if (decide_var_idx == DConstants::INVALID_INDEX) {
                    continue;
                }
                auto &col = ec.row_coefficients[term_idx];
                for (idx_t k = g_begin; k < g_end; k++) {
                    idx_t row = flat_rows[k];
                    double coeff = col.Get(row);
                    if (std::abs(coeff) < 1e-15) {
                        continue;
                    }
                    int var_idx = static_cast<int>(var_indexer.Get(decide_var_idx, row));
                    accum.Add(var_idx, coeff);
                }
            }

            // Flush once into a deduped (idx, coeff) snapshot reused for both rc1 and rc2.
            vector<int> common_indices;
            vector<double> common_coefs;
            accum.Flush(common_indices, common_coefs);

            // Collapsed: one plain inequality, no indicator term and no Big-M.
            if (collapse != NECollapse::DISJUNCTION) {
                bool lower = collapse == NECollapse::LOWER_ONLY;
                SolverInput::RawConstraint rc;
                rc.sense = lower ? '<' : '>';
                rc.rhs = lower ? rhs - 1.0 : rhs + 1.0;
                rc.indices = std::move(common_indices);
                rc.coefficients = std::move(common_coefs);
                rc.kind = ConstraintKind::USER_MECHANISM;
                rc.source_clause_id = ec.source_clause_id;
                rc.repair_group_id = ec.repair_group_id;
                rc.indicator_col = z_idx;
                input.global_constraints.push_back(std::move(rc));
                continue;
            }

            // ec1: SUM(coeffs) - M*z <= K - 1
            SolverInput::RawConstraint rc1;
            rc1.sense = '<';
            rc1.rhs = rhs - 1.0;
            rc1.indices = common_indices;
            rc1.coefficients = common_coefs;
            rc1.indices.push_back(static_cast<int>(z_idx));
            rc1.coefficients.push_back(-M);
            rc1.kind = ConstraintKind::USER_MECHANISM;
            rc1.source_clause_id = ec.source_clause_id;
            rc1.repair_group_id = ec.repair_group_id;
            rc1.indicator_col = z_idx;
            input.global_constraints.push_back(std::move(rc1));

            // ec2: SUM(coeffs) - M*z >= K + 1 - M
            SolverInput::RawConstraint rc2;
            rc2.sense = '>';
            rc2.rhs = rhs + 1.0 - M;
            rc2.indices = std::move(common_indices);
            rc2.coefficients = std::move(common_coefs);
            rc2.indices.push_back(static_cast<int>(z_idx));
            rc2.coefficients.push_back(-M);
            rc2.kind = ConstraintKind::USER_MECHANISM;
            rc2.source_clause_id = ec.source_clause_id;
            rc2.repair_group_id = ec.repair_group_id;
            rc2.indicator_col = z_idx;
            input.global_constraints.push_back(std::move(rc2));
        }
    }
}

//===--------------------------------------------------------------------===//
// Bilinear products and ABS
//===--------------------------------------------------------------------===//

void LinearizeBilinear(SolverInput &input, const vector<string> &var_names) {
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
        double U = input.upper_bounds[link.other_var_idx];
        double L = input.lower_bounds[link.other_var_idx];
        if (U >= 1e20) {
            throw InvalidInputException(
                "Bilinear term requires a finite upper bound on variable '%s'. "
                "Add a constraint like '%s <= <bound>' to provide one.",
                var_names[link.other_var_idx], var_names[link.other_var_idx]);
        }

        // ec1: w <= U * b  (i.e., w - U*b <= 0)
        EvaluatedConstraint ec1;
        ec1.variable_indices = {link.aux_idx, link.bool_var_idx};
        ec1.row_coefficients.push_back(CoefficientColumn::MakeScalar(1.0, num_rows));
        ec1.row_coefficients.push_back(CoefficientColumn::MakeScalar(-U, num_rows));
        ec1.rhs_values.AssignScalar(num_rows, 0.0);
        ec1.comparison_type = ExpressionType::COMPARE_LESSTHANOREQUALTO;
        ec1.lhs_is_aggregate = false;
        ec1.kind = ConstraintKind::STRUCTURAL;
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
        input.constraints.push_back(std::move(ec3));

        // ec4: lower corner `w >= L*b`, only needed when x can be negative. Also
        // widen the aux's own lower bound so w may equal the negative x at b=1.
        if (L < 0.0) {
            input.lower_bounds[link.aux_idx] = std::min(input.lower_bounds[link.aux_idx], L);
            EvaluatedConstraint ec4;
            ec4.variable_indices = {link.aux_idx, link.bool_var_idx};
            ec4.row_coefficients.push_back(CoefficientColumn::MakeScalar(1.0, num_rows));   // +w
            ec4.row_coefficients.push_back(CoefficientColumn::MakeScalar(-L, num_rows));    // -L*b
            ec4.rhs_values.AssignScalar(num_rows, 0.0);
            ec4.comparison_type = ExpressionType::COMPARE_GREATERTHANOREQUALTO;
            ec4.lhs_is_aggregate = false;
            ec4.kind = ConstraintKind::STRUCTURAL;
            input.constraints.push_back(std::move(ec4));
        }
    }
}

void LinearizeAbsMaximize(SolverInput &input, const vector<string> &var_names) {
    if (input.abs_maximize_links.empty()) {
        return;
    }
    const idx_t num_rows = input.num_rows;

    // For each link, find the two tagged lower-bound EvaluatedConstraints
    // (C1: aux >= inner tagged ABS_UB_POS, C2: aux >= -inner tagged ABS_UB_NEG) and emit:
    //   C_ub1: derived from C1, add y with coeff +2M, comparison <=, rhs[r] += 2M
    //   C_ub2: derived from C2, add y with coeff -2M, comparison <=, rhs unchanged
    // Together with C1/C2 these force aux = |inner| under MAXIMIZE.
    struct AbsConstraintPair {
        idx_t c1 = DConstants::INVALID_INDEX;
        idx_t c2 = DConstants::INVALID_INDEX;
    };
    unordered_map<idx_t, AbsConstraintPair> abs_tag_map;
    for (idx_t ci = 0; ci < input.constraints.size(); ci++) {
        auto &ec = input.constraints[ci];
        if (ec.abs_y_idx == DConstants::INVALID_INDEX) {
            continue;
        }
        if (ec.abs_is_pos_bound) {
            abs_tag_map[ec.abs_y_idx].c1 = ci;
        } else {
            abs_tag_map[ec.abs_y_idx].c2 = ci;
        }
    }

    // Reserve up-front so the two push_back calls per link cannot reallocate
    // input.constraints. With capacity guaranteed, references to existing C1/C2 stay
    // valid across appends and we don't need defensive copies of their fields.
    input.constraints.reserve(input.constraints.size() + 2 * input.abs_maximize_links.size());

    for (auto &link : input.abs_maximize_links) {
        auto it = abs_tag_map.find(link.y_idx);
        D_ASSERT(it != abs_tag_map.end() &&
                 it->second.c1 != DConstants::INVALID_INDEX &&
                 it->second.c2 != DConstants::INVALID_INDEX);

        const auto &c1 = input.constraints[it->second.c1];
        const auto &c2 = input.constraints[it->second.c2];

        // Compute M = max over rows of |rhs[r]| + sum_{t: var != aux} |coeff[t][r]| * max(|lb|, |ub|),
        // reusing the shared per-row range helper (skipping the aux term). This
        // upper-bounds |inner| across all rows and variable values. Unlike the
        // indicator sites, ABS-maximize is STRICT: if no finite bound can be derived
        // for a contributing variable, there is no valid M, so we throw rather than
        // fall back. (Implied-bound propagation may have already supplied a bound,
        // in which case this succeeds.)
        bool abs_unbounded = false;
        double M = 0.0;
        for (idx_t r = 0; r < num_rows; r++) {
            // The link's bound carries the constant part of the expression the user
            // wrote inside ABS. Infinity there admits no finite M (see
            // DecideTightPerRowBigM), and the value came from their data, so name
            // that rather than the linearization.
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
        double two_M = 2.0 * M;

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
        input.constraints.push_back(std::move(ec_ub2));
    }
}

} // namespace duckdb
