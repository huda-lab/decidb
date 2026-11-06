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

//! One `sign * scale * SUM(...)` of the objective: its linear terms, per-row constants and square, on the rows it
//! reads.
struct Part {
	DirectLinearPart linear;
	//! The rows the part reads (DirectObjectivePartKept), null for every row.
	unique_ptr<Expression> kept;
	vector<DirectConstantTerm> constants;
	bool has_square = false;
	DirectSquareTerm square;
};

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
	//! `limit`, or `limit_value`, a value of the source row, whose tightest value in a group applies and whose NULL
	//! skips the instance. `limit_invalid` is the message a row raises for a NaN `limit_value`, when the driver's row
	//! checks do not make that check.
	double limit = 0.0;
	unique_ptr<Expression> limit_value;
	unique_ptr<Expression> limit_invalid;
	double square_factor = 1.0;
	vector<Part> parts;
	bool has_square = false;
};

//! The sum of `values` as a constant, where a NULL guard keeps its row (a skipped row reads 0, as coefficient 0). False
//! when one is not a finite constant.
bool FoldSum(ClientContext &context, const vector<DirectSignedValue> &values, double &total) {
	total = 0.0;
	for (auto &value : values) {
		double constant;
		if (!TryEvaluateFoldableDouble(context, DecideUnguarded(*value.expr), constant, DIRECT_FINITE_CONSTANT)) {
			return false;
		}
		total += value.sign * constant;
	}
	return true;
}

//! `sign * scale * w^2` of a square, where its inner slope `w` folds to a constant; false otherwise.
bool FoldSquareFactor(ClientContext &context, const DirectSquareTerm &square, double &factor) {
	double slope;
	if (!FoldSum(context, square.inner_linear, slope)) {
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
		return "min/max SUM(c*x), SUM(x^2) <= B";
	}

	DirectSubstitute Apply(DirectSourceReader &reader, Optimizer &optimizer,
	                       const DirectApplicabilityCondition &candidate) const override;

private:
	bool ProveLimitClause(const DirectIR &ir, ClientContext &context, Condition &condition) const;
	bool ProveLimitValue(const DirectIR &ir, ClientContext &context, const DirectIRConstraint &clause,
	                     Condition &condition) const;
	bool ProveBox(const DirectIR &ir, ClientContext &context, Condition &condition) const;
	bool ProveObjective(const DirectIR &ir, ClientContext &context, Condition &condition) const;
	bool ProveObjectiveTerm(const DirectIR &ir, ClientContext &context, const DecideSplitTerm &term,
	                        Condition &condition, Part &read) const;
	bool ProveSquare(const DirectIR &ir, ClientContext &context, const DecideSplitTerm &term, Part &read) const;
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
	if (terms.size() != 1 || terms[0].kind != DecideTermKind::SQUARE) {
		return false;
	}
	DirectSquareTerm square;
	if (!DirectReadSquareTerm(ir, terms[0], clause->written, square) || !square.inner_constant.empty() ||
	    !FoldSquareFactor(context, square, condition.square_factor) || condition.square_factor <= 0.0) {
		return false;
	}
	return ProveLimitValue(ir, context, *clause, condition);
}

