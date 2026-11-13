#include "duckdb/optimizer/decide/direct/direct_rules.hpp"

#include "duckdb/optimizer/decide/direct/direct_bound_reader.hpp"
#include "duckdb/optimizer/decide/direct/direct_expression.hpp"
#include "duckdb/optimizer/decide/direct/direct_plan_fragments.hpp"
#include "duckdb/optimizer/decide/direct/direct_runtime_checks.hpp"
#include "duckdb/optimizer/decide/direct/direct_source_reader.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_window.hpp"

namespace duckdb {

//! Each rule keeps its helpers in its own named namespace: unity builds compile every rule file in one translation
//! unit, where two anonymous namespaces would merge and same-named helpers would collide.
namespace direct_r4_max_min_fairness {

constexpr const char *RULE_NAME = "R4_MAX_MIN_FAIRNESS";

//! What CheckCondition proved: one REAL decision `x`, one for each row or one shared by the rows of a key (`per K:`),
//! in the box [0, cap], where the capacity is a constant, a value of the row or absent; one budget `SUM(w*x) <= B` with
//! a weight `w` of any sign, and a limit `B` that is a constant or read from the data, over the groups of a `by` key
//! and the rows of a `when`; and an objective `MAXIMIZE MIN(c*x)` over one term whose coefficient `c` is a value of the
//! row. The objective is the canonical form of `MAXIMIZE MIN(x / demand)`: `c = 1 / demand`.
struct Condition final : DirectApplicabilityCondition {
	//! The box's upper end before any per-row bound, and the per-row clauses the plan evaluates: each an upper end.
	double upper = POSITIVE_INFINITY;
	vector<DirectBoundClause> clauses;
	//! The MIN's one term `c*x`, and the rows it reads (`kept`).
	DirectQuadraticPart objective;
	//! The budget `SUM(w*x) <= B`: the rows and groups it covers, its limit, and the weight of a row. A weight that
	//! folds to a constant is the literal `weight`, which the plan builds with (every expression over it folds);
	//! otherwise it is the sum of the terms of `weights` on the decision, a value of the row.
	DirectSharedScope scope;
	DirectLimit limit;
	//! The key of a decision shared by the rows of a key, empty for one decision on each row; and whether a row has a
	//! decision, a predicate over the source (null when every row has one): a row with a NULL in an element that puts
	//! it in no class has none, so nothing is read on it.
	vector<DirectKeyElement> keys;
	unique_ptr<Expression> present;
	bool constant_weight = true;
	double weight = 0.0;
	DirectLinearPart weights;
};

class Rule final : public DirectSolveRule {
public:
	const char *Name() const override {
		return RULE_NAME;
	}

	unique_ptr<DirectApplicabilityCondition> CheckCondition(const DirectIR &ir, ClientContext &context) const override {
		auto condition = make_uniq<Condition>();
		// The class plan reads the source, and the rows read it again to be given their answer.
		condition->source_reads = 2;
		if (!ProveDecision(ir, *condition) || !ProveBudget(ir, context, *condition) ||
		    !ProveBox(ir, context, *condition) || !ProveObjective(ir, context, *condition)) {
			return nullptr;
		}
		ReadOnRowsWithADecision(context, *condition);
		return std::move(condition);
	}

	string Summary() const override {
		return "max MIN(c*x), 0 <= x <= cap, SUM(w*x) <= B";
	}

