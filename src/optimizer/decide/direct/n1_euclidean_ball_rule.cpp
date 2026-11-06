#include "duckdb/optimizer/decide/direct/direct_rules.hpp"

#include <cmath>
#include <limits>

#include "duckdb/common/decide_numeric_policy.hpp"
#include "duckdb/optimizer/decide/direct/direct_bound_reader.hpp"
#include "duckdb/optimizer/decide/direct/direct_expression.hpp"
#include "duckdb/optimizer/decide/direct/direct_plan_fragments.hpp"
#include "duckdb/optimizer/decide/direct/direct_runtime_checks.hpp"
#include "duckdb/optimizer/decide/direct/direct_source_reader.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/decide/decide_source_provenance.hpp"
#include "duckdb/planner/decide/decide_term_split.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_window.hpp"

namespace duckdb {

//! Each rule keeps its helpers in its own named namespace: unity builds compile every rule file in one translation
//! unit, where two anonymous namespaces would merge and same-named helpers would collide.
namespace direct_n1_euclidean_ball {

constexpr const char *RULE_NAME = "N1_EUCLIDEAN_BALL";

//! What CheckCondition proved: one row-scoped REAL decision in a box [0, inf) or (-inf, inf), one `SUM(q * x^2) <= B`
//! per group over the rows the clause covers, and an objective that orients on every row as minimize `K*x^2 + b*x`.
//! K is one constant on every row with a linear term, and positive.
struct Condition final : DirectApplicabilityCondition {
	//! The box is [0, inf), rather than (-inf, inf).
	bool nonnegative = true;
	DecideSense sense = DecideSense::FEASIBILITY;
	//! The objective as a message names it: a number computed past a DOUBLE names it, as the solver path names one.
	string objective_written;
	//! The clause's scope (DirectSharedScope).
	bool scoped = false;
	bool gated = false;
	unique_ptr<Expression> when;
	vector<DirectKeyElement> group_keys;
	vector<DirectKeyElement> per_class_keys;
	//! The limit B and the factor q of the sum of squares, so the ball is `SUM(x^2) <= B / q`. B is the constant
	//! `limit`, or a limit read from the data, whose NULL skips the instance: `limit_group`, the aggregates of its
	//! group that the bound-side rewrite left to the plan (DecideGroupBound), or `limit_value`, a value of the source
	//! row, whose tightest value in a group applies (DirectTightestBound).
	double limit = 0.0;
	unique_ptr<Expression> limit_value;
	unique_ptr<DecideGroupBound> limit_group;
	double square_factor = 1.0;
	vector<DirectQuadraticPart> parts;
	bool has_square = false;
};

//! `sign * scale * w^2` of a square, where its inner slope `w` folds to a constant; false otherwise.
bool FoldSquareFactor(ClientContext &context, const DirectSquareTerm &square, double &factor) {
	double slope;
	if (!DirectFoldConstantSum(context, square.inner_linear, slope)) {
		return false;
	}
	factor = square.sign * square.scale * (slope * slope);
	return std::isfinite(factor);
}

class Rule final : public DirectSolveRule {
public:
	const char *Name() const override {
		return RULE_NAME;
	}

	unique_ptr<DirectApplicabilityCondition> CheckCondition(const DirectIR &ir, ClientContext &context) const override {
		if (ir.decisions.size() != 1 || ir.decisions[0].domain != DirectDomain::REAL ||
		    ir.decisions[0].scope != DecideVarScope::ROW || !ir.keyed_scopes.empty()) {
			return nullptr;
		}
		auto condition = make_uniq<Condition>();
		condition->sense = ir.objective.sense;
		condition->objective_written = ir.objective.written;
		if (!ProveLimitClause(ir, context, *condition) || !ProveBox(ir, context, *condition) ||
		    !ProveObjective(ir, context, *condition)) {
			return nullptr;
		}
		return std::move(condition);
	}

