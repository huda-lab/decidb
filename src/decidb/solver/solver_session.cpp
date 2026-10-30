#include "duckdb/decidb/solver/solver_session.hpp"

#include "duckdb/decidb/formulation/ilp_model.hpp"

namespace duckdb {

SolverResult SolverSession::Solve(const SolverModel &model, double time_limit_seconds) {
	objective_constant = model.obj_constant;
	return AddObjectiveConstant(SolveChunk(model, time_limit_seconds));
}

SolverResult SolverSession::Continue(double time_limit_seconds) {
	return AddObjectiveConstant(ContinueChunk(time_limit_seconds));
}

//! An objective value is read only with an incumbent (OPTIMAL, or `has_solution`); a bound that does not exist is
//! NaN and stays NaN. A constant whose terms overflowed a DOUBLE is infinite, and so is the value it reports.
SolverResult SolverSession::AddObjectiveConstant(SolverResult result) const {
	if (result.status == SolverStatus::OPTIMAL || result.has_solution) {
		result.objective_value += objective_constant;
	}
	result.best_bound += objective_constant;
	return result;
}

} // namespace duckdb
