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

#include "duckdb/planner/decide/decide_tree_nodes.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_decide_frame_expression.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"

namespace duckdb {

struct DecideReducerPath {
	//! The clause's `when`, then each enclosing reducer's own `when`, outermost first.
	vector<const Expression *> conditions;
	//! Each enclosing reducer's `by` key, outermost first, into LogicalDecide::keys;
	//! INVALID_INDEX for `by ()`.
	vector<idx_t> keys;

	//! The path of the reducers in `reducer`'s argument: its own `when` and `by` added.
	DecideReducerPath Below(const BoundDecideReducerExpression &reducer) const;
	//! The rows a reducer on this path reads, as one condition; null for every row.
	unique_ptr<Expression> Rows() const;
	//! The keys a reducer on this path whose own `by` key is `own` groups by.
	vector<idx_t> KeysOf(idx_t own) const;
};

//! The key holding the elements of `path_keys`, each once (FindOrCreateDecideKey), written as
//! its parts are; INVALID_INDEX when every key is `by ()`.
idx_t FindOrCreateDecidePathKey(const vector<idx_t> &path_keys, vector<DecideKey> &keys);

//! The frame selector `expr` is the marker of -- the node the binder binds a frame to,
//! which holds its index into LogicalDecide::frames -- or false. A frame is a tuple term
//! read at another row (language/guide.md §5.6): no layer reads its marker as a reducer,
//! and its payload holds none (D80).
bool TryGetDecideFrame(const Expression &expr, idx_t &frame);
//! Whether `expr` holds a frame marker anywhere, itself included.
bool ContainsDecideFrame(const Expression &expr);
//! The aggregate that folds a window of `frame`'s range over data: the range's reducer (DecideFrameSpec::reduce) over
//! what the frame node reads. Null for a frame whose record holds no reducer (a point, a range over decisions).
unique_ptr<Expression> DecideFrameFold(const DecideFrameSpec &spec, const BoundDecideFrameExpression &frame);

} // namespace duckdb