	string Summary() const override {
		return "SUM(x^2) <= B: linear objective or nearest point to a target";
	}

	DirectSubstitute Apply(DirectSourceReader &reader, Optimizer &optimizer,
	                       const DirectApplicabilityCondition &candidate,
	                       const DirectStateErrors &errors) const override;

private:
	bool ProveLimitClause(const DirectIR &ir, ClientContext &context, Condition &condition) const;
	bool ProveLimitValue(const DirectIR &ir, ClientContext &context, const DirectIRConstraint &clause,
	                     Condition &condition) const;
	bool ProveBox(const DirectIR &ir, ClientContext &context, Condition &condition) const;
	bool ProveObjective(const DirectIR &ir, ClientContext &context, Condition &condition) const;
};

//! The one aggregate clause: a plain `SUM` of one square, centered on zero, with `<=`. Every other clause is a per-row
//! bound, which ProveBox reads.
bool Rule::ProveLimitClause(const DirectIR &ir, ClientContext &context, Condition &condition) const {
	const DirectIRConstraint *clause = nullptr;
	for (auto &ir_node : ir.constraints) {
		if (!ir_node.aggregate) {
			if (!DirectIsSingletonRow(ir, ir_node)) {
				return false;
			}
			continue;
		}
		if (clause) {
			return false;
		}
		clause = &ir_node;
	}
	if (!clause || clause->comparison != ExpressionType::COMPARE_LESSTHANOREQUALTO) {
		return false;
	}
	DirectSharedScope scope;
	if (!DirectProveSharedScope(ir, {clause}, scope)) {
		return false;
	}
	condition.scoped = scope.Scoped();
	condition.gated = scope.gated;
	condition.when = std::move(scope.when);
	condition.group_keys = std::move(scope.keys);
	condition.per_class_keys = std::move(scope.classes);
	// DirectProveSharedScope has proved that the clause is a plain SUM.
	auto &terms = clause->PlainSum()->terms;
	if (terms.size() != 1 || !terms[0].IsSquare()) {
		return false;
	}
	DirectSquareTerm square;
	if (!DirectReadSquareTerm(ir, context, terms[0], clause->written, square) || !square.inner_constant.empty() ||
	    !FoldSquareFactor(context, square, condition.square_factor) || condition.square_factor <= 0.0) {
		return false;
	}
	return ProveLimitValue(ir, context, *clause, condition);
}

//! A constant the solver reads as a number, or a limit read from the data: the aggregates of its group that the
//! bound-side rewrite left to the plan, or a numeric, deterministic expression over source columns. A limit of
//! magnitude 1e20 or more is where the solver stops reading a right side as a number, so those stay with the solver.
bool Rule::ProveLimitValue(const DirectIR &ir, ClientContext &context, const DirectIRConstraint &clause,
                           Condition &condition) const {
	if (TryEvaluateFoldableDouble(context, *clause.rhs, condition.limit, DIRECT_FINITE_CONSTANT)) {
		return std::fabs(condition.limit) < DECIDE_EFFECTIVE_INFINITY;
	}
	if (clause.group_bound) {
		if (!DirectGroupBoundIsReadable(ir, *clause.group_bound)) {
			return false;
		}
		condition.limit_group = clause.group_bound->Copy();
		return true;
	}
	if (!DirectIsSourceOnlyNumericMayRaise(*clause.rhs, ir.decide_index, ir.source_bindings)) {
		return false;
	}
	condition.limit_value = clause.rhs->Copy();
	return true;
}

//! The box is the default [0, inf), or (-inf, inf) with a lower bound of magnitude 1e20 or more. Any other bound, even
//! one that would not bind, is not this class: it needs the projection onto a clipped ball.
bool Rule::ProveBox(const DirectIR &ir, ClientContext &context, Condition &condition) const {
	DirectBoundReading reading;
	if (!DirectReadBounds(ir, context, reading) || !reading.clauses.empty()) {
		return false;
	}
	auto &box = reading.boxes[0];
	if (box.upper != POSITIVE_INFINITY || (box.lower != 0.0 && box.lower != NEGATIVE_INFINITY)) {
		return false;
	}
	condition.nonnegative = box.lower == 0.0;
	return true;
}

//! The square of an objective part: its factor folds to one constant, and it is convex in the sense.
static bool SquareIsConvex(ClientContext &context, const DirectIR &ir, const DirectQuadraticPart &part) {
	double factor;
	if (!FoldSquareFactor(context, part.squares[0], factor)) {
		return false;
	}
	auto &linear = part.linear;
	double part_factor = linear.scaled ? (linear.divides ? 1.0 / linear.scale : linear.scale) : 1.0;
	double sense = ir.objective.sense == DecideSense::MAXIMIZE ? -1.0 : 1.0;
	double oriented = sense * linear.sign * part_factor * factor;
	return std::isfinite(oriented) && oriented > 0.0;
}

//! K is one constant on every row with a linear term. A `when`, or a NULL a guard skips, leaves a row with no square,
//! so a square on some rows must share its part with every linear term.
//!
//! The NULL guard of a nullable `t` in `(w*x + t)^2` wraps every coefficient of the term (GuardCoefficients), `w` among
//! them, so a guarded `w` is how the square's absence on a row shows here. This reads that reach: a guard that spared
//! `w` would leave the square on every row and this check blind to it.
static bool SquareCoversEveryLinearTerm(const vector<DirectQuadraticPart> &parts) {
	for (auto &square : parts) {
		bool on_some_rows = !square.squares.empty() && square.kept;
		for (auto &term : square.squares) {
			for (auto &inner : term.inner_linear) {
				on_some_rows = on_some_rows || DecideIsGuarded(*inner.expr);
			}
		}
		for (auto &other : parts) {
			if (on_some_rows && &other != &square && !other.linear.terms.empty()) {
				return false;
			}
		}
	}
	return true;
}

//! No objective, or `SUM` parts of linear terms, per-row constants and at most one square, convex in the sense.
bool Rule::ProveObjective(const DirectIR &ir, ClientContext &context, Condition &condition) const {
	for (auto &part : ir.objective.parts) {
		DirectQuadraticPart read;
		if (!part.IsPlainSum() || !DirectReadQuadraticPart(ir, context, part, ir.objective.written, read)) {
			return false;
		}
		if (!read.squares.empty()) {
			if (condition.has_square || read.squares.size() > 1 || !SquareIsConvex(context, ir, read)) {
				return false;
			}
			condition.has_square = true;
		}
		condition.parts.push_back(std::move(read));
	}
	return SquareCoversEveryLinearTerm(condition.parts);
}

//===--------------------------------------------------------------------===//
// Plan
//===--------------------------------------------------------------------===//

//! Builds the relational plan for one N1 problem whose condition holds, over one read of the source. Each stage reads
//! only the columns of the stage before it; a window passes its child's columns on as they are.
//!
//!   [W0]  whether a `per K:` clause has an instance, when a NULL in K can leave it none (DirectAddInstanceFlag)
//!   scope the row's membership and group key (DirectProjectScope)
//!   P1    raw values: every coefficient, constant and limit, each on the rows that read it
//!   P2    K and b of the row, and the check on every raw value
//!   [W1]  the group's tightest limit, for a limit read from the data
//!   P3    the row's target (or direction) in the domain, whether it lost precision, whether it is in a ball, and the
//!         ball's limit
//!   P4    the row's squared length in the ball, every other check a row can raise, and the target's warning
//!   W2    h, the sum of the squared lengths of the row's group
//!   P5    the answer, the warning of the scale, and the column that keeps the checks live
//!
//! The solver reads every row before it returns, so a bad value on the last row raises even under an outer LIMIT 1. W2
//! holds every row before it releases one, and each check on a row stands below it. The two checks above it read h,
//! which only the plan computes: h past a DOUBLE (the solver has no such number, and every row that would read h
//! raises), and the warning that h has lost precision, which a row that reads the scale prints.
class PlanBuilder {
public:
	PlanBuilder(const Condition &condition_p, Optimizer &optimizer_p, const vector<ColumnBinding> &source_bindings_p,
	            const vector<LogicalType> &source_types_p, const DirectStateErrors &errors_p)
	    : condition(condition_p), optimizer(optimizer_p), binder(optimizer_p.binder), calc(optimizer_p.context),
	      source_bindings(source_bindings_p), source_types(source_types_p), errors(errors_p) {
		D_ASSERT(source_bindings.size() == source_types.size());
		// A limit the rewrite left as a group bound is one already; any other is one value per row, whose tightest
		// value in the group is the limit.
		if (condition.limit_group) {
			limit_bound = condition.limit_group->Copy();
		} else if (condition.limit_value) {
			limit_bound = DirectTightestBound(optimizer, ExpressionType::COMPARE_LESSTHANOREQUALTO,
			                                  condition.limit_value->Copy(), string(), nullptr);
		}
	}

