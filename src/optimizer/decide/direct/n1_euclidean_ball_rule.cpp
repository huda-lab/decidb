#include "duckdb/optimizer/decide/direct/direct_rules.hpp"

#include <cmath>

#include "duckdb/common/decide_numeric_policy.hpp"
#include "duckdb/function/aggregate_function.hpp"
#include "duckdb/optimizer/decide/direct/direct_bound_reader.hpp"
#include "duckdb/optimizer/decide/direct/direct_expression.hpp"
#include "duckdb/optimizer/decide/direct/direct_plan_fragments.hpp"
#include "duckdb/optimizer/decide/direct/direct_runtime_checks.hpp"
#include "duckdb/optimizer/decide/direct/direct_source_reader.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/decide/decide_source_provenance.hpp"
#include "duckdb/planner/decide/decide_term_split.hpp"
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
	//! The objective as a message names it: a number computed past a DOUBLE names it, as the solver path names one.
	string objective_written;
	//! The clause's scope.
	DirectSharedScope scope;
	//! The limit B and the factor q of the sum of squares, so the ball is `SUM(x^2) <= B / q`. B is a constant, or a
	//! limit read from the data, whose NULL skips the instance.
	DirectLimit limit;
	double square_factor = 1.0;
	vector<DirectQuadraticPart> parts;
	bool has_square = false;
};

//! `weight * w^2` of a square, where its weight and its inner slope `w` fold to constants; false otherwise. A weight
//! read from the data is not one: the rule declines it here.
bool FoldSquareFactor(ClientContext &context, const DirectWeightedForm &square, double &factor) {
	double weight, slope;
	if (!DirectFoldConstantSum(context, square.weight, weight) ||
	    !DirectFoldConstantSum(context, square.inner.linear, slope)) {
		return false;
	}
	factor = weight * (slope * slope);
	return std::isfinite(factor);
}

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
		condition->objective_written = ir.objective.written;
		condition->source_reads = 2;
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
	bool ProveBox(const DirectIR &ir, ClientContext &context, Condition &condition) const;
	bool ProveObjective(const DirectIR &ir, ClientContext &context, Condition &condition) const;
};

