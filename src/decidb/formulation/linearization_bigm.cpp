//===----------------------------------------------------------------------===//
//                         DecidB
//
// src/decidb/formulation/linearization_bigm.cpp
//
// Data-driven Big-M sizing and the per-row range walks it rests on. See ilp_linearization.cpp.
//
//===----------------------------------------------------------------------===//
#include "duckdb/decidb/formulation/ilp_linearization.hpp"
#include "duckdb/common/exception.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include "duckdb/decidb/formulation/ilp_linearization_internal.hpp"

namespace duckdb {

using namespace decide_linearize; // NOLINT: internal DECIDE linearization helpers

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

double decide_linearize::DecideRowFixedLhsOffset(const vector<idx_t> &variable_indices,
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
double decide_linearize::DecideRowEffectiveBound(const EvaluatedConstraint &ec, idx_t row) {
    double rhs = ec.rhs_values.IsUniform() ? ec.rhs_values.UniformValue() : ec.rhs_values.Get(row);
    return rhs - DecideRowFixedLhsOffset(ec.variable_indices, ec.row_coefficients, row);
}

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
void decide_linearize::DecideRowSignedRange(const EvaluatedConstraint &ec, idx_t row,
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
[[noreturn]] void decide_linearize::ThrowUnboundedBigMNaming(idx_t bad, const vector<string> &var_names,
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

//! Locate the blame for a row Big-M: the first contributing decision variable whose box
//! is open, and so a column the user can actually bound. The indicators a rewrite created
//! are skipped — they are ours, not theirs, and they are binary anyway.
//!
//! Separate from the throw because a caller working in FLAT columns still has to name a
//! decide variable, and the range walk that discovers the openness there
//! (`DecideRowSignedRange`) reports an infinity rather than a culprit.
idx_t decide_linearize::FindUnboundedContributor(const EvaluatedConstraint &ec,
                                      const vector<double> &lower_bounds,
                                      const vector<double> &upper_bounds) {
    for (idx_t t = 0; t < ec.variable_indices.size(); t++) {
        idx_t v = ec.variable_indices[t];
        if (v == DConstants::INVALID_INDEX || v == ec.abs_aux_idx) {
            continue;
        }
        if (v < upper_bounds.size() && v < lower_bounds.size() &&
            (upper_bounds[v] >= 1e20 || lower_bounds[v] <= -1e20)) {
            return v;
        }
    }
    return DConstants::INVALID_INDEX;
}

[[noreturn]] static void ThrowUnboundedBigM(const EvaluatedConstraint &ec, const vector<double> &lower_bounds,
                                            const vector<double> &upper_bounds,
                                            const vector<string> &var_names, const char *construct) {
    ThrowUnboundedBigMNaming(FindUnboundedContributor(ec, lower_bounds, upper_bounds), var_names,
                             construct);
}

//! The auxiliary-family twin of ThrowUnboundedBigM: a MIN/MAX auxiliary is linked to
//! its expression by `(aux - expr) +/- M*y`, so M has to stay slack across the whole
//! family's span. An unbounded contributor leaves no such M, and the same rule
//! applies — refuse rather than guess.
[[noreturn]] static void ThrowUnboundedAuxBigM(const AuxRange &range, const vector<string> &var_names,
                                               const char *construct) {
    // The range already recorded which variable opened it, so there is nothing to search.
    ThrowUnboundedBigMNaming(range.unbounded_var, var_names, construct);
}

//! Which construct asked for this Big-M, for the refusal above. Read off the
//! indicator the rewrite attached, so the message names what the user wrote.
static const char *DescribeBigMConstruct(const EvaluatedConstraint &ec) {
    if (ec.ne_clause_idx != DConstants::INVALID_INDEX) {
        return "<>";
    }
    if (ec.abs_aux_idx != DConstants::INVALID_INDEX) {
        return "ABS";
    }
    return "MIN/MAX";
}

double DecideTightPerRowBigM(const EvaluatedConstraint &ec, const FormulationBox &box,
                             idx_t num_rows,
                             const vector<string> &var_names) {
    const vector<double> &lower_bounds = box.lower;
    const vector<double> &upper_bounds = box.upper;
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
        if (!ec.minmax_agg_type.empty() || ec.ne_clause_idx != DConstants::INVALID_INDEX ||
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

} // namespace duckdb
