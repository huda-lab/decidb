#include "duckdb/optimizer/decide/direct/direct_rules.hpp"

#include <algorithm>
#include <cmath>
#include <map>

#include "duckdb/common/decide_numeric_policy.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/optimizer/decide/direct/direct_bound_reader.hpp"
#include "duckdb/optimizer/decide/direct/direct_expression.hpp"
#include "duckdb/optimizer/decide/direct/direct_plan_fragments.hpp"
#include "duckdb/optimizer/decide/direct/direct_runtime_checks.hpp"
#include "duckdb/optimizer/decide/direct/direct_source_reader.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/decide/decide_term_split.hpp"
#include "duckdb/planner/expression/bound_unnest_expression.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_cross_product.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_unnest.hpp"
#include "duckdb/planner/operator/logical_window.hpp"

namespace duckdb {

//! Each rule keeps its helpers in its own named namespace: unity builds compile every rule file in one translation
//! unit, where two anonymous namespaces would merge and same-named helpers would collide.
namespace direct_r1_continuous_knapsack {

constexpr const char *RULE_NAME = "R1_CONTINUOUS_KNAPSACK";

//! What CheckCondition proved: one or more row-scoped REAL decisions, each with its per-row bounds, one budget `lower
//! <= SUM(w0*x0 + w1*x1 + ...) <= upper` over the rows the budget covers, and a linear objective. Nothing links two
//! decisions of a row: every per-row bound reads one decision, and no objective term multiplies two.
struct Condition final : DirectApplicabilityCondition {
	//! A limit of the budget read from the data: a source column or an expression over source columns (an aggregate of
	//! the source is a column by the time the rule reads it). Each group's limit is the group's MIN for `<=`, its MAX
	//! for `>=` and both for `=`, which must agree (DirectValidateBounds).
	struct DataLimit {
		ExpressionType comparison = ExpressionType::INVALID;
		unique_ptr<Expression> value;
		//! The source column the limit is exactly, with its type and the name the user wrote; INVALID_INDEX and an
		//! empty name for a computed limit.
		idx_t source_slot = DConstants::INVALID_INDEX;
		LogicalType source_type;
		string column_name;
		//! The limit is reduced over every row of the group, not only the rows of the budget: the sum has a `WHEN` of
		//! its own, which scopes the sum and not the right side, as the solver reduces it.
		bool all_group_rows = false;
	};
	DecideSense sense = DecideSense::FEASIBILITY;
	//! Which rows the budget covers, and in which groups: a row whose `WHEN` holds and whose `PER` keys are all not
	//! NULL belongs to the budget of its group; any other row is outside every budget. Every clause of the budget has
	//! this scope. `group_key_slots` index the source's columns.
	vector<idx_t> group_key_slots;
	unique_ptr<Expression> when;
	bool scoped = false;
	//! Each decision's box before any per-row bound (one per decision), and the per-row clauses the plan evaluates;
	//! a clause names the decision it bounds.
	vector<DirectDecisionBox> boxes;
	vector<DirectBoundClause> clauses;
	//! The budget's weight terms; a decision's weight is `sum(sign * coefficient)` over its terms, and zero when it has
	//! none.
	DirectLinearPart budget;
	//! The budget's constant limits: `<=` sets the upper one, `>=` the lower one, `=` both. A side with no limit is
	//! infinite. Several constraints on the same weights are one budget, with the largest lower and smallest upper
	//! limit, counting the data limits below.
	double lower = NEGATIVE_INFINITY;
	double upper = POSITIVE_INFINITY;
	vector<DataLimit> data_limits;
	//! The objective's parts, each a signed, scaled sum of linear terms. None for no objective.
	vector<DirectLinearPart> objective;
};

//! The terms are linear in the declared decisions and none is missing its coefficient.
bool OnlyLinearTerms(const vector<DecideSplitTerm> &terms, idx_t decision_count) {
	if (terms.empty()) {
		return false;
	}
	for (auto &term : terms) {
		if (term.kind != DecideTermKind::LINEAR || term.var_a >= decision_count || !term.coefficient) {
			return false;
		}
	}
	return true;
}

class Rule final : public DirectSolveRule {
public:
	const char *Name() const override {
		return RULE_NAME;
	}

	unique_ptr<DirectApplicabilityCondition> CheckCondition(const DirectIR &ir, ClientContext &context) const override {
		vector<const DirectIRConstraint *> budgets;
		if (!ProveDecision(ir) || !ProveConstraints(ir, budgets)) {
			return nullptr;
		}
		auto condition = make_uniq<Condition>();
		condition->sense = ir.objective.sense;
		if (!ProveBudget(ir, context, budgets, *condition) || !ProveBounds(ir, context, *condition) ||
		    !ProveObjective(ir, context, *condition)) {
			return nullptr;
		}
		return std::move(condition);
	}

	string Summary() const override {
		return "min/max SUM(v*x), l <= x <= u, L <= SUM(w*x) <= U";
	}

	DirectSubstitute Apply(unique_ptr<LogicalOperator> source, Optimizer &optimizer,
	                       const DirectApplicabilityCondition &candidate) const override;

private:
	bool ProveDecision(const DirectIR &ir) const;
	bool ProveConstraints(const DirectIR &ir, vector<const DirectIRConstraint *> &budgets) const;
	bool ProveBudget(const DirectIR &ir, ClientContext &context, const vector<const DirectIRConstraint *> &budgets,
	                 Condition &condition) const;
	bool ProveBudgetScope(const DirectIR &ir, const vector<const DirectIRConstraint *> &budgets,
	                      Condition &condition) const;
	bool ProveBudgetClause(const DirectIR &ir, ClientContext &context, const DirectIRConstraint &ir_node,
	                       Condition &condition, bool first) const;
	bool ProveLimit(const DirectIR &ir, ClientContext &context, const DirectIRConstraint &ir_node,
	                Condition &condition) const;
	bool ProveDataLimit(const DirectIR &ir, const DirectIRConstraint &ir_node, Condition &condition) const;
	bool ProveBounds(const DirectIR &ir, ClientContext &context, Condition &condition) const;
	bool ProveObjective(const DirectIR &ir, ClientContext &context, Condition &condition) const;
};

//! Every decision is a row-scoped REAL decision: an item is one decision on one row.
bool Rule::ProveDecision(const DirectIR &ir) const {
	if (ir.decisions.empty() || !ir.entity_scopes.empty()) {
		return false;
	}
	for (auto &decision : ir.decisions) {
		if (decision.domain != DirectDomain::REAL || decision.scope != DecideVarScope::ROW ||
		    decision.output_type != LogicalType::DOUBLE) {
			return false;
		}
	}
	return true;
}

//! The aggregate constraints are the budget's clauses (it is what keeps R1 apart from A1); the caller proves they are
//! one budget. Every other constraint is a per-row bound on one decision: a constraint that reads two decisions links
//! them, and stays with the solver.
bool Rule::ProveConstraints(const DirectIR &ir, vector<const DirectIRConstraint *> &budgets) const {
	for (auto &ir_node : ir.constraints) {
		if (ir_node.aggregate) {
			budgets.push_back(&ir_node);
			continue;
		}
		// A membership and a not-equal constraint are neither a lower nor an upper bound.
		if (!DirectIsLowerBound(ir_node.comparison) && !DirectIsUpperBound(ir_node.comparison)) {
			return false;
		}
		if (!ir_node.scope.per_key_slots.empty() || !DirectReadsOneDecision(ir_node, ir.decisions.size())) {
			return false;
		}
	}
	return !budgets.empty();
}

//! The same linear terms, in the same order: a `BETWEEN`, or two comparisons written apart, are one budget.
bool SameBudgetTerms(const DirectLinearPart &a, const DirectLinearPart &b) {
	if (a.terms.size() != b.terms.size()) {
		return false;
	}
	for (idx_t i = 0; i < a.terms.size(); i++) {
		if (a.terms[i].decision != b.terms[i].decision || a.terms[i].sign != b.terms[i].sign ||
		    !a.terms[i].coefficient->Equals(*b.terms[i].coefficient)) {
			return false;
		}
	}
	return true;
}

//! Every clause of the budget covers the same rows: the same `PER` keys in the same order, and the same `WHEN` (written
//! on the clause or on the aggregate: both read as a membership of the budget). Records the scope the plan applies.
bool Rule::ProveBudgetScope(const DirectIR &ir, const vector<const DirectIRConstraint *> &budgets,
                            Condition &condition) const {
	bool ok;
	auto first_when = budgets[0]->Membership(ok);
	if (!ok) {
		return false;
	}
	for (idx_t i = 1; i < budgets.size(); i++) {
		auto when = budgets[i]->Membership(ok);
		if (!ok || budgets[i]->scope.per_key_slots != budgets[0]->scope.per_key_slots ||
		    static_cast<bool>(when) != static_cast<bool>(first_when) ||
		    (when && !Expression::Equals(*when, *first_when))) {
			return false;
		}
	}
	if (first_when) {
		if (!DirectIsSourceOnlyPredicate(*first_when, ir.decide_index, ir.source_bindings)) {
			return false;
		}
		condition.when = first_when->Copy();
	}
	condition.group_key_slots = budgets[0]->scope.per_key_slots;
	condition.scoped = !condition.group_key_slots.empty() || condition.when != nullptr;
	return true;
}

//! One clause of the budget: `SUM(w*x)`, a plain SUM of linear terms (its scope is proved by ProveBudgetScope), against
//! a limit with `<=`, `>=` or `=`. The first clause sets the weights; each later one must have the same.
bool Rule::ProveBudgetClause(const DirectIR &ir, ClientContext &context, const DirectIRConstraint &ir_node,
                             Condition &condition, bool first) const {
	// ProveBudgetScope has proved that every clause is a plain SUM.
	auto sum = ir_node.PlainSum();
	if (!OnlyLinearTerms(sum->terms, ir.decisions.size())) {
		return false;
	}
	DirectLinearPart weights;
	if (!DirectReadLinearPart(ir, context, *sum, weights)) {
		return false;
	}
	if (first) {
		condition.budget = std::move(weights);
	} else if (!SameBudgetTerms(condition.budget, weights)) {
		return false;
	}
	return ProveLimit(ir, context, ir_node, condition);
}

//! The limit of a clause: a constant the solver reads as a number, folded into the budget's interval, or a value of the
//! source row, kept for the plan to reduce per group. A constant of magnitude 1e20 or more is where the solver stops
//! reading a right side as a number, so those stay with the solver; a value of the data is read at run time instead
//! (PlanBuilder::Limit).
bool Rule::ProveLimit(const DirectIR &ir, ClientContext &context, const DirectIRConstraint &ir_node,
                      Condition &condition) const {
	auto comparison = ir_node.comparison;
	if (comparison != ExpressionType::COMPARE_LESSTHANOREQUALTO &&
	    comparison != ExpressionType::COMPARE_GREATERTHANOREQUALTO && comparison != ExpressionType::COMPARE_EQUAL) {
		return false;
	}
	double limit;
	if (!TryEvaluateFoldableDouble(context, *ir_node.rhs, limit, DIRECT_FINITE_CONSTANT)) {
		return ProveDataLimit(ir, ir_node, condition);
	}
	if (std::fabs(limit) >= DECIDE_EFFECTIVE_INFINITY) {
		return false;
	}
	if (DirectIsUpperBound(comparison)) {
		condition.upper = std::min(condition.upper, limit);
	}
	if (DirectIsLowerBound(comparison)) {
		condition.lower = std::max(condition.lower, limit);
	}
	return true;
}

//! A limit read from the data: a numeric, deterministic expression over source columns that mentions at least one, and
//! may raise (the plan evaluates it on every row, as the solver does, so both raise on the same inputs). The
//! right-hand-side rewrite has made an aggregate on the right a source column; one it left is not a value of the row.
bool Rule::ProveDataLimit(const DirectIR &ir, const DirectIRConstraint &ir_node, Condition &condition) const {
	auto &rhs = *ir_node.rhs;
	if (!DirectIsSourceOnlyNumericMayRaise(rhs, ir.decide_index, ir.source_bindings)) {
		return false;
	}
	Condition::DataLimit limit;
	limit.comparison = ir_node.comparison;
	limit.value = rhs.Copy();
	DirectSourceNumericColumn(rhs, ir.source_bindings, limit.source_slot, limit.source_type, limit.column_name);
	limit.all_group_rows = ir_node.PlainSum()->filter != nullptr;
	condition.data_limits.push_back(std::move(limit));
	return true;
}

bool Rule::ProveBudget(const DirectIR &ir, ClientContext &context, const vector<const DirectIRConstraint *> &budgets,
                       Condition &condition) const {
	if (!ProveBudgetScope(ir, budgets, condition)) {
		return false;
	}
	for (idx_t i = 0; i < budgets.size(); i++) {
		if (!ProveBudgetClause(ir, context, *budgets[i], condition, i == 0)) {
			return false;
		}
	}
	return true;
}

//! Each decision's box before any per-row bound, and the clauses the plan evaluates per row, from the shared reader.
bool Rule::ProveBounds(const DirectIR &ir, ClientContext &context, Condition &condition) const {
	DirectBoundReading reading;
	if (!DirectReadBounds(ir, context, reading)) {
		return false;
	}
	condition.clauses = std::move(reading.clauses);
	condition.boxes = std::move(reading.boxes);
	return true;
}

//! The objective: signed, scaled `SUM(v*x)` parts, or none. Each coefficient must be a value of the source row.
bool Rule::ProveObjective(const DirectIR &ir, ClientContext &context, Condition &condition) const {
	if (!ir.objective.scope.per_key_slots.empty() || ir.objective.scope.when) {
		return false;
	}
	for (auto &part : ir.objective.parts) {
		if (part.reducer != DirectReducer::SUM || part.filter || part.inner ||
		    part.qualifier != DConstants::INVALID_INDEX || !OnlyLinearTerms(part.terms, ir.decisions.size())) {
			return false;
		}
		DirectLinearPart read;
		if (!DirectReadLinearPart(ir, context, part, read)) {
			return false;
		}
		condition.objective.push_back(std::move(read));
	}
	return true;
}

//===--------------------------------------------------------------------===//
// Plan
//===--------------------------------------------------------------------===//

//! The algebra of definition.md § Relational construction over expressions alone, which the two builders below share:
//! PlanBuilder runs it in windows over items, AggregatePlanBuilder in aggregates over items and joins. Nothing here
//! knows which column an expression reads or which operator evaluates it.
class ItemAlgebra {
public:
	explicit ItemAlgebra(Optimizer &optimizer_p) : optimizer(optimizer_p), calc(optimizer_p.context) {
	}

protected:
	//! One item's values, each as the expression that reads it.
	struct ItemExprs {
		//! The break-even price, the finite rooms up and down (units of budget), and whether each room is infinite.
		ExprPtr t, up_fin, down_fin, up_inf, down_inf;
		//! The same infinite rooms as 1.0 / 0.0 counts, the item's anchor spend, whether its box is empty, and 1.0 when
		//! an item outside the ranking improves without end.
		ExprPtr upi, dni, sterm, empty, u1;
		//! Outside the ranking (w = 0 or no budget), the sign of w, the item's best end, its anchor, weight and box.
		ExprPtr wz, pos, best, a, w, l, u;
	};

