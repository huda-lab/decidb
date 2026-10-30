//===----------------------------------------------------------------------===//
//                         DecidB
//
// duckdb/planner/decide/decide_reducer_path.hpp
//
// The path rule (syntax_reference.md §5.5): which rows a nested reducer reads.
//===----------------------------------------------------------------------===//
//
// A reducer inside another reads, at the row it stands for, the rows of the clause's
// kept input that every enclosing reducer's own `when` keeps and that agree with that
// row on every enclosing `by` key and on its own. An instance of it is therefore named
// by a tuple of key values, its path key, and no relation is ever built per scope.
//
// Every pass that reads a path must read the same one: the canonicalizer, for a nested
// reducer's D8 check, the rows its variants are counted over and where its frames stand;
// the value rewrite, which keys and filters a nested reducer's helper decision by it; and
// the rewrites that add rows for a construct inside a reducer, which read the reducer's
// member rows (the ABS envelope) and name the reducer whose members they define
// (`innermost`). So it is stated here, once, beside the canonicalizer that owns scope.
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
	//! Each enclosing reducer's `by` key, outermost first, into LogicalDecide::group_keys;
	//! INVALID_INDEX for `by ()`.
	vector<idx_t> keys;

	//! The path of the reducers in `reducer`'s argument: its own `when` and `by` added.
	DecideReducerPath Below(const BoundAggregateExpression &reducer) const;
	//! The rows a reducer on this path reads, as one condition; null for every row.
	unique_ptr<Expression> Rows() const;
	//! The keys a reducer on this path whose own `by` key is `own` groups by.
	vector<idx_t> KeysOf(idx_t own) const;

	//! The innermost reducer of the path (the last `Below`), or null at a comparison's root.
	//! Valid while the tree it was read from stands.
	const BoundAggregateExpression *innermost = nullptr;
};

//! The `by` key index of `reducer`, or INVALID_INDEX for `by ()`.
idx_t DecideReducerGroupKey(const BoundAggregateExpression &reducer);

//! The frame selector `expr` is the marker of -- the aggregate the binder binds a frame to,
//! tagged with its index into LogicalDecide::frames -- or false. A frame is a tuple term
//! read at another row (syntax_reference.md §5.6): no layer reads its marker as a reducer,
//! and its payload holds none (D74).
bool TryGetDecideFrame(const Expression &expr, idx_t &frame);
//! Whether `expr` holds a frame marker anywhere, itself included.
bool ContainsDecideFrame(const Expression &expr);
//! What a message about `frame` reading nothing at some anchor (DecideFrameSpec::
//! MaySelectNothing) ends on, in lower case: the remedy, or for a range that may request
//! no position, the cause, which no `else` cures.
string DecideFrameAbsenceRemedy(const DecideFrameSpec &frame);
//! The frames that drop a member of `reducer` where they select nothing (syntax_reference.md
//! §5.6): each frame in its argument, outside the reducers nested in it, that may select no
//! position (DecideFrameSpec::MaySelectNothing), appended to `out` in written order.
void DecideMemberFrames(const BoundAggregateExpression &reducer, const vector<DecideFrameSpec> &frames,
                        vector<idx_t> &out);
//! Tags every reducer in `expr` with its member frames (MEMBER_FRAME_TAG_PREFIX, one tag each):
//! the one record of DecideMemberFrames every layer reads. Run on the bound tree before
//! canonicalization, which decides shape off the tags and keeps a reducer's alias.
void TagDecideMemberFrames(Expression &expr, const vector<DecideFrameSpec> &frames);

//! The expressions of `keys`, each once, with their names when `names` is given.
vector<unique_ptr<Expression>> DecidePathKeyExpressions(const vector<idx_t> &keys,
                                                        const vector<DecideGroupKey> &group_keys,
                                                        vector<string> *names = nullptr);

//! The group key holding the expressions of `keys`, each once: an existing key equal to
//! them as a set, else a new one appended to `group_keys`, written as its parts are.
//! INVALID_INDEX when every key is `by ()`.
idx_t FindOrCreateDecidePathKey(const vector<idx_t> &keys, vector<DecideGroupKey> &group_keys);

} // namespace duckdb
