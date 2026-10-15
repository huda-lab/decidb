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

//! What EXPLAIN shows for a plan the direct solver built: the setting, the rule that applied, and one fixed line
//! naming the problem class. A query no rule applies to keeps its solver plan and has no annotation.
struct DirectExplainAnnotation {
	string mode;
	string rule;
	string condition;

	InsertionOrderPreservingMap<string> Render() const;
};

DirectSolveMode GetDirectSolveMode(ClientContext &context);
void RegisterDirectSolve(DBConfig &config);

//! Leaves the original DECIDE node intact when no rule applies. When one does, it owns a complete
//! relational replacement with its external bindings and runtime obligations.
unique_ptr<LogicalOperator> ApplyDirectSolveRules(unique_ptr<LogicalOperator> op, Optimizer &optimizer,
                                                  DirectSolveMode mode);

} // namespace duckdb
