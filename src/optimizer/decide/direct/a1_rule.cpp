#include "duckdb/optimizer/decide/direct/a1_rule.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

#include "duckdb/common/decide_numeric_policy.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/optimizer/decide/direct/direct_builder.hpp"
#include "duckdb/optimizer/decide/direct/direct_expression.hpp"
#include "duckdb/optimizer/decide/direct/direct_solve.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"

namespace duckdb {

//! Each rule keeps its helpers in its own named namespace: unity builds compile every rule file in one translation
//! unit, where two anonymous namespaces would merge and same-named helpers would collide.
namespace direct_a1 {

constexpr const char *A1_RULE = "A1_INDEPENDENT_BOUNDS";
constexpr double POSITIVE_INFINITY = std::numeric_limits<double>::infinity();
constexpr double NEGATIVE_INFINITY = -std::numeric_limits<double>::infinity();

struct A1Term {
	enum class Kind : uint8_t { LINEAR, CONSTANT, SQUARE };
	Kind kind = Kind::LINEAR;
	idx_t decision = 0;
	int sign = 1;
	//! LINEAR: the coefficient of the decision. CONSTANT: the value.
	unique_ptr<Expression> value;
	//! SQUARE: `square_scale * (inner)^2`, where the inner expression is linear in one decision.
	double square_scale = 1.0;
	vector<DirectSignedValue> inner_linear;
	vector<DirectSignedValue> inner_constant;
};

struct A1Part {
	int sign = 1;
	bool scaled = false;
	double scale = 1.0;
	bool divides = false;
	unique_ptr<Expression> filter;
	vector<A1Term> terms;
};

struct A1Decision {
	//! The declared name, which the whole-number range error quotes.
	string name;
	DirectDomain domain = DirectDomain::REAL;
	LogicalType output_type;
	//! The decision's box before any per-row bound: its default, replaced or tightened by the bounds the solver folds
	//! into the variable.
	double lower = 0.0;
	double upper = POSITIVE_INFINITY;
};

struct A1Condition final : DirectApplicabilityCondition {
	idx_t decide_index = 0;
	DecideSense sense = DecideSense::FEASIBILITY;
	vector<A1Decision> decisions;
	vector<DirectBoundClause> clauses;
	unique_ptr<Expression> objective_when;
	vector<A1Part> parts;
	bool has_square = false;
	//! Every value is a constant and nothing is conditional, so a check that fails on one row fails on all of them.
	bool constant_problem = false;
};

//! No value reads a source column and nothing is conditional: every row sees the same bounds and coefficients.
bool IsConstantProblem(const A1Condition &condition) {
	auto constant = [](const Expression &expr) {
		return expr.IsFoldable();
	};
	auto constant_values = [&](const vector<DirectSignedValue> &values) {
		for (auto &value : values) {
			if (!constant(*value.expr)) {
				return false;
			}
		}
		return true;
	};
	if (condition.objective_when) {
		return false;
	}
	for (auto &clause : condition.clauses) {
		if (clause.when || !constant(*clause.rhs) || !constant_values(clause.coefficient)) {
			return false;
		}
	}
	for (auto &part : condition.parts) {
		if (part.filter) {
			return false;
		}
		for (auto &term : part.terms) {
			if (term.kind == A1Term::Kind::SQUARE) {
				if (!constant_values(term.inner_linear) || !constant_values(term.inner_constant)) {
					return false;
				}
			} else if (!constant(*term.value)) {
				return false;
			}
		}
	}
	return true;
}

//===--------------------------------------------------------------------===//
// Rule
//===--------------------------------------------------------------------===//

class A1BoundedRule final : public DirectSolveRule {
public:
	const char *Name() const override {
		return A1_RULE;
	}

