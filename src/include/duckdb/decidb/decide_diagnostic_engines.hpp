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
	//! Clause text for global-block columns (aggregate `<>` indicators), parallel
	//! to the global block; empty for unnamed globals. Lets the removal dial name a
	//! dropped aggregate `<>`. Forwarded into BuildColumnProvenance.
	const vector<string> &global_variable_labels;
	const DecideDiagParams &params;

	//! True when the operator absorbed a user bound it could not re-emit as a
	//! slackable row for this engine (the variable's column is missing from the
	//! retained model). The bound stays rigid in `model`, so if the elastic program
	//! is infeasible the engine must NOT claim a structural conflict ("can't be
	//! fixed by loosening"); it falls through to the static error instead. Set by
	//! the re-emission loop in PhysicalDecide's INFEASIBLE arm; expected to stay
	//! false in practice (every absorbed bound, BOOLEAN pins included, is re-emitted).
	bool has_unhandled_user_bounds = false;

	//! Solve a (transformed) model with the same backend as the primary solve.
	//! Injected by the caller so the engine runs the elastic re-solve without
	//! depending on the operator or the solver facade — same boundary, and the
	//! same testability, as DiagnoseUnbounded's get_candidates.
	std::function<SolverResult(const SolverModel &model)> solve_model;
};

enum class ElasticRepairTier {
	REMOVAL,
	DATA_OFFSET,
	EDITABLE_LOOSEN
};

//! A slack wired into one or more relaxable rows of the elastic program. A block
//! may span N matrix rows that share ONE editable user knob (SHARED_SCALAR shapes:
//! easy MIN/MAX, per-row literal, multi-instance bound) so the minimal slack is the
//! max overshoot across the block, not the sum. For `=` rows two slacks are used
//! (s⁺, s⁻); `neg_col` is INVALID for `<` / `>` (one-sided loosening). A size-1
//! block is the simple-shape (I1) case. `tier`/`weight` drive the lexicographic
//! stage-1 repair objective: data offsets use raw slack, editable loosening uses
//! the T1 scale-normalized weight.
struct BlockSlackRef {
	vector<idx_t> rows;                      //!< Row indices this slack spans (≥1).
	idx_t pos_col;                           //!< Positive slack column in the elastic model.
	idx_t neg_col = DConstants::INVALID_INDEX; //!< Negative slack column (`=` rows only).
	char sense;                              //!< '<', '>', or '=' (uniform across the block).
	ElasticRepairTier tier = ElasticRepairTier::EDITABLE_LOOSEN;
	double weight = 1.0;
	//! When true, `rows` index `model.quadratic_constraints` (slack on the linear
	//! RHS only, never Q — I2.d); otherwise they index `model.constraints`.
	bool quadratic = false;
};

//! A binary removal indicator wired into the rows of one remove-only `<>` clause
//! (I4). `<>` cannot be loosened — only dropped — so a single binary `w` is wired
//! into both Big-M disjunction rows with a ±M₂ coefficient (sign by sense): w=1
//! makes both rows vacuous (the clause is dropped). The pair is identified by a
//! shared `indicator_col` (the `<>` disjunction binary). The lexicographic stage-1
//! repair objective minimizes the removal tier first, so removal stays a last resort
//! without a Big-M-style objective weight.
struct RemovalRef {
	vector<idx_t> rows;        //!< Rows of this `<>` disjunction (the pair sharing the indicator).
	idx_t w_col;               //!< The binary removal column in the elastic model.
	idx_t indicator_col;       //!< The `<>` disjunction binary — sources the label (resolved later).
};

//! The elastic program built from a base model: the transformed SolverModel (user
//! objective zeroed, slacks/removal switches appended) plus the repair-knob wiring needed
//! to read the result back. Pure function of the base model — the structural test
//! seam for the elastic transform.
struct ElasticModel {
	SolverModel model;
	vector<BlockSlackRef> slacks;
	vector<RemovalRef> removals; //!< Remove-only `<>` clauses (I4); empty when none.
};

//! Build the elastic program from a base model: zero the user objective, drop the
//! quadratic objective, and add a non-negative REAL slack to every relaxable row
//! (SHARED_SCALAR blocks share one slack across their rows; others get one each).
//! Rigid STRUCTURAL rows are untouched; remove-only `<>` rows (USER_MECHANISM with a
//! valid `indicator_col`) instead get a binary removal indicator (I4). `removal_bigm`
//! is the M₂ used to neutralize a dropped `<>` (0 = auto-derive per clause from its
//! existing disjunction Big-M).
//!
//! `slack_scope` (T3) governs how a data-backed RHS (`x <= col`, PER_ROW_DATA) is
//! slacked: "query" (default) folds a clause's data rows into ONE shared slack (the
//! amount is the max overshoot = a single virtual query offset `x <= col + delta`);
//! "expanded" leaves them as independent per-row slacks so the readback can expose the
//! per-row conflict profile. SHARED_SCALAR knobs fold in both modes.
ElasticModel BuildElasticModel(const SolverModel &base, double removal_bigm = 0.0,
                               const string &slack_scope = "query");

//! Build the infeasible diagnosis (the elastic least-change fix): build the elastic
//! program (BuildElasticModel), solve it via the injected callback, and read the
//! positive-slack support as the minimal edit list. Returns valid=false (caller
//! falls through to the static error) when nothing is loosenable or the result is
//! unusable.
DecideDiagnostic DiagnoseInfeasible(const InfeasibleDiagnosisInput &input);

} // namespace duckdb
