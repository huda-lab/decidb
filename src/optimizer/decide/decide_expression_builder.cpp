#include "duckdb/optimizer/decide/decide_expression_builder.hpp"

#include "duckdb/common/decide_value_messages.hpp"
#include "duckdb/common/enums/decide.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"

namespace duckdb {

bool DecideIsLowerBound(ExpressionType comparison) {
	return comparison == ExpressionType::COMPARE_GREATERTHANOREQUALTO ||
	       comparison == ExpressionType::COMPARE_GREATERTHAN || comparison == ExpressionType::COMPARE_EQUAL;
}

bool DecideIsUpperBound(ExpressionType comparison) {
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
void DecideStripTags(Expression &expr) {
	expr.SetAlias(StripDecideTags(expr.GetAlias()));
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		expr.Cast<BoundFunctionExpression>().definition = DecideHelperRef();
	}
	ExpressionIterator::EnumerateChildren(expr, [](Expression &child) { DecideStripTags(child); });
}

ExprPtr ExpressionBuilder::Of(const Expression &expr) const {
	auto copy = expr.Copy();
	DecideStripTags(*copy);
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
	// Its callers name only built-in functions over arguments they typed themselves.
	D_ASSERT(!error.HasError() && expr);
	return Wrap(std::move(expr));
}
ExprPtr ExpressionBuilder::RaiseInvalidValue(DecideValueRole role, const string &written, optional_idx place) const {
	return RaiseInvalidValue(role, Constant(Value(written)), place);
}
ExprPtr ExpressionBuilder::RaiseInvalidValue(DecideValueRole role, const ExprPtr &written, optional_idx place) const {
	auto raise = Fn("__internal_decide_invalid_value", {Constant(Value::INTEGER(static_cast<int32_t>(role))), written});
	// The call carries its own place, as a cast does; the function reads it from its expression.
	raise->SetQueryLocation(place);
	return raise;
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
ExprPtr ExpressionBuilder::And(const ExprPtr &a, const ExprPtr &b) const {
	return If(a, b, Bool(false));
}
ExprPtr ExpressionBuilder::Not(const ExprPtr &a) const {
	return If(a, Bool(false), Bool(true));
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

} // namespace duckdb
