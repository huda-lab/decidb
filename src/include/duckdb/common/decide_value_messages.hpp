//===----------------------------------------------------------------------===//
//                         DeciDB
//
// duckdb/common/decide_value_messages.hpp
//
// What DECIDE tells the user when a number it reads from the source cannot be used: a NaN or infinite one. The solver
// path and the direct-solve plans both raise these errors, and both build their text here, so one sentence cannot read
// two ways.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/value.hpp"

namespace duckdb {

//! What a number the model reads is: a coefficient must be finite, and a bound only has to be a number (an infinite
//! bound means "no bound" or "nothing satisfies it"). TOO_LARGE is no number read but one the model computes from
//! finite ones: like coefficients merged into one, a factor applied, a square, the fixed terms moved to the bound side.
//! Past a DOUBLE it names the clause, as no row holds it. A coefficient computed from values a column holds is checked
//! once, on the result; a row where it is not finite names the first of those values that is not finite (COEFFICIENT),
//! and the clause when each is (TOO_LARGE). The solver path (ThrowInvalidValue) and the direct-solve plans
//! (DirectNumberMessage) apply the one rule. CONSTANT is a coefficient that is not a column's value but a constant, NaN
//! or infinite itself (`1 / 0`, an
//! `'Infinity'` literal): SQL computes it, DeciDB's arithmetic did not, and no row can be skipped.
enum class DecideValueRole : uint8_t { COEFFICIENT, BOUND, TOO_LARGE, CONSTANT };

//! The error for a NaN or infinite value, the one text every site that refuses one raises. `written` is the value as
//! the user wrote it (the cut text of the query, or the tree rendered where no text was cut). The message gives the
//! smallest edit, a `when` that skips the rows, and names no row: a row's position in the input depends on the scan.
//! For TOO_LARGE, `written` is the clause (or the objective) the sum belongs to, and the edit is to scale its data.
inline string DecideInvalidValueMessage(DecideValueRole role, const string &written) {
	if (role == DecideValueRole::TOO_LARGE) {
		return "DECIDE: " + written + " computes a number too large for a DOUBLE. Scale its data down.";
	}
	if (role == DecideValueRole::CONSTANT) {
		return "DECIDE: " + written + " is NaN or infinite. Use a finite value.";
	}
	if (role == DecideValueRole::BOUND) {
		return "DECIDE: the bound " + written + " is NaN on some rows. Skip those rows with 'when NOT isnan(" +
		       written + "):'.";
	}
	return "DECIDE: " + written + " is NaN or infinite on some rows. Skip those rows with 'when isfinite(" + written +
	       "):'.";
}

//! The error of a model with no feasible point, and of one whose objective improves without end. Both paths raise
//! them, from these two texts. They advise prefixing the query with DIAGNOSE only where DIAGNOSE would run on it
//! (`diagnose_runs`): a query whose model no solver on the host takes is refused by DIAGNOSE
//! (RequireDecideSolverSupport) before it names anything. The solver path raises them only after a backend ran the
//! model, so it passes true.
inline string DecideInfeasibleMessage(bool diagnose_runs) {
	return diagnose_runs ? "DECIDE optimization is infeasible. Prefix the query with DIAGNOSE to see which clause to "
	                       "change."
	                     : "DECIDE optimization is infeasible.";
}

inline string DecideUnboundedMessage(bool diagnose_runs) {
	return diagnose_runs ? "DECIDE optimization is unbounded. Prefix the query with DIAGNOSE to see which decision "
	                       "needs a bound."
	                     : "DECIDE optimization is unbounded.";
}

//! The error for a NaN a frame's reducer reads (`written`, the reducer or range as written): an error wherever its
//! aggregate would pass over it, as SQL's MIN does. A frame cannot stand in a `when`, so the edit names the column.
inline string DecideFrameNanMessage(const string &written) {
	return "DECIDE: " + written + " reads a NaN on some rows. Skip those rows with a 'when' on the column it reads.";
}

//! The objective as a TOO_LARGE message names it: the sense, then the objective as EXPLAIN prints it
//! (`MAXIMIZE sum(a * x)`). Empty with no objective. The solver path and the direct-solve plans both build it here.
inline string DecideWrittenObjective(DecideSense sense, const string &objective) {
	if (sense == DecideSense::FEASIBILITY) {
		return string();
	}
	return (sense == DecideSense::MAXIMIZE ? "MAXIMIZE " : "MINIMIZE ") + objective;
}

//! A value as SQL writes it in a message: 'East', 10, NULL. A number or a Boolean is bare, any other type is quoted.
inline bool DecideLiteralIsBare(const LogicalType &type) {
	return type.IsNumeric() || type.id() == LogicalTypeId::BOOLEAN;
}

//! A value as SQL writes it, for a message: 'East', 10, NULL.
inline string DecideSqlLiteral(const Value &value) {
	if (value.IsNull()) {
		return "NULL";
	}
	if (DecideLiteralIsBare(value.type())) {
		return value.ToString();
	}
	return "'" + StringUtil::Replace(value.ToString(), "'", "''") + "'";
}

} // namespace duckdb
