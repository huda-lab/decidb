//===----------------------------------------------------------------------===//
//                         DecidB
//
// duckdb/decidb/ilp_solver.hpp
//
// Single entry point for optimization solving. Builds a SolverModel from
// SolverInput, selects the best available backend (Gurobi > HiGHS), and
// returns a structured SolverResult. Supports LP, MILP, and convex QP/MIQP.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/decidb/solver_input.hpp"
#include "duckdb/decidb/ilp_model.hpp"
#include "duckdb/decidb/solver_result.hpp"

namespace duckdb {

//! Builds a SolverModel from the given SolverInput, selects the best available
//! solver backend (Gurobi if licensed, otherwise HiGHS), solves, and returns a
//! SolverResult: the terminal status plus, when optimal, the solution vector of
//! size (num_rows * num_decide_vars). A non-optimal status is returned, not
//! thrown — the operator decides whether to surface the default error or route
//! to diagnosis (manual-first).
//!
//! `input` is taken by non-const reference because the raw global constraints
//! are moved (not copied) into the SolverModel during Build(). Callers must not
//! read `input.global_constraints` after this call returns.
//!
//! `indexer` is the VarIndexer constructed once in Finalize() and threaded
//! through here to avoid duplicate construction inside SolverModel::Build().
SolverResult SolveModel(SolverInput &input, const VarIndexer &indexer);

} // namespace duckdb
