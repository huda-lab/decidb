//===----------------------------------------------------------------------===//
//                         DeciDB
//
// duckdb/optimizer/decide/decide_linear_form.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"

#include <functional>

namespace duckdb {
class BoundAggregateExpression;
class ClientContext;
class LogicalDecide;

//! The tree node a reducer stage 05 lowers ranges over (DecideTreeConstruction::ReducerChild, plan_decide.cpp); null
//! when no tree is built.
using DecideReducerChild = std::function<idx_t(const BoundAggregateExpression &reducer)>;

//! Flatten every canonical constraint and the objective into the prepared linear
//! form on `decide.prepared`.
//!
//! This is the LAST pass of DECIDE optimization, and it must stay last for the
//! same reason singleton-row folding does: the nine rewrites above it emit fresh
//! constraint rows through `LogicalDecide::AddConstraint`, and folding tags
//! the comparisons it folded into the variable box. Flattening earlier would
//! miss the former and re-emit the latter.
//!
//! Doing the algebra here rather than at execution time is what lets a rebuilt
//! coefficient be bound through `FunctionBinder` instead of inheriting another
//! node's signature. Nothing in this pass reads a data row -- it needs types,
//! not values -- so every coefficient stays an unevaluated `Expression` for
//! stage 08 to evaluate against the relational input.
//!
//! With `child`, every term a reducer produces names the tree node it ranges over (DecideTerm::child), which only the
//! tree reads: the flat model is the same with and without it.
void BuildDecidePreparedModel(ClientContext &context, LogicalDecide &decide, const DecideReducerChild &child = nullptr);

} // namespace duckdb
