#include "duckdb/decidb/ilp_solver.hpp"
#include "duckdb/decidb/diagnostic_solves.hpp"
#include "duckdb/decidb/diagnostic_constants.hpp"
#include "duckdb/decidb/ilp_model.hpp"
#include "duckdb/decidb/solver_config.hpp"
#include "duckdb/decidb/solver_session.hpp"
#include "duckdb/decidb/gurobi/gurobi_solver.hpp"
#include "duckdb/decidb/naive/deterministic_naive.hpp"
#include "duckdb/common/exception.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
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

SolveModelOptions MakeDiagnosticSolveOptions(const SolveModelOptions &options) {
	SolveModelOptions diagnostic_options;
	diagnostic_options.time_limit_seconds = ResolveDecideDiagnosticTimeLimit(options.time_limit_seconds);
	diagnostic_options.interrupt_poll = options.interrupt_poll;
	return diagnostic_options;
}

bool DidDiagnosticSolveStopEarly(const SolverResult &result) {
	return result.status == SolverStatus::TIME_LIMIT;
}

void AttachUnboundedRayIfRequested(const SolverModel &model, SolverBackend backend,
                                   const SolveModelOptions &options,
                                   const SolveModelOptions &diagnostic_options,
                                   SolverResult &result) {
	if (!options.extract_unbounded_ray ||
	    (result.status != SolverStatus::UNBOUNDED && result.status != SolverStatus::INF_OR_UNBD)) {
		return;
	}

	SolverModel ray_model;
	if (!BuildUnboundedRayFallbackModel(model, ray_model)) {
		return;
	}

	SolverResult ray_result = SolvePreparedModel(ray_model, backend, diagnostic_options);
	if (DidDiagnosticSolveStopEarly(ray_result)) {
		result.diagnostic_timed_out = true;
		return;
	}
	if (ray_result.status != SolverStatus::OPTIMAL || ray_result.solution.size() != model.num_vars) {
		return;
	}

	double improvement = ComputeLinearObjectiveValue(ray_model, ray_result.solution);
	if (std::isfinite(improvement) && improvement > DIAGNOSTIC_RAY_EPSILON) {
		result.ray = std::move(ray_result.solution);
	}
}

//===--------------------------------------------------------------------===//
// Model dump (DECIDB_DUMP_MODEL)
//===--------------------------------------------------------------------===//
// Deterministic textual rendering of a built SolverModel, used as a
// characterization oracle. A change that claims not to alter the model is
// checked by diffing dumps rather than by comparing optimal values — two
// genuinely different models routinely share an optimum, so equal results are
// not evidence of an equal model.
//
//   DECIDB_DUMP_MODEL=1        → stderr
//   DECIDB_DUMP_MODEL=<path>   → appended to <path>
//
// Coefficients are sorted by column index within each row: term order inside a
// row carries no meaning to the solver, and canonicalization legitimately
// changes it (migrated terms are appended). Row order is left alone so that a
// genuine reordering stays visible in the diff.

void AppendDouble(std::string &out, double v) {
	if (std::isinf(v)) {
		out += v > 0 ? "inf" : "-inf";
		return;
	}
	if (std::isnan(v)) {
		out += "nan";
		return;
	}
	if (v == 0.0) {
		v = 0.0; // normalize -0.0 so it never diffs against 0.0
	}
	char buf[32];
	snprintf(buf, sizeof(buf), "%.10g", v);
	out += buf;
}

void AppendSparseRow(std::string &out, const vector<int> &indices, const vector<double> &coefficients) {
	vector<std::pair<int, double>> terms;
	terms.reserve(indices.size());
	for (idx_t i = 0; i < indices.size() && i < coefficients.size(); i++) {
		terms.emplace_back(indices[i], coefficients[i]);
	}
	std::sort(terms.begin(), terms.end());
	for (auto &term : terms) {
		out += " " + std::to_string(term.first) + ":";
		AppendDouble(out, term.second);
	}
}

