#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/optimizer/decide/direct/direct_rule.hpp"
#include "duckdb/planner/logical_operator.hpp"

namespace duckdb {

class ClientContext;
class DirectSourceReader;
class LogicalDecide;
class Optimizer;

//! Makes the IR's row checks (DirectRowCheck) on the reads of `reader`, where the solver makes them: before a value is
//! read. Each check is a side aggregate over its own read of the source, pruned to the columns it reads, that comes to
//! one row: its message, NULL when it holds. A projection over their cross product, the verdict, raises the first
//! message in the order given, and gates the reader (DirectSourceReader::Gate): every later read passes its rows only
//! once the verdict holds. This is DuckDB's plan of an uncorrelated scalar subquery, the verdict a one-row CTE. A check
//! that is not a row check is not in `checks`; with none, the reader stays ungated.
//!
//! A value check takes the least message a row raises. A determination check (the solver's `RunScopeChecks`) groups
//! the rows it looks at by its key, finds the classes that show more than one value (or hold more than one row of a
//! decision), names the least of them by key value, and quotes its least and greatest value, NULL after every value.
void DirectAddRowChecks(Optimizer &optimizer, DirectSourceReader &reader, const vector<DirectRowCheck> &checks);

//! The solver path's checks: the source of `decide`, a node in `root`, becomes one gated read of it
//! (DirectAddRowChecks), joined to the side tables of its bounds (DirectAddBoundTables), which the checks read. A
//! recomputed source keeps its bindings. A materialized one is read under new bindings, and every reader of the old
//! ones above the source in `root`, the node's user names of its columns included, is re-pointed
//! (ColumnBindingReplacer, LogicalDecide::RepointSourceColumns).
void DirectInstallRowChecks(Optimizer &optimizer, LogicalOperator &root, const vector<DirectRowCheck> &checks,
                            LogicalDecide &decide);

//! The scope checks of a DECIDE node that the plan can make, moved out of the node (LogicalDecide::scope_checks), for
//! the solver path to install under it with DirectInstallRowChecks, so one implementation makes them on both paths.
//! What remains in the node is for the operator: the determination of a keyed decision.
vector<DirectRowCheck> DirectTakeRowChecks(ClientContext &context, LogicalDecide &decide);

} // namespace duckdb
