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
};

} // namespace duckdb
