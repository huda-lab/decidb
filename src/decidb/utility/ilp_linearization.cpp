#include "duckdb/decidb/ilp_linearization.hpp"
#include "duckdb/common/exception.hpp"

#include <algorithm>
#include <cmath>

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

void LinearizeMinMaxIndicators(vector<EvaluatedConstraint> &constraints,
                               const vector<double> &lower_bounds,
                               const vector<double> &upper_bounds, idx_t num_rows) {
    vector<EvaluatedConstraint> new_constraints;
    for (auto &ec : constraints) {
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
    constraints = std::move(new_constraints);
}

} // namespace duckdb
