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

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

namespace duckdb {

//! The solver model's "no bound" value: a column bound of -1e30 or 1e30, and the unset side of a singleton-row bound.
//! The builder writes it, and every backend reads it as infinite.
constexpr double DECIDE_SOLVER_INFINITY = 1e30;

//! The tolerances DeciDB sets on every backend, so the solver and the large-number warning read one value. How far a
//! value of an integer column may sit from a whole number and still count as integral (Gurobi `IntFeasTol`, HiGHS
//! `mip_feasibility_tolerance`), and how far a row may be violated and still count as satisfied (Gurobi
//! `FeasibilityTol`, HiGHS `primal_feasibility_tolerance`).
constexpr double DECIDE_INTEGRALITY_TOLERANCE = 1e-6;
constexpr double DECIDE_FEASIBILITY_TOLERANCE = 1e-6;

//! A coefficient of this magnitude or more: an integer column accepted this far from a whole number moves the row by
//! the tolerance times the coefficient, which reaches 1 (1e6). The model is left as it is; DECIDE only says so (see
//! LargeNumberWarning).
constexpr double DECIDE_LARGE_COEFFICIENT = 1.0 / DECIDE_INTEGRALITY_TOLERANCE;

//! A right-hand side or bound of this magnitude or more: a double holds a number only to its magnitude times the
//! machine epsilon, and at this size that rounding reaches the feasibility tolerance (about 4.5e9).
constexpr double DECIDE_LARGE_TOTAL = DECIDE_FEASIBILITY_TOLERANCE / std::numeric_limits<double>::epsilon();

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

//! The inclusive bound a strict `>` (`is_lower`) or `<` against `k` means on a whole-numbered decision: the first whole
//! value above `k` (`floor(k) + 1`) or below it (`ceil(k) - 1`). `x > 0.5` is `x >= 1` and `x < 2.5` is `x <= 2`, while
//! `x > 1` is `x >= 2` and `x < 3` is `x <= 2`. Moving the bound a whole unit before rounding is wrong against a
//! fraction. The solver's folding, its model builder (linear and quadratic rows) and direct solve all take the shift
//! from here so that they cannot disagree.
inline double DecideStrictBound(bool is_lower, double k) {
	return is_lower ? std::floor(k) + 1.0 : std::ceil(k) - 1.0;
}

//! The box the solver path folds a decision's unconditional bounds into: the values each bound admits for the column
//! box, or the bounds as written for the domain check (DecideWrittenBox). `lower_written` records that some
//! lower bound was written, so a written bound of -1e30 or below ("no bound") is told apart from no bound written at
//! all, which leaves the decision at its default floor of 0. The unset upper side starts at the solver sentinel, which
//! reads as infinite.
struct DecideBoundBox {
	double lower = -DECIDE_SOLVER_INFINITY;
	double upper = DECIDE_SOLVER_INFINITY;
	bool lower_written = false;

	//! The lower bound the decision has: the written one, else the default floor of 0.
	double EffectiveLower() const {
		return lower_written ? lower : 0.0;
	}
};

//! Folds one bound into a decision's box: `sense` is '<' (an upper bound), '>' (a lower bound) or '=' (both), `k` the
//! raw value. Each side keeps the tightest value written. This is the one place that says how bounds combine; the
//! solver's folding pass, the domain check's written box and direct solve's bound reader all call it.
inline void DecideFoldSingletonRow(DecideBoundBox &box, char sense, double k) {
	if (sense != '>') {
		box.upper = std::min(box.upper, k);
	}
	if (sense != '<') {
		box.lower = std::max(box.lower, k);
		box.lower_written = true;
	}
}

//! True when a solved value, rounded to the nearest whole number (half away from zero, as `std::round`), lies in
//! `[lo, hi]`, both ends included. NaN is outside every range. The solver path checks an answer on its way into an
//! integer column with this; A1 spells the same test in SQL (`abs(round(value)) <= 2^53`).
inline bool DecideRoundedAnswerWithin(double solution_value, double lo, double hi) {
	double rounded = std::round(solution_value);
	return rounded >= lo && rounded <= hi;
}

//! The text before the value in the error for a whole-numbered answer that cannot be returned exactly. The value text
//! (`%.0f` of the rounded answer) goes between the two halves, so a plan that formats the value at run time builds the
//! same message as `DecideWholeNumberRangeMessage`.
inline std::string DecideWholeNumberRangeMessageBefore(const std::string &name) {
	return "DECIDE variable '" + name + "' solves to ";
}

//! The text after the value in that error.
inline std::string DecideWholeNumberRangeMessageAfter(const std::string &name) {
	return ", which is beyond the range DeciDB can return as a whole number. Tighten the bound on '" + name +
	       "', or declare it '" + name + "(REAL)' to read the value approximately.";
}

//! The whole error: the variable's name and its rounded answer as `value_text`.
inline std::string DecideWholeNumberRangeMessage(const std::string &name, const std::string &value_text) {
	return DecideWholeNumberRangeMessageBefore(name) + value_text + DecideWholeNumberRangeMessageAfter(name);
}

//! True when a value that was converted to a DOUBLE is usable as an exact number: the double is finite and
//! `convert_back(converted)` gives the original value again. `convert_back` may throw on a double the original type
//! cannot hold; the caller decides what a throw means.
template <class ORIGINAL, class CONVERT_BACK>
inline bool DecideIsExactFiniteDouble(double converted, const ORIGINAL &original, CONVERT_BACK convert_back) {
	return std::isfinite(converted) && convert_back(converted) == original;
}

} // namespace duckdb
