//===----------------------------------------------------------------------===//
//                         DecidB
//
// duckdb/planner/decide/decide_tree_nodes.hpp
//
// The facts of a reducer and of a computed value read whole.
//===----------------------------------------------------------------------===//
//
// A reducer (`sum`, `avg`, `min`, `max`, `count`, `norm`) is a bound aggregate that holds more than SQL's: its `by`
// key, its own `per`, a norm's order, the name it was written under, and what the rewrites record on it. A computed
// value read whole is a flattened subquery or the column of a reducer over data the bound-side rewrite recorded.
// Every reader and writer of these facts goes through this file.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/expression.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

namespace duckdb {

//! The order of `norm(e, p)` and, for `norm(e, 0, M)`, its bound.
struct DecideNorm {
	enum class Order : uint8_t { NONE, L0, L1, L2, INF };

	Order order = Order::NONE;
	//! The M of an L0 norm as written; 0 for the bound stage 06 derives.
	double bound = 0;

	explicit operator bool() const {
		return order != Order::NONE;
	}
	//! Whether the norm counts the nonzero values of its body.
	bool IsCount() const {
		return order == Order::L0;
	}
	//! The arguments after the body as the user writes them: `1`, `2`, `'inf'` (a string literal: `inf` alone names
	//! a column), `0`, or `0, <M>`.
	string Written() const;
};

//===--------------------------------------------------------------------===//
// A reducer
//===--------------------------------------------------------------------===//
//! The `by` key of `reducer`, an index into LogicalDecide::keys: the rows it reads at a class are the kept rows
//! sharing the class's key value. INVALID_INDEX for `by ()`, every kept row.
idx_t DecideReducerGroupKey(const BoundAggregateExpression &reducer);
void SetDecideReducerGroupKey(BoundAggregateExpression &reducer, idx_t key);

//! The keyed scope of a reducer's own `per`, `sum(per K: e)`: an index into LogicalDecide::keyed_scopes, whose entry
//! supplies the classes the reducer takes one value from. False for a reducer with none.
bool DecideReducerPer(const BoundAggregateExpression &reducer, idx_t &keyed_scope);
bool DecideReducerHasPer(const BoundAggregateExpression &reducer);
void SetDecideReducerPer(BoundAggregateExpression &reducer, idx_t keyed_scope);

//! Whether `expr` is a norm no definition has replaced, and its order. Canonicalization replaces the orders 1, 2 and
//! 'inf', and an L0 norm of data, by their definitions; DecideOptimizer::RewriteNorm lowers an L0 norm of a decision.
bool IsDecideNormMarker(const Expression &expr, DecideNorm &norm);
bool IsDecideNormMarker(const Expression &expr);
void SetDecideNormMarker(BoundAggregateExpression &reducer, const DecideNorm &norm);
void ClearDecideNormMarker(BoundAggregateExpression &reducer);

//! The norm the definition `reducer` was written as (`SUM(ABS(e))` for 1, `SUM(POWER(e, 2))` for 2, `MAX(ABS(e))`
//! for 'inf'); no order for any other reducer. The tree means what it says: this lets the canonical text read as
//! written, until RewriteNorm clears it.
DecideNorm DecideWrittenNorm(const BoundAggregateExpression &reducer);
void SetDecideWrittenNorm(BoundAggregateExpression &reducer, const DecideNorm &norm);

//! Whether `reducer` is the SUM an AVG became (DecideOptimizer's AVG rewrite): its terms scale by its row count.
bool DecideReducerCountScale(const BoundAggregateExpression &reducer);
void SetDecideReducerCountScale(BoundAggregateExpression &reducer);

//! Whether `reducer` is an objective's reducer over a decision reducer: the canonicalizer sets it once, and
//! DecideOptimizer::RewriteScopedReducers replaces the reducer by a helper decision.
bool DecideReducerHoldsReducer(const Expression &reducer);
void SetDecideReducerHoldsReducer(Expression &reducer, bool holds);

//! A reducer inlined in place of a nest (InlineSumOfGroupSums) names the nest the tree lowers for it, an index into
//! LogicalDecide::retained_expressions.
bool DecideRetainedExpression(const Expression &expr, idx_t &retained);
void SetDecideRetainedExpression(Expression &expr, idx_t retained);

//! The keyed scope of a frame aggregate's `per`, which the tree reads and term extraction does not.
bool DecideTreeReducerPer(const BoundAggregateExpression &reducer, idx_t &keyed_scope);
void SetDecideTreeReducerPer(BoundAggregateExpression &reducer, idx_t keyed_scope);

//! Whether two nodes agree on every fact their names hold, the name a call was written under apart. On a
//! decision-bearing node these are structure (a reducer's `by` and `per`), which Expression::Equals does not compare.
bool DecideFactsEqual(const Expression &a, const Expression &b);

//===--------------------------------------------------------------------===//
// A reducer's NULL tests
//===--------------------------------------------------------------------===//
//! `value IS NOT NULL`, as a test a reducer's rows gain beside its written `when` (BoundDecideNullTestExpression).
unique_ptr<Expression> MakeDecideNullTest(unique_ptr<Expression> value);

//===--------------------------------------------------------------------===//
// The name a call was written under
//===--------------------------------------------------------------------===//
//! DuckDB registers some functions under several names (`abs` for `@`; `pow`, `power` and `^` for `**`; `mean` for
//! `avg`), and the binder renames each such call in a DECIDE clause to its primary (RespellDecideAliases), so every
//! check matches one name. The call keeps the name the user wrote, which a plan and a message print. Display only:
//! two calls that differ in it are equal.
void SetDecideWrittenName(BoundAggregateExpression &call, const string &name);
inline const string &DecideWrittenName(const BoundFunctionExpression &call) {
	return call.written_name.empty() ? call.function.name : call.written_name;
}
string DecideWrittenName(const BoundAggregateExpression &call);

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
