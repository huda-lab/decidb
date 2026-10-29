#include "duckdb/optimizer/decide/direct/direct_expression.hpp"
#include "duckdb/optimizer/decide/direct/direct_rule.hpp"

#include <algorithm>
#include <cmath>

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
		if (found != columns.end()) {
			expr = values[NumericCast<idx_t>(found - columns.begin())]->Copy();
		}
		return;
	}
	ExpressionIterator::EnumerateChildren(
	    *expr, [&](unique_ptr<Expression> &child) { DirectReplaceColumns(child, columns, values); });
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

ExpressionBuilder::ExpressionBuilder(ClientContext &context_p) : context(context_p) {
}

ExprPtr ExpressionBuilder::Wrap(unique_ptr<Expression> expr) const {
	return ExprPtr(expr.release());
}
unique_ptr<Expression> ExpressionBuilder::Out(const ExprPtr &expr) const {
	return expr->Copy();
}
void DirectStripTags(Expression &expr) {
	expr.SetAlias(StripDecideTags(expr.GetAlias()));
	ExpressionIterator::EnumerateChildren(expr, [](Expression &child) { DirectStripTags(child); });
}

ExprPtr ExpressionBuilder::Of(const Expression &expr) const {
	auto copy = expr.Copy();
	DirectStripTags(*copy);
	return Wrap(std::move(copy));
}
ExprPtr ExpressionBuilder::Num(double value) const {
	return Wrap(make_uniq<BoundConstantExpression>(Value::DOUBLE(value)));
}
ExprPtr ExpressionBuilder::Bool(bool value) const {
	return Constant(Value::BOOLEAN(value));
}
ExprPtr ExpressionBuilder::Constant(const Value &value) const {
	return Wrap(make_uniq<BoundConstantExpression>(value));
}
ExprPtr ExpressionBuilder::Col(const LogicalType &type, ColumnBinding binding) const {
	return Wrap(make_uniq<BoundColumnRefExpression>(type, binding));
}
ExprPtr ExpressionBuilder::Fn(const string &name, const ExprPtr &a) const {
	return Fn(name, vector<ExprPtr> {a});
}
ExprPtr ExpressionBuilder::Fn(const string &name, const ExprPtr &a, const ExprPtr &b) const {
	return Fn(name, vector<ExprPtr> {a, b});
}
ExprPtr ExpressionBuilder::Fn(const string &name, const vector<ExprPtr> &args) const {
	vector<unique_ptr<Expression>> children;
	for (auto &arg : args) {
		children.push_back(arg->Copy());
	}
	FunctionBinder binder(context);
	ErrorData error;
	auto expr = binder.BindScalarFunction(DEFAULT_SCHEMA, name, std::move(children), error);
	// The direct-solve code names only built-in functions over arguments it typed itself.
	D_ASSERT(!error.HasError() && expr);
	return Wrap(std::move(expr));
}
ExprPtr ExpressionBuilder::IsNull(const ExprPtr &a) const {
	auto is_null = make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_IS_NULL, LogicalType::BOOLEAN);
	is_null->children.push_back(a->Copy());
	return Wrap(std::move(is_null));
}
ExprPtr ExpressionBuilder::Cast(const ExprPtr &a, const LogicalType &type) const {
	return Wrap(BoundCastExpression::AddCastToType(context, a->Copy(), type));
}
ExprPtr ExpressionBuilder::Coalesce(const vector<ExprPtr> &values) const {
	auto coalesce = make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_COALESCE, values[0]->return_type);
	for (auto &value : values) {
		coalesce->children.push_back(value->Copy());
	}
	return Wrap(std::move(coalesce));
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
	return values.size() == 1 ? values[0] : Fn(name, values);
}

bool DirectIsWholeNumbered(DirectDomain domain) {
	return domain != DirectDomain::REAL;
}

bool DirectProveValue(const DirectIR &ir, const Expression &expr) {
	return expr.return_type.IsNumeric() && DirectIsDecisionFreeDeterministic(expr, ir.decide_index) &&
	       DirectReferencesOnlySource(expr, ir.source_bindings);
}

} // namespace duckdb
