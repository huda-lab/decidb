//===----------------------------------------------------------------------===//
//                         DecidB
//
// src/decidb/formulation/linearization_minmax.cpp
//
// MIN/MAX lowering: hard constraints, extremum links, objectives, and the composed (additive) form. See ilp_linearization.cpp.
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
                    // Drop the MIN/MAX marking: what is emitted is an ordinary per-row
                    // constraint, and nothing downstream should read its LHS as an
                    // extremum again.
                    ec_no_solution.minmax_clause_idx = DConstants::INVALID_INDEX;
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


//! The family one group of a MIN/MAX constraint reduces over, in flat columns: one
//! member per active row, each holding that row's inner expression negated (the shape
//! `MinMaxLinkRow` and `EmitExtremumLink` take) with its constant part separate.
//!
//! `family` accumulates the reach of the whole group, which boxes the extremum column
//! and sizes the Big-M; each member also keeps its OWN reach, which is the box a native
//! arm gives a column pinned to it. Each end is kept independently — a row open above
//! still contributes the floor it does have.
struct MinMaxGroup {
    vector<ExtremumMember> members;
    AuxRange family;
    //! The row this group reads its bound off. Group-constant by construction:
    //! `ReduceAggregateRhsPerGroup` has already collapsed a row-varying bound.
    idx_t first_row = DConstants::INVALID_INDEX;
};

//! Split one tagged constraint into its groups' families. An ungrouped clause is the
//! single group 0.
static vector<MinMaxGroup> BuildMinMaxGroups(const SolverInput &input, const VarIndexer &indexer,
                                             const EvaluatedConstraint &ec) {
    const idx_t num_rows = input.num_rows;
    bool has_groups = !ec.row_group_ids.empty();
    idx_t group_count = has_groups ? MaxValue<idx_t>(ec.num_groups, 1) : 1;
    vector<MinMaxGroup> groups(group_count);

    for (idx_t r = 0; r < num_rows; r++) {
        idx_t g = has_groups ? ec.row_group_ids[r] : 0;
        if (g == DConstants::INVALID_INDEX || g >= group_count) {
            continue;
        }
        auto &group = groups[g];
        if (group.first_row == DConstants::INVALID_INDEX) {
            group.first_row = r;
        }
        ExtremumMember member;
        double row_lo = 0.0, row_hi = 0.0;
        DecideRowSignedRange(ec, r, input.lower_bounds, input.upper_bounds, row_lo, row_hi);
        double constant = DecideRowFixedLhsOffset(ec.variable_indices, ec.row_coefficients, r);
        // The member carries the constant part too, so the box is the variable-only
        // bracket shifted by it — which is also what the Big-M over the family reads.
        member.range.CoverRowSided(row_lo + constant, row_hi + constant,
                                   DConstants::INVALID_INDEX);
        group.family.Cover(member.range);

        // `z - expr`: the expression enters negated, its constant part lands on the bound.
        for (idx_t t = 0; t < ec.variable_indices.size(); t++) {
            idx_t v = ec.variable_indices[t];
            if (v == DConstants::INVALID_INDEX) {
                continue;
            }
            double coef = ec.row_coefficients[t].Get(r);
            if (coef == 0.0) {
                continue;
            }
            member.link.AddColumn(static_cast<int>(indexer.Get(v, r)), -coef);
        }
        member.link.constant = constant;
        group.members.push_back(std::move(member));
    }
    return groups;
}

} // namespace

