#pragma once

#include "duckdb/optimizer/decide/direct/direct_rule.hpp"

namespace duckdb {

//! The first direct-solve rule: one row-scoped Boolean under cardinality-interval bounds (S1).
unique_ptr<DirectSolveRule> MakeS1CardinalityRule();

} // namespace duckdb
