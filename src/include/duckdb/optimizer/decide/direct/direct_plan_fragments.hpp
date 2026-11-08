#pragma once

#include <functional>

#include "duckdb/common/common.hpp"
#include "duckdb/optimizer/decide/direct/direct_bound_reader.hpp"
#include "duckdb/optimizer/decide/direct/direct_expression.hpp"
#include "duckdb/planner/column_binding.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"

namespace duckdb {

class Binder;
class ClientContext;
class LogicalAggregate;
class LogicalWindow;
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

//! The columns every stage of a rule forwards: the source columns, the eligible flag and the group key of its scope
//! (DirectScopeState).
struct DirectCarried {
	vector<ColumnBinding> source;
	//! Set only for a scoped rule.
	ColumnBinding eligible;
	vector<ColumnBinding> keys;
	//! The types of `keys`.
	vector<LogicalType> key_types;
};

//! One projection under construction: each column is added as an expression and gets its binding at once, and the
//! stage becomes a LogicalProjection over a child.
class DirectProjectionStage {
public:
	explicit DirectProjectionStage(idx_t index_p) : index(index_p) {
	}
	//! The projection's table index.
	const idx_t index;

	ColumnBinding Add(unique_ptr<Expression> expression);
	//! The same, for a value of the ExpressionBuilder; the stage keeps a copy.
	ColumnBinding Add(const ExprPtr &expression);
	//! Adds a column that reads each of `columns` of the child, whose types are `types`; returns where they are.
	vector<ColumnBinding> Forward(const vector<ColumnBinding> &columns, const vector<LogicalType> &types);
	//! The same for what a scoped rule carries, in whose scope (`scoped`) the eligible flag is set.
	DirectCarried Forward(const DirectCarried &from, const vector<LogicalType> &source_types, bool scoped);
	unique_ptr<LogicalOperator> Finish(unique_ptr<LogicalOperator> child);

private:
	vector<unique_ptr<Expression>> expressions;
};

//! One hash aggregate under construction: each group and each aggregate gets its binding in the output at once, the
//! groups first, then the aggregates, and the stage becomes a LogicalAggregate over a child.
class DirectAggregateStage {
public:
	explicit DirectAggregateStage(Binder &binder);

