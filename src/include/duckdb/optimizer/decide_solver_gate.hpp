//===----------------------------------------------------------------------===//
//                         DecidB
//
// duckdb/optimizer/decide_solver_gate.hpp
//
// Stage 05's solver decisions, in the order they are made: which backend runs
// this query, which constructs are left for it to state natively, and — once the
// shape of the model is settled — whether it can take that model at all.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/decidb/solver_capabilities.hpp"

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
//! Called first thing in DecideOptimizer::OptimizeDecide, before any rewrite. Physical
//! planning calls it too, but only as a fallback for the case where the DECIDE
//! optimizer never ran (`SET disabled_optimizers='decide_optimizer'`) — the decision
//! still belongs to this file, it is merely triggered from there.
//!
//! Idempotent in the sense that matters: it is a no-op once a name is recorded, so the
//! fallback can never overwrite a choice the rewrites were already selected against.
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

} // namespace duckdb
