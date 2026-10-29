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
#include "duckdb/common/types.hpp"

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
			string result;
			for (auto character : text) {
				result += character;
				if (character == '%') {
					result += '%';
				}
			}
			return result;
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
