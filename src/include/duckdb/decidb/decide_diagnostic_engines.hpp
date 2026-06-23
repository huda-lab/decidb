//===----------------------------------------------------------------------===//
//                         DecidB
//
// duckdb/decidb/decide_diagnostic_engines.hpp
//
// Diagnosis engine orchestration for DECIDE failure states. Engines are kept
// outside PhysicalDecide so new states can attach without growing the operator.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/decidb/decide_diagnostic.hpp"
#include "duckdb/decidb/ilp_model.hpp"

#include <functional>

namespace duckdb {

struct UnboundedDiagnosisInput {
	const SolverResult &result;
	const VarIndexer &indexer;
	const vector<string> &var_labels;
	const vector<bool> &var_is_aux;
	const DecideDiagParams &params;

	//! Return categorical candidates for a partially escaping user variable.
	//! The callback is injected by the caller because building these groupings
	//! depends on executor-local chunks and entity-scope metadata.
	std::function<vector<ColumnGrouping>(idx_t decide_var_idx, idx_t total_instances)> get_candidates;
};

//! Build the unbounded diagnosis from an attached ray. Returns valid=false when
//! the ray has no named per-variable content (the caller should fall through to
//! the static error).
DecideDiagnostic DiagnoseUnbounded(const UnboundedDiagnosisInput &input);

} // namespace duckdb