	DirectSubstitute Apply(DirectSourceReader &reader, Optimizer &optimizer,
	                       const DirectApplicabilityCondition &candidate,
	                       const DirectStateErrors &errors) const override;

private:
	bool ProveDecision(const DirectIR &ir, Condition &condition) const;
	void ReadOnRowsWithADecision(ClientContext &context, Condition &condition) const;
	bool ProveBudget(const DirectIR &ir, ClientContext &context, Condition &condition) const;
	bool BudgetFitsKeys(const Condition &condition) const;
	bool ProveBox(const DirectIR &ir, ClientContext &context, Condition &condition) const;
	bool ProveObjective(const DirectIR &ir, ClientContext &context, Condition &condition) const;
};

//! One REAL decision, on each row or shared by the rows of a key. A decision of the whole query is another class.
bool Rule::ProveDecision(const DirectIR &ir, Condition &condition) const {
	if (ir.decisions.size() != 1 || ir.decisions[0].domain != DirectDomain::REAL ||
	    ir.decisions[0].scope == DecideVarScope::GLOBAL) {
		return false;
	}
	bool keyed = ir.decisions[0].scope == DecideVarScope::KEYED;
	if (ir.keyed_scopes.size() != (keyed ? 1 : 0)) {
		return false;
	}
	if (keyed) {
		// A key's columns are columns of the source (the IR reads them as a row check would), so a copy is all it
		// takes.
		for (auto &element : ir.keyed_scopes[ir.decisions[0].keyed_scope].key) {
			condition.keys.push_back(element.Copy());
		}
	}
	return true;
}

//! A row with a NULL in an element of the decision's key that puts it in no class has no decision (D5), and nothing is
//! read on it, as on the solver path: not its capacity, its weight or its coefficient. The predicate that says so joins
//! the `when` of every clause that reads the row.
void Rule::ReadOnRowsWithADecision(ClientContext &context, Condition &condition) const {
	ExpressionBuilder calc(context);
	ExprPtr present;
	for (auto &element : condition.keys) {
		if (element.no_group) {
			auto has_value = calc.Not(calc.IsNull(calc.Of(*element.grouping)));
			present = present ? calc.And(present, has_value) : has_value;
		}
	}
	if (!present) {
		return;
	}
	condition.present = calc.Out(present);
	auto both = [&](unique_ptr<Expression> &when) {
		when = when ? calc.Out(calc.And(calc.Wrap(std::move(when)), present)) : calc.Out(present);
	};
	both(condition.objective.kept);
	for (auto &clause : condition.clauses) {
		both(clause.when);
	}
}

//! The budget `SUM(w*x) <= B`, the one aggregate clause, over the rows of its `when` and the groups of its `by` key,
//! with a weight of any sign and a limit that is a constant or read from the data. The other constraints are per-row
//! bounds on the decision, which ProveBox reads. A clause `per K:` repeats the whole total for each class, which is
//! another class of problem.
bool Rule::ProveBudget(const DirectIR &ir, ClientContext &context, Condition &condition) const {
	const DirectIRConstraint *budget = nullptr;
	for (auto &ir_node : ir.constraints) {
		if (!ir_node.aggregate) {
			if (!ir_node.singleton_row) {
				return false;
			}
			continue;
		}
		if (budget) {
			return false;
		}
		budget = &ir_node;
	}
	if (!budget || budget->comparison != ExpressionType::COMPARE_LESSTHANOREQUALTO) {
		return false;
	}
	if (!DirectProveSharedScope(ir, {budget}, condition.scope) || condition.scope.per) {
		return false;
	}
	// DirectProveSharedScope has proved that the clause is a plain SUM.
	auto &sum = *budget->PlainSum();
	if (!DirectOnlyLinearTerms(sum.terms) ||
	    !DirectReadLinearPart(ir, context, sum, budget->written, condition.weights) ||
	    !DirectReadLimit(ir, context, *budget, condition.limit)) {
		return false;
	}
	condition.constant_weight = DirectFoldConstantSum(context, DirectValuesOn(condition.weights, 0), condition.weight);
	return condition.keys.empty() || BudgetFitsKeys(condition);
}

//! The rows of a key are one decision, so the budgets of two of them are one budget only when the key says which: the
//! groups are within the key, and no `when` keeps some rows of a key and drops others. The key's one row carries the
//! limit, a constant: a limit read from the data is reduced over the rows of a group, and the aggregates of a limit (a
//! sum, a count) do not split by key.
bool Rule::BudgetFitsKeys(const Condition &condition) const {
	return !condition.scope.when && DirectKeyWithin(condition.scope.keys, condition.keys) && condition.limit.constant;
}

//! The box is [0, cap]: the lower end is the default 0 or a written 0, and every per-row clause bounds the decision
//! from above alone, `a * x <= r` with a constant `a` above 0. A clause that also bounds it from below (`x >= l`, `x =
//! cap`) is another class, and so is a coefficient `a` read from the data, whose sign decides the side.
bool Rule::ProveBox(const DirectIR &ir, ClientContext &context, Condition &condition) const {
	DirectBoundReading reading;
	if (!DirectReadBounds(ir, context, reading) || reading.boxes[0].lower != 0.0) {
		return false;
	}
	for (auto &ir_node : ir.constraints) {
		// A bound the solver folds into the box is in `boxes`, whose lower end is 0.
		if (ir_node.aggregate || ir_node.variable_bound) {
			continue;
		}
		double coefficient;
		if (ir_node.comparison != ExpressionType::COMPARE_LESSTHANOREQUALTO ||
		    !TryEvaluateFoldableDouble(context, *ir_node.bound.a, coefficient, DIRECT_FINITE_CONSTANT) ||
		    !(coefficient > 0.0)) {
			return false;
		}
	}
	condition.upper = reading.boxes[0].upper;
	condition.clauses = std::move(reading.clauses);
	return true;
}

//! One plain `MIN` of one linear term, maximized (the part's sign is negative) with no constant factor. A row whose
//! data is NULL, or that a `when` drops, is not in the MIN (`kept`).
bool Rule::ProveObjective(const DirectIR &ir, ClientContext &context, Condition &condition) const {
	if (ir.objective.parts.size() != 1) {
		return false;
	}
	auto &part = ir.objective.parts[0];
	if (part.reducer != DirectReducer::MIN || part.qualifier != DConstants::INVALID_INDEX || !part.by.empty() ||
	    part.terms.size() != 1 || !part.terms[0].IsLinear()) {
		return false;
	}
	auto &read = condition.objective;
	return DirectReadLinearPart(ir, context, part, ir.objective.written, read.linear) &&
	       DirectObjectivePartKept(ir, part, read.kept) && read.linear.sign < 0 && !read.linear.factor.present;
}

//===--------------------------------------------------------------------===//
// Plan
//===--------------------------------------------------------------------===//

//! Builds the relational plan for one R4 problem whose condition holds, as group-then-join (DirectJoinClassAnswers)
//! over two reads of the source, with one class for each group of the budget. A row the MIN reads has the coefficient
//! `c`; at level `s` it must hold `x >= s/c`, and `x <= cap` gives `s <= cap*c`. A row with `c <= 0` caps the level at
//! 0, since `c*x <= 0`. Each row takes the end of its range that uses the least budget, which its weight `w` decides:
//!
//!  - `w >= 0`: the low end, `s/c` for a row the MIN reads and 0 for any other. A weight of 0 is no case of its own,
//!    since `w*x` adds nothing.
//!  - `w < 0`: its capacity `cap`, which frees the budget `-w*cap`. A row with no capacity (NULL, or 1e20 or more)
//!    frees as much as it is asked to: the budget is then unlimited, and the level is the least `cap*c` alone.
//!
//! Each group `g` of the budget has its own level. With `N` the sum of `w/c` over the rows the MIN reads with `w > 0`
//! and `c > 0`, `F` the freed budget of the capped rows with `w < 0`, and `W` the sum of `-w` over the uncapped rows
//! with `w < 0`, the level is `s_g = least((B+F)/N, U)` (just `U` when `W > 0`), `U` being the least `cap*c`. The
//! budget is infeasible when `B+F < 0` and `W = 0`: every other row needs at least 0. A group with no limit (a NULL
//! one, or none of its rows kept) is no budget, and the rows outside every budget are one group of rows with no
//! budget, whose level is `U` alone.
//!
//! The objective is the least `c*x` over all the rows, so the level of the answer is `s* = min_g s_g`. A group whose
//! level is infinite (nothing bounds it) and every row outside every budget take `s*`; when `s*` is infinite too, the
//! query is unbounded. An uncapped row with `w < 0` takes its low end, or more when the other rows of its group need
//! budget it must free: `max(lo, max(0, N*s - (B+F)) / W)`; taking more than the deficit over `W` only frees more. A
//! row the MIN does not read has no `c`.
//!
//! A decision shared by the rows of a key is one variable `x_K`. Its weight is the sum of its rows' weights, its
//! capacity the least of their capacities (`x <= cap` for each) and the MIN reads its least coefficient (`min(c*x) =
//! min(c)*x` for `x >= 0`, so any `c <= 0` gives level 0). The plan reduces the rows to one row for each key first, and
//! then runs the plan below over those rows as if each were a row with a decision of its own: late materialization,
//! the shape of A2 for a decision per key. The budgets are those of groups within the key (ProveBudget), so the key
//! fixes the group, and the key's row carries the group's key. A row whose key has a NULL where that puts it in no
//! class has no decision: nothing is read on it, it counts nowhere and its amount is NULL.
//!
//! For a decision on each row, on each row of the first read:
//!
//!   [W0] the row's membership in the budget and its group key (DirectProjectScopes)
//!   P1   raw values: the capacity clauses' right sides, the budget's weight and limit inputs and the MIN's
//!        coefficient `c`, each on the rows that read it, and the check of every value that can fail
//!   P2   the row's capacity (the least upper end of the box on it) and the verdict of the raw checks
//!   P3   the verdict of the row, with its box check, and `w/c`, the freed budget, the unlimited weight and `cap*c` (0
//!        for `c <= 0`), each NULL where the row does not count
//!   H    a hash aggregate on the group key, one row for each group and one for the rows outside every budget: the
//!        sums N, F and W, the least U, the group's limit and whether the checks of the rows held
//!   C1   the level of each group
//!   [W1] `s*`, the least level over the groups, on every group's row (a window with no partition, over the groups)
//!   C2   what the rows read of their group: its level, the deficit's share of one unit of `W`, whether it has a
//!        budget and `s*`, with the checks of the group (infeasible, unbounded) and of the rows together
//!
//! The second read computes [W0] and P1 to P3 again for its `c`, `w` and `cap`, joined with the group's row, then
//!
//!   P4   the answer, and the column that keeps the checks live
//!
//! For a decision shared by the rows of a key, H is preceded by K and the second read is a join by the key:
//!
//!   K    a hash aggregate on the decision's key and the group's key, over P1 to P3 of the first read, one row for each
//!        key: the sum of the weights, the least capacity and coefficient, and whether the checks of the rows held. It
//!        is held (DirectHeldRows), as a CTE the plan reads twice, so it runs once
//!   H, C1, [W1], C2   over the key rows, each counting where its key has a decision (P3's expressions on the key's
//!        row, over the held columns)
//!   P4   the key's answer, over the key rows joined with the group's row, one row for each key
//!   J    the rows of the second read, joined with the key's answer by the decision's key: a row whose key is NULL
//!   where
//!        that puts it in no class matches none, and has no amount
//!
//! A row outside every budget has a NULL in the key's columns that say so, and the join compares them with IS NOT
//! DISTINCT FROM: it finds the row of the rows outside every budget, which is how it reads `s*`. The solver reads every
//! row before it returns, so a bad value on the last row raises even under an outer LIMIT 1. H (and K, which the plan
//! holds in a CTE whose definition runs to its end before any read of it) holds every row of the first read before it
//! gives one, and each check stands below it, so no joined row exists before every row has passed its checks. The
//! second read raises nothing: the first has raised on any value it would.
class PlanBuilder {
public:
	//! [W0] to P3 over a read: the plan, where its columns are.
	struct Row {
		unique_ptr<LogicalOperator> plan;
		//! The source's columns, and the row's scope: whether it is in the budget's rows and its group key.
		DirectCarried carried;
		//! The decision's key, for a decision shared by the rows of a key.
		DirectScopeColumns decision;
		//! What the limit's aggregates read, one member per input of the limit's group bound.
		vector<DirectGroupInputColumns> limit_inputs;
		//! The MIN's coefficient `c`, NULL on a row the MIN does not read, the weight (a column only for one read from
		//! the data, Weight) and the capacity of the row.
		ColumnBinding coefficient;
		ColumnBinding weight;
		ColumnBinding capacity;
		//! What the row adds to each sum of its group (BuildGroups): `w/c`, the freed budget, the unlimited weight and
		//! `cap*c`, NULL where the row does not count; and whether the row's checks held.
		ColumnBinding need;
		ColumnBinding freed;
		ColumnBinding unlimited;
		ColumnBinding allowed;
		ColumnBinding valid;
	};