	//! Groups by `expression`, over the child's columns; returns the group's column.
	ColumnBinding Group(unique_ptr<Expression> expression);
	//! Adds an aggregate; returns its column.
	ColumnBinding Add(unique_ptr<Expression> aggregate);
	unique_ptr<LogicalAggregate> Finish(unique_ptr<LogicalOperator> child);

private:
	idx_t group_index;
	idx_t aggregate_index;
	vector<unique_ptr<Expression>> groups;
	vector<unique_ptr<Expression>> aggregates;
};

//! `sum(sign * value)` over proved values, each read as DOUBLE (DirectScaledTerm); null for none. `over` re-points a
//! value before it is read (null: the value as it is).
unique_ptr<Expression> DirectSumOf(Optimizer &optimizer, const vector<DirectSignedValue> &values,
                                   const std::function<unique_ptr<Expression>(const Expression &)> &over = nullptr);

//===--------------------------------------------------------------------===//
// A separable quadratic objective: `a*x^2 + b*x` per decision
//===--------------------------------------------------------------------===//

//! The columns of one DirectQuadraticPart in a rule's first projection: the rows the part reads, the sums of its
//! linear coefficients, and the weight `k`, slope `w` and constant `t` of each square `k*(w*x + t)^2`. Each is NULL on
//! a row the part does not read.
struct DirectQuadraticColumns {
	struct Square {
		idx_t decision = 0;
		//! The square's weight, the `k` of `k*(w*x + t)^2`: a constant, or a value of the row.
		ColumnBinding k;
		ColumnBinding w;
		ColumnBinding t;
	};
	bool has_mask = false;
	ColumnBinding mask;
	//! The decisions that have a linear coefficient in the part, and the sum of each one's.
	vector<idx_t> linear_decision;
	vector<ColumnBinding> linear;
	vector<Square> squares;
};

//! Lays out a part in the projection `p1`: its mask, its sums and its squares, each computed on the rows the part
//! reads. The check of each value that can fail (DirectCheckNumber) is appended to `checks`; a value NULL on a row the
//! part drops raises nothing there. `over` re-points a value of the source before it is read (null: as it is), and
//! `source_bindings` are the source columns `p1` carries first.
DirectQuadraticColumns DirectLayoutQuadraticPart(Optimizer &optimizer, const ExpressionBuilder &calc,
                                                 DirectProjectionStage &p1, const DirectQuadraticPart &part,
                                                 idx_t decision_count, const vector<ColumnBinding> &source_bindings,
                                                 const std::function<unique_ptr<Expression>(const Expression &)> &over,
                                                 vector<unique_ptr<Expression>> &checks);

//! The columns of each bound clause (DirectBoundClause) in the projection `p1`: its coefficient, right side and
//! admitted ends, each computed only on the rows the clause keeps (NULL elsewhere), and the row's `when`. The check of
//! the coefficient (a number, DirectCheckNumber) and of the right side (not NaN; a NULL one skips the row) is appended
//! to `checks`; both values are NULL on a row the clause drops, and a NULL passes. `over` re-points a value of the
//! source before it is read (null: as it is), and `source_bindings` are the source columns `p1` carries first.
vector<DirectBoundColumns>
DirectLayoutBoundClauses(Optimizer &optimizer, const ExpressionBuilder &calc, DirectProjectionStage &p1,
                         const vector<DirectBoundClause> &clauses, const vector<ColumnBinding> &source_bindings,
                         const std::function<unique_ptr<Expression>(const Expression &)> &over,
                         vector<unique_ptr<Expression>> &checks);

//! One decision's objective as `a*x^2 + b*x`, summed over the parts, each scaled by its sign and factor. A part that
//! has neither a linear term nor a square on the decision adds nothing. `linear` is the part of `b` the linear terms
//! make, which a rule tests for a number past a DOUBLE; it is null when no part has one. The rule applies the
//! objective's sense.
struct DirectQuadraticSum {
	ExprPtr a;
	ExprPtr b;
	ExprPtr linear;
};
DirectQuadraticSum DirectFoldQuadratic(Optimizer &optimizer, const ExpressionBuilder &calc,
                                       const vector<DirectQuadraticPart> &parts,
                                       const vector<DirectQuadraticColumns> &columns, idx_t decision);

//! The turning point of `a*x^2 + b*x` and whether computing it lost precision. Every rule that finds a square's
//! minimum, or weighs its ends, warns from this one expression: the point divides `b` by `a`, so a factor `a` or a
//! nonzero `b` below the normal range of a DOUBLE (DirectBelowNormalRange) has lost bits there. `target` is
//! `-b / (2*a)`, and for a whole-numbered decision (`whole`) the first integer whose next step no longer improves,
//! which keeps the smaller one on a tie. `loses_precision` is false where `a` is 0, since a row with no square divides
//! by nothing. `target` is read for `a` positive; the flag holds for either sign, since a concave square weighs its two
//! ends against each other by the same coefficients.
struct DirectSquareTarget {
	ExprPtr target;
	ExprPtr loses_precision;
};
DirectSquareTarget DirectTurningPoint(const ExpressionBuilder &calc, const ExprPtr &a, const ExprPtr &b, bool whole);

//! A copy of a value re-pointed at a projection that carries the source (see DirectRemapSourceReferences), for an
//! operator above that projection to read it.
unique_ptr<Expression> DirectOverProjection(const Expression &value, const vector<ColumnBinding> &source_bindings,
                                            idx_t projection_index);

//! A hash aggregate of `child` on `groups` (expressions over its columns); with none, one row however many rows
//! `child` has. Its columns are the groups, then the aggregates: (group_index, i) and (aggregate_index, j).
unique_ptr<LogicalAggregate> DirectAggregateOver(Optimizer &optimizer, unique_ptr<LogicalOperator> child,
                                                 const vector<ExprPtr> &groups,
                                                 vector<unique_ptr<Expression>> aggregates);

//! Joins each row of `rows` to the one row of `groups` that has its key. `row_keys` and `group_keys` are the key
//! expressions of the two sides, pairwise, compared with IS NOT DISTINCT FROM so a NULL key matches; with no key the
//! join is the cross product with the group's one row.
unique_ptr<LogicalOperator> DirectJoinGroupRow(unique_ptr<LogicalOperator> rows, unique_ptr<LogicalOperator> groups,
                                               vector<unique_ptr<Expression>> row_keys,
                                               vector<unique_ptr<Expression>> group_keys);

//! What DirectJoinClassAnswers gives back: the join, whose columns are the rows' columns, then the class plan's, and
//! where the answer columns of the class each row's key names are.
struct DirectJoinedAnswers {
	unique_ptr<LogicalOperator> plan;
	//! The class plan's columns after its key, in its order.
	vector<ColumnBinding> answers;
};

//! Group-then-join: a class's answer is computed once on the class's own row and joined back to every row of the
//! class, in place of a window that repeats the class's aggregates on every row (the shape DuckDB gives a decorrelated
//! scalar subquery). `classes` is a plan of one row per class: the key columns (`key_types`), then the answer columns;
//! a rule makes it as a hash aggregate of a first read of its source on the key (an ungrouped aggregate for a
//! query-wide decision, whose one row joins to every row), computes the answer and its checks on the aggregate's rows,
//! and passes it here with a second read as `rows`. `row_keys` are the key columns of `rows`, pairwise with the
//! class's, compared as DirectJoinGroupRow compares them (a NULL key is a class of its own).
//!
//! Precondition: a row's class is its whole key. A rule that drops rows from its classes (a `when`, a NULL in a key
//! element that puts a row in no class) makes whether the row is in one the first key column, on both sides, so that a
//! row no class takes never joins a class's answer: its other key columns are NULL, which a real class's can be too.
//!
//! No joined row exists before its class row has passed its checks: the aggregate consumes every row of the first read
//! before it gives its one row, and the class plan computes the answer and its checks above the aggregate, below the
//! join. So every check on a row of the first read, and on a class's row, has run before the first row of the second
//! read leaves, as the solver reads every row before it answers. A key with no row in `classes` joins NULL answers.
//! With a key the join is a SINGLE join, which has no inverse, so BuildProbeSideOptimizer keeps the classes on the
//! build side and the rows stream; the cross product of a query-wide decision may be flipped, which changes nothing.
DirectJoinedAnswers DirectJoinClassAnswers(Binder &binder, unique_ptr<LogicalOperator> rows,
                                           const vector<ColumnBinding> &row_keys, const vector<LogicalType> &key_types,
                                           unique_ptr<LogicalOperator> classes);

//! A verdict on every row of `rows` (DirectSourceReader::Gate): one row whose one BOOLEAN column is true when `valid`,
//! over `rows`' columns, is true on every row, and otherwise the error of a row that raises it. It is NULL for no rows,
//! which a gate passes none of and which have no row to raise on. The solver reads every input row before it returns,
//! so a bad value on the last row raises even under an outer LIMIT 1; a rule that streams its rows keeps that
//! obligation by gating them on this.
unique_ptr<LogicalOperator> DirectAllRowsValid(Optimizer &optimizer, unique_ptr<LogicalOperator> rows,
                                               unique_ptr<Expression> valid);

//===--------------------------------------------------------------------===//
// DECIDE semantics every aggregate rule shares
//===--------------------------------------------------------------------===//

//! Whether a clause's `when` holds on a row: true without one, and false when it is false or NULL.
unique_ptr<Expression> DirectActiveWhen(const ExpressionBuilder &calc, const Expression *when);

//! `value` on a row where `kept` is true, and NULL on any other; `value` itself when `kept` is null (every row is
//! kept). Nothing is computed on a row a `when` drops, so a value that raises (a logarithm of 0, an overflow) raises
//! only on a kept row, as on the solver path.
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

//! The hash counterpart of DirectWindowExtremum: the aggregate `name` over `argument` (null for COUNT(*)), on the rows
//! where `reads` holds (null: every row). A rule adds it to a DirectAggregateStage.
unique_ptr<Expression> DirectHashAggregate(Optimizer &optimizer, const ExpressionBuilder &calc, const char *name,
                                           const ExprPtr &argument, const ExprPtr &reads = nullptr);

//! The inclusive running sum of `value` within each partition, in order of `order` (`direction`, NULLs last), ties in
//! any order: DirectWindowExtremum's `sum` with a frame that ends at the current row, and the sort that gives the frame
//! its meaning. Rows of one partition and one order share a sort with any other window of the same partition and order.
unique_ptr<BoundWindowExpression> DirectRunningSum(Optimizer &optimizer, const ExpressionBuilder &calc,
                                                   const ExprPtr &value, const ExprPtr &order, OrderType direction,
                                                   const vector<ExprPtr> &partitions);

//===--------------------------------------------------------------------===//
// Group bounds (DecideGroupBound): a bound computed from the aggregates of its group
//===--------------------------------------------------------------------===//

//! The group bound of a raw `rhs`, which may differ between the rows of a group: the group's tightest value. `<=` reads
//! the MIN of `rhs`, `>=` its MAX, `=` both and owes the check that they agree. `invalid` is the message a row raises
//! for a NaN `rhs` (null for none), `text` the bound as written.
unique_ptr<DecideGroupBound> DirectTightestBound(Optimizer &optimizer, ExpressionType comparison,
                                                 unique_ptr<Expression> rhs, const string &text,
                                                 unique_ptr<Expression> invalid);

//! Whether a rule can compute the bound: its inputs and the filters of its aggregates are deterministic values of the
//! source row, and the limits it gives are numbers.
bool DirectGroupBoundIsReadable(const DirectIR &ir, const DecideGroupBound &bound);

//! What the rule projects before its group aggregate for one input of the bound: whether the row is one the aggregate
//! reads (the row is eligible and passes the input's own `when`), its argument on those rows, and the message it
//! raises. A member is null when the input has none.
struct DirectGroupInput {
	ExprPtr reads;
	ExprPtr argument;
	ExprPtr invalid;
};

//! Which members of an input are present, so a rule can lay out its columns before it has the expressions. `scoped`
//! says whether the rule has an eligible flag for the row.
struct DirectGroupInputLayout {
	bool reads;
	bool argument;
	bool invalid;
};
DirectGroupInputLayout DirectGroupLayout(const DecideGroupBound::Input &input, bool scoped);

//! The inputs of the bound as expressions over the rule's rows. `over` re-points an expression over the source at those
//! rows; `eligible` says whether the row is in a group (null for a rule with no scope).
vector<DirectGroupInput> DirectGroupInputs(const ExpressionBuilder &calc, const DecideGroupBound &bound,
                                           const ExprPtr &eligible,
                                           const std::function<unique_ptr<Expression>(const Expression &)> &over);

//! `function` (a BoundAggregateExpression of a group bound) as the aggregate itself, reading `argument` (null for
//! COUNT(*)) on the rows where `reads` holds (null for every row).
unique_ptr<Expression> DirectAggregateOf(const Expression &function, unique_ptr<Expression> argument,
                                         unique_ptr<Expression> reads);

//! The aggregate of a rule that groups its rows, which takes the bound's aggregates: a window over the group, or a hash
//! aggregate.
class DirectGroupSink {
public:
	virtual ~DirectGroupSink() = default;
	//! Adds `function` (a BoundAggregateExpression with no argument or filter) reading `argument` (null for COUNT(*))
	//! on the rows where `reads` holds (null for every row), and returns the column that holds its result per group.
	virtual ColumnBinding Add(const Expression &function, unique_ptr<Expression> argument,
	                          unique_ptr<Expression> reads) = 0;
};

//! The window form of a DirectGroupSink: each aggregate becomes a window over the whole partition, reading the
//! argument on the rows where `reads` holds, added to `window`, and its column is the window's. `partition` gives each
//! window the rule's group (the partition and nothing else, so every row of the group reads the same value).
class DirectWindowSink final : public DirectGroupSink {
public:
	DirectWindowSink(LogicalWindow &window_p, std::function<void(BoundWindowExpression &)> partition_p)
	    : window(window_p), partition(std::move(partition_p)) {
	}
	ColumnBinding Add(const Expression &function, unique_ptr<Expression> argument,
	                  unique_ptr<Expression> reads) override;

private:
	LogicalWindow &window;
	std::function<void(BoundWindowExpression &)> partition;
};

//! The hash form of a DirectGroupSink: each aggregate is added to `aggregate`, and its column is the aggregate's, one
//! per group. A rule that groups its rows with a hash aggregate, and computes the group's values on its rows, takes
//! the bound's aggregates through it.
class DirectAggregateSink final : public DirectGroupSink {
public:
	explicit DirectAggregateSink(DirectAggregateStage &aggregate_p) : aggregate(aggregate_p) {
	}
	ColumnBinding Add(const Expression &function, unique_ptr<Expression> argument,
	                  unique_ptr<Expression> reads) override;

private:
	DirectAggregateStage &aggregate;
};

//! What the rule reads of the bound once its group is computed, over the results: the limit as an upper and as a lower
//! one (null for a side the bound does not give), and the messages that group raises, in the order the solver checks
//! them (a NaN limit, then an `=` bound that takes two values). Each is NULL outside a group (`eligible`).
struct DirectGroupValues {
	ExprPtr upper;
	ExprPtr lower;
	vector<ExprPtr> messages;
};

//! Adds the bound's aggregates to the rule's group aggregate. `inputs` says where the rule projected each input's reads
//! and argument (a binding is unset for a member the input has not); `eligible` is over the group aggregate's output.
struct DirectGroupInputColumns {
	ColumnBinding reads;
	ColumnBinding argument;
};
DirectGroupValues DirectAddGroupBound(const ExpressionBuilder &calc, const DecideGroupBound &bound,
                                      const vector<DirectGroupInputColumns> &inputs, DirectGroupSink &sink,
                                      const ExprPtr &eligible);

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

//! What a rule carries through its stages from the scope: the first `source_width` columns of the state are the
//! source's.
DirectCarried DirectCarriedOf(const DirectScopeState &state, idx_t source_width);

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

} // namespace duckdb
