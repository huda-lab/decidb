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
//! The pass changes no value a plan computes and no error it raises: each rule is one DuckDB applies to any query.
void DirectCanonicalize(Optimizer &optimizer, LogicalOperator &plan, const LogicalOperator &source);

} // namespace duckdb
