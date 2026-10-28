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

//! A copy of a value re-pointed at a projection that carries the source (see DirectRemapSourceReferences), for an
//! operator above that projection to read it.
unique_ptr<Expression> DirectOverProjection(const Expression &value, const vector<ColumnBinding> &source_bindings,
                                            idx_t projection_index);

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

//! Whether a clause's `when` holds on a row: true without one, and false when it is false or NULL.
unique_ptr<Expression> DirectActiveWhen(const ExpressionBuilder &calc, const Expression *when);

//! `value` on a row where `kept` is true, and NULL on any other. Nothing is computed on a row a `when` drops, so a
//! value that raises (a logarithm of 0, an overflow) raises only on a kept row, as on the solver path.
ExprPtr DirectOnKeptRows(const ExpressionBuilder &calc, const ExprPtr &kept, const ExprPtr &value);

//===--------------------------------------------------------------------===//
// Scope projection and data-valued bounds
//===--------------------------------------------------------------------===//

//! A one-argument aggregate (MIN, MAX or SUM, by function name) of the value over the whole partition.
unique_ptr<BoundWindowExpression> DirectWindowExtremum(Optimizer &optimizer, const char *name,
                                                       unique_ptr<Expression> value);

//! The per-row state of a scoped aggregate, projected once over the input: every input column, then whether the
//! row is eligible (its `when` holds and no group key is NULL), then any per-row columns the rule adds. With no scope
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

//! A row whose `when` is false or NULL does not join the clause, and neither does a row with a NULL group key (the
//! removed PER; a `by` key makes NULL a group of its own, D5, when direct solve reads one). `key_slots` index
//! the input's columns; `extra` are expressions over the input.
DirectScopeState DirectProjectScope(Binder &binder, unique_ptr<LogicalOperator> input, bool scoped,
                                    const Expression *when, const vector<idx_t> &key_slots,
                                    vector<unique_ptr<Expression>> extra);

//! A per-row bound read from the data: it must not be NULL on a row its clause keeps, nor NaN when `reject_nan`.
struct DirectRowBound {
	unique_ptr<Expression> value;
	//! The rows the clause keeps, as a BOOLEAN over the plan's columns; null for every row.
	unique_ptr<Expression> kept;
	bool reject_nan = false;
	//! A coefficient of a decision: a NaN or an infinity is the solver's coefficient error, where a bound may be
	//! infinite.
	bool coefficient = false;
};

//! Raises the solver's error when a per-row bound is NaN or, for a coefficient, infinite, on a row its clause keeps;
//! nothing is read on a row a `when` drops, and a NULL passes (it skips the instance). Which failing row is reported is
//! not specified. Nothing is added when there is nothing to check.
unique_ptr<LogicalOperator> DirectCheckRowBounds(Optimizer &optimizer, unique_ptr<LogicalOperator> input,
                                                 const vector<DirectRowBound> &bounds);

} // namespace duckdb