	//! The group sums the verdicts and the target spend read, in this order.
	enum TotalSlot : idx_t {
		TOTAL_EMPTY,
		TOTAL_S,
		TOTAL_SUP,
		TOTAL_CUP,
		TOTAL_SDN,
		TOTAL_CDN,
		TOTAL_U1,
		TOTAL_LO,
		TOTAL_HI,
		TOTAL_ZUP,
		TOTAL_ZDN,
		TOTAL_CZI,
		TOTAL_CZD,
		TOTAL_COUNT
	};
	//! The sums at the price, in this order.
	enum PriceSlot : idx_t { SUM_ABOVE, SUM_BELOW, SUM_UP, SUM_DOWN, SUM_UP_OPEN, SUM_DOWN_OPEN, SUM_COUNT };

	struct Verdict {
		ExprPtr infeasible, unbounded, over, under;
		//! The highest price of an item with infinite room up (-infinity for none) and the lowest of one with infinite
		//! room down (+infinity for none).
		ExprPtr highest_open_up, lowest_open_down;
	};
	//! What the price tests read of the sort, which runs in order of price, highest first, for one item.
	struct Reach {
		//! BOOLEAN: some infinite room up lies at or before the item in the sort, some infinite room down lies after
		//! it, some infinite room down lies at or after it, and some infinite room up lies before it.
		ExprPtr up_open_so_far, down_open_after, down_open_from_here, up_open_before;
		//! The most the group can spend with the items up to and including this one at their high end and every other
		//! item at its low end, and the least it can spend with the items before this one at their high end and this
		//! item and the ones after it at their low end.
		ExprPtr spend_at_most, spend_at_least;
	};

	Optimizer &optimizer;
	ExpressionBuilder calc;

	ExprPtr Zero() const {
		return calc.Num(0.0);
	}
	ExprPtr PositiveInfinity() const {
		return calc.Num(POSITIVE_INFINITY);
	}
	ExprPtr NegativeInfinity() const {
		return calc.Num(NEGATIVE_INFINITY);
	}
	ExprPtr NullDouble() const {
		return calc.Constant(Value(LogicalType::DOUBLE));
	}
	ExprPtr And(const ExprPtr &a, const ExprPtr &b) const {
		return calc.If(a, b, calc.Bool(false));
	}
	ExprPtr Not(const ExprPtr &a) const {
		return calc.If(a, calc.Bool(false), calc.Bool(true));
	}
	ExprPtr Greater(const ExprPtr &a, const ExprPtr &b) const {
		return calc.Cmp(ExpressionType::COMPARE_GREATERTHAN, a, b);
	}
	ExprPtr Less(const ExprPtr &a, const ExprPtr &b) const {
		return calc.Cmp(ExpressionType::COMPARE_LESSTHAN, a, b);
	}
	ExprPtr Equal(const ExprPtr &a, const ExprPtr &b) const {
		return calc.Cmp(ExpressionType::COMPARE_EQUAL, a, b);
	}
	//! 1.0 where the condition holds, else 0.0.
	ExprPtr Flag(const ExprPtr &condition_p) const {
		return calc.If(condition_p, calc.Num(1.0), Zero());
	}
	//! `value` where the condition holds, else 0.0.
	ExprPtr WhenHolds(const ExprPtr &condition_p, const ExprPtr &value) const {
		return calc.If(condition_p, value, Zero());
	}

	//! The value each item adds to one group sum, in TotalSlot order. Every sum is over finite parts; the open rooms
	//! are counts. The sums are the spend of the anchors, the finite rooms up and down and the counts of infinite ones,
	//! the items outside the ranking that improve without end, the highest price of an item with infinite room up and
	//! the lowest of one with infinite room down, and the zero-price spend: the anchors plus the room up of every item
	//! that gains and minus the room down of every item that loses at price zero (with the infinite rooms among them
	//! counted).
	vector<ExprPtr> TotalInputs(const ItemExprs &item) const {
		auto &t = item.t;
		vector<ExprPtr> inputs(TOTAL_COUNT);
		inputs[TOTAL_EMPTY] = Flag(item.empty);
		inputs[TOTAL_S] = item.sterm;
		inputs[TOTAL_SUP] = item.up_fin;
		inputs[TOTAL_CUP] = item.upi;
		inputs[TOTAL_SDN] = item.down_fin;
		inputs[TOTAL_CDN] = item.dni;
		inputs[TOTAL_U1] = item.u1;
		inputs[TOTAL_LO] = calc.If(item.up_inf, t, NullDouble());
		inputs[TOTAL_HI] = calc.If(item.down_inf, t, NullDouble());
		inputs[TOTAL_ZUP] = WhenHolds(Greater(t, Zero()), item.up_fin);
		inputs[TOTAL_ZDN] = WhenHolds(Less(t, Zero()), item.down_fin);
		inputs[TOTAL_CZI] = Flag(And(Greater(t, Zero()), item.up_inf));
		inputs[TOTAL_CZD] = Flag(And(Less(t, Zero()), item.down_inf));
		return inputs;
	}
	//! The aggregate that reduces TotalInputs(...)[slot] over a group.
	static const char *TotalFunction(idx_t slot) {
		return slot == TOTAL_LO ? "max" : (slot == TOTAL_HI ? "min" : "sum");
	}
	//! What one row adds to a group sum when it holds several items: the sum of its items' parts, or for the extreme
	//! prices their maximum or minimum (NULL parts skipped).
	ExprPtr CombineOfRow(const char *function, const vector<ExprPtr> &parts) const {
		if (string(function) == "max") {
			return calc.Max(parts);
		}
		if (string(function) == "min") {
			return calc.Min(parts);
		}
		auto total = parts[0];
		for (idx_t i = 1; i < parts.size(); i++) {
			total = calc.Add(total, parts[i]);
		}
		return total;
	}
	//! TotalInputs for a row of several items: each slot is what the row's items add together.
	vector<ExprPtr> TotalInputsOfRow(const vector<ItemExprs> &items) const {
		vector<vector<ExprPtr>> per_item;
		for (auto &item : items) {
			per_item.push_back(TotalInputs(item));
		}
		vector<ExprPtr> inputs(TOTAL_COUNT);
		for (idx_t slot = 0; slot < TOTAL_COUNT; slot++) {
			vector<ExprPtr> parts;
			for (auto &item_inputs : per_item) {
				parts.push_back(item_inputs[slot]);
			}
			inputs[slot] = CombineOfRow(TotalFunction(slot), parts);
		}
		return inputs;
	}

	//! Infeasible when a box is empty, the limits cross, or the reachable spend [S - down, S + up] misses [L, U] (an
	//! infinite room puts that end of the range at infinity). Unbounded when an item outside the ranking improves
	//! without end, or when an open room up has a higher price than an open room down. A side with no limit absorbs
	//! spend at price zero: with no lower limit the open rooms up count as at least zero, and with no upper limit the
	//! open rooms down as at most zero. The zero-price spend is infinite when such a room is open. The budget binds at
	//! U when that spend is above it (over) and at L when it is below (under). `eligible` is true on a row that belongs
	//! to a budget; it is null without a scope. The limits and the reachable spend are tested for the rows of a group
	//! alone: the rows outside every budget have no spend to reach a limit with.
	Verdict VerdictOf(const vector<ExprPtr> &totals, const ExprPtr &lower, const ExprPtr &upper,
	                  const ExprPtr &eligible) const {
		auto within = [&](const ExprPtr &holds) {
			return eligible ? And(eligible, holds) : holds;
		};
		// Limits that cross are infeasible, and so is a lower limit of +infinity or an upper one of -infinity.
		auto limits_unmet =
		    calc.Any({Greater(lower, upper), Equal(lower, PositiveInfinity()), Equal(upper, NegativeInfinity())});
		Verdict verdict;
		verdict.infeasible = calc.Any(
		    {Greater(totals[TOTAL_EMPTY], Zero()),
		     within(calc.Any(
		         {limits_unmet,
		          And(Equal(totals[TOTAL_CDN], Zero()), Greater(calc.Sub(totals[TOTAL_S], totals[TOTAL_SDN]), upper)),
		          And(Equal(totals[TOTAL_CUP], Zero()), Less(calc.Add(totals[TOTAL_S], totals[TOTAL_SUP]), lower))}))});
		verdict.highest_open_up = calc.Coalesce({totals[TOTAL_LO], NegativeInfinity()});
		verdict.lowest_open_down = calc.Coalesce({totals[TOTAL_HI], PositiveInfinity()});
		auto lo_effective = calc.If(Equal(lower, NegativeInfinity()), calc.Max({verdict.highest_open_up, Zero()}),
		                            verdict.highest_open_up);
		auto hi_effective = calc.If(Equal(upper, PositiveInfinity()), calc.Min({verdict.lowest_open_down, Zero()}),
		                            verdict.lowest_open_down);
		verdict.unbounded = calc.Any({Greater(totals[TOTAL_U1], Zero()), Greater(lo_effective, hi_effective)});
		auto zero_price_spend = calc.Sub(calc.Add(totals[TOTAL_S], totals[TOTAL_ZUP]), totals[TOTAL_ZDN]);
		// An open room at a price on the wrong side of zero makes the zero-price spend infinite.
		verdict.over = calc.Any({Greater(totals[TOTAL_CZI], Zero()),
		                         And(Equal(totals[TOTAL_CZD], Zero()), Greater(zero_price_spend, upper))});
		verdict.under = calc.Any(
		    {Greater(totals[TOTAL_CZD], Zero()), And(Equal(totals[TOTAL_CZI], Zero()), Less(zero_price_spend, lower))});
		return verdict;
	}

	//! The price an item would fix for a binding budget (NULL when it does not fix it).
	//!
	//! Over, the items before one in the sort, taken to their high end, with the others at their low end, spend at most
	//! `spend_at_most`, and the price is that of the first item whose spend reaches U. Under, the items before one at
	//! their high end and the item and those after it at their low end spend at least `spend_at_least`, and the price
	//! is that of the last item whose spend does not pass L. Both tests are monotone along the one sort, because
	//! rounded sums do not land exactly on a limit. An infinite room settles a test by itself: an open room up at or
	//! before the item reaches any U, and an open room down after the item keeps the spend from reaching it (the
	//! reverse for L).
	ExprPtr CandidatePrice(const Verdict &verdict, const Reach &reach, const ExprPtr &price, const ExprPtr &lower,
	                       const ExprPtr &upper) const {
		auto reach_high =
		    calc.If(reach.up_open_so_far, calc.Bool(true),
		            calc.If(reach.down_open_after, calc.Bool(false),
		                    calc.Cmp(ExpressionType::COMPARE_GREATERTHANOREQUALTO, reach.spend_at_most, upper)));
		auto reach_low =
		    calc.If(reach.down_open_from_here, calc.Bool(true),
		            calc.If(reach.up_open_before, calc.Bool(false),
		                    calc.Cmp(ExpressionType::COMPARE_LESSTHANOREQUALTO, reach.spend_at_least, lower)));
		return calc.If(verdict.over, calc.If(reach_high, price, NullDouble()),
		               calc.If(verdict.under, calc.If(reach_low, price, NullDouble()), NullDouble()));
	}

