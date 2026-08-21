//===----------------------------------------------------------------------===//
//                         DecidB
//
// duckdb/optimizer/decide_solver_gate.hpp
//
// Stage 05's model-class gate: refuse, before a single row is read, a query no
// solver on this machine can run.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/decidb/solver_capabilities.hpp"

namespace duckdb {

class LogicalDecide;

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
