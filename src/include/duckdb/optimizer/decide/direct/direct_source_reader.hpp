#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/planner/column_binding.hpp"
#include "duckdb/planner/logical_operator.hpp"

namespace duckdb {

class LogicalGet;
class Optimizer;

//! A rule whose plan needs the DECIDE source more than once (one read per stage) gets each read from here. This is the
//! recompute-or-materialize choice DuckDB makes for a CTE, made once for the source:
//!
//!  - a source that is a chain of non-volatile projections and filters over one scan of a DuckDB base table
//!    (DirectRepeatableSourceChain) is recomputed: every read is a copy of the chain with fresh table indexes, so each
//!    read is a parallel scan and no copy of the rows is kept (the Prototype pattern);
//!  - any other source is materialized once, as the definition of a LogicalMaterializedCTE, and every read is a
//!    LogicalCTERef to it.
//!
//! Both kinds of read return the same rows in the same column order. Nothing may depend on the order of the rows.
class DirectSourceReader {
public:
	//! One read of the source: a projection over it that forwards every source column in source order, so column `i` is
	//! the binding (`table_index`, `i`). The source's own bindings are gone, so an expression written over them is
	//! re-pointed with DirectOverProjection(expression, SourceBindings(), table_index).
	struct Read {
		unique_ptr<LogicalOperator> plan;
		idx_t table_index = DConstants::INVALID_INDEX;
	};

	//! `source` is the DECIDE input with its types resolved. The reader owns it from here on.
	DirectSourceReader(Optimizer &optimizer, unique_ptr<LogicalOperator> source);

	//! The bindings of the source columns, which the proved expressions refer to.
	const vector<ColumnBinding> &SourceBindings() const {
		return bindings;
	}
	const vector<LogicalType> &SourceTypes() const {
		return types;
	}
	//! A new read of the source.
	Read ReadSource();

	//! The plan built from the reads. A materialized source becomes the definition of a LogicalMaterializedCTE whose
	//! child is `plan`, which is the root every read needs above it; a recomputed source needs nothing, and `plan`
	//! comes back as it is.
	unique_ptr<LogicalOperator> Finish(unique_ptr<LogicalOperator> plan);

private:
	Read ForwardAllColumns(unique_ptr<LogicalOperator> top);
	unique_ptr<LogicalOperator> CopyOfScan() const;
	unique_ptr<LogicalOperator> CopyOfChain() const;

	Optimizer &optimizer;
	unique_ptr<LogicalOperator> source;
	vector<ColumnBinding> bindings;
	vector<LogicalType> types;
	//! The chain of a recomputed source (the source first, the scan excluded) and its scan; empty and null otherwise.
	vector<reference<LogicalOperator>> chain;
	optional_ptr<LogicalGet> chain_scan;
	//! The table index of the materialized CTE, for a source that is not a chain.
	idx_t cte_index = DConstants::INVALID_INDEX;
};

} // namespace duckdb
