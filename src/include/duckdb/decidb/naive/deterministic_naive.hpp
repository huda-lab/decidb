#pragma once

#include "duckdb/decidb/solver_input.hpp"
#include "duckdb/decidb/solver_result.hpp"

namespace duckdb {

struct SolverModel;

class DeterministicNaive {
public:
    //! Solves the optimization problem using HiGHS.
    //! Takes a solver-agnostic SolverModel (already built from SolverInput).
    //! Returns a SolverResult carrying the terminal status and, when optimal,
    //! the solution vector (size = num_rows * num_decide_vars). Non-optimal
    //! statuses are returned, not thrown; only genuine internal errors throw.
    static SolverResult Solve(const SolverModel &model);
};

} // namespace duckdb
