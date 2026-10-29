//===----------------------------------------------------------------------===//
//                         DeciDB
//
// src/decidb/formulation/instance_floors.cpp
//
// The instances of a decision whose default floor of 0 a constant lower bound under a `when` replaces. See
// ilp_linearization.hpp.
//
//===----------------------------------------------------------------------===//
#include "duckdb/decidb/formulation/ilp_linearization.hpp"
#include "duckdb/common/decide_numeric_policy.hpp"
#include "duckdb/common/limits.hpp"

#include <algorithm>
#include <limits>

namespace duckdb {

void ResolveLiftedFloors(SolverInput &input, const VarIndexer &indexer, const vector<DecideBoundBox> &boxes) {
	constexpr double INF = std::numeric_limits<double>::infinity();
	// Per decision, the floor each instance takes from the bounds that keep it, or +infinity for an instance no bound
	// lowers. Allocated for the decisions a bound lowers on some row.
	vector<vector<double>> lowered(input.num_decide_vars);
	for (const auto &ec : input.constraints) {
		idx_t var = ec.lifted_floor;
		// A decision with a lower bound written (an unconditional one, a BOOL, a signed helper) never takes the
		// default floor of 0, so a conditional bound only intersects with it.
		if (var == DConstants::INVALID_INDEX || boxes[var].lower_written) {
			continue;
		}
		for (idx_t row = 0; row < input.num_rows; row++) {
			// The rows the clause keeps are the ones that become a model row; no row_group_ids means every row.
			if (!ec.row_group_ids.empty() && ec.row_group_ids[row] == DConstants::INVALID_INDEX) {
				continue;
			}
			if (lowered[var].empty()) {
				lowered[var].assign(indexer.NumInstances(var), INF);
			}
			// A floor of "no bound" (-infinity) is the solver's open end, as an unconditional one writes it (D41).
			double floor = std::max(ec.admitted_lower[row], -DECIDE_SOLVER_INFINITY);
			auto &instance_floor = lowered[var][indexer.InstanceOf(var, row)];
			// Every clause that keeps the instance applies to it, so the tightest floor is the instance's.
			instance_floor = instance_floor == INF ? floor : std::max(instance_floor, floor);
		}
	}
	input.instance_lower_bounds.resize(input.num_decide_vars);
	for (idx_t var = 0; var < input.num_decide_vars; var++) {
		if (lowered[var].empty()) {
			continue;
		}
		// The decision's box holds the lowest floor of any instance: a lowered instance's floor is its tightest kept
		// bound, and an instance no bound keeps holds the default of 0. Each instance's own floor is restored by its
		// refinement (0 where the default holds) or by its clause's row.
		double hull = INF;
		auto &refinement = input.instance_lower_bounds[var];
		refinement.resize(lowered[var].size());
		for (idx_t instance = 0; instance < refinement.size(); instance++) {
			bool is_lowered = lowered[var][instance] != INF;
			hull = std::min(hull, is_lowered ? lowered[var][instance] : 0.0);
			refinement[instance] = is_lowered ? -INF : 0.0;
		}
		input.lower_bounds[var] = hull;
		// A floor the query wrote is re-opened in the rigid box, as a folded one is: a diagnosis may loosen it.
		input.rigid_lower_bounds[var] = -NumericLimits<double>::Maximum();
	}
}

} // namespace duckdb
