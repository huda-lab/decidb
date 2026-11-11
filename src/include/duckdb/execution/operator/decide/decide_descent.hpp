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
#include "duckdb/common/types/string_heap.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/execution/operator/decide/decide_key_map.hpp"
#include "duckdb/planner/decide/decide_tree.hpp"

#include <set>

namespace duckdb {

//! The order of a key's groups, value by value with NULL after every value, as ORDER BY places it and flat numbers a
//! key's groups (NumberGroupsByValue), so a group's rank is flat's group number.
bool DecideDescentKeyLess(const vector<Value> &left, const vector<Value> &right);

//! A group's least spelling of each collated column of its key, as `min` folds a group's values (D69): its label, a
//! column not collated as it groups, and per column the sort key of the spelling the label holds.
struct DecideSpelling {
	vector<Value> label;
	vector<string> least;
	//! Folds row `i` of `spelled`, the columns' spellings on rows of the group whose key values are `group`, by their
	//! sort keys `sort_keys`, empty for a column not collated.
	void Fold(const vector<Value> &group, const vector<vector<Value>> &spelled, const vector<vector<string>> &sort_keys,
	          idx_t i);
};

//! Columns of values, a row per entry or slot, in chunks of STANDARD_VECTOR_SIZE rows as DuckDB's ColumnDataCollection
//! chunks its rows, but held in memory so that a row is read by its index and a window of rows is one chunk, which an
//! executor reads in place (DecideTreeColumns).
class DecideColumns {
public:
	void Initialize(Allocator &allocator, vector<LogicalType> types);
	const vector<LogicalType> &Types() const {
		return types;
	}
	idx_t Count() const {
		return count;
	}
	//! Copies rows `selection[0, count)` of `source`'s first Types().size() columns after the last row, a string into
	//! the chunk's own heap: no row refers to `source` afterwards.
	void Append(DataChunk &source, const SelectionVector &selection, idx_t count);
	Value GetValue(idx_t column, idx_t row) const;
	//! The values of row `row`'s columns `begin` to `end`.
	vector<Value> GetValues(idx_t row, idx_t begin, idx_t end) const;
	//! The rows from `window * STANDARD_VECTOR_SIZE` on, at most STANDARD_VECTOR_SIZE of them.
	DataChunk &Window(idx_t window) const {
		return *chunks[window];
	}
	idx_t Windows() const {
		return chunks.size();
	}

private:
	Allocator *allocator = nullptr;
	vector<LogicalType> types;
	vector<unique_ptr<DataChunk>> chunks;
	idx_t count = 0;
};

//! A node's slots (language/guide.md §6.1), or a keyed scope's classes: per slot the G or O slot an E's class lies in
//! (INVALID for a G's or O's group, whose key is its whole path key), its first row, its rank by value, and the
//! values its rows group on, a collated column under its collation, which a message quotes through Label.
struct DecideNodeGroups {
	unsafe_vector<idx_t> parent_slots;
	unsafe_vector<idx_t> first_rows;
	//! Value order, of the enclosing slot and then the key, assigned by Finish (flat's NumberGroupsByValue).
	unsafe_vector<idx_t> ranks;
	//! False for a slot of rows in no class of the key: a root's (DecideNode::holds_rows_in_no_class), and a G's, whose
	//! rows reach its operand alone and hold no entry of it. Empty for a node per row.
	vector<bool> in_a_class;
	DecideColumns keys;
	//! A collated key's least spelling over the rows its node holds in each slot, which flat names a variant of the
	//! class by; Label's is the least over every read of the key. Empty for a key with no collated
	//! column.
	vector<DecideSpelling> spellings;
	//! A node per row's slots are its entries, each a class of its own keyed by its row: none is held here, and no
	//! reader numbers them.
	bool per_row = false;
	idx_t per_row_slots = 0;

