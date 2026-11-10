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

//! Mark every column that appears in a non-linear part of `model`: a square or product (the objective's or a row's),
//! a general constraint's result and arguments, and an indicator constraint's binary and row columns.
static vector<bool> ColumnsInNonLinearParts(const SolverModel &model) {
	vector<bool> in_part(model.num_vars, false);
	auto mark = [&](int col) {
		D_ASSERT(col >= 0 && static_cast<idx_t>(col) < in_part.size());
		in_part[col] = true;
	};
	for (idx_t k = 0; k < model.q_vals.size(); k++) {
		mark(model.q_rows[k]);
		mark(model.q_cols[k]);
	}
	for (const auto &quadratic : model.quadratic_constraints) {
		for (idx_t k = 0; k < quadratic.q_coefficients.size(); k++) {
			mark(quadratic.q_rows[k]);
			mark(quadratic.q_cols[k]);
		}
	}
	for (const auto &general : model.general_constraints) {
		mark(general.result_column);
		for (int col : general.argument_columns) {
			mark(col);
		}
	}
	for (const auto &indicator : model.indicator_constraints) {
		mark(indicator.binary_column);
		for (int col : indicator.indices) {
			mark(col);
		}
	}
	return in_part;
}

SolverModel BuildUnboundedRayFallbackModel(const SolverModel &model) {
	// The ray model is a homogeneous LP over the linear rows and the linear part of each quadratic row. A column in a
	// non-linear part (a square or product, a general constraint, an indicator constraint) stays still along the ray,
	// so every such part keeps its value: a feasible point moved along the ray stays feasible and the objective
	// changes by the linear part alone. The ray is then a real escape (a direction of the recession cone), never a
	// relaxation. A runaway inside a non-linear part finds no ray.
	vector<bool> held_still = ColumnsInNonLinearParts(model);

	SolverModel ray_model;
	ray_model.num_vars = model.num_vars;

	// The recession cone of the column box: a column may move along the ray only in a direction its box leaves open,
	// up to a unit step either way.
	ray_model.col_lower.resize(model.num_vars, 0.0);
	ray_model.col_upper.resize(model.num_vars, 0.0);
	for (idx_t col = 0; col < model.num_vars; col++) {
		if (held_still[col]) {
			continue;
		}
		ray_model.col_lower[col] = DecideIsOpenBelow(model.col_lower[col]) ? -1.0 : 0.0;
		ray_model.col_upper[col] = DecideIsOpenAbove(model.col_upper[col]) ? 1.0 : 0.0;
	}

	ray_model.is_integer.assign(model.num_vars, false);
	ray_model.is_binary.assign(model.num_vars, false);

	// The ray improves the objective by moving it down: the model is a minimization, so its value along the ray is
	// negative.
	ray_model.obj_coeffs = model.obj_coeffs;

	ray_model.constraints.reserve(model.constraints.size());
	for (const auto &constraint : model.constraints) {
		ModelConstraint ray_constraint;
		ray_constraint.indices = constraint.indices;
		ray_constraint.coefficients = constraint.coefficients;
		ray_constraint.sense = constraint.sense;
		ray_constraint.rhs = 0.0;
		ray_model.constraints.push_back(std::move(ray_constraint));
	}

	// The linear part of each quadratic row. Its quadratic part is constant along the ray.
	for (const auto &quadratic : model.quadratic_constraints) {
		ModelConstraint ray_constraint;
		ray_constraint.indices = quadratic.linear_indices;
		ray_constraint.coefficients = quadratic.linear_coefficients;
		ray_constraint.sense = quadratic.sense;
		ray_constraint.rhs = 0.0;
		ray_model.constraints.push_back(std::move(ray_constraint));
	}

	return ray_model;
}

} // namespace duckdb
