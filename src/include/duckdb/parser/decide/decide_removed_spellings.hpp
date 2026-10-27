//===----------------------------------------------------------------------===//
//                         DuckDB / DeciDB
//
// duckdb/parser/decide/decide_removed_spellings.hpp
//
// The removed DECIDE spellings (language/errors.md, "Removed spellings"): the
// postfix `C WHEN c`, `C PER g`, `SUM(e) WHEN c`, `MAXIMIZE E WHEN c`,
// `MINIMIZE SUM(e) PER g`, and the declarations `T.x(TYPE)` and `scalar x(TYPE)`.
// The grammar still parses them, so that the error can name the spelling that
// replaces each one, built from the user's own expressions.
//===----------------------------------------------------------------------===//

#pragma once

namespace duckdb {

class SelectNode;

//! Throw a ParserException for the first removed spelling in a parsed DECIDE clause.
//! Every message carries the phrase "is removed" and the new spelling. The transformer
//! calls it on every DECIDE query it builds; the binder calls it on a parse tree it
//! receives already built (a view or a macro stored before the spellings were removed).
void RefuseRemovedDecideSpellings(const SelectNode &node);

} // namespace duckdb
