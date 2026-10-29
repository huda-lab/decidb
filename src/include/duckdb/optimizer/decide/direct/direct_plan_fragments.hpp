#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/optimizer/decide/direct/direct_bound_reader.hpp"
#include "duckdb/optimizer/decide/direct/direct_expression.hpp"
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

//! The error of an `=` bound that takes two values in one group, on a row of the group: its smallest (`low`) and
//! largest (`high`) value, and NULL where they agree. `text` is the bound as written. The bound-side rewrite and a rule
//! that reduces the bound itself both raise it.
ExprPtr DirectBoundDisagreement(const ExpressionBuilder &calc, const string &text, const ExprPtr &low,
                                const ExprPtr &high);

//===--------------------------------------------------------------------===//
// Scope projection and data-valued bounds
//===--------------------------------------------------------------------===//

//! A one-argument aggregate (MIN, MAX or SUM, by function name) of the value over the whole partition.
unique_ptr<BoundWindowExpression> DirectWindowExtremum(Optimizer &optimizer, const char *name,
                                                       unique_ptr<Expression> value);

//! The rows and groups the clauses of one rule's aggregate share (S1's count bounds, R1's budget): a rule reads them
//! as one scope or declines.
struct DirectSharedScope {
	//! The rows every clause covers (the clause's `when`, with the sum's own), or null for all.
	unique_ptr<Expression> when;
	//! The `by` key: the groups, each its own bound. Empty for one group.
	vector<DirectKeyElement> keys;
	//! The clauses are written `per K:` and a NULL in K puts a row in no class, so a query whose kept rows all have a
	//! NULL K has no instance (the plan owes that: DirectAddInstanceFlag). `classes` is K.
	bool gated = false;
	vector<DirectKeyElement> classes;

	bool Scoped() const {
		return when || !keys.empty() || gated;
	}
};

//! Proves that every clause covers the same rows in the same groups and reads the scope: the same plain SUM scope
//! (`Membership`), `by` key and `per` key, and a `when` and keys a plan can compute for each row (no decision, nothing
//! volatile, no subquery). False when they differ.
bool DirectProveSharedScope(const DirectIR &ir, const vector<const DirectIRConstraint *> &clauses,
                            DirectSharedScope &scope);

//! Appends to `input` a column that is true when some row `when` keeps has a class of `classes` (no NULL in a NO_GROUP
//! element), and returns its binding in `flag`. The rows are the same on every row. A clause `per K: C` has no
//! instance without one (D5: a NULL key is in no class); `when` and `classes` are over `input`'s columns, which keep
//! their bindings.
unique_ptr<LogicalOperator> DirectAddInstanceFlag(Optimizer &optimizer, unique_ptr<LogicalOperator> input,
                                                  const Expression *when, const vector<DirectKeyElement> &classes,
                                                  ColumnBinding &flag);

//! The per-row state of a scoped aggregate, projected once over the input: every input column, then whether the
//! row is eligible (its `when` holds and no NO_GROUP key element is NULL), then the group key, then any per-row columns
//! the rule adds. With no scope and no extra columns the input passes through unchanged.
struct DirectScopeState {
	unique_ptr<LogicalOperator> plan;
	//! Where each input column lives in `plan`, in input order.
	vector<ColumnBinding> input_bindings;
	//! The projection's table index, or INVALID_INDEX when the input passed through.
	idx_t state_index = DConstants::INVALID_INDEX;
	//! Set only for a scoped state.
	ColumnBinding eligible;
	//! The group key's elements as the state carries them (NULL on a row whose `when` drops it), in key order.
	vector<ColumnBinding> keys;
	vector<LogicalType> key_types;
	//! The rule's extra columns, in the order given.
	vector<ColumnBinding> extra;
};

//! A row whose `when` is false or NULL does not join the clause, and neither does a row with a NULL in a column of a
//! `by` key (D5: a NULL key is in no group; the columns of a relation compare NULL as a value, and group). `keys` are
//! the `by` key, its expressions over the input's columns; they are computed on the rows the `when` keeps alone, as
//! nothing is computed on a row it drops. `extra` are expressions over the input. A row is eligible only when
//! `instance` (a BOOLEAN over the input, or null for none) holds.
DirectScopeState DirectProjectScope(Binder &binder, unique_ptr<LogicalOperator> input, bool scoped,
                                    const Expression *when, const vector<DirectKeyElement> &keys,
                                    vector<unique_ptr<Expression>> extra, unique_ptr<Expression> instance = nullptr);

//! A per-row bound read from the data: it must not be NULL on a row its clause keeps, nor NaN when `reject_nan`.
struct DirectRowBound {
	unique_ptr<Expression> value;
	//! The rows the clause keeps, as a BOOLEAN over the plan's columns; null for every row.
	unique_ptr<Expression> kept;
	bool reject_nan = false;
	//! A coefficient of a decision: a NaN or an infinity is the solver's coefficient error, where a bound may be
	//! infinite.
	bool coefficient = false;
	//! The value as the user wrote it, which the error names.
	string written;
};

//! The message a row raises for a per-row bound that is NaN or, for a coefficient, infinite, on a row its clause keeps;
//! NULL when the bound is valid, or skips the instance it reaches.
ExprPtr DirectRowBoundMessage(const ExpressionBuilder &calc, const DirectRowBound &bound);

//! Raises the solver's error when a per-row bound is NaN or, for a coefficient, infinite, on a row its clause keeps;
//! nothing is read on a row a `when` drops, and a NULL passes (it skips the instance). Which failing row is reported is
//! not specified. Nothing is added when there is nothing to check.
unique_ptr<LogicalOperator> DirectCheckRowBounds(Optimizer &optimizer, unique_ptr<LogicalOperator> input,
                                                 const vector<DirectRowBound> &bounds);

} // namespace duckdb
