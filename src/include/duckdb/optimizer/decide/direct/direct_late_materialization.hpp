#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/optimizer/decide/direct/direct_result_adapter.hpp"
#include "duckdb/planner/column_binding_map.hpp"

namespace duckdb {

class ClientContext;
class LogicalGet;
class LogicalOperator;
class Optimizer;

//! Direct solve checks every input row before it releases one, so its plan holds the rows, and any wide column a
//! parent reads is held with them. When the DECIDE source is a stored table, a column the rule never uses can instead
//! be read from the table a second time and joined to the answers on the row id, so it never enters the hold. This is
//! late materialization, the technique behind DuckDB's own LATE_MATERIALIZATION pass; it is a separate step, and it
//! runs inside the UNUSED_COLUMNS pass (see optimizer.cpp).
//!
//! The shape both reads of a stored table need: a chain of non-volatile projections and filters over one scan of a
//! DuckDB base table, read through the ordinary table scan. Fills `chain` with the chain's operators, `source` first
//! and the scan excluded, and returns the scan; returns null for any other source (a join, a subquery with an aggregate
//! or a limit, a volatile expression, a table function). Late materialization and the repeated reads of
//! DirectSourceReader share it.
optional_ptr<LogicalGet> DirectRepeatableSourceChain(LogicalOperator &source,
                                                     vector<reference<LogicalOperator>> &chain);

//! Step one, in the rule driver before the rule applies: if the source is a chain of non-volatile projections
//! and filters over one scan of a DuckDB base table, append that table's row id to the source as a trailing column and
//! record which source columns forward a stored column unchanged and are not in `read`, the source bindings the DECIDE
//! node's own expressions read (LogicalDecide::EnumerateExpressions: constraints, objective, keys, scope checks, bound
//! aggregates). The rule passes the row id along like any other source column. Returns false and leaves the source
//! untouched for any other source (a join, a subquery with an aggregate or a limit, a volatile expression, a table
//! function).
bool DirectExposeStoredRowId(ClientContext &context, LogicalOperator &source, const column_binding_set_t &read,
                             DirectLateMaterialization &late_materialization);

//! Step two, once unused columns are removed and the parents' reads are known: for every result adapter whose parent
//! reads a variable-width stored column (VARCHAR, BLOB, a nested type) that the rule only forwards, read that column
//! from a second scan of the table joined to the adapter's input on the row id, then prune again so the column leaves
//! the rule's plan. The step redirects what the adapter forwards and removes nothing itself. An adapter whose row id
//! is not needed lets go of it. Knows nothing about any rule.
unique_ptr<LogicalOperator> ApplyDirectLateMaterialization(unique_ptr<LogicalOperator> plan, Optimizer &optimizer);

} // namespace duckdb
