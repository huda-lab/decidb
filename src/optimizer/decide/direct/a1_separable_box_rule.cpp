#include "duckdb/optimizer/decide/direct/direct_rules.hpp"

#include "duckdb/common/decide_numeric_policy.hpp"
#include "duckdb/optimizer/decide/direct/direct_bound_reader.hpp"
#include "duckdb/optimizer/decide/direct/direct_plan_fragments.hpp"
#include "duckdb/optimizer/decide/direct/direct_source_reader.hpp"
#include "duckdb/optimizer/decide/direct/direct_runtime_checks.hpp"
#include "duckdb/optimizer/decide/direct/direct_expression.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/binder.hpp"

namespace duckdb {

//! Each rule keeps its helpers in its own named namespace: unity builds compile every rule file in one translation
//! unit, where two anonymous namespaces would merge and same-named helpers would collide.
namespace direct_a1_separable_box {

constexpr const char *RULE_NAME = "A1_SEPARABLE_BOX";

struct Decision {
	//! The declared name, which the whole-number range error quotes.
	string name;
	DirectDomain domain = DirectDomain::REAL;
	LogicalType output_type;
	//! The decision's box before any per-row bound: its default, replaced or tightened by the bounds the solver folds
	//! into the variable.
	double lower = 0.0;
	double upper = POSITIVE_INFINITY;
};

struct Condition final : DirectApplicabilityCondition {
	DecideSense sense = DecideSense::FEASIBILITY;
	//! The objective as written, which a message names when a decision's quadratic or linear coefficient, computed from
	//! finite values, is too large for a DOUBLE (TOO_LARGE), as the solver path names it.
	string objective_written;
	vector<Decision> decisions;
	vector<DirectBoundClause> clauses;
	vector<DirectQuadraticPart> parts;
	//! Every value is a constant and nothing is conditional, so a check that fails on one row fails on all of them.
	bool constant_problem = false;
};

//! No value reads a source column and nothing is conditional: every row sees the same bounds and coefficients.
bool IsConstantProblem(const Condition &condition) {
	auto constant_values = [&](const vector<DirectSignedValue> &values) {
		for (auto &value : values) {
			if (!value.expr->IsFoldable()) {
				return false;
			}
		}
		return true;
	};
	for (auto &clause : condition.clauses) {
		if (clause.when || !clause.rhs->IsFoldable() || !constant_values(clause.coefficient)) {
			return false;
		}
	}
	for (auto &part : condition.parts) {
		if (part.kept) {
			return false;
		}
		for (auto &term : part.linear.terms) {
			if (!term.coefficient->IsFoldable()) {
				return false;
			}
		}
		for (auto &square : part.squares) {
			if (!constant_values(square.inner.linear) || !constant_values(square.inner.constant)) {
				return false;
			}
		}
		for (auto &term : part.constants) {
			if (!term.value->IsFoldable()) {
				return false;
			}
		}
	}
	return true;
}

//===--------------------------------------------------------------------===//
// Rule
//===--------------------------------------------------------------------===//

class Rule final : public DirectSolveRule {
public:
	const char *Name() const override {
		return RULE_NAME;
	}

	unique_ptr<DirectApplicabilityCondition> CheckCondition(const DirectIR &ir, ClientContext &context) const override {
		if (ir.decisions.empty() || !ir.keyed_scopes.empty()) {
			return nullptr;
		}
		for (auto &decision : ir.decisions) {
			if (decision.scope != DecideVarScope::ROW) {
				return nullptr;
			}
		}
		for (auto &ir_node : ir.constraints) {
			if (ir_node.aggregate) {
				return nullptr;
			}
			if (!DirectIsSingletonRow(ir, ir_node)) {
				return nullptr;
			}
		}
		for (auto &part : ir.objective.parts) {
			if (!part.IsPlainSum()) {
				return nullptr;
			}
		}
		auto condition = make_uniq<Condition>();
		condition->sense = ir.objective.sense;
		condition->objective_written = ir.objective.written;
		for (auto &decision : ir.decisions) {
			Decision prepared;
			prepared.name = decision.name;
			prepared.domain = decision.domain;
			prepared.output_type = decision.output_type;
			condition->decisions.push_back(std::move(prepared));
		}
		if (!ProveBounds(ir, context, *condition) || !ProveObjective(ir, context, *condition)) {
			return nullptr;
		}
		condition->constant_problem = IsConstantProblem(*condition);
		// The rows are read once to validate them and once to answer, unless every row raises the same error.
		condition->source_reads = condition->constant_problem ? 1 : 2;
		return std::move(condition);
	}

