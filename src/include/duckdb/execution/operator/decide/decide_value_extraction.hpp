//===----------------------------------------------------------------------===//
//                         DeciDB
//
// duckdb/execution/operator/decide/decide_value_extraction.hpp
//
// How stage 08 computes a value over the input's rows, refuses one the model cannot read, and counts once what stands
// outside every reducer: the leaf functions every evaluator of a DECIDE query shares, so each value and each message
// has one copy (language/guide.md §3.2, §7.2, §7.3).
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/decide_value_messages.hpp"
#include "duckdb/decidb/formulation/solver_input.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/decide/decide_prepared_model.hpp"

#include <functional>
#include <map>

namespace duckdb {

class ClientContext;
struct DecideSourceNames;

//! Maps a node of an expression into another chunk layout, or returns null to leave it to ToChunkReferences.
using DecideChunkLeaf = std::function<unique_ptr<Expression>(const Expression &expr)>;

//! Transform a coefficient/value expression for ExpressionExecutor.
//! Handles: BOUND_COLUMN_REF, BOUND_FUNCTION, BOUND_CAST, BOUND_COMPARISON,
//! BOUND_CONJUNCTION, BOUND_OPERATOR. Falls back to Copy() for others.
//! With `leaf`, every node is offered to it first, and the children of a node copied whole are transformed too: the
//! tree's programs (CompileDecideTree) are rebuilt by these rules over their node's layout.
unique_ptr<Expression> ToChunkReferences(const Expression &expr, ClientContext &context,
                                         const DecideChunkLeaf &leaf = nullptr);

//! `expr` constant-folded once by DuckDB's own rule, as the expression rewriter folds a query's own expressions.
unique_ptr<Expression> DecideFoldConstants(unique_ptr<Expression> expr, ClientContext &context);

//! `expr` as stage 08 evaluates it: over the chunk's columns, and constant-folded once by DuckDB's own rule, as the
//! expression rewriter folds a query's own expressions. Every tree stage 08 evaluates passes through here, the ones
//! the DECIDE optimizer builds among them, so no constant subtree (`CAST(CAST(45 AS DECIMAL(18,2)) AS DOUBLE)`,
//! `45 >= 1e20`) is evaluated on every row. A message names the prepared tree, not this one, so it reads as written.
unique_ptr<Expression> TransformToChunkExpression(const Expression &expr, ClientContext &context);

//! Runs each expression of `executor` over `chunk`, whose first row is row `row_offset` of
//! the data, into `results`: expression i only on the rows `*reads[i]` marks (every row
//! when null), NULL on the others. A scoped clause filters before it generates
//! (language/guide.md §6.1), so nothing is computed on a row its `when` drops:
//! `when w > 0: x <= ln(w)` takes no logarithm of 0.
void ExecuteOnReadRows(ExpressionExecutor &executor, DataChunk &chunk, idx_t row_offset,
                       const vector<const vector<bool> *> &reads, DataChunk &results);

//! The same, with one set of rows for every expression; without one, every row.
void ExecuteOnReadRows(ExpressionExecutor &executor, DataChunk &chunk, idx_t row_offset, const vector<bool> *rows,
                       DataChunk &results);

//! Whether an error stops the query wherever it is raised, rather than belonging to the row it is raised on.
bool DecideErrorStopsTheQuery(const ErrorData &error);

//! Expression `expr_idx` of `*executor` (`lowered`) on `chunk`, whose first row is row `offset` of what it is evaluated
//! over, into `values` from `offset` on. When a row raises, each row is evaluated alone, a failing one keeping its
//! error in `errors` and NULL; the executor, whose states the failure may have left half written, is made again by
//! `remake`. So an error waits for a read that reaches its row (language/guide.md §6.3), and the one pass's rows
//! (DecideRowStore) and a tree node's entries (DecideTreeColumns) are evaluated alike.
void DecideEvaluateWindow(ClientContext &context, unique_ptr<ExpressionExecutor> &executor, idx_t expr_idx,
                          const std::function<unique_ptr<ExpressionExecutor>()> &remake, const Expression &lowered,
                          Vector &values, std::map<idx_t, ErrorData> &errors, DataChunk &chunk, idx_t offset);

//! What a coefficient DeciDB computed is computed from (DecideTerm::inputs), and how a message reads one of them on the
//! row the coefficient fails on: `value_at(input, i)` is its value at row i of the window the coefficient was computed
//! over.
struct DecideValueInputs {
	const vector<unique_ptr<Expression>> &values;
	std::function<Value(const Expression &input, idx_t i)> value_at;
};

//! A number read from the source, and what the message that refuses it calls it: `value` is the expression the number
//! comes from, printed as the user wrote it (DecideInvalidValueMessage). `clause` is the clause (or the objective) it
//! belongs to, which a coefficient past a DOUBLE that no column's value made so names (TOO_LARGE), as AddCoefficient
//! does. `inputs` are what a coefficient DeciDB computed is computed from; none for one as written.
struct ValueSource {
	DecideValueRole role;
	const Expression &value;
	const DecideSourceNames &names;
	string clause = string();
	const DecideValueInputs *inputs = nullptr;
};

//! Refuses a number the model cannot read (NaN, or infinite where a bound is not allowed to be), naming `source`. A
//! coefficient names the first value it is computed from that a column holds and that is not finite on `row`, the row
//! it fails on; with none, DeciDB's own arithmetic (or a literal) left the DOUBLE range, and it names its clause. A
//! coefficient as written is the one value it is computed from.
[[noreturn]] void ThrowInvalidValue(const ValueSource &source, idx_t row = 0);

//! Vectorized extraction of a chunk-result column into a vector<double>, multiplied by `sign`.
//! Throws InvalidInputException on a number the model cannot read, naming `source`. A NULL reads
//! 0 and marks its row in `null_rows`: the caller skips what it reaches (deck p. 50). With
//! no `null_rows` the value cannot be NULL (a reducer's `when` tests its data,
//! DecideCanonicalizer::SkipNullData).
//! Fast path for DOUBLE; otherwise casts via VectorOperations::DefaultCast.
//!
//! A BOUND relaxes the check to NaN-only, and is used exclusively for values
//! on the path to a model row's `rhs` — the row's own bound, and the reducer input
//! that a per-group bound is folded from. An infinite bound is not an error there:
//! it is the absence of a constraint (`<= +inf`) or a constraint nothing satisfies
//! (`>= +inf`), both of which the solver contract already expresses — the model
//! validator accepts a non-finite rhs and rejects only NaN, and the constant and
//! singleton-row-bound RHS paths have always passed infinities straight through. A
//! coefficient has no such reading, so every other caller stays strict.
void ExtractDoubleColumn(Vector &result_vec, idx_t count, double sign, vector<double> &out, const ValueSource &source,
                         vector<bool> *null_rows = nullptr, const vector<bool> *read_rows = nullptr,
                         idx_t row_offset = 0);

//! A decision outside the reducers (`MAXIMIZE when c: sum(x) + w * s`, `sum(x) by (g) + s <= 8`) is counted once
//! (language/guide.md §4): once in the objective, once in each group's row of a clause in the aggregate shape. Its
//! coefficient stays on one row -- the first kept row, the group's first row -- and every other row is zeroed; the
//! scope checks made sure that row stands for all of them (one instance, one value of its data). This is the one
//! place the rule is applied: the model sums every row of every term (ilp_model_builder.cpp).
//! FirstKeptRow is the first row `kept` marks (every row without it), INVALID_INDEX with none.
idx_t FirstKeptRow(const vector<bool> *kept, idx_t num_rows);

//! Keeps the coefficient a term has on each row of `counted`, and zeroes every other row.
void CountOnce(CoefficientColumn &coefficients, const vector<idx_t> &counted);

//! Whether a term stands outside every reducer, which counts once.
bool CountsOnce(const DecideTerm &term);

//! The objective's terms outside its reducers, counted on its first kept row.
void CountTupleTermsOnce(const vector<DecideTerm> &terms, vector<CoefficientColumn> &coefficients,
                         const vector<bool> *kept, idx_t num_rows);

//! The first row of each group of a clause in the aggregate shape: every row for `row_group_ids` empty (one group),
//! none for a group with no row.
vector<idx_t> FirstRowOfEachGroup(const vector<idx_t> &row_group_ids, idx_t num_groups, idx_t num_rows);

//! The instances `row_group_ids` names: in the aggregate shape each group a row stands in, else each row in a group.
//! Empty is every row in one group.
idx_t CountInstances(const vector<idx_t> &row_group_ids, idx_t num_groups, idx_t num_rows, bool grouped);

//! A NULL that reaches a comparison unreduced makes that instance NULL, and a NULL instance
//! is skipped (deck p. 50): in the aggregate shape the group the row is in (its bound and its
//! terms beside the reducers are one value per group), else the row. `row_group_ids` empty
//! is every row in one group.
void SkipNullInstances(const vector<bool> &null_rows, bool grouped, idx_t num_rows, vector<idx_t> &row_group_ids,
                       idx_t &num_groups);

//! A clause's evaluation before its rows are read (EvaluatedConstraint): its facts, its repair group, and each term's
//! decision as a row reads it (`read_variable`, DecideGlobalSinkState::ReadVariable) and coefficient as written, which
//! a diagnosis renders a data-weighted clause with.
EvaluatedConstraint InitializeEvaluatedConstraint(const DecideConstraint &constraint, idx_t repair_group_id,
                                                  const std::function<idx_t(const DecideTerm &)> &read_variable);

//! How a message and a repair name `constraint`'s bound (EvaluatedConstraint::rhs_written, rhs_label), whether one
//! editable value stands for it on every row, and a constant's value there. Returns whether it is read on each row
//! instead.
bool DescribeBound(const DecideConstraint &constraint, idx_t num_rows, const DecideSourceNames &names,
                   const vector<string> &source_fragments, const vector<KeyedScopeInfo> &keyed_scopes,
                   const vector<DecideKey> &keys, EvaluatedConstraint &evaluated);

} // namespace duckdb
