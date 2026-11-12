#include "duckdb/optimizer/decide/direct/direct_rules.hpp"

#include <algorithm>

#include "duckdb/common/decide_numeric_policy.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/optimizer/decide/direct/direct_bound_reader.hpp"
#include "duckdb/optimizer/decide/direct/direct_plan_fragments.hpp"
#include "duckdb/optimizer/decide/direct/direct_ranked_prefix.hpp"
#include "duckdb/optimizer/decide/direct/direct_source_reader.hpp"
#include "duckdb/optimizer/decide/direct/direct_row_checks.hpp"
#include "duckdb/optimizer/decide/direct/direct_runtime_checks.hpp"
#include "duckdb/optimizer/decide/direct/direct_expression.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/decide/decide_term_split.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"
#include "duckdb/planner/operator/logical_window.hpp"

namespace duckdb {

//! Each rule keeps its helpers in its own named namespace: unity builds compile every rule file in one translation
//! unit, where two anonymous namespaces would merge and same-named helpers would collide.
namespace direct_s1_uniform_matroid {

constexpr const char *RULE_NAME = "S1_UNIFORM_MATROID";

struct Condition final : DirectApplicabilityCondition {
	//! The constraints of the IR, which outlives the condition: the count bounds, and the fixed Boolean values.
	vector<const DirectIRConstraint *> bound_factors;
	vector<const DirectIRConstraint *> local_fixes;

