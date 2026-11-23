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
struct DecideFrameMap;

//! Maps a node of an expression into another chunk layout, or returns null to leave it to ToChunkReferences.
using DecideChunkLeaf = std::function<unique_ptr<Expression>(const Expression &expr)>;

//! A coefficient/value expression for ExpressionExecutor: a copy whose column references read the chunk's columns, and
//! nothing re-bound, so a DuckDB error raised on a node points where the user wrote it. With `leaf`, every node is
//! offered to it first: the tree's programs (CompileDecideTree) read their node's layout this way.
unique_ptr<Expression> ToChunkReferences(const Expression &expr, ClientContext &context,
                                         const DecideChunkLeaf &leaf = nullptr);

//! The class of a keyed scope at each data row. A declared decision's scope holds a class for every row
//! (KeyedMapping::row_to_class). A helper's is read by the rows its node holds an entry for, so it costs what reads
//! it: few of the input's rows are listed by row, and more than that hold a class each, as a declared decision's do.
//! A row that reads no class of a helper reads the spare one (KeyedMapping::absent_class).
struct DecideScopeRows {
	//! A list is shorter than the row per class it stands for while fewer than one row in LISTED_SHARE reads.
	static constexpr idx_t LISTED_SHARE = 8;

	const idx_t *declared = nullptr;
	vector<idx_t> class_of_row;
	//! (row, class), by row once closed.
	vector<pair<idx_t, idx_t>> listed;
	idx_t num_classes = 0;
	idx_t absent_class = DConstants::INVALID_INDEX;

	DecideScopeRows() = default;
	//! A declared decision's.
	explicit DecideScopeRows(const KeyedMapping &mapping)
	    : declared(mapping.row_to_class.data()), num_classes(mapping.num_classes), absent_class(mapping.absent_class) {
	}
	//! A helper's, of `num_classes` classes, read by at most `reads` of the input's `num_rows` rows: List each read,
	//! then Close.
	DecideScopeRows(idx_t num_rows, idx_t num_classes, idx_t reads);
	//! `row` reads class `class_id`, or none (INVALID_INDEX). A node read from several places lists a row once for
	//! each, with the same class.
	void List(idx_t row, idx_t class_id) {
		if (!class_of_row.empty()) {
			class_of_row[row] = class_id;
		} else if (class_id != DConstants::INVALID_INDEX) {
			listed.emplace_back(row, class_id);
		}
	}
	//! Numbers the spare class if some row reads none, and gives the scope's classes as stage 06 reads them.
	KeyedMapping Close(idx_t num_rows);
	idx_t At(idx_t row) const;
	bool Absent(idx_t row) const {
		return absent_class != DConstants::INVALID_INDEX && At(row) == absent_class;
	}
};

//! How a data row gives the instance of the decision one term reads there, which the term names
//! (ClauseRecords::instances): the row itself for a decision per row, the row's class for a keyed decision, 0 for a
//! query-wide one. A term a frame reads (language/guide.md §5.6) reads its decision at the row the frame selects, or
//! at its own row where the frame selects none, where the term has no value or 0.
struct DecideInstanceOfRow {
	const DecideScopeRows *keyed = nullptr;
	const DecideFrameMap *frame = nullptr;
	bool query_wide = false;

	idx_t At(idx_t row) const {
		return query_wide ? 0 : keyed ? keyed->At(FrameRow(row)) : FrameRow(row);
	}
	idx_t FrameRow(idx_t row) const {
		return frame ? SelectedRow(row) : row;
	}
	idx_t SelectedRow(idx_t row) const;
};

//! `expr` constant-folded once by DuckDB's own rule, as the expression rewriter folds a query's own expressions.
unique_ptr<Expression> DecideFoldConstants(unique_ptr<Expression> expr, ClientContext &context);

//! Whether an error stops the query wherever it is raised, rather than belonging to the row it is raised on.
bool DecideErrorStopsTheQuery(const ErrorData &error);