	//! The inclusive running sum of a value in order of the price, highest first, ties in any order, within a
	//! partition. A window hand-built from the whole-partition aggregate: the same node with a frame that ends at the
	//! current row, and the sort that gives the frame its meaning.
	unique_ptr<BoundWindowExpression> RunningSum(const ExprPtr &value, const ExprPtr &price,
	                                             const vector<ExprPtr> &partitions) const {
		auto sum = DirectWindowExtremum(optimizer, "sum", calc.Out(value));
		sum->end = WindowBoundary::CURRENT_ROW_ROWS;
		for (auto &partition : partitions) {
			sum->partitions.push_back(calc.Out(partition));
		}
		sum->orders.emplace_back(OrderType::DESCENDING, OrderByNullType::NULLS_LAST, calc.Out(price));
		return sum;
	}

	//! The price of a group: over U, the highest t of the items whose high end reaches it; none reaches it only when U
	//! is the top of the range up to rounding, and then every item is at its high end: a price below every t. Under L,
	//! the lowest t of the items whose low end does not pass it; none does only when L is the bottom of the range up to
	//! rounding, and then every item is at its low end: a price above every t. Zero when the budget does not bind.
	ExprPtr PriceOf(const ExprPtr &over, const ExprPtr &under, const ExprPtr &highest, const ExprPtr &lowest) const {
		return calc.If(over, calc.Coalesce({highest, NegativeInfinity()}),
		               calc.If(under, calc.Coalesce({lowest, PositiveInfinity()}), Zero()));
	}

	//! The value each item adds to the sums at a price, in PriceSlot order: the finite room up of the items above the
	//! price and the finite room down of the items below it, the finite rooms of the items at the price, and the number
	//! of their infinite rooms.
	vector<ExprPtr> PriceSumInputs(const ItemExprs &item, const ExprPtr &price) const {
		auto at_price = Equal(item.t, price);
		vector<ExprPtr> inputs(SUM_COUNT);
		inputs[SUM_ABOVE] = WhenHolds(Greater(item.t, price), item.up_fin);
		inputs[SUM_BELOW] = WhenHolds(Less(item.t, price), item.down_fin);
		inputs[SUM_UP] = WhenHolds(at_price, item.up_fin);
		inputs[SUM_DOWN] = WhenHolds(at_price, item.down_fin);
		inputs[SUM_UP_OPEN] = Flag(And(at_price, item.up_inf));
		inputs[SUM_DOWN_OPEN] = Flag(And(at_price, item.down_inf));
		return inputs;
	}

	//! PriceSumInputs for a row of several items: each slot is what the row's items add together.
	vector<ExprPtr> PriceSumInputsOfRow(const vector<ItemExprs> &items, const ExprPtr &price) const {
		vector<vector<ExprPtr>> per_item;
		for (auto &item : items) {
			per_item.push_back(PriceSumInputs(item, price));
		}
		vector<ExprPtr> inputs(SUM_COUNT);
		for (idx_t slot = 0; slot < SUM_COUNT; slot++) {
			vector<ExprPtr> parts;
			for (auto &item_inputs : per_item) {
				parts.push_back(item_inputs[slot]);
			}
			inputs[slot] = CombineOfRow("sum", parts);
		}
		return inputs;
	}

	//! An item's answer. Above the price an item is at its high end, below it at its low end, and an item with w = 0 at
	//! its best end. An item at the price takes `shared`. The answer is held to the box.
	ExprPtr AnswerOf(const ItemExprs &item, const ExprPtr &price, const ExprPtr &shared) const {
		auto high = calc.If(item.pos, item.u, item.l);
		auto low = calc.If(item.pos, item.l, item.u);
		auto answer = calc.If(item.wz, item.best,
		                      calc.If(Greater(item.t, price), high, calc.If(Less(item.t, price), low, shared)));
		return calc.Add(calc.Min({calc.Max({answer, item.l}), item.u}), Zero());
	}
};

//! One decision's item values on one read of the source, as bindings of the plan PlanBuilder::BuildItems returns. A
//! binding is DOUBLE unless the name says otherwise.
struct DecisionColumns {
	//! The value v.
	ColumnBinding v;
	//! The weight, the box, the anchor, the break-even price and the best end.
	ColumnBinding w, l, u, a, t, best;
	//! BOOLEAN: the box is empty, the item is outside the ranking (w = 0 or no budget), w is positive, and the room up
	//! and the room down are infinite.
	ColumnBinding empty, wz, pos, up_inf, down_inf;
	//! The finite rooms up and down, the anchor spend, 1.0 when an item outside the ranking improves without end, and
	//! the open rooms up and down as 1.0 / 0.0 counts.
	ColumnBinding up_fin, down_fin, sterm, u1, upi, dni;
};

//! Where PlanBuilder::BuildItems leaves the values of one read of the source: the first stage of the aggregate plan,
//! "item values". A row has one item per decision, each in columns of its own.
struct ItemColumns {
	//! The source columns, in source order.
	vector<ColumnBinding> sources;
	//! The group key: whether the row belongs to a budget (BOOLEAN), then the `PER` keys. Empty without a scope.
	vector<ColumnBinding> keys;
	//! The check of every raw value (BOOLEAN: true, or the row raises).
	ColumnBinding valid_data;
	//! The items of the row, one per decision.
	vector<DecisionColumns> decisions;
};

//! Builds the relational plan of one R1 problem whose condition holds (definition.md § Relational construction), as
//! windows over every row: the plan of limits read from the data, until the aggregate plan (AggregatePlanBuilder)
//! takes them. It also computes the item values that plan reads (BuildItems). A
//! *group* is the rows of one budget: the whole input, or with a scope the rows whose `WHEN` holds and whose `PER` keys
//! are not NULL, one group per key. Every other row is outside every budget. An *item* is one decision on one row. Its
//! box [l, u] has ends of magnitude 1e20 or more read as infinite, as the shared bound reader reads them; w is its
//! weight in the budget (zero for a decision the budget does not name), v its value (negated under MINIMIZE, zero with
//! no objective).
//!
//! With one decision an item is a row and the stages below run on the source rows. With several, the source rows
//! carry each decision's w, v, l and u in columns of their own (P1 to P3), then the plan unpivots (the plan DuckDB's
//! own UNPIVOT makes): it numbers the rows with `ROW_NUMBER() OVER ()` (no order, no partition, so no sort), unnests
//! lists of the decision index and of each decision's w, v, l and u into one item per decision, runs the stages P4 to
//! P7 on the items unchanged, and groups the items back by row number with a hash aggregate: a source column by
//! ANY_VALUE, each decision's answer by ANY_VALUE of the answer of the items of that decision index. Every source
//! column rides through the unnest, replicated once per decision.
//!
//! An item with w != 0 has a break-even price t = v / w, a high end (u when w > 0, l when w < 0), a low end, an anchor
//! a (0 clamped into the box), and room up and room down in units of budget (w * (high - a) and w * (a - low)). A
//! room of infinity is carried as a count beside the finite sum of the others, so no window adds infinity to minus
//! infinity. Items with w = 0, and the items of rows outside every budget, take their best end and do not enter the
//! ranking: they sort last, and their weight enters no sum.
//!
//! Stages (each a projection or a window over every column so far; a window of a scoped budget is partitioned by
//! whether the row belongs to a budget and by its `PER` keys, so each group has its own sums, verdicts and price):
//!
//!   P0 scope             whether the row belongs to a budget; the empty-aggregate guard (scoped budgets only)
//!   P0 data limits       the group's extremum of each limit read from the data, and the check on every value (only
//!                        a budget with such a limit)
//!   P1 raw values        every coefficient, bound and mask, evaluated on every row
//!   P2 bounds, v, limits each clause's bounds, each decision's value, the budget's limits on every row, the check on
//!                        every raw value
//!   P3 box               each decision's l and u
//!   (several decisions)  row number, unnest into items
//!   P4 anchor, price     whether the box is empty, a, whether w = 0, t
//!   P5 rooms             room up and down, finite and infinite, the spend of the anchors, unbounded items
//!   W0 (no sort)         group totals: S, rooms, counts, the extreme prices of the open items, zero-price spend
//!   W1 (the one sort)    running sums of the rooms, in order of t, highest first
//!   P6 verdicts, reach   infeasible, unbounded, which limit the budget binds at, whether an item reaches it
//!   W2 (no sort)         the price: over the upper limit, the highest t among the items whose high end reaches it;
//!                        under the lower limit, the lowest t among the items whose low end does not pass it
//!   W3 (no sort)         sums of the rooms above, below and at the price
//!   P7 answer            the high end, the low end, or the share of the items tied at the price
//!   breaker, output      every item is checked before one is released (several decisions: then grouped back)
class PlanBuilder : public ItemAlgebra {
public:
	PlanBuilder(const Condition &condition_p, Optimizer &optimizer_p, LogicalOperator &source)
	    : PlanBuilder(condition_p, optimizer_p, source.GetColumnBindings(), source.types) {
	}

	//! `proved_bindings` are the bindings the proved expressions of the condition refer to, and `source_types` the
	//! types of the source columns in the same order. A plan that reads the source more than once reads it through a
	//! fresh set of bindings each time, and the expressions are re-pointed at it (BuildItems).
	PlanBuilder(const Condition &condition_p, Optimizer &optimizer_p, vector<ColumnBinding> proved_bindings_p,
	            vector<LogicalType> source_types_p)
	    : ItemAlgebra(optimizer_p), condition(condition_p), binder(optimizer_p.binder),
	      proved_bindings(std::move(proved_bindings_p)), source_types(std::move(source_types_p)) {
		// The rule driver hands the input over with its types resolved.
		D_ASSERT(proved_bindings.size() == source_types.size());
	}

	DirectSubstitute Build(unique_ptr<LogicalOperator> source) {
		// The scope projection comes first: it carries the source and whether each row belongs to a budget. A WHEN
		// that covers no row of a nonempty source is the empty-aggregate error.
		auto state = DirectProjectScope(binder, std::move(source), condition.scoped, condition.when.get(),
		                                condition.group_key_slots, {});
		state_index = state.state_index;
		plan = std::move(state.plan);
		if (condition.scoped) {
			plan = DirectGuardEmptyAggregate(optimizer, std::move(plan), state.eligible);
		}
		AddItemStages(state);
		AddGroupTotals();
		AddRunningSums();
		AddVerdicts();
		AddPrice();
		AddPriceSums();
		AddAnswer();
		return Output(Release());
	}

	//! P0 to P5 over one read of a source that the aggregate plan reads several times (without the empty-aggregate
	//! guard, which that plan makes once, over its groups): the plan that computes every item's values, one row of the
	//! read per source row. `read` is a projection that forwards the source columns (DirectSourceReader::Read) and
	//! `read_table_index` its table index; the proved expressions are re-pointed at it. One decision and no limit read
	//! from the data. With several decisions the row is not unpivoted: each decision's item values are columns of their
	//! own (`t_0`, `t_1`, ...).
	unique_ptr<LogicalOperator> BuildItems(unique_ptr<LogicalOperator> read, idx_t read_table_index) {
		D_ASSERT(!HasDataLimits());
		items_by_decision = true;
		read_index = read_table_index;
		auto when = condition.when ? OverState(*condition.when) : nullptr;
		auto state =
		    DirectProjectScope(binder, std::move(read), condition.scoped, when.get(), condition.group_key_slots, {});
		state_index = state.state_index;
		plan = std::move(state.plan);
		AddItemStages(state);
		return std::move(plan);
	}

	//! Where BuildItems put the columns the aggregate plan reads.
	ItemColumns Items() const {
		ItemColumns items;
		for (idx_t i = 0; i < proved_bindings.size(); i++) {
			items.sources.push_back(columns.at(SourceName(i)).binding);
		}
		if (condition.scoped) {
			items.keys.push_back(columns.at(ELIGIBLE).binding);
			for (auto slot : condition.group_key_slots) {
				items.keys.push_back(columns.at(SourceName(slot)).binding);
			}
		}
		items.valid_data = columns.at("valid_data").binding;
		for (idx_t k = 0; k < DecisionCount(); k++) {
			auto binding = [&](const char *name) {
				return columns.at(Item(name, k)).binding;
			};
			DecisionColumns decision;
			decision.v = columns.at(Dec("v", k)).binding;
			decision.w = columns.at(Dec("w", k)).binding;
			decision.l = columns.at(Dec("l", k)).binding;
			decision.u = columns.at(Dec("u", k)).binding;
			decision.a = binding("a");
			decision.t = binding("t");
			decision.best = binding("best");
			decision.empty = binding("empty");
			decision.wz = binding("wz");
			decision.pos = binding("pos");
			decision.up_inf = binding("up_inf");
			decision.down_inf = binding("down_inf");
			decision.up_fin = binding("up_fin");
			decision.down_fin = binding("down_fin");
			decision.sterm = binding("sterm");
			decision.u1 = binding("u1");
			decision.upi = binding("upi");
			decision.dni = binding("dni");
			items.decisions.push_back(decision);
		}
		return items;
	}

private:
	struct Column {
		ColumnBinding binding;
		LogicalType type;
	};