	idx_t size() const {
		return per_row ? per_row_slots : parent_slots.size();
	}
	bool InAClass(idx_t slot) const {
		return per_row || in_a_class[slot];
	}
	vector<Value> Key(idx_t slot) const;
	const DecideSpelling &Spelling(idx_t slot) const;
};

//! An O's timelines (language/guide.md §5.6), in its slots' rank order: per timeline its slot and its positions, per
//! position its entries, by row, the entry whose order value names it -- its least spelling over its rows, which a
//! collation lets them differ in (D69) -- and whether its entries' payload signatures differ, which is raised only if
//! bottom-up selection reads that position (D73). Set by Finish.
struct DecideNodeTimelines {
	vector<idx_t> slots;
	//! Per timeline, its first position; one more for the end.
	vector<idx_t> first_position;
	//! Per position, its first entry in `entries`; one more for the end.
	vector<idx_t> first_entry;
	vector<idx_t> entries;
	vector<idx_t> named_by;
	vector<bool> conflict;

	idx_t size() const {
		return slots.size();
	}
	idx_t Positions() const {
		return named_by.size();
	}
};

//! Where an entry's values lie among its node's columns (DecideNodeContainer::values): its signature's values
//! (DecideDescent::Signature), its carried columns, its program columns, and an O's order value, its comparison bytes,
//! its D75 partition's values -- which can be wider than its timeline -- and whether a class of it holds the entry.
struct DecideEntryLayout {
	//! Per value of the signature, its column, or INVALID for a decision's of an entry that stands for its row alone,
	//! which its row gives (DecideDescent::Signature): the decision and its instance there.
	vector<idx_t> signature;
	idx_t columns = 0;
	idx_t program_columns = 0;
	idx_t order = DConstants::INVALID_INDEX;
	idx_t order_key = DConstants::INVALID_INDEX;
	idx_t partition = DConstants::INVALID_INDEX;
	idx_t partition_present = DConstants::INVALID_INDEX;
	idx_t end = 0;
};

//! A node's entries as columns (language/guide.md §6.1): per entry its row, the enclosing reducer or timeline slot
//! (E's per never changes it), its own slot -- an E instance, a reducer group or a frame timeline -- and its instance,
//! its distinct sigma within the class, which several entries of a set E can share; and its values.
struct DecideNodeContainer {
	unsafe_vector<idx_t> rows;
	unsafe_vector<idx_t> parent_slots;
	//! Per entry its slot, read through Slot: empty for a node per row, whose entry is its own slot
	//! (DecideNodeGroups::per_row).
	unsafe_vector<idx_t> slots;
	//! Per entry its instance and per instance its first entry, read through Instance and Instances: held for a set E
	//! alone, whose entries can share an instance; any other node's entry is an instance of its own.
	bool shares_instances = false;
	unsafe_vector<idx_t> instances;
	unsafe_vector<idx_t> instance_entries;
	DecideColumns values;
	DecideEntryLayout layout;
	//! A set E keeps one entry per distinct slot, signature, carried columns and reads -- where its row reads each node
	//! it depends on: a G's group, an O's timeline, or the row where the O has an `else` (D88) -- at least one per
	//! variant and fewer than its rows (DecideDescent::InsertDistinct): each row it keeps, with the entry standing for
	//! it, a pair per row for every set E, though only the objective's root reads them yet. Empty for a node per row,
	//! whose entries are its rows.
	unsafe_vector<pair<idx_t, idx_t>> routed;
	DecideNodeGroups groups;
	DecideNodeTimelines timelines;
	idx_t rows_in = 0;
	idx_t rows_out = 0;