//! Expression `expr_idx` of `*executor` (`lowered`) on `chunk`, whose first row is row `offset` of what it is evaluated
//! over, into `values` from `offset` on. When a row raises, each row is evaluated alone, a failing one keeping its
//! error in `errors` and NULL; the executor, whose states the failure may have left half written, is made again by
//! `remake`. So an error waits for a read that reaches its row (language/guide.md §6.3), as a tree node's entries
//! (DecideTreeColumns) are evaluated. `raised`, when given, keeps the
//! error the whole window's evaluation raised: the one DuckDB's vectorized evaluation of the window meets first.
void DecideEvaluateWindow(ClientContext &context, unique_ptr<ExpressionExecutor> &executor, idx_t expr_idx,
                          const std::function<unique_ptr<ExpressionExecutor>()> &remake, const Expression &lowered,
                          Vector &values, std::map<idx_t, ErrorData> &errors, DataChunk &chunk, idx_t offset,
                          ErrorData *raised = nullptr);

//! A number read from the source, and what the message that refuses it calls it: `value` is the expression the number
//! comes from, printed as the user wrote it and pointed at where it stands (DecideInvalidValueError).
struct ValueSource {
	DecideValueRole role;
	const Expression &value;
	const DecideSourceNames &names;
};

//! Refuses a number the model cannot read (NaN, or infinite where a bound is not allowed to be), naming `source` at
//! the place it stands: the value's own location, where the location rule gave it one (DecideReplaces). A coefficient
//! names itself, as a constant when it reads no column and as a value of some rows when it does.
[[noreturn]] void ThrowInvalidValue(const ValueSource &source);

//! The entries among the first `count` of `values` that hold a NaN: none for a NULL, and none for a type that holds no
//! NaN. A fold over frames (a range's window, a reducer over frames) reads a NaN through these, and its value is NaN
//! (DecideNanValue), which the value's reader refuses with the one message (ThrowInvalidValue): SQL's MIN would pass
//! over it.
vector<idx_t> DecideNanEntries(Vector &values, idx_t count);

//! What a fold that read a NaN gives, in the type its aggregate returns: a NaN, for the FLOAT or DOUBLE that every
//! aggregate of a value that holds a NaN returns.
Value DecideNanValue(const LogicalType &type);

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

//! A clause as the emitter evaluates it over the rows it keeps, before its records are written
//! (DecideWriteClauseRecords): each term's coefficient and the bound on every kept row, and each one's group. A row is
//! numbered by its place among the kept rows, in the input's order, so no column is as long as the input.
struct EvaluatedClauseColumns : EvaluatedConstraint {
	//! The record stage 06 reads, once its rows are written: the working columns stay behind.
	EvaluatedConstraint Take() {
		return std::move(static_cast<EvaluatedConstraint &>(*this));
	}
	//! Per term, its decision (INVALID_INDEX for a term that reads none) and its coefficient column.
	vector<idx_t> term_variables;
	vector<CoefficientColumn> row_coefficients;
	CoefficientColumn rhs_values; // RHS column (logical size = the kept rows)
	//! A per-row bound on one decision: the values it admits on each row (ClauseRecords::admitted_lower). Logical
	//! size 0 for any other constraint.
	CoefficientColumn admitted_lower;
	CoefficientColumn admitted_upper;
	//! Each kept row's group, 0..num_groups-1: the clause's `by` groups or classes, or the one group of a clause with
	//! neither. DConstants::INVALID_INDEX = row excluded (outside every group, or its instance has no value).
	//! An aggregate clause always holds one entry per kept row. A per-row clause leaves it empty where every kept
	//! row emits and no class numbers its rows.
	vector<idx_t> row_group_ids;
	idx_t num_groups = 0;
};