	string Summary() const override {
		return "min/max SUM(c*x), l <= x <= u, each row independent";
	}

	DirectSubstitute Apply(DirectSourceReader &reader, Optimizer &optimizer,
	                       const DirectApplicabilityCondition &candidate,
	                       const DirectStateErrors &errors) const override;

private:
	bool ProveBounds(const DirectIR &ir, ClientContext &context, Condition &condition) const;
	bool ProveObjective(const DirectIR &ir, ClientContext &context, Condition &condition) const;
};

//! The decision's box before any per-row bound, and the clauses the plan evaluates per row, from the shared reader.
bool Rule::ProveBounds(const DirectIR &ir, ClientContext &context, Condition &condition) const {
	DirectBoundReading reading;
	if (!DirectReadBounds(ir, context, reading)) {
		return false;
	}
	condition.clauses = std::move(reading.clauses);
	for (idx_t decision = 0; decision < condition.decisions.size(); decision++) {
		condition.decisions[decision].lower = reading.boxes[decision].lower;
		condition.decisions[decision].upper = reading.boxes[decision].upper;
	}
	return true;
}

//! Each part reads as linear terms, per-row constants and squares, and the objective holds at most one square (the
//! solver rejects a second).
bool Rule::ProveObjective(const DirectIR &ir, ClientContext &context, Condition &condition) const {
	idx_t squares = 0;
	for (auto &part : ir.objective.parts) {
		DirectQuadraticPart prepared;
		if (!DirectReadQuadraticPart(ir, context, part, ir.objective.written, prepared)) {
			return false;
		}
		for (auto &square : prepared.squares) {
			// A1 takes a nonzero constant weight; one read from the data (or zero) stays on the solver path.
			double weight;
			if (!DirectFoldConstantSum(context, square.weight, weight) || weight == 0.0) {
				return false;
			}
		}
		squares += prepared.squares.size();
		condition.parts.push_back(std::move(prepared));
	}
	return squares <= 1;
}

//===--------------------------------------------------------------------===//
// Plan
//===--------------------------------------------------------------------===//

//! Builds the relational plan for one A1 problem whose condition holds, stage by stage. Each stage is one projection
//! that reads only the columns of the stage before it:
//!
//!   P1 raw values (every coefficient, bound and mask, each computed on the rows that read it and NULL elsewhere)
//!   P2 per-clause bounds, per-decision objective coefficients, and the check on every raw value
//!   P3 each decision's box
//!   P4 each decision's answer and whether its row is infeasible
//!
//! The solver reads every row before it returns, so a bad value on the last row raises even under an outer LIMIT 1.
//! The plan keeps that obligation without holding rows (verify, then stream). When the rows differ, one read runs
//! P1-P4 into a verdict, `bool_and` of every row's checks, true or raising, and the reader gates the second read on
//! it (DirectSourceReader::Gate): that read runs P1-P4 again and releases each row as it comes. When every row sees
//! the same values, the first row raises whatever the others would and the plan is one pipelined read.
class PlanBuilder {
public:
	PlanBuilder(const Condition &condition_p, Optimizer &optimizer_p, const vector<ColumnBinding> &source_bindings_p,
	            const vector<LogicalType> &source_types_p, const DirectStateErrors &errors_p)
	    : condition(condition_p), optimizer(optimizer_p), binder(optimizer_p.binder), calc(optimizer_p.context),
	      source_bindings(source_bindings_p), source_types(source_types_p), errors(errors_p) {
		D_ASSERT(source_bindings.size() == source_types.size());
	}

	//! One row: true when every row of `source` is valid, and otherwise the error of a row that is not.
	unique_ptr<LogicalOperator> BuildVerdict(unique_ptr<LogicalOperator> source) const {
		auto stages = RunStages(std::move(source));
		return DirectAllRowsValid(optimizer, std::move(stages.plan), ValidAnswers(stages.answers));
	}

