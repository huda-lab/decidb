//===----------------------------------------------------------------------===//
//                         DecidB
//
// duckdb/planner/decide/decide_tree_nodes.hpp
//
// The facts of a reducer and of a computed value read whole.
//===----------------------------------------------------------------------===//
//
// A reducer (`sum`, `avg`, `min`, `max`, `count`, `norm`) holds its facts as fields
// (BoundDecideReducerExpression); what reads them off any expression is here. A computed value read whole is a
// flattened subquery or the column of a reducer over data the bound-side rewrite recorded.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/expression.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_decide_reducer_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// A reducer
//===--------------------------------------------------------------------===//
//! Whether `expr` is a norm no definition has replaced (a reducer with no function), and its order.
bool IsDecideNormMarker(const Expression &expr, DecideNorm &norm);
bool IsDecideNormMarker(const Expression &expr);

//! Whether two nodes agree on every fact Expression::Equals leaves out: their names' tags and, for two reducers,
//! their facts (BoundDecideReducerExpression::FactsEqual). On a decision-bearing node these are structure (a
//! reducer's `by` and `per`).
bool DecideFactsEqual(const Expression &a, const Expression &b);

//===--------------------------------------------------------------------===//
// A reducer's NULL tests
//===--------------------------------------------------------------------===//
//! `value IS NOT NULL`, as a test a reducer's rows gain beside its written `when` (BoundDecideNullTestExpression).
unique_ptr<Expression> MakeDecideNullTest(unique_ptr<Expression> value);

//===--------------------------------------------------------------------===//
// The name a call was written under
//===--------------------------------------------------------------------===//
//! DuckDB registers some functions under several names (`abs` for `@`; `pow`, `power` and `^` for `**`), and the
//! binder renames each such call in a DECIDE clause to its primary (RespellDecideAliases), so every check matches one
//! name. The call keeps the name the user wrote, which a plan and a message print. Display only: two calls that
//! differ in it are equal. A reducer's is BoundDecideReducerExpression::WrittenName.
inline const string &DecideWrittenName(const BoundFunctionExpression &call) {
	return call.written_name.empty() ? call.function.name : call.written_name;
}

//===--------------------------------------------------------------------===//
// A computed value read whole
//===--------------------------------------------------------------------===//
//! A value the query computes and a clause reads whole, by the expression that reads it (BoundDecide::computed_values).
//! Shape alone cannot tell it from row data: once PlanSubqueries has run, `(SELECT max(w) FROM p)` is a column like
//! `weight`. The table holds the fact and DuckDB's passes rebind `value` with the clauses
//! (BoundDecide::EnumerateExpressions), so a read of the value equals it wherever a rewrite copies it.
struct DecideComputedValue {
	//! What a clause reads: the column a flattened scalar subquery became, or the placeholder of a recorded reducer
	//! (DecideBoundAggregate::column).
	unique_ptr<Expression> value;
	//! One value for the whole query: an uncorrelated subquery, and an objective's reducer over data with no `by`. A
	//! correlated subquery is a value per row.
	bool query_wide = false;
	//! A flattened scalar subquery, correlated or not. Its column is the planner's placeholder, not SQL the user can
	//! edit, so a message about a bound holding one states the evaluated number.
	bool subquery = false;

	//! Generated from `storage/serialization/nodes.json`.
	void Serialize(Serializer &serializer) const;
	static DecideComputedValue Deserialize(Deserializer &deserializer);
};

//! Whether `expr` is a computed value that is one value for the whole query.
bool IsDecideQueryWideValue(const vector<DecideComputedValue> &values, const Expression &expr);
//! Whether `expr` reads a flattened scalar subquery anywhere.
bool DecideHoldsSubqueryValue(const vector<DecideComputedValue> &values, const Expression &expr);

} // namespace duckdb
