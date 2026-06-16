//===----------------------------------------------------------------------===//
//                         DecidB
//
// duckdb/decidb/decide_diagnostic.hpp
//
// F5: the shared, structured diagnostic reporting surface. A state engine
// (unbounded this session; infeasible / slow later) populates a DecideDiagnostic,
// it is stashed per-connection, and the decide_diagnostics() table function reads
// it back as a fixed-schema relation. See
// context/descriptions/08_query_diagnostics/.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/decidb/solver_result.hpp"
#include "duckdb/main/client_context_state.hpp"

namespace duckdb {

class ClientContext;
class BuiltinFunctions;
class DBConfig;
struct SolverResult;
struct ColumnProvenance;

//! One row of the relation surfaced by decide_diagnostics(). Empty string fields
//! render as SQL NULL.
struct DiagnosticRow {
	string clause;           //!< user-clause label (F2); empty => not clause-scoped
	string group_key;        //!< PER/WHEN group label; empty => ungrouped
	string edit_kind;        //!< "add bound" / "loosen RHS" / "widen bound" / "remove"
	string suggested_change; //!< human-readable prescription (never a chosen value for unbounded)
};

//! Structured diagnosis produced by a state engine and rendered by the table
//! function. Shared across all diagnosis states so output stays consistent.
struct DecideDiagnostic {
	bool valid = false;                       //!< false => nothing diagnosed yet
	SolverStatus status = SolverStatus::OTHER;
	string state;                             //!< "unbounded" / "infeasible" / "slow"
	string summary;                           //!< one-line human summary
	vector<DiagnosticRow> rows;
};

//! Per-connection stash: the most recent diagnosis, read back by
//! decide_diagnostics() in a subsequent statement on the same connection.
//! (A failed DECIDE throws, but the stash mutation precedes the throw and is not
//! rolled back, so it survives into the next statement.)
class DecideDiagnosticState : public ClientContextState {
public:
	DecideDiagnostic latest;
};

//! Key under which DecideDiagnosticState is registered on the ClientContext.
static constexpr const char *DECIDE_DIAGNOSTIC_STATE_KEY = "decide_diagnostics";

//! Build the unbounded diagnosis. Names the escaping variables (F6): collects the
//! columns with a non-zero entry in `result.ray`, resolves each through `columns`
//! (F6 variable provenance — user name or aux source expression), dedups by label,
//! and emits one "add bound" row per escaping variable. Falls back to the generic
//! "add a bound" prescription when no ray is attached (e.g. quadratic models, where
//! U2 extracts none). DeciDB never picks the bound value — any finite bound works.
DecideDiagnostic BuildUnboundedDiagnostic(const SolverResult &result,
                                          const vector<ColumnProvenance> &columns);

//! Store `diag` on the connection so decide_diagnostics() can read it next statement.
void StashDecideDiagnostic(ClientContext &context, DecideDiagnostic diag);

//! Throw the short pointer error a failed-but-diagnosed DECIDE surfaces (the
//! relation itself is read via SELECT * FROM decide_diagnostics()).
[[noreturn]] void ThrowDecideDiagnosisReady(const DecideDiagnostic &diag);

//! Registers the decide_diagnostics() table function.
struct DecideDiagnosticsFun {
	static void RegisterFunction(BuiltinFunctions &set);
};

//===----------------------------------------------------------------------===//
// F4: the `diagnose_decide` consent gate (manual-first).
//===----------------------------------------------------------------------===//

//! Register the sticky `diagnose_decide` session setting. Modes: none (default) /
//! infeasible / unbounded / slow / auto. Called once at DBConfig setup; the
//! set-callback validates the mode so a typo fails fast at SET time. Filter
//! semantics: a mode diagnoses only when the solve actually lands in that state.
void RegisterDecideDiagnosticOptions(DBConfig &config);

//! Read the current diagnose_decide mode (lowercased; "none" if unset).
string GetDiagnoseDecideMode(ClientContext &context);

//! Filter predicate: does `mode` request a diagnosis for a solve that ended in
//! `status`? (none => never; auto => any diagnosable state; otherwise exact match.)
bool DiagnosisApplies(const string &mode, SolverStatus status);

//! Whether `mode` should pre-arm unbounded-ray extraction before the solve, so
//! the ray is available if the solve turns out unbounded (manual-first: only paid
//! for when the user opted into unbounded/auto diagnosis).
bool DiagnoseModeWantsUnboundedRay(const string &mode);

} // namespace duckdb