	unique_ptr<DirectApplicabilityCondition> CheckCondition(const DirectIR &ir, ClientContext &context,
	                                                        string &reason) const override {
		if (ir.decisions_status != DirectAnalysisStatus::PROVED || ir.decisions.empty() || !ir.entity_scopes.empty() ||
		    ir.source_status != DirectAnalysisStatus::PROVED) {
			reason = "variable_shape: expected row-scoped BOOL, INT or REAL decisions";
			return nullptr;
		}
		for (auto &decision : ir.decisions) {
			if (decision.scope != DecideVarScope::ROW) {
				reason = "variable_shape: expected row-scoped BOOL, INT or REAL decisions";
				return nullptr;
			}
		}
		if (!std::isfinite(ir.objective.offset) ||
		    (ir.objective.sense != DecideSense::MAXIMIZE && ir.objective.sense != DecideSense::MINIMIZE &&
		     ir.objective.sense != DecideSense::FEASIBILITY)) {
			reason = "problem_shape: expected a finite objective constant and MINIMIZE, MAXIMIZE or no objective";
			return nullptr;
		}
		if (ir.constraints_status != DirectAnalysisStatus::PROVED) {
			reason = "constraints_not_proved: " + ir.constraints_reason;
			return nullptr;
		}
		for (auto &ir_node : ir.constraints) {
			if (ir_node.status != DirectAnalysisStatus::PROVED) {
				reason = "constraints_not_proved: " + ir.FirstNotProvedReason();
				return nullptr;
			}
			if (ir_node.aggregate) {
				reason = "constraint_shape: expected a bound on one decision, found an aggregate constraint";
				return nullptr;
			}
			if (ir_node.comparison == ExpressionType::COMPARE_IN ||
			    ir_node.comparison == ExpressionType::COMPARE_NOTEQUAL) {
				reason = "constraint_shape: expected an interval bound, found <> or IN";
				return nullptr;
			}
			if (ir_node.comparison != ExpressionType::COMPARE_EQUAL && !DirectIsLowerBound(ir_node.comparison) &&
			    !DirectIsUpperBound(ir_node.comparison)) {
				reason = "constraint_shape: expected an interval bound";
				return nullptr;
			}
			if (!ir_node.scope.per_key_slots.empty()) {
				reason = "constraint_scope: PER does not apply to a bound on one row";
				return nullptr;
			}
			if (!ReadsOneDecision(ir_node, ir.decisions.size())) {
				reason = "constraint_shape: expected a bound of the form a*x <comparison> value on one decision";
				return nullptr;
			}
		}
		if (ir.objective.status != DirectAnalysisStatus::PROVED) {
			reason = "objective_not_proved: " + ir.objective.reason;
			return nullptr;
		}
		if (!ir.objective.scope.per_key_slots.empty()) {
			reason = "objective_scope: expected an objective without PER";
			return nullptr;
		}
		for (auto &part : ir.objective.parts) {
			if (part.reducer != DirectReducer::SUM || part.inner || part.qualifier != DConstants::INVALID_INDEX) {
				reason = "objective_shape: expected SUM(...) terms";
				return nullptr;
			}
			for (auto &term : part.terms) {
				if (term.kind != DecideTermKind::LINEAR && term.kind != DecideTermKind::CONSTANT &&
				    term.kind != DecideTermKind::SQUARE) {
					reason = "objective_shape: expected linear, constant or squared terms in independent decisions";
					return nullptr;
				}
			}
		}
		auto condition = make_uniq<A1Condition>();
		condition->decide_index = ir.decide_index;
		condition->sense = ir.objective.sense;
		for (auto &decision : ir.decisions) {
			A1Decision prepared;
			prepared.name = decision.name;
			prepared.domain = decision.domain;
			prepared.output_type = decision.output_type;
			condition->decisions.push_back(std::move(prepared));
		}
		if (!ProveBounds(ir, context, *condition, reason) || !ProveObjective(ir, context, *condition, reason)) {
			return nullptr;
		}
		condition->constant_problem = IsConstantProblem(*condition);
		return std::move(condition);
	}

	string Summary() const override {
		return "min/max SUM(c*x), l <= x <= u, each row independent";
	}

	DirectSubstitute Apply(unique_ptr<LogicalOperator> source, Optimizer &optimizer,
	                       const DirectApplicabilityCondition &candidate) const override;

private:
	//! One unfiltered per-row left side: only linear terms, in one decision, and no constant.
	static bool ReadsOneDecision(const DirectIRConstraint &ir_node, idx_t decision_count) {
		if (ir_node.lhs.size() != 1 || ir_node.degree != 1) {
			return false;
		}
		auto &part = ir_node.lhs[0];
		if (part.sign != 1 || part.reducer != DirectReducer::NONE || part.scale || part.filter || part.inner ||
		    part.terms.empty()) {
			return false;
		}
		idx_t decision = part.terms[0].var_a;
		for (auto &term : part.terms) {
			if (term.kind != DecideTermKind::LINEAR || term.var_a != decision || !term.coefficient) {
				return false;
			}
		}
		return decision < decision_count;
	}

