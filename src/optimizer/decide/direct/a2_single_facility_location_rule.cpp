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

//! What CheckCondition proved: one query-wide or keyed REAL decision `c`, bounded by constants and by per-row bounds
//! that each class of the key intersects, and an objective that is one square `K * (w*c + t)^2` of a linear form of `c`
//! and a source value, with `w` a nonzero constant and `K` positive once the sense is applied. The square is summed
//! over the rows the objective keeps, so the objective is `a*c^2 + b*c` with `a` the sum of `K*w^2` and `b` the sum of
//! `2*K*w*t`, and its minimum over the interval is the turning point `-b / (2*a)` clamped into it. A keyed decision has
//! one such problem per class of its key.
struct Condition final : DirectApplicabilityCondition {
	//! The box before any per-row bound (DirectDecisionBox): the default floor of 0 or the written lower bound, and the
	//! written upper bound.
	double lower = 0.0;
	double upper = POSITIVE_INFINITY;
	//! The bounds the plan evaluates per row: read from the data, or under a `when`. A class's interval is the box
	//! tightened by the tightest end of each clause over the rows of the class.
	vector<DirectBoundClause> clauses;
	DecideSense sense = DecideSense::FEASIBILITY;
	//! The objective as a message names it: a number computed past a DOUBLE names it, as the solver path names one.
	string objective_written;
	//! The decision's key, empty for a query-wide decision (one class). A row with a NULL in a `no_group` element is in
	//! no class: it has no decision, and the front end already leaves it out of the objective's `kept` rows.
	vector<DirectKeyElement> keys;
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
		    ir.decisions[0].scope == DecideVarScope::ROW) {
			return nullptr;
		}
		auto condition = make_uniq<Condition>();
		condition->sense = ir.objective.sense;
		condition->objective_written = ir.objective.written;
		if (ir.decisions[0].scope == DecideVarScope::KEYED) {
			// A key's columns are columns of the source (the IR reads them as a row check would), so a copy is all it takes.
			for (auto &element : ir.keyed_scopes[ir.decisions[0].keyed_scope].key) {
				condition->keys.push_back(element.Copy());
			}
		}
		if (!ProveInterval(ir, context, *condition) || !ProveObjective(ir, context, *condition)) {
			return nullptr;
		}
		return std::move(condition);
	}

	string Summary() const override {
		return "min SUM((c - v)^2), MAX(lo) <= c <= MIN(hi), one shared c for each class";
	}

	DirectSubstitute Apply(DirectSourceReader &reader, Optimizer &optimizer,
	                       const DirectApplicabilityCondition &candidate,
	                       const DirectStateErrors &errors) const override;

private:
	bool ProveInterval(const DirectIR &ir, ClientContext &context, Condition &condition) const;
	bool ProveObjective(const DirectIR &ir, ClientContext &context, Condition &condition) const;
};

