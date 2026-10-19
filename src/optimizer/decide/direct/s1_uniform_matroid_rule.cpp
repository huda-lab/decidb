#include "duckdb/optimizer/decide/direct/s1_uniform_matroid_rule.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <tuple>

#include "duckdb/common/decide_numeric_policy.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/aggregate/distributive_functions.hpp"
#include "duckdb/optimizer/decide/decide_singleton_row_folding.hpp"
#include "duckdb/optimizer/decide/direct/direct_plan_factory.hpp"
#include "duckdb/optimizer/decide/direct/direct_expression.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/decide/decide_term_split.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_window.hpp"
#include "s1_uniform_matroid_helpers.hpp"

namespace duckdb {

//! Each rule keeps its helpers in its own named namespace: unity builds compile every rule file in one translation
//! unit, where two anonymous namespaces would merge and same-named helpers would collide.
namespace direct_s1_uniform_matroid {

constexpr const char *RULE_NAME = "S1_UNIFORM_MATROID";

//! An aggregate on the right of a count bound, in the order a walk of the bound meets them: `COUNT(*)`, or COUNT, MIN,
//! MAX, SUM or AVG of one numeric source column. The plan replaces each by a window over the clause's group.
struct Reducer {
	//! The aggregate function's name: `count_star`, `count`, `min`, `max`, `sum` or `avg`.
	string function;
	//! The source column it reduces; invalid for `count_star`.
	idx_t source_slot = DConstants::INVALID_INDEX;
	LogicalType column_type;
	string column_name;
};

struct Condition final : DirectApplicabilityCondition {
	struct SourceBound {
		idx_t source_slot;
		LogicalType source_type;
		ExpressionType comparison;
		unique_ptr<Expression> value;
		bool rhs_all_group_rows;
		//! Name of the source column when the bound is exactly one; empty for a computed bound.
		string column_name;
		//! The bound holds aggregates over the clause's group (`reducers`), which the plan replaces by windows.
		bool counts_rows = false;
		//! The aggregates in the bound, in walk order.
		vector<Reducer> reducers;
		//! One of them needs the row count of the group: `COUNT(*)`, COUNT(col) or AVG.
		bool needs_count = false;
		//! The count body is `c - x`, so `comparison` is already flipped and the value to compare against is `n*c - B`
		//! (or `-B` without a constant).
		bool negated = false;
		//! The constant `c` of a count body `x + c` or `c - x`, in its own SQL type; null for a body with no constant
		//! term. The plan shifts the bound by `n*c`, where `n` is the number of rows the SUM covers in the group.
		unique_ptr<Expression> offset;
	};
	//! A fixed value that comes from the row: it must be neither NULL nor, for a floating point value, NaN.
	struct SourceFixedValue {
		unique_ptr<Expression> value;
		//! The source column the value is exactly; empty for a computed value.
		string column_name;
		//! The error a computed value raises when it is NULL (or NaN, when `reject_nan`).
		string invalid_message;
		bool reject_nan;
	};
	struct ObjectivePart {
		int sign;
		unique_ptr<Expression> coefficient;
		//! A constant factor the part's objective term is multiplied by, or divided by when `divides`.
		bool scaled = false;
		double scale = 1.0;
		bool divides = false;
	};

	//! The constraints of the IR, which outlives the condition: the count bounds, and the fixed Boolean values.
	vector<const DirectIRConstraint *> bound_factors;
	vector<const DirectIRConstraint *> local_fixes;

	idx_t decide_index;
	idx_t lower = 0;
	idx_t upper = 0;
	bool has_upper = false;
	bool impossible = false;
	DecideSense sense;
	vector<ObjectivePart> objective_parts;
	vector<idx_t> group_key_slots;
	unique_ptr<Expression> when_condition;
	vector<unique_ptr<Expression>> fixed_one_conditions;
	vector<unique_ptr<Expression>> fixed_zero_conditions;
	vector<SourceBound> source_bounds;
	vector<SourceFixedValue> source_fixed_values;
	bool scoped = false;
};

//! Every term is the decision times a coefficient.
bool OnlyDecisionTerms(const vector<DecideSplitTerm> &terms) {
	for (auto &term : terms) {
		if (term.kind != DecideTermKind::LINEAR || term.var_a != 0) {
			return false;
		}
	}
	return !terms.empty();
}

//! The terms add up to exactly one `x`: `x`, `1*x`, `x*1.0`, `(1+0)*x`. Their signed constant coefficients are
//! summed as the solver's like-term collection sums them; a constant term (`x + 0`) is not a unit contribution.
bool IsUnitContribution(ClientContext &context, const vector<DecideSplitTerm> &terms) {
	if (!OnlyDecisionTerms(terms)) {
		return false;
	}
	double total = 0;
	for (auto &term : terms) {
		double value;
		if (!term.coefficient ||
		    !TryEvaluateFoldableDouble(context, *term.coefficient, value, DIRECT_FINITE_CONSTANT)) {
			return false;
		}
		total += term.sign * value;
	}
	return total == 1.0;
}

//! The largest magnitude of a constant term in a count body (`SUM(x + c)`). The plan shifts the bound by `n*c`, where
//! `n` is the number of rows the SUM covers, known only at run time. A constant this small keeps `|n*c|` within 2^53
//! (exact in a DOUBLE and in BIGINT) for any `n` up to 2^32 rows, so DuckDB's own arithmetic and the solver's DOUBLE
//! arithmetic read the same shifted bound. A larger constant stays on the solver.
constexpr double MAX_OFFSET = 2097152.0;

//! What the sum counts: `direction * (number of selected rows) + n * offset`, where n is the number of rows the sum
//! covers in the group.
struct CountBody {
	//! +1 for `x`, `x + c`; -1 for `-x`, `c - x`.
	int direction = 1;
	//! The constant terms added up, in their own types; null when the body has none.
	unique_ptr<Expression> offset;
};

//! Reads a reducer body as `a*x + c`. The signed constant coefficients of `x` must add up to exactly +1 or -1, as the
//! solver's like-term collection sums them, and every other term must be a finite foldable constant of at most
//! MAX_OFFSET in magnitude. False with `problem` filled otherwise.
bool ReadCountBody(ClientContext &context, const vector<DecideSplitTerm> &terms, CountBody &body, string &problem) {
	const string not_a_count = "expected SUM(x) with unit contribution";
	double slope = 0;
	bool has_decision = false;
	vector<const DecideSplitTerm *> constants;
	for (auto &term : terms) {
		if (term.kind == DecideTermKind::CONSTANT) {
			constants.push_back(&term);
			continue;
		}
		double value;
		if (term.kind != DecideTermKind::LINEAR || term.var_a != 0 || !term.coefficient ||
		    !TryEvaluateFoldableDouble(context, *term.coefficient, value, DIRECT_FINITE_CONSTANT)) {
			problem = not_a_count;
			return false;
		}
		slope += term.sign * value;
		has_decision = true;
	}
	if (!has_decision || (slope != 1.0 && slope != -1.0)) {
		problem = not_a_count;
		return false;
	}
	body.direction = slope > 0 ? 1 : -1;
	for (auto term : constants) {
		double value;
		if (!term->coefficient || !term->coefficient->return_type.IsNumeric() ||
		    !TryEvaluateFoldableDouble(context, *term->coefficient, value, DIRECT_FINITE_CONSTANT)) {
			problem = "a term added to x in the count must be a finite constant, not a column or NULL";
			return false;
		}
		if (std::fabs(value) > MAX_OFFSET) {
			problem = "a constant added to x in the count is larger than 2097152";
			return false;
		}
		auto copy = term->coefficient->Copy();
		if (!body.offset) {
			if (term->sign < 0) {
				vector<unique_ptr<Expression>> negate;
				negate.push_back(std::move(copy));
				copy = DecideRebindOperator(context, "-", std::move(negate));
			}
			body.offset = std::move(copy);
		} else {
			vector<unique_ptr<Expression>> operands;
			operands.push_back(std::move(body.offset));
			operands.push_back(std::move(copy));
			body.offset = DecideRebindOperator(context, term->sign < 0 ? "-" : "+", std::move(operands));
		}
	}
	return true;
}

//! The comparison that holds between `-a` and `-b` when `comparison` holds between `a` and `b`.
ExpressionType FlipComparison(ExpressionType comparison) {
	switch (comparison) {
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		return ExpressionType::COMPARE_GREATERTHANOREQUALTO;
	case ExpressionType::COMPARE_LESSTHAN:
		return ExpressionType::COMPARE_GREATERTHAN;
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		return ExpressionType::COMPARE_LESSTHANOREQUALTO;
	case ExpressionType::COMPARE_GREATERTHAN:
		return ExpressionType::COMPARE_LESSTHAN;
	default:
		return comparison;
	}
}

//! The decision's terms on a per-row left side, as in a fixed value `x = 1`; null otherwise. CheckCondition checks that
//! they add up to exactly one `x`.
const vector<DecideSplitTerm> *FixedTerms(const DirectIRConstraint &ir_node) {
	if (ir_node.aggregate || ir_node.comparison == ExpressionType::COMPARE_IN || ir_node.lhs.size() != 1) {
		return nullptr;
	}
	auto &part = ir_node.lhs[0];
	if (part.sign != 1 || part.reducer != DirectReducer::NONE || part.scale || !OnlyDecisionTerms(part.terms)) {
		return nullptr;
	}
	return &part.terms;
}

//! Turns `SUM(x) <comparison> expr` (`SUM(x) <comparison> -expr` when `negate`) into an inclusive count limit in
//! [0, 2^53]. A bound below zero on an upper side is infeasible; one at or below zero on a lower side is no bound.
//! False when `expr` is not an exactly representable finite constant.
bool NormalizeCapacity(ClientContext &context, const Expression &expr, ExpressionType comparison, idx_t &capacity,
                       bool &impossible, bool negate = false) {
	double as_double;
	if (!TryEvaluateFoldableDouble(context, expr, as_double, DIRECT_FINITE_CONSTANT)) {
		return false;
	}
	if (negate) {
		as_double = -as_double;
	}
	double inclusive;
	switch (comparison) {
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		inclusive = std::ceil(as_double);
		break;
	case ExpressionType::COMPARE_GREATERTHAN:
		if (as_double >= DECIDE_MAX_EXACT_INTEGER) {
			return false;
		}
		inclusive = DecideStrictBound(true, as_double);
		break;
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		inclusive = std::floor(as_double);
		break;
	case ExpressionType::COMPARE_LESSTHAN:
		inclusive = DecideStrictBound(false, as_double);
		break;
	default:
		return false;
	}
	if (DirectIsLowerBound(comparison)) {
		if (inclusive <= 0.0) {
			capacity = 0;
			return true;
		}
	} else if (inclusive < 0.0) {
		impossible = true;
		capacity = 0;
		return true;
	}
	if (!DecideIsWithinExactRange(inclusive)) {
		return false;
	}
	capacity = static_cast<idx_t>(inclusive);
	return true;
}

//! Which of the values 0 and 1 a per-row bound on `x` leaves allowed. Neither allowed means no row it covers can
//! satisfy it.
struct FixedDomain {
	bool zero = true;
	bool one = true;
};

bool SatisfiesComparison(ExpressionType comparison, double value, double bound) {
	switch (comparison) {
	case ExpressionType::COMPARE_EQUAL:
		return value == bound;
	case ExpressionType::COMPARE_NOTEQUAL:
		return value != bound;
	case ExpressionType::COMPARE_LESSTHAN:
		return value < bound;
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		return value <= bound;
	case ExpressionType::COMPARE_GREATERTHAN:
		return value > bound;
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		return value >= bound;
	default:
		throw InternalException("S1 direct solve received a fixed-value comparison it did not match");
	}
}

//! Reads `x <comparison> bound` as the values of {0, 1} it allows.
//!
//! The solver reads a bound with a WHEN as a per-row constraint and applies plain arithmetic to it. A bound without a
//! WHEN is a bound on the variable itself. The solver seeds the box of a Boolean at [0, 1] and a written bound only
//! narrows it, so a bound that always holds (`x >= -3`, `x > -3`) changes nothing and reads here as both values
//! allowed, which is what comparing 0 and 1 against it gives. A bound that cannot hold makes the solver raise DECIDE's
//! usual infeasible error before it reads a row; that early error is not S1's to reproduce, so false for those. A
//! strict bound is shifted to a whole number first, with the solver's own function, so `x > 0.5` is the lower bound 1
//! and `x < 1.5` the upper bound 1 here as there; for a variable that is 0 or 1 that is the same as the comparison
//! itself. Whether the box contradicts the built-in domain of a Boolean is the solver's own test
//! (DecideBoxContradictsDomain).
bool ConstantFixedDomain(const DirectIRConstraint &ir_node, double bound, FixedDomain &domain) {
	auto comparison = ir_node.comparison;
	if (!ir_node.scope.when && comparison != ExpressionType::COMPARE_NOTEQUAL) {
		bool has_lower = comparison == ExpressionType::COMPARE_GREATERTHAN ||
		                 comparison == ExpressionType::COMPARE_GREATERTHANOREQUALTO ||
		                 comparison == ExpressionType::COMPARE_EQUAL;
		bool has_upper = comparison == ExpressionType::COMPARE_LESSTHAN ||
		                 comparison == ExpressionType::COMPARE_LESSTHANOREQUALTO ||
		                 comparison == ExpressionType::COMPARE_EQUAL;
		double lower = comparison == ExpressionType::COMPARE_GREATERTHAN ? DecideStrictBound(true, bound) : bound;
		double upper = comparison == ExpressionType::COMPARE_LESSTHAN ? DecideStrictBound(false, bound) : bound;
		DecideBoundBox box;
		if (has_lower) {
			DecideFoldSingletonRow(box, '>', lower);
		}
		if (has_upper) {
			DecideFoldSingletonRow(box, '<', upper);
		}
		if (DecideBoxContradictsDomain(box, true)) {
			return false;
		}
	}
	domain.zero = SatisfiesComparison(comparison, 0.0, bound);
	domain.one = SatisfiesComparison(comparison, 1.0, bound);
	return true;
}

const Expression *SourceBooleanFixedValue(const Expression &expr, idx_t decide_index,
                                          const vector<ColumnBinding> &source_bindings) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_CAST) {
		return nullptr;
	}
	auto &cast = expr.Cast<BoundCastExpression>();
	if (cast.child->return_type != LogicalType::BOOLEAN || !expr.return_type.IsNumeric() ||
	    DirectMayThrow(*cast.child) || !DirectIsDecisionFreeDeterministic(*cast.child, decide_index) ||
	    !DirectReferencesOnlySource(*cast.child, source_bindings) || !DirectHasColumnReference(*cast.child)) {
		return nullptr;
	}
	return cast.child.get();
}