	idx_t lower = 0;
	idx_t upper = 0;
	bool has_upper = false;
	bool impossible = false;
	//! The score: what S1 ranks the rows by.
	DirectLinearScore score;
	//! The rows and groups every bound counts: the `by` key (the groups, each its own interval; one group of all the
	//! rows the bounds cover when empty; a row with a NULL in a NO_GROUP element is in no group, D5), the `per` key,
	//! and the `when` membership.
	DirectSharedScope scope;
	DirectFixedValues fixed;
	//! The count limits read from the data, one per bound whose limit is not a constant (the constant ones fold into
	//! `lower` and `upper`): a value on every row the clause keeps already, or the aggregates of the group
	//! (DecideGroupBound), which the plan computes in a hash aggregate over the group, joined to every row.
	vector<DirectCountLimit> source_bounds;
};

class Rule final : public DirectSolveRule {
public:
	const char *Name() const override {
		return RULE_NAME;
	}
	unique_ptr<DirectApplicabilityCondition> CheckCondition(const DirectIR &ir, ClientContext &context) const override {
		if (!DirectIsOneRowBool(ir)) {
			return nullptr;
		}
		auto condition = make_uniq<Condition>();
		if (!DirectSplitFixes(ir, condition->local_fixes, condition->bound_factors) ||
		    condition->bound_factors.empty()) {
			return nullptr;
		}
		if (!ProveScope(ir, *condition) ||
		    !DirectReadFixedValues(ir, context, condition->local_fixes, condition->fixed) ||
		    !ProveBounds(ir, context, *condition) || !condition->score.Prove(ir, context)) {
			return nullptr;
		}
		return std::move(condition);
	}
	string Summary() const override {
		return "min/max SUM(s*x), x in {0,1}, lo <= SUM(x) <= hi";
	}
	DirectSubstitute Apply(DirectSourceReader &reader, Optimizer &optimizer,
	                       const DirectApplicabilityCondition &candidate,
	                       const DirectStateErrors &errors) const override;

private:
	bool ProveScope(const DirectIR &ir, Condition &condition) const;
	bool ProveBounds(const DirectIR &ir, ClientContext &context, Condition &condition) const;
};

//! Every bound counts the same rows: one `SUM(x)` per bound with the same group keys and the same `when` membership.
//! Records the group keys (the `by` key of the `SUM(x)`s, and the `per` key of the clauses) and the membership the plan
//! scopes by. A row whose `by` key is NULL is in no group (D5): the plan ranks it apart from every group, and a `when`
//! that drops a row does the same.
bool Rule::ProveScope(const DirectIR &ir, Condition &condition) const {
	return DirectProveSharedScope(ir, condition.bound_factors, condition.scope);
}

//! The count bounds: constant ones fold into one inclusive interval, source-valued ones are kept for the plan to
//! read per group. Every bound counts `x` once, as `x` or `-x` (DirectReadCountBody), and bounds the count from one
//! side or both (a `<>` is a decline).
bool Rule::ProveBounds(const DirectIR &ir, ClientContext &context, Condition &condition) const {
	for (auto factor : condition.bound_factors) {
		bool negated;
		DirectCountLimit count;
		if (!DirectReadCountBody(context, factor->PlainSum()->terms, negated) ||
		    !DirectReadCountLimit(ir, context, *factor, negated, count) ||
		    (!DirectIsLowerBound(count.comparison) && !DirectIsUpperBound(count.comparison))) {
			return false;
		}
		// A source-valued bound has one value per group already. The plan evaluates it on the rows of the groups, as
		// the solver does, so an expression that can raise (an overflow in `COUNT(*) * k` over a window column) raises
		// on both paths or on neither.
		if (!count.limit.constant) {
			condition.source_bounds.push_back(std::move(count));
			continue;
		}
		auto fold_side = [&](bool upper, idx_t &rows) {
			bool impossible = false;
			if (!DirectInclusiveRows(count.limit.number, count.comparison, upper, rows, impossible)) {
				return false;
			}
			condition.impossible |= impossible;
			return true;
		};
		if (DirectIsLowerBound(count.comparison)) {
			idx_t lower;
			if (!fold_side(false, lower)) {
				return false;
			}
			condition.lower = std::max(condition.lower, lower);
		}
		if (DirectIsUpperBound(count.comparison)) {
			idx_t upper;
			if (!fold_side(true, upper)) {
				return false;
			}
			condition.upper = condition.has_upper ? std::min(condition.upper, upper) : upper;
			condition.has_upper = true;
		}
	}
	if (condition.has_upper && condition.lower > condition.upper) {
		condition.impossible = true;
	}
	return true;
}

//! Builds the relational plan for one S1 problem whose condition holds, on the shared ranked-prefix plan
//! (DirectRankedPrefix): S1 ranks each group's rows by score, best first, and selects the prefix its count bounds
//! allow. The scope projection, the rank window, the group bounds' aggregates and the answer are the builder's; S1
//! gives the score, the counts, the limits and what they decide.
//!
//!   S    the scope projection: the eligible flag, the group key and the fixed-value flags
//!   P1   the score; each source-valued bound's value; the check of the score; the checks of the fixed values (the
//!        builder's)
//!   W1   per group, in order of the score: the rank, the count of the rows fixed to 1 and of the rows free to take
//!        either value, and the aggregates of the group bounds (what a bound reads is one value per group, computed
//!        here over the one read)
//!   P2   the count limits and what remains of them after the fixed rows, and whether the group can meet them
//!   H    the rows held in a materialized CTE, and whether every group can be met, raised in the hold; only where a
//!        group can fail (NeedsVerdict)
//!   A    the 0/1 assignment, the source columns and the column of the checks
//!
//! The solver reads every row before it answers, so a bad value or an infeasible bound in any group raises even under
//! an outer LIMIT 1. A window holds every row before it releases one, and releases them group by group, so a check that
//! stands above it reaches a later group only when the parent reads that far. The checks of the row therefore stand in
//! P1, below the rank window. The checks of a group (what its bounds give, and whether its rows and fixed values can
//! meet them) read the rank window's counts and the group's bounds, so they stand above it: what a bound gives raises
//! in P2, and the hold raises whether each group is feasible over all the rows, so every row has passed them when the
//! first leaves. The hold stands only where a group can fail.
class PlanBuilder final : public DirectRankedPrefix {
public:
	PlanBuilder(const Condition &condition_p, Optimizer &optimizer_p, const vector<ColumnBinding> &source_bindings_p,
	            const vector<LogicalType> &source_types_p, const DirectStateErrors &errors_p)
	    : DirectRankedPrefix(optimizer_p, source_bindings_p, source_types_p, errors_p, condition_p.scope,
	                         condition_p.fixed),
	      condition(condition_p) {
		for (auto &bound : condition.source_bounds) {
			source_lower |= DirectIsLowerBound(bound.comparison);
			source_upper |= DirectIsUpperBound(bound.comparison);
			group_bound.push_back(bound.limit.data.group ? AddGroupBound(*bound.limit.data.group)
			                                             : DConstants::INVALID_INDEX);
		}
		has_lower = condition.lower || source_lower;
		has_upper = condition.has_upper || source_upper;
		has_score_message = condition.score.CanFail(optimizer);
	}

private:
	const Condition &condition;
	bool source_lower = false;
	bool source_upper = false;
	bool has_lower = false;
	bool has_upper = false;
	//! Whether a score can fail: be NaN or infinite, or the sum of the parts' scores pass a DOUBLE. A score made of
	//! values of exact types, whose bounds sum below the range of a DOUBLE (DirectScaledMagnitude), cannot, and the
	//! plan has no check for it.
	bool has_score_message = false;
	//! Each source-valued count bound's index among the builder's group bounds; unset for a bound read per row.
	vector<idx_t> group_bound;
	//! P1: the row's score, and each source-valued count bound's value (unset for a bound of the group's aggregates).
	ColumnBinding score_column;
	vector<ColumnBinding> bound_columns;
	//! W1: the count of the group's rows fixed to 1 (set only with fixed rows) and of its rows free to take either
	//! value, or all of them without fixed rows (set only where a lower bound can fail).
	ColumnBinding fixed_count;
	ColumnBinding row_count;
	//! P2: the score; how many rows of the group the lower and the upper limit leave to the free rows, set for the
	//! sides the plan has; and whether the group's rows and fixed values can meet the limits, set where a group can
	//! fail.
	ColumnBinding score_after;
	ColumnBinding remaining_lower;
	ColumnBinding remaining_upper;
	ColumnBinding group_ok;

