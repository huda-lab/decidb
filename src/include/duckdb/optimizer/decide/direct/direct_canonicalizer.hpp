#pragma once

#include "duckdb/common/common.hpp"

namespace duckdb {

class LogicalOperator;
class Optimizer;

//! Canonicalization after lowering, as a compiler's back end folds what lowering leaves (MLIR's canonicalize, LLVM's
//! InstCombine). The rules build their expressions generally, with a constant where a case does not arise (a flag that
//! is always true, a term of 0.0), and DuckDB's expression rewriter has run before the DECIDE optimizer, so nothing
//! simplifies them. This is one pass over the operators a rule built, in the order a plan is read, stopping at
//! `source`, the DECIDE input, which the user wrote and DuckDB has simplified already:
//!
//!  - constant-column propagation: a reference to a projection column that holds a constant becomes the constant in
//!    the operators above the projection, except across the NULL-padded side of an outer join, where the column is
//!    NULL for a row with no partner, and in the keys and conditions of a join or an aggregate, which stay columns;
//!  - then DuckDB's own rule set (`Optimizer::rewriter`) on the operator's expressions.
//!
//! Statistics propagation reads a constant column as its value too (`StatisticsPropagator::constant_columns`), but it
//! runs after DuckDB's rewriter and the removal of unused columns, so this pass is not redundant. Without it the
//! benchmark plans keep the arithmetic identities the rewriter removes once it sees the constant (`x * 1.0` over a
//! weight column that holds 1.0) and carry the constant columns up to the operators that read them as values.
//!
//! The pass changes no value a plan computes and no error it raises: each rule is one DuckDB applies to any query.
void DirectCanonicalize(Optimizer &optimizer, LogicalOperator &plan, const LogicalOperator &source);

} // namespace duckdb
