#include "duckdb/decidb/diagnostic_solves.hpp"

namespace duckdb {

SolverModel MakeZeroObjectiveProbeModel(const SolverModel &model) {
	SolverModel probe = model;
	probe.obj_coeffs.assign(probe.obj_coeffs.size(), 0.0);

	probe.q_rows.clear();
	probe.q_cols.clear();
	probe.q_vals.clear();
	probe.has_quadratic_obj = false;
	probe.nonconvex_quadratic = false;

	return probe;
}

} // namespace duckdb