	//! The columns of the rows of the keys (BuildKeys), as the held plan names them: the decision's key, the group's
	//! key, and the weight, capacity and coefficient of the key and whether the checks of its rows held.
	struct Keys {
		vector<ColumnBinding> decision;
		DirectClassKey decision_key;
		vector<ColumnBinding> budget;
		DirectClassKey budget_key;
		ColumnBinding weight;
		ColumnBinding capacity;
		ColumnBinding coefficient;
		ColumnBinding valid;
	};

	PlanBuilder(const Condition &condition_p, Optimizer &optimizer_p, const vector<ColumnBinding> &source_bindings_p,
	            const vector<LogicalType> &source_types_p, const DirectStateErrors &errors_p)
	    : condition(condition_p), optimizer(optimizer_p), binder(optimizer_p.binder), calc(optimizer_p.context),
	      source_bindings(source_bindings_p), source_types(source_types_p), errors(errors_p) {
		D_ASSERT(source_bindings.size() == source_types.size());
		if (!condition.limit.constant) {
			limit_bound = condition.limit.data.GroupBound(optimizer);
		}
	}

	//! The class plan over `row`, the rows of the first read or the rows of their keys: the group key's columns, then
	//! what a row reads of its group (C2), one row for each group.
	unique_ptr<LogicalOperator> BuildGroups(Row row) const {
		auto &carried = row.carried;
		DirectAggregateStage aggregate(binder);
		vector<ColumnBinding> keys;
		for (idx_t i = 0; i < carried.keys.size(); i++) {
			keys.push_back(aggregate.Group(calc.Out(calc.Col(carried.key.types[i], carried.keys[i]))));
		}
		auto need = aggregate.Add(DirectHashAggregate(optimizer, calc, "sum", Double(row.need)));
		auto freed = aggregate.Add(DirectHashAggregate(optimizer, calc, "sum", Double(row.freed)));
		auto unlimited = aggregate.Add(DirectHashAggregate(optimizer, calc, "sum", Double(row.unlimited)));
		auto allowed = aggregate.Add(DirectHashAggregate(optimizer, calc, "min", Double(row.allowed)));
		auto valid = aggregate.Add(DirectHashAggregate(optimizer, calc, "bool_and", Boolean(row.valid)));
		DirectGroupValues limit_values;
		if (limit_bound) {
			DirectAggregateSink sink(aggregate);
			limit_values = DirectAddGroupBound(calc, *limit_bound, row.limit_inputs, sink, nullptr);
		}
		unique_ptr<LogicalOperator> plan = aggregate.Finish(std::move(row.plan));

		// C1. With no row the MIN reads, `need` and `allowed` are NULL, the level is NULL and no row reads it. Adding
		// 0.0 turns a -0.0 level (a limit of -0.0) into 0.0. An unlimited budget has no level of its own, and NULL is
		// what LEAST skips. The rows outside every budget have no limit: their group is not present.
		auto zero = calc.Num(0.0);
		auto no_value = calc.Constant(Value(LogicalType::DOUBLE));
		vector<ExprPtr> key_values;
		for (idx_t i = 0; i < keys.size(); i++) {
			key_values.push_back(calc.Col(carried.key.types[i], keys[i]));
		}
		auto limit =
		    limit_bound ? calc.Cast(limit_values.upper, LogicalType::DOUBLE) : calc.Num(condition.limit.number);
		auto present = DirectClassPresent(calc, carried.key, key_values);
		auto unlimited_weight = calc.Coalesce({Double(unlimited), zero});
		auto is_unlimited = Positive(unlimited_weight);
		auto budget = calc.Add(calc.If(present, limit, no_value), calc.Coalesce({Double(freed), zero}));
		auto from_budget = calc.If(is_unlimited, no_value, calc.Div(budget, Double(need)));
		DirectProjectionStage c1(binder.GenerateTableIndex());
		c1.Forward(keys, carried.key.types);
		auto level = c1.Add(calc.Add(calc.Min({from_budget, Double(allowed)}), zero));
		auto budget_column = c1.Add(budget);
		auto need_column = c1.Add(Double(need));
		auto unlimited_column = c1.Add(unlimited_weight);
		auto valid_column = c1.Add(Boolean(valid));
		// The group's limit can fail as a value (a NaN limit, which the solver names); its message is a column of the
		// group's row.
		vector<ColumnBinding> messages;
		for (auto &message : limit_values.messages) {
			messages.push_back(c1.Add(message));
		}
		vector<ColumnBinding> keys1;
		for (idx_t i = 0; i < keys.size(); i++) {
			keys1.emplace_back(c1.index, i);
		}
		plan = c1.Finish(std::move(plan));

		// W1: the objective is the least level of any group, and a group that nothing bounds takes it.
		ColumnBinding star = level;
		if (condition.scope.Scoped()) {
			auto window = make_uniq<LogicalWindow>(binder.GenerateTableIndex());
			window->expressions.push_back(DirectWindowExtremum(optimizer, "min", calc.Out(Double(level))));
			star = ColumnBinding(window->window_index, 0);
			window->children.push_back(std::move(plan));
			plan = std::move(window);
		}

		// C2. The group's checks follow the rows': no budget is left for rows that need at least 0 when B+F is negative
		// and nothing frees more, and a level nothing bounds improves without limit. Both read the group's row, so an
		// empty source, which has no row to raise on, raises nothing.
		DirectProjectionStage c2(binder.GenerateTableIndex());
		c2.Forward(keys1, carried.key.types);
		auto own_level = calc.If(calc.Cmp(ExpressionType::COMPARE_EQUAL, Double(level), calc.Num(POSITIVE_INFINITY)),
		                         Double(star), Double(level));
		// A deficit is what the rows that take their low end need beyond the budget; a group with no row the MIN reads
		// needs none.
		auto need_total = calc.Coalesce({calc.Mul(Double(need_column), own_level), zero});
		auto deficit = calc.Max({zero, calc.Sub(need_total, Double(budget_column))});
		auto group_unlimited = Positive(Double(unlimited_column));
		c2.Add(own_level);
		c2.Add(calc.If(group_unlimited, calc.Div(deficit, Double(unlimited_column)), no_value));
		c2.Add(calc.Not(calc.IsNull(Double(budget_column))));
		c2.Add(Double(star));
		vector<unique_ptr<Expression>> checks;
		checks.push_back(calc.Out(Boolean(valid_column)));
		for (auto &message : messages) {
			checks.push_back(DirectRaiseMessage(optimizer, calc, calc.Col(LogicalType::VARCHAR, message)));
		}
		auto has_rows = calc.Not(calc.IsNull(Boolean(valid_column)));
		auto no_budget =
		    calc.And(has_rows, calc.And(calc.Not(group_unlimited),
		                                calc.Cmp(ExpressionType::COMPARE_LESSTHAN, Double(budget_column), zero)));
		checks.push_back(DirectRaiseIf(calc, calc.Out(no_budget), DirectInfeasibleErrorPredicate(optimizer, errors)));
		auto no_bound = calc.Cmp(ExpressionType::COMPARE_EQUAL, Double(star), calc.Num(POSITIVE_INFINITY));
		checks.push_back(DirectRaiseIf(calc, calc.Out(no_bound), DirectUnboundedErrorPredicate(optimizer, errors)));
		c2.Add(DirectAllHold(calc, std::move(checks)));
		return c2.Finish(std::move(plan));
	}

