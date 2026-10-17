#include "duckdb/planner/decide/decide_cast_policy.hpp"

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

const BoundColumnRefExpression *GetBareDecideColumnRef(const Expression &expr, idx_t decide_index) {
	const Expression *current = UnwrapDecideCasts(expr, decide_index);
	if (current->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
		return nullptr;
	}
	auto &colref = current->Cast<BoundColumnRefExpression>();
	return colref.binding.table_index == decide_index ? &colref : nullptr;
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

bool TryEvaluateFoldableDoubleNoThrow(ClientContext &context, const Expression &expr, double &out) {
	try {
		return TryEvaluateFoldableDouble(context, expr, out);
	} catch (...) {
		return false;
	}
}

int ScaleSignAtPlanTime(ClientContext &context, const Expression *scale, bool divides) {
	if (!scale) {
		return 1;
	}
	double d;
	if (!TryEvaluateFoldableDoubleNoThrow(context, *scale, d)) {
		return 0;
	}
	if (d == 0.0) {
		// A zero factor annihilates the term. Dividing by it is an error the
		// evaluator will raise; multiplying by it makes the sign irrelevant, and
		// "positive" keeps the cheap path (every coefficient is zero either way).
		return divides ? 0 : 1;
	}
	return d < 0.0 ? -1 : 1;
}

ExtremumPush ExtremumPushOf(ExpressionType comparison) {
	switch (comparison) {
	case ExpressionType::COMPARE_LESSTHAN:
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		return ExtremumPush::DOWN;
	case ExpressionType::COMPARE_GREATERTHAN:
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		return ExtremumPush::UP;
	default:
		return ExtremumPush::NONE;
	}
}

ExtremumPush ExtremumPushOf(ExtremumPush outer, int sign) {
	if (outer == ExtremumPush::NONE || sign == 0) {
		return ExtremumPush::NONE;
	}
	if (sign > 0) {
		return outer;
	}
	return outer == ExtremumPush::DOWN ? ExtremumPush::UP : ExtremumPush::DOWN;
}

bool IsCastWrappedConstant(const Expression &expr) {
	// StripCastsForIdentity is the right tool despite its warning: only the innermost
	// node's CLASS is read here, never its value. The value comes from evaluating the
	// complete expression via TryEvaluateFoldableDouble, casts included.
	return StripCastsForIdentity(expr)->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT;
}

} // namespace duckdb
