//===----------------------------------------------------------------------===//
//                         DecidB
//
// duckdb/decidb/solver/ilp_solver.hpp
//
// Single entry point for optimization solving. Builds a SolverModel from
// SolverInput, selects the best available backend (Gurobi > HiGHS), and
// returns a structured SolverResult. Supports LP, MILP, and convex QP/MIQP.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/decidb/formulation/solver_input.hpp"
#include "duckdb/decidb/formulation/ilp_model.hpp"
#include "duckdb/decidb/solver/solver_registry.hpp"
#include "duckdb/decidb/solver/solver_result.hpp"
#include "duckdb/decidb/solver/solver_session.hpp"

namespace duckdb {

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

//! Resolve the backend DeciDB should use for a DECIDE query, honoring the
//! DECIDB_FORCE_SOLVER test override and otherwise taking the first available
//! entry in registry order (SolverRegistry::Backends).
//!
//! Throws when DECIDB_FORCE_SOLVER names a backend that is unregistered or is not
//! installed on this host. An unrecognized name is refused rather than ignored:
//! anything that pins the backend is asking for one specific solver, so silently
//! running the host default under that name would be a lie.
//!
//! Called ONCE per query, by stage 05's ChooseDecideSolver before any rewrite runs.
//! The answer rides the plan from there as a NAME (LogicalDecide::solver_backend_name
//! → PhysicalDecide::solver_backend_name), alongside the formulation it implies
//! (::use_native_constructs); stage 08 turns the name back into a backend only where a
//! solve is about to run. Nothing downstream may call this again: once a rewrite has
//! consulted the backend's capabilities, a second call that answered differently would
//! leave the plan and the solve disagreeing about what was lowered.
SolverBackend SelectSolverBackend();

//! Solve an already-built SolverModel with the selected backend. Diagnostic
//! engines use this to run transformed models without rebuilding SolverInput.
SolverResult SolvePreparedModel(const SolverModel &model, SolverBackend backend);
SolverResult SolvePreparedModel(const SolverModel &model, SolverBackend backend,
                                const SolveModelOptions &options);

//! Builds a SolverModel from the given SolverInput, solves it on `backend`, and
//! returns a SolverResult: the terminal status plus, when optimal, the solution vector of
//! size (num_rows * num_decide_vars). A non-optimal status is returned, not
//! thrown — the operator decides whether an unprefixed statement surfaces the
//! default error or DIAGNOSE routes to a diagnosis terminal.
//!
//! `input` is taken by non-const reference because the raw global constraints
//! are moved (not copied) into the SolverModel during Build(). Callers must not
//! read `input.global_constraints` after this call returns.
//!
//! `indexer` is the VarIndexer constructed once in Finalize() and threaded
//! through here to avoid duplicate construction inside SolverModel::Build().
//!
//! `backend` is the choice made at plan time (SelectSolverBackend, called by the
//! DECIDE optimizer) and carried down on the operator. It is passed in rather than
//! resolved here so the backend that runs the solve is provably the same one whose
//! capabilities the rewrites were selected against.
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
SolverResult SolveModel(SolverInput &input, const VarIndexer &indexer, SolverBackend backend,
                         const SolveModelOptions &options, SolverModel *retained_model = nullptr,
                         unique_ptr<SolverSession> *retained_session = nullptr);

} // namespace duckdb
