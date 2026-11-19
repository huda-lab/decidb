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
#include "duckdb/common/decide_numeric_policy.hpp"
#include "duckdb/common/decide_source_info.hpp"
#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/value.hpp"

#include <functional>

namespace duckdb {

//! What a number the model reads is: a coefficient must be finite, and a bound only has to be a number (an infinite
//! bound means "no bound" or "nothing satisfies it"). A coefficient that is not finite is named where it stands in the
//! query: its own expression, or, for one DeciDB made from the user's text (like terms added into one, a factor
//! applied, a quotient's reciprocal), the text it replaces (DecideReplaces). COEFFICIENT is a value of some rows;
//! CONSTANT reads no column (`1 / 0`, an `'Infinity'` literal), so no row can be skipped. TOO_LARGE is no number read
//! but one the model computes across rows (a sum over rows, a square, the fixed terms moved to the bound side), which
//! no expression of the query holds: past a DOUBLE it names the clause. INTEGER_ANSWER is no number the query holds
//! either but an answer the solver reads back for a whole-numbered decision, past what a DOUBLE counts exactly; its
//! `written` is the decision and its value (DecideWholeNumberRangeBefore). MESSAGE is no refusal of a number but one a
//! direct plan carries as a string until its check chain raises it, so the least of several can be raised in order:
//! `written` is the message, with the place it points at in front (DecideLocatedMessage). The solver path
//! (ThrowInvalidValue) and the direct-solve plans (DirectNumberMessage) apply the one rule.
enum class DecideValueRole : uint8_t { COEFFICIENT, BOUND, TOO_LARGE, CONSTANT, INTEGER_ANSWER, MESSAGE };

//! The error for a NaN or infinite value, the one text every site that refuses one raises. `written` is the value as
//! the user wrote it (cut from the query where it stands, or the tree rendered where no text was cut); the error
//! points at it (`Exception::InitializeExtraInfo`), so the sentence only names it. It names no row: a row's position
//! in the input depends on the scan.
//! For TOO_LARGE, `written` is what the sum belongs to (DecideClauseRef::Noun): the error points at the clause.
inline string DecideInvalidValueMessage(DecideValueRole role, const string &written) {
	if (role == DecideValueRole::TOO_LARGE) {
		return "the " + written + " computes a number too large for a DOUBLE";
	}
	if (role == DecideValueRole::CONSTANT) {
		return "'" + written + "' is NaN or infinite";
	}
	if (role == DecideValueRole::INTEGER_ANSWER) {
		return written + DECIDE_WHOLE_NUMBER_RANGE_AFTER;
	}
	if (role == DecideValueRole::BOUND) {
		return "the bound '" + written + "' is NaN on some rows";
	}
	return "'" + written + "' is NaN or infinite on some rows";
}

//! A refusal a direct plan carries as a string value: `message` with the `place` it points at (a position in the query)
//! in front, which DecideLocatedError reads back where the plan raises it. A refusal with no place is the message.
inline string DecideLocatedMessage(const string &message, optional_idx place) {
	return place.IsValid() ? to_string(place.GetIndex()) + '\x1f' + message : message;
}

//! The error of a message DecideLocatedMessage made.
inline InvalidInputException DecideLocatedError(const string &located) {
	auto separator = located.find('\x1f');
	if (separator == string::npos) {
		return InvalidInputException(located);
	}
	return InvalidInputException(located.substr(separator + 1), Exception::InitializeExtraInfo(optional_idx(
	                                                                std::stoull(located.substr(0, separator)))));
}

//! The error for a NaN or infinite value, raised where the value stands: the message of DecideInvalidValueMessage,
//! with `place` (the value's span in the query, invalid for a value the query text holds none for) as its position,
//! so DuckDB prints the caret under it. The solver path and the direct-solve plans both throw it here, as the SQL
//! function `__internal_decide_invalid_value` does.
inline InvalidInputException DecideInvalidValueError(DecideValueRole role, const string &written, optional_idx place) {
	if (role == DecideValueRole::MESSAGE) {
		return DecideLocatedError(written);
	}
	auto message = DecideInvalidValueMessage(role, written);
	return place.IsValid() ? InvalidInputException(message, Exception::InitializeExtraInfo(place))
	                       : InvalidInputException(message);
}

//! The error of a number the model computes across the rows of `clause` that is past a DOUBLE.
inline InvalidInputException DecideTooLargeError(const DecideClauseRef &clause) {
	return DecideInvalidValueError(DecideValueRole::TOO_LARGE, clause.Noun(), clause.place);
}

//! The error of a model with no feasible point, and of one whose objective improves without end. Both paths raise
//! them, from these two texts.
inline string DecideInfeasibleMessage() {
	return "the DECIDE query is infeasible";
}

inline string DecideUnboundedMessage() {
	return "the DECIDE query is unbounded";
}

//! The outcomes a solve can end in without an answer: the solver could not tell the two apart, or it stopped at a
//! limit.
inline string DecideInfeasibleOrUnboundedMessage() {
	return "the DECIDE query is infeasible or unbounded";
}

inline string DecideTimeLimitMessage() {
	return "the DECIDE query reached the time limit";
}

inline string DecideIterationLimitMessage() {
	return "the DECIDE query reached the iteration limit";
}

//! The objective as a plan and a warning name it: the sense, then the objective as EXPLAIN prints it
//! (`MAXIMIZE sum(a * x)`). Empty with no objective.
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

//! One column of a group, as a message names it: `D.region = 'East'`, or `D.region IS NULL` for a NULL, which an
//! equality would never select.
inline string DescribeKeyValue(const string &name, const Value &value) {
	return name + (value.IsNull() ? " IS NULL" : " = " + DecideSqlLiteral(value));
}

//! A class or group, as a message names it: `D.depotID = 'D1', D.region = 'East'`. `values` holds the group's value of
//! each column, `names` the columns as the query writes them (it may name more than `values` reads).
inline string DescribeKeyValues(const vector<string> &names, const vector<Value> &values) {
	D_ASSERT(names.size() >= values.size());
	string result;
	for (idx_t i = 0; i < values.size(); i++) {
		result += (i > 0 ? ", " : "") + DescribeKeyValue(names[i], values[i]);
	}
	return result;
}

//! True when `box`, the bounds written on one decision, contradicts the decision's built-in domain. A box with no lower
//! bound written has the default floor of 0. It contradicts the domain when the upper bound is below 0 while the floor
//! is still at or above 0 (a BOOL's floor is always 0, so for it any upper bound below 0), or when a BOOL's floor is
//! above 1. A purely inverted box such as `x >= 5 AND x <= 1` is not a contradiction here; the elastic engine reports
//! it.
inline bool DecideBoxContradictsDomain(const DecideBoundBox &box, bool is_bool) {
	double floor = box.EffectiveLower();
	// A BOOLEAN's floor is 0 whatever lower bound was written (a lower bound below 0 changes nothing), so an upper
	// bound below 0 contradicts it even when the written lower bound is negative (`x = -1`).
	return (box.upper < 0.0 && (floor >= 0.0 || is_bool)) || (is_bool && floor > 1.0);
}

//! A bound the decision's domain cannot hold: the error and which side of the box it is. `message` is empty when the
//! box holds. `upper` and `k` say the bound that contradicts the domain, which a caller looks up among the clauses it
//! reads, so the error points at the clause that wrote it.
struct DecideDomainRefusal {
	string message;
	bool upper = true;
	double k = 0.0;
};

//! The refusal of `box`, the bounds written on one decision called `name` (an auxiliary stands for what it computes: an
//! ABS, a norm). The check of the bounds written as constants and the check of each row a scoped clause writes
//! (`per`, `when`) both ask it, so the test and the text are one.
inline DecideDomainRefusal DecideRefuseDomain(const DecideBoundBox &box, bool is_bool, const string &name) {
	DecideDomainRefusal refusal;
	if (!DecideBoxContradictsDomain(box, is_bool)) {
		return refusal;
	}
	// Both sides can contradict at once for a BOOL (`x <= -1 AND x >= 2`); the upper bound is reported.
	refusal.upper = !is_bool || box.upper < 0.0;
	refusal.k = refusal.upper ? box.upper : box.lower;
	refusal.message =
	    "the constraint cannot hold: '" + name + "' " + (is_bool ? "is a BOOL decision" : "is never negative");
	return refusal;
}

//! The error of an `=` bound that takes two values (`low` and `high`) in one `by` group: `written` is the bound as the
//! query writes it, `names` the group's key columns as the query writes them and `values` the group's value of each.
//! Without a key (a bound over the whole query) it names no group. The solver path, the frame bound and the
//! direct-solve plans (`__internal_decide_bound_disagreement`) all build it here.
inline string DecideBoundDisagreementMessage(const string &written, double low, double high,
                                             const vector<string> &names, const vector<Value> &values) {
	auto group = DescribeKeyValues(names, values);
	return "the = bound '" + written + "' takes two values (" + StringUtil::Format("%g and %g", low, high) + ")" +
	       (group.empty() ? string() : " in the group " + group);
}

} // namespace duckdb
