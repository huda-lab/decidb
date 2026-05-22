#include "duckdb/decidb/ilp_solver.hpp"
#include "duckdb/decidb/ilp_model.hpp"
#include "duckdb/decidb/gurobi/gurobi_solver.hpp"
#include "duckdb/decidb/naive/deterministic_naive.hpp"
#include "duckdb/common/exception.hpp"

#include <cstdlib>
#include <string>

namespace duckdb {

vector<double> SolveModel(SolverInput &input, const VarIndexer &indexer) {
    SolverModel model = SolverModel::Build(input, indexer);

    // Test-only override: DECIDB_FORCE_SOLVER=highs|gurobi pins the backend.
    // Used by the DECIDE test suite (see test/decide/conftest.py fixtures
    // decidb_cli_highs / decidb_cli_gurobi) to exercise both backends on a
    // single host. Unknown values fall through to default auto-selection.
    if (const char *force = std::getenv("DECIDB_FORCE_SOLVER")) {
        std::string choice(force);
        if (choice == "highs" || choice == "HIGHS") {
            return DeterministicNaive::Solve(model);
        }
        if (choice == "gurobi" || choice == "GUROBI") {
            if (!GurobiSolver::IsAvailable()) {
                throw InvalidInputException(
                    "DECIDB_FORCE_SOLVER=gurobi but Gurobi is not available on this host");
            }
            return GurobiSolver::Solve(model);
        }
    }

    if (GurobiSolver::IsAvailable()) {
        return GurobiSolver::Solve(model);
    }
    return DeterministicNaive::Solve(model);
}

} // namespace duckdb