	//! P1 to P5: the source columns and the scope as columns, the raw values, bounds and checks, the box, the anchor
	//! and price, and the rooms. With several decisions the items come between the box and the anchor, unless the
	//! decisions stay in columns of their own (BuildItems).
	void AddItemStages(const DirectScopeState &state) {
		for (idx_t i = 0; i < proved_bindings.size(); i++) {
			Declare(SourceName(i), state.input_bindings[i], source_types[i]);
		}
		if (condition.scoped) {
			Declare(ELIGIBLE, state.eligible, LogicalType::BOOLEAN);
		}
		ValidateDataLimits(state);
		auto checks = LayoutRawValues();
		AddBoundsAndValue(std::move(checks));
		AddBox();
		if (Several() && !items_by_decision) {
			AddItems();
		}
		AddAnchorAndPrice();
		AddRooms();
	}
	using Named = vector<std::pair<string, ExprPtr>>;

	const Condition &condition;
	Binder &binder;
	//! The bindings the proved expressions refer to, and the types of the source columns.
	vector<ColumnBinding> proved_bindings;
	vector<LogicalType> source_types;
	//! The scope projection's table index, or INVALID_INDEX when the input passed through it unchanged.
	idx_t state_index = DConstants::INVALID_INDEX;
	//! The table index of the projection that carries the source when it is not the source the expressions were proved
	//! over (a read of DirectSourceReader), or INVALID_INDEX.
	idx_t read_index = DConstants::INVALID_INDEX;
	//! The item stages (P4 and P5) run once per decision, each decision's values in columns of their own, instead of
	//! once over unpivoted items (BuildItems).
	bool items_by_decision = false;

	//! The plan so far and the columns it carries, in output order. Every stage forwards every column, and unused
	//! ones are removed with the rest of the unused columns after the DECIDE optimizer has run.
	unique_ptr<LogicalOperator> plan;
	vector<string> names;
	std::map<string, Column> columns;

	static string SourceName(idx_t i) {
		return "s" + std::to_string(i);
	}

	idx_t DecisionCount() const {
		return condition.boxes.size();
	}
	//! More than one decision: the plan works on items, one per decision of a row.
	bool Several() const {
		return DecisionCount() > 1;
	}
	//! The name of a per-decision column. With one decision it is the name of the item column itself, so the stages
	//! that follow read the same column whether or not the plan unpivots.
	string Dec(const string &base, idx_t decision) const {
		return Several() ? base + "_" + std::to_string(decision) : base;
	}
	//! How many times the item stages run, and the name of an item column in the run for `decision`: one run over the
	//! unpivoted items reads unsuffixed names, one run per decision suffixed ones.
	idx_t ItemRuns() const {
		return items_by_decision ? DecisionCount() : 1;
	}
	string Item(const string &base, idx_t decision) const {
		return items_by_decision ? Dec(base, decision) : base;
	}

	//! The column that says whether a row belongs to a budget; only a scoped budget has one.
	static constexpr const char *ELIGIBLE = "elig";
	//! The budget's lower and upper limit on every row, constant within a group; only a budget with a limit read from
	//! the data has them.
	static constexpr const char *BUDGET_LOWER = "budget_lo";
	static constexpr const char *BUDGET_UPPER = "budget_up";

	//! A copy of a proved expression, re-pointed at the scope projection when there is one.
	unique_ptr<Expression> OverState(const Expression &expr) const {
		auto carrier = state_index != DConstants::INVALID_INDEX ? state_index : read_index;
		if (carrier == DConstants::INVALID_INDEX) {
			return expr.Copy();
		}
		return DirectOverProjection(expr, proved_bindings, carrier);
	}

	void Declare(const string &name, ColumnBinding binding, const LogicalType &type) {
		D_ASSERT(columns.find(name) == columns.end());
		names.push_back(name);
		columns[name] = {binding, type};
	}

	//! The column of that name, as a value.
	ExprPtr C(const string &name) const {
		auto &column = columns.at(name);
		return calc.Col(column.type, column.binding);
	}

	//! One projection that forwards the columns in `keep` and appends the new ones, in order. Returns its table
	//! index.
	idx_t ProjectOnly(const vector<string> &keep, const Named &added) {
		auto index = binder.GenerateTableIndex();
		vector<unique_ptr<Expression>> expressions;
		vector<string> next_names;
		std::map<string, Column> next_columns;
		for (auto &name : keep) {
			auto &column = columns.at(name);
			next_columns[name] = {ColumnBinding(index, expressions.size()), column.type};
			next_names.push_back(name);
			expressions.push_back(calc.Out(calc.Col(column.type, column.binding)));
		}
		for (auto &entry : added) {
			D_ASSERT(next_columns.find(entry.first) == next_columns.end());
			auto expression = calc.Out(entry.second);
			next_columns[entry.first] = {ColumnBinding(index, expressions.size()), expression->return_type};
			next_names.push_back(entry.first);
			expressions.push_back(std::move(expression));
		}
		auto projection = make_uniq<LogicalProjection>(index, std::move(expressions));
		projection->children.push_back(std::move(plan));
		plan = std::move(projection);
		names = std::move(next_names);
		columns = std::move(next_columns);
		return index;
	}

	//! One projection that forwards every column and appends the new ones.
	idx_t Project(const Named &added) {
		return ProjectOnly(names, added);
	}

	//! One window node over the plan; its columns follow the existing ones.
	void Window(vector<std::pair<string, unique_ptr<BoundWindowExpression>>> added) {
		auto index = binder.GenerateTableIndex();
		auto window = make_uniq<LogicalWindow>(index);
		for (auto &entry : added) {
			D_ASSERT(columns.find(entry.first) == columns.end());
			names.push_back(entry.first);
			columns[entry.first] = {ColumnBinding(index, window->expressions.size()), entry.second->return_type};
			window->expressions.push_back(std::move(entry.second));
		}
		window->children.push_back(std::move(plan));
		plan = std::move(window);
	}

	//===------------------------------------------------------------------===//
	// Small value helpers
	//===------------------------------------------------------------------===//

	//! Every item's values as expressions over the columns so far, from P5 on.
	ItemExprs ItemOf() const {
		ItemExprs item;
		item.t = C("t");
		item.up_fin = C("up_fin");
		item.down_fin = C("down_fin");
		item.up_inf = C("up_inf");
		item.down_inf = C("down_inf");
		item.upi = C("upi");
		item.dni = C("dni");
		item.sterm = C("sterm");
		item.empty = C("empty");
		item.u1 = C("u1");
		item.wz = C("wz");
		item.pos = C("pos");
		item.best = C("best");
		item.a = C("a");
		item.w = C("w");
		item.l = C("l");
		item.u = C("u");
		return item;
	}

	//! The group sums W0 adds, in TotalSlot order.
	static const char *TotalName(idx_t slot) {
		static const char *const names[TOTAL_COUNT] = {"n_empty", "S",  "sup", "cup", "sdn", "cdn", "n_u1",
		                                               "lo",      "hi", "zup", "zdn", "czi", "czd"};
		return names[slot];
	}
	//! The sums at the price W3 adds, in PriceSlot order.
	static const char *PriceSumName(idx_t slot) {
		static const char *const names[SUM_COUNT] = {"a_star", "b_star", "tup", "tdn", "ctup", "ctdn"};
		return names[slot];
	}
	//! The budget's limits on a row: the constants, or with a limit read from the data the columns P2 computes.
	bool HasDataLimits() const {
		return !condition.data_limits.empty();
	}
	ExprPtr Lower() const {
		return HasDataLimits() ? C(BUDGET_LOWER) : calc.Num(condition.lower);
	}
	ExprPtr Upper() const {
		return HasDataLimits() ? C(BUDGET_UPPER) : calc.Num(condition.upper);
	}

	//! `sum(sign * value)` over the proved values, read as DOUBLE.
	unique_ptr<Expression> SumOf(const vector<DirectSignedValue> &values) const {
		unique_ptr<Expression> total;
		for (auto &value : values) {
			auto term = DirectScaledTerm(optimizer, OverState(*value.expr), value.sign, false, 1.0, false);
			total =
			    total ? calc.Out(calc.Add(calc.Wrap(std::move(total)), calc.Wrap(std::move(term)))) : std::move(term);
		}
		return total;
	}

	//! The terms of a part that name the decision.
	static vector<DirectSignedValue> SignedValues(const DirectLinearPart &part, idx_t decision) {
		vector<DirectSignedValue> values;
		for (auto &term : part.terms) {
			if (term.decision == decision) {
				values.push_back({term.sign, term.coefficient->Copy()});
			}
		}
		return values;
	}

	//===------------------------------------------------------------------===//
	// P0: limits read from the data
	//===------------------------------------------------------------------===//

	//! The limits read from the data. Every value is checked for NULL and NaN on every row of the source, the rows
	//! outside the budget included, and each limit is reduced per group as the solver reduces it: an upper limit takes
	//! the group's MIN, a lower one its MAX, and an equality both, which must agree (DirectValidateBounds, as S1 reads
	//! its data-valued bound). The extrema are window columns, constant within a group; they ride through the unnest of
	//! several decisions as the source columns do, so the reduction runs once per group, on rows.
	void ValidateDataLimits(const DirectScopeState &state) {
		if (!HasDataLimits()) {
			return;
		}
		vector<DirectBoundSpec> specs;
		for (auto &limit : condition.data_limits) {
			DirectBoundSpec spec;
			spec.comparison = limit.comparison;
			spec.value = OverState(*limit.value);
			spec.all_group_rows = limit.all_group_rows;
			if (limit.source_slot == DConstants::INVALID_INDEX) {
				spec.invalid_message = DirectInvalidBoundMessage();
			} else {
				// The same column on every row, so the solver's message is known before any row is read.
				spec.is_column = true;
				spec.column = state.input_bindings[limit.source_slot];
				spec.column_type = limit.source_type;
				spec.null_message = DirectBoundNullMessage(limit.column_name);
				if (limit.source_type == LogicalType::FLOAT || limit.source_type == LogicalType::DOUBLE) {
					spec.nan_message = DirectBoundNaNMessage();
				}
			}
			specs.push_back(std::move(spec));
		}
		vector<ColumnBinding> keys;
		vector<LogicalType> key_types;
		for (auto slot : condition.group_key_slots) {
			keys.push_back(state.input_bindings[slot]);
			key_types.push_back(source_types[slot]);
		}
		vector<DirectNotNullSpec> no_checks;
		auto validation = DirectValidateBounds(optimizer, std::move(plan), specs, no_checks,
		                                       condition.scoped ? &state.eligible : nullptr, keys, key_types);
		plan = std::move(validation.plan);
		for (idx_t j = 0; j < specs.size(); j++) {
			if (validation.min_slots[j] != DConstants::INVALID_INDEX) {
				Declare(LimitName(false, j), ColumnBinding(validation.window_index, validation.min_slots[j]),
				        LogicalType::DOUBLE);
			}
			if (validation.max_slots[j] != DConstants::INVALID_INDEX) {
				Declare(LimitName(true, j), ColumnBinding(validation.window_index, validation.max_slots[j]),
				        LogicalType::DOUBLE);
			}
		}
	}

	//! The window column of the group MIN (an upper limit) or MAX (a lower one) of data limit `j`.
	static string LimitName(bool maximum, idx_t j) {
		return string(maximum ? "limit_max_" : "limit_min_") + std::to_string(j);
	}

	//! One side of the budget on a row: the constant limit and the group extremum of each data limit on that side. A
	//! value is the number it is, however large (the solver loads a right side of 1e25 as that number), and an infinite
	//! one is infinite: an upper limit of +infinity is no bound, a lower limit of +infinity cannot be met. A row outside
	//! every budget has no extremum (NULL), which LEAST and GREATEST skip, so it takes the constant limit alone.
	ExprPtr Limit(bool upper) const {
		vector<ExprPtr> sides {calc.Num(upper ? condition.upper : condition.lower)};
		for (idx_t j = 0; j < condition.data_limits.size(); j++) {
			auto name = LimitName(!upper, j);
			if (columns.find(name) != columns.end()) {
				sides.push_back(C(name));
			}
		}
		return upper ? calc.Min(sides) : calc.Max(sides);
	}

	//===------------------------------------------------------------------===//
	// P1 and P2: raw values, per-clause bounds, the objective value
	//===------------------------------------------------------------------===//

