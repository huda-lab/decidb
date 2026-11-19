#include "duckdb/optimizer/decide/direct/direct_expression.hpp"
#include "duckdb/optimizer/decide/direct/direct_rule.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "duckdb/common/decide_numeric_policy.hpp"
#include "duckdb/common/decide_value_messages.hpp"
#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/planner/decide/decide_cast_policy.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"

namespace duckdb {

bool DirectIsDecisionFreeDeterministic(const Expression &expr, idx_t decide_index) {
	// The binder refuses a volatile function in a clause (RefuseVolatile). The consistency test still declines one that
	// is constant only within a query (`now()`, `txid_current()`), which a rule does not prove safe to evaluate in its
	// plan.
	if (BoundExpressionReferencesDecide(expr, decide_index) || !expr.IsConsistent() || expr.HasSubquery() ||
	    expr.HasParameter() || expr.IsAggregate() || expr.IsWindow()) {
		return false;
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF &&
	    expr.Cast<BoundColumnRefExpression>().depth != 0) {
		return false;
	}
	bool safe = true;
	ExpressionIterator::EnumerateChildren(
	    expr, [&](const Expression &child) { safe = safe && DirectIsDecisionFreeDeterministic(child, decide_index); });
	return safe;
}

bool DirectReferencesOnlySource(const Expression &expr, const vector<ColumnBinding> &source_bindings) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto &ref = expr.Cast<BoundColumnRefExpression>();
		return ref.depth == 0 &&
		       std::find(source_bindings.begin(), source_bindings.end(), ref.binding) != source_bindings.end();
	}
	bool valid = true;
	ExpressionIterator::EnumerateChildren(
	    expr, [&](const Expression &child) { valid = valid && DirectReferencesOnlySource(child, source_bindings); });
	return valid;
}

bool DirectHasColumnReference(const Expression &expr) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		return true;
	}
	bool found = false;
	ExpressionIterator::EnumerateChildren(
	    expr, [&](const Expression &child) { found = found || DirectHasColumnReference(child); });
	return found;
}

bool DirectRemapSourceReferences(Expression &expr, const vector<ColumnBinding> &source_bindings,
                                 idx_t projection_index) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto &ref = expr.Cast<BoundColumnRefExpression>();
		auto found = std::find(source_bindings.begin(), source_bindings.end(), ref.binding);
		if (ref.depth != 0 || found == source_bindings.end()) {
			return false;
		}
		ref.binding = ColumnBinding(projection_index, found - source_bindings.begin());
		return true;
	}
	bool valid = true;
	ExpressionIterator::EnumerateChildren(expr, [&](Expression &child) {
		valid = valid && DirectRemapSourceReferences(child, source_bindings, projection_index);
	});
	return valid;
}

void DirectReplaceColumns(unique_ptr<Expression> &expr, const vector<ColumnBinding> &columns,
                          const vector<unique_ptr<Expression>> &values) {
	D_ASSERT(columns.size() == values.size());
	if (expr->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto found = std::find(columns.begin(), columns.end(), expr->Cast<BoundColumnRefExpression>().binding);
		if (found == columns.end()) {
			throw InternalException("DECIDE: a direct-solve expression reads a column outside the ones replaced; its "
			                        "caller promises it reads only those");
		}
		expr = values[NumericCast<idx_t>(found - columns.begin())]->Copy();
		return;
	}
	ExpressionIterator::EnumerateChildren(
	    *expr, [&](unique_ptr<Expression> &child) { DirectReplaceColumns(child, columns, values); });
}

//! True for a cast that can fail on some value: a narrowing cast that is not TRY_CAST. This node only, not its child.
static bool DirectCastMayThrow(const BoundCastExpression &cast) {
	// TRY_CAST turns conversion failures into NULL; the solver validates that result on every row.
	// BoundCastExpression::CanThrow conservatively treats it like a regular narrowing cast.
	return !cast.try_cast && cast.return_type.id() != cast.child->return_type.id() &&
	       LogicalType::ForceMaxLogicalType(cast.return_type, cast.child->return_type) == cast.child->return_type.id();
}