	//! The source columns and the answers. A source that is read twice has already been validated by the verdict.
	DirectSubstitute Build(unique_ptr<LogicalOperator> source) const {
		auto stages = RunStages(std::move(source));
		DirectSubstitute substitute;
		for (idx_t d = 0; d < condition.decisions.size(); d++) {
			substitute.prunable_decisions.push_back(1);
		}
		for (idx_t i = 0; i < source_bindings.size() + condition.decisions.size(); i++) {
			substitute.slot_map.push_back(i);
		}
		DirectProjectionStage p5(binder.GenerateTableIndex());
		for (idx_t i = 0; i < source_bindings.size(); i++) {
			p5.Add(calc.Out(calc.Col(source_types[i], stages.passthrough[i])));
		}
		for (idx_t d = 0; d < condition.decisions.size(); d++) {
			p5.Add(TypedAnswer(d, stages.answers.value[d]));
		}
		if (condition.constant_problem) {
			// The validation column raises wherever a row is read. An empty source has no row to raise on and returns
			// no rows. A bound the solver rejects outright (against the variable's built-in domain) is never proved,
			// so it cannot differ on an empty input.
			substitute.validated_columns.push_back(source_bindings.size() + condition.decisions.size());
			p5.Add(ValidAnswers(stages.answers));
		}
		substitute.child = p5.Finish(std::move(stages.plan));
		substitute.pipelined = true;
		return substitute;
	}

private:
	//! Columns of P1.
	struct Raw {
		vector<ColumnBinding> source;
		vector<DirectBoundColumns> clauses;
		vector<DirectQuadraticColumns> parts;
	};

	//! Columns of P2: each clause's bounds on its decision, and each decision's objective coefficients.
	struct Bounds {
		vector<ColumnBinding> lower;
		vector<ColumnBinding> upper;
		//! Each decision's static lower end on the row (DirectStaticFloor).
		vector<ColumnBinding> floor;
		vector<ColumnBinding> a;
		vector<ColumnBinding> b;
		ColumnBinding valid_data;
	};

	//! Columns of P3.
	struct Box {
		vector<ColumnBinding> lower;
		vector<ColumnBinding> upper;
		vector<ColumnBinding> a;
		vector<ColumnBinding> b;
		ColumnBinding valid_data;
	};

	//! Columns of P4.
	struct Answers {
		vector<ColumnBinding> value;
		vector<ColumnBinding> infeasible;
		vector<ColumnBinding> a;
		vector<ColumnBinding> b;
		//! Whether the decision's turning point lost precision (DirectTurningPoint).
		vector<ColumnBinding> loses_precision;
		ColumnBinding valid_data;
	};

	//! P1-P4 over a read: the plan, where each source column is in it, and the answers.
	struct Stages {
		unique_ptr<LogicalOperator> plan;
		vector<ColumnBinding> passthrough;
		Answers answers;
	};

	Stages RunStages(unique_ptr<LogicalOperator> source) const {
		DirectProjectionStage p1(binder.GenerateTableIndex());
		vector<unique_ptr<Expression>> checks;
		Raw raw;
		LayoutRawValues(p1, checks, raw);
		Stages stages;
		stages.plan = p1.Finish(std::move(source));

		DirectProjectionStage p2(binder.GenerateTableIndex());
		Bounds bounds;
		auto passthrough2 = p2.Forward(raw.source, source_types);
		ComputeBounds(p2, raw, checks, bounds);
		stages.plan = p2.Finish(std::move(stages.plan));

		DirectProjectionStage p3(binder.GenerateTableIndex());
		auto passthrough3 = p3.Forward(passthrough2, source_types);
		Box box;
		ComputeBox(p3, bounds, box);
		stages.plan = p3.Finish(std::move(stages.plan));

		DirectProjectionStage p4(binder.GenerateTableIndex());
		stages.passthrough = p4.Forward(passthrough3, source_types);
		ComputeAnswers(p4, box, stages.answers);
		stages.plan = p4.Finish(std::move(stages.plan));
		return stages;
	}

	const Condition &condition;
	Optimizer &optimizer;
	Binder &binder;
	ExpressionBuilder calc;
	vector<ColumnBinding> source_bindings;
	vector<LogicalType> source_types;
	const DirectStateErrors &errors;

	//===------------------------------------------------------------------===//
	// P1: raw values
	//===------------------------------------------------------------------===//

