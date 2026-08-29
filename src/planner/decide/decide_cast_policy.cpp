#include "duckdb/decidb/decide_cast_policy.hpp"

#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"

namespace duckdb {

bool BoundExpressionReferencesDecide(const Expression &expr, idx_t decide_index) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		return expr.Cast<BoundColumnRefExpression>().binding.table_index == decide_index;
	}
	bool found = false;
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) {
		if (!found) {
			found = BoundExpressionReferencesDecide(child, decide_index);
		}
	});
	return found;
}

const Expression *UnwrapDecideCasts(const Expression &expr, idx_t decide_index) {
	const Expression *current = &expr;
	while (current->GetExpressionClass() == ExpressionClass::BOUND_CAST &&
	       BoundExpressionReferencesDecide(*current, decide_index)) {
		current = current->Cast<BoundCastExpression>().child.get();
	}
	return current;
}

Expression *UnwrapDecideCasts(Expression &expr, idx_t decide_index) {
	Expression *current = &expr;
	while (current->GetExpressionClass() == ExpressionClass::BOUND_CAST &&
	       BoundExpressionReferencesDecide(*current, decide_index)) {
		current = current->Cast<BoundCastExpression>().child.get();
	}
	return current;
}

const Expression *StripCastsForIdentity(const Expression &expr) {
	const Expression *current = &expr;
	while (current->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
		current = current->Cast<BoundCastExpression>().child.get();
	}
	return current;
}

bool TryEvaluateFoldableDouble(ClientContext &context, const Expression &expr, double &out) {
	if (!expr.IsFoldable()) {
		return false;
	}
	Value value;
	if (!ExpressionExecutor::TryEvaluateScalar(context, expr, value) || value.IsNull() ||
	    !value.type().IsNumeric()) {
		return false;
	}
	out = value.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
	return true;
}

bool IsCastWrappedConstant(const Expression &expr) {
	// StripCastsForIdentity is the right tool despite its warning: only the innermost
	// node's CLASS is read here, never its value. The value comes from evaluating the
	// complete expression via TryEvaluateFoldableDouble, casts included.
	return StripCastsForIdentity(expr)->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT;
}

} // namespace duckdb