	DirectSubstitute Build(unique_ptr<LogicalOperator> rows) const {
		unique_ptr<Expression> instance;
		if (condition.gated) {
			ColumnBinding flag;
			rows =
			    DirectAddInstanceFlag(optimizer, std::move(rows), condition.when.get(), condition.per_class_keys, flag);
			instance = calc.Out(calc.Col(LogicalType::BOOLEAN, flag));
		}
		auto state = DirectProjectScope(binder, std::move(rows), condition.scoped, condition.when.get(),
		                                condition.group_keys, {}, std::move(instance));
		Carried from_scope;
		for (idx_t i = 0; i < source_bindings.size(); i++) {
			from_scope.source.push_back(state.input_bindings[i]);
		}
		from_scope.eligible = state.eligible;
		from_scope.keys = state.keys;
		from_scope.key_types = state.key_types;

		DirectProjectionStage p1(binder.GenerateTableIndex());
		auto raw = LayoutRawValues(p1, from_scope, state.state_index);
		auto plan = p1.Finish(std::move(state.plan));

		DirectProjectionStage p2(binder.GenerateTableIndex());
		auto coefficients = ComputeCoefficients(p2, raw);
		plan = p2.Finish(std::move(plan));
		auto carried = coefficients.carried;

		DirectGroupValues limit_values;
		if (limit_bound) {
			auto limit_window = make_uniq<LogicalWindow>(binder.GenerateTableIndex());
			DirectWindowSink sink(*limit_window,
			                      [&](BoundWindowExpression &aggregate) { PartitionByGroup(carried, aggregate); });
			limit_values = DirectAddGroupBound(calc, *limit_bound, coefficients.limit_inputs, sink,
			                                   condition.scoped ? Boolean(carried.eligible) : nullptr);
			limit_window->children.push_back(std::move(plan));
			plan = std::move(limit_window);
		}

		DirectProjectionStage p3(binder.GenerateTableIndex());
		auto targets = ComputeTargets(p3, coefficients, limit_values);
		plan = p3.Finish(std::move(plan));

		DirectProjectionStage p4(binder.GenerateTableIndex());
		auto lengths = ComputeLengths(p4, targets);
		plan = p4.Finish(std::move(plan));

		auto window_index = binder.GenerateTableIndex();
		auto window = make_uniq<LogicalWindow>(window_index);
		auto sum =
		    DirectWindowExtremum(optimizer, "sum", calc.Out(calc.Col(LogicalType::DOUBLE, lengths.squared_length)));
		PartitionByGroup(lengths.carried, *sum);
		window->expressions.push_back(std::move(sum));
		window->children.push_back(std::move(plan));

		DirectProjectionStage p5(binder.GenerateTableIndex());
		for (idx_t i = 0; i < source_bindings.size(); i++) {
			p5.Add(calc.Out(calc.Col(source_types[i], lengths.carried.source[i])));
		}
		auto total = calc.Coalesce({calc.Col(LogicalType::DOUBLE, ColumnBinding(window_index, 0)), calc.Num(0.0)});
		auto scale = Scale(lengths, total);
		p5.Add(scale.answer);
		vector<unique_ptr<Expression>> checks;
		checks.push_back(calc.Out(calc.Col(LogicalType::BOOLEAN, lengths.valid)));
		checks.push_back(DirectRaiseTooLarge(optimizer, total, condition.objective_written));
		checks.push_back(DirectWarnIf(calc, calc.Out(scale.loses_precision)));
		p5.Add(DirectAllHold(calc, std::move(checks)));

		DirectSubstitute substitute;
		substitute.prunable_decisions.push_back(1);
		// The source columns, then x. The window holds every row, and the validation column reads it.
		for (idx_t i = 0; i < source_bindings.size() + 1; i++) {
			substitute.slot_map.push_back(i);
		}
		substitute.validated_columns.push_back(source_bindings.size() + 1);
		substitute.child = p5.Finish(std::move(window));
		return substitute;
	}

private:
	//! The columns every stage forwards.
	struct Carried {
		vector<ColumnBinding> source;
		//! Set only for a scoped clause.
		ColumnBinding eligible;
		vector<ColumnBinding> keys;
		//! The types of `keys`.
		vector<LogicalType> key_types;
	};
	struct Raw {
		Carried carried;
		//! What the aggregates of the limit read, one member per input of the limit's group bound.
		vector<DirectGroupInputColumns> limit_inputs;
		vector<DirectQuadraticColumns> parts;
		vector<unique_ptr<Expression>> checks;
	};
	//! Columns of P2.
	struct Coefficients {
		Carried carried;
		vector<DirectGroupInputColumns> limit_inputs;
		ColumnBinding a;
		ColumnBinding b;
		ColumnBinding valid;
	};
	//! Columns of P3.
	struct Targets {
		Carried carried;
		ColumnBinding limit;
		ColumnBinding target;
		ColumnBinding value;
		ColumnBinding in_ball;
		//! Whether the target lost precision (DirectTurningPoint): false on a row with no square.
		ColumnBinding loses_precision;
		ColumnBinding valid;
	};
	//! Columns of P4.
	struct Lengths {
		Carried carried;
		ColumnBinding limit;
		ColumnBinding value;
		ColumnBinding in_ball;
		ColumnBinding squared_length;
		ColumnBinding valid;
	};

