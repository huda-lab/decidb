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

namespace {

//! A sum of term ends, each finite or infinite, kept apart so that one term can be left
//! out of it again: an end some term leaves open is open unless that term is the one
//! left out.
struct EndSum {
	double finite = 0.0;
	idx_t open = 0;

	void Add(double end) {
		if (std::isinf(end)) {
			open++;
		} else {
			finite += end;
		}
	}
	//! The sum without `end`, one of the ends added; infinite when another is open.
	double Without(double end, double open_value) const {
		const bool this_open = std::isinf(end);
		if (open > (this_open ? 1 : 0)) {
			return open_value;
		}
		return finite - (this_open ? 0.0 : end);
	}
};

//! One term `c * v` of a definition, at the column standing for `v` there.
struct MemberTerm {
	idx_t var;
	idx_t column;
	double coefficient;
	double lo; //!< the term's floor, `-inf` when the box leaves it open
	double hi; //!< its top, `+inf` when open
};

MemberTerm MakeMemberTerm(const SolverInput &input, idx_t var, idx_t column, double c) {
	constexpr double INF = std::numeric_limits<double>::infinity();
	const double lb = input.lower_bounds[var] <= -1e20 ? -INF : input.lower_bounds[var];
	const double ub = input.upper_bounds[var] >= 1e20 ? INF : input.upper_bounds[var];
	const double a = c * (c > 0 ? lb : ub);
	const double b = c * (c > 0 ? ub : lb);
	return MemberTerm {var, column, c, a, b};
}

//! `terms` with each column once, its coefficients summed: a group's members may read one
//! helper column from several rows (`sum(t.w * max(x))` is `2m + m`), and read apart, each
//! occurrence bounds the column by the others' ends, closing in on its value by ever
//! smaller steps instead of in one.
vector<MemberTerm> CombineColumns(const SolverInput &input, const vector<MemberTerm> &terms) {
	vector<MemberTerm> combined;
	unordered_map<idx_t, idx_t> position;
	for (auto &term : terms) {
		auto entry = position.find(term.column);
		if (entry == position.end()) {
			position[term.column] = combined.size();
			combined.push_back(term);
		} else {
			combined[entry->second].coefficient += term.coefficient;
		}
	}
	vector<MemberTerm> result;
	for (auto &term : combined) {
		if (std::abs(term.coefficient) >= 1e-15) {
			result.push_back(MakeMemberTerm(input, term.var, term.column, term.coefficient));
		}
	}
	return result;
}

//! The bounds `terms` puts on each helper column among them, given that their sum plus
//! `constant` lies in [`lo`, `hi`] (either end infinite for none): each term is at most
//! `hi` less every other term's floor, and at least `lo` less every other term's top.
//! A column keeps the tightest bound any reading gives it.
void BoundTermsOfASum(const vector<MemberTerm> &terms, double constant, double lo, double hi,
                      const vector<bool> &is_helper, unordered_map<idx_t, double> &column_lo,
                      unordered_map<idx_t, double> &column_hi) {
	constexpr double INF = std::numeric_limits<double>::infinity();
	EndSum floors, tops;
	for (auto &term : terms) {
		floors.Add(term.lo);
		tops.Add(term.hi);
	}
	for (auto &term : terms) {
		if (term.var >= is_helper.size() || !is_helper[term.var]) {
			continue;
		}
		const double others_lo = floors.Without(term.lo, -INF) + constant;
		const double others_hi = tops.Without(term.hi, INF) + constant;
		// The term `c * v` lies in [lo - others_hi, hi - others_lo].
		const double term_hi = std::isinf(hi) || std::isinf(others_lo) ? INF : hi - others_lo;
		const double term_lo = std::isinf(lo) || std::isinf(others_hi) ? -INF : lo - others_hi;
		const double v_hi = term.coefficient > 0 ? term_hi / term.coefficient : term_lo / term.coefficient;
		const double v_lo = term.coefficient > 0 ? term_lo / term.coefficient : term_hi / term.coefficient;
		if (!std::isinf(v_hi) && !std::isnan(v_hi)) {
			auto entry = column_hi.find(term.column);
			column_hi[term.column] = entry == column_hi.end() ? v_hi : MinValue<double>(entry->second, v_hi);
		}
		if (!std::isinf(v_lo) && !std::isnan(v_lo)) {
			auto entry = column_lo.find(term.column);
			column_lo[term.column] = entry == column_lo.end() ? v_lo : MaxValue<double>(entry->second, v_lo);
		}
	}
}

} // namespace

