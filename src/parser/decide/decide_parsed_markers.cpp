#include "duckdb/parser/decide/decide_parsed_markers.hpp"

#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/expression/function_expression.hpp"

namespace duckdb {

bool IsDecideMarker(const ParsedExpression &expr, const char *tag) {
	if (expr.GetExpressionClass() != ExpressionClass::FUNCTION) {
		return false;
	}
	auto &func = expr.Cast<FunctionExpression>();
	return func.is_operator && func.function_name == tag && !func.children.empty();
}

bool IsDecideScopeMarker(const ParsedExpression &expr) {
	return IsDecideMarker(expr, SCOPE_WHEN_TAG) || IsDecideMarker(expr, SCOPE_PER_TAG) ||
	       IsDecideMarker(expr, SCOPE_PER_ROW_TAG);
}

bool IsDecideAggregateName(const string &name) {
	auto lname = StringUtil::Lower(name);
	return lname == "sum" || lname == "avg" || lname == "min" || lname == "max";
}

bool IsScopedReducerCall(const ParsedExpression &expr) {
	if (expr.GetExpressionClass() != ExpressionClass::FUNCTION) {
		return false;
	}
	auto &func = expr.Cast<FunctionExpression>();
	return !func.is_operator && IsDecideAggregateName(func.function_name);
}

bool TakesReducerBy(const ParsedExpression &expr) {
	if (IsScopedReducerCall(expr)) {
		return true;
	}
	if (expr.GetExpressionClass() != ExpressionClass::FUNCTION) {
		return false;
	}
	auto &func = expr.Cast<FunctionExpression>();
	return !func.is_operator &&
	       (StringUtil::CIEquals(func.function_name, "norm") || StringUtil::CIEquals(func.function_name, "count") ||
	        StringUtil::CIEquals(func.function_name, "count_star"));
}

const ParsedExpression &UnwrapReducerMarkers(const ParsedExpression &expr) {
	const ParsedExpression *result = &expr;
	for (auto tag : {REDUCER_BY_TAG, QUALIFIED_REDUCER_TAG}) {
		if (IsDecideMarker(*result, tag)) {
			result = result->Cast<FunctionExpression>().children[0].get();
		}
	}
	return *result;
}

const ParsedExpression &ReducerArgumentBody(const ParsedExpression &argument) {
	const ParsedExpression *body = &argument;
	while (IsDecideScopeMarker(*body)) {
		body = body->Cast<FunctionExpression>().children[0].get();
	}
	return *body;
}

} // namespace duckdb
