//===----------------------------------------------------------------------===//
//                         DeciDB
//
// duckdb/execution/operator/decide/decide_key_map.hpp
//
// A key's dictionary: its values to dense slots, on DuckDB's GROUP BY hash table.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/aggregate_hashtable.hpp"

namespace duckdb {

//! Key -> dense slot (language/guide.md §6.1): DuckDB's GROUP BY hash table (GroupedAggregateHashTable) with no
//! aggregate but one register in each group's row, which holds the group's slot. A lookup is the table's vectorized
//! probe, and values compare as GROUP BY compares them: NULL equal to NULL, a collated column under its collation
//! when the caller passes its grouping form.
class DecideKeyMap {
public:
	//! `types`, at least one: an empty key has one slot, which needs no table.
	DecideKeyMap(ClientContext &context, const vector<LogicalType> &types);

	//! The slot of each row of `keys`: a key no earlier row read takes `next`, which then counts up, so slots number
	//! the keys in the order rows first read them. Slots outside this table's own (a scope whose keys come in several
	//! layouts) share one count through `next`.
	void FindOrCreate(DataChunk &keys, idx_t *slots, idx_t &next);

private:
	GroupedAggregateHashTable table;
	Vector addresses;
	SelectionVector created;
	//! Where the register lies in a group's row.
	idx_t offset;
};

} // namespace duckdb
