#include "duckdb/decidb/ilp_solver.hpp"
#include "duckdb/decidb/ilp_model.hpp"
#include "duckdb/decidb/gurobi/gurobi_solver.hpp"
#include "duckdb/decidb/naive/deterministic_naive.hpp"
#include "duckdb/common/exception.hpp"

#include <cstdlib>
#include <string>

namespace duckdb {

SolverResult SolveModel(SolverInput &input, const VarIndexer &indexer) {
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

void ThrowDecideSolveError(const SolverResult &result) {
    switch (result.status) {
    case SolverStatus::INFEASIBLE:
        throw InvalidInputException(
            "DECIDE optimization is infeasible: No valid solution exists that satisfies all constraints.\n\n"
            "This means the SUCH THAT conditions cannot all be met simultaneously.\n\n"
            "Common causes:\n"
            "  • Contradictory bounds (e.g., x >= 10 AND x <= 5)\n"
            "  • SUM constraints impossible to satisfy with available data\n"
            "  • Variable types too restrictive (BOOLEAN when INTEGER needed)\n\n"
            "Suggestion: Try relaxing constraints or verify input data.");
    case SolverStatus::UNBOUNDED:
        throw InvalidInputException(
            "DECIDE optimization is unbounded: The objective can grow infinitely.\n\n"
            "This means the MAXIMIZE/MINIMIZE goal has no finite optimal value.\n"
            "You must add constraints to bound the decision variables.\n\n"
            "Examples:\n"
            "  • Add upper bounds: SUCH THAT x <= 100\n"
            "  • Add budget limits: SUCH THAT SUM(x * cost) <= budget\n"
            "  • Use BOOLEAN instead of INTEGER for selection problems");
    case SolverStatus::INF_OR_UNBD:
        throw InvalidInputException(
            "DECIDE optimization is infeasible or unbounded.\n\n"
            "Either the SUCH THAT conditions cannot all be met simultaneously,\n"
            "or the MAXIMIZE/MINIMIZE goal has no finite optimal value.\n\n"
            "Suggestion: Check for contradictory constraints, and ensure the\n"
            "decision variables are bounded (e.g., SUCH THAT x <= 100).");
    case SolverStatus::TIME_LIMIT:
        throw InvalidInputException(
            "DECIDE optimization exceeded time limit.\n"
            "The problem may be too complex to solve in reasonable time.\n"
            "Try simplifying constraints or reducing data size.");
    case SolverStatus::ITERATION_LIMIT:
        throw InvalidInputException(
            "DECIDE optimization exceeded iteration limit.\n"
            "The problem may be too complex. Try simplifying constraints.");
    case SolverStatus::OTHER:
    case SolverStatus::OPTIMAL:
        // OTHER, or OPTIMAL passed here in error: fall through to the generic
        // message. (The switch lists every enum value so -Wswitch flags any
        // future addition; control still reaches the throw below.)
        break;
    }
    throw InvalidInputException(
        "DECIDE optimization failed with solver status %d.\n"
        "The optimization could not find a solution.\n"
        "This may indicate a problem with the constraints or objective.",
        result.raw_status);
}

} // namespace duckdb
