#include "duckdb/decidb/ilp_linearization.hpp"
#include "duckdb/common/exception.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace duckdb {

// --- Global auxiliary creation ---------------------------------------------
// Every auxiliary column DeciDB introduces is created here. Both helpers exist so
// that a continuous auxiliary cannot be declared without stating the range of the
// expression it stands for: an infinite bound is reachable only through
// `AuxRange::lo_unbounded` / `hi_unbounded`, i.e. only on an end no contributing
// decision variable's box could derive. The two are separate because a range open
// on one side still boxes the other. A continuous auxiliary left free
// when its range was in fact derivable costs the root LP dearly — the simplex has
// no box to start from and crawls toward the answer one pivot at a time.

//! Give the auxiliary at flat index `aux_idx` its diagnosis label, padding the label
//! channel out to that column first.
//!
//! `global_variable_labels` is positional — entry `i` names global column `i` — but it
//! is written only where a label exists, so it can trail the block and a bare
//! `push_back` would land the label on whatever column happens to be next. The pad is
//! what keeps the two aligned. Every creation site used to spell it out itself, which
//! left a new site one forgotten line away from naming the wrong column; both creation
//! helpers below call this instead, so a column and its label are set in one step.
//! Unnamed columns take an empty entry, which is what the readback fills in anyway.
static void LabelGlobalAux(SolverInput &input, const VarIndexer &indexer, idx_t aux_idx,
                           const string &label) {
    input.global_variable_labels.resize(aux_idx - indexer.global_block_start);
    input.global_variable_labels.push_back(label);
}

//! Append one continuous auxiliary column bounded by the family it reduces over.
//! `label` is the clause text a diagnosis should render for it; empty means unnamed.
//! Returns its flat column index.
static idx_t AddGlobalContinuousAux(SolverInput &input, const VarIndexer &indexer,
                                    const AuxRange &range, double obj_coeff,
                                    const string &label = string()) {
    idx_t aux_idx = indexer.global_block_start + input.num_global_vars;
    input.num_global_vars += 1;
    LabelGlobalAux(input, indexer, aux_idx, label);
    input.global_variable_types.push_back(LogicalType::DOUBLE);
    // Per side. An auxiliary over `x >= 0` with no ceiling is emitted `[0, 1e30]`,
    // not `[-1e30, 1e30]`: the floor was derived, and throwing it away because the
    // ceiling was not is a box given up for nothing.
    input.global_lower_bounds.push_back(range.lo_unbounded ? -1e30 : range.lo);
    input.global_upper_bounds.push_back(range.hi_unbounded ? 1e30 : range.hi);
    input.global_bounds_unbounded.push_back(range.Unbounded());
    input.global_obj_coeffs.push_back(obj_coeff);
    return aux_idx;
}

//! Append one binary auxiliary column. Its [0,1] box comes from the domain, so it
//! needs no range.
static idx_t AddGlobalBinaryAux(SolverInput &input, const VarIndexer &indexer,
                                double obj_coeff, const string &label = string()) {
    idx_t aux_idx = indexer.global_block_start + input.num_global_vars;
    input.num_global_vars += 1;
    LabelGlobalAux(input, indexer, aux_idx, label);
    input.global_variable_types.push_back(LogicalType::BOOLEAN);
    input.global_lower_bounds.push_back(0.0);
    input.global_upper_bounds.push_back(1.0);
    input.global_bounds_unbounded.push_back(false);
    input.global_obj_coeffs.push_back(obj_coeff);
    return aux_idx;
}

//! Pin an already-created auxiliary to exactly `value`. Used where a reducer turns
//! out to range over nothing at all, so the auxiliary has no pinning rows and would
//! otherwise float on whatever box it was given.
static void PinGlobalAux(SolverInput &input, const VarIndexer &indexer, idx_t aux_idx,
                         double value) {
    idx_t local = aux_idx - indexer.global_block_start;
    input.global_lower_bounds[local] = value;
    input.global_upper_bounds[local] = value;
}

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

//! Signed bracket of one row's variable terms, defined below — forward-declared so the
//! native MIN/MAX emitter above it can box its auxiliary columns by the same walk the
//! Big-M paths use.
//!
//! `skip_idx` leaves one column out of the walk, the way DecideRowTermRange does: the
//! ABS envelope rows carry the auxiliary itself as a term, and what has to be bracketed
//! there is the expression the auxiliary is pinned AGAINST, not the auxiliary.
static void DecideRowSignedRange(const EvaluatedConstraint &ec, idx_t row,
                                 const vector<double> &lower_bounds,
                                 const vector<double> &upper_bounds, double &out_lo,
                                 double &out_hi, idx_t skip_idx = DConstants::INVALID_INDEX);

//! Refuse a Big-M linearization that has no finite M, naming a column to bound.
//!
//! A Big-M constant must dominate the reachable magnitude of the row's left-hand
//! side. When a contributing decision variable has no finite bound, no constant does
//! — so there is no M, only a guess. DeciDB refuses rather than guessing: a guessed M
//! that the true range exceeds does not fail, it silently cuts the feasible region
//! and returns a confidently wrong answer.
//!
//! `LinearizeAbsMaximize` has always refused in exactly this situation. This is the
//! same refusal, in the same words, for the constructs that used to take a 1e6 floor
//! instead — and it applies on every backend, so one query never answers correctly on
//! one solver and wrongly on another.
//!
//! The two callers below differ only in how they locate the column to blame, so the
//! words the user reads live here, once. `bad` is that column, or INVALID_INDEX when
//! the open bound could not be attributed to any named column.
[[noreturn]] static void ThrowUnboundedBigMNaming(idx_t bad, const vector<string> &var_names,
                                                  const char *construct) {
    if (bad == DConstants::INVALID_INDEX || bad >= var_names.size() || var_names[bad].empty()) {
        throw InvalidInputException("Rewriting %s requires a finite bound on every decision variable it "
                                    "reads, and one of them is unbounded. Add an upper and a lower bound "
                                    "to the variables in that clause.",
                                    construct);
    }
    const string &name = var_names[bad];
    throw InvalidInputException("Rewriting %s requires a finite bound on '%s'. Add constraints "
                                "'%s >= <lower>' and '%s <= <upper>'.",
                                construct, name, name, name);
}

//! Locate the blame for a row Big-M: a contributing decision variable with an open box.
[[noreturn]] static void ThrowUnboundedBigM(const EvaluatedConstraint &ec, const vector<double> &lower_bounds,
                                            const vector<double> &upper_bounds,
                                            const vector<string> &var_names, const char *construct) {
    // Name a column the user can actually bound: the first contributing decision
    // variable whose box is open. The indicator this rewrite created is skipped — it
    // is ours, not theirs, and it is binary anyway.
    idx_t bad = DConstants::INVALID_INDEX;
    for (idx_t t = 0; t < ec.variable_indices.size(); t++) {
        idx_t v = ec.variable_indices[t];
        if (v == DConstants::INVALID_INDEX || v == ec.minmax_indicator_idx || v == ec.ne_indicator_idx ||
            v == ec.abs_aux_idx) {
            continue;
        }
        if (v < upper_bounds.size() && v < lower_bounds.size() &&
            (upper_bounds[v] >= 1e20 || lower_bounds[v] <= -1e20)) {
            bad = v;
            break;
        }
    }
    ThrowUnboundedBigMNaming(bad, var_names, construct);
}

//! The auxiliary-family twin of ThrowUnboundedBigM: a MIN/MAX auxiliary is linked to
//! its expression by `(aux - expr) +/- M*y`, so M has to stay slack across the whole
//! family's spread. An unbounded contributor leaves no such M, and the same rule
//! applies — refuse rather than guess.
[[noreturn]] static void ThrowUnboundedAuxBigM(const AuxRange &range, const vector<string> &var_names,
                                               const char *construct) {
    // The range already recorded which variable opened it, so there is nothing to search.
    ThrowUnboundedBigMNaming(range.unbounded_var, var_names, construct);
}

//! Which construct asked for this Big-M, for the refusal above. Read off the
//! indicator the rewrite attached, so the message names what the user wrote.
static const char *DescribeBigMConstruct(const EvaluatedConstraint &ec) {
    if (ec.ne_indicator_idx != DConstants::INVALID_INDEX) {
        return "<>";
    }
    if (ec.abs_aux_idx != DConstants::INVALID_INDEX) {
        return "ABS";
    }
    return "MIN/MAX";
}

double DecideTightPerRowBigM(const EvaluatedConstraint &ec,
                             const vector<double> &lower_bounds,
                             const vector<double> &upper_bounds,
                             idx_t num_rows,
                             const vector<string> &var_names) {
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
    // Refuse before finishing the constant: an open contributor means there is no M
    // to return, so the slack term below would only be computed and thrown away.
    if (has_unbounded) {
        ThrowUnboundedBigM(ec, lower_bounds, upper_bounds, var_names, DescribeBigMConstruct(ec));
    }
    return M + 1.0;
}

//! Does the lowering have a Big-M for this clause at all?
//!
//! The same walk `DecideTightPerRowBigM` makes, minus its refusals, because this is a
//! ROUTING question rather than an encoding one: a clause with no derivable M is exactly
//! the clause the native arm exists to answer, and asking by catching an exception would
//! make the normal case pay for the rare one. The non-finite BOUND check is deliberately
//! not repeated — `ClassifyMinMaxGroups` settles an infinite bound by direction on both
//! arms, before either gets here.
bool MinMaxBigMDerivable(const EvaluatedConstraint &ec, const vector<double> &lower_bounds,
                         const vector<double> &upper_bounds, idx_t num_rows) {
    bool has_unbounded = false;
    for (idx_t r = 0; r < num_rows; r++) {
        if (!ec.row_group_ids.empty() && ec.row_group_ids[r] == DConstants::INVALID_INDEX) {
            continue;
        }
        DecideRowTermRange(ec.variable_indices, ec.row_coefficients, r, lower_bounds,
                           upper_bounds, has_unbounded);
        if (has_unbounded) {
            return false;
        }
    }
    return true;
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
        // A row that means something other than what its terms say: a hard MIN/MAX
        // (marked by its aggregate name, which both arms carry), a `<>` disjunction, or
        // an ABS envelope. None of them implies a bound on anything.
        if (!ec.minmax_agg_type.empty() || ec.ne_indicator_idx != DConstants::INVALID_INDEX ||
            ec.abs_aux_idx != DConstants::INVALID_INDEX) {
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

namespace {

//! Settle every group's bound before either arm encodes anything, and mask off the
//! groups neither has work for. A bound that is out of reach or beyond reach is
//! answered by the direction it points, not by any encoding, so this is shared: the
//! Big-M rows and the native general constraint make exactly the same decisions here.
//!
//! Mutates `ec` (masking excluded groups out of `row_group_ids`) and appends the
//! plain per-row constraint an unsatisfiable group becomes. Returns false when no
//! group is left to encode.
bool ClassifyMinMaxGroups(EvaluatedConstraint &ec, idx_t num_rows, bool is_max_agg,
                          vector<EvaluatedConstraint> &new_constraints) {
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
                    // Drop the MIN/MAX marking as well as the indicator: what is emitted
                    // is an ordinary per-row constraint, and nothing downstream should
                    // read its LHS as an extremum again.
                    ec_no_solution.minmax_indicator_idx = DConstants::INVALID_INDEX;
                    ec_no_solution.minmax_agg_type.clear();
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
                    return false; // no group left to encode
                }
            }
        }
    return true;
}


