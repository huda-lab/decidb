#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/optimizer/decide/direct/direct_rule.hpp"
#include "duckdb/planner/logical_operator.hpp"

namespace duckdb {

class ClientContext;
class LogicalDecide;
class Optimizer;

//! Puts the IR's row checks (DirectRowCheck) under a rule's plan, where the solver makes them: before it reads a value.
//! Each check is a side aggregate over its own read of the source, pruned to the columns it reads, that comes to one
//! row: its message, NULL when it holds. A projection over their cross product raises the first message in the order
//! given, and the cross product of the source's rows with that one row passes on exactly the rows' columns: DuckDB's
//! plan of an uncorrelated scalar subquery. A check that is not a row check is not in `checks`; with none, the source
//! stays as it is.
//!
//! A value check takes the least message a row raises. A determination check (the solver's `RunScopeChecks`) groups
//! the rows it looks at by its key, finds the classes that show more than one value (or hold more than one row of a
//! decision), names the least of them by key value, and quotes its least and greatest value, NULL after every value.
//!
//! The checks go under `decide`, a node in `root`, and read its child, the source, through a DirectSourceReader that
//! never copies (DirectSourceCopy::NEVER): once for the rows and once per check. A recomputed source keeps its
//! bindings, and the function returns false. A materialized one is read under new bindings, and every reader of the
//! old ones above the source in `root`, the node's user names of its columns included, is re-pointed
//! (ColumnBindingReplacer, LogicalDecide::RepointSourceColumns); then the function returns true.
bool DirectAddRowChecks(Optimizer &optimizer, LogicalOperator &root, const vector<DirectRowCheck> &checks,
                        LogicalDecide &decide);

//! The scope checks of a DECIDE node that the plan can make, moved out of the node (LogicalDecide::scope_checks), for
//! the solver path to install under it with DirectAddRowChecks, so one implementation makes them on both paths. What
//! remains in the node is for the operator: the determination of a keyed decision.
vector<DirectRowCheck> DirectTakeRowChecks(ClientContext &context, LogicalDecide &decide);

} // namespace duckdb
