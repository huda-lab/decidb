//===----------------------------------------------------------------------===//
//                         DeciDB
//
// src/decidb/formulation/linearization_reducer_value.cpp
//
// Stage 06 — the helper decisions of the value rewrite (language/guide.md §6.4;
// 05_optimizer/done.md, "The value rewrite"). Stage 05 replaced each decision reducer
// of a VALUE-shape clause, and each range frame over decisions, by a helper decision and
// recorded what it holds; stage 08 listed its members per group -- a `by` group, or a
// frame's window (ReducerValueMembers). This boxes every helper by its members' reach and
// emits its definition: a row per group for SUM and AVG, an extremum link per group for
// MIN and MAX. The helper's own column enters only here: stage 08 hands over the members
// alone.
//
//===----------------------------------------------------------------------===//
#include "duckdb/common/decide_profile.hpp"
#include "duckdb/decidb/formulation/ilp_linearization.hpp"
#include "duckdb/decidb/formulation/ilp_linearization_internal.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace duckdb {

using namespace decide_linearize; // NOLINT: internal DECIDE linearization helpers

namespace {

bool IsExtremum(const ReducerValueLinkSpec &link) {
	return link.agg_name == "min" || link.agg_name == "max";
}

//! The members of group `group`: [first, last).
struct MemberSpan {
	idx_t first;
	idx_t last;
};

MemberSpan MembersOf(const ReducerValueMembers &members, idx_t group) {
	return MemberSpan {members.group_begin[group], members.group_begin[group + 1]};
}

//! The signed reachable interval of member `member`'s terms under the box, its constant left out, an end a term
//! leaves open infinite: DecideRowSignedRange's reading, over a listed member instead of a row.
void MemberRange(const ReducerValueMembers &members, idx_t member, const SolverInput &input, double &out_lo,
                 double &out_hi) {
	constexpr double INF = std::numeric_limits<double>::infinity();
	double lo = 0.0, hi = 0.0;
	bool lo_open = false, hi_open = false;
	for (idx_t t = members.member_begin[member]; t < members.member_begin[member + 1]; t++) {
		const double coef = members.coefficients[t];
		if (std::abs(coef) < 1e-15) {
			continue;
		}
		const idx_t v = members.variables[t];
		const double lb = input.lower_bounds[v];
		const double ub = input.upper_bounds[v];
		// The low end of a positive term comes from the variable's low end, and from its high end when the
		// coefficient flips the sense.
		const bool flips = coef < 0.0;
		if (flips ? ub >= 1e20 : lb <= -1e20) {
			lo_open = true;
		} else {
			lo += coef * (flips ? ub : lb);
		}
		if (flips ? lb <= -1e20 : ub >= 1e20) {
			hi_open = true;
		} else {
			hi += coef * (flips ? lb : ub);
		}
	}
	out_lo = lo_open ? -INF : lo;
	out_hi = hi_open ? INF : hi;
}

//! The reach of member `member`, its constant included.
AuxRange MemberReach(const ReducerValueMembers &members, idx_t member, const SolverInput &input) {
	double lo, hi;
	MemberRange(members, member, input, lo, hi);
	AuxRange range;
	range.CoverRowSided(lo + members.constants[member], hi + members.constants[member], DConstants::INVALID_INDEX);
	return range;
}

//! The reach of one group's value: the sum of its members' brackets for SUM and AVG
//! (an AVG's coefficients already carry the 1/n), their cover for MIN and MAX. Each
//! end is folded or marked open on its own, like every other auxiliary box.
AuxRange GroupReach(const ReducerValueLinkSpec &link, const SolverInput &input, idx_t group) {
	AuxRange reach;
	const bool extremum = IsExtremum(link);
	double sum_lo = 0.0, sum_hi = 0.0;
	auto span = MembersOf(link.members, group);
	for (idx_t m = span.first; m < span.last; m++) {
		double lo, hi;
		MemberRange(link.members, m, input, lo, hi);
		const double constant = link.members.constants[m];
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

//! The reads of each instance of a helper every reader lets stage 06 inline: by the rows and the objective, or by its
//! outer link's members (InlineReducerValueLinks).
struct HelperReads {
	vector<idx_t> count;

	void Read(const ReducerValueLinkSpec &link, idx_t instance) {
		if (instance >= count.size()) {
			throw InternalException("DECIDE: %s is read at an instance it does not have", link.label);
		}
		count[instance]++;
	}
};

//! Whether writing `link`'s members where they are read adds no nonzero to the model (D89): an instance of `m` terms
//! read `k` times costs `m * k` inlined, against `m + 1` for its definition and `k` for its reads. One reader always
//! qualifies; so does a sum of three read twice (West's, deck p. 115). An instance with no member is absent, and no
//! reader reads it (InlineReducerValueLinks counts none there).
bool InliningAddsNoNonzero(const ReducerValueLinkSpec &link, const HelperReads &reads,
                           const vector<idx_t> &group_of_instance) {
	idx_t inlined = 0, defined = 0;
	for (idx_t instance = 0; instance < group_of_instance.size(); instance++) {
		const idx_t group = group_of_instance[instance];
		if (group == DConstants::INVALID_INDEX) {
			continue;
		}
		const idx_t k = reads.count[instance];
		auto span = MembersOf(link.members, group);
		const idx_t m = link.members.member_begin[span.last] - link.members.member_begin[span.first];
		inlined += m * k;
		defined += m + 1 + k;
	}
	return inlined <= defined;
}

//! Whether every group of `link` has one member: a MIN or MAX that is then its members.
bool EveryGroupSingleton(const ReducerValueLinkSpec &link) {
	for (idx_t g = 0; g < link.members.GroupCount(); g++) {
		auto span = MembersOf(link.members, g);
		if (span.last - span.first > 1) {
			return false;
		}
	}
	return true;
}

//! Whether `ec` is a linear row of a plain clause, the only row that can read a helper's members in place of its
//! column: no hard MIN/MAX, `<>`, Big-M ABS pair, product or square, and no bound stage 08 may fold into its decision's
//! box, which would be lost with the column.
bool IsPlainLinearRow(const EvaluatedConstraint &ec) {
	return ec.minmax_agg_type.empty() && ec.ne_clause_idx == DConstants::INVALID_INDEX &&
	       ec.abs_aux_idx == DConstants::INVALID_INDEX && ec.bilinear_terms.empty() && !ec.has_quadratic &&
	       ec.admitted_lower.Empty();
}

//! Where a helper is read (D89): whether a reader holds its column, whether rows or the objective read it, and the one
//! other link whose members read it, if any.
struct HelperPlace {
	bool held = false;
	bool read_by_readers = false;
	idx_t outer = DConstants::INVALID_INDEX;

	//! Its members can be written where it is read: no reader holds its column, and it is read by rows and the
	//! objective or by one outer link's members, not both.
	bool Inlinable() const {
		return !held && !(read_by_readers && outer != DConstants::INVALID_INDEX);
	}
};

//! Where each helper of `link_of` is read, over every reader stage 06 holds and the decisions `held` the caller reads
//! elsewhere. A linear row of a plain clause and the linear objective can read a helper's members, and so can the
//! members of one other link, pinned on one side if a MIN or MAX; every other reader -- a product, a square, a frame
//! read, a bilinear or ABS link, a row of a hard MIN/MAX, `<>` or ABS -- reads its column.
vector<HelperPlace> PlaceHelpers(const SolverInput &input, const vector<idx_t> &held, const vector<idx_t> &link_of) {
	auto &links = input.reducer_value_links;
	vector<HelperPlace> place(links.size());
	auto link = [&](idx_t var) {
		return var < link_of.size() ? link_of[var] : DConstants::INVALID_INDEX;
	};
	auto read = [&](idx_t var, bool takes_members) {
		const idx_t i = link(var);
		if (i == DConstants::INVALID_INDEX) {
			return;
		}
		if (takes_members) {
			place[i].read_by_readers = true;
		} else {
			place[i].held = true;
		}
	};
	for (auto var : held) {
		read(var, false);
	}
	// A frame reads its base's column at the row it selects.
	for (auto &frame_read : input.frame_reads) {
		read(frame_read.base, false);
	}
	for (auto &spec : input.bilinear_links) {
		read(spec.bool_var_idx, false);
		read(spec.other_var_idx, false);
	}
	for (auto &ec : input.constraints) {
		const bool plain = IsPlainLinearRow(ec);
		for (auto var : ec.variable_indices) {
			read(var, plain);
		}
		for (auto &term : ec.bilinear_terms) {
			read(term.var_a, false);
			read(term.var_b, false);
		}
		for (auto &group : ec.quadratic_groups) {
			for (auto var : group.variable_indices) {
				read(var, false);
			}
		}
	}
	for (auto var : input.objective_variable_indices) {
		read(var, true);
	}
	for (auto var : input.quadratic_inner_variable_indices) {
		read(var, false);
	}
	for (auto &term : input.bilinear_objective_terms) {
		read(term.var_a, false);
		read(term.var_b, false);
	}
	for (idx_t k = 0; k < links.size(); k++) {
		for (auto var : links[k].members.variables) {
			const idx_t i = link(var);
			if (i == DConstants::INVALID_INDEX) {
				continue;
			}
			if (place[i].outer != DConstants::INVALID_INDEX && place[i].outer != k) {
				place[i].held = true;
			}
			place[i].outer = k;
		}
	}
	// An outer MIN or MAX pinned on both sides states each member in two rows.
	for (auto &p : place) {
		if (p.outer == DConstants::INVALID_INDEX) {
			continue;
		}
		auto &holder = links[p.outer];
		if (IsExtremum(holder) && holder.need_envelope && holder.need_closing) {
			p.held = true;
		}
	}
	return place;
}

} // namespace

bool InlineReducerValueLinks(SolverInput &input, const vector<idx_t> &held) {
	auto &links = input.reducer_value_links;
	vector<idx_t> link_of(input.num_decide_vars, DConstants::INVALID_INDEX);
	for (idx_t i = 0; i < links.size(); i++) {
		if (links[i].value_var < link_of.size()) {
			link_of[links[i].value_var] = i;
		}
	}
	auto place = PlaceHelpers(input, held, link_of);
	bool any = false;
	for (idx_t i = 0; i < links.size(); i++) {
		if (!place[i].Inlinable()) {
			if (links[i].value_var < link_of.size()) {
				link_of[links[i].value_var] = DConstants::INVALID_INDEX;
			}
			continue;
		}
		any = true;
	}
	if (!any) {
		return false;
	}
	auto candidate = [&](idx_t var) {
		return var < link_of.size() ? link_of[var] : DConstants::INVALID_INDEX;
	};
	// A helper's instances: its scope's classes, or one. A helper inlined below keeps the scope that names them.
	auto mapping_of = [&](idx_t var) -> const KeyedMapping * {
		auto &scope = input.variable_scopes[var];
		if (scope.keyed_scope_idx != DConstants::INVALID_INDEX &&
		    (scope.scope == DecideVarScope::KEYED || scope.scope == DecideVarScope::INLINED)) {
			return &input.keyed_mappings[scope.keyed_scope_idx];
		}
		if (scope.scope != DecideVarScope::GLOBAL && scope.scope != DecideVarScope::INLINED) {
			throw InternalException("DECIDE: a helper decision is neither keyed nor query-wide");
		}
		return nullptr;
	};
	auto instance_of = [&](idx_t var, idx_t row) {
		auto mapping = mapping_of(var);
		return mapping ? mapping->row_to_class[row] : idx_t(0);
	};
	vector<HelperReads> reads(links.size());
	vector<vector<idx_t>> group_of_instance(links.size());
	for (idx_t i = 0; i < links.size(); i++) {
		if (!place[i].Inlinable()) {
			continue;
		}
		auto mapping = mapping_of(links[i].value_var);
		const idx_t instances = mapping ? mapping->num_classes : 1;
		reads[i].count.assign(instances, 0);
		group_of_instance[i].assign(instances, DConstants::INVALID_INDEX);
		for (idx_t g = 0; g < links[i].members.GroupCount(); g++) {
			group_of_instance[i][instance_of(links[i].value_var, links[i].members.group_rows[g])] = g;
		}
	}
	// The rows, as SolverModel::Build emits them: a row of its own per kept row of a per-row clause, one per group of
	// an aggregate, where a group's rows reading one instance sum into one row. A row whose instance is absent was
	// skipped.
	for (auto &ec : input.constraints) {
		const bool grouped = !ec.row_group_ids.empty();
		for (idx_t t = 0; t < ec.variable_indices.size(); t++) {
			const idx_t var = ec.variable_indices[t];
			const idx_t link = candidate(var);
			if (link == DConstants::INVALID_INDEX) {
				continue;
			}
			auto &column = ec.row_coefficients[t];
			std::unordered_set<uint64_t> seen;
			for (idx_t row = 0; row < input.num_rows; row++) {
				const idx_t group = grouped ? ec.row_group_ids[row] : 0;
				if (group == DConstants::INVALID_INDEX || column.Get(row) == 0.0) {
					continue;
				}
				const idx_t instance = instance_of(var, row);
				if (group_of_instance[link][instance] == DConstants::INVALID_INDEX) {
					throw InternalException("DECIDE: a row reads %s where it is absent", links[link].label);
				}
				const idx_t emitted = ec.lhs_is_aggregate ? group : row;
				if (seen.insert((static_cast<uint64_t>(emitted) << 32) ^ static_cast<uint64_t>(instance)).second) {
					reads[link].Read(links[link], instance);
				}
			}
		}
	}

	// The objective is one reader of each instance it reads, however many of its terms do. An absent instance reads
	// as no value, its term dropped (D84): no read.
	{
		vector<std::unordered_set<idx_t>> read_by_objective(links.size());
		for (idx_t t = 0; t < input.objective_variable_indices.size(); t++) {
			const idx_t var = input.objective_variable_indices[t];
			const idx_t link = candidate(var);
			if (link == DConstants::INVALID_INDEX || t >= input.objective_coefficients.size()) {
				continue;
			}
			auto &column = input.objective_coefficients[t];
			for (idx_t row = 0; row < input.num_rows && row < column.Size(); row++) {
				const idx_t instance = instance_of(var, row);
				if (column.Get(row) != 0.0 && group_of_instance[link][instance] != DConstants::INVALID_INDEX) {
					read_by_objective[link].insert(instance);
				}
			}
		}
		for (idx_t link = 0; link < links.size(); link++) {
			for (auto instance : read_by_objective[link]) {
				reads[link].Read(links[link], instance);
			}
		}
	}

	// Another link's members. An outer SUM or AVG states each group in one row, so the terms of a group reading one
	// instance are one read; an outer MIN or MAX states each member in a row of its own, so each member reading it is
	// one.
	for (idx_t k = 0; k < links.size(); k++) {
		auto &members = links[k].members;
		const bool row_per_member = IsExtremum(links[k]);
		// (helper, instance) pairs already read, within the group or the member.
		std::unordered_set<uint64_t> seen;
		for (idx_t g = 0; g < members.GroupCount(); g++) {
			seen.clear();
			auto span = MembersOf(members, g);
			for (idx_t m = span.first; m < span.last; m++) {
				if (row_per_member) {
					seen.clear();
				}
				for (idx_t t = members.member_begin[m]; t < members.member_begin[m + 1]; t++) {
					const idx_t link = candidate(members.variables[t]);
					if (link == DConstants::INVALID_INDEX) {
						continue;
					}
					const idx_t instance = instance_of(members.variables[t], members.rows[t]);
					if (seen.insert((static_cast<uint64_t>(link) << 32) ^ static_cast<uint64_t>(instance)).second) {
						reads[link].Read(links[link], instance);
					}
				}
			}
		}
	}

	// Writes link `i`'s instances that `written` holds into its outer link's members, each scaled by the coefficient
	// that read it.
	auto write_into_outer = [&](idx_t i, const vector<bool> &written) {
		auto &link = links[i];
		auto &outer = links[place[i].outer].members;
		ReducerValueMembers expanded;
		for (idx_t g = 0; g < outer.GroupCount(); g++) {
			expanded.AddGroup(outer.group_rows[g]);
			auto span = MembersOf(outer, g);
			for (idx_t m = span.first; m < span.last; m++) {
				expanded.AddMember(outer.constants[m]);
				for (idx_t t = outer.member_begin[m]; t < outer.member_begin[m + 1]; t++) {
					const idx_t instance =
					    outer.variables[t] == link.value_var ? instance_of(link.value_var, outer.rows[t]) : 0;
					if (outer.variables[t] != link.value_var || !written[instance]) {
						expanded.AddTerm(outer.variables[t], outer.rows[t], outer.coefficients[t]);
						continue;
					}
					const double c = outer.coefficients[t];
					const idx_t group = group_of_instance[i][instance];
					if (group == DConstants::INVALID_INDEX) {
						throw InternalException("DECIDE: %s's member reads %s where it is absent",
						                        links[place[i].outer].label, link.label);
					}
					auto inner = MembersOf(link.members, group);
					for (idx_t n = inner.first; n < inner.last; n++) {
						expanded.constants.back() += c * link.members.constants[n];
						for (idx_t u = link.members.member_begin[n]; u < link.members.member_begin[n + 1]; u++) {
							expanded.AddTerm(link.members.variables[u], link.members.rows[u],
							                 c * link.members.coefficients[u]);
						}
					}
				}
			}
		}
		outer = std::move(expanded);
	};

	// The SUM and AVG reducers of a nest -- one nested in a SUM or AVG, and one a nested reducer stands in -- are
	// written where they are read whatever that costs, as the deck draws a nest's model (pp. 115, 119; owner,
	// 2026-11-05); every other helper, a range frame's among them, only where that adds no nonzero (D89).
	vector<bool> flattened(links.size(), false);
	for (idx_t i = 0; i < links.size(); i++) {
		const idx_t outer = place[i].outer;
		if (outer == DConstants::INVALID_INDEX || links[i].frame || links[outer].frame || IsExtremum(links[outer])) {
			continue;
		}
		flattened[outer] = true;
		flattened[i] = !IsExtremum(links[i]);
	}

	// Inline, innermost first: a link folded into its outer one has had its own inner links folded into it already,
	// so what it hands on is decisions and kept helpers alone.
	vector<bool> folded(links.size(), false);
	bool inlined_any = false;
	for (idx_t i = 0; i < links.size(); i++) {
		auto &link = links[i];
		if (!place[i].Inlinable()) {
			continue;
		}
		auto &count = reads[i].count;
		// A MIN or MAX group of one member is that member (deck p. 112): written in, group by group, wherever the
		// helper may be. A link whose every group is one is written in whole.
		vector<bool> single(count.size(), false);
		bool any_single = false;
		if (IsExtremum(link)) {
			for (idx_t instance = 0; instance < count.size(); instance++) {
				const idx_t group = group_of_instance[i][instance];
				if (group != DConstants::INVALID_INDEX) {
					auto span = MembersOf(link.members, group);
					single[instance] = span.last - span.first == 1;
					any_single = any_single || single[instance];
				}
			}
		}
		const bool whole = !IsExtremum(link) || EveryGroupSingleton(link);
		if (!whole && !any_single) {
			continue;
		}
		const bool into_outer = place[i].outer != DConstants::INVALID_INDEX;
		if (into_outer) {
			if (place[i].outer <= i) {
				throw InternalException("DECIDE: %s's outer link is not listed after it", link.label);
			}
			// On its closing side an outer MIN or MAX may be stated natively, over columns: a member that is an
			// expression then costs a column and a row of its own, which pays only where it replaces the one column
			// and definition of an instance read once.
			auto &outer = links[place[i].outer];
			if (IsExtremum(outer) && outer.need_closing &&
			    std::any_of(count.begin(), count.end(), [](idx_t k) { return k > 1; })) {
				continue;
			}
		}
		if (!whole) {
			// Its groups of one member, written in alone: an instance of one term read `k` times costs `k` written in
			// against `2 + k` defined. The others keep their columns and definitions.
			input.written_in.resize(input.num_decide_vars);
			input.written_in[link.value_var] = single;
			inlined_any = true;
			if (into_outer) {
				write_into_outer(i, single);
			}
			continue;
		}
		if (!flattened[i] && !InliningAddsNoNonzero(link, reads[i], group_of_instance[i])) {
			continue;
		}
		input.variable_scopes[link.value_var].scope = DecideVarScope::INLINED;
		inlined_any = true;
		if (!into_outer) {
			link.inlined = true;
			continue;
		}
		write_into_outer(i, vector<bool>(count.size(), true));
		folded[i] = true;
	}
	idx_t kept = 0, inlined = 0;
	for (idx_t i = 0; i < links.size(); i++) {
		inlined += folded[i] || links[i].inlined;
		if (!folded[i]) {
			if (kept != i) {
				links[kept] = std::move(links[i]);
			}
			kept++;
		}
	}
	links.resize(kept);
	DecideProfileScope::Counter("anr.inlined_helpers", static_cast<double>(inlined));
	return inlined_any;
}

void DeriveReducerValueBounds(SolverInput &input, const VarIndexer &indexer) {
	constexpr double INF = std::numeric_limits<double>::infinity();
	input.instance_reach_lower.resize(input.num_decide_vars);
	input.instance_reach_upper.resize(input.num_decide_vars);
	for (auto &link : input.reducer_value_links) {
		auto &members = link.members;
		// Each instance is boxed by the reach of the group it stands for: the helper has one instance per group, so a
		// small group is not boxed as wide as the largest. An instance no group reaches keeps the open ends, and
		// reads the decision's box.
		const idx_t value_var = link.value_var;
		// An inlined helper has no column, but its instances still bound what its readers read.
		const idx_t num_instances = indexer.NamedInstances(value_var);
		vector<AuxRange> reach(num_instances);
		vector<bool> reached(num_instances, false);
		AuxRange box;
		for (idx_t group = 0; group < members.GroupCount(); group++) {
			auto span = MembersOf(members, group);
			if (span.first == span.last) {
				continue;
			}
			auto group_reach = GroupReach(link, input, group);
			auto instance = indexer.InstanceOf(value_var, members.group_rows[group]);
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
		auto &members = link.members;
		const double value_lo = input.lower_bounds[link.value_var] <= -1e20 ? -INF : input.lower_bounds[link.value_var];
		const double value_hi = input.upper_bounds[link.value_var] >= 1e20 ? INF : input.upper_bounds[link.value_var];
		if (std::isinf(value_lo) && std::isinf(value_hi)) {
			continue;
		}
		const bool extremum = IsExtremum(link);
		if (extremum && !link.need_envelope) {
			continue; // a closing side alone pins no member
		}
		// A SUM or AVG helper bounds its group's members on the sides its definition states: at most its top under
		// `value >= members` (the envelope), at least its floor under `value <= members` (the closing side).
		const double sum_lo = link.need_closing ? value_lo : -INF;
		const double sum_hi = link.need_envelope ? value_hi : INF;
		// Only a helper's column takes a bound here, so a member reading none -- every member of a reducer over
		// decisions alone -- is passed over unread.
		auto reads_helper = [&](idx_t first, idx_t last) {
			for (idx_t t = members.member_begin[first]; t < members.member_begin[last]; t++) {
				const idx_t v = members.variables[t];
				if (v < is_helper.size() && is_helper[v]) {
					return true;
				}
			}
			return false;
		};
		for (idx_t group = 0; group < members.GroupCount(); group++) {
			auto span = MembersOf(members, group);
			if (!reads_helper(span.first, span.last)) {
				continue;
			}
			if (!link.inlined &&
			    !indexer.HasColumn(link.value_var, indexer.InstanceOf(link.value_var, members.group_rows[group]))) {
				continue; // written into its readers: the box below is not derived from it, so it does not bound it
			}
			vector<MemberTerm> group_terms;
			double group_constant = 0.0;
			for (idx_t m = span.first; m < span.last; m++) {
				if (extremum && !reads_helper(m, m + 1)) {
					continue;
				}
				vector<MemberTerm> member;
				for (idx_t t = members.member_begin[m]; t < members.member_begin[m + 1]; t++) {
					const double c = members.coefficients[t];
					if (std::abs(c) >= 1e-15) {
						const idx_t var = members.variables[t];
						member.push_back(MakeMemberTerm(input, var, indexer.Get(var, members.rows[t]), c));
					}
				}
				if (extremum) {
					// A MAX's envelope keeps every member at or under the helper's top, a
					// MIN's at or over its floor.
					const bool is_max = link.agg_name == "max";
					BoundTermsOfASum(CombineColumns(input, member), members.constants[m], is_max ? -INF : value_lo,
					                 is_max ? value_hi : INF, is_helper, column_lo, column_hi);
					continue;
				}
				group_terms.insert(group_terms.end(), member.begin(), member.end());
				group_constant += members.constants[m];
			}
			if (!extremum && !group_terms.empty()) {
				BoundTermsOfASum(CombineColumns(input, group_terms), group_constant, sum_lo, sum_hi, is_helper,
				                 column_lo, column_hi);
			}
		}
	}
	// A helper's box is one for every instance it defines: tightened only when each of
	// them was bounded, by the loosest. Its other columns -- the rows its path drops, the
	// windows that read nothing -- are read by no row. An inlined helper has no column; its
	// box holds through the rows it is written into, for every instance.
	for (auto &link : input.reducer_value_links) {
		if (link.inlined) {
			continue;
		}
		const idx_t var = link.value_var;
		auto &members = link.members;
		double loosest_lo = INF, loosest_hi = -INF;
		bool every_lo = true, every_hi = true, any = false;
		for (idx_t group = 0; group < members.GroupCount(); group++) {
			auto span = MembersOf(members, group);
			if (span.first == span.last) {
				continue;
			}
			if (!indexer.HasColumn(var, indexer.InstanceOf(var, members.group_rows[group]))) {
				continue; // written into its readers as its one member: no column to box
			}
			any = true;
			const idx_t column = indexer.Get(var, members.group_rows[group]);
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
	SparseCoeffAccumulator member_terms;
	// One scratch row for every group of every SUM or AVG helper: dense over the decisions' columns while they fit, so
	// a helper of a million windows allocates it once.
	SparseCoeffAccumulator sum_terms;
	input.inlined_helper_of.assign(input.num_decide_vars, DConstants::INVALID_INDEX);
	for (auto &link : input.reducer_value_links) {
		// The instances written into their readers (InlineReducerValueLinks): every one of a helper inlined whole, or a
		// MIN or MAX group of one member. Its readers read its members there (SolverModel::Build).
		auto *written_in = link.value_var < input.written_in.size() && !input.written_in[link.value_var].empty()
		                       ? &input.written_in[link.value_var]
		                       : nullptr;
		if (link.inlined || written_in) {
			SolverInput::InlinedHelper helper;
			helper.group_of_instance.assign(indexer.NamedInstances(link.value_var), DConstants::INVALID_INDEX);
			for (idx_t g = 0; g < link.members.GroupCount(); g++) {
				const idx_t instance = indexer.InstanceOf(link.value_var, link.members.group_rows[g]);
				if (link.inlined || (*written_in)[instance]) {
					helper.group_of_instance[instance] = g;
				}
			}
			helper.members = link.inlined ? std::move(link.members) : link.members;
			helper.label = link.label;
			input.inlined_helper_of[link.value_var] = input.inlined_helpers.size();
			input.inlined_helpers.push_back(std::move(helper));
			if (link.inlined) {
				continue;
			}
		}
		const bool is_max = link.agg_name == "max";
		const idx_t value_var = link.value_var;
		auto &members = link.members;
		// A member is its expression negated with its constant apart, the shape the extremum link takes; the equality
		// reads it the same way. Its columns are accumulated: a keyed decision may stand in several terms, and a row
		// states each column once. A helper serves every clause that reads it, so a sum past a DOUBLE names it as
		// written.
		auto fill_member = [&](idx_t m, MinMaxLinkRow &member) {
			member_terms.BeginForDecisions(indexer, members.member_begin[m + 1] - members.member_begin[m], link.label);
			for (idx_t t = members.member_begin[m]; t < members.member_begin[m + 1]; t++) {
				if (members.coefficients[t] != 0.0) {
					member_terms.Add(static_cast<int>(indexer.Get(members.variables[t], members.rows[t])),
					                 -members.coefficients[t]);
				}
			}
			member_terms.Flush(member.indices, member.coefficients);
			member.constant = members.constants[m];
		};
		for (idx_t group = 0; group < members.GroupCount(); group++) {
			auto span = MembersOf(members, group);
			if (span.first == span.last) {
				continue;
			}
			const idx_t group_row = members.group_rows[group];
			if (written_in && (*written_in)[indexer.InstanceOf(value_var, group_row)]) {
				continue; // its readers read its one member
			}
			if (!IsExtremum(link)) {
				// `value - sum of members' expressions` against `sum of their constants`: the side the clauses push
				// the value against (`value >= sum` when they push it down, `value <= sum` when up), the equality only
				// when both sides are needed. The members' columns are accumulated first: a keyed decision may stand
				// in several members of the group, and a row states each column once.
				sum_terms.BeginForDecisions(indexer, 0, link.label);
				double constants = 0.0;
				for (idx_t m = span.first; m < span.last; m++) {
					for (idx_t t = members.member_begin[m]; t < members.member_begin[m + 1]; t++) {
						if (members.coefficients[t] != 0.0) {
							sum_terms.Add(static_cast<int>(indexer.Get(members.variables[t], members.rows[t])),
							              -members.coefficients[t]);
						}
					}
					AddCoefficient(constants, members.constants[m], link.label);
				}
				MinMaxLinkRow sum;
				sum_terms.Flush(sum.indices, sum.coefficients);
				auto rc = sum.LeftHandSide(static_cast<int>(indexer.Get(value_var, group_row)));
				rc.sense = link.need_envelope && link.need_closing ? '=' : link.need_envelope ? '>' : '<';
				rc.rhs = constants;
				rc.kind = ConstraintKind::STRUCTURAL;
				input.global_constraints.push_back(std::move(rc));
				continue;
			}
			AuxRange family;
			for (idx_t m = span.first; m < span.last; m++) {
				family.Cover(MemberReach(members, m, input));
			}
			ExtremumLinkSpec spec;
			spec.result_column = indexer.Get(value_var, group_row);
			spec.is_max = is_max;
			spec.need_envelope = link.need_envelope;
			spec.need_closing = link.need_closing;
			// Deactivated, a closing row reads `value - member <= M` for MAX (`>= -M` for
			// MIN): the helper's box end against the family's far end. The end is this
			// group's own instance's (its reach, DeriveReducerValueBounds), not the
			// decision's cover of every group's.
			const idx_t instance = indexer.InstanceOf(value_var, group_row);
			const double value_hi = input.InstanceBoxUpper(value_var, instance, input.upper_bounds);
			const double value_lo = input.InstanceBoxLower(value_var, instance, input.lower_bounds);
			const bool value_hi_open = value_hi >= 1e20;
			const bool value_lo_open = value_lo <= -1e20;
			spec.closing_underivable =
			    is_max ? (value_hi_open || family.lo_unbounded) : (value_lo_open || family.hi_unbounded);
			spec.closing_big_m = spec.closing_underivable ? 0.0 : is_max ? value_hi - family.lo : family.hi - value_lo;
			if (spec.closing_underivable) {
				spec.blame_var = FirstUnboundedMemberVariable(members, input.lower_bounds, input.upper_bounds);
			}
			spec.written = link.label;
			spec.label = link.label;
			spec.envelope_kind = ConstraintKind::STRUCTURAL;
			spec.closing_kind = ConstraintKind::STRUCTURAL;
			ExtremumMemberSource source;
			source.count = span.last - span.first;
			source.fill_link = [&](idx_t i, MinMaxLinkRow &member) {
				fill_member(span.first + i, member);
			};
			source.range = [&](idx_t i) {
				return MemberReach(members, span.first + i, input);
			};
			EmitExtremumLink(input, indexer, spec, source, var_names, policy);
		}
	}
	input.reducer_value_links.clear();
}

idx_t decide_linearize::FirstUnboundedMemberVariable(const ReducerValueMembers &members,
                                                     const vector<double> &lower_bounds,
                                                     const vector<double> &upper_bounds) {
	for (idx_t t = 0; t < members.variables.size(); t++) {
		const idx_t v = members.variables[t];
		if (members.coefficients[t] != 0.0 && v < lower_bounds.size() && v < upper_bounds.size() &&
		    (DecideIsOpenAbove(upper_bounds[v]) || DecideIsOpenBelow(lower_bounds[v]))) {
			return v;
		}
	}
	return DConstants::INVALID_INDEX;
}

} // namespace duckdb
