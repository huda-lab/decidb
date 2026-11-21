#include "duckdb/planner/expression_binder/decide/decide_objective_binder.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"

namespace duckdb {

DecideObjectiveBinder::DecideObjectiveBinder(Binder &binder, ClientContext &context,
                                             const case_insensitive_map_t<idx_t> &variables,
                                             const case_insensitive_set_t &scalar_variables,
                                             optional_ptr<DecideQualifierContext> qualifier_context)
    : DecideBinder(binder, context, variables, scalar_variables, qualifier_context) {
}

BindResult DecideObjectiveBinder::BindScopedObjective(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth) {
	// `[when c] [per ()]: objective`. An objective is produced once, so `per ()` is its
	// default and adds nothing; the parser refuses any other key.
	auto *body = &expr_ptr;
	if (IsDecideMarker(**body, SCOPE_PER_TAG)) {
		body = &(*body)->Cast<FunctionExpression>().children[0];
	}
	optional_ptr<FunctionExpression> when;
	if (IsDecideMarker(**body, SCOPE_WHEN_TAG)) {
		when = &(*body)->Cast<FunctionExpression>();
		body = &when->children[0];
	}
	ErrorData body_error;
	BindChild(*body, depth, body_error);
	if (body_error.HasError()) {
		return BindResult(std::move(body_error));
	}
	auto bound = std::move(BoundExpression::GetExpression(**body));
	if (!when) {
		return BindResult(std::move(bound));
	}
	auto condition_error = BindDataChild(when->children[1], depth);
	if (condition_error.HasError()) {
		return BindResult(std::move(condition_error));
	}
	auto wrap = [&](unique_ptr<Expression> objective) -> unique_ptr<Expression> {
		auto wrapper = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
		wrapper->children.push_back(std::move(objective));
		wrapper->children.push_back(BoundCastExpression::AddCastToType(
		    context, std::move(BoundExpression::GetExpression(*when->children[1])), LogicalType::BOOLEAN));
		wrapper->alias = WHEN_FILTER_TAG;
		return std::move(wrapper);
	};
	return BindResult(wrap(std::move(bound)));
}

BindResult DecideObjectiveBinder::BindExpression(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth,
                                                 bool root_expression) {
	auto location = expr_ptr->GetQueryLocation();
	return ThrowClauseError(PreserveQueryLocation(location, BindExpressionInternal(expr_ptr, depth, root_expression)),
	                        root_expression);
}

BindResult DecideObjectiveBinder::BindExpressionInternal(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth,
                                                         bool root_expression) {
	if (binding_when_condition) {
		return ExpressionBinder::BindExpression(expr_ptr, depth);
	}
	if (depth > 0) {
		return ExpressionBinder::BindExpression(expr_ptr, depth, root_expression);
	}
	auto &expr = *expr_ptr;
	// Whether the objective reads a decision, and in what shape, is judged on the bound tree after canonicalization
	// (DecideShapeCheck). What only the parsed tree can tell is left here.
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::FUNCTION: {
		auto &func = expr.Cast<FunctionExpression>();
		if (func.is_operator && (func.function_name == SCOPE_PER_TAG || func.function_name == SCOPE_WHEN_TAG)) {
			return BindScopedObjective(expr_ptr, depth);
		}
		return BindFunction(expr_ptr, depth);
	}
	case ExpressionClass::SUBQUERY:
	case ExpressionClass::WINDOW:
	case ExpressionClass::DECIDE_FRAME:
		return DecideBinder::BindExpression(expr_ptr, depth, root_expression);
	case ExpressionClass::COMPARISON:
	case ExpressionClass::BETWEEN:
	case ExpressionClass::CONJUNCTION:
		// A comparison, BETWEEN or conjunction over a decision is a truth value, not a number.
		if (ExpressionContainsDecideVariable(expr, variables)) {
			return BindResult(BinderException(
			    expr, "%s", ComparisonUsedAsNumberMessage(DecideWrittenSql(expr, qualifier_context->source_text))));
		}
		return ExpressionBinder::BindExpression(expr_ptr, depth);
	default:
		return ExpressionBinder::BindExpression(expr_ptr, depth);
	}
}

} // namespace duckdb
