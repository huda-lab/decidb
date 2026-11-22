//===----------------------------------------------------------------------===//
//                         DeciDB
//
// duckdb/decidb/solver/decide_solve.hpp
//
// The solve of a DECIDE query as ordinary execution behaviour: the options, the solve, and the stop on a time limit
// or a Ctrl-C. The operator routes what comes back.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/decidb/solver/ilp_solver.hpp"

namespace duckdb {

class ClientContext;

//! The time-limit report's closeness figure: `gap`, a fraction, as a percentage with two significant digits.
string FormatDecideGapPercent(double gap);

//! What the solve leaves for the operator to route and read back.
struct DecideSolveOutcome {
	//! The final result: after a time-limit stop, the result of the stop (a resumed solve's, when the user continued).
	SolverResult result;
	//! The built model, kept only when diagnosis is armed (SolveModel's `retained_model`); empty otherwise.
	SolverModel model;
	//! A time-limit or Ctrl-C stop that returns the best solution found so far: the operator routes it as solved.
	bool deliver_incumbent = false;
	//! The options a diagnosis's follow-up solves run under: the diagnostic time limit and the same interrupt poll.
	SolveModelOptions diagnostic_options;
};

//! Solve `input` on `backend`. A time-limit stop (or a Ctrl-C, which arrives as one) is handled here, with or without
//! the DIAGNOSE prefix: it delivers an incumbent, resumes to a proven optimum, or raises, and never diagnoses. With
//! `can_prompt` (stdin is a terminal) the stop asks whether to keep solving; without it a stop that finds no solution
//! raises. `negate_objective`: the query was written MAXIMIZE, so the objective is shown negated. Every other status
//! is returned for the operator to route.
DecideSolveOutcome SolveDecide(ClientContext &context, SolverInput &input, const VarIndexer &indexer,
                               SolverBackend backend, bool negate_objective, bool diagnosis_armed, bool can_prompt);

} // namespace duckdb