	//! The rows of the first read, the plan of a decision on each row: their values and their contribution to their
	//! group's sums.
	Row BuildRows(unique_ptr<LogicalOperator> rows) const {
		return RowStages(std::move(rows), false);
	}

	//! K: the rows of the first read reduced to one row for each key: the definition of the held rows. `keys` says
	//! where its columns are.
	unique_ptr<LogicalOperator> BuildKeys(unique_ptr<LogicalOperator> rows, Keys &keys) const {
		auto row = RowStages(std::move(rows), true);
		DirectAggregateStage aggregate(binder);
		auto group = [&](const DirectScopeColumns &scope, vector<ColumnBinding> &columns) {
			for (idx_t i = 0; i < scope.keys.size(); i++) {
				columns.push_back(aggregate.Group(calc.Out(calc.Col(scope.key.types[i], scope.keys[i]))));
			}
		};
		group(row.decision, keys.decision);
		group(row.carried, keys.budget);
		keys.decision_key = row.decision.key;
		keys.budget_key = row.carried.key;
		keys.weight = aggregate.Add(DirectHashAggregate(optimizer, calc, "sum", Weight(row.weight)));
		keys.capacity = aggregate.Add(DirectHashAggregate(optimizer, calc, "min", Double(row.capacity)));
		keys.coefficient = aggregate.Add(DirectHashAggregate(optimizer, calc, "min", Double(row.coefficient)));
		keys.valid = aggregate.Add(DirectHashAggregate(optimizer, calc, "bool_and", Boolean(row.valid)));
		return aggregate.Finish(std::move(row.plan));
	}

