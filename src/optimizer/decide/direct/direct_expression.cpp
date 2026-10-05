#include "duckdb/optimizer/decide/direct/direct_expression.hpp"

#include <algorithm>
#include <cmath>

#include "duckdb/common/exception.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/decide/decide_cast_policy.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"

namespace duckdb {

bool DirectIsDecisionFreeDeterministic(const Expression &expr, idx_t decide_index) {
	if (BoundExpressionReferencesDecide(expr, decide_index) || !expr.IsConsistent() || expr.IsVolatile() ||
	    expr.HasSubquery() || expr.HasParameter() || expr.IsAggregate() || expr.IsWindow()) {
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

void DirectCollectNullSources(const Expression &expr, const vector<ColumnBinding> &source_bindings,
                              vector<DirectNullSource> &out) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
		ExpressionIterator::EnumerateChildren(
		    expr, [&](const Expression &child) { DirectCollectNullSources(child, source_bindings, out); });
		return;
	}
	auto &ref = expr.Cast<BoundColumnRefExpression>();
	auto found = std::find(source_bindings.begin(), source_bindings.end(), ref.binding);
	if (ref.depth != 0 || found == source_bindings.end() || ref.GetAlias().empty()) {
		return;
	}
	for (auto &seen : out) {
		if (seen.name == ref.GetAlias()) {
			return;
		}
	}
	out.push_back({static_cast<idx_t>(found - source_bindings.begin()), ref.GetAlias()});
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

bool DirectMayThrow(const Expression &expr) {
	// TRY_CAST turns conversion failures into NULL; the solver validates that result on every row.
	// BoundCastExpression::CanThrow conservatively treats it like a regular narrowing cast.
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION &&
	    expr.Cast<BoundFunctionExpression>().function.errors == FunctionErrors::CAN_THROW_RUNTIME_ERROR) {
		return true;
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
		auto &cast = expr.Cast<BoundCastExpression>();
		if (!cast.try_cast && cast.return_type.id() != cast.child->return_type.id() &&
		    LogicalType::ForceMaxLogicalType(cast.return_type, cast.child->return_type) ==
		        cast.child->return_type.id()) {
			return true;
		}
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

bool DirectFiniteFoldableDouble(ClientContext &context, const Expression &expr, double &result) {
	if (!expr.IsFoldable() || !expr.IsConsistent() || expr.HasParameter() || expr.HasSubquery() || expr.IsAggregate() ||
	    expr.IsWindow()) {
		return false;
	}
	try {
		Value value;
		if (!ExpressionExecutor::TryEvaluateScalar(context, expr, value) || value.IsNull() ||
		    !value.type().IsNumeric()) {
			return false;
		}
		result = value.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
		return std::isfinite(result) && Value::DOUBLE(result).DefaultCastAs(value.type()) == value;
	} catch (Exception &) {
		return false;
	}
}

const BoundColumnRefExpression *DirectBareNumericColumn(const Expression &expr) {
	const Expression *current = &expr;
	while (current->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
		if (!current->return_type.IsNumeric() || current->CanThrow()) {
			return nullptr;
		}
		current = current->Cast<BoundCastExpression>().child.get();
	}
	if (current->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
		return nullptr;
	}
	auto &ref = current->Cast<BoundColumnRefExpression>();
	return ref.depth == 0 && ref.return_type.IsNumeric() ? &ref : nullptr;
}

bool DirectIsNumericDecisionFree(const Expression &expr, idx_t decide_index) {
	return expr.return_type.IsNumeric() && DirectIsDecisionFreeDeterministic(expr, decide_index);
}

bool DirectSourceNumericColumn(const Expression &expr, const vector<ColumnBinding> &source_bindings, idx_t &slot,
                               LogicalType &type, string &name) {
	auto ref = DirectBareNumericColumn(expr);
	if (!ref) {
		return false;
	}
	auto found = std::find(source_bindings.begin(), source_bindings.end(), ref->binding);
	if (found == source_bindings.end()) {
		return false;
	}
	slot = found - source_bindings.begin();
	type = ref->return_type;
	name = ref->GetAlias();
	return true;
}

bool DirectIsSourceOnlyNumeric(const Expression &expr, idx_t decide_index,
                               const vector<ColumnBinding> &source_bindings) {
	return expr.return_type.IsNumeric() && !DirectMayThrow(expr) &&
	       DirectIsDecisionFreeDeterministic(expr, decide_index) && DirectReferencesOnlySource(expr, source_bindings) &&
	       DirectHasColumnReference(expr);
}

bool DirectIsLowerBound(ExpressionType comparison) {
	return comparison == ExpressionType::COMPARE_GREATERTHANOREQUALTO ||
	       comparison == ExpressionType::COMPARE_GREATERTHAN || comparison == ExpressionType::COMPARE_EQUAL;
}

bool DirectIsUpperBound(ExpressionType comparison) {
	return comparison == ExpressionType::COMPARE_LESSTHANOREQUALTO || comparison == ExpressionType::COMPARE_LESSTHAN ||
	       comparison == ExpressionType::COMPARE_EQUAL;
}

} // namespace duckdb
