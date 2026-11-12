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
#include "duckdb/decidb/formulation/ilp_linearization.hpp"
#include "duckdb/decidb/formulation/solver_input.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/decide/decide_tree_terms.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"

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
//! (DecideRowStore) and a tree node's entries (DecideTreeColumns) are evaluated alike. `raised`, when given, keeps the
//! error the whole window's evaluation raised: the one DuckDB's vectorized evaluation of the window meets first.
void DecideEvaluateWindow(ClientContext &context, unique_ptr<ExpressionExecutor> &executor, idx_t expr_idx,
                          const std::function<unique_ptr<ExpressionExecutor>()> &remake, const Expression &lowered,
                          Vector &values, std::map<idx_t, ErrorData> &errors, DataChunk &chunk, idx_t offset,
                          ErrorData *raised = nullptr);

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

//! The objective's products outside its reducers (`MAXIMIZE sum(x) + s * u`), counted on its first kept row as a
//! linear term there is; one inside a reducer sums over its rows.
void CountProductsOnce(const vector<DecideObjective::BilinearTerm> &terms,
                       vector<EvaluatedConstraint::BilinearTerm> &products, const vector<bool> *kept, idx_t num_rows);

//! The first row of each group of a clause in the aggregate shape: every row for `row_group_ids` empty (one group),
//! none for a group with no row.
vector<idx_t> FirstRowOfEachGroup(const vector<idx_t> &row_group_ids, idx_t num_groups, idx_t num_rows);

//! A clause's terms outside its reducers in the aggregate shape, counted on the first row of each of its groups.
void CountTupleTermsOncePerGroup(const vector<DecideTerm> &terms, idx_t num_rows, EvaluatedConstraint &evaluated);

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

//! Per-term filter state for a reducer's own `when` (constraint terms, bilinear terms, quadratic groups, and their
//! objective-side equivalents all share this shape).
struct TermFilterState {
	vector<bool> mask;
	bool has_filter = false;
	bool avg_scale = false;
	//! An objective AVG's `by` groups among the rows it reads (DecideTerm::avg_group_key), which its 1/n divides by;
	//! empty for one group.
	vector<idx_t> avg_group_ids;
	idx_t avg_num_groups = 0;
};

//! A key's groups over the rows a clause reads it on, numbered by the key's values: each row's group, INVALID_INDEX
//! for a row in none (flat's PerGroupCacheEntry; the tree's G slots by rank).
struct DecideKeyGroups {
	const vector<idx_t> &row_group_ids;
	idx_t num_groups;
};

//! The rows `read_rows` holds (every row when null) and `row_filter` keeps, numbered by their group of `groups`, the
//! groups renumbered densely in key-value order; INVALID_INDEX on every other row. Returns the kept groups, as
//! `groups` numbers them. The optional second map is over the same numbering but the looser `loose_row_filter`: a
//! group no strict row reached emits no constraint, so its rows stay out.
vector<idx_t> DecideKeptGroups(const DecideKeyGroups &groups, idx_t num_rows,
                               const std::function<bool(idx_t)> &row_filter, const vector<bool> *read_rows,
                               vector<idx_t> &out_row_group_ids, idx_t &out_num_groups,
                               const std::function<bool(idx_t)> *loose_row_filter = nullptr,
                               vector<idx_t> *out_loose_row_group_ids = nullptr);

//! The rows of a clause in the grouped shape (the aggregate shape, an easy MIN/MAX; language/guide.md §6.4): its kept
//! rows `scoped_kept` grouped by its `by` key's `groups` (null with no key), a group emitting the rows a reducer reads
//! (`local_row_active`, with `has_local_filters`), or, under `<>`, every kept row. A `per K:` that names classes holds
//! once per class, so with no kept row in a class (`any_class`) there is none. Writes `evaluated`'s row groups,
//! `kept_row_group_ids` (when given: every kept row of an emitted group) and the instances the clause holds before any
//! with no value is skipped (D90). Returns the groups of `groups` it keeps, which its names name.
vector<idx_t> GroupRowsByShape(const DecideConstraint &constraint, idx_t num_rows, const vector<bool> &scoped_kept,
                               bool has_local_filters, const vector<bool> &local_row_active,
                               const DecideKeyGroups *groups, const vector<bool> &in_a_class,
                               vector<idx_t> *kept_row_group_ids, EvaluatedConstraint &evaluated,
                               idx_t &grouped_instances);