void DeriveReducerMemberBounds(SolverInput &input, const VarIndexer &indexer) {
	constexpr double INF = std::numeric_limits<double>::infinity();
	vector<bool> is_helper(input.num_decide_vars, false);
	for (auto &link : input.reducer_value_links) {
		if (link.value_var < is_helper.size()) {
			is_helper[link.value_var] = true;
		}
	}
	unordered_map<idx_t, double> column_lo, column_hi;
	// The outermost helper first (links are stored innermost first), so a bound flows down
	// a nest in one sweep.
	for (idx_t i = input.reducer_value_links.size(); i-- > 0;) {
		auto &link = input.reducer_value_links[i];
		auto &ec = link.rows;
		const double value_lo = input.lower_bounds[link.value_var] <= -1e20 ? -INF : input.lower_bounds[link.value_var];
		const double value_hi = input.upper_bounds[link.value_var] >= 1e20 ? INF : input.upper_bounds[link.value_var];
		if (std::isinf(value_lo) && std::isinf(value_hi)) {
			continue;
		}
		const bool extremum = IsExtremum(link);
		if (extremum && !link.need_envelope) {
			continue; // a closing side alone pins no member
		}
		const bool grouped = !ec.row_group_ids.empty();
		const idx_t num_groups = grouped ? ec.num_groups : 1;
		vector<vector<MemberTerm>> group_terms(num_groups);
		vector<double> group_constant(num_groups, 0.0);
		for (idx_t row = 0; row < input.num_rows; row++) {
			const idx_t group = grouped ? ec.row_group_ids[row] : 0;
			if (group == DConstants::INVALID_INDEX || group >= num_groups) {
				continue;
			}
			vector<MemberTerm> member;
			double constant = 0.0;
			for (idx_t t = 0; t < ec.variable_indices.size(); t++) {
				const double c = ec.row_coefficients[t].Get(row);
				if (std::abs(c) < 1e-15) {
					continue;
				}
				const idx_t var = ec.variable_indices[t];
				if (var == DConstants::INVALID_INDEX) {
					constant += c;
					continue;
				}
				member.push_back(MakeMemberTerm(input, var, indexer.Get(var, row), c));
			}
			if (extremum) {
				// A MAX's envelope keeps every member at or under the helper's top, a
				// MIN's at or over its floor.
				const bool is_max = link.agg_name == "max";
				BoundTermsOfASum(CombineColumns(input, member), constant, is_max ? -INF : value_lo,
				                 is_max ? value_hi : INF, is_helper, column_lo, column_hi);
				continue;
			}
			for (auto &term : member) {
				group_terms[group].push_back(term);
			}
			group_constant[group] += constant;
		}
		if (extremum) {
			continue;
		}
		// A SUM or AVG helper bounds its group's members on the sides its definition states: at most its top under
		// `value >= members` (the envelope), at least its floor under `value <= members` (the closing side).
		const double sum_lo = link.need_closing ? value_lo : -INF;
		const double sum_hi = link.need_envelope ? value_hi : INF;
		for (idx_t group = 0; group < num_groups; group++) {
			if (!group_terms[group].empty()) {
				BoundTermsOfASum(CombineColumns(input, group_terms[group]), group_constant[group], sum_lo, sum_hi,
				                 is_helper, column_lo, column_hi);
			}
		}
	}
	// A helper's box is one for every instance it defines: tightened only when each of
	// them was bounded, by the loosest. Its other columns -- the rows its path drops -- are
	// read by no row.
	for (auto &link : input.reducer_value_links) {
		const idx_t var = link.value_var;
		auto &ec = link.rows;
		double loosest_lo = INF, loosest_hi = -INF;
		bool every_lo = true, every_hi = true, any = false;
		for (idx_t row = 0; row < input.num_rows; row++) {
			if (!ec.row_group_ids.empty() && ec.row_group_ids[row] == DConstants::INVALID_INDEX) {
				continue;
			}
			any = true;
			const idx_t column = indexer.Get(var, row);
			auto lo = column_lo.find(column);
			auto hi = column_hi.find(column);
			every_lo = every_lo && lo != column_lo.end();
			every_hi = every_hi && hi != column_hi.end();
			if (lo != column_lo.end()) {
				loosest_lo = MinValue<double>(loosest_lo, lo->second);
			}
			if (hi != column_hi.end()) {
				loosest_hi = MaxValue<double>(loosest_hi, hi->second);
			}
		}
		// Only a tightening by more than rounding counts, so the fixed point it shares with
		// the forward derivation settles.
		auto tighter = [](double candidate, double current, bool upper) {
			const double margin = 1e-9 * MaxValue<double>(1.0, std::abs(current));
			return upper ? candidate < current - margin : candidate > current + margin;
		};
		if (any && every_hi && tighter(loosest_hi, input.upper_bounds[var], true)) {
			input.upper_bounds[var] = loosest_hi;
		}
		if (any && every_lo && tighter(loosest_lo, input.lower_bounds[var], false)) {
			input.lower_bounds[var] = loosest_lo;
		}
	}
}

