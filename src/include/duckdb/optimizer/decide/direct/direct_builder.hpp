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

//===--------------------------------------------------------------------===//
// DECIDE semantics every aggregate rule shares
//===--------------------------------------------------------------------===//

//! The per-row state of a scoped aggregate, projected once over the input: every input column, then whether the
//! row is eligible (its WHEN holds and no PER key is NULL), then any per-row columns the rule adds. With no scope
//! and no extra columns the input passes through unchanged.
struct DirectScopeState {
	unique_ptr<LogicalOperator> plan;
	//! Where each input column lives in `plan`, in input order.
	vector<ColumnBinding> input_bindings;
	//! The projection's table index, or INVALID_INDEX when the input passed through.
	idx_t state_index = DConstants::INVALID_INDEX;
	//! Set only for a scoped state.
	ColumnBinding eligible;
	//! The rule's extra columns, in the order given.
	vector<ColumnBinding> extra;
};

//! A WHEN false or NULL row does not join the clause, and neither does a row with a NULL PER key. `key_slots` index
//! the input's columns; `extra` are expressions over the input.
DirectScopeState DirectProjectScope(Binder &binder, unique_ptr<LogicalOperator> input, bool scoped,
                                    const Expression *when, const vector<idx_t> &key_slots,
                                    vector<unique_ptr<Expression>> extra);

//! DECIDE's empty-aggregate error: a scoped aggregate over a nonempty input with no eligible row has no value. It
//! raises before any value of the clause is validated.
unique_ptr<LogicalOperator> DirectGuardEmptyAggregate(Optimizer &optimizer, unique_ptr<LogicalOperator> input,
                                                      ColumnBinding eligible);

//! One clause's data-valued bound, reduced per group: an upper bound takes the group MIN, a lower bound the group
//! MAX, and an equality both, which must agree.
struct DirectBoundSpec {
	idx_t source_clause_id;
	ExpressionType comparison;
	//! The bound over the plan's columns, in its own SQL type.
	unique_ptr<Expression> value;
	//! Set when the bound is exactly one source column: its binding in the plan and its type.
	bool is_column = false;
	ColumnBinding column;
	LogicalType column_type;
	//! Reduce over every group row rather than only the eligible ones (a source-valued bound under an
	//! aggregate-local WHEN, as the solver reduces it).
	bool all_group_rows = false;
	//! The error a NULL or NaN value raises.
	string invalid_message;
};

//! One clause's data-valued expression that must not be NULL on any row.
struct DirectNotNullSpec {
	idx_t source_clause_id;
	unique_ptr<Expression> value;
	string message;
};

//! Where the validated group extrema live.
struct DirectBoundValidation {
	unique_ptr<LogicalOperator> plan;
	idx_t window_index = DConstants::INVALID_INDEX;
	//! Per bound, the window slot of its group MIN or MAX, or INVALID_INDEX when its comparison needs none.
	vector<idx_t> min_slots;
	vector<idx_t> max_slots;
};

//! Validates data-valued bounds the way the solver reads them: every bound is checked for NULL and NaN on every input
//! row, including rows the clause does not apply to; equality bounds must not vary within a group; and the first
//! failing clause raises in source-clause order. `eligible` is set for a scoped clause, and the extrema are
//! partitioned by it and by `keys`. Nothing is added when there is nothing to validate.
DirectBoundValidation DirectValidateBounds(Optimizer &optimizer, unique_ptr<LogicalOperator> input,
                                           vector<DirectBoundSpec> &bounds, vector<DirectNotNullSpec> &not_null,
                                           const ColumnBinding *eligible, const vector<ColumnBinding> &keys,
                                           const vector<LogicalType> &key_types);

//! The error for a NULL or NaN source-valued bound, worded like the solver path: name the column when the bound is
//! one, otherwise point at the bound expression. Only floating point values can be NaN.
string DirectInvalidBoundMessage(const string &column_name, const LogicalType &type);

//! The solver consumes every source output. Skipping an unreferenced output must not suppress a computed error
//! or volatile expression. This proves only stored columns, constants, and their passthrough aliases safe,
//! including passthrough bindings from one child of an inner comparison join.
bool DirectCanSkipSourceOutput(LogicalOperator &source, ColumnBinding binding);

} // namespace duckdb
