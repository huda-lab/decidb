#include "duckdb/optimizer/decide/direct/direct_rules.hpp"

#include "duckdb/optimizer/decide/direct/direct_bound_reader.hpp"
#include "duckdb/optimizer/decide/direct/direct_expression.hpp"
#include "duckdb/optimizer/decide/direct/direct_plan_fragments.hpp"
#include "duckdb/optimizer/decide/direct/direct_runtime_checks.hpp"
#include "duckdb/optimizer/decide/direct/direct_source_reader.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"

namespace duckdb {

//! Each rule keeps its helpers in its own named namespace: unity builds compile every rule file in one translation
//! unit, where two anonymous namespaces would merge and same-named helpers would collide.
namespace direct_r4_max_min_fairness {

constexpr const char *RULE_NAME = "R4_MAX_MIN_FAIRNESS";

//! What CheckCondition proved: one row-scoped REAL decision `x` in the box [0, cap], where the capacity is a constant,
//! a value of the row or absent; one budget `SUM(w*x) <= B` with a weight `w` of any sign and a constant `B`; and an
//! objective `MAXIMIZE MIN(c*x)` over one term whose coefficient `c` is a value of the row. The objective is the
//! canonical form of `MAXIMIZE MIN(x / demand)`: `c = 1 / demand`.
struct Condition final : DirectApplicabilityCondition {
	//! The box's upper end before any per-row bound, and the per-row clauses the plan evaluates: each an upper end.
	double upper = POSITIVE_INFINITY;
	vector<DirectBoundClause> clauses;
	//! The MIN's one term `c*x`, and the rows it reads (`kept`).
	DirectQuadraticPart objective;
	//! The budget `SUM(w*x) <= B`: the limit `B`, and the weight of a row. A weight that folds to a constant is the
	//! literal `weight`, which the plan builds with (every expression over it folds); otherwise it is the sum of the
	//! terms in `weights.linear`, a value of the row.
	double budget = 0.0;
	bool constant_weight = true;
	double weight = 0.0;
	DirectQuadraticPart weights;
};

class Rule final : public DirectSolveRule {
public:
	const char *Name() const override {
		return RULE_NAME;
	}

	unique_ptr<DirectApplicabilityCondition> CheckCondition(const DirectIR &ir, ClientContext &context) const override {
		if (!DirectIsOneRowDecision(ir, DirectDomain::REAL)) {
			return nullptr;
		}
		auto condition = make_uniq<Condition>();
		// The level plan reads the source, and the rows read it again to be given the level.
		condition->source_reads = 2;
		if (!ProveBudget(ir, context, *condition) || !ProveBox(ir, context, *condition) ||
		    !ProveObjective(ir, context, *condition)) {
			return nullptr;
		}
		return std::move(condition);
	}

	string Summary() const override {
		return "max MIN(c*x), 0 <= x <= cap, SUM(w*x) <= B";
	}

