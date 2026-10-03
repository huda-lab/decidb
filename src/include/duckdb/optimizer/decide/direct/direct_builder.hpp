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

//! True when any condition is true (false for an empty list), as nested CASE expressions.
unique_ptr<Expression> DirectAnyCondition(const vector<unique_ptr<Expression>> &conditions);

//! COUNT over the whole partition of rows where the predicate holds.
unique_ptr<BoundWindowExpression> DirectWindowMatchingCount(unique_ptr<Expression> predicate);

//! MIN or MAX (by function name) of the value over the whole partition.
unique_ptr<BoundWindowExpression> DirectWindowExtremum(Optimizer &optimizer, const char *name,
                                                       unique_ptr<Expression> value);

//! A BOOLEAN expression that raises the message as an error when evaluated.
unique_ptr<Expression> DirectErrorPredicate(Optimizer &optimizer, const string &message);

//! Evaluates to true for a finite score and raises an error for a NULL or non-finite one, worded as the solver
//! words it. `null_column` names the source column the score is exactly; leave it empty for a computed score.
unique_ptr<Expression> DirectValidScorePredicate(Optimizer &optimizer, const LogicalType &score_type,
                                                 ColumnBinding score_binding, const string &null_column);

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

//! The solver consumes every source output. Skipping an unreferenced output must not suppress a computed error
//! or volatile expression. This proves only stored columns, constants, and their passthrough aliases safe,
//! including passthrough bindings from one child of an inner comparison join.
bool DirectCanSkipSourceOutput(LogicalOperator &source, ColumnBinding binding);

} // namespace duckdb