void LinearizeMinMaxConstraints(SolverInput &input, const VarIndexer &indexer,
                                const vector<string> &var_names, NativeConstructPolicy policy) {
    const idx_t num_rows = input.num_rows;
    vector<EvaluatedConstraint> new_constraints;
    for (auto &ec : input.constraints) {
        // The aggregate name is the marking: `MAX(e) >= K` arrives spelled as an ordinary
        // `SUM(e) >= K` row and means something else entirely until this rewrites it.
        if (ec.minmax_agg_type.empty()) {
            new_constraints.push_back(std::move(ec));
            continue;
        }
        bool is_max_agg = (ec.minmax_agg_type == "max");
        // Settle every group's bound before anything is encoded. A bound out of reach or
        // beyond reach is answered by the direction it points, not by an encoding, and
        // whatever this emits (the unsatisfiable group's plain row) is an ordinary
        // constraint that rejoins the model.
        if (!ClassifyMinMaxGroups(ec, num_rows, is_max_agg, new_constraints)) {
            continue;
        }

        // The clause becomes what it says: an extremum column, and the user's own bound
        // as a single row over it. That is the same shape whichever way the extremum is
        // then pinned — so the row diagnosis reports is the row the user wrote, and it no
        // longer depends on which backend the host has. It used to: the lowering spread
        // the bound across a per-row Big-M family and carried `K` in a mechanism offset,
        // while the native arm stated it once.
        //
        // The extra column and the extra row per group are what buy that. They are also
        // all it costs: the per-member rows below are the same rows the Big-M family
        // emitted, with `z` in place of the bound.
        auto groups = BuildMinMaxGroups(input, indexer, ec);
        for (idx_t g = 0; g < groups.size(); g++) {
            auto &group = groups[g];
            if (group.members.empty()) {
                continue; // masked-out group: nothing to take an extremum over
            }
            // The extremum of a family lies inside the family's own bracket. Labelled
            // with the clause text stage 05 recorded, so a diagnosis over this column
            // reads `MAX(x * c)` rather than an internal name.
            string label;
            if (ec.minmax_clause_idx < input.minmax_clause_labels.size()) {
                label = input.minmax_clause_labels[ec.minmax_clause_idx];
            }
            // The raw bound, not the effective one: each member carries its own row's
            // constant LHS part, because a per-row constant can differ inside a group and
            // the extremum is taken over the whole expression, constant included. A
            // uniform bound is the same value whichever row is picked, so the
            // representative row is not even consulted.
            double clause_bound = ec.rhs_values.IsUniform() ? ec.rhs_values.UniformValue()
                                                            : ec.rhs_values.Get(group.first_row);
            // A bound the family cannot reach is still part of the reach this encoding
            // has to cover. `MAX(x) >= 5` over `x` in [-1, 1] boxes the extremum column at
            // 1 and sizes the closing Big-M off a span of 2, so the repair "widen x's box"
            // is not representable and the infeasible diagnosis can only offer to weaken
            // the MAX — advice worth 2 where advice worth 10 was available at the same
            // edit cost. Widening here is the same rule `DemandedAuxReach` applies to ABS,
            // and it fires under the same condition: only when the bound lies outside the
            // family, which is only when the clause cannot be met as written. Nothing is
            // loosened for a query that solves.
            bool bound_out_of_reach = false;
            if (std::isfinite(clause_bound)) {
                if (is_max_agg) {
                    if (!group.family.hi_unbounded && clause_bound > group.family.hi) {
                        group.family.hi = clause_bound;
                        bound_out_of_reach = true;
                    }
                } else if (!group.family.lo_unbounded && clause_bound < group.family.lo) {
                    group.family.lo = clause_bound;
                    bound_out_of_reach = true;
                }
            }
            // The two arms need different boxes here, and only one number separates
            // them. The lowering emits the CLOSING side alone (`z <= max`), so `K` is a
            // perfectly good ceiling for `z` and the family's widened span is a valid
            // Big-M. The native arm states `z = MAX(t..)` as an EQUALITY, so `z` has to
            // hold whatever the members actually reach — and a member may have to go
            // past `K` for the extremum to get there (`MAX(x + c) >= 25` is met at
            // `x = 23` with `c = 4`, an expression of 27). There is no derivable number
            // for that: how far the repair travels is not known until it is solved. So
            // the native arm's demanded end is OPENED, and the pin row tying each member
            // column to its expression does the constraining, as it already does. The
            // native arm derives no Big-M from this family, so an open end costs it
            // nothing but a looser root relaxation on a query that cannot solve anyway.
            AuxRange z_range = group.family;
            if (bound_out_of_reach && policy.Use(group.family.Unbounded())) {
                auto open_demanded_end = [&](AuxRange &range) {
                    if (is_max_agg) {
                        range.MarkHiUnbounded(DConstants::INVALID_INDEX);
                    } else {
                        range.MarkLoUnbounded(DConstants::INVALID_INDEX);
                    }
                };
                open_demanded_end(z_range);
                // `member.range` boxes the native argument column and is read nowhere
                // else — the lowering reads `member.link` and the family span.
                for (auto &member : group.members) {
                    open_demanded_end(member.range);
                }
            }
            idx_t z_idx = AddGlobalContinuousAux(input, indexer, z_range, 0.0, label);

            ExtremumLinkSpec spec;
            spec.result_column = z_idx;
            spec.is_max = is_max_agg;
            // Closing only. `MAX(e) >= K` needs `z <= MAX(e)`, or a `z` inflated past
            // every member would satisfy the bound that nothing else does; it does not
            // need `z >= MAX(e)`, because nothing pushes `z` down. The mirror holds for
            // MIN. Emitting the envelope as well would double the rows for nothing.
            spec.need_closing = true;
            // Deactivated, a member row reads `z - expr_var <= M + const`, whose worst
            // case is `family.hi - member.lo`. Maximised over members that is the
            // family's full span, constants included — which is what `Span()` is, and
            // every extremum link now scales off it for the same reason.
            spec.closing_underivable = group.family.Unbounded();
            spec.closing_big_m = spec.closing_underivable ? 0.0 : group.family.Span();
            // The range walk reports an infinity, not a culprit, so the column to name is
            // found the same way every other row-Big-M refusal finds it.
            spec.blame_var =
                FindUnboundedContributor(ec, input.lower_bounds, input.upper_bounds);
            spec.closing_kind = ConstraintKind::USER_MECHANISM;
            EmitExtremumLink(input, indexer, spec, group.members, var_names, policy);

            // The user's clause, over the extremum itself: `z <op> K`.
            SolverInput::RawConstraint outer;
            outer.indices.push_back(static_cast<int>(z_idx));
            outer.coefficients.push_back(1.0);
            outer.sense = is_max_agg ? '>' : '<';
            outer.rhs = clause_bound;
            outer.kind = ConstraintKind::USER_PARAMETER;
            // The elastic shape, without which this row does not fold. One `PER` clause
            // emits one of these per group, and they are all the same line of SQL: the
            // user edits a single literal and every group moves with it. Left UNSET they
            // never fold, so an infeasible `MAX(e) >= K PER g` reported one edit per group
            // — and only the loosest of them repaired anything. A genuinely per-group
            // bound stays PER_ROW_DATA and reports a virtual offset, as it does elsewhere.
            outer.shape = ec.rhs_is_shared_scalar ? ElasticShape::SHARED_SCALAR
                                                  : ElasticShape::PER_ROW_DATA;
            outer.rhs_label = ec.rhs_is_shared_scalar ? string() : ec.rhs_label;
            outer.source_clause_id = ec.source_clause_id;
            outer.repair_group_id = ec.repair_group_id;
            outer.removal_group_id = ec.removal_group_id;
            outer.group_key = ec.row_group_ids.empty() ? DConstants::INVALID_INDEX : g;
            outer.qualifier = ec.qualifier;
            outer.is_aggregate = true;
            if (g < ec.group_labels.size()) {
                outer.group_label = ec.group_labels[g];
            }
            input.global_constraints.push_back(std::move(outer));
        }
    }
    input.constraints = std::move(new_constraints);
}

