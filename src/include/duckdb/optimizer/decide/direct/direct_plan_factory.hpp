#pragma once

#include <limits>

#include "duckdb/common/common.hpp"
#include "duckdb/optimizer/decide/direct/direct_expression.hpp"
#include "duckdb/optimizer/decide/direct/direct_rule.hpp"
#include "duckdb/planner/bound_result_modifier.hpp"
#include "duckdb/planner/column_binding.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/logical_operator.hpp"

namespace duckdb {

class Binder;
class Optimizer;

//! Plan factory functions for direct-solve relational plans: free functions that each return one plan fragment (a
//! factory method in function form, not a builder). A rule composes these into its own construction; none of them
//! knows which problem class it is serving.

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
//! a valid bound (no bound on that side). `source_value` is the bound as the user wrote it, over the columns the check
//! reads, so a NULL names the column it came from.
unique_ptr<Expression> DirectValidBoundPredicate(Optimizer &optimizer, ColumnBinding bound_binding,
                                                 const Expression &source_value);

//! The errors a source column used as a bound raises, for a plan that counts the bad rows and raises one fixed message
//! (the column is the same on every row): NULL, and NaN. Both are the solver's text, built by the function the solver
//! uses; the NaN message lacks the solver's row position.
string DirectBoundNullMessage(const string &column_name);
string DirectBoundNaNMessage();

//! A copy of a value re-pointed at a projection that carries the source (see DirectRemapSourceReferences), for the
//! check above that projection to name the value's source columns.
unique_ptr<Expression> DirectOverProjection(const Expression &value, const vector<ColumnBinding> &source_bindings,
                                            idx_t projection_index);

//! Which clause a number belongs to, so an error names the object the user wrote.
enum class DirectNumberKind : uint8_t { OBJECTIVE, CONSTRAINT };

//! Evaluates to true for a finite number and raises an error for a NULL or non-finite one. A NULL number's message is
//! the solver's: it names the columns that are NULL in `source`, the expression the number is made of as the solver
//! evaluates it, over the columns the check reads, and quotes `source` when no column is NULL. A null `source` makes
//! the message generic. The NaN and infinity message names `kind`.
unique_ptr<Expression> DirectValidNumberPredicate(Optimizer &optimizer, const LogicalType &number_type,
                                                  ColumnBinding number_binding, const Expression *source,
                                                  DirectNumberKind kind);

//! A plan that releases no row before `valid` has been evaluated on every input row.
struct DirectPipelineBreaker {
	unique_ptr<LogicalOperator> plan;
	//! Where each input column lives in `plan`, in input order.
	vector<ColumnBinding> input_bindings;
	//! Carry this column to the rule's output and declare it as a validated column, so pruning keeps the breaker.
	ColumnBinding breaker;
};

//! The solver reads every input row before it returns, so a bad value on the last row raises even under an outer
//! LIMIT 1. A rule whose plan could otherwise stream rows out keeps that obligation here: `valid`, over `input`'s
//! columns, returns true or raises the row's error; a projection evaluates it on every row and a window counting the
//! whole input cannot emit until all of them are read. The predicate is a projected column rather than a filter, so
//! filter pushdown cannot move it below a join onto rows the DECIDE input never sees.
DirectPipelineBreaker DirectValidationBreaker(Binder &binder, unique_ptr<LogicalOperator> input,
                                              unique_ptr<Expression> valid);

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

//! The error for a NULL or NaN bound that is not exactly one source column. Which column is NULL depends on the row,
//! and a plan that counts bad rows has no single row to name, so it points at the bound expression.
string DirectInvalidBoundMessage();

//! The solver consumes every source output. Skipping an unreferenced output must not suppress a computed error
//! or volatile expression. This proves only stored columns, constants, and their passthrough aliases safe,
//! including passthrough bindings from one child of an inner comparison join.
bool DirectCanSkipSourceOutput(LogicalOperator &source, ColumnBinding binding);

//===--------------------------------------------------------------------===//
// Per-row bounds on one decision
//===--------------------------------------------------------------------===//
//
// A bound `coefficient * x  comparison  rhs` on one decision reads, on each row, as a lower and an upper bound on x.
// The reader has two halves that a rule composes. At plan time, DirectReadBounds proves the bounds and separates what
// the solver folds into the decision's static box from the clauses the plan evaluates per row. In the plan,
// DirectClauseBounds turns one clause's raw values into its two bounds and DirectRowBox intersects them with the box.

//! A numeric expression over the source, read as DOUBLE and multiplied by `sign`.
struct DirectSignedValue {
	int sign = 1;
	unique_ptr<Expression> expr;
};

//! What a NULL value quotes when none of its columns is NULL: the source column when the value is exactly one,
//! otherwise the expression.
string DirectTextOf(const Expression &expr);

//! The values summed into one coefficient as the solver sums like terms (`DecideMergeLikeCoefficients`), re-pointed at
//! a projection over the source. Null for no values.
unique_ptr<Expression> DirectMergedOverProjection(ClientContext &context, const vector<DirectSignedValue> &values,
                                                  const vector<ColumnBinding> &source_bindings, idx_t projection_index);

//! True for an INT or BOOL decision, whose bounds round to whole numbers.
bool DirectIsWholeNumbered(DirectDomain domain);

//! A coefficient or value the plan evaluates per row: numeric, deterministic, independent of the decisions, over the
//! source. It may raise when evaluated; the plan evaluates it on every row, as the solver does.
bool DirectProveValue(const DirectIR &ir, const Expression &expr);

//! One bound the plan evaluates per row: `coefficient * x  comparison  rhs`, under an optional WHEN. A bound the solver
//! folds into the variable's box is not a clause, it is part of the decision's static box.
struct DirectBoundClause {
	idx_t decision = 0;
	ExpressionType comparison = ExpressionType::INVALID;
	vector<DirectSignedValue> coefficient;
	unique_ptr<Expression> rhs;
	unique_ptr<Expression> when;

