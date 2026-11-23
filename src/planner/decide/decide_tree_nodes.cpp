//===----------------------------------------------------------------------===//
//                         DecidB
//
// src/planner/decide/decide_tree_nodes.cpp
//
// The facts of a reducer and of a computed value read whole; see decide_tree_nodes.hpp.
//
//===----------------------------------------------------------------------===//
#include "duckdb/planner/decide/decide_tree_nodes.hpp"

#include "duckdb/planner/expression/bound_decide_null_test_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// A reducer
//===--------------------------------------------------------------------===//
bool IsDecideNormMarker(const Expression &expr, DecideNorm &norm) {
	if (!IsDecideNormMarker(expr)) {
		return false;
	}
	norm = expr.Cast<BoundDecideReducerExpression>().norm;
	return true;
}

bool IsDecideNormMarker(const Expression &expr) {
	return expr.GetExpressionClass() == ExpressionClass::BOUND_DECIDE_REDUCER &&
	       !expr.Cast<BoundDecideReducerExpression>().function;
}

DecideHelperRef DecideDefinitionOf(const Expression &expr) {
	return expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION
	           ? expr.Cast<BoundFunctionExpression>().definition
	           : DecideHelperRef();
}

bool DecideFactsEqual(const Expression &a, const Expression &b) {
	if (a.GetAlias() != b.GetAlias() || !(DecideDefinitionOf(a) == DecideDefinitionOf(b))) {
		return false;
	}
	return a.GetExpressionClass() != ExpressionClass::BOUND_DECIDE_REDUCER ||
	       b.GetExpressionClass() != ExpressionClass::BOUND_DECIDE_REDUCER ||
	       a.Cast<BoundDecideReducerExpression>().FactsEqual(b.Cast<BoundDecideReducerExpression>());
}

//===--------------------------------------------------------------------===//
// A reducer's NULL tests
//===--------------------------------------------------------------------===//
unique_ptr<Expression> MakeDecideNullTest(unique_ptr<Expression> value) {
	auto test = make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_IS_NOT_NULL, LogicalType::BOOLEAN);
	test->children.push_back(std::move(value));
	return make_uniq<BoundDecideNullTestExpression>(std::move(test));
}

//===--------------------------------------------------------------------===//
// A computed value read whole
//===--------------------------------------------------------------------===//
bool IsDecideQueryWideValue(const vector<DecideComputedValue> &values, const Expression &expr) {
	for (auto &known : values) {
		if (known.query_wide && Expression::Equals(*known.value, expr)) {
			return true;
		}
	}
	return false;
}

bool DecideHoldsSubqueryValue(const vector<DecideComputedValue> &values, const Expression &expr) {
	for (auto &known : values) {
		if (known.subquery && Expression::Equals(*known.value, expr)) {
			return true;
		}
	}
	bool found = false;
	ExpressionIterator::EnumerateChildren(
	    expr, [&](const Expression &child) { found = found || DecideHoldsSubqueryValue(values, child); });
	return found;
}

} // namespace duckdb
