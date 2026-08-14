//===----------------------------------------------------------------------===//
//                         DecidB
//
// duckdb/decidb/parsed/decide_grammar_repair.hpp
//
// Post-parse repair of DECIDE clause association.
//===----------------------------------------------------------------------===//
//
// WHAT THIS IS. `WHEN` is a postfix operator DeciDB adds to DuckDB's expression
// grammar as `a_expr WHEN_DECIDE b_expr` (grammar/statements/select.y). `a_expr` is
// fully general and swallows `AND`; `b_expr` excludes it; and WHEN_DECIDE carries no
// declared precedence, so bison resolves the ambiguity by its defaults rather than by
// intent. Two shapes come out mis-associated:
//
//   SUCH THAT A AND B WHEN c   parses as  (A AND B) WHEN c   -- left side too greedy
//   MAXIMIZE  SUM(x) WHEN a > b parses as (SUM(x) WHEN a) > b -- right side too shy
//
// The functions here rebuild the tree the user meant. This is a PARSE repair, not
// algebra: nothing below moves a term across a comparison, folds a constant, or
// touches a coefficient. All of that belongs to DecideCanonicalizer, on the bound
// tree, at the single planning boundary.
//
// WHY IT IS HERE AND NOT IN THE GRAMMAR. It should be in the grammar. Fixing it
// there means giving WHEN_DECIDE an explicit precedence below the comparison
// operators and narrowing the nonterminal on its left, then deleting this file. The
// conflicts at grammar/grammar.y:9-19 are documented as deliberately shift-resolved,
// so that change needs its own regeneration and regression pass; it is filed
// separately rather than bundled into the canonicalization work.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/parsed_expression.hpp"

namespace duckdb {

//! Repair `A AND B WHEN c` / `A AND B PER col` association in a SUCH THAT tree.
//! WHEN and PER bind to the RIGHTMOST constraint, not to the whole conjunction.
unique_ptr<ParsedExpression> RepairDecideConstraintGrammar(const ParsedExpression &expr);

//! Repair `SUM(x) WHEN a > b` association in an objective: the comparison belongs
//! INSIDE the WHEN condition, not wrapped around the WHEN node.
unique_ptr<ParsedExpression> RepairDecideObjectiveGrammar(const ParsedExpression &expr);

//! Graphviz DOT rendering of a parsed expression tree. Debug-only; the `deb(...)`
//! call sites in bind_select_node.cpp are commented out by default.
string ExpressionToDot(const ParsedExpression &expr);

} // namespace duckdb
