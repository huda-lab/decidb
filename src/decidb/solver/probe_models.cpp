#include "duckdb/decidb/solver/probe_models.hpp"

#include "duckdb/common/decide_numeric_policy.hpp"
#include "duckdb/decidb/diagnostics/diagnostic_constants.hpp"

#include <utility>

namespace duckdb {

SolverModel MakeZeroObjectiveProbeModel(const SolverModel &model) {
	SolverModel probe = model;
	probe.ClearObjective();
	return probe;
}

bool BuildUnboundedRayFallbackModel(const SolverModel &model, SolverModel &ray_model) {
	// The ray model is a homogeneous LP: it rebuilds the matrix row by row, so anything
	// that is not a row cannot come along. A quadratic term cannot, and neither can a
	// general constraint (`aux = |t|`) — a backend expressed it natively precisely
	// because it is not linear. Dropping one would relax the model further than a ray
	// argument allows, and the direction found might not be a real escape. Decline
	// instead, and let the diagnosis say it could not identify one.
	if (model.has_quadratic_obj || !model.quadratic_constraints.empty() ||
	    !model.general_constraints.empty() || !model.indicator_constraints.empty()) {
		ray_model = SolverModel();
		return false;
	}

	ray_model = SolverModel();
	ray_model.num_vars = model.num_vars;

	// The recession cone of the column box: a column may move along the ray only in a direction its box leaves open,
	// up to a unit step either way.
	ray_model.col_lower.resize(model.num_vars, 0.0);
	ray_model.col_upper.resize(model.num_vars, 0.0);
	for (idx_t col = 0; col < model.num_vars; col++) {
		ray_model.col_lower[col] = DecideIsOpenBelow(model.col_lower[col]) ? -1.0 : 0.0;
		ray_model.col_upper[col] = DecideIsOpenAbove(model.col_upper[col]) ? 1.0 : 0.0;
	}

	ray_model.is_integer.assign(model.num_vars, false);
	ray_model.is_binary.assign(model.num_vars, false);

	ray_model.obj_coeffs.resize(model.obj_coeffs.size(), 0.0);
	for (idx_t col = 0; col < model.obj_coeffs.size(); col++) {
		ray_model.obj_coeffs[col] = model.maximize ? model.obj_coeffs[col] : -model.obj_coeffs[col];
	}
	ray_model.maximize = true;

	ray_model.constraints.reserve(model.constraints.size());
	for (const auto &constraint : model.constraints) {
		ModelConstraint ray_constraint;
		ray_constraint.indices = constraint.indices;
		ray_constraint.coefficients = constraint.coefficients;
		ray_constraint.sense = constraint.sense;
		ray_constraint.rhs = 0.0;
		ray_model.constraints.push_back(std::move(ray_constraint));
	}

	ray_model.q_rows.clear();
	ray_model.q_cols.clear();
	ray_model.q_vals.clear();
	ray_model.has_quadratic_obj = false;
	ray_model.nonconvex_quadratic = false;
	ray_model.quadratic_constraints.clear();

	return true;
}

} // namespace duckdb
