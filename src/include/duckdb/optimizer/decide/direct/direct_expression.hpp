#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/planner/column_binding.hpp"
#include "duckdb/planner/decide/decide_cast_policy.hpp"
#include "duckdb/planner/expression.hpp"

namespace duckdb {

class BoundCastExpression;
class BoundColumnRefExpression;
class ClientContext;
struct DirectPart;

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

//! True for a cast that can fail on some value: a narrowing cast that is not TRY_CAST. This node only, not its child.
bool DirectCastMayThrow(const BoundCastExpression &cast);

//! A deterministic, nonthrowing BOOLEAN expression over source columns only: a predicate that can be evaluated for
//! every row without changing which rows raise an error.
bool DirectIsSourceOnlyPredicate(const Expression &expr, idx_t decide_index,
                                 const vector<ColumnBinding> &source_bindings);

//! What a direct rule demands of a constant it reads with `TryEvaluateFoldableDouble` (`decide_cast_policy.hpp`): one
//! that no row, parameter or subquery can change, with an evaluation error read as "not a constant". The value may be
//! infinite or NaN; a rule that reads a Boolean adds `FOLD_ALLOW_BOOLEAN`.
constexpr uint32_t DIRECT_CONSTANT = FOLD_STABLE | FOLD_CATCH_DUCKDB_ERRORS;

//! The same, for a numeric constant that must also be finite and survive a round trip through DOUBLE.
constexpr uint32_t DIRECT_FINITE_CONSTANT = DIRECT_CONSTANT | FOLD_EXACT_FINITE;

//! The factor a query-wide `2 * SUM(...)` or `SUM(...) / 2` puts on a reducer: 1 without one. False unless it is a
//! finite foldable constant, and nonzero when it divides.
bool DirectPartScale(ClientContext &context, const DirectPart &part, double &scale);

//! The uncorrelated numeric column an expression is exactly, looking through nonthrowing numeric casts; null when
//! the expression is anything else.
const BoundColumnRefExpression *DirectBareNumericColumn(const Expression &expr);

//! A numeric, deterministic expression independent of the decisions: a coefficient a rule may evaluate per row. It
//! may still raise; check DirectMayThrow where the order of errors matters.
bool DirectIsNumericDecisionFree(const Expression &expr, idx_t decide_index);

//! The source column a data-valued bound is exactly, as DirectBareNumericColumn reads it: its slot in the source, its
//! type, and the name the user wrote, for an error that names it. False for anything else.
bool DirectSourceNumericColumn(const Expression &expr, const vector<ColumnBinding> &source_bindings, idx_t &slot,
                               LogicalType &type, string &name);

//! A data-valued bound a rule may validate and reduce per row (DirectValidateBounds): numeric, nonthrowing,
//! deterministic, decision-free, over source columns only, and mentioning at least one. A bound that can raise stays
//! a solver case until its error order is proved.
bool DirectIsSourceOnlyNumeric(const Expression &expr, idx_t decide_index,
                               const vector<ColumnBinding> &source_bindings);

//! A numeric or Boolean value a rule may read per row: deterministic, independent of the decisions, over source columns
//! only, with or without a column. Unlike DirectIsSourceOnlyNumeric it may raise when evaluated, for a rule that
//! evaluates it on every row (the solver raises on every row, including rows a WHEN excludes).
bool DirectIsSourceOnlyValue(const Expression &expr, idx_t decide_index, const vector<ColumnBinding> &source_bindings);

//! `x >= v`, `x > v`, and `x = v` all bound a count from below; `x <= v`, `x < v`, and `x = v` from above.
bool DirectIsLowerBound(ExpressionType comparison);
bool DirectIsUpperBound(ExpressionType comparison);

} // namespace duckdb