	void LayoutRawValues(DirectProjectionStage &p1, vector<unique_ptr<Expression>> &checks, Raw &raw) const {
		for (idx_t i = 0; i < source_bindings.size(); i++) {
			raw.source.push_back(p1.Add(calc.Out(calc.Col(source_types[i], source_bindings[i]))));
		}
		raw.clauses = DirectLayoutBoundClauses(optimizer, calc, p1, condition.clauses, source_bindings, nullptr, checks);
		for (auto &part : condition.parts) {
			raw.parts.push_back(DirectLayoutQuadraticPart(optimizer, calc, p1, part, condition.decisions.size(),
			                                              source_bindings, nullptr, checks));
		}
	}

	//===------------------------------------------------------------------===//
	// P2: per-clause bounds, objective coefficients, raw-value checks
	//===------------------------------------------------------------------===//

	void ComputeBounds(DirectProjectionStage &p2, const Raw &raw, vector<unique_ptr<Expression>> &checks,
	                   Bounds &bounds) const {
		for (idx_t j = 0; j < condition.clauses.size(); j++) {
			ExprPtr lower, upper;
			DirectClauseBounds(calc, raw.clauses[j], lower, upper);
			bounds.lower.push_back(p2.Add(calc.Out(lower)));
			bounds.upper.push_back(p2.Add(calc.Out(upper)));
		}
		for (idx_t d = 0; d < condition.decisions.size(); d++) {
			// The default floor of 0 is lowered on the rows a clause with a lower bound under a `when` keeps.
			vector<ColumnBinding> lowering_whens;
			for (idx_t j = 0; j < condition.clauses.size(); j++) {
				if (condition.clauses[j].decision == d && condition.clauses[j].lifts_floor) {
					D_ASSERT(raw.clauses[j].has_when);
					lowering_whens.push_back(raw.clauses[j].when);
				}
			}
			bounds.floor.push_back(
			    p2.Add(calc.Out(DirectStaticFloor(calc, condition.decisions[d].lower, lowering_whens))));
		}
		double sense = condition.sense == DecideSense::MAXIMIZE ? -1.0 : 1.0;
		for (idx_t d = 0; d < condition.decisions.size(); d++) {
			auto sum = DirectFoldQuadratic(optimizer, calc, condition.parts, raw.parts, d);
			// The linear coefficient the parts' like terms merge into, `b + b` for SUM(b * x) + SUM(b * x), as the
			// solver path reads it. P1 checked each value, so a total past a DOUBLE names the objective.
			if (sum.linear) {
				checks.push_back(DirectRaiseTooLarge(optimizer, sum.linear, condition.objective_written));
			}
			bounds.a.push_back(p2.Add(calc.Out(calc.Mul(calc.Num(sense), sum.a))));
			bounds.b.push_back(p2.Add(calc.Out(calc.Mul(calc.Num(sense), sum.b))));
		}
		// Every check is true or raises; a row passes when all of them do.
		bounds.valid_data = p2.Add(DirectAllHold(calc, std::move(checks)));
	}

	//===------------------------------------------------------------------===//
	// P3: each decision's box
	//===------------------------------------------------------------------===//

	void ComputeBox(DirectProjectionStage &p3, const Bounds &bounds, Box &box) const {
		for (idx_t d = 0; d < condition.decisions.size(); d++) {
			vector<ColumnBinding> clause_lowers, clause_uppers;
			for (idx_t j = 0; j < condition.clauses.size(); j++) {
				if (condition.clauses[j].decision == d) {
					clause_lowers.push_back(bounds.lower[j]);
					clause_uppers.push_back(bounds.upper[j]);
				}
			}
			ExprPtr lower, upper;
			DirectRowBox(calc, calc.Col(LogicalType::DOUBLE, bounds.floor[d]), condition.decisions[d].upper,
			             clause_lowers, clause_uppers, lower, upper);
			box.lower.push_back(p3.Add(calc.Out(lower)));
			box.upper.push_back(p3.Add(calc.Out(upper)));
			box.a.push_back(p3.Add(calc.Out(calc.Col(LogicalType::DOUBLE, bounds.a[d]))));
			box.b.push_back(p3.Add(calc.Out(calc.Col(LogicalType::DOUBLE, bounds.b[d]))));
		}
		box.valid_data = p3.Add(calc.Out(calc.Col(LogicalType::BOOLEAN, bounds.valid_data)));
	}

	//===------------------------------------------------------------------===//
	// P4: each decision's answer
	//===------------------------------------------------------------------===//