//! The kept rows of each row a clause emits, by place, rows[begin[e] .. begin[e + 1]), and its group: a group of an
//! aggregate clause that holds a row (all kept rows, in the one row of a clause of no groups), a row of a per-row
//! clause that its group keeps.
struct ClauseRowSets {
	vector<idx_t> begin {0};
	vector<idx_t> rows;
	vector<idx_t> groups;
};
ClauseRowSets DecideClauseRowSets(const EvaluatedClauseColumns &evaluated, idx_t num_rows);

//! Writes the record of each row `evaluated` emits (EvaluatedConstraint::records) from its columns: the one place the
//! rows stage 06 reads are made. A row's terms are listed term by term, each over the row's kept rows, which `rows`
//! gives the data row of, by place, and `instances` the instance each term's decision has there. `written` names the
//! clause when the left side's constant grows past a DOUBLE (AddCoefficient).
void DecideWriteClauseRecords(EvaluatedClauseColumns &evaluated, const ClauseRowSets &sets, const vector<idx_t> &rows,
                              const vector<DecideInstanceOfRow> &instances, const DecideClauseRef &written);

//! The objective's product groups outside its reducers (`MAXIMIZE sum(x) + s * u`), counted on its first kept row as
//! a linear term there is: their weight; one inside a reducer sums over its rows.
void CountProductsOnce(const vector<DecideProductGroup> &groups, vector<EvaluatedProductGroup> &evaluated,
                       const vector<bool> *kept, idx_t num_rows);

//! The first row of each group of a clause in the aggregate shape, none for a group with no row.
vector<idx_t> FirstRowOfEachGroup(const vector<idx_t> &row_group_ids, idx_t num_groups);

//! A clause's terms outside its reducers in the aggregate shape, counted on the first row of each of its groups.
void CountTupleTermsOncePerGroup(const vector<DecideTerm> &terms, EvaluatedClauseColumns &evaluated);

//! The instances `row_group_ids` names: in the aggregate shape (`grouped`) each group a row stands in, else each row
//! in a group, every row where a per-row clause numbers none.
idx_t CountInstances(const vector<idx_t> &row_group_ids, idx_t num_groups, idx_t num_rows, bool grouped);

//! A clause's evaluation before its rows are read (EvaluatedConstraint): its facts, its repair group, and each term's
//! decision.
EvaluatedClauseColumns InitializeEvaluatedConstraint(const DecideConstraint &constraint, idx_t repair_group_id);

//! How a repair names `constraint`'s bound (ConstraintProvenance::rhs_label of the row's record, under DIAGNOSE only:
//! `diagnose`), whether one editable value stands for it on every row, and a constant's value there. A constant
//! canonicalization left of a written bound that reads data (`x <= cap * o` is `x - cap * o <= 0`,
//! ConstraintSourceInfo::source_rhs_reads_data) repairs as that bound does. Returns whether it is read on each row
//! instead.
bool DescribeBound(const DecideConstraint &constraint, idx_t num_rows, bool diagnose,
                   const DecideSourceText &source_text, const vector<ConstraintSourceInfo> &constraint_sources,
                   const vector<KeyedScopeInfo> &keyed_scopes, const vector<DecideKey> &keys,
                   EvaluatedClauseColumns &evaluated);

//! Per-term filter state for a reducer's own `when` (constraint terms, product groups, and their objective-side
//! equivalents all share this shape).
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
//! for a row in none (the tree's G slots by rank).
struct DecideKeyGroups {
	const vector<idx_t> &row_group_ids;
	idx_t num_groups;
};

//! The rows `row_filter` keeps, numbered by their group of `groups`, the
//! groups renumbered densely in key-value order; INVALID_INDEX on every other row. Returns the kept groups, as
//! `groups` numbers them. `out_every_row_group_ids`, when given, takes every row of a kept group under the same
//! numbering: a group no row of `row_filter` reached emits no constraint, so its rows stay out.
vector<idx_t> DecideKeptGroups(const DecideKeyGroups &groups, idx_t num_rows,
                               const std::function<bool(idx_t)> &row_filter, vector<idx_t> &out_row_group_ids,
                               idx_t &out_num_groups, vector<idx_t> *out_every_row_group_ids);

