#include "duckdb/planner/expression/bound_decide_null_test_expression.hpp"

#include "duckdb/common/types/hash.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"

namespace duckdb {

BoundDecideNullTestExpression::BoundDecideNullTestExpression(unique_ptr<Expression> test_p)
    : Expression(ExpressionType::BOUND_DECIDE_NULL_TEST, ExpressionClass::BOUND_DECIDE_NULL_TEST, LogicalType::BOOLEAN),
      test(std::move(test_p)) {
}

bool BoundDecideNullTestExpression::KeepsEveryRow() const {
	if (test->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
		return false;
	}
	auto &verdict = test->Cast<BoundConstantExpression>().value;
	return verdict.type().id() == LogicalTypeId::BOOLEAN && !verdict.IsNull() && verdict.GetValue<bool>();
}

bool BoundDecideNullTestExpression::IsFoldable() const {
	return false;
}

string BoundDecideNullTestExpression::ToString() const {
	return test->ToString();
}

bool BoundDecideNullTestExpression::Equals(const BaseExpression &other_p) const {
	return Expression::Equals(other_p) &&
	       Expression::Equals(*test, *other_p.Cast<BoundDecideNullTestExpression>().test);
}

unique_ptr<Expression> BoundDecideNullTestExpression::Copy() const {
	auto copy = make_uniq<BoundDecideNullTestExpression>(test->Copy());
	copy->CopyProperties(*this);
	return std::move(copy);
}

} // namespace duckdb
