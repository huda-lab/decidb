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

//! Evaluates to true for a DOUBLE bound that is not NaN, and raises the bound error otherwise. Infinity is a valid
//! bound (no bound on that side), and a NULL one skips the instance it reaches, which the plan reads as no bound.
unique_ptr<Expression> DirectValidBoundPredicate(Optimizer &optimizer, ColumnBinding bound_binding);

//! The solver's error for a NaN bound, without the row position the solver adds.
string DirectBoundNaNMessage();
//! The solver's message for a coefficient that is NaN or infinite.
string DirectCoefficientInvalidMessage();

//! Which clause a number belongs to, so an error names the object the user wrote.
enum class DirectNumberKind : uint8_t { OBJECTIVE, CONSTRAINT };

//! Evaluates to true for a finite number and raises an error for a non-finite one, naming `kind`. A NULL number skips
//! what it reaches (a SUM reads a skipped row's terms as 0, DirectIR), so it passes.
unique_ptr<Expression> DirectValidNumberPredicate(Optimizer &optimizer, const LogicalType &number_type,
                                                  ColumnBinding number_binding, DirectNumberKind kind);

} // namespace duckdb
