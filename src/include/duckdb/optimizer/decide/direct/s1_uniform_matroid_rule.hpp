#pragma once

#include "duckdb/optimizer/decide/direct/direct_rule.hpp"

namespace duckdb {

//! The first direct-solve rule: one row-scoped Boolean under uniform-matroid (cardinality) bounds (S1).
unique_ptr<DirectSolveRule> MakeS1UniformMatroidRule();

} // namespace duckdb