//! The NATIVE arm for one clause: `z = MAX(t..)` stated for the backend rather than
//! encoded. One extremum column per group, pinned by a general constraint to that
//! group's member columns, and the user's own bound as a single row over it. No Big-M
//! and no indicators, so no contributing variable needs a finite bound — which is the
//! only reason this arm exists.
void EmitNativeMinMaxConstraint(SolverInput &input, const VarIndexer &indexer,
                                const EvaluatedConstraint &ec, bool is_max_agg) {
    const idx_t num_rows = input.num_rows;
    bool has_groups = !ec.row_group_ids.empty();
    idx_t group_count = has_groups ? MaxValue<idx_t>(ec.num_groups, 1) : 1;

    vector<vector<int>> group_args(group_count);
    vector<AuxRange> group_range(group_count);
    // The row each group's outer clause reads its bound off, recorded on the walk
    // that already visits every row. Finding it again per group below would rescan
    // the whole table once per group.
    vector<idx_t> group_first_row(group_count, DConstants::INVALID_INDEX);
    for (idx_t r = 0; r < num_rows; r++) {
        idx_t g = has_groups ? ec.row_group_ids[r] : 0;
        if (g == DConstants::INVALID_INDEX || g >= group_count) {
            continue;
        }
        if (group_first_row[g] == DConstants::INVALID_INDEX) {
            group_first_row[g] = r;
        }
        // A column pinned to this row's inner expression. The general constraint
        // relates variables, so the linear half is a row either way. Boxed by that
        // expression's own range wherever one exists — a free continuous column is
        // a measured performance cliff — and free only where none does, which is
        // the case this arm exists to answer at all.
        AuxRange row_range;
        double row_constant =
            DecideRowFixedLhsOffset(ec.variable_indices, ec.row_coefficients, r);
        {
            double row_lo = 0.0, row_hi = 0.0;
            DecideRowSignedRange(ec, r, input.lower_bounds, input.upper_bounds, row_lo, row_hi);
            // Each end on its own. `DecideRowSignedRange` already reports them
            // independently, and a row open on one side — `x >= 0` with no ceiling
            // is the common one — still has a derived bound on the other.
            // `t` carries the constant part too, so shift the variable-only bracket
            // by it before using it as the box.
            row_range.CoverRowSided(row_lo + row_constant, row_hi + row_constant, row_lo, row_hi,
                                    DConstants::INVALID_INDEX);
        }
        // `MAX(x) <op> K` over a decision variable is a renaming, not an expression:
        // `t = x` would cost one column and one equality row per data row to say
        // nothing, and presolve cannot substitute a column that a general constraint
        // reads. So when this row's inner expression is exactly one variable with
        // coefficient 1 and no constant part, the variable's own column IS the
        // argument. Any other coefficient or a constant makes it a real expression,
        // and a general constraint relates columns, so that one still needs one.
        idx_t sole_var = DConstants::INVALID_INDEX;
        double sole_coef = 0.0;
        idx_t term_count = 0;
        for (idx_t t = 0; t < ec.variable_indices.size(); t++) {
            idx_t v = ec.variable_indices[t];
            if (v == DConstants::INVALID_INDEX) {
                continue;
            }
            double coef = ec.row_coefficients[t].Get(r);
            if (coef == 0.0) {
                continue;
            }
            term_count++;
            sole_var = v;
            sole_coef = coef;
        }
        if (term_count == 1 && sole_coef == 1.0 && row_constant == 0.0) {
            group_args[g].push_back(static_cast<int>(indexer.Get(sole_var, r)));
            group_range[g].Cover(row_range);
            continue;
        }

        idx_t t_idx = AddGlobalContinuousAux(input, indexer, row_range, 0.0);

        SolverInput::RawConstraint pin;
        pin.indices.push_back(static_cast<int>(t_idx));
        pin.coefficients.push_back(1.0);
        for (idx_t t = 0; t < ec.variable_indices.size(); t++) {
            idx_t v = ec.variable_indices[t];
            if (v == DConstants::INVALID_INDEX) {
                continue;
            }
            double coef = ec.row_coefficients[t].Get(r);
            if (coef == 0.0) {
                continue;
            }
            // `t - inner = 0`. A constant term of `inner` folds into the bound.
            pin.indices.push_back(static_cast<int>(indexer.Get(v, r)));
            pin.coefficients.push_back(-coef);
        }
        pin.sense = '=';
        pin.rhs = row_constant;
        pin.kind = ConstraintKind::STRUCTURAL;
        pin.source_clause_id = ec.source_clause_id;
        pin.repair_group_id = ec.repair_group_id;
        input.global_constraints.push_back(std::move(pin));

        group_args[g].push_back(static_cast<int>(t_idx));
        group_range[g].Cover(row_range);
    }

    for (idx_t g = 0; g < group_count; g++) {
        if (group_args[g].empty()) {
            continue; // masked-out group: nothing to take an extremum over
        }
        // The extremum of a family lies inside the family's own bracket, so the
        // union of the member ranges boxes it.
        idx_t z_idx = AddGlobalContinuousAux(input, indexer, group_range[g], 0.0);

        GeneralConstraintSpec gc;
        gc.kind = is_max_agg ? GeneralConstraintKind::MAX : GeneralConstraintKind::MIN;
        gc.result_column = static_cast<int>(z_idx);
        gc.argument_columns = std::move(group_args[g]);
        gc.source_clause_id = ec.source_clause_id;
        gc.repair_group_id = ec.repair_group_id;
        input.general_constraints.push_back(std::move(gc));

        // The user's clause, now over the extremum itself: `z <op> K`. This is the
        // row the elastic engine sees, and it is the one the user actually wrote —
        // an improvement on the lowered form, where the bound was spread across a
        // per-row Big-M family.
        D_ASSERT(group_first_row[g] != DConstants::INVALID_INDEX);

        SolverInput::RawConstraint outer;
        outer.indices.push_back(static_cast<int>(z_idx));
        outer.coefficients.push_back(1.0);
        outer.sense = is_max_agg ? '>' : '<';
        // The raw bound, not the effective one: each `t` above already carries its
        // row's constant LHS part, because a per-row constant can differ inside a
        // group and the extremum is taken over the whole expression, constant
        // included. (The lowering arm nets the constant out instead, since it
        // compares row by row.) Group-constant by construction —
        // ReduceAggregateRhsPerGroup collapsed a row-varying bound already.
        // A uniform bound is the same value whichever row is picked, so the
        // representative row is not even consulted.
        outer.rhs = ec.rhs_values.IsUniform() ? ec.rhs_values.UniformValue()
                                              : ec.rhs_values.Get(group_first_row[g]);
        outer.kind = ConstraintKind::USER_PARAMETER;
        // The elastic shape, without which this row does not fold. One `PER` clause
        // emits one of these rows per group, and they are all the same line of SQL:
        // the user edits a single literal and every group moves with it. Left UNSET
        // they never fold, so an infeasible `MAX(e) >= K PER g` reported one edit per
        // group — and only the loosest of them repaired anything. Applying any other
        // left the query infeasible, which is a worse failure than reporting nothing.
        // Read from the same flag the linear builder reads, so the native and lowered
        // arms classify the clause identically; a genuinely per-group bound stays
        // PER_ROW_DATA and reports a virtual offset, as it does elsewhere.
        outer.shape = ec.rhs_is_shared_scalar ? ElasticShape::SHARED_SCALAR
                                              : ElasticShape::PER_ROW_DATA;
        outer.source_clause_id = ec.source_clause_id;
        outer.repair_group_id = ec.repair_group_id;
        input.global_constraints.push_back(std::move(outer));
    }
}

//! The LOWERED arm for one clause: the per-row Big-M family plus the `SUM(y) >= 1` that
//! makes one row bind. Rewrites the clause in the per-row representation it arrived in,
//! so a `MAX(e) >= K` over a large relation stays one spec until the model builder fans
//! it out.
void EmitLoweredMinMaxConstraint(SolverInput &input, const EvaluatedConstraint &ec,
                                 bool is_max_agg, const vector<string> &var_names,
                                 vector<EvaluatedConstraint> &out) {
    const idx_t num_rows = input.num_rows;
    // Stage 05 allocates the indicator exactly when it routed this query here.
    // Reaching this arm without one would mean the plan and the solve disagree about
    // which formulation was chosen.
    D_ASSERT(ec.minmax_indicator_idx != DConstants::INVALID_INDEX);
    idx_t indicator_idx = ec.minmax_indicator_idx;

    // Compute Big-M from variable bounds. Skip constant LHS terms
    // (var_idx == INVALID_INDEX) — they have no associated variable
    // bound; their contribution will be folded into the RHS by the
    // per-row constraint emitter.
    double M = DecideTightPerRowBigM(ec, input.lower_bounds, input.upper_bounds, num_rows,
                                     var_names);

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

    // Hard MAX(expr) >= K: for each row i, expr_i - M*y_i >= K - M.
    // Hard MIN(expr) <= K: for each row i, expr_i + M*y_i <= K + M.
    // The two directions are mirror images: only the Big-M's sign and the
    // comparison direction flip.
    double m_sign = is_max_agg ? -1.0 : 1.0;
    ExpressionType cmp = is_max_agg ? ExpressionType::COMPARE_GREATERTHANOREQUALTO
                                     : ExpressionType::COMPARE_LESSTHANOREQUALTO;

    EvaluatedConstraint ec_row;
    ec_row.variable_indices = ec.variable_indices;
    ec_row.row_coefficients = ec.row_coefficients;
    ec_row.variable_indices.push_back(indicator_idx);
    ec_row.row_coefficients.push_back(CoefficientColumn::MakeScalar(m_sign * M, num_rows));
    ec_row.rhs_values = BuildShiftedRhs(m_sign * M);
    ec_row.comparison_type = cmp;
    ec_row.lhs_is_aggregate = false; // per-row!
    ec_row.row_group_ids = ec.row_group_ids;
    ec_row.num_groups = ec.num_groups;
    ec_row.group_labels = ec.group_labels;
    ec_row.qualifier = ec.qualifier;
    // These N rows ARE the user's clause, so they carry it. `MAX(e) >= K` lowers to
    // `e_i - M*y_i >= K - M` per row, and lowering K moves every one of them by the
    // same amount — one editable literal, one shared slack, one reported edit. Left
    // rigid, the clause had no loosenable row at all and diagnosis fell back to
    // whatever column bound happened to be nearby, so the same SQL was repaired
    // differently depending on which solver the host had. The Big-M lives in
    // `rhs_mechanism_offset` so the report subtracts it back off and quotes `K`.
    //
    // The companion `SUM(y) >= 1` below stays rigid: it is the disjunction itself,
    // not a number the user wrote, and slackening it would let the clause be
    // satisfied by no row at all — a "repair" with no SQL edit behind it.
    ec_row.kind = ConstraintKind::USER_PARAMETER;
    ec_row.source_clause_id = ec.source_clause_id;
    ec_row.repair_group_id = ec.repair_group_id;
    ec_row.rhs_is_shared_scalar = ec.rhs_is_shared_scalar;
    ec_row.rhs_label = ec.rhs_label;
    ec_row.rhs_mechanism_offset = m_sign * M;
    out.push_back(std::move(ec_row));

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
    out.push_back(std::move(ec_sum));
}

} // namespace