	//! P1: every raw value, evaluated unconditionally on every row. Returns the check on each, built over P1. A
	//! decision the budget does not name has the weight 0, which needs no check; an objective part has a raw value for
	//! each decision it names.
	vector<unique_ptr<Expression>> LayoutRawValues() {
		Named added;
		for (idx_t j = 0; j < condition.clauses.size(); j++) {
			auto &clause = condition.clauses[j];
			added.emplace_back("c" + std::to_string(j), calc.Wrap(SumOf(clause.coefficient)));
			added.emplace_back("r" + std::to_string(j),
			                   calc.Cast(calc.Of(*OverState(*clause.rhs)), LogicalType::DOUBLE));
			if (clause.when) {
				added.emplace_back("wh" + std::to_string(j),
				                   calc.Wrap(DirectActiveWhen(calc, OverState(*clause.when).get())));
			}
		}
		for (idx_t k = 0; k < DecisionCount(); k++) {
			auto weights = SignedValues(condition.budget, k);
			added.emplace_back(Dec("w", k), weights.empty() ? Zero() : calc.Wrap(SumOf(weights)));
			for (idx_t p = 0; p < condition.objective.size(); p++) {
				auto values = SignedValues(condition.objective[p], k);
				if (!values.empty()) {
					added.emplace_back(Dec("o" + std::to_string(p), k), calc.Wrap(SumOf(values)));
				}
			}
		}
		auto p1 = Project(added);

		// The checks name the source columns a NULL comes from, so they read the source through P1.
		vector<unique_ptr<Expression>> checks;
		auto number = [&](const string &name, const vector<DirectSignedValue> &values, DirectNumberKind kind) {
			auto source = DirectMergedOverProjection(optimizer.context, values, proved_bindings, p1);
			checks.push_back(DirectValidNumberPredicate(optimizer, LogicalType::DOUBLE, columns.at(name).binding,
			                                            source.get(), kind));
		};
		for (idx_t j = 0; j < condition.clauses.size(); j++) {
			auto &clause = condition.clauses[j];
			number("c" + std::to_string(j), clause.coefficient, DirectNumberKind::CONSTRAINT);
			checks.push_back(DirectValidBoundPredicate(optimizer, columns.at("r" + std::to_string(j)).binding,
			                                           *DirectOverProjection(*clause.rhs, proved_bindings, p1)));
		}
		for (idx_t k = 0; k < DecisionCount(); k++) {
			auto weights = SignedValues(condition.budget, k);
			if (!weights.empty()) {
				number(Dec("w", k), weights, DirectNumberKind::CONSTRAINT);
			}
			for (idx_t p = 0; p < condition.objective.size(); p++) {
				auto values = SignedValues(condition.objective[p], k);
				if (!values.empty()) {
					number(Dec("o" + std::to_string(p), k), values, DirectNumberKind::OBJECTIVE);
				}
			}
		}
		return checks;
	}

	//! The value of a decision's items: the objective as a maximum, so MINIMIZE negates it, and zero for no objective
	//! or for a decision the objective does not name.
	ExprPtr ValueOf(idx_t decision) const {
		ExprPtr value = Zero();
		for (idx_t p = 0; p < condition.objective.size(); p++) {
			auto &part = condition.objective[p];
			auto name = Dec("o" + std::to_string(p), decision);
			if (columns.find(name) == columns.end()) {
				continue;
			}
			auto scaled = calc.Wrap(
			    DirectScaledTerm(optimizer, calc.Out(C(name)), part.sign, part.scaled, part.scale, part.divides));
			value = calc.Add(value, scaled);
		}
		return condition.sense == DecideSense::MINIMIZE ? calc.Neg(value) : value;
	}

	//! P2: the check on every raw value (true, or the row raises), the budget's limits on every row when it has a
	//! limit read from the data, each clause's bounds, and each decision's v.
	void AddBoundsAndValue(vector<unique_ptr<Expression>> checks) {
		Named added;
		added.emplace_back("valid_data", calc.Wrap(DirectAllHold(calc, std::move(checks))));
		if (HasDataLimits()) {
			added.emplace_back(BUDGET_LOWER, Limit(false));
			added.emplace_back(BUDGET_UPPER, Limit(true));
		}
		for (idx_t j = 0; j < condition.clauses.size(); j++) {
			auto suffix = std::to_string(j);
			DirectBoundColumns raw;
			raw.coefficient = columns.at("c" + suffix).binding;
			raw.rhs = columns.at("r" + suffix).binding;
			if (condition.clauses[j].when) {
				raw.has_when = true;
				raw.when = columns.at("wh" + suffix).binding;
			}
			ExprPtr lower, upper;
			DirectClauseBounds(calc, condition.clauses[j].comparison, false, raw, lower, upper);
			added.emplace_back("lo" + suffix, lower);
			added.emplace_back("up" + suffix, upper);
		}
		for (idx_t k = 0; k < DecisionCount(); k++) {
			added.emplace_back(Dec("v", k), ValueOf(k));
		}
		Project(added);
	}

	//===------------------------------------------------------------------===//
	// P3 to P5: the box, the anchor and price, the rooms
	//===------------------------------------------------------------------===//

	//! P3: each decision's box, the static box intersected with the bounds of the clauses on that decision. An end of
	//! magnitude 1e20 or more is infinite where the shared reader reads it: in the static box, and in a clause's right
	//! side.
	void AddBox() {
		Named added;
		for (idx_t k = 0; k < DecisionCount(); k++) {
			vector<ColumnBinding> clause_lowers, clause_uppers;
			for (idx_t j = 0; j < condition.clauses.size(); j++) {
				if (condition.clauses[j].decision == k) {
					clause_lowers.push_back(columns.at("lo" + std::to_string(j)).binding);
					clause_uppers.push_back(columns.at("up" + std::to_string(j)).binding);
				}
			}
			ExprPtr lower, upper;
			DirectRowBox(calc, condition.boxes[k].lower, condition.boxes[k].upper, clause_lowers, clause_uppers, lower,
			             upper);
			added.emplace_back(Dec("l", k), lower);
			added.emplace_back(Dec("u", k), upper);
		}
		Project(added);
	}

	//! The items of several decisions: the unpivot DuckDB's own UNPIVOT makes. The source rows are numbered, and each
	//! row is unnested into one item per decision, carrying the decision's index and its w, v, l and u under the names
	//! the stages that follow read. The source columns ride along, once per item.
	void AddItems() {
		// Hand-built: a window is an operator-specific node, which the builder (scalar expressions) has no operation
		// for. With no order and no partition the number sorts nothing.
		auto number =
		    make_uniq<BoundWindowExpression>(ExpressionType::WINDOW_ROW_NUMBER, LogicalType::BIGINT, nullptr, nullptr);
		number->start = WindowBoundary::UNBOUNDED_PRECEDING;
		number->end = WindowBoundary::CURRENT_ROW_ROWS;
		vector<std::pair<string, unique_ptr<BoundWindowExpression>>> numbered;
		numbered.emplace_back("rn", std::move(number));
		Window(std::move(numbered));

		auto index = binder.GenerateTableIndex();
		auto unnest = make_uniq<LogicalUnnest>(index);
		auto unnest_list = [&](const string &name, const ExprPtr &list, const LogicalType &element) {
			auto item = make_uniq<BoundUnnestExpression>(element);
			item->child = calc.Out(list);
			names.push_back(name);
			columns[name] = {ColumnBinding(index, unnest->expressions.size()), element};
			unnest->expressions.push_back(std::move(item));
		};
		vector<Value> decision_indexes;
		for (idx_t k = 0; k < DecisionCount(); k++) {
			decision_indexes.push_back(Value::INTEGER(NumericCast<int32_t>(k)));
		}
		unnest_list("k", calc.Constant(Value::LIST(LogicalType::INTEGER, decision_indexes)), LogicalType::INTEGER);
		for (auto base : {"w", "v", "l", "u"}) {
			vector<ExprPtr> per_decision;
			for (idx_t k = 0; k < DecisionCount(); k++) {
				per_decision.push_back(C(Dec(base, k)));
			}
			unnest_list(base, calc.Fn("list_value", per_decision), LogicalType::DOUBLE);
		}
		unnest->children.push_back(std::move(plan));
		plan = std::move(unnest);
	}

	//! P4: whether the box is empty, the anchor (0 clamped into the box; 0 on an empty box, where it is never read),
	//! whether the item is outside the ranking (w = 0, or its row belongs to no budget), and its break-even price.
	//! Items outside the ranking sort last.
	void AddAnchorAndPrice() {
		Named added;
		for (idx_t k = 0; k < ItemRuns(); k++) {
			auto l = C(Item("l", k)), u = C(Item("u", k)), w = C(Item("w", k)), v = C(Item("v", k));
			auto empty = DirectBoxIsEmpty(calc, l, u);
			auto anchor = calc.If(empty, Zero(), calc.Min({calc.Max({Zero(), l}), u}));
			auto free_of_budget = Equal(w, Zero());
			if (condition.scoped) {
				free_of_budget = calc.Any({free_of_budget, Not(C(ELIGIBLE))});
			}
			// Adding 0.0 turns -0.0 into 0.0, so prices that are equal sort and compare as equal.
			auto price = calc.If(free_of_budget, NegativeInfinity(), calc.Add(calc.Div(v, w), Zero()));
			added.emplace_back(Item("empty", k), empty);
			added.emplace_back(Item("a", k), anchor);
			added.emplace_back(Item("wz", k), free_of_budget);
			added.emplace_back(Item("pos", k), Greater(w, Zero()));
			added.emplace_back(Item("t", k), price);
		}
		Project(added);
	}

	//! P5: each item's room up and room down, in units of budget. A finite room goes in `up_fin` and `down_fin`; an
	//! infinite one is a flag, counted apart. The spend of the anchors, the best end of an item outside the ranking,
	//! and whether that item is unbounded.
	void AddRooms() {
		Named added;
		for (idx_t k = 0; k < ItemRuns(); k++) {
			auto w = C(Item("w", k)), v = C(Item("v", k)), l = C(Item("l", k)), u = C(Item("u", k)),
			     a = C(Item("a", k));
			auto empty = C(Item("empty", k)), wz = C(Item("wz", k)), pos = C(Item("pos", k));
			auto inert = calc.Any({empty, wz});
			auto high = calc.If(pos, u, l);
			auto low = calc.If(pos, l, u);
			auto up_open = And(Not(inert), calc.If(pos, Equal(u, PositiveInfinity()), Equal(l, NegativeInfinity())));
			auto down_open = And(Not(inert), calc.If(pos, Equal(l, NegativeInfinity()), Equal(u, PositiveInfinity())));
			auto up_room = calc.If(calc.Any({inert, up_open}), Zero(), calc.Mul(w, calc.Sub(high, a)));
			auto down_room = calc.If(calc.Any({inert, down_open}), Zero(), calc.Mul(w, calc.Sub(a, low)));
			auto best_end = calc.If(Greater(v, Zero()), u, calc.If(Less(v, Zero()), l, a));
			auto unbounded_end = calc.Any({And(Greater(v, Zero()), Equal(u, PositiveInfinity())),
			                               And(Less(v, Zero()), Equal(l, NegativeInfinity()))});
			added.emplace_back(Item("up_inf", k), up_open);
			added.emplace_back(Item("down_inf", k), down_open);
			added.emplace_back(Item("up_fin", k), up_room);
			added.emplace_back(Item("down_fin", k), down_room);
			added.emplace_back(Item("sterm", k), calc.Mul(w, a));
			added.emplace_back(Item("best", k), best_end);
			added.emplace_back(Item("u1", k), Flag(And(And(wz, Not(empty)), unbounded_end)));
			added.emplace_back(Item("upi", k), Flag(up_open));
			added.emplace_back(Item("dni", k), Flag(down_open));
		}
		Project(added);
	}

	//! Partitions a window by the budget's groups: the rows outside every budget apart from the rest, then the `PER`
	//! keys. Without a scope the window covers the whole input.
	void PartitionByGroup(BoundWindowExpression &window) const {
		for (auto &column : GroupColumns()) {
			window.partitions.push_back(calc.Out(column));
		}
	}

	//! The columns that tell the groups apart: whether the row belongs to a budget, then the `PER` keys. None without a
	//! scope.
	vector<ExprPtr> GroupColumns() const {
		vector<ExprPtr> group;
		if (condition.scoped) {
			group.push_back(C(ELIGIBLE));
			for (auto slot : condition.group_key_slots) {
				group.push_back(C(SourceName(slot)));
			}
		}
		return group;
	}

	//! A sum, maximum or minimum over the group, as a window.
	unique_ptr<BoundWindowExpression> OverAll(const char *function, const ExprPtr &value) const {
		auto window = DirectWindowExtremum(optimizer, function, calc.Out(value));
		PartitionByGroup(*window);
		return window;
	}

	//! W0: the group totals. Every sum here is over finite parts; the open rooms are counts.
	void AddGroupTotals() {
		auto inputs = TotalInputs(ItemOf());
		vector<std::pair<string, unique_ptr<BoundWindowExpression>>> added;
		for (idx_t slot = 0; slot < TOTAL_COUNT; slot++) {
			added.emplace_back(TotalName(slot), OverAll(TotalFunction(slot), inputs[slot]));
		}
		Window(std::move(added));
	}

