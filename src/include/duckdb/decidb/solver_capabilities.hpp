//===----------------------------------------------------------------------===//
//                         DecidB
//
// duckdb/decidb/solver_capabilities.hpp
//
// What one solver backend can be asked to do. Read by the stages ABOVE the
// solver facade — never by the backend itself, which knows its own answers.
//
//===----------------------------------------------------------------------===//

#pragma once

namespace duckdb {

//! Declares the differences between backends that an UPSTREAM stage has to branch
//! on. That is the whole membership rule: a difference only the backend itself acts
//! on does not belong here — it stays a virtual on SolverSession with a safe default
//! (SolverSession::SetInterruptPoll is the reference: a backend that cannot interrupt
//! mid-solve inherits the no-op and nothing above it changes).
//!
//! The flags split into two kinds, and the split decides what "unsupported" MEANS:
//!
//!   - Construct flags. A DECIDE construct the backend can express natively (ABS,
//!     MIN/MAX, `<>`, `IN`, bilinear). A lowering into plain rows always exists, so
//!     `false` costs accuracy or speed, never legality: stage 05 lowers as it always
//!     has. These are optimizations, and the lowering path is never deleted.
//!
//!   - Model-class flags. A shape of model the backend can load at all (quadratic
//!     constraints, non-convex quadratic objectives, MIQP). No lowering exists, so
//!     `false` is a gate and the only answer is refusal. The refusal happens at PLAN
//!     time and blames the host, not the query — the same SQL is legal everywhere,
//!     it just cannot run on a machine with no solver that accepts it.
//!
//! A flag is only worth a field if it is A/B-verifiable: forcing the construct back
//! down its lowering path must reach the same optimum. A flag that cannot be tested
//! that way does not belong in the table.
struct SolverCapabilities {
	// --- Construct flags: native is faster/tighter, lowering is always valid ---

	//! ABS(e) as a native absolute-value constraint, so an unbounded contributor
	//! needs no Big-M envelope (Gurobi: GRBaddgenconstrAbs).
	bool abs = false;
	//! MIN/MAX over decision terms as a native general constraint, so the indicator
	//! + Big-M formulation is not needed (Gurobi: GRBaddgenconstrMin/Max).
	bool min_max = false;
	//! `<>` as a native indicator constraint rather than a Big-M disjunction.
	bool not_equal = false;
	//! `x IN (a, b, c)` as a native SOS1 set rather than explicit indicators.
	bool in_list = false;
	//! A product of two decision variables handled natively rather than by
	//! McCormick linearization.
	bool bilinear = false;

	// --- Model-class flags: no lowering exists; false means refuse ---

	//! Quadratic terms in a CONSTRAINT row (QCQP), not only in the objective.
	bool quadratic_constraints = false;
	//! A quadratic objective that is not positive semi-definite. Every bilinear
	//! objective is in this class.
	bool nonconvex_quadratic = false;
	//! A quadratic objective together with integer or boolean variables (MIQP).
	bool miqp = false;
	//! A convex quadratic objective whose Q is rank-deficient, which is what one
	//! squared expression spanning two or more decision variables always produces:
	//! `POWER(p*x + q*y + r, 2)` gives a 2x2 Q of determinant zero. Such a model is
	//! perfectly well posed — its optimum lies along a flat valley rather than at an
	//! isolated point — but not every QP solver navigates that valley. HiGHS does not:
	//! it stops partway down, or errors, on roughly half of them, and a stopped-early
	//! answer is returned as though optimal. Gurobi solves them.
	//!
	//! This is the one model-class flag that is about solver QUALITY rather than
	//! expressiveness — HiGHS loads these models happily, it just answers them wrong —
	//! so unlike the three above it should be revisited as HiGHS improves rather than
	//! treated as a permanent property of the backend.
	bool singular_quadratic = false;
};

//! The demand side of the model-class flags above: what a particular DECIDE query
//! needs from whichever solver runs it. Derived twice, deliberately:
//!
//!   - at PLAN time from the prepared model, by stage 05, which is what produces the
//!     user-facing refusal early enough that no row is ever read;
//!   - from the BUILT model, by stage 06, which `SolveModel` checks against the
//!     chosen backend as an internal invariant.
//!
//! The first is a prediction and the second is the fact. The first must never
//! under-report the second — if it did, a model would reach a backend that cannot
//! load it — so the plan-time derivation errs toward demanding more, and the
//! invariant check exists to catch the day it stops.
struct SolverModelClass {
	bool quadratic_constraints = false;
	bool nonconvex_quadratic = false;
	bool miqp = false;
	bool singular_quadratic = false;
};

//! Which demand a capability table fails to meet. NONE means the backend can take
//! the model. The order below is the reporting order, so one query always names the
//! same reason rather than a different one per run.
enum class SolverModelClassGap { NONE, QUADRATIC_CONSTRAINTS, NONCONVEX_QUADRATIC, MIQP, SINGULAR_QUADRATIC };

inline SolverModelClassGap FindModelClassGap(const SolverModelClass &needed,
                                             const SolverCapabilities &capabilities) {
	if (needed.quadratic_constraints && !capabilities.quadratic_constraints) {
		return SolverModelClassGap::QUADRATIC_CONSTRAINTS;
	}
	if (needed.nonconvex_quadratic && !capabilities.nonconvex_quadratic) {
		return SolverModelClassGap::NONCONVEX_QUADRATIC;
	}
	if (needed.miqp && !capabilities.miqp) {
		return SolverModelClassGap::MIQP;
	}
	// Reported last on purpose. A query can demand this alongside one of the three
	// above, and when it does the other is both the more fundamental reason and the
	// more useful thing to tell the user.
	if (needed.singular_quadratic && !capabilities.singular_quadratic) {
		return SolverModelClassGap::SINGULAR_QUADRATIC;
	}
	return SolverModelClassGap::NONE;
}

//! Does a capability table cover this demand? Reads at call sites where only the
//! yes/no matters.
inline bool SupportsModelClass(const SolverModelClass &needed, const SolverCapabilities &capabilities) {
	return FindModelClassGap(needed, capabilities) == SolverModelClassGap::NONE;
}

} // namespace duckdb