//! A reducer over no rows is NULL, and a NULL instance is skipped (deck p. 56): in the aggregate shape, a group in
//! which some reducer reads no row has no instance. Only a reducer's term carries a filter (its own `when`, its
//! NULL-data and absent-decision tests, its de-duplication), so a term with none reads every row of its group.
//! `row_group_ids` empty is every row in one group.
void SkipGroupsWithAnEmptyReducer(const vector<const TermFilterState *> &filters, idx_t num_rows,
                                  vector<idx_t> &row_group_ids, idx_t &num_groups);

//! A composed MIN/MAX term's evaluation before its rows are read (ComposedMinMaxTermData): what it reduces, its sign,
//! whether its direction is easy, and a MIN's or a MAX's name as the plan prints the reducer, so a diagnosis over its
//! extremum column reads `max(ship)`.
ComposedMinMaxTermData DecideComposedTerm(const LogicalDecide::ComposedMinMaxTerm &term,
                                          const vector<string> &source_fragments,
                                          const vector<KeyedScopeInfo> &keyed_scopes, const vector<DecideKey> &keys);

//! A composed term's factor (LogicalDecide::ComposedMinMaxTerm::scale), one value for the whole query by construction
//! (the canonicalizer refuses one that varies by row), as read on a row: a NULL one makes its clause's instance NULL,
//! which sets `is_null` and reads 0; one the model cannot read is refused, named by `source`; a divisor of 0 is a
//! division by zero in `what`, the clause ("constraint") or the objective.
double DecideQueryWideScale(const Value &value, bool divides, const char *what, const ValueSource &source,
                            bool &is_null);

//! A squared reducer's factor (DecideConstraint::QuadraticGroup::scale), one value for the whole query, as read on the
//! input's first row: the square's sign times the factor, or over it where it divides. A NULL factor makes every
//! instance of the clause NULL, which sets `is_null` and leaves the sign as written; one the model cannot read is
//! refused, named by `source`; a divisor of 0 is a division by zero, and a reciprocal past a DOUBLE names the clause
//! (`source.clause`).
double ScaleSquaredGroup(const Value &value, const DecideConstraint::QuadraticGroup &group, const ValueSource &source,
                         bool &is_null);

//! Whether a composed term reads no row: a reducer over none is NULL (deck p. 56), so its clause is skipped and its
//! objective term dropped.
bool DecideComposedTermReadsNoRow(const ComposedMinMaxTermData &term);

//! A composed clause's bound (LogicalDecide::ComposedMinMaxConstraint::rhs_expr): one constant by construction, which
//! the optimizer asserts (AssertComposable) and the value rewrite leaves no other shape for. Decided by foldability,
//! since canonicalization rebuilds the bound side as an additive chain, so a constant arrives as `(0 - 3) + 0` just as
//! legitimately as `-3`.
double DecideComposedBound(ClientContext &context, const LogicalDecide::ComposedMinMaxConstraint &spec);

//! AVG denominator scaling, shared by the constraint and objective sides and by the linear/bilinear (single-column)
//! and quadratic-inner (sqrt-scaled) cases. The rows a term reads are its filter: a clause's `when` is folded into it
//! upstream.
void ScaleAvgRows(CoefficientColumn &col, bool has_filter, const vector<bool> &filter_mask, bool quadratic_inner,
                  idx_t num_rows, const vector<idx_t> &row_group_ids, idx_t num_groups);

//! Each AVG term of `constraint` divided by the rows its filter reads in its group (ScaleAvgRows); the clause is
//! `avg_scaled` when every linear term was, a pure AVG left side, which the model builder tags so a diagnosis renders
//! it as `AVG(...)` (a mixed or a bilinear or quadratic left side has no clean AVG re-quote).
void ScaleAvgTerms(const DecideConstraint &constraint, const vector<TermFilterState> &filters, idx_t num_rows,
                   EvaluatedConstraint &evaluated);

//! An AVG under `<>`: dividing its terms by their group's count gives fractional coefficients, which the `<>`
//! expansion's integer step refuses. So a left side of linear terms that are all AVG keeps them as sums, and the
//! bound is multiplied by each group's count instead (EvaluatedConstraint::ne_avg_rhs_scale): `filters` then scale
//! nothing. A mixed, bilinear or quadratic left side keeps the division, which may still be refused.
void HoistAvgIntoNotEqualBound(const DecideConstraint &constraint, vector<TermFilterState> &filters,
                               EvaluatedConstraint &evaluated);

