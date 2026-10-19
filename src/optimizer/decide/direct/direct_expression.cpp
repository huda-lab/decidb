#include "duckdb/optimizer/decide/direct/direct_expression.hpp"
#include "duckdb/optimizer/decide/direct/direct_plan_factory.hpp"
#include "duckdb/optimizer/decide/direct/direct_rule.hpp"

#include <algorithm>
#include <cmath>

#include "duckdb/common/decide_numeric_policy.hpp"
#include "duckdb/common/decide_value_messages.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/decide/decide_cast_policy.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
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

bool DirectCastMayThrow(const BoundCastExpression &cast) {
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

bool DirectPartScale(ClientContext &context, const DirectPart &part, double &scale) {
	scale = 1.0;
	return !part.scale || (TryEvaluateFoldableDouble(context, *part.scale, scale, DIRECT_FINITE_CONSTANT) &&
	                       !(part.scale_divides && scale == 0.0));
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
	return !DirectMayThrow(expr) && DirectIsSourceOnlyNumericMayRaise(expr, decide_index, source_bindings);
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

bool DirectIsLowerBound(ExpressionType comparison) {
	return comparison == ExpressionType::COMPARE_GREATERTHANOREQUALTO ||
	       comparison == ExpressionType::COMPARE_GREATERTHAN || comparison == ExpressionType::COMPARE_EQUAL;
}

bool DirectIsUpperBound(ExpressionType comparison) {
	return comparison == ExpressionType::COMPARE_LESSTHANOREQUALTO || comparison == ExpressionType::COMPARE_LESSTHAN ||
	       comparison == ExpressionType::COMPARE_EQUAL;
}

ExpressionBuilder::ExpressionBuilder(Optimizer &optimizer_p) : optimizer(optimizer_p) {
}

ExprPtr ExpressionBuilder::Wrap(unique_ptr<Expression> expr) const {
	return ExprPtr(expr.release());
}
unique_ptr<Expression> ExpressionBuilder::Out(const ExprPtr &expr) const {
	return expr->Copy();
}
ExprPtr ExpressionBuilder::Num(double value) const {
	return Wrap(make_uniq<BoundConstantExpression>(Value::DOUBLE(value)));
}
ExprPtr ExpressionBuilder::Bool(bool value) const {
	return Wrap(DirectConstantBool(value));
}
ExprPtr ExpressionBuilder::Col(const LogicalType &type, ColumnBinding binding) const {
	return Wrap(DirectColumn(type, binding));
}
ExprPtr ExpressionBuilder::Fn(const string &name, const ExprPtr &a) const {
	return Wrap(optimizer.BindScalarFunction(name, a->Copy()));
}
ExprPtr ExpressionBuilder::Fn(const string &name, const ExprPtr &a, const ExprPtr &b) const {
	return Wrap(optimizer.BindScalarFunction(name, a->Copy(), b->Copy()));
}
ExprPtr ExpressionBuilder::Add(const ExprPtr &a, const ExprPtr &b) const {
	return Fn("+", a, b);
}
ExprPtr ExpressionBuilder::Sub(const ExprPtr &a, const ExprPtr &b) const {
	return Fn("-", a, b);
}
ExprPtr ExpressionBuilder::Mul(const ExprPtr &a, const ExprPtr &b) const {
	return Fn("*", a, b);
}
ExprPtr ExpressionBuilder::Div(const ExprPtr &a, const ExprPtr &b) const {
	return Fn("/", a, b);
}
ExprPtr ExpressionBuilder::Neg(const ExprPtr &a) const {
	return Mul(a, Num(-1.0));
}
ExprPtr ExpressionBuilder::Cmp(ExpressionType type, const ExprPtr &a, const ExprPtr &b) const {
	return Wrap(make_uniq<BoundComparisonExpression>(type, a->Copy(), b->Copy()));
}
ExprPtr ExpressionBuilder::If(const ExprPtr &condition, const ExprPtr &then_value, const ExprPtr &else_value) const {
	return Wrap(make_uniq<BoundCaseExpression>(condition->Copy(), then_value->Copy(), else_value->Copy()));
}
ExprPtr ExpressionBuilder::Min(const vector<ExprPtr> &values) const {
	return Extremum("least", values);
}
ExprPtr ExpressionBuilder::Max(const vector<ExprPtr> &values) const {
	return Extremum("greatest", values);
}
ExprPtr ExpressionBuilder::Any(const vector<ExprPtr> &conditions) const {
	ExprPtr result = Bool(false);
	for (idx_t i = conditions.size(); i > 0; i--) {
		result = If(conditions[i - 1], Bool(true), result);
	}
	return result;
}

ExprPtr ExpressionBuilder::Extremum(const char *name, const vector<ExprPtr> &values) const {
	if (values.size() == 1) {
		return values[0];
	}
	vector<unique_ptr<Expression>> children;
	for (auto &value : values) {
		children.push_back(value->Copy());
	}
	return Wrap(optimizer.BindScalarFunction(name, std::move(children)));
}

} // namespace duckdb
