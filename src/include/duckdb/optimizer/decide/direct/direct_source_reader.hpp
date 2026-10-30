#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/planner/column_binding.hpp"
#include "duckdb/planner/logical_operator.hpp"

namespace duckdb {

class LogicalGet;
class Optimizer;

//! Whether one materialized copy of a source that could be re-scanned is cheaper than `reads` re-scans of it.
//! Re-scanning and copying both scale with the size of the rows, so the comparison is of `reads` alone: the copy wins
//! when reading it often enough pays for writing it (a constant measured on R1). The size of the rows decides only
//! whether the copy fits in `memory` (DuckDB's estimate of the rows times the physical width of one row, within a fixed
//! share of it). A source with no rows, or no estimate (0), is re-scanned.
bool DirectSourceCopyIsCheaper(idx_t reads, idx_t rows, idx_t row_width, idx_t memory);

//! A rule whose plan needs the DECIDE source more than once (one read per stage) gets each read from here. This is the
//! recompute-or-materialize choice DuckDB makes for a CTE, made once for the source:
//!
//!  - a source that is a chain of non-volatile projections and filters over one scan of a DuckDB base table
//!    (DirectRepeatableSourceChain) can be recomputed: every read is a copy of the chain with fresh table indexes, so
//!    each read is a parallel scan and no copy of the rows is kept (the Prototype pattern). It is recomputed unless a
//!    copy is cheaper (CopyIsCheaper: the plan reads the source often enough and the copy fits in memory);
//!  - any other source is materialized once, as the definition of a LogicalMaterializedCTE, and every read is a
//!    LogicalCTERef to it; read only once, it is read as itself, as DuckDB inlines a CTE referenced once.
//!
//! Both kinds of read return the same rows in the same column order. Nothing may depend on the order of the rows.
//!
//! A reader can be gated (Gate) by a verdict on the rows, the scope checks', after which every read passes its rows
//! only once the verdict holds.
class DirectSourceReader {
public:
	//! One read of the source: a projection over it that forwards every source column in source order, so column `i` is
	//! the binding (`table_index`, `i`). The source's own bindings are gone, so an expression written over them is
	//! re-pointed with DirectOverProjection(expression, SourceBindings(), table_index).
	struct Read {
		unique_ptr<LogicalOperator> plan;
		idx_t table_index = DConstants::INVALID_INDEX;
	};

	//! `source` is the DECIDE input with its types resolved. The reader owns it from here on. `reads` is how many times
	//! the plan reads it: the rule's reads and the checks'.
	DirectSourceReader(Optimizer &optimizer, unique_ptr<LogicalOperator> source, idx_t reads);

	//! The bindings of the source columns, which the proved expressions refer to.
	const vector<ColumnBinding> &SourceBindings() const {
		return bindings;
	}
	const vector<LogicalType> &SourceTypes() const {
		return types;
	}
	//! The source, until TakeRecomputedSource or Finish takes it.
	LogicalOperator &Source() {
		D_ASSERT(source);
		return *source;
	}
	//! A new read of the source.
	Read ReadSource();
	//! The source itself, as the last read of a source that is not materialized (recomputed, or read once): that read
	//! keeps the source's own bindings. Null for a materialized source, every read of which has new bindings. The
	//! reader reads nothing after it.
	unique_ptr<LogicalOperator> TakeRecomputedSource();

	//! Gates every later read on `verdict`, a plan of one row whose one BOOLEAN column is true or raises.
	//! The verdict is the definition of a one-row materialized CTE, and a gated read is
	//! Filter[v, projection_map = the read's columns](CrossProduct(read, CTERef v)): it keeps the read's bindings and
	//! passes a row only once the CTE is computed, whichever side of the product is built first.
	void Gate(unique_ptr<LogicalOperator> verdict);

	//! The plan built from the reads. A gate's verdict becomes the definition of a LogicalMaterializedCTE whose child
	//! is `plan`. A materialized source becomes the definition of a LogicalMaterializedCTE outside that, the root every
	//! read needs above it; a recomputed source needs nothing.
	unique_ptr<LogicalOperator> Finish(unique_ptr<LogicalOperator> plan);

private:
	Read ForwardAllColumns(unique_ptr<LogicalOperator> top);
	unique_ptr<LogicalOperator> Gated(unique_ptr<LogicalOperator> read);
	unique_ptr<LogicalOperator> CopyOfScan() const;
	unique_ptr<LogicalOperator> CopyOfChain() const;

	Optimizer &optimizer;
	unique_ptr<LogicalOperator> source;
	idx_t reads;
	vector<ColumnBinding> bindings;
	vector<LogicalType> types;
	//! The chain of a recomputed source (the source first, the scan excluded) and its scan; empty and null otherwise.
	vector<reference<LogicalOperator>> chain;
	optional_ptr<LogicalGet> chain_scan;
	//! Whether the source is materialized, and the table index of its CTE.
	bool materialized = false;
	idx_t cte_index = DConstants::INVALID_INDEX;
	//! The gate's verdict, until Finish takes it, and the table index of its CTE; null and INVALID_INDEX ungated.
	unique_ptr<LogicalOperator> verdict;
	idx_t verdict_index = DConstants::INVALID_INDEX;
};

} // namespace duckdb