	//! W1, the plan's one sort: running sums of the rooms in order of price, highest first, ties in any order. The
	//! inclusive sum of an item is its own room plus that of every item before it.
	void AddRunningSums() {
		auto item = ItemOf();
		auto group = GroupColumns();
		vector<std::pair<string, unique_ptr<BoundWindowExpression>>> added;
		added.emplace_back("uinc", RunningSum(item.up_fin, item.t, group));
		added.emplace_back("dinc", RunningSum(item.down_fin, item.t, group));
		added.emplace_back("cui", RunningSum(item.upi, item.t, group));
		added.emplace_back("cdi", RunningSum(item.dni, item.t, group));
		Window(std::move(added));
	}

	//! P6: the verdicts, which limit the budget binds at, and whether each item reaches it (VerdictOf and CandidateOf).
	//! L and U are the budget's limits on the row, constant within a group (Lower and Upper). The budget binds at U
	//! when the zero-price spend is above it ("over") and at L when it is below ("under"); the target spend is then
	//! that limit. Otherwise the price is zero.
	//!
	//! The rows outside every budget are a partition of their own. Their items are all outside the ranking, so the
	//! price, which limit binds and the spends of that partition are never read (each item answers with its own best
	//! end). Only a box no value satisfies and an item that improves without end are tested on them; the test for an
	//! unreachable spend is for the rows of a group alone.
	void AddVerdicts() {
		auto lower = Lower(), upper = Upper();
		vector<ExprPtr> totals;
		for (idx_t slot = 0; slot < TOTAL_COUNT; slot++) {
			totals.push_back(C(TotalName(slot)));
		}
		auto verdict = VerdictOf(totals, lower, upper, condition.scoped ? C(ELIGIBLE) : nullptr);
		auto item = ItemOf();
		// The finite room down of the items after this one, and the finite room up of the items before it.
		auto after = calc.Sub(totals[TOTAL_SDN], C("dinc"));
		auto prior_up = calc.Sub(C("uinc"), item.up_fin);
		// The running counts say where the infinite rooms lie: up to and including the item (cui, cdi), and in the
		// group.
		auto open_after = calc.Sub(totals[TOTAL_CDN], C("cdi"));
		Reach reach {Greater(C("cui"), Zero()),
		             Greater(open_after, Zero()),
		             Greater(calc.Add(open_after, item.dni), Zero()),
		             Greater(calc.Sub(C("cui"), item.upi), Zero()),
		             calc.Sub(calc.Add(totals[TOTAL_S], C("uinc")), after),
		             calc.Sub(calc.Sub(calc.Add(totals[TOTAL_S], prior_up), after), item.down_fin)};
		Project({{"infeasible", verdict.infeasible},
		         {"unbounded", verdict.unbounded},
		         {"over", verdict.over},
		         {"under", verdict.under},
		         {"dafter", after},
		         {"cand", CandidatePrice(verdict, reach, item.t, lower, upper)},
		         {"prior_up", prior_up}});
	}

	//! W2: the price of a binding budget (PriceOf): the highest and the lowest candidate of the group.
	void AddPrice() {
		vector<std::pair<string, unique_ptr<BoundWindowExpression>>> added;
		added.emplace_back("lam_over", OverAll("max", C("cand")));
		added.emplace_back("lam_under", OverAll("min", C("cand")));
		Window(std::move(added));
	}

	//! The price: that of the item the limit picks when the budget binds, zero when it does not.
	ExprPtr Price() const {
		return PriceOf(C("over"), C("under"), C("lam_over"), C("lam_under"));
	}

	//! W3: the rooms above, below and at the price.
	void AddPriceSums() {
		auto inputs = PriceSumInputs(ItemOf(), Price());
		vector<std::pair<string, unique_ptr<BoundWindowExpression>>> added;
		for (idx_t slot = 0; slot < SUM_COUNT; slot++) {
			added.emplace_back(PriceSumName(slot), OverAll("sum", inputs[slot]));
		}
		Window(std::move(added));
	}

	//! P7: each item's answer. Above the price an item is at its high end, below it at its low end, and an item with
	//! w = 0 at its best end. The items at the price share what the binding limit leaves them, D, starting from their
	//! anchors:
	//! up when D is positive, down when it is negative, each by its room or by what remains after the items before it
	//! in the sort (those after it, for a move down), found from the running sums. An open room takes what remains of
	//! the finite rooms, evenly with the other open rooms at the price. The answer is held to the box.
	void AddAnswer() {
		auto lam = Price(), w = C("w"), a = C("a");
		// What the binding limit leaves to the items at the price, from their anchors; none when the budget does not
		// bind.
		auto others = calc.Sub(calc.Add(C("S"), C("a_star")), C("b_star"));
		auto d = calc.If(C("over"), calc.Sub(Upper(), others), calc.If(C("under"), calc.Sub(Lower(), others), Zero()));
		auto e = calc.Neg(d);
		auto remaining_up = calc.Max({calc.Sub(d, calc.Sub(C("prior_up"), C("a_star"))), Zero()});
		auto move_up = calc.If(C("up_inf"), calc.Div(calc.Max({calc.Sub(d, C("tup")), Zero()}), C("ctup")),
		                       calc.Min({remaining_up, C("up_fin")}));
		auto remaining_down = calc.Max({calc.Sub(e, calc.Sub(C("dafter"), C("b_star"))), Zero()});
		auto move_down = calc.If(C("down_inf"), calc.Div(calc.Max({calc.Sub(e, C("tdn")), Zero()}), C("ctdn")),
		                         calc.Min({remaining_down, C("down_fin")}));
		auto shared = calc.If(calc.Cmp(ExpressionType::COMPARE_GREATERTHANOREQUALTO, d, Zero()),
		                      calc.Add(a, calc.Div(move_up, w)), calc.Sub(a, calc.Div(move_down, w)));
		Project({{"x", AnswerOf(ItemOf(), lam, shared)}});
	}

	//! What the plan releases: the source columns, one answer per decision, and the column that proves every item was
	//! checked, each as a binding of `plan`.
	struct Released {
		unique_ptr<LogicalOperator> plan;
		vector<ColumnBinding> sources;
		vector<ColumnBinding> decisions;
		ColumnBinding validated;
	};

	//! The breaker over the items, and with several decisions the group-back. The breaker holds the source, the
	//! answer, the flags it checks and, with several decisions, the row number and the decision index.
	Released Release() {
		vector<string> keep;
		for (idx_t i = 0; i < proved_bindings.size(); i++) {
			keep.push_back(SourceName(i));
		}
		if (Several()) {
			keep.push_back("rn");
			keep.push_back("k");
		}
		keep.push_back("x");
		for (auto name : {"valid_data", "v", "infeasible", "unbounded"}) {
			keep.push_back(name);
		}
		ProjectOnly(keep, {});
		auto breaker = DirectValidationBreaker(binder, std::move(plan), Valid());
		auto sources = proved_bindings.size();
		Released released;
		released.sources.assign(breaker.input_bindings.begin(), breaker.input_bindings.begin() + sources);
		released.validated = breaker.breaker;
		if (!Several()) {
			released.decisions.push_back(breaker.input_bindings[sources]);
			released.plan = std::move(breaker.plan);
			return released;
		}
		return GroupBack(std::move(breaker), released);
	}

	//! Several decisions: the items of a row are grouped back into the row by a hash aggregate on the row number. A
	//! source column is the same on every item of its row, so ANY_VALUE returns it. A decision's answer is the answer
	//! of its own item, picked by the decision index (ANY_VALUE skips the NULLs of the other items).
	Released GroupBack(DirectPipelineBreaker breaker, const Released &released) {
		auto sources = proved_bindings.size();
		auto row_number = breaker.input_bindings[sources], index = breaker.input_bindings[sources + 1],
		     answer = breaker.input_bindings[sources + 2];
		vector<unique_ptr<Expression>> aggregates;
		auto any_value = [&](const ExprPtr &value) {
			vector<unique_ptr<Expression>> children;
			children.push_back(calc.Out(value));
			aggregates.push_back(optimizer.BindAggregateFunction("any_value", std::move(children)));
		};
		for (idx_t i = 0; i < sources; i++) {
			any_value(calc.Col(source_types[i], released.sources[i]));
		}
		for (idx_t k = 0; k < DecisionCount(); k++) {
			auto is_decision =
			    Equal(calc.Col(LogicalType::INTEGER, index), calc.Constant(Value::INTEGER(NumericCast<int32_t>(k))));
			any_value(calc.If(is_decision, calc.Col(LogicalType::DOUBLE, answer), NullDouble()));
		}
		any_value(calc.Col(LogicalType::BIGINT, released.validated));
		auto group_index = binder.GenerateTableIndex();
		auto aggregate_index = binder.GenerateTableIndex();
		auto aggregate = make_uniq<LogicalAggregate>(group_index, aggregate_index, std::move(aggregates));
		aggregate->groups.push_back(calc.Out(calc.Col(LogicalType::BIGINT, row_number)));
		aggregate->children.push_back(std::move(breaker.plan));

		Released grouped;
		idx_t slot = 0;
		for (idx_t i = 0; i < sources; i++) {
			grouped.sources.emplace_back(aggregate_index, slot++);
		}
		for (idx_t k = 0; k < DecisionCount(); k++) {
			grouped.decisions.emplace_back(aggregate_index, slot++);
		}
		grouped.validated = ColumnBinding(aggregate_index, slot);
		grouped.plan = std::move(aggregate);
		return grouped;
	}

	//! The substitute: the source columns, one answer per decision, then the validated column.
	DirectSubstitute Output(Released released) {
		DirectSubstitute substitute;
		auto sources = proved_bindings.size();
		for (idx_t k = 0; k < DecisionCount(); k++) {
			substitute.prunable_decisions.push_back(1);
		}
		for (idx_t i = 0; i < sources + DecisionCount(); i++) {
			substitute.slot_map.push_back(i);
		}
		substitute.validated_columns.push_back(sources + DecisionCount());
		auto result_index = binder.GenerateTableIndex();
		vector<unique_ptr<Expression>> result;
		for (idx_t i = 0; i < sources; i++) {
			result.push_back(calc.Out(calc.Col(source_types[i], released.sources[i])));
		}
		for (auto &decision : released.decisions) {
			result.push_back(calc.Out(calc.Col(LogicalType::DOUBLE, decision)));
		}
		result.push_back(calc.Out(calc.Col(LogicalType::BIGINT, released.validated)));
		auto projection = make_uniq<LogicalProjection>(result_index, std::move(result));
		projection->children.push_back(std::move(released.plan));
		substitute.child = std::move(projection);
		return substitute;
	}

