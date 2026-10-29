//===----------------------------------------------------------------------===//
//                         DeciDB
//
// src/decidb/formulation/linearization_reducer_value.cpp
//
// Stage 06 — the helper decisions of the value rewrite (language/guide.md §6.4;
// 05_optimizer/done.md, "The value rewrite"). Stage 05 replaced each decision reducer
// of a VALUE-shape clause by a helper decision and recorded what it holds; stage 08
// evaluated that reducer's members per `by` group. This boxes every helper by its
// members' reach and emits its definition: an equality row per group for SUM and AVG,
// an extremum link per group for MIN and MAX. The helper's own column enters only
// here: stage 08 hands over the members alone.
//
//===----------------------------------------------------------------------===//
#include "duckdb/decidb/formulation/ilp_linearization.hpp"
#include "duckdb/decidb/formulation/ilp_linearization_internal.hpp"

#include <cmath>
#include <limits>

namespace duckdb {

using namespace decide_linearize; // NOLINT: internal DECIDE linearization helpers

namespace {

bool IsExtremum(const ReducerValueLinkSpec &link) {
	return link.agg_name == "min" || link.agg_name == "max";
}

//! The reach of one group's value: the sum of its members' brackets for SUM and AVG
//! (an AVG's coefficients already carry the 1/n), their cover for MIN and MAX. Each
//! end is folded or marked open on its own, like every other auxiliary box.
AuxRange GroupReach(const ReducerValueLinkSpec &link, const SolverInput &input, const vector<idx_t> &rows) {
	AuxRange reach;
	const bool extremum = IsExtremum(link);
	double sum_lo = 0.0, sum_hi = 0.0;
	for (auto row : rows) {
		double lo, hi;
		DecideRowSignedRange(link.rows, row, input.lower_bounds, input.upper_bounds, lo, hi);
		double constant =
		    DecideRowFixedLhsOffset(link.rows.variable_indices, link.rows.row_coefficients, row, link.label);
		if (extremum) {
			reach.CoverRowSided(lo + constant, hi + constant, DConstants::INVALID_INDEX);
			continue;
		}
		if (std::isinf(lo)) {
			reach.MarkLoUnbounded(DConstants::INVALID_INDEX);
		} else {
			sum_lo += lo + constant;
		}
		if (std::isinf(hi)) {
			reach.MarkHiUnbounded(DConstants::INVALID_INDEX);
		} else {
			sum_hi += hi + constant;
		}
	}
	if (!extremum) {
		reach.CoverRowSided(sum_lo, sum_hi, DConstants::INVALID_INDEX);
	}
	return reach;
}

} // namespace

void DeriveReducerValueBounds(SolverInput &input, const VarIndexer &indexer) {
	constexpr double INF = std::numeric_limits<double>::infinity();
	input.instance_reach_lower.resize(input.num_decide_vars);
	input.instance_reach_upper.resize(input.num_decide_vars);
	for (auto &link : input.reducer_value_links) {
		auto &ec = link.rows;
		const bool grouped = !ec.row_group_ids.empty();
		vector<vector<idx_t>> members(grouped ? ec.num_groups : 1);
		for (idx_t row = 0; row < input.num_rows; row++) {
			idx_t group = grouped ? ec.row_group_ids[row] : 0;
			if (group != DConstants::INVALID_INDEX) {
				members[group].push_back(row);
			}
		}
		// Each instance is boxed by the reach of the group it stands for: the helper has one instance per group, so a
		// small group is not boxed as wide as the largest. An instance no group reaches keeps the open ends, and
		// reads the decision's box.
		const idx_t value_var = link.value_var;
		const idx_t num_instances = indexer.NumInstances(value_var);
		vector<AuxRange> reach(num_instances);
		vector<bool> reached(num_instances, false);
		AuxRange box;
		for (auto &rows : members) {
			if (rows.empty()) {
				continue;
			}
			auto group_reach = GroupReach(link, input, rows);
			auto instance = indexer.InstanceOf(value_var, rows.front());
			reach[instance].Cover(group_reach);
			reached[instance] = true;
			box.Cover(group_reach);
		}
		// Written afresh on every call: the members' boxes have moved since the last one (an ABS auxiliary among them),
		// and a diagnosis derives it again from its widened members.
		auto &reach_lower = input.instance_reach_lower[value_var];
		auto &reach_upper = input.instance_reach_upper[value_var];
		reach_lower.assign(num_instances, -INF);
		reach_upper.assign(num_instances, INF);
		for (idx_t instance = 0; instance < num_instances; instance++) {
			if (!reached[instance]) {
				continue;
			}
			if (!reach[instance].lo_unbounded) {
				reach_lower[instance] = reach[instance].lo;
			}
			if (!reach[instance].hi_unbounded) {
				reach_upper[instance] = reach[instance].hi;
			}
		}
		// The decision's box stays the cover of every instance's reach, so every pass that reads it stays sound. An end
		// no member bounds stays open; an end already tighter stands.
		if (!box.lo_unbounded) {
			input.lower_bounds[value_var] = MaxValue<double>(input.lower_bounds[value_var], box.lo);
		}
		if (!box.hi_unbounded) {
			input.upper_bounds[value_var] = MinValue<double>(input.upper_bounds[value_var], box.hi);
		}
	}
}

void LinearizeReducerValueLinks(SolverInput &input, const VarIndexer &indexer, const vector<string> &var_names,
                                NativeConstructPolicy policy) {
	for (auto &link : input.reducer_value_links) {
		const bool is_max = link.agg_name == "max";
		const idx_t value_var = link.value_var;
		// A member is a row's expression negated with its constant apart, the shape the
		// extremum link takes; the equality reads it the same way.
		// A helper serves every clause that reads its reducer, so a sum past a DOUBLE names the reducer as written.
		auto groups = BuildMinMaxGroups(input, indexer, link.rows, link.label);
		for (auto &group : groups) {
			if (group.members.empty()) {
				continue;
			}
			if (!IsExtremum(link)) {
				// `value - sum of members' expressions` against `sum of their constants`: the
				// side the clauses push the value against (`value >= sum` when they push it
				// down, `value <= sum` when up), the equality only when both sides are needed.
				// The members' columns are accumulated first: a keyed decision may stand in
				// several rows of the group, and a row states each column once.
				MinMaxLinkRow sum;
				double constants = 0.0;
				for (auto &member : group.members) {
					for (idx_t i = 0; i < member.link.indices.size(); i++) {
						sum.AddColumn(member.link.indices[i], member.link.coefficients[i], link.label);
					}
					AddCoefficient(constants, member.link.constant, link.label);
				}
				SolverInput::RawConstraint rc;
				rc.indices.push_back(static_cast<int>(indexer.Get(value_var, group.first_row)));
				rc.coefficients.push_back(1.0);
				sum.AppendTo(rc);
				rc.sense = link.need_envelope && link.need_closing ? '=' : link.need_envelope ? '>' : '<';
				rc.rhs = constants;
				rc.kind = ConstraintKind::STRUCTURAL;
				input.global_constraints.push_back(std::move(rc));
				continue;
			}
			ExtremumLinkSpec spec;
			spec.result_column = indexer.Get(value_var, group.first_row);
			spec.is_max = is_max;
			spec.need_envelope = link.need_envelope;
			spec.need_closing = link.need_closing;
			// Deactivated, a closing row reads `value - member <= M` for MAX (`>= -M` for
			// MIN): the helper's box end against the family's far end. The end is this
			// group's own instance's (its reach, DeriveReducerValueBounds), not the
			// decision's cover of every group's.
			const idx_t instance = indexer.InstanceOf(value_var, group.first_row);
			const double value_hi = input.InstanceBoxUpper(value_var, instance, input.upper_bounds);
			const double value_lo = input.InstanceBoxLower(value_var, instance, input.lower_bounds);
			const bool value_hi_open = value_hi >= 1e20;
			const bool value_lo_open = value_lo <= -1e20;
			spec.closing_underivable =
			    is_max ? (value_hi_open || group.family.lo_unbounded) : (value_lo_open || group.family.hi_unbounded);
			spec.closing_big_m = spec.closing_underivable ? 0.0
			                     : is_max                 ? value_hi - group.family.lo
			                                              : group.family.hi - value_lo;
			if (spec.closing_underivable) {
				spec.blame_var = FindUnboundedContributor(link.rows, input.lower_bounds, input.upper_bounds);
			}
			spec.written = link.label;
			spec.label = link.label;
			spec.envelope_kind = ConstraintKind::STRUCTURAL;
			spec.closing_kind = ConstraintKind::STRUCTURAL;
			EmitExtremumLink(input, indexer, spec, group.members, var_names, policy);
		}
	}
	input.reducer_value_links.clear();
}

} // namespace duckdb