	const Condition &condition;
	Optimizer &optimizer;
	Binder &binder;
	ExpressionBuilder calc;
	vector<ColumnBinding> source_bindings;
	vector<LogicalType> source_types;
	const DirectStateErrors &errors;
	//! The group bound of the data limit; null for a constant limit.
	unique_ptr<DecideGroupBound> limit_bound;

	ExprPtr Double(ColumnBinding binding) const {
		return calc.Col(LogicalType::DOUBLE, binding);
	}
	ExprPtr Boolean(ColumnBinding binding) const {
		return calc.Col(LogicalType::BOOLEAN, binding);
	}

	Carried Forward(DirectProjectionStage &stage, const Carried &from) const {
		Carried to;
		for (idx_t i = 0; i < from.source.size(); i++) {
			to.source.push_back(stage.Add(calc.Out(calc.Col(source_types[i], from.source[i]))));
		}
		if (condition.scoped) {
			to.eligible = stage.Add(calc.Out(Boolean(from.eligible)));
		}
		for (idx_t i = 0; i < from.keys.size(); i++) {
			to.keys.push_back(stage.Add(calc.Out(calc.Col(from.key_types[i], from.keys[i]))));
		}
		to.key_types = from.key_types;
		return to;
	}

	//! Makes `window` run over the rows of the row's group. A row outside every ball reads NULL in the window's input,
	//! so it adds nothing to the group whose key it shares, and the answer does not read what the window returns on it.
	void PartitionByGroup(const Carried &carried, BoundWindowExpression &window) const {
		for (idx_t i = 0; i < carried.keys.size(); i++) {
			window.partitions.push_back(calc.Out(calc.Col(carried.key_types[i], carried.keys[i])));
		}
	}