//! The bound of a clause over groups that reads a frame and may differ within a group (FRAME_TIGHTEST_BOUND_TAG, D85):
//! each group's instance takes its tightest value among all its kept rows where the bound has one, a row its reducers'
//! own `when`s drop included (a group's every kept row bounds it) -- the least under `<`
//! and `<=`, the greatest under `>` and `>=`, its one value under `=` -- as a side table takes a column's (paper
//! §3.2.1). A row where the bound has none (`bound_null`) bounds nothing, and a group with no such row has no instance
//! (`null_rows`). `kept_groups` numbers each kept row's group, empty for every kept row in one; `instances` is what
//! remains of those groups once a clause's rules have dropped some (empty: all of one group), and a group it no longer
//! holds has no instance, so no bound to read (D68). So does a `grouped` clause's group holding a row of `null_rows`, a
//! NULL its left side reads, which SkipNullInstances drops next.
void TakeTightestFrameBound(ExpressionType comparison, const string &written, const vector<bool> &kept,
                            const vector<bool> &bound_null, const vector<idx_t> &kept_groups,
                            const vector<idx_t> &instances, idx_t num_groups, bool grouped, vector<double> &rhs,
                            vector<bool> &null_rows);

//! The classes each group's rows stand for (language/guide.md §8.1, D28), kept for the diagnosis: under a `per K:`,
//! K's classes among the group's kept rows, in the order its rows first read them; with no `per`, every kept row of
//! the group; under `per ()`, the whole clause, one set its groups share. One SolverInput::ClassSet per group with
//! kept rows, appended to `class_sets` and referenced from the result. `kept_row_group` maps each kept row to its
//! group; empty when the clause has one implicit group, which then holds every kept row. `classes` reads K: each kept
//! row's class (INVALID_INDEX in none) and each class's name.
vector<idx_t> DecideGroupClassSets(const DecideClauseScope &scope, idx_t num_rows, const vector<bool> &kept_rows,
                                   const vector<idx_t> &kept_row_group, idx_t groups,
                                   const std::function<void(vector<idx_t> &, vector<GroupName> &)> &classes,
                                   vector<SolverInput::ClassSet> &class_sets);

//! The row each variant of a clause in the per-row shape under `per K:` stands on (D70): `variant_of` numbers each
//! kept row's variant, INVALID_INDEX on every other row, and `class_of_row` its class of K. A variant's rows agree on
//! every field the clause reads, so its first stands for them, unless the auxiliary the clause defines
//! (`defined_var`) has no value there and has one at a later row of the variant: its definition stands on a row where
//! it has one, if any, so a row that comes first does not leave it undefined for the rest (D88, `absent_reads`).
//! Writes `row_group_ids` (one row per variant), each variant's class and row, and each class's count of variants.
void DecideOneRowPerVariant(const vector<idx_t> &variant_of, const vector<idx_t> &class_of_row, idx_t num_variants,
                            idx_t num_classes, const vector<vector<bool>> &absent_reads, idx_t defined_var,
                            vector<idx_t> &row_group_ids, vector<idx_t> &class_of, vector<idx_t> &first_rows,
                            vector<idx_t> &variants_per_class);

//! Under DIAGNOSE, a variant of a class that holds more than one stands for itself, named as its row is
//! (`group_names`), not for its class (D28): its own class set replaces its class's in `group_class_set`. Only where
//! the clause has more variants than classes are its variants named as their own.
void DecideNameVariantClassSets(const vector<idx_t> &class_of, const vector<idx_t> &variants_per_class,
                                const vector<GroupName> &group_names, vector<idx_t> &group_class_set,
                                vector<SolverInput::ClassSet> &class_sets);

//! The names of a clause's variants, for a diagnosis (D28): each is named by its class's key values, followed by its
//! own field values where its class holds more than one variant (`D1, 100` beside `D1, 90`), and by the row it is read
//! at where another variant of its class shows the same values, differing only by the decision instance it reads.
//! `read(variant)` gives a variant's values, its class's `class_columns` and then its fields', each at its first row
//! (`first_rows`). Sets the reader of `key_values`, which holds a NULL mode per value read, and adds the row's.
void DecideNameVariants(const vector<DecideSignatureField> &fields, idx_t class_columns,
                        std::function<vector<Value>(idx_t)> read, const vector<idx_t> &class_of,
                        const vector<idx_t> &variants_per_class, const vector<idx_t> &first_rows,
                        SolverInput::KeyValues &key_values);

