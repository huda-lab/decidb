#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/optimizer/decide/direct/direct_result_boundary.hpp"

namespace duckdb {

class LogicalOperator;
class Optimizer;

//! Direct solve checks every input row before it releases one, so its plan holds the rows, and any wide column a
//! parent reads is held with them. When the DECIDE source is a stored table, a column the rule never uses can instead
//! be read from the table a second time and joined to the answers on the row id, so it never enters the hold.
//!
//! Step one, at rewrite time, before the rule builds its plan: if the source is a chain of non-volatile projections
//! and filters over one scan of a DuckDB base table, append that table's row id to the source as a trailing column and
//! record which source columns forward a stored column unchanged. The rule passes the row id along like any other
//! source column. Returns false and leaves the source untouched for any other source (a join, a subquery with an
//! aggregate or a limit, a volatile expression, a table function).
bool DirectExposeStoredRowId(LogicalOperator &source, DirectSecondRead &second_read);

//! Step two, once unused columns are removed and the parents' reads are known: for every result boundary whose parent
//! reads a stored column that nothing else in the rule's plan uses, read that column from a second scan of the table
//! joined to the plan on the row id, then prune again so the column leaves the rule's plan. A boundary whose row id is
//! not needed lets go of it. Knows nothing about any rule.
unique_ptr<LogicalOperator> ApplyDirectSecondRead(unique_ptr<LogicalOperator> plan, Optimizer &optimizer);

} // namespace duckdb