	//! A copy of a proved expression read over a projection that carries the source first; `index` is INVALID_INDEX
	//! when the source's own columns are still in reach.
	unique_ptr<Expression> Over(const Expression &expr, idx_t index) const {
		return index == DConstants::INVALID_INDEX ? expr.Copy() : DirectOverProjection(expr, source_bindings, index);
	}

	//===------------------------------------------------------------------===//
	// P1: raw values
	//===------------------------------------------------------------------===//

	Raw LayoutRawValues(DirectProjectionStage &p1, const Carried &from_scope, idx_t state_index) const {
		Raw raw;
		raw.carried = Forward(p1, from_scope);
		if (limit_bound) {
			auto eligible = condition.scoped ? Boolean(from_scope.eligible) : nullptr;
			auto inputs = DirectGroupInputs(calc, *limit_bound, eligible,
			                                [&](const Expression &value) { return Over(value, state_index); });
			for (auto &input : inputs) {
				DirectGroupInputColumns columns;
				if (input.reads) {
					columns.reads = p1.Add(calc.Out(input.reads));
				}
				if (input.argument) {
					columns.argument = p1.Add(calc.Out(input.argument));
				}
				if (input.invalid) {
					auto message = calc.Col(LogicalType::VARCHAR, p1.Add(calc.Out(input.invalid)));
					raw.checks.push_back(DirectRaiseMessage(optimizer, calc, message));
				}
				raw.limit_inputs.push_back(columns);
			}
		}
		auto over_state = [&](const Expression &value) {
			return Over(value, state_index);
		};
		for (auto &part : condition.parts) {
			raw.parts.push_back(
			    DirectLayoutQuadraticPart(optimizer, calc, p1, part, 1, source_bindings, over_state, raw.checks));
		}
		return raw;
	}