	void ComputeAnswers(DirectProjectionStage &p4, const Box &box, Answers &answers) const {
		for (idx_t d = 0; d < condition.decisions.size(); d++) {
			auto lower = calc.Col(LogicalType::DOUBLE, box.lower[d]);
			auto upper = calc.Col(LogicalType::DOUBLE, box.upper[d]);
			auto a = calc.Col(LogicalType::DOUBLE, box.a[d]);
			auto b = calc.Col(LogicalType::DOUBLE, box.b[d]);
			auto zero = calc.Num(0.0);
			bool whole = DirectIsWholeNumbered(condition.decisions[d].domain);

			// No row holds when the box is empty; the answer is then never read.
			auto infeasible = DirectBoxIsEmpty(calc, lower, upper);

			// Convex: the turning point, clamped into the box (for whole numbers, the first integer whose next step
			// no longer improves, which keeps the smaller one on a tie).
			auto turning = DirectTurningPoint(calc, a, b, whole);
			auto convex = calc.Min({calc.Max({turning.target, lower}), upper});
			// Concave: the better endpoint, from the sign of f(upper) - f(lower) = (upper - lower) * s.
			auto s = calc.Add(calc.Mul(a, calc.Add(upper, lower)), b);
			auto concave = calc.If(calc.Cmp(ExpressionType::COMPARE_LESSTHAN, s, zero), upper, lower);
			// Linear: the end the slope points away from. With no slope at all the lowest value allowed, or zero
			// clamped into the box when the box is open below.
			auto flat = calc.If(calc.Fn("isfinite", lower), lower, calc.Min({calc.Max({zero, lower}), upper}));
			auto linear = calc.If(calc.Cmp(ExpressionType::COMPARE_LESSTHAN, b, zero), upper,
			                      calc.If(calc.Cmp(ExpressionType::COMPARE_GREATERTHAN, b, zero), lower, flat));
			auto chosen = calc.If(calc.Cmp(ExpressionType::COMPARE_GREATERTHAN, a, zero), convex,
			                      calc.If(calc.Cmp(ExpressionType::COMPARE_LESSTHAN, a, zero), concave, linear));
			// An end that is not finite means the objective improves without limit. Adding 0.0 turns -0.0 into 0.0.
			auto value = calc.Add(chosen, zero);

			answers.infeasible.push_back(p4.Add(calc.Out(infeasible)));
			answers.a.push_back(p4.Add(calc.Out(calc.Col(LogicalType::DOUBLE, box.a[d]))));
			answers.b.push_back(p4.Add(calc.Out(calc.Col(LogicalType::DOUBLE, box.b[d]))));
			answers.value.push_back(p4.Add(calc.Out(value)));
			answers.loses_precision.push_back(p4.Add(calc.Out(turning.loses_precision)));
		}
		answers.valid_data = p4.Add(calc.Out(calc.Col(LogicalType::BOOLEAN, box.valid_data)));
	}

	//! The answer rounded as the solver rounds it (`round`, half away from zero).
	unique_ptr<Expression> RoundedAnswer(ColumnBinding value) const {
		return calc.Out(calc.Fn("round", calc.Col(LogicalType::DOUBLE, value)));
	}

	//! `abs(round(value)) <= 2^53`, the SQL spelling of `DecideRoundedAnswerWithin(value, -2^53, 2^53)`, the test the
	//! solver path applies to an answer on its way into a BIGINT column. Callers test finiteness first, so an infinite
	//! or NaN value never reaches it.
	unique_ptr<Expression> WithinExactRange(ColumnBinding value) const {
		return calc.Out(calc.Cmp(ExpressionType::COMPARE_LESSTHANOREQUALTO,
		                         calc.Fn("abs", calc.Wrap(RoundedAnswer(value))), calc.Num(DECIDE_MAX_EXACT_INTEGER)));
	}

	//! The solver's error for an answer beyond the exact range, character for character: the name is filled in now and
	//! the value (`%.0f` of the rounded answer, as the solver prints it) at run time.
	unique_ptr<Expression> OutOfRangeError(const string &name, ColumnBinding value) const {
		vector<ExprPtr> parts {calc.Constant(Value(DecideWholeNumberRangeMessageBefore(name))),
		                       calc.Fn("printf", calc.Constant(Value("%.0f")), calc.Wrap(RoundedAnswer(value))),
		                       calc.Constant(Value(DecideWholeNumberRangeMessageAfter(name)))};
		return DirectErrorPredicate(optimizer, calc.Out(calc.Fn("concat", parts)));
	}

