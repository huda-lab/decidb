//===----------------------------------------------------------------------===//
// Hand-built clause rows for the stage 06 tests (ClauseRecords).
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/decidb/formulation/solver_input.hpp"

#include <tuple>

namespace duckdb {

//! One term of a row: (decision, instance, coefficient).
using RecordTerm = std::tuple<idx_t, idx_t, double>;

//! Appends a row `ec` emits: `terms` against `bound`, in `group`, standing for the data row numbered as its first
//! term's instance.
inline void AddRecord(EvaluatedConstraint &ec, const vector<RecordTerm> &terms, double bound,
                      idx_t group = DConstants::INVALID_INDEX) {
	auto &records = ec.records;
	for (auto &term : terms) {
		records.variables.push_back(std::get<0>(term));
		records.instances.push_back(std::get<1>(term));
		records.coefficients.push_back(std::get<2>(term));
	}
	records.term_begin.push_back(records.variables.size());
	records.constants.push_back(0.0);
	records.bounds.push_back(bound);
	records.groups.push_back(group);
	records.data_rows.push_back(terms.empty() ? 0 : std::get<1>(terms[0]));
}

} // namespace duckdb
