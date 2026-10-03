#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/planner/column_binding.hpp"
#include "duckdb/planner/expression.hpp"

namespace duckdb {

class BoundColumnRefExpression;
class ClientContext;

//! Rule-independent questions about a bound expression. A rule asks them while proving that an expression may be
//! evaluated per source row; none of them knows which problem class it is serving.

//! True when the expression is deterministic and independent of the decision variables: it mentions no decision
//! column, is consistent and non-volatile, and has no subquery, parameter, aggregate, window, or correlated column.
bool DirectIsDecisionFreeDeterministic(const Expression &expr, idx_t decide_index);

//! True when every column reference is an uncorrelated column of the source plan.
bool DirectReferencesOnlySource(const Expression &expr, const vector<ColumnBinding> &source_bindings);

//! True when the expression mentions at least one column.
bool DirectHasColumnReference(const Expression &expr);

//! Re-points every source column reference at the same position in a projection over the source. Returns false,
//! leaving the expression partly rewritten, when a reference is not a source column.
bool DirectRemapSourceReferences(Expression &expr, const vector<ColumnBinding> &source_bindings,
                                 idx_t projection_index);

//! True when evaluating the expression can raise at run time. TRY_CAST turns a failed conversion into NULL, so it
//! does not count.
bool DirectMayThrow(const Expression &expr);

//! A deterministic, nonthrowing BOOLEAN expression over source columns only: a predicate that can be evaluated for
//! every row without changing which rows raise an error.
bool DirectIsSourceOnlyPredicate(const Expression &expr, idx_t decide_index,
                                 const vector<ColumnBinding> &source_bindings);

//! Evaluates a foldable numeric expression to a DOUBLE. False when it is not foldable, NULL, non-numeric, not
//! finite, or does not survive a round trip through DOUBLE.
bool DirectFiniteFoldableDouble(ClientContext &context, const Expression &expr, double &result);

//! The uncorrelated numeric column an expression is exactly, looking through nonthrowing numeric casts; null when
//! the expression is anything else.
const BoundColumnRefExpression *DirectBareNumericColumn(const Expression &expr);

//! `x >= v`, `x > v`, and `x = v` all bound a count from below; `x <= v`, `x < v`, and `x = v` from above.
bool DirectIsLowerBound(ExpressionType comparison);
bool DirectIsUpperBound(ExpressionType comparison);

} // namespace duckdb