	//! The condition every row must meet. It is true, or the row raises the error that applies: a coefficient the
	//! arithmetic made non-finite, an empty box, an improvement without limit, or a whole number the answer cannot hold
	//! exactly. Every raw value was checked in P2 and arrives as `valid_data`. A row that passes them all may warn
	//! (DirectTurningPoint).
	unique_ptr<Expression> ValidAnswers(const Answers &answers) const {
		vector<unique_ptr<Expression>> checks;
		// The raw values come first: P2 folded their checks into one flag that is true or false, never an error.
		checks.push_back(calc.Out(calc.Col(LogicalType::BOOLEAN, answers.valid_data)));
		for (idx_t d = 0; d < condition.decisions.size(); d++) {
			checks.push_back(DirectRaiseTooLarge(optimizer, calc.Col(LogicalType::DOUBLE, answers.a[d]),
			                                     condition.objective_written));
			checks.push_back(DirectRaiseTooLarge(optimizer, calc.Col(LogicalType::DOUBLE, answers.b[d]),
			                                     condition.objective_written));
			checks.push_back(DirectRaiseIf(calc, calc.Out(calc.Col(LogicalType::BOOLEAN, answers.infeasible[d])),
			                               DirectInfeasibleErrorPredicate(optimizer, errors)));
			checks.push_back(
			    DirectRaiseUnless(calc, calc.Out(calc.Fn("isfinite", calc.Col(LogicalType::DOUBLE, answers.value[d]))),
			                      DirectUnboundedErrorPredicate(optimizer, errors)));
			if (DirectIsWholeNumbered(condition.decisions[d].domain)) {
				checks.push_back(DirectRaiseUnless(calc, WithinExactRange(answers.value[d]),
				                                   OutOfRangeError(condition.decisions[d].name, answers.value[d])));
			}
		}
		vector<ExprPtr> imprecise;
		for (auto &binding : answers.loses_precision) {
			imprecise.push_back(calc.Col(LogicalType::BOOLEAN, binding));
		}
		checks.push_back(DirectWarnIf(calc, calc.Out(calc.Any(imprecise))));
		return DirectAllHold(calc, std::move(checks));
	}

	//! The answer in the decision's own output type. The verdict has already raised for anything that is not finite or
	//! not exactly representable, so the guard only keeps the cast from raising on its own if that ever changed.
	unique_ptr<Expression> TypedAnswer(idx_t decision, ColumnBinding value) const {
		auto &prepared = condition.decisions[decision];
		if (!DirectIsWholeNumbered(prepared.domain)) {
			return calc.Out(calc.Col(LogicalType::DOUBLE, value));
		}
		auto column = calc.Col(LogicalType::DOUBLE, value);
		auto finite = calc.If(calc.Fn("isfinite", column), calc.Wrap(WithinExactRange(value)), calc.Bool(false));
		return calc.Out(
		    calc.If(finite, calc.Cast(column, prepared.output_type), calc.Constant(Value(prepared.output_type))));
	}
};

DirectSubstitute Rule::Apply(DirectSourceReader &reader, Optimizer &optimizer,
                             const DirectApplicabilityCondition &candidate, const DirectStateErrors &errors) const {
	auto &condition = static_cast<const Condition &>(candidate);
	PlanBuilder plan_builder(condition, optimizer, reader.SourceBindings(), reader.SourceTypes(), errors);
	if (!condition.constant_problem) {
		// Verify, then stream: the verdict is read first, and every read after it waits for it.
		reader.Gate(DirectBuildOverRead(reader, reader.ReadSource(), [&](unique_ptr<LogicalOperator> rows) {
			return plan_builder.BuildVerdict(std::move(rows));
		}));
	}
	return DirectBuildOverOneRead(
	    reader, [&](unique_ptr<LogicalOperator> rows) { return plan_builder.Build(std::move(rows)); });
}

} // namespace direct_a1_separable_box

unique_ptr<DirectSolveRule> MakeA1SeparableBoxRule() {
	return make_uniq<direct_a1_separable_box::Rule>();
}

} // namespace duckdb
