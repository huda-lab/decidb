#include "duckdb/planner/decide/decide_cast_policy.hpp"
#include "duckdb/common/decide_numeric_policy.hpp"
#include "duckdb/common/exception.hpp"

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

static bool EvaluateFoldableDouble(ClientContext &context, const Expression &expr, double &out, uint32_t flags) {
	if (!expr.IsFoldable()) {
		return false;
	}
	if ((flags & FOLD_STABLE) &&
	    (!expr.IsConsistent() || expr.HasParameter() || expr.HasSubquery() || expr.IsAggregate() || expr.IsWindow())) {
		return false;
	}
	Value value;
	if (!ExpressionExecutor::TryEvaluateScalar(context, expr, value) || value.IsNull()) {
		return false;
	}
	bool readable = value.type().IsNumeric() || ((flags & FOLD_ALLOW_BOOLEAN) && value.type() == LogicalType::BOOLEAN);
	if (!readable) {
		return false;
	}
	out = value.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
	if (flags & FOLD_EXACT_FINITE) {
		return DecideIsExactFiniteDouble(
		    out, value, [&](double converted) { return Value::DOUBLE(converted).DefaultCastAs(value.type()); });
	}
	return true;
}

bool TryEvaluateFoldableDouble(ClientContext &context, const Expression &expr, double &out, uint32_t flags) {
	if ((flags & FOLD_CATCH_ALL) == FOLD_CATCH_ALL) {
		try {
			return EvaluateFoldableDouble(context, expr, out, flags);
		} catch (...) {
			return false;
		}
	}
	if (flags & FOLD_CATCH_DUCKDB_ERRORS) {
		try {
			return EvaluateFoldableDouble(context, expr, out, flags);
		} catch (Exception &) {
			return false;
		}
	}
	return EvaluateFoldableDouble(context, expr, out, flags);
}

bool IsCastWrappedConstant(const Expression &expr) {
	// StripCastsForIdentity is the right tool despite its warning: only the innermost
	// node's CLASS is read here, never its value. The value comes from evaluating the
	// complete expression via TryEvaluateFoldableDouble, casts included.
	return StripCastsForIdentity(expr)->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT;
}

} // namespace duckdb
