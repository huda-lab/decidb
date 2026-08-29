#include "duckdb/execution/operator/decide/physical_decide_diagnose.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/planner/operator/decide/logical_decide_diagnose.hpp"

namespace duckdb {

unique_ptr<PhysicalOperator> PhysicalPlanGenerator::CreatePlan(LogicalDecideDiagnose &op) {
	D_ASSERT(op.children.size() == 1);
	auto child = CreatePlan(*op.children[0]);
	// One row per finding, and a feasible query still reports one — so the cardinality is
	// never zero and is small by construction.
	auto diagnose = make_uniq<PhysicalDecideDiagnose>(op.types, op.estimated_cardinality);
	diagnose->children.push_back(std::move(child));
	return std::move(diagnose);
}

} // namespace duckdb
