#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/insertion_order_preserving_map.hpp"

namespace duckdb {

class ClientContext;
class DBConfig;
class LogicalDecide;
class LogicalOperator;
class Optimizer;

enum class DirectSolveMode : uint8_t { OFF, AUTO, REQUIRE };

//! What EXPLAIN shows for a plan the direct solver built: the setting, the rule that was proved, and the proof and
//! runtime guards it carries. A query the rules do not prove keeps its solver plan and has no record.
struct DirectSolveDecisionRecord {
	string mode;
	string rule;
	string proof;
	string guards;

	InsertionOrderPreservingMap<string> Render() const;
};

DirectSolveMode GetDirectSolveMode(ClientContext &context);
void RegisterDirectSolve(DBConfig &config);

//! Leaves the original DECIDE node intact on a miss. A hit owns a complete
//! relational replacement with its external bindings and runtime obligations.
unique_ptr<LogicalOperator> TryDirectSolve(unique_ptr<LogicalOperator> op, Optimizer &optimizer,
                                           DirectSolveMode mode);

} // namespace duckdb