	//===------------------------------------------------------------------===//
	// P2: K and b of the row, oriented as a minimum
	//===------------------------------------------------------------------===//

	Coefficients ComputeCoefficients(DirectProjectionStage &p2, Raw &raw) const {
		Coefficients result;
		result.carried = Forward(p2, raw.carried);
		for (idx_t i = 0; i < raw.limit_inputs.size(); i++) {
			DirectGroupInputColumns columns;
			if (raw.limit_inputs[i].reads.table_index != DConstants::INVALID_INDEX) {
				columns.reads = p2.Add(calc.Out(Boolean(raw.limit_inputs[i].reads)));
			}
			if (raw.limit_inputs[i].argument.table_index != DConstants::INVALID_INDEX) {
				auto type = limit_bound->inputs[i].argument->return_type;
				columns.argument = p2.Add(calc.Out(calc.Col(type, raw.limit_inputs[i].argument)));
			}
			result.limit_inputs.push_back(columns);
		}
		auto sum = DirectFoldQuadratic(optimizer, calc, condition.parts, raw.parts, 0);
		auto sense = calc.Num(condition.sense == DecideSense::MAXIMIZE ? -1.0 : 1.0);
		result.a = p2.Add(calc.Out(calc.Mul(sense, sum.a)));
		result.b = p2.Add(calc.Out(calc.Mul(sense, sum.b)));
		// Every check is true or raises; a row passes when all of them do.
		result.valid = p2.Add(DirectAllHold(calc, std::move(raw.checks)));
		return result;
	}

	//===------------------------------------------------------------------===//
	// P3: the row's target or direction, and its ball
	//===------------------------------------------------------------------===//