	bool ProveBounds(const DirectIR &ir, ClientContext &context, A1Condition &condition, string &reason) const;
	bool ProveObjective(const DirectIR &ir, ClientContext &context, A1Condition &condition, string &reason) const;
};

//! The decision's box before any per-row bound, and the clauses the plan evaluates per row, from the shared reader.
bool A1BoundedRule::ProveBounds(const DirectIR &ir, ClientContext &context, A1Condition &condition,
                                string &reason) const {
	DirectBoundReading reading;
	if (!DirectReadBounds(ir, context, reading, reason)) {
		return false;
	}
	condition.clauses = std::move(reading.clauses);
	for (idx_t decision = 0; decision < condition.decisions.size(); decision++) {
		condition.decisions[decision].lower = reading.boxes[decision].lower;
		condition.decisions[decision].upper = reading.boxes[decision].upper;
	}
	return true;
}

bool A1BoundedRule::ProveObjective(const DirectIR &ir, ClientContext &context, A1Condition &condition,
                                   string &reason) const {
	auto &objective = ir.objective;
	if (objective.scope.when) {
		if (!DirectIsSourceOnlyPredicate(*objective.scope.when, ir.decide_index, ir.source_bindings)) {
			reason = "objective_scope: WHEN must be a deterministic source-only predicate";
			return false;
		}
		condition.objective_when = objective.scope.when->Copy();
	}
	idx_t decision_count = ir.decisions.size();
	for (auto &part : objective.parts) {
		A1Part prepared;
		prepared.sign = part.sign;
		prepared.scaled = part.scale != nullptr;
		if (!DirectPartScale(context, part, prepared.scale)) {
			reason = "objective_shape: a factor on SUM(...) must be a finite constant (not zero when dividing)";
			return false;
		}
		prepared.divides = part.scale_divides;
		if (part.filter) {
			if (!DirectIsSourceOnlyPredicate(*part.filter, ir.decide_index, ir.source_bindings)) {
				reason = "objective_scope: WHEN must be a deterministic source-only predicate";
				return false;
			}
			prepared.filter = part.filter->Copy();
		}
		for (auto &term : part.terms) {
			A1Term built;
			built.sign = term.sign;
			if (term.kind == DecideTermKind::LINEAR || term.kind == DecideTermKind::CONSTANT) {
				built.kind = term.kind == DecideTermKind::LINEAR ? A1Term::Kind::LINEAR : A1Term::Kind::CONSTANT;
				if (term.kind == DecideTermKind::LINEAR) {
					if (term.var_a >= decision_count) {
						reason = "objective_shape: expected a coefficient times one decision";
						return false;
					}
					built.decision = term.var_a;
				}
				if (!term.coefficient || !DirectProveValue(ir, *term.coefficient)) {
					reason = "coefficient_shape: expected a deterministic numeric expression over the source columns";
					return false;
				}
				built.value = term.coefficient->Copy();
			} else {
				built.kind = A1Term::Kind::SQUARE;
				built.square_scale = term.square_scale;
				if (!std::isfinite(term.square_scale) || term.square_scale == 0.0) {
					reason = "objective_shape: the factor on a squared term must be a finite nonzero constant";
					return false;
				}
				if (condition.has_square) {
					reason = "objective_shape: expected at most one squared term, as the solver accepts";
					return false;
				}
				condition.has_square = true;
				bool has_decision = false;
				for (auto &inner : term.inner) {
					if (inner.kind == DecideTermKind::LINEAR) {
						if (inner.var_a >= decision_count || (has_decision && built.decision != inner.var_a)) {
							reason = "objective_shape: a squared term must hold one decision";
							return false;
						}
						built.decision = inner.var_a;
						has_decision = true;
					} else if (inner.kind != DecideTermKind::CONSTANT) {
						reason = "objective_shape: a squared term must be linear in one decision";
						return false;
					}
				}
				if (!has_decision) {
					reason = "objective_shape: a squared term must be linear in one decision";
					return false;
				}
				for (auto &inner : term.inner) {
					if (!inner.coefficient || !DirectProveValue(ir, *inner.coefficient)) {
						reason = "coefficient_shape: expected a deterministic numeric expression over the source "
						         "columns";
						return false;
					}
					DirectSignedValue value;
					value.sign = inner.sign;
					value.expr = inner.coefficient->Copy();
					(inner.kind == DecideTermKind::LINEAR ? built.inner_linear : built.inner_constant)
					    .push_back(std::move(value));
				}
			}
			prepared.terms.push_back(std::move(built));
		}
		condition.parts.push_back(std::move(prepared));
	}
	return true;
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

//! Builds the relational plan for one proved A1 problem, stage by stage. Each stage is one projection that reads only
//! the columns of the stage before it:
//!
//!   P1 raw values (every coefficient, bound and mask, evaluated unconditionally on every row)
//!   P2 per-clause bounds, per-decision objective coefficients, and the check on every raw value
//!   P3 each decision's box
//!   P4 each decision's answer and whether its row is infeasible
//!   breaker: every row is validated before one is released
//!   P5 the source columns, the typed answers, and the breaker
class A1PlanBuilder {
public:
	A1PlanBuilder(const A1Condition &condition_p, Optimizer &optimizer_p, LogicalOperator &source)
	    : condition(condition_p), optimizer(optimizer_p), binder(optimizer_p.binder), calc(optimizer_p) {
		// The rule driver hands the input over with its types resolved.
		source_bindings = source.GetColumnBindings();
		source_types = source.types;
		if (source_bindings.size() != source_types.size()) {
			throw InternalException("Direct solve source bindings and types differ");
		}
	}

	DirectSubstitute Build(unique_ptr<LogicalOperator> source) const {
		Stage p1(binder.GenerateTableIndex());
		vector<unique_ptr<Expression>> checks;
		Raw raw;
		LayoutRawValues(p1, checks, raw);
		unique_ptr<LogicalOperator> plan = p1.Finish(std::move(source));
		for (auto &mask : raw.guarded_masks) {
			plan = DirectGuardEmptyAggregate(optimizer, std::move(plan), mask, "objective");
		}

		Stage p2(binder.GenerateTableIndex());
		Bounds bounds;
		auto passthrough2 = Passthrough(p2, raw.source);
		ComputeBounds(p2, raw, checks, bounds);
		plan = p2.Finish(std::move(plan));

		Stage p3(binder.GenerateTableIndex());
		auto passthrough3 = Passthrough(p3, passthrough2);
		Box box;
		ComputeBox(p3, bounds, box);
		plan = p3.Finish(std::move(plan));

		Stage p4(binder.GenerateTableIndex());
		auto passthrough4 = Passthrough(p4, passthrough3);
		Answers answers;
		ComputeAnswers(p4, box, answers);
		plan = p4.Finish(std::move(plan));

		DirectSubstitute substitute;
		for (idx_t d = 0; d < condition.decisions.size(); d++) {
			substitute.prunable_decisions.push_back(1);
		}
		for (idx_t i = 0; i < source_bindings.size() + condition.decisions.size(); i++) {
			substitute.slot_map.push_back(i);
		}
		substitute.validated_columns.push_back(source_bindings.size() + condition.decisions.size());

		Stage p5(binder.GenerateTableIndex());
		if (condition.constant_problem) {
			// Every row sees the same values, so the first row raises whatever the others would, and nothing has to be
			// held back: the plan streams, and the validation column raises wherever a row is read. An empty source
			// has no row to raise on and returns no rows. A bound the solver rejects outright (against the variable's
			// built-in domain) is never proved, so it cannot differ on an empty input.
			for (idx_t i = 0; i < source_bindings.size(); i++) {
				p5.Add(DirectColumn(source_types[i], passthrough4[i]));
			}
			for (idx_t d = 0; d < condition.decisions.size(); d++) {
				p5.Add(TypedAnswer(d, answers.value[d]));
			}
			p5.Add(ValidAnswers(answers));
			substitute.child = p5.Finish(std::move(plan));
			substitute.pipelined = true;
			return substitute;
		}
		auto breaker = DirectValidationBreaker(binder, std::move(plan), ValidAnswers(answers));
		for (idx_t i = 0; i < source_bindings.size(); i++) {
			p5.Add(DirectColumn(source_types[i], breaker.input_bindings[i]));
		}
		for (idx_t d = 0; d < condition.decisions.size(); d++) {
			p5.Add(TypedAnswer(d, breaker.input_bindings[answers.value[d].column_index]));
		}
		p5.Add(DirectColumn(LogicalType::BIGINT, breaker.breaker));
		substitute.child = p5.Finish(std::move(breaker.plan));
		return substitute;
	}

private:
	//! Columns of P1.
	struct Raw {
		vector<ColumnBinding> source;
		vector<DirectBoundColumns> clauses;
		struct SquareColumns {
			idx_t decision = 0;
			double scale = 1.0;
			ColumnBinding linear;
			ColumnBinding constant;
		};
		struct PartColumns {
			bool has_mask = false;
			ColumnBinding mask;
			//! The sum of the linear coefficients of each decision, with their signs.
			vector<idx_t> linear_decision;
			vector<ColumnBinding> linear;
			vector<SquareColumns> squares;
		};
		vector<PartColumns> parts;
		//! The distinct masks that need DECIDE's empty-aggregate guard.
		vector<ColumnBinding> guarded_masks;
	};

	//! Columns of P2: each clause's bounds on its decision, and each decision's objective coefficients.
	struct Bounds {
		vector<ColumnBinding> lower;
		vector<ColumnBinding> upper;
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
		ColumnBinding valid_data;
	};

	const A1Condition &condition;
	Optimizer &optimizer;
	Binder &binder;
	Calc calc;
	vector<ColumnBinding> source_bindings;
	vector<LogicalType> source_types;

	vector<ColumnBinding> Passthrough(Stage &stage, const vector<ColumnBinding> &columns) const {
		vector<ColumnBinding> result;
		for (idx_t i = 0; i < columns.size(); i++) {
			result.push_back(stage.Add(DirectColumn(source_types[i], columns[i])));
		}
		return result;
	}

	unique_ptr<Expression> AsDouble(unique_ptr<Expression> value) const {
		return BoundCastExpression::AddCastToType(optimizer.context, std::move(value), LogicalType::DOUBLE);
	}

	//! `sum(sign * value)` over the proved values, read as DOUBLE. Null for none.
	unique_ptr<Expression> SumOf(const vector<DirectSignedValue> &values) const {
		unique_ptr<Expression> total;
		for (auto &value : values) {
			auto term = DirectScaledTerm(optimizer, value.expr->Copy(), value.sign, false, 1.0, false);
			total = total ? optimizer.BindScalarFunction("+", std::move(total), std::move(term)) : std::move(term);
		}
		return total;
	}

	//===------------------------------------------------------------------===//
	// P1: raw values
	//===------------------------------------------------------------------===//

	void LayoutRawValues(Stage &p1, vector<unique_ptr<Expression>> &checks, Raw &raw) const {
		for (idx_t i = 0; i < source_bindings.size(); i++) {
			raw.source.push_back(p1.Add(DirectColumn(source_types[i], source_bindings[i])));
		}
		for (auto &clause : condition.clauses) {
			DirectBoundColumns columns;
			columns.coefficient = p1.Add(SumOf(clause.coefficient));
			columns.rhs = p1.Add(AsDouble(clause.rhs->Copy()));
			if (clause.when) {
				columns.has_when = true;
				columns.when = p1.Add(DirectActiveWhen(clause.when.get()));
			}
			auto coefficient_source =
			    DirectMergedOverProjection(optimizer.context, clause.coefficient, source_bindings, p1.index);
			checks.push_back(DirectValidNumberPredicate(optimizer, LogicalType::DOUBLE, columns.coefficient,
			                                            coefficient_source.get(), DirectNumberKind::CONSTRAINT));
			checks.push_back(DirectValidBoundPredicate(optimizer, columns.rhs,
			                                           *DirectOverProjection(*clause.rhs, source_bindings, p1.index)));
			raw.clauses.push_back(columns);
		}
		bool has_shared_mask = false;
		ColumnBinding shared_mask;
		for (auto &part : condition.parts) {
			Raw::PartColumns columns;
			if (condition.objective_when || part.filter) {
				columns.has_mask = true;
				if (!part.filter && has_shared_mask) {
					columns.mask = shared_mask;
				} else {
					auto base = DirectActiveWhen(condition.objective_when.get());
					if (part.filter) {
						base = make_uniq<BoundCaseExpression>(std::move(base), DirectActiveWhen(part.filter.get()),
						                                      DirectConstantBool(false));
					}
					columns.mask = p1.Add(std::move(base));
					raw.guarded_masks.push_back(columns.mask);
					if (!part.filter) {
						has_shared_mask = true;
						shared_mask = columns.mask;
					}
				}
			}
			AddObjectiveValues(p1, checks, part, columns);
			raw.parts.push_back(std::move(columns));
		}
	}

	void AddObjectiveValues(Stage &p1, vector<unique_ptr<Expression>> &checks, const A1Part &part,
	                        Raw::PartColumns &columns) const {
		// Linear coefficients add up per decision; a constant adds nothing to the answer but must still be valid.
		vector<unique_ptr<Expression>> linear_sums(condition.decisions.size());
		// The values each sum is made of, so a NULL in P2's check names the column it came from.
		vector<vector<DirectSignedValue>> linear_terms(condition.decisions.size());
		unique_ptr<Expression> constant_sum;
		vector<DirectSignedValue> constant_terms;
		for (auto &term : part.terms) {
			if (term.kind == A1Term::Kind::LINEAR) {
				auto value = DirectScaledTerm(optimizer, term.value->Copy(), term.sign, false, 1.0, false);
				auto &sum = linear_sums[term.decision];
				sum = sum ? optimizer.BindScalarFunction("+", std::move(sum), std::move(value)) : std::move(value);
				linear_terms[term.decision].push_back({term.sign, term.value->Copy()});
			} else if (term.kind == A1Term::Kind::CONSTANT) {
				auto value = DirectScaledTerm(optimizer, term.value->Copy(), term.sign, false, 1.0, false);
				constant_sum = constant_sum
				                   ? optimizer.BindScalarFunction("+", std::move(constant_sum), std::move(value))
				                   : std::move(value);
				constant_terms.push_back({term.sign, term.value->Copy()});
			} else {
				Raw::SquareColumns square;
				square.decision = term.decision;
				square.scale = term.sign * term.square_scale;
				square.linear = p1.Add(SumOf(term.inner_linear));
				auto linear_source =
				    DirectMergedOverProjection(optimizer.context, term.inner_linear, source_bindings, p1.index);
				checks.push_back(DirectValidNumberPredicate(optimizer, LogicalType::DOUBLE, square.linear,
				                                            linear_source.get(), DirectNumberKind::OBJECTIVE));
				if (!term.inner_constant.empty()) {
					square.constant = p1.Add(SumOf(term.inner_constant));
					auto constant_source =
					    DirectMergedOverProjection(optimizer.context, term.inner_constant, source_bindings, p1.index);
					checks.push_back(DirectValidNumberPredicate(optimizer, LogicalType::DOUBLE, square.constant,
					                                            constant_source.get(), DirectNumberKind::OBJECTIVE));
				} else {
					square.constant = p1.Add(make_uniq<BoundConstantExpression>(Value::DOUBLE(0.0)));
				}
				columns.squares.push_back(square);
			}
		}
		for (idx_t d = 0; d < condition.decisions.size(); d++) {
			if (!linear_sums[d]) {
				continue;
			}
			auto binding = p1.Add(std::move(linear_sums[d]));
			auto linear_source =
			    DirectMergedOverProjection(optimizer.context, linear_terms[d], source_bindings, p1.index);
			checks.push_back(DirectValidNumberPredicate(optimizer, LogicalType::DOUBLE, binding, linear_source.get(),
			                                            DirectNumberKind::OBJECTIVE));
			columns.linear_decision.push_back(d);
			columns.linear.push_back(binding);
		}
		if (constant_sum) {
			auto binding = p1.Add(std::move(constant_sum));
			auto constant_source =
			    DirectMergedOverProjection(optimizer.context, constant_terms, source_bindings, p1.index);
			checks.push_back(DirectValidNumberPredicate(optimizer, LogicalType::DOUBLE, binding, constant_source.get(),
			                                            DirectNumberKind::OBJECTIVE));
		}
	}

	//===------------------------------------------------------------------===//
	// P2: per-clause bounds, objective coefficients, raw-value checks
	//===------------------------------------------------------------------===//

	void ComputeBounds(Stage &p2, const Raw &raw, vector<unique_ptr<Expression>> &checks, Bounds &bounds) const {
		for (idx_t j = 0; j < condition.clauses.size(); j++) {
			Ex lower, upper;
			DirectClauseBounds(calc, condition.clauses[j].comparison,
			                   DirectIsWholeNumbered(condition.decisions[condition.clauses[j].decision].domain),
			                   raw.clauses[j], lower, upper);
			bounds.lower.push_back(p2.Add(calc.Out(lower)));
			bounds.upper.push_back(p2.Add(calc.Out(upper)));
		}
		double sense = condition.sense == DecideSense::MAXIMIZE ? -1.0 : 1.0;
		for (idx_t d = 0; d < condition.decisions.size(); d++) {
			Ex a_total = calc.Num(0.0);
			Ex b_total = calc.Num(0.0);
			for (idx_t p = 0; p < condition.parts.size(); p++) {
				auto &part = condition.parts[p];
				auto &columns = raw.parts[p];
				Ex a_part = calc.Num(0.0);
				Ex b_part = calc.Num(0.0);
				bool touched = false;
				for (idx_t i = 0; i < columns.linear.size(); i++) {
					if (columns.linear_decision[i] == d) {
						b_part = calc.Add(b_part, calc.Col(LogicalType::DOUBLE, columns.linear[i]));
						touched = true;
					}
				}
				for (auto &square : columns.squares) {
					if (square.decision != d) {
						continue;
					}
					auto k = calc.Num(square.scale);
					auto w = calc.Col(LogicalType::DOUBLE, square.linear);
					auto t = calc.Col(LogicalType::DOUBLE, square.constant);
					a_part = calc.Add(a_part, calc.Mul(k, calc.Mul(w, w)));
					b_part = calc.Add(b_part, calc.Mul(calc.Num(2.0), calc.Mul(k, calc.Mul(w, t))));
					touched = true;
				}
				if (!touched) {
					continue;
				}
				auto a_scaled = calc.Wrap(
				    DirectScaledTerm(optimizer, calc.Out(a_part), part.sign, part.scaled, part.scale, part.divides));
				auto b_scaled = calc.Wrap(
				    DirectScaledTerm(optimizer, calc.Out(b_part), part.sign, part.scaled, part.scale, part.divides));
				if (columns.has_mask) {
					auto mask = calc.Col(LogicalType::BOOLEAN, columns.mask);
					a_scaled = calc.If(mask, a_scaled, calc.Num(0.0));
					b_scaled = calc.If(mask, b_scaled, calc.Num(0.0));
				}
				a_total = calc.Add(a_total, a_scaled);
				b_total = calc.Add(b_total, b_scaled);
			}
			bounds.a.push_back(p2.Add(calc.Out(calc.Mul(calc.Num(sense), a_total))));
			bounds.b.push_back(p2.Add(calc.Out(calc.Mul(calc.Num(sense), b_total))));
		}
		// Every check is true or raises; a row passes when all of them do.
		unique_ptr<Expression> valid = DirectConstantBool(true);
		for (idx_t i = checks.size(); i > 0; i--) {
			valid =
			    make_uniq<BoundCaseExpression>(std::move(checks[i - 1]), std::move(valid), DirectConstantBool(false));
		}
		bounds.valid_data = p2.Add(std::move(valid));
	}

	//===------------------------------------------------------------------===//
	// P3: each decision's box
	//===------------------------------------------------------------------===//

	void ComputeBox(Stage &p3, const Bounds &bounds, Box &box) const {
		for (idx_t d = 0; d < condition.decisions.size(); d++) {
			vector<ColumnBinding> clause_lowers, clause_uppers;
			for (idx_t j = 0; j < condition.clauses.size(); j++) {
				if (condition.clauses[j].decision == d) {
					clause_lowers.push_back(bounds.lower[j]);
					clause_uppers.push_back(bounds.upper[j]);
				}
			}
			Ex lower, upper;
			DirectRowBox(calc, condition.decisions[d].lower, condition.decisions[d].upper, clause_lowers, clause_uppers,
			             lower, upper);
			box.lower.push_back(p3.Add(calc.Out(lower)));
			box.upper.push_back(p3.Add(calc.Out(upper)));
			box.a.push_back(p3.Add(DirectColumn(LogicalType::DOUBLE, bounds.a[d])));
			box.b.push_back(p3.Add(DirectColumn(LogicalType::DOUBLE, bounds.b[d])));
		}
		box.valid_data = p3.Add(DirectColumn(LogicalType::BOOLEAN, bounds.valid_data));
	}

	//===------------------------------------------------------------------===//
	// P4: each decision's answer
	//===------------------------------------------------------------------===//

	void ComputeAnswers(Stage &p4, const Box &box, Answers &answers) const {
		for (idx_t d = 0; d < condition.decisions.size(); d++) {
			auto lower = calc.Col(LogicalType::DOUBLE, box.lower[d]);
			auto upper = calc.Col(LogicalType::DOUBLE, box.upper[d]);
			auto a = calc.Col(LogicalType::DOUBLE, box.a[d]);
			auto b = calc.Col(LogicalType::DOUBLE, box.b[d]);
			auto zero = calc.Num(0.0);
			bool whole = DirectIsWholeNumbered(condition.decisions[d].domain);

			// No row holds when the box is empty; the answer is then never read.
			auto infeasible = calc.Any({calc.Cmp(ExpressionType::COMPARE_GREATERTHAN, lower, upper),
			                            calc.Cmp(ExpressionType::COMPARE_EQUAL, lower, calc.Num(POSITIVE_INFINITY)),
			                            calc.Cmp(ExpressionType::COMPARE_EQUAL, upper, calc.Num(NEGATIVE_INFINITY))});

			// Convex: the turning point, clamped into the box (for whole numbers, the first integer whose next step
			// no longer improves, which keeps the smaller one on a tie).
			Ex turning = whole ? calc.Fn("ceil", calc.Div(calc.Sub(calc.Neg(b), a), calc.Mul(calc.Num(2.0), a)))
			                   : calc.Div(calc.Neg(b), calc.Mul(calc.Num(2.0), a));
			auto convex = calc.Min({calc.Max({turning, lower}), upper});
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
			answers.a.push_back(p4.Add(DirectColumn(LogicalType::DOUBLE, box.a[d])));
			answers.b.push_back(p4.Add(DirectColumn(LogicalType::DOUBLE, box.b[d])));
			answers.value.push_back(p4.Add(calc.Out(value)));
		}
		answers.valid_data = p4.Add(DirectColumn(LogicalType::BOOLEAN, box.valid_data));
	}

	//! The answer rounded as the solver rounds it (`round`, half away from zero).
	unique_ptr<Expression> RoundedAnswer(ColumnBinding value) const {
		return optimizer.BindScalarFunction("round", DirectColumn(LogicalType::DOUBLE, value));
	}

	//! `abs(round(value)) <= 2^53`, the SQL spelling of `DecideRoundedAnswerWithin(value, -2^53, 2^53)`, the test the
	//! solver path applies to an answer on its way into a BIGINT column. Callers test finiteness first, so an infinite
	//! or NaN value never reaches it.
	unique_ptr<Expression> WithinExactRange(ColumnBinding value) const {
		return make_uniq<BoundComparisonExpression>(
		    ExpressionType::COMPARE_LESSTHANOREQUALTO, optimizer.BindScalarFunction("abs", RoundedAnswer(value)),
		    make_uniq<BoundConstantExpression>(Value::DOUBLE(DECIDE_MAX_EXACT_INTEGER)));
	}

	//! The solver's error for an answer beyond the exact range, character for character: the name is filled in now and
	//! the value (`%.0f` of the rounded answer, as the solver prints it) at run time.
	unique_ptr<Expression> OutOfRangeError(const string &name, ColumnBinding value) const {
		vector<unique_ptr<Expression>> parts;
		parts.push_back(make_uniq<BoundConstantExpression>(Value(DecideWholeNumberRangeMessageBefore(name))));
		parts.push_back(optimizer.BindScalarFunction("printf", make_uniq<BoundConstantExpression>(Value("%.0f")),
		                                             RoundedAnswer(value)));
		parts.push_back(make_uniq<BoundConstantExpression>(Value(DecideWholeNumberRangeMessageAfter(name))));
		return DirectErrorPredicate(optimizer, optimizer.BindScalarFunction("concat", std::move(parts)));
	}

	//! The condition the breaker evaluates on every row. It is true, or the row raises the error that applies: a
	//! coefficient the arithmetic made non-finite, an empty box, an improvement without limit, or a whole number the
	//! answer cannot hold exactly. Every raw value was checked in P2 and arrives as `valid_data`.
	unique_ptr<Expression> ValidAnswers(const Answers &answers) const {
		vector<unique_ptr<Expression>> checks;
		for (idx_t d = 0; d < condition.decisions.size(); d++) {
			auto value = [&]() {
				return DirectColumn(LogicalType::DOUBLE, answers.value[d]);
			};
			// Raw values were named in P2; these are derived, so a NULL here has no source to name.
			checks.push_back(DirectValidNumberPredicate(optimizer, LogicalType::DOUBLE, answers.a[d], nullptr,
			                                            DirectNumberKind::OBJECTIVE));
			checks.push_back(DirectValidNumberPredicate(optimizer, LogicalType::DOUBLE, answers.b[d], nullptr,
			                                            DirectNumberKind::OBJECTIVE));
			checks.push_back(make_uniq<BoundCaseExpression>(DirectColumn(LogicalType::BOOLEAN, answers.infeasible[d]),
			                                                DirectInfeasibleErrorPredicate(optimizer),
			                                                DirectConstantBool(true)));
			checks.push_back(make_uniq<BoundCaseExpression>(optimizer.BindScalarFunction("isfinite", value()),
			                                                DirectConstantBool(true),
			                                                DirectUnboundedErrorPredicate(optimizer)));
			if (DirectIsWholeNumbered(condition.decisions[d].domain)) {
				checks.push_back(
				    make_uniq<BoundCaseExpression>(WithinExactRange(answers.value[d]), DirectConstantBool(true),
				                                   OutOfRangeError(condition.decisions[d].name, answers.value[d])));
			}
		}
		unique_ptr<Expression> chain = DirectConstantBool(true);
		for (idx_t i = checks.size(); i > 0; i--) {
			chain =
			    make_uniq<BoundCaseExpression>(std::move(checks[i - 1]), std::move(chain), DirectConstantBool(false));
		}
		return make_uniq<BoundCaseExpression>(DirectColumn(LogicalType::BOOLEAN, answers.valid_data), std::move(chain),
		                                      DirectConstantBool(false));
	}

	//! The answer in the decision's own output type. The breaker has already raised for anything that is not finite or
	//! not exactly representable, so the guard only keeps the cast from raising on its own if that ever changed.
	unique_ptr<Expression> TypedAnswer(idx_t decision, ColumnBinding value) const {
		auto &prepared = condition.decisions[decision];
		if (!DirectIsWholeNumbered(prepared.domain)) {
			return DirectColumn(LogicalType::DOUBLE, value);
		}
		auto finite = make_uniq<BoundCaseExpression>(
		    optimizer.BindScalarFunction("isfinite", DirectColumn(LogicalType::DOUBLE, value)), WithinExactRange(value),
		    DirectConstantBool(false));
		auto cast = BoundCastExpression::AddCastToType(optimizer.context, DirectColumn(LogicalType::DOUBLE, value),
		                                               prepared.output_type);
		return make_uniq<BoundCaseExpression>(std::move(finite), std::move(cast),
		                                      make_uniq<BoundConstantExpression>(Value(prepared.output_type)));
	}
};

DirectSubstitute A1BoundedRule::Apply(unique_ptr<LogicalOperator> source, Optimizer &optimizer,
                                      const DirectApplicabilityCondition &candidate) const {
	auto &condition = static_cast<const A1Condition &>(candidate);
	A1PlanBuilder plan_builder(condition, optimizer, *source);
	return plan_builder.Build(std::move(source));
}

} // namespace direct_a1

unique_ptr<DirectSolveRule> MakeA1BoundedRule() {
	return make_uniq<direct_a1::A1BoundedRule>();
}

} // namespace duckdb