	//! Whether a group can fail a check of its own. Those checks read the rank window's counts and the group's bounds,
	//! so they stand above the rank window, and the hold must raise them before the first row leaves: whether
	//! the group's rows and fixed values can meet its bounds, and what a group bound gives (a NaN limit, an `=` bound
	//! that takes two values in a group), which only a source-valued bound has, and which always makes the limits'
	//! check below necessary.
	bool NeedsVerdict() const {
		return condition.impossible || has_lower || ((source_lower || source_upper) && has_upper) ||
		       (HasFixes() && has_upper);
	}

	//! The best score is the least: the score is minimized.
	OrderType RankOrder() const override {
		return OrderType::ASCENDING;
	}

	//! P1: the score, the value of each source-valued bound, and the check of the score. What a group bound reads and
	//! the fixed values are the builder's.
	ColumnBinding LayoutRows(DirectProjectionStage &p1, const DirectScopeState &state) override {
		vector<ExprPtr> part_scores;
		auto over_state = [&](const Expression &value) {
			return OverState(state, value);
		};
		auto combined_score = condition.score.Combined(calc, optimizer, over_state, part_scores);
		score_column = p1.Add(calc.Out(combined_score));
		for (auto &bound : condition.source_bounds) {
			if (bound.limit.data.group) {
				bound_columns.emplace_back();
				continue;
			}
			// A bound is read on the eligible rows only. On a row its clause's `when` drops, the windows it reads hold
			// NULL or a count of 0, so computing it there raises nothing.
			auto value = over_state(*bound.limit.data.value);
			if (bound.negated) {
				value = DirectScaledTerm(optimizer, std::move(value), -1);
			}
			bound_columns.push_back(p1.Add(calc.Out(calc.Cast(calc.Wrap(std::move(value)), LogicalType::DOUBLE))));
		}
		if (has_score_message) {
			checks_below.push_back(DirectRaiseMessage(
			    optimizer, calc, condition.score.Message(calc, optimizer, over_state, combined_score, part_scores)));
		}
		return score_column;
	}

	//! A row with no fixed value, neither to one nor to zero.
	unique_ptr<Expression> FreeRow(const DirectRankedColumns &rows) const {
		return calc.Out(
		    calc.If(Column(LogicalType::BOOLEAN, rows.fixed_one), calc.Bool(false),
		            calc.If(Column(LogicalType::BOOLEAN, rows.fixed_zero), calc.Bool(false), calc.Bool(true))));
	}

	//! W1: the counts of the group's rows, which the lower bound's feasibility reads.
	void AddGroupWindows(LogicalWindow &w1, const DirectRankedColumns &rows) override {
		auto add = [&](unique_ptr<BoundWindowExpression> count) {
			OverRank(rows.carried, *count);
			w1.expressions.push_back(std::move(count));
			return ColumnBinding(w1.window_index, w1.expressions.size() - 1);
		};
		if (HasFixes()) {
			fixed_count = add(DirectWindowMatchingCount(calc, calc.Out(Column(LogicalType::BOOLEAN, rows.fixed_one))));
		}
		if (!has_lower || condition.impossible) {
			return;
		}
		row_count = add(DirectWindowMatchingCount(calc, HasFixes() ? FreeRow(rows) : calc.Out(calc.Bool(true))));
	}

