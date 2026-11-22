#include "duckdb/planner/expression_binder/decide/decide_constraints_binder.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/between_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/common/constants.hpp"
#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/planner/decide/decide_constraint_walk.hpp"
#include "duckdb/planner/decide/decide_source_provenance.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {

DecideConstraintsBinder::DecideConstraintsBinder(Binder &binder, ClientContext &context,
                                                 const case_insensitive_map_t<idx_t> &variables,
                                                 const case_insensitive_set_t &scalar_variables,
                                                 optional_ptr<DecideQualifierContext> qualifier_context)
    : DecideBinder(binder, context, variables, scalar_variables, qualifier_context) {
}

static bool IsSupportedComparison(ExpressionType type) {
	switch (type) {
	case ExpressionType::COMPARE_EQUAL:
	case ExpressionType::COMPARE_LESSTHAN:
	case ExpressionType::COMPARE_GREATERTHAN:
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
	case ExpressionType::COMPARE_NOTEQUAL:
		return true;
	default:
		return false;
	}
}

//! The refusal for a comparison, BETWEEN or conjunction that reads a decision and stands where a number does.
static BindResult ComparisonUsedAsNumber(const ParsedExpression &expr, const DecideSourceText *text) {
	return BindResult(BinderException(expr, "%s", ComparisonUsedAsNumberMessage(DecideWrittenSql(expr, text))));
}

//! The refusal for a bare decision or frame standing as a whole constraint: it says nothing to hold.
static BindResult NotAComparison(const ParsedExpression &expr, const DecideSourceText *text) {
	return BindResult(BinderException(expr, "'%s' is not a comparison", DecideWrittenSql(expr, text)));
}

//! The refusal for a constraint shape DECIDE cannot read, quoting the part as written.
static BindResult UnsupportedConstraint(ParsedExpression &expr, const DecideSourceText *text) {
	return BindResult(NotImplementedException(expr, "'%s' is not supported", DecideWrittenSql(expr, text)));
}

BindResult DecideConstraintsBinder::BindComparison(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth) {
	auto &expr = *expr_ptr;
	if (!IsSupportedComparison(expr.Cast<ComparisonExpression>().type)) {
		return UnsupportedConstraint(expr, qualifier_context->source_text);
	}
	// This function validates and never rewrites the parsed tree. Which side a term belongs on and whether the
	// comparison reads a decision at all are judged on the bound tree, after canonicalization (DecideShapeCheck).
	is_top_expression = false;
	return ExpressionBinder::BindExpression(expr_ptr, depth);
}

BindResult DecideConstraintsBinder::BindOperator(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth) {
	auto &expr = *expr_ptr;
	auto &op = expr.Cast<OperatorExpression>();
	switch (op.type) {
	case ExpressionType::COMPARE_IN: {
		if (!IsVariableExpression(*op.children.front(), variables)) {
			return BindResult(
			    NotImplementedException(expr, "IN on '%s' is not supported",
			                            DecideWrittenSql(*op.children.front(), qualifier_context->source_text)));
		}
		for (idx_t i = 1; i < op.children.size(); i++) {
			if (ExpressionContainsDecideVariable(*op.children[i], variables)) {
				return BindResult(BinderException(expr, "an IN list reads a decision"));
			}
		}
		// Keep the native bound operator as an optimizer marker. DuckDB binds its
		// normal coercions here; DecideOptimizer expands it into the exact existing
		// indicator/cardinality/linking formulation.
		auto was_top_expression = is_top_expression;
		is_top_expression = false;
		auto result = ExpressionBinder::BindExpression(expr_ptr, depth);
		is_top_expression = was_top_expression;
		return result;
	}
	default:
		return UnsupportedConstraint(expr, qualifier_context->source_text);
	}
}

//! `input BETWEEN lower AND upper` as the conjunction `input >= lower AND input <= upper`. Both halves take the
//! BETWEEN's location, where the parser kept the text of the clause, and a tag saying which half they are.
static unique_ptr<ConjunctionExpression> ExpandBetween(const BetweenExpression &between) {
	auto lower_bound = make_uniq<ComparisonExpression>(ExpressionType::COMPARE_GREATERTHANOREQUALTO,
	                                                   between.input->Copy(), between.lower->Copy());
	auto upper_bound = make_uniq<ComparisonExpression>(ExpressionType::COMPARE_LESSTHANOREQUALTO, between.input->Copy(),
	                                                   between.upper->Copy());
	lower_bound->query_location = between.query_location;
	upper_bound->query_location = between.query_location;
	SetBetweenHalf(*lower_bound, DecideBetweenHalf::LOWER);
	SetBetweenHalf(*upper_bound, DecideBetweenHalf::UPPER);
	return make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(lower_bound),
	                                        std::move(upper_bound));
}

