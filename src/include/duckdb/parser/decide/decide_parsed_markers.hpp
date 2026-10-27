//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/decide/decide_parsed_markers.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/parsed_expression.hpp"

namespace duckdb {

//! The parsed DECIDE markers are operator functions the transformer builds: a scope in
//! front of a body (SCOPE_WHEN_TAG, SCOPE_PER_TAG, SCOPE_PER_ROW_TAG), a reducer's `by`
//! (REDUCER_BY_TAG), the colon form (QUALIFIED_REDUCER_TAG) and the removed postfix forms
//! (WHEN_CONSTRAINT_TAG, PER_CONSTRAINT_TAG). The parser's refusal of the removed spellings
//! and the binder read them, and the reducer calls they attach to, through these.

//! `expr` is the marker `tag` with at least one child.
bool IsDecideMarker(const ParsedExpression &expr, const char *tag);

//! A scope written in front of a body: `when c:`, `per K:` or `per row:`.
bool IsDecideScopeMarker(const ParsedExpression &expr);

//! SUM, AVG, MIN or MAX: the reducers a scope may stand inside.
bool IsDecideAggregateName(const string &name);

//! A call to SUM, AVG, MIN or MAX.
bool IsScopedReducerCall(const ParsedExpression &expr);

//! A call a `by` may follow: a reducer, `norm` (lowered onto one, which keeps the key) or
//! `count` (D34, D35).
bool TakesReducerBy(const ParsedExpression &expr);

//! The call under a reducer's `by` and colon-form markers.
const ParsedExpression &UnwrapReducerMarkers(const ParsedExpression &expr);

//! A reducer argument's body under the scopes written in front of it.
const ParsedExpression &ReducerArgumentBody(const ParsedExpression &argument);

} // namespace duckdb
