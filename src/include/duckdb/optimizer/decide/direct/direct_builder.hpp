#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/planner/bound_result_modifier.hpp"
#include "duckdb/planner/column_binding.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/logical_operator.hpp"

namespace duckdb {

class Binder;
class Optimizer;

//! Rule-independent building blocks for direct-solve relational plans. A rule composes these into its own
//! construction; none of them knows which problem class it is serving.

//! Typed reference to a child column.
unique_ptr<Expression> DirectColumn(const LogicalType &type, ColumnBinding binding);

unique_ptr<Expression> DirectConstantBool(bool value);

//! `value IS NULL`.
unique_ptr<Expression> DirectIsNull(unique_ptr<Expression> value);

//! COUNT over the whole partition of rows where the predicate holds.
unique_ptr<BoundWindowExpression> DirectWindowMatchingCount(unique_ptr<Expression> predicate);

//! A BOOLEAN expression that raises the message as an error when evaluated.
unique_ptr<Expression> DirectErrorPredicate(Optimizer &optimizer, const string &message);

//! The same, with a message computed per row. The message is evaluated only on a row where the predicate is reached.
unique_ptr<Expression> DirectErrorPredicate(Optimizer &optimizer, unique_ptr<Expression> message);

//! A per-row numeric value read as the solver reads a coefficient: cast to DOUBLE, negated when `sign` is negative,
//! then multiplied or divided by the query-wide factor of its reducer (`scale`, when `scaled`).
unique_ptr<Expression> DirectScaledTerm(Optimizer &optimizer, unique_ptr<Expression> value, int sign, bool scaled,
                                        double scale, bool divides);

//! DECIDE's infeasibility error, as a BOOLEAN expression that raises it when evaluated.
unique_ptr<Expression> DirectInfeasibleErrorPredicate(Optimizer &optimizer);

//! DECIDE's unboundedness error: some decision can improve the objective without limit.
unique_ptr<Expression> DirectUnboundedErrorPredicate(Optimizer &optimizer);

//! Evaluates to true for a DOUBLE bound that is neither NULL nor NaN, and raises the bound error otherwise. Infinity is
//! a valid bound (no bound on that side). `column_name` names the source column the bound is exactly, or is empty.
unique_ptr<Expression> DirectValidBoundPredicate(Optimizer &optimizer, ColumnBinding bound_binding,
                                                 const string &column_name);

//! Which clause a number belongs to, so an error names the object the user wrote.
enum class DirectNumberKind : uint8_t { OBJECTIVE, CONSTRAINT };

//! Evaluates to true for a finite number and raises an error for a NULL or non-finite one. A NULL number's message
//! quotes `number_text`, or stays generic when that is empty. The NaN and infinity message names `kind`.
unique_ptr<Expression> DirectValidNumberPredicate(Optimizer &optimizer, const LogicalType &number_type,
                                                  ColumnBinding number_binding, const string &number_text,
                                                  DirectNumberKind kind);

//! A plan that releases no row before `valid` has been evaluated on every input row.
struct DirectBarrier {
	unique_ptr<LogicalOperator> plan;
	//! Where each input column lives in `plan`, in input order.
	vector<ColumnBinding> input_bindings;
	//! Carry this column to the rule's output and declare it as a validation slot, so pruning keeps the barrier.
	ColumnBinding barrier;
};

//! The solver reads every input row before it returns, so a bad value on the last row raises even under an outer
//! LIMIT 1. A rule whose plan could otherwise stream rows out keeps that obligation here: `valid`, over `input`'s
//! columns, returns true or raises the row's error; a projection evaluates it on every row and a window counting the
//! whole input cannot emit until all of them are read. The predicate is a projected column rather than a filter, so
//! filter pushdown cannot move it below a join onto rows the DECIDE input never sees.
DirectBarrier DirectValidationBarrier(Binder &binder, unique_ptr<LogicalOperator> input, unique_ptr<Expression> valid);

//===--------------------------------------------------------------------===//
// DECIDE semantics every aggregate rule shares
//===--------------------------------------------------------------------===//

//! Whether a clause's WHEN holds on a row: true without a WHEN, and false when the WHEN is false or NULL.
unique_ptr<Expression> DirectActiveWhen(const Expression *when);

//! DECIDE's empty-aggregate error: a scoped aggregate over a nonempty input with no eligible row has no value.
//! `clause` names where the aggregate sits in the message (`constraint` or `objective`). Which error is reported
//! when the query has several is not specified.
unique_ptr<LogicalOperator> DirectGuardEmptyAggregate(Optimizer &optimizer, unique_ptr<LogicalOperator> input,
                                                      ColumnBinding eligible, const string &clause = "constraint");

//! The error for a NULL or NaN source-valued bound: name the column when the bound is one, otherwise point at the
//! bound expression.
string DirectInvalidBoundMessage(const string &column_name);

//! The solver consumes every source output. Skipping an unreferenced output must not suppress a computed error
//! or volatile expression. This proves only stored columns, constants, and their passthrough aliases safe,
//! including passthrough bindings from one child of an inner comparison join.
bool DirectCanSkipSourceOutput(LogicalOperator &source, ColumnBinding binding);

} // namespace duckdb
