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

//! What CheckCondition proved: one query-wide or keyed REAL decision `c` in a constant interval [lower, upper], and an
//! objective that is one square `K * (w*c + t)^2` of a linear form of `c` and a source value, with `w` a nonzero
//! constant and `K` positive once the sense is applied. The square is summed over the rows the objective keeps, so the
//! objective is `a*c^2 + b*c` with `a` the sum of `K*w^2` and `b` the sum of `2*K*w*t`, and its minimum over the
//! interval is the turning point `-b / (2*a)` clamped into it. A keyed decision has one such problem per class of its
//! key.
struct Condition final : DirectApplicabilityCondition {
	double lower = 0.0;
	double upper = POSITIVE_INFINITY;
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
//!   scope the row's class: whether it has one, and its key (DirectProjectScope), for a keyed decision
//!   P1    raw values: the rows the objective keeps, and the slope `w` and constant `t` of the square on each, with the
//!         check of every value that can fail
//!   P2    the row's `a` and `b` (zero on a row the objective drops), oriented as a minimum, and the row's checks
//!   W     the sums of `a` and `b` over every row of the row's class (one window partitioned by the key)
//!   P3    the answer: the turning point `-b / (2*a)` clamped into the interval, repeated on every row of the class,
//!         and the column that keeps the checks live
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
		auto laid =
		    DirectLayoutQuadraticPart(optimizer, calc, p1, condition.parts[0], 1, source_bindings, over_state, checks);
		auto plan = p1.Finish(std::move(state.plan));

		DirectProjectionStage p2(binder.GenerateTableIndex());
		auto carried2 = Forward(p2, carried1);
		auto sum = DirectFoldQuadratic(optimizer, calc, condition.parts, {laid}, 0);
		auto sense = calc.Num(condition.sense == DecideSense::MAXIMIZE ? -1.0 : 1.0);
		auto a = p2.Add(calc.Out(calc.Mul(sense, sum.a)));
		auto b = p2.Add(calc.Out(calc.Mul(sense, sum.b)));
		// Every check is true or raises; a row passes when all of them do.
		auto valid = p2.Add(DirectAllHold(calc, std::move(checks)));
		plan = p2.Finish(std::move(plan));

		auto window_index = binder.GenerateTableIndex();
		auto window = make_uniq<LogicalWindow>(window_index);
		for (auto column : {a, b}) {
			auto total = DirectWindowExtremum(optimizer, "sum", calc.Out(calc.Col(LogicalType::DOUBLE, column)));
			for (idx_t i = 0; i < carried2.keys.size(); i++) {
				total->partitions.push_back(calc.Out(calc.Col(carried2.key_types[i], carried2.keys[i])));
			}
			window->expressions.push_back(std::move(total));
		}
		window->children.push_back(std::move(plan));

		// A row the objective drops has `a = b = 0`, never NULL, so every row carries a sum.
		auto total_a = calc.Col(LogicalType::DOUBLE, ColumnBinding(window_index, 0));
		auto total_b = calc.Col(LogicalType::DOUBLE, ColumnBinding(window_index, 1));
		auto zero = calc.Num(0.0);
		// Convex: the turning point, clamped into the interval. With no square on any row of the class every value is
		// optimal, and the point of the interval nearest 0 is the answer.
		auto turning = DirectTurningPoint(calc, total_a, total_b, false);
		auto target = calc.If(calc.Cmp(ExpressionType::COMPARE_GREATERTHAN, total_a, zero), turning.target, zero);
		auto clamped = calc.Min({calc.Max({target, calc.Num(condition.lower)}), calc.Num(condition.upper)});
		// A row with no class has no decision and shows NULL.
		auto has_class = keyed ? calc.Col(LogicalType::BOOLEAN, carried2.eligible) : calc.Bool(true);

		DirectProjectionStage p3(binder.GenerateTableIndex());
		for (idx_t i = 0; i < source_bindings.size(); i++) {
			p3.Add(calc.Out(calc.Col(source_types[i], carried2.source[i])));
		}
		// Adding 0.0 turns -0.0 into 0.0.
		auto answer = calc.Add(clamped, zero);
		p3.Add(calc.Out(keyed ? calc.If(has_class, answer, calc.Constant(Value(LogicalType::DOUBLE))) : answer));
		vector<unique_ptr<Expression>> answer_checks;
		answer_checks.push_back(calc.Out(calc.Col(LogicalType::BOOLEAN, valid)));
		// The interval is two constants, so a query-wide decision's check is folded by DuckDB: it is true, or raises on
		// every row. A keyed decision raises on the rows that have a class.
		auto empty = DirectBoxIsEmpty(calc, calc.Num(condition.lower), calc.Num(condition.upper));
		answer_checks.push_back(DirectRaiseIf(calc, calc.Out(calc.And(has_class, empty)),
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
	//! The columns every stage forwards.
	struct Carried {
		vector<ColumnBinding> source;
		//! Set only for a keyed decision.
		ColumnBinding eligible;
		vector<ColumnBinding> keys;
		//! The types of `keys`.
		vector<LogicalType> key_types;
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
