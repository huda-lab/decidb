#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/optimizer/decide/direct/direct_rule.hpp"
#include "duckdb/planner/logical_operator.hpp"

namespace duckdb {

class ClientContext;
class LogicalDecide;
class Optimizer;

//! Puts the IR's row checks (DirectRowCheck) under a rule's plan, where the solver makes them: before it reads a value.
//! Windows over the whole `source` compute, on every row, the message of each check (NULL when it passes), and a filter
//! above them raises the first check that fails, in the order given. The filter passes on exactly the source's columns,
//! so the source keeps its bindings. `source` has its types resolved. A check that is not a row check is not in
//! `checks`; with none, `source` comes back unchanged.
//!
//! A value check takes the least message a row raises, so that every row reads the same one. A determination check
//! (the solver's `RunScopeChecks`) partitions the rows it looks at by its key, finds the groups that show more than
//! one value (or hold more than one row of a decision), names the least of them by key value, and quotes its least and
//! greatest value, NULL after every value.
unique_ptr<LogicalOperator> DirectAddRowChecks(Optimizer &optimizer, const vector<DirectRowCheck> &checks,
                                               unique_ptr<LogicalOperator> source);

//! The scope checks of a DECIDE node that the plan can make, moved out of the node (LogicalDecide::scope_checks), for
//! the solver path to install under it with DirectAddRowChecks, so one implementation makes them on both paths. What
//! remains in the node is for the operator: the determination of an entity-scoped decision.
vector<DirectRowCheck> DirectTakeRowChecks(ClientContext &context, LogicalDecide &decide);

} // namespace duckdb