//! A row in no class of a keyed scope (a NULL written key, a dropped row, a helper's group with no member) has no
//! decision: it maps to one spare class, so that every row has an id (KeyedMapping::absent_class).
void DecideMapRowsInNoClass(KeyedMapping &mapping);

//! What stage 06 reads of a helper decision besides its members (ReducerValueLinkSpec), as the value rewrite made it.
ReducerValueLinkSpec DecideValueLinkSpec(const LogicalDecide::ReducerValueLink &link);

//! One member: the terms whose coefficients `columns` and variables `variables` are read at `row`, each coefficient
//! times `scale`, those of no decision its constant. A constant past a DOUBLE names `label`, the helper as written.
void DecideListMember(const vector<CoefficientColumn> &columns, const vector<idx_t> &variables, idx_t row, double scale,
                      const string &label, ReducerValueMembers &out);

//! `ec`'s members -- its rows, each in the group `row_group_ids` names, a reducer's `by` groups partitioning them --
//! listed per group (ReducerValueMembers), each group read at its first member row; a group with no member is left
//! out.
void DecideListRowMembers(const EvaluatedConstraint &ec, idx_t num_rows, const string &label, ReducerValueMembers &out);

//! The helpers' sizes, which DECIDB_PROFILE reports: their instances, members and members' terms.
void DecideCountValueLinks(const vector<ReducerValueLinkSpec> &links);

//! The least and the greatest of the values added, as ORDER BY places them (NULL last), so a message naming them does
//! not depend on the order rows arrive in. It holds the values' addresses, which must outlive it.
class DecideValueRange {
public:
	void Add(const Value &value);
	//! The two as SQL literals, `least, greatest`; false when every value added is one value.
	bool Spans(string &values) const;

private:
	const Value *least = nullptr;
	const Value *greatest = nullptr;
};

//! How an evaluator reads the fields of the objective's signature over its kept rows, for the scope check's message:
//! flat from the rows, the tree from its root's entries.
struct DecideVariantReader {
	//! The least and the greatest of `value`, field `field`'s own or element `element` of its `by` key, over the kept
	//! rows (DecideValueRange::Spans).
	std::function<bool(idx_t field, idx_t element, const Expression &value, string &values)> values;
	//! Whether field `field`'s decision has more than one instance over the kept rows.
	std::function<bool(idx_t field)> instances;
	//! Why field `field`'s frame reads more than one value over the kept rows (DecideFrameDifference); empty when it
	//! reads one.
	std::function<string(idx_t field)> frame;
};

//! The error for an objective whose kept rows are more than one variant of `fields`, so it would be produced more than
//! once (D72, language/guide.md §7.3): the first field `read` finds two values or instances of, and the edit.
string DecideObjectiveTwiceMessage(const vector<DecideSignatureField> &fields, const vector<DecideKey> &keys,
                                   const vector<DecideVarScopeInfo> &scopes, const DecideVariantReader &read);

//! How an evaluator reads a frame field of the objective's signature over its kept rows, for the scope check's message
//! (DecideFrameDifference): flat from the rows, the tree from its root's entries.
struct DecideFrameReader {
	//! Each kept row's timeline and peer (DecideFrameMap::timeline, peer), in row order.
	vector<pair<idx_t, idx_t>> rows;
	//! The names of peers `first` and `last`.
	std::function<pair<Value, Value>(idx_t first, idx_t last)> positions;
	//! The least and the greatest of element `element` of the frame's timeline key, `value`, over the kept rows
	//! (DecideValueRange::Spans).
	std::function<bool(idx_t element, const Expression &value, string &values)> timeline;
	//! Whether what the frame's `else` reads differs over the kept rows: whether the anchor reaches it, and there its
	//! coefficients, a NULL as one value, and its decisions' instances.
	std::function<bool()> fallbacks;
};

//! Why the kept rows are more than one variant of the frame field `field`, by the frame's keys (D82): more than one
//! position of one timeline, when it reads relative to the anchor, named by its first and last in its own order, more
//! than one timeline, or more than one value its `else` reads; empty when none (language/guide.md §7.3).
string DecideFrameDifference(const DecideSignatureField &field, const DecideFrameSpec &frame,
                             const vector<DecideKey> &keys, const DecideFrameReader &read);

} // namespace duckdb