//! The rows of a clause in the grouped shape (the aggregate shape, an easy MIN/MAX; language/guide.md §6.4): its kept
//! rows grouped by its `by` key's `groups` (null with no key), a group emitting the rows a reducer reads
//! (`local_row_active`, with `has_local_filters`), or, under `<>`, every kept row. A `per K:` that names classes holds
//! once per class (`class_of_row`: a row's class, INVALID_INDEX for a row in none), so with no kept row in a class
//! there is none. Writes `evaluated`'s row groups, `kept_row_group_ids` (when given: every kept row of an emitted
//! group) and the instances the clause holds before any with no value is skipped (D90): the groups a class reaches,
//! and one for each class whose rows reach none (a NULL `by` key), which has no value. Returns the groups of `groups`
//! it keeps, which its names name.
vector<idx_t> GroupRowsByShape(const DecideConstraint &constraint, idx_t num_rows, bool has_local_filters,
                               const vector<bool> &local_row_active, const DecideKeyGroups *groups,
                               const vector<idx_t> &class_of_row, vector<idx_t> *kept_row_group_ids,
                               EvaluatedClauseColumns &evaluated, idx_t &grouped_instances);

//! AVG denominator scaling, shared by the constraint and objective sides: a linear term's coefficient, or a product
//! group's weight, which multiplies the whole product. The rows a term reads are its filter: a clause's `when` is
//! folded into it upstream.
void ScaleAvgRows(CoefficientColumn &col, bool has_filter, const vector<bool> &filter_mask, idx_t num_rows,
                  const vector<idx_t> &row_group_ids, idx_t num_groups);

//! Each AVG term of `constraint` divided by the rows its filter reads in its group (ScaleAvgRows).
void ScaleAvgTerms(const DecideConstraint &constraint, const vector<TermFilterState> &filters, idx_t num_rows,
                   EvaluatedClauseColumns &evaluated);

//! An AVG under `<>`: dividing its terms by their group's count gives fractional coefficients, which the `<>`
//! expansion's integer step refuses. So a left side of linear terms that are all AVG keeps them as sums, and the
//! bound is multiplied by each group's count instead (NotEqualClause::excluded; this returns whether it is): `filters`
//! then scale nothing. A mixed left side keeps the division, which may still be refused.
bool HoistAvgIntoNotEqualBound(const DecideConstraint &constraint, vector<TermFilterState> &filters);

//! The bound of a clause over groups that reads a frame and may differ within a group
//! (BoundDecideClause::tightest_frame_bound, D85): each group's instance takes its tightest value among all its kept
//! rows where the bound has one, a row its reducers' own `when`s drop included (a group's every kept row bounds it) --
//! the least under `<` and `<=`, the greatest under `>` and `>=`, its one value under `=` -- as a side table takes a
//! column's (paper §3.2.1). A row where the bound has none (`bound_null`) bounds nothing, and a group with no such row
//! has no instance
//! (`null_rows`). `kept_groups` numbers each kept row's group, empty for every kept row in one; `instances` is what
//! remains of those groups once a clause's rules have dropped some (empty: all of one group), and a group it no longer
//! holds has no instance, so no bound to read (D68). So does a group holding a row of `null_rows`, a
//! NULL its left side reads, which DecideAbsence::SkipNullInstances drops next. An `=` bound that takes two values in a
//! group names the group by its key (`group_names`, whose values `key_values` reads), at `place` in the query (invalid
//! for none).
void TakeTightestFrameBound(ExpressionType comparison, const string &written, optional_idx place,
                            const vector<bool> &bound_null, const vector<idx_t> &kept_groups,
                            const vector<idx_t> &instances, idx_t num_groups, const vector<GroupName> &group_names,
                            const vector<SolverInput::KeyValues> &key_values, vector<double> &rhs,
                            vector<bool> &null_rows);