	//! The value of source-valued bound `index` on a side, as DOUBLE: the group's value when the plan computes its
	//! aggregates, else the bound's own column. A negated bound (`SUM(-x) <= B` is `SUM(x) >= -B`) holds B, which is
	//! the limit the bound gives on the other side as written, negated.
	ExprPtr BoundValue(idx_t index, bool upper) const {
		auto &bound = condition.source_bounds[index];
		if (!bound.limit.data.group) {
			return calc.Col(LogicalType::DOUBLE, bound_columns[index]);
		}
		auto &values = GroupValues(group_bound[index]);
		auto &limit = upper != bound.negated ? values.upper : values.lower;
		D_ASSERT(limit);
		if (bound.negated) {
			return calc.Wrap(DirectScaledTerm(optimizer, calc.Out(limit), -1));
		}
		return calc.Cast(limit, LogicalType::DOUBLE);
	}

	//! Inclusive count limit of one source-valued bound, as BIGINT: its value rounded, clamped to the BIGINT range,
	//! and -1 (upper) or 0 (lower) when the value is below zero. A NULL value skips its instance: no limit.
	ExprPtr SourceLimit(idx_t index, bool upper) const {
		auto &bound = condition.source_bounds[index];
		// Every caller asks for a side the bound has: an upper limit of an upper bound, a lower of a lower bound.
		D_ASSERT(upper ? DirectIsUpperBound(bound.comparison) : DirectIsLowerBound(bound.comparison));
		return DirectInclusiveRowsExpr(calc, bound.comparison, upper, [&]() { return BoundValue(index, upper); });
	}

	//! Largest lower limit across the constant and source-valued bounds.
	ExprPtr LowerLimit() const {
		vector<ExprPtr> limits;
		limits.push_back(calc.Constant(Value::BIGINT(NumericCast<int64_t>(condition.lower))));
		for (idx_t i = 0; i < condition.source_bounds.size(); i++) {
			if (DirectIsLowerBound(condition.source_bounds[i].comparison)) {
				limits.push_back(SourceLimit(i, false));
			}
		}
		return calc.Max(limits);
	}

	//! Smallest upper limit across the constant and source-valued bounds. Only asked for when there is an upper bound.
	ExprPtr UpperLimit() const {
		vector<ExprPtr> limits;
		if (condition.has_upper) {
			limits.push_back(calc.Constant(Value::BIGINT(NumericCast<int64_t>(condition.upper))));
		}
		for (idx_t i = 0; i < condition.source_bounds.size(); i++) {
			if (DirectIsUpperBound(condition.source_bounds[i].comparison)) {
				limits.push_back(SourceLimit(i, true));
			}
		}
		D_ASSERT(!limits.empty());
		return calc.Min(limits);
	}