	idx_t Count() const {
		return rows.size();
	}
	idx_t Slot(idx_t entry) const {
		return groups.per_row ? entry : slots[entry];
	}
	idx_t Instance(idx_t entry) const {
		return shares_instances ? instances[entry] : entry;
	}
	idx_t Instances() const {
		return shares_instances ? instance_entries.size() : Count();
	}
	vector<Value> Columns(idx_t entry) const {
		return values.GetValues(entry, layout.columns, layout.program_columns);
	}
	Value Order(idx_t entry) const {
		return values.GetValue(layout.order, entry);
	}
};

//! Composite traversal, vectorized by selection: a node's `when` parts, keys and signature fields execute only on the
//! rows reaching it, a window of the input at a time, and each key's rows find their slots in a DecideKeyMap. Bodies
//! and aggregates are deferred.
class DecideDescent {
public:
	DecideDescent(const DecideTree &tree, ClientContext &context);
	~DecideDescent();
	void Append(DataChunk &chunk, idx_t row_base);
	void Finish();
	const vector<DecideNodeContainer> &Containers() const {
		return containers;
	}
	vector<DecideNodeContainer> &Containers() {
		return containers;
	}
	const vector<DecideNodeGroups> &DecisionGroups() const {
		return decision_groups;
	}
	const vector<vector<idx_t>> &DecisionRows() const {
		return decision_rows;
	}
	//! Entry `entry` of `node`'s signature values (Signature), a decision's instance read off its row where the entry
	//! holds none.
	vector<Value> Signature(idx_t node, idx_t entry) const;
	//! The label a message quotes for group `group` of key `key` (D69; flat's GroupKeyReader): a collated column at its
	//! least spelling over every row the query read the key on, as GROUP BY shows a collated group, and any other
	//! column as it groups. Complete once every row is appended.
	vector<Value> Label(idx_t key, const vector<Value> &group) const;
	string Dump() const;
	const DecideErrorLedger &Errors() const {
		return errors;
	}

private:
	//! The rows of a window reaching a node: each row of the window, and the enclosing slot it arrives in.
	struct Reach {
		unsafe_vector<sel_t> rows;
		unsafe_vector<idx_t> parents;
		idx_t size() const {
			return rows.size();
		}
		bool empty() const {
			return rows.empty();
		}
		void Add(sel_t row, idx_t parent) {
			rows.push_back(row);
			parents.push_back(parent);
		}
		void Reserve(idx_t count) {
			rows.reserve(count);
			parents.reserve(count);
		}
	};
	//! A group of a collated key and its least spelling so far: a hash aggregate over every read of the key (Key),
	//! whose label Label returns.
	struct KeySpelling {
		vector<Value> group;
		DecideSpelling spelling;
	};
	//! What a read of a collated key spelled, per column and row, and the sort keys of those spellings: empty for a
	//! column that is not collated.
	struct KeySpellings {
		vector<vector<Value>> spelled;
		vector<vector<string>> sort_keys;
	};
	//! A keyed scope's classes, one dictionary per layout its keys come in (DecisionClasses), all numbered in one
	//! count: a declaration's key, a definition's key and signature.
	struct DecisionLayout {
		vector<LogicalType> types;
		unique_ptr<DecideKeyMap> map;
		//! The one class of an empty layout.
		idx_t empty_slot = DConstants::INVALID_INDEX;
	};
	//! A set E's entries of a window, inserted once every root has descended (DecideNodeContainer::routed): its rows,
	//! their enclosing slots and their own, and their signature, carried and program columns.
	struct Pending {
		idx_t node;
		Reach rows;
		unsafe_vector<idx_t> slots;
		DataChunk values;
	};
	//! A key's values on the window's rows read so far: a key is evaluated once on a row of the window, by the first
	//! node that reads it there, and every later read of it on the row slices these. A row a read raised on is not
	//! held, so each node reading it raises at its own rank.
	struct KeyReads {
		DataChunk values;
		KeySpellings spelled;
		//! Whether `values` holds every row of the window at its own place, as a read of the whole window leaves it.
		bool whole = false;
		//! Otherwise, per row of the window, where `values` holds it; INVALID where no read holds it.
		unsafe_vector<idx_t> position;