	//! The rows of the keys as rows with a decision of their own, for BuildGroups: each key counts where it has a
	//! decision.
	Row KeyRows(DirectHeldRows &held, const Keys &keys) const {
		auto reference = held.Read();
		Row row;
		DirectProjectionStage p3(binder.GenerateTableIndex());
		row.carried.keys = p3.Forward(keys.budget, keys.budget_key.types);
		row.carried.key = keys.budget_key;
		auto contributions = AddContributions(p3, Double(keys.coefficient), Double(keys.weight), Double(keys.capacity),
		                                      HasDecision(keys));
		row.need = contributions.need;
		row.freed = contributions.freed;
		row.unlimited = contributions.unlimited;
		row.allowed = contributions.allowed;
		row.valid = p3.Add(calc.Out(Boolean(keys.valid)));
		row.plan = p3.Finish(std::move(reference.plan));
		reference.replacer.VisitOperator(*row.plan);
		return row;
	}

	//! P4 for the keys: the answer of each key, joined from the group's row, one row for each key: the decision's key,
	//! then the amount and the verdict.
	unique_ptr<LogicalOperator> BuildKeyAnswers(DirectHeldRows &held, const Keys &keys,
	                                            unique_ptr<LogicalOperator> groups) const {
		auto reference = held.Read();
		DirectProjectionStage p3(binder.GenerateTableIndex());
		auto decision = p3.Forward(keys.decision, keys.decision_key.types);
		auto budget = p3.Forward(keys.budget, keys.budget_key.types);
		auto coefficient = p3.Add(calc.Out(Double(keys.coefficient)));
		auto weight = p3.Add(calc.Out(Double(keys.weight)));
		auto capacity = p3.Add(calc.Out(Double(keys.capacity)));
		auto present = p3.Add(calc.Out(HasDecision(keys)));
		auto plan = p3.Finish(std::move(reference.plan));
		reference.replacer.VisitOperator(*plan);

		auto answered = JoinGroups(std::move(plan), budget, keys.budget_key, Double(coefficient), Double(weight),
		                           Double(capacity), Boolean(present), std::move(groups));
		DirectProjectionStage p4(binder.GenerateTableIndex());
		p4.Forward(decision, keys.decision_key.types);
		p4.Add(calc.Out(answered.amount));
		p4.Add(std::move(answered.verdict));
		return p4.Finish(std::move(answered.plan));
	}

