//===----------------------------------------------------------------------===//
//                         DeciDB
//
// duckdb/optimizer/decide/decide_expression_builder.hpp
//
// Building bound expressions as values, for every DECIDE pass that writes per-row algebra: the bound-side rewrite, the
// ABS rewrite, singleton-row folding and the direct-solve rules.
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>

#include "duckdb/common/common.hpp"
#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/planner/column_binding.hpp"
#include "duckdb/planner/expression.hpp"

namespace duckdb {

class ClientContext;
enum class DecideValueRole : uint8_t;

//! Unsets the definition every kept call in `expr` names (BoundFunctionExpression::definition): the copy is a plain
//! call, read by none of the definition's readers.
void DecideForgetDefinitions(Expression &expr);

//! `x >= v`, `x > v`, and `x = v` all bound a count from below; `x <= v`, `x < v`, and `x = v` from above.
bool DecideIsLowerBound(ExpressionType comparison);
bool DecideIsUpperBound(ExpressionType comparison);

//! Expressions as values, so the per-row algebra reads like the algebra. Each operation copies its operands.
using ExprPtr = std::shared_ptr<Expression>;

class ExpressionBuilder {
public:
	explicit ExpressionBuilder(ClientContext &context_p);

	ExprPtr Wrap(unique_ptr<Expression> expr) const;
	unique_ptr<Expression> Out(const ExprPtr &expr) const;
	//! A copy of an existing expression, to combine with others.
	ExprPtr Of(const Expression &expr) const;
	ExprPtr Num(double value) const;
	ExprPtr Bool(bool value) const;
	//! A constant of exactly this value and type (`Num` always makes a DOUBLE). A NULL of a type is `Value(type)`.
	ExprPtr Constant(const Value &value) const;
	ExprPtr Col(const LogicalType &type, ColumnBinding binding) const;
	ExprPtr Fn(const string &name, const ExprPtr &a) const;
	ExprPtr Fn(const string &name, const ExprPtr &a, const ExprPtr &b) const;
	//! A scalar function of any number of arguments.
	ExprPtr Fn(const string &name, const vector<ExprPtr> &args) const;
	//! The call that raises the error of a NaN or infinite value (DecideInvalidValueError), `written` as the user wrote
	//! it, at `place` in the query (invalid for none), which the call carries as its own query location: NULL where it
	//! is not reached, so it stands as the branch of a CASE that tests the value.
	ExprPtr RaiseInvalidValue(DecideValueRole role, const string &written, optional_idx place) const;
	//! The same for a `written` computed at run time (an answer's value), a VARCHAR expression.
	ExprPtr RaiseInvalidValue(DecideValueRole role, const ExprPtr &written, optional_idx place) const;
	//! `a IS NULL`.
	ExprPtr IsNull(const ExprPtr &a) const;
	//! `a` converted to `type`; `a` itself when it already has that type.
	ExprPtr Cast(const ExprPtr &a, const LogicalType &type) const;
	//! The first value that is not NULL, of the first value's type.
	ExprPtr Coalesce(const vector<ExprPtr> &values) const;
	ExprPtr Add(const ExprPtr &a, const ExprPtr &b) const;
	ExprPtr Sub(const ExprPtr &a, const ExprPtr &b) const;
	ExprPtr Mul(const ExprPtr &a, const ExprPtr &b) const;
	ExprPtr Div(const ExprPtr &a, const ExprPtr &b) const;
	ExprPtr Neg(const ExprPtr &a) const;
	ExprPtr Cmp(ExpressionType type, const ExprPtr &a, const ExprPtr &b) const;
	ExprPtr If(const ExprPtr &condition, const ExprPtr &then_value, const ExprPtr &else_value) const;
	ExprPtr Min(const vector<ExprPtr> &values) const;
	ExprPtr Max(const vector<ExprPtr> &values) const;
	//! `b` where `a` is true, and false elsewhere (a NULL `a` is false), as a CASE: `b` is evaluated only where `a`
	//! holds.
	ExprPtr And(const ExprPtr &a, const ExprPtr &b) const;
	//! True unless `a` is true (a NULL `a` gives true).
	ExprPtr Not(const ExprPtr &a) const;
	//! True when any condition is true; false for none.
	ExprPtr Any(const vector<ExprPtr> &conditions) const;

private:
	ClientContext &context;

	ExprPtr Extremum(const char *name, const vector<ExprPtr> &values) const;
};

} // namespace duckdb
