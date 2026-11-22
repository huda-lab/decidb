//===----------------------------------------------------------------------===//
//                         DeciDB
//
// duckdb/decidb/diagnostics/decide_diagnostic_terminals.hpp
//
// The two failure terminals of DIAGNOSE, one function each. The router (decide_router.hpp) names the terminal; the
// operator calls it and hands the finding off.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/decidb/diagnostics/decide_diagnostic.hpp"
#include "duckdb/decidb/solver/ilp_solver.hpp"

namespace duckdb {

class ColumnDataCollection;

//! The model input and column space as assembly left them — before a single Big-M or McCormick constant was derived
//! — plus the scoped bounds the column box enforces, whose rows only a repair model carries. Retained only under
//! `DIAGNOSE`, so the infeasible terminal can formulate a *second* model against the widened box a repair searches
//! in, instead of re-using constants that were derived against the narrower box the solved model declares.
struct ElasticFormulation {
	SolverInput input;
	VarIndexer var_indexer;
};

//! The unbounded terminal: the diagnosis of an unbounded `result` from its ray, with the categorical groupings of
//! the buffered rows `data` to say which instances run away; when the engine names no variable, why it could not.
//! `retained_model` is the solved model, `var_labels` and `var_is_aux` name each decision, and the `input_column_*`
//! are positionally aligned with `data`'s columns.
DecideDiagnostic UnboundedTerminal(ClientContext &context, const SolverResult &solve_result,
                                   const SolverModel &retained_model, const VarIndexer &var_indexer,
                                   const vector<string> &var_labels, const vector<bool> &var_is_aux,
                                   const ColumnDataCollection &data, const vector<string> &input_column_names,
                                   const vector<bool> &input_column_in_clause);

//! The infeasible terminal: formulate the repair model from `elastic` against the widened box, solve it on `backend`
//! under `diagnostic_solve_options`, and read the least-change repair off it; when there is none, why. `backend` is
//! the one the primary solve ran on, named at plan time. `negate_objective`: the query was written MAXIMIZE.
DecideDiagnostic InfeasibleTerminal(ClientContext &context, const SolverResult &solve_result,
                                    const ElasticFormulation &elastic, const vector<string> &var_labels,
                                    const vector<bool> &var_is_aux, SolverBackend backend,
                                    const SolveModelOptions &diagnostic_solve_options, bool negate_objective);

} // namespace duckdb
