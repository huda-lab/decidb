#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/planner/column_binding.hpp"
#include "duckdb/planner/expression.hpp"

namespace duckdb {

class Optimizer;

//! Runtime checks of direct-solve plans and the messages they raise: BOOLEAN expressions that evaluate to true for an
//! acceptable value and raise the solver's error otherwise, and the text of those errors. None of them knows which
//! problem class it is serving.

//! A BOOLEAN expression that raises the message as an error when evaluated.
unique_ptr<Expression> DirectErrorPredicate(Optimizer &optimizer, const string &message);

//! The same, with a message computed per row. The message is evaluated only on a row where the predicate is reached.
unique_ptr<Expression> DirectErrorPredicate(Optimizer &optimizer, unique_ptr<Expression> message);

//! A check that is true when `ok` is true and raises `error` otherwise; a NULL `ok` raises.
unique_ptr<Expression> DirectRaiseUnless(unique_ptr<Expression> ok, unique_ptr<Expression> error);

//! A check that raises `error` when `bad` is true and is true otherwise; a NULL `bad` passes.
unique_ptr<Expression> DirectRaiseIf(unique_ptr<Expression> bad, unique_ptr<Expression> error);

//! Joins checks into one BOOLEAN expression that is true when every check is. The checks run in the order given, as a
//! CASE chain, so an earlier check guards the later ones: a row raises the error of the first check it fails. A check
//! that is false or NULL, not an error, makes the whole false. No checks join to true.
unique_ptr<Expression> DirectAllHold(vector<unique_ptr<Expression>> checks);

//! DECIDE's infeasibility error, as a BOOLEAN expression that raises it when evaluated.
unique_ptr<Expression> DirectInfeasibleErrorPredicate(Optimizer &optimizer);

//! DECIDE's unboundedness error: some decision can improve the objective without limit.
unique_ptr<Expression> DirectUnboundedErrorPredicate(Optimizer &optimizer);

//! The solver's NULL message for a value, as a VARCHAR expression over the row it is evaluated on: it names the columns
//! of `value` that are NULL on that row (the leaves that still carry the name the user wrote), and quotes the value
//! when none is. The DECIDE optimizer's right-hand-side window rewrite uses it too.
unique_ptr<Expression> DirectNullMessageOf(Optimizer &optimizer, const Expression &value);

//! Evaluates to true for a DOUBLE bound that is neither NULL nor NaN, and raises the bound error otherwise. Infinity is
//! a valid bound (no bound on that side). `source_value` is the bound as the user wrote it, over the columns the check
//! reads, so a NULL names the column it came from.
unique_ptr<Expression> DirectValidBoundPredicate(Optimizer &optimizer, ColumnBinding bound_binding,
                                                 const Expression &source_value);

//! The errors a source column used as a bound raises, for a plan that counts the bad rows and raises one fixed message
//! (the column is the same on every row): NULL, and NaN. Both are the solver's text, built by the function the solver
//! uses; the NaN message lacks the solver's row position.
string DirectBoundNullMessage(const string &column_name);
string DirectBoundNaNMessage();

//! Which clause a number belongs to, so an error names the object the user wrote.
enum class DirectNumberKind : uint8_t { OBJECTIVE, CONSTRAINT };

//! Evaluates to true for a finite number and raises an error for a NULL or non-finite one. A NULL number's message is
//! the solver's: it names the columns that are NULL in `source`, the expression the number is made of as the solver
//! evaluates it, over the columns the check reads, and quotes `source` when no column is NULL. A null `source` makes
//! the message generic. The NaN and infinity message names `kind`.
unique_ptr<Expression> DirectValidNumberPredicate(Optimizer &optimizer, const LogicalType &number_type,
                                                  ColumnBinding number_binding, const Expression *source,
                                                  DirectNumberKind kind);

//! The error for a NULL or NaN bound that is not exactly one source column. Which column is NULL depends on the row,
//! and a plan that counts bad rows has no single row to name, so it points at the bound expression.
string DirectInvalidBoundMessage();

} // namespace duckdb
