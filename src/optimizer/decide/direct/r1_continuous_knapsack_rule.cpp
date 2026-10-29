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
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_cross_product.hpp"
#include "duckdb/planner/operator/logical_dummy_scan.hpp"
#include "duckdb/planner/operator/logical_expression_get.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
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
	//! A limit of the budget read from the data: a source column or an expression over source columns, or the
	//! bound-side rewrite's aggregates of the group (DecideGroupBound). The plan takes the group's tightest value of a
	//! `value` (DirectTightestBound) and computes a `group`'s aggregates in its group aggregate, and makes the checks
	//! the rewrite would have made. A NULL skips its instance: the group's MIN or MAX passes over it.
	struct DataLimit {
		ExpressionType comparison = ExpressionType::INVALID;
		//! The limit, one value on every kept row of a group already. Null with a `group`.
		unique_ptr<Expression> value;
		unique_ptr<DecideGroupBound> group;
	};
	DecideSense sense = DecideSense::FEASIBILITY;
	//! Which rows the budget covers, and in which groups: a row whose `when` holds and whose `by` key has no NULL in a
	//! NO_GROUP element (D5) belongs to the budget of its group; any other row is outside every budget. Every clause
	//! of the budget has this scope. Empty without a key: one budget for all the rows it covers.
	vector<DirectKeyElement> group_keys;
	//! The clauses are written `per K:` and a NULL in K puts a row in no class: a query whose kept rows all have one
	//! has no instance (DirectSharedScope::gated).
	bool gated = false;
	vector<DirectKeyElement> per_class_keys;
	unique_ptr<Expression> when;
	bool scoped = false;
	//! Each decision's box before any per-row bound (one per decision), and the per-row clauses the plan evaluates;
	//! a clause names the decision it bounds.
	vector<DirectDecisionBox> boxes;
	vector<DirectBoundClause> clauses;
	//! The budget's weight terms; a decision's weight is `sum(sign * coefficient)` over its terms, and zero when it has
	//! none.
	DirectLinearPart budget;
	//! The budget's first clause as a message names it: a price, a room or a sum the plan computes past a DOUBLE names
	//! it (DecideValueRole::TOO_LARGE), as the solver path names a number it computes.
	string budget_written;
	//! The budget's constant limits: `<=` sets the upper one, `>=` the lower one, `=` both. A side with no limit is
	//! infinite. Several constraints on the same weights are one budget, with the largest lower and smallest upper
	//! limit, counting the data limits below.
	double lower = NEGATIVE_INFINITY;
	double upper = POSITIVE_INFINITY;
	vector<DataLimit> data_limits;
	//! The objective's parts, each a signed, scaled sum of linear terms. None for no objective.
	vector<DirectLinearPart> objective;
	//! For each decision, what a message calls its value, the objective's coefficient of it over all the parts as the
	//! user wrote it. Empty for a decision the objective does not name.
	vector<string> value_text;
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
	if (ir.decisions.empty() || !ir.keyed_scopes.empty()) {
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
		if (!DirectIsSingletonRow(ir, ir_node)) {
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

//! Every clause of the budget covers the same rows: the same group keys in the same order, and the same `when` (written
//! on the clause or on the sum: both read as a membership of the budget). Records the scope the plan applies.
bool Rule::ProveBudgetScope(const DirectIR &ir, const vector<const DirectIRConstraint *> &budgets,
                            Condition &condition) const {
	DirectSharedScope scope;
	if (!DirectProveSharedScope(ir, budgets, scope)) {
		return false;
	}
	condition.scoped = scope.Scoped();
	condition.gated = scope.gated;
	condition.when = std::move(scope.when);
	condition.group_keys = std::move(scope.keys);
	condition.per_class_keys = std::move(scope.classes);
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
//! (AggregatePlanBuilder::GroupTotals).
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
//! may raise (the plan evaluates it on the rows that read it, as the solver does, so both raise on the same inputs), or
//! the aggregates of its group that the bound-side rewrite left to the plan.
bool Rule::ProveDataLimit(const DirectIR &ir, const DirectIRConstraint &ir_node, Condition &condition) const {
	Condition::DataLimit limit;
	limit.comparison = ir_node.comparison;
	if (ir_node.group_bound) {
		if (!DirectGroupBoundIsReadable(ir, *ir_node.group_bound)) {
			return false;
		}
		limit.group = ir_node.group_bound->Copy();
	} else {
		if (!DirectIsSourceOnlyNumericMayRaise(*ir_node.rhs, ir.decide_index, ir.source_bindings)) {
			return false;
		}
		limit.value = ir_node.rhs->Copy();
	}
	condition.data_limits.push_back(std::move(limit));
	return true;
}

bool Rule::ProveBudget(const DirectIR &ir, ClientContext &context, const vector<const DirectIRConstraint *> &budgets,
                       Condition &condition) const {
	if (!ProveBudgetScope(ir, budgets, condition)) {
		return false;
	}
	condition.budget_written = budgets[0]->written;
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
	if (ir.objective.scope.when) {
		return false;
	}
	for (auto &part : ir.objective.parts) {
		if (part.reducer != DirectReducer::SUM || part.filter || part.inner ||
		    part.qualifier != DConstants::INVALID_INDEX || !part.by.empty() ||
		    !OnlyLinearTerms(part.terms, ir.decisions.size())) {
			return false;
		}
		DirectLinearPart read;
		if (!DirectReadLinearPart(ir, context, part, read)) {
			return false;
		}
		condition.objective.push_back(std::move(read));
	}
	condition.value_text.resize(ir.decisions.size());
	for (idx_t decision = 0; decision < ir.decisions.size(); decision++) {
		vector<DirectSignedValue> like;
		for (auto &part : condition.objective) {
			for (auto &term : part.terms) {
				if (term.decision == decision) {
					like.push_back({part.sign * term.sign, term.scaled->Copy()});
				}
			}
		}
		if (!like.empty()) {
			condition.value_text[decision] = DirectCoefficientText(ir, context, like);
		}
	}
	return true;
}

//===--------------------------------------------------------------------===//
// Plan
//===--------------------------------------------------------------------===//

//! The algebra of definition.md § Relational construction over expressions alone, which the two classes below share:
//! ItemStages computes the item values it reads, AggregatePlanBuilder runs the verdicts, the price, the sums at the
//! price and the answer in aggregates over items and joins. Nothing here knows which column an expression reads or
//! which operator evaluates it.
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

//! One decision's item values on one read of the source, as bindings of the plan ItemStages::Build returns. A binding
//! is DOUBLE unless the name says otherwise.
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

//! Where ItemStages::Build leaves the values of one read of the source: the first stage of the aggregate plan, "item
//! values". A row has one item per decision, each in columns of its own.
struct ItemColumns {
	//! The source columns, in source order.
	vector<ColumnBinding> sources;
	//! The group key: whether the row belongs to a budget (BOOLEAN), then the group keys. Empty without a scope.
	vector<ColumnBinding> keys;
	//! The message each check of a raw value or of a limit read from the data raises on the row (VARCHAR, NULL when it
	//! passes), in the order the solver checks them: the NaN checks of what the limits read (`limit_messages` of them),
	//! then the others.
	vector<ColumnBinding> messages;
	idx_t limit_messages = 0;
	//! The message of each item's price past a DOUBLE (VARCHAR, NULL when finite): a number the plan computes from the
	//! checked values. Raised after every check of a value read; the group sums are checked after it (GroupTotals).
	vector<ColumnBinding> computed_messages;
	//! What the aggregates of each limit read from the data read on the row, per data limit and per input.
	vector<vector<DirectGroupInputColumns>> limits;
	//! The items of the row, one per decision.
	vector<DecisionColumns> decisions;
};

//! Stage 1 of the aggregate plan (definition.md § The plan): one projection chain over one read of the source that
//! computes the values of every item, one row of the read per source row. An *item* is one decision on one row. Its box
//! [l, u] has ends of magnitude 1e20 or more read as infinite, as the shared bound reader reads them; w is its weight
//! in the budget (zero for a decision the budget does not name), v its value (negated under MINIMIZE, zero with no
//! objective).
//!
//! An item with w != 0 has a break-even price t = v / w, a high end (u when w > 0, l when w < 0), a low end, an anchor
//! a (0 clamped into the box), and room up and room down in units of budget (w * (high - a) and w * (a - low)). A
//! room of infinity is carried as a flag and a count beside the finite sum of the others, so no sum adds infinity to
//! minus infinity. Items with w = 0, and the items of rows outside every budget, take their best end and do not enter
//! the ranking: their price is -infinity and their weight enters no sum.
//!
//! The stages, each a projection over the columns so far (a row has one item per decision, each decision's values in
//! columns of its own, suffixed with the decision's index):
//!
//!   P0 scope             whether the row belongs to a budget (scoped budgets only)
//!   P1 raw values        every coefficient, bound, mask and limit read from the data, evaluated on every row
//!   P2 bounds, v         each clause's bounds, each decision's value, the check on every raw value and every limit
//!   P3 box               each decision's l and u
//!   P4 anchor, price     whether the box is empty, a, whether w = 0, t
//!   P5 rooms             room up and down, finite and infinite, the spend of the anchors, unbounded items
class ItemStages : public ItemAlgebra {
public:
	//! `proved_bindings` are the bindings the proved expressions of the condition refer to, and `source_types` the
	//! types of the source columns in the same order. The plan reads the source through a fresh set of bindings each
	//! time, and the expressions are re-pointed at it (Build).
	ItemStages(const Condition &condition_p, const vector<unique_ptr<DecideGroupBound>> &limit_bounds_p,
	           Optimizer &optimizer_p, vector<ColumnBinding> proved_bindings_p, vector<LogicalType> source_types_p)
	    : ItemAlgebra(optimizer_p), condition(condition_p), limit_bounds(limit_bounds_p), binder(optimizer_p.binder),
	      proved_bindings(std::move(proved_bindings_p)), source_types(std::move(source_types_p)) {
		// The source reader hands the input over with its types resolved.
		D_ASSERT(proved_bindings.size() == source_types.size());
	}

	//! P0 to P5 over one read of the source. `read` is a projection that forwards the source columns
	//! (DirectSourceReader::Read) and `read_table_index` its table index; the proved expressions are re-pointed at it.
	unique_ptr<LogicalOperator> Build(unique_ptr<LogicalOperator> read, idx_t read_table_index) {
		read_index = read_table_index;
		auto when = condition.when ? OverState(*condition.when) : nullptr;
		// The key is read on the rows of the read projection.
		vector<DirectKeyElement> keys;
		for (auto &key : condition.group_keys) {
			DirectKeyElement element;
			element.grouping = OverState(*key.grouping);
			element.no_group = key.no_group;
			keys.push_back(std::move(element));
		}
		unique_ptr<Expression> instance;
		if (condition.gated) {
			vector<DirectKeyElement> classes;
			for (auto &key : condition.per_class_keys) {
				DirectKeyElement element;
				element.grouping = OverState(*key.grouping);
				element.no_group = key.no_group;
				classes.push_back(std::move(element));
			}
			ColumnBinding flag;
			read = DirectAddInstanceFlag(optimizer, std::move(read), when.get(), classes, flag);
			instance = calc.Out(calc.Col(LogicalType::BOOLEAN, flag));
		}
		auto state =
		    DirectProjectScope(binder, std::move(read), condition.scoped, when.get(), keys, {}, std::move(instance));
		state_index = state.state_index;
		plan = std::move(state.plan);
		for (idx_t i = 0; i < proved_bindings.size(); i++) {
			Declare(SourceName(i), state.input_bindings[i], source_types[i]);
		}
		if (condition.scoped) {
			Declare(ELIGIBLE, state.eligible, LogicalType::BOOLEAN);
			for (idx_t i = 0; i < state.keys.size(); i++) {
				Declare(KeyName(i), state.keys[i], state.key_types[i]);
			}
		}
		auto checks = LayoutRawValues();
		AddBoundsAndValue(std::move(checks));
		AddBox();
		AddAnchorAndPrice();
		AddRooms();
		return std::move(plan);
	}

	//! Where Build put the columns the aggregate plan reads.
	ItemColumns Columns() const {
		ItemColumns items;
		for (idx_t i = 0; i < proved_bindings.size(); i++) {
			items.sources.push_back(columns.at(SourceName(i)).binding);
		}
		if (condition.scoped) {
			items.keys.push_back(columns.at(ELIGIBLE).binding);
			for (idx_t i = 0; i < condition.group_keys.size(); i++) {
				items.keys.push_back(columns.at(KeyName(i)).binding);
			}
		}
		for (auto &name : message_names) {
			items.messages.push_back(columns.at(name).binding);
		}
		for (auto &name : computed_message_names) {
			items.computed_messages.push_back(columns.at(name).binding);
		}
		items.limit_messages = limit_message_count;
		for (idx_t j = 0; j < limit_bounds.size(); j++) {
			items.limits.emplace_back();
			for (idx_t i = 0; i < limit_bounds[j]->inputs.size(); i++) {
				auto layout = DirectGroupLayout(limit_bounds[j]->inputs[i], condition.scoped);
				DirectGroupInputColumns inputs;
				if (layout.reads) {
					inputs.reads = columns.at(LimitReadsName(j, i)).binding;
				}
				if (layout.argument) {
					inputs.argument = columns.at(LimitArgumentName(j, i)).binding;
				}
				items.limits.back().push_back(inputs);
			}
		}
		for (idx_t k = 0; k < DecisionCount(); k++) {
			auto binding = [&](const char *name) {
				return columns.at(Dec(name, k)).binding;
			};
			DecisionColumns decision;
			decision.v = binding("v");
			decision.w = binding("w");
			decision.l = binding("l");
			decision.u = binding("u");
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
	using Named = vector<std::pair<string, ExprPtr>>;

	const Condition &condition;
	//! The group bound of each data limit of the condition (AggregatePlanBuilder::LimitBounds).
	const vector<unique_ptr<DecideGroupBound>> &limit_bounds;
	Binder &binder;
	//! The bindings the proved expressions refer to, and the types of the source columns.
	vector<ColumnBinding> proved_bindings;
	vector<LogicalType> source_types;
	//! The scope projection's table index, or INVALID_INDEX when the input passed through it unchanged.
	idx_t state_index = DConstants::INVALID_INDEX;
	//! The table index of the projection that carries the source (a read of DirectSourceReader).
	idx_t read_index = DConstants::INVALID_INDEX;

	//! The plan so far and the columns it carries, in output order. Every stage forwards every column, and unused
	//! ones are removed with the rest of the unused columns after the DECIDE optimizer has run.
	unique_ptr<LogicalOperator> plan;
	vector<string> names;
	std::map<string, Column> columns;
	//! The columns of the checks' messages, and how many are the NaN checks of what the limits read.
	vector<string> message_names;
	idx_t limit_message_count = 0;
	//! The columns of the messages of the numbers the plan computes (ItemColumns::computed_messages).
	vector<string> computed_message_names;

	//! Adds, beside `value`, the message of it past a DOUBLE: TOO_LARGE, naming the budget. NULL passes.
	void AddComputed(Named &added, const string &name, const ExprPtr &value) {
		computed_message_names.push_back("too_large_" + name);
		added.emplace_back(computed_message_names.back(),
		                   DirectNumberMessage(calc, value, condition.budget_written, DecideValueRole::TOO_LARGE));
	}

	static string SourceName(idx_t i) {
		return "s" + std::to_string(i);
	}
	//! The name of the column that holds element `i` of the group key.
	static string KeyName(idx_t i) {
		return "key_" + std::to_string(i);
	}

	idx_t DecisionCount() const {
		return condition.boxes.size();
	}
	//! The name of a per-decision column.
	static string Dec(const string &base, idx_t decision) {
		return base + "_" + std::to_string(decision);
	}
	//! The names of the columns of input `i` of data limit `j`: whether the row is one its aggregates read, its
	//! argument there, and its NaN message.
	static string LimitReadsName(idx_t j, idx_t i) {
		return "limit_reads_" + std::to_string(j) + "_" + std::to_string(i);
	}
	static string LimitArgumentName(idx_t j, idx_t i) {
		return "limit_argument_" + std::to_string(j) + "_" + std::to_string(i);
	}
	static string LimitMessageName(idx_t j, idx_t i) {
		return "limit_message_" + std::to_string(j) + "_" + std::to_string(i);
	}

	//! The column that says whether a row belongs to a budget; only a scoped budget has one.
	static constexpr const char *ELIGIBLE = "elig";

	//! A copy of a proved expression, re-pointed at the scope projection when there is one, else at the read.
	unique_ptr<Expression> OverState(const Expression &expr) const {
		auto carrier = state_index != DConstants::INVALID_INDEX ? state_index : read_index;
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

	//! One projection that forwards every column and appends the new ones, in order. Returns its table index.
	idx_t Project(const Named &added) {
		auto index = binder.GenerateTableIndex();
		vector<unique_ptr<Expression>> expressions;
		vector<string> next_names;
		std::map<string, Column> next_columns;
		for (auto &name : names) {
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
	// P1 and P2: raw values, limits from the data, per-clause bounds, the objective value
	//===------------------------------------------------------------------===//

	//! What the aggregates of each limit read from the data read, on the rows of the budget alone (NULL on the others),
	//! and the message each raises. Stage 2 computes the aggregates over the group.
	void AddLimitValues(Named &added) const {
		auto eligible = condition.scoped ? C(ELIGIBLE) : nullptr;
		auto over_state = [&](const Expression &expression) {
			return OverState(expression);
		};
		for (idx_t j = 0; j < limit_bounds.size(); j++) {
			auto inputs = DirectGroupInputs(calc, *limit_bounds[j], eligible, over_state);
			for (idx_t i = 0; i < inputs.size(); i++) {
				if (inputs[i].reads) {
					added.emplace_back(LimitReadsName(j, i), inputs[i].reads);
				}
				if (inputs[i].argument) {
					added.emplace_back(LimitArgumentName(j, i), inputs[i].argument);
				}
				if (inputs[i].invalid) {
					added.emplace_back(LimitMessageName(j, i), inputs[i].invalid);
				}
			}
		}
	}

	//! The check, read only on the rows of the budget when the budget has a scope.
	unique_ptr<Expression> ReadOnBudgetRows(unique_ptr<Expression> check) const {
		if (!condition.scoped) {
			return check;
		}
		return DirectCheckOnKeptRows(calc, calc.Out(C(ELIGIBLE)), std::move(check));
	}

	//! P1: every raw value, computed on the rows that read it: a row a `when` drops reads nothing, as on the solver
	//! path. A clause's values are NULL on the rows its `when` drops, where its bounds are not read; a row outside the
	//! budget has the weight 0, as an item outside every budget. Returns the check on each, built over P1, which raises
	//! only where the value is read. A decision the budget does not name has the weight 0, which needs no check; an
	//! objective part has a raw value for each decision it names. A limit's group aggregates are stage 2's.
	vector<ExprPtr> LayoutRawValues() {
		Named added;
		AddLimitValues(added);
		for (idx_t j = 0; j < condition.clauses.size(); j++) {
			auto &clause = condition.clauses[j];
			auto on_kept = [&](ExprPtr value) {
				return clause.when ? DirectOnKeptRows(
				                         calc, calc.Wrap(DirectActiveWhen(calc, OverState(*clause.when).get())), value)
				                   : value;
			};
			added.emplace_back("c" + std::to_string(j), on_kept(calc.Wrap(SumOf(clause.coefficient))));
			added.emplace_back("r" + std::to_string(j),
			                   on_kept(calc.Cast(calc.Of(*OverState(*clause.rhs)), LogicalType::DOUBLE)));
			added.emplace_back("lo_raw" + std::to_string(j), on_kept(calc.Of(*OverState(*clause.lower))));
			added.emplace_back("up_raw" + std::to_string(j), on_kept(calc.Of(*OverState(*clause.upper))));
			if (clause.when) {
				added.emplace_back("wh" + std::to_string(j),
				                   calc.Wrap(DirectActiveWhen(calc, OverState(*clause.when).get())));
			}
		}
		for (idx_t k = 0; k < DecisionCount(); k++) {
			auto weights = SignedValues(condition.budget, k);
			auto weight = weights.empty() ? Zero() : calc.Wrap(SumOf(weights));
			added.emplace_back(Dec("w", k),
			                   condition.scoped && !weights.empty() ? calc.If(C(ELIGIBLE), weight, Zero()) : weight);
			for (idx_t p = 0; p < condition.objective.size(); p++) {
				auto values = SignedValues(condition.objective[p], k);
				if (!values.empty()) {
					added.emplace_back(Dec("o" + std::to_string(p), k), calc.Wrap(SumOf(values)));
				}
			}
		}
		auto p1 = Project(added);

		vector<ExprPtr> messages;
		auto number = [&](const string &name, const string &written,
		                  DecideValueRole role = DecideValueRole::COEFFICIENT) {
			return DirectNumberMessage(calc, C(name), written, role);
		};
		// The bound-side rewrite's checks of what a limit reads come before any other, as its scope checks did.
		for (idx_t j = 0; j < limit_bounds.size(); j++) {
			for (idx_t i = 0; i < limit_bounds[j]->inputs.size(); i++) {
				if (limit_bounds[j]->inputs[i].invalid) {
					messages.push_back(C(LimitMessageName(j, i)));
				}
			}
		}
		limit_message_count = messages.size();
		for (idx_t j = 0; j < condition.clauses.size(); j++) {
			auto &clause = condition.clauses[j];
			auto suffix = std::to_string(j);
			auto read_on = [&](ExprPtr message) {
				return clause.when ? DirectOnKeptRows(calc, C("wh" + suffix), message) : message;
			};
			messages.push_back(read_on(number("c" + suffix, clause.coefficient_text, clause.coefficient_role)));
			messages.push_back(read_on(
			    calc.If(calc.Fn("isnan", C("r" + suffix)),
			            calc.Constant(Value(DecideInvalidValueMessage(DecideValueRole::BOUND, clause.rhs_text))),
			            calc.Constant(Value(LogicalType::VARCHAR)))));
		}
		for (idx_t k = 0; k < DecisionCount(); k++) {
			auto weights = SignedValues(condition.budget, k);
			if (!weights.empty()) {
				auto message = number(Dec("w", k), condition.budget.text[k]);
				messages.push_back(condition.scoped ? DirectOnKeptRows(calc, C(ELIGIBLE), message) : message);
			}
			for (idx_t p = 0; p < condition.objective.size(); p++) {
				auto values = SignedValues(condition.objective[p], k);
				if (!values.empty()) {
					messages.push_back(number(Dec("o" + std::to_string(p), k), condition.objective[p].text[k]));
				}
			}
		}
		return messages;
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

	//! P2: the message of every check of a raw value, each clause's bounds, and each decision's v.
	void AddBoundsAndValue(vector<ExprPtr> messages) {
		Named added;
		for (idx_t i = 0; i < messages.size(); i++) {
			message_names.push_back("message_" + std::to_string(i));
			added.emplace_back(message_names.back(), messages[i]);
		}
		for (idx_t j = 0; j < condition.clauses.size(); j++) {
			auto suffix = std::to_string(j);
			DirectBoundColumns raw;
			raw.lower = columns.at("lo_raw" + suffix).binding;
			raw.upper = columns.at("up_raw" + suffix).binding;
			if (condition.clauses[j].when) {
				raw.has_when = true;
				raw.when = columns.at("wh" + suffix).binding;
			}
			ExprPtr lower, upper;
			DirectClauseBounds(calc, raw, lower, upper);
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
			vector<ColumnBinding> clause_lowers, clause_uppers, lowering_whens;
			for (idx_t j = 0; j < condition.clauses.size(); j++) {
				if (condition.clauses[j].decision == k) {
					clause_lowers.push_back(columns.at("lo" + std::to_string(j)).binding);
					clause_uppers.push_back(columns.at("up" + std::to_string(j)).binding);
					if (condition.clauses[j].lifts_floor) {
						lowering_whens.push_back(columns.at("wh" + std::to_string(j)).binding);
					}
				}
			}
			ExprPtr lower, upper;
			DirectRowBox(calc, DirectStaticFloor(calc, condition.boxes[k].lower, lowering_whens),
			             condition.boxes[k].upper, clause_lowers, clause_uppers, lower, upper);
			added.emplace_back(Dec("l", k), lower);
			added.emplace_back(Dec("u", k), upper);
		}
		Project(added);
	}

	//! P4: whether the box is empty, the anchor (0 clamped into the box; 0 on an empty box, where it is never read),
	//! whether the item is outside the ranking (w = 0, or its row belongs to no budget), and its break-even price.
	void AddAnchorAndPrice() {
		Named added;
		for (idx_t k = 0; k < DecisionCount(); k++) {
			auto l = C(Dec("l", k)), u = C(Dec("u", k)), w = C(Dec("w", k)), v = C(Dec("v", k));
			auto empty = DirectBoxIsEmpty(calc, l, u);
			auto anchor = calc.If(empty, Zero(), calc.Min({calc.Max({Zero(), l}), u}));
			auto free_of_budget = Equal(w, Zero());
			if (condition.scoped) {
				free_of_budget = calc.Any({free_of_budget, Not(C(ELIGIBLE))});
			}
			// Adding 0.0 turns -0.0 into 0.0, so prices that are equal sort and compare as equal.
			auto price = calc.If(free_of_budget, NegativeInfinity(), calc.Add(calc.Div(v, w), Zero()));
			// The -infinity of an item outside the ranking is a mark, not a number computed.
			AddComputed(added, Dec("t", k), calc.If(free_of_budget, NullDouble(), price));
			added.emplace_back(Dec("empty", k), empty);
			added.emplace_back(Dec("a", k), anchor);
			added.emplace_back(Dec("wz", k), free_of_budget);
			added.emplace_back(Dec("pos", k), Greater(w, Zero()));
			added.emplace_back(Dec("t", k), price);
		}
		Project(added);
	}

	//! P5: each item's room up and room down, in units of budget. A finite room goes in `up_fin` and `down_fin`; an
	//! infinite one is a flag, counted apart. The spend of the anchors, the best end of an item outside the ranking,
	//! and whether that item is unbounded.
	void AddRooms() {
		Named added;
		for (idx_t k = 0; k < DecisionCount(); k++) {
			auto w = C(Dec("w", k)), v = C(Dec("v", k)), l = C(Dec("l", k)), u = C(Dec("u", k)), a = C(Dec("a", k));
			auto empty = C(Dec("empty", k)), wz = C(Dec("wz", k)), pos = C(Dec("pos", k));
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
			added.emplace_back(Dec("up_inf", k), up_open);
			added.emplace_back(Dec("down_inf", k), down_open);
			added.emplace_back(Dec("up_fin", k), up_room);
			added.emplace_back(Dec("down_fin", k), down_room);
			added.emplace_back(Dec("sterm", k), calc.Mul(w, a));
			added.emplace_back(Dec("best", k), best_end);
			added.emplace_back(Dec("u1", k), Flag(And(And(wz, Not(empty)), unbounded_end)));
			added.emplace_back(Dec("upi", k), Flag(up_open));
			added.emplace_back(Dec("dni", k), Flag(down_open));
		}
		Project(added);
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

//! Builds the plan of definition.md § The plan for one or several decisions, with limits that are constants or read
//! from the data: aggregate-then-join. Each group is reduced to one row of numbers (its totals, its price, its sums at
//! the price) and every source row joins its group's row and computes its answer in one projection. A group key is
//! whether the row belongs to a budget, then its group keys; keys are compared with IS NOT DISTINCT FROM, so a NULL key
//! matches. Without a scope there is one group: the aggregates have no GROUP BY and the join is a cross product with
//! that one row.
//!
//! Each stage reads the source once more (DirectSourceReader) through ItemStages::Build, which computes the
//! values of every item on that read, and keeps what it needs of them in its own struct of bindings. A row has one
//! item per decision, each in columns of its own; no stage unpivots the row except the price, which crosses the narrow
//! prices and rooms with the decision numbers (Narrow):
//!
//!   1. item values      ItemColumns   a projection over a read of the source; per decision, DecisionColumns
//!   2. group totals     TotalColumns  a hash aggregate per group over stage 1 (a row adds the sum of its items, and
//!                                     the MIN or MAX of each limit read from the data), then a projection: the
//!                                     group's limits L and U, the verdicts, as checks that raise, and the target
//!                                     spend's side. Every row's NULL and NaN checks, limits included, are evaluated
//!                                     inside the aggregate, so they raise before a verdict does.
//!   3. price            PriceColumns  stage 1's few columns through the plan's one window (partitioned by group,
//!                                     ordered by price, highest first), joined to stage 2, then a hash aggregate;
//!                                     L and U ride along as group columns
//!   4. shares           ShareColumns  a hash aggregate per group over stage 1 joined to stage 3: the sums at the
//!   price,
//!                                     then the shares of the tied items (L and U are read here)
//!   5. answer           a join of stage 1 with stage 4 and one projection
//!
//! Every row is checked before any is released: stage 5's rows come out of a join with stage 4, whose rows exist only
//! once stages 2 to 4 have read every row. The verdict column rides from stage 2 to the output, so a parent that reads
//! no answer still raises.
class AggregatePlanBuilder : public ItemAlgebra {
public:
	AggregatePlanBuilder(const Condition &condition_p, Optimizer &optimizer_p, DirectSourceReader &reader_p)
	    : ItemAlgebra(optimizer_p), condition(condition_p), binder(optimizer_p.binder), reader(reader_p) {
		// A limit the rewrite left as a group bound is one already; any other is one value per group, whose group
		// extremum is the limit.
		for (auto &limit : condition.data_limits) {
			limit_bounds.push_back(
			    limit.group ? limit.group->Copy()
			                : DirectTightestBound(optimizer, limit.comparison, limit.value->Copy(), string(), nullptr));
		}
		if (condition.scoped) {
			key_types.push_back(LogicalType::BOOLEAN);
			for (auto &key : condition.group_keys) {
				key_types.push_back(key.grouping->return_type);
			}
		}
	}

	//! How many times the plan reads the source: stages 2 to 4 and the answer of stage 5 each read it once.
	static constexpr idx_t SOURCE_READS = 4;

	DirectSubstitute Build() {
		auto totals = GroupTotals();
		auto price = Price(std::move(totals));
		auto shares = Shares(std::move(price));
		auto answer = Answer(std::move(shares));
		D_ASSERT(reads_made == SOURCE_READS);
		return answer;
	}

private:
	//! Stage 1 as one read of the source builds it.
	struct Items {
		unique_ptr<LogicalOperator> plan;
		ItemColumns columns;
	};
	//! Stage 2: the group's key, what stage 3 reads of its totals (the spend of the anchors, the finite room down, the
	//! highest price of an infinite room up and the lowest of one down), which limit the budget binds at, and the check
	//! that raises a verdict. `limits` holds the group's L and U when a limit is read from the data, and none when both
	//! are constants.
	struct TotalColumns {
		vector<ColumnBinding> key;
		ColumnBinding anchors, down, highest_open_up, lowest_open_down, over, under, ok;
		vector<ColumnBinding> limits;
	};
	//! Stage 3: the group's key, its price, and what stage 4 needs of stage 2.
	struct PriceColumns {
		vector<ColumnBinding> key;
		ColumnBinding price, anchors, over, under, ok;
		vector<ColumnBinding> limits;
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
	//! The group bound of each data limit of the condition.
	vector<unique_ptr<DecideGroupBound>> limit_bounds;
	Binder &binder;
	DirectSourceReader &reader;
	idx_t reads_made = 0;
	//! The types of the group key: BOOLEAN, then the group keys. Empty without a scope.
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
	bool HasDataLimits() const {
		return !condition.data_limits.empty();
	}
	//! The budget's limits L and U in a group: the constants, or with a limit read from the data the group columns
	//! stage 2 computes.
	ExprPtr LowerOf(const vector<ColumnBinding> &limits) const {
		return limits.empty() ? calc.Num(condition.lower) : DoubleColumn(limits[0]);
	}
	ExprPtr UpperOf(const vector<ColumnBinding> &limits) const {
		return limits.empty() ? calc.Num(condition.upper) : DoubleColumn(limits[1]);
	}

	//! A new read of the source, as stage 1 computes it.
	Items ReadItems() {
		reads_made++;
		auto read = reader.ReadSource();
		ItemStages stages(condition, limit_bounds, optimizer, reader.SourceBindings(), reader.SourceTypes());
		Items items;
		items.plan = stages.Build(std::move(read.plan), read.table_index);
		items.columns = stages.Columns();
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
	//! the group's one row when there is no key. With a key the join is a SINGLE join, the form DuckDB uses to give
	//! each row of a scalar subquery its one matching row: the group key is unique in the group plan and every item's
	//! group is in it. A SINGLE join has no inverse, so BuildProbeSideOptimizer keeps the group plan the build side,
	//! and JOIN_ORDER estimates it as its item child, whereas an inner join is estimated from the aggregates' child/2
	//! cardinalities as far larger than either side and gets flipped to build on the items.
	unique_ptr<LogicalOperator> JoinOnKey(unique_ptr<LogicalOperator> items, const vector<ColumnBinding> &item_key,
	                                      unique_ptr<LogicalOperator> groups, const vector<ColumnBinding> &group_key) {
		if (key_types.empty()) {
			return LogicalCrossProduct::Create(std::move(items), std::move(groups));
		}
		auto join = make_uniq<LogicalComparisonJoin>(JoinType::SINGLE);
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

	//! Takes the aggregates of the limits' group bounds into stage 2's hash aggregate.
	class AggregateSink final : public DirectGroupSink {
	public:
		explicit AggregateSink(AggregateStage &aggregate_p) : aggregate(aggregate_p) {
		}
		ColumnBinding Add(const Expression &function, unique_ptr<Expression> argument,
		                  unique_ptr<Expression> reads) override {
			return aggregate.Add(DirectAggregateOf(function, std::move(argument), std::move(reads)));
		}

	private:
		AggregateStage &aggregate;
	};

	//! One side of the budget in a group: the constant limit and the limit each data limit gives on that side (the
	//! group's MIN of an upper limit, its MAX of a lower one, an equality both). A value is the number it is, however
	//! large (the solver loads a right side of 1e25 as that number), and an infinite one is infinite: an upper limit of
	//! +infinity is no bound, a lower limit of +infinity cannot be met. A group with no value (the aggregate over no
	//! row) takes the constant alone, as LEAST and GREATEST skip a NULL.
	ExprPtr LimitOf(bool upper, const vector<DirectGroupValues> &limits) const {
		vector<ExprPtr> sides {calc.Num(upper ? condition.upper : condition.lower)};
		for (auto &limit : limits) {
			if (auto &value = upper ? limit.upper : limit.lower) {
				sides.push_back(calc.Cast(value, LogicalType::DOUBLE));
			}
		}
		return upper ? calc.Min(sides) : calc.Max(sides);
	}

	//! Stage 2. The aggregate sums stage 1's values over the group (the sums of TotalInputs, the number of rows, the
	//! MIN and MAX of the limits read from the data, and the check of every row's raw values and v: a NULL or NaN
	//! raises here). The projection after it gives the group's limits L and U, and tests the verdicts: the data
	//! checks, then infeasible, then unbounded; a source with no row
	//! has no verdict.
	Stage<TotalColumns> GroupTotals() {
		auto items = ReadItems();
		auto inputs = TotalInputsOfRow(ItemsOf(items.columns));
		AggregateStage aggregate(binder);
		vector<ColumnBinding> key;
		for (idx_t i = 0; i < key_types.size(); i++) {
			key.push_back(aggregate.Group(calc.Out(KeyColumn(i, items.columns.keys))));
		}
		auto rows = aggregate.Add(optimizer.BindAggregateFunction("count_star", vector<unique_ptr<Expression>>()));
		// The least message of each check in the group; the group raises them in the solver's order below.
		vector<ColumnBinding> messages;
		for (auto message : items.columns.messages) {
			messages.push_back(aggregate.Add(Aggregate("min", calc.Col(LogicalType::VARCHAR, message))));
		}
		vector<ColumnBinding> value_messages;
		for (idx_t k = 0; k < items.columns.decisions.size(); k++) {
			value_messages.push_back(
			    aggregate.Add(Aggregate("min", DirectNumberMessage(calc, DoubleColumn(items.columns.decisions[k].v),
			                                                       condition.value_text[k]))));
		}
		for (auto message : items.columns.computed_messages) {
			value_messages.push_back(aggregate.Add(Aggregate("min", calc.Col(LogicalType::VARCHAR, message))));
		}
		vector<ColumnBinding> sums;
		for (idx_t slot = 0; slot < TOTAL_COUNT; slot++) {
			sums.push_back(aggregate.Add(Aggregate(TotalFunction(slot), inputs[slot])));
		}
		AggregateSink sink(aggregate);
		auto limit_eligible = condition.scoped ? BoolColumn(key[0]) : nullptr;
		vector<DirectGroupValues> limits;
		for (idx_t j = 0; j < limit_bounds.size(); j++) {
			limits.push_back(
			    DirectAddGroupBound(calc, *limit_bounds[j], items.columns.limits[j], sink, limit_eligible));
		}
		auto plan = aggregate.Finish(std::move(items.plan));

		vector<ExprPtr> totals;
		for (auto &sum : sums) {
			totals.push_back(DoubleColumn(sum));
		}
		auto lower = LimitOf(false, limits), upper = LimitOf(true, limits);
		auto verdict = VerdictOf(totals, lower, upper, condition.scoped ? BoolColumn(key[0]) : nullptr);
		auto has_rows = calc.Cmp(ExpressionType::COMPARE_GREATERTHAN, calc.Col(LogicalType::BIGINT, rows),
		                         calc.Constant(Value::BIGINT(0)));
		// The solver's order: the NaN checks of what the limits read, what the limits give (a NaN limit, an `=` limit
		// that takes two values in a group), the row messages, then infeasible and unbounded.
		vector<unique_ptr<Expression>> checks;
		auto raise = [&](ColumnBinding message) {
			checks.push_back(DirectRaiseMessage(optimizer, calc, calc.Col(LogicalType::VARCHAR, message)));
		};
		for (idx_t i = 0; i < items.columns.limit_messages; i++) {
			raise(messages[i]);
		}
		for (auto &limit : limits) {
			for (auto &message : limit.messages) {
				checks.push_back(DirectRaiseMessage(optimizer, calc, message));
			}
		}
		for (idx_t i = items.columns.limit_messages; i < messages.size(); i++) {
			raise(messages[i]);
		}
		for (auto message : value_messages) {
			raise(message);
		}
		// The group's sums of the finite parts: the spend of the anchors and the finite rooms. An item's own part past
		// a DOUBLE makes its sum so. The sums of the rooms at price zero are parts of these, and the running sums of
		// stage 3 parts of the rooms.
		for (auto slot : {TOTAL_S, TOTAL_SUP, TOTAL_SDN}) {
			checks.push_back(DirectRaiseMessage(
			    optimizer, calc,
			    DirectNumberMessage(calc, totals[slot], condition.budget_written, DecideValueRole::TOO_LARGE)));
		}
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
		if (HasDataLimits()) {
			stage.columns.limits.push_back(out.Add(lower));
			stage.columns.limits.push_back(out.Add(upper));
		}
		stage.plan = out.Finish(std::move(plan));
		return stage;
	}

	//! The narrow items of stage 3: the group key, the price t and the finite room up plus the finite room down of
	//! every item. With several decisions a row's items are unpivoted here, and only here: the row, with each
	//! decision's price and room in columns of their own, is crossed with a constant relation of the decision numbers
	//! (the unpivot of a fixed list of columns; DuckDB's UNNEST of two lists costs far more per item), and a
	//! projection after the cross product picks the price and room of the decision the number names, so the window
	//! carries nothing else. No row number or group-back is needed, because an item of the price is read for its
	//! price and room alone.
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
		ProjectionStage narrowed(binder, calc);
		vector<ColumnBinding> item_key;
		for (idx_t i = 0; i < key_types.size(); i++) {
			item_key.push_back(narrowed.Add(KeyColumn(i, columns.keys)));
		}
		if (prices.size() == 1) {
			narrow.key = item_key;
			narrow.price = narrowed.Add(prices[0]);
			narrow.rooms = narrowed.Add(rooms[0]);
			narrow.plan = narrowed.Finish(std::move(items.plan));
			return narrow;
		}
		// One item per decision: the row's plan is crossed with a constant relation that numbers the decisions, and
		// each item takes the price and room of the decision its number names.
		vector<ColumnBinding> price_columns, room_columns;
		vector<vector<unique_ptr<Expression>>> numbers;
		for (idx_t k = 0; k < prices.size(); k++) {
			price_columns.push_back(narrowed.Add(prices[k]));
			room_columns.push_back(narrowed.Add(rooms[k]));
			vector<unique_ptr<Expression>> row;
			row.push_back(calc.Out(calc.Constant(Value::BIGINT(NumericCast<int64_t>(k)))));
			numbers.push_back(std::move(row));
		}
		auto numbers_index = binder.GenerateTableIndex();
		auto numbers_get = make_uniq<LogicalExpressionGet>(numbers_index, vector<LogicalType> {LogicalType::BIGINT},
		                                                   std::move(numbers));
		numbers_get->children.push_back(make_uniq<LogicalDummyScan>(binder.GenerateTableIndex()));
		auto crossed = LogicalCrossProduct::Create(narrowed.Finish(std::move(items.plan)), std::move(numbers_get));
		auto number = calc.Col(LogicalType::BIGINT, ColumnBinding(numbers_index, 0));
		auto pick = [&](const vector<ColumnBinding> &values) {
			auto chosen = DoubleColumn(values.back());
			for (idx_t k = values.size() - 1; k > 0; k--) {
				auto named = calc.Cmp(ExpressionType::COMPARE_EQUAL, number,
				                      calc.Constant(Value::BIGINT(NumericCast<int64_t>(k - 1))));
				chosen = calc.If(named, DoubleColumn(values[k - 1]), chosen);
			}
			return chosen;
		};
		ProjectionStage out(binder, calc);
		for (idx_t i = 0; i < key_types.size(); i++) {
			narrow.key.push_back(out.Add(KeyColumn(i, item_key)));
		}
		narrow.price = out.Add(pick(price_columns));
		narrow.rooms = out.Add(pick(room_columns));
		narrow.plan = out.Finish(std::move(crossed));
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
		auto candidate = CandidatePrice(verdict, reach, t, LowerOf(group.limits), UpperOf(group.limits));
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
		vector<ColumnBinding> candidate_limits;
		for (auto &limit : group.limits) {
			candidate_limits.push_back(candidates.Add(DoubleColumn(limit)));
		}

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
		vector<ColumnBinding> group_limits;
		for (auto &limit : candidate_limits) {
			group_limits.push_back(aggregate.Add(Aggregate("any_value", DoubleColumn(limit))));
		}
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
		for (auto &limit : group_limits) {
			stage.columns.limits.push_back(out.Add(DoubleColumn(limit)));
		}
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
		vector<ColumnBinding> limits;
		for (auto &limit : group.limits) {
			limits.push_back(aggregate.Add(Aggregate("any_value", DoubleColumn(limit))));
		}
		auto reduced = aggregate.Finish(std::move(joined));

		auto others = calc.Sub(calc.Add(DoubleColumn(anchors), sums[SUM_ABOVE]), sums[SUM_BELOW]);
		auto left_over = calc.If(BoolColumn(over), calc.Sub(UpperOf(limits), others),
		                         calc.If(BoolColumn(under), calc.Sub(LowerOf(limits), others), Zero()));
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
	DirectSourceReader reader(optimizer, std::move(source), AggregatePlanBuilder::SOURCE_READS);
	AggregatePlanBuilder builder(condition, optimizer, reader);
	return builder.Build();
}

} // namespace direct_r1_continuous_knapsack

unique_ptr<DirectSolveRule> MakeR1ContinuousKnapsackRule() {
	return make_uniq<direct_r1_continuous_knapsack::Rule>();
}

} // namespace duckdb
