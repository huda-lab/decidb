#pragma once

#include "duckdb/optimizer/decide/direct/direct_rule.hpp"

namespace duckdb {

//! The factory of every direct-solve rule. A new rule adds its declaration here and one line in direct_registry.cpp.

//! The first direct-solve rule: one row-scoped Boolean under uniform-matroid (cardinality) bounds (S1).
unique_ptr<DirectSolveRule> MakeS1UniformMatroidRule();

//! The second direct-solve rule: separable box-constrained decisions: independent row-scoped decisions, each under
//! bounds of its own, with an objective that is a sum of one linear or quadratic term per row (A1).
unique_ptr<DirectSolveRule> MakeA1SeparableBoxRule();

} // namespace duckdb