	//! The substitute over the second read `rows` for a decision shared by the rows of a key (J): each row is joined
	//! with its key's answer. `answers` is BuildKeyAnswers' plan.
	DirectSubstitute JoinKeyAnswers(unique_ptr<LogicalOperator> rows, unique_ptr<LogicalOperator> answers) const {
		auto state = DirectProjectScope(binder, std::move(rows), true, nullptr, condition.keys, {});
		auto carried = DirectCarriedOf(state, source_bindings.size());
		auto joined =
		    DirectJoinClassAnswers(binder, std::move(state.plan), carried.keys, carried.key, std::move(answers));
		D_ASSERT(joined.answers.size() == 2);
		DirectProjectionStage p5(binder.GenerateTableIndex());
		p5.Forward(carried.source, source_types);
		p5.Add(calc.Out(Double(joined.answers[0])));
		vector<unique_ptr<Expression>> checks;
		checks.push_back(calc.Out(Boolean(joined.answers[1])));
		p5.Add(DirectAllHold(calc, std::move(checks)));
		return Substitute(p5.Finish(std::move(joined.plan)));
	}

	//! The substitute over the second read `rows` for a decision on each row: its source columns, then each row's
	//! answer, and the column that keeps the class plan's checks live (P4).
	DirectSubstitute JoinAnswers(unique_ptr<LogicalOperator> rows, unique_ptr<LogicalOperator> groups) const {
		auto row = RowStages(std::move(rows), false);
		auto answered = JoinGroups(std::move(row.plan), row.carried.keys, row.carried.key, Double(row.coefficient),
		                           Weight(row.weight), Double(row.capacity), nullptr, std::move(groups));
		DirectProjectionStage p4(binder.GenerateTableIndex());
		for (idx_t i = 0; i < source_bindings.size(); i++) {
			p4.Add(calc.Out(calc.Col(source_types[i], row.carried.source[i])));
		}
		p4.Add(calc.Out(answered.amount));
		p4.Add(std::move(answered.verdict));
		return Substitute(p4.Finish(std::move(answered.plan)));
	}

private:
	//! A plan joined with the rows of the groups: the plan, the amount of each of its rows and the verdict of the
	//! checks.
	struct Answered {
		unique_ptr<LogicalOperator> plan;
		ExprPtr amount;
		unique_ptr<Expression> verdict;
	};

	const Condition &condition;
	Optimizer &optimizer;
	Binder &binder;
	ExpressionBuilder calc;
	vector<ColumnBinding> source_bindings;
	vector<LogicalType> source_types;
	const DirectStateErrors &errors;
	//! The group bound of a limit read from the data; null for a constant limit.
	unique_ptr<DecideGroupBound> limit_bound;

	ExprPtr Double(ColumnBinding binding) const {
		return calc.Col(LogicalType::DOUBLE, binding);
	}
	ExprPtr Boolean(ColumnBinding binding) const {
		return calc.Col(LogicalType::BOOLEAN, binding);
	}
	ExprPtr Positive(const ExprPtr &value) const {
		return calc.Cmp(ExpressionType::COMPARE_GREATERTHAN, value, calc.Num(0.0));
	}
	ExprPtr Negative(const ExprPtr &value) const {
		return calc.Cmp(ExpressionType::COMPARE_LESSTHAN, value, calc.Num(0.0));
	}
	//! The weight of a decision: the literal of a constant weight, which DuckDB's rewriter folds into every expression
	//! over it, or the column `weight`.
	ExprPtr Weight(ColumnBinding weight) const {
		return condition.constant_weight ? calc.Num(condition.weight) : Double(weight);
	}

	//! Forwards the columns the limit's aggregates read through `stage`.
	void ForwardLimitInputs(DirectProjectionStage &stage, vector<DirectGroupInputColumns> &inputs) const {
		for (idx_t i = 0; i < inputs.size(); i++) {
			if (inputs[i].reads.table_index != DConstants::INVALID_INDEX) {
				inputs[i].reads = stage.Add(calc.Out(Boolean(inputs[i].reads)));
			}
			if (inputs[i].argument.table_index != DConstants::INVALID_INDEX) {
				auto type = limit_bound->inputs[i].argument->return_type;
				inputs[i].argument = stage.Add(calc.Out(calc.Col(type, inputs[i].argument)));
			}
		}
	}

	//! The row's capacity: the least upper end of the box on it. A clause's end is +infinity where its `when` drops the
	//! row or its right side is NULL.
	ExprPtr RowCapacity(const vector<DirectBoundColumns> &clauses) const {
		vector<ExprPtr> uppers;
		if (condition.upper != POSITIVE_INFINITY) {
			uppers.push_back(calc.Num(condition.upper));
		}
		for (auto &clause : clauses) {
			ExprPtr lower, upper;
			DirectClauseBounds(calc, clause, lower, upper);
			uppers.push_back(upper);
		}
		return uppers.empty() ? calc.Num(POSITIVE_INFINITY) : calc.Min(uppers);
	}

	//! The substitute of a plan whose columns are the source's, then x, and the column that keeps the checks live.
	DirectSubstitute Substitute(unique_ptr<LogicalOperator> plan) const {
		DirectSubstitute substitute;
		substitute.prunable_decisions.push_back(1);
		for (idx_t i = 0; i < source_bindings.size() + 1; i++) {
			substitute.slot_map.push_back(i);
		}
		substitute.validated_columns.push_back(source_bindings.size() + 1);
		// The rows are probed against the class plan and held by nothing.
		substitute.pipelined = true;
		substitute.child = std::move(plan);
		return substitute;
	}