//! Every constraint is a bound on `c`. The solver folds the ones known before the query runs into the decision's box,
//! and the rest, read from the data or under a `when`, bind the same decision on each row they keep, so a class's
//! interval is their intersection. An empty interval is not declined: the plan raises the solver's infeasible error.
bool Rule::ProveInterval(const DirectIR &ir, ClientContext &context, Condition &condition) const {
	for (auto &ir_node : ir.constraints) {
		if (!DirectIsSingletonRow(ir, ir_node)) {
			return false;
		}
	}
	DirectBoundReading reading;
	if (!DirectReadBounds(ir, context, reading)) {
		return false;
	}
	condition.lower = reading.boxes[0].lower;
	condition.upper = reading.boxes[0].upper;
	condition.clauses = std::move(reading.clauses);
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
//!   scope the row's class: whether it has one, and its key (DirectProjectScope), for a keyed decision
//!   P1    raw values: the rows the objective keeps, and the slope `w` and constant `t` of the square on each; each
//!         bound clause's ends on the rows it keeps; the check of every value that can fail
//!   P2    the row's `a` and `b` (zero on a row the objective drops), oriented as a minimum, each clause's lower and
//!         upper end on the row (no bound where its `when` drops the row), and the row's checks
//!   W     over the rows of the row's class (one window partitioned by the key): the sums of `a` and `b`, the MAX of
//!         each clause's lower ends and the MIN of its upper ends, and whether a floor-lifting clause keeps some row
//!   P3    the class's interval as columns: each clause's tightest ends, and the floor
//!   P4    the answer: the turning point `-b / (2*a)` clamped into the interval (the box of DirectRowBox over the
//!         columns of P3), repeated on every row of the class, and the column that keeps the checks live
//!
//! Aggregate-then-repeat: the window is the aggregate and the row's own columns are what it repeats on, so the source
//! is read once. A query-wide decision is the partition with no key. The window holds every row before it releases one,
//! so each check on a row (P2) has run on all of them first, as the solver reads every row before it returns.
class PlanBuilder {
public:
	PlanBuilder(const Condition &condition_p, Optimizer &optimizer_p, const vector<ColumnBinding> &source_bindings_p,
	            const vector<LogicalType> &source_types_p, const DirectStateErrors &errors_p)
	    : condition(condition_p), optimizer(optimizer_p), binder(optimizer_p.binder), calc(optimizer_p.context),
	      source_bindings(source_bindings_p), source_types(source_types_p), errors(errors_p) {
		D_ASSERT(source_bindings.size() == source_types.size());
	}

	DirectSubstitute Build(unique_ptr<LogicalOperator> rows) const {
		auto state = DirectProjectScope(binder, std::move(rows), IsKeyed(), nullptr, condition.keys, {});
		Carried from_scope;
		from_scope.source = state.input_bindings;
		from_scope.eligible = state.eligible;
		from_scope.keys = state.keys;
		from_scope.key_types = state.key_types;

		DirectProjectionStage p1(binder.GenerateTableIndex());
		auto raw = LayoutRawValues(p1, from_scope, state.state_index);
		auto plan = p1.Finish(std::move(state.plan));

		DirectProjectionStage p2(binder.GenerateTableIndex());
		auto coefficients = ComputeCoefficients(p2, raw);
		plan = p2.Finish(std::move(plan));

		auto aggregates = AggregateClasses(coefficients, plan);

		DirectProjectionStage p3(binder.GenerateTableIndex());
		auto interval = ComputeInterval(p3, aggregates);
		plan = p3.Finish(std::move(plan));

		DirectProjectionStage p4(binder.GenerateTableIndex());
		ComputeAnswer(p4, interval);
		plan = p4.Finish(std::move(plan));

		DirectSubstitute substitute;
		substitute.prunable_decisions.push_back(1);
		// The source columns, then c. The window holds every row, and the validation column reads it.
		for (idx_t i = 0; i < source_bindings.size() + 1; i++) {
			substitute.slot_map.push_back(i);
		}
		substitute.validated_columns.push_back(source_bindings.size() + 1);
		substitute.child = std::move(plan);
		return substitute;
	}

private:
	//! The columns every stage forwards.
	struct Carried {
		vector<ColumnBinding> source;
		//! Set only for a keyed decision.
		ColumnBinding eligible;
		vector<ColumnBinding> keys;
		//! The types of `keys`.
		vector<LogicalType> key_types;
	};
	//! Columns of P1.
	struct Raw {
		Carried carried;
		vector<DirectBoundColumns> clauses;
		DirectQuadraticColumns part;
		vector<unique_ptr<Expression>> checks;
	};
	//! A clause's lower and upper end on the row (DirectClauseBounds), and the row's `when` for a clause that lifts the
	//! floor.
	struct ClauseEnds {
		ColumnBinding lower;
		ColumnBinding upper;
		ColumnBinding when;
	};
	//! Columns of P2.
	struct Coefficients {
		Carried carried;
		vector<ClauseEnds> ends;
		ColumnBinding a;
		ColumnBinding b;
		ColumnBinding valid;
	};
	//! Columns of W: the class's sums and the aggregates of its clauses, over the results.
	struct Aggregates {
		Carried carried;
		ColumnBinding valid;
		ColumnBinding sum_a;
		ColumnBinding sum_b;
		vector<DirectGroupValues> lowers;
		vector<DirectGroupValues> uppers;
		//! Whether some row of a floor-lifting clause is kept, one per such clause.
		vector<ColumnBinding> lowering_whens;
	};
	//! Columns of P3.
	struct Interval {
		Carried carried;
		ColumnBinding valid;
		ColumnBinding total_a;
		ColumnBinding total_b;
		vector<ColumnBinding> clause_lowers;
		vector<ColumnBinding> clause_uppers;
		ColumnBinding floor;
	};

	const Condition &condition;
	Optimizer &optimizer;
	Binder &binder;
	ExpressionBuilder calc;
	vector<ColumnBinding> source_bindings;
	vector<LogicalType> source_types;
	const DirectStateErrors &errors;

	bool IsKeyed() const {
		return !condition.keys.empty();
	}

	Carried Forward(DirectProjectionStage &stage, const Carried &from) const {
		Carried to;
		for (idx_t i = 0; i < from.source.size(); i++) {
			to.source.push_back(stage.Add(calc.Out(calc.Col(source_types[i], from.source[i]))));
		}
		if (IsKeyed()) {
			to.eligible = stage.Add(calc.Out(calc.Col(LogicalType::BOOLEAN, from.eligible)));
		}
		for (idx_t i = 0; i < from.keys.size(); i++) {
			to.keys.push_back(stage.Add(calc.Out(calc.Col(from.key_types[i], from.keys[i]))));
		}
		to.key_types = from.key_types;
		return to;
	}

	//! Makes `window` run over the rows of the row's class.
	void PartitionByKey(const Carried &carried, BoundWindowExpression &window) const {
		for (idx_t i = 0; i < carried.keys.size(); i++) {
			window.partitions.push_back(calc.Out(calc.Col(carried.key_types[i], carried.keys[i])));
		}
	}

	//! The aggregate `function` of a column over the rows of the class; its column is the window's.
	ColumnBinding AddWindow(LogicalWindow &window, const Carried &carried, const char *function, ColumnBinding column,
	                        const LogicalType &type) const {
		auto aggregate = DirectWindowExtremum(optimizer, function, calc.Out(calc.Col(type, column)));
		PartitionByKey(carried, *aggregate);
		window.expressions.push_back(std::move(aggregate));
		return ColumnBinding(window.window_index, window.expressions.size() - 1);
	}

	//===------------------------------------------------------------------===//
	// P1: raw values
	//===------------------------------------------------------------------===//

	Raw LayoutRawValues(DirectProjectionStage &p1, const Carried &from_scope, idx_t state_index) const {
		Raw raw;
		raw.carried = Forward(p1, from_scope);
		auto over_state = [&](const Expression &value) {
			return state_index == DConstants::INVALID_INDEX ? value.Copy()
			                                                : DirectOverProjection(value, source_bindings, state_index);
		};
		raw.clauses = DirectLayoutBoundClauses(optimizer, calc, p1, condition.clauses, source_bindings, over_state,
		                                       raw.checks);
		raw.part = DirectLayoutQuadraticPart(optimizer, calc, p1, condition.parts[0], 1, source_bindings, over_state,
		                                     raw.checks);
		return raw;
	}

	//===------------------------------------------------------------------===//
	// P2: the row's `a` and `b`, each clause's ends, and the checks
	//===------------------------------------------------------------------===//

	Coefficients ComputeCoefficients(DirectProjectionStage &p2, Raw &raw) const {
		Coefficients result;
		result.carried = Forward(p2, raw.carried);
		for (auto &columns : raw.clauses) {
			ExprPtr lower, upper;
			DirectClauseBounds(calc, columns, lower, upper);
			ClauseEnds ends;
			ends.lower = p2.Add(calc.Out(lower));
			ends.upper = p2.Add(calc.Out(upper));
			if (columns.has_when) {
				ends.when = p2.Add(calc.Out(calc.Col(LogicalType::BOOLEAN, columns.when)));
			}
			result.ends.push_back(ends);
		}
		auto sum = DirectFoldQuadratic(optimizer, calc, condition.parts, {raw.part}, 0);
		auto sense = calc.Num(condition.sense == DecideSense::MAXIMIZE ? -1.0 : 1.0);
		result.a = p2.Add(calc.Out(calc.Mul(sense, sum.a)));
		result.b = p2.Add(calc.Out(calc.Mul(sense, sum.b)));
		// Every check is true or raises; a row passes when all of them do.
		result.valid = p2.Add(DirectAllHold(calc, std::move(raw.checks)));
		return result;
	}

	//===------------------------------------------------------------------===//
	// W: the aggregates of a class
	//===------------------------------------------------------------------===//

	//! Puts one window over `plan`: the sums of `a` and `b`, and for each clause the MAX of its lower ends and the MIN of
	//! its upper ends (DirectTightestBound), and whether some row a floor-lifting clause keeps exists. A row with no
	//! class sits in a partition of its own, and its answer is NULL whatever the partition holds.
	Aggregates AggregateClasses(const Coefficients &coefficients, unique_ptr<LogicalOperator> &plan) const {
		auto window = make_uniq<LogicalWindow>(binder.GenerateTableIndex());
		auto &carried = coefficients.carried;
		Aggregates result;
		result.carried = carried;
		result.valid = coefficients.valid;
		result.sum_a = AddWindow(*window, carried, "sum", coefficients.a, LogicalType::DOUBLE);
		result.sum_b = AddWindow(*window, carried, "sum", coefficients.b, LogicalType::DOUBLE);
		DirectWindowSink sink(*window, [&](BoundWindowExpression &aggregate) { PartitionByKey(carried, aggregate); });
		for (idx_t j = 0; j < condition.clauses.size(); j++) {
			auto &clause = condition.clauses[j];
			auto &ends = coefficients.ends[j];
			result.lowers.push_back(
			    TightestEnd(sink, ExpressionType::COMPARE_GREATERTHANOREQUALTO, *clause.lower, ends.lower));
			result.uppers.push_back(TightestEnd(sink, ExpressionType::COMPARE_LESSTHANOREQUALTO, *clause.upper, ends.upper));
			if (clause.lifts_floor) {
				result.lowering_whens.push_back(
				    AddWindow(*window, carried, "bool_or", ends.when, LogicalType::BOOLEAN));
			}
		}
		window->children.push_back(std::move(plan));
		plan = std::move(window);
		return result;
	}

	//! The tightest value of a clause's end `column` (of the admitted end `end`) over the class: the MAX of lower ends
	//! for `>=`, the MIN of upper ends for `<=`.
	DirectGroupValues TightestEnd(DirectGroupSink &sink, ExpressionType comparison, const Expression &end,
	                              ColumnBinding column) const {
		auto bound = DirectTightestBound(optimizer, comparison, end.Copy(), string(), nullptr);
		vector<DirectGroupInputColumns> input(1);
		input[0].argument = column;
		return DirectAddGroupBound(calc, *bound, input, sink, nullptr);
	}

	//===------------------------------------------------------------------===//
	// P3: the class's interval as columns
	//===------------------------------------------------------------------===//

	//! Each clause's tightest ends, and the static floor, lowered when a row a floor-lifting clause keeps exists.
	Interval ComputeInterval(DirectProjectionStage &p3, const Aggregates &aggregates) const {
		Interval result;
		result.carried = Forward(p3, aggregates.carried);
		result.valid = p3.Add(calc.Out(calc.Col(LogicalType::BOOLEAN, aggregates.valid)));
		result.total_a = p3.Add(calc.Out(calc.Col(LogicalType::DOUBLE, aggregates.sum_a)));
		result.total_b = p3.Add(calc.Out(calc.Col(LogicalType::DOUBLE, aggregates.sum_b)));
		for (idx_t j = 0; j < condition.clauses.size(); j++) {
			result.clause_lowers.push_back(p3.Add(calc.Out(calc.Cast(aggregates.lowers[j].lower, LogicalType::DOUBLE))));
			result.clause_uppers.push_back(p3.Add(calc.Out(calc.Cast(aggregates.uppers[j].upper, LogicalType::DOUBLE))));
		}
		result.floor = p3.Add(calc.Out(DirectStaticFloor(calc, condition.lower, aggregates.lowering_whens)));
		return result;
	}

	//===------------------------------------------------------------------===//
	// P4: the answer
	//===------------------------------------------------------------------===//

	void ComputeAnswer(DirectProjectionStage &p4, const Interval &interval) const {
		for (idx_t i = 0; i < source_bindings.size(); i++) {
			p4.Add(calc.Out(calc.Col(source_types[i], interval.carried.source[i])));
		}
		auto total_a = calc.Col(LogicalType::DOUBLE, interval.total_a);
		auto total_b = calc.Col(LogicalType::DOUBLE, interval.total_b);
		auto zero = calc.Num(0.0);
		ExprPtr lower, upper;
		DirectRowBox(calc, calc.Col(LogicalType::DOUBLE, interval.floor), condition.upper, interval.clause_lowers,
		             interval.clause_uppers, lower, upper);
		// Convex: the turning point, clamped into the interval. With no square on any row of the class every value is
		// optimal, and the point of the interval nearest 0 is the answer. A row the objective drops has `a = b = 0`,
		// never NULL, so every row carries a sum.
		auto turning = DirectTurningPoint(calc, total_a, total_b, false);
		auto target = calc.If(calc.Cmp(ExpressionType::COMPARE_GREATERTHAN, total_a, zero), turning.target, zero);
		// Adding 0.0 turns -0.0 into 0.0.
		auto answer = calc.Add(calc.Min({calc.Max({target, lower}), upper}), zero);
		// A row with no class has no decision and shows NULL.
		auto has_class = IsKeyed() ? calc.Col(LogicalType::BOOLEAN, interval.carried.eligible) : calc.Bool(true);
		p4.Add(calc.Out(calc.If(has_class, answer, calc.Constant(Value(LogicalType::DOUBLE)))));

		vector<unique_ptr<Expression>> checks;
		checks.push_back(calc.Out(calc.Col(LogicalType::BOOLEAN, interval.valid)));
		// An empty interval raises on every row that has a class. The interval of a class is the same on every row of
		// it, so a query whose only bounds are constants has two constants here, which DuckDB folds.
		checks.push_back(DirectRaiseIf(calc, calc.Out(calc.And(has_class, DirectBoxIsEmpty(calc, lower, upper))),
		                               DirectInfeasibleErrorPredicate(optimizer, errors)));
		checks.push_back(DirectRaiseTooLarge(optimizer, total_a, condition.objective_written));
		checks.push_back(DirectRaiseTooLarge(optimizer, total_b, condition.objective_written));
		checks.push_back(DirectWarnIf(calc, calc.Out(turning.loses_precision)));
		p4.Add(DirectAllHold(calc, std::move(checks)));
	}
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