//! The membership filter of one bound factor: the aggregate-local WHEN on `SUM(x)` or the clause-level WHEN, never
//! both. Null when the bound has none; `ok` is false when the factor is not a plain `SUM(x)` with one filter.
const Expression *BoundMembership(const DirectIRConstraint &ir_node, bool &ok) {
	auto sum = ir_node.PlainSum(true);
	ok = sum && !(sum->filter && ir_node.scope.when);
	if (!ok) {
		return nullptr;
	}
	return sum->filter ? sum->filter.get() : ir_node.scope.when.get();
}

//! `COUNT(*)` with nothing attached: no DISTINCT, ORDER BY, FILTER or qualifier. A FILTER on the right-hand count
//! scopes the count apart from the left side (`SUM(x) <= COUNT(*) WHEN active` counts the active rows while the left
//! side sums every row), which this rule does not model.
bool IsPlainCountStar(const Expression &expr) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
		return false;
	}
	auto &aggregate = expr.Cast<BoundAggregateExpression>();
	idx_t qualifier;
	return aggregate.function.name == "count_star" && aggregate.children.empty() && !aggregate.filter &&
	       !aggregate.IsDistinct() && !aggregate.order_bys &&
	       !TryParseQualifiedReducerTag(aggregate.GetAlias(), qualifier);
}

//! The column types whose DOUBLE running sum is always exact (|sum| < 2^53 for any realistic row count), so that the
//! solver's row-by-row DOUBLE sum equals the exact sum a window computes. Wider integers, decimals and floating point
//! types are not: their sums depend on the order and precision of the addition, so SUM and AVG over them stay on the
//! solver.
bool IsSmallIntegerType(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
		return true;
	default:
		return false;
	}
}

//! Why an aggregate on the right of a count bound is not one this rule reads, or empty when it is: plain `COUNT(*)`,
//! or COUNT, MIN or MAX of one numeric source column, or SUM or AVG of one small integer source column, with no WHEN
//! of its own, DISTINCT, ORDER BY or relation qualifier. Fills `reducer` when it is one.
string ReducerProblem(const Expression &expr, const vector<ColumnBinding> &source_bindings, Reducer &reducer) {
	if (IsPlainCountStar(expr)) {
		reducer = Reducer();
		reducer.function = "count_star";
		return string();
	}
	auto &aggregate = expr.Cast<BoundAggregateExpression>();
	idx_t qualifier;
	if (TryParseQualifiedReducerTag(aggregate.GetAlias(), qualifier)) {
		return "a relation-qualified aggregate";
	}
	if (aggregate.filter) {
		return "a WHEN on the right-hand aggregate";
	}
	if (aggregate.IsDistinct() || aggregate.order_bys) {
		return "DISTINCT or ORDER BY in an aggregate";
	}
	auto &name = aggregate.function.name;
	if ((name != "count" && name != "min" && name != "max" && name != "sum" && name != "avg") ||
	    aggregate.children.size() != 1) {
		return "an aggregate other than COUNT(*), COUNT, MIN, MAX, SUM or AVG";
	}
	reducer = Reducer();
	reducer.function = name;
	if (!DirectSourceNumericColumn(*aggregate.children[0], source_bindings, reducer.source_slot, reducer.column_type,
	                               reducer.column_name)) {
		return "an aggregate over something other than a numeric column";
	}
	if ((name == "sum" || name == "avg") && !IsSmallIntegerType(reducer.column_type)) {
		return "SUM or AVG of a column that is not TINYINT, SMALLINT, UTINYINT or USMALLINT";
	}
	return string();
}

//! Why a bound is not a group-aggregate expression, or empty when it is one: the aggregates `ReducerProblem` admits,
//! finite exact constants, numeric casts that cannot throw, and `+ - * / //` over them, where a divisor is a nonzero
//! constant. The plan evaluates such a bound with DuckDB's own operators, on the rows the solver evaluates it for (see
//! `PlanBuilder::OnlyWhereCovered`), so an overflow raises on both paths or on neither and no range is proved here.
//! Appends the aggregates it meets to `reducers`, in walk order.
string RowCountBoundProblem(ClientContext &context, const Expression &expr,
                            const vector<ColumnBinding> &source_bindings, vector<Reducer> &reducers) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
		Reducer reducer;
		auto problem = ReducerProblem(expr, source_bindings, reducer);
		if (problem.empty()) {
			reducers.push_back(std::move(reducer));
		}
		return problem;
	}
	double constant;
	if (expr.IsFoldable() && !expr.IsAggregate()) {
		return TryEvaluateFoldableDouble(context, expr, constant, DIRECT_FINITE_CONSTANT)
		           ? string()
		           : "a constant that is not finite or not exact";
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
		auto &cast = expr.Cast<BoundCastExpression>();
		if (!expr.return_type.IsNumeric() || !cast.child->return_type.IsNumeric()) {
			return "a cast that is not numeric";
		}
		auto problem = RowCountBoundProblem(context, *cast.child, source_bindings, reducers);
		if (!problem.empty()) {
			return problem;
		}
		return DirectCastMayThrow(cast) ? "a narrowing cast" : string();
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		auto &function = expr.Cast<BoundFunctionExpression>();
		auto &name = function.function.name;
		bool division = name == "/" || name == "//";
		bool negation = name == "-" && function.children.size() == 1;
		if (!negation && (function.children.size() != 2 || (!division && name != "+" && name != "-" && name != "*"))) {
			return "the function " + name;
		}
		for (auto &child : function.children) {
			auto problem = RowCountBoundProblem(context, *child, source_bindings, reducers);
			if (!problem.empty()) {
				return problem;
			}
		}
		if (division && (!TryEvaluateFoldableDouble(context, *function.children[1], constant, DIRECT_FINITE_CONSTANT) ||
		                 constant == 0.0)) {
			return "a divisor that is not a nonzero constant";
		}
		return string();
	}
	return "an expression other than group aggregates, constants and arithmetic";
}