	//! Whether the key's row has a decision: no element of the key that puts a row in no class is NULL.
	ExprPtr HasDecision(const Keys &keys) const {
		vector<ExprPtr> columns;
		for (idx_t i = 0; i < keys.decision.size(); i++) {
			columns.push_back(calc.Col(keys.decision_key.types[i], keys.decision[i]));
		}
		return DirectClassPresent(calc, keys.decision_key, columns);
	}

	//! What a row (or a key) with the coefficient `c`, weight `w` and capacity `cap` adds to the sums of its group, in
	//! `stage`: NULL where it does not count (`counts`), and where its term is not in the sum.
	struct Contributions {
		ColumnBinding need;
		ColumnBinding freed;
		ColumnBinding unlimited;
		ColumnBinding allowed;
	};
	Contributions AddContributions(DirectProjectionStage &stage, const ExprPtr &c, const ExprPtr &w, const ExprPtr &cap,
	                               const ExprPtr &counts) const {
		auto no_value = calc.Constant(Value(LogicalType::DOUBLE));
		auto uncapped = calc.Cmp(ExpressionType::COMPARE_EQUAL, cap, calc.Num(POSITIVE_INFINITY));
		auto frees = Negative(w);
		auto counted = [&](const ExprPtr &condition_value, const ExprPtr &value) {
			return stage.Add(calc.Out(calc.If(calc.And(counts, condition_value), value, no_value)));
		};
		Contributions result;
		result.need = counted(calc.And(Positive(w), Positive(c)), calc.Div(w, c));
		result.freed = counted(calc.And(frees, calc.Not(uncapped)), calc.Mul(calc.Neg(w), cap));
		result.unlimited = counted(calc.And(frees, uncapped), calc.Neg(w));
		auto no_level = calc.Cmp(ExpressionType::COMPARE_LESSTHANOREQUALTO, c, calc.Num(0.0));
		result.allowed = counted(calc.Bool(true), calc.If(no_level, calc.Num(0.0), calc.Mul(cap, c)));
		return result;
	}

	//! The end of the range of each of the rows of `plan` that uses the least budget, joined with its group's row
	//! (`budget_keys` are its group key's columns). `present` says whether the row has a decision (null: always).
	Answered JoinGroups(unique_ptr<LogicalOperator> plan, const vector<ColumnBinding> &budget_keys,
	                    const DirectClassKey &budget_key, const ExprPtr &c, const ExprPtr &w, const ExprPtr &cap,
	                    const ExprPtr &present, unique_ptr<LogicalOperator> groups) const {
		auto key = budget_key;
		key.equal.assign(key.equal.size(), false);
		auto joined = DirectJoinClassAnswers(binder, std::move(plan), budget_keys, key, std::move(groups));
		D_ASSERT(joined.answers.size() == 5);
		Answered result;
		result.plan = std::move(joined.plan);
		// A row with no budget takes the level of the objective.
		auto in_budget = Boolean(joined.answers[2]);
		auto level = calc.If(in_budget, Double(joined.answers[0]), Double(joined.answers[3]));
		auto low = calc.If(Positive(c), calc.Div(level, c), calc.Num(0.0));
		auto uncapped = calc.Cmp(ExpressionType::COMPARE_EQUAL, cap, calc.Num(POSITIVE_INFINITY));
		auto freeing = calc.If(uncapped, calc.Max({low, Double(joined.answers[1])}), cap);
		result.amount = calc.If(calc.And(in_budget, Negative(w)), freeing, low);
		// A row with no decision has no amount.
		if (present) {
			result.amount = calc.If(present, result.amount, calc.Constant(Value(LogicalType::DOUBLE)));
		}
		vector<unique_ptr<Expression>> checks;
		checks.push_back(calc.Out(Boolean(joined.answers[4])));
		result.verdict = DirectAllHold(calc, std::move(checks));
		return result;
	}

	//! The weight of a row read from the data, in the projection `p1` (it is the column returned), and its check. The
	//! solver reads a weight on every row the clause's `when` keeps, a row with a NULL key included, and on a row that
	//! has a decision. No `when` is kept for a decision of a key, so the rows read are the latter.
	ColumnBinding LayoutWeight(DirectProjectionStage &p1,
	                           const std::function<unique_ptr<Expression>(const Expression &)> &over,
	                           vector<unique_ptr<Expression>> &checks) const {
		if (condition.constant_weight) {
			return ColumnBinding();
		}
		auto values = DirectValuesOn(condition.weights, 0);
		auto &read = condition.scope.when ? condition.scope.when : condition.present;
		auto kept = read ? calc.Wrap(DirectActiveWhen(calc, over(*read).get())) : nullptr;
		auto weight = p1.Add(calc.Out(DirectOnKeptRows(calc, kept, calc.Wrap(DirectSumOf(optimizer, values, over)))));
		if (auto check =
		        DirectCheckNumber(optimizer, weight, condition.weights.names[0], values, source_bindings, p1.index)) {
			checks.push_back(std::move(check));
		}
		return weight;
	}

