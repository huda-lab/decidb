//===----------------------------------------------------------------------===//
//                         DeciDB
//
// duckdb/execution/operator/decide/decide_tree_columns.hpp
//
// The bottom-up half's values (language/guide.md §6.1): each node's programs computed over its entries, one executor
// per window of entries, and read as stage 08 reads a coefficient or a bound, so a value and a message are flat's.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/decide_error_ledger.hpp"
#include "duckdb/execution/operator/decide/decide_descent.hpp"
#include "duckdb/execution/operator/decide/decide_frame_map.hpp"
#include "duckdb/function/aggregate_function.hpp"
#include "duckdb/storage/arena_allocator.hpp"

#include <functional>

namespace duckdb {

class ClientContext;
struct DecideSourceNames;
struct KeyedMapping;

//! A G over data's value per slot (language/guide.md §6.1): one AggregateFunction state per slot in a dense array, as
//! DuckDB's PerfectAggregateHashTable lays out its groups, updated a window of members at a time and finalized once.
//! The tree's columns fold a data reducer here (DecideTreeColumns::Folds).
class DecideAggregateStates {
public:
	//! `slots` states of `aggregate`; `text` is the reducer as written, which a NaN it reads is named by, unless
	//! `rejects_nan` is false: a count reads none of its values, and the columns leave a NaN to the data check, which
	//! ranks first (DecideTreeColumns::Folds). A NaN reaches a fold that refuses it only where no check reads it: a
	//! reducer over frames.
	DecideAggregateStates(ClientContext &context, const BoundAggregateExpression &aggregate, idx_t slots, string text,
	                      bool rejects_nan);
	~DecideAggregateStates();
	//! Folds member i, `arguments[a][i]` for each argument a, into the state of slot `slots[i]`. A NaN of what it reads
	//! (`read`, its first argument when null: a norm's body otherwise) raises the reducer's message.
	void Update(const vector<vector<Value>> &arguments, const vector<idx_t> &slots,
	            const vector<Value> *read = nullptr);
	//! Each slot's value; a slot that folded no member holds what the aggregate gives no rows.
	vector<Value> Finalize();

private:
	const BoundAggregateExpression &aggregate;
	ArenaAllocator allocator;
	AggregateInputData input;
	idx_t slot_count;
	idx_t state_size;
	data_ptr_t states;
	string text;
	bool rejects_nan;
};

//! DECIDB_TREE_INJECT=node:data:value, a test's stand-in for a node's data value, which shows a parent reads the
//! bottom-up values (L7): the node, INVALID_INDEX when unset, and the value, cast to the node's type.
struct DecideTreeInjection {
	idx_t node = DConstants::INVALID_INDEX;
	string data;
};
DecideTreeInjection ReadDecideTreeInjection(const DecideTree &tree);

//! A node's programs on its entries (DecideNode::programs), computed once its descent is finished: each program's
//! values on every entry, and the entries its evaluation raised on, with the error, which is held in the ledger at its
//! rank when the program is read (DecideEvaluateWindow).
class DecideTreeColumns {
public:
	DecideTreeColumns(const DecideTree &tree, const DecideDescent &descent, ClientContext &context,
	                  const DecideSourceNames &names, std::function<string(idx_t source_clause_id)> written_clause,
	                  DecideErrorLedger &errors);
	~DecideTreeColumns();

