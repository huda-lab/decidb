#include "duckdb/optimizer/decide/direct/a1_rule.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/optimizer/decide/decide_bound_absorption.hpp"
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
//! Below this magnitude a DOUBLE can still carry a fraction, so a rounded bound is worth testing.
constexpr double FRACTION_LIMIT = 4503599627370496.0;
//! The solver reads a bound of this magnitude as no bound at all: an upper bound of 1e20 or more, or a lower bound of
//! -1e20 or less.
constexpr double INFINITE_BOUND = 1e20;
constexpr double POSITIVE_INFINITY = std::numeric_limits<double>::infinity();
constexpr double NEGATIVE_INFINITY = -std::numeric_limits<double>::infinity();

struct A1Match final : DirectRuleMatch {};

//! A numeric expression over the source, read as DOUBLE and multiplied by `sign`.
struct A1Value {
	int sign = 1;
	unique_ptr<Expression> expr;
};

//! One bound the plan evaluates per row: `coefficient * x  comparison  rhs`, under an optional WHEN. A bound the solver
//! folds into the variable's box is not a clause, it is part of the decision's static box.
struct A1Clause {
	idx_t decision = 0;
	ExpressionType comparison = ExpressionType::INVALID;
	vector<A1Value> coefficient;
	//! The coefficient as the user wrote it, which a NULL or non-finite one quotes.
	string coefficient_text;
	unique_ptr<Expression> rhs;
	//! The source column the right side is exactly, for the error that names it; empty for a computed bound.
	string rhs_column;
	unique_ptr<Expression> when;

	//! A deep copy of every field, so a field added later cannot be missing from a clause split in two.
	A1Clause Copy() const {
		A1Clause result;
		result.decision = decision;
		result.comparison = comparison;
		for (auto &value : coefficient) {
			result.coefficient.push_back({value.sign, value.expr->Copy()});
		}
		result.coefficient_text = coefficient_text;
		result.rhs = rhs->Copy();
		result.rhs_column = rhs_column;
		if (when) {
			result.when = when->Copy();
		}
		return result;
	}
};

struct A1Term {
	enum class Kind : uint8_t { LINEAR, CONSTANT, SQUARE };
	Kind kind = Kind::LINEAR;
	idx_t decision = 0;
	int sign = 1;
	//! LINEAR: the coefficient of the decision. CONSTANT: the value.
	unique_ptr<Expression> value;
	//! SQUARE: `square_scale * (inner)^2`, where the inner expression is linear in one decision.
	double square_scale = 1.0;
	vector<A1Value> inner_linear;
	vector<A1Value> inner_constant;
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
	DirectDomain domain = DirectDomain::REAL;
	LogicalType output_type;
	//! The decision's box before any per-row bound: its default, replaced or tightened by the bounds the solver folds
	//! into the variable.
	double lower = 0.0;
	double upper = POSITIVE_INFINITY;
};

struct A1Proof final : DirectRuleProof {
	idx_t decide_index = 0;
	DecideSense sense = DecideSense::FEASIBILITY;
	vector<A1Decision> decisions;
	vector<A1Clause> clauses;
	unique_ptr<Expression> objective_when;
	vector<A1Part> parts;
	bool has_square = false;
	//! Every value is a constant and nothing is conditional, so a check that fails on one row fails on all of them.
	bool constant_problem = false;
};

bool IsWholeNumbered(DirectDomain domain) {
	return domain != DirectDomain::REAL;
}

//! What a NULL or non-finite value quotes: the source column when the value is exactly one, otherwise the expression.
string TextOfExpression(const Expression &expr) {
	auto column = DirectBareNumericColumn(expr);
	return column ? column->GetAlias() : expr.ToString();
}

string TextOf(const vector<A1Value> &values) {
	return values.size() == 1 ? TextOfExpression(*values[0].expr) : string();
}

