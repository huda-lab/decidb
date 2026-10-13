#include "duckdb/optimizer/decide/direct/direct_expression.hpp"
#include "duckdb/optimizer/decide/direct/direct_builder.hpp"
#include "duckdb/optimizer/decide/direct/direct_rule.hpp"

#include <algorithm>
#include <cmath>

#include "duckdb/common/decide_numeric_policy.hpp"
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
	return expr.return_type.IsNumeric() && !DirectMayThrow(expr) &&
	       DirectIsDecisionFreeDeterministic(expr, decide_index) && DirectReferencesOnlySource(expr, source_bindings) &&
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

Calc::Calc(Optimizer &optimizer_p) : optimizer(optimizer_p) {
}

Ex Calc::Wrap(unique_ptr<Expression> expr) const {
	return Ex(expr.release());
}
unique_ptr<Expression> Calc::Out(const Ex &expr) const {
	return expr->Copy();
}
Ex Calc::Num(double value) const {
	return Wrap(make_uniq<BoundConstantExpression>(Value::DOUBLE(value)));
}
Ex Calc::Bool(bool value) const {
	return Wrap(DirectConstantBool(value));
}
Ex Calc::Col(const LogicalType &type, ColumnBinding binding) const {
	return Wrap(DirectColumn(type, binding));
}
Ex Calc::Fn(const string &name, const Ex &a) const {
	return Wrap(optimizer.BindScalarFunction(name, a->Copy()));
}
Ex Calc::Fn(const string &name, const Ex &a, const Ex &b) const {
	return Wrap(optimizer.BindScalarFunction(name, a->Copy(), b->Copy()));
}
Ex Calc::Add(const Ex &a, const Ex &b) const {
	return Fn("+", a, b);
}
Ex Calc::Sub(const Ex &a, const Ex &b) const {
	return Fn("-", a, b);
}
Ex Calc::Mul(const Ex &a, const Ex &b) const {
	return Fn("*", a, b);
}
Ex Calc::Div(const Ex &a, const Ex &b) const {
	return Fn("/", a, b);
}
Ex Calc::Neg(const Ex &a) const {
	return Mul(a, Num(-1.0));
}
Ex Calc::Cmp(ExpressionType type, const Ex &a, const Ex &b) const {
	return Wrap(make_uniq<BoundComparisonExpression>(type, a->Copy(), b->Copy()));
}
Ex Calc::If(const Ex &condition, const Ex &then_value, const Ex &else_value) const {
	return Wrap(make_uniq<BoundCaseExpression>(condition->Copy(), then_value->Copy(), else_value->Copy()));
}
Ex Calc::Min(const vector<Ex> &values) const {
	return Extremum("least", values);
}
Ex Calc::Max(const vector<Ex> &values) const {
	return Extremum("greatest", values);
}
Ex Calc::Any(const vector<Ex> &conditions) const {
	Ex result = Bool(false);
	for (idx_t i = conditions.size(); i > 0; i--) {
		result = If(conditions[i - 1], Bool(true), result);
	}
	return result;
}

Ex Calc::Extremum(const char *name, const vector<Ex> &values) const {
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
