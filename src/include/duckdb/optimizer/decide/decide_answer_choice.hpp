//===----------------------------------------------------------------------===//
//                         DeciDB
//
// duckdb/optimizer/decide/decide_answer_choice.hpp
//
// How a DECIDE query is answered, chosen in one place: a direct-solve rule, else a solver.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"

namespace duckdb {

class LogicalDecide;
class LogicalOperator;
class Optimizer;

//! Choose how this DECIDE node is answered: Strategy selection with fallback. Try the direct-solve rules, and when none
//! applies choose a solver (ChooseDecideSolver). It runs once per node, after the bound-side rewrite
//! (DecideOptimizer::RewriteRhsAggregates), whose output the rules read and the solver path builds on.
//!
//! The policy lives here, with the choice it governs. `decide_direct_solve = 'off'` skips the rules. A DIAGNOSE query
//! and a backend forced by `DECIDB_FORCE_SOLVER` need the solver, so they skip the rules too; under `require` they
//! raise instead, as does a query no rule applies to. The solver is chosen lazily: a query a rule answers never
//! consults a backend, so it never probes a licence, and `require` with a bad `DECIDB_FORCE_SOLVER` reports the
//! conflict, not the bad name.
//!
//! Returns true when a rule replaced the node: `op` is then a relational plan and every reference above it in `root`
//! (which holds `op`) reads its bindings. Returns false with the node unchanged and its solver recorded on it.
bool ChooseDecideAnswer(unique_ptr<LogicalOperator> &op, unique_ptr<LogicalOperator> &root, Optimizer &optimizer);

} // namespace duckdb