	DirectSubstitute Apply(DirectSourceReader &reader, Optimizer &optimizer,
	                       const DirectApplicabilityCondition &candidate,
	                       const DirectStateErrors &errors) const override;

private:
	bool ProveBudget(const DirectIR &ir, ClientContext &context, Condition &condition) const;
	bool ProveBox(const DirectIR &ir, ClientContext &context, Condition &condition) const;
	bool ProveObjective(const DirectIR &ir, ClientContext &context, Condition &condition) const;
};

//! The one aggregate clause, a `SUM <= B` over every row: the others are per-row bounds on the decision, which ProveBox
//! reads. Null when there is not exactly one.
static const DirectIRConstraint *FindBudget(const DirectIR &ir) {
	const DirectIRConstraint *budget = nullptr;
	for (auto &ir_node : ir.constraints) {
		if (!ir_node.aggregate) {
			if (!ir_node.singleton_row) {
				return nullptr;
			}
			continue;
		}
		if (budget) {
			return nullptr;
		}
		budget = &ir_node;
	}
	if (!budget || budget->comparison != ExpressionType::COMPARE_LESSTHANOREQUALTO) {
		return nullptr;
	}
	DirectSharedScope scope;
	if (!DirectProveSharedScope(ir, {budget}, scope) || scope.Scoped() || scope.per) {
		return nullptr;
	}
	return budget;
}

//! The budget `SUM(w*x) <= B` has a weight of any sign, a constant or read from the data, and a constant limit.
bool Rule::ProveBudget(const DirectIR &ir, ClientContext &context, Condition &condition) const {
	auto budget = FindBudget(ir);
	if (!budget) {
		return false;
	}
	// FindBudget has proved that the clause is a plain SUM.
	auto &sum = *budget->PlainSum();
	DirectLimit limit;
	if (!DirectOnlyLinearTerms(sum.terms) ||
	    !DirectReadLinearPart(ir, context, sum, budget->written, condition.weights.linear) ||
	    !DirectReadLimit(ir, context, *budget, limit) || !limit.constant) {
		return false;
	}
	condition.budget = limit.number;
	condition.constant_weight =
	    DirectFoldConstantSum(context, DirectValuesOn(condition.weights.linear, 0), condition.weight);
	return true;
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
//! over two reads of the source, with one class, the whole table. A row the MIN reads has the coefficient `c`; at level
//! `s` it must hold `x >= s/c`, and `x <= cap` gives `s <= cap*c`. A row with `c <= 0` caps the level at 0, since
//! `c*x <= 0`. Each row takes the end of its range that uses the least budget, which its weight `w` decides:
//!
//!  - `w >= 0`: the low end, `s/c` for a row the MIN reads and 0 for any other. A weight of 0 is no case of its own,
//!    since `w*x` adds nothing.
//!  - `w < 0`: its capacity `cap`, which frees the budget `-w*cap`. A row with no capacity (NULL, or 1e20 or more)
//!    frees as much as it is asked to: the budget is then unlimited, and the level is the least `cap*c` alone.
//!
//! With `N` the sum of `w/c` over the rows the MIN reads with `w > 0` and `c > 0`, `F` the freed budget of the capped
//! rows with `w < 0`, and `W` the sum of `-w` over the uncapped rows with `w < 0`, the level is `s = least(B+F over N,
//! U)` (just `U` when `W > 0`), `U` being the least `cap*c`. The budget is infeasible when `B+F < 0` and `W = 0`: every
//! other row needs at least 0. An uncapped row with `w < 0` takes its low end, or more when the other rows need
//! budget it must free: `max(lo, max(0, N*s - (B+F)) / W)`; taking more than the deficit over `W` only frees more.
//! A level that is infinite (no row bounds it) is unbounded. A row the MIN does not read has no `c`.
//!
//! The first read, on each row:
//!
//!   P1  raw values: the capacity clauses' right sides, the budget's weight and the MIN's coefficient `c`, each on the
//!       rows that read it, and the check of every value that can fail
//!   P2  the row's capacity (the least upper end of the box on it) and the verdict of the raw checks
//!   P3  `w/c`, the freed budget, the unlimited weight and `cap*c` (0 for `c <= 0`), each NULL where the row does not
//!       count, and every other check the row can raise
//!   H   a hash aggregate with no key, one row: the sums N, F and W, the least U, and whether the checks held
//!   C   the level, the deficit's share of one unit of `W`, and the checks of the class (infeasible, unbounded)
//!       together with the rows' verdict, computed once
//!
//! The second read computes P1 to P3 again for its `c`, `w` and `cap`, joined with the class's row, then
//!
//!   P4  the answer, and the column that keeps the checks live
//!
//! The solver reads every row before it returns, so a bad value on the last row raises even under an outer LIMIT 1. H
//! holds every row of the first read before it gives one, and each check stands below it, so no joined row exists
//! before every row has passed its checks. The second read raises nothing: the first has raised on any value it would.
class PlanBuilder {
public:
	PlanBuilder(const Condition &condition_p, Optimizer &optimizer_p, const vector<ColumnBinding> &source_bindings_p,
	            const vector<LogicalType> &source_types_p, const DirectStateErrors &errors_p)
	    : condition(condition_p), optimizer(optimizer_p), binder(optimizer_p.binder), calc(optimizer_p.context),
	      source_bindings(source_bindings_p), source_types(source_types_p), errors(errors_p) {
		D_ASSERT(source_bindings.size() == source_types.size());
	}

	//! The class's plan over the first read `rows`: one row, the level, the deficit's share and whether the checks
	//! held.
	unique_ptr<LogicalOperator> BuildLevel(unique_ptr<LogicalOperator> rows) const {
		auto row = RowStages(std::move(rows));
		DirectAggregateStage aggregate(binder);
		auto need = aggregate.Add(DirectHashAggregate(optimizer, calc, "sum", Double(row.need)));
		auto freed = aggregate.Add(DirectHashAggregate(optimizer, calc, "sum", Double(row.freed)));
		auto unlimited = aggregate.Add(DirectHashAggregate(optimizer, calc, "sum", Double(row.unlimited)));
		auto allowed = aggregate.Add(DirectHashAggregate(optimizer, calc, "min", Double(row.allowed)));
		auto valid = aggregate.Add(DirectHashAggregate(optimizer, calc, "bool_and", Boolean(row.valid)));
		auto plan = aggregate.Finish(std::move(row.plan));

		// With no row the MIN reads, `need` and `allowed` are NULL, the level is NULL and no row reads it. Adding 0.0
		// turns a -0.0 level (a limit of -0.0) into 0.0. An unlimited budget has no level of its own, and NULL is what
		// LEAST skips.
		auto zero = calc.Num(0.0);
		auto no_value = calc.Constant(Value(LogicalType::DOUBLE));
		auto unlimited_weight = calc.Coalesce({Double(unlimited), zero});
		auto is_unlimited = Positive(unlimited_weight);
		auto budget = calc.Add(calc.Num(condition.budget), calc.Coalesce({Double(freed), zero}));
		auto from_budget = calc.If(is_unlimited, no_value, calc.Div(budget, Double(need)));
		auto level_value = calc.Add(calc.Min({from_budget, Double(allowed)}), zero);
		DirectProjectionStage level(binder.GenerateTableIndex());
		level.Add(level_value);
		// An uncapped row with `w < 0` frees the budget the others' low ends need beyond B+F.
		auto deficit = calc.Max({zero, calc.Sub(calc.Mul(Double(need), level_value), budget)});
		level.Add(calc.If(is_unlimited, calc.Div(deficit, unlimited_weight), no_value));
		// The class's checks follow the rows': no budget is left for rows that need at least 0 when B+F is negative and
		// nothing frees more, and a level nothing bounds improves without limit. Both read the class's row, so an empty
		// source, which has no row to raise on, raises nothing.
		vector<unique_ptr<Expression>> checks;
		checks.push_back(calc.Out(Boolean(valid)));
		auto has_rows = calc.Not(calc.IsNull(Boolean(valid)));
		auto no_budget = calc.And(
		    has_rows, calc.And(calc.Not(is_unlimited), calc.Cmp(ExpressionType::COMPARE_LESSTHAN, budget, zero)));
		checks.push_back(DirectRaiseIf(calc, calc.Out(no_budget), DirectInfeasibleErrorPredicate(optimizer, errors)));
		auto no_bound = calc.Cmp(ExpressionType::COMPARE_EQUAL, level_value, calc.Num(POSITIVE_INFINITY));
		checks.push_back(DirectRaiseIf(calc, calc.Out(no_bound), DirectUnboundedErrorPredicate(optimizer, errors)));
		level.Add(DirectAllHold(calc, std::move(checks)));
		return level.Finish(std::move(plan));
	}

	//! The substitute over the second read `rows`: its source columns, then each row's answer, and the column that
	//! keeps the level plan's checks live (P4).
	DirectSubstitute JoinAnswers(unique_ptr<LogicalOperator> rows, unique_ptr<LogicalOperator> level) const {
		auto row = RowStages(std::move(rows));
		auto joined =
		    DirectJoinClassAnswers(binder, std::move(row.plan), {}, DirectClassKeyOf({}, false), std::move(level));
		D_ASSERT(joined.answers.size() == 3);

		DirectProjectionStage p4(binder.GenerateTableIndex());
		for (idx_t i = 0; i < source_bindings.size(); i++) {
			p4.Add(calc.Out(calc.Col(source_types[i], row.source[i])));
		}
		// The end of the row's range that uses the least budget.
		auto coefficient = Double(row.coefficient);
		auto low = calc.If(Positive(coefficient), calc.Div(Double(joined.answers[0]), coefficient), calc.Num(0.0));
		auto capacity = Double(row.capacity);
		auto uncapped = calc.Cmp(ExpressionType::COMPARE_EQUAL, capacity, calc.Num(POSITIVE_INFINITY));
		auto freeing = calc.If(uncapped, calc.Max({low, Double(joined.answers[1])}), capacity);
		auto amount = calc.If(Negative(Weight(row.weight)), freeing, low);
		p4.Add(calc.Out(amount));
		vector<unique_ptr<Expression>> checks;
		checks.push_back(calc.Out(Boolean(joined.answers[2])));
		p4.Add(DirectAllHold(calc, std::move(checks)));

		DirectSubstitute substitute;
		substitute.prunable_decisions.push_back(1);
		// The source columns, then x. The level plan's checks are read by the column after them.
		for (idx_t i = 0; i < source_bindings.size() + 1; i++) {
			substitute.slot_map.push_back(i);
		}
		substitute.validated_columns.push_back(source_bindings.size() + 1);
		// The rows are probed against the level plan and held by nothing.
		substitute.pipelined = true;
		substitute.child = p4.Finish(std::move(joined.plan));
		return substitute;
	}

private:
	//! P1 to P3 over a read: the plan, where its columns are.
	struct Row {
		unique_ptr<LogicalOperator> plan;
		vector<ColumnBinding> source;
		//! The MIN's coefficient `c`: NULL on a row the MIN does not read.
		ColumnBinding coefficient;
		//! The row's weight `w`, a column only for a weight read from the data (Weight), and its capacity.
		ColumnBinding weight;
		ColumnBinding capacity;
		//! What the row adds to each sum of the class (BuildLevel): `w/c`, the freed budget, the unlimited weight and
		//! `cap*c`, NULL where the row does not count; and whether the row's checks held.
		ColumnBinding need;
		ColumnBinding freed;
		ColumnBinding unlimited;
		ColumnBinding allowed;
		ColumnBinding valid;
	};

	const Condition &condition;
	Optimizer &optimizer;
	Binder &binder;
	ExpressionBuilder calc;
	vector<ColumnBinding> source_bindings;
	vector<LogicalType> source_types;
	const DirectStateErrors &errors;

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
	//! The row's weight: the literal of a constant weight, which DuckDB's rewriter folds into every expression over it,
	//! or the column `weight`.
	ExprPtr Weight(ColumnBinding weight) const {
		return condition.constant_weight ? calc.Num(condition.weight) : Double(weight);
	}

	Row RowStages(unique_ptr<LogicalOperator> rows) const {
		Row result;
		vector<unique_ptr<Expression>> checks;

		// P1
		DirectProjectionStage p1(binder.GenerateTableIndex());
		auto source = p1.Forward(source_bindings, source_types);
		auto clauses =
		    DirectLayoutBoundClauses(optimizer, calc, p1, condition.clauses, source_bindings, nullptr, checks);
		auto laid =
		    DirectLayoutQuadraticPart(optimizer, calc, p1, condition.objective, 1, source_bindings, nullptr, checks);
		DirectQuadraticColumns laid_weights;
		if (!condition.constant_weight) {
			laid_weights =
			    DirectLayoutQuadraticPart(optimizer, calc, p1, condition.weights, 1, source_bindings, nullptr, checks);
		}
		auto plan = p1.Finish(std::move(rows));

		// P2: a clause's upper end is +infinity where its `when` drops the row or its right side is NULL.
		DirectProjectionStage p2(binder.GenerateTableIndex());
		auto source2 = p2.Forward(source, source_types);
		auto coefficient = p2.Add(calc.Out(Double(laid.linear[0])));
		ColumnBinding weight;
		if (!condition.constant_weight) {
			weight = p2.Add(calc.Out(Double(laid_weights.linear[0])));
		}
		vector<ExprPtr> uppers;
		if (condition.upper != POSITIVE_INFINITY) {
			uppers.push_back(calc.Num(condition.upper));
		}
		for (auto &clause : clauses) {
			ExprPtr lower, upper;
			DirectClauseBounds(calc, clause, lower, upper);
			uppers.push_back(upper);
		}
		auto capacity = p2.Add(calc.Out(uppers.empty() ? calc.Num(POSITIVE_INFINITY) : calc.Min(uppers)));
		auto raw_valid = p2.Add(DirectAllHold(calc, std::move(checks)));
		plan = p2.Finish(std::move(plan));

		// P3: a row the MIN does not read has no `c`, and every CASE on it gives NULL there.
		DirectProjectionStage p3(binder.GenerateTableIndex());
		result.source = p3.Forward(source2, source_types);
		result.coefficient = p3.Add(calc.Out(Double(coefficient)));
		if (!condition.constant_weight) {
			result.weight = p3.Add(calc.Out(Double(weight)));
		}
		result.capacity = p3.Add(calc.Out(Double(capacity)));
		auto c = Double(coefficient);
		auto w = Weight(weight);
		auto cap = Double(capacity);
		auto no_value = calc.Constant(Value(LogicalType::DOUBLE));
		auto uncapped = calc.Cmp(ExpressionType::COMPARE_EQUAL, cap, calc.Num(POSITIVE_INFINITY));
		auto frees = Negative(w);
		result.need = p3.Add(calc.Out(calc.If(calc.And(Positive(w), Positive(c)), calc.Div(w, c), no_value)));
		result.freed =
		    p3.Add(calc.Out(calc.If(calc.And(frees, calc.Not(uncapped)), calc.Mul(calc.Neg(w), cap), no_value)));
		result.unlimited = p3.Add(calc.Out(calc.If(calc.And(frees, uncapped), calc.Neg(w), no_value)));
		auto no_level = calc.Cmp(ExpressionType::COMPARE_LESSTHANOREQUALTO, c, calc.Num(0.0));
		result.allowed = p3.Add(calc.Out(calc.If(no_level, calc.Num(0.0), calc.Mul(cap, c))));
		// The checks of the raw values hold first, then the box: a negative capacity leaves no value.
		vector<unique_ptr<Expression>> row_checks;
		row_checks.push_back(calc.Out(Boolean(raw_valid)));
		auto negative_capacity = Negative(cap);
		row_checks.push_back(
		    DirectRaiseIf(calc, calc.Out(negative_capacity), DirectInfeasibleErrorPredicate(optimizer, errors)));
		result.valid = p3.Add(DirectAllHold(calc, std::move(row_checks)));
		result.plan = p3.Finish(std::move(plan));
		return result;
	}
};

DirectSubstitute Rule::Apply(DirectSourceReader &reader, Optimizer &optimizer,
                             const DirectApplicabilityCondition &candidate, const DirectStateErrors &errors) const {
	auto &condition = static_cast<const Condition &>(candidate);
	PlanBuilder plan_builder(condition, optimizer, reader.SourceBindings(), reader.SourceTypes(), errors);
	auto level = DirectBuildOverRead(reader, reader.ReadSource(), [&](unique_ptr<LogicalOperator> rows) {
		return plan_builder.BuildLevel(std::move(rows));
	});
	return DirectBuildOverOneRead(reader, [&](unique_ptr<LogicalOperator> rows) {
		return plan_builder.JoinAnswers(std::move(rows), std::move(level));
	});
}

} // namespace direct_r4_max_min_fairness

unique_ptr<DirectSolveRule> MakeR4MaxMinFairnessRule() {
	return make_uniq<direct_r4_max_min_fairness::Rule>();
}

} // namespace duckdb
