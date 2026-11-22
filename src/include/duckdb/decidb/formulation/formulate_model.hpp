//===----------------------------------------------------------------------===//
//                         DeciDB
//
// duckdb/decidb/formulation/formulate_model.hpp
//
// Stage 06's driver: the evaluated clauses become solver rows against a box.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/decidb/formulation/ilp_model.hpp"
#include "duckdb/decidb/formulation/solver_input.hpp"

namespace duckdb {

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
