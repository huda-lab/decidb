#pragma once

#include "duckdb/optimizer/decide/direct/direct_rule.hpp"

namespace duckdb {

//! The second direct-solve rule: independent row-scoped decisions, each under bounds of its own, with an objective that
//! is a sum of one linear or quadratic term per row (A1).
unique_ptr<DirectSolveRule> MakeA1BoundedRule();

} // namespace duckdb
