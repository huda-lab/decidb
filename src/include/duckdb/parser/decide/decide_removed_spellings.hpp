//===----------------------------------------------------------------------===//
//                         DuckDB / DeciDB
//
// duckdb/parser/decide/decide_removed_spellings.hpp
//
// The removed DECIDE spellings (syntax_reference.md, "Removed spellings"): the
// postfix `C WHEN c`, `C PER g`, `SUM(e) WHEN c`, `MAXIMIZE E WHEN c`,
// `MINIMIZE SUM(e) PER g`, and the declarations `T.x(TYPE)` and `scalar x(TYPE)`.
// The grammar still parses them, so that the error can name the spelling that
// replaces each one, built from the user's own expressions.
//===----------------------------------------------------------------------===//

#pragma once

namespace duckdb {

class QueryNode;

//! Throw a ParserException for the first removed spelling in `node`'s own expressions and
//! DECIDE clause, if it has one; a query node inside it is refused on its own. Every
//! message carries the phrase "is removed" and the new spelling. The transformer calls it
//! on every query node it builds -- a subquery inside a DECIDE clause is lexed with the
//! clause's spellings, wherever in it they stand -- and the binder on a DECIDE query it
//! receives already built (a view or a macro stored before the spellings were removed).
void RefuseRemovedDecideSpellings(const QueryNode &node);

} // namespace duckdb