	//! Whether the group of the row is feasible: a list of checks, each true when it passes and false when it fails.
	//! The hold reads it over every row (the builder takes a row outside every group as feasible), so every
	//! group has passed the raising checks (P2: what the group's bounds give) when the first row leaves; a false raises
	//! DECIDE's infeasible error then.
	ExprPtr GroupsAreFeasible(const DirectRankedColumns &input) const {
		// `ok` must hold; a NULL `ok` fails.
		auto must_hold = [&](const ExprPtr &ok) {
			return calc.Out(calc.If(ok, calc.Bool(true), calc.Bool(false)));
		};
		// `bad` must not hold; a NULL `bad` passes.
		auto must_not_hold = [&](const ExprPtr &bad) {
			return calc.Out(calc.If(bad, calc.Bool(false), calc.Bool(true)));
		};
		auto fixed_rows = [&]() {
			return Column(LogicalType::BIGINT, fixed_count);
		};
		vector<unique_ptr<Expression>> group_checks;
		if (condition.impossible) {
			// No bound can hold, so a group with any row is infeasible.
			group_checks.push_back(
			    must_not_hold(calc.Cmp(ExpressionType::COMPARE_GREATERTHANOREQUALTO,
			                           Column(LogicalType::BIGINT, input.rank), calc.Constant(Value::BIGINT(1)))));
		} else {
			if (has_lower) {
				auto free_rows = Column(LogicalType::BIGINT, row_count);
				group_checks.push_back(
				    must_hold(calc.Cmp(ExpressionType::COMPARE_GREATERTHANOREQUALTO,
				                       HasFixes() ? calc.Add(fixed_rows(), free_rows) : free_rows, LowerLimit())));
			}
			if ((source_lower || source_upper) && has_upper) {
				group_checks.push_back(
				    must_hold(calc.Cmp(ExpressionType::COMPARE_LESSTHANOREQUALTO, LowerLimit(), UpperLimit())));
			}
			if (source_lower) {
				for (idx_t i = 0; i < condition.source_bounds.size(); i++) {
					if (!DirectIsLowerBound(condition.source_bounds[i].comparison)) {
						continue;
					}
					group_checks.push_back(
					    must_not_hold(calc.Cmp(ExpressionType::COMPARE_GREATERTHANOREQUALTO, BoundValue(i, false),
					                           calc.Num(DECIDE_FIRST_DOUBLE_BEYOND_BIGINT))));
				}
			}
			if (HasFixes() && has_upper) {
				group_checks.push_back(
				    must_hold(calc.Cmp(ExpressionType::COMPARE_LESSTHANOREQUALTO, fixed_rows(), UpperLimit())));
			}
		}
		D_ASSERT(group_checks.empty() != NeedsVerdict());
		return calc.Wrap(DirectAllHold(calc, std::move(group_checks)));
	}

	//! P2: the score, how many rows of the group the limits leave to the free rows, and whether the group is feasible.
	void LayoutPrefix(DirectProjectionStage &p2, const DirectRankedColumns &input) override {
		score_after = p2.Add(Column(LogicalType::DOUBLE, score_column));
		auto remaining = [&](const ExprPtr &limit) {
			return calc.Out(HasFixes() ? calc.Sub(limit, Column(LogicalType::BIGINT, fixed_count)) : limit);
		};
		if (has_lower) {
			remaining_lower = p2.Add(remaining(LowerLimit()));
		}
		if (has_upper) {
			remaining_upper = p2.Add(remaining(UpperLimit()));
		}
		if (NeedsVerdict()) {
			group_ok = p2.Add(calc.Out(GroupsAreFeasible(input)));
		}
	}

	ExprPtr GroupVerdict(const DirectRankedColumns &rows) override {
		return NeedsVerdict() ? Column(LogicalType::BOOLEAN, group_ok) : nullptr;
	}

	//! The 0/1 assignment of a row no clause fixes: scope, then upper and lower rank limits, then score sign.
	ExprPtr Chosen(const DirectRankedColumns &rows) override {
		auto improving =
		    calc.Cmp(ExpressionType::COMPARE_LESSTHAN, Column(LogicalType::DOUBLE, score_after), calc.Num(0.0));
		auto choose = improving;
		auto rank = Column(LogicalType::BIGINT, rows.rank);
		if (has_lower) {
			auto within_lower =
			    calc.Cmp(ExpressionType::COMPARE_LESSTHANOREQUALTO, rank, Column(LogicalType::BIGINT, remaining_lower));
			choose = calc.If(within_lower, calc.Bool(true), choose);
		}
		if (has_upper) {
			auto within_upper =
			    calc.Cmp(ExpressionType::COMPARE_LESSTHANOREQUALTO, rank, Column(LogicalType::BIGINT, remaining_upper));
			choose = calc.If(within_upper, choose, calc.Bool(false));
		}
		if (scoped) {
			choose = calc.If(Column(LogicalType::BOOLEAN, rows.carried.eligible), choose, improving);
		}
		return choose;
	}
};

DirectSubstitute Rule::Apply(DirectSourceReader &reader, Optimizer &optimizer,
                             const DirectApplicabilityCondition &candidate, const DirectStateErrors &errors) const {
	auto &condition = static_cast<const Condition &>(candidate);
	PlanBuilder plan_builder(condition, optimizer, reader.SourceBindings(), reader.SourceTypes(), errors);
	return DirectBuildOverOneRead(
	    reader, [&](unique_ptr<LogicalOperator> rows) { return plan_builder.Build(std::move(rows)); });
}

} // namespace direct_s1_uniform_matroid

unique_ptr<DirectSolveRule> MakeS1UniformMatroidRule() {
	return make_uniq<direct_s1_uniform_matroid::Rule>();
}

} // namespace duckdb