	//! [W0] to P3 over a read. `reduced` says the rows are reduced to their keys (BuildKeys), which reads each key's
	//! sums in the columns of its row, so a row adds nothing to its group's sums here.
	Row RowStages(unique_ptr<LogicalOperator> rows, bool reduced) const {
		Row result;
		vector<unique_ptr<Expression>> checks;
		bool scoped = condition.scope.Scoped();
		bool keyed = !condition.keys.empty();

		// W0: the budget's scope, and the decision's key for a decision shared by the rows of a key.
		vector<DirectScopeInput> scopes;
		scopes.push_back({scoped, condition.scope.when.get(), &condition.scope.keys, nullptr});
		scopes.push_back({keyed, nullptr, &condition.keys, nullptr});
		auto state = DirectProjectScopes(binder, std::move(rows), std::move(scopes), {});
		auto from_scope = DirectCarriedOf(state, source_bindings.size());
		auto over = [&](const Expression &value) {
			return state.state_index == DConstants::INVALID_INDEX
			           ? value.Copy()
			           : DirectOverProjection(value, source_bindings, state.state_index);
		};
		auto eligible = scoped ? Boolean(from_scope.eligible) : nullptr;
		auto &decision = state.scopes[1];

		// P1: a value is read and checked on the rows the budget covers alone, nothing is computed on any other.
		DirectProjectionStage p1(binder.GenerateTableIndex());
		auto carried1 = p1.Forward(from_scope, source_types, scoped);
		auto decision1 = p1.Forward(decision, keyed);
		auto clauses = DirectLayoutBoundClauses(optimizer, calc, p1, condition.clauses, source_bindings, over, checks);
		auto laid =
		    DirectLayoutQuadraticPart(optimizer, calc, p1, condition.objective, 1, source_bindings, over, checks);
		auto raw_weight = LayoutWeight(p1, over, checks);
		if (limit_bound) {
			result.limit_inputs = DirectLayoutGroupInputs(optimizer, calc, p1, *limit_bound, eligible, over, &checks);
		}
		auto plan = p1.Finish(std::move(state.plan));

		// P2: a clause's upper end is +infinity where its `when` drops the row or its right side is NULL.
		DirectProjectionStage p2(binder.GenerateTableIndex());
		auto carried2 = p2.Forward(carried1, source_types, scoped);
		auto decision2 = p2.Forward(decision1, keyed);
		auto coefficient = p2.Add(calc.Out(Double(laid.linear[0])));
		ColumnBinding weight;
		if (!condition.constant_weight) {
			weight = p2.Add(calc.Out(Double(raw_weight)));
		}
		ForwardLimitInputs(p2, result.limit_inputs);
		auto capacity = p2.Add(calc.Out(RowCapacity(clauses)));
		auto raw_valid = p2.Add(DirectAllHold(calc, std::move(checks)));
		plan = p2.Finish(std::move(plan));

		// P3: a row the MIN does not read has no `c`, and every CASE on it gives NULL there.
		DirectProjectionStage p3(binder.GenerateTableIndex());
		result.carried = p3.Forward(carried2, source_types, scoped);
		result.decision = p3.Forward(decision2, keyed);
		ForwardLimitInputs(p3, result.limit_inputs);
		result.coefficient = p3.Add(calc.Out(Double(coefficient)));
		if (!condition.constant_weight) {
			result.weight = p3.Add(calc.Out(Double(weight)));
		}
		result.capacity = p3.Add(calc.Out(Double(capacity)));
		if (!reduced) {
			auto contributions =
			    AddContributions(p3, Double(coefficient), Weight(weight), Double(capacity), calc.Bool(true));
			result.need = contributions.need;
			result.freed = contributions.freed;
			result.unlimited = contributions.unlimited;
			result.allowed = contributions.allowed;
		}
		// The checks of the raw values hold first, then the box: a negative capacity leaves no value, on a row that has
		// a decision (the capacity is infinite where it has none).
		vector<unique_ptr<Expression>> row_checks;
		row_checks.push_back(calc.Out(Boolean(raw_valid)));
		row_checks.push_back(DirectRaiseIf(calc, calc.Out(Negative(Double(capacity))),
		                                   DirectInfeasibleErrorPredicate(optimizer, errors)));
		result.valid = p3.Add(DirectAllHold(calc, std::move(row_checks)));
		result.plan = p3.Finish(std::move(plan));
		return result;
	}
};

DirectSubstitute Rule::Apply(DirectSourceReader &reader, Optimizer &optimizer,
                             const DirectApplicabilityCondition &candidate, const DirectStateErrors &errors) const {
	auto &condition = static_cast<const Condition &>(candidate);
	PlanBuilder plan_builder(condition, optimizer, reader.SourceBindings(), reader.SourceTypes(), errors);
	if (condition.keys.empty()) {
		auto groups = DirectBuildOverRead(reader, reader.ReadSource(), [&](unique_ptr<LogicalOperator> rows) {
			return plan_builder.BuildGroups(plan_builder.BuildRows(std::move(rows)));
		});
		return DirectBuildOverOneRead(reader, [&](unique_ptr<LogicalOperator> rows) {
			return plan_builder.JoinAnswers(std::move(rows), std::move(groups));
		});
	}
	// The rows of each key are held: the groups and the answer of each key both read them.
	PlanBuilder::Keys keys;
	auto definition = DirectBuildOverRead(reader, reader.ReadSource(), [&](unique_ptr<LogicalOperator> rows) {
		return plan_builder.BuildKeys(std::move(rows), keys);
	});
	definition->ResolveOperatorTypes();
	DirectHeldRows held(optimizer.binder, "direct_r4_keys", definition->GetColumnBindings(), definition->types);
	auto groups = plan_builder.BuildGroups(plan_builder.KeyRows(held, keys));
	auto answers = plan_builder.BuildKeyAnswers(held, keys, std::move(groups));
	return DirectBuildOverOneRead(reader, [&](unique_ptr<LogicalOperator> rows) {
		auto substitute = plan_builder.JoinKeyAnswers(std::move(rows), std::move(answers));
		substitute.child = held.Close(std::move(definition), std::move(substitute.child));
		return substitute;
	});
}

} // namespace direct_r4_max_min_fairness

unique_ptr<DirectSolveRule> MakeR4MaxMinFairnessRule() {
	return make_uniq<direct_r4_max_min_fairness::Rule>();
}

} // namespace duckdb