class Rule final : public DirectSolveRule {
public:
	const char *Name() const override {
		return RULE_NAME;
	}
	unique_ptr<DirectApplicabilityCondition> CheckCondition(const DirectIR &ir, ClientContext &context,
	                                                        string &reason) const override {
		if (ir.decisions_status != DirectAnalysisStatus::PROVED || ir.decisions.size() != 1 ||
		    ir.decisions[0].domain != DirectDomain::BOOL || ir.decisions[0].scope != DecideVarScope::ROW ||
		    ir.decisions[0].output_type != LogicalType::INTEGER || !ir.entity_scopes.empty() ||
		    ir.source_status != DirectAnalysisStatus::PROVED) {
			reason = "variable_shape: expected one row-scoped BOOL";
			return nullptr;
		}
		// No objective (`DecideSense::FEASIBILITY`) is admitted: every score is 0.
		if (!std::isfinite(ir.objective.offset) ||
		    (ir.objective.sense != DecideSense::MAXIMIZE && ir.objective.sense != DecideSense::MINIMIZE &&
		     ir.objective.sense != DecideSense::FEASIBILITY)) {
			reason = "problem_shape: expected a linear objective or no objective, and cardinality bounds";
			return nullptr;
		}
		if (ir.constraints_status != DirectAnalysisStatus::PROVED) {
			reason = "constraints_not_proved: " + ir.constraints_reason;
			return nullptr;
		}
		if (ir.constraints.empty() || ir.source_clause_count != ir.constraints.size()) {
			reason = "constraint_shape: expected attributed cardinality bounds and optional fixed Boolean values";
			return nullptr;
		}
		auto condition = make_uniq<Condition>();
		vector<uint8_t> seen_sources(ir.source_clause_count, 0);
		for (auto &ir_node : ir.constraints) {
			if (ir_node.source_clause_id >= seen_sources.size() || seen_sources[ir_node.source_clause_id]++) {
				reason = "constraint_provenance: expected one distinct attributed source clause per factor";
				return nullptr;
			}
			if (ir_node.status != DirectAnalysisStatus::PROVED) {
				reason = "constraints_not_proved: " + ir.FirstNotProvedReason();
				return nullptr;
			}
			if (ir_node.comparison == ExpressionType::COMPARE_IN) {
				reason = "constraint_shape: expected SUM(x) bounds or fixed Boolean values";
				return nullptr;
			}
			if (FixedTerms(ir_node)) {
				condition->local_fixes.push_back(&ir_node);
				continue;
			}
			if (!DirectIsLowerBound(ir_node.comparison) && !DirectIsUpperBound(ir_node.comparison)) {
				reason = "constraint_shape: expected SUM(x) cardinality bounds";
				return nullptr;
			}
			condition->bound_factors.push_back(&ir_node);
		}
		if (condition->bound_factors.empty()) {
			reason = "constraint_shape: expected a SUM(x) cardinality bound";
			return nullptr;
		}
		if (ir.objective.status != DirectAnalysisStatus::PROVED) {
			reason = "objective_not_proved: " + ir.objective.reason;
			return nullptr;
		}
		if (ir.objective.scope.when || !ir.objective.scope.per_key_slots.empty()) {
			reason = "objective_scope: expected an objective without WHEN or PER";
			return nullptr;
		}
		if (ir.objective.parts.empty() && ir.objective.sense != DecideSense::FEASIBILITY) {
			reason = "objective_shape: expected additive SUM(coefficient * x) terms";
			return nullptr;
		}
		condition->decide_index = ir.decide_index;
		condition->sense = ir.objective.sense;
		if (!ProveScope(ir, *condition, reason) || !ProveFixedVariables(ir, context, *condition, reason) ||
		    !ProveBounds(ir, context, *condition, reason) || !ProveObjective(ir, context, *condition, reason)) {
			return nullptr;
		}
		return std::move(condition);
	}
	string Summary() const override {
		return "min/max SUM(s*x), x in {0,1}, lo <= SUM(x) <= hi";
	}
	DirectSubstitute Apply(unique_ptr<LogicalOperator> source, Optimizer &optimizer,
	                       const DirectApplicabilityCondition &candidate) const override;

private:
	bool ProveScope(const DirectIR &ir, Condition &condition, string &reason) const;
	bool ProveFixedVariables(const DirectIR &ir, ClientContext &context, Condition &condition, string &reason) const;
	bool ProveBounds(const DirectIR &ir, ClientContext &context, Condition &condition, string &reason) const;
	bool ProveObjective(const DirectIR &ir, ClientContext &context, Condition &condition, string &reason) const;
};

//! Every bound counts the same rows: one `SUM(x)` per bound with the same PER keys and the same WHEN membership.
//! Records the group keys and membership the plan scopes by.
bool Rule::ProveScope(const DirectIR &ir, Condition &condition, string &reason) const {
	const auto &first_scope = *condition.bound_factors.front();
	bool ok;
	const Expression *first_when = BoundMembership(first_scope, ok);
	if (!ok) {
		reason = "constraint_shape: expected one SUM(x) membership filter";
		return false;
	}
	for (auto factor : condition.bound_factors) {
		const Expression *when = BoundMembership(*factor, ok);
		if (!ok) {
			reason = "constraint_shape: expected one SUM(x) membership filter";
			return false;
		}
		if (factor->scope.per_key_slots != first_scope.scope.per_key_slots ||
		    static_cast<bool>(when) != static_cast<bool>(first_when)) {
			reason = "constraint_scope: cardinality bounds use different PER or WHEN membership";
			return false;
		}
		if (when && !Expression::Equals(*when, *first_when)) {
			reason = "constraint_scope: cardinality bounds use different WHEN membership";
			return false;
		}
	}
	condition.group_key_slots = first_scope.scope.per_key_slots;
	if (first_when) {
		if (!DirectIsSourceOnlyPredicate(*first_when, ir.decide_index, ir.source_bindings)) {
			reason = "constraint_scope: WHEN must be a deterministic source-only predicate";
			return false;
		}
		condition.when_condition = first_when->Copy();
	}
	condition.scoped = !condition.group_key_slots.empty() || condition.when_condition != nullptr;
	return true;
}

//! Per-row fixed Boolean values: `x = 0/1` and its one-sided forms against a constant, or against a source Boolean.
//! Each becomes a condition under which the row is fixed to one or to zero.
bool Rule::ProveFixedVariables(const DirectIR &ir, ClientContext &context, Condition &condition, string &reason) const {
	for (auto factor : condition.local_fixes) {
		if (!factor->scope.per_key_slots.empty()) {
			reason = "constraint_scope: fixed Boolean values must be per-row without PER keys";
			return false;
		}
		if (!IsUnitContribution(context, *FixedTerms(*factor))) {
			reason = "constraint_shape: expected a fixed Boolean value on x itself";
			return false;
		}
		auto &when = factor->scope.when;
		if (when && !DirectIsSourceOnlyPredicate(*when, ir.decide_index, ir.source_bindings)) {
			reason = "constraint_scope: fixed Boolean value WHEN must be deterministic and source-only";
			return false;
		}
		double constant;
		if (TryEvaluateFoldableDouble(context, *factor->rhs, constant, DIRECT_FINITE_CONSTANT)) {
			FixedDomain domain;
			if (!ConstantFixedDomain(*factor, constant, domain)) {
				reason =
				    "constraint_shape: an unconditional bound on x that the solver reads differently from a Boolean "
				    "(negative lower bound, or a bound that cannot hold)";
				return false;
			}
			// Both values allowed is no constraint. Neither allowed fixes the row to both, which the plan reports as
			// DECIDE's infeasible error.
			if (!domain.zero || !domain.one) {
				auto row_condition = [&]() {
					return when ? when->Copy() : make_uniq<BoundConstantExpression>(Value::BOOLEAN(true));
				};
				if (!domain.zero) {
					condition.fixed_one_conditions.push_back(row_condition());
				}
				if (!domain.one) {
					condition.fixed_zero_conditions.push_back(row_condition());
				}
			}
			continue;
		}
		auto type = factor->comparison;
		auto active = [&](unique_ptr<Expression> value) -> unique_ptr<Expression> {
			return when ? make_uniq<BoundCaseExpression>(when->Copy(), std::move(value),
			                                             make_uniq<BoundConstantExpression>(Value::BOOLEAN(false)))
			            : std::move(value);
		};
		auto source_value = SourceBooleanFixedValue(*factor->rhs, ir.decide_index, ir.source_bindings);
		if (source_value &&
		    (type == ExpressionType::COMPARE_EQUAL || type == ExpressionType::COMPARE_LESSTHANOREQUALTO ||
		     type == ExpressionType::COMPARE_GREATERTHANOREQUALTO)) {
			if (type == ExpressionType::COMPARE_EQUAL || type == ExpressionType::COMPARE_GREATERTHANOREQUALTO) {
				condition.fixed_one_conditions.push_back(active(source_value->Copy()));
			}
			if (type == ExpressionType::COMPARE_EQUAL || type == ExpressionType::COMPARE_LESSTHANOREQUALTO) {
				condition.fixed_zero_conditions.push_back(active(
				    make_uniq<BoundComparisonExpression>(ExpressionType::COMPARE_EQUAL, source_value->Copy(),
				                                         make_uniq<BoundConstantExpression>(Value::BOOLEAN(false)))));
			}
			string column_name;
			if (source_value->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
				column_name = source_value->GetAlias();
			}
			condition.source_fixed_values.push_back(
			    {source_value->Copy(), column_name, "DECIDE per-row Boolean bound contains NULL", false});
			continue;
		}
		// A numeric value from the row: the row allows 0 when `0 <comparison> value` holds and 1 when `1 <comparison>
		// value` holds, as the solver reads it (with a WHEN or not), so the row is fixed to the one value it allows
		// and is infeasible when it allows neither.
		auto &value = *factor->rhs;
		if (value.IsFoldable() || !DirectIsSourceOnlyNumeric(value, ir.decide_index, ir.source_bindings)) {
			reason = "constraint_shape: a bound on x must be a finite constant, a source column, or a deterministic "
			         "nonthrowing numeric expression over source columns";
			return false;
		}
		idx_t column_slot;
		LogicalType column_type;
		string column_name;
		bool is_column = DirectSourceNumericColumn(value, ir.source_bindings, column_slot, column_type, column_name);
		auto invalid_type = is_column ? column_type : value.return_type;
		auto disallows = [&](double allowed_value) -> unique_ptr<Expression> {
			auto as_double = BoundCastExpression::AddCastToType(context, value.Copy(), LogicalType::DOUBLE);
			auto allowed = make_uniq<BoundComparisonExpression>(
			    type, make_uniq<BoundConstantExpression>(Value::DOUBLE(allowed_value)), std::move(as_double));
			return make_uniq<BoundCaseExpression>(std::move(allowed), DirectConstantBool(false),
			                                      DirectConstantBool(true));
		};
		condition.fixed_one_conditions.push_back(active(disallows(0.0)));
		condition.fixed_zero_conditions.push_back(active(disallows(1.0)));
		condition.source_fixed_values.push_back(
		    {value.Copy(), is_column ? column_name : string(), DirectInvalidBoundMessage(),
		     invalid_type == LogicalType::FLOAT || invalid_type == LogicalType::DOUBLE});
	}
	return true;
}

