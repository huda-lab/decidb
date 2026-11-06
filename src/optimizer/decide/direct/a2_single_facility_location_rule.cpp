#include "duckdb/optimizer/decide/direct/direct_rules.hpp"

#include <cmath>

#include "duckdb/common/decide_numeric_policy.hpp"
#include "duckdb/optimizer/decide/direct/direct_bound_reader.hpp"
#include "duckdb/optimizer/decide/direct/direct_expression.hpp"
#include "duckdb/optimizer/decide/direct/direct_plan_fragments.hpp"
#include "duckdb/optimizer/decide/direct/direct_runtime_checks.hpp"
#include "duckdb/optimizer/decide/direct/direct_source_reader.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/decide/decide_source_provenance.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_window.hpp"

namespace duckdb {

//! Each rule keeps its helpers in its own named namespace: unity builds compile every rule file in one translation
//! unit, where two anonymous namespaces would merge and same-named helpers would collide.
namespace direct_a2_single_facility_location {

constexpr const char *RULE_NAME = "A2_SINGLE_FACILITY_LOCATION";

//! What CheckCondition proved: one query-wide REAL decision `c` in a constant interval [lower, upper], and an objective
//! that is one square `K * (w*c + t)^2` of a linear form of `c` and a source value, with `w` a nonzero constant and
//! `K` positive once the sense is applied. The square is summed over the rows the objective keeps, so the objective is
//! `a*c^2 + b*c` with `a` the sum of `K*w^2` and `b` the sum of `2*K*w*t`, and its minimum over the interval is the
//! turning point `-b / (2*a)` clamped into it.
struct Condition final : DirectApplicabilityCondition {
	double lower = 0.0;
	double upper = POSITIVE_INFINITY;
	DecideSense sense = DecideSense::FEASIBILITY;
	//! The objective as a message names it: a number computed past a DOUBLE names it, as the solver path names one.
	string objective_written;
	//! The one part of the objective, in a vector because DirectFoldQuadratic reads a list of parts.
	vector<DirectQuadraticPart> parts;
};

class Rule final : public DirectSolveRule {
public:
	const char *Name() const override {
		return RULE_NAME;
	}

	unique_ptr<DirectApplicabilityCondition> CheckCondition(const DirectIR &ir, ClientContext &context) const override {
		if (ir.decisions.size() != 1 || ir.decisions[0].domain != DirectDomain::REAL ||
		    ir.decisions[0].scope != DecideVarScope::GLOBAL) {
			return nullptr;
		}
		auto condition = make_uniq<Condition>();
		condition->sense = ir.objective.sense;
		condition->objective_written = ir.objective.written;
		if (!ProveInterval(ir, context, *condition) || !ProveObjective(ir, context, *condition)) {
			return nullptr;
		}
		return std::move(condition);
	}

	string Summary() const override {
		return "min SUM((c - v)^2), lo <= c <= hi, one shared c";
	}

