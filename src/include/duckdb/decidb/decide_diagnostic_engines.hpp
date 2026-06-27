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

struct InfeasibleDiagnosisInput {
	const SolverModel &model;
	const VarIndexer &indexer;
	const vector<string> &var_labels;
	const vector<bool> &var_is_aux;
	const DecideDiagParams &params;

	//! True when the operator absorbed a user bound it could not re-emit as a
	//! slackable row for this engine — today, a bound on a multi-instance variable
	//! (its shared-slack form is I2). The bound stays rigid in `model`, so if the
	//! elastic program is infeasible the engine must NOT claim a structural conflict
	//! ("can't be fixed by loosening"); it falls through to the static error instead.
	bool has_unhandled_user_bounds = false;

	//! Solve a (transformed) model with the same backend as the primary solve.
	//! Injected by the caller so the engine runs the elastic re-solve without
	//! depending on the operator or the solver facade — same boundary, and the
	//! same testability, as DiagnoseUnbounded's get_candidates.
	std::function<SolverResult(const SolverModel &model)> solve_model;
};

//! Build the infeasible diagnosis (the elastic least-change fix). I0 is a seam
//! only: it returns valid=false so the caller falls through to the static error.
//! The elastic transform + stage-1 solve land in I1; this carries the built model
//! (which the transform reshapes) in place of the unbounded engine's solved ray.
DecideDiagnostic DiagnoseInfeasible(const InfeasibleDiagnosisInput &input);

} // namespace duckdb
