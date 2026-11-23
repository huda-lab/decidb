//===----------------------------------------------------------------------===//
//                         DeciDB
//
// duckdb/decidb/formulation/formulate_model.hpp
//
// Stage 06's driver: the evaluated clauses become solver rows against a box. PrepareModel runs once for a query,
// FormulateModel once for each box the model is formulated against.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/decide_numeric_policy.hpp"
#include "duckdb/decidb/formulation/ilp_model.hpp"
#include "duckdb/decidb/formulation/solver_input.hpp"

namespace duckdb {

//! What stage 06 settles once for a query, on the model as the emitter wrote it and the box the plan declares
//! (`boxes`, each decision's as written): the rows that need no box, and the column box every formulation starts
//! from. The order is load-bearing. The ABS envelopes join the rows before any pass reads them: implied-bound
//! propagation reads `aux >= inner` of `ABS(x) <= 3` as a bound on x. A constant lower bound under a `when` replaces
//! the default floor of 0 on the instances its rows reach before any pass reads the box. A product has no default
//! floor, so it is floored at its factor's once those floors stand, and a SUM or MAX helper over non-negative
//! decisions is floored by its members' reach: both are then non-negative terms of a budget row, from which the
//! propagation derives a finite top for a decision with none. Only provably implied bounds are applied.
void PrepareModel(SolverInput &input, const VarIndexer &var_indexer, const vector<DecideBoundBox> &boxes);

//! Turn the evaluated clauses of `input` into solver rows against `box`: the auto-`M` refill, every linearization
//! pass, and the lowering of whatever the chosen backend cannot state natively.
//!
//! A pure function of its arguments, and `SolverInput` is the whole contract of what it reads: no operator, no
//! `ClientContext`, no data scan, no expression evaluation. That is what lets it run a second time, on a copy of
//! `input` against a widened box, to derive the constants an elastic (infeasibility-repair) model needs. Keep it that
//! way: anything here that reads the data or the plan belongs in the emitter, which writes it into `SolverInput`.
//!
//! The flat column space is settled by assembly and is never rebuilt here: only the global block of `var_indexer`
//! grows, as auxiliaries are appended. That is what lets a second formulation, against a different box, name exactly
//! the same columns as the first.
void FormulateModel(SolverInput &input, const FormulationBox &box, VarIndexer &var_indexer);

} // namespace duckdb
