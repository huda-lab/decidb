//===----------------------------------------------------------------------===//
//                         DecidB
//
// duckdb/decidb/ilp_solver.hpp
//
// Single entry point for optimization solving. Builds a SolverModel from
// SolverInput, selects the best available backend (Gurobi > HiGHS), and
// returns the solution vector. Supports LP, MILP, and convex QP/MIQP.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/decidb/solver_input.hpp"
#include "duckdb/decidb/ilp_model.hpp"

namespace duckdb {

//! Builds a SolverModel from the given SolverInput, selects the best available
//! solver backend (Gurobi if licensed, otherwise HiGHS), solves, and returns
//! the solution vector of size (num_rows * num_decide_vars).
//!
//! `input` is taken by non-const reference because the raw global constraints
//! are moved (not copied) into the SolverModel during Build(). Callers must not
//! read `input.global_constraints` after this call returns.
//!
//! `indexer` is the VarIndexer constructed once in Finalize() and threaded
//! through here to avoid duplicate construction inside SolverModel::Build().
vector<double> SolveModel(SolverInput &input, const VarIndexer &indexer);

} // namespace duckdb