//===--------------------------------------------------------------------===//
// Extremum links — the one place a MIN/MAX auxiliary is pinned
//===--------------------------------------------------------------------===//

//! The column a native general constraint takes for one member.
//!
//! The member's OWN column where the expression is nothing but that column. `MAX(x)`
//! over a decision variable is the common shape, and there a pinned `t = x` costs one
//! continuous column and one equality row per member to say nothing — and they are not
//! free, because a general constraint READS its arguments, so presolve cannot substitute
//! the copies away. Measured on `MAXIMIZE MAX(x)` at 15K rows, handing the column itself
//! to the general constraint took the solve from 0.97s to 0.29s for the same answer.
//!
//! Only an EXACT renaming qualifies: one surviving column, unit coefficient, no constant.
//! `MAX(2 * x)` and `MAX(x + 1)` are genuine expressions, and a general constraint relates
//! columns, so those still earn one. A pinned column is BOXED by the member's own reach —
//! a free continuous column is a measured cliff at the root LP — and left free only where
//! no reach is derivable at all, which is exactly the query the native arm exists for.
//!
//! `member.link` holds `(result - expr)`, so a renaming reads as the single coefficient
//! `-1`.
static int ExtremumArgumentColumn(SolverInput &input, const VarIndexer &indexer,
                                  const ExtremumMember &member, const string &label,
                                  idx_t source_clause_id, idx_t removal_group_id) {
    int sole_column = -1;
    double sole_coefficient = 0.0;
    idx_t surviving = 0;
    for (idx_t i = 0; i < member.link.indices.size(); i++) {
        if (std::abs(member.link.coefficients[i]) < 1e-15) {
            continue;
        }
        surviving++;
        sole_column = member.link.indices[i];
        sole_coefficient = member.link.coefficients[i];
    }
    if (surviving == 1 && sole_coefficient == -1.0 && member.link.constant == 0.0) {
        return sole_column;
    }
    idx_t t_idx = AddGlobalContinuousAux(input, indexer, member.range, 0.0, label);
    SolverInput::RawConstraint pin;
    pin.indices.push_back((int)t_idx);
    pin.coefficients.push_back(1.0);
    member.link.AppendTo(pin);
    pin.sense = '=';
    pin.rhs = member.link.constant;
    pin.kind = ConstraintKind::STRUCTURAL;
    pin.source_clause_id = source_clause_id;
    pin.removal_group_id = removal_group_id;
    input.global_constraints.push_back(std::move(pin));
    return (int)t_idx;
}