void LinearizeReducerValueLinks(SolverInput &input, const VarIndexer &indexer, const vector<string> &var_names,
                                NativeConstructPolicy policy) {
	SparseCoeffAccumulator sum_terms;
	for (auto &link : input.reducer_value_links) {
		const bool is_max = link.agg_name == "max";
		const idx_t value_var = link.value_var;
		// A member is a row's expression negated with its constant apart, the shape the
		// extremum link takes; the equality reads it the same way.
		// A helper serves every clause that reads its reducer, so a sum past a DOUBLE names the reducer as written.
		auto groups = BuildMinMaxGroups(input, link.rows, link.label);
		MinMaxRowMembers row_members(input, indexer, link.rows, link.label);
		for (auto &group : groups) {
			if (group.rows.empty()) {
				continue;
			}
			if (!IsExtremum(link)) {
				// `value - sum of members' expressions` against `sum of their constants`: the
				// side the clauses push the value against (`value >= sum` when they push it
				// down, `value <= sum` when up), the equality only when both sides are needed.
				// The members' columns are accumulated first: a keyed decision may stand in
				// several rows of the group, and a row states each column once.
				sum_terms.BeginForDecisions(indexer, 0, link.label);
				double constants = 0.0;
				MinMaxLinkRow member;
				for (idx_t row : group.rows) {
					member.indices.clear();
					member.coefficients.clear();
					row_members.FillLink(row, member);
					for (idx_t i = 0; i < member.indices.size(); i++) {
						sum_terms.Add(member.indices[i], member.coefficients[i]);
					}
					AddCoefficient(constants, member.constant, link.label);
				}
				MinMaxLinkRow sum;
				sum_terms.Flush(sum.indices, sum.coefficients);
				auto rc = sum.LeftHandSide(static_cast<int>(indexer.Get(value_var, group.first_row)));
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
			EmitExtremumLink(input, indexer, spec, row_members.Source(group), var_names, policy);
		}
	}
	input.reducer_value_links.clear();
}

} // namespace duckdb