//! The count bounds: constant ones fold into one inclusive interval, source-valued ones are kept for the plan to
//! reduce per group. Every bound counts `x` once, as `x`, `x + c`, `-x` or `c - x`: `SUM(x + c) <op> B` is
//! `SUM(x) <op> B - n*c` and `SUM(c - x) <op> B` is `SUM(x) <flipped op> n*c - B`, where n is the number of rows the
//! SUM covers in the group. A constant bound without a constant term in the body folds here; every other bound is
//! shifted per group by the plan.
bool Rule::ProveBounds(const DirectIR &ir, ClientContext &context, Condition &condition, string &reason) const {
	auto fold_bound = [&](const Expression &bound, ExpressionType side, idx_t &target, bool negate) {
		bool impossible = false;
		if (!NormalizeCapacity(context, bound, side, target, impossible, negate)) {
			reason = "constraint_shape: cardinality bound must be a finite consistent foldable numeric expression "
			         "with an inclusive limit in [0, 2^53], or a deterministic nonthrowing source-only "
			         "numeric expression";
			return false;
		}
		condition.impossible |= impossible;
		return true;
	};
	for (auto factor : condition.bound_factors) {
		auto sum = factor->PlainSum(true);
		CountBody body;
		string problem;
		if (!sum || !ReadCountBody(context, sum->terms, body, problem)) {
			reason =
			    "constraint_shape: " + (problem.empty() ? string("expected SUM(x) with unit contribution") : problem);
			return false;
		}
		auto &bound = *factor->rhs;
		bool negated = body.direction < 0;
		auto type = negated ? FlipComparison(factor->comparison) : factor->comparison;
		idx_t source_slot = DConstants::INVALID_INDEX;
		LogicalType source_type;
		string source_name;
		bool source_column =
		    DirectSourceNumericColumn(bound, ir.source_bindings, source_slot, source_type, source_name);
		auto add_source_bound = [&](idx_t slot, const LogicalType &bound_type, string column_name,
		                            vector<Reducer> reducers) {
			bool counts_rows = !reducers.empty();
			Condition::SourceBound source_bound {
			    slot, bound_type, type, bound.Copy(), sum->filter != nullptr, std::move(column_name), counts_rows};
			for (auto &reducer : reducers) {
				source_bound.needs_count |=
				    reducer.function == "count_star" || reducer.function == "count" || reducer.function == "avg";
			}
			source_bound.reducers = std::move(reducers);
			source_bound.negated = negated;
			source_bound.offset = std::move(body.offset);
			condition.source_bounds.push_back(std::move(source_bound));
		};
		if (!bound.IsFoldable() && bound.IsAggregate()) {
			vector<Reducer> reducers;
			auto problem = RowCountBoundProblem(context, bound, ir.source_bindings, reducers);
			if (!problem.empty()) {
				reason = "constraint_shape: a bound with an aggregate must combine COUNT(*), COUNT, MIN or MAX of a "
				         "numeric column, or SUM or AVG of a small integer column, with constants by + - * / // (" +
				         problem + ")";
				return false;
			}
			add_source_bound(DConstants::INVALID_INDEX, bound.return_type, string(), std::move(reducers));
			continue;
		}
		// The plan evaluates a source-valued count bound on every row, as the solver does, so an expression that can
		// raise (an overflow in `COUNT(*) * k` over a window column, or in `cap * k`) raises on both paths or on neither.
		// A row no group covers reads 0 for a rewritten right-hand aggregate, so it cannot raise there.
		if (!bound.IsFoldable() &&
		    (source_column || DirectIsSourceOnlyNumericMayRaise(bound, ir.decide_index, ir.source_bindings))) {
			add_source_bound(source_slot, source_column ? source_type : bound.return_type,
			                 source_column ? source_name : string(), {});
			continue;
		}
		if (body.offset) {
			// The shift depends on the group's row count, so the constant bound is reduced per group like any other.
			double constant;
			if (!TryEvaluateFoldableDouble(context, bound, constant, DIRECT_FINITE_CONSTANT) ||
			    !DecideIsWithinExactRange(constant)) {
				reason = "constraint_shape: cardinality bound must be a finite consistent foldable numeric expression "
				         "with an inclusive limit in [0, 2^53], or a deterministic nonthrowing source-only "
				         "numeric expression";
				return false;
			}
			add_source_bound(DConstants::INVALID_INDEX, bound.return_type, string(), {});
			continue;
		}
		if (DirectIsLowerBound(type)) {
			idx_t lower;
			if (!fold_bound(bound,
			                type == ExpressionType::COMPARE_EQUAL ? ExpressionType::COMPARE_GREATERTHANOREQUALTO : type,
			                lower, negated)) {
				return false;
			}
			condition.lower = std::max(condition.lower, lower);
		}
		if (DirectIsUpperBound(type)) {
			idx_t upper;
			if (!fold_bound(bound,
			                type == ExpressionType::COMPARE_EQUAL ? ExpressionType::COMPARE_LESSTHANOREQUALTO : type,
			                upper, negated)) {
				return false;
			}
			condition.upper = condition.has_upper ? std::min(condition.upper, upper) : upper;
			condition.has_upper = true;
		}
	}
	if (condition.has_upper && condition.lower > condition.upper) {
		condition.impossible = true;
	}
	return true;
}

//! The objective: a signed sum of `SUM(coefficient * x)` terms. Each coefficient must be evaluable per source row.
bool Rule::ProveObjective(const DirectIR &ir, ClientContext &context, Condition &condition, string &reason) const {
	// Every linear term in x scores separately: `SUM((p + q) * x)` splits into `p * x` and `q * x`, and the score
	// adds the signed coefficients in order, as the solver's like-term collection does.
	auto &parts = ir.objective.parts;
	idx_t term_count = 0;
	for (auto &part : parts) {
		if (part.reducer != DirectReducer::SUM || part.filter || part.inner ||
		    part.qualifier != DConstants::INVALID_INDEX) {
			reason = "objective_shape: expected unfiltered SUM(coefficient * x) terms";
			return false;
		}
		for (auto &term : part.terms) {
			if (term.kind != DecideTermKind::LINEAR || term.var_a != 0) {
				reason = "objective_shape: expected a coefficient times x";
				return false;
			}
		}
		term_count += part.terms.size();
	}
	for (auto &part : parts) {
		// A query-wide factor on the reducer (`2 * SUM(...)`, `SUM(...) / 2`) scales every term of that part and no
		// other. It must be a finite constant, and a divisor must not be zero. Anything else stays on the solver.
		double scale;
		if (!DirectPartScale(context, part, scale)) {
			reason = "objective_shape: a factor on SUM(...) must be a finite constant (not zero when dividing)";
			return false;
		}
		for (auto &term : part.terms) {
			if (!DirectIsNumericDecisionFree(*term.coefficient, ir.decide_index)) {
				reason = "coefficient_shape: expected a deterministic numeric expression without decisions";
				return false;
			}
			if (!DirectReferencesOnlySource(*term.coefficient, ir.source_bindings) ||
			    (term_count > 1 && DirectMayThrow(*term.coefficient))) {
				reason = "coefficient_shape: multiple terms need nonthrowing source-only numeric coefficients";
				return false;
			}
			condition.objective_parts.push_back(
			    {part.sign * term.sign, term.coefficient->Copy(), part.scale != nullptr, scale, part.scale_divides});
		}
	}
	return true;
}

//! Builds the relational plan for one S1 problem whose condition holds. The constructor derives what every stage
//! shares; each stage takes the plan built so far and returns it extended. Data a later stage needs travels in explicit
//! structs.
//!
//!   ProjectScopeState -> GuardEmptyAggregate -> AddBoundValidation -> ProjectScore
//!     -> RankAndFilterFeasibility -> ProjectAssignment
class PlanBuilder {
public:
	PlanBuilder(const Condition &condition_p, Optimizer &optimizer_p, LogicalOperator &source)
	    : condition(condition_p), optimizer(optimizer_p), binder(optimizer_p.binder) {
		// The rule driver hands the input over with its types resolved.
		source_bindings = source.GetColumnBindings();
		source_types = source.types;
		if (source_bindings.size() != source_types.size()) {
			throw InternalException("Direct solve source bindings and types differ");
		}
		score_index = binder.GenerateTableIndex();
		window_index = binder.GenerateTableIndex();
		result_index = binder.GenerateTableIndex();
		has_fixes = !condition.fixed_one_conditions.empty() || !condition.fixed_zero_conditions.empty();
		for (auto &bound : condition.source_bounds) {
			source_lower |= DirectIsLowerBound(bound.comparison);
			source_upper |= DirectIsUpperBound(bound.comparison);
		}
		has_lower = condition.lower || source_lower;
		has_upper = condition.has_upper || source_upper;
		// Slots of the score projection, which every later stage reads. This is the one place that lays them out;
		// ProjectScore appends its columns in this order and checks each against it.
		idx_t next_slot = source_bindings.size();
		score_binding = ColumnBinding(score_index, next_slot++);
		if (condition.scoped) {
			eligible_binding = ColumnBinding(score_index, next_slot++);
		}
		if (has_fixes) {
			fixed_one_binding = ColumnBinding(score_index, next_slot++);
			fixed_zero_binding = ColumnBinding(score_index, next_slot++);
		}
	}

