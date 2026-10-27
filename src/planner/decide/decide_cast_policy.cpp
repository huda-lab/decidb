#include "duckdb/planner/decide/decide_cast_policy.hpp"
#include "duckdb/common/decide_numeric_policy.hpp"
#include "duckdb/common/exception.hpp"

#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

#include <cmath>
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

PlanTimeSign PlanTimeSignOf(ClientContext &context, const Expression &expr) {
	double value;
	if (TryEvaluateFoldableDouble(context, expr, value, FOLD_CATCH_ALL)) {
		if (!std::isfinite(value)) {
			return PlanTimeSign::UNKNOWN;
		}
		return value == 0.0 ? PlanTimeSign::ZERO : (value < 0.0 ? PlanTimeSign::NEGATIVE : PlanTimeSign::POSITIVE);
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
		return PlanTimeSignOf(context, *expr.Cast<BoundCastExpression>().child);
	}
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return PlanTimeSign::UNKNOWN;
	}
	auto &func = expr.Cast<BoundFunctionExpression>();
	const auto &name = func.function.name;
	auto flip = [](PlanTimeSign sign) {
		return sign == PlanTimeSign::POSITIVE   ? PlanTimeSign::NEGATIVE
		       : sign == PlanTimeSign::NEGATIVE ? PlanTimeSign::POSITIVE
		                                        : sign;
	};
	// The sign of a product: zero absorbs, unknown spreads, otherwise the signs multiply.
	auto times = [&](PlanTimeSign left, PlanTimeSign right) {
		if (left == PlanTimeSign::ZERO || right == PlanTimeSign::ZERO) {
			return PlanTimeSign::ZERO;
		}
		if (left == PlanTimeSign::UNKNOWN || right == PlanTimeSign::UNKNOWN) {
			return PlanTimeSign::UNKNOWN;
		}
		return left == right ? PlanTimeSign::POSITIVE : PlanTimeSign::NEGATIVE;
	};
	if (name == "-" && func.children.size() == 1) {
		return flip(PlanTimeSignOf(context, *func.children[0]));
	}
	if (name == "*" && func.children.size() == 2) {
		return times(PlanTimeSignOf(context, *func.children[0]), PlanTimeSignOf(context, *func.children[1]));
	}
	if (name == "/" && func.children.size() == 2) {
		// A zero or unknown divisor leaves the quotient unknown; a zero numerator over
		// anything else is zero.
		auto divisor = PlanTimeSignOf(context, *func.children[1]);
		if (divisor == PlanTimeSign::ZERO || divisor == PlanTimeSign::UNKNOWN) {
			return PlanTimeSign::UNKNOWN;
		}
		return times(PlanTimeSignOf(context, *func.children[0]), divisor);
	}
	return PlanTimeSign::UNKNOWN;
}

int ScaleSignAtPlanTime(ClientContext &context, const Expression *scale, bool divides) {
	if (!scale) {
		return 1;
	}
	switch (PlanTimeSignOf(context, *scale)) {
	case PlanTimeSign::POSITIVE:
		return 1;
	case PlanTimeSign::NEGATIVE:
		return -1;
	case PlanTimeSign::ZERO:
		// A zero factor annihilates the term. Dividing by it is an error the
		// evaluator will raise; multiplying by it makes the sign irrelevant, and
		// "positive" keeps the cheap path (every coefficient is zero either way).
		return divides ? 0 : 1;
	default:
		return 0;
	}
}

bool ScaleVanishesAtPlanTime(ClientContext &context, const Expression *scale, bool divides) {
	return scale && !divides && PlanTimeSignOf(context, *scale) == PlanTimeSign::ZERO;
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

} // namespace duckdb