//! The classes each group's rows stand for (language/guide.md §8.1, D28), kept for the diagnosis: under a `per K:`,
//! K's classes among the group's kept rows, in the order its rows first read them; with no `per`, every kept row of
//! the group; under `per ()`, the whole clause, one set its groups share. One SolverInput::ClassSet per group with
//! kept rows, appended to `class_sets` and referenced from the result. `kept_row_group` maps each kept row to its
//! group; empty when the clause has one implicit group, which then holds every kept row. `classes` reads K: each kept
//! row's class (INVALID_INDEX in none) and each class's name.
vector<idx_t> DecideGroupClassSets(const DecideClauseScope &scope, const vector<idx_t> &rows,
                                   const vector<idx_t> &kept_row_group, idx_t groups,
                                   const std::function<void(vector<idx_t> &, vector<GroupName> &)> &classes,
                                   vector<SolverInput::ClassSet> &class_sets);

//! The row each variant of a clause in the per-row shape under `per K:` stands on (D70): `variant_of` numbers each
//! kept row's variant, INVALID_INDEX on every other row, and `class_of_row` its class of K. A variant's rows agree on
//! every field the clause reads, so its first stands for them, unless the auxiliary the clause defines
//! has no value there (`defines_at_no_value`) and has one at a later row of the variant: its definition stands on a
//! row where it has one, if any, so a row that comes first does not leave it undefined for the rest (D88).
//! Writes `row_group_ids` (one row per variant), each variant's class and row, and each class's count of variants.
void DecideOneRowPerVariant(const vector<idx_t> &variant_of, const vector<idx_t> &class_of_row, idx_t num_variants,
                            idx_t num_classes, const std::function<bool(idx_t)> &defines_at_no_value,
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
//! (`first_rows`). Sets the reader of `key_values`, whose `names` already name those values, and adds the row's, `row`.
void DecideNameVariants(const vector<DecideSignatureField> &fields, idx_t class_columns,
                        std::function<vector<Value>(idx_t)> read, const vector<idx_t> &class_of,
                        const vector<idx_t> &variants_per_class, const vector<idx_t> &first_rows,
                        SolverInput::KeyValues &key_values);

//! One member: the terms whose coefficients `columns` and variables `variables` are read at `row`, each on the
//! instance `instances` gives there and its coefficient times `scale`, those of no decision its constant. A constant
//! past a DOUBLE points at `clause`, the one that reads the helper.
void DecideListMember(const vector<CoefficientColumn> &columns, const vector<idx_t> &variables,
                      const vector<DecideInstanceOfRow> &instances, idx_t row, double scale,
                      const DecideClauseRef &clause, ReducerValueMembers &out);

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

//! How the fields of the objective's signature are read over its kept rows, for the scope check's message: from its
//! root's entries.
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

//! Why an objective's kept rows are more than one variant, and where the query writes what differs.
struct DecideVariantDifference {
	string message;
	optional_idx place;
};

//! The error for an objective whose kept rows are more than one variant of `fields`, so it would be produced more than
//! once (D72, language/guide.md §7.3): the first field `read` finds two values or instances of. The place is the
//! field's expression, the key element, the decision's declaration or the frame, as `text` and `frames` have them.
DecideVariantDifference DecideObjectiveTwice(const vector<DecideSignatureField> &fields, const vector<DecideKey> &keys,
                                             const vector<DecideVarScopeInfo> &scopes,
                                             const vector<DecideFrameSpec> &frames, const DecideSourceText &text,
                                             const DecideVariantReader &read);

//! How a frame field of the objective's signature is read over its kept rows, for the scope check's message
//! (DecideFrameDifference): from its root's entries.
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
