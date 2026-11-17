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
#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/value.hpp"

#include <functional>

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

//! The error for a bound the decision's domain cannot hold, empty when `box` holds. The decision is `name` (an
//! auxiliary, `is_aux`, stands for what it computes: an ABS, a norm). `written_bound(upper, k)` is the clause that set
//! the bound `k` as the user wrote it, on the upper side or the lower. The check of the bounds written as constants
//! and the check of each row a scoped clause writes (`per`, `when`) both ask it, so the test and the text are one.
inline string DecideDomainRefusal(const DecideBoundBox &box, bool is_bool, bool is_aux, const string &name,
                                  const std::function<string(bool upper, double k)> &written_bound) {
	if (!DecideBoxContradictsDomain(box, is_bool)) {
		return string();
	}
	if (is_bool) {
		// Both sides can contradict at once (`x <= -1 AND x >= 2`); the upper bound is reported.
		const bool upper = box.upper < 0.0;
		return StringUtil::Format("DECIDE optimization is infeasible: %s cannot hold for a BOOL variable.",
		                          written_bound(upper, upper ? box.upper : box.lower));
	}
	if (is_aux) {
		return StringUtil::Format("DECIDE optimization is infeasible: %s cannot hold because %s is never negative.",
		                          written_bound(true, box.upper), name);
	}
	return StringUtil::Format("DECIDE optimization is infeasible: %s cannot hold because %s is non-negative (>= 0). "
	                          "Add an explicit lower bound if it may be negative.",
	                          written_bound(true, box.upper), name);
}

//! The error of an `=` bound that takes two values (`low` and `high`) in one group: `written` is the bound as written,
//! `names` the group's key columns as the query writes them and `values` the group's value of each. Without a key (a
//! bound over the whole query) it names no group. The solver path and the direct-solve plans
//! (`__internal_decide_bound_disagreement`) both build it here.
inline string DecideBoundDisagreementMessage(const string &written, double low, double high,
                                             const vector<string> &names, const vector<Value> &values) {
	auto group = DescribeKeyValues(names, values);
	return "`" + written + "` takes more than one value here (" + StringUtil::Format("%g and %g", low, high) + ")" +
	       (group.empty() ? string() : ", in the group " + group) +
	       " — an `=` bound cannot hold two different values at once, so this DECIDE query is infeasible.";
}

} // namespace duckdb
