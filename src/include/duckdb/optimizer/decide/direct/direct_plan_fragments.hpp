#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/optimizer/decide/direct/direct_bound_reader.hpp"
#include "duckdb/planner/column_binding.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/logical_operator.hpp"

namespace duckdb {

class Binder;
class ClientContext;
class ExpressionBuilder;
class Optimizer;

//! Plan factory functions for direct-solve relational plans: free functions that each return one plan fragment (a
//! factory method in function form, not a builder). A rule composes these into its own construction; none of them
//! knows which problem class it is serving.

//! COUNT over the whole partition of rows where the predicate holds.
unique_ptr<BoundWindowExpression> DirectWindowMatchingCount(const ExpressionBuilder &calc,
                                                            unique_ptr<Expression> predicate);

//! A per-row numeric value read as the solver reads a coefficient: cast to DOUBLE, negated when `sign` is negative,
//! then multiplied or divided by the query-wide factor of its reducer (`scale`, when `scaled`).
unique_ptr<Expression> DirectScaledTerm(Optimizer &optimizer, unique_ptr<Expression> value, int sign, bool scaled,
                                        double scale, bool divides);

//! A copy of a value re-pointed at a projection that carries the source (see DirectRemapSourceReferences), for the
//! check above that projection to name the value's source columns.
unique_ptr<Expression> DirectOverProjection(const Expression &value, const vector<ColumnBinding> &source_bindings,
                                            idx_t projection_index);

//! The values summed into one coefficient as the solver sums like terms (`DecideMergeLikeCoefficients`), re-pointed at
//! a projection over the source. Null for no values.
unique_ptr<Expression> DirectMergedOverProjection(ClientContext &context, const vector<DirectSignedValue> &values,
                                                  const vector<ColumnBinding> &source_bindings, idx_t projection_index);

//! A plan that releases no row before `valid` has been evaluated on every input row.
struct DirectPipelineBreaker {
	unique_ptr<LogicalOperator> plan;
	//! Where each input column lives in `plan`, in input order.
	vector<ColumnBinding> input_bindings;
	//! Carry this column to the rule's output and declare it as a validated column, so pruning keeps the breaker.
	ColumnBinding breaker;
};

//! The solver reads every input row before it returns, so a bad value on the last row raises even under an outer
//! LIMIT 1. A rule whose plan would otherwise be pipelined keeps that obligation here: `valid`, over `input`'s
//! columns, returns true or raises the row's error; a projection evaluates it on every row and a window counting the
//! whole input cannot emit until all of them are read. The predicate is a projected column rather than a filter, so
//! filter pushdown cannot move it below a join onto rows the DECIDE input never sees.
DirectPipelineBreaker DirectValidationBreaker(Binder &binder, unique_ptr<LogicalOperator> input,
                                              unique_ptr<Expression> valid);

//===--------------------------------------------------------------------===//
// DECIDE semantics every aggregate rule shares
//===--------------------------------------------------------------------===//

//! Whether a clause's WHEN holds on a row: true without a WHEN, and false when the WHEN is false or NULL.
unique_ptr<Expression> DirectActiveWhen(const ExpressionBuilder &calc, const Expression *when);

//! DECIDE's empty-aggregate error: a scoped aggregate over a nonempty input with no eligible row has no value.
//! `clause` names where the aggregate sits in the message (`constraint` or `objective`). Which error is reported
//! when the query has several is not specified.
unique_ptr<LogicalOperator> DirectGuardEmptyAggregate(Optimizer &optimizer, unique_ptr<LogicalOperator> input,
                                                      ColumnBinding eligible, const string &clause = "constraint");

//===--------------------------------------------------------------------===//
// Scope projection and data-valued bounds
//===--------------------------------------------------------------------===//

//! A one-argument aggregate (MIN, MAX or SUM, by function name) of the value over the whole partition.
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
	//! The error a NULL or NaN value raises, for a bound that is not exactly one source column.
	string invalid_message;
	//! For a bound that is exactly one source column: the error a NULL raises, and the error a NaN raises (empty when
	//! the column cannot hold NaN). The column is the same on every row, so each message is fixed.
	string null_message;
	string nan_message;
};

//! One clause's data-valued expression that must not be NULL on any row, nor NaN when `reject_nan` is set.
struct DirectNotNullSpec {
	unique_ptr<Expression> value;
	//! The error a NULL raises, and a NaN too when `nan_message` is empty.
	string message;
	//! The error a NaN raises, when it differs from the NULL error.
	string nan_message;
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

} // namespace duckdb
