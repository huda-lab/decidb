//===----------------------------------------------------------------------===//
//                         DeciDB
//
// duckdb/decidb/solver/probe_models.hpp
//
// Helpers for diagnostic re-solves over prepared SolverModel objects.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/decidb/formulation/ilp_model.hpp"

namespace duckdb {

//! Return a copy of `model` with the objective replaced by the constant zero.
//! Constraints, bounds, integrality, and variables are preserved exactly.
SolverModel MakeZeroObjectiveProbeModel(const SolverModel &model);

//! Build the portable fallback LP for extracting an unbounded ray:
//! maximize signed(c)^T d subject to homogenized linear rows, each d_j in [-1, 1] on the
//! sides column j's box leaves open and 0 on a bounded side.
//!
//! Returns false for model classes U2 intentionally does not support yet
//! (quadratic objective or quadratic constraints).
bool BuildUnboundedRayFallbackModel(const SolverModel &model, SolverModel &ray_model);

} // namespace duckdb
