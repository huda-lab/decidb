//===----------------------------------------------------------------------===//
//                         DecidB
//
// duckdb/decidb/gurobi/gurobi_solver.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/decidb/solver_result.hpp"

namespace duckdb {

struct SolverModel;
class SolverSession;

class GurobiSolver {
public:
    //! Check if Gurobi is available at runtime (library linked + valid license)
    static bool IsAvailable();

    //! Solves the optimization problem using Gurobi.
    //! Takes a solver-agnostic SolverModel (already built from SolverInput).
    //! Returns a SolverResult carrying the terminal status and, when optimal,
    //! the solution vector (size = num_vars). Non-optimal statuses are returned,
    //! not thrown; only genuine internal/API errors throw.
    static SolverResult Solve(const SolverModel &model);

    //! Create a resumable Gurobi session (warm-continuation substrate). The
    //! single-shot Solve() above is a thin wrapper over one Solve() on a session.
    static unique_ptr<SolverSession> CreateSession();
};

} // namespace duckdb
