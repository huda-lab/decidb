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
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/decide/decide_term_split.hpp"
#include "duckdb/planner/expression/bound_unnest_expression.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
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

//! Builds the relational plan of one R1 problem whose condition holds (definition.md § Relational construction). A
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
class PlanBuilder {
public:
	PlanBuilder(const Condition &condition_p, Optimizer &optimizer_p, LogicalOperator &source)
	    : condition(condition_p), optimizer(optimizer_p), binder(optimizer_p.binder), calc(optimizer_p.context) {
		// The rule driver hands the input over with its types resolved.
		source_bindings = source.GetColumnBindings();
		source_types = source.types;
		D_ASSERT(source_bindings.size() == source_types.size());
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
		for (idx_t i = 0; i < source_bindings.size(); i++) {
			Declare(SourceName(i), state.input_bindings[i], source_types[i]);
		}
		if (condition.scoped) {
			Declare(ELIGIBLE, state.eligible, LogicalType::BOOLEAN);
		}
		ValidateDataLimits(state);
		auto checks = LayoutRawValues();
		AddBoundsAndValue(std::move(checks));
		AddBox();
		if (Several()) {
			AddItems();
		}
		AddAnchorAndPrice();
		AddRooms();
		AddGroupTotals();
		AddRunningSums();
		AddVerdicts();
		AddPrice();
		AddPriceSums();
		AddAnswer();
		return Output(Release());
	}

private:
	struct Column {
		ColumnBinding binding;
		LogicalType type;
	};
	using Named = vector<std::pair<string, ExprPtr>>;

	const Condition &condition;
	Optimizer &optimizer;
	Binder &binder;
	ExpressionBuilder calc;
	//! The source's own bindings, which the proved expressions refer to, and its types.
	vector<ColumnBinding> source_bindings;
	vector<LogicalType> source_types;
	//! The scope projection's table index, or INVALID_INDEX when the input passed through it unchanged.
	idx_t state_index = DConstants::INVALID_INDEX;

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

	//! The column that says whether a row belongs to a budget; only a scoped budget has one.
	static constexpr const char *ELIGIBLE = "elig";
	//! The budget's lower and upper limit on every row, constant within a group; only a budget with a limit read from
	//! the data has them.
	static constexpr const char *BUDGET_LOWER = "budget_lo";
	static constexpr const char *BUDGET_UPPER = "budget_up";