//! The one aggregate clause: a plain `SUM` of one square, centered on zero, with `<=`. Every other clause is a per-row
//! bound, which ProveBox reads.
bool Rule::ProveLimitClause(const DirectIR &ir, ClientContext &context, Condition &condition) const {
	const DirectIRConstraint *clause = nullptr;
	for (auto &ir_node : ir.constraints) {
		if (!ir_node.aggregate) {
			if (!ir_node.singleton_row) {
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
	if (!DirectProveSharedScope(ir, {clause}, condition.scope)) {
		return false;
	}
	// DirectProveSharedScope has proved that the clause is a plain SUM.
	auto &terms = clause->PlainSum()->terms;
	if (terms.size() != 1 || !terms[0].IsSquare()) {
		return false;
	}
	DirectWeightedForm square;
	if (!DirectReadSquareTerm(ir, terms[0], clause->written, square) || !square.inner.constant.empty() ||
	    !FoldSquareFactor(context, square, condition.square_factor) || condition.square_factor <= 0.0) {
		return false;
	}
	return DirectReadLimit(ir, context, *clause, condition.limit);
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

//! The square of an objective part: its factor folds to one constant, and it is convex.
static bool SquareIsConvex(ClientContext &context, const DirectQuadraticPart &part) {
	double factor;
	if (!FoldSquareFactor(context, part.squares[0], factor)) {
		return false;
	}
	double oriented = DirectOriented(part.linear, factor);
	return std::isfinite(oriented) && oriented > 0.0;
}

//! K is one constant on every row with a linear term. A `when`, or a NULL a guard skips, leaves a row with no square,
//! so a square on some rows must share its part with every linear term. Every part reads the decision (the binder
//! refuses one that does not), so a part beside it has a linear term, or is the square's part.
//!
//! The NULL guard of a nullable `t` in `(w*x + t)^2` wraps every coefficient of the term (GuardCoefficients), `w` among
//! them, so a guarded `w` is how the square's absence on a row shows here. This reads that reach: a guard that spared
//! `w` would leave the square on every row and this check blind to it.
static bool SquareCoversEveryLinearTerm(const vector<DirectQuadraticPart> &parts) {
	for (auto &square : parts) {
		bool on_some_rows = !square.squares.empty() && square.kept;
		for (auto &term : square.squares) {
			for (auto &inner : term.inner.linear) {
				on_some_rows = on_some_rows || DecideIsGuarded(*inner.expr);
			}
		}
		if (on_some_rows && parts.size() > 1) {
			return false;
		}
	}
	return true;
}

//! No objective, or `SUM` parts of linear terms, per-row constants and at most one square, convex.
bool Rule::ProveObjective(const DirectIR &ir, ClientContext &context, Condition &condition) const {
	for (auto &part : ir.objective.parts) {
		DirectQuadraticPart read;
		if (!part.IsPlainSum() || !DirectReadQuadraticPart(ir, context, part, ir.objective.written, read)) {
			return false;
		}
		if (!read.squares.empty()) {
			if (condition.has_square || read.squares.size() > 1 || !SquareIsConvex(context, read)) {
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

//! Builds the relational plan for one N1 problem whose condition holds, as group-then-join (DirectJoinClassAnswers)
//! over two reads of the source. The first read computes the sum of the squared lengths of each ball's rows once, on
//! the group's one row; the second gives every row its target and its group's sum. Each stage reads only the columns
//! of the stage before it; a window passes its child's columns on as they are.
//!
//! The first read:
//!
//!   [W0]  whether a `per K:` clause has an instance, when a NULL in K can leave it none (DirectAddInstanceFlag)
//!   scope the row's membership and group key (DirectProjectScope)
//!   P1    raw values: every coefficient, constant and limit, each on the rows that read it
//!   P2    K and b of the row, and the check on every raw value
//!   [W1]  the group's tightest limit, for a limit read from the data
//!   P3    the row's target (or direction) in the domain, whether it lost precision, whether it is in a ball, and the
//!         ball's limit
//!   P4    the row's squared length in the ball, every other check a row can raise, and the target's warning
//!   H     a hash aggregate on the group key, one row per group (one row for a single ball): h, the sum of the squared
//!         lengths; whether the checks of the rows held; and the group's limit, for a limit read from the data
//!   C     the group's values, computed once on its row (loop-invariant code motion: they depend on the group alone):
//!         the length sqrt(h), whether h is below the normal range of a DOUBLE, whether h is 0, and the checks of the
//!         rows together with h's own: h past a DOUBLE (the solver has no such number, and every row that would read h
//!         raises)
//!
//! The second read is [W0], the scope, P1 and P2 again and the row's target and value (P3), joined with the group's row
//! on the key, then
//!
//!   P5    the answer, the warning of the scale, and the column that keeps the checks live
//!
//! The solver reads every row before it returns, so a bad value on the last row raises even under an outer LIMIT 1. H
//! holds every row of the first read before it gives one, and each check on a row stands below it, so no joined row
//! exists before every row has passed its checks. The second read computes P1 and P2 again for the row's target and
//! raises nothing: the first read has raised on any value it would. A row outside every ball reads NULL in H's input,
//! so it adds nothing to the group whose key it shares, and the answer does not read what the join gives it. Because C
//! raises for h when the groups are built, a group whose h passes a DOUBLE raises whether or not a streamed row reads
//! it, as the solver, which reads every row, would. The row's warning of the scale stays on the row: it prints for a
//! row that reads the scale with a value of its own.
class PlanBuilder {
public:
	PlanBuilder(const Condition &condition_p, Optimizer &optimizer_p, const vector<ColumnBinding> &source_bindings_p,
	            const vector<LogicalType> &source_types_p, const DirectStateErrors &errors_p)
	    : condition(condition_p), optimizer(optimizer_p), binder(optimizer_p.binder), calc(optimizer_p.context),
	      source_bindings(source_bindings_p), source_types(source_types_p), errors(errors_p) {
		D_ASSERT(source_bindings.size() == source_types.size());
		if (!condition.limit.constant) {
			limit_bound = condition.limit.data.GroupBound(optimizer);
		}
	}

	//! The class plan over the first read `rows`: the group key's columns, then the group's values
	//! (ComputeClassValues), one row per group.
	unique_ptr<LogicalOperator> BuildClasses(unique_ptr<LogicalOperator> rows) const {
		auto state = ScopeOf(std::move(rows));
		auto from_scope = DirectCarriedOf(state, source_bindings.size());

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
			                                   condition.scope.Scoped() ? Boolean(carried.eligible) : nullptr);
			limit_window->children.push_back(std::move(plan));
			plan = std::move(limit_window);
		}

		DirectProjectionStage p3(binder.GenerateTableIndex());
		auto targets = ComputeTargets(p3, coefficients, limit_values);
		plan = p3.Finish(std::move(plan));

		DirectProjectionStage p4(binder.GenerateTableIndex());
		auto lengths = ComputeLengths(p4, targets);
		plan = p4.Finish(std::move(plan));

		DirectAggregateStage stage(binder);
		for (idx_t i = 0; i < lengths.carried.keys.size(); i++) {
			stage.Group(calc.Out(calc.Col(lengths.carried.key.types[i], lengths.carried.keys[i])));
		}
		stage.Add(DirectHashAggregate(optimizer, calc, "sum", Double(lengths.squared_length)));
		stage.Add(DirectHashAggregate(optimizer, calc, "bool_and", Boolean(lengths.valid)));
		if (limit_bound) {
			// The limit is one value on the rows a group's ball holds, and NULL on the others.
			stage.Add(DirectHashAggregate(optimizer, calc, "max", Double(lengths.limit)));
		}
		return ComputeClassValues(stage.Finish(std::move(plan)), lengths.carried.key);
	}

	//! The substitute over the second read `rows`: its source columns, then each row's answer, and the column that
	//! keeps the class plan's checks live (P5).
	DirectSubstitute JoinAnswers(unique_ptr<LogicalOperator> rows, unique_ptr<LogicalOperator> classes) const {
		auto state = ScopeOf(std::move(rows));
		auto from_scope = DirectCarriedOf(state, source_bindings.size());

		DirectProjectionStage p1(binder.GenerateTableIndex());
		auto raw = LayoutRawValues(p1, from_scope, state.state_index);
		auto plan = p1.Finish(std::move(state.plan));

		DirectProjectionStage p2(binder.GenerateTableIndex());
		auto coefficients = ComputeCoefficients(p2, raw);
		plan = p2.Finish(std::move(plan));

		DirectProjectionStage p3(binder.GenerateTableIndex());
		auto values = ComputeValues(p3, coefficients);
		plan = p3.Finish(std::move(plan));

		auto &carried = values.carried;
		auto joined = DirectJoinClassAnswers(binder, std::move(plan), carried.keys, carried.key, std::move(classes));
		D_ASSERT(joined.answers.size() == ClassValueCount());
		ClassValues group;
		group.length = Double(joined.answers[0]);
		group.below_normal = Boolean(joined.answers[1]);
		idx_t next = 3;
		if (!condition.has_square) {
			group.still = Boolean(joined.answers[next++]);
		}
		// A row is in a ball when the clause keeps it and the group has a limit.
		auto in_ball = condition.scope.Scoped() ? Boolean(carried.eligible) : calc.Bool(true);
		ExprPtr limit = calc.Num(condition.limit.number);
		if (limit_bound) {
			limit = Double(joined.answers[next]);
			in_ball = calc.And(in_ball, calc.Not(calc.IsNull(limit)));
		}

		DirectProjectionStage p5(binder.GenerateTableIndex());
		for (idx_t i = 0; i < source_bindings.size(); i++) {
			p5.Add(calc.Out(calc.Col(source_types[i], carried.source[i])));
		}
		auto scale = Scale(Double(values.value), limit, in_ball, group);
		p5.Add(scale.answer);
		vector<unique_ptr<Expression>> checks;
		checks.push_back(calc.Out(Boolean(joined.answers[2])));
		checks.push_back(DirectWarnIf(calc, calc.Out(scale.loses_precision)));
		p5.Add(DirectAllHold(calc, std::move(checks)));

		DirectSubstitute substitute;
		substitute.prunable_decisions.push_back(1);
		// The source columns, then x. The class plan's checks are read by the column after them.
		for (idx_t i = 0; i < source_bindings.size() + 1; i++) {
			substitute.slot_map.push_back(i);
		}
		substitute.validated_columns.push_back(source_bindings.size() + 1);
		// The rows are probed against the class plan and held by nothing.
		substitute.pipelined = true;
		substitute.child = p5.Finish(std::move(joined.plan));
		return substitute;
	}

private:
	struct Raw {
		DirectCarried carried;
		//! What the aggregates of the limit read, one member per input of the limit's group bound.
		vector<DirectGroupInputColumns> limit_inputs;
		vector<DirectQuadraticColumns> parts;
		vector<unique_ptr<Expression>> checks;
	};
	//! Columns of P2.
	struct Coefficients {
		DirectCarried carried;
		vector<DirectGroupInputColumns> limit_inputs;
		ColumnBinding a;
		//! The linear coefficient oriented as a minimum; the direction -b of a row with no square.
		ColumnBinding b;
		ColumnBinding valid;
	};
	//! Columns of P3.
	struct Targets {
		DirectCarried carried;
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
		DirectCarried carried;
		ColumnBinding limit;
		ColumnBinding squared_length;
		ColumnBinding valid;
	};
	//! A group's values on the second read, from its row (ComputeClassValues).
	struct ClassValues {
		ExprPtr length;
		ExprPtr below_normal;
		//! Whether h is 0; only a direction without a square reads it.
		ExprPtr still;
	};
	//! Columns of the second read's P3: the row's value in the domain.
	struct Values {
		DirectCarried carried;
		ColumnBinding value;
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

	//! Makes `window` run over the rows of the row's group. A row outside every ball reads NULL in the window's input,
	//! so it adds nothing to the group whose key it shares, and the answer does not read what the window returns on it.
	void PartitionByGroup(const DirectCarried &carried, BoundWindowExpression &window) const {
		for (idx_t i = 0; i < carried.keys.size(); i++) {
			window.partitions.push_back(calc.Out(calc.Col(carried.key.types[i], carried.keys[i])));
		}
	}

	//! The scope of a read of the source: [W0] and the scope projection, which both reads take.
	DirectScopeState ScopeOf(unique_ptr<LogicalOperator> rows) const {
		unique_ptr<Expression> instance;
		if (condition.scope.gated) {
			ColumnBinding flag;
			rows = DirectAddInstanceFlag(optimizer, std::move(rows), condition.scope.when.get(),
			                             condition.scope.classes, flag);
			instance = calc.Out(calc.Col(LogicalType::BOOLEAN, flag));
		}
		return DirectProjectScope(binder, std::move(rows), condition.scope.Scoped(), condition.scope.when.get(),
		                          condition.scope.keys, {}, std::move(instance));
	}

	//! A copy of a proved expression read over a projection that carries the source first; `index` is INVALID_INDEX
	//! when the source's own columns are still in reach.
	unique_ptr<Expression> Over(const Expression &expr, idx_t index) const {
		return index == DConstants::INVALID_INDEX ? expr.Copy() : DirectOverProjection(expr, source_bindings, index);
	}

	//===------------------------------------------------------------------===//
	// P1: raw values
	//===------------------------------------------------------------------===//

	Raw LayoutRawValues(DirectProjectionStage &p1, const DirectCarried &from_scope, idx_t state_index) const {
		Raw raw;
		raw.carried = p1.Forward(from_scope, source_types, condition.scope.Scoped());
		if (limit_bound) {
			auto eligible = condition.scope.Scoped() ? Boolean(from_scope.eligible) : nullptr;
			raw.limit_inputs = DirectLayoutGroupInputs(
			    optimizer, calc, p1, *limit_bound, eligible,
			    [&](const Expression &value) { return Over(value, state_index); }, &raw.checks);
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
		result.carried = p2.Forward(raw.carried, source_types, condition.scope.Scoped());
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
		result.a = p2.Add(calc.Out(sum.a));
		// Without a square the row's target is the direction -b (TargetOf), so b is oriented that way here, and no
		// column is negated again above it.
		result.b = p2.Add(calc.Out(condition.has_square ? sum.b : calc.Mul(calc.Num(-1.0), sum.b)));
		// Every check is true or raises; a row passes when all of them do.
		result.valid = p2.Add(DirectAllHold(calc, std::move(raw.checks)));
		return result;
	}

	//===------------------------------------------------------------------===//
	// P3: the row's target or direction, and its ball
	//===------------------------------------------------------------------===//

	//! The row's target (or direction) and its value in the domain, from K and b of the row.
	struct RowTarget {
		ExprPtr target;
		ExprPtr value;
		//! Whether computing the target lost precision (DirectTurningPoint): false on a row with no square.
		ExprPtr loses_precision;
	};

	RowTarget TargetOf(const Coefficients &coefficients) const {
		auto a = Double(coefficients.a);
		auto b = Double(coefficients.b);
		auto zero = calc.Num(0.0);
		// With a square the target -b / (2*K) (DirectTurningPoint). A row with no square has no objective at all.
		// Without one the direction, which ComputeCoefficients has stored as b.
		auto turning = DirectTurningPoint(calc, a, b, false);
		RowTarget result;
		result.target = condition.has_square
		                    ? calc.If(calc.Cmp(ExpressionType::COMPARE_GREATERTHAN, a, zero), turning.target, zero)
		                    : b;
		result.value = condition.nonnegative ? calc.Max({result.target, zero}) : result.target;
		result.loses_precision = turning.loses_precision;
		return result;
	}

	//! The first read: the row's target, whether it is in a ball, and the ball's limit.
	Targets ComputeTargets(DirectProjectionStage &p3, const Coefficients &coefficients,
	                       const DirectGroupValues &limit_values) const {
		Targets result;
		result.carried = p3.Forward(coefficients.carried, source_types, condition.scope.Scoped());
		auto row = TargetOf(coefficients);
		// A row is in a ball when the clause keeps it and the group has a limit.
		auto in_ball = condition.scope.Scoped() ? Boolean(coefficients.carried.eligible) : calc.Bool(true);
		ExprPtr limit = calc.Num(condition.limit.number);
		if (limit_bound) {
			limit = calc.Cast(limit_values.upper, LogicalType::DOUBLE);
			in_ball = calc.And(in_ball, calc.Not(calc.IsNull(limit)));
		}
		result.limit = p3.Add(calc.Out(limit));
		result.target = p3.Add(calc.Out(row.target));
		result.value = p3.Add(calc.Out(row.value));
		result.in_ball = p3.Add(calc.Out(in_ball));
		result.loses_precision = p3.Add(calc.Out(row.loses_precision));
		// The checks of the row's values hold first, then what the limit gives: a group's NaN limit.
		vector<unique_ptr<Expression>> checks;
		checks.push_back(calc.Out(Boolean(coefficients.valid)));
		for (auto &message : limit_values.messages) {
			checks.push_back(DirectRaiseMessage(optimizer, calc, message));
		}
		result.valid = p3.Add(DirectAllHold(calc, std::move(checks)));
		return result;
	}

	//! The second read: the row's value alone. Its checks ran on the first read.
	Values ComputeValues(DirectProjectionStage &p3, const Coefficients &coefficients) const {
		Values result;
		result.carried = p3.Forward(coefficients.carried, source_types, condition.scope.Scoped());
		result.value = p3.Add(calc.Out(TargetOf(coefficients).value));
		return result;
	}

	//===------------------------------------------------------------------===//
	// P4: the squared length, and the checks of a row
	//===------------------------------------------------------------------===//

	Lengths ComputeLengths(DirectProjectionStage &p4, const Targets &targets) const {
		Lengths result;
		result.carried = p4.Forward(targets.carried, source_types, condition.scope.Scoped());
		auto value = Double(targets.value);
		auto in_ball = Boolean(targets.in_ball);
		auto limit = Double(targets.limit);
		auto zero = calc.Num(0.0);
		auto squared_length = calc.If(in_ball, calc.Mul(value, value), calc.Constant(Value(LogicalType::DOUBLE)));
		result.limit = p4.Add(calc.Out(limit));
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
	// C: the group's values, from its row
	//===------------------------------------------------------------------===//

	//! The columns ComputeClassValues gives after the key: the length, whether h is below the normal range, whether the
	//! checks held, then whether h is 0 (with no square) and the group's limit (read from the data).
	idx_t ClassValueCount() const {
		return 3 + (condition.has_square ? 0 : 1) + (limit_bound ? 1 : 0);
	}

	//! The group's row over the aggregate `classes` (the key, then h, whether the rows' checks held and the limit):
	//! what depends on h alone. The checks of the rows come first in the chain, so h's own raises only for a group
	//! whose rows passed.
	unique_ptr<LogicalOperator> ComputeClassValues(unique_ptr<LogicalOperator> classes,
	                                               const DirectClassKey &key) const {
		auto bindings = classes->GetColumnBindings();
		idx_t keys = key.types.size();
		DirectProjectionStage c(binder.GenerateTableIndex());
		c.Forward(vector<ColumnBinding>(bindings.begin(), bindings.begin() + NumericCast<int64_t>(keys)), key.types);
		// A group with no row in a ball has no h, and no row of it reads one.
		auto total = Double(bindings[keys]);
		c.Add(calc.Fn("sqrt", total));
		c.Add(DirectBelowNormalRange(calc, total));
		vector<unique_ptr<Expression>> checks;
		checks.push_back(calc.Out(Boolean(bindings[keys + 1])));
		checks.push_back(DirectRaiseTooLarge(optimizer, total, condition.objective_written));
		c.Add(DirectAllHold(calc, std::move(checks)));
		if (!condition.has_square) {
			c.Add(calc.Cmp(ExpressionType::COMPARE_EQUAL, total, calc.Num(0.0)));
		}
		if (limit_bound) {
			c.Add(Double(bindings[keys + 2]));
		}
		return c.Finish(std::move(classes));
	}

	//===------------------------------------------------------------------===//
	// P5: the answer
	//===------------------------------------------------------------------===//

	//! The ball's radius is sqrt(B / q), computed as sqrt(B) / sqrt(q): B / q passes a DOUBLE where B does not when q
	//! is below 1. B is negative only on a row that P4 has raised for, so no square root is taken of a negative number.
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
	Scaled Scale(const ExprPtr &value, const ExprPtr &limit, const ExprPtr &in_ball, const ClassValues &group) const {
		auto zero = calc.Num(0.0);
		auto radius = Radius(limit);
		auto scaled = calc.Mul(calc.Div(value, group.length), radius);
		auto reads = in_ball;
		ExprPtr answer;
		if (condition.has_square) {
			reads = calc.And(reads, calc.Cmp(ExpressionType::COMPARE_GREATERTHAN, group.length, radius));
			answer = calc.If(reads, scaled, value);
		} else {
			answer = calc.If(calc.And(reads, calc.Not(group.still)), scaled, zero);
		}
		auto has_value = calc.Not(calc.Cmp(ExpressionType::COMPARE_EQUAL, value, zero));
		Scaled result;
		// Adding 0.0 turns -0.0 into 0.0.
		result.answer = calc.Out(calc.Add(answer, zero));
		result.loses_precision = calc.Out(calc.And(reads, calc.And(has_value, group.below_normal)));
		return result;
	}
};

DirectSubstitute Rule::Apply(DirectSourceReader &reader, Optimizer &optimizer,
                             const DirectApplicabilityCondition &candidate, const DirectStateErrors &errors) const {
	auto &condition = static_cast<const Condition &>(candidate);
	PlanBuilder plan_builder(condition, optimizer, reader.SourceBindings(), reader.SourceTypes(), errors);
	auto classes = DirectBuildOverRead(reader, reader.ReadSource(), [&](unique_ptr<LogicalOperator> rows) {
		return plan_builder.BuildClasses(std::move(rows));
	});
	return DirectBuildOverOneRead(reader, [&](unique_ptr<LogicalOperator> rows) {
		return plan_builder.JoinAnswers(std::move(rows), std::move(classes));
	});
}

} // namespace direct_n1_euclidean_ball

unique_ptr<DirectSolveRule> MakeN1EuclideanBallRule() {
	return make_uniq<direct_n1_euclidean_ball::Rule>();
}

} // namespace duckdb
