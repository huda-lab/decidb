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
			CopyKey(ir.keyed_scopes[ir.decisions[0].keyed_scope].key, condition->keys);
		}
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
	//! A key's columns are columns of the source (the IR reads them as a row check would), so a copy is all it takes.
	static void CopyKey(const vector<DirectKeyElement> &from, vector<DirectKeyElement> &to) {
		for (auto &element : from) {
			DirectKeyElement copy;
			copy.grouping = element.grouping->Copy();
			copy.label = element.label->Copy();
			copy.name = element.name;
			copy.no_group = element.no_group;
			to.push_back(std::move(copy));
		}
	}
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
		bool keyed = !condition.keys.empty();
		auto state = DirectProjectScope(binder, std::move(rows), keyed, nullptr, condition.keys, {});
		Carried from_scope;
		from_scope.source = state.input_bindings;
		from_scope.eligible = state.eligible;
		from_scope.keys = state.keys;
		from_scope.key_types = state.key_types;

		vector<unique_ptr<Expression>> checks;
		DirectProjectionStage p1(binder.GenerateTableIndex());
		auto carried1 = Forward(p1, from_scope);
		auto over_state = [&](const Expression &value) {
			return state.state_index == DConstants::INVALID_INDEX
			           ? value.Copy()
			           : DirectOverProjection(value, source_bindings, state.state_index);
		};
		auto clause_columns = LayoutClauses(p1, over_state, checks);
		auto laid =
		    DirectLayoutQuadraticPart(optimizer, calc, p1, condition.parts[0], 1, source_bindings, over_state, checks);
		auto plan = p1.Finish(std::move(state.plan));

		DirectProjectionStage p2(binder.GenerateTableIndex());
		auto carried2 = Forward(p2, carried1);
		vector<ClauseEnds> ends;
		for (auto &columns : clause_columns) {
			ExprPtr lower, upper;
			DirectClauseBounds(calc, columns, lower, upper);
			ClauseEnds end;
			end.lower = p2.Add(calc.Out(lower));
			end.upper = p2.Add(calc.Out(upper));
			if (columns.has_when) {
				end.when = p2.Add(calc.Out(calc.Col(LogicalType::BOOLEAN, columns.when)));
			}
			ends.push_back(end);
		}
		auto sum = DirectFoldQuadratic(optimizer, calc, condition.parts, {laid}, 0);
		auto sense = calc.Num(condition.sense == DecideSense::MAXIMIZE ? -1.0 : 1.0);
		auto a = p2.Add(calc.Out(calc.Mul(sense, sum.a)));
		auto b = p2.Add(calc.Out(calc.Mul(sense, sum.b)));
		// Every check is true or raises; a row passes when all of them do.
		auto valid = p2.Add(DirectAllHold(calc, std::move(checks)));
		plan = p2.Finish(std::move(plan));

		// W: every aggregate of a class, over the rows of its partition.
		auto window_index = binder.GenerateTableIndex();
		auto window = make_uniq<LogicalWindow>(window_index);
		auto partition = [&](BoundWindowExpression &aggregate) {
			for (idx_t i = 0; i < carried2.keys.size(); i++) {
				aggregate.partitions.push_back(calc.Out(calc.Col(carried2.key_types[i], carried2.keys[i])));
			}
		};
		auto add_window = [&](const char *function, ColumnBinding column, LogicalType type) {
			auto aggregate = DirectWindowExtremum(optimizer, function, calc.Out(calc.Col(type, column)));
			partition(*aggregate);
			window->expressions.push_back(std::move(aggregate));
			return ColumnBinding(window_index, window->expressions.size() - 1);
		};
		auto sum_a = add_window("sum", a, LogicalType::DOUBLE);
		auto sum_b = add_window("sum", b, LogicalType::DOUBLE);
		// The class's interval: the tightest end of each clause, MAX of its lower ends and MIN of its upper ends
		// (DirectTightestBound), and whether some row a floor-lifting clause keeps exists. A row with no class sits in
		// a partition of its own, and its answer is NULL whatever the partition holds.
		DirectWindowSink sink(*window, partition);
		vector<DirectGroupValues> lowers, uppers;
		vector<ColumnBinding> lowering_whens;
		for (idx_t j = 0; j < condition.clauses.size(); j++) {
			auto &clause = condition.clauses[j];
			vector<DirectGroupInputColumns> lower_input(1), upper_input(1);
			lower_input[0].argument = ends[j].lower;
			upper_input[0].argument = ends[j].upper;
			auto largest = DirectTightestBound(optimizer, ExpressionType::COMPARE_GREATERTHANOREQUALTO,
			                                   clause.lower->Copy(), string(), nullptr);
			auto smallest = DirectTightestBound(optimizer, ExpressionType::COMPARE_LESSTHANOREQUALTO,
			                                    clause.upper->Copy(), string(), nullptr);
			lowers.push_back(DirectAddGroupBound(calc, *largest, lower_input, sink, nullptr));
			uppers.push_back(DirectAddGroupBound(calc, *smallest, upper_input, sink, nullptr));
			if (clause.lifts_floor) {
				lowering_whens.push_back(add_window("bool_or", ends[j].when, LogicalType::BOOLEAN));
			}
		}
		window->children.push_back(std::move(plan));

		// P3: the class's interval pieces as columns: each clause's tightest ends, and the static floor, lowered when a
		// row a floor-lifting clause keeps exists.
		DirectProjectionStage p3(binder.GenerateTableIndex());
		auto carried3 = Forward(p3, carried2);
		auto valid3 = p3.Add(calc.Out(calc.Col(LogicalType::BOOLEAN, valid)));
		auto total_a3 = p3.Add(calc.Out(calc.Col(LogicalType::DOUBLE, sum_a)));
		auto total_b3 = p3.Add(calc.Out(calc.Col(LogicalType::DOUBLE, sum_b)));
		vector<ColumnBinding> clause_lowers, clause_uppers;
		for (idx_t j = 0; j < condition.clauses.size(); j++) {
			clause_lowers.push_back(p3.Add(calc.Out(calc.Cast(lowers[j].lower, LogicalType::DOUBLE))));
			clause_uppers.push_back(p3.Add(calc.Out(calc.Cast(uppers[j].upper, LogicalType::DOUBLE))));
		}
		auto floor = p3.Add(calc.Out(DirectStaticFloor(calc, condition.lower, lowering_whens)));
		auto plan3 = p3.Finish(std::move(window));

		// P4: the answer. A row the objective drops has `a = b = 0`, never NULL, so every row carries a sum.
		DirectProjectionStage p4(binder.GenerateTableIndex());
		auto total_a = calc.Col(LogicalType::DOUBLE, total_a3);
		auto total_b = calc.Col(LogicalType::DOUBLE, total_b3);
		auto zero = calc.Num(0.0);
		ExprPtr lower, upper;
		DirectRowBox(calc, calc.Col(LogicalType::DOUBLE, floor), condition.upper, clause_lowers, clause_uppers, lower,
		             upper);
		// Convex: the turning point, clamped into the interval. With no square on any row of the class every value is
		// optimal, and the point of the interval nearest 0 is the answer.
		auto turning = DirectTurningPoint(calc, total_a, total_b, false);
		auto target = calc.If(calc.Cmp(ExpressionType::COMPARE_GREATERTHAN, total_a, zero), turning.target, zero);
		auto clamped = calc.Min({calc.Max({target, lower}), upper});
		// A row with no class has no decision and shows NULL.
		auto has_class = keyed ? calc.Col(LogicalType::BOOLEAN, carried3.eligible) : calc.Bool(true);

		for (idx_t i = 0; i < source_bindings.size(); i++) {
			p4.Add(calc.Out(calc.Col(source_types[i], carried3.source[i])));
		}
		// Adding 0.0 turns -0.0 into 0.0.
		auto answer = calc.Add(clamped, zero);
		p4.Add(calc.Out(keyed ? calc.If(has_class, answer, calc.Constant(Value(LogicalType::DOUBLE))) : answer));
		vector<unique_ptr<Expression>> answer_checks;
		answer_checks.push_back(calc.Out(calc.Col(LogicalType::BOOLEAN, valid3)));
		// An empty interval raises on every row that has a class. The interval of a class is the same on every row of
		// it, so a query whose only bounds are constants has two constants here, which DuckDB folds.
		answer_checks.push_back(DirectRaiseIf(calc, calc.Out(calc.And(has_class, DirectBoxIsEmpty(calc, lower, upper))),
		                                      DirectInfeasibleErrorPredicate(optimizer, errors)));
		answer_checks.push_back(DirectRaiseTooLarge(optimizer, total_a, condition.objective_written));
		answer_checks.push_back(DirectRaiseTooLarge(optimizer, total_b, condition.objective_written));
		answer_checks.push_back(DirectWarnIf(calc, calc.Out(turning.loses_precision)));
		p4.Add(DirectAllHold(calc, std::move(answer_checks)));

		DirectSubstitute substitute;
		substitute.prunable_decisions.push_back(1);
		// The source columns, then c. The window holds every row, and the validation column reads it.
		for (idx_t i = 0; i < source_bindings.size() + 1; i++) {
			substitute.slot_map.push_back(i);
		}
		substitute.validated_columns.push_back(source_bindings.size() + 1);
		substitute.child = p4.Finish(std::move(plan3));
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

	//! The columns of one clause in P2: its lower and upper end on the row (DirectClauseBounds), and the row's `when`
	//! for a clause that lifts the floor.
	struct ClauseEnds {
		ColumnBinding lower;
		ColumnBinding upper;
		ColumnBinding when;
	};

	Carried Forward(DirectProjectionStage &stage, const Carried &from) const {
		Carried to;
		for (idx_t i = 0; i < from.source.size(); i++) {
			to.source.push_back(stage.Add(calc.Out(calc.Col(source_types[i], from.source[i]))));
		}
		if (!condition.keys.empty()) {
			to.eligible = stage.Add(calc.Out(calc.Col(LogicalType::BOOLEAN, from.eligible)));
		}
		for (idx_t i = 0; i < from.keys.size(); i++) {
			to.keys.push_back(stage.Add(calc.Out(calc.Col(from.key_types[i], from.keys[i]))));
		}
		to.key_types = from.key_types;
		return to;
	}

	//! P1 columns of every clause, each value computed only on the rows its clause keeps (NULL elsewhere), with the
	//! check of each: the coefficient is a number, and the right side is not NaN (a NULL one skips the row).
	vector<DirectBoundColumns> LayoutClauses(DirectProjectionStage &p1,
	                                         const std::function<unique_ptr<Expression>(const Expression &)> &over,
	                                         vector<unique_ptr<Expression>> &checks) const {
		vector<DirectBoundColumns> result;
		for (auto &clause : condition.clauses) {
			DirectBoundColumns columns;
			ExprPtr active;
			if (clause.when) {
				active = calc.Wrap(DirectActiveWhen(calc, over(*clause.when).get()));
			}
			auto on_kept = [&](ExprPtr value) {
				return active ? DirectOnKeptRows(calc, active, value) : value;
			};
			auto coefficient = p1.Add(calc.Out(on_kept(calc.Wrap(DirectSumOf(optimizer, clause.coefficient, over)))));
			auto rhs = p1.Add(calc.Out(on_kept(calc.Cast(calc.Wrap(over(*clause.rhs)), LogicalType::DOUBLE))));
			columns.lower = p1.Add(calc.Out(on_kept(calc.Wrap(over(*clause.lower)))));
			columns.upper = p1.Add(calc.Out(on_kept(calc.Wrap(over(*clause.upper)))));
			if (clause.when) {
				columns.has_when = true;
				columns.when = p1.Add(calc.Out(active));
			}
			// Both values are NULL on a row the clause drops, and a NULL passes (it skips the row).
			if (auto check = DirectCheckNumber(optimizer, coefficient, clause.coefficient_names, clause.coefficient,
			                                   source_bindings, p1.index)) {
				checks.push_back(std::move(check));
			}
			checks.push_back(DirectValidBoundPredicate(optimizer, rhs, clause.rhs_text));
			result.push_back(columns);
		}
		return result;
	}

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
