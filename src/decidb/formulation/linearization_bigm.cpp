//===----------------------------------------------------------------------===//
//                         DeciDB
//
// src/decidb/formulation/linearization_bigm.cpp
//
// Data-driven Big-M sizing and the per-row range walks it rests on. See ilp_linearization.cpp.
//
//===----------------------------------------------------------------------===//
#include "duckdb/decidb/formulation/ilp_linearization.hpp"
#include "duckdb/common/decide_numeric_policy.hpp"
#include "duckdb/common/decide_value_messages.hpp"
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
                          idx_t skip_idx,
                          const InstanceBox *instances) {
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
        double lb = instances ? instances->LowerOfRow(v, row, lower_bounds) : lower_bounds[v];
        double ub = instances ? instances->UpperOfRow(v, row, upper_bounds) : upper_bounds[v];
        if (DecideIsOpenAbove(ub) || DecideIsOpenBelow(lb)) {
            has_unbounded = true;
            continue;
        }
        sum += coef * std::max(std::abs(lb), std::abs(ub));
    }
    return sum;
}

double decide_linearize::DecideRowFixedLhsOffset(const vector<idx_t> &variable_indices,
                                      const vector<CoefficientColumn> &row_coefficients,
                                      idx_t row, const string &written) {
    double offset = 0.0;
    for (idx_t t = 0; t < variable_indices.size(); t++) {
        if (variable_indices[t] == DConstants::INVALID_INDEX) {
            AddCoefficient(offset, row_coefficients[t].Get(row), written);
        }
    }
    return offset;
}

//! The bound a row actually imposes: its right-hand side less the constant part of
//! its left-hand side. This is the value a Big-M has to dominate, so it is also the
//! value a caller must classify before asking for an M over it.
double decide_linearize::DecideRowEffectiveBound(const EvaluatedConstraint &ec, idx_t row, const string &written) {
    double rhs = ec.rhs_values.IsUniform() ? ec.rhs_values.UniformValue() : ec.rhs_values.Get(row);
    return rhs - DecideRowFixedLhsOffset(ec.variable_indices, ec.row_coefficients, row, written);
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
                                 double &out_hi, idx_t skip_idx, const InstanceBox *instances) {
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
        double lb = instances ? instances->LowerOfRow(v, row, lower_bounds) : lower_bounds[v];
        double ub = instances ? instances->UpperOfRow(v, row, upper_bounds) : upper_bounds[v];
        bool lb_unbounded = DecideIsOpenBelow(lb);
        bool ub_unbounded = DecideIsOpenAbove(ub);
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
//! The callers differ only in how they locate the column to blame, so the words the
//! user reads live here, once. `open` says whether a column of the row has an open box
//! (an auxiliary's counts); `bad` is then that column, or INVALID_INDEX when the open
//! bound could not be attributed to any named column. With no open column the reach
//! overflowed a DOUBLE: nothing is missing a bound, and the refusal names `written`, the
//! clause the row belongs to, as every number too large for a DOUBLE does (AddCoefficient).
[[noreturn]] void decide_linearize::ThrowUnboundedBigMNaming(bool open, idx_t bad, const vector<string> &var_names,
                                                             const char *construct, const string &written) {
    if (!open) {
        throw InvalidInputException(DecideInvalidValueMessage(DecideValueRole::TOO_LARGE, written));
    }
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
            (DecideIsOpenAbove(upper_bounds[v]) || DecideIsOpenBelow(lower_bounds[v]))) {
            return v;
        }
    }
    return DConstants::INVALID_INDEX;
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

double DecideTightPerRowBigM(const EvaluatedConstraint &ec, const FormulationBox &box, idx_t num_rows,
                             const vector<string> &var_names, const InstanceBox &instances, const string &written) {
    const vector<double> &lower_bounds = box.lower;
    const vector<double> &upper_bounds = box.upper;
    bool has_unbounded = false;
    double M = 0.0;
    for (idx_t r = 0; r < num_rows; r++) {
        if (!ec.row_group_ids.empty() &&
            ec.row_group_ids[r] == DConstants::INVALID_INDEX) {
            continue;
        }
        double rhs = DecideRowEffectiveBound(ec, r, written);
        // An infinite bound reaches the model unchanged everywhere else — it is
        // simply "no constraint" or "no solution" — but it cannot be linearized:
        // M has to dominate the row's slack, and nothing finite dominates
        // infinity. Refuse it here, in SQL terms, rather than let a non-finite
        // coefficient reach the model validator as an internal error. Callers that
        // can read an infinity by direction (`<>`, MIN/MAX) classify it before
        // getting here, so what still arrives is a bound with no reading at all.
        if (!std::isfinite(rhs)) {
            throw InvalidInputException(DecideInvalidValueMessage(
                std::isnan(rhs) ? DecideValueRole::BOUND : DecideValueRole::FINITE_BOUND, ec.rhs_written));
        }
        double rhs_mag = std::abs(rhs);
        double range = rhs_mag + DecideRowTermRange(ec.variable_indices, ec.row_coefficients,
                                                    r, lower_bounds, upper_bounds, has_unbounded,
                                                    DConstants::INVALID_INDEX, &instances);
        M = std::max(M, range);
    }
    // Refuse before finishing the constant: an open contributor means there is no M
    // to return, so the slack term below would only be computed and thrown away.
    if (has_unbounded) {
        ThrowUnboundedBigMNaming(true, FindUnboundedContributor(ec, lower_bounds, upper_bounds), var_names,
                                 DescribeBigMConstruct(ec), written);
    }
    return M + 1.0;
}