	Targets ComputeTargets(DirectProjectionStage &p3, const Coefficients &coefficients,
	                       const DirectGroupValues &limit_values) const {
		Targets result;
		result.carried = Forward(p3, coefficients.carried);
		auto a = Double(coefficients.a);
		auto b = Double(coefficients.b);
		auto zero = calc.Num(0.0);
		// With a square the target -b / (2*K) (DirectTurningPoint). A row with no square has no objective at all.
		// Without one the direction -b.
		auto turning = DirectTurningPoint(calc, a, b, false);
		auto target = condition.has_square
		                  ? calc.If(calc.Cmp(ExpressionType::COMPARE_GREATERTHAN, a, zero), turning.target, zero)
		                  : calc.Neg(b);
		auto value = condition.nonnegative ? calc.Max({target, zero}) : target;
		// A row is in a ball when the clause keeps it and the group has a limit.
		auto in_ball = condition.scoped ? Boolean(coefficients.carried.eligible) : calc.Bool(true);
		ExprPtr limit = calc.Num(condition.limit);
		if (limit_bound) {
			limit = calc.Cast(limit_values.upper, LogicalType::DOUBLE);
			in_ball = calc.And(in_ball, calc.Not(calc.IsNull(limit)));
		}
		result.limit = p3.Add(calc.Out(limit));
		result.target = p3.Add(calc.Out(target));
		result.value = p3.Add(calc.Out(value));
		result.in_ball = p3.Add(calc.Out(in_ball));
		result.loses_precision = p3.Add(calc.Out(turning.loses_precision));
		// The checks of the row's values hold first, then what the limit gives: a group's NaN limit.
		vector<unique_ptr<Expression>> checks;
		checks.push_back(calc.Out(Boolean(coefficients.valid)));
		for (auto &message : limit_values.messages) {
			checks.push_back(DirectRaiseMessage(optimizer, calc, message));
		}
		result.valid = p3.Add(DirectAllHold(calc, std::move(checks)));
		return result;
	}

	//===------------------------------------------------------------------===//
	// P4: the squared length, and the checks of a row
	//===------------------------------------------------------------------===//

	Lengths ComputeLengths(DirectProjectionStage &p4, const Targets &targets) const {
		Lengths result;
		result.carried = Forward(p4, targets.carried);
		auto value = Double(targets.value);
		auto in_ball = Boolean(targets.in_ball);
		auto limit = Double(targets.limit);
		auto zero = calc.Num(0.0);
		auto squared_length = calc.If(in_ball, calc.Mul(value, value), calc.Constant(Value(LogicalType::DOUBLE)));
		result.limit = p4.Add(calc.Out(limit));
		result.value = p4.Add(calc.Out(value));
		result.in_ball = p4.Add(calc.Out(in_ball));
		result.squared_length = p4.Add(calc.Out(squared_length));

		vector<unique_ptr<Expression>> checks;
		checks.push_back(calc.Out(Boolean(targets.valid)));
		checks.push_back(DirectRaiseTooLarge(optimizer, Double(targets.target), condition.objective_written));
		checks.push_back(DirectRaiseTooLarge(optimizer, squared_length, condition.objective_written));
		auto negative = calc.And(in_ball, calc.Cmp(ExpressionType::COMPARE_LESSTHAN, limit, zero));
		checks.push_back(DirectRaiseIf(calc, calc.Out(negative), DirectInfeasibleErrorPredicate(optimizer, errors)));
		if (!condition.has_square) {
			// A direction with no limit improves without end: outside every ball, or in a ball whose limit B the user
			// wrote as infinite. B itself is tested: B / q can pass a DOUBLE where B does not.
			auto no_limit = calc.If(
			    in_ball, calc.Cmp(ExpressionType::COMPARE_EQUAL, limit, calc.Num(POSITIVE_INFINITY)), calc.Bool(true));
			auto moves = calc.Cmp(ExpressionType::COMPARE_NOTEQUAL, value, zero);
			checks.push_back(DirectRaiseIf(calc, calc.Out(calc.And(no_limit, moves)),
			                               DirectUnboundedErrorPredicate(optimizer, errors)));
		} else {
			// A warning reads every row the clause keeps, before any is released, as the checks above do.
			checks.push_back(DirectWarnIf(calc, calc.Out(Boolean(targets.loses_precision))));
		}
		result.valid = p4.Add(DirectAllHold(calc, std::move(checks)));
		return result;
	}

