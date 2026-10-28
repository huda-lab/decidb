//===----------------------------------------------------------------------===//
//                         DecidB
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

namespace duckdb {

//! The error for a NaN or infinite value. `context` names where the value sits (`objective coefficient`), `kind` what
//! is not allowed (`NaN` or `NaN or Infinity`). It names no row: a row's position in the input depends on the scan.
inline string DecideInvalidValueMessage(const string &context, const string &kind) {
	return "DECIDE " + context + " contains invalid value (" + kind +
	       "). Common causes:\n"
	       "  • Division by zero in the expression\n"
	       "  • Arithmetic overflow in calculations\n"
	       "Check your expressions and input data.";
}

//! A `when` in front of a clause that keeps no row of a non-empty input (D4). `condition` is the condition as written.
inline string DecideWhenMatchesNoRowsMessage(const string &condition) {
	return "DECIDE: when " + condition + " matches no rows.";
}

//! A reducer whose own `when` keeps no row of the rows its clause keeps (D8, no `by` key). `reducer` is as written.
inline string DecideReducerHasNoRowsMessage(const string &reducer) {
	return "DECIDE: " + reducer + " has no rows.";
}

} // namespace duckdb