void DecidePropagateImpliedBounds(SolverInput &input, const VarIndexer &indexer, idx_t num_rows) {
    const auto &constraints = input.constraints;
    const auto &lower_bounds = input.lower_bounds;
    auto &upper_bounds = input.upper_bounds;
    // The bound each solver column gets from the clause at hand, NaN for none; `touched` resets it per clause.
    vector<double> column_bound(indexer.global_block_start, std::numeric_limits<double>::quiet_NaN());
    vector<int> touched;
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
            if (gid == DConstants::INVALID_INDEX && !ec.rows_stand_with_their_class) {
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
        // bound and cut feasible points (or make the model infeasible). A per-row
        // constraint over one variable drops nothing, so its sign does not matter there:
        // `x <= cap` bounds a signed `x` as well, by a bound that may itself be negative.
        // An aggregate one (`SUM(x) = -3`) sums every row's instance, so it does not.
        idx_t only_variable = DConstants::INVALID_INDEX;
        bool single_variable = !ec.lhs_is_aggregate;
        for (idx_t v : ec.variable_indices) {
            if (v == DConstants::INVALID_INDEX) {
                continue;
            }
            single_variable = single_variable && (only_variable == DConstants::INVALID_INDEX || only_variable == v);
            only_variable = v;
        }
        bool nonneg = true;
        for (idx_t v : ec.variable_indices) {
            if (!single_variable && v != DConstants::INVALID_INDEX && lower_bounds[v] < 0.0) {
                nonneg = false;
                break;
            }
        }
        if (!nonneg) {
            continue;
        }
        const string written = DecideWrittenClause(input, ec.source_clause_id);
        // Row e the clause emits, read as the model builder emits it (ClauseEmittedRows), over the data rows
        // [begin, end) of `rows` (row k itself for none). A solver column's coefficient is the sum of its terms over
        // the row's data rows, so a decision that stands in several rows of a group, or one counted once beside the
        // reducers, is bounded by what the row says about its instance. The row's bound is the clause's less its fixed
        // terms, as the builder states it.
        auto &emitted = ClauseEmittedRows(ec, indexer, written);
        bool uniform_rhs = ec.rhs_values.IsUniform();
        auto bound_row = [&](idx_t e, const vector<idx_t> *rows, idx_t begin, idx_t end) {
            if (begin == end) {
                return; // an empty group, or no data rows
            }
            idx_t first_row = rows ? (*rows)[begin] : begin;
            double k = (uniform_rhs ? ec.rhs_values.UniformValue() : ec.rhs_values.Get(first_row)) -
                       SumFixedAggregateLhsOffset(ec, rows, begin, end, written);
            for (idx_t i = emitted.offsets[e]; i < emitted.offsets[e + 1]; i++) {
                double a = emitted.values[i];
                if (a < -1e-15) {
                    nonneg = false;
                    break;
                }
                if (a <= 1e-15 || (k < 0.0 && !single_variable)) {
                    continue;
                }
                // A bound on one decision caps it at the greatest value it admits (stage 08), which the quotient
                // may miss: `0.1 * x <= 0.3` admits 3, while 0.3 / 0.1 is just below 3.
                double bound = ec.admitted_upper.logical_size ? ec.admitted_upper.Get(first_row) : k / a;
                auto column = emitted.columns[i];
                if (std::isnan(column_bound[column])) {
                    touched.push_back(column);
                    column_bound[column] = bound;
                } else {
                    column_bound[column] = std::min(column_bound[column], bound);
                }
            }
        };
        // The rows the clause emits: one per group of a grouped aggregate (ClauseRows), one of all rows for an
        // ungrouped one, one per kept row of a per-row clause.
        if (ec.lhs_is_aggregate && !ec.row_group_ids.empty()) {
            auto &rows = ClauseRows(ec);
            for (idx_t g = 0; nonneg && g < ec.num_groups; g++) {
                bound_row(g, &rows, ec.group_offsets[g], ec.group_offsets[g + 1]);
            }
        } else if (ec.lhs_is_aggregate) {
            bound_row(0, nullptr, 0, num_rows);
        } else {
            for (idx_t r = 0; nonneg && r < num_rows; r++) {
                if (ec.row_group_ids.empty() || ec.row_group_ids[r] != DConstants::INVALID_INDEX) {
                    bound_row(r, nullptr, r, r + 1); // an excluded row's class's first row bounds the same instance
                }
            }
        }
        // The shared bound ub_x applies to EVERY instance of x, so the clause bounds x only when one of its rows
        // bounds each instance, and then by the loosest of their bounds. With no data rows the loop never runs and
        // the bound stays at its start: harmless, there is no instance of x to bind.
        for (idx_t t = 0; nonneg && t < ec.variable_indices.size(); t++) {
            idx_t v = ec.variable_indices[t];
            if (v == DConstants::INVALID_INDEX) {
                continue;
            }
            bool already_handled = false;
            for (idx_t prev = 0; prev < t; prev++) {
                already_handled = already_handled || ec.variable_indices[prev] == v;
            }
            if (already_handled) {
                continue;
            }
            double bound = single_variable ? -std::numeric_limits<double>::infinity() : 0.0;
            bool every_instance_bounded = true;
            for (idx_t r = 0; r < num_rows && every_instance_bounded; r++) {
                double instance_bound = column_bound[indexer.Get(v, r)];
                every_instance_bounded = !std::isnan(instance_bound);
                bound = std::max(bound, instance_bound);
            }
            if (every_instance_bounded && bound < upper_bounds[v] && bound >= lower_bounds[v]) {
                upper_bounds[v] = bound;
            }
        }
        for (auto column : touched) {
            column_bound[column] = std::numeric_limits<double>::quiet_NaN();
        }
        touched.clear();
    }
}

} // namespace duckdb
