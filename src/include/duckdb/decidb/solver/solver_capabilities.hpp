//===----------------------------------------------------------------------===//
//                         DecidB
//
// duckdb/decidb/solver/solver_capabilities.hpp
//
// What one solver backend can be asked to do. Read by the stages ABOVE the
// solver facade — never by the backend itself, which knows its own answers.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enums/decide.hpp"

namespace duckdb {

//! This header declares the differences between backends that an UPSTREAM stage has
//! to branch on. That is the whole membership rule: a difference only the backend
//! itself acts on does not belong here — it stays a virtual on SolverSession with a
//! safe default (SolverSession::SetInterruptPoll is the reference: a backend that
//! cannot interrupt mid-solve inherits the no-op and nothing above it changes).
//!
//! There are exactly two kinds of difference, and they mean opposite things when the
//! answer is "no". They are therefore two TYPES, not two halves of one struct:
//!
//!   - SolverConstructSupport — an optimization. `false` runs the lowering path.
//!   - SolverModelClass       — a gate. `false` refuses the query.
//!
//! Keeping them apart is what lets each predicate take exactly the table it reads:
//! nothing is handed a set of flags half of which it must ignore.

//===----------------------------------------------------------------------===//
// Construct support — an optimization, never a gate
//===----------------------------------------------------------------------===//

//! Which DECIDE constructs the backend can state itself rather than as lowered rows.
//! A lowering into plain rows always exists, so `false` costs accuracy or speed, never
//! legality: stage 05 lowers as it always has. These are optimizations, and the
//! lowering path is never deleted.
//!
//! A construct is only worth a field if it is A/B-verifiable: forcing it back down its
//! lowering path must reach the same optimum. `DECIDB_NATIVE_CONSTRUCTS=off` is how
//! that is checked, and SolverBackend::Capabilities() applies it to EVERY backend —
//! the switch belongs to the contract, not to whichever backend happens to declare a
//! construct today.
//!
//! The table mapping a GeneralConstraintKind onto the flag that gates it is
//! `DeclaresGeneralConstraint`, which lives beside that enum in `solver_input.hpp` so
//! the kinds and the flags covering them cannot drift apart.
struct SolverConstructSupport {
	//! ABS(e) as a native absolute-value constraint, so an unbounded contributor
	//! needs no Big-M envelope (Gurobi: GRBaddgenconstrAbs).
	bool abs = false;
	//! MIN/MAX over decision terms as a native general constraint, so the indicator
	//! + Big-M formulation is not needed (Gurobi: GRBaddgenconstrMin/Max).
	bool min_max = false;
	//! `<>` as a native indicator constraint rather than a Big-M disjunction.
	bool not_equal = false;
	//! A product of two decision variables handled natively rather than by
	//! McCormick linearization.
	bool bilinear = false;

	// There is deliberately no `in_list` flag. SOS1 acceleration for `x IN (a, b, c)`
	// was measured on the exact model shape DeciDB emits and declined: the
	// formulation's LP relaxation is already integral, so the solve finishes at the
	// root and branching — SOS1's only lever — has nothing to work on. The numbers are
	// in `context/descriptions/01_pipeline/07_solver/todo.md`, "SOS1 for `IN` —
	// measured and declined". A measured-and-rejected capability is not a field: a
	// permanently-false flag nobody reads cannot be told apart from one whose
	// implementation is merely still pending.
};

//===----------------------------------------------------------------------===//
// Model class — a gate, never an optimization
//===----------------------------------------------------------------------===//

//! A shape of model, used for BOTH sides of the same question: which shapes a backend
//! can load at all, and which shapes a particular DECIDE query demands. No lowering
//! exists for any of them, so a demand a backend cannot meet is a gate: the only
//! answer is refusal, at PLAN time, blaming the host rather than the query — the same
//! SQL is legal everywhere, it just cannot run on a machine with no solver that
//! accepts it.
//!
//! The demand side is derived twice, deliberately:
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
	//! This is the one model class that is about solver QUALITY rather than
	//! expressiveness — HiGHS loads these models happily, it just answers them wrong —
	//! so unlike the three above it should be revisited as HiGHS improves rather than
	//! treated as a permanent property of the backend.
	bool singular_quadratic = false;
};

//! Is a quadratic objective non-convex under this sense? Non-convex exactly when the
//! sign of Q fights the direction of optimization: MAXIMIZE of a positive-semi-definite
//! Q, or MINIMIZE of a negative-semi-definite one.
//!
//! Shared on purpose. Stage 05 asks it of the prepared objective's `quadratic_sign` to
//! PREDICT the model class, and stage 06 asks it again of the built model's `q_sign` to
//! record the FACT. Those two answers must agree for the plan-time refusal to be
//! honest, so they read one predicate rather than two copies of one expression.
inline bool IsNonconvexQuadraticObjective(double quadratic_sign, DecideSense sense) {
	return (quadratic_sign > 0.0) == (sense == DecideSense::MAXIMIZE);
}

//! Which demand a backend fails to meet. NONE means it can take the model. The order
//! below is the reporting order, so one query always names the same reason rather than
//! a different one per run.
enum class SolverModelClassGap { NONE, QUADRATIC_CONSTRAINTS, NONCONVEX_QUADRATIC, MIQP, SINGULAR_QUADRATIC };

//! `needed` is what a query demands, `supported` is what a backend accepts — both
//! spelled as the same set of model classes, so the comparison is a plain containment
//! test with nothing to ignore on either side.
inline SolverModelClassGap FindModelClassGap(const SolverModelClass &needed, const SolverModelClass &supported) {
	if (needed.quadratic_constraints && !supported.quadratic_constraints) {
		return SolverModelClassGap::QUADRATIC_CONSTRAINTS;
	}
	if (needed.nonconvex_quadratic && !supported.nonconvex_quadratic) {
		return SolverModelClassGap::NONCONVEX_QUADRATIC;
	}
	if (needed.miqp && !supported.miqp) {
		return SolverModelClassGap::MIQP;
	}
	// Reported last on purpose. A query can demand this alongside one of the three
	// above, and when it does the other is both the more fundamental reason and the
	// more useful thing to tell the user.
	if (needed.singular_quadratic && !supported.singular_quadratic) {
		return SolverModelClassGap::SINGULAR_QUADRATIC;
	}
	return SolverModelClassGap::NONE;
}

//! Does a backend cover this demand? Reads at call sites where only the yes/no matters.
inline bool SupportsModelClass(const SolverModelClass &needed, const SolverModelClass &supported) {
	return FindModelClassGap(needed, supported) == SolverModelClassGap::NONE;
}

//===----------------------------------------------------------------------===//
// The pair, as one backend answers it
//===----------------------------------------------------------------------===//

//! What one backend declares, both kinds together. This exists only so the registry
//! asks each backend a single question; every call site below it reads one member or
//! the other, never the whole thing.
struct SolverCapabilities {
	//! Constructs this backend states natively. Masked centrally by
	//! SolverBackend::Capabilities() when DECIDB_NATIVE_CONSTRUCTS=off.
	SolverConstructSupport constructs;
	//! Model classes this backend can load at all.
	SolverModelClass model_classes;
};

} // namespace duckdb