bool DirectMayThrow(const Expression &expr) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION &&
	    expr.Cast<BoundFunctionExpression>().function.errors == FunctionErrors::CAN_THROW_RUNTIME_ERROR) {
		return true;
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST &&
	    DirectCastMayThrow(expr.Cast<BoundCastExpression>())) {
		return true;
	}
	bool may_throw = false;
	ExpressionIterator::EnumerateChildren(
	    expr, [&](const Expression &child) { may_throw = may_throw || DirectMayThrow(child); });
	return may_throw;
}

bool DirectIsSourceOnlyPredicate(const Expression &expr, idx_t decide_index,
                                 const vector<ColumnBinding> &source_bindings) {
	return expr.return_type == LogicalType::BOOLEAN && !expr.CanThrow() &&
	       DirectIsDecisionFreeDeterministic(expr, decide_index) && DirectReferencesOnlySource(expr, source_bindings);
}

bool DirectPartFactor(ClientContext &context, const DirectPart &part, DirectFactor &factor) {
	factor = DirectFactor();
	if (!part.scale) {
		return true;
	}
	factor.present = true;
	factor.divides = part.scale_divides;
	return TryEvaluateFoldableDouble(context, *part.scale, factor.value, DIRECT_FINITE_CONSTANT) &&
	       !(factor.divides && factor.value == 0.0);
}

bool DirectIsSourceOnlyNumericMayRaise(const Expression &expr, idx_t decide_index,
                                       const vector<ColumnBinding> &source_bindings) {
	return expr.return_type.IsNumeric() && DirectIsSourceOnlyValue(expr, decide_index, source_bindings) &&
	       DirectHasColumnReference(expr);
}

bool DirectIsSourceOnlyValue(const Expression &expr, idx_t decide_index, const vector<ColumnBinding> &source_bindings) {
	return (expr.return_type.IsNumeric() || expr.return_type == LogicalType::BOOLEAN) &&
	       DirectIsDecisionFreeDeterministic(expr, decide_index) && DirectReferencesOnlySource(expr, source_bindings);
}

bool DirectIsWholeNumbered(DirectDomain domain) {
	return domain != DirectDomain::REAL;
}

double DirectTypeMagnitude(const LogicalType &type) {
	if (!type.IsIntegral() && type.id() != LogicalTypeId::DECIMAL) {
		return std::numeric_limits<double>::infinity();
	}
	auto largest = Value::MaximumValue(type).GetValue<double>();
	auto least = Value::MinimumValue(type).GetValue<double>();
	return std::max(std::fabs(largest), std::fabs(least));
}

//! The largest finite magnitude a value of `type` holds: FLOAT's and DOUBLE's largest finite value, else the type's
//! magnitude (DirectTypeMagnitude).
static double DirectLargestFinite(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::FLOAT:
		return std::numeric_limits<float>::max();
	case LogicalTypeId::DOUBLE:
		return std::numeric_limits<double>::max();
	default:
		return DirectTypeMagnitude(type);
	}
}

double DirectMagnitudeBound(ClientContext &context, const Expression &expr) {
	double constant;
	if (TryEvaluateFoldableDouble(context, expr, constant, DIRECT_CONSTANT)) {
		return std::isnan(constant) ? std::numeric_limits<double>::infinity() : std::fabs(constant);
	}
	// A cast keeps its operand's magnitude while its type holds it (the binder reads every coefficient as DOUBLE).
	// Past that, a FLOAT or DOUBLE rounds the value to an infinity and an exact type raises, so the bound is the
	// type's. An operand that is not a number has its type's bound, which is infinite.
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
		auto bound = DirectMagnitudeBound(context, *expr.Cast<BoundCastExpression>().child);
		return bound <= DirectLargestFinite(expr.return_type) ? bound : DirectTypeMagnitude(expr.return_type);
	}
	return DirectTypeMagnitude(expr.return_type);
}

bool DirectProveValue(const DirectIR &ir, const Expression &expr) {
	return expr.return_type.IsNumeric() && DirectIsDecisionFreeDeterministic(expr, ir.decide_index) &&
	       DirectReferencesOnlySource(expr, ir.source_bindings);
}

} // namespace duckdb
