//===----------------------------------------------------------------------===//
//                         DecidB
//
// src/decidb/formulation/linearization_reducer_value.cpp
//
// Stage 06 — the helper decisions of the value rewrite (syntax_reference.md §6.4;
// 05_optimizer/done.md, "The value rewrite"). Stage 05 replaced each decision reducer
// of a VALUE-shape clause, and each range frame over decisions, by a helper decision and
// recorded what it holds; stage 08 listed that reducer's members per group -- a `by`
// group, or a frame's window (ReducerValueMembers). This boxes every helper by its
// members' reach and emits its definition: an equality row per group for SUM and AVG,
// an extremum link per group for MIN and MAX. The helper's own column enters only
// here: stage 08 hands over the members alone.
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

//! Signed reachable interval of member `member`'s terms under the box, its constant
//! left out, an end a term leaves open infinite: DecideRowSignedRange's reading, over a
//! listed member instead of a row.
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
		if (v >= input.lower_bounds.size() || v >= input.upper_bounds.size()) {
			lo_open = hi_open = true;
			continue;
		}
		const double lb = input.lower_bounds[v];
		const double ub = input.upper_bounds[v];
		// The low end of a positive term comes from the variable's low end, and from its
		// high end when the coefficient flips the sense.
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

//! The first decision among the members whose box is open, which a refusal names.
idx_t FirstUnboundedMemberVariable(const ReducerValueMembers &members, const SolverInput &input) {
	for (auto v : members.variables) {
		if (v < input.lower_bounds.size() && v < input.upper_bounds.size() &&
		    (input.upper_bounds[v] >= 1e20 || input.lower_bounds[v] <= -1e20)) {
			return v;
		}
	}
	return DConstants::INVALID_INDEX;
}

//! The reach of group `group`'s value: the sum of its members' brackets for SUM and AVG
//! (an AVG's coefficients already carry the 1/n), their cover for MIN and MAX. Each
//! end is folded or marked open on its own, like every other auxiliary box.
AuxRange GroupReach(const ReducerValueLinkSpec &link, const SolverInput &input, idx_t group) {
	AuxRange reach;
	const bool extremum = IsExtremum(link);
	auto span = MembersOf(link.members, group);
	double sum_lo = 0.0, sum_hi = 0.0;
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

} // namespace

namespace {

//! The reads of each instance of a helper the plan lets stage 06 inline: by the rows and
//! the objective, or by its outer link's members (InlineReducerValueLinks).
struct HelperReads {
	vector<idx_t> count;