//! No value reads a source column and nothing is conditional: every row sees the same bounds and coefficients.
bool IsConstantProblem(const A1Proof &proof) {
	auto constant = [](const Expression &expr) {
		return expr.IsFoldable();
	};
	auto constant_values = [&](const vector<A1Value> &values) {
		for (auto &value : values) {
			if (!constant(*value.expr)) {
				return false;
			}
		}
		return true;
	};
	if (proof.objective_when) {
		return false;
	}
	for (auto &clause : proof.clauses) {
		if (clause.when || !constant(*clause.rhs) || !constant_values(clause.coefficient)) {
			return false;
		}
	}
	for (auto &part : proof.parts) {
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

	unique_ptr<DirectRuleMatch> Match(const DirectProblemFacts &facts, string &reason) const override {
		if (facts.decisions_status != DirectFactStatus::KNOWN || facts.decisions.empty() ||
		    !facts.entity_scopes.empty() || facts.source_status != DirectFactStatus::KNOWN) {
			reason = "variable_shape: expected row-scoped BOOL, INT or REAL decisions";
			return nullptr;
		}
		for (auto &decision : facts.decisions) {
			if (decision.scope != DecideVarScope::ROW) {
				reason = "variable_shape: expected row-scoped BOOL, INT or REAL decisions";
				return nullptr;
			}
		}
		if (!std::isfinite(facts.objective.offset) ||
		    (facts.objective.sense != DecideSense::MAXIMIZE && facts.objective.sense != DecideSense::MINIMIZE &&
		     facts.objective.sense != DecideSense::FEASIBILITY)) {
			reason = "problem_shape: expected a finite objective constant and MINIMIZE, MAXIMIZE or no objective";
			return nullptr;
		}
		if (facts.constraints_status != DirectFactStatus::KNOWN) {
			reason = "constraint_facts_unknown: " + facts.constraints_reason;
			return nullptr;
		}
		for (auto &fact : facts.constraints) {
			if (fact.status != DirectFactStatus::KNOWN) {
				reason = "constraint_facts_unknown: " + facts.FirstUnknownReason();
				return nullptr;
			}
			if (fact.aggregate) {
				reason = "constraint_shape: expected a bound on one decision, found an aggregate constraint";
				return nullptr;
			}
			if (fact.comparison == ExpressionType::COMPARE_IN || fact.comparison == ExpressionType::COMPARE_NOTEQUAL) {
				reason = "constraint_shape: expected an interval bound, found <> or IN";
				return nullptr;
			}
			if (fact.comparison != ExpressionType::COMPARE_EQUAL && !DirectIsLowerBound(fact.comparison) &&
			    !DirectIsUpperBound(fact.comparison)) {
				reason = "constraint_shape: expected an interval bound";
				return nullptr;
			}
			if (!fact.scope.per_key_slots.empty()) {
				reason = "constraint_scope: PER does not apply to a bound on one row";
				return nullptr;
			}
			if (!ReadsOneDecision(fact, facts.decisions.size())) {
				reason = "constraint_shape: expected a bound of the form a*x <comparison> value on one decision";
				return nullptr;
			}
		}
		if (facts.objective.status != DirectFactStatus::KNOWN) {
			reason = "objective_facts_unknown: " + facts.objective.reason;
			return nullptr;
		}
		if (!facts.objective.scope.per_key_slots.empty()) {
			reason = "objective_scope: expected an objective without PER";
			return nullptr;
		}
		for (auto &part : facts.objective.parts) {
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
		return make_uniq<A1Match>();
	}

	unique_ptr<DirectRuleProof> Prove(const DirectProblemFacts &facts, const DirectRuleMatch &, ClientContext &context,
	                                  string &reason) const override {
		auto proof = make_uniq<A1Proof>();
		proof->decide_index = facts.decide_index;
		proof->sense = facts.objective.sense;
		for (auto &decision : facts.decisions) {
			A1Decision prepared;
			prepared.domain = decision.domain;
			prepared.output_type = decision.output_type;
			proof->decisions.push_back(std::move(prepared));
		}
		if (!ProveBounds(facts, context, *proof, reason) || !ProveObjective(facts, context, *proof, reason)) {
			return nullptr;
		}
		proof->constant_problem = IsConstantProblem(*proof);
		return std::move(proof);
	}

	double Cost(const DirectRuleProof &, const DirectCostContext &) const override {
		// A1 needs no aggregate constraint and S1 needs one, so two proved rules never compete for one query.
		return 0.0;
	}

	void Explain(const DirectRuleProof &, DirectSolveDecisionRecord &record) const override {
		record.proof = "min/max SUM(c*x), l <= x <= u, each row independent";
	}

	DirectRelationalProposal Rewrite(unique_ptr<LogicalOperator> source, Optimizer &optimizer,
	                                 const DirectRuleProof &candidate) const override;

private:
	//! One unfiltered per-row left side: only linear terms, in one decision, and no constant.
	static bool ReadsOneDecision(const DirectConstraintFact &fact, idx_t decision_count) {
		if (fact.lhs.size() != 1 || fact.degree != 1) {
			return false;
		}
		auto &part = fact.lhs[0];
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

	bool ProveBounds(const DirectProblemFacts &facts, ClientContext &context, A1Proof &proof, string &reason) const;
	bool ProveObjective(const DirectProblemFacts &facts, ClientContext &context, A1Proof &proof, string &reason) const;
};

//! A coefficient or value the plan evaluates per row: numeric, deterministic, independent of the decisions, over the
//! source. It may raise when evaluated; the plan evaluates it on every row, as the solver does.
bool ProveValue(const DirectProblemFacts &facts, const Expression &expr) {
	return expr.return_type.IsNumeric() && DirectIsNumericDecisionFree(expr, facts.decide_index) &&
	       DirectReferencesOnlySource(expr, facts.source_bindings);
}

bool ProveValues(const DirectProblemFacts &facts, const vector<DecideSplitTerm> &terms, vector<A1Value> &out) {
	for (auto &term : terms) {
		if (!term.coefficient || !ProveValue(facts, *term.coefficient)) {
			return false;
		}
		A1Value value;
		value.sign = term.sign;
		value.expr = term.coefficient->Copy();
		out.push_back(std::move(value));
	}
	return true;
}

//! The decision's box before any per-row bound. A bound the solver folds into the variable (an unconditional comparison
//! of the bare decision against a constant) replaces the default lower bound of 0 and tightens the upper one; every
//! other bound only intersects with the default, so it never reaches the box.
bool A1BoundedRule::ProveBounds(const DirectProblemFacts &facts, ClientContext &context, A1Proof &proof,
                                string &reason) const {
	idx_t decision_count = facts.decisions.size();
	vector<bool> lower_written(decision_count, false);
	vector<double> absorbed_lower(decision_count, NEGATIVE_INFINITY);
	vector<double> absorbed_upper(decision_count, POSITIVE_INFINITY);
	// The same box as the solver folds it, from the raw values it reads (no rounding, no mapping to infinity), kept
	// only to apply the solver's check of the box against the variable's built-in domain below.
	vector<double> solver_lower(decision_count, LogicalDecide::ABSORBED_LOWER_UNSET);
	vector<double> solver_upper(decision_count, LogicalDecide::ABSORBED_UPPER_UNSET);
	for (auto &fact : facts.constraints) {
		idx_t decision = fact.lhs[0].terms[0].var_a;
		auto domain = proof.decisions[decision].domain;
		auto &rhs = *fact.rhs;
		auto comparison = fact.comparison;
		if (!DirectIsSourceOnlyValue(rhs, facts.decide_index, facts.source_bindings)) {
			reason = "coefficient_shape: expected a deterministic numeric expression over the source columns on the "
			         "right of a bound";
			return false;
		}
		if (fact.scope.when &&
		    !DirectIsSourceOnlyPredicate(*fact.scope.when, facts.decide_index, facts.source_bindings)) {
			reason = "constraint_scope: WHEN must be a deterministic source-only predicate";
			return false;
		}
		if (fact.variable_bound) {
			double k;
			if (!DirectFoldableDouble(context, rhs, k)) {
				reason = "constraint_shape: expected a finite constant on the right of a bound on the decision";
				return false;
			}
			bool whole = IsWholeNumbered(domain);
			bool strict =
			    comparison == ExpressionType::COMPARE_LESSTHAN || comparison == ExpressionType::COMPARE_GREATERTHAN;
			if (strict && !whole) {
				reason = "constraint_shape: a strict bound on a continuous decision";
				return false;
			}
			if (DirectIsLowerBound(comparison)) {
				solver_lower[decision] = std::max(solver_lower[decision], strict ? DecideStrictBound(true, k) : k);
				double value = strict ? DecideStrictBound(true, k) : (whole ? std::ceil(k) : k);
				if (value <= -INFINITE_BOUND) {
					value = NEGATIVE_INFINITY;
				}
				lower_written[decision] = true;
				absorbed_lower[decision] = std::max(absorbed_lower[decision], value);
			}
			if (DirectIsUpperBound(comparison)) {
				solver_upper[decision] = std::min(solver_upper[decision], strict ? DecideStrictBound(false, k) : k);
				double value = strict ? DecideStrictBound(false, k) : (whole ? std::floor(k) : k);
				if (value >= INFINITE_BOUND) {
					value = POSITIVE_INFINITY;
				}
				absorbed_upper[decision] = std::min(absorbed_upper[decision], value);
			}
			continue;
		}
		A1Clause clause;
		clause.decision = decision;
		clause.comparison = comparison;
		if (!ProveValues(facts, fact.lhs[0].terms, clause.coefficient)) {
			reason = "coefficient_shape: expected a deterministic numeric coefficient over the source columns";
			return false;
		}
		clause.coefficient_text = TextOf(clause.coefficient);
		clause.rhs = fact.rhs->Copy();
		idx_t slot;
		LogicalType type;
		string name;
		if (DirectSourceNumericColumn(rhs, facts.source_bindings, slot, type, name)) {
			clause.rhs_column = name;
		}
		if (fact.scope.when) {
			clause.when = fact.scope.when->Copy();
		}
		if (comparison == ExpressionType::COMPARE_EQUAL) {
			// An equality is a lower and an upper bound that may not agree: a fractional value for a whole-numbered
			// decision then leaves no row.
			auto upper = clause.Copy();
			upper.comparison = ExpressionType::COMPARE_LESSTHANOREQUALTO;
			clause.comparison = ExpressionType::COMPARE_GREATERTHANOREQUALTO;
			proof.clauses.push_back(std::move(clause));
			proof.clauses.push_back(std::move(upper));
		} else {
			proof.clauses.push_back(std::move(clause));
		}
	}
	for (idx_t decision = 0; decision < decision_count; decision++) {
		// The solver rejects, before it reads any row, a box that contradicts the variable's built-in domain
		// (DecideBoxContradictsDomain). It says so on an empty source too, so the query is left to the solver, which
		// reports it, rather than proved here and answered with no rows.
		if (DecideBoxContradictsDomain(solver_lower[decision], solver_upper[decision],
		                               proof.decisions[decision].domain == DirectDomain::BOOL)) {
			reason =
			    "variable_bound: a bound contradicts the decision's built-in domain (non-negative, or BOOL 0 or 1); "
			    "the solver reports it";
			return false;
		}
	}
	for (idx_t decision = 0; decision < decision_count; decision++) {
		auto &prepared = proof.decisions[decision];
		bool boolean = prepared.domain == DirectDomain::BOOL;
		// A written lower bound replaces the default of 0, except that a BOOL never leaves {0, 1}.
		if (lower_written[decision]) {
			prepared.lower = boolean ? std::max(0.0, absorbed_lower[decision]) : absorbed_lower[decision];
		} else {
			prepared.lower = 0.0;
		}
		prepared.upper = boolean ? std::min(1.0, absorbed_upper[decision]) : absorbed_upper[decision];
	}
	return true;
}

bool A1BoundedRule::ProveObjective(const DirectProblemFacts &facts, ClientContext &context, A1Proof &proof,
                                   string &reason) const {
	auto &objective = facts.objective;
	if (objective.scope.when) {
		if (!DirectIsSourceOnlyPredicate(*objective.scope.when, facts.decide_index, facts.source_bindings)) {
			reason = "objective_scope: WHEN must be a deterministic source-only predicate";
			return false;
		}
		proof.objective_when = objective.scope.when->Copy();
	}
	idx_t decision_count = facts.decisions.size();
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
			if (!DirectIsSourceOnlyPredicate(*part.filter, facts.decide_index, facts.source_bindings)) {
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
				if (!term.coefficient || !ProveValue(facts, *term.coefficient)) {
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
				if (proof.has_square) {
					reason = "objective_shape: expected at most one squared term, as the solver accepts";
					return false;
				}
				proof.has_square = true;
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
					if (!inner.coefficient || !ProveValue(facts, *inner.coefficient)) {
						reason = "coefficient_shape: expected a deterministic numeric expression over the source "
						         "columns";
						return false;
					}
					A1Value value;
					value.sign = inner.sign;
					value.expr = inner.coefficient->Copy();
					(inner.kind == DecideTermKind::LINEAR ? built.inner_linear : built.inner_constant)
					    .push_back(std::move(value));
				}
			}
			prepared.terms.push_back(std::move(built));
		}
		proof.parts.push_back(std::move(prepared));
	}
	return true;
}

//===--------------------------------------------------------------------===//
// Plan
//===--------------------------------------------------------------------===//

//! Expressions as values, so the per-row algebra reads like the algebra. Each operation copies its operands.
using Ex = std::shared_ptr<Expression>;

class Calc {
public:
	explicit Calc(Optimizer &optimizer_p) : optimizer(optimizer_p) {
	}

	Ex Wrap(unique_ptr<Expression> expr) const {
		return Ex(expr.release());
	}
	unique_ptr<Expression> Out(const Ex &expr) const {
		return expr->Copy();
	}
	Ex Num(double value) const {
		return Wrap(make_uniq<BoundConstantExpression>(Value::DOUBLE(value)));
	}
	Ex Bool(bool value) const {
		return Wrap(DirectConstantBool(value));
	}
	Ex Col(const LogicalType &type, ColumnBinding binding) const {
		return Wrap(DirectColumn(type, binding));
	}
	Ex Fn(const string &name, const Ex &a) const {
		return Wrap(optimizer.BindScalarFunction(name, a->Copy()));
	}
	Ex Fn(const string &name, const Ex &a, const Ex &b) const {
		return Wrap(optimizer.BindScalarFunction(name, a->Copy(), b->Copy()));
	}
	Ex Add(const Ex &a, const Ex &b) const {
		return Fn("+", a, b);
	}
	Ex Sub(const Ex &a, const Ex &b) const {
		return Fn("-", a, b);
	}
	Ex Mul(const Ex &a, const Ex &b) const {
		return Fn("*", a, b);
	}
	Ex Div(const Ex &a, const Ex &b) const {
		return Fn("/", a, b);
	}
	Ex Neg(const Ex &a) const {
		return Mul(a, Num(-1.0));
	}
	Ex Cmp(ExpressionType type, const Ex &a, const Ex &b) const {
		return Wrap(make_uniq<BoundComparisonExpression>(type, a->Copy(), b->Copy()));
	}
	Ex If(const Ex &condition, const Ex &then_value, const Ex &else_value) const {
		return Wrap(make_uniq<BoundCaseExpression>(condition->Copy(), then_value->Copy(), else_value->Copy()));
	}
	Ex Min(const vector<Ex> &values) const {
		return Extremum("least", values);
	}
	Ex Max(const vector<Ex> &values) const {
		return Extremum("greatest", values);
	}
	//! True when any condition is true; false for none.
	Ex Any(const vector<Ex> &conditions) const {
		Ex result = Bool(false);
		for (idx_t i = conditions.size(); i > 0; i--) {
			result = If(conditions[i - 1], Bool(true), result);
		}
		return result;
	}

private:
	Optimizer &optimizer;

	Ex Extremum(const char *name, const vector<Ex> &values) const {
		if (values.size() == 1) {
			return values[0];
		}
		vector<unique_ptr<Expression>> children;
		for (auto &value : values) {
			children.push_back(value->Copy());
		}
		return Wrap(optimizer.BindScalarFunction(name, std::move(children)));
	}
};

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
//!   barrier: every row is validated before one is released
//!   P5 the source columns, the typed answers, and the barrier
class A1Rewriter {
public:
	A1Rewriter(const A1Proof &proof_p, Optimizer &optimizer_p, LogicalOperator &source)
	    : proof(proof_p), optimizer(optimizer_p), binder(optimizer_p.binder), calc(optimizer_p) {
		// The coordinator hands the input over with its types resolved.
		source_bindings = source.GetColumnBindings();
		source_types = source.types;
		if (source_bindings.size() != source_types.size()) {
			throw InternalException("Direct solve source bindings and types differ");
		}
	}

	DirectRelationalProposal Build(unique_ptr<LogicalOperator> source) const {
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

		DirectRelationalProposal proposal;
		for (idx_t d = 0; d < proof.decisions.size(); d++) {
			proposal.prunable_decisions.push_back(1);
		}
		for (idx_t i = 0; i < source_bindings.size() + proof.decisions.size(); i++) {
			proposal.output_slots.push_back(i);
		}
		proposal.validation_slots.push_back(source_bindings.size() + proof.decisions.size());

		Stage p5(binder.GenerateTableIndex());
		if (proof.constant_problem) {
			// Every row sees the same values, so the first row raises whatever the others would, and nothing has to be
			// held back: the plan streams, and the validation column raises wherever a row is read. An empty source
			// has no row to raise on and returns no rows. A bound the solver rejects outright (against the variable's
			// built-in domain) is never proved, so it cannot differ on an empty input.
			for (idx_t i = 0; i < source_bindings.size(); i++) {
				p5.Add(DirectColumn(source_types[i], passthrough4[i]));
			}
			for (idx_t d = 0; d < proof.decisions.size(); d++) {
				p5.Add(TypedAnswer(d, answers.value[d]));
			}
			p5.Add(ValidAnswers(answers));
			proposal.child = p5.Finish(std::move(plan));
			return proposal;
		}
		auto barrier = DirectValidationBarrier(binder, std::move(plan), ValidAnswers(answers));
		for (idx_t i = 0; i < source_bindings.size(); i++) {
			p5.Add(DirectColumn(source_types[i], barrier.input_bindings[i]));
		}
		for (idx_t d = 0; d < proof.decisions.size(); d++) {
			p5.Add(TypedAnswer(d, barrier.input_bindings[answers.value[d].column_index]));
		}
		p5.Add(DirectColumn(LogicalType::BIGINT, barrier.barrier));
		proposal.child = p5.Finish(std::move(barrier.plan));
		return proposal;
	}

private:
	//! Columns of P1.
	struct Raw {
		vector<ColumnBinding> source;
		struct ClauseColumns {
			ColumnBinding coefficient;
			ColumnBinding rhs;
			bool has_when = false;
			ColumnBinding when;
		};
		vector<ClauseColumns> clauses;
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

	const A1Proof &proof;
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
	unique_ptr<Expression> SumOf(const vector<A1Value> &values) const {
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
		for (auto &clause : proof.clauses) {
			Raw::ClauseColumns columns;
			columns.coefficient = p1.Add(SumOf(clause.coefficient));
			columns.rhs = p1.Add(AsDouble(clause.rhs->Copy()));
			if (clause.when) {
				columns.has_when = true;
				columns.when = p1.Add(DirectActiveWhen(clause.when.get()));
			}
			checks.push_back(DirectValidNumberPredicate(optimizer, LogicalType::DOUBLE, columns.coefficient,
			                                            clause.coefficient_text, DirectNumberKind::CONSTRAINT));
			checks.push_back(DirectValidBoundPredicate(optimizer, columns.rhs, clause.rhs_column));
			raw.clauses.push_back(columns);
		}
		bool has_shared_mask = false;
		ColumnBinding shared_mask;
		for (auto &part : proof.parts) {
			Raw::PartColumns columns;
			if (proof.objective_when || part.filter) {
				columns.has_mask = true;
				if (!part.filter && has_shared_mask) {
					columns.mask = shared_mask;
				} else {
					auto base = DirectActiveWhen(proof.objective_when.get());
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
		vector<unique_ptr<Expression>> linear_sums(proof.decisions.size());
		vector<string> linear_text(proof.decisions.size());
		vector<idx_t> linear_count(proof.decisions.size(), 0);
		unique_ptr<Expression> constant_sum;
		idx_t constant_count = 0;
		string constant_text;
		for (auto &term : part.terms) {
			if (term.kind == A1Term::Kind::LINEAR) {
				auto value = DirectScaledTerm(optimizer, term.value->Copy(), term.sign, false, 1.0, false);
				auto &sum = linear_sums[term.decision];
				sum = sum ? optimizer.BindScalarFunction("+", std::move(sum), std::move(value)) : std::move(value);
				linear_text[term.decision] = TextOfExpression(*term.value);
				linear_count[term.decision]++;
			} else if (term.kind == A1Term::Kind::CONSTANT) {
				auto value = DirectScaledTerm(optimizer, term.value->Copy(), term.sign, false, 1.0, false);
				constant_sum = constant_sum
				                   ? optimizer.BindScalarFunction("+", std::move(constant_sum), std::move(value))
				                   : std::move(value);
				constant_text = TextOfExpression(*term.value);
				constant_count++;
			} else {
				Raw::SquareColumns square;
				square.decision = term.decision;
				square.scale = term.sign * term.square_scale;
				square.linear = p1.Add(SumOf(term.inner_linear));
				checks.push_back(DirectValidNumberPredicate(optimizer, LogicalType::DOUBLE, square.linear,
				                                            TextOf(term.inner_linear), DirectNumberKind::OBJECTIVE));
				if (!term.inner_constant.empty()) {
					square.constant = p1.Add(SumOf(term.inner_constant));
					checks.push_back(DirectValidNumberPredicate(optimizer, LogicalType::DOUBLE, square.constant,
					                                            TextOf(term.inner_constant),
					                                            DirectNumberKind::OBJECTIVE));
				} else {
					square.constant = p1.Add(make_uniq<BoundConstantExpression>(Value::DOUBLE(0.0)));
				}
				columns.squares.push_back(square);
			}
		}
		for (idx_t d = 0; d < proof.decisions.size(); d++) {
			if (!linear_sums[d]) {
				continue;
			}
			auto binding = p1.Add(std::move(linear_sums[d]));
			checks.push_back(DirectValidNumberPredicate(optimizer, LogicalType::DOUBLE, binding,
			                                            linear_count[d] == 1 ? linear_text[d] : string(),
			                                            DirectNumberKind::OBJECTIVE));
			columns.linear_decision.push_back(d);
			columns.linear.push_back(binding);
		}
		if (constant_sum) {
			auto binding = p1.Add(std::move(constant_sum));
			checks.push_back(DirectValidNumberPredicate(optimizer, LogicalType::DOUBLE, binding,
			                                            constant_count == 1 ? constant_text : string(),
			                                            DirectNumberKind::OBJECTIVE));
		}
	}

	//===------------------------------------------------------------------===//
	// P2: per-clause bounds, objective coefficients, raw-value checks
	//===------------------------------------------------------------------===//

	void ComputeBounds(Stage &p2, const Raw &raw, vector<unique_ptr<Expression>> &checks, Bounds &bounds) const {
		for (idx_t j = 0; j < proof.clauses.size(); j++) {
			Ex lower, upper;
			ClauseBounds(proof.clauses[j], raw.clauses[j], lower, upper);
			bounds.lower.push_back(p2.Add(calc.Out(lower)));
			bounds.upper.push_back(p2.Add(calc.Out(upper)));
		}
		double sense = proof.sense == DecideSense::MAXIMIZE ? -1.0 : 1.0;
		for (idx_t d = 0; d < proof.decisions.size(); d++) {
			Ex a_total = calc.Num(0.0);
			Ex b_total = calc.Num(0.0);
			for (idx_t p = 0; p < proof.parts.size(); p++) {
				auto &part = proof.parts[p];
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

	//! The lower and upper bound one clause puts on its decision on one row, as the solver reads it: a clause holds
	//! `coefficient * x  comparison  rhs`, so the coefficient's sign picks the side, a zero coefficient leaves the row
	//! alone or infeasible, and a whole-numbered decision rounds to the integer DOUBLE arithmetic accepts.
	void ClauseBounds(const A1Clause &clause, const Raw::ClauseColumns &columns, Ex &lower, Ex &upper) const {
		auto a = calc.Col(LogicalType::DOUBLE, columns.coefficient);
		auto r = calc.Col(LogicalType::DOUBLE, columns.rhs);
		auto zero = calc.Num(0.0);
		auto positive_infinity = calc.Num(POSITIVE_INFINITY);
		auto negative_infinity = calc.Num(NEGATIVE_INFINITY);
		auto comparison = clause.comparison;
		bool at_most =
		    comparison == ExpressionType::COMPARE_LESSTHANOREQUALTO || comparison == ExpressionType::COMPARE_LESSTHAN;
		bool whole = IsWholeNumbered(proof.decisions[clause.decision].domain);

		auto feasible = [&](const Ex &k) {
			return calc.Cmp(comparison, calc.Mul(a, k), r);
		};
		auto v = calc.Div(r, a);
		Ex upper_candidate = v;
		Ex lower_candidate = v;
		if (whole) {
			auto huge =
			    calc.Cmp(ExpressionType::COMPARE_GREATERTHANOREQUALTO, calc.Fn("abs", v), calc.Num(FRACTION_LIMIT));
			auto floored = calc.Fn("floor", v);
			auto up_one = calc.Add(floored, calc.Num(1.0));
			auto down_one = calc.Sub(floored, calc.Num(1.0));
			upper_candidate = calc.If(huge, floored,
			                          calc.If(feasible(up_one), up_one, calc.If(feasible(floored), floored, down_one)));
			auto ceiled = calc.Fn("ceil", v);
			auto ceil_up = calc.Add(ceiled, calc.Num(1.0));
			auto ceil_down = calc.Sub(ceiled, calc.Num(1.0));
			lower_candidate = calc.If(
			    huge, ceiled, calc.If(feasible(ceil_down), ceil_down, calc.If(feasible(ceiled), ceiled, ceil_up)));
		}
		auto positive = calc.Cmp(ExpressionType::COMPARE_GREATERTHAN, a, zero);
		auto negative = calc.Cmp(ExpressionType::COMPARE_LESSTHAN, a, zero);
		// `at most` clauses bound x above when the coefficient is positive and below when it is negative; `at least`
		// clauses do the opposite.
		auto upper_by_sign = at_most ? calc.If(positive, upper_candidate, positive_infinity)
		                             : calc.If(negative, upper_candidate, positive_infinity);
		auto lower_by_sign = at_most ? calc.If(negative, lower_candidate, negative_infinity)
		                             : calc.If(positive, lower_candidate, negative_infinity);
		// A zero coefficient says `0 comparison rhs`: it holds for every x, or for none.
		auto holds = calc.Cmp(comparison, zero, r);
		auto zero_coefficient = calc.Cmp(ExpressionType::COMPARE_EQUAL, a, zero);
		auto row_upper = calc.If(zero_coefficient, positive_infinity, upper_by_sign);
		auto row_lower = calc.If(zero_coefficient, calc.If(holds, negative_infinity, positive_infinity), lower_by_sign);
		// A right side the solver reads as infinite binds nothing.
		auto vacuous = at_most ? calc.Cmp(ExpressionType::COMPARE_GREATERTHANOREQUALTO, r, calc.Num(INFINITE_BOUND))
		                       : calc.Cmp(ExpressionType::COMPARE_LESSTHANOREQUALTO, r, calc.Num(-INFINITE_BOUND));
		upper = calc.If(vacuous, positive_infinity, row_upper);
		lower = calc.If(vacuous, negative_infinity, row_lower);
		if (columns.has_when) {
			auto active = calc.Col(LogicalType::BOOLEAN, columns.when);
			upper = calc.If(active, upper, positive_infinity);
			lower = calc.If(active, lower, negative_infinity);
		}
	}

	//===------------------------------------------------------------------===//
	// P3: each decision's box
	//===------------------------------------------------------------------===//

	void ComputeBox(Stage &p3, const Bounds &bounds, Box &box) const {
		for (idx_t d = 0; d < proof.decisions.size(); d++) {
			vector<Ex> lowers {calc.Num(proof.decisions[d].lower)};
			vector<Ex> uppers {calc.Num(proof.decisions[d].upper)};
			for (idx_t j = 0; j < proof.clauses.size(); j++) {
				if (proof.clauses[j].decision == d) {
					lowers.push_back(calc.Col(LogicalType::DOUBLE, bounds.lower[j]));
					uppers.push_back(calc.Col(LogicalType::DOUBLE, bounds.upper[j]));
				}
			}
			box.lower.push_back(p3.Add(calc.Out(calc.Max(lowers))));
			box.upper.push_back(p3.Add(calc.Out(calc.Min(uppers))));
			box.a.push_back(p3.Add(DirectColumn(LogicalType::DOUBLE, bounds.a[d])));
			box.b.push_back(p3.Add(DirectColumn(LogicalType::DOUBLE, bounds.b[d])));
		}
		box.valid_data = p3.Add(DirectColumn(LogicalType::BOOLEAN, bounds.valid_data));
	}

	//===------------------------------------------------------------------===//
	// P4: each decision's answer
	//===------------------------------------------------------------------===//

	void ComputeAnswers(Stage &p4, const Box &box, Answers &answers) const {
		for (idx_t d = 0; d < proof.decisions.size(); d++) {
			auto lower = calc.Col(LogicalType::DOUBLE, box.lower[d]);
			auto upper = calc.Col(LogicalType::DOUBLE, box.upper[d]);
			auto a = calc.Col(LogicalType::DOUBLE, box.a[d]);
			auto b = calc.Col(LogicalType::DOUBLE, box.b[d]);
			auto zero = calc.Num(0.0);
			bool whole = IsWholeNumbered(proof.decisions[d].domain);

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

	//! The condition the barrier evaluates on every row. It is true, or the row raises the error that applies: a
	//! coefficient the arithmetic made non-finite, an empty box, an improvement without limit, or a whole number the
	//! answer cannot hold exactly. Every raw value was checked in P2 and arrives as `valid_data`.
	unique_ptr<Expression> ValidAnswers(const Answers &answers) const {
		vector<unique_ptr<Expression>> checks;
		for (idx_t d = 0; d < proof.decisions.size(); d++) {
			auto value = [&]() {
				return DirectColumn(LogicalType::DOUBLE, answers.value[d]);
			};
			checks.push_back(DirectValidNumberPredicate(optimizer, LogicalType::DOUBLE, answers.a[d], string(),
			                                            DirectNumberKind::OBJECTIVE));
			checks.push_back(DirectValidNumberPredicate(optimizer, LogicalType::DOUBLE, answers.b[d], string(),
			                                            DirectNumberKind::OBJECTIVE));
			checks.push_back(make_uniq<BoundCaseExpression>(DirectColumn(LogicalType::BOOLEAN, answers.infeasible[d]),
			                                                DirectInfeasibleErrorPredicate(optimizer),
			                                                DirectConstantBool(true)));
			checks.push_back(make_uniq<BoundCaseExpression>(optimizer.BindScalarFunction("isfinite", value()),
			                                                DirectConstantBool(true),
			                                                DirectUnboundedErrorPredicate(optimizer)));
			if (IsWholeNumbered(proof.decisions[d].domain)) {
				auto within = make_uniq<BoundComparisonExpression>(
				    ExpressionType::COMPARE_LESSTHANOREQUALTO, optimizer.BindScalarFunction("abs", value()),
				    make_uniq<BoundConstantExpression>(Value::DOUBLE(DIRECT_MAX_EXACT_INTEGER)));
				checks.push_back(make_uniq<BoundCaseExpression>(
				    std::move(within), DirectConstantBool(true),
				    DirectErrorPredicate(optimizer, "DECIDE: a decision solves to a whole number beyond the range "
				                                    "DeciDB can return. Tighten its bounds, or declare it REAL to read "
				                                    "the value approximately.")));
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

	//! The answer in the decision's own output type. The barrier has already raised for anything that is not finite or
	//! not exactly representable, so the guard only keeps the cast from raising on its own if that ever changed.
	unique_ptr<Expression> TypedAnswer(idx_t decision, ColumnBinding value) const {
		auto &prepared = proof.decisions[decision];
		if (!IsWholeNumbered(prepared.domain)) {
			return DirectColumn(LogicalType::DOUBLE, value);
		}
		auto within = make_uniq<BoundComparisonExpression>(
		    ExpressionType::COMPARE_LESSTHANOREQUALTO,
		    optimizer.BindScalarFunction("abs", DirectColumn(LogicalType::DOUBLE, value)),
		    make_uniq<BoundConstantExpression>(Value::DOUBLE(DIRECT_MAX_EXACT_INTEGER)));
		auto finite = make_uniq<BoundCaseExpression>(
		    optimizer.BindScalarFunction("isfinite", DirectColumn(LogicalType::DOUBLE, value)), std::move(within),
		    DirectConstantBool(false));
		auto cast = BoundCastExpression::AddCastToType(optimizer.context, DirectColumn(LogicalType::DOUBLE, value),
		                                               prepared.output_type);
		return make_uniq<BoundCaseExpression>(std::move(finite), std::move(cast),
		                                      make_uniq<BoundConstantExpression>(Value(prepared.output_type)));
	}
};

DirectRelationalProposal A1BoundedRule::Rewrite(unique_ptr<LogicalOperator> source, Optimizer &optimizer,
                                                const DirectRuleProof &candidate) const {
	auto &proof = static_cast<const A1Proof &>(candidate);
	A1Rewriter rewriter(proof, optimizer, *source);
	return rewriter.Build(std::move(source));
}

} // namespace direct_a1

unique_ptr<DirectSolveRule> MakeA1BoundedRule() {
	return make_uniq<direct_a1::A1BoundedRule>();
}

} // namespace duckdb
