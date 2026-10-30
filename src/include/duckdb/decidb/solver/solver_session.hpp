//===----------------------------------------------------------------------===//
//                         DeciDB
//
// duckdb/decidb/solver/solver_session.hpp
//
// A persistent, resumable solver handle. Where SolvePreparedModel is a single
// shot (build → solve → discard), a SolverSession keeps the live backend solver
// (Gurobi env+model / HiGHS object) alive across chunks so the MIP search can be
// RESUMED for more wall-clock instead of restarted. This is the substrate for
// interactive slow-solve continuation: on a time-limit stop the DECIDE operator can
// call Continue() to give the same warm solver another chunk.
//
// Warm-resume semantics were probed on both backends: each honors a PER-CALL
// time-limit budget (a fresh `chunk_seconds` per call) and RESUMES the search
// after a time-limit stop (node counts grow cumulatively, the bound tightens
// monotonically). Continue() therefore just re-runs with a fresh chunk budget;
// no incumbent re-seeding is needed.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/decidb/solver/solver_result.hpp"

#include <functional>

namespace duckdb {

struct SolverModel;

//! A live solver handle that can be resumed for additional wall-clock. Solve()
//! loads the model into the backend once and runs the first chunk; Continue()
//! resumes that same warm solver for another chunk without reloading. Both return
//! a normalized SolverResult snapshot. Continue() has a precondition that Solve()
//! ran first on this session.
//!
//! Solve() and Continue() are the template methods (non-virtual interface) every caller
//! goes through; a backend implements the chunks. The backend never sees the objective's
//! constant (SolverModel::obj_constant): it moves no optimum, and the MIP gap a backend
//! measures stays the gap of the decisions. These two methods add it to the objective value
//! and the bound a chunk reads back, so every caller reads the objective as written.
class SolverSession {
public:
	virtual ~SolverSession() = default;

	//! Load `model` into the backend and optimize it with a `time_limit_seconds`
	//! wall-clock budget for this first chunk.
	SolverResult Solve(const SolverModel &model, double time_limit_seconds);

	//! Resume the live solver for another `time_limit_seconds` chunk. The MIP tree
	//! and incumbent from the previous chunk are preserved (warm resume).
	SolverResult Continue(double time_limit_seconds);

	//! Install a poll the session checks *during* a solve so a running chunk can be cut
	//! short (mid-chunk Ctrl-C) instead of only at the chunk boundary. `poll()` returning
	//! true requests early termination; the chunk then returns its best-so-far as a
	//! TIME_LIMIT result, exactly like a natural time-limit stop. The base is a no-op:
	//! a backend that cannot interrupt mid-solve (or lacks the symbol) simply keeps the
	//! boundary-only behavior — the poll never changes a solve's *outcome*, only *when* it
	//! returns. Set once by the operator before the continuation loop.
	virtual void SetInterruptPoll(std::function<bool()> poll) {
	}

	//! The relative MIP gap a solve may stop at (`decide_mip_gap`). Set before Solve(); both backends read it when
	//! they load the model, so it holds for every Continue() chunk too.
	void SetMipGap(double gap) {
		mip_gap = gap;
	}

protected:
	//! The backend's first chunk: load `model` and optimize it.
	virtual SolverResult SolveChunk(const SolverModel &model, double time_limit_seconds) = 0;
	//! The backend's next chunk on the model SolveChunk loaded.
	virtual SolverResult ContinueChunk(double time_limit_seconds) = 0;

	double mip_gap = 0.0;

private:
	//! The loaded model's objective constant, added to every chunk's read-back.
	double objective_constant = 0.0;
	SolverResult AddObjectiveConstant(SolverResult result) const;
};

} // namespace duckdb
