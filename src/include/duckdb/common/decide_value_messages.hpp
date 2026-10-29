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
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types.hpp"

namespace duckdb {

//! What a number the model reads is: a coefficient must be finite, a bound only has to be a number (an infinite bound
//! means "no bound" or "nothing satisfies it"), and a bound a rewrite needs (ABS, MIN, MAX, `<>`) must be finite.
//! TOO_LARGE is no number read but one the model computes from finite ones: like coefficients merged into one, a
//! square, the fixed terms moved to the bound side. Past a DOUBLE it names the clause, as no row holds it.
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

//! The error of a scope check that finds a class showing more than one value or decision (language/guide.md §7.3).
//! The solver path (PhysicalDecide::RunScopeChecks) and the direct-solve plans both raise it from this one text. A
//! message names the least failing group of the check's key and quotes the group's least and greatest value, NULL
//! after every value as ORDER BY places it, so it never depends on the order rows arrive in.
struct DecideDeterminesText {
	//! The key as written (`D.region`, `()`), what it must determine (`D.capacity`, a decision's name), and where the
	//! check stands (`in by (D.region)`, `declared per row`); the context is empty for none.
	string subject;
	string object;
	string context;
	//! The empty key: one class, so no group is named.
	bool one_class = false;
	//! The check is on a decision rather than on a value.
	bool decision = false;

	//! A printf format: `%s` for the group (none for one class), then for a value check `%s, %s` for the least and the
	//! greatest value.
	string Format() const {
		auto escaped = [](const string &text) {
			return StringUtil::Replace(text, "%", "%%");
		};
		string head = "DECIDE: per " + escaped(subject) + " does not determine " + (decision ? "the decision " : "") +
		              escaped(object) + (context.empty() ? "" : " (" + escaped(context) + ")") + ". ";
		if (decision) {
			return head + (one_class ? "The kept rows cover" : "%s covers") + " more than one " + escaped(object) + ".";
		}
		return head + (one_class ? "The kept rows have" : "%s has") + " more than one value: %s, %s.";
	}
};

//! A value as SQL writes it in a message: 'East', 10, NULL. A number or a Boolean is bare, any other type is quoted.
inline bool DecideLiteralIsBare(const LogicalType &type) {
	return type.IsNumeric() || type.id() == LogicalTypeId::BOOLEAN;
}

} // namespace duckdb