void DumpSolverModel(const SolverModel &model) {
	const char *dest = std::getenv("DECIDB_DUMP_MODEL");
	if (!dest || !*dest) {
		return;
	}

	std::string out = "=== DECIDB MODEL DUMP ===\n";
	out += "sense: " + std::string(model.maximize ? "maximize" : "minimize") + "\n";
	out += "num_vars: " + std::to_string(model.num_vars) + "\n";
	out += "num_rows: " + std::to_string(model.constraints.size()) + "\n";
	out += "num_qrows: " + std::to_string(model.quadratic_constraints.size()) + "\n";

	for (idx_t col = 0; col < model.num_vars; col++) {
		out += "col " + std::to_string(col) + ": lb=";
		AppendDouble(out, col < model.col_lower.size() ? model.col_lower[col] : 0.0);
		out += " ub=";
		AppendDouble(out, col < model.col_upper.size() ? model.col_upper[col] : 0.0);
		out += " int=" + std::string(col < model.is_integer.size() && model.is_integer[col] ? "1" : "0");
		out += " bin=" + std::string(col < model.is_binary.size() && model.is_binary[col] ? "1" : "0");
		out += " obj=";
		AppendDouble(out, col < model.obj_coeffs.size() ? model.obj_coeffs[col] : 0.0);
		out += "\n";
	}

	if (model.has_quadratic_obj) {
		out += "qobj: nonconvex=" + std::string(model.nonconvex_quadratic ? "1" : "0") + "\n";
		vector<std::pair<std::pair<int, int>, double>> q_terms;
		q_terms.reserve(model.q_vals.size());
		for (idx_t i = 0; i < model.q_vals.size() && i < model.q_rows.size() && i < model.q_cols.size(); i++) {
			q_terms.push_back({{model.q_rows[i], model.q_cols[i]}, model.q_vals[i]});
		}
		std::sort(q_terms.begin(), q_terms.end());
		for (auto &term : q_terms) {
			out += "  q " + std::to_string(term.first.first) + "," + std::to_string(term.first.second) + ":";
			AppendDouble(out, term.second);
			out += "\n";
		}
	}

	for (idx_t r = 0; r < model.constraints.size(); r++) {
		auto &constr = model.constraints[r];
		out += "row " + std::to_string(r) + ": sense=" + std::string(1, constr.sense) + " rhs=";
		AppendDouble(out, constr.rhs);
		out += " nnz=" + std::to_string(constr.indices.size()) + " |";
		AppendSparseRow(out, constr.indices, constr.coefficients);
		out += "\n";
	}

	for (idx_t r = 0; r < model.quadratic_constraints.size(); r++) {
		auto &qc = model.quadratic_constraints[r];
		out += "qrow " + std::to_string(r) + ": sense=" + std::string(1, qc.sense) + " rhs=";
		AppendDouble(out, qc.rhs);
		out += " |";
		AppendSparseRow(out, qc.linear_indices, qc.linear_coefficients);
		out += " |";
		vector<std::pair<std::pair<int, int>, double>> q_terms;
		q_terms.reserve(qc.q_coefficients.size());
		for (idx_t i = 0; i < qc.q_coefficients.size() && i < qc.q_rows.size() && i < qc.q_cols.size(); i++) {
			q_terms.push_back({{qc.q_rows[i], qc.q_cols[i]}, qc.q_coefficients[i]});
		}
		std::sort(q_terms.begin(), q_terms.end());
		for (auto &term : q_terms) {
			out += " " + std::to_string(term.first.first) + "," + std::to_string(term.first.second) + ":";
			AppendDouble(out, term.second);
		}
		out += "\n";
	}

	out += "=== END MODEL DUMP ===\n";

	if (std::string(dest) == "1") {
		fputs(out.c_str(), stderr);
		return;
	}
	FILE *f = fopen(dest, "a");
	if (!f) {
		fputs(out.c_str(), stderr);
		return;
	}
	fputs(out.c_str(), f);
	fclose(f);
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

SolverResult SolvePreparedModel(const SolverModel &model, SolverBackend backend,
                                const SolveModelOptions &options) {
	double time_limit = options.time_limit_seconds > 0.0 ? options.time_limit_seconds : ResolveDecideTimeLimit();
	auto session = CreateSolverSession(backend);
	if (options.interrupt_poll) {
		session->SetInterruptPoll(options.interrupt_poll);
	}
	return session->Solve(model, time_limit);
}

SolverResult SolvePreparedModel(const SolverModel &model, SolverBackend backend) {
	return SolvePreparedModel(model, backend, SolveModelOptions());
}

unique_ptr<SolverSession> CreateSolverSession(SolverBackend backend) {
    switch (backend) {
    case SolverBackend::GUROBI:
        return GurobiSolver::CreateSession();
    case SolverBackend::HIGHS:
        return DeterministicNaive::CreateSession();
    }
    throw InternalException("Unknown DECIDE solver backend");
}

static SolverResult DisambiguateInfOrUnbd(const SolverModel &model, SolverBackend backend,
                                          const SolveModelOptions &diagnostic_options,
                                          const SolverResult &original) {
    if (original.status != SolverStatus::INF_OR_UNBD) {
        return original;
    }

    SolverModel probe_model = MakeZeroObjectiveProbeModel(model);
    SolverResult probe_result = SolvePreparedModel(probe_model, backend, diagnostic_options);

    SolverResult disambiguated = original;
    if (DidDiagnosticSolveStopEarly(probe_result)) {
        disambiguated.diagnostic_timed_out = true;
        return disambiguated;
    }
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
                        const SolveModelOptions &options, SolverModel *retained_model,
                        unique_ptr<SolverSession> *retained_session) {
	SolverModel model;
	try {
		model = SolverModel::Build(input, indexer);
	} catch (const DecideInfeasibleModelException &) {
		// Infeasibility proven during Build by a throw that abandoned the half-built
		// model, so there is nothing to retain and diagnosis cannot run. Only the
		// conflicting-column-bounds check still throws this way, and only when
		// diagnosis is off (under diagnosis it keeps the inverted box instead) — the
		// unsatisfiable-constraint case flags `build_proven_infeasible` and keeps its
		// model. The operator must treat an unretained model as "no diagnosis".
		SolverResult result;
		result.status = SolverStatus::INFEASIBLE;
		return result;
	}
	// Characterization oracle (no-op unless DECIDB_DUMP_MODEL is set). Emitted here,
	// on the freshly built model, so diagnostic re-solves (probe / ray / elastic
	// models built later) never pollute the dump.
	DumpSolverModel(model);
	// Rows as actually handed to the backend, for the benchmark's model-size metric. Read
	// off the built model because the pre-expansion inputs are not a row count at all: one
	// per-row spec becomes a row per data row, one PER spec a row per group. Captured here,
	// before any `std::move(model)` below.
	idx_t built_constraint_rows = model.constraints.size() + model.quadratic_constraints.size();
	// Build proved a constraint unsatisfiable while assembling the model: a row whose
	// left-hand side reduced to no terms at all, against a bound it cannot meet
	// (`SUM(0 * x) <= -1`, `x - x <= -1`). The row was kept so diagnosis can name and
	// relax it, but it is a coefficient-free row that no backend should be asked to
	// load. Short-circuit to INFEASIBLE here — retaining the model — for the same
	// reason as the inverted column box below. Unconditional: with diagnosis off the
	// retained model is simply discarded and the operator throws its static error.
	if (model.build_proven_infeasible) {
		SolverResult result;
		result.status = SolverStatus::INFEASIBLE;
		result.model_constraint_rows = built_constraint_rows;
		if (retained_model) {
			*retained_model = std::move(model);
		}
		return result;
	}
	// Under diagnosis (tolerate_infeasible_bounds), Build keeps an inverted column box
	// (col_lower > col_upper) instead of throwing, so the model can be retained for the
	// elastic engine. But the box IS infeasible, and some backends reject it at load
	// (HiGHS errors hard, poisoning the session). Short-circuit to INFEASIBLE here —
	// retaining the model — without ever handing the inverted box to the solver.
	if (input.tolerate_infeasible_bounds) {
		for (idx_t col = 0; col < model.num_vars; col++) {
			if (model.col_lower[col] > model.col_upper[col]) {
				SolverResult result;
				result.status = SolverStatus::INFEASIBLE;
				result.model_constraint_rows = built_constraint_rows;
				if (retained_model) {
					*retained_model = std::move(model);
				}
				return result;
			}
		}
	}
	SolverBackend backend = SelectSolverBackend();
	// Solve on a live session (the warm-continuation substrate). The first chunk
	// uses the caller's requested limit, or the shared default when unset (<0).
	double time_limit = options.time_limit_seconds > 0.0 ? options.time_limit_seconds : ResolveDecideTimeLimit();
	SolveModelOptions diagnostic_options = MakeDiagnosticSolveOptions(options);
	auto session = CreateSolverSession(backend);
	// Install the interrupt poll before the first solve so a user Ctrl-C cuts the
	// *initial* solve short (not just continuation chunks). The poll is a session
	// member, so it carries into every later Continue() when the session is retained.
	if (options.interrupt_poll) {
		session->SetInterruptPoll(options.interrupt_poll);
	}
	SolverResult result = session->Solve(model, time_limit);
	result = DisambiguateInfOrUnbd(model, backend, diagnostic_options, result);
	AttachUnboundedRayIfRequested(model, backend, options, diagnostic_options, result);
	// Hand the live session to the continuation loop if one asked for it, so a
	// time-limit stop can Continue() the same warm solver. Done before moving the
	// model (the session keeps its own copy of the model data, so the two are
	// independent), but the model move must still come last for the helpers above.
	if (retained_session) {
		*retained_session = std::move(session);
	}
	// Hand the built model to a diagnosis engine if one asked for it (it is
	// otherwise discarded here). Done last: the helpers above still need it alive.
	if (retained_model) {
		*retained_model = std::move(model);
	}
	result.model_constraint_rows = built_constraint_rows;
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
    case SolverStatus::SUBOPTIMAL:
        // OTHER, or OPTIMAL / SUBOPTIMAL passed here in error: fall through to the
        // generic message. (SUBOPTIMAL routes to the SOLVED success terminal, so it
        // never reaches this error path in practice — it is listed only so -Wswitch
        // flags any future addition; control still reaches the throw below.)
        break;
    }
    throw InvalidInputException("DECIDE optimization failed (solver status %d).", result.raw_status);
}

} // namespace duckdb