	DirectSubstitute Apply(DirectSourceReader &reader, Optimizer &optimizer,
	                       const DirectApplicabilityCondition &candidate,
	                       const DirectStateErrors &errors) const override;

private:
	bool ProveInterval(const DirectIR &ir, ClientContext &context, Condition &condition) const;
	bool ProveObjective(const DirectIR &ir, ClientContext &context, Condition &condition) const;
};

//! Every constraint is a bound on `c` known before the query runs, so the solver folds them into one interval (the
//! reader's static box). A bound read from the data, or under a `when`, is a clause the plan evaluates per row, which
//! this class does not take. An empty interval is not declined: the plan raises the solver's infeasible error.
bool Rule::ProveInterval(const DirectIR &ir, ClientContext &context, Condition &condition) const {
	for (auto &ir_node : ir.constraints) {
		if (!DirectIsSingletonRow(ir, ir_node)) {
			return false;
		}
	}
	DirectBoundReading reading;
	if (!DirectReadBounds(ir, context, reading) || !reading.clauses.empty()) {
		return false;
	}
	condition.lower = reading.boxes[0].lower;
	condition.upper = reading.boxes[0].upper;
	return true;
}

//! One plain `SUM` of one square, and nothing else: no linear term, no per-row constant, no second part. The square's
//! slope folds to a nonzero constant, and the square is convex once the sense, the part's sign and its factor are
//! applied. The rows it reads (the objective's `when` and the part's own) are the part's `kept`.
bool Rule::ProveObjective(const DirectIR &ir, ClientContext &context, Condition &condition) const {
	if (ir.objective.parts.size() != 1 || !ir.objective.parts[0].IsPlainSum()) {
		return false;
	}
	DirectQuadraticPart part;
	if (!DirectReadQuadraticPart(ir, context, ir.objective.parts[0], ir.objective.written, part)) {
		return false;
	}
	if (part.squares.size() != 1 || !part.linear.terms.empty() || !part.constants.empty()) {
		return false;
	}
	auto &square = part.squares[0];
	double slope;
	if (!DirectFoldConstantSum(context, square.inner_linear, slope) || slope == 0.0) {
		return false;
	}
	auto &linear = part.linear;
	double part_factor = linear.scaled ? (linear.divides ? 1.0 / linear.scale : linear.scale) : 1.0;
	double sense = ir.objective.sense == DecideSense::MAXIMIZE ? -1.0 : 1.0;
	double oriented = sense * linear.sign * part_factor * (square.sign * square.scale * (slope * slope));
	if (!std::isfinite(oriented) || oriented <= 0.0) {
		return false;
	}
	condition.parts.push_back(std::move(part));
	return true;
}

//===--------------------------------------------------------------------===//
// Plan
//===--------------------------------------------------------------------===//

//! Builds the relational plan for one A2 problem whose condition holds, over one read of the source. Each stage reads
//! only the columns of the stage before it:
//!
//!   P1 raw values: the rows the objective keeps, and the slope `w` and constant `t` of the square on each, with the
//!      check of every value that can fail
//!   P2 the row's `a` and `b` (zero on a row the objective drops), oriented as a minimum, and the row's checks
//!   W  the sums of `a` and `b` over every row (one window over the whole input)
//!   P3 the answer: the turning point `-b / (2*a)` clamped into the interval, repeated on every row, and the column
//!      that keeps the checks live
//!
//! Aggregate-then-repeat: the window is the aggregate and the row's own columns are what it repeats on, so the source
//! is read once. The window holds every row before it releases one, so each check on a row (P2) has run on all of them
//! first, as the solver reads every row before it returns.
class PlanBuilder {
public:
	PlanBuilder(const Condition &condition_p, Optimizer &optimizer_p, const vector<ColumnBinding> &source_bindings_p,
	            const vector<LogicalType> &source_types_p, const DirectStateErrors &errors_p)
	    : condition(condition_p), optimizer(optimizer_p), binder(optimizer_p.binder), calc(optimizer_p.context),
	      source_bindings(source_bindings_p), source_types(source_types_p), errors(errors_p) {
		D_ASSERT(source_bindings.size() == source_types.size());
	}

