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
//! render as SQL NULL. Long-form EAV so each diagnosis state can choose its own
//! attributes without changing the table schema.
struct DiagnosticRow {
	string subject_kind; //!< "variable" / "clause" / future state-specific subject
	string subject;      //!< e.g. variable name or clause id; empty => NULL
	string attribute;    //!< state-owned attribute name; empty => NULL
	string value;        //!< state-owned attribute value; empty => NULL
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
	bool is_entity_scoped = false; //!< true => instances are entities, not rows (noun)
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
	int64_t diagnosis_id = 0;                 //!< per-connection diagnosis id; ties together
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
	int64_t next_diagnosis_id = 1; //!< monotonic per-connection id, assigned at each stash
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
//! `affected_rows` / `affected_entities` cell (self-describing, no legend): `all N
//! rows` when all escape, the `; `-joined categorical rules `a of b rows where
//! c = 'v'` when any clear the threshold, else the bare count `a of b rows`
//! (`entities` in place of `rows` for entity-scoped vars). The summary prescribes the
//! forced remedy (add a finite bound) without inventing the cap. Precondition:
//! `escapes` is non-empty — the caller falls through to the static error when the ray
//! names nothing (quadratic model, or only internal auxiliaries escaped).
DecideDiagnostic BuildUnboundedDiagnostic(const vector<VarEscape> &escapes);

//! One least-change edit the infeasible (elastic) engine found: loosen the
//! constraint as written (`label`, e.g. "x <= 10") to `suggestion` (e.g.
//! "x <= 12.5"). All fields are pre-formatted strings so the builder is pure layout.
struct ClauseEdit {
	string label;      //!< the constraint as the user wrote it
	string suggestion; //!< the constraint after the minimal loosening
	string amount;     //!< magnitude of the loosening (formatted)
};

//! Build the infeasible diagnosis from the minimal edit list the elastic stage-1
//! solve produced (one clause per positive slack). The summary prescribes the
//! smallest change(s) that restore feasibility; each edit emits `suggested_change`
//! and `amount` rows keyed by the clause as written. Precondition: `edits`
//! non-empty (the caller falls through to the static error otherwise).
DecideDiagnostic BuildInfeasibleDiagnostic(const vector<ClauseEdit> &edits);

//! Build the diagnosis for the case where the elastic program itself is infeasible:
//! loosening the user's editable constraints cannot restore feasibility because the
//! conflict reaches rigid (structural/mechanism) rows. A distinct, honest outcome —
//! not a minimal edit list.
DecideDiagnostic BuildElasticInfeasibleDiagnostic();

//! Store `diag` on the connection so decide_diagnostics() can read it next statement.
void StashDecideDiagnostic(ClientContext &context, DecideDiagnostic diag);

//! Invalidate any stashed diagnosis on the connection. Called on a successful
//! solve so decide_diagnostics() does not keep reporting a now-resolved failure.
//! The per-connection id counter is left intact (ids stay monotonic across solves).
void ClearDecideDiagnostic(ClientContext &context);

//! Throw the short pointer error a failed-but-diagnosed DECIDE surfaces (the
//! relation itself is read via SELECT * FROM decide_diagnostics()).
[[noreturn]] void ThrowDecideDiagnosisReady(const DecideDiagnostic &diag);
[[noreturn]] void ThrowDecideDiagnosisReady(const DecideDiagnostic &diag,
                                            const string &extra_message);

//! Throw the unbounded error used when diagnosis WAS requested for an unbounded
//! solve but produced no per-variable content (e.g. a quadratic model, or a ray
//! that escaped only via internal auxiliaries). `reason` completes the sentence
//! "DECIDE optimization is unbounded: <reason> Add an upper bound, e.g. SUCH THAT
//! x <= <cap>." Distinct from the generic
//! ThrowDecideSolveError advert, which (correctly) tells a user who has NOT
//! enabled diagnosis to turn it on — useless advice once it is already on.
[[noreturn]] void ThrowUnboundedDiagnosisUnavailable(const string &reason);

//! Registers the decide_diagnostics() table function.
struct DecideDiagnosticsFun {
	static void RegisterFunction(BuiltinFunctions &set);
};

//===----------------------------------------------------------------------===//
// F4: the `diagnose_decide` setting (auto by default; off to suppress).
//===----------------------------------------------------------------------===//

//! Register the sticky `diagnose_decide` session setting. Modes: auto (default) /
//! off. Called once at DBConfig setup; the set-callback validates the mode so a
//! typo fails fast at SET time. Under auto a failed solve is diagnosed wherever an
//! engine exists; off reproduces the plain static solver error.
void RegisterDecideDiagnosticOptions(DBConfig &config);

//! Read the current diagnose_decide mode (lowercased; "auto" if unset).
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
