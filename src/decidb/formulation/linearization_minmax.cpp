//===----------------------------------------------------------------------===//
//                         DeciDB
//
// src/decidb/formulation/linearization_minmax.cpp
//
// MIN/MAX lowering: hard constraints, extremum links, objectives, and the composed (additive) form. See
// ilp_linearization.cpp.
//
//===----------------------------------------------------------------------===//
#include "duckdb/decidb/formulation/ilp_linearization.hpp"
#include "duckdb/common/decide_numeric_policy.hpp"
#include "duckdb/common/exception.hpp"

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
                          vector<EvaluatedConstraint> &new_constraints, const DecideClauseRef &written) {
	// Classify each group's bound before asking for a Big-M over it. The RHS
	// reaching here already holds one value per group: the optimizer's bound-side
	// window rewrite (RewriteRhsAggregates) reduced a row-varying bound to the
	// tightest one (MAX for `>=`, MIN for `<=`), which is also what settles a group
	// that mixed finite and infinite bounds — the infinity either dominates the
	// reduction or is dominated by a finite value. So a group's verdict is any of its rows' verdicts, and only
	// a linearizable group reaches DecideTightPerRowBigM.
	{
		bool has_groups = !ec.row_group_ids.empty();
		idx_t group_count = has_groups ? MaxValue<idx_t>(ec.num_groups, 1) : 1;
		vector<MinMaxBoundKind> group_kind(group_count, MinMaxBoundKind::LINEARIZE);
		vector<bool> group_active(group_count, false);
		for (idx_t r = 0; r < num_rows; r++) {
			idx_t g = has_groups ? ec.row_group_ids[r] : 0;
			if (g == DConstants::INVALID_INDEX) {
				continue;
			}
			group_kind[g] = ClassifyMinMaxBound(DecideRowEffectiveBound(ec, r, written), is_max_agg);
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
					if (g != DConstants::INVALID_INDEX && group_kind[g] != MinMaxBoundKind::NO_SOLUTION) {
						ec_no_solution.row_group_ids[r] = DConstants::INVALID_INDEX;
					}
				}
				new_constraints.push_back(std::move(ec_no_solution));
			}
			// Drop every group the rewrite has no work for: a vacuous one
			// constrains nothing, and an unsatisfiable one was just emitted.
			for (idx_t r = 0; r < num_rows; r++) {
				idx_t g = ec.row_group_ids[r];
				if (g != DConstants::INVALID_INDEX && group_kind[g] != MinMaxBoundKind::LINEARIZE) {
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

} // namespace

// `MinMaxGroup`, this builder and `MinMaxRowMembers` are declared in ilp_linearization_internal.hpp: the
// value rewrite's MIN/MAX helpers (linearization_reducer_value.cpp) reduce over the same
// families.

//! The reach of one row's expression, constant part included: the variable-only bracket
//! shifted by it, which is also what the Big-M over the family reads.
static AuxRange MinMaxRowRange(const SolverInput &input, const EvaluatedConstraint &ec, idx_t row,
                               const DecideClauseRef &written) {
	auto rr = DecideRowSignedRange(ec, row, input.lower_bounds, input.upper_bounds);
	AuxRange range;
	rr.CoverInto(range, DecideRowFixedLhsOffset(ec.variable_indices, ec.row_coefficients, row, written));
	return range;
}

vector<MinMaxGroup> decide_linearize::BuildMinMaxGroups(const SolverInput &input, const EvaluatedConstraint &ec,
                                                        const DecideClauseRef &written) {
	const idx_t num_rows = input.num_rows;
	bool has_groups = !ec.row_group_ids.empty();
	idx_t group_count = has_groups ? MaxValue<idx_t>(ec.num_groups, 1) : 1;
	vector<MinMaxGroup> groups(group_count);
	for (idx_t r = 0; r < num_rows; r++) {
		idx_t g = has_groups ? ec.row_group_ids[r] : 0;
		if (g == DConstants::INVALID_INDEX) {
			continue;
		}
		auto &group = groups[g];
		if (group.first_row == DConstants::INVALID_INDEX) {
			group.first_row = r;
		}
		group.family.Cover(MinMaxRowRange(input, ec, r, written));
		group.rows.push_back(r);
	}
	return groups;
}

decide_linearize::MinMaxRowMembers::MinMaxRowMembers(const SolverInput &input, const VarIndexer &indexer,
                                                     const EvaluatedConstraint &ec, const DecideClauseRef &written)
    : input(input), indexer(indexer), ec(ec), written(written) {
	row_terms.BeginForDecisions(indexer, ec.variable_indices.size(), written);
}

void decide_linearize::MinMaxRowMembers::FillLink(idx_t row, MinMaxLinkRow &link) {
	// `z - expr`: the expression enters negated, its constant part lands on the bound.
	for (idx_t t = 0; t < ec.variable_indices.size(); t++) {
		idx_t v = ec.variable_indices[t];
		if (v == DConstants::INVALID_INDEX) {
			continue;
		}
		double coef = ec.row_coefficients[t].Get(row);
		if (coef == 0.0) {
			continue;
		}
		row_terms.Add(static_cast<int>(indexer.Get(v, row)), -coef);
	}
	row_terms.Flush(link.indices, link.coefficients);
	link.constant = DecideRowFixedLhsOffset(ec.variable_indices, ec.row_coefficients, row, written);
}

AuxRange decide_linearize::MinMaxRowMembers::Range(idx_t row) const {
	return MinMaxRowRange(input, ec, row, written);
}

ExtremumMemberSource decide_linearize::MinMaxRowMembers::Source(const MinMaxGroup &group) {
	ExtremumMemberSource source;
	source.count = group.rows.size();
	source.fill_link = [this, &group](idx_t i, MinMaxLinkRow &link) {
		FillLink(group.rows[i], link);
	};
	source.range = [this, &group](idx_t i) {
		return Range(group.rows[i]);
	};
	return source;
}

void LinearizeMinMaxConstraints(SolverInput &input, const VarIndexer &indexer, const vector<string> &var_names,
                                NativeConstructPolicy policy) {
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
		const auto written = DecideWrittenClause(input, ec.source_clause_id);
		// Settle every group's bound before anything is encoded. A bound out of reach or
		// beyond reach is answered by the direction it points, not by an encoding, and
		// whatever this emits (the unsatisfiable group's plain row) is an ordinary
		// constraint that rejoins the model.
		if (!ClassifyMinMaxGroups(ec, num_rows, is_max_agg, new_constraints, written)) {
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
		auto groups = BuildMinMaxGroups(input, ec, written);
		MinMaxRowMembers row_members(input, indexer, ec, written);
		for (idx_t g = 0; g < groups.size(); g++) {
			auto &group = groups[g];
			if (group.rows.empty()) {
				continue; // masked-out group: nothing to take an extremum over
			}
			// The extremum of a family lies inside the family's own bracket. Labelled
			// with the clause text stage 05 recorded, so a diagnosis over this column
			// reads `MAX(x * c)` rather than an internal name.
			// The tag carries the index minted beside the label (RewriteMinMax), so it is always in range.
			const string &label = input.minmax_clause_labels[ec.minmax_clause_idx];
			// The raw bound, not the effective one: each member carries its own row's
			// constant LHS part, because a per-row constant can differ inside a group and
			// the extremum is taken over the whole expression, constant included.
			double clause_bound = ec.rhs_values.Get(group.first_row);
			// A bound the family cannot reach is still part of the reach this encoding
			// has to cover. `MAX(x) >= 5` over `x` in [-1, 1] boxes the extremum column at
			// 1 and sizes the closing Big-M off a span of 2, so the repair "widen x's box"
			// is not representable and the infeasible diagnosis can only offer to weaken
			// the MAX — advice worth 2 where advice worth 10 was available at the same
			// edit cost. Widening here is the same rule `DemandedAuxReach` applies to ABS,
			// and it fires under the same condition: only when the bound lies outside the
			// family, which is only when the clause cannot be met as written. Nothing is
			// loosened for a query that solves.
			// The bound is finite here: an infinite one was classified, and a NaN one refused, before this.
			bool bound_out_of_reach = false;
			if (is_max_agg) {
				if (!group.family.hi_unbounded && clause_bound > group.family.hi) {
					group.family.hi = clause_bound;
					bound_out_of_reach = true;
				}
			} else if (!group.family.lo_unbounded && clause_bound < group.family.lo) {
				group.family.lo = clause_bound;
				bound_out_of_reach = true;
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
			auto members = row_members.Source(group);
			auto open_demanded_end = [is_max_agg](AuxRange &range) {
				if (is_max_agg) {
					range.MarkHiUnbounded(DConstants::INVALID_INDEX);
				} else {
					range.MarkLoUnbounded(DConstants::INVALID_INDEX);
				}
			};
			if (bound_out_of_reach && policy.Use(group.family.Unbounded())) {
				open_demanded_end(z_range);
				// A member's range boxes the native argument column and is read nowhere
				// else — the lowering reads the member's link and the family span.
				members.range = [&, open_demanded_end](idx_t i) {
					AuxRange range = row_members.Range(group.rows[i]);
					open_demanded_end(range);
					return range;
				};
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
			spec.blame_var = FindUnboundedContributor(ec, input.lower_bounds, input.upper_bounds);
			spec.written = written;
			spec.closing_kind = ConstraintKind::USER_MECHANISM;
			EmitExtremumLink(input, indexer, spec, members, var_names, policy);

			// The user's clause, over the extremum itself: `z <op> K`.
			SolverInput::RawConstraint outer;
			outer.indices.push_back(static_cast<int>(z_idx));
			outer.coefficients.push_back(1.0);
			outer.sense = is_max_agg ? '>' : '<';
			outer.rhs = clause_bound;
			// A strict relation over whole-numbered members -- the bind-time gate's
			// guarantee, a constant inside the extremum included -- is the next value on
			// the lattice, as `ApplyComparisonSense` states it for every other row.
			bool greater = ec.comparison_type == ExpressionType::COMPARE_GREATERTHAN;
			if (greater || ec.comparison_type == ExpressionType::COMPARE_LESSTHAN) {
				outer.rhs = greater ? std::floor(clause_bound) + 1.0 : std::ceil(clause_bound) - 1.0;
				outer.strict = true;
				outer.typed_k = clause_bound;
			}
			outer.kind = ConstraintKind::USER_PARAMETER;
			// The elastic shape, without which this row does not fold. One grouped clause
			// emits one of these per group, and they are all the same line of SQL: the
			// user edits a single literal and every group moves with it. Left UNSET they
			// never fold, so an infeasible `per g: max(e) by (g) >= K` reported one edit per group
			// — and only the loosest of them repaired anything. A genuinely per-group
			// bound stays PER_ROW_DATA and reports a virtual offset, as it does elsewhere.
			outer.shape = ec.rhs_is_shared_scalar ? ElasticShape::SHARED_SCALAR : ElasticShape::PER_ROW_DATA;
			outer.rhs_label = ec.rhs_is_shared_scalar ? string() : ec.rhs_label;
			outer.source_clause_id = ec.source_clause_id;
			outer.repair_group_id = ec.repair_group_id;
			outer.removal_group_id = ec.removal_group_id;
			outer.StampGroup(ec, ec.row_group_ids.empty() ? DConstants::INVALID_INDEX : g);
			outer.is_aggregate = true;
			if (g < ec.group_class_set.size()) {
				outer.class_set = ec.group_class_set[g];
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
//! `link` holds member `i`'s `(result - expr)`, so a renaming reads as the single coefficient `-1`. The member's reach
//! is asked of `members` only once it is known to need a column.
static int ExtremumArgumentColumn(SolverInput &input, const VarIndexer &indexer, const MinMaxLinkRow &link,
                                  const ExtremumMemberSource &members, idx_t i, const string &label,
                                  idx_t source_clause_id, idx_t removal_group_id) {
	int sole_column = -1;
	double sole_coefficient = 0.0;
	idx_t surviving = 0;
	for (idx_t k = 0; k < link.indices.size(); k++) {
		if (std::abs(link.coefficients[k]) < 1e-15) {
			continue;
		}
		surviving++;
		sole_column = link.indices[k];
		sole_coefficient = link.coefficients[k];
	}
	if (surviving == 1 && sole_coefficient == -1.0 && link.constant == 0.0) {
		return sole_column;
	}
	idx_t t_idx = AddGlobalContinuousAux(input, indexer, members.range(i), 0.0, label);
	auto pin = link.LeftHandSide((int)t_idx);
	pin.sense = '=';
	pin.rhs = link.constant;
	pin.kind = ConstraintKind::STRUCTURAL;
	pin.source_clause_id = source_clause_id;
	pin.removal_group_id = removal_group_id;
	input.global_constraints.push_back(std::move(pin));
	return (int)t_idx;
}

void EmitExtremumLink(SolverInput &input, const VarIndexer &indexer, const ExtremumLinkSpec &spec,
                      const ExtremumMemberSource &members, const vector<string> &var_names,
                      NativeConstructPolicy policy) {
	if (members.count == 0) {
		return;
	}
	int result_col = (int)spec.result_column;
	// One scratch row serves every member: each is built into it, copied into the row the model receives, and
	// overwritten by the next.
	MinMaxLinkRow link;
	auto fill_member = [&](idx_t i) -> const MinMaxLinkRow & {
		link.indices.clear();
		link.coefficients.clear();
		link.constant = 0.0;
		members.fill_link(i, link);
		return link;
	};

	// A family of one member is that member (deck p. 112): one equality pins `result` from both sides, with no
	// indicator and no Big-M -- so a side no box bounds needs none. It takes the kind of the side the site needs,
	// where that side is mechanism a diagnosis never loosens; a composed clause's closing rows are its user's clause,
	// loosened one way, which an equality would not be.
	const auto one_kind = spec.need_closing ? spec.closing_kind : spec.envelope_kind;
	const bool kinds_agree = !(spec.need_envelope && spec.need_closing && spec.envelope_kind != spec.closing_kind);
	if (members.count == 1 && kinds_agree && !IsRelaxableForElastic(one_kind)) {
		auto &member = fill_member(0);
		auto rc = member.LeftHandSide(result_col);
		rc.sense = '=';
		rc.rhs = member.constant;
		rc.kind = one_kind;
		rc.source_clause_id = spec.source_clause_id;
		rc.removal_group_id = spec.removal_group_id;
		input.global_constraints.push_back(std::move(rc));
		return;
	}

	// The envelope, on both arms. It is one row per member with no constant to dominate,
	// and on the native arm it is implied by the general constraint rather than
	// contradicted by it — so whether it is emitted is a question about the SITE (does
	// the surrounding model already supply this side?), never about the backend.
	if (spec.need_envelope) {
		char sense = spec.is_max ? '>' : '<';
		for (idx_t i = 0; i < members.count; i++) {
			auto &member = fill_member(i);
			auto rc = member.LeftHandSide(result_col);
			rc.sense = sense;
			rc.rhs = member.constant;
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
		args.reserve(members.count);
		for (idx_t i = 0; i < members.count; i++) {
			args.push_back(ExtremumArgumentColumn(input, indexer, fill_member(i), members, i, spec.label,
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
	// A span past a DOUBLE (`MAX(x + c)` over `c` of either sign near the limit) is no Big-M either, and no column is
	// open: the refusal names the clause. The closing rows carry no clause of their own to name at the hand-off.
	if (spec.closing_underivable || !std::isfinite(spec.closing_big_m)) {
		ThrowUnboundedBigMNaming(spec.blame_var != DConstants::INVALID_INDEX, spec.blame_var, var_names, "MIN or MAX",
		                         spec.written);
	}

	// One indicator per member, a Big-M row that binds `result` to that member when the
	// indicator is on, and `SUM(y) >= 1` so at least one binds.
	double M = spec.closing_big_m;
	double m_coeff = spec.is_max ? M : -M;
	char sense = spec.is_max ? '<' : '>';
	SolverInput::RawConstraint sum_y;
	sum_y.indices.reserve(members.count);
	sum_y.coefficients.reserve(members.count);
	for (idx_t i = 0; i < members.count; i++) {
		auto &member = fill_member(i);
		idx_t y_idx = AddGlobalBinaryAux(input, indexer, 0.0, spec.label);
		sum_y.indices.push_back((int)y_idx);
		sum_y.coefficients.push_back(1.0);

		auto rc = member.LeftHandSide(result_col, 1);
		rc.indices.push_back((int)y_idx);
		rc.coefficients.push_back(m_coeff);
		rc.sense = sense;
		rc.rhs = m_coeff + member.constant;
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

// --- Composed (additive) MIN/MAX --------------------------------------------
// A composed clause mixes reducers additively (`SUM(a) + MAX(b) <= K`). Each
// MIN/MAX term becomes a global auxiliary z_k pinned per row; SUM/AVG terms fold
// straight into the outer row (constraint) or into the objective coefficients.
// The two entry points below share that auxiliary layer and differ only in what
// they do with the terms afterwards.

//! Accumulate one row of a composed term's inner expression into `link`, negated:
//! the pinning row is `z - expr op bound`, so the expression's coefficients enter
//! with the opposite sign and its constant part lands on the bound.
static void AddComposedRowTerms(const VarIndexer &indexer, SparseCoeffAccumulator &row_terms, MinMaxLinkRow &link,
                                const vector<idx_t> &variables, const vector<vector<double>> &per_term_coefs, idx_t row,
                                const DecideClauseRef &written) {
	for (idx_t it = 0; it < variables.size(); it++) {
		double coeff = per_term_coefs[it][row];
		idx_t v = variables[it];
		if (v == DConstants::INVALID_INDEX) {
			link.AddConstant(coeff, written);
		} else {
			row_terms.Add((int)indexer.Get(v, row), -coeff);
		}
	}
	row_terms.Flush(link.indices, link.coefficients);
}

//! The family a composed MIN/MAX term reduces over: one entry per row the term's
//! filter admits. Feeds both the term's auxiliary box and the hard direction's Big-M,
//! so the two can never disagree about the term's reach.
//!
//! A term admitting no rows yields the seeded [0,0], which pins its auxiliary at 0 —
//! better than letting an auxiliary with no pinning rows float on the outer pressure.
static AuxRange ComposedTermRange(const SolverInput &input, const vector<idx_t> &variables,
                                  const vector<vector<double>> &per_term_coefs, const vector<bool> &filter_mask) {
	AuxRange range;
	for (idx_t row = 0; row < input.num_rows; row++) {
		if (!filter_mask[row]) {
			continue;
		}
		auto rr = SignedRowRange::Of(
		    variables.size(), [&](idx_t it) { return variables[it]; },
		    [&](idx_t it) { return per_term_coefs[it][row]; }, [&](idx_t v) { return input.lower_bounds[v]; },
		    [&](idx_t v) { return input.upper_bounds[v]; });
		rr.CoverInto(range);
	}
	return range;
}

//! Number of rows a composed term reduces over: its filtered row count. For an AVG term this is the divisor, and zero
//! means the term is empty and contributes nothing (the caller skips it).
static idx_t ComposedTermRows(const ComposedMinMaxTermData &ta, idx_t num_rows) {
	idx_t cnt = 0;
	for (idx_t r = 0; r < num_rows; r++) {
		if (ta.filter_mask[r])
			cnt++;
	}
	return cnt;
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
                                          vector<ComposedMinMaxTermData> &terms, idx_t source_clause_id,
                                          idx_t removal_group_id, const vector<string> &var_names,
                                          NativeConstructPolicy native_min_max) {
	idx_t num_rows = input.num_rows;

	// A term reads its variable through the indexer at each row, so a frame's payload term
	// -- a read of its decision at the row the frame selects -- lands on that row's column,
	// and its box is its base decision's (SolverInput::frame_reads).
	// Every `z_k` first, so one clause's auxiliaries occupy a contiguous run of the
	// global block whatever formulation each term then turns out to need. The range is
	// computed once here and reused as the closing Big-M and as the box of any column a
	// native arm pins — one walk, one answer, rather than each arm deriving its own.
	const auto written = DecideWrittenClause(input, source_clause_id);
	vector<AuxRange> term_range(terms.size());
	for (idx_t i = 0; i < terms.size(); i++) {
		auto &ta = terms[i];
		if (!ta.is_minmax) {
			continue;
		}
		term_range[i] = ComposedTermRange(input, ta.variables, ta.per_term_coefs, ta.filter_mask);
		// The label names the z through the global label channel, so a diagnosis
		// renders `MAX(x)` rather than an internal column name.
		ta.z_idx = AddGlobalContinuousAux(input, indexer, term_range[i], 0.0, ta.label);
	}

	for (idx_t i = 0; i < terms.size(); i++) {
		auto &ta = terms[i];
		if (!ta.is_minmax) {
			continue;
		}
		vector<idx_t> rows;
		rows.reserve(ComposedTermRows(ta, num_rows));
		for (idx_t row = 0; row < num_rows; row++) {
			if (ta.filter_mask[row]) {
				rows.push_back(row);
			}
		}
		SparseCoeffAccumulator row_terms;
		row_terms.BeginForDecisions(indexer, ta.variables.size(), written);
		ExtremumMemberSource members;
		members.count = rows.size();
		members.fill_link = [&](idx_t k, MinMaxLinkRow &link) {
			AddComposedRowTerms(indexer, row_terms, link, ta.variables, ta.per_term_coefs, rows[k], written);
		};
		// The TERM's range, not the row's: looser than a per-row bracket, but it is
		// the one the auxiliary itself is boxed by and the one the Big-M comes from,
		// so a pinned column shares it rather than introducing a second answer.
		members.range = [&](idx_t) {
			return term_range[i];
		};

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
		spec.written = written;
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

void LinearizeComposedMinMaxConstraint(SolverInput &input, const VarIndexer &indexer,
                                       vector<ComposedMinMaxTermData> &terms, double rhs_val, ExpressionType outer_cmp,
                                       idx_t source_clause_id, idx_t removal_group_id, idx_t repair_group_id,
                                       idx_t class_set, const vector<string> &var_names,
                                       NativeConstructPolicy native_min_max) {
	idx_t num_rows = input.num_rows;
	EmitComposedMinMaxAuxiliaries(input, indexer, terms, source_clause_id, removal_group_id, var_names, native_min_max);

	// Build the outer composed RawConstraint. Its fixed terms move to the bound side as one sum, as the model builder
	// moves a clause's (SumFixedAggregateLhsOffset).
	const auto written = DecideWrittenClause(input, source_clause_id);
	// A term's `z_k` is its own column, so it enters the row as it comes; the decision columns of the SUM/AVG terms
	// sum per column.
	SolverInput::RawConstraint outer;
	SparseCoeffAccumulator outer_terms;
	outer_terms.BeginForDecisions(indexer, 0, written);
	double outer_fixed = 0.0;
	for (auto &ta : terms) {
		if (ta.is_minmax) {
			double z_coefficient = (double)ta.sign * ta.scale;
			if (z_coefficient != 0.0) {
				outer.indices.push_back((int)ta.z_idx);
				outer.coefficients.push_back(z_coefficient);
			}
			continue;
		}
		// SUM/AVG term. For AVG, divide by filtered row count.
		double avg_divisor = 1.0;
		if (ta.agg_name == "avg") {
			idx_t cnt = ComposedTermRows(ta, num_rows);
			if (cnt == 0) {
				// Empty aggregate — contributes 0; skip.
				continue;
			}
			avg_divisor = static_cast<double>(cnt);
		}
		for (idx_t it = 0; it < ta.variables.size(); it++) {
			const idx_t variable = ta.variables[it];
			for (idx_t row = 0; row < num_rows; row++) {
				if (!ta.filter_mask[row])
					continue;
				double coef = ta.per_term_coefs[it][row] * (double)ta.sign * ta.scale / avg_divisor;
				if (variable == DConstants::INVALID_INDEX) {
					AddCoefficient(outer_fixed, coef, written);
				} else {
					outer_terms.Add((int)indexer.Get(variable, row), coef);
				}
			}
		}
	}
	outer_terms.Flush(outer.indices, outer.coefficients);
	double outer_rhs = rhs_val - outer_fixed;

	// A strict relation over whole-numbered terms (the bind-time gate's guarantee) is the
	// next value on the lattice, as `ApplyComparisonSense` states it for every other row.
	switch (outer_cmp) {
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
	case ExpressionType::COMPARE_LESSTHAN:
		outer.sense = '<';
		outer.rhs = outer_rhs;
		if (outer_cmp == ExpressionType::COMPARE_LESSTHAN) {
			outer.rhs = std::ceil(outer_rhs) - 1.0;
			outer.strict = true;
			outer.typed_k = outer_rhs;
		}
		break;
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
	case ExpressionType::COMPARE_GREATERTHAN:
		outer.sense = '>';
		outer.rhs = outer_rhs;
		if (outer_cmp == ExpressionType::COMPARE_GREATERTHAN) {
			outer.rhs = std::floor(outer_rhs) + 1.0;
			outer.strict = true;
			outer.typed_k = outer_rhs;
		}
		break;
	case ExpressionType::COMPARE_EQUAL:
		// One native equality row, not a pair of opposing inequalities: the backends take
		// '=' directly, and presolve can eliminate against an equality in a way it cannot
		// against two inequalities that happen to meet. Every `z_k` in it carries both the
		// envelope and the closing side (see `exact_pin` in the DECIDE rewrite), so the
		// row is exact in both directions.
		outer.sense = '=';
		outer.rhs = outer_rhs;
		break;
	default:
		throw InternalException("Composed MIN/MAX: unexpected comparison type.");
	}
	outer.kind = ConstraintKind::USER_PARAMETER;
	outer.shape = ElasticShape::SHARED_SCALAR;
	outer.source_clause_id = source_clause_id;
	outer.removal_group_id = removal_group_id;
	outer.class_set = class_set;
	outer.repair_group_id = repair_group_id;
	input.global_constraints.push_back(std::move(outer));
}

} // namespace duckdb
