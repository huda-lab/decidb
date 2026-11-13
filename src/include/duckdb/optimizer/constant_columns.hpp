//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/constant_columns.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/value.hpp"
#include "duckdb/planner/column_binding.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"

namespace duckdb {

//! A constant column holds one value on every row. Each function takes the caller's `lookup`, a callable
//! `bool(ColumnBinding, Value &)` that returns the value of a column it proves constant: the statistics propagator
//! proves one from its statistics, the direct-solve constant propagation from the projections it has walked.

//! The value `expr` gives on every row, when it is a constant or a reference to a constant column.
template <class LOOKUP>
bool TryGetConstantValue(const Expression &expr, const LOOKUP &lookup, Value &value) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
		value = expr.Cast<BoundConstantExpression>().value;
		return true;
	}
	return expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF &&
	       lookup(expr.Cast<BoundColumnRefExpression>().binding, value);
}

//! Replace each child of `expr` that is a reference to a constant column by its value. A reference that is the
//! whole expression stays a column.
template <class LOOKUP>
void SubstituteConstantColumns(Expression &expr, const LOOKUP &lookup) {
	ExpressionIterator::EnumerateChildren(expr, [&](unique_ptr<Expression> &child) {
		Value value;
		if (child->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF &&
		    lookup(child->Cast<BoundColumnRefExpression>().binding, value)) {
			child = make_uniq<BoundConstantExpression>(std::move(value));
		}
	});
}

} // namespace duckdb