	DirectSubstitute Build(unique_ptr<LogicalOperator> source) const {
		ScopeBindings scope;
		source = ProjectScopeState(std::move(source), scope);
		source = GuardEmptyAggregate(std::move(source), scope);
		RowCounts counts;
		source = CountRows(std::move(source), scope, counts);
		BoundChecks bounds(condition.source_bounds.size());
		source = AddBoundValidation(std::move(source), scope, counts, bounds);
		source = ProjectScore(std::move(source), scope, bounds);
		auto ranked = RankAndFilterFeasibility(std::move(source), bounds);
		auto result = ProjectAssignment(std::move(ranked), bounds);

		DirectSubstitute substitute;
		substitute.prunable_decisions.push_back(1); // x is pure once score validation and ranking remain live
		// The rank window reads every row before it emits one, so the hidden rank is this plan's validation breaker.
		substitute.validated_columns.push_back(source_bindings.size() + 1);
		for (idx_t i = 0; i < source_bindings.size() + 1; i++) {
			substitute.slot_map.push_back(i);
		}
		substitute.child = std::move(result);
		return substitute;
	}

private:
	//! Where the source columns and per-row flags live once the optional state projection has run.
	struct ScopeBindings {
		vector<ColumnBinding> input_bindings;
		idx_t state_index = DConstants::INVALID_INDEX;
		ColumnBinding eligible_binding;
		ColumnBinding fixed_one_binding;
		ColumnBinding fixed_zero_binding;
	};

	//! Window slots of one source-valued count bound.
	struct SourceBoundSlots {
		idx_t min_window = DConstants::INVALID_INDEX;
		idx_t max_window = DConstants::INVALID_INDEX;
		//! Slots of the same extrema once the score projection forwards them.
		idx_t min_score = DConstants::INVALID_INDEX;
		idx_t max_score = DConstants::INVALID_INDEX;
	};

	//! Where the per-group row counts a `COUNT(*)` bound reads live.
	struct RowCounts {
		idx_t window_index = DConstants::INVALID_INDEX;
		//! The rows the clause covers in each group: the eligible ones.
		idx_t eligible_slot = DConstants::INVALID_INDEX;
		//! Every row of each group, for a bound whose left side has an aggregate-local WHEN.
		idx_t group_slot = DConstants::INVALID_INDEX;
		//! The window slot and type of each MIN, MAX or SUM a bound reads: (function, source column, reduced over every
		//! group row) -> slot.
		std::map<std::tuple<string, idx_t, bool>, std::pair<idx_t, LogicalType>> reducers;
		//! The rows the SUM itself covers in each group, whatever row reads it: the rows whose WHEN holds (the clause's
		//! or the aggregate-local one) and whose PER keys are not NULL. A bound shifted by a constant term reads it.
		idx_t covered_slot = DConstants::INVALID_INDEX;
	};

	//! The window the source-valued bounds were validated in, and the slots later stages read from it.
	struct BoundChecks {
		explicit BoundChecks(idx_t bound_count) : slots(bound_count) {
		}
		idx_t window_index = DConstants::INVALID_INDEX;
		vector<SourceBoundSlots> slots;
	};

	//! Ranked, feasibility-checked rows and the window slots the assignment reads.
	struct RankedRows {
		unique_ptr<LogicalOperator> plan;
		ColumnBinding rank_binding;
		idx_t fixed_count_slot = DConstants::INVALID_INDEX;
	};

	const Condition &condition;
	Optimizer &optimizer;
	Binder &binder;
	vector<ColumnBinding> source_bindings;
	vector<LogicalType> source_types;
	idx_t score_index;
	idx_t window_index;
	idx_t result_index;
	bool has_fixes = false;
	bool source_lower = false;
	bool source_upper = false;
	bool has_lower = false;
	bool has_upper = false;
	ColumnBinding score_binding;
	ColumnBinding eligible_binding;
	ColumnBinding fixed_one_binding;
	ColumnBinding fixed_zero_binding;

	//! Copy of a proved expression whose source references follow the state projection, when there is one.
	unique_ptr<Expression> CopyIntoState(const Expression &expr, const ScopeBindings &scope, const char *what) const {
		auto value = expr.Copy();
		if (scope.state_index != DConstants::INVALID_INDEX &&
		    !DirectRemapSourceReferences(*value, source_bindings, scope.state_index)) {
			throw InternalException(string("S1 direct solve could not remap its proved ") + what);
		}
		return value;
	}

	//! Stage 1: pass the source through and append the per-row scope flags (group eligibility, fixed values).
	unique_ptr<LogicalOperator> ProjectScopeState(unique_ptr<LogicalOperator> source, ScopeBindings &scope) const {
		vector<unique_ptr<Expression>> fixed_flags;
		if (has_fixes) {
			fixed_flags.push_back(AnyCondition(condition.fixed_one_conditions));
			fixed_flags.push_back(AnyCondition(condition.fixed_zero_conditions));
		}
		auto state = ProjectScope(binder, std::move(source), condition.scoped, condition.when_condition.get(),
		                          condition.group_key_slots, std::move(fixed_flags));
		scope.input_bindings = std::move(state.input_bindings);
		scope.state_index = state.state_index;
		scope.eligible_binding = state.eligible;
		if (has_fixes) {
			scope.fixed_one_binding = state.extra[0];
			scope.fixed_zero_binding = state.extra[1];
		}
		return std::move(state.plan);
	}

	//! Stage 2: a scoped aggregate over no eligible row is DECIDE's empty-aggregate error.
	unique_ptr<LogicalOperator> GuardEmptyAggregate(unique_ptr<LogicalOperator> source,
	                                                const ScopeBindings &scope) const {
		if (!condition.scoped) {
			return source;
		}
		return DirectGuardEmptyAggregate(optimizer, std::move(source), scope.eligible_binding);
	}

	//! Stage 2b: the row counts the bounds read. `COUNT(*)` counts the rows the clause covers in each group, so a WHEN
	//! or a NULL PER key narrows it. When the left side's WHEN is aggregate-local, the right side is not narrowed by it
	//! and counts every row of the group. A bound shifted by a constant term in the count body (`SUM(x + c)`) needs the
	//! number of rows the SUM itself covers, which an aggregate-local WHEN does narrow. Nothing is added when no bound
	//! reads a count.
	unique_ptr<LogicalOperator> CountRows(unique_ptr<LogicalOperator> source, const ScopeBindings &scope,
	                                      RowCounts &counts) const {
		bool eligible_needed = false;
		bool group_needed = false;
		bool covered_needed = false;
		bool reducers_needed = false;
		for (auto &bound : condition.source_bounds) {
			if (bound.needs_count) {
				(bound.rhs_all_group_rows ? group_needed : eligible_needed) = true;
			}
			covered_needed |= bound.offset != nullptr;
			for (auto &reducer : bound.reducers) {
				reducers_needed |= reducer.function == "min" || reducer.function == "max" ||
				                   reducer.function == "sum" || reducer.function == "avg";
			}
		}
		if (!eligible_needed && !group_needed && !covered_needed && !reducers_needed) {
			return source;
		}
		counts.window_index = binder.GenerateTableIndex();
		auto window = make_uniq<LogicalWindow>(counts.window_index);
		auto add_count = [&](bool narrowed_by_eligibility) {
			auto count =
			    make_uniq<BoundWindowExpression>(ExpressionType::WINDOW_AGGREGATE, LogicalType::BIGINT,
			                                     make_uniq<AggregateFunction>(CountStarFun::GetFunction()), nullptr);
			count->start = WindowBoundary::UNBOUNDED_PRECEDING;
			count->end = WindowBoundary::UNBOUNDED_FOLLOWING;
			if (narrowed_by_eligibility && condition.scoped) {
				count->partitions.push_back(DirectColumn(LogicalType::BOOLEAN, scope.eligible_binding));
			}
			for (auto slot : condition.group_key_slots) {
				count->partitions.push_back(DirectColumn(source_types[slot], scope.input_bindings[slot]));
			}
			window->expressions.push_back(std::move(count));
			return window->expressions.size() - 1;
		};
		if (eligible_needed) {
			counts.eligible_slot = add_count(true);
		}
		if (group_needed) {
			counts.group_slot = add_count(false);
		}
		if (reducers_needed) {
			// MIN, MAX and SUM of a column over the same group the counts cover. AVG is its SUM over the group's count.
			for (auto &bound : condition.source_bounds) {
				for (auto &reducer : bound.reducers) {
					bool sum = reducer.function == "sum" || reducer.function == "avg";
					bool extremum = reducer.function == "min" || reducer.function == "max";
					if (!sum && !extremum) {
						continue;
					}
					auto function = sum ? string("sum") : reducer.function;
					auto key = std::make_tuple(function, reducer.source_slot, bound.rhs_all_group_rows);
					if (counts.reducers.count(key)) {
						continue;
					}
					auto value =
					    DirectColumn(source_types[reducer.source_slot], scope.input_bindings[reducer.source_slot]);
					auto reduced = WindowExtremum(optimizer, function.c_str(), std::move(value));
					if (!bound.rhs_all_group_rows && condition.scoped) {
						reduced->partitions.push_back(DirectColumn(LogicalType::BOOLEAN, scope.eligible_binding));
					}
					for (auto slot : condition.group_key_slots) {
						reduced->partitions.push_back(DirectColumn(source_types[slot], scope.input_bindings[slot]));
					}
					counts.reducers[key] = {window->expressions.size(), reduced->return_type};
					window->expressions.push_back(std::move(reduced));
				}
			}
		}
		if (covered_needed) {
			// Counted over the whole group, so a row the SUM does not cover (and a bound reduced over every group row)
			// reads the same n as the rows it does cover.
			if (!condition.scoped) {
				counts.covered_slot = add_count(false);
			} else {
				auto covered = DirectWindowMatchingCount(DirectColumn(LogicalType::BOOLEAN, scope.eligible_binding));
				for (auto slot : condition.group_key_slots) {
					covered->partitions.push_back(DirectColumn(source_types[slot], scope.input_bindings[slot]));
				}
				counts.covered_slot = window->expressions.size();
				window->expressions.push_back(std::move(covered));
			}
		}
		window->children.push_back(std::move(source));
		return std::move(window);
	}