	void Read(const ReducerValueLinkSpec &link, idx_t instance) {
		if (instance >= count.size()) {
			throw InternalException("DECIDE: %s is read at an instance it does not have", link.label);
		}
		count[instance]++;
	}
};

//! Whether writing `link`'s members where they are read adds no nonzero to the model (D83):
//! an instance of `m` terms read `k` times costs `m * k` inlined, against `m + 1` for its
//! definition and `k` for its reads. One reader always qualifies; so does a sum of three
//! read twice (West's, deck p. 115). An instance with no member is absent, and its readers
//! were skipped (rows) or dropped (members) where it is, so none reads it.
bool InliningAddsNoNonzero(const ReducerValueLinkSpec &link, const HelperReads &reads,
                           const vector<idx_t> &group_of_instance) {
	idx_t inlined = 0, defined = 0;
	for (idx_t instance = 0; instance < group_of_instance.size(); instance++) {
		const idx_t group = group_of_instance[instance];
		const idx_t k = reads.count[instance];
		if (group == DConstants::INVALID_INDEX) {
			if (k > 0) {
				throw InternalException("DECIDE: %s is read where it is absent", link.label);
			}
			continue;
		}
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

//! Whether `ec` is a linear row of a plain clause, the only row the plan lets read a
//! helper it inlines into its readers (the linear form's census): no hard MIN/MAX, `<>`,
//! Big-M ABS pair, product or square.
bool IsPlainLinearRow(const EvaluatedConstraint &ec) {
	return ec.minmax_agg_type.empty() && ec.ne_clause_idx == DConstants::INVALID_INDEX &&
	       ec.abs_aux_idx == DConstants::INVALID_INDEX && ec.bilinear_terms.empty() && !ec.has_quadratic;
}

} // namespace

void InlineReducerValueLinks(SolverInput &input) {
	auto &links = input.reducer_value_links;
	vector<idx_t> link_of(input.num_decide_vars, DConstants::INVALID_INDEX);
	bool any = false;
	for (idx_t i = 0; i < links.size(); i++) {
		if (links[i].inlining != DecideHelperInlining::NONE && links[i].value_var < link_of.size()) {
			link_of[links[i].value_var] = i;
			any = true;
		}
	}
	if (!any) {
		return;
	}
	auto candidate = [&](idx_t var) {
		return var < link_of.size() ? link_of[var] : DConstants::INVALID_INDEX;
	};
	// A helper's instances: its scope's entities, or one. A helper inlined below keeps the
	// scope that names them.
	auto mapping_of = [&](idx_t var) -> const EntityMapping * {
		auto &scope = input.variable_scopes[var];
		const bool inlined = scope.scope == DecideVarScope::INLINED;
		if (scope.IsEntity() || (inlined && scope.entity_scope_idx != DConstants::INVALID_INDEX)) {
			return &input.entity_mappings[scope.entity_scope_idx];
		}
		if (!scope.IsScalar() && !inlined) {
			throw InternalException("DECIDE: a helper decision is neither keyed nor query-wide");
		}
		return nullptr;
	};
	auto instance_of = [&](idx_t var, idx_t row) {
		auto mapping = mapping_of(var);
		return mapping ? mapping->row_to_entity[row] : idx_t(0);
	};
	vector<HelperReads> reads(links.size());
	for (idx_t i = 0; i < links.size(); i++) {
		if (links[i].inlining != DecideHelperInlining::NONE) {
			auto mapping = mapping_of(links[i].value_var);
			reads[i].count.assign(mapping ? mapping->num_entities : 1, 0);
		}
	}
	// Where the plan said a helper is read is where it is read: a read anywhere else is the
	// census missing a reader, which would read a column inlining takes away.
	auto read_by_readers = [&](idx_t link) {
		if (links[link].inlining != DecideHelperInlining::INTO_READERS) {
			throw InternalException("DECIDE: %s is read by a row or the objective, which its plan does not list",
			                        links[link].label);
		}
	};

	// The rows, as SolverModel::Build reads them: a row of its own per kept row of a per-row
	// clause, one per group of an aggregate (where a group's rows reading one instance sum
	// into one row), and a decision beside an aggregate's reducers once per group's row.
	for (auto &ec : input.constraints) {
		const bool grouped = !ec.row_group_ids.empty();
		for (idx_t t = 0; t < ec.variable_indices.size(); t++) {
			const idx_t var = ec.variable_indices[t];
			const idx_t link = candidate(var);
			if (link == DConstants::INVALID_INDEX) {
				continue;
			}
			read_by_readers(link);
			if (!IsPlainLinearRow(ec)) {
				throw InternalException("DECIDE: %s is read by a row its plan does not list as a reader",
				                        links[link].label);
			}
			auto &reader = reads[link];
			auto &column = ec.row_coefficients[t];
			const bool reduced =
			    t < ec.linear_term_reductions.size() && ec.linear_term_reductions[t] == LinearTermReduction::SUM;
			if (ec.lhs_is_aggregate && !reduced && !mapping_of(var)) {
				if (!grouped) {
					reader.Read(links[link], 0);
					continue;
				}
				vector<bool> has_row(ec.num_groups, false);
				for (auto group : ec.row_group_ids) {
					if (group < ec.num_groups && !has_row[group]) {
						has_row[group] = true;
						reader.Read(links[link], 0);
					}
				}
				continue;
			}
			if (!ec.lhs_is_aggregate) {
				for (idx_t row = 0; row < input.num_rows; row++) {
					if ((grouped && ec.row_group_ids[row] == DConstants::INVALID_INDEX) || column.Get(row) == 0.0) {
						continue;
					}
					reader.Read(links[link], instance_of(var, row));
				}
				continue;
			}
			std::unordered_set<uint64_t> seen;
			for (idx_t row = 0; row < input.num_rows; row++) {
				const idx_t group = grouped ? ec.row_group_ids[row] : 0;
				if (group == DConstants::INVALID_INDEX || column.Get(row) == 0.0) {
					continue;
				}
				const idx_t instance = instance_of(var, row);
				if (seen.insert((static_cast<uint64_t>(group) << 32) ^ static_cast<uint64_t>(instance)).second) {
					reader.Read(links[link], instance);
				}
			}
		}
	}

	// The objective is one reader of each instance it reads, however many of its terms do.
	{
		vector<std::unordered_set<idx_t>> read_by_objective(links.size());
		for (idx_t t = 0; t < input.objective_variable_indices.size(); t++) {
			const idx_t var = input.objective_variable_indices[t];
			const idx_t link = candidate(var);
			if (link == DConstants::INVALID_INDEX || t >= input.objective_coefficients.size()) {
				continue;
			}
			read_by_readers(link);
			auto &column = input.objective_coefficients[t];
			const bool reduced = t < input.objective_term_reductions.size() &&
			                     input.objective_term_reductions[t] == LinearTermReduction::SUM;
			if (!reduced && !mapping_of(var)) {
				read_by_objective[link].insert(0);
				continue;
			}
			for (idx_t row = 0; row < input.num_rows && row < column.Size(); row++) {
				if (column.Get(row) != 0.0) {
					read_by_objective[link].insert(instance_of(var, row));
				}
			}
		}
		for (idx_t link = 0; link < links.size(); link++) {
			for (auto instance : read_by_objective[link]) {
				reads[link].Read(links[link], instance);
			}
		}
	}

	// Another link's members. An outer SUM or AVG states each group in one row, so the terms
	// of a group reading one instance are one read; an outer MIN or MAX states each member
	// in a row of its own, so each member reading it is one.
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
					if (links[link].inlining != DecideHelperInlining::INTO_OUTER || links[link].inline_outer != k) {
						throw InternalException("DECIDE: %s is read by %s's members, which its plan does not list",
						                        links[link].label, links[k].label);
					}
					const idx_t instance = instance_of(members.variables[t], members.rows[t]);
					if (seen.insert((static_cast<uint64_t>(link) << 32) ^ static_cast<uint64_t>(instance)).second) {
						reads[link].Read(links[link], instance);
					}
				}
			}
		}
	}

	// Inline, innermost first: a link folded into its outer one has had its own inner links
	// folded into it already, so what it hands on is decisions and kept helpers alone.
	vector<bool> folded(links.size(), false);
	for (idx_t i = 0; i < links.size(); i++) {
		auto &link = links[i];
		if (link.inlining == DecideHelperInlining::NONE) {
			continue;
		}
		if (IsExtremum(link) && !EveryGroupSingleton(link)) {
			continue;
		}
		auto &count = reads[i].count;
		const bool into_outer = link.inlining == DecideHelperInlining::INTO_OUTER;
		if (into_outer) {
			if (link.inline_outer <= i || link.inline_outer >= links.size()) {
				throw InternalException("DECIDE: %s's outer link is not listed after it", link.label);
			}
			// On its closing side an outer MIN or MAX may be stated natively, over columns: a
			// member that is an expression then costs a column and a row of its own, which pays
			// only where it replaces the one column and definition of an instance read once.
			auto &outer = links[link.inline_outer];
			if (IsExtremum(outer) && outer.need_envelope && outer.need_closing) {
				throw InternalException("DECIDE: %s's plan writes it into %s, pinned on both sides", link.label,
				                        outer.label);
			}
			if (IsExtremum(outer) && outer.need_closing &&
			    std::any_of(count.begin(), count.end(), [](idx_t k) { return k > 1; })) {
				continue;
			}
		}
		vector<idx_t> group_of_instance(count.size(), DConstants::INVALID_INDEX);
		for (idx_t g = 0; g < link.members.GroupCount(); g++) {
			group_of_instance[instance_of(link.value_var, link.members.group_rows[g])] = g;
		}
		if (!InliningAddsNoNonzero(link, reads[i], group_of_instance)) {
			continue;
		}
		input.variable_scopes[link.value_var].scope = DecideVarScope::INLINED;
		if (!into_outer) {
			link.inlined = true;
			continue;
		}
		// Written into the outer link's members, scaled by the coefficient that read it.
		auto &outer = links[link.inline_outer].members;
		ReducerValueMembers expanded;
		for (idx_t g = 0; g < outer.GroupCount(); g++) {
			expanded.AddGroup(outer.group_rows[g]);
			auto span = MembersOf(outer, g);
			for (idx_t m = span.first; m < span.last; m++) {
				expanded.AddMember(outer.constants[m]);
				for (idx_t t = outer.member_begin[m]; t < outer.member_begin[m + 1]; t++) {
					if (outer.variables[t] != link.value_var) {
						expanded.AddTerm(outer.variables[t], outer.rows[t], outer.coefficients[t]);
						continue;
					}
					const double c = outer.coefficients[t];
					auto inner = MembersOf(link.members, group_of_instance[instance_of(link.value_var, outer.rows[t])]);
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
}

void DeriveReducerValueBounds(SolverInput &input) {
	for (auto &link : input.reducer_value_links) {
		// One column box for the helper, whichever group an instance stands for: the
		// cover of every group's reach. An end no member bounds stays open; an end
		// already tighter stands.
		AuxRange box;
		for (idx_t group = 0; group < link.members.GroupCount(); group++) {
			auto span = MembersOf(link.members, group);
			if (span.first < span.last) {
				box.Cover(GroupReach(link, input, group));
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

//! `terms` with each column once, its coefficients summed: an AVG's members may read one
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

//! Member `member`'s terms, each at the column standing for its decision at its row.
vector<MemberTerm> MemberTermsOf(const SolverInput &input, const VarIndexer &indexer,
                                 const ReducerValueMembers &members, idx_t member) {
	vector<MemberTerm> terms;
	for (idx_t t = members.member_begin[member]; t < members.member_begin[member + 1]; t++) {
		const double c = members.coefficients[t];
		if (std::abs(c) >= 1e-15) {
			const idx_t var = members.variables[t];
			terms.push_back(MakeMemberTerm(input, var, indexer.Get(var, members.rows[t]), c));
		}
	}
	return terms;
}

//! Group `g` of `link` as the extremum link reads it: one member per listed member, its
//! expression negated with its constant apart, with its own reach; the group's family their
//! cover, read at the group's row. Built one group at a time, into `group`'s storage, so a
//! link of a million windows never holds them all.
void ExtremumGroupAt(const ReducerValueLinkSpec &link, const SolverInput &input, const VarIndexer &indexer,
                     idx_t g, MinMaxGroup &group) {
	auto &members = link.members;
	group.members.clear();
	group.family = AuxRange();
	group.first_row = members.group_rows[g];
	{
		auto span = MembersOf(members, g);
		for (idx_t m = span.first; m < span.last; m++) {
			ExtremumMember member;
			double lo, hi;
			MemberRange(members, m, input, lo, hi);
			const double constant = members.constants[m];
			// The member carries its constant too, so its box is the terms' bracket
			// shifted by it -- which is also what the Big-M over the family reads.
			member.range.CoverRowSided(lo + constant, hi + constant, DConstants::INVALID_INDEX);
			group.family.Cover(member.range);
			// `z - expr`: the expression enters negated, its constant part lands on the bound.
			for (idx_t t = members.member_begin[m]; t < members.member_begin[m + 1]; t++) {
				if (members.coefficients[t] != 0.0) {
					member.link.AddColumn(static_cast<int>(indexer.Get(members.variables[t], members.rows[t])),
					                      -members.coefficients[t]);
				}
			}
			member.link.constant = constant;
			group.members.push_back(std::move(member));
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
		const double value_lo = input.lower_bounds[link.value_var] <= -1e20 ? -INF : input.lower_bounds[link.value_var];
		const double value_hi = input.upper_bounds[link.value_var] >= 1e20 ? INF : input.upper_bounds[link.value_var];
		if (std::isinf(value_lo) && std::isinf(value_hi)) {
			continue;
		}
		const bool extremum = IsExtremum(link);
		if (extremum && !link.need_envelope) {
			continue; // a closing side alone pins no member
		}
		auto &members = link.members;
		// Only a helper's column takes a bound here, so a member reading none -- every
		// member of a reducer over decisions alone -- is passed over unread.
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
			vector<MemberTerm> group_terms;
			double group_constant = 0.0;
			for (idx_t m = span.first; m < span.last; m++) {
				if (extremum && !reads_helper(m, m + 1)) {
					continue;
				}
				auto member = MemberTermsOf(input, indexer, members, m);
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
			// A SUM or AVG helper equals its group's members, every instance in the box.
			if (!extremum && !group_terms.empty()) {
				BoundTermsOfASum(CombineColumns(input, group_terms), group_constant, value_lo, value_hi, is_helper,
				                 column_lo, column_hi);
			}
		}
	}
	// A helper's box is one for every instance it defines: tightened only when each of
	// them was bounded, by the loosest. Its other columns -- the rows its path drops, the
	// windows that read nothing -- are read by no row. An inlined helper has no column.
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

namespace {

//! `value - sum of members' expressions = sum of their constants`, one row per group of a
//! SUM or AVG helper. The members' columns are accumulated first: a keyed decision may
//! stand in several members of the group, and a row states each column once.
//! `sum` is the scratch row every group reuses: dense over the decisions' columns while
//! they fit (SolverModel::Build's choice), so columns keep first-appearance order.
void EmitSumDefinitions(SolverInput &input, const VarIndexer &indexer, const ReducerValueLinkSpec &link,
                        SparseCoeffAccumulator &sum) {
	auto &members = link.members;
	constexpr idx_t DENSE_CAP = 1u << 20;
	const bool dense = indexer.global_block_start <= DENSE_CAP;
	for (idx_t g = 0; g < members.GroupCount(); g++) {
		auto span = MembersOf(members, g);
		if (span.first == span.last) {
			continue;
		}
		if (dense) {
			sum.BeginDense(indexer.global_block_start);
		} else {
			sum.BeginSparse(members.member_begin[span.last] - members.member_begin[span.first]);
		}
		double constants = 0.0;
		for (idx_t m = span.first; m < span.last; m++) {
			constants += members.constants[m];
			for (idx_t t = members.member_begin[m]; t < members.member_begin[m + 1]; t++) {
				if (members.coefficients[t] != 0.0) {
					sum.Add(static_cast<int>(indexer.Get(members.variables[t], members.rows[t])),
					        -members.coefficients[t]);
				}
			}
		}
		SolverInput::RawConstraint rc;
		rc.indices.push_back(static_cast<int>(indexer.Get(link.value_var, members.group_rows[g])));
		rc.coefficients.push_back(1.0);
		sum.Flush(rc.indices, rc.coefficients);
		// A sum cancelled to rounding is no term (MinMaxLinkRow::AppendTo's rule).
		idx_t kept = 1;
		for (idx_t i = 1; i < rc.indices.size(); i++) {
			if (std::abs(rc.coefficients[i]) >= 1e-15) {
				rc.indices[kept] = rc.indices[i];
				rc.coefficients[kept] = rc.coefficients[i];
				kept++;
			}
		}
		rc.indices.resize(kept);
		rc.coefficients.resize(kept);
		rc.sense = '=';
		rc.rhs = constants;
		rc.kind = ConstraintKind::STRUCTURAL;
		rc.source_clause_id = link.source_clause_id;
		rc.removal_group_id = link.removal_group_id;
		input.global_constraints.push_back(std::move(rc));
	}
}

} // namespace

void LinearizeReducerValueLinks(SolverInput &input, const VarIndexer &indexer, const vector<string> &var_names,
                                NativeConstructPolicy policy) {
	SparseCoeffAccumulator sum;
	input.inlined_helper_of.assign(input.num_decide_vars, DConstants::INVALID_INDEX);
	for (auto &link : input.reducer_value_links) {
		if (link.inlined) {
			// Its readers read its members (SolverModel::Build), each instance its group's.
			SolverInput::InlinedHelper helper;
			helper.group_of_instance.assign(indexer.var_entity_mapping_idx[link.value_var] == DConstants::INVALID_INDEX
			                                    ? 1
			                                    : indexer.entity_mappings[indexer.var_entity_mapping_idx[link.value_var]]
			                                          .num_entities,
			                                DConstants::INVALID_INDEX);
			for (idx_t g = 0; g < link.members.GroupCount(); g++) {
				helper.group_of_instance[indexer.InstanceOf(link.value_var, link.members.group_rows[g])] = g;
			}
			helper.members = std::move(link.members);
			helper.label = link.label;
			input.inlined_helper_of[link.value_var] = input.inlined_helpers.size();
			input.inlined_helpers.push_back(std::move(helper));
			continue;
		}
		if (!IsExtremum(link)) {
			EmitSumDefinitions(input, indexer, link, sum);
			continue;
		}
		const bool is_max = link.agg_name == "max";
		const idx_t value_var = link.value_var;
		// A member is its expression negated with its constant apart, the shape the
		// extremum link takes.
		MinMaxGroup group;
		for (idx_t g = 0; g < link.members.GroupCount(); g++) {
			ExtremumGroupAt(link, input, indexer, g, group);
			if (group.members.empty()) {
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
			spec.closing_underivable = is_max ? (value_hi_open || group.family.lo_unbounded)
			                                  : (value_lo_open || group.family.hi_unbounded);
			spec.closing_big_m = spec.closing_underivable ? 0.0
			                     : is_max ? input.upper_bounds[value_var] - group.family.lo
			                              : group.family.hi - input.lower_bounds[value_var];
			if (spec.closing_underivable) {
				spec.blame_var =
				    group.family.Unbounded() ? FirstUnboundedMemberVariable(link.members, input) : value_var;
			}
			spec.label = link.label;
			spec.envelope_kind = ConstraintKind::STRUCTURAL;
			spec.closing_kind = ConstraintKind::STRUCTURAL;
			spec.source_clause_id = link.source_clause_id;
			spec.removal_group_id = link.removal_group_id;
			EmitExtremumLink(input, indexer, spec, group.members, var_names, policy);
		}
	}
	input.reducer_value_links.clear();
}

} // namespace duckdb