BindResult DecideConstraintsBinder::BindBetween(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth) {
	expr_ptr = ExpandBetween(expr_ptr->Cast<BetweenExpression>());
	return BindConjunction(expr_ptr, depth);
}

BindResult DecideConstraintsBinder::BindConjunction(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth) {
	auto &expr = *expr_ptr;
	auto &conj = expr.Cast<ConjunctionExpression>();
	// Constraints are joined with AND. An OR between them would need an indicator per
	// alternative, which no layer builds, and every later walk reads a conjunction as AND.
	if (conj.GetExpressionType() != ExpressionType::CONJUNCTION_AND) {
		return BindResult(NotImplementedException(expr, "OR between SUCH THAT constraints is not supported"));
	}
	// Only a constraint list reaches here (an operand conjunction is data or refused), so
	// each child is a constraint.
	ErrorData error;
	for (idx_t i = 0; i < conj.children.size(); i++) {
		is_top_expression = true;
		BindChild(conj.children[i], depth, error);
	}
	if (error.HasError()) {
		return BindResult(std::move(error));
	}
	// the children have been successfully resolved
	// cast the input types to boolean (if necessary)
	// and construct the bound conjunction expression
	auto result = make_uniq<BoundConjunctionExpression>(conj.GetExpressionType());
	for (auto &child_expr : conj.children) {
		auto &child = BoundExpression::GetExpression(*child_expr);
		result->children.push_back(BoundCastExpression::AddCastToType(context, std::move(child), LogicalType::BOOLEAN));
	}
	// now create the bound conjunction expression
	return BindResult(std::move(result));
}

unique_ptr<Expression> DecideConstraintsBinder::BindConstraints(vector<DecideClause> &constraints) {
	// Each constraint binds on its own; the first error is the one raised.
	ErrorData error;
	vector<unique_ptr<Expression>> bound;
	for (auto &clause : constraints) {
		auto result = BindClause(clause);
		if (!result.HasError()) {
			bound.push_back(std::move(result.expression));
		} else if (!error.HasError()) {
			error = std::move(result.error);
		}
	}
	if (error.HasError()) {
		error.Throw();
	}
	if (bound.size() == 1) {
		return std::move(bound[0]);
	}
	auto result = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
	for (auto &constraint : bound) {
		result->children.push_back(
		    BoundCastExpression::AddCastToType(context, std::move(constraint), LogicalType::BOOLEAN));
	}
	return std::move(result);
}

BindResult DecideConstraintsBinder::BindClause(DecideClause &clause) {
	// `[when c] [per K]: body`. The scope's parts bind in place. `per row`, the default written out, adds no class
	// key: one constraint per kept row.
	auto &scope = clause.scope;
	bool per = scope.per == DecidePer::ALL || scope.per == DecidePer::KEY;
	auto &when = scope.when;
	idx_t depth = 0;

	ErrorData body_error;
	is_top_expression = true;
	BindChild(clause.body, depth, body_error);
	if (body_error.HasError()) {
		return BindResult(std::move(body_error));
	}
	auto bound = std::move(BoundExpression::GetExpression(*clause.body));
	if (scope.IsEmpty()) {
		return BindResult(std::move(bound));
	}

	// The condition takes data only (ValidateDecideScopeSpellings): plain SQL, no
	// parentheses needed around a compound one.
	auto bind_condition = [&](unique_ptr<ParsedExpression> &parsed, unique_ptr<Expression> &out) {
		is_top_expression = false;
		auto error = BindDataChild(parsed, depth);
		if (!error.HasError()) {
			out = BoundCastExpression::AddCastToType(context, std::move(BoundExpression::GetExpression(*parsed)),
			                                         LogicalType::BOOLEAN);
		}
		return error;
	};
	// A bound subquery cannot be copied, so a second constraint of the scope (a BETWEEN's
	// other half) binds the condition afresh from it as written.
	unique_ptr<ParsedExpression> condition_text;
	unique_ptr<Expression> condition;
	if (when) {
		condition_text = when->Copy();
		auto error = bind_condition(when, condition);
		if (error.HasError()) {
			return BindResult(std::move(error));
		}
	}
	idx_t key = DConstants::INVALID_INDEX;
	if (per) {
		// The key as written (`per D`), read before its elements bind.
		auto key_text = DecideKeyToString(scope.key);
		key = BindKey(context, binder, scope.key, DecisionNames(variables), key_text, qualifier_context->source_text,
		              *qualifier_context->keys);
	}

	// One scope per constraint: a BETWEEN bound to the conjunction of its two halves, and
	// each half gets the whole scope.
	auto apply_scope = [&](unique_ptr<Expression> constraint) -> unique_ptr<Expression> {
		unique_ptr<Expression> this_condition = std::move(condition);
		if (when && !this_condition) {
			auto parsed = condition_text->Copy();
			auto error = bind_condition(parsed, this_condition);
			if (error.HasError()) {
				error.Throw();
			}
		}
		// The wrapper stands at its `per`, where the parser kept the key as written.
		DecideRowPer written_per;
		written_per.key = key;
		written_per.location = scope.location;
		return MakeScopedRow(std::move(constraint), std::move(this_condition), per ? &written_per : nullptr);
	};
	if (bound->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto result = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
		for (auto &half : bound->Cast<BoundConjunctionExpression>().children) {
			result->children.push_back(apply_scope(std::move(half)));
		}
		return BindResult(std::move(result));
	}
	return BindResult(apply_scope(std::move(bound)));
}

