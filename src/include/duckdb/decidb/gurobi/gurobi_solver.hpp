//===----------------------------------------------------------------------===//
//                         DecidB
//
// duckdb/decidb/gurobi/gurobi_solver.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"

namespace duckdb {

struct SolverModel;

class GurobiSolver {
public:
    //! Check if Gurobi is available at runtime (library linked + valid license)
    static bool IsAvailable();

    //! Solves the optimization problem using Gurobi.
    //! Takes a solver-agnostic SolverModel (already built from SolverInput).
    //! Returns the solution vector (size = num_vars).
    static vector<double> Solve(const SolverModel &model);
};

} // namespace duckdb
