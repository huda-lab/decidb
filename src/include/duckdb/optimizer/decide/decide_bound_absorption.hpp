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

//! The inclusive bound a strict `>` (`is_lower`) or `<` against `k` means on a whole-numbered decision: the first whole
//! value above `k` (`floor(k) + 1`) or below it (`ceil(k) - 1`). `x > 0.5` is `x >= 1` and `x < 2.5` is `x <= 2`, while
//! `x > 1` is `x >= 2` and `x < 3` is `x <= 2`. Moving the bound a whole unit before rounding is wrong against a
//! fraction. The solver's absorption (comparisons and BETWEEN sides) and direct solve all take the shift from here so
//! that they cannot disagree.
double DecideStrictBound(bool is_lower, double k);

//! True when a decision's folded box contradicts its built-in domain, which the solver rejects with an error before it
//! reads any row (so on an empty table too). `lower` and `upper` are the box the solver folds, from the bounds as it
//! reads them (after `DecideStrictBound`, before any rounding of a fraction); an unwritten side is
//! `LogicalDecide::ABSORBED_LOWER_UNSET` or `ABSORBED_UPPER_UNSET`. An unwritten lower bound means the default floor of
//! 0. The box contradicts the domain when the upper bound is below 0 while the floor is still at or above 0, or when
//! a BOOL's floor is above 1. A purely inverted box such as `x >= 5 AND x <= 1` is not a contradiction here; the
//! elastic engine reports it. A rule that answers a query itself must decline when this is true, so that the solver
//! raises the error.
bool DecideBoxContradictsDomain(double lower, double upper, bool is_bool);

} // namespace duckdb