	//! What the breaker evaluates on every row: true, or the row's error. The raw values were checked in P2 and arrive
	//! as `valid_data`; v is checked again because the objective's constant factors and the sum of its parts can
	//! overflow. Infeasible comes before unbounded, as a problem with no assignment is infeasible whatever its open
	//! bounds would allow. Each row reads its own group's verdicts, so when one group is infeasible and another
	//! unbounded, which error the query raises is not specified.
	unique_ptr<Expression> Valid() {
		vector<unique_ptr<Expression>> checks;
		checks.push_back(calc.Out(C("valid_data")));
		checks.push_back(DirectValidNumberPredicate(optimizer, LogicalType::DOUBLE, columns.at("v").binding, nullptr,
		                                            DirectNumberKind::OBJECTIVE));
		checks.push_back(DirectRaiseIf(calc, calc.Out(C("infeasible")), DirectInfeasibleErrorPredicate(optimizer)));
		checks.push_back(DirectRaiseIf(calc, calc.Out(C("unbounded")), DirectUnboundedErrorPredicate(optimizer)));
		return DirectAllHold(calc, std::move(checks));
	}
};

//! One projection under construction: each column is added as an expression and gets its binding at once.
class ProjectionStage {
public:
	ProjectionStage(Binder &binder, const ExpressionBuilder &calc_p)
	    : calc(calc_p), index(binder.GenerateTableIndex()) {
	}
	ColumnBinding Add(const ExprPtr &expression) {
		expressions.push_back(calc.Out(expression));
		return ColumnBinding(index, expressions.size() - 1);
	}
	unique_ptr<LogicalOperator> Finish(unique_ptr<LogicalOperator> child) {
		auto projection = make_uniq<LogicalProjection>(index, std::move(expressions));
		projection->children.push_back(std::move(child));
		return std::move(projection);
	}

private:
	const ExpressionBuilder &calc;
	idx_t index;
	vector<unique_ptr<Expression>> expressions;
};

//! One hash aggregate under construction: the groups, then the aggregates, each with the binding it gets in the output.
class AggregateStage {
public:
	explicit AggregateStage(Binder &binder)
	    : group_index(binder.GenerateTableIndex()), aggregate_index(binder.GenerateTableIndex()) {
	}
	ColumnBinding Group(unique_ptr<Expression> expression) {
		groups.push_back(std::move(expression));
		return ColumnBinding(group_index, groups.size() - 1);
	}
	ColumnBinding Add(unique_ptr<Expression> aggregate) {
		aggregates.push_back(std::move(aggregate));
		return ColumnBinding(aggregate_index, aggregates.size() - 1);
	}
	unique_ptr<LogicalOperator> Finish(unique_ptr<LogicalOperator> child) {
		auto aggregate = make_uniq<LogicalAggregate>(group_index, aggregate_index, std::move(aggregates));
		aggregate->groups = std::move(groups);
		aggregate->children.push_back(std::move(child));
		return std::move(aggregate);
	}

private:
	idx_t group_index;
	idx_t aggregate_index;
	vector<unique_ptr<Expression>> groups;
	vector<unique_ptr<Expression>> aggregates;
};

//! Builds the plan of definition.md § The plan for one or several decisions and limits that are constants:
//! aggregate-then-join.
//! Each group is reduced to one row of numbers (its totals, its price, its sums at the price) and every source row
//! joins its group's row and computes its answer in one projection. A group key is whether the row belongs to a budget,
//! then its `PER` keys; keys are compared with IS NOT DISTINCT FROM, so a NULL key matches. Without a scope there is
//! one group: the aggregates have no GROUP BY and the join is a cross product with that one row.
//!
//! Each stage reads the source once more (DirectSourceReader) through PlanBuilder::BuildItems, which computes the
//! values of every item on that read, and keeps what it needs of them in its own struct of bindings. A row has one
//! item per decision, each in columns of its own; no stage unpivots the row except the price, which unnests the narrow
//! lists of prices and rooms (Narrow):
//!
//!   1. item values      ItemColumns   a projection over a read of the source; per decision, DecisionColumns
//!   2. group totals     TotalColumns  a hash aggregate per group over stage 1 (a row adds the sum of its items), then
//!                                     a projection: the verdicts, as
//!                                     checks that raise, and the target spend's side. Every row's NULL and NaN checks
//!                                     are evaluated inside the aggregate, so they raise before a verdict does.
//!   3. price            PriceColumns  stage 1's few columns through the plan's one window (partitioned by group,
//!                                     ordered by price, highest first), joined to stage 2, then a hash aggregate
//!   4. shares           ShareColumns  a hash aggregate per group over stage 1 joined to stage 3: the sums at the price
//!   5. answer           a join of stage 1 with stage 4 and one projection
//!
//! Every row is checked before any is released: stage 5's rows come out of a join with stage 4, whose rows exist only
//! once stages 2 to 4 have read every row. The verdict column rides from stage 2 to the output, so a parent that reads
//! no answer still raises.
class AggregatePlanBuilder : public ItemAlgebra {
public:
	AggregatePlanBuilder(const Condition &condition_p, Optimizer &optimizer_p, DirectSourceReader &reader_p)
	    : ItemAlgebra(optimizer_p), condition(condition_p), binder(optimizer_p.binder), reader(reader_p) {
		if (condition.scoped) {
			key_types.push_back(LogicalType::BOOLEAN);
			for (auto slot : condition.group_key_slots) {
				key_types.push_back(reader.SourceTypes()[slot]);
			}
		}
	}

	DirectSubstitute Build() {
		auto totals = GroupTotals();
		auto price = Price(std::move(totals));
		auto shares = Shares(std::move(price));
		return Answer(std::move(shares));
	}

private:
	//! Stage 1 as one read of the source builds it.
	struct Items {
		unique_ptr<LogicalOperator> plan;
		ItemColumns columns;
	};
	//! Stage 2: the group's key, what stage 3 reads of its totals (the spend of the anchors, the finite room down, the
	//! highest price of an infinite room up and the lowest of one down), which limit the budget binds at, and the check
	//! that raises a verdict.
	struct TotalColumns {
		vector<ColumnBinding> key;
		ColumnBinding anchors, down, highest_open_up, lowest_open_down, over, under, ok;
	};
	//! Stage 3: the group's key, its price, and what stage 4 needs of stage 2.
	struct PriceColumns {
		vector<ColumnBinding> key;
		ColumnBinding price, anchors, over, under, ok;
	};
	//! Stage 4: the group's key, its price, whether the tied items move up, and the two shares of step 6: the fraction
	//! of its room that each tied item with a finite room moves, and what each tied item with an infinite room moves.
	struct ShareColumns {
		vector<ColumnBinding> key;
		ColumnBinding price, up, fraction, open_share, ok;
	};
	template <class Columns>
	struct Stage {
		unique_ptr<LogicalOperator> plan;
		Columns columns;
	};

	const Condition &condition;
	Binder &binder;
	DirectSourceReader &reader;
	//! The types of the group key: BOOLEAN, then the `PER` keys. Empty without a scope.
	vector<LogicalType> key_types;

	ExprPtr DoubleColumn(ColumnBinding binding) const {
		return calc.Col(LogicalType::DOUBLE, binding);
	}
	ExprPtr BoolColumn(ColumnBinding binding) const {
		return calc.Col(LogicalType::BOOLEAN, binding);
	}
	ExprPtr KeyColumn(idx_t i, const vector<ColumnBinding> &key) const {
		return calc.Col(key_types[i], key[i]);
	}
	ExprPtr Lower() const {
		return calc.Num(condition.lower);
	}
	ExprPtr Upper() const {
		return calc.Num(condition.upper);
	}

	//! A new read of the source, as stage 1 computes it.
	Items ReadItems() {
		auto read = reader.ReadSource();
		PlanBuilder builder(condition, optimizer, reader.SourceBindings(), reader.SourceTypes());
		Items items;
		items.plan = builder.BuildItems(std::move(read.plan), read.table_index);
		items.columns = builder.Items();
		return items;
	}

	ItemExprs ItemOf(const DecisionColumns &c) const {
		ItemExprs item;
		item.t = DoubleColumn(c.t);
		item.up_fin = DoubleColumn(c.up_fin);
		item.down_fin = DoubleColumn(c.down_fin);
		item.up_inf = BoolColumn(c.up_inf);
		item.down_inf = BoolColumn(c.down_inf);
		item.upi = DoubleColumn(c.upi);
		item.dni = DoubleColumn(c.dni);
		item.sterm = DoubleColumn(c.sterm);
		item.empty = BoolColumn(c.empty);
		item.u1 = DoubleColumn(c.u1);
		item.wz = BoolColumn(c.wz);
		item.pos = BoolColumn(c.pos);
		item.best = DoubleColumn(c.best);
		item.a = DoubleColumn(c.a);
		item.w = DoubleColumn(c.w);
		item.l = DoubleColumn(c.l);
		item.u = DoubleColumn(c.u);
		return item;
	}
	//! The items of a row, one per decision.
	vector<ItemExprs> ItemsOf(const ItemColumns &c) const {
		vector<ItemExprs> items;
		for (auto &decision : c.decisions) {
			items.push_back(ItemOf(decision));
		}
		return items;
	}

	unique_ptr<Expression> Aggregate(const char *function, const ExprPtr &value) const {
		vector<unique_ptr<Expression>> children;
		children.push_back(calc.Out(value));
		return optimizer.BindAggregateFunction(function, std::move(children));
	}

	//! Joins an item plan to a group plan on the group key, compared with IS NOT DISTINCT FROM: the cross product with
	//! the group's one row when there is no key.
	unique_ptr<LogicalOperator> JoinOnKey(unique_ptr<LogicalOperator> items, const vector<ColumnBinding> &item_key,
	                                      unique_ptr<LogicalOperator> groups, const vector<ColumnBinding> &group_key) {
		if (key_types.empty()) {
			return LogicalCrossProduct::Create(std::move(items), std::move(groups));
		}
		auto join = make_uniq<LogicalComparisonJoin>(JoinType::INNER);
		for (idx_t i = 0; i < key_types.size(); i++) {
			JoinCondition key_equal;
			key_equal.comparison = ExpressionType::COMPARE_NOT_DISTINCT_FROM;
			key_equal.left = calc.Out(KeyColumn(i, item_key));
			key_equal.right = calc.Out(KeyColumn(i, group_key));
			join->conditions.push_back(std::move(key_equal));
		}
		join->children.push_back(std::move(items));
		join->children.push_back(std::move(groups));
		return std::move(join);
	}

	//! Stage 2. The aggregate sums stage 1's values over the group (the sums of TotalInputs, the number of rows, and
	//! the check of every row's raw values and v: a NULL or NaN raises here). The projection after it tests the
	//! verdicts: the empty-aggregate error for a scope that covers no row, then the data checks, then infeasible, then
	//! unbounded; a source with no row has no verdict.
	Stage<TotalColumns> GroupTotals() {
		auto items = ReadItems();
		auto inputs = TotalInputsOfRow(ItemsOf(items.columns));
		AggregateStage aggregate(binder);
		vector<ColumnBinding> key;
		for (idx_t i = 0; i < key_types.size(); i++) {
			key.push_back(aggregate.Group(calc.Out(KeyColumn(i, items.columns.keys))));
		}
		auto rows = aggregate.Add(optimizer.BindAggregateFunction("count_star", vector<unique_ptr<Expression>>()));
		vector<unique_ptr<Expression>> data_checks;
		data_checks.push_back(calc.Out(BoolColumn(items.columns.valid_data)));
		for (auto &decision : items.columns.decisions) {
			data_checks.push_back(DirectValidNumberPredicate(optimizer, LogicalType::DOUBLE, decision.v, nullptr,
			                                                 DirectNumberKind::OBJECTIVE));
		}
		auto data_valid = aggregate.Add(Aggregate("bool_and", calc.Wrap(DirectAllHold(calc, std::move(data_checks)))));
		vector<ColumnBinding> sums;
		for (idx_t slot = 0; slot < TOTAL_COUNT; slot++) {
			sums.push_back(aggregate.Add(Aggregate(TotalFunction(slot), inputs[slot])));
		}
		auto plan = aggregate.Finish(std::move(items.plan));

		// With a scope the rows outside every budget are groups of their own, and the budget must cover some row.
		ColumnBinding active;
		if (condition.scoped) {
			auto window_index = binder.GenerateTableIndex();
			auto window = make_uniq<LogicalWindow>(window_index);
			window->expressions.push_back(DirectWindowMatchingCount(calc, calc.Out(BoolColumn(key[0]))));
			window->children.push_back(std::move(plan));
			plan = std::move(window);
			active = ColumnBinding(window_index, 0);
		}

		vector<ExprPtr> totals;
		for (auto &sum : sums) {
			totals.push_back(DoubleColumn(sum));
		}
		auto verdict = VerdictOf(totals, Lower(), Upper(), condition.scoped ? BoolColumn(key[0]) : nullptr);
		auto has_rows = calc.Cmp(ExpressionType::COMPARE_GREATERTHAN, calc.Col(LogicalType::BIGINT, rows),
		                         calc.Constant(Value::BIGINT(0)));
		vector<unique_ptr<Expression>> checks;
		if (condition.scoped) {
			checks.push_back(DirectActiveRowsCheck(optimizer, active));
		}
		checks.push_back(calc.Out(BoolColumn(data_valid)));
		checks.push_back(DirectRaiseIf(calc, calc.Out(And(has_rows, verdict.infeasible)),
		                               DirectInfeasibleErrorPredicate(optimizer)));
		checks.push_back(
		    DirectRaiseIf(calc, calc.Out(And(has_rows, verdict.unbounded)), DirectUnboundedErrorPredicate(optimizer)));

		ProjectionStage out(binder, calc);
		Stage<TotalColumns> stage;
		for (idx_t i = 0; i < key_types.size(); i++) {
			stage.columns.key.push_back(out.Add(KeyColumn(i, key)));
		}
		stage.columns.anchors = out.Add(totals[TOTAL_S]);
		stage.columns.down = out.Add(totals[TOTAL_SDN]);
		stage.columns.highest_open_up = out.Add(verdict.highest_open_up);
		stage.columns.lowest_open_down = out.Add(verdict.lowest_open_down);
		stage.columns.over = out.Add(verdict.over);
		stage.columns.under = out.Add(verdict.under);
		stage.columns.ok = out.Add(calc.Wrap(DirectAllHold(calc, std::move(checks))));
		stage.plan = out.Finish(std::move(plan));
		return stage;
	}