BindResult DecideConstraintsBinder::BindExpression(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth,
                                                   bool root_expression) {
	auto location = expr_ptr->GetQueryLocation();
	return ThrowClauseError(PreserveQueryLocation(location, BindExpressionInternal(expr_ptr, depth, root_expression)),
	                        root_expression);
}

BindResult DecideConstraintsBinder::BindExpressionInternal(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth,
                                                           bool root_expression) {
	if (binding_when_condition) {
		return ExpressionBinder::BindExpression(expr_ptr, depth);
	}
	if (depth > 0) {
		return ExpressionBinder::BindExpression(expr_ptr, depth);
	}
	auto &expr = *expr_ptr;
	// Below a constraint's top, an operand that reads no decision is data, whatever its
	// shape: `CAST(cap > 2 AS INT)`, a parameter `$1`, a lambda and its parameters.
	if (!is_top_expression && IsDataOperand(expr)) {
		return ExpressionBinder::BindExpression(expr_ptr, depth);
	}
	// A comparison over a decision below the top, as in `x <= (x > 1)`, is neither a
	// constraint nor data.
	if (!is_top_expression && (expr.GetExpressionClass() == ExpressionClass::COMPARISON ||
	                           expr.GetExpressionClass() == ExpressionClass::BETWEEN ||
	                           expr.GetExpressionClass() == ExpressionClass::CONJUNCTION)) {
		return ComparisonUsedAsNumber(expr, qualifier_context->source_text);
	}
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::COLUMN_REF: {
		if (!is_top_expression) {
			return ExpressionBinder::BindExpression(expr_ptr, depth);
		}
		// A bare column at the top level of SUCH THAT is never a valid constraint.
		return NotAComparison(expr, qualifier_context->source_text);
	}
	case ExpressionClass::FUNCTION:
		if (!is_top_expression) {
			return BindFunction(expr_ptr, depth);
		}
		break;
	case ExpressionClass::DECIDE_REDUCER:
		if (!is_top_expression) {
			return DecideBinder::BindExpression(expr_ptr, depth, root_expression);
		}
		break;
	case ExpressionClass::COMPARISON:
		return BindComparison(expr_ptr, depth);
	case ExpressionClass::OPERATOR: {
		if (!is_top_expression) {
			return ExpressionBinder::BindExpression(expr_ptr, depth);
		}
		return BindOperator(expr_ptr, depth);
	}
	case ExpressionClass::BETWEEN:
		return BindBetween(expr_ptr, depth);
	case ExpressionClass::CONJUNCTION:
		return BindConjunction(expr_ptr, depth);
	case ExpressionClass::SUBQUERY:
	case ExpressionClass::WINDOW:
		return DecideBinder::BindExpression(expr_ptr, depth, root_expression);
	case ExpressionClass::CASE:
		// A CASE below the top binds as SQL; DecideShapeCheck refuses one over a decision.
		if (!is_top_expression) {
			return ExpressionBinder::BindExpression(expr_ptr, depth);
		}
		break;
	case ExpressionClass::DECIDE_FRAME:
		if (!is_top_expression) {
			return DecideBinder::BindExpression(expr_ptr, depth, root_expression);
		}
		// A frame alone, like a bare decision, says nothing to hold.
		return NotAComparison(expr, qualifier_context->source_text);
	default:
		break;
	}
	return UnsupportedConstraint(expr, qualifier_context->source_text);
}

} // namespace duckdb
