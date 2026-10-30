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
//! whether the copy fits in `memory` (DuckDB's estimate of the rows times the physical width of one row of the copy,
//! within a fixed share of it). A source with no rows, or no estimate (0), is re-scanned.
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
//! A reader can carry side tables (AddSideTable): the value of an aggregate per group of the rows, which the bounds of
//! the clauses read as one more column of the source. Every read, the source itself included, joins them back to its
//! rows by the group key, so the source stays what it was and a repeatable chain stays repeatable. This is how DuckDB
//! decorrelates a scalar subquery: a grouped aggregate over a second read, joined back by the correlation key.
//!
//! A reader can be gated (Gate) by a verdict on the rows, the scope checks', after which every read passes its rows
//! only once the verdict holds. Gates chain: a later verdict is computed from reads the earlier one gates, so the
//! order of the obligations (row checks, then a rule's own verdict, then the rows) is a data dependency.
class DirectSourceReader {
public:
	//! One read of the source: a projection over it that forwards every source column in source order, then the value
	//! column of every side table in the order they were added, so column `i` is the binding (`table_index`, `i`). The
	//! source's own bindings are gone, so an expression written over them is re-pointed with
	//! DirectOverProjection(expression, SourceBindings(), table_index).
	struct Read {
		unique_ptr<LogicalOperator> plan;
		idx_t table_index = DConstants::INVALID_INDEX;
	};

	//! `source` is the DECIDE input with its types resolved. The reader owns it from here on. `reads` is how many times
	//! the plan reads it: the rule's reads, the side tables' and the checks'. `stored_columns` is
	//! DirectLateMaterialization::stored_columns of the source, empty when the source carries no row id: a copy of the
	//! source leaves out the variable-width stored columns the node does not read, because late materialization reads
	//! them again by row id, so a copy's width is the other columns' and the row id's.
	DirectSourceReader(Optimizer &optimizer, unique_ptr<LogicalOperator> source, idx_t reads,
	                   const vector<idx_t> &stored_columns);

	//! The bindings of the source columns, then the placeholder of every side table added so far, which the proved
	//! expressions refer to. These are the columns a read has. The DECIDE node's own outputs are the source columns
	//! alone.
	const vector<ColumnBinding> &SourceBindings() const {
		return bindings;
	}
	const vector<LogicalType> &SourceTypes() const {
		return types;
	}
	//! Whether the source is materialized once. The copy holds every row the plan reads, so a plan over it holds rows
	//! even when the rule's own plan streams.
	bool Materialized() const {
		return materialized;
	}
	//! The source, until TakeRecomputedSource or Finish takes it.
	LogicalOperator &Source() {
		D_ASSERT(source);
		return *source;
	}
	//! A new read of the source.
	Read ReadSource();
	//! The source itself, as the last read of a source that is not materialized (recomputed, or read once): that read
	//! keeps the source's own bindings, and the side tables' values keep their placeholders. Null for a materialized
	//! source, every read of which has new bindings. The reader reads nothing after it.
	unique_ptr<LogicalOperator> TakeRecomputedSource();

	//! Adds a side table: `definition` is a plan of one row per group whose columns are the group key's, then the one
	//! value. `keys` are the key's expressions over the source's columns (none for one group); a read joins the table
	//! to its rows on them with IS NOT DISTINCT FROM, so a NULL key is a group. The value is the column `placeholder`,
	//! which the clauses' expressions read; `placeholder.column_index` is the number of key columns. Every group of a
	//! row is in the table, because `definition` reads the source's rows. A table's definition may read the tables
	//! added before it, not those after. The reader owns the table from here on.
	void AddSideTable(unique_ptr<LogicalOperator> definition, vector<unique_ptr<Expression>> keys,
	                  ColumnBinding placeholder);

	//! Gates every later read on `verdict`, a plan of one row whose one BOOLEAN column is true or raises. A verdict
	//! built from reads taken before the gate is gated by the verdicts before it; one added later reads only what the
	//! newest verdict passed. The verdict is the definition of a one-row materialized CTE, and a gated read is
	//! Filter[v](CrossProduct(rows, CTERef v)) on the newest one, under the read's forwarding projection: it keeps the
	//! rows' bindings and passes a row only once the CTE is computed, whichever side of the product is built first.
	void Gate(unique_ptr<LogicalOperator> verdict);

	//! The plan built from the reads. A gate's verdict becomes the definition of a LogicalMaterializedCTE whose child
	//! is `plan`, the first outermost, and each side table that of one outside all of them, the first outermost. A
	//! materialized source becomes the definition of a LogicalMaterializedCTE outside all of them, the root every read
	//! needs above it; a recomputed source needs nothing.
	unique_ptr<LogicalOperator> Finish(unique_ptr<LogicalOperator> plan);

private:
	//! A side table: its definition until Finish takes it, the key as the source's columns have it, the types of its
	//! columns, the index of its CTE, and the table index of its placeholder.
	struct SideTable {
		unique_ptr<LogicalOperator> definition;
		vector<unique_ptr<Expression>> keys;
		vector<LogicalType> types;
		idx_t cte_index;
		idx_t ref_index;
	};

	Read ForwardAllColumns(unique_ptr<LogicalOperator> top, const vector<ColumnBinding> &side_values);
	//! `top`, whose first columns are the source's, joined to a reference to every side table. The values are
	//! appended to `side_values`. With `own_bindings` the references carry the tables' placeholders.
	unique_ptr<LogicalOperator> JoinSideTables(unique_ptr<LogicalOperator> top, bool own_bindings,
	                                           vector<ColumnBinding> &side_values);
	unique_ptr<LogicalOperator> Gated(unique_ptr<LogicalOperator> read);
	unique_ptr<LogicalOperator> CopyOfScan() const;
	unique_ptr<LogicalOperator> CopyOfChain() const;

	Optimizer &optimizer;
	unique_ptr<LogicalOperator> source;
	idx_t reads;
	//! The source's columns, then the side tables' values; `source_width` of them are the source's.
	vector<ColumnBinding> bindings;
	vector<LogicalType> types;
	idx_t source_width;
	vector<SideTable> side_tables;
	//! The chain of a recomputed source (the source first, the scan excluded) and its scan; empty and null otherwise.
	vector<reference<LogicalOperator>> chain;
	optional_ptr<LogicalGet> chain_scan;
	//! Whether the source is materialized, and the table index of its CTE.
	bool materialized = false;
	idx_t cte_index = DConstants::INVALID_INDEX;
	//! A gate's verdict until Finish takes it, and the table index of its CTE.
	struct Verdict {
		unique_ptr<LogicalOperator> definition;
		idx_t cte_index = DConstants::INVALID_INDEX;
	};
	//! The gates, oldest first; none when ungated.
	vector<Verdict> verdicts;
};

} // namespace duckdb