		void Initialize(Allocator &allocator, const vector<LogicalType> &types, idx_t columns, idx_t window);
		//! Holds nothing, for the window of `window` rows that starts.
		void Reset(idx_t window);
		//! A copy that holds what this one holds.
		void CopyInto(Allocator &allocator, KeyReads &copy, idx_t room) const;
		//! The rows of `rows` this does not hold, `rows` itself where it holds none.
		const Reach &Missing(const Reach &rows, Reach &own) const;
		//! Holds the values and spellings `read` and `spelled` evaluated on `missing`, of a window of `window` rows.
		void Hold(DataChunk &read, const KeySpellings &spelled, const Reach &missing, idx_t window);
		//! Where row `i` of `rows` lies among the held values.
		idx_t At(const Reach &rows, idx_t i) const {
			return whole ? rows.rows[i] : position[rows.rows[i]];
		}
		//! The values of `rows`, in place where they are the window's.
		void Slice(const Reach &rows, idx_t window, DataChunk &result) const;
	};
	//! A key's dictionary, which every node of the key outside an enclosing slot shares (a Flyweight): its slots are
	//! numbered across those nodes, and each node numbers its own (own_slots) in the order its rows reach them. Each
	//! row of the window keeps the slot it found, so a row probes the dictionary once.
	struct SharedDictionary {
		unique_ptr<DecideKeyMap> map;
		idx_t count = 0;
		unsafe_vector<idx_t> window_slots;
	};
	const DecideTree &tree;
	ClientContext &context;
	vector<DecideNodeContainer> containers;
	//! Per node in an enclosing slot, the dictionary of its (enclosing slot, key); null for a node per row, whose rows
	//! are each a slot of their own, for an empty key, whose slots its enclosing slots find (empty_key_slots), and for
	//! a node outside an enclosing slot, which reads its key's shared dictionary.
	vector<unique_ptr<DecideKeyMap>> group_maps;
	//! Per key, the dictionary its nodes outside an enclosing slot share; per node of one, its slot by the shared one,
	//! INVALID where it has none.
	vector<unique_ptr<SharedDictionary>> dictionaries;
	vector<unsafe_vector<idx_t>> own_slots;
	//! Per key, its values on the window's rows read so far.
	vector<KeyReads> key_reads;
	//! How many reads raised: a read that raised on a row holds none of its values.
	idx_t raised = 0;
	//! The slots HoldKeyed finds for a node's rows, until its children are visited.
	unsafe_vector<idx_t> slot_buffer;
	//! Per node of an empty key, its slot by enclosing slot plus one, 0 for none: INVALID where none is made yet.
	vector<unsafe_vector<idx_t>> empty_key_slots;
	//! Per set E, its entries by (slot, signature, columns, reads) and its instances by (slot, signature).
	vector<unique_ptr<DecideKeyMap>> entry_maps;
	vector<unique_ptr<DecideKeyMap>> instance_maps;
	//! Per node, the types of its signature's values (Signature).
	vector<vector<LogicalType>> signature_types;
	//! Per node, whether more than one path of the tree reaches it: only then may a window's row arrive twice with one
	//! enclosing slot, and only its first arrival is read.
	vector<bool> shared;
	//! Per node, whether its rows can arrive in an enclosing slot: an E some node routes rows to. Only then is the
	//! slot part of its key's dictionary.
	vector<bool> enclosed;
	//! Per shared node and window, the (row, enclosing slot) pairs that arrived.
	vector<std::set<pair<idx_t, idx_t>>> seen;
	//! Per G or O and window: where each row it reaches reads its value (DecideNodeContainer::routed), INVALID where it
	//! reaches none.
	vector<vector<idx_t>> landed;
	vector<DecideNodeGroups> decision_groups;
	//! Per keyed scope, each class's sort key (Finish ranks them), held in `decision_heap`.
	vector<vector<string_t>> decision_sort_keys;
	StringHeap decision_heap;
	vector<vector<idx_t>> decision_rows;
	vector<vector<bool>> decision_recorded;
	vector<vector<DecisionLayout>> decision_layouts;
	//! Per key, its groups' spellings; empty for a key with no collated column.
	vector<unordered_map<hash_t, vector<KeySpelling>>> spellings;
	//! One executor per expression the descent reads, kept across windows as DuckDB's operators keep theirs.
	unordered_map<const Expression *, unique_ptr<ExpressionExecutor>> executors;
	bool finished = false;
	DecideErrorLedger errors;
	DecideErrorRank current_rank;
	idx_t active_node = DConstants::INVALID_INDEX;
	idx_t active_row_base = 0;
	//! The block of flat's in which the visit's read ranks, by the row's window: a reducer's own `when` in the term
	//! filters, a grouped clause's key in its grouping; ROUTE for a read that routes the node's rows.
	DecideBlock read_block = DecideBlock::ROUTE;
	vector<unordered_set<idx_t>> failed_rows;
	//! The active node's failed rows, null before any node is visited.
	const unordered_set<idx_t> *active_failed = nullptr;
	bool Failed(idx_t row) const {
		return active_failed && !active_failed->empty() && active_failed->count(active_row_base + row);
	}
	//! A set E's entries of the current window, inserted once every root has descended (DecideNodeContainer::routed).
	vector<unique_ptr<Pending>> pending;

