#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/optimizer/decide/direct/direct_rule.hpp"
#include "duckdb/planner/logical_operator.hpp"

namespace duckdb {

class ClientContext;
class DirectSourceReader;
class LogicalDecide;
class Optimizer;

//! Makes the IR's row checks (LogicalDecide::value_checks) on the reads of `reader`, where the solver makes them:
//! before a value is read. The checks are one side aggregate over one read of the source that comes to one row: true
//! when every check holds on every row. Each row makes them in order, as the solver does, so a row that fails one
//! raises its error where it is evaluated. A projection over it, the verdict, gates the reader
//! (DirectSourceReader::Gate): every later read passes its rows only once the verdict holds. This is DuckDB's plan of
//! an uncorrelated scalar subquery, the verdict a one-row CTE. A check that is not a row check is not in `checks`; with
//! none, the reader stays ungated.
//!
//! A value check is a BOOLEAN expression over the source row that raises on a refused value.
void DirectAddRowChecks(Optimizer &optimizer, DirectSourceReader &reader, const vector<unique_ptr<Expression>> &checks);

} // namespace duckdb
