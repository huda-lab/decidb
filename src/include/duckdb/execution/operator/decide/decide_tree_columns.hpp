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
#include "duckdb/function/aggregate_function.hpp"
#include "duckdb/storage/arena_allocator.hpp"

#include <functional>

namespace duckdb {

class ClientContext;
struct DecideSourceNames;

//! A G over data's value per slot (language/guide.md §6.1): one AggregateFunction state per slot in a dense array, as
//! DuckDB's PerfectAggregateHashTable lays out its groups, updated a window of members at a time and finalized once.
//! The tree's columns fold a data reducer here, and so does the interim evaluation.
class DecideAggregateStates {
public:
	//! `slots` states of `aggregate`; `text` is the reducer as written, which a NaN it reads is named by, unless
	//! `rejects_nan` is false: a count reads none of its values, and the columns leave a NaN to the data check, which
	//! ranks first (DecideTreeColumns::Folds). A NaN reaches a fold that refuses it only where no check reads it: the
	//! interim's fold of a reducer over a frame.
	DecideAggregateStates(ClientContext &context, const BoundAggregateExpression &aggregate, idx_t slots, string text,
	                      bool rejects_nan);
	~DecideAggregateStates();
	//! Folds member i, `arguments[a][i]` for each argument a, into the state of slot `slots[i]`. A NaN its first
	//! argument holds raises the reducer's message.
	void Update(const vector<vector<Value>> &arguments, const vector<idx_t> &slots);
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
	//! The entries of `node` an objective term of decision `variable` is read on, flat's read rows of the term: those
	//! its member tests keep that read an instance of the decision. A term reading none has no value there (D84).
	vector<bool> ReadEntries(idx_t node, idx_t variable);
	//! Whether decision `variable` has no instance to read at `row` (D84, D88): a keyed decision's row in no class of
	//! its key, or an auxiliary's row where what defines it has no value (`absent_reads`, once given, or a factor of
	//! its product). None for no decision (INVALID_INDEX).
	bool AbsentAt(idx_t variable, idx_t row) const;
	//! The rows at which each auxiliary has no value (DecideGlobalSinkState::absent_reads), which AbsentAt reads.
	void ReadAbsence(const vector<vector<bool>> &reads);
	//! Of the entries `reads` marks, the first of each instance of `node` in a class of its key, in row order: a
	//! reducer per K reads each variant of a class once in each group, at its first row (language/guide.md §5.1, §7.1),
	//! however many entries the variant holds, and nothing of a row in no class.
	vector<bool> FirstOfEachInstance(idx_t node, const vector<bool> &reads) const;

	//! The value of a G over data in each of its slots: its function folded over its operand's members there
	//! (DecideAggregateStates), NULL where the fold raised, whose error is held at the G's rank.
	const vector<Value> &Folds(idx_t reducer);
	//! Each data check's message, a hidden root's, where it is not NULL, at the check's rank.
	void CheckData();

	//! Interim, apart from the model emitter's shapes: the first node these columns compute no value for yet, a read
	//! of a frame or a decision-valued child's read where only data is computed (DecideBody::uncompiled), or a hidden
	//! root other than a data check, empty when there is none. A query with one has no arm. It goes once they compute
	//! every node.
	static string Uncomputed(const DecideTree &tree);

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
	const vector<vector<bool>> *absent_reads = nullptr;

	//! Per G over data, its folds, once computed; per G, the slot each row lies in, once read.
	unordered_map<idx_t, vector<Value>> folds;
	unordered_map<idx_t, vector<idx_t>> row_slots;
	DecideTreeInjection injection;

	NodeColumns &Computed(idx_t node);
	void ComputeNode(idx_t node, NodeColumns &columns);
	//! Each entry of `node`'s value of its hole `hole`: the fold of the hole's G in the slot the entry's row reads.
	vector<Value> HoleValues(idx_t node, idx_t hole);
	//! Which of its entries the node's member tests keep, and the errors they raise.
	void ReadTests(idx_t node, NodeColumns &columns);
	//! Holds each error of `program` on an entry `reads` marks, at its rank.
	void Report(idx_t node, idx_t program, const vector<bool> &reads);
	DecideErrorRank Rank(idx_t node, idx_t program, idx_t entry, bool value) const;
};

} // namespace duckdb
