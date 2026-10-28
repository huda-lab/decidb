//===----------------------------------------------------------------------===//
//                         DecidB
//
// duckdb/optimizer/decide/decide_solver_gate.hpp
//
// Stage 05's solver decisions, in the order they are made: which backend runs
// this query, which constructs are left for it to state natively, and — once the
// shape of the model is settled — whether it can take that model at all.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/decide_solver_capabilities.hpp"

namespace duckdb {

class LogicalDecide;

//! Choose the backend for this query and record what that choice implies, on the plan.
//!
//! Two things are settled here and nowhere else:
//!
//!   - WHICH backend (`solver_backend_name`), resolved once via SelectSolverBackend so
//!     every later stage reads one answer instead of asking again;
//!   - WHICH CONSTRUCTS are left native (`use_native_constructs`), which is a
//!     FORMULATION choice and therefore stage 05's to make. Every rewrite below runs
//!     against it, and stage 08 later reads the recorded decision rather than
//!     re-deriving it from a backend.
//!
//! Called once, first thing in DecideOptimizer::OptimizeDecide, before any rewrite. A plan
//! the DECIDE optimizer never saw is refused at physical planning (`plan_decide.cpp`), so
//! no other stage chooses a backend.
void ChooseDecideSolver(LogicalDecide &op);

//! What model class this query will demand of its solver, read off the prepared
//! linear form. A *prediction* of what `SolverModel::ModelClass()` will report once
//! the rows are in, and it must never predict less — see SolverModelClass.
SolverModelClass DeriveDecideModelClass(const LogicalDecide &op);

//! Throw if the backend chosen for this query cannot take the model class it needs.
//!
//! Model class is a gate, not an optimization: nothing lowers a quadratic constraint
//! or a non-convex objective into plain rows, so the only honest answer is refusal.
//! It happens HERE, at plan time, for two reasons. The message arrives before the
//! query scans anything, and — more importantly — the same SQL is legal on every
//! machine. What differs is whether this machine has a solver that can run it, so
//! the refusal blames the host and names the solver to install.
//!
//! Called once, right after BuildDecidePreparedModel, which is the first point at
//! which the shape of every constraint and the objective is settled.
void RequireDecideSolverSupport(const LogicalDecide &op);

//! The refusal both gates raise: `cause` names what the query does in the user's words, `remedy` the smallest edit
//! that avoids it, and the sentence names the solvers that take `needed`, from the registry. RequireDecideSolverSupport
//! raises it for a model class; the definition of a non-linear reducer body raises it when the backend chosen here
//! states no MIN/MAX natively, before any model is built.
[[noreturn]] void ThrowDecideSolverGap(const SolverModelClass &needed, const string &cause, const string &remedy);

} // namespace duckdb
