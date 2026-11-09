//===----------------------------------------------------------------------===//
//                         DeciDB
//
// duckdb/common/decide_solver_capabilities.hpp
//
// What one solver backend can be asked to do. Read by the stages ABOVE the
// solver facade — never by the backend itself, which knows its own answers.
//
// It lives in `common/` rather than in `decidb/solver/` for exactly that reason.
// Stage 07's backends ANSWER these types, but stages 03, 05, 06 and 08 all READ
// them: the plan carries a SolverConstructSupport, stage 05 branches on it to pick
// a formulation, and stage 06 checks a built model's class against it. A contract
// answered by one stage and read by four does not belong inside the answering
// stage's directory — filed there, every reader looks like it is reaching into the
// backend's internals. Nothing here names a backend, and nothing here depends on
// one: the constructs are DECIDE's, the model classes are mathematical.
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
//! that is checked, and SolverBackend::Constructs() applies it to EVERY backend —
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
	// root and branching — SOS1's only lever — has nothing to work on. See
	// `context/docs/pipeline/07_solver/done.md` § Capabilities. A measured-and-rejected
	// capability is not a field: a permanently-false flag nobody reads cannot be told apart from one whose
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

//! Is one product group of the objective (DecideProductGroup) non-convex under this sense? A product of two forms is
//! indefinite whatever its weight. A square `w * f * fᵀ` is semi-definite with the sign of `w`, so it is non-convex
//! when that sign fights the direction of optimization (MAXIMIZE of a positive weight, MINIMIZE of a negative one) or
//! is not known until the query runs (`weight_sign` 0).
//!
//! Shared on purpose. Stage 05 asks it of the prepared groups to PREDICT the model class, and stage 06 asks it again
//! of the evaluated groups to record the FACT. Those two answers must agree for the plan-time refusal to be honest, so
//! they read one predicate rather than two copies of one expression.
inline bool IsNonconvexProductGroup(bool square, int weight_sign, DecideSense sense) {
	return !square || weight_sign == 0 || (weight_sign > 0) == (sense == DecideSense::MAXIMIZE);
}

//! What the model-class question reads of one query (ClassifyDecideModel), filled term by term with AddTerm by each
//! reader of the query's terms: the solver gate from the prepared model's product groups (DeriveDecideModelClass), and
//! direct solve from its IR's split terms (DirectIR::model_shape). The built model fills the same fields from its
//! matrix (SolverModel::ModelClass), so the prediction and the fact are classified by one function.
//!
//! A reader that cannot tell what a term becomes in the built model says which way it errs: the gate demands more
//! (SolverModelClass), direct solve reports less.
struct DecideModelShape {
	DecideSense sense = DecideSense::FEASIBILITY;
	//! Some column of the model may be integral: a declared INT or BOOL, or a helper a rewrite adds as one.
	bool integral_column = false;
	bool quadratic_constraints = false;
	bool quadratic_objective = false;
	//! Some product of the objective is non-convex under `sense` (IsNonconvexProductGroup).
	bool nonconvex_objective = false;
	//! Some product of the objective couples two decisions, so its Q is rank-deficient (SolverModelClass).
	bool coupled_objective = false;

	//! One term of degree 2, in the objective or in a SUCH THAT clause: a square of one linear form or a product of
	//! two, the sign of its weight as the plan knows it (0 when only the query does), and how many distinct decisions
	//! its forms read. A square of a form over two or more decisions fills an off-diagonal entry of Q, as a product
	//! always does.
	void AddTerm(bool objective, bool square, int weight_sign, idx_t decisions) {
		if (!objective) {
			quadratic_constraints = true;
			return;
		}
		quadratic_objective = true;
		nonconvex_objective = nonconvex_objective || IsNonconvexProductGroup(square, weight_sign, sense);
		coupled_objective = coupled_objective || !square || decisions >= 2;
	}
};

//! The one classifier of a model's shape into the class a backend must take. An integral column and a coupled Q
//! matter only beside a quadratic objective: a MILP with quadratic rows is QCQP, not MIQP.
inline SolverModelClass ClassifyDecideModel(const DecideModelShape &shape) {
	SolverModelClass needed;
	needed.quadratic_constraints = shape.quadratic_constraints;
	if (shape.quadratic_objective) {
		needed.nonconvex_quadratic = shape.nonconvex_objective;
		needed.miqp = shape.integral_column;
		needed.singular_quadratic = shape.coupled_objective;
	}
	return needed;
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

} // namespace duckdb
