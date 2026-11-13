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
//! a value of the row or absent; one budget `SUM(w*x) <= B` with the constants `w > 0` and `B`; and an objective
//! `MAXIMIZE MIN(c*x)` over one term whose coefficient `c` is a value of the row. The objective is the canonical form
//! of `MAXIMIZE MIN(x / demand)`: `c = 1 / demand`.
struct Condition final : DirectApplicabilityCondition {
	//! The box's upper end before any per-row bound, and the per-row clauses the plan evaluates: each an upper end.
	double upper = POSITIVE_INFINITY;
	vector<DirectBoundClause> clauses;
	//! The MIN's one term `c*x`, and the rows it reads (`kept`).
	DirectQuadraticPart objective;
	//! The budget `SUM(w*x) <= B`.
	double weight = 0.0;
	double budget = 0.0;
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

//! The budget `SUM(w*x) <= B` has a constant weight above 0 and a constant limit.
bool Rule::ProveBudget(const DirectIR &ir, ClientContext &context, Condition &condition) const {
	auto budget = FindBudget(ir);
	if (!budget) {
		return false;
	}
	// FindBudget has proved that the clause is a plain SUM.
	auto &sum = *budget->PlainSum();
	DirectLinearPart weights;
	DirectLimit limit;
	if (!DirectOnlyLinearTerms(sum.terms) || !DirectReadLinearPart(ir, context, sum, budget->written, weights) ||
	    !DirectFoldConstantSum(context, DirectValuesOn(weights, 0), condition.weight) || !(condition.weight > 0.0) ||
	    !DirectReadLimit(ir, context, *budget, limit) || !limit.constant) {
		return false;
	}
	condition.budget = limit.number;
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
//! over two reads of the source, with one class, the whole table. A row `i` that the MIN reads has the coefficient `c`;
//! at level `s` it must hold `x >= s/c`, so the budget needs `w*s/c` of it, and `x <= cap` gives `s <= cap*c`. A row
//! with `c <= 0` caps the level at 0, since `c*x <= 0`. The level is the least of what the budget and every capacity
//! allow, and every row that the MIN reads takes `s/c`, the end of its range that uses the least budget. A row the MIN
//! does not read takes 0.
//!
//! The first read, on each row:
//!
//!   P1  raw values: the capacity clauses' right sides and the MIN's coefficient `c`, each on the rows that read it,
//!       and the check of every value that can fail
//!   P2  the row's capacity (the least upper end of the box on it) and the verdict of the raw checks
//!   P3  `w/c` (the budget the row needs per unit of level), `cap*c` (the level its capacity allows, 0 for `c <= 0`),
//!       and every other check the row can raise
//!   H   a hash aggregate with no key, one row: the sum of `w/c`, the least of `cap*c`, and whether the checks held
//!   C   the level `s = least(B / sum, least)`, computed once
//!
//! The second read computes P1 to P3 again for its `c`, joined with the level's row, then
//!
//!   P4  the answer `s/c`, and the column that keeps the checks live
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

	//! The level's plan over the first read `rows`: one row, the level and whether the checks held.
	unique_ptr<LogicalOperator> BuildLevel(unique_ptr<LogicalOperator> rows) const {
		auto row = RowStages(std::move(rows));
		DirectAggregateStage aggregate(binder);
		auto need = aggregate.Add(DirectHashAggregate(optimizer, calc, "sum", Double(row.need)));
		auto allowed = aggregate.Add(DirectHashAggregate(optimizer, calc, "min", Double(row.allowed)));
		auto valid = aggregate.Add(DirectHashAggregate(optimizer, calc, "bool_and", Boolean(row.valid)));
		auto plan = aggregate.Finish(std::move(row.plan));

		// With no row the MIN reads, `need` and `allowed` are NULL, the level is NULL and no row reads it. Adding 0.0
		// turns a -0.0 level (a limit of -0.0) into 0.0.
		DirectProjectionStage level(binder.GenerateTableIndex());
		auto from_budget = calc.Div(calc.Num(condition.budget), Double(need));
		level.Add(calc.Add(calc.Min({from_budget, Double(allowed)}), calc.Num(0.0)));
		level.Add(Boolean(valid));
		return level.Finish(std::move(plan));
	}

	//! The substitute over the second read `rows`: its source columns, then each row's answer, and the column that
	//! keeps the level plan's checks live (P4).
	DirectSubstitute JoinAnswers(unique_ptr<LogicalOperator> rows, unique_ptr<LogicalOperator> level) const {
		auto row = RowStages(std::move(rows));
		auto joined =
		    DirectJoinClassAnswers(binder, std::move(row.plan), {}, DirectClassKeyOf({}, false), std::move(level));
		D_ASSERT(joined.answers.size() == 2);

		DirectProjectionStage p4(binder.GenerateTableIndex());
		for (idx_t i = 0; i < source_bindings.size(); i++) {
			p4.Add(calc.Out(calc.Col(source_types[i], row.source[i])));
		}
		auto coefficient = Double(row.coefficient);
		auto amount = calc.If(Positive(coefficient), calc.Div(Double(joined.answers[0]), coefficient), calc.Num(0.0));
		p4.Add(calc.Out(amount));
		vector<unique_ptr<Expression>> checks;
		checks.push_back(calc.Out(Boolean(joined.answers[1])));
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
		//! `w/c` and `cap*c`, NULL on a row the MIN does not read; and whether the row's checks held.
		ColumnBinding need;
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
		auto plan = p1.Finish(std::move(rows));

		// P2: a clause's upper end is +infinity where its `when` drops the row or its right side is NULL.
		DirectProjectionStage p2(binder.GenerateTableIndex());
		auto source2 = p2.Forward(source, source_types);
		auto coefficient = p2.Add(calc.Out(Double(laid.linear[0])));
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

		// P3: a row the MIN does not read has no `c`, and both CASEs give NULL there.
		DirectProjectionStage p3(binder.GenerateTableIndex());
		result.source = p3.Forward(source2, source_types);
		result.coefficient = p3.Add(calc.Out(Double(coefficient)));
		auto c = Double(coefficient);
		auto no_value = calc.Constant(Value(LogicalType::DOUBLE));
		result.need = p3.Add(calc.Out(calc.If(Positive(c), calc.Div(calc.Num(condition.weight), c), no_value)));
		auto no_level = calc.Cmp(ExpressionType::COMPARE_LESSTHANOREQUALTO, c, calc.Num(0.0));
		result.allowed = p3.Add(calc.Out(calc.If(no_level, calc.Num(0.0), calc.Mul(Double(capacity), c))));
		// The checks of the raw values hold first, then the box: a negative capacity leaves no value, and so does a
		// negative budget, since every `w*x` is at least 0.
		vector<unique_ptr<Expression>> row_checks;
		row_checks.push_back(calc.Out(Boolean(raw_valid)));
		auto negative_capacity = calc.Cmp(ExpressionType::COMPARE_LESSTHAN, Double(capacity), calc.Num(0.0));
		row_checks.push_back(
		    DirectRaiseIf(calc, calc.Out(negative_capacity), DirectInfeasibleErrorPredicate(optimizer, errors)));
		auto negative_budget = calc.Cmp(ExpressionType::COMPARE_LESSTHAN, calc.Num(condition.budget), calc.Num(0.0));
		row_checks.push_back(
		    DirectRaiseIf(calc, calc.Out(negative_budget), DirectInfeasibleErrorPredicate(optimizer, errors)));
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
