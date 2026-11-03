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

//! What a number the model reads is: a coefficient must be finite, a bound only has to be a number (an infinite bound
//! means "no bound" or "nothing satisfies it"), and a bound a rewrite needs (ABS, MIN, MAX, `<>`) must be finite.
//! TOO_LARGE is no number read but one the model computes from finite ones: like coefficients merged into one, a
//! factor applied, a square, the fixed terms moved to the bound side. Past a DOUBLE it names the clause, as no row
//! holds it. A coefficient computed from values a column holds is checked once, on the result; a row where it is not
//! finite names the first of those values that is not finite (COEFFICIENT), and the clause when each is (TOO_LARGE).
//! The solver path (ThrowInvalidValue) and the direct-solve plans (DirectNumberMessage) apply the one rule.
enum class DecideValueRole : uint8_t { COEFFICIENT, BOUND, FINITE_BOUND, TOO_LARGE };

//! The error for a NaN or infinite value, the one text every site that refuses one raises. `written` is the value as
//! the user wrote it (the cut text of the query, or the tree rendered where no text was cut). The message gives the
//! smallest edit, a `when` that skips the rows, and names no row: a row's position in the input depends on the scan.
//! For TOO_LARGE, `written` is the clause (or the objective) the sum belongs to, and the edit is to scale its data.
inline string DecideInvalidValueMessage(DecideValueRole role, const string &written) {
	if (role == DecideValueRole::TOO_LARGE) {
		return "DECIDE: " + written + " computes a number too large for a DOUBLE. Scale its data down.";
	}
	if (role == DecideValueRole::FINITE_BOUND) {
		return "DECIDE: the bound " + written +
		       " is infinite on some rows, and this clause needs a finite one. "
		       "Skip those rows with 'when isfinite(" +
		       written + "):'.";
	}
	if (role == DecideValueRole::BOUND) {
		return "DECIDE: the bound " + written + " is NaN on some rows. Skip those rows with 'when NOT isnan(" +
		       written + "):'.";
	}
	return "DECIDE: " + written + " is NaN or infinite on some rows. Skip those rows with 'when isfinite(" + written +
	       "):'.";
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
