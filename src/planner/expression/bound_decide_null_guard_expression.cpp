#include "duckdb/planner/expression/bound_decide_null_guard_expression.hpp"

#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"

namespace duckdb {

BoundDecideNullGuardExpression::BoundDecideNullGuardExpression(unique_ptr<Expression> guard_p)
    : Expression(ExpressionType::BOUND_DECIDE_NULL_GUARD, ExpressionClass::BOUND_DECIDE_NULL_GUARD,
                 guard_p->return_type),
      guard(std::move(guard_p)) {
}

const Expression &BoundDecideNullGuardExpression::Value() const {
	if (guard->GetExpressionClass() == ExpressionClass::BOUND_CASE) {
		return *guard->Cast<BoundCaseExpression>().case_checks[0].then_expr;
	}
	D_ASSERT(guard->type == ExpressionType::OPERATOR_COALESCE);
	return *guard->Cast<BoundOperatorExpression>().children[0];
}

string BoundDecideNullGuardExpression::ToString() const {
	return guard->ToString();
}

bool BoundDecideNullGuardExpression::Equals(const BaseExpression &other_p) const {
	return Expression::Equals(other_p) &&
	       Expression::Equals(*guard, *other_p.Cast<BoundDecideNullGuardExpression>().guard);
}

unique_ptr<Expression> BoundDecideNullGuardExpression::Copy() const {
	auto copy = make_uniq<BoundDecideNullGuardExpression>(guard->Copy());
	copy->CopyProperties(*this);
	return std::move(copy);
}

} // namespace duckdb
