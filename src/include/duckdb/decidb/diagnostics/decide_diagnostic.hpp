//===----------------------------------------------------------------------===//
//                         DecidB
//
// duckdb/decidb/diagnostics/decide_diagnostic.hpp
//
// The shared, structured diagnostic reporting surface. A state engine (unbounded or
// infeasible) populates a DecideDiagnostic, the DECIDE operator places it in a
// statement-scoped handoff, and the DIAGNOSE operator above reads it back as a flat,
// fixed-schema relation. Nothing else
// starts an engine: a query with no DIAGNOSE prefix never builds one of these. See
// context/descriptions/07_query_diagnostics/.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/decidb/solver/solver_result.hpp"
#include "duckdb/main/client_context_state.hpp"

#include <set>

namespace duckdb {

class ClientContext;
class DataChunk;
class DBConfig;
struct SolverResult;
struct ColumnProvenance;

//! One row of the relation DIAGNOSE returns: one finding, real columns, real types.
//! Empty strings and the `has_*` flags render as SQL NULL.
//!
//! The relation is
//!   state | clause | suggested_change | amount | total | scope | edit_source | group | row
//! and `state` is carried once by the owning DecideDiagnostic, not per finding.
struct DiagnosticFinding {
	//! The clause exactly as the user wrote it (`x <= 10`, `SUM(buy * price) <= 500`),
	//! or the runaway variable's name for an unbounded finding. Empty => NULL for a
	//! model-level finding that implicates no single clause.
	string clause;
	//! The smallest edit that addresses this finding, in the user's own language: the
	//! clause re-quoted after loosening (`x <= 12.5`), the SUCH THAT fragment to add
	//! (`x <= <cap>`), or a plain instruction when there is no re-quotable text
	//! ("remove this clause"). Empty => NULL.
	string suggested_change;
	//! Size of the edit: how far a bound moves, how many row/entity instances a variable
	//! escapes on, or the achievable objective. `has_amount` false => NULL.
	bool has_amount = false;
	double amount = 0.0;
	//! Denominator for a counted unbounded escape. For a categorical slice this is
	//! the number of instances in that slice; for a whole-variable/fallback count it
	//! is the variable's total instance count. `has_total` false => NULL.
	bool has_total = false;
	int64_t total = 0;
	//! Kind of instance counted by `amount` / `total`: `row` or `entity` for an
	//! unbounded escape count. Empty => NULL for findings without an instance count.
	string scope;
	//! What KIND of finding this row is, and where it came from. One vocabulary, so a
	//! query can filter on it:
	//!   source_literal      a literal the user wrote, loosened in place
	//!   virtual_offset      a synthetic offset over a data-backed RHS (`x <= col + d`)
	//!   expanded_row        one emitted row's own overshoot (`expanded` scope)
	//!   expanded_group      one PER group's own overshoot (`expanded` scope)
	//!   remove_only         a `<>` that cannot be loosened, only deleted
	//!   unreachable_bound   a bound no assignment can reach
	//!   rigid_conflict      loosening the editable clauses cannot restore feasibility
	//!   runaway_+inf        an unbounded decision growing toward +infinity
	//!   runaway_-inf        ... toward -infinity
	//!   achievable_objective  what the objective reaches once the edits are applied
	//!   unbounded_after_fix   the repaired problem has no finite optimum
	//!   undiagnosed         the state is known but the engine could not name a cause
	string edit_source;
	//! Printable PER key of this finding's group (`EU, 2024`), or the categorical slice
	//! an unbounded variable escapes on (`region = 'EU'`). Empty => NULL.
	string group;
	//! Identity of the single emitted row this finding covers, under the `expanded`
	//! slack scope. `has_row` false => NULL, which is every finding under `query` scope.
	bool has_row = false;
	int64_t row = 0;
};

//! One categorical "sufficient-direction" rule for an escaping variable: among the
//! `total` instances where `column = value`, `escaping` of them escape. Reported
//! when escaping/total ≥ the escape-rate threshold.
struct EscapeRule {
	string column;
	string value;
	idx_t escaping = 0; //!< a
	idx_t total = 0;    //!< b (group size, counted in instances)
	//! True when this rule is part of a set that accounts for EVERY escaping instance
	//! of the variable, each rule of it escaping wholly (rate 1.0). Only then can the
	//! prescribed cap be scoped to the escaping rows instead of the whole relation:
	//! a rule missing an escaper elsewhere would leave the program unbounded. Set
	//! together on the whole covering set, or on nothing.
	bool covers_scope = false;
};

//! A categorical column's grouping over a variable's instances, fed to
//! CharacterizeEscape. Pure data (no DuckDB execution types) so the core unit-tests.
struct ColumnGrouping {
	string column;                  //!< column name (for the rule)
	vector<idx_t> instance_to_group; //!< size = total_instances; group id per
	                                 //!< instance (INVALID_INDEX = excluded)
	vector<string> group_value;      //!< size = num_groups; value label per group
	//! True when the DECIDE clause references this column. Only used to break a tie
	//! between columns that describe exactly the same escaping instances.
	bool clause_referenced = false;
};

//! Direction in which an unbounded decision variable escapes. Keep this typed across
//! the engine/builder boundary so presentation spellings cannot disagree.
enum class EscapeDirection : uint8_t { POSITIVE, NEGATIVE };

//! Per-variable escape characterization, assembled by the operator and formatted
//! by BuildUnboundedDiagnostic into its findings.
struct VarEscape {
	string name;             //!< user variable name (USER) or source expr (AUX)
	EscapeDirection direction = EscapeDirection::POSITIVE;
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
	//! Infeasible slack-scope policy (T3). "query" (default): one slack per SQL-level
	//! knob — a data-backed RHS (`x <= col`) reports a virtual query offset
	//! (`x <= col + delta`) plus a conflict profile row. "expanded": one slack per emitted
	//! relaxable row/group — a diagnostic profile, not a directly pasteable SQL edit.
	string slack_scope = "query";
};

//! Structured diagnosis produced by a state engine and rendered by the DIAGNOSE
//! operator. Shared across all diagnosis states so output stays consistent.
struct DecideDiagnostic {
	bool valid = false; //!< false => nothing diagnosed
	//! "infeasible" / "unbounded" / "feasible". The whole `state` column, in one place:
	//! every finding of one diagnosis shares it.
	string state;
	vector<DiagnosticFinding> findings;
};

//! Statement-scoped handoff backed by ClientContextState. The DECIDE operator writes it
//! during Finalize; the DIAGNOSE operator sitting above it in the same plan consumes it
//! once the child pipeline is done. Written ONLY under DIAGNOSE — an unprefixed query
//! never diagnoses, and the value is cleared on read.
class DecideDiagnosticState : public ClientContextState {
public:
	DecideDiagnostic latest;
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

//! Build the unbounded diagnosis from the per-variable characterizations the operator
//! assembled. One finding per escaping variable, or one per categorical slice when the
//! escape is characterized: `clause` names the variable, `suggested_change` prescribes
//! the forced remedy without inventing the cap (`x <= <cap>`), `edit_source` carries the
//! direction (`runaway_+inf` / `runaway_-inf`), `group` names the slice (`region = 'EU'`)
//! and `amount` / `total` give the escaping and considered instance counts. `scope`
//! says whether those instances are rows or entities. Precondition: `escapes` is
//! non-empty; the caller reports the bare status when the ray names nothing (quadratic
//! model, or only internal auxiliaries).
DecideDiagnostic BuildUnboundedDiagnostic(const vector<VarEscape> &escapes);

//! How an elastic edit reads back to the user.
//!   LOOSEN          — an editable knob: loosen `label` to `suggestion` by `amount`.
//!                     Covers the I1/I2.a/b literal shapes (plus AVG/strict/quadratic),
//!                     the T3 query-mode virtual offset over a data RHS
//!                     (`x <= col + delta`), and the T3 expanded-mode per-row profile
//!                     entries. `edit_source` distinguishes these.
//!   DROP            — a remove-only clause (`<>`) cannot be loosened, only removed;
//!                     `label` names the clause and the fix is to delete it (I4).
enum class ClauseEditKind : uint8_t { LOOSEN, DROP };

//! One least-change edit the infeasible (elastic) engine found. For LOOSEN: loosen
//! the constraint as written (`label`, e.g. "x <= 10") to `suggestion` (e.g.
//! "x <= 12.5") by `amount`. For DROP: `label` names the remove-only clause to delete
//! (other fields unused). All fields are pre-formatted strings so the builder is pure
//! layout.
struct ClauseEdit {
	ClauseEditKind kind = ClauseEditKind::LOOSEN;
	string label;      //!< the constraint as the user wrote it
	string suggestion; //!< the constraint after the minimal loosening (LOOSEN only)
	bool has_amount = false; //!< LOOSEN only; false => the `amount` column is NULL
	double amount = 0.0;     //!< magnitude of the loosening, in the user's own units
	string group;      //!< printable PER key of this edit's group (empty if ungrouped),
	                   //!< so folded SUM clauses stay distinguishable in the relation
	//! Identity of the single emitted row this edit covers, under the `expanded` slack
	//! scope. INVALID_INDEX (the `query`-scope case) => the `row` column is NULL.
	idx_t row = DConstants::INVALID_INDEX;
	//! T3 slack-scope provenance, and the `edit_source` column verbatim: how the edit
	//! was derived — `source_literal` (a literal the user wrote), `virtual_offset` (a
	//! query-mode synthetic offset over a data RHS, `x <= col + delta`), `expanded_row` /
	//! `expanded_group` (an expanded-mode per-row / per-group profile entry). A DROP
	//! reports `remove_only`. Empty => the column is NULL.
	string edit_source;
};

//! Build the infeasible diagnosis from the minimal edit list the elastic engine
//! produced: exactly one finding per edit, keyed by the clause as written. A LOOSEN
//! edit fills `suggested_change` / `amount`; a DROP names the clause and asks for its
//! removal (`edit_source = 'remove_only'`).
//!
//! `achievable_objective` (I3, the stage-2 freeze-budget re-solve): when non-empty,
//! appends one model-level finding, `edit_source = 'achievable_objective'`, carrying
//! the value in `amount`. `unbounded_after_fix` overrides it with a
//! `unbounded_after_fix` finding: the relaxed problem has no finite optimum. Both
//! default off (used by the unchanged data-RHS-only path).
//! Precondition: `edits` non-empty (the caller emits an `undiagnosed` finding otherwise).
DecideDiagnostic BuildInfeasibleDiagnostic(const vector<ClauseEdit> &edits,
                                           const string &achievable_objective = "",
                                           bool unbounded_after_fix = false);

//! Build the diagnosis for the case where the elastic program itself is infeasible:
//! loosening the user's editable constraints cannot restore feasibility because the
//! conflict reaches rigid (structural/mechanism) rows. A distinct, honest outcome —
//! not a minimal edit list.
DecideDiagnostic BuildElasticInfeasibleDiagnostic();

//! One user clause whose bound no assignment can reach (`x >= inf`). A pre-solve
//! finding, so there is no slack to read and no edit to suggest — only the clause to
//! name. `group` carries the printable PER key when the clause is grouped.
struct UnreachableClause {
	string label; //!< the constraint as the user wrote it
	string group; //!< printable PER key of this clause's group (empty if ungrouped)
};

//! Build the diagnosis for a bound that is out of reach for every assignment
//! (`x >= inf`, `SUM(x) >= inf`, `MIN(x) <= -inf`). Distinct from the elastic edit
//! list: the row is infeasible on its own, no other constraint is implicated, and no
//! finite loosening reaches it — so the diagnosis names the clause and stops rather
//! than quoting the user's own text back as a suggested change.
//! Precondition: `clauses` non-empty.
DecideDiagnostic BuildUnreachableBoundDiagnostic(const vector<UnreachableClause> &clauses);

//! User-facing reason for an unbounded solve whose diagnosis could not produce a
//! named runaway variable. The physical operator computes the booleans from the
//! retained solve state; this helper keeps the precedence unit-testable:
//! timeout first, then non-linear empty-ray limitation, then neutral empty-ray,
//! then internal-helper escape.
string BuildUnboundedDiagnosisUnavailableReason(bool diagnostic_timed_out, bool ray_empty,
                                                bool has_nonlinear_terms);

//! Build the one-finding diagnosis for a failure whose state is known but whose cause
//! no engine could name: a quadratic model with no ray, a ray that escaped only through
//! internal auxiliaries, a diagnostic solve that ran out of time. `reason` is the
//! plain-language explanation, and lands in `suggested_change` under
//! `edit_source = 'undiagnosed'`.
DecideDiagnostic BuildUndiagnosedDiagnostic(const string &state, const string &reason);

//! Build the diagnosis a query that SOLVED returns under DIAGNOSE: exactly one finding,
//! `state = 'feasible'`, every other column NULL. There is no separate output path for
//! a query that worked.
DecideDiagnostic BuildFeasibleDiagnostic();

//! The columns of the relation DIAGNOSE returns. One definition, read by the logical
//! operator (to resolve its types) and by the physical operator (to fill the chunk).
void GetDecideDiagnoseSchema(vector<string> &names, vector<LogicalType> &types);

//! Write `diag` into `output` starting at finding `offset`, advancing it past what fit.
//! The single renderer of the DIAGNOSE relation.
void RenderDecideDiagnostic(const DecideDiagnostic &diag, idx_t &offset, DataChunk &output);

//! Store `diag` in the statement-scoped handoff so the DIAGNOSE operator above this
//! DECIDE can consume it once the child pipeline finishes. Written only under DIAGNOSE.
void StashDecideDiagnostic(ClientContext &context, DecideDiagnostic diag);

//! Invalidate any waiting diagnosis, so a DIAGNOSE can never report a finding belonging
//! to an earlier statement.
void ClearDecideDiagnostic(ClientContext &context);

//! Read back (and consume) the diagnosis the DECIDE operator handed off during this
//! statement. An invalid result means the solve succeeded and nothing was diagnosed.
DecideDiagnostic TakeDecideDiagnostic(ClientContext &context);

//===----------------------------------------------------------------------===//
// Engine TUNING settings. Nothing here starts a diagnosis — only the DIAGNOSE
// prefix does that. These configure how the engine works once it is running.
//===----------------------------------------------------------------------===//

//! Register the sticky DECIDE session settings: the unbounded-characterization knobs,
//! the infeasible slack scope and the L0 tolerance. Called once at
//! DBConfig setup; each set-callback validates its value so a typo fails fast at SET
//! time.
void RegisterDecideDiagnosticOptions(DBConfig &config);

//! Read the L0 (norm(e, 0)) nonzero threshold from the session settings (default
//! 1e-4). Clamped to the floor (>= 1e-5) so the indicator link is always enforced.
double GetDecideL0Tolerance(ClientContext &context);

//! Read the unbounded-characterization knobs (escape rate / categorical ratio /
//! min-categories floor) from the session settings, falling back to defaults.
DecideDiagParams GetDecideDiagnosticParams(ClientContext &context);

//! Filter predicate: is there an engine for a solve that ended in `status`, and was a
//! diagnosis asked for? `armed` is the statement's DIAGNOSE prefix — nothing else.
bool DiagnosisApplies(bool armed, SolverStatus status);

} // namespace duckdb