void EmitExtremumLink(SolverInput &input, const VarIndexer &indexer,
                      const ExtremumLinkSpec &spec, vector<ExtremumMember> &members,
                      const vector<string> &var_names, NativeConstructPolicy policy) {
    if (members.empty()) {
        return;
    }
    int result_col = (int)spec.result_column;

    // The envelope, on both arms. It is one row per member with no constant to dominate,
    // and on the native arm it is implied by the general constraint rather than
    // contradicted by it — so whether it is emitted is a question about the SITE (does
    // the surrounding model already supply this side?), never about the backend.
    if (spec.need_envelope) {
        char sense = spec.is_max ? '>' : '<';
        for (auto &member : members) {
            SolverInput::RawConstraint rc;
            rc.indices.push_back(result_col);
            rc.coefficients.push_back(1.0);
            member.link.AppendTo(rc);
            rc.sense = sense;
            rc.rhs = member.link.constant;
            rc.kind = spec.envelope_kind;
            rc.source_clause_id = spec.source_clause_id;
            rc.removal_group_id = spec.removal_group_id;
            input.global_constraints.push_back(std::move(rc));
        }
    }
    if (!spec.need_closing) {
        return;
    }

    // THE CHOICE, made once. A general constraint states the closing side outright and
    // needs no constant, which is the whole reason the arm exists: a family with no
    // derivable reach has no Big-M and therefore no lowering. Where a Big-M does exist
    // the lowering is the smaller model — measured — so native is the fallback.
    if (policy.Use(spec.closing_underivable)) {
        vector<int> args;
        args.reserve(members.size());
        for (auto &member : members) {
            args.push_back(ExtremumArgumentColumn(input, indexer, member, spec.label,
                                                  spec.source_clause_id, spec.removal_group_id));
        }
        GeneralConstraintSpec gc;
        gc.kind = spec.is_max ? GeneralConstraintKind::MAX : GeneralConstraintKind::MIN;
        gc.result_column = result_col;
        gc.argument_columns = std::move(args);
        gc.source_clause_id = spec.source_clause_id;
        gc.removal_group_id = spec.removal_group_id;
        input.general_constraints.push_back(std::move(gc));
        return;
    }
    if (spec.closing_underivable) {
        ThrowUnboundedBigMNaming(spec.blame_var, var_names, "MIN/MAX");
    }

    // One indicator per member, a Big-M row that binds `result` to that member when the
    // indicator is on, and `SUM(y) >= 1` so at least one binds.
    double M = spec.closing_big_m;
    double m_coeff = spec.is_max ? M : -M;
    char sense = spec.is_max ? '<' : '>';
    SolverInput::RawConstraint sum_y;
    sum_y.indices.reserve(members.size());
    sum_y.coefficients.reserve(members.size());
    for (auto &member : members) {
        idx_t y_idx = AddGlobalBinaryAux(input, indexer, 0.0, spec.label);
        sum_y.indices.push_back((int)y_idx);
        sum_y.coefficients.push_back(1.0);

        SolverInput::RawConstraint rc;
        rc.indices.push_back(result_col);
        rc.coefficients.push_back(1.0);
        member.link.AppendTo(rc);
        rc.indices.push_back((int)y_idx);
        rc.coefficients.push_back(m_coeff);
        rc.sense = sense;
        rc.rhs = m_coeff + member.link.constant;
        rc.kind = spec.closing_kind;
        rc.source_clause_id = spec.source_clause_id;
        rc.removal_group_id = spec.removal_group_id;
        input.global_constraints.push_back(std::move(rc));
    }
    sum_y.sense = '>';
    sum_y.rhs = 1.0;
    sum_y.kind = spec.closing_kind;
    sum_y.source_clause_id = spec.source_clause_id;
    sum_y.removal_group_id = spec.removal_group_id;
    input.global_constraints.push_back(std::move(sum_y));
}

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
    // variable's box. Constant terms are INCLUDED: an auxiliary is pinned against the
    // whole expression, and the Big-M over the family is this bracket's own span.
    //! An end no contributing variable's box could derive is reported as an infinity
    //! rather than as a flag beside a partial sum. That is what lets a caller add these
    //! up: a group sum is open below exactly when some member is, and `-inf + finite`
    //! already says so. The two ends never mix (`lo` is only ever `-inf`, `hi` only
    //! ever `+inf`), so no accumulation can reach a NaN.
    struct SavedRowRange {
        double lo = 0.0;
        double hi = 0.0;
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
                out.lo = -INF;
            } else {
                out.lo += lo_from_ub ? c * ub : c * lb;
            }
            if (lo_from_ub ? lb_open : ub_open) {
                if (out.unbounded_var == DConstants::INVALID_INDEX) {
                    out.unbounded_var = v;
                }
                out.hi = INF;
            } else {
                out.hi += lo_from_ub ? c * lb : c * ub;
            }
        }
        return out;
    };

    // The family every per-row MIN/MAX auxiliary (z, z_g) reduces over: one entry per
    // row of the saved objective expression. Unlike the per-row constraint sites (where
    // M bounds an expression against a fixed RHS), an objective auxiliary is linked via
    // (aux - expr) +/- M*y (>=|<=) +/- M, so the deactivated branch must stay slack
    // across the GLOBAL span
    //   max_r exprmax_r  -  min_r exprmin_r
    // taking the SIGN of every coefficient against the variable's [lb, ub]. Constants
    // INCLUDED: `aux` reaches the family's own ceiling, so a deactivated row r has to
    // stay slack across `family.hi - exprmin_r`, and row r's constant is measured
    // against a DIFFERENT row's — it is only within one row's `(aux - expr)` that a
    // constant cancels. Computed once and reused; an unbounded contributor has no such
    // value at all, and the query is refused rather than given a constant.
    bool row_family_cached = false;
    AuxRange row_family_range;
    auto row_family = [&]() -> const AuxRange & {
        if (!row_family_cached) {
            for (idx_t r = 0; r < num_rows; r++) {
                auto rr = saved_row_range(r);
                row_family_range.CoverRowSided(rr.lo, rr.hi, rr.unbounded_var);
            }
            row_family_cached = true;
        }
        return row_family_range;
    };
    //! This row's own expression range, as an AuxRange — tighter than the family's,
    //! and the box a `t` column pinned to that row deserves.
    auto row_range_for = [&](idx_t row) -> AuxRange {
        AuxRange range;
        auto rr = saved_row_range(row);
        range.CoverRowSided(rr.lo, rr.hi, rr.unbounded_var);
        return range;
    };

    //! The spec for one objective-side extremum link. An objective supplies exactly one
    //! of the two sides for free — the easy direction supplies the closing side, the hard
    //! direction supplies the envelope — so these are always complementary here, unlike
    //! at a composed site where the clause sits in a constraint and neither is supplied.
    //!
    //! `family` is the range the MEMBERS of this link reduce over, and it is a parameter
    //! rather than `row_family()` because the outer PER link reduces over group SUMS, not
    //! over rows. Both the Big-M and the blame column come from it, so a link can never
    //! be scaled off one family while its members live in another.
    auto objective_link = [&](idx_t result_column, bool is_max, bool is_easy,
                              const AuxRange &family) {
        ExtremumLinkSpec link_spec;
        link_spec.result_column = result_column;
        link_spec.is_max = is_max;
        link_spec.need_envelope = is_easy;
        link_spec.need_closing = !is_easy;
        link_spec.closing_underivable = family.Unbounded();
        link_spec.closing_big_m = link_spec.closing_underivable ? 0.0 : family.Span();
        link_spec.blame_var = family.unbounded_var;
        return link_spec;
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

            // Each z_g is an extremum over rows of its group, so every one of them is
            // boxed by the same per-row family.
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

            for (idx_t g = 0; g < K; g++) {
                if (active_offsets[g] == active_offsets[g + 1]) {
                    PinZGroupToZero(g);
                    continue;
                }
                vector<ExtremumMember> members;
                members.reserve(active_offsets[g + 1] - active_offsets[g]);
                for (idx_t k = active_offsets[g]; k < active_offsets[g + 1]; k++) {
                    idx_t row = active_flat_rows[k];
                    ExtremumMember member;
                    AddObjectiveRowTerms(member.link, row, 1.0);
                    member.range = row_range_for(row);
                    members.push_back(std::move(member));
                }
                EmitExtremumLink(input, indexer,
                                 objective_link(group_value_indices[g], !inner_is_min,
                                                inner_easy, row_family()),
                                 members, var_names, native_min_max);
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

            // The members are the group values themselves — already columns, so a
            // native arm needs no pinning rows at all and the envelope reads
            // `w - z_g <op> 0`.
            vector<ExtremumMember> members;
            members.reserve(K);
            for (idx_t g = 0; g < K; g++) {
                ExtremumMember member;
                member.link.AddColumn((int)group_value_indices[g], -1.0);
                member.range = row_family();
                members.push_back(std::move(member));
            }
            // Outer Big-M: the family range is the global span of the objective
            // expression (max_r exprmax - min_r exprmin), which bounds the span of
            // (w - z_g) since every z_g is boxed by that same range.
            EmitExtremumLink(input, indexer,
                             objective_link(w_idx, !outer_is_min, outer_easy, row_family()),
                             members, var_names, native_min_max);
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
                double g_lo = 0.0, g_hi = 0.0;
                idx_t g_unbounded_var = DConstants::INVALID_INDEX;
                for (idx_t k = obj_offsets[g]; k < obj_offsets[g + 1]; k++) {
                    auto rr = saved_row_range(obj_flat_rows[k]);
                    g_lo += rr.lo * scale;
                    g_hi += rr.hi * scale;
                    if (g_unbounded_var == DConstants::INVALID_INDEX) {
                        g_unbounded_var = rr.unbounded_var;
                    }
                }
                group_sum_family.CoverRowSided(g_lo, g_hi, g_unbounded_var);
            }
            idx_t w_idx = AddGlobalContinuousAux(input, indexer, group_sum_family, 1.0); // objective = w

            // Each member is one group's sum: sum_g = Σ_{r ∈ group_g} Σ_t coeff_t_r * x.
            vector<ExtremumMember> members;
            members.reserve(K);
            for (idx_t g = 0; g < K; g++) {
                double scale = spec.per_inner_was_avg ? 1.0 / static_cast<double>(group_size(g)) : 1.0;
                ExtremumMember member;
                for (idx_t k = obj_offsets[g]; k < obj_offsets[g + 1]; k++) {
                    AddObjectiveRowTerms(member.link, obj_flat_rows[k], scale);
                }
                // On the envelope side a group that is identically zero contributes a
                // vacuous `w op 0` row, which the outer optimization direction settles on
                // its own. The closing side cannot drop it: there every group is a real
                // member of the extremum, and an all-zero one participates as the
                // constant 0 it is. A group left holding only a constant still bounds w
                // either way, so it stays.
                if (outer_easy && member.link.HasNoColumns() && std::abs(member.link.constant) < 1e-15) {
                    continue;
                }
                // Boxed by the family over group sums — looser than a per-group bracket,
                // but sound (it covers every group) and finite whenever any of them is.
                // The same range `w` itself was boxed by, so the two cannot drift.
                member.range = group_sum_family;
                members.push_back(std::move(member));
            }
            if (members.empty()) {
                // Every group is identically zero — pin w to 0 so outer optimization
                // doesn't push the otherwise-unconstrained w to ±∞.
                PinGlobalAux(input, indexer, w_idx, 0.0);
            } else {
                // The members ARE the group sums, so the Big-M comes from the family
                // over group sums — the same range `w` and every member is boxed by. It
                // used to be the per-row span multiplied by the row count, which is a
                // different family and a bound on it rather than a measurement of it.
                EmitExtremumLink(input, indexer,
                                 objective_link(w_idx, !outer_is_min, outer_easy,
                                                group_sum_family),
                                 members, var_names, native_min_max);
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

        vector<ExtremumMember> members;
        members.reserve(active_rows.size());
        for (idx_t row : active_rows) {
            ExtremumMember member;
            AddObjectiveRowTerms(member.link, row, 1.0);
            member.range = row_range_for(row);
            members.push_back(std::move(member));
        }
        EmitExtremumLink(input, indexer,
                         objective_link(z_idx, !is_min_agg, is_easy, row_family()), members,
                         var_names, native_min_max);
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
        double row_lo = 0.0, row_hi = 0.0;
        for (idx_t it = 0; it < inner_terms.size(); it++) {
            double c = per_term_coefs[it][row];
            if (std::abs(c) < 1e-15) {
                continue;
            }
            idx_t v = inner_terms[it].variable_index;
            if (v == DConstants::INVALID_INDEX) {
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
            row_lo += (c > 0.0) ? c * lb : c * ub;
            row_hi += (c > 0.0) ? c * ub : c * lb;
        }
        range.CoverRow(row_lo, row_hi);
    }
    return range;
}

//! The auxiliary layer both composed paths share: one global `z_k` per MIN/MAX term,
//! pinned to that term's extremum over its active rows. Fills in `z_idx` on every
//! MIN/MAX term, which the caller then references from the outer row or the objective.
//!
//! A composed clause lives in a CONSTRAINT — `SUM(a) + 2*MAX(b) <= K` — or in one term
//! of an objective sum, and in neither case does any optimization pressure act on `z_k`
//! directly. So its easy direction still needs the envelope (nothing else holds `z_k`
//! against the members), and its hard direction needs the envelope AND the closing
//! side. That is the difference from an objective-side link, and it is expressed as two
//! flags on the spec rather than as a separate emitter.
static void EmitComposedMinMaxAuxiliaries(SolverInput &input, const VarIndexer &indexer,
                                          vector<ComposedMinMaxTermData> &terms,
                                          idx_t source_clause_id, idx_t removal_group_id,
                                          const vector<string> &var_names,
                                          NativeConstructPolicy native_min_max) {
    idx_t num_rows = input.num_rows;

    // Every `z_k` first, so one clause's auxiliaries occupy a contiguous run of the
    // global block whatever formulation each term then turns out to need. The range is
    // computed once here and reused as the closing Big-M and as the box of any column a
    // native arm pins — one walk, one answer, rather than each arm deriving its own.
    vector<AuxRange> term_range(terms.size());
    for (idx_t i = 0; i < terms.size(); i++) {
        auto &ta = terms[i];
        if (!ta.is_minmax) {
            continue;
        }
        term_range[i] =
            ComposedTermRange(input, (*ta.inner_terms), ta.per_term_coefs, ta.filter_mask);
        // The label names the z through the global label channel, so a diagnosis
        // renders `MAX(x)` rather than an internal column name.
        ta.z_idx = AddGlobalContinuousAux(input, indexer, term_range[i], 0.0, ta.label);
    }

    for (idx_t i = 0; i < terms.size(); i++) {
        auto &ta = terms[i];
        if (!ta.is_minmax) {
            continue;
        }
        vector<ExtremumMember> members;
        for (idx_t row = 0; row < num_rows; row++) {
            if (!ta.filter_mask[row]) {
                continue;
            }
            ExtremumMember member;
            AddComposedRowTerms(indexer, member.link, (*ta.inner_terms), ta.per_term_coefs, row);
            // The TERM's range, not the row's: looser than a per-row bracket, but it is
            // the one the auxiliary itself is boxed by and the one the Big-M comes from,
            // so a pinned column shares it rather than introducing a second answer.
            member.range = term_range[i];
            members.push_back(std::move(member));
        }

        ExtremumLinkSpec spec;
        spec.result_column = ta.z_idx;
        spec.is_max = (ta.agg_name == "max");
        spec.need_envelope = true;
        spec.need_closing = !ta.is_easy;
        spec.closing_underivable = term_range[i].Unbounded();
        // Constants INCLUDED, for the reason the objective path records: a deactivated
        // member row has to stay slack across `range.hi - member_lo`, which compares one
        // row's constant against another's. Only within a single row does it cancel.
        spec.closing_big_m = spec.closing_underivable ? 0.0 : term_range[i].Span();
        spec.blame_var = term_range[i].unbounded_var;
        spec.label = ta.label;
        spec.source_clause_id = source_clause_id;
        spec.removal_group_id = removal_group_id;
        // Mechanism, matching the hard MIN/MAX path above. The closing rows encode
        // `z = MIN/MAX(members)`; their slack loosens that *definition*, not the user's
        // bound, and the two differ by whatever coefficient `z` carries in the outer
        // row -- `3*MIN(e) + SUM(x) <= 22` needs three times the slack a closing row
        // reports, so quoting it back as an edit understates the repair and re-solves
        // to infeasible. Relaxing the outer row is mathematically equivalent (lowering
        // `z` by s/a and raising the bound by s admit the same solutions) and is already
        // in the user's own units, so repair goes there: EmitComposedMinMaxOuter stamps
        // USER_PARAMETER, SHARED_SCALAR, a repair_group_id and a source_clause_id.
        // A closing row had none of the last of those, so a diagnosis that blamed one
        // rebuilt its text from raw coefficients and printed the Big-M as the user's
        // own clause.
        spec.closing_kind = ConstraintKind::USER_MECHANISM;
        EmitExtremumLink(input, indexer, spec, members, var_names, native_min_max);
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
                                       idx_t removal_group_id,
                                       const vector<string> &var_names, NativeConstructPolicy native_min_max) {
    idx_t num_rows = input.num_rows;
    EmitComposedMinMaxAuxiliaries(input, indexer, terms, source_clause_id, removal_group_id,
                                  var_names, native_min_max);

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
    outer.removal_group_id = removal_group_id;
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

    EmitComposedMinMaxAuxiliaries(input, indexer, terms, DConstants::INVALID_INDEX,
                                  DConstants::INVALID_INDEX, var_names, native_min_max);

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
