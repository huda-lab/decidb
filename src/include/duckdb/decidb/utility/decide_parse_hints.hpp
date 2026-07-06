//===----------------------------------------------------------------------===//
//                         DuckDB / DeciDB
//
// duckdb/decidb/utility/decide_parse_hints.hpp
//
// Post-parse error augmentation for DECIDE clauses. The DECIDE grammar rejects
// unparenthesized WHEN conditions that use comparisons, NOT, or arithmetic
// (the condition grammar is restricted to avoid clashing with the constraint
// operator). Those failures surface as a bare bison "syntax error at or near
// <tok>" with no clue about the fix. This helper appends an actionable hint.
//===----------------------------------------------------------------------===//

#pragma once

#include <string>

namespace duckdb {

// Returns error_message unchanged unless it looks like a syntax error caused by
// an unparenthesized DECIDE WHEN condition, in which case a one-line hint about
// wrapping the condition in parentheses is appended. Safe to call on every
// parser syntax error — it no-ops for non-DECIDE queries.
std::string MaybeAppendDecideWhenHint(const std::string &query, const std::string &error_message);

} // namespace duckdb
