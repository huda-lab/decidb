#pragma once

#include "duckdb/decidb/solver_input.hpp"

namespace duckdb {

struct SolverModel;

class DeterministicNaive {
public:
    //! Solves the optimization problem using HiGHS.
    //! Takes a solver-agnostic SolverModel (already built from SolverInput).
    //! Returns the solution vector (size = num_rows * num_decide_vars).
    static vector<double> Solve(const SolverModel &model);
};

} // namespace duckdb