	void Visit(idx_t node, DataChunk &chunk, idx_t row_base, const Reach &incoming);
	//! The rows of `reaching` each `when` part of `node` keeps, in `own` unless none is written: null once none is
	//! left.
	const Reach *Gate(const DecideNode &node, DataChunk &chunk, const Reach *reaching, Reach &own);
	//! What a keyed node holds of `rows`: each row's slot by its key and the entries of those its key classes, and the
	//! rows its children read, in `own_children` for a G's or O's, which reach them in their slots.
	const Reach &HoldKeyed(idx_t node, DataChunk &chunk, const Reach &rows, idx_t row_base, Reach &own_children);
	//! `expression` on `rows` into `result`, a vector of its type; a row it fails on is NULL, its error held at the
	//! read's rank.
	void Evaluate(const Expression &expression, DataChunk &chunk, const Reach &rows, Vector &result);
	vector<uint8_t> Truth(const Expression &expression, DataChunk &chunk, const Reach &rows);
	//! The values of `key` on `rows`, a column per element into `values`, and which hold no NULL that leaves a row in
	//! no group; with `read`, what a collated key spelled, left empty for a key with no collated column.
	void Key(idx_t key, DataChunk &chunk, const Reach &rows, DataChunk &values, unsafe_vector<bool> &present,
	         KeySpellings *read = nullptr);
	//! The values of `key` on `missing`, the rows of `rows` it holds no read of, into `values`, a collated column's
	//! spellings into `spelled`; with none missing, the ranks advance as if `rows` were read.
	void ReadKey(idx_t key, DataChunk &chunk, const Reach &rows, const Reach &missing, DataChunk &values,
	             KeySpellings &spelled);
	void Spell(idx_t key, const vector<vector<Value>> &groups, const vector<vector<Value>> &spelled,
	           const vector<vector<string>> &sort_keys);
	//! `node`'s signature on `rows`, its values' columns (signature_types) into `values`.
	void Signature(idx_t node, DataChunk &chunk, const Reach &rows, idx_t row_base, DataChunk &values);
	//! The slot each of `rows` lies in, `selected` of them by their key `keys`, created in row order.
	void FindGroups(idx_t node, const Reach &rows, const unsafe_vector<idx_t> &selected, DataChunk &keys,
	                const unsafe_vector<bool> &present, idx_t row_base, unsafe_vector<idx_t> &slots);
	//! The slots of `selected` of `rows` for a node outside an enclosing slot, by its key `keys` (aligned with `rows`)
	//! in the key's shared dictionary, each the node's own (`found`, aligned with `selected`).
	void FindShared(idx_t node, const Reach &rows, const unsafe_vector<idx_t> &selected, DataChunk &keys,
	                unsafe_vector<idx_t> &found);
	//! Of a keyed scope, the class of each row of `keys` (its layout's columns), INVALID where `present` is false.
	void FindDecisionGroups(idx_t scope, DataChunk &keys, const unsafe_vector<bool> &present, const vector<idx_t> &rows,
	                        vector<idx_t> &classes);
	vector<idx_t> DecisionClasses(idx_t variable, DataChunk &chunk, const Reach &rows, idx_t row_base);
	//! The scope decision `variable`'s instances lie in, INVALID for one not keyed, and whether it is query-wide.
	idx_t DecisionScope(idx_t variable, bool &global) const;
	//! A set E's window of entries once every root has descended: one entry per distinct slot, signature, columns
	//! and reads.
	void InsertDistinct(Pending &batch, idx_t row_base);
	//! What `node` holds of its `stored` rows, each in its slot of `slots`: the classes of the keyed decisions it
	//! reads, and an entry per row no read failed on, its signature, carried and program columns and an O's order;
	//! a set E's wait for every root (pending).
	void Store(idx_t node, DataChunk &chunk, const Reach &stored, const unsafe_vector<idx_t> &slots, idx_t row_base);
	//! An O's order value on `stored`, its comparison bytes and its partition, into `values` at `layout`.
	void FrameOrder(const DecideNode &node, const DecideEntryLayout &layout, DataChunk &chunk, const Reach &stored,
	                DataChunk &values);
	//! An O's timelines (DecideNodeTimelines), once every row is appended and its slots are ranked.
	void Timelines(idx_t node);
	//! `rows` without those a read failed on: `rows` itself where none failed, else `kept`.
	const Reach &Unfailed(const Reach &rows, Reach &kept) const;
};

} // namespace duckdb