	DirectSubstitute Build(unique_ptr<LogicalOperator> rows) const {
		vector<unique_ptr<Expression>> checks;
		DirectProjectionStage p1(binder.GenerateTableIndex());
		vector<ColumnBinding> source;
		for (idx_t i = 0; i < source_bindings.size(); i++) {
			source.push_back(p1.Add(calc.Out(calc.Col(source_types[i], source_bindings[i]))));
		}
		auto laid =
		    DirectLayoutQuadraticPart(optimizer, calc, p1, condition.parts[0], 1, source_bindings, nullptr, checks);
		auto plan = p1.Finish(std::move(rows));

		DirectProjectionStage p2(binder.GenerateTableIndex());
		vector<ColumnBinding> carried;
		for (idx_t i = 0; i < source_bindings.size(); i++) {
			carried.push_back(p2.Add(calc.Out(calc.Col(source_types[i], source[i]))));
		}
		auto sum = DirectFoldQuadratic(optimizer, calc, condition.parts, {laid}, 0);
		auto sense = calc.Num(condition.sense == DecideSense::MAXIMIZE ? -1.0 : 1.0);
		auto a = p2.Add(calc.Out(calc.Mul(sense, sum.a)));
		auto b = p2.Add(calc.Out(calc.Mul(sense, sum.b)));
		// Every check is true or raises; a row passes when all of them do.
		auto valid = p2.Add(DirectAllHold(calc, std::move(checks)));
		plan = p2.Finish(std::move(plan));

		auto window_index = binder.GenerateTableIndex();
		auto window = make_uniq<LogicalWindow>(window_index);
		window->expressions.push_back(
		    DirectWindowExtremum(optimizer, "sum", calc.Out(calc.Col(LogicalType::DOUBLE, a))));
		window->expressions.push_back(
		    DirectWindowExtremum(optimizer, "sum", calc.Out(calc.Col(LogicalType::DOUBLE, b))));
		window->children.push_back(std::move(plan));

		// A row the objective drops has `a = b = 0`, never NULL, so every row carries a sum.
		auto total_a = calc.Col(LogicalType::DOUBLE, ColumnBinding(window_index, 0));
		auto total_b = calc.Col(LogicalType::DOUBLE, ColumnBinding(window_index, 1));
		auto zero = calc.Num(0.0);
		// Convex: the turning point, clamped into the interval. With no square on any row every value is optimal, and
		// the point of the interval nearest 0 is the answer.
		auto turning = DirectTurningPoint(calc, total_a, total_b, false);
		auto target = calc.If(calc.Cmp(ExpressionType::COMPARE_GREATERTHAN, total_a, zero), turning.target, zero);
		auto clamped = calc.Min({calc.Max({target, calc.Num(condition.lower)}), calc.Num(condition.upper)});

		DirectProjectionStage p3(binder.GenerateTableIndex());
		for (idx_t i = 0; i < source_bindings.size(); i++) {
			p3.Add(calc.Out(calc.Col(source_types[i], carried[i])));
		}
		// Adding 0.0 turns -0.0 into 0.0.
		p3.Add(calc.Out(calc.Add(clamped, zero)));
		vector<unique_ptr<Expression>> answer_checks;
		answer_checks.push_back(calc.Out(calc.Col(LogicalType::BOOLEAN, valid)));
		// The interval is two constants, so DuckDB folds this check: it is true, or raises on every row.
		answer_checks.push_back(
		    DirectRaiseIf(calc, calc.Out(DirectBoxIsEmpty(calc, calc.Num(condition.lower), calc.Num(condition.upper))),
		                  DirectInfeasibleErrorPredicate(optimizer, errors)));
		answer_checks.push_back(DirectRaiseTooLarge(optimizer, total_a, condition.objective_written));
		answer_checks.push_back(DirectRaiseTooLarge(optimizer, total_b, condition.objective_written));
		answer_checks.push_back(DirectWarnIf(calc, calc.Out(turning.loses_precision)));
		p3.Add(DirectAllHold(calc, std::move(answer_checks)));

		DirectSubstitute substitute;
		substitute.prunable_decisions.push_back(1);
		// The source columns, then c. The window holds every row, and the validation column reads it.
		for (idx_t i = 0; i < source_bindings.size() + 1; i++) {
			substitute.slot_map.push_back(i);
		}
		substitute.validated_columns.push_back(source_bindings.size() + 1);
		substitute.child = p3.Finish(std::move(window));
		return substitute;
	}

private:
	const Condition &condition;
	Optimizer &optimizer;
	Binder &binder;
	ExpressionBuilder calc;
	vector<ColumnBinding> source_bindings;
	vector<LogicalType> source_types;
	const DirectStateErrors &errors;
};

DirectSubstitute Rule::Apply(DirectSourceReader &reader, Optimizer &optimizer,
                             const DirectApplicabilityCondition &candidate, const DirectStateErrors &errors) const {
	auto &condition = static_cast<const Condition &>(candidate);
	PlanBuilder plan_builder(condition, optimizer, reader.SourceBindings(), reader.SourceTypes(), errors);
	return DirectBuildOverOneRead(
	    reader, [&](unique_ptr<LogicalOperator> rows) { return plan_builder.Build(std::move(rows)); });
}

} // namespace direct_a2_single_facility_location

unique_ptr<DirectSolveRule> MakeA2SingleFacilityLocationRule() {
	return make_uniq<direct_a2_single_facility_location::Rule>();
}

} // namespace duckdb
