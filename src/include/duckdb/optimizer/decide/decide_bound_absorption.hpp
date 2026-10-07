//===----------------------------------------------------------------------===//
//                         DecidB
//
// duckdb/optimizer/decide/decide_bound_absorption.hpp
//
// Which comparisons the solver path folds into a variable's box.
//===----------------------------------------------------------------------===//

#pragma once

namespace duckdb {

class BoundComparisonExpression;
class ClientContext;
class LogicalDecide;

//! True when bound absorption folds this comparison into its decision's box instead of emitting a model row: the left
//! side is one bare decision, the right side is a finite constant (cast-wrapped at most), and a strict `<` or `>` has
//! an integer or BOOL decision. A bound the solver folds into the box replaces the variable's default lower bound of 0,
//! while one that becomes a model row only intersects with it, so a rule that reads bounds on the solver's behalf asks
//! this instead of guessing from the spelling. The caller owns the WHEN question: absorption never reads a clause that
//! carries one.
bool DecideBoundIsAbsorbed(ClientContext &context, LogicalDecide &decide, const BoundComparisonExpression &comparison);

} // namespace duckdb