	//! The bound with each aggregate replaced by its value over the clause's group. The solver reads a reducer as a
	//! DOUBLE and hands it back to the surrounding expression as the aggregate's own type, so the same two casts are
	//! made here. AVG is the DOUBLE sum over the DOUBLE count. `next` walks `bound.reducers`, which the condition
	//! listed in the same order.
	unique_ptr<Expression> ReplaceAggregates(unique_ptr<Expression> expr, const Condition::SourceBound &bound,
	                                         const RowCounts &counts, idx_t &next) const {
		if (expr->GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
			ExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<Expression> &child) {
				child = ReplaceAggregates(std::move(child), bound, counts, next);
			});
			return expr;
		}
		if (next >= bound.reducers.size() ||
		    bound.reducers[next].function != expr->Cast<BoundAggregateExpression>().function.name) {
			throw InternalException("S1 direct solve met an aggregate it did not prove");
		}
		auto &reducer = bound.reducers[next++];
		auto type = expr->return_type;
		auto count_slot = bound.rhs_all_group_rows ? counts.group_slot : counts.eligible_slot;
		auto count = [&]() {
			return DirectColumn(LogicalType::BIGINT, ColumnBinding(counts.window_index, count_slot));
		};
		if (reducer.function == "count_star" || reducer.function == "count") {
			return count();
		}
		bool average = reducer.function == "avg";
		auto found = counts.reducers.find(
		    std::make_tuple(average ? string("sum") : reducer.function, reducer.source_slot, bound.rhs_all_group_rows));
		if (found == counts.reducers.end()) {
			throw InternalException("S1 direct solve lost the window of an aggregate it proved");
		}
		auto reduced = BoundCastExpression::AddCastToType(
		    optimizer.context,
		    DirectColumn(found->second.second, ColumnBinding(counts.window_index, found->second.first)),
		    LogicalType::DOUBLE);
		if (average) {
			return optimizer.BindScalarFunction(
			    "/", std::move(reduced),
			    BoundCastExpression::AddCastToType(optimizer.context, count(), LogicalType::DOUBLE));
		}
		return BoundCastExpression::AddCastToType(optimizer.context, std::move(reduced), type);
	}

	//! The solver evaluates a count bound once per group that has a row the clause covers: a row whose WHEN holds and
	//! whose PER keys are not NULL. The window that reduces the bound reads every row, so a bound that can raise (an
	//! overflow in `COUNT(*) * k`) would be evaluated for groups the solver never evaluates it for. The bound is
	//! therefore read as the harmless 0 on a row that is not eligible, and the extremum ignores those rows (the
	//! caller leaves `all_group_rows` unset for a count bound, which has one value per group).
	unique_ptr<Expression> OnlyWhereCovered(unique_ptr<Expression> value, const ScopeBindings &scope) const {
		if (!condition.scoped) {
			return value;
		}
		return make_uniq<BoundCaseExpression>(
		    DirectColumn(LogicalType::BOOLEAN, scope.eligible_binding),
		    BoundCastExpression::AddCastToType(optimizer.context, std::move(value), LogicalType::DOUBLE),
		    make_uniq<BoundConstantExpression>(Value::DOUBLE(0.0)));
	}

	//! Stage 3: validate source-valued bounds and fixed values on every row, and reduce each bound to its group
	//! extremum.
	unique_ptr<LogicalOperator> AddBoundValidation(unique_ptr<LogicalOperator> source, const ScopeBindings &scope,
	                                               const RowCounts &counts, BoundChecks &bounds) const {
		vector<BoundSpec> specs;
		for (auto &bound : condition.source_bounds) {
			if (!bound.value ||
			    (bound.source_slot != DConstants::INVALID_INDEX &&
			     (bound.source_slot >= source_types.size() || source_types[bound.source_slot] != bound.source_type))) {
				throw InternalException("S1 direct solve received an invalid source bound slot");
			}
			BoundSpec spec;
			spec.comparison = bound.comparison;
			spec.value = CopyIntoState(*bound.value, scope, "bound");
			if (bound.counts_rows) {
				idx_t next = 0;
				spec.value = ReplaceAggregates(std::move(spec.value), bound, counts, next);
				if (next != bound.reducers.size()) {
					throw InternalException("S1 direct solve found a different number of aggregates than it proved");
				}
			}
			if (bound.offset) {
				// `SUM(x + c) <op> B` is `SUM(x) <op> B - n*c`; `SUM(c - x) <op> B` is `SUM(x) <flipped op> n*c - B`.
				// Evaluated in DuckDB's own types so that a decimal offset keeps its exact value.
				auto covered =
				    DirectColumn(LogicalType::BIGINT, ColumnBinding(counts.window_index, counts.covered_slot));
				auto shift = optimizer.BindScalarFunction("*", std::move(covered), bound.offset->Copy());
				// An integer bound next to an integer shift is subtracted in HUGEINT, so a bound near the BIGINT limit
				// cannot overflow where the solver (which subtracts in DOUBLE) does not.
				auto widen = [&](unique_ptr<Expression> value) {
					return value->return_type.IsIntegral() && value->return_type.id() != LogicalTypeId::UHUGEINT
					           ? BoundCastExpression::AddCastToType(optimizer.context, std::move(value),
					                                                LogicalType::HUGEINT)
					           : std::move(value);
				};
				shift = widen(std::move(shift));
				spec.value = widen(std::move(spec.value));
				spec.value = bound.negated ? optimizer.BindScalarFunction("-", std::move(shift), std::move(spec.value))
				                           : optimizer.BindScalarFunction("-", std::move(spec.value), std::move(shift));
			} else if (bound.negated) {
				spec.value = DirectScaledTerm(optimizer, std::move(spec.value), -1, false, 1.0, false);
			}
			if (bound.counts_rows) {
				spec.value = OnlyWhereCovered(std::move(spec.value), scope);
			}
			if (bound.source_slot != DConstants::INVALID_INDEX) {
				spec.is_column = true;
				spec.column = scope.input_bindings[bound.source_slot];
				spec.column_type = bound.source_type;
			}
			spec.all_group_rows = bound.rhs_all_group_rows && !bound.counts_rows;
			if (bound.column_name.empty()) {
				spec.invalid_message = DirectInvalidBoundMessage();
			} else {
				// The same column on every row, so the solver's message is known before any row is read.
				spec.null_message = DirectBoundNullMessage(bound.column_name);
				if (bound.source_type == LogicalType::FLOAT || bound.source_type == LogicalType::DOUBLE) {
					spec.nan_message = DirectBoundNaNMessage();
				}
			}
			specs.push_back(std::move(spec));
		}
		vector<NotNullSpec> not_null;
		for (auto &fixed : condition.source_fixed_values) {
			NotNullSpec spec;
			spec.value = CopyIntoState(*fixed.value, scope, "per-row fixed value");
			spec.reject_nan = fixed.reject_nan;
			if (fixed.column_name.empty()) {
				spec.message = fixed.invalid_message;
			} else {
				spec.message = DirectBoundNullMessage(fixed.column_name);
				if (fixed.reject_nan) {
					spec.nan_message = DirectBoundNaNMessage();
				}
			}
			not_null.push_back(std::move(spec));
		}
		// A column an aggregate on the right reduces is read on every row, as the solver reads it: NULL and NaN raise
		// on every row, including rows the clause does not cover; infinity is a value.
		vector<idx_t> checked_columns;
		for (auto &bound : condition.source_bounds) {
			for (auto &reducer : bound.reducers) {
				if (reducer.source_slot == DConstants::INVALID_INDEX ||
				    std::find(checked_columns.begin(), checked_columns.end(), reducer.source_slot) !=
				        checked_columns.end()) {
					continue;
				}
				checked_columns.push_back(reducer.source_slot);
				auto type = source_types[reducer.source_slot];
				NotNullSpec spec;
				spec.value = DirectColumn(type, scope.input_bindings[reducer.source_slot]);
				spec.reject_nan = type == LogicalType::FLOAT || type == LogicalType::DOUBLE;
				spec.message = DirectBoundNullMessage(reducer.column_name);
				if (spec.reject_nan) {
					spec.nan_message = DirectBoundNaNMessage();
				}
				not_null.push_back(std::move(spec));
			}
		}
		vector<ColumnBinding> keys;
		vector<LogicalType> key_types;
		for (auto slot : condition.group_key_slots) {
			keys.push_back(scope.input_bindings[slot]);
			key_types.push_back(source_types[slot]);
		}
		auto validation = ValidateBounds(optimizer, std::move(source), specs, not_null,
		                                 condition.scoped ? &scope.eligible_binding : nullptr, keys, key_types);
		bounds.window_index = validation.window_index;
		for (idx_t i = 0; i < bounds.slots.size(); i++) {
			bounds.slots[i].min_window = validation.min_slots[i];
			bounds.slots[i].max_window = validation.max_slots[i];
		}
		return std::move(validation.plan);
	}

	//! Stage 4: the score projection. It forwards the source columns, the combined linear score, the scope and
	//! fixed-value flags, and the per-group bound extrema, so every later stage reads one projection. The constructor
	//! laid out the score and flag slots; each is appended here at exactly that position.
	unique_ptr<LogicalOperator> ProjectScore(unique_ptr<LogicalOperator> source, const ScopeBindings &scope,
	                                         BoundChecks &bounds) const {
		vector<unique_ptr<Expression>> score_expressions;
		auto append_at = [&](const ColumnBinding &slot, unique_ptr<Expression> expression) {
			if (slot.table_index != score_index || slot.column_index != score_expressions.size()) {
				throw InternalException("S1 direct solve score projection does not match its layout");
			}
			score_expressions.push_back(std::move(expression));
		};
		for (idx_t i = 0; i < source_bindings.size(); i++) {
			score_expressions.push_back(DirectColumn(source_types[i], scope.input_bindings[i]));
		}
		unique_ptr<Expression> combined_score;
		// Without an objective there are no parts and every row scores 0.
		if (condition.objective_parts.size() != 1 || condition.objective_parts[0].sign < 0) {
			combined_score = make_uniq<BoundConstantExpression>(Value::DOUBLE(0.0));
		}
		for (auto &part : condition.objective_parts) {
			auto coefficient = CopyIntoState(*part.coefficient, scope, "coefficient");
			auto term =
			    DirectScaledTerm(optimizer, std::move(coefficient), part.sign, part.scaled, part.scale, part.divides);
			combined_score = combined_score
			                     ? optimizer.BindScalarFunction("+", std::move(combined_score), std::move(term))
			                     : std::move(term);
		}
		append_at(score_binding, std::move(combined_score));
		if (condition.scoped) {
			append_at(eligible_binding, DirectColumn(LogicalType::BOOLEAN, scope.eligible_binding));
		}
		if (has_fixes) {
			append_at(fixed_one_binding, DirectColumn(LogicalType::BOOLEAN, scope.fixed_one_binding));
			append_at(fixed_zero_binding, DirectColumn(LogicalType::BOOLEAN, scope.fixed_zero_binding));
		}
		for (idx_t i = 0; i < bounds.slots.size(); i++) {
			auto &slots = bounds.slots[i];
			if (slots.min_window != DConstants::INVALID_INDEX) {
				slots.min_score = score_expressions.size();
				score_expressions.push_back(
				    DirectColumn(LogicalType::DOUBLE, ColumnBinding(bounds.window_index, slots.min_window)));
			}
			if (slots.max_window != DConstants::INVALID_INDEX) {
				slots.max_score = score_expressions.size();
				score_expressions.push_back(
				    DirectColumn(LogicalType::DOUBLE, ColumnBinding(bounds.window_index, slots.max_window)));
			}
		}
		auto score = make_uniq<LogicalProjection>(score_index, std::move(score_expressions));
		score->children.push_back(std::move(source));
		return std::move(score);
	}

	//! Inclusive count limit of one source-valued bound, as BIGINT: its group extremum rounded, clamped to the
	//! BIGINT range, and -1 (upper) or 0 (lower) when the value is below zero.
	unique_ptr<Expression> SourceLimit(idx_t index, bool upper, const BoundChecks &bounds) const {
		auto &bound = condition.source_bounds[index];
		auto &slots = bounds.slots[index];
		auto slot = upper ? slots.min_score : slots.max_score;
		if (slot == DConstants::INVALID_INDEX) {
			throw InternalException("S1 direct solve lost its source-valued count limit");
		}
		auto cap = [&]() {
			return DirectColumn(LogicalType::DOUBLE, ColumnBinding(score_index, slot));
		};
		bool strict =
		    bound.comparison == (upper ? ExpressionType::COMPARE_LESSTHAN : ExpressionType::COMPARE_GREATERTHAN);
		auto rounded = optimizer.BindScalarFunction(upper == strict ? "ceil" : "floor", cap());
		unique_ptr<Expression> inclusive =
		    BoundCastExpression::AddCastToType(optimizer.context, std::move(rounded), LogicalType::BIGINT);
		if (strict) {
			inclusive = optimizer.BindScalarFunction(upper ? "-" : "+", std::move(inclusive),
			                                         make_uniq<BoundConstantExpression>(Value::BIGINT(1)));
		}
		auto beyond_bigint = make_uniq<BoundComparisonExpression>(
		    ExpressionType::COMPARE_GREATERTHANOREQUALTO, cap(),
		    make_uniq<BoundConstantExpression>(Value::DOUBLE(DECIDE_FIRST_DOUBLE_BEYOND_BIGINT)));
		auto bounded = make_uniq<BoundCaseExpression>(
		    std::move(beyond_bigint),
		    make_uniq<BoundConstantExpression>(Value::BIGINT(std::numeric_limits<int64_t>::max())),
		    std::move(inclusive));
		auto below_zero = make_uniq<BoundComparisonExpression>(
		    strict == upper ? ExpressionType::COMPARE_LESSTHANOREQUALTO : ExpressionType::COMPARE_LESSTHAN, cap(),
		    make_uniq<BoundConstantExpression>(Value::DOUBLE(0.0)));
		return make_uniq<BoundCaseExpression>(std::move(below_zero),
		                                      make_uniq<BoundConstantExpression>(Value::BIGINT(upper ? -1 : 0)),
		                                      std::move(bounded));
	}

	//! Largest lower limit across the constant and source-valued bounds.
	unique_ptr<Expression> LowerLimit(const BoundChecks &bounds) const {
		vector<unique_ptr<Expression>> limits;
		limits.push_back(make_uniq<BoundConstantExpression>(Value::BIGINT(NumericCast<int64_t>(condition.lower))));
		for (idx_t i = 0; i < bounds.slots.size(); i++) {
			if (bounds.slots[i].max_score != DConstants::INVALID_INDEX) {
				limits.push_back(SourceLimit(i, false, bounds));
			}
		}
		return limits.size() == 1 ? std::move(limits[0]) : optimizer.BindScalarFunction("greatest", std::move(limits));
	}

	//! Smallest upper limit across the constant and source-valued bounds; null when there is none.
	unique_ptr<Expression> UpperLimit(const BoundChecks &bounds) const {
		vector<unique_ptr<Expression>> limits;
		if (condition.has_upper) {
			limits.push_back(make_uniq<BoundConstantExpression>(Value::BIGINT(NumericCast<int64_t>(condition.upper))));
		}
		for (idx_t i = 0; i < bounds.slots.size(); i++) {
			if (bounds.slots[i].min_score != DConstants::INVALID_INDEX) {
				limits.push_back(SourceLimit(i, true, bounds));
			}
		}
		if (limits.empty()) {
			return nullptr;
		}
		return limits.size() == 1 ? std::move(limits[0]) : optimizer.BindScalarFunction("least", std::move(limits));
	}

	//! Partition a rank or count window by eligibility and PER key.
	void AddScope(BoundWindowExpression &expression) const {
		if (!condition.scoped) {
			return;
		}
		expression.partitions.push_back(DirectColumn(LogicalType::BOOLEAN, eligible_binding));
		for (auto slot : condition.group_key_slots) {
			expression.partitions.push_back(DirectColumn(source_types[slot], ColumnBinding(score_index, slot)));
		}
	}

	//! A row with no fixed value, neither to one nor to zero.
	unique_ptr<Expression> FreeRow() const {
		return make_uniq<BoundCaseExpression>(
		    DirectColumn(LogicalType::BOOLEAN, fixed_one_binding), DirectConstantBool(false),
		    make_uniq<BoundCaseExpression>(DirectColumn(LogicalType::BOOLEAN, fixed_zero_binding),
		                                   DirectConstantBool(false), DirectConstantBool(true)));
	}

	//! Order a window: free rows first when fixed values exist, then best score first.
	void AddOrder(BoundWindowExpression &expression) const {
		if (has_fixes) {
			expression.orders.emplace_back(OrderType::DESCENDING, OrderByNullType::NULLS_LAST, FreeRow());
		}
		expression.orders.emplace_back(condition.sense == DecideSense::MAXIMIZE ? OrderType::DESCENDING
		                                                                        : OrderType::ASCENDING,
		                               OrderByNullType::NULLS_LAST, DirectColumn(LogicalType::DOUBLE, score_binding));
	}

	//! Stage 5: validate the score, rank rows within their scope, and raise the infeasibility error when the
	//! bounds, fixed values, and row counts cannot all hold.
	RankedRows RankAndFilterFeasibility(unique_ptr<LogicalOperator> score, const BoundChecks &bounds) const {
		// The score is checked above the score projection, which carries the source columns a NULL names.
		vector<DirectSignedValue> score_terms;
		for (auto &part : condition.objective_parts) {
			score_terms.push_back({part.sign, part.coefficient->Copy()});
		}
		auto score_source = DirectMergedOverProjection(optimizer.context, score_terms, source_bindings, score_index);
		auto guard = make_uniq<LogicalFilter>(DirectValidNumberPredicate(
		    optimizer, LogicalType::DOUBLE, score_binding, score_source.get(), DirectNumberKind::OBJECTIVE));
		guard->children.push_back(std::move(score));
		auto rank =
		    make_uniq<BoundWindowExpression>(ExpressionType::WINDOW_ROW_NUMBER, LogicalType::BIGINT, nullptr, nullptr);
		rank->start = WindowBoundary::UNBOUNDED_PRECEDING;
		rank->end = WindowBoundary::UNBOUNDED_FOLLOWING;
		AddOrder(*rank);
		AddScope(*rank);
		auto window = make_uniq<LogicalWindow>(window_index);
		window->expressions.push_back(std::move(rank));
		RankedRows result;
		idx_t free_count_slot = DConstants::INVALID_INDEX;
		if (has_fixes) {
			result.fixed_count_slot = window->expressions.size();
			auto count = DirectWindowMatchingCount(DirectColumn(LogicalType::BOOLEAN, fixed_one_binding));
			AddOrder(*count);
			AddScope(*count);
			window->expressions.push_back(std::move(count));
			if (has_lower && !condition.impossible) {
				free_count_slot = window->expressions.size();
				auto free_count = DirectWindowMatchingCount(FreeRow());
				AddOrder(*free_count);
				AddScope(*free_count);
				window->expressions.push_back(std::move(free_count));
			}
		} else if (has_lower && !condition.impossible) {
			// Match the rank's order so the full-partition count can share its sort.
			auto count =
			    make_uniq<BoundWindowExpression>(ExpressionType::WINDOW_AGGREGATE, LogicalType::BIGINT,
			                                     make_uniq<AggregateFunction>(CountStarFun::GetFunction()), nullptr);
			count->start = WindowBoundary::UNBOUNDED_PRECEDING;
			count->end = WindowBoundary::UNBOUNDED_FOLLOWING;
			AddOrder(*count);
			AddScope(*count);
			window->expressions.push_back(std::move(count));
		}
		window->children.push_back(std::move(guard));
		result.rank_binding = ColumnBinding(window_index, 0);
		result.plan = std::move(window);
		if (condition.impossible || has_lower || has_fixes || source_upper) {
			auto feasible = make_uniq<LogicalFilter>(FeasiblePredicate(result, free_count_slot, bounds));
			feasible->children.push_back(std::move(result.plan));
			result.plan = std::move(feasible);
		}
		return result;
	}

	//! The feasibility predicate of stage 5: a list of checks, each true when it passes and DECIDE's infeasible error
	//! when it fails. Which failing check raises is not specified, and all of them raise the same error. The checks on
	//! the rows of a group apply to eligible rows only; a fixed-value conflict applies to every row.
	unique_ptr<Expression> FeasiblePredicate(const RankedRows &ranked, idx_t free_count_slot,
	                                         const BoundChecks &bounds) const {
		// `ok` must hold; a NULL `ok` fails.
		auto must_hold = [&](unique_ptr<Expression> ok) -> unique_ptr<Expression> {
			return make_uniq<BoundCaseExpression>(std::move(ok), DirectConstantBool(true),
			                                      DirectInfeasibleErrorPredicate(optimizer));
		};
		// `bad` must not hold; a NULL `bad` passes.
		auto must_not_hold = [&](unique_ptr<Expression> bad) -> unique_ptr<Expression> {
			return make_uniq<BoundCaseExpression>(std::move(bad), DirectInfeasibleErrorPredicate(optimizer),
			                                      DirectConstantBool(true));
		};
		auto fixed_count = [&]() {
			return DirectColumn(LogicalType::BIGINT, ColumnBinding(window_index, ranked.fixed_count_slot));
		};
		vector<unique_ptr<Expression>> group_checks;
		if (condition.impossible) {
			// No bound can hold, so a group with any row is infeasible.
			group_checks.push_back(must_not_hold(make_uniq<BoundComparisonExpression>(
			    ExpressionType::COMPARE_GREATERTHANOREQUALTO, DirectColumn(LogicalType::BIGINT, ranked.rank_binding),
			    make_uniq<BoundConstantExpression>(Value::BIGINT(1)))));
		} else {
			if (has_lower) {
				unique_ptr<Expression> available_rows =
				    has_fixes ? optimizer.BindScalarFunction(
				                    "+", fixed_count(),
				                    DirectColumn(LogicalType::BIGINT, ColumnBinding(window_index, free_count_slot)))
				              : DirectColumn(LogicalType::BIGINT, ColumnBinding(window_index, 1));
				group_checks.push_back(must_hold(make_uniq<BoundComparisonExpression>(
				    ExpressionType::COMPARE_GREATERTHANOREQUALTO, std::move(available_rows), LowerLimit(bounds))));
			}
			if ((source_lower || source_upper) && has_upper) {
				group_checks.push_back(must_hold(make_uniq<BoundComparisonExpression>(
				    ExpressionType::COMPARE_LESSTHANOREQUALTO, LowerLimit(bounds), UpperLimit(bounds))));
			}
			if (source_lower) {
				for (auto &slots : bounds.slots) {
					if (slots.max_score == DConstants::INVALID_INDEX) {
						continue;
					}
					group_checks.push_back(must_not_hold(make_uniq<BoundComparisonExpression>(
					    ExpressionType::COMPARE_GREATERTHANOREQUALTO,
					    DirectColumn(LogicalType::DOUBLE, ColumnBinding(score_index, slots.max_score)),
					    make_uniq<BoundConstantExpression>(Value::DOUBLE(DECIDE_FIRST_DOUBLE_BEYOND_BIGINT)))));
				}
			}
			if (has_fixes && has_upper) {
				group_checks.push_back(must_hold(make_uniq<BoundComparisonExpression>(
				    ExpressionType::COMPARE_LESSTHANOREQUALTO, fixed_count(), UpperLimit(bounds))));
			}
		}
		vector<unique_ptr<Expression>> checks;
		if (!group_checks.empty()) {
			unique_ptr<Expression> group = std::move(group_checks[0]);
			for (idx_t i = 1; i < group_checks.size(); i++) {
				group = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(group),
				                                              std::move(group_checks[i]));
			}
			checks.push_back(condition.scoped
			                     ? make_uniq<BoundCaseExpression>(DirectColumn(LogicalType::BOOLEAN, eligible_binding),
			                                                      std::move(group), DirectConstantBool(true))
			                     : std::move(group));
		}
		if (has_fixes) {
			// A row fixed to both values.
			checks.push_back(must_not_hold(make_uniq<BoundCaseExpression>(
			    DirectColumn(LogicalType::BOOLEAN, fixed_one_binding),
			    DirectColumn(LogicalType::BOOLEAN, fixed_zero_binding), DirectConstantBool(false))));
		}
		if (checks.empty()) {
			return DirectConstantBool(true);
		}
		unique_ptr<Expression> predicate = std::move(checks[0]);
		for (idx_t i = 1; i < checks.size(); i++) {
			predicate = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(predicate),
			                                                  std::move(checks[i]));
		}
		return predicate;
	}

	//! Stage 6: the 0/1 assignment (fixed values, then scope, then upper and lower rank limits, then score sign), the
	//! source columns, and the hidden rank that keeps score validation live.
	unique_ptr<LogicalOperator> ProjectAssignment(RankedRows ranked, const BoundChecks &bounds) const {
		auto &rank_binding = ranked.rank_binding;
		auto fixed_count_slot = ranked.fixed_count_slot;
		vector<unique_ptr<Expression>> result_expressions;
		for (idx_t i = 0; i < source_bindings.size(); i++) {
			result_expressions.push_back(DirectColumn(source_types[i], ColumnBinding(score_index, i)));
		}
		auto improving = make_uniq<BoundComparisonExpression>(
		    condition.sense == DecideSense::MAXIMIZE ? ExpressionType::COMPARE_GREATERTHAN
		                                             : ExpressionType::COMPARE_LESSTHAN,
		    DirectColumn(LogicalType::DOUBLE, score_binding), make_uniq<BoundConstantExpression>(Value::DOUBLE(0.0)));
		auto free_choose = improving->Copy();
		unique_ptr<Expression> choose = std::move(improving);
		if (has_lower) {
			unique_ptr<Expression> remaining_lower = LowerLimit(bounds);
			if (has_fixes) {
				remaining_lower = optimizer.BindScalarFunction(
				    "-", std::move(remaining_lower),
				    DirectColumn(LogicalType::BIGINT, ColumnBinding(window_index, fixed_count_slot)));
			}
			auto within_lower = make_uniq<BoundComparisonExpression>(ExpressionType::COMPARE_LESSTHANOREQUALTO,
			                                                         DirectColumn(LogicalType::BIGINT, rank_binding),
			                                                         std::move(remaining_lower));
			choose =
			    make_uniq<BoundCaseExpression>(std::move(within_lower), DirectConstantBool(true), std::move(choose));
		}
		if (has_upper) {
			unique_ptr<Expression> remaining_upper = UpperLimit(bounds);
			if (has_fixes) {
				remaining_upper = optimizer.BindScalarFunction(
				    "-", std::move(remaining_upper),
				    DirectColumn(LogicalType::BIGINT, ColumnBinding(window_index, fixed_count_slot)));
			}
			auto within_upper = make_uniq<BoundComparisonExpression>(ExpressionType::COMPARE_LESSTHANOREQUALTO,
			                                                         DirectColumn(LogicalType::BIGINT, rank_binding),
			                                                         std::move(remaining_upper));
			choose =
			    make_uniq<BoundCaseExpression>(std::move(within_upper), std::move(choose), DirectConstantBool(false));
		}
		if (condition.scoped) {
			choose = make_uniq<BoundCaseExpression>(DirectColumn(LogicalType::BOOLEAN, eligible_binding),
			                                        std::move(choose), std::move(free_choose));
		}
		if (has_fixes) {
			choose = make_uniq<BoundCaseExpression>(DirectColumn(LogicalType::BOOLEAN, fixed_zero_binding),
			                                        DirectConstantBool(false), std::move(choose));
			choose = make_uniq<BoundCaseExpression>(DirectColumn(LogicalType::BOOLEAN, fixed_one_binding),
			                                        DirectConstantBool(true), std::move(choose));
		}
		result_expressions.push_back(
		    make_uniq<BoundCaseExpression>(std::move(choose), make_uniq<BoundConstantExpression>(Value::INTEGER(1)),
		                                   make_uniq<BoundConstantExpression>(Value::INTEGER(0))));
		// The otherwise unused rank is a blocking dependency. It forces a complete
		// value read before an outer LIMIT, COUNT, or column-pruning parent can return.
		result_expressions.push_back(DirectColumn(LogicalType::BIGINT, rank_binding));
		auto result = make_uniq<LogicalProjection>(result_index, std::move(result_expressions));
		result->children.push_back(std::move(ranked.plan));
		return std::move(result);
	}
};

DirectSubstitute Rule::Apply(unique_ptr<LogicalOperator> source, Optimizer &optimizer,
                             const DirectApplicabilityCondition &candidate) const {
	auto &condition = static_cast<const Condition &>(candidate);
	if (condition.objective_parts.empty() && condition.sense != DecideSense::FEASIBILITY) {
		throw InternalException("S1 direct solve received a condition without an objective");
	}
	PlanBuilder plan_builder(condition, optimizer, *source);
	return plan_builder.Build(std::move(source));
}

} // namespace direct_s1_uniform_matroid

unique_ptr<DirectSolveRule> MakeS1UniformMatroidRule() {
	return make_uniq<direct_s1_uniform_matroid::Rule>();
}

} // namespace duckdb
