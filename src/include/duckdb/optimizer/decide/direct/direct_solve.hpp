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

//! A plan decision, shared by the hit boundary, solver fallback, and explanation.
struct DirectSolveDecisionRecord {
	bool attempted = false;
	string mode;
	string rule;
	string reason;
	string proof;
	string guards;
	bool hit = false;
	bool skipped_solver = false;

	InsertionOrderPreservingMap<string> Render() const;
};

DirectSolveMode GetDirectSolveMode(ClientContext &context);
void RegisterDirectSolve(DBConfig &config);

//! Leaves the original DECIDE node intact on a miss. A hit owns a complete
//! relational replacement with its external bindings and runtime obligations.
unique_ptr<LogicalOperator> TryDirectSolve(unique_ptr<LogicalOperator> op, Optimizer &optimizer,
                                           DirectSolveMode mode);

} // namespace duckdb
