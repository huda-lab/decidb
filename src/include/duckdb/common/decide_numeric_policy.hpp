//===----------------------------------------------------------------------===//
//                         DecidB
//
// duckdb/common/decide_numeric_policy.hpp
//
// The numeric facts every DECIDE stage shares: what the solver reads as "no bound", how large a whole number a
// DOUBLE still counts exactly, and what makes a converted value exact and finite.
//
// It lives in `common/` and includes nothing from DuckDB, because the planner, the optimizer (including the
// direct-solve rules), the model builder and the solver backends all read it, and none of them may include another's
// headers. A number written here once cannot drift from the same number written in a second layer.
//
// Scope. This header names the facts that DuckDB's own evaluation cannot supply: the solver's sentinels, the limits of
// a DOUBLE, and the check on a value that was converted to a DOUBLE. Whether an expression overflows or raises is
// DuckDB's to answer; a rule that needs it evaluates the expression (parity by construction) rather than proving a
// range here.
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cmath>

namespace duckdb {

//! The solver model's "no bound" value: a column bound of -1e30 or 1e30, and the unset side of an absorbed bound. The
//! builder writes it, and every backend reads it as infinite.
constexpr double DECIDE_SOLVER_INFINITY = 1e30;

//! A finite bound at or beyond this magnitude is read as infinite (the variable is unbounded in that direction). It is
//! the solver sentinel with a margin, so a bound that arithmetic pushed near the sentinel is still recognised.
constexpr double DECIDE_EFFECTIVE_INFINITY = 1e20;

//! The largest whole number a DOUBLE holds exactly (2^53). A count limit or an INT answer beyond it cannot be read or
//! returned exactly.
constexpr double DECIDE_MAX_EXACT_INTEGER = 9007199254740992.0;

//! Below this magnitude (2^52) a DOUBLE can still carry a fraction, so rounding a bound can change it.
constexpr double DECIDE_MAX_FRACTIONAL_MAGNITUDE = 4503599627370496.0;

//! The smallest DOUBLE that no BIGINT holds (2^63). A DOUBLE at or beyond it cannot be cast to BIGINT without raising;
//! a plan that casts a double answer to BIGINT guards on it.
constexpr double DECIDE_FIRST_DOUBLE_BEYOND_BIGINT = 9223372036854775808.0;

//! True when an upper bound leaves the variable unbounded above.
inline bool DecideIsOpenAbove(double upper_bound) {
	return upper_bound >= DECIDE_EFFECTIVE_INFINITY;
}

//! True when a lower bound leaves the variable unbounded below.
inline bool DecideIsOpenBelow(double lower_bound) {
	return lower_bound <= -DECIDE_EFFECTIVE_INFINITY;
}

//! True when the value is finite and no larger in magnitude than 2^53: a whole number in this range, and only such a
//! number, is held exactly by a DOUBLE. NaN and the infinities are outside it.
inline bool DecideIsWithinExactRange(double value) {
	return std::isfinite(value) && std::fabs(value) <= DECIDE_MAX_EXACT_INTEGER;
}

//! True when a value that was converted to a DOUBLE is usable as an exact number: the double is finite and
//! `convert_back(converted)` gives the original value again. `convert_back` may throw on a double the original type
//! cannot hold; the caller decides what a throw means.
template <class ORIGINAL, class CONVERT_BACK>
inline bool DecideIsExactFiniteDouble(double converted, const ORIGINAL &original, CONVERT_BACK convert_back) {
	return std::isfinite(converted) && convert_back(converted) == original;
}

} // namespace duckdb
