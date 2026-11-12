//===----------------------------------------------------------------------===//
//                         DeciDB
//
// duckdb/optimizer/decide/decide_linear_form.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"

namespace duckdb {
class ClientContext;
class DecideTreeConstruction;
class LogicalDecide;

//! Lower every canonical constraint and the objective into the tree under construction: each clause's records
//! (DecideConstraint) into the root the tree made of its comparison, the objective's into the objective's root, and
//! the frames' columns on `decide.prepared`. Every term a reducer produces names the G it ranges over
//! (DecideTerm::child, DecideTreeConstruction::ReducerChild).
//!
//! This is the LAST pass of DECIDE optimization, and it must stay last for the
//! same reason singleton-row folding does: the nine rewrites above it emit fresh
//! constraint rows through `LogicalDecide::AddConstraint`, and folding tags
//! the comparisons it folded into the variable box. Lowering earlier would
//! miss the former and re-emit the latter.
//!
//! Doing the algebra here rather than at execution time is what lets a rebuilt
//! coefficient be bound through `FunctionBinder` instead of inheriting another
//! node's signature. Nothing in this pass reads a data row -- it needs types,
//! not values -- so every coefficient stays an unevaluated `Expression` for
//! stage 08 to evaluate against the relational input.
void LowerDecideTree(ClientContext &context, LogicalDecide &decide, DecideTreeConstruction &tree);

} // namespace duckdb
