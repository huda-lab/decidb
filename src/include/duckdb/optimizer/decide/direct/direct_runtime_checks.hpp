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

//! Runtime checks of direct-solve plans and the messages they raise: BOOLEAN expressions that evaluate to true for an
//! acceptable value and raise the solver's error otherwise, and the text of those errors. None of them knows which
//! problem class it is serving.

//! A BOOLEAN expression that raises the message as an error when evaluated.
unique_ptr<Expression> DirectErrorPredicate(Optimizer &optimizer, const string &message);

//! The same, with a message computed per row. The message is evaluated only on a row where the predicate is reached.
unique_ptr<Expression> DirectErrorPredicate(Optimizer &optimizer, unique_ptr<Expression> message);

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

//! A check that raises `message` as an error on a row where it is not NULL, and is true otherwise.
unique_ptr<Expression> DirectRaiseMessage(Optimizer &optimizer, const ExpressionBuilder &calc, const ExprPtr &message);

//! What a message names for a number a plan checks, as the solver path names it (ThrowInvalidValue): the expression of
//! the query the number stands for, as the user wrote it, and where it stands (the error points at it). One value is
//! named by its own text and place; a number made of several (like terms added into one coefficient) stands where the
//! least expression holding them all stands (DecideCoveringLocation). It is a constant when no value reads a column
//! (DecideValueRole::CONSTANT). `clause` is the clause (or the objective) it belongs to, which a number the plan
//! computes from values it checked before names when it is past a DOUBLE (TOO_LARGE).
struct DirectNumberNames {
	bool named = false;
	bool constant = true;
	string text;
	optional_idx place;
	string clause;
};

//! Adds `value`, an expression of the source whose text the user wrote as `text`, to what `names` calls the number. A
//! number the engine made for itself (a constant the query text holds no place for) is no value of the user's.
//! A value held by an expression of the query other than its own (`held_at`, valid then) is named by that one.
void DirectNameValue(DirectNumberNames &names, const DecideSourceText &source, const Expression &value,
                     const string &text, optional_idx held_at = optional_idx());

//! Adds the name of another number to `names`: the number is made of both.
void DirectNameMerge(DirectNumberNames &names, const DecideSourceText &source, const DirectNumberNames &other);

//! The message of a number the solver refuses (NaN or infinite), NULL for a valid number; a NULL number skips what it
//! reaches, so it is valid. A number with a name raises where the name stands; one with none, the plan's own
//! arithmetic, names `names.clause` past a DOUBLE (TOO_LARGE).
ExprPtr DirectNumberMessage(const ExpressionBuilder &calc, const ExprPtr &number, const DirectNumberNames &names);

//! The message of a number the plan computes from values it checked before: past a DOUBLE it names `clause`
//! (TOO_LARGE), as the solver path names a number it computes (AddCoefficient).
ExprPtr DirectNumberMessage(const ExpressionBuilder &calc, const ExprPtr &number, const string &clause);

//! The check of a number the plan computes from values checked before it: it raises TOO_LARGE naming `clause` when
//! `number` (a DOUBLE) is past the DOUBLE range, and passes a NULL.
unique_ptr<Expression> DirectRaiseTooLarge(Optimizer &optimizer, const ExprPtr &number, const string &clause);

//! The same for a number the plan computes from values it checked before, which `names` calls (a name with a place
//! raises where the number stands, as DirectNumberMessage does; one without names `names.clause`).
unique_ptr<Expression> DirectRaiseNotFinite(Optimizer &optimizer, const ExprPtr &number,
                                            const DirectNumberNames &names);

//! The check of a raw value of a projection: `number`, a DOUBLE column. It raises what DirectNumberMessage names.
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