	//! `program` on each of `node`'s entries `reads` marks, as stage 08 reads a coefficient or a bound
	//! (ExtractDoubleColumn): signed, and 0 on the others and where it is NULL, which `nulls` marks; with no `nulls` a
	//! NULL is an internal error, as for an objective's coefficient. A value or an error on an entry it does not read
	//! is never read. A coefficient computed from `inputs` (DecideTerm::inputs, computed at `site`) names the first of
	//! them that is not finite where it fails.
	vector<double> Numbers(idx_t node, idx_t program, const vector<bool> &reads, vector<bool> *nulls,
	                       const DecideTermSite *site = nullptr,
	                       const vector<unique_ptr<Expression>> *inputs = nullptr);
	//! Of the entries of `node` that `reads` marks, those at which `program` is NULL, as flat reads what a definition
	//! reads for its absence on the rows it keeps (DefinitionNulls), and what its evaluation raised there, held at
	//! `rank` in the window of its entry's row: no value is checked, which the clause's read of the program does
	//! (Numbers).
	vector<bool> NullEntries(idx_t node, idx_t program, const vector<bool> &reads, DecideErrorRank rank);
	//! `program` on entry `entry` of `node`, as written: what a variant's name reads (DecideBody::names). What it
	//! raises is never read: the descent read the same key, and the clause the same value.
	Value ValueAt(idx_t node, idx_t program, idx_t entry);
	//! The entries of `node` its member tests keep (DecideNode::tests), flat's read rows of a reducer's terms: its
	//! other programs are read on these alone.
	const vector<bool> &Tested(idx_t node);
	//! The entries of `node` an objective term `term` is read on, flat's read rows of the term: those its member tests
	//! keep that read an instance of its decision (TermAbsentAt). A term reading none has no value there (D84).
	vector<bool> ReadEntries(idx_t node, const DecideTerm &term);
	//! Whether decision `variable` has no instance to read at `row` (D84, D88): a keyed decision's row in no class of
	//! its scope, a helper's where its group holds no member, or an auxiliary's row where what defines it has no value
	//! (`absent_reads`, or a factor of its product). None for no decision (INVALID_INDEX).
	bool AbsentAt(idx_t variable, idx_t row) const;
	//! Whether `term` reads, at `row`, an instance with no value: its decision's at the row (AbsentAt), or, for a term
	//! of a frame, where the frame reads nothing or at the row it selects (DecideFrameReadsAbsent).
	bool TermAbsentAt(const DecideTerm &term, idx_t row) const;
	//! The frames' maps over the input's `num_rows` rows (language/guide.md §5.6), each from its O's timelines and
	//! positions (SelectDecideFramePeers), and each frame's columns, through flat's own blocks
	//! (SkipDecideFrameNullReads, ComputeDecideFrameColumns), every expression they read a program on the frame's O
	//! (DecideFrameColumnPrograms): read once the descent is finished, before any program that reads a frame. What a
	//! frame refuses is held at its rank: its range's limit and its partition (D75) where its rows are routed, a
	//! position of two payloads it reads (D73) once the decisions' classes are known.
	void ReadFrames(const vector<DecideFrameSpec> &frames, const DecidePreparedModel &prepared,
	                const vector<LogicalDecide::ReducerValueLink> &links, idx_t num_rows, vector<DecideFrameMap> &maps);
	//! Frame column `column`'s value at `row` (DecidePreparedModel::frame_columns), as ReadFrames computed it.
	Value FrameValue(idx_t column, idx_t row) const;
	//! The name of the position frame `frame` holds `row` at: its order key's least spelling over its rows (D69).
	Value PositionName(idx_t frame, idx_t row) const;
	//! The keyed scopes' classes and the rows at which each auxiliary has no value
	//! (DecideGlobalSinkState::absent_reads), which AbsentAt reads: each complete once what defines it is read, before
	//! what reads it.
	void ReadAbsence(const vector<KeyedMapping> &classes, const vector<vector<bool>> &reads);
	//! Of the entries `reads` marks, the first of each instance of `node` in a class of its key, in row order: a
	//! reducer per K reads each variant of a class once in each group, at its first row (language/guide.md §5.1, §7.1),
	//! however many entries the variant holds, and nothing of a row in no class.
	vector<bool> FirstOfEachInstance(idx_t node, const vector<bool> &reads) const;

	//! The value of a G over data in each of its slots: its function folded over its operand's members there
	//! (DecideAggregateStates), NULL where the fold raised, whose error is held at the G's rank.
	const vector<Value> &Folds(idx_t reducer);
	//! Each data check's message, a hidden root's, where it is not NULL, at the check's rank.
	void CheckData();

