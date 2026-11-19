//===----------------------------------------------------------------------===//
//                         DeciDB
//
// duckdb/optimizer/decide/decide_value_checks.hpp
//
// The per-row pieces of a value check (LogicalDecide::value_checks) and of the checks a direct plan makes: which rows a
// `when` keeps, and the check of a number the solver refuses, raised with the message that names it.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/decide_source_info.hpp"
#include "duckdb/optimizer/decide/decide_expression_builder.hpp"

namespace duckdb {

//! Whether a clause's `when` holds on a row: true without one, and false when it is false or NULL.
unique_ptr<Expression> DecideActiveWhen(const ExpressionBuilder &calc, const Expression *when);

//! `value` on a row where `kept` is true, and NULL on any other; `value` itself when `kept` is null (every row is
//! kept). Nothing is computed on a row a `when` drops, so a value that raises (a logarithm of 0, an overflow) raises
//! only on a kept row, as on the solver path.
ExprPtr DecideOnKeptRows(const ExpressionBuilder &calc, const ExprPtr &kept, const ExprPtr &value);

//! The same for a check (a BOOLEAN that is true or raises): it holds on a row `kept` drops, so the row raises nothing
//! (a row a `when` drops raises nothing). The guard stays inside the expression, where no pushdown can move the check
//! below it.
ExprPtr DecideCheckOnKeptRows(const ExpressionBuilder &calc, const ExprPtr &kept, const ExprPtr &check);

//! What a message names for a number a check reads: the values it is made of as the user wrote them,
//! and the place of the one value when it is one and has a place of its own. It is a constant when no value reads a
//! column (DecideValueRole::CONSTANT). `clause` is the clause (or the objective) it belongs to, which a number with no
//! value of the user's names when it is past a DOUBLE (TOO_LARGE).
struct DecideNumberNames {
	//! How many values of the user's the number is made of; none names the clause.
	idx_t values = 0;
	bool constant = true;
	//! The first value's sign, written in front of the text once a second value joins it.
	int first_sign = 1;
	string text;
	optional_idx place;
	DecideClauseRef clause;
};

//! Adds `value`, written as `text` and standing at `place` in the query, to what `names` calls the number, with its
//! `sign`. A constant the engine made for itself (no place in the query) is no value of the user's. A number made of
//! several values is named by their texts and has no place.
void DecideNameValue(DecideNumberNames &names, const Expression &value, int sign, const string &text,
                     optional_idx place);

//! The check of a number the solver refuses (NaN or infinite): true for a valid number, and a NULL number skips what it
//! reaches, so it is valid. A number with a name raises where the name stands; one with none, the plan's own
//! arithmetic, raises TOO_LARGE at `names.clause`, past a DOUBLE.
ExprPtr DecideNumberCheck(const ExpressionBuilder &calc, const ExprPtr &number, const DecideNumberNames &names);

//! The check of a number the plan computes from values it checked before: past a DOUBLE it raises TOO_LARGE at
//! `clause`, as the solver path does for a number it computes (AddCoefficient).
ExprPtr DecideNumberCheck(const ExpressionBuilder &calc, const ExprPtr &number, const DecideClauseRef &clause);

} // namespace duckdb
