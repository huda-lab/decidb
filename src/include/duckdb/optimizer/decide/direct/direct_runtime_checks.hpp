#pragma once

#include <functional>

#include "duckdb/common/common.hpp"
#include "duckdb/common/decide_source_info.hpp"
#include "duckdb/common/decide_value_messages.hpp"
#include "duckdb/optimizer/decide/direct/direct_expression.hpp"
#include "duckdb/planner/column_binding.hpp"
#include "duckdb/planner/expression.hpp"

namespace duckdb {

class ExpressionBuilder;
class Optimizer;
struct DirectSignedValue;

//! Runtime checks of direct-solve plans: BOOLEAN expressions that evaluate to true for an acceptable value and raise
//! the solver's error otherwise. A check raises where its value is found, and the call that raises carries its own
//! place in the query (Expression::query_location), so no place travels as row data. None of them knows which problem
//! class it is serving.

//! A BOOLEAN expression that raises the message as an error, with no place, when evaluated.
unique_ptr<Expression> DirectErrorPredicate(Optimizer &optimizer, const string &message);

//! A check that is true when `ok` is true and raises `error` otherwise; a NULL `ok` raises.
unique_ptr<Expression> DirectRaiseUnless(const ExpressionBuilder &calc, unique_ptr<Expression> ok,
                                         unique_ptr<Expression> error);

//! A check that raises `error` when `bad` is true and is true otherwise; a NULL `bad` passes.
unique_ptr<Expression> DirectRaiseIf(const ExpressionBuilder &calc, unique_ptr<Expression> bad,
                                     unique_ptr<Expression> error);

//! A check that prints the number-range warning when `unstable` is true, once per query, and is always true.
unique_ptr<Expression> DirectWarnIf(const ExpressionBuilder &calc, unique_ptr<Expression> unstable);

//! True where the DOUBLE's magnitude is below the normal range (`std::numeric_limits<double>::min()`), 0 included. A
//! nonzero number there holds fewer significant bits, and a product or quotient taken below it may have become 0, so a
//! rule that computed it from a nonzero value has lost precision and passes that to DirectWarnIf.
ExprPtr DirectBelowNormalRange(const ExpressionBuilder &calc, const ExprPtr &value);

//! What a message names for a number a plan checks (DirectNamesOf): the values it is made of as the user wrote them,
//! and the place of the one value when it is one and has a place of its own. It is a constant when no value reads a
//! column (DecideValueRole::CONSTANT). `clause` is the clause (or the objective) it belongs to, which a number with no
//! value of the user's names when it is past a DOUBLE (TOO_LARGE).
struct DirectNumberNames {
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
void DirectNameValue(DirectNumberNames &names, const Expression &value, int sign, const string &text,
                     optional_idx place);

//! The check of a number the solver refuses (NaN or infinite): true for a valid number, and a NULL number skips what it
//! reaches, so it is valid. A number with a name raises where the name stands; one with none, the plan's own
//! arithmetic, raises TOO_LARGE at `names.clause`, past a DOUBLE.
ExprPtr DirectNumberCheck(const ExpressionBuilder &calc, const ExprPtr &number, const DirectNumberNames &names);

//! The check of a number the plan computes from values it checked before: past a DOUBLE it raises TOO_LARGE at
//! `clause`, as the solver path does for a number it computes (AddCoefficient).
ExprPtr DirectNumberCheck(const ExpressionBuilder &calc, const ExprPtr &number, const DecideClauseRef &clause);

//! The check of a number the plan computes from values checked before it: it raises TOO_LARGE at `clause` when
//! `number` (a DOUBLE) is past the DOUBLE range, and passes a NULL.
unique_ptr<Expression> DirectRaiseTooLarge(Optimizer &optimizer, const ExprPtr &number, const DecideClauseRef &clause);

//! The check of a raw value of a projection: `number`, a DOUBLE column. It raises what DirectNumberCheck names.
//! `values` are what the number is made of (with the part's `factor`). Null when they are finite constants, which no
//! row can fail (DirectIsFiniteWhereKept).
unique_ptr<Expression> DirectCheckNumber(Optimizer &optimizer, ColumnBinding number, const DirectNumberNames &names,
                                         const vector<DirectSignedValue> &values,
                                         const DirectFactor &factor = DirectFactor());

//! Joins checks into one BOOLEAN expression that is true when every check is. The checks run in the order given, as a
//! CASE chain, so an earlier check guards the later ones: a row raises the error of the first check it fails. A check
//! that is false or NULL, not an error, makes the whole false. No checks join to true.
unique_ptr<Expression> DirectAllHold(const ExpressionBuilder &calc, vector<unique_ptr<Expression>> checks);

//! DECIDE's infeasibility error, as a BOOLEAN expression that raises it when evaluated.
unique_ptr<Expression> DirectInfeasibleErrorPredicate(Optimizer &optimizer);

//! DECIDE's unboundedness error: some decision can improve the objective without limit.
unique_ptr<Expression> DirectUnboundedErrorPredicate(Optimizer &optimizer);

//! Evaluates to true for a DOUBLE bound that is not NaN, and raises the bound error, naming the bound as the user
//! `written` it, at `place` in the query, otherwise. Infinity is a valid bound (no bound on that side), and a NULL one
//! skips the instance it reaches, which the plan reads as no bound.
unique_ptr<Expression> DirectValidBoundPredicate(Optimizer &optimizer, ColumnBinding bound_binding,
                                                 const string &written, optional_idx place);

} // namespace duckdb