	//! Each node's programs the evaluation computed, on each of its entries by its row, with the entry's instance
	//! (DECIDB_DUMP_VALUES): what a node holds once it is read, as the deck's bottom-up pages show it.
	string Dump() const;

private:
	struct ProgramColumn;
	struct NodeColumns;

	const DecideTree &tree;
	const DecideDescent &descent;
	ClientContext &context;
	const DecideSourceNames &names;
	std::function<string(idx_t)> written_clause;
	DecideErrorLedger &errors;
	vector<unique_ptr<NodeColumns>> nodes;
	const vector<KeyedMapping> *mappings = nullptr;
	const vector<vector<bool>> *absent_reads = nullptr;
	const vector<DecideFrameMap> *frame_maps = nullptr;
	//! Each frame column's value on every row, and the error a row carries, read where a reader reaches the row.
	DecideFrameColumnValues frame_values;
	struct FramePeer {
		idx_t slot;
		const DecideDescentPosition *position;
	};
	//! Per frame, each peer's timeline slot and position (DecideDescentTimeline), which name it.
	vector<vector<FramePeer>> frame_peers;

	//! Per G over data, its folds, once computed; per G, the slot each row lies in, once read.
	unordered_map<idx_t, vector<Value>> folds;
	unordered_map<idx_t, vector<idx_t>> row_slots;
	DecideTreeInjection injection;

	NodeColumns &Computed(idx_t node);
	//! `node`'s programs that read no frame column, with no frame read in its layout: an O's own frame columns are
	//! computed from them, before its other programs read those columns.
	NodeColumns &FramePrograms(idx_t node);
	//! `node`'s programs not computed yet, each with its holes' values; with `frames` false only those that read no
	//! frame column. A program reading a frame column at an entry whose row carries an error raises that error there.
	void ComputeNode(idx_t node, NodeColumns &columns, bool frames);
	//! Whether `program` of `node` reads one of its frame columns.
	bool ReadsFrame(idx_t node, idx_t program) const;
	//! A frame column's value carries the error its row raised (DecideFrameValues), which `program` of `node`, reading
	//! it there, raises, as flat's read of the row raises it.
	void CarryFrameErrors(idx_t node, idx_t program, ProgramColumn &column);
	//! A frame's map from the timelines and positions of its O `node` (DecideDescentTimeline): its anchors, timelines
	//! and peers, which SelectDecideFramePeers selects from; and each peer's timeline slot and position, which D73
	//! names a position of two payloads by.
	DecideFrameMap FrameMap(const DecideFrameSpec &frame, idx_t node, idx_t num_rows, vector<FramePeer> &peers) const;
	//! D75 over the anchors of the O `node`: each class of its partition key lies in one timeline.
	void CheckPartition(const DecideFrameSpec &frame, idx_t node);
	//! The program on a frame's O that computes each expression its columns read (DecideFrameColumnPrograms) and each
	//! payload term's coefficient of a range over decisions, its helper's (`links`): the O and the program.
	std::unordered_map<const Expression *, pair<idx_t, idx_t>>
	FrameExpressions(const DecidePreparedModel &prepared, const vector<LogicalDecide::ReducerValueLink> &links) const;
	//! `program` of the O `node` on the rows `rows` marks, its anchors there, as a frame's block reads it
	//! (DecideFrameRead).
	DecideFrameValues FrameRows(idx_t node, idx_t program, const vector<bool> &rows);
	//! D73, frame after frame, position after position: a position of two payloads a frame reads is refused, once the
	//! decisions' classes are known (FRAME_PEERS).
	void CheckPositions(const vector<DecideFrameSpec> &frames);
	//! Each entry of `node`'s value of its hole `hole`: the fold of the hole's G in the slot the entry's row reads.
	vector<Value> HoleValues(idx_t node, idx_t hole);
	//! Which of its entries the node's member tests keep, and the errors they raise.
	void ReadTests(idx_t node, NodeColumns &columns);
	//! Holds each error of `program` on an entry `reads` marks, at its rank.
	void Report(idx_t node, idx_t program, const vector<bool> &reads);
	DecideErrorRank Rank(idx_t node, idx_t program, idx_t entry, bool value) const;
};

} // namespace duckdb
