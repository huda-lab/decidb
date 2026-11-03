//===----------------------------------------------------------------------===//
//                         DecidB
//
// duckdb/planner/decide/decide_reducer_path.hpp
//
// The path rule (language/guide.md §5.5): which rows a nested reducer reads.
//===----------------------------------------------------------------------===//
//
// A reducer inside another reads, at the row it stands for, the rows of the clause's
// kept input that every enclosing reducer's own `when` keeps and that agree with that
// row on every enclosing `by` key and on its own. An instance of it is therefore named
// by a tuple of key values, its path key, and no relation is ever built per scope.
//
// Three passes read a path and must read the same one: the canonicalizer, for the rows a
// nested reducer's variants are counted over; the bound-side rewrite, which records a
// nested reducer over data per group of its path key; and the value rewrite, which keys
// and filters a nested reducer's helper decision by it. So it is stated here, once,
// beside the canonicalizer that owns scope. A group of the path key that the rows keep
// no row of has no value, and no reader reads it (D68).
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/expression.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"

namespace duckdb {

struct DecideReducerPath {
	//! The clause's `when`, then each enclosing reducer's own `when`, outermost first.
	vector<const Expression *> conditions;
	//! Each enclosing reducer's `by` key, outermost first, into LogicalDecide::keys;
	//! INVALID_INDEX for `by ()`.
	vector<idx_t> keys;

	//! The path of the reducers in `reducer`'s argument: its own `when` and `by` added.
	DecideReducerPath Below(const BoundAggregateExpression &reducer) const;
	//! The rows a reducer on this path reads, as one condition; null for every row.
	unique_ptr<Expression> Rows() const;
	//! The keys a reducer on this path whose own `by` key is `own` groups by.
	vector<idx_t> KeysOf(idx_t own) const;
};

//! The `by` key index of `reducer`, or INVALID_INDEX for `by ()`.
idx_t DecideReducerGroupKey(const BoundAggregateExpression &reducer);

//! The key holding the elements of `path_keys`, each once (FindOrCreateDecideKey), written as
//! its parts are; INVALID_INDEX when every key is `by ()`.
idx_t FindOrCreateDecidePathKey(const vector<idx_t> &path_keys, vector<DecideKey> &keys);

//! The frame selector `expr` is the marker of -- the aggregate the binder binds a frame to,
//! tagged with its index into LogicalDecide::frames -- or false. A frame is a tuple term
//! read at another row (language/guide.md §5.6): no layer reads its marker as a reducer,
//! and its payload holds none (D80).
bool TryGetDecideFrame(const Expression &expr, idx_t &frame);
//! Whether `expr` holds a frame marker anywhere, itself included.
bool ContainsDecideFrame(const Expression &expr);
//! Expression::Equals, with every frame marker the same frame in both: a marker names its frame only by its tag, an
//! alias, which Equals ignores, so `at(previous: x)` and `at(next: x)` are otherwise one expression. Every DECIDE
//! comparison of trees that may hold a frame reads this one.
bool DecideExpressionsEqual(const Expression &a, const Expression &b);
//! What a message about `frame` reading nothing at some anchor (DecideFrameSpec::MaySelectNothing) ends on, in lower
//! case: the remedy, or for a range that may request no position, the cause, which no `else` cures.
string DecideFrameAbsenceRemedy(const DecideFrameSpec &frame);

} // namespace duckdb
