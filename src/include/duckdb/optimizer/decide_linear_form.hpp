//===----------------------------------------------------------------------===//
//                         DecidB
//
// duckdb/optimizer/decide_linear_form.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"

namespace duckdb {
class ClientContext;
class LogicalDecide;

//! Flatten every canonical constraint and the objective into the prepared linear
//! form on `decide.prepared`.
//!
//! This is the LAST pass of DECIDE optimization, and it must stay last for the
//! same reason bound absorption does: the nine rewrites above it emit fresh
//! constraint rows through `LogicalDecide::AddConstraint`, and absorption tags
//! the comparisons it folded into the variable box. Flattening earlier would
//! miss the former and re-emit the latter.
//!
//! Doing the algebra here rather than at execution time is what lets a rebuilt
//! coefficient be bound through `FunctionBinder` instead of inheriting another
//! node's signature. Nothing in this pass reads a data row -- it needs types,
//! not values -- so every coefficient stays an unevaluated `Expression` for
//! stage 08 to evaluate against the relational input.
void BuildDecidePreparedModel(ClientContext &context, LogicalDecide &decide);

} // namespace duckdb
