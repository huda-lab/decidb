//===----------------------------------------------------------------------===//
//                         DeciDB
//
// duckdb/execution/operator/decide/decide_descent.hpp
//
// The top-down half of the E/G/O evaluator. Containers own their values: no
// operation after Append needs a source chunk.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/planner/decide/decide_tree.hpp"

#include <functional>

namespace duckdb {

struct DecideDescentEntry {
	idx_t row;
	//! The enclosing reducer/timeline slot; E's per never changes it.
	idx_t parent_slot;
	//! This E instance, reducer group or frame timeline.
	idx_t slot;
	//! Distinct sigma within the class. Carried-value error witnesses may
	//! have several entries belonging to this one instance.
	idx_t instance = DConstants::INVALID_INDEX;
	vector<Value> signature;
	vector<Value> columns;
	//! What its node's programs read beyond `columns` (DecideNode::program_columns): no field, so it splits no entry.
	vector<Value> program_columns;
	//! A set E's, per node it depends on, where its row reads that node's value: a G's group, an O's timeline, or the
	//! row where the O has an `else`, INVALID where the row did not reach it or an E's, read by class (D88). Rows
	//! equal in sigma, columns and reads are one entry.
	vector<idx_t> reads;
	//! Raw and comparison forms of a frame's over key.
	Value order;
	Value order_key;
	//! D75's per/path partition can be wider than this frame's timeline.
	vector<Value> partition;
	bool partition_present = true;
};

struct DecideDescentGroup {
	//! The G or O slot an E's class lies in; INVALID for a G's or O's group, whose key is its whole path key.
	idx_t parent_slot;
	//! The values its rows group on, a collated column under its collation; a message quotes its Label.
	vector<Value> key;
	idx_t first_row;
	//! Value-order rank, assigned by Finish; storage addresses remain stable.
	idx_t rank = DConstants::INVALID_INDEX;
	vector<idx_t> entries;
};

struct DecideDescentPosition {
	//! Its least spelling of the order key over its rows, which a collation lets them differ in: the name a message
	//! gives it (D69). Set by Finish.
	Value order;
	vector<idx_t> entries;
	//! A duplicate position has different payload signatures. This is raised
	//! only if bottom-up selection reads that position (D73).
	bool conflict = false;
};

struct DecideDescentTimeline {
	idx_t slot;
	vector<DecideDescentPosition> positions;
};

struct DecideNodeContainer {
	vector<DecideDescentEntry> entries;
	vector<idx_t> instance_entries;
	vector<DecideDescentGroup> groups;
	vector<DecideDescentTimeline> timelines;
	idx_t rows_in = 0;
	idx_t rows_out = 0;
};

//! Composite traversal, vectorized by selection. A node's when/key expressions
//! execute only on its incoming selection. Bodies and aggregates are deferred.
class DecideDescent {
public:
	DecideDescent(const DecideTree &tree, ClientContext &context);
	void Append(DataChunk &chunk, idx_t row_base);
	void Finish();
	const vector<DecideNodeContainer> &Containers() const {
		return containers;
	}
	vector<DecideNodeContainer> &Containers() {
		return containers;
	}
	//! A solver's existing keyed mapping can supply identity while the evaluators
	//! coexist. The complete tree evaluator resolves it from declaration keys.
	void SetDecisionResolver(std::function<idx_t(idx_t, idx_t)> resolver);
	const vector<vector<DecideDescentGroup>> &DecisionGroups() const {
		return decision_groups;
	}
	const vector<vector<idx_t>> &DecisionRows() const {
		return decision_rows;
	}
	//! The label a message quotes for group `group` of key `key` (D69; flat's GroupKeyReader): a collated column at its
	//! least spelling over every row the query read the key on, as GROUP BY shows a collated group, and any other
	//! column as it groups. Complete once every row is appended.
	vector<Value> Label(idx_t key, const vector<Value> &group) const;
	string Dump() const;
	const DecideErrorLedger &Errors() const {
		return errors;
	}

private:
	struct RoutingRow {
		idx_t row;
		idx_t parent_slot;
	};
	struct NodeIndex {
		unordered_map<hash_t, vector<idx_t>> groups;
		//! A set E's instances by (slot, sigma), and its entries by (slot, sigma, columns, reads).
		unordered_map<hash_t, vector<idx_t>> signatures;
		unordered_map<hash_t, vector<idx_t>> entries;
		//! Shared DAG edges may route the same row/context twice. Its input
		//! value is inserted once, independently of which reader visits first.
		unordered_map<hash_t, vector<pair<idx_t, idx_t>>> seen;
		//! A G's or O's, for the current chunk: where each row it reaches reads its value (DecideDescentEntry::reads).
		unordered_map<idx_t, idx_t> landed;
	};
	//! A group of a collated key and its least spelling so far: a hash aggregate over every read of the key (Key),
	//! whose label Label returns.
	struct KeySpelling {
		vector<Value> group;
		vector<Value> label;
		//! Per column, the sort key of its spelling in `label`; empty for a column that is not collated.
		vector<string> least;
	};
	const DecideTree &tree;
	ClientContext &context;
	vector<DecideNodeContainer> containers;
	vector<NodeIndex> indexes;
	vector<vector<DecideDescentGroup>> decision_groups;
	vector<vector<idx_t>> decision_rows;
	vector<vector<bool>> decision_recorded;
	vector<unordered_map<hash_t, vector<idx_t>>> decision_indexes;
	//! Per key, its groups' spellings; empty for a key with no collated column.
	vector<unordered_map<hash_t, vector<KeySpelling>>> spellings;
	std::function<idx_t(idx_t, idx_t)> decision_resolver;
	bool finished = false;
	DecideErrorLedger errors;
	DecideErrorRank current_rank;
	idx_t active_node = DConstants::INVALID_INDEX;
	idx_t active_row_base = 0;
	vector<unordered_set<idx_t>> failed_rows;
	bool Failed(idx_t row) const {
		return active_node < failed_rows.size() && failed_rows[active_node].count(active_row_base + row);
	}

	void Visit(idx_t node, DataChunk &chunk, idx_t row_base, const vector<RoutingRow> &incoming);
	vector<Value> Evaluate(const Expression &expression, DataChunk &chunk, const vector<RoutingRow> &rows);
	vector<uint8_t> Truth(const Expression &expression, DataChunk &chunk, const vector<RoutingRow> &rows);
	vector<vector<Value>> Key(idx_t key, DataChunk &chunk, const vector<RoutingRow> &rows,
	                          vector<bool> &present);
	void Spell(idx_t key, const vector<vector<Value>> &groups, const vector<vector<Value>> &spelled,
	           const vector<vector<string>> &sort_keys);
	vector<vector<Value>> Signature(const DecideNode &node, DataChunk &chunk,
	                                const vector<RoutingRow> &rows, idx_t row_base);
	idx_t FindGroup(idx_t node, idx_t parent, vector<Value> key, idx_t row);
	idx_t FindDecisionGroup(idx_t scope, vector<Value> key, idx_t row);
	vector<idx_t> DecisionClasses(idx_t variable, DataChunk &chunk, const vector<RoutingRow> &rows, idx_t row_base);
	void Insert(idx_t node, DecideDescentEntry entry, bool distinct);
	idx_t FindInstance(idx_t node, hash_t variant, const DecideDescentEntry &entry);
	//! A set E's entries of the current chunk, inserted once every root has descended (DecideDescentEntry::reads).
	vector<pair<idx_t, DecideDescentEntry>> pending;
};

} // namespace duckdb
