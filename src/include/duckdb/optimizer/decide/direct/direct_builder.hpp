#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/planner/bound_result_modifier.hpp"
#include "duckdb/planner/column_binding.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"

namespace duckdb {

class LogicalOperator;
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

//! Evaluates to true for a finite score and raises an error for a NULL or non-finite one.
unique_ptr<Expression> DirectValidScorePredicate(Optimizer &optimizer, const LogicalType &score_type,
                                                 ColumnBinding score_binding);

//! The solver consumes every source output. Skipping an unreferenced output must not suppress a computed error
//! or volatile expression. This proves only stored columns, constants, and their passthrough aliases safe,
//! including passthrough bindings from one child of an inner comparison join.
bool DirectCanSkipSourceOutput(LogicalOperator &source, ColumnBinding binding);

} // namespace duckdb
