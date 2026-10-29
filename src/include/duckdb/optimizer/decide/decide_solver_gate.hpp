//===----------------------------------------------------------------------===//
//                         DecidB
//
// duckdb/optimizer/decide/decide_solver_gate.hpp
//
// Stage 05's gate on the solver chosen for a query (decide_answer_choice.hpp): which model class the query demands of
// it, and whether it can take that model at all.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/decide_solver_capabilities.hpp"

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

//! The refusal both gates raise: `cause` names what the query does in the user's words, `remedy` the smallest edit
//! that avoids it, and the sentence names the solvers that take `needed`, from the registry. RequireDecideSolverSupport
//! raises it for a model class; the definition of a non-linear reducer body raises it when the backend chosen here
//! states no MIN/MAX natively, before any model is built.
[[noreturn]] void ThrowDecideSolverGap(const SolverModelClass &needed, const string &cause, const string &remedy);

} // namespace duckdb