void LinearizeMinMaxConstraints(SolverInput &input, const VarIndexer &indexer,
                                const vector<string> &var_names, NativeConstructPolicy policy) {
    const idx_t num_rows = input.num_rows;
    vector<EvaluatedConstraint> new_constraints;
    for (auto &ec : input.constraints) {
        // The aggregate name is the marking, not the indicator index: both arms carry
        // the name, and the indicator now exists whichever arm runs.
        if (ec.minmax_agg_type.empty()) {
            new_constraints.push_back(std::move(ec));
            continue;
        }
        // The routing, per clause. A `MAX(e) >= K` whose contributors are all bounded
        // lowers to a smaller model than a general constraint does; one whose range is
        // underivable has no lowering at all and goes native, which is the only reason
        // that arm exists.
        //
        // Per CLAUSE and not per query: one clause can be bounded while another in the
        // same statement is not, and each gets the formulation it can actually use.
        // Asked BEFORE the bound classification below, which masks groups off: what the
        // policy weighs is whether the clause as written has a Big-M, not what is left
        // of it after the vacuous groups are dropped.
        bool native =
            policy.Use(!MinMaxBigMDerivable(ec, input.lower_bounds, input.upper_bounds, num_rows));
        bool is_max_agg = (ec.minmax_agg_type == "max");
        // Settle every group's bound before either arm encodes anything. A bound that is
        // out of reach or beyond reach is answered by the direction it points, not by an
        // encoding, so both arms make exactly the same decisions here — and whatever it
        // emits (the unsatisfiable group's plain row) is an ordinary constraint that
        // rejoins the model.
        if (!ClassifyMinMaxGroups(ec, num_rows, is_max_agg, new_constraints)) {
            continue;
        }
        if (native) {
            EmitNativeMinMaxConstraint(input, indexer, ec, is_max_agg);
            continue;
        }
        EmitLoweredMinMaxConstraint(input, ec, is_max_agg, var_names, new_constraints);
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
                                 double &out_hi, idx_t skip_idx) {
    constexpr double INF = std::numeric_limits<double>::infinity();
    double lo = 0.0;
    double hi = 0.0;
    bool lo_unbounded = false;
    bool hi_unbounded = false;
    for (idx_t t = 0; t < ec.variable_indices.size(); t++) {
        idx_t v = ec.variable_indices[t];
        if (v == DConstants::INVALID_INDEX || v == skip_idx) {
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

//! The NATIVE arm of one per-row `<>`: each row's disjunction as two implications,
//! `z == 0 => LHS <= K-1` and `z == 1 => LHS >= K+1`, instead of a Big-M pair. No
//! constant to dominate the row, so no contributing variable needs a finite bound —
//! the same payoff ABS and MIN/MAX get from their general constraints.
static void EmitNativeNotEqual(SolverInput &input, const VarIndexer &indexer,
                               const EvaluatedConstraint &ec) {
    const idx_t num_rows = input.num_rows;
    {
        idx_t z_var = ec.ne_indicator_idx;
        for (idx_t r = 0; r < num_rows; r++) {
            if (!ec.row_group_ids.empty() && ec.row_group_ids[r] == DConstants::INVALID_INDEX) {
                continue; // masked out by WHEN/PER, or a non-integer bound on this row
            }
            // `z` is row-scoped: each row's disjunction gets its own binary, exactly as
            // the Big-M encoding does.
            int z_col = static_cast<int>(indexer.Get(z_var, r));
            double k = ec.rhs_values.Get(r);

            vector<int> indices;
            vector<double> coefficients;
            double constant = 0.0;
            for (idx_t t = 0; t < ec.variable_indices.size(); t++) {
                idx_t v = ec.variable_indices[t];
                double coef = ec.row_coefficients[t].Get(r);
                if (v == DConstants::INVALID_INDEX) {
                    constant += coef; // constant LHS part folds into the bound
                    continue;
                }
                if (coef == 0.0) {
                    continue;
                }
                indices.push_back(static_cast<int>(indexer.Get(v, r)));
                coefficients.push_back(coef);
            }

            // Both halves carry the clause's provenance and its indicator column, so
            // the infeasible removal dial groups them into one droppable `<>` — the
            // same grouping the Big-M rows get, and the reason this construct is
            // expressed as indicator constraints rather than general ones.
            // The row's terms are taken by value so the half that no longer needs them
            // can hand them straight over: only the first half copies.
            auto emit = [&](vector<int> row_indices, vector<double> row_coefficients, int binval,
                            char sense, double rhs) {
                SolverInput::IndicatorConstraintSpec ic;
                ic.binary_column = z_col;
                ic.binary_value = binval;
                ic.indices = std::move(row_indices);
                ic.coefficients = std::move(row_coefficients);
                ic.sense = sense;
                ic.rhs = rhs;
                ic.kind = ConstraintKind::USER_MECHANISM;
                ic.source_clause_id = ec.source_clause_id;
                ic.repair_group_id = ec.repair_group_id;
                ic.indicator_col = static_cast<idx_t>(z_col);
                input.indicator_constraints.push_back(std::move(ic));
            };
            // z = 0  =>  LHS <= K - 1
            emit(indices, coefficients, 0, '<', k - 1.0 - constant);
            // z = 1  =>  LHS >= K + 1. Last use of the row's terms.
            emit(std::move(indices), std::move(coefficients), 1, '>', k + 1.0 - constant);
        }
    }
}


static void ExpandAggregateNotEqual(SolverInput &input, const VarIndexer &var_indexer,
                                    vector<EvaluatedConstraint> &deferred_aggregate,
                                    const vector<pair<idx_t, string>> &aux_var_expressions,
                                    const vector<string> &var_names, bool native_not_equal);

void LinearizeNotEqual(SolverInput &input, const VarIndexer &indexer,
                       const vector<pair<idx_t, string>> &aux_var_expressions,
                       const vector<string> &var_names, bool native_not_equal) {
    const idx_t num_rows = input.num_rows;

    // The aggregate spelling is collected on this walk and finished below: it needs one
    // global binary per GROUP, which the per-row loop has no notion of.
    vector<EvaluatedConstraint> deferred_aggregate;
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

        idx_t indicator_var_idx = ec.ne_indicator_idx;

        if (native_not_equal) {
            EmitNativeNotEqual(input, indexer, ec);
            continue;
        }

        // Tight data-driven per-row Big-M for the inline NE expansion. Computed
        // after the tautology filter: a row this rewrite never emits must not be
        // asked for an M, and an infinite bound is exactly such a row
        // (`LHS <> Infinity` always holds), so it is dropped above rather than
        // refused by the Big-M guard.
        double M = DecideTightPerRowBigM(ec, input.lower_bounds, input.upper_bounds, num_rows, var_names);

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

    ExpandAggregateNotEqual(input, indexer, deferred_aggregate, aux_var_expressions, var_names,
                            native_not_equal);
}

//! The AGGREGATE spelling of `<>`. It cannot expand against the row-scoped indicator
//! the per-row spelling uses: it needs one *global* binary per group, and the group's
//! Big-M must cover the summed range over its rows rather than a single row's, which a
//! per-row bound would silently cap. `aux_var_expressions` supplies the clause text
//! stage 05 recorded for the indicator, so a dropped aggregate `<>` can be named in a
//! repair.
static void ExpandAggregateNotEqual(SolverInput &input, const VarIndexer &var_indexer,
                                    vector<EvaluatedConstraint> &deferred_aggregate,
                                    const vector<pair<idx_t, string>> &aux_var_expressions,
                                    const vector<string> &var_names, bool native_not_equal) {
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
            if (collapse == NECollapse::DISJUNCTION && !native_not_equal) {
                bool grp_unbounded = false;
                double grp_range = 0.0;
                for (idx_t k = g_begin; k < g_end; k++) {
                    grp_range += DecideRowTermRange(ec.variable_indices, ec.row_coefficients,
                                                    flat_rows[k], input.lower_bounds,
                                                    input.upper_bounds, grp_unbounded);
                }
                M = grp_range + std::abs(rhs) + 1.0;
                if (grp_unbounded) {
                    ThrowUnboundedBigM(ec, input.lower_bounds, input.upper_bounds, var_names, "<>");
                }
            }

            // Allocate one global binary z for this group. A collapsed group still gets
            // one, unreferenced by any row: it is what carries the clause's label and
            // groups its rows for the remove-only `<>` repair, so allocating it keeps
            // diagnosis identical whichever encoding the group received. The per-row
            // path is in the same position — its indicator is allocated at stage 05,
            // before any range is knowable.
            idx_t z_idx = AddGlobalBinaryAux(input, var_indexer, 0.0, ne_label);

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

            if (native_not_equal) {
                // Native: the two halves as implications on this group's binary, so the
                // group's summed Big-M — which is what forces a bound on every
                // contributing variable — is not needed at all.
                // By value, as in the per-row twin above: only the first half copies
                // the group's snapshot, the second hands it over.
                auto emit = [&](vector<int> grp_indices, vector<double> grp_coefs, int binval,
                                char sense, double bound) {
                    SolverInput::IndicatorConstraintSpec ic;
                    ic.binary_column = static_cast<int>(z_idx);
                    ic.binary_value = binval;
                    ic.indices = std::move(grp_indices);
                    ic.coefficients = std::move(grp_coefs);
                    ic.sense = sense;
                    ic.rhs = bound;
                    ic.kind = ConstraintKind::USER_MECHANISM;
                    ic.source_clause_id = ec.source_clause_id;
                    ic.repair_group_id = ec.repair_group_id;
                    ic.indicator_col = z_idx;
                    input.indicator_constraints.push_back(std::move(ic));
                };
                emit(common_indices, common_coefs, 0, '<', rhs - 1.0);
                // Last use of the snapshot on this arm.
                emit(std::move(common_indices), std::move(common_coefs), 1, '>', rhs + 1.0);
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
            // that earns a free column. `lo`/`hi` carry the constant, `spread` does not.
            AuxRange inner_range;
            inner_range.CoverRowSided(k - vars_hi, k - vars_lo, -vars_hi, -vars_lo,
                                      DConstants::INVALID_INDEX);
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
                input.global_constraints.push_back(std::move(link_row));
            }

            GeneralConstraintSpec gc;
            gc.kind = GeneralConstraintKind::ABS;
            gc.result_column = static_cast<int>(indexer.Get(link.aux_idx, r));
            gc.argument_columns.push_back(static_cast<int>(t_idx));
            gc.source_clause_id = c1.source_clause_id;
            gc.repair_group_id = c1.repair_group_id;
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

void LinearizeMinMaxObjective(SolverInput &input, const VarIndexer &indexer,
                              const MinMaxObjectiveSpec &spec, const vector<string> &var_names,
                              NativeConstructPolicy native_min_max) {
    idx_t num_rows = input.num_rows;

    // Save objective data (needed for constraint generation in the PER MIN/MAX
    // and flat aggregate paths). Defer the deep copy of objective_coefficients
    // — which is a vector<vector<double>> sized num_terms * num_rows — until
    // we know we'll take one of those paths.
    auto saved_obj_var_indices = input.objective_variable_indices;
    bool need_saved_obj =
        !saved_obj_var_indices.empty() &&
        ((spec.per_inner_agg != ObjectiveAggregateType::NONE && input.objective_num_groups > 0) ||
         spec.flat_agg != ObjectiveAggregateType::NONE);
    vector<CoefficientColumn> saved_obj_coefficients;
    if (need_saved_obj) {
        saved_obj_coefficients = input.objective_coefficients;
    }

    // One row of the saved objective expression, bracketed against every contributing
    // variable's box. `lo`/`hi` include constant terms — an auxiliary is pinned against
    // the whole expression — while `var_lo`/`var_hi` exclude them, because a constant
    // cancels in the (aux - expr) difference a Big-M row slackens.
    //! An end no contributing variable's box could derive is reported as an infinity
    //! rather than as a flag beside a partial sum. That is what lets a caller add these
    //! up: a group sum is open below exactly when some member is, and `-inf + finite`
    //! already says so. The two ends never mix (`lo` is only ever `-inf`, `hi` only
    //! ever `+inf`), so no accumulation can reach a NaN.
    struct SavedRowRange {
        double lo = 0.0;
        double hi = 0.0;
        double var_lo = 0.0;
        double var_hi = 0.0;
        idx_t unbounded_var = DConstants::INVALID_INDEX;
    };
    auto saved_row_range = [&](idx_t r) -> SavedRowRange {
        constexpr double INF = std::numeric_limits<double>::infinity();
        SavedRowRange out;
        for (idx_t t = 0; t < saved_obj_var_indices.size(); t++) {
            double c = saved_obj_coefficients[t].Get(r);
            if (std::abs(c) < 1e-15) {
                continue;
            }
            idx_t v = saved_obj_var_indices[t];
            if (v == DConstants::INVALID_INDEX) {
                out.lo += c;
                out.hi += c;
                continue;
            }
            double lb = input.lower_bounds[v];
            double ub = input.upper_bounds[v];
            // Each end of this term separately, exactly as `DecideRowSignedRange` does
            // it on the constraint side. A negative coefficient swaps which end of the
            // variable's box feeds which end of the term, so an open ceiling on `v` can
            // open the term's FLOOR — sign has to be respected before blaming a side.
            bool lb_open = lb <= -1e20;
            bool ub_open = ub >= 1e20;
            bool lo_from_ub = c < 0.0;
            if (lo_from_ub ? ub_open : lb_open) {
                if (out.unbounded_var == DConstants::INVALID_INDEX) {
                    out.unbounded_var = v;
                }
                out.var_lo = -INF;
                out.lo = -INF;
            } else {
                double term_lo = lo_from_ub ? c * ub : c * lb;
                out.var_lo += term_lo;
                out.lo += term_lo;
            }
            if (lo_from_ub ? lb_open : ub_open) {
                if (out.unbounded_var == DConstants::INVALID_INDEX) {
                    out.unbounded_var = v;
                }
                out.var_hi = INF;
                out.hi = INF;
            } else {
                double term_hi = lo_from_ub ? c * lb : c * ub;
                out.var_hi += term_hi;
                out.hi += term_hi;
            }
        }
        return out;
    };

    // The family every per-row MIN/MAX auxiliary (z, z_g) reduces over: one entry per
    // row of the saved objective expression. Its `spread` is the Big-M these sites use.
    // Unlike the per-row constraint sites (where M bounds an expression against a fixed
    // RHS), an objective auxiliary is linked via (aux - expr) +/- M*y (>=|<=) +/- M, so
    // the deactivated branch must stay slack across the GLOBAL spread
    //   max_r exprmax_r  -  min_r exprmin_r
    // taking the SIGN of every coefficient against the variable's [lb, ub]. That is the
    // tight, data-driven value (a per-row range can under-estimate it when coefficient
    // signs differ across rows). Computed once and reused; an unbounded contributor has
    // no such value at all, and the query is refused rather than given a constant.
    bool row_family_cached = false;
    AuxRange row_family_range;
    auto row_family = [&]() -> const AuxRange & {
        if (!row_family_cached) {
            for (idx_t r = 0; r < num_rows; r++) {
                auto rr = saved_row_range(r);
                row_family_range.CoverRowSided(rr.lo, rr.hi, rr.var_lo, rr.var_hi,
                                               rr.unbounded_var);
            }
            row_family_cached = true;
        }
        return row_family_range;
    };
    auto compute_big_m = [&]() -> double {
        if (row_family().Unbounded()) {
            ThrowUnboundedAuxBigM(row_family(), var_names, "MIN/MAX");
        }
        return row_family().BigM();
    };

    //! Every hard site in this function scales its Big-M off the same row family, so
    //! they all face the same question and must answer it the same way: the lowering is
    //! available exactly when `compute_big_m()` would not throw. Evaluated once here so
    //! that a query cannot take the native arm for its inner aggregate and the lowered
    //! arm for its outer one, which would be two formulations for one objective.
    auto use_native = [&]() { return native_min_max.Use(row_family().Unbounded()); };

    //! This row's own expression range, as an AuxRange — tighter than the family's,
    //! and the box a `t` column pinned to that row deserves.
    auto row_range_for = [&](idx_t row) -> AuxRange {
        AuxRange range;
        auto rr = saved_row_range(row);
        range.CoverRowSided(rr.lo, rr.hi, rr.var_lo, rr.var_hi, rr.unbounded_var);
        return range;
    };

    // --- The native arm's two primitives -------------------------------------
    // Every hard site below has the same shape: an auxiliary that must equal the
    // extremum of a family of linear expressions. Lowered, that is one Big-M row per
    // member plus a `SUM(y) >= 1`. Natively it is one free column per member, pinned
    // to that member's expression, and one general constraint over them — no constant
    // to dominate anything, and so no requirement that any contributing variable be
    // bounded.

    //! Pin a fresh free column to the expression `link` describes and return it.
    //! `link` is built as the `(aux - expr)` half of a linking row, so its stored
    //! coefficients are already negated and `link.constant` holds the constant part:
    //! `t + link_terms = link.constant` is exactly `t = expr`.
    auto pin_native_column = [&](const MinMaxLinkRow &link, const AuxRange &range) -> int {
        // Boxed, not free. `t` stands for a known expression, so its range is the same
        // one the Big-M walk produces — and a free continuous column is a measured
        // performance cliff (the root simplex has no box to start from). It falls back
        // to free only when the range is genuinely underivable, which is exactly the
        // case the native path exists to answer at all.
        idx_t t_idx = AddGlobalContinuousAux(input, indexer, range, 0.0);
        SolverInput::RawConstraint pin;
        pin.indices.push_back((int)t_idx);
        pin.coefficients.push_back(1.0);
        link.AppendTo(pin);
        pin.sense = '=';
        pin.rhs = link.constant;
        pin.kind = ConstraintKind::STRUCTURAL;
        input.global_constraints.push_back(std::move(pin));
        return (int)t_idx;
    };

    //! `result = MIN/MAX(args)`, stated for the backend rather than encoded.
    auto emit_native_extremum = [&](idx_t result_col, vector<int> args, bool is_min) {
        GeneralConstraintSpec gc;
        gc.kind = is_min ? GeneralConstraintKind::MIN : GeneralConstraintKind::MAX;
        gc.result_column = (int)result_col;
        gc.argument_columns = std::move(args);
        input.general_constraints.push_back(std::move(gc));
    };


    // Accumulate one row of the saved objective expression into `link`, negated:
    // the linking row is `z - expr op bound`, so the expression's coefficients
    // enter with the opposite sign and its constant part lands on the bound.
    // `scale` carries the inner-AVG 1/n_g factor at the PER sites; 1.0 elsewhere.
    auto AddObjectiveRowTerms = [&](MinMaxLinkRow &link, idx_t row, double scale) {
        for (idx_t t = 0; t < saved_obj_var_indices.size(); t++) {
            double coeff = saved_obj_coefficients[t].Get(row) * scale;
            if (std::abs(coeff) < 1e-15) {
                continue;
            }
            idx_t v = saved_obj_var_indices[t];
            if (v == DConstants::INVALID_INDEX) {
                link.constant += coeff;
            } else {
                link.AddColumn((int)indexer.Get(v, row), -coeff);
            }
        }
    };

    //! The argument column for one row's member expression: the row's OWN column when
    //! the expression is nothing but that column, and a pinned auxiliary otherwise.
    //!
    //! `MAX(x)` over a decision variable is the common shape, and there `t = x` is a
    //! renaming: it cost one continuous column and one equality row PER DATA ROW to say
    //! nothing. Measured on `MAXIMIZE MAX(x)` at 15K rows, handing Gurobi the column
    //! itself instead of the copy took the solve from 0.97s to 0.29s for the same
    //! answer, because the copies survive into the general constraint's own expansion
    //! and presolve cannot substitute a column a general constraint reads.
    //!
    //! Only an EXACT renaming qualifies: one variable term, coefficient 1, no constant.
    //! Any coefficient (`MAX(2 * x)`) or constant (`MAX(x + 1)`) means the member is a
    //! genuine expression, and a general constraint relates columns, so that one still
    //! has to become a column. The box needs no separate test — a single unit term's
    //! derived range IS the variable's own box, asserted below rather than assumed.
    auto native_argument = [&](idx_t row, double scale) -> int {
        idx_t sole_var = DConstants::INVALID_INDEX;
        double sole_coeff = 0.0;
        bool renaming = true;
        for (idx_t t = 0; t < saved_obj_var_indices.size() && renaming; t++) {
            double coeff = saved_obj_coefficients[t].Get(row) * scale;
            if (std::abs(coeff) < 1e-15) {
                continue;
            }
            idx_t v = saved_obj_var_indices[t];
            if (v == DConstants::INVALID_INDEX || sole_var != DConstants::INVALID_INDEX) {
                renaming = false; // a constant part, or a second term
                break;
            }
            sole_var = v;
            sole_coeff = coeff;
        }
        if (renaming && sole_var != DConstants::INVALID_INDEX && sole_coeff == 1.0) {
            auto range = row_range_for(row);
            D_ASSERT(range.lo_unbounded || range.lo == input.lower_bounds[sole_var]);
            D_ASSERT(range.hi_unbounded || range.hi == input.upper_bounds[sole_var]);
            return (int)indexer.Get(sole_var, row);
        }
        MinMaxLinkRow link;
        AddObjectiveRowTerms(link, row, scale);
        return pin_native_column(link, row_range_for(row));
    };

    if (spec.per_inner_agg != ObjectiveAggregateType::NONE && !saved_obj_var_indices.empty() &&
        input.objective_num_groups > 0) {
        // ================================================================
        // PATH B: PER objective with nested OUTER(INNER(expr)) aggregate
        // ================================================================
        idx_t K = input.objective_num_groups;
        auto &row_groups = input.objective_row_group_ids;

        // Build group→rows CSR index once, reuse across phases.
        BuildGroupCSR(row_groups, K,
                      input.objective_group_offsets,
                      input.objective_group_row_ids);
        auto &obj_offsets = input.objective_group_offsets;
        auto &obj_flat_rows = input.objective_group_row_ids;
        auto group_size = [&](idx_t g) {
            return obj_offsets[g + 1] - obj_offsets[g];
        };

        // Clear per-row objective (auxiliaries become the objective)
        input.objective_coefficients.clear();
        input.objective_variable_indices.clear();

        // Phase A: Inner aggregate — produces K per-group values
        // These are either group sums (no aux) or z_g auxiliaries (inner MIN/MAX)
        bool inner_is_minmax = (spec.per_inner_agg == ObjectiveAggregateType::MIN_AGG || spec.per_inner_agg == ObjectiveAggregateType::MAX_AGG);
        bool inner_is_min = (spec.per_inner_agg == ObjectiveAggregateType::MIN_AGG);

        // group_value_indices[g] = solver variable index for group g's value
        // For inner SUM: not used (group sums go directly to outer as coefficients)
        // For inner MIN/MAX: index of z_g global variable
        vector<idx_t> group_value_indices(K);

        if (inner_is_minmax) {
            // Inner MIN/MAX: create z_g auxiliary per group
            bool inner_easy = spec.per_inner_is_easy;
            double M = compute_big_m();

            // Each z_g is an extremum over rows of its group, so every one of them is
            // boxed by the same per-row family.
            idx_t z_base = indexer.global_block_start + input.num_global_vars;
            for (idx_t g = 0; g < K; g++) {
                group_value_indices[g] =
                    AddGlobalContinuousAux(input, indexer, row_family(), 0.0); // obj set by outer phase
            }

            // Build a per-group active-rows CSR: drop rows whose every term coefficient
            // is zero. Mirrors PATH A's active_rows pre-filter (lines below). Without it
            // the easy path emits vacuous z_g op 0 rows and the hard path allocates an
            // indicator binary plus a Big-M row for each, then references them in the
            // sum_y >= 1 constraint — all wasted on rows that contribute nothing.
            vector<idx_t> active_offsets(K + 1, 0);
            vector<idx_t> active_flat_rows;
            active_flat_rows.reserve(obj_flat_rows.size());
            for (idx_t g = 0; g < K; g++) {
                active_offsets[g] = active_flat_rows.size();
                for (idx_t k = obj_offsets[g]; k < obj_offsets[g + 1]; k++) {
                    idx_t row = obj_flat_rows[k];
                    bool has_nonzero = false;
                    for (idx_t t = 0; t < saved_obj_var_indices.size(); t++) {
                        if (std::abs(saved_obj_coefficients[t][row]) >= 1e-15) {
                            has_nonzero = true;
                            break;
                        }
                    }
                    if (has_nonzero) active_flat_rows.push_back(row);
                }
            }
            active_offsets[K] = active_flat_rows.size();

            // For groups with no active rows, the original code emitted vacuous
            // z_g op 0 rows that — combined with the outer optimization direction —
            // implicitly pinned z_g at 0. Skipping those rows lets z_g float free,
            // so we instead pin z_g's bounds directly. Captured as a lambda so both
            // easy and hard branches use identical pinning logic.
            auto PinZGroupToZero = [&](idx_t g) {
                PinGlobalAux(input, indexer, group_value_indices[g], 0.0);
            };

            if (inner_easy) {
                // Easy: z_g >= expr_r (for MAX) or z_g <= expr_r (for MIN)
                char sense_char = inner_is_min ? '<' : '>';
                for (idx_t g = 0; g < K; g++) {
                    if (active_offsets[g] == active_offsets[g + 1]) {
                        PinZGroupToZero(g);
                        continue;
                    }
                    for (idx_t k = active_offsets[g]; k < active_offsets[g + 1]; k++) {
                        idx_t row = active_flat_rows[k];
                        MinMaxLinkRow link;
                        AddObjectiveRowTerms(link, row, 1.0);
                        SolverInput::RawConstraint rc;
                        rc.sense = sense_char;
                        rc.rhs = link.constant;
                        rc.indices.push_back((int)group_value_indices[g]);
                        rc.coefficients.push_back(1.0);
                        link.AppendTo(rc);
                        input.global_constraints.push_back(std::move(rc));
                    }
                }
            } else if (use_native()) {
                // Native inner: z_g = MIN/MAX over this group's row expressions.
                for (idx_t g = 0; g < K; g++) {
                    if (active_offsets[g] == active_offsets[g + 1]) {
                        PinZGroupToZero(g);
                        continue;
                    }
                    vector<int> args;
                    args.reserve(active_offsets[g + 1] - active_offsets[g]);
                    for (idx_t k = active_offsets[g]; k < active_offsets[g + 1]; k++) {
                        args.push_back(native_argument(active_flat_rows[k], 1.0));
                    }
                    emit_native_extremum(group_value_indices[g], std::move(args), inner_is_min);
                }
            } else {
                // Hard: per-row indicators per group, allocated only for active rows.
                idx_t first_y = z_base + K;
                idx_t num_active = active_flat_rows.size();
                for (idx_t r = 0; r < num_active; r++) {
                    AddGlobalBinaryAux(input, indexer, 0.0);
                }

                for (idx_t g = 0; g < K; g++) {
                    if (active_offsets[g] == active_offsets[g + 1]) {
                        PinZGroupToZero(g);
                        continue;
                    }
                    for (idx_t k = active_offsets[g]; k < active_offsets[g + 1]; k++) {
                        idx_t row = active_flat_rows[k];
                        idx_t active_idx = k; // position in active_flat_rows
                        MinMaxLinkRow link;
                        AddObjectiveRowTerms(link, row, 1.0);
                        SolverInput::RawConstraint rc;
                        rc.indices.push_back((int)group_value_indices[g]);
                        rc.coefficients.push_back(1.0);
                        link.AppendTo(rc);
                        idx_t y_idx = first_y + active_idx;
                        if (inner_is_min) {
                            // MINIMIZE MIN inner: z_g - expr_r - M*y_r >= -M
                            rc.indices.push_back((int)y_idx);
                            rc.coefficients.push_back(-M);
                            rc.sense = '>';
                            rc.rhs = -M + link.constant;
                        } else {
                            // MAXIMIZE MAX inner: z_g - expr_r + M*y_r <= M
                            rc.indices.push_back((int)y_idx);
                            rc.coefficients.push_back(M);
                            rc.sense = '<';
                            rc.rhs = M + link.constant;
                        }
                        input.global_constraints.push_back(std::move(rc));
                    }
                    // SUM(y) >= 1 per group
                    SolverInput::RawConstraint sum_y;
                    for (idx_t k = active_offsets[g]; k < active_offsets[g + 1]; k++) {
                        sum_y.indices.push_back((int)(first_y + k));
                        sum_y.coefficients.push_back(1.0);
                    }
                    sum_y.sense = '>';
                    sum_y.rhs = 1.0;
                    input.global_constraints.push_back(std::move(sum_y));
                }
            }
        }

        // Phase B: Outer aggregate — combines K group values into scalar objective
        bool outer_is_sum = (spec.per_outer_agg == ObjectiveAggregateType::SUM);
        bool outer_is_minmax = (spec.per_outer_agg == ObjectiveAggregateType::MIN_AGG || spec.per_outer_agg == ObjectiveAggregateType::MAX_AGG);
        bool outer_is_min = (spec.per_outer_agg == ObjectiveAggregateType::MIN_AGG);

        if (inner_is_minmax && outer_is_sum) {
            // Outer SUM: objective = sum of z_g's
            for (idx_t g = 0; g < K; g++) {
                input.global_obj_coeffs[group_value_indices[g] - indexer.global_block_start] = 1.0;
            }
        } else if (inner_is_minmax && outer_is_minmax) {
            // Outer MIN/MAX over z_g's: create global w auxiliary
            bool outer_easy = spec.per_outer_is_easy;

            // w is an extremum over the z_g's, and every z_g is boxed by the per-row
            // family, so w inherits that same box.
            idx_t w_idx = AddGlobalContinuousAux(input, indexer, row_family(), 1.0); // objective = w

            if (outer_easy) {
                // w >= z_g (for outer MAX) or w <= z_g (for outer MIN)
                char sense_char = outer_is_min ? '<' : '>';
                for (idx_t g = 0; g < K; g++) {
                    SolverInput::RawConstraint rc;
                    rc.sense = sense_char;
                    rc.rhs = 0.0;
                    rc.indices.push_back((int)w_idx);
                    rc.coefficients.push_back(1.0);
                    rc.indices.push_back((int)group_value_indices[g]);
                    rc.coefficients.push_back(-1.0);
                    input.global_constraints.push_back(std::move(rc));
                }
            } else if (use_native()) {
                // Native outer: w = MIN/MAX(z_g). The group values are already columns,
                // so this needs no pinning rows at all — the general constraint is the
                // whole formulation.
                vector<int> args;
                args.reserve(K);
                for (idx_t g = 0; g < K; g++) {
                    args.push_back((int)group_value_indices[g]);
                }
                emit_native_extremum(w_idx, std::move(args), outer_is_min);
            } else {
                // Hard outer: indicators over K groups
                idx_t first_u = w_idx + 1;
                for (idx_t g = 0; g < K; g++) {
                    AddGlobalBinaryAux(input, indexer, 0.0);
                }
                // Outer Big-M: compute_big_m() returns the global spread of the
                // objective expression (max_r exprmax - min_r exprmin), which bounds
                // the spread of (w - z_g) since each z_g lies within that range.
                double M_outer = compute_big_m();
                for (idx_t g = 0; g < K; g++) {
                    SolverInput::RawConstraint rc;
                    rc.indices.push_back((int)w_idx);
                    rc.coefficients.push_back(1.0);
                    rc.indices.push_back((int)group_value_indices[g]);
                    rc.coefficients.push_back(-1.0);
                    idx_t u_idx = first_u + g;
                    if (outer_is_min) {
                        // MINIMIZE MIN outer: w - z_g - M*u_g >= -M
                        rc.indices.push_back((int)u_idx);
                        rc.coefficients.push_back(-M_outer);
                        rc.sense = '>';
                        rc.rhs = -M_outer;
                    } else {
                        // MAXIMIZE MAX outer: w - z_g + M*u_g <= M
                        rc.indices.push_back((int)u_idx);
                        rc.coefficients.push_back(M_outer);
                        rc.sense = '<';
                        rc.rhs = M_outer;
                    }
                    input.global_constraints.push_back(std::move(rc));
                }
                SolverInput::RawConstraint sum_u;
                for (idx_t g = 0; g < K; g++) {
                    sum_u.indices.push_back((int)(first_u + g));
                    sum_u.coefficients.push_back(1.0);
                }
                sum_u.sense = '>';
                sum_u.rhs = 1.0;
                input.global_constraints.push_back(std::move(sum_u));
            }
        } else if (!inner_is_minmax && outer_is_sum) {
            if (spec.per_inner_was_avg) {
                // Inner AVG + Outer SUM: scale each row's coefficient by 1/n_g
                // SUM over groups of AVG(expr) = Σ_g (Σ_{r∈g} c_r * x_r) / n_g
                for (idx_t t = 0; t < saved_obj_var_indices.size(); t++) {
                    auto &col = saved_obj_coefficients[t].MutableDense();
                    for (idx_t row = 0; row < num_rows; row++) {
                        if (row_groups[row] != DConstants::INVALID_INDEX) {
                            idx_t g = row_groups[row];
                            col[row] /= static_cast<double>(group_size(g));
                        }
                    }
                }
            }
            // Restore (possibly scaled) objective coefficients
            input.objective_coefficients = std::move(saved_obj_coefficients);
            input.objective_variable_indices = std::move(saved_obj_var_indices);
        } else if (!inner_is_minmax && outer_is_minmax) {
            // Inner SUM + Outer MIN/MAX: compute per-group sums, then optimize over them
            // Create w auxiliary for outer MIN/MAX over group sums
            bool outer_easy = spec.per_outer_is_easy;

            // This w reduces over group SUMS, not over rows, so it does NOT share the
            // per-row family's box: a group sum leaves any single row's range as soon
            // as the group holds more than one row. Derive the box from the actual
            // per-group sums — each group's rows added up under the same inner-AVG
            // scale the pinning rows below use — which is both correct and tighter than
            // widening the per-row family by num_rows.
            // `scale` is positive, so a member's floor stays the sum's floor and an
            // infinite end carries through the addition on its own.
            AuxRange group_sum_family;
            for (idx_t g = 0; g < K; g++) {
                double scale = spec.per_inner_was_avg ? 1.0 / static_cast<double>(group_size(g)) : 1.0;
                double g_lo = 0.0, g_hi = 0.0, g_var_lo = 0.0, g_var_hi = 0.0;
                idx_t g_unbounded_var = DConstants::INVALID_INDEX;
                for (idx_t k = obj_offsets[g]; k < obj_offsets[g + 1]; k++) {
                    auto rr = saved_row_range(obj_flat_rows[k]);
                    g_lo += rr.lo * scale;
                    g_hi += rr.hi * scale;
                    g_var_lo += rr.var_lo * scale;
                    g_var_hi += rr.var_hi * scale;
                    if (g_unbounded_var == DConstants::INVALID_INDEX) {
                        g_unbounded_var = rr.unbounded_var;
                    }
                }
                group_sum_family.CoverRowSided(g_lo, g_hi, g_var_lo, g_var_hi, g_unbounded_var);
            }
            idx_t w_idx = AddGlobalContinuousAux(input, indexer, group_sum_family, 1.0); // objective = w

            // For each group g: w >= (or <=) sum_g(coeffs * x)
            // sum_g = Σ_{r ∈ group_g} Σ_t coeff_t_r * x_{r,var_t}
            if (outer_easy) {
                char sense_char = outer_is_min ? '<' : '>';
                bool any_group_emitted = false;
                for (idx_t g = 0; g < K; g++) {
                    double scale = spec.per_inner_was_avg ? 1.0 / static_cast<double>(group_size(g)) : 1.0;
                    MinMaxLinkRow link;
                    for (idx_t k = obj_offsets[g]; k < obj_offsets[g + 1]; k++) {
                        AddObjectiveRowTerms(link, obj_flat_rows[k], scale);
                    }
                    // Skip vacuous w op 0 rows: outer MIN/MAX of group sums settles
                    // dominated zero-sum groups via the optimization direction itself.
                    // A group left holding only a constant still bounds w, so it stays.
                    if (link.HasNoColumns() && std::abs(link.constant) < 1e-15) continue;
                    SolverInput::RawConstraint rc;
                    rc.sense = sense_char;
                    rc.rhs = link.constant;
                    rc.indices.push_back((int)w_idx);
                    rc.coefficients.push_back(1.0);
                    link.AppendTo(rc);
                    input.global_constraints.push_back(std::move(rc));
                    any_group_emitted = true;
                }
                if (!any_group_emitted) {
                    // Every group is identically zero — pin w to 0 so outer
                    // optimization doesn't push the otherwise-unconstrained w to ±∞.
                    PinGlobalAux(input, indexer, w_idx, 0.0);
                }
            } else if (use_native()) {
                // Native outer over group SUMS: one free column per group, pinned to
                // that group's sum, then w = MIN/MAX over them. The easy arm above
                // skips a group that is identically zero because the optimization
                // direction settles it; here every group is a real member of the
                // extremum, so an all-zero group participates as the constant 0 it is.
                //
                // Every `t_g` is boxed by the family range over group sums — looser than
                // a per-group box, but sound (it covers every group) and finite whenever
                // any of them is. That is `group_sum_family`, already computed above for
                // `w`: the two reduce over the same group sums, so recomputing it here
                // could only ever produce the same value or drift away from it.
                vector<int> args;
                args.reserve(K);
                for (idx_t g = 0; g < K; g++) {
                    double scale = spec.per_inner_was_avg ? 1.0 / static_cast<double>(group_size(g)) : 1.0;
                    MinMaxLinkRow link;
                    for (idx_t k = obj_offsets[g]; k < obj_offsets[g + 1]; k++) {
                        AddObjectiveRowTerms(link, obj_flat_rows[k], scale);
                    }
                    args.push_back(pin_native_column(link, group_sum_family));
                }
                emit_native_extremum(w_idx, std::move(args), outer_is_min);
            } else {
                // Hard outer: indicators over K groups
                // Outer Big-M over group SUMS: a group sum spans at most num_rows
                // times the per-row spread, so the global spread (compute_big_m())
                // scaled by num_rows bounds the spread of (w - group_sum).
                double M_outer = compute_big_m() * num_rows;
                idx_t first_u = w_idx + 1;
                for (idx_t g = 0; g < K; g++) {
                    AddGlobalBinaryAux(input, indexer, 0.0);
                }
                for (idx_t g = 0; g < K; g++) {
                    double scale = spec.per_inner_was_avg ? 1.0 / static_cast<double>(group_size(g)) : 1.0;
                    MinMaxLinkRow link;
                    for (idx_t k = obj_offsets[g]; k < obj_offsets[g + 1]; k++) {
                        AddObjectiveRowTerms(link, obj_flat_rows[k], scale);
                    }
                    SolverInput::RawConstraint rc;
                    rc.indices.push_back((int)w_idx);
                    rc.coefficients.push_back(1.0);
                    link.AppendTo(rc);
                    idx_t u_idx = first_u + g;
                    if (outer_is_min) {
                        rc.indices.push_back((int)u_idx);
                        rc.coefficients.push_back(-M_outer);
                        rc.sense = '>';
                        rc.rhs = -M_outer + link.constant;
                    } else {
                        rc.indices.push_back((int)u_idx);
                        rc.coefficients.push_back(M_outer);
                        rc.sense = '<';
                        rc.rhs = M_outer + link.constant;
                    }
                    input.global_constraints.push_back(std::move(rc));
                }
                SolverInput::RawConstraint sum_u;
                for (idx_t g = 0; g < K; g++) {
                    sum_u.indices.push_back((int)(first_u + g));
                    sum_u.coefficients.push_back(1.0);
                }
                sum_u.sense = '>';
                sum_u.rhs = 1.0;
                input.global_constraints.push_back(std::move(sum_u));
            }
        }
    } else if (spec.flat_agg != ObjectiveAggregateType::NONE && !saved_obj_var_indices.empty()) {
        // ================================================================
        // PATH A: Non-PER flat MIN/MAX objective (existing behavior)
        // ================================================================
        bool is_min_agg = (spec.flat_agg == ObjectiveAggregateType::MIN_AGG);
        bool is_easy = spec.flat_is_easy;

        // Compute active rows: pass WHEN, and have at least one nonzero coefficient.
        // - Easy path: skipping inactive rows just avoids vacuous linking constraints
        //   (the existing code already skipped zero coefficients individually, but still
        //   emitted an empty linking row).
        // - Hard path: this also reduces the number of indicator binaries and the size
        //   of the SUM(y) >= 1 constraint sent to the solver.
        vector<idx_t> active_rows;
        active_rows.reserve(num_rows);
        for (idx_t row = 0; row < num_rows; row++) {
            if (spec.has_when && !spec.when_mask[row]) continue;
            bool has_nonzero = false;
            for (idx_t t = 0; t < saved_obj_var_indices.size(); t++) {
                if (std::abs(saved_obj_coefficients[t].Get(row)) >= 1e-15) {
                    has_nonzero = true;
                    break;
                }
            }
            if (has_nonzero) {
                active_rows.push_back(row);
            }
        }
        if (active_rows.empty()) {
            throw InvalidInputException(
                "MIN/MAX objective has no active rows after WHEN filtering and zero-coefficient "
                "elimination. The auxiliary variable would have no pinning constraints, "
                "making the optimization unbounded or vacuous.");
        }

        // z is the extremum over the objective's rows, so the per-row family boxes it.
        idx_t z_idx = AddGlobalContinuousAux(input, indexer, row_family(), 1.0); // objective = z

        // Clear per-row objective (z is the sole objective term now)
        input.objective_coefficients.clear();
        input.objective_variable_indices.clear();

        if (is_easy) {
            char sense_char = is_min_agg ? '<' : '>';
            for (idx_t row : active_rows) {
                MinMaxLinkRow link;
                AddObjectiveRowTerms(link, row, 1.0);
                SolverInput::RawConstraint rc;
                rc.sense = sense_char;
                rc.rhs = link.constant;
                rc.indices.push_back((int)z_idx);
                rc.coefficients.push_back(1.0);
                link.AppendTo(rc);
                input.global_constraints.push_back(std::move(rc));
            }
        } else if (use_native()) {
            // Native flat: z = MIN/MAX over the objective's active row expressions.
            vector<int> args;
            args.reserve(active_rows.size());
            for (idx_t row : active_rows) {
                args.push_back(native_argument(row, 1.0));
            }
            emit_native_extremum(z_idx, std::move(args), is_min_agg);
        } else {
            double M = compute_big_m();

            // Allocate one indicator binary per ACTIVE row (not per total row).
            idx_t first_y_idx = z_idx + 1;
            idx_t num_active = active_rows.size();
            for (idx_t r = 0; r < num_active; r++) {
                AddGlobalBinaryAux(input, indexer, 0.0);
            }

            for (idx_t a = 0; a < active_rows.size(); a++) {
                idx_t row = active_rows[a];
                MinMaxLinkRow link;
                AddObjectiveRowTerms(link, row, 1.0);
                SolverInput::RawConstraint rc;
                rc.indices.push_back((int)z_idx);
                rc.coefficients.push_back(1.0);
                link.AppendTo(rc);
                idx_t y_idx = first_y_idx + a;
                if (is_min_agg) {
                    rc.indices.push_back((int)y_idx);
                    rc.coefficients.push_back(-M);
                    rc.sense = '>';
                    rc.rhs = -M + link.constant;
                } else {
                    rc.indices.push_back((int)y_idx);
                    rc.coefficients.push_back(M);
                    rc.sense = '<';
                    rc.rhs = M + link.constant;
                }
                input.global_constraints.push_back(std::move(rc));
            }

            SolverInput::RawConstraint sum_y;
            for (idx_t a = 0; a < active_rows.size(); a++) {
                sum_y.indices.push_back((int)(first_y_idx + a));
                sum_y.coefficients.push_back(1.0);
            }
            sum_y.sense = '>';
            sum_y.rhs = 1.0;
            input.global_constraints.push_back(std::move(sum_y));
        }
    }
}

// --- Composed (additive) MIN/MAX --------------------------------------------
// A composed clause mixes reducers additively (`SUM(a) + MAX(b) <= K`). Each
// MIN/MAX term becomes a global auxiliary z_k pinned per row; SUM/AVG terms fold
// straight into the outer row (constraint) or into the objective coefficients.
// The two entry points below share that auxiliary layer and differ only in what
// they do with the terms afterwards.

//! Accumulate one row of a composed term's inner expression into `link`, negated:
//! the pinning row is `z - expr op bound`, so the expression's coefficients enter
//! with the opposite sign and its constant part lands on the bound.
static void AddComposedRowTerms(const VarIndexer &indexer, MinMaxLinkRow &link,
                                const vector<DecideTerm> &inner_terms,
                                const vector<vector<double>> &per_term_coefs, idx_t row) {
    for (idx_t it = 0; it < inner_terms.size(); it++) {
        double coeff = per_term_coefs[it][row];
        idx_t v = inner_terms[it].variable_index;
        if (v == DConstants::INVALID_INDEX) {
            link.constant += coeff;
        } else {
            link.AddColumn((int)indexer.Get(v, row), -coeff);
        }
    }
}

//! Shared hard-direction indicator layer for one composed MIN/MAX term whose
//! global auxiliary is `z_idx`. The caller emits the base one-sided envelope pin
//! (z >= inner for MAX / z <= inner for MIN) for BOTH directions; that alone
//! suffices for the easy direction (the outer pressure drives z to the extreme).
//! The hard direction adds, per active row, a binary y_i, a SUM(y_i) >= 1 pin,
//! and a Big-M link on the *opposite* envelope side so z is pinned to the actual
//! MIN/MAX rather than floating:
//!   MAX: z <= inner_i + M(1 - y_i)  ->  z - sum(c*var) + M*y_i <= M + const
//!   MIN: z >= inner_i - M(1 - y_i)  ->  z - sum(c*var) - M*y_i >= -M + const
//! M is the signed spread of `inner` over the term's active rows (identical
//! formula to the flat MIN/MAX objective's: global_max - global_min), which always
//! dominates |z - inner_i|; constant inner terms cancel in the spread. This mirrors
//! the flat (non-composed) hard MIN/MAX emission so both share one M model.
//! The family a composed MIN/MAX term reduces over: one entry per row the term's
//! filter admits. Feeds both the term's auxiliary box and the hard direction's Big-M,
//! so the two can never disagree about the term's reach.
//!
//! A term admitting no rows yields the seeded [0,0], which pins its auxiliary at 0 —
//! the same settlement the PER sites give a group with no active rows, and better than
//! letting an auxiliary with no pinning rows float on the outer pressure.
static AuxRange ComposedTermRange(const SolverInput &input, const vector<DecideTerm> &inner_terms,
                                  const vector<vector<double>> &per_term_coefs,
                                  const vector<bool> &filter_mask) {
    AuxRange range;
    for (idx_t row = 0; row < input.num_rows; row++) {
        if (!filter_mask[row]) {
            continue;
        }
        double row_lo = 0.0, row_hi = 0.0, var_lo = 0.0, var_hi = 0.0;
        for (idx_t it = 0; it < inner_terms.size(); it++) {
            double c = per_term_coefs[it][row];
            if (std::abs(c) < 1e-15) {
                continue;
            }
            idx_t v = inner_terms[it].variable_index;
            if (v == DConstants::INVALID_INDEX) {
                // A constant reaches the auxiliary's box (it is pinned against the whole
                // expression) but cancels in the spread a Big-M row slackens.
                row_lo += c;
                row_hi += c;
                continue;
            }
            double lb = input.lower_bounds[v];
            double ub = input.upper_bounds[v];
            if (ub >= 1e20 || lb <= -1e20) {
                range.MarkUnbounded(v);
                continue;
            }
            double term_lo = (c > 0.0) ? c * lb : c * ub;
            double term_hi = (c > 0.0) ? c * ub : c * lb;
            var_lo += term_lo;
            var_hi += term_hi;
            row_lo += term_lo;
            row_hi += term_hi;
        }
        range.CoverRow(row_lo, row_hi, var_lo, var_hi);
    }
    return range;
}

static void EmitComposedHardMinMaxIndicators(SolverInput &input, const VarIndexer &indexer,
                                             idx_t z_idx, bool is_max,
                                             const vector<DecideTerm> &inner_terms,
                                             const vector<vector<double>> &per_term_coefs,
                                             const vector<bool> &filter_mask,
                                             const string &label, const vector<string> &var_names,
                                             NativeConstructPolicy native_min_max) {
    idx_t num_rows = input.num_rows;

    // The arm this term takes, decided by the one thing that separates them: whether the
    // lowering has a Big-M to use. Computed before either arm so both read the same
    // range rather than each deriving its own.
    AuxRange composed_range = ComposedTermRange(input, inner_terms, per_term_coefs, filter_mask);

    if (native_min_max.Use(composed_range.Unbounded())) {
        // Native: z = MIN/MAX over this term's active row expressions. One column per
        // row pinned to that row's expression, then one general constraint — no Big-M,
        // so no contributing variable has to be bounded. The columns are still BOXED by
        // the term's derived range wherever one exists: a free continuous column is a
        // measured performance cliff, and only a genuinely underivable range earns one.
        const AuxRange &term_range = composed_range;
        vector<int> args;
        for (idx_t row = 0; row < num_rows; row++) {
            if (!filter_mask[row]) continue;

            // A member that is exactly one variable with coefficient 1 and no constant
            // is a renaming: `t = x` would add a column and an equality row per data row
            // to say nothing, and a general constraint's own expansion keeps the copy
            // alive through presolve. Hand the general constraint that column instead.
            {
                idx_t sole_var = DConstants::INVALID_INDEX;
                double sole_coeff = 0.0;
                double constant = 0.0;
                idx_t term_count = 0;
                for (idx_t it = 0; it < inner_terms.size(); it++) {
                    double coeff = per_term_coefs[it][row];
                    idx_t v = inner_terms[it].variable_index;
                    if (v == DConstants::INVALID_INDEX) {
                        constant += coeff;
                    } else if (coeff != 0.0) {
                        term_count++;
                        sole_var = v;
                        sole_coeff = coeff;
                    }
                }
                if (term_count == 1 && sole_coeff == 1.0 && constant == 0.0) {
                    args.push_back((int)indexer.Get(sole_var, row));
                    continue;
                }
            }

            MinMaxLinkRow row_terms;
            AddComposedRowTerms(indexer, row_terms, inner_terms, per_term_coefs, row);

            idx_t t_idx = AddGlobalContinuousAux(input, indexer, term_range, 0.0, label);

            // `row_terms` is the `(z - expr)` half of a linking row, so its stored
            // coefficients are negated: `t + row_terms = row_terms.constant` is `t = expr`.
            SolverInput::RawConstraint pin;
            pin.indices.push_back((int)t_idx);
            pin.coefficients.push_back(1.0);
            row_terms.AppendTo(pin);
            pin.sense = '=';
            pin.rhs = row_terms.constant;
            pin.kind = ConstraintKind::STRUCTURAL;
            input.global_constraints.push_back(std::move(pin));

            args.push_back((int)t_idx);
        }
        if (args.empty()) {
            return; // no active row: the caller already pinned z
        }
        GeneralConstraintSpec gc;
        gc.kind = is_max ? GeneralConstraintKind::MAX : GeneralConstraintKind::MIN;
        gc.result_column = (int)z_idx;
        gc.argument_columns = std::move(args);
        input.general_constraints.push_back(std::move(gc));
        return;
    }

    const AuxRange &range = composed_range;
    if (range.Unbounded()) {
        ThrowUnboundedAuxBigM(range, var_names, "MIN/MAX");
    }
    double M = range.BigM();

    SolverInput::RawConstraint sum_y;
    for (idx_t row = 0; row < num_rows; row++) {
        if (!filter_mask[row]) continue;
        idx_t y_idx = AddGlobalBinaryAux(input, indexer, 0.0, label);

        sum_y.indices.push_back((int)y_idx);
        sum_y.coefficients.push_back(1.0);

        MinMaxLinkRow row_terms;
        AddComposedRowTerms(indexer, row_terms, inner_terms, per_term_coefs, row);
        SolverInput::RawConstraint link;
        link.indices.push_back((int)z_idx);
        link.coefficients.push_back(1.0);
        row_terms.AppendTo(link);
        if (is_max) {
            link.indices.push_back((int)y_idx);
            link.coefficients.push_back(M);
            link.sense = '<';
            link.rhs = M + row_terms.constant;
        } else {
            link.indices.push_back((int)y_idx);
            link.coefficients.push_back(-M);
            link.sense = '>';
            link.rhs = -M + row_terms.constant;
        }
        link.kind = ConstraintKind::USER_PARAMETER;
        input.global_constraints.push_back(std::move(link));
    }
    sum_y.sense = '>';
    sum_y.rhs = 1.0;
    sum_y.kind = ConstraintKind::USER_PARAMETER;
    input.global_constraints.push_back(std::move(sum_y));
}

//! The auxiliary layer both composed paths share: one global z_k per MIN/MAX term,
//! its base one-sided envelope pin per active row, and — for the hard direction —
//! the indicator layer that pins z_k to the actual extremum. Fills in `z_idx` on
//! every MIN/MAX term, which the caller then references from the outer row or the
//! objective.
static void EmitComposedMinMaxAuxiliaries(SolverInput &input, const VarIndexer &indexer,
                                          vector<ComposedMinMaxTermData> &terms,
                                          const vector<string> &var_names, NativeConstructPolicy native_min_max) {
    idx_t num_rows = input.num_rows;

    // Allocate global z_k for each MIN/MAX term. Both directions supported:
    // hard terms get the indicator layer emitted after the base envelope pin.
    for (auto &ta : terms) {
        if (!ta.is_minmax) continue;
        // The label names the z through the global label channel, so a diagnosis
        // renders `MAX(x)` rather than an internal column name.
        ta.z_idx = AddGlobalContinuousAux(
            input, indexer,
            ComposedTermRange(input, (*ta.inner_terms), ta.per_term_coefs, ta.filter_mask), 0.0,
            ta.label);
    }

    // Emit the base one-sided envelope pin for each MIN/MAX term (both
    // directions): MAX → z_k >= inner_expr per row (z_k >= max), MIN →
    // z_k <= inner_expr per row (z_k <= min). For the easy direction the
    // outer pressure drives z_k to the extreme; the hard direction adds an
    // indicator layer below to pin z_k to the actual MIN/MAX.
    for (auto &ta : terms) {
        if (!ta.is_minmax) continue;
        bool is_max = (ta.agg_name == "max");
        char sense = is_max ? '>' : '<';
        for (idx_t row = 0; row < num_rows; row++) {
            if (!ta.filter_mask[row]) continue;
            MinMaxLinkRow link;
            AddComposedRowTerms(indexer, link, (*ta.inner_terms), ta.per_term_coefs, row);
            SolverInput::RawConstraint rc;
            rc.indices.push_back((int)ta.z_idx);
            rc.coefficients.push_back(1.0);
            link.AppendTo(rc);
            rc.sense = sense;
            rc.rhs = link.constant;
            input.global_constraints.push_back(std::move(rc));
        }
    }

    // Hard-direction terms: add the indicator layer so z_k is pinned to the
    // actual MIN/MAX (the outer pressure pushes z_k the "wrong" way, so the
    // base envelope pin alone would let it float).
    for (auto &ta : terms) {
        if (!ta.is_minmax || ta.is_easy) continue;
        EmitComposedHardMinMaxIndicators(input, indexer, ta.z_idx, ta.agg_name == "max",
                                         (*ta.inner_terms), ta.per_term_coefs,
                                         ta.filter_mask, ta.label, var_names, native_min_max);
    }
}

//! Number of rows an AVG term divides by: its filtered row count. Zero means the
//! term is empty and contributes nothing (the caller skips it).
static idx_t ComposedAvgDivisor(const ComposedMinMaxTermData &ta, idx_t num_rows) {
    idx_t cnt = 0;
    for (idx_t r = 0; r < num_rows; r++) {
        if (ta.filter_mask[r]) cnt++;
    }
    return cnt;
}

void LinearizeComposedMinMaxConstraint(SolverInput &input, const VarIndexer &indexer,
                                       vector<ComposedMinMaxTermData> &terms, double rhs_val,
                                       ExpressionType outer_cmp, idx_t source_clause_id,
                                       const vector<string> &var_names, NativeConstructPolicy native_min_max) {
    idx_t num_rows = input.num_rows;
    EmitComposedMinMaxAuxiliaries(input, indexer, terms, var_names, native_min_max);

    // Build the outer composed RawConstraint
    std::unordered_map<int, double> outer_accum;
    double outer_rhs = rhs_val;
    for (auto &ta : terms) {
        if (ta.is_minmax) {
            outer_accum[(int)ta.z_idx] += (double)ta.sign * ta.scale;
            continue;
        }
        // SUM/AVG term. For AVG, divide by filtered row count.
        double avg_divisor = 1.0;
        if (ta.agg_name == "avg") {
            idx_t cnt = ComposedAvgDivisor(ta, num_rows);
            if (cnt == 0) {
                // Empty aggregate — contributes 0; skip.
                continue;
            }
            avg_divisor = static_cast<double>(cnt);
        }
        for (idx_t it = 0; it < (*ta.inner_terms).size(); it++) {
            auto &inner_t = (*ta.inner_terms)[it];
            for (idx_t row = 0; row < num_rows; row++) {
                if (!ta.filter_mask[row]) continue;
                double coef = ta.per_term_coefs[it][row] * (double)ta.sign * ta.scale / avg_divisor;
                if (inner_t.variable_index == DConstants::INVALID_INDEX) {
                    outer_rhs -= coef;
                } else {
                    int abs_idx = (int)indexer.Get(inner_t.variable_index, row);
                    outer_accum[abs_idx] += coef;
                }
            }
        }
    }

    SolverInput::RawConstraint outer;
    for (auto &p : outer_accum) {
        if (p.second != 0.0) {
            outer.indices.push_back(p.first);
            outer.coefficients.push_back(p.second);
        }
    }
    switch (outer_cmp) {
    case ExpressionType::COMPARE_LESSTHANOREQUALTO:
    case ExpressionType::COMPARE_LESSTHAN:
        outer.sense = '<';
        outer.rhs = outer_rhs;
        break;
    case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
    case ExpressionType::COMPARE_GREATERTHAN:
        outer.sense = '>';
        outer.rhs = outer_rhs;
        break;
    default:
        throw InternalException("Composed MIN/MAX: unexpected comparison type.");
    }
    outer.kind = ConstraintKind::USER_PARAMETER;
    outer.shape = ElasticShape::SHARED_SCALAR;
    outer.source_clause_id = source_clause_id;
    outer.repair_group_id = input.constraints.size() + input.global_constraints.size();
    input.global_constraints.push_back(std::move(outer));
}

void LinearizeComposedMinMaxObjective(SolverInput &input, const VarIndexer &indexer,
                                      vector<ComposedMinMaxTermData> &terms,
                                      const vector<string> &var_names, NativeConstructPolicy native_min_max) {
    idx_t num_rows = input.num_rows;

    // Clear any existing objective terms — the placeholder constant produced
    // none, but be defensive in case other paths populated them.
    input.objective_coefficients.clear();
    input.objective_variable_indices.clear();

    EmitComposedMinMaxAuxiliaries(input, indexer, terms, var_names, native_min_max);

    // Populate objective coefficients. For MIN/MAX terms, the obj coef on z_k
    // is ta.sign (i.e., sign×1.0); set via global_obj_coeffs. For SUM/AVG
    // terms, accumulate per-row linear coefficients into objective_coefficients
    // keyed by decide variable.
    // Accumulator: decide_var_index -> per-row coefficient vector.
    std::unordered_map<idx_t, vector<double>> obj_coef_accum;
    for (auto &ta : terms) {
        if (ta.is_minmax) {
            // The z_k's obj coef is ta.sign (the MIN/MAX term's sign in the additive sum).
            idx_t gslot = ta.z_idx - indexer.global_block_start;
            input.global_obj_coeffs[gslot] = (double)ta.sign * ta.scale;
            continue;
        }
        double avg_divisor = 1.0;
        if (ta.agg_name == "avg") {
            idx_t cnt = ComposedAvgDivisor(ta, num_rows);
            if (cnt == 0) continue;
            avg_divisor = (double)cnt;
        }
        for (idx_t it = 0; it < (*ta.inner_terms).size(); it++) {
            auto &inner_t = (*ta.inner_terms)[it];
            if (inner_t.variable_index == DConstants::INVALID_INDEX) continue;
            auto &dst = obj_coef_accum[inner_t.variable_index];
            if (dst.empty()) dst.assign(num_rows, 0.0);
            for (idx_t row = 0; row < num_rows; row++) {
                if (!ta.filter_mask[row]) continue;
                dst[row] += ta.per_term_coefs[it][row] * (double)ta.sign * ta.scale / avg_divisor;
            }
        }
    }
    for (auto &p : obj_coef_accum) {
        input.objective_variable_indices.push_back(p.first);
        input.objective_coefficients.push_back(CoefficientColumn::FromVector(std::move(p.second)));
    }
}

} // namespace duckdb
