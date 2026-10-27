//===----------------------------------------------------------------------===//
//                         DecidB
//
// duckdb/optimizer/decide/decide_singleton_row_folding.hpp
//
// Which comparisons the solver path folds into a variable's box.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/decide_numeric_policy.hpp"

namespace duckdb {

class BoundComparisonExpression;
class ClientContext;
class LogicalDecide;

//! True when singleton-row folding folds this comparison into its decision's box instead of emitting a model row: the
//! decision side is one decision in an affine spelling with constants known before the query runs (`x`, `2 * x`,
//! `x - 1`) or an easy `min(x) >= K` / `max(x) <= K` (D41), the other side folds to a finite number, and a strict `<`
//! or `>` has an integer or BOOL decision. A bound the solver folds into the box replaces the variable's default lower
//! bound of 0, while one that becomes a model row only intersects with it, so a rule that reads bounds on the solver's
//! behalf asks this instead of guessing from the spelling. `plain` is set when the comparison is spelled as one bare
//! decision against a literal under casts (`x <= 10`), the only spelling a direct-solve rule reads its bound from. The
//! caller owns the `when` question: folding never reads a clause that carries one.
bool DecideIsSingletonRowBound(ClientContext &context, LogicalDecide &decide,
                               const BoundComparisonExpression &comparison, bool &plain);

//! True when a decision's folded box contradicts its built-in domain, which the solver rejects with an error before it
//! reads any row (so on an empty table too). `box` is the box the solver folds, from the bounds as it reads them (after
//! `DecideStrictBound`, before any rounding of a fraction); a box with no lower bound written has the default floor of
//! 0. The box contradicts the domain when the upper bound is below 0 while the floor is still at or above 0 (a BOOL's
//! floor is always 0, so for it any upper bound below 0), or when a BOOL's floor is above 1. A purely inverted box such
//! as `x >= 5 AND x <= 1` is not a contradiction here; the elastic engine reports it. A rule that answers a query
//! itself must decline when this is true, so that the solver raises the error.
bool DecideBoxContradictsDomain(const DecideBoundBox &box, bool is_bool);

} // namespace duckdb
