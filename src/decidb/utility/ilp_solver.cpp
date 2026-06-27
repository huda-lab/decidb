#include "duckdb/decidb/ilp_solver.hpp"
#include "duckdb/decidb/diagnostic_solves.hpp"
#include "duckdb/decidb/diagnostic_constants.hpp"
#include "duckdb/decidb/ilp_model.hpp"
#include "duckdb/decidb/gurobi/gurobi_solver.hpp"
#include "duckdb/decidb/naive/deterministic_naive.hpp"
#include "duckdb/common/exception.hpp"

#include <cmath>
#include <cstdlib>
#include <string>
#include <utility>

namespace duckdb {

namespace {

double ComputeLinearObjectiveValue(const SolverModel &model, const vector<double> &solution) {
	if (solution.size() != model.obj_coeffs.size()) {
		return 0.0;
	}

	double objective = 0.0;
	for (idx_t col = 0; col < solution.size(); col++) {
		objective += model.obj_coeffs[col] * solution[col];
	}
	return objective;
}

void AttachUnboundedRayIfRequested(const SolverModel &model, SolverBackend backend,
                                   const SolveModelOptions &options, SolverResult &result) {
	if (!options.extract_unbounded_ray ||
	    (result.status != SolverStatus::UNBOUNDED && result.status != SolverStatus::INF_OR_UNBD)) {
		return;
	}

	SolverModel ray_model;
	if (!BuildUnboundedRayFallbackModel(model, ray_model)) {
		return;
	}

	SolverResult ray_result = SolvePreparedModel(ray_model, backend);
	if (ray_result.status != SolverStatus::OPTIMAL || ray_result.solution.size() != model.num_vars) {
		return;
	}

	double improvement = ComputeLinearObjectiveValue(ray_model, ray_result.solution);
	if (std::isfinite(improvement) && improvement > DIAGNOSTIC_RAY_EPSILON) {
		result.ray = std::move(ray_result.solution);
	}
}

} // namespace

SolverBackend SelectSolverBackend() {
    // Test-only override: DECIDB_FORCE_SOLVER=highs|gurobi pins the backend.
    // Used by the DECIDE test suite (see test/decide/conftest.py fixtures
    // decidb_cli_highs / decidb_cli_gurobi) to exercise both backends on a
    // single host. Unknown values fall through to default auto-selection.
    if (const char *force = std::getenv("DECIDB_FORCE_SOLVER")) {
        std::string choice(force);
        if (choice == "highs" || choice == "HIGHS") {
            return SolverBackend::HIGHS;
        }
        if (choice == "gurobi" || choice == "GUROBI") {
            if (!GurobiSolver::IsAvailable()) {
                throw InvalidInputException(
                    "DECIDB_FORCE_SOLVER=gurobi but Gurobi is not available on this host");
            }
            return SolverBackend::GUROBI;
        }
    }

    if (GurobiSolver::IsAvailable()) {
        return SolverBackend::GUROBI;
    }
    return SolverBackend::HIGHS;
}

SolverResult SolvePreparedModel(const SolverModel &model, SolverBackend backend) {
    switch (backend) {
    case SolverBackend::GUROBI:
        return GurobiSolver::Solve(model);
    case SolverBackend::HIGHS:
        return DeterministicNaive::Solve(model);
    }
    throw InternalException("Unknown DECIDE solver backend");
}

static SolverResult DisambiguateInfOrUnbd(const SolverModel &model, SolverBackend backend,
                                          const SolverResult &original) {
    if (original.status != SolverStatus::INF_OR_UNBD) {
        return original;
    }

    SolverModel probe_model = MakeZeroObjectiveProbeModel(model);
    SolverResult probe_result = SolvePreparedModel(probe_model, backend);

    SolverResult disambiguated = original;
    switch (probe_result.status) {
    case SolverStatus::OPTIMAL:
        disambiguated.status = SolverStatus::UNBOUNDED;
        return disambiguated;
    case SolverStatus::INFEASIBLE:
        disambiguated.status = SolverStatus::INFEASIBLE;
        return disambiguated;
    default:
        return original;
    }
}

SolverResult SolveModel(SolverInput &input, const VarIndexer &indexer,
                        const SolveModelOptions &options, SolverModel *retained_model) {
	SolverModel model;
	try {
		model = SolverModel::Build(input, indexer);
	} catch (const DecideInfeasibleModelException &) {
		// Infeasibility proven during Build: no model exists to retain.
		SolverResult result;
		result.status = SolverStatus::INFEASIBLE;
		return result;
	}
	SolverBackend backend = SelectSolverBackend();
	SolverResult result = SolvePreparedModel(model, backend);
	result = DisambiguateInfOrUnbd(model, backend, result);
	AttachUnboundedRayIfRequested(model, backend, options, result);
	// Hand the built model to a diagnosis engine if one asked for it (it is
	// otherwise discarded here). Done last: the helpers above still need it alive.
	if (retained_model) {
		*retained_model = std::move(model);
	}
	return result;
}

SolverResult SolveModel(SolverInput &input, const VarIndexer &indexer) {
	return SolveModel(input, indexer, SolveModelOptions());
}

void ThrowDecideSolveError(const SolverResult &result) {
    // User-facing failure text: one line naming the state + the smallest fix, no
    // solver/LP jargon, no bullet-list lectures. (UNBOUNDED here is the diagnosis-off
    // path — under auto the named engine runs instead — so it points back to the
    // pragma for the per-variable detail.)
    switch (result.status) {
    case SolverStatus::INFEASIBLE:
        throw InvalidInputException(
            "DECIDE optimization is infeasible: the SUCH THAT constraints cannot all be satisfied at once. "
            "Check for conflicting constraints or an unreachable SUM target.");
    case SolverStatus::UNBOUNDED:
        throw InvalidInputException(
            "DECIDE optimization is unbounded: a decision variable can grow without bound. "
            "Add an upper bound, e.g. SUCH THAT x <= <cap>. "
            "For the variable, set PRAGMA diagnose_decide='auto' and re-run.");
    case SolverStatus::INF_OR_UNBD:
        throw InvalidInputException(
            "DECIDE optimization is infeasible or unbounded: the constraints conflict, or a decision "
            "variable is unbounded. Add bounds, e.g. SUCH THAT x <= <cap>.");
    case SolverStatus::TIME_LIMIT:
        throw InvalidInputException(
            "DECIDE optimization hit the time limit. Simplify the constraints or reduce the input size.");
    case SolverStatus::ITERATION_LIMIT:
        throw InvalidInputException(
            "DECIDE optimization hit the iteration limit. Simplify the constraints.");
    case SolverStatus::OTHER:
    case SolverStatus::OPTIMAL:
        // OTHER, or OPTIMAL passed here in error: fall through to the generic
        // message. (The switch lists every enum value so -Wswitch flags any
        // future addition; control still reaches the throw below.)
        break;
    }
    throw InvalidInputException("DECIDE optimization failed (solver status %d).", result.raw_status);
}

} // namespace duckdb
