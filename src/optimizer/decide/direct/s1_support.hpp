#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/planner/column_binding.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/logical_operator.hpp"

namespace duckdb {

class Binder;
class Optimizer;

//! Plan-building helpers that only S1 uses. A helper moves to `direct_builder.hpp` when a second rule needs it
//! (`architecture/rules.md`).
namespace direct_s1 {

//! True when any condition is true (false for an empty list), as nested CASE expressions.
unique_ptr<Expression> DirectAnyCondition(const vector<unique_ptr<Expression>> &conditions);

//! MIN or MAX (by function name) of the value over the whole partition.
unique_ptr<BoundWindowExpression> DirectWindowExtremum(Optimizer &optimizer, const char *name,
                                                       unique_ptr<Expression> value);

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

//! One clause's data-valued bound, reduced per group: an upper bound takes the group MIN, a lower bound the group
//! MAX, and an equality both, which must agree.
struct DirectBoundSpec {
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

//! One clause's data-valued expression that must not be NULL on any row, nor NaN when `reject_nan` is set.
struct DirectNotNullSpec {
	unique_ptr<Expression> value;
	string message;
	bool reject_nan = false;
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
//! row, including rows the clause does not apply to; and equality bounds must not vary within a group. Which failing
//! check is reported is not specified. `eligible` is set for a scoped clause, and the extrema are partitioned by it
//! and by `keys`. Nothing is added when there is nothing to validate.
DirectBoundValidation DirectValidateBounds(Optimizer &optimizer, unique_ptr<LogicalOperator> input,
                                           vector<DirectBoundSpec> &bounds, vector<DirectNotNullSpec> &not_null,
                                           const ColumnBinding *eligible, const vector<ColumnBinding> &keys,
                                           const vector<LogicalType> &key_types);

} // namespace direct_s1
} // namespace duckdb