//! A constant the solver reads as a number, or a value of the source row. A limit of magnitude 1e20 or more is where
//! the solver stops reading a right side as a number, so those stay with the solver, and so does a ball whose radius
//! leaves the DOUBLE range. A bound of its group's reducers (`<= sum(budget) by (k)`) reads their placeholders, not the
//! source.
bool Rule::ProveLimitValue(const DirectIR &ir, ClientContext &context, const DirectIRConstraint &clause,
                           Condition &condition) const {
	if (TryEvaluateFoldableDouble(context, *clause.rhs, condition.limit, DIRECT_FINITE_CONSTANT)) {
		return std::fabs(condition.limit) < DECIDE_EFFECTIVE_INFINITY &&
		       std::isfinite(condition.limit / condition.square_factor);
	}
	if (!DirectIsSourceOnlyNumericMayRaise(*clause.rhs, ir.decide_index, ir.source_bindings)) {
		return false;
	}
	condition.limit_value = clause.rhs->Copy();
	// A value that differs between the rows of a group is left as its group's tightest value, with its check.
	if (clause.group_bound && clause.group_bound->inputs[0].invalid) {
		condition.limit_invalid = clause.group_bound->inputs[0].invalid->Copy();
	}
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
bool Rule::ProveSquare(const DirectIR &ir, ClientContext &context, const DecideSplitTerm &term, Part &read) const {
	double factor;
	if (!DirectReadSquareTerm(ir, term, ir.objective.written, read.square) ||
	    !FoldSquareFactor(context, read.square, factor)) {
		return false;
	}
	auto &linear = read.linear;
	double part_factor = linear.scaled ? (linear.divides ? 1.0 / linear.scale : linear.scale) : 1.0;
	double sense = ir.objective.sense == DecideSense::MAXIMIZE ? -1.0 : 1.0;
	double oriented = sense * linear.sign * part_factor * factor;
	read.has_square = std::isfinite(oriented) && oriented > 0.0;
	return read.has_square;
}

//! A term of an objective part: linear (DirectReadLinearPart read it), a per-row constant, or the one square.
bool Rule::ProveObjectiveTerm(const DirectIR &ir, ClientContext &context, const DecideSplitTerm &term,
                              Condition &condition, Part &read) const {
	switch (term.kind) {
	case DecideTermKind::LINEAR:
		return true;
	case DecideTermKind::CONSTANT:
		read.constants.emplace_back();
		return DirectReadConstantTerm(ir, term, ir.objective.written, read.constants.back());
	case DecideTermKind::SQUARE:
		return !condition.has_square && !read.has_square && ProveSquare(ir, context, term, read);
	default:
		return false;
	}
}

//! K is one constant on every row with a linear term. A `when`, or a NULL a guard skips, leaves a row with no square,
//! so a square on some rows must share its part with every linear term.
static bool SquareCoversEveryLinearTerm(const vector<Part> &parts) {
	for (auto &square : parts) {
		bool on_some_rows = square.has_square && square.kept;
		for (auto &inner : square.square.inner_linear) {
			on_some_rows = on_some_rows || &DecideUnguarded(*inner.expr) != inner.expr.get();
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
		if (part.reducer != DirectReducer::SUM || part.inner || part.qualifier != DConstants::INVALID_INDEX ||
		    !part.by.empty()) {
			return false;
		}
		Part read;
		if (!DirectReadLinearPart(ir, context, part, ir.objective.written, read.linear) ||
		    !DirectObjectivePartKept(ir, part, read.kept)) {
			return false;
		}
		for (auto &term : part.terms) {
			if (!ProveObjectiveTerm(ir, context, term, condition, read)) {
				return false;
			}
		}
		condition.has_square = condition.has_square || read.has_square;
		condition.parts.push_back(std::move(read));
	}
	return SquareCoversEveryLinearTerm(condition.parts);
}

//===--------------------------------------------------------------------===//
// Plan
//===--------------------------------------------------------------------===//

//! One projection being assembled.
struct Stage {
	explicit Stage(idx_t index_p) : index(index_p) {
	}
	idx_t index;
	vector<unique_ptr<Expression>> expressions;

	ColumnBinding Add(unique_ptr<Expression> expression) {
		ColumnBinding binding(index, expressions.size());
		expressions.push_back(std::move(expression));
		return binding;
	}
	unique_ptr<LogicalOperator> Finish(unique_ptr<LogicalOperator> child) {
		auto projection = make_uniq<LogicalProjection>(index, std::move(expressions));
		projection->children.push_back(std::move(child));
		return std::move(projection);
	}
};

//! Builds the relational plan for one N1 problem whose condition holds, over one read of the source. Each stage reads
//! only the columns of the stage before it; a window passes its child's columns on as they are.
//!
//!   [W0]  whether a `per K:` clause has an instance, when a NULL in K can leave it none (DirectAddInstanceFlag)
//!   scope the row's membership and group key (DirectProjectScope)
//!   P1    raw values: every coefficient, constant and limit, each on the rows that read it
//!   P2    K and b of the row, and the check on every raw value
//!   [W1]  the group's tightest limit, for a limit read from the data
//!   P3    the row's target (or direction) in the domain, whether it is in a ball, and the ball's limit
//!   P4    the row's squared length in the ball, and every other check a row can raise
//!   W2    h, the sum of the squared lengths of the row's group
//!   P5    the answer, the warning, and the column that keeps the checks live
//!
//! The solver reads every row before it returns, so a bad value on the last row raises even under an outer LIMIT 1. W2
//! holds every row before it releases one, and each check on a row stands below it. The one check above it is h past a
//! DOUBLE, which only the plan computes: the solver has no such number, and every row that would read h raises.
class PlanBuilder {
public:
	PlanBuilder(const Condition &condition_p, Optimizer &optimizer_p, const vector<ColumnBinding> &source_bindings_p,
	            const vector<LogicalType> &source_types_p)
	    : condition(condition_p), optimizer(optimizer_p), binder(optimizer_p.binder), calc(optimizer_p.context),
	      source_bindings(source_bindings_p), source_types(source_types_p) {
		D_ASSERT(source_bindings.size() == source_types.size());
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
		key_types = state.key_types;

		Stage p1(binder.GenerateTableIndex());
		auto raw = LayoutRawValues(p1, from_scope, state.state_index);
		auto plan = p1.Finish(std::move(state.plan));

		Stage p2(binder.GenerateTableIndex());
		auto coefficients = ComputeCoefficients(p2, raw);
		plan = p2.Finish(std::move(plan));
		auto carried = coefficients.carried;

		if (condition.limit_value) {
			auto window_index = binder.GenerateTableIndex();
			auto window = make_uniq<LogicalWindow>(window_index);
			window->expressions.push_back(OverGroup(
			    carried, DirectWindowExtremum(optimizer, "min",
			                                  calc.Out(calc.Col(LogicalType::DOUBLE, coefficients.raw_limit)))));
			window->children.push_back(std::move(plan));
			plan = std::move(window);
			coefficients.raw_limit = ColumnBinding(window_index, 0);
		}

		Stage p3(binder.GenerateTableIndex());
		auto targets = ComputeTargets(p3, coefficients);
		plan = p3.Finish(std::move(plan));

		Stage p4(binder.GenerateTableIndex());
		auto lengths = ComputeLengths(p4, targets);
		plan = p4.Finish(std::move(plan));

		auto window_index = binder.GenerateTableIndex();
		auto window = make_uniq<LogicalWindow>(window_index);
		window->expressions.push_back(OverGroup(
		    lengths.carried,
		    DirectWindowExtremum(optimizer, "sum", calc.Out(calc.Col(LogicalType::DOUBLE, lengths.squared_length)))));
		window->children.push_back(std::move(plan));

		Stage p5(binder.GenerateTableIndex());
		for (idx_t i = 0; i < source_bindings.size(); i++) {
			p5.Add(calc.Out(calc.Col(source_types[i], lengths.carried.source[i])));
		}
		auto total = calc.Coalesce({calc.Col(LogicalType::DOUBLE, ColumnBinding(window_index, 0)), calc.Num(0.0)});
		p5.Add(Answer(lengths, total));
		vector<unique_ptr<Expression>> checks;
		checks.push_back(calc.Out(calc.Col(LogicalType::BOOLEAN, lengths.valid)));
		checks.push_back(TooLarge(total));
		checks.push_back(LosesPrecision(lengths, total));
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
	};
	//! Columns of P1, per part of the objective.
	struct RawPart {
		bool has_mask = false;
		ColumnBinding mask;
		bool has_linear = false;
		ColumnBinding linear;
		//! The square's slope and constant inside it: `(w*x + t)^2`.
		ColumnBinding w;
		ColumnBinding t;
	};
	struct Raw {
		Carried carried;
		ColumnBinding limit;
		vector<RawPart> parts;
		vector<unique_ptr<Expression>> checks;
	};
	//! Columns of P2.
	struct Coefficients {
		Carried carried;
		ColumnBinding raw_limit;
		ColumnBinding a;
		ColumnBinding b;
		ColumnBinding valid;
	};
	//! Columns of P3.
	struct Targets {
		Carried carried;
		ColumnBinding raw_limit;
		ColumnBinding limit;
		ColumnBinding target;
		ColumnBinding value;
		ColumnBinding in_ball;
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
	mutable vector<LogicalType> key_types;

	ExprPtr Double(ColumnBinding binding) const {
		return calc.Col(LogicalType::DOUBLE, binding);
	}
	ExprPtr Boolean(ColumnBinding binding) const {
		return calc.Col(LogicalType::BOOLEAN, binding);
	}
	ExprPtr And(const ExprPtr &a, const ExprPtr &b) const {
		return calc.If(a, b, calc.Bool(false));
	}
	ExprPtr Not(const ExprPtr &a) const {
		return calc.If(a, calc.Bool(false), calc.Bool(true));
	}

	Carried Forward(Stage &stage, const Carried &from) const {
		Carried to;
		for (idx_t i = 0; i < from.source.size(); i++) {
			to.source.push_back(stage.Add(calc.Out(calc.Col(source_types[i], from.source[i]))));
		}
		if (condition.scoped) {
			to.eligible = stage.Add(calc.Out(Boolean(from.eligible)));
		}
		for (idx_t i = 0; i < from.keys.size(); i++) {
			to.keys.push_back(stage.Add(calc.Out(calc.Col(key_types[i], from.keys[i]))));
		}
		return to;
	}

	//! `window` over the rows of the row's group. A row outside every ball reads NULL in the window's input, so it adds
	//! nothing to the group whose key it shares, and the answer does not read what the window returns on it.
	unique_ptr<Expression> OverGroup(const Carried &carried, unique_ptr<BoundWindowExpression> window) const {
		for (idx_t i = 0; i < carried.keys.size(); i++) {
			window->partitions.push_back(calc.Out(calc.Col(key_types[i], carried.keys[i])));
		}
		return std::move(window);
	}

	//! A copy of a proved expression read over a projection that carries the source first; `index` is INVALID_INDEX
	//! when the source's own columns are still in reach.
	unique_ptr<Expression> Over(const Expression &expr, idx_t index) const {
		return index == DConstants::INVALID_INDEX ? expr.Copy() : DirectOverProjection(expr, source_bindings, index);
	}

	//! `sum(sign * value)` over the proved values, read as DOUBLE.
	ExprPtr SumOf(const vector<DirectSignedValue> &values, idx_t index) const {
		ExprPtr total;
		for (auto &value : values) {
			auto term = calc.Wrap(DirectScaledTerm(optimizer, Over(*value.expr, index), value.sign, false, 1.0, false));
			total = total ? calc.Add(total, term) : term;
		}
		return total;
	}

	//! The check of a raw value of P1, over P1: null when the values it is made of are finite constants, which no row
	//! can fail (DirectIsFiniteWhereKept).
	void CheckNumber(const Stage &p1, Raw &raw, ColumnBinding number, const DirectNumberNames &names,
	                 const vector<DirectSignedValue> &values, bool scaled = false, double scale = 1.0,
	                 bool divides = false) const {
		if (DirectIsFiniteWhereKept(optimizer, values, scaled, scale, divides)) {
			return;
		}
		auto message = DirectNumberMessage(calc, Double(number), names, [&](const Expression &input) {
			return calc.Wrap(DirectOverProjection(input, source_bindings, p1.index));
		});
		raw.checks.push_back(DirectRaiseMessage(optimizer, calc, message));
	}

	//! The check of a number computed from values checked before it: past a DOUBLE it names the objective.
	unique_ptr<Expression> TooLarge(const ExprPtr &number) const {
		return DirectRaiseMessage(optimizer, calc, DirectNumberMessage(calc, number, condition.objective_written));
	}

	//===------------------------------------------------------------------===//
	// P1: raw values
	//===------------------------------------------------------------------===//

	Raw LayoutRawValues(Stage &p1, const Carried &from_scope, idx_t state_index) const {
		Raw raw;
		raw.carried = Forward(p1, from_scope);
		if (condition.limit_value) {
			auto value = calc.Cast(calc.Wrap(Over(*condition.limit_value, state_index)), LogicalType::DOUBLE);
			auto eligible = condition.scoped ? Boolean(from_scope.eligible) : nullptr;
			raw.limit = p1.Add(calc.Out(DirectOnKeptRows(calc, eligible, value)));
			if (condition.limit_invalid) {
				auto message = DirectOverProjection(*condition.limit_invalid, source_bindings, p1.index);
				raw.checks.push_back(DirectRaiseMessage(optimizer, calc, calc.Wrap(std::move(message))));
			}
		}
		for (auto &part : condition.parts) {
			raw.parts.push_back(LayoutPart(p1, raw, part, state_index));
		}
		return raw;
	}

	RawPart LayoutPart(Stage &p1, Raw &raw, const Part &part, idx_t state_index) const {
		RawPart columns;
		// A value is computed and checked only on the rows the part keeps.
		ExprPtr kept;
		if (part.kept) {
			columns.has_mask = true;
			kept = calc.Wrap(DirectActiveWhen(calc, Over(*part.kept, state_index).get()));
			columns.mask = p1.Add(calc.Out(kept));
		}
		auto on_kept = [&](const ExprPtr &value) {
			return p1.Add(calc.Out(DirectOnKeptRows(calc, kept, value)));
		};
		vector<DirectSignedValue> linear;
		for (auto &term : part.linear.terms) {
			linear.push_back({term.sign, term.coefficient->Copy()});
		}
		if (!linear.empty()) {
			columns.has_linear = true;
			columns.linear = on_kept(SumOf(linear, state_index));
			CheckNumber(p1, raw, columns.linear, part.linear.names[0], linear);
		}
		if (part.has_square) {
			columns.w = on_kept(SumOf(part.square.inner_linear, state_index));
			auto &constant = part.square.inner_constant;
			columns.t = on_kept(constant.empty() ? calc.Num(0.0) : SumOf(constant, state_index));
			if (!constant.empty()) {
				CheckNumber(p1, raw, columns.t, part.square.inner_constant_names, constant);
			}
		}
		// A constant adds nothing to the answer, so like the solver path it is read one term at a time, with the part's
		// factor applied, and never added up; each must still be valid.
		for (auto &term : part.constants) {
			auto value = calc.Wrap(DirectScaledTerm(optimizer, Over(*term.value, state_index), term.sign,
			                                        part.linear.scaled, part.linear.scale, part.linear.divides));
			vector<DirectSignedValue> read;
			read.push_back({term.sign, term.value->Copy()});
			CheckNumber(p1, raw, on_kept(value), term.value_names, read, part.linear.scaled, part.linear.scale,
			            part.linear.divides);
		}
		return columns;
	}

	//===------------------------------------------------------------------===//
	// P2: K and b of the row, oriented as a minimum
	//===------------------------------------------------------------------===//

	Coefficients ComputeCoefficients(Stage &p2, Raw &raw) const {
		Coefficients result;
		result.carried = Forward(p2, raw.carried);
		if (condition.limit_value) {
			result.raw_limit = p2.Add(calc.Out(Double(raw.limit)));
		}
		ExprPtr a = calc.Num(0.0);
		ExprPtr b = calc.Num(0.0);
		for (idx_t p = 0; p < condition.parts.size(); p++) {
			auto &part = condition.parts[p];
			auto &columns = raw.parts[p];
			auto scaled = [&](const ExprPtr &value) {
				auto result_value =
				    calc.Wrap(DirectScaledTerm(optimizer, calc.Out(value), part.linear.sign, part.linear.scaled,
				                               part.linear.scale, part.linear.divides));
				return columns.has_mask ? calc.If(Boolean(columns.mask), result_value, calc.Num(0.0)) : result_value;
			};
			if (columns.has_linear) {
				b = calc.Add(b, scaled(Double(columns.linear)));
			}
			if (part.has_square) {
				auto k = calc.Num(part.square.sign * part.square.scale);
				auto w = Double(columns.w);
				auto t = Double(columns.t);
				a = calc.Add(a, scaled(calc.Mul(k, calc.Mul(w, w))));
				b = calc.Add(b, scaled(calc.Mul(calc.Num(2.0), calc.Mul(k, calc.Mul(w, t)))));
			}
		}
		auto sense = calc.Num(condition.sense == DecideSense::MAXIMIZE ? -1.0 : 1.0);
		result.a = p2.Add(calc.Out(calc.Mul(sense, a)));
		result.b = p2.Add(calc.Out(calc.Mul(sense, b)));
		// Every check is true or raises; a row passes when all of them do.
		result.valid = p2.Add(DirectAllHold(calc, std::move(raw.checks)));
		return result;
	}

	//===------------------------------------------------------------------===//
	// P3: the row's target or direction, and its ball
	//===------------------------------------------------------------------===//

	Targets ComputeTargets(Stage &p3, const Coefficients &coefficients) const {
		Targets result;
		result.carried = Forward(p3, coefficients.carried);
		auto a = Double(coefficients.a);
		auto b = Double(coefficients.b);
		auto zero = calc.Num(0.0);
		// With a square the target -b / (2*K), and a row with no square has no objective at all. Without one the
		// direction -b.
		auto target = condition.has_square ? calc.If(calc.Cmp(ExpressionType::COMPARE_GREATERTHAN, a, zero),
		                                             calc.Div(calc.Neg(b), calc.Mul(calc.Num(2.0), a)), zero)
		                                   : calc.Neg(b);
		auto value = condition.nonnegative ? calc.Max({target, zero}) : target;
		// A row is in a ball when the clause keeps it and the group has a limit.
		auto in_ball = condition.scoped ? Boolean(coefficients.carried.eligible) : calc.Bool(true);
		ExprPtr limit = calc.Num(condition.limit);
		if (condition.limit_value) {
			limit = Double(coefficients.raw_limit);
			in_ball = And(in_ball, Not(calc.IsNull(limit)));
		}
		result.raw_limit = p3.Add(calc.Out(limit));
		result.limit = p3.Add(calc.Out(calc.Div(limit, calc.Num(condition.square_factor))));
		result.target = p3.Add(calc.Out(target));
		result.value = p3.Add(calc.Out(value));
		result.in_ball = p3.Add(calc.Out(in_ball));
		result.valid = p3.Add(calc.Out(Boolean(coefficients.valid)));
		return result;
	}

	//===------------------------------------------------------------------===//
	// P4: the squared length, and the checks of a row
	//===------------------------------------------------------------------===//

	Lengths ComputeLengths(Stage &p4, const Targets &targets) const {
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
		checks.push_back(TooLarge(Double(targets.target)));
		checks.push_back(TooLarge(squared_length));
		auto negative = And(in_ball, calc.Cmp(ExpressionType::COMPARE_LESSTHAN, Double(targets.raw_limit), zero));
		checks.push_back(DirectRaiseIf(calc, calc.Out(negative), DirectInfeasibleErrorPredicate(optimizer)));
		if (!condition.has_square) {
			// A direction with no limit improves without end: outside every ball, or in a ball of infinite radius.
			auto no_limit = calc.If(
			    in_ball, calc.Cmp(ExpressionType::COMPARE_EQUAL, limit, calc.Num(POSITIVE_INFINITY)), calc.Bool(true));
			auto moves = calc.Cmp(ExpressionType::COMPARE_NOTEQUAL, value, zero);
			checks.push_back(
			    DirectRaiseIf(calc, calc.Out(And(no_limit, moves)), DirectUnboundedErrorPredicate(optimizer)));
		}
		result.valid = p4.Add(DirectAllHold(calc, std::move(checks)));
		return result;
	}

	//===------------------------------------------------------------------===//
	// P5: the answer
	//===------------------------------------------------------------------===//

	//! The radius over the length is sqrt(B / h). It is computed as sqrt(B) / sqrt(h), which stays in range where B / h
	//! does not. A row outside every ball keeps its target, and with no square its direction is 0 (P4 raised
	//! otherwise).
	//! h below the normal range of a DOUBLE has lost precision, so a row that reads it through the scale, or finds it 0
	//! with a value of its own, warns. A row with a square inside the ball returns its own value and reads no scale.
	unique_ptr<Expression> LosesPrecision(const Lengths &lengths, const ExprPtr &total) const {
		auto value = Double(lengths.value);
		auto has_value = Not(calc.Cmp(ExpressionType::COMPARE_EQUAL, value, calc.Num(0.0)));
		auto unstable = And(Boolean(lengths.in_ball), And(has_value, DirectBelowNormalRange(calc, total)));
		if (condition.has_square) {
			unstable = And(unstable, calc.Cmp(ExpressionType::COMPARE_GREATERTHAN, total, Double(lengths.limit)));
		}
		return DirectWarnIf(calc, calc.Out(unstable));
	}

	unique_ptr<Expression> Answer(const Lengths &lengths, const ExprPtr &total) const {
		auto value = Double(lengths.value);
		auto in_ball = Boolean(lengths.in_ball);
		auto limit = Double(lengths.limit);
		auto zero = calc.Num(0.0);
		auto scaled = calc.Mul(value, calc.Div(calc.Fn("sqrt", limit), calc.Fn("sqrt", total)));
		ExprPtr answer;
		if (condition.has_square) {
			auto outside = calc.Cmp(ExpressionType::COMPARE_GREATERTHAN, total, limit);
			answer = calc.If(And(in_ball, outside), scaled, value);
		} else {
			auto still = calc.Cmp(ExpressionType::COMPARE_EQUAL, total, zero);
			answer = calc.If(And(in_ball, Not(still)), scaled, zero);
		}
		// Adding 0.0 turns -0.0 into 0.0.
		return calc.Out(calc.Add(answer, zero));
	}
};

DirectSubstitute Rule::Apply(DirectSourceReader &reader, Optimizer &optimizer,
                             const DirectApplicabilityCondition &candidate) const {
	auto &condition = static_cast<const Condition &>(candidate);
	PlanBuilder plan_builder(condition, optimizer, reader.SourceBindings(), reader.SourceTypes());
	return DirectBuildOverOneRead(
	    reader, [&](unique_ptr<LogicalOperator> rows) { return plan_builder.Build(std::move(rows)); });
}

} // namespace direct_n1_euclidean_ball

unique_ptr<DirectSolveRule> MakeN1EuclideanBallRule() {
	return make_uniq<direct_n1_euclidean_ball::Rule>();
}

} // namespace duckdb