	//===------------------------------------------------------------------===//
	// P5: the answer
	//===------------------------------------------------------------------===//

	//! The ball's radius is sqrt(B / q), computed as sqrt(B) / sqrt(q): B / q passes a DOUBLE where B does not when q is
	//! below 1. B is negative only on a row that P4 has raised for, so no square root is taken of a negative number.
	ExprPtr Radius(const ExprPtr &limit) const {
		return calc.Div(calc.Fn("sqrt", limit), calc.Num(std::sqrt(condition.square_factor)));
	}

	//! A row's answer and whether it lost precision (DirectWarnIf).
	struct Scaled {
		ExprPtr answer;
		ExprPtr loses_precision;
	};

	//! The scale is the radius over the length sqrt(h), computed as (value / sqrt(h)) * radius: the first factor is at
	//! most 1, where sqrt(B) / sqrt(h) passes a DOUBLE when h is subnormal. A row outside every ball keeps its target,
	//! and with no square its direction is 0 (P4 raised otherwise).
	//!
	//! A row reads the scale when a ball holds it and, with a square, its target lies outside that ball. A row with a
	//! square inside the ball returns its own value and reads no scale. h below the normal range of a DOUBLE has lost
	//! precision, so a row that reads the scale with a value of its own, or finds h 0 with one, warns.
	Scaled Scale(const Lengths &lengths, const ExprPtr &total) const {
		auto value = Double(lengths.value);
		auto zero = calc.Num(0.0);
		auto radius = Radius(Double(lengths.limit));
		auto length = calc.Fn("sqrt", total);
		auto scaled = calc.Mul(calc.Div(value, length), radius);
		auto reads = Boolean(lengths.in_ball);
		ExprPtr answer;
		if (condition.has_square) {
			reads = calc.And(reads, calc.Cmp(ExpressionType::COMPARE_GREATERTHAN, length, radius));
			answer = calc.If(reads, scaled, value);
		} else {
			auto still = calc.Cmp(ExpressionType::COMPARE_EQUAL, total, zero);
			answer = calc.If(calc.And(reads, calc.Not(still)), scaled, zero);
		}
		auto has_value = calc.Not(calc.Cmp(ExpressionType::COMPARE_EQUAL, value, zero));
		Scaled result;
		// Adding 0.0 turns -0.0 into 0.0.
		result.answer = calc.Out(calc.Add(answer, zero));
		result.loses_precision = calc.Out(calc.And(reads, calc.And(has_value, DirectBelowNormalRange(calc, total))));
		return result;
	}
};

DirectSubstitute Rule::Apply(DirectSourceReader &reader, Optimizer &optimizer,
                             const DirectApplicabilityCondition &candidate, const DirectStateErrors &errors) const {
	auto &condition = static_cast<const Condition &>(candidate);
	PlanBuilder plan_builder(condition, optimizer, reader.SourceBindings(), reader.SourceTypes(), errors);
	return DirectBuildOverOneRead(
	    reader, [&](unique_ptr<LogicalOperator> rows) { return plan_builder.Build(std::move(rows)); });
}

} // namespace direct_n1_euclidean_ball

unique_ptr<DirectSolveRule> MakeN1EuclideanBallRule() {
	return make_uniq<direct_n1_euclidean_ball::Rule>();
}

} // namespace duckdb
