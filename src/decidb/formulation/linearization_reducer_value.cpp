//===----------------------------------------------------------------------===//
//                         DecidB
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
		double constant = DecideRowFixedLhsOffset(link.rows.variable_indices, link.rows.row_coefficients, row);
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

void DeriveReducerValueBounds(SolverInput &input) {
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
		// One column box for the helper, whichever group an instance stands for: the
		// cover of every group's reach. An end no member bounds stays open; an end
		// already tighter stands.
		AuxRange box;
		for (auto &rows : members) {
			if (!rows.empty()) {
				box.Cover(GroupReach(link, input, rows));
			}
		}
		if (!box.lo_unbounded) {
			input.lower_bounds[link.value_var] = MaxValue<double>(input.lower_bounds[link.value_var], box.lo);
		}
		if (!box.hi_unbounded) {
			input.upper_bounds[link.value_var] = MinValue<double>(input.upper_bounds[link.value_var], box.hi);
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
		auto groups = BuildMinMaxGroups(input, indexer, link.rows);
		for (auto &group : groups) {
			if (group.members.empty()) {
				continue;
			}
			if (!IsExtremum(link)) {
				// `value - sum of members' expressions = sum of their constants`. The
				// members' columns are accumulated first: a keyed decision may stand in
				// several rows of the group, and a row states each column once.
				MinMaxLinkRow sum;
				double constants = 0.0;
				for (auto &member : group.members) {
					for (idx_t i = 0; i < member.link.indices.size(); i++) {
						sum.AddColumn(member.link.indices[i], member.link.coefficients[i]);
					}
					constants += member.link.constant;
				}
				SolverInput::RawConstraint rc;
				rc.indices.push_back(static_cast<int>(indexer.Get(value_var, group.first_row)));
				rc.coefficients.push_back(1.0);
				sum.AppendTo(rc);
				rc.sense = '=';
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
			// MIN): the helper's box end against the family's far end. The helper's box is
			// the cover of every group's reach (DeriveReducerValueBounds), so it may exceed
			// this family's, which is why it is read rather than the family's own span.
			const bool value_hi_open = input.upper_bounds[value_var] >= 1e20;
			const bool value_lo_open = input.lower_bounds[value_var] <= -1e20;
			spec.closing_underivable =
			    is_max ? (value_hi_open || group.family.lo_unbounded) : (value_lo_open || group.family.hi_unbounded);
			spec.closing_big_m = spec.closing_underivable ? 0.0
			                     : is_max                 ? input.upper_bounds[value_var] - group.family.lo
			                                              : group.family.hi - input.lower_bounds[value_var];
			if (spec.closing_underivable) {
				spec.blame_var = group.family.Unbounded()
				                     ? FindUnboundedContributor(link.rows, input.lower_bounds, input.upper_bounds)
				                     : value_var;
			}
			spec.label = link.label;
			spec.envelope_kind = ConstraintKind::STRUCTURAL;
			spec.closing_kind = ConstraintKind::STRUCTURAL;
			EmitExtremumLink(input, indexer, spec, group.members, var_names, policy);
		}
	}
	input.reducer_value_links.clear();
}

} // namespace duckdb