	//! A deep copy of every field, so a field added later cannot be missing from a clause split in two.
	DirectBoundClause Copy() const;
};

//! A decision's box before any per-row bound: its default, replaced or tightened by the bounds the solver folds into
//! the variable. An end of magnitude 1e20 or more is infinite.
struct DirectDecisionBox {
	double lower = 0.0;
	double upper = std::numeric_limits<double>::infinity();
};

struct DirectBoundReading {
	//! The per-row clauses, in the order the constraints came. An equality is split into its lower and its upper half.
	vector<DirectBoundClause> clauses;
	//! One static box per decision.
	vector<DirectDecisionBox> boxes;
};

//! Reads every non-aggregate constraint of `ir` as a bound on one decision (a constraint of the form `a*x
//! <comparison> value`; the rule's CheckCondition has already checked the shape). A bound the solver folds into the
//! variable
//! (`variable_bound`: an unconditional comparison of the bare decision against a constant) replaces the default lower
//! bound of 0 and tightens the upper one; every other bound is a clause. Returns false with a `reason` when a bound
//! cannot be proved, or when the folded box contradicts the decision's built-in domain, which the solver reports.
bool DirectReadBounds(const DirectIR &ir, ClientContext &context, DirectBoundReading &reading, string &reason);

//! The columns of one clause's raw values, evaluated on every row.
struct DirectBoundColumns {
	ColumnBinding coefficient;
	ColumnBinding rhs;
	bool has_when = false;
	ColumnBinding when;
};

//! The lower and upper bound one clause puts on its decision on one row, as the solver reads it: a clause holds
//! `coefficient * x  comparison  rhs`, so the coefficient's sign picks the side, a zero coefficient leaves the row
//! alone or infeasible, and a whole-numbered decision rounds to the integer DOUBLE arithmetic accepts.
void DirectClauseBounds(const ExpressionBuilder &calc, ExpressionType comparison, bool whole,
                        const DirectBoundColumns &columns, ExprPtr &lower, ExprPtr &upper);

//! A decision's box on one row: the static box intersected with the bounds of its clauses.
void DirectRowBox(const ExpressionBuilder &calc, double static_lower, double static_upper,
                  const vector<ColumnBinding> &clause_lowers, const vector<ColumnBinding> &clause_uppers,
                  ExprPtr &lower, ExprPtr &upper);

} // namespace duckdb
