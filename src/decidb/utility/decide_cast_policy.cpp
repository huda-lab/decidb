#include "duckdb/decidb/decide_cast_policy.hpp"

#include "duckdb/common/types/decimal.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"

namespace duckdb {

bool DecideHasFractionalResolution(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
		return true;
	case LogicalTypeId::DECIMAL:
		return DecimalType::GetScale(type) > 0;
	default:
		return false;
	}
}

bool DecidePreservesResolution(const LogicalType &from, const LogicalType &to) {
	if (!from.IsNumeric() || !to.IsNumeric()) {
		return false;
	}
	if (from == to) {
		return true;
	}

	// DOUBLE is the model's numeric domain -- solver_input.hpp hands the backend
	// `vector<double>` for every bound and coefficient, so a value arriving here was
	// always going to end up in a double. This is the edge of what DECIDE represents,
	// not a loss inside it.
	if (to.id() == LogicalTypeId::DOUBLE) {
		return true;
	}

	// A binary-floating source keeps its resolution nowhere else: FLOAT drops mantissa
	// bits, DECIMAL pins the value to a fixed number of places, and an integer type
	// rounds it. Each changes the number rather than its container.
	if (from.id() == LogicalTypeId::FLOAT || from.id() == LogicalTypeId::DOUBLE) {
		return false;
	}

	// A fractional source in an integral target rounds. `CAST(x AS INTEGER)` is
	// `round(x)` -- a step function over a decision, not a cast of one.
	if (DecideHasFractionalResolution(from) && !DecideHasFractionalResolution(to)) {
		return false;
	}

	// Both sides are integral or fixed-point: resolution survives as long as the target
	// still names the same decimal places. This is the test DuckDB itself applies to
	// DECIMAL pairs in BoundCastExpression::CastIsInvertible. Integral types answer
	// GetDecimalProperties with their equivalent width and a scale of 0, so this covers
	// integer -> DECIMAL and integer -> integer without a separate branch.
	uint8_t from_width;
	uint8_t from_scale;
	uint8_t to_width;
	uint8_t to_scale;
	if (from.GetDecimalProperties(from_width, from_scale) && to.GetDecimalProperties(to_width, to_scale)) {
		return to_scale >= from_scale;
	}
	return false;
}

//! Shared by both overloads: a cast is transparent only while it keeps the value.
static bool PeelsThrough(const Expression &expr) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_CAST) {
		return false;
	}
	auto &cast = expr.Cast<BoundCastExpression>();
	return DecidePreservesResolution(cast.child->return_type, cast.return_type);
}

const Expression *UnwrapDecideCasts(const Expression &expr) {
	const Expression *current = &expr;
	while (PeelsThrough(*current)) {
		current = current->Cast<BoundCastExpression>().child.get();
	}
	return current;
}

Expression *UnwrapDecideCasts(Expression &expr) {
	Expression *current = &expr;
	while (PeelsThrough(*current)) {
		current = current->Cast<BoundCastExpression>().child.get();
	}
	return current;
}

const Expression *StripCastsForIdentity(const Expression &expr) {
	const Expression *current = &expr;
	while (current->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
		current = current->Cast<BoundCastExpression>().child.get();
	}
	return current;
}

//! Lift every cast out of a tree the caller owns. Works top-down so a chain of casts
//! collapses before its child is visited.
static void StripCastsInPlace(unique_ptr<Expression> &expr) {
	while (expr->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
		expr = std::move(expr->Cast<BoundCastExpression>().child);
	}
	ExpressionIterator::EnumerateChildren(*expr,
	                                      [](unique_ptr<Expression> &child) { StripCastsInPlace(child); });
}

string DecideDisplayString(const Expression &expr) {
	auto copy = expr.Copy();
	StripCastsInPlace(copy);
	return copy->ToString();
}

} // namespace duckdb
