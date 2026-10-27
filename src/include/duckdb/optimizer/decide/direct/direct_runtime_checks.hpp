#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/planner/column_binding.hpp"
#include "duckdb/planner/expression.hpp"

namespace duckdb {

class ExpressionBuilder;
class Optimizer;

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

//! The check, evaluated only on a row where `kept` is true. A value on a row a `when` drops is never read, so it raises
//! nothing there, as on the solver path.
unique_ptr<Expression> DirectCheckOnKeptRows(const ExpressionBuilder &calc, unique_ptr<Expression> kept,
                                             unique_ptr<Expression> check);

//! Joins checks into one BOOLEAN expression that is true when every check is. The checks run in the order given, as a
//! CASE chain, so an earlier check guards the later ones: a row raises the error of the first check it fails. A check
//! that is false or NULL, not an error, makes the whole false. No checks join to true.
unique_ptr<Expression> DirectAllHold(const ExpressionBuilder &calc, vector<unique_ptr<Expression>> checks);

//! DECIDE's infeasibility error, as a BOOLEAN expression that raises it when evaluated.
unique_ptr<Expression> DirectInfeasibleErrorPredicate(Optimizer &optimizer);

//! DECIDE's unboundedness error: some decision can improve the objective without limit.
unique_ptr<Expression> DirectUnboundedErrorPredicate(Optimizer &optimizer);

//! The solver's NULL message for a value, as a VARCHAR expression over the row it is evaluated on: it names the first
//! flattened subquery of `value` that is NULL on that row, else the columns that are (the leaves that still carry the
//! name the user wrote), else quotes the value as written. `fragments` is the DECIDE node's source-fragment registry,
//! which holds the written text of each subquery and cast. The bound-side window rewrite uses it too.
unique_ptr<Expression> DirectNullMessageOf(Optimizer &optimizer, const Expression &value,
                                           const vector<string> &fragments);

//! Evaluates to true for a DOUBLE bound that is neither NULL nor NaN, and raises the bound error otherwise. Infinity is
//! a valid bound (no bound on that side). `source_value` is the bound as the user wrote it, over the columns the check
//! reads, so a NULL names the column it came from.
unique_ptr<Expression> DirectValidBoundPredicate(Optimizer &optimizer, ColumnBinding bound_binding,
                                                 const Expression &source_value, const vector<string> &fragments);

//! The solver's error for a NaN bound, without the row position the solver adds.
string DirectBoundNaNMessage();

//! Which clause a number belongs to, so an error names the object the user wrote.
enum class DirectNumberKind : uint8_t { OBJECTIVE, CONSTRAINT };

//! Evaluates to true for a finite number and raises an error for a NULL or non-finite one. A NULL number's message is
//! the solver's: it names the columns that are NULL in `source`, the expression the number is made of as the solver
//! evaluates it, over the columns the check reads, and quotes `source` when no column is NULL. A null `source` makes
//! the message generic. The NaN and infinity message names `kind`. `fragments` is as for DirectNullMessageOf.
unique_ptr<Expression> DirectValidNumberPredicate(Optimizer &optimizer, const LogicalType &number_type,
                                                  ColumnBinding number_binding, const Expression *source,
                                                  DirectNumberKind kind, const vector<string> &fragments);

} // namespace duckdb
