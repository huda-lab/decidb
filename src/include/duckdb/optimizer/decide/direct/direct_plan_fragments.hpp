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

} // namespace duckdb
