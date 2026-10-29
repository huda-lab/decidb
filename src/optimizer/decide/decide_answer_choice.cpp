#include "duckdb/optimizer/decide/decide_answer_choice.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/decidb/solver/ilp_solver.hpp"
#include "duckdb/optimizer/decide/direct/direct_solve.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"

namespace duckdb {

bool ChooseDecideAnswer(unique_ptr<LogicalOperator> &op, unique_ptr<LogicalOperator> &root, Optimizer &optimizer) {
	auto &decide = op->Cast<LogicalDecide>();
	auto mode = GetDirectSolveMode(optimizer.context);
	if (mode != DirectSolveMode::OFF) {
		// A DIAGNOSE query or a forced backend keeps the solver path; there is nothing to report.
		if (decide.diagnose || !ForcedSolverName().empty()) {
			if (mode == DirectSolveMode::REQUIRE) {
				throw InvalidInputException("decide_direct_solve=require conflicts with %s: the solver is required",
				                            decide.diagnose ? "DIAGNOSE" : "DECIDB_FORCE_SOLVER");
			}
		} else {
			if (ApplyDirectSolveRules(op, root, optimizer, mode)) {
				return true;
			}
			if (mode == DirectSolveMode::REQUIRE) {
				throw InvalidInputException(
				    "decide_direct_solve=require: direct solve did not apply; the solver is required");
			}
		}
	}
	ChooseDecideSolver(decide);
	return false;
}

void ChooseDecideSolver(LogicalDecide &op) {
	D_ASSERT(op.solver_backend_name.empty());
	// From here this plan carries formulation state chosen for THIS host's solver, which is not portable and so is not
	// serialized. Setting the flag before the first formulation rewrite is what makes LogicalDecide::SupportSerialization()
	// stop reporting true, so the post-optimizer PRAGMA verify_serializer round trip skips the node instead of silently
	// replacing the plan with a copy missing everything below.
	op.optimized = true;
	SolverBackend backend = SelectSolverBackend();
	op.solver_backend_name = backend.Name();
	// The formulation decision, made HERE and read everywhere else. `Capabilities()`
	// answers what the backend CAN state; recording it on the plan turns that into what
	// this query WILL leave native. The two are the same table, which is why one type
	// carries both — but the question is asked exactly once, at the only stage entitled
	// to choose a formulation.
	op.use_native_constructs = backend.Capabilities().constructs;
	// ...and the policy that governs their use. Read here, with the rest of the
	// formulation decision, rather than at the emission site: an environment read
	// scattered across stage 08 would be a second place the formulation gets decided.
	op.force_native_constructs = NativeConstructsForced();
}

} // namespace duckdb