	//! A copy of a proved expression, re-pointed at the scope projection when there is one.
	unique_ptr<Expression> OverState(const Expression &expr) const {
		if (state_index == DConstants::INVALID_INDEX) {
			return expr.Copy();
		}
		return DirectOverProjection(expr, source_bindings, state_index);
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
	//! The condition, for a row that belongs to a budget; false for any other row. Without a scope every row does.
	ExprPtr Within(const ExprPtr &condition_p) const {
		return condition.scoped ? And(C(ELIGIBLE), condition_p) : condition_p;
	}
	//! 1.0 where the condition holds, else 0.0.
	ExprPtr Flag(const ExprPtr &condition_p) const {
		return calc.If(condition_p, calc.Num(1.0), Zero());
	}
	//! `value` where the condition holds, else 0.0.
	ExprPtr WhenHolds(const ExprPtr &condition_p, const ExprPtr &value) const {
		return calc.If(condition_p, value, Zero());
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
			auto source = DirectMergedOverProjection(optimizer.context, values, source_bindings, p1);
			checks.push_back(DirectValidNumberPredicate(optimizer, LogicalType::DOUBLE, columns.at(name).binding,
			                                            source.get(), kind));
		};
		for (idx_t j = 0; j < condition.clauses.size(); j++) {
			auto &clause = condition.clauses[j];
			number("c" + std::to_string(j), clause.coefficient, DirectNumberKind::CONSTRAINT);
			checks.push_back(DirectValidBoundPredicate(optimizer, columns.at("r" + std::to_string(j)).binding,
			                                           *DirectOverProjection(*clause.rhs, source_bindings, p1)));
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
		auto empty = DirectBoxIsEmpty(calc, C("l"), C("u"));
		auto anchor = calc.If(empty, Zero(), calc.Min({calc.Max({Zero(), C("l")}), C("u")}));
		auto free_of_budget = Equal(C("w"), Zero());
		if (condition.scoped) {
			free_of_budget = calc.Any({free_of_budget, Not(C(ELIGIBLE))});
		}
		// Adding 0.0 turns -0.0 into 0.0, so prices that are equal sort and compare as equal.
		auto price = calc.If(free_of_budget, NegativeInfinity(), calc.Add(calc.Div(C("v"), C("w")), Zero()));
		Project(
		    {{"empty", empty}, {"a", anchor}, {"wz", free_of_budget}, {"pos", Greater(C("w"), Zero())}, {"t", price}});
	}

	//! P5: each item's room up and room down, in units of budget. A finite room goes in `up_fin` and `down_fin`; an
	//! infinite one is a flag, counted apart. The spend of the anchors, the best end of an item outside the ranking,
	//! and whether that item is unbounded.
	void AddRooms() {
		auto w = C("w"), v = C("v"), l = C("l"), u = C("u"), a = C("a");
		auto empty = C("empty"), wz = C("wz"), pos = C("pos");
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
		Project({{"up_inf", up_open},
		         {"down_inf", down_open},
		         {"up_fin", up_room},
		         {"down_fin", down_room},
		         {"sterm", calc.Mul(w, a)},
		         {"best", best_end},
		         {"u1", Flag(And(And(wz, Not(empty)), unbounded_end))},
		         {"upi", Flag(up_open)},
		         {"dni", Flag(down_open)}});
	}

	//! Partitions a window by the budget's groups: the rows outside every budget apart from the rest, then the `PER`
	//! keys. Without a scope the window covers the whole input.
	void PartitionByGroup(BoundWindowExpression &window) const {
		if (!condition.scoped) {
			return;
		}
		window.partitions.push_back(calc.Out(C(ELIGIBLE)));
		for (auto slot : condition.group_key_slots) {
			window.partitions.push_back(calc.Out(C(SourceName(slot))));
		}
	}

	//! A sum, maximum or minimum over the group, as a window.
	unique_ptr<BoundWindowExpression> OverAll(const char *function, const ExprPtr &value) const {
		auto window = DirectWindowExtremum(optimizer, function, calc.Out(value));
		PartitionByGroup(*window);
		return window;
	}

	//! W0: the group totals. Every sum here is over finite parts; the open rooms are counts.
	void AddGroupTotals() {
		auto t = C("t"), up_fin = C("up_fin"), down_fin = C("down_fin"), up_inf = C("up_inf"), down_inf = C("down_inf");
		vector<std::pair<string, unique_ptr<BoundWindowExpression>>> added;
		//! Items with a box no value satisfies.
		added.emplace_back("n_empty", OverAll("sum", Flag(C("empty"))));
		//! S: the spend of the anchors. sup and cup, sdn and cdn: the finite room up and down, and how many rooms up
		//! and down are infinite.
		added.emplace_back("S", OverAll("sum", C("sterm")));
		added.emplace_back("sup", OverAll("sum", up_fin));
		added.emplace_back("cup", OverAll("sum", C("upi")));
		added.emplace_back("sdn", OverAll("sum", down_fin));
		added.emplace_back("cdn", OverAll("sum", C("dni")));
		//! Items outside the ranking that improve without end.
		added.emplace_back("n_u1", OverAll("sum", C("u1")));
		//! The highest price of an item with infinite room up, the lowest of one with infinite room down.
		added.emplace_back("lo", OverAll("max", calc.If(up_inf, t, NullDouble())));
		added.emplace_back("hi", OverAll("min", calc.If(down_inf, t, NullDouble())));
		//! The zero-price spend: the anchors, plus the room up of every item that gains and minus the room down of
		//! every item that loses at price zero. czi counts the infinite rooms up among the first, czd the infinite
		//! rooms down among the second.
		added.emplace_back("zup", OverAll("sum", WhenHolds(Greater(t, Zero()), up_fin)));
		added.emplace_back("zdn", OverAll("sum", WhenHolds(Less(t, Zero()), down_fin)));
		added.emplace_back("czi", OverAll("sum", Flag(And(Greater(t, Zero()), up_inf))));
		added.emplace_back("czd", OverAll("sum", Flag(And(Less(t, Zero()), down_inf))));
		Window(std::move(added));
	}

	//! W1, the plan's one sort: running sums of the rooms in order of price, highest first, ties in any order. The
	//! inclusive sum of an item is its own room plus that of every item before it.
	void AddRunningSums() {
		auto running = [&](const ExprPtr &value) {
			// Hand-built from the whole-partition aggregate: the same node with a frame that ends at the current row,
			// and the sort that gives the frame its meaning.
			auto sum = DirectWindowExtremum(optimizer, "sum", calc.Out(value));
			sum->end = WindowBoundary::CURRENT_ROW_ROWS;
			PartitionByGroup(*sum);
			sum->orders.emplace_back(OrderType::DESCENDING, OrderByNullType::NULLS_LAST, calc.Out(C("t")));
			return sum;
		};
		vector<std::pair<string, unique_ptr<BoundWindowExpression>>> added;
		added.emplace_back("uinc", running(C("up_fin")));
		added.emplace_back("dinc", running(C("down_fin")));
		added.emplace_back("cui", running(C("upi")));
		added.emplace_back("cdi", running(C("dni")));
		Window(std::move(added));
	}

	//! P6: the verdicts, which limit the budget binds at, and whether each item reaches it. L and U are the budget's
	//! limits on the row, constant within a group (Lower and Upper).
	//!
	//! Infeasible when a box is empty, the limits cross, or the reachable spend [S - down, S + up] misses [L, U] (an
	//! infinite room puts that end of the range at infinity). Unbounded when an item outside the ranking improves
	//! without end, or when an open room up has a higher price than an open room down. A side with no limit absorbs
	//! spend at price zero: with no lower limit the open rooms up count as at least zero, and with no upper limit the
	//! open rooms down as at most zero. The zero-price spend, the anchors plus the room up of every item that gains and
	//! minus the room down of every item that loses, is infinite when such a room is open. The budget binds at U when
	//! that spend is above it ("over") and at L when it is below ("under"); the target spend is then that limit.
	//! Otherwise the price is zero.
	//!
	//! The rows outside every budget are a partition of their own. Their items are all outside the ranking, so the
	//! price, which limit binds and the spends of that partition are never read (each item answers with its own best
	//! end). Only a box no value satisfies and an item that improves without end are tested on them; the test for an
	//! unreachable spend is for the rows of a group alone.
	//!
	//! Over, the items before one in the sort, taken to their high end, with the others at their low end, spend
	//! `S + uinc - dafter` at most, and the price is that of the first item whose spend reaches U. Under, the items
	//! before one at their high end and the item and those after it at their low end spend `S + prior_up - dafter -
	//! down` at least, and the price is that of the last item whose spend does not pass L. Both tests are monotone
	//! along the one sort, because rounded sums do not land exactly on a limit.
	void AddVerdicts() {
		auto lower = Lower(), upper = Upper();
		// A box no value satisfies is infeasible on any row. The limits are tested for the rows of a group only, as is
		// the reachable spend: the rows outside every budget have no spend to reach a limit with. Limits that cross
		// are infeasible, and so is a lower limit of +infinity or an upper one of -infinity (a limit read from the
		// data).
		auto limits_unmet =
		    calc.Any({Greater(lower, upper), Equal(lower, PositiveInfinity()), Equal(upper, NegativeInfinity())});
		auto infeasible = calc.Any(
		    {Greater(C("n_empty"), Zero()),
		     Within(calc.Any({limits_unmet, And(Equal(C("cdn"), Zero()), Greater(calc.Sub(C("S"), C("sdn")), upper)),
		                      And(Equal(C("cup"), Zero()), Less(calc.Add(C("S"), C("sup")), lower))}))});
		auto lo = calc.Coalesce({C("lo"), NegativeInfinity()}), hi = calc.Coalesce({C("hi"), PositiveInfinity()});
		// With no lower limit the open rooms up count as at least zero, and with no upper limit the open rooms down as
		// at most zero.
		auto lo_effective = calc.If(Equal(lower, NegativeInfinity()), calc.Max({lo, Zero()}), lo);
		auto hi_effective = calc.If(Equal(upper, PositiveInfinity()), calc.Min({hi, Zero()}), hi);
		auto unbounded = calc.Any({Greater(C("n_u1"), Zero()), Greater(lo_effective, hi_effective)});
		auto zero_price_spend = calc.Sub(calc.Add(C("S"), C("zup")), C("zdn"));
		// An open room at a price on the wrong side of zero makes the zero-price spend infinite.
		auto over =
		    calc.Any({Greater(C("czi"), Zero()), And(Equal(C("czd"), Zero()), Greater(zero_price_spend, upper))});
		auto under = calc.Any({Greater(C("czd"), Zero()), And(Equal(C("czi"), Zero()), Less(zero_price_spend, lower))});
		auto after = calc.Sub(C("sdn"), C("dinc"));
		auto open_after = calc.Sub(C("cdn"), C("cdi"));
		auto spend_at_most = calc.Sub(calc.Add(C("S"), C("uinc")), after);
		auto reach_high =
		    calc.If(Greater(C("cui"), Zero()), calc.Bool(true),
		            calc.If(Greater(open_after, Zero()), calc.Bool(false),
		                    calc.Cmp(ExpressionType::COMPARE_GREATERTHANOREQUALTO, spend_at_most, upper)));
		auto prior_up = calc.Sub(C("uinc"), C("up_fin"));
		auto open_from_here = calc.Add(open_after, C("dni"));
		auto spend_at_least = calc.Sub(calc.Sub(calc.Add(C("S"), prior_up), after), C("down_fin"));
		auto reach_low = calc.If(Greater(open_from_here, Zero()), calc.Bool(true),
		                         calc.If(Greater(calc.Sub(C("cui"), C("upi")), Zero()), calc.Bool(false),
		                                 calc.Cmp(ExpressionType::COMPARE_LESSTHANOREQUALTO, spend_at_least, lower)));
		auto candidate = calc.If(over, calc.If(reach_high, C("t"), NullDouble()),
		                         calc.If(under, calc.If(reach_low, C("t"), NullDouble()), NullDouble()));
		Project({{"infeasible", infeasible},
		         {"unbounded", unbounded},
		         {"over", over},
		         {"under", under},
		         {"dafter", after},
		         {"cand", candidate},
		         {"prior_up", prior_up}});
	}

	//! W2: the price of a binding budget. Over U, the highest t of the items whose high end reaches it; none reaches it
	//! only when U is the top of the range up to rounding, and then every item is at its high end: a price below every
	//! t. Under L, the lowest t of the items whose low end does not pass it; none does only when L is the bottom of the
	//! range up to rounding, and then every item is at its low end: a price above every t.
	void AddPrice() {
		vector<std::pair<string, unique_ptr<BoundWindowExpression>>> added;
		added.emplace_back("lam_over", OverAll("max", C("cand")));
		added.emplace_back("lam_under", OverAll("min", C("cand")));
		Window(std::move(added));
	}

	//! The price: that of the item the limit picks when the budget binds, zero when it does not.
	ExprPtr Price() const {
		return calc.If(C("over"), calc.Coalesce({C("lam_over"), NegativeInfinity()}),
		               calc.If(C("under"), calc.Coalesce({C("lam_under"), PositiveInfinity()}), Zero()));
	}

	//! W3: the rooms above, below and at the price.
	void AddPriceSums() {
		auto t = C("t"), lam = Price(), up_inf = C("up_inf"), down_inf = C("down_inf");
		auto at_price = Equal(t, lam);
		vector<std::pair<string, unique_ptr<BoundWindowExpression>>> added;
		added.emplace_back("a_star", OverAll("sum", WhenHolds(Greater(t, lam), C("up_fin"))));
		added.emplace_back("b_star", OverAll("sum", WhenHolds(Less(t, lam), C("down_fin"))));
		added.emplace_back("tup", OverAll("sum", WhenHolds(at_price, C("up_fin"))));
		added.emplace_back("tdn", OverAll("sum", WhenHolds(at_price, C("down_fin"))));
		added.emplace_back("ctup", OverAll("sum", Flag(And(at_price, up_inf))));
		added.emplace_back("ctdn", OverAll("sum", Flag(And(at_price, down_inf))));
		Window(std::move(added));
	}

	//! P7: each item's answer. Above the price an item is at its high end, below it at its low end, and an item with
	//! w = 0 at its best end. The items at the price share what the binding limit leaves them, D, starting from their
	//! anchors:
	//! up when D is positive, down when it is negative, each by its room or by what remains after the items before it
	//! in the sort (those after it, for a move down), found from the running sums. An open room takes what remains of
	//! the finite rooms, evenly with the other open rooms at the price. The answer is held to the box.
	void AddAnswer() {
		auto t = C("t"), lam = Price(), w = C("w"), l = C("l"), u = C("u"), a = C("a"), pos = C("pos");
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
		auto high = calc.If(pos, u, l);
		auto low = calc.If(pos, l, u);
		auto answer = calc.If(C("wz"), C("best"), calc.If(Greater(t, lam), high, calc.If(Less(t, lam), low, shared)));
		Project({{"x", calc.Add(calc.Min({calc.Max({answer, l}), u}), Zero())}});
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
		for (idx_t i = 0; i < source_bindings.size(); i++) {
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
		auto sources = source_bindings.size();
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
		auto sources = source_bindings.size();
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
		auto sources = source_bindings.size();
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

DirectSubstitute Rule::Apply(unique_ptr<LogicalOperator> source, Optimizer &optimizer,
                             const DirectApplicabilityCondition &candidate) const {
	auto &condition = static_cast<const Condition &>(candidate);
	PlanBuilder plan_builder(condition, optimizer, *source);
	return plan_builder.Build(std::move(source));
}

} // namespace direct_r1_continuous_knapsack

unique_ptr<DirectSolveRule> MakeR1ContinuousKnapsackRule() {
	return make_uniq<direct_r1_continuous_knapsack::Rule>();
}

} // namespace duckdb
