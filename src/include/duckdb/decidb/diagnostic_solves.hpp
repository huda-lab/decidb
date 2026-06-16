//===----------------------------------------------------------------------===//
//                         DecidB
//
// duckdb/decidb/diagnostic_solves.hpp
//
// Helpers for diagnostic re-solves over prepared SolverModel objects.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/decidb/ilp_model.hpp"

namespace duckdb {

//! Return a copy of `model` with the objective replaced by the constant zero.
//! Constraints, bounds, integrality, and variables are preserved exactly.
SolverModel MakeZeroObjectiveProbeModel(const SolverModel &model);

} // namespace duckdb
