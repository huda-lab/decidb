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
#include "duckdb/decidb/solver_session.hpp"

namespace duckdb {

//! Backend chosen for a prepared SolverModel solve. Kept explicit so diagnostic
//! re-solves can use the same backend as the primary solve.
enum class SolverBackend {
	GUROBI,
	HIGHS
};

//! Internal solver toggles for diagnostic-only follow-up solves. The default
//! preserves today's user-facing path: non-optimal solves return a status and
//! no extra diagnostic artifacts.
struct SolveModelOptions {
	bool extract_unbounded_ray = false;
	//! Per-solve wall-clock budget (seconds). A negative value means "resolve the
	//! shared default" (ResolveDecideTimeLimit). The slow-solve continuation loop
	//! sets this so the first chunk uses the same limit later Continue() chunks do.
	double time_limit_seconds = -1.0;
	//! Installed on the session before the first solve, so a user Ctrl-C interrupts
	//! the *initial* solve — not only continuation chunks after a timeout. The poll
	//! is a session member, so it persists across Continue() automatically. Empty =
	//! boundary-only (no mid-solve interrupt). Gurobi honors it via a watcher thread;
	//! HiGHS (no thread-safe terminate) ignores it.
	std::function<bool()> interrupt_poll;
};

//! Resolve the backend DeciDB should use for this solve, honoring the
//! DECIDB_FORCE_SOLVER test override and otherwise preferring Gurobi.
SolverBackend SelectSolverBackend();

//! Solve an already-built SolverModel with the selected backend. Diagnostic
//! engines use this to run transformed models without rebuilding SolverInput.
SolverResult SolvePreparedModel(const SolverModel &model, SolverBackend backend);

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
//!
//! `retained_model` (optional): when non-null, the built SolverModel is moved
//! into it after the solve so a diagnosis engine can transform and re-solve it
//! (the model is otherwise a local and discarded once the solve returns). Left
//! untouched when Build() proves infeasibility before a model exists.
//!
//! `retained_session` (optional): when non-null, the live solver session used for
//! the solve is moved into it so the slow-solve continuation loop can Continue()
//! the warm solver on a time-limit stop. Left untouched when Build() proves
//! infeasibility before any solver runs.
SolverResult SolveModel(SolverInput &input, const VarIndexer &indexer,
                         const SolveModelOptions &options, SolverModel *retained_model = nullptr,
                         unique_ptr<SolverSession> *retained_session = nullptr);
SolverResult SolveModel(SolverInput &input, const VarIndexer &indexer);

} // namespace duckdb