	//! The narrow items of stage 3: the group key, the price t and the finite room up plus the finite room down of
	//! every item. With several decisions a row's items are unpivoted here, and only here: one list of the prices and
	//! one of the rooms, one entry per decision, unnested in step (DuckDB's UNNEST of two lists). A projection after
	//! the unnest drops the lists, so the window carries nothing else. No row number or group-back is needed, because
	//! an item of the price is read for its price and room alone.
	struct Narrow {
		unique_ptr<LogicalOperator> plan;
		vector<ColumnBinding> key;
		ColumnBinding price, rooms;
	};
	Narrow NarrowOf(Items items) {
		auto &columns = items.columns;
		vector<ExprPtr> prices, rooms;
		for (auto &decision : columns.decisions) {
			auto item = ItemOf(decision);
			prices.push_back(item.t);
			rooms.push_back(calc.Add(item.up_fin, item.down_fin));
		}
		Narrow narrow;
		ProjectionStage lists(binder, calc);
		vector<ColumnBinding> list_key;
		for (idx_t i = 0; i < key_types.size(); i++) {
			list_key.push_back(lists.Add(KeyColumn(i, columns.keys)));
		}
		if (prices.size() == 1) {
			narrow.key = list_key;
			narrow.price = lists.Add(prices[0]);
			narrow.rooms = lists.Add(rooms[0]);
			narrow.plan = lists.Finish(std::move(items.plan));
			return narrow;
		}
		auto list_type = LogicalType::LIST(LogicalType::DOUBLE);
		auto price_list = lists.Add(calc.Fn("list_value", prices));
		auto room_list = lists.Add(calc.Fn("list_value", rooms));
		auto unnest_index = binder.GenerateTableIndex();
		auto unnest = make_uniq<LogicalUnnest>(unnest_index);
		for (auto list : {price_list, room_list}) {
			auto element = make_uniq<BoundUnnestExpression>(LogicalType::DOUBLE);
			element->child = calc.Out(calc.Col(list_type, list));
			unnest->expressions.push_back(std::move(element));
		}
		unnest->children.push_back(lists.Finish(std::move(items.plan)));
		ProjectionStage out(binder, calc);
		for (idx_t i = 0; i < key_types.size(); i++) {
			narrow.key.push_back(out.Add(KeyColumn(i, list_key)));
		}
		narrow.price = out.Add(DoubleColumn(ColumnBinding(unnest_index, 0)));
		narrow.rooms = out.Add(DoubleColumn(ColumnBinding(unnest_index, 1)));
		narrow.plan = out.Finish(std::move(unnest));
		return narrow;
	}

	//! Stage 3. The price of each group. The narrow items (the key, t and the sum of the two finite rooms) go through
	//! the plan's one window, which gives each item the running sum of the rooms of the items up to it, highest price
	//! first. Joined to the group's totals, each item says whether it is the one that fixes the price (CandidatePrice),
	//! and the group's price is the highest, or the lowest, such candidate.
	//!
	//! Where the infinite rooms lie is read from the group's extreme prices instead of from running counts: an infinite
	//! room up lies at or before an item when the highest price of such a room is at least the item's, and so on. They
	//! agree on the last item of every run of equal prices (for a test that reads from the top) and on the first item
	//! (for one that reads from the bottom), and a tied item that disagrees only repeats the price of its run, so the
	//! price is the same.
	Stage<PriceColumns> Price(Stage<TotalColumns> totals) {
		auto narrow = NarrowOf(ReadItems());
		auto &narrow_key = narrow.key;
		vector<ExprPtr> partitions;
		for (idx_t i = 0; i < key_types.size(); i++) {
			partitions.push_back(KeyColumn(i, narrow_key));
		}
		auto narrow_price = narrow.price;
		auto narrow_rooms = narrow.rooms;
		auto window_index = binder.GenerateTableIndex();
		auto window = make_uniq<LogicalWindow>(window_index);
		// Both reach tests read the finite rooms through one running sum. With U the rooms up of the items so far and V
		// their rooms down, the most the group can spend is S + U - (down - V) = (S - down) + (U + V), so one running
		// sum of both rooms serves where a sum of each would be two columns through the sort.
		window->expressions.push_back(RunningSum(DoubleColumn(narrow_rooms), DoubleColumn(narrow_price), partitions));
		auto run = DoubleColumn(ColumnBinding(window_index, 0));
		window->children.push_back(std::move(narrow.plan));

		auto &group = totals.columns;
		auto joined = JoinOnKey(std::move(window), narrow_key, std::move(totals.plan), group.key);
		Verdict verdict;
		verdict.over = BoolColumn(group.over);
		verdict.under = BoolColumn(group.under);
		auto t = DoubleColumn(narrow_price);
		auto highest_open_up = DoubleColumn(group.highest_open_up);
		auto lowest_open_down = DoubleColumn(group.lowest_open_down);
		auto spend_at_most = calc.Add(calc.Sub(DoubleColumn(group.anchors), DoubleColumn(group.down)), run);
		Reach reach {calc.Cmp(ExpressionType::COMPARE_GREATERTHANOREQUALTO, highest_open_up, t),
		             Less(lowest_open_down, t),
		             calc.Cmp(ExpressionType::COMPARE_LESSTHANOREQUALTO, lowest_open_down, t),
		             Greater(highest_open_up, t),
		             spend_at_most,
		             calc.Sub(spend_at_most, DoubleColumn(narrow_rooms))};
		auto candidate = CandidatePrice(verdict, reach, t, Lower(), Upper());
		ProjectionStage candidates(binder, calc);
		vector<ColumnBinding> candidate_key;
		for (idx_t i = 0; i < key_types.size(); i++) {
			candidate_key.push_back(candidates.Add(KeyColumn(i, narrow_key)));
		}
		auto price_candidate = candidates.Add(candidate);
		auto anchors = candidates.Add(DoubleColumn(group.anchors));
		auto over = candidates.Add(BoolColumn(group.over));
		auto under = candidates.Add(BoolColumn(group.under));
		auto ok = candidates.Add(BoolColumn(group.ok));

		AggregateStage aggregate(binder);
		vector<ColumnBinding> price_key;
		for (idx_t i = 0; i < key_types.size(); i++) {
			price_key.push_back(aggregate.Group(calc.Out(KeyColumn(i, candidate_key))));
		}
		auto highest = aggregate.Add(Aggregate("max", DoubleColumn(price_candidate)));
		auto lowest = aggregate.Add(Aggregate("min", DoubleColumn(price_candidate)));
		auto group_anchors = aggregate.Add(Aggregate("any_value", DoubleColumn(anchors)));
		auto group_over = aggregate.Add(Aggregate("any_value", BoolColumn(over)));
		auto group_under = aggregate.Add(Aggregate("any_value", BoolColumn(under)));
		auto group_ok = aggregate.Add(Aggregate("any_value", BoolColumn(ok)));
		auto reduced = aggregate.Finish(candidates.Finish(std::move(joined)));

		ProjectionStage out(binder, calc);
		Stage<PriceColumns> stage;
		for (idx_t i = 0; i < key_types.size(); i++) {
			stage.columns.key.push_back(out.Add(KeyColumn(i, price_key)));
		}
		stage.columns.price = out.Add(
		    PriceOf(BoolColumn(group_over), BoolColumn(group_under), DoubleColumn(highest), DoubleColumn(lowest)));
		stage.columns.anchors = out.Add(DoubleColumn(group_anchors));
		stage.columns.over = out.Add(BoolColumn(group_over));
		stage.columns.under = out.Add(BoolColumn(group_under));
		stage.columns.ok = out.Add(BoolColumn(group_ok));
		stage.plan = out.Finish(std::move(reduced));
		return stage;
	}

	//! Stage 4. The sums at the price (PriceSumInputs) over the group, then step 6: the items tied at the price share
	//! D = T - (S + A - B), where T is the limit the budget binds at, S the spend of the anchors, A the room up above
	//! the price and B the room down below it. They move up when D is positive and down when it is negative, by F, the
	//! finite room the tie has in that direction. Each finite room moves by its room times min(|D|, F) / F, and the
	//! infinite rooms share |D| - min(|D|, F) evenly. D is zero when the budget does not bind.
	Stage<ShareColumns> Shares(Stage<PriceColumns> price) {
		auto items = ReadItems();
		auto &group = price.columns;
		auto joined = JoinOnKey(std::move(items.plan), items.columns.keys, std::move(price.plan), group.key);
		auto lam = DoubleColumn(group.price);
		auto inputs = PriceSumInputsOfRow(ItemsOf(items.columns), lam);

		AggregateStage aggregate(binder);
		vector<ColumnBinding> key;
		for (idx_t i = 0; i < key_types.size(); i++) {
			key.push_back(aggregate.Group(calc.Out(KeyColumn(i, items.columns.keys))));
		}
		vector<ExprPtr> sums;
		for (idx_t slot = 0; slot < SUM_COUNT; slot++) {
			sums.push_back(DoubleColumn(aggregate.Add(Aggregate("sum", inputs[slot]))));
		}
		auto group_price = aggregate.Add(Aggregate("any_value", lam));
		auto anchors = aggregate.Add(Aggregate("any_value", DoubleColumn(group.anchors)));
		auto over = aggregate.Add(Aggregate("any_value", BoolColumn(group.over)));
		auto under = aggregate.Add(Aggregate("any_value", BoolColumn(group.under)));
		auto ok = aggregate.Add(Aggregate("any_value", BoolColumn(group.ok)));
		auto reduced = aggregate.Finish(std::move(joined));

		auto others = calc.Sub(calc.Add(DoubleColumn(anchors), sums[SUM_ABOVE]), sums[SUM_BELOW]);
		auto left_over = calc.If(BoolColumn(over), calc.Sub(Upper(), others),
		                         calc.If(BoolColumn(under), calc.Sub(Lower(), others), Zero()));
		auto up = calc.Cmp(ExpressionType::COMPARE_GREATERTHANOREQUALTO, left_over, Zero());
		auto distance = calc.If(up, left_over, calc.Neg(left_over));
		auto finite_room = calc.If(up, sums[SUM_UP], sums[SUM_DOWN]);
		auto open_rooms = calc.If(up, sums[SUM_UP_OPEN], sums[SUM_DOWN_OPEN]);
		auto moved = calc.Min({distance, finite_room});

		ProjectionStage out(binder, calc);
		Stage<ShareColumns> stage;
		for (idx_t i = 0; i < key_types.size(); i++) {
			stage.columns.key.push_back(out.Add(KeyColumn(i, key)));
		}
		stage.columns.price = out.Add(DoubleColumn(group_price));
		stage.columns.up = out.Add(up);
		stage.columns.fraction = out.Add(calc.If(Greater(finite_room, Zero()), calc.Div(moved, finite_room), Zero()));
		stage.columns.open_share =
		    out.Add(calc.If(Greater(open_rooms, Zero()), calc.Div(calc.Sub(distance, moved), open_rooms), Zero()));
		stage.columns.ok = out.Add(BoolColumn(ok));
		stage.plan = out.Finish(std::move(reduced));
		return stage;
	}

	//! Stage 5. Every source row joins its group's numbers. An item at the price starts from its anchor and moves by
	//! its share, in the direction of the group's tie (the room is in units of budget, so a move is divided by the
	//! weight).
	DirectSubstitute Answer(Stage<ShareColumns> shares) {
		auto items = ReadItems();
		auto &group = shares.columns;
		auto joined = JoinOnKey(std::move(items.plan), items.columns.keys, std::move(shares.plan), group.key);
		auto sources = items.columns.sources.size();
		auto decisions = items.columns.decisions.size();

		ProjectionStage out(binder, calc);
		for (idx_t i = 0; i < sources; i++) {
			out.Add(calc.Col(reader.SourceTypes()[i], items.columns.sources[i]));
		}
		for (auto &decision : items.columns.decisions) {
			auto item = ItemOf(decision);
			auto up_move = calc.If(item.up_inf, DoubleColumn(group.open_share),
			                       calc.Mul(item.up_fin, DoubleColumn(group.fraction)));
			auto down_move = calc.If(item.down_inf, DoubleColumn(group.open_share),
			                         calc.Mul(item.down_fin, DoubleColumn(group.fraction)));
			auto shared = calc.If(BoolColumn(group.up), calc.Add(item.a, calc.Div(up_move, item.w)),
			                      calc.Sub(item.a, calc.Div(down_move, item.w)));
			out.Add(AnswerOf(item, DoubleColumn(group.price), shared));
		}
		out.Add(calc.Cast(BoolColumn(group.ok), LogicalType::BIGINT));
		DirectSubstitute substitute;
		for (idx_t k = 0; k < decisions; k++) {
			substitute.prunable_decisions.push_back(1);
		}
		for (idx_t i = 0; i < sources + decisions; i++) {
			substitute.slot_map.push_back(i);
		}
		substitute.validated_columns.push_back(sources + decisions);
		// The root a materialized source needs sits above every read.
		substitute.child = reader.Finish(out.Finish(std::move(joined)));
		return substitute;
	}
};

DirectSubstitute Rule::Apply(unique_ptr<LogicalOperator> source, Optimizer &optimizer,
                             const DirectApplicabilityCondition &candidate) const {
	auto &condition = static_cast<const Condition &>(candidate);
	// The strangler: the condition goes to the builder that supports it. Constant limits run on the aggregate-then-join
	// plan, with one decision or several; limits read from the data stay with PlanBuilder.
	if (condition.data_limits.empty()) {
		DirectSourceReader reader(optimizer, std::move(source));
		AggregatePlanBuilder aggregate_builder(condition, optimizer, reader);
		return aggregate_builder.Build();
	}
	PlanBuilder plan_builder(condition, optimizer, *source);
	return plan_builder.Build(std::move(source));
}

} // namespace direct_r1_continuous_knapsack

unique_ptr<DirectSolveRule> MakeR1ContinuousKnapsackRule() {
	return make_uniq<direct_r1_continuous_knapsack::Rule>();
}

} // namespace duckdb
