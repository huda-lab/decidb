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

#include <set>

namespace duckdb {

class ClientContext;
class BuiltinFunctions;
class DBConfig;
struct SolverResult;
struct ColumnProvenance;

//! One row of the relation surfaced by decide_diagnostics(). Empty string fields
//! render as SQL NULL. For the unbounded state, one row = one escaping variable.
struct DiagnosticRow {
	string variable;        //!< escaping variable NAME (F6); empty => NULL
	string direction;       //!< direction it escapes: "+inf" / "-inf" (ASCII, robust
	                        //!< in CSV / WHERE filters); empty => NULL
	string escaping_instances; //!< which instances escape: categorical rule set
	                        //!< (`c=v (a/b); …`), an `all N instances …` / `a of b
	                        //!< instances escape` summary, or empty => NULL (nothing
	                        //!< to disambiguate / not resolved).
};

//! One categorical "sufficient-direction" rule for an escaping variable: among the
//! `total` instances where `column = value`, `escaping` of them escape. Reported
//! when escaping/total ≥ the escape-rate threshold.
struct EscapeRule {
	string column;
	string value;
	idx_t escaping = 0; //!< a
	idx_t total = 0;    //!< b (group size, counted in instances)
};

//! A categorical column's grouping over a variable's instances, fed to
//! CharacterizeEscape. Pure data (no DuckDB execution types) so the core unit-tests.
struct ColumnGrouping {
	string column;                  //!< column name (for the rule)
	vector<idx_t> instance_to_group; //!< size = total_instances; group id per
	                                 //!< instance (INVALID_INDEX = excluded)
	vector<string> group_value;      //!< size = num_groups; value label per group
};

//! Per-variable escape characterization, assembled by the operator and formatted
//! by BuildUnboundedDiagnostic into one DiagnosticRow.
struct VarEscape {
	string name;             //!< user variable name (USER) or source expr (AUX)
	string direction;        //!< "+∞" / "-∞"
	idx_t escaping = 0;      //!< number of escaping instances of this variable
	idx_t total = 0;        //!< total instances of this variable
	bool all_escape = false; //!< escaping == total
	bool is_aux = false;     //!< aux/linearization column (name-only, no rules)
	vector<EscapeRule> rules; //!< categorical rules (empty => count fallback)
};

//! Pragma-tunable knobs for the unbounded characterization. Read once per solve.
struct DecideDiagParams {
	double escape_rate = 0.8;       //!< report groups with rate ≥ this
	double categorical_ratio = 0.1; //!< column is categorical if distinct ≤ ratio×N
	idx_t min_categories = 20;       //!< …or ≤ this absolute floor (small tables)
};

//! Structured diagnosis produced by a state engine and rendered by the table
//! function. Shared across all diagnosis states so output stays consistent.
struct DecideDiagnostic {
	bool valid = false;                       //!< false => nothing diagnosed yet
	int64_t query_id = 0;                     //!< per-connection diagnosis id; ties together
	                                          //!< every row produced by the same failed solve
	SolverStatus status = SolverStatus::OTHER;
	string state;                             //!< "unbounded" / "infeasible" / "slow"
	string summary;                           //!< one-line human summary (the stderr pointer)
	vector<DiagnosticRow> rows;
};

//! Per-connection stash: the most recent diagnosis, read back by
//! decide_diagnostics() in a subsequent statement on the same connection.
//! (A failed DECIDE throws, but the stash mutation precedes the throw and is not
//! rolled back, so it survives into the next statement.)
class DecideDiagnosticState : public ClientContextState {
public:
	DecideDiagnostic latest;
	int64_t next_query_id = 1; //!< monotonic per-connection id, assigned at each stash
};

//! Key under which DecideDiagnosticState is registered on the ClientContext.
static constexpr const char *DECIDE_DIAGNOSTIC_STATE_KEY = "decide_diagnostics";

//! Pure characterization core (unit-testable; no DuckDB execution types). For one
//! escaping variable, given its escaping-instance set, total instance count, the
//! candidate categorical columns' groupings, and the escape-rate threshold, returns
//! every `(column, value)` group whose within-group escape rate (escaping/total) ≥
//! `escape_rate_threshold`, sorted by rate descending then column/value for
//! determinism. The "sufficient-direction" rules: when `column = value` holds in an
//! instance, that instance's variable escapes (a of b of the time).
vector<EscapeRule> CharacterizeEscape(const std::set<idx_t> &escaping, idx_t total_instances,
                                      const vector<ColumnGrouping> &candidates,
                                      double escape_rate_threshold);

//! Build the unbounded diagnosis from the per-variable characterizations the
//! operator assembled (one row per escaping variable). Formats each VarEscape's
//! `escaping_instances` cell: `all N instances …` when all escape, the `; `-joined
//! categorical rules `c=v (a/b)` when any clear the threshold, else the bare count
//! `a of b instances escape`. The summary prescribes the forced remedy (add a finite
//! bound) without inventing the cap, and appends a one-line legend for the
//! `escaping_instances` cell format when categorical rules are present. Precondition:
//! `escapes` is non-empty — the caller falls through to the static error when the ray
//! names nothing (quadratic model, or only internal auxiliaries escaped).
DecideDiagnostic BuildUnboundedDiagnostic(const vector<VarEscape> &escapes);

//! Store `diag` on the connection so decide_diagnostics() can read it next statement.
void StashDecideDiagnostic(ClientContext &context, DecideDiagnostic diag);

//! Invalidate any stashed diagnosis on the connection. Called on a successful
//! solve so decide_diagnostics() does not keep reporting a now-resolved failure.
//! The per-connection id counter is left intact (ids stay monotonic across solves).
void ClearDecideDiagnostic(ClientContext &context);

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

//! Read the unbounded-characterization knobs (escape rate / categorical ratio /
//! min-categories floor) from the session settings, falling back to defaults.
DecideDiagParams GetDecideDiagnosticParams(ClientContext &context);

//! Filter predicate: does `mode` request a diagnosis for a solve that ended in
//! `status`? (none => never; auto => any diagnosable state; otherwise exact match.)
bool DiagnosisApplies(const string &mode, SolverStatus status);

//! Whether `mode` should pre-arm unbounded-ray extraction before the solve, so
//! the ray is available if the solve turns out unbounded (manual-first: only paid
//! for when the user opted into unbounded/auto diagnosis).
bool DiagnoseModeWantsUnboundedRay(const string &mode);

} // namespace duckdb
