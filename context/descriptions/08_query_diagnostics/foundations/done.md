# Query Diagnostics — Foundations (how it works)

Shared plumbing every diagnosis state consumes. This doc describes the shipped
infrastructure, topic by topic. Remaining foundation work is in `todo.md`.

## Structured solver result

The solve path returns a structured result instead of throwing on solver status,
so callers branch on the outcome. This gates the whole area.

- **`SolverResult` + `SolverStatus`** (`src/include/duckdb/decidb/solver_result.hpp`).
  `SolverStatus = {OPTIMAL, INFEASIBLE, UNBOUNDED, INF_OR_UNBD, TIME_LIMIT,
  ITERATION_LIMIT, OTHER}`; `SolverResult { status, solution, ray, raw_status }`.
  `ray` is populated only on the unbounded ray-extraction path; `raw_status` carries
  the backend-native code for the `OTHER` message.
- **Backends map-and-return** instead of throwing on solver status
  (`gurobi_solver.cpp`, `deterministic_naive.cpp`). HiGHS `kUnboundedOrInfeasible`
  (status 9) maps to `INF_OR_UNBD`. Genuine internal/API errors (NaN/Inf,
  extraction, API failures) still throw.
- **The `SolveModel` facade** returns `SolverResult` and no longer throws on solver
  status (`ilp_solver.cpp`). The default user-facing error text is a single helper,
  `ThrowDecideSolveError(const SolverResult &)`.
- **Pre-solve model-builder infeasibility is normalized.** Contradictory normalized
  rows and contradictory accumulated bounds throw `DecideInfeasibleModelException`
  inside `SolverModel::Build`; `SolveModel` catches that internal exception and
  returns `SolverResult{status = INFEASIBLE}`. A fast contradiction such as
  `x >= 5 AND x <= 1` therefore reaches the same `PhysicalDecide::Finalize` gate as
  backend-reported infeasibility, instead of bypassing diagnostics with a direct
  builder error.
- **The throw lives in the operator.** `PhysicalDecide::Finalize`
  (`physical_decide.cpp`) branches on status: optimal → store the solution;
  non-optimal → the pragma gate decides whether to diagnose or call
  `ThrowDecideSolveError`. This is the single gated call site.

Behavior is unchanged for users with no pragma — same errors, same wording.

## Constraint provenance (row → clause)

Every emitted matrix row records where it came from, so diagnosis reports at the
user-clause level instead of raw rows.

- **`ConstraintProvenance {clause_id, group_key, kind}`** on `ModelConstraint` and
  `SolverModel::QuadraticConstraint` (`src/include/duckdb/decidb/ilp_model.hpp`).
  `clause_id` indexes `SolverInput::constraints` (`INVALID_INDEX` for synthetic
  rows); `group_key` is the PER/WHEN group id at emission (or row id for per-row,
  INVALID when ungrouped); `kind` is the row role consumed by the future elastic
  engine.
- **Single row-role enum:** `ConstraintKind = { USER_PARAMETER, USER_MECHANISM,
  STRUCTURAL }` (`duckdb/common/enums/decide.hpp`). `USER_PARAMETER` carries a
  user-editable parameter/RHS and is relaxable. `USER_MECHANISM` is a rigid helper
  row attached to a user clause (for example generated `<>` and hard MIN/MAX
  mechanism rows). `STRUCTURAL` is a synthesized definition/linking row. The
  elastic predicate is one equality: `IsRelaxableForElastic(kind)` is true only for
  `USER_PARAMETER`.
- **Stamped at every builder fan-out site** in `ilp_model_builder.cpp` (linear
  aggregate-ungrouped / aggregate-PER / per-row, the raw global push, and the three
  quadratic analogues). `clause_id` is derived by pointer arithmetic into
  `input.constraints` so it survives `continue` skips. Evaluated constraints default
  to `USER_PARAMETER` unless the planner/operator stamps a rigid role; raw generated
  constraints default to `STRUCTURAL` unless explicitly marked as user mechanism or
  user parameter.
- **Rigid rewrite rows are stamped at the source.** The optimizer tags structural
  rewrites that re-enter constraint parsing with `STRUCTURAL_CONSTRAINT_TAG`, and
  `PhysicalDecide` stamps generated McCormick / ABS envelope rows as `STRUCTURAL`;
  generated `<>` and hard MIN/MAX mechanism rows as `USER_MECHANISM`; and composed
  MIN/MAX outer user pins as `USER_PARAMETER`.
- **`BuildClauseRowIndex(const SolverModel&)`** is the reverse index. It returns
  `ClauseRowIndex { by_clause, by_clause_group }`, where each value is a list of
  `ConstraintRowRef { type, index }`. `type` distinguishes linear matrix rows from
  quadratic rows; `by_clause_group` keys `(clause_id, group_key)` for PER/per-row
  elastic slack placement.

Tested in `test/common/test_decidb_constraint_provenance.cpp`.

## Variable provenance (column → name / instance)

The column-side complement: map every solver column back to the user-facing thing
it represents, so the unbounded diagnosis names escaping variables.

- **Aux→source-expression capture.** `LogicalDecide::aux_var_expressions` maps an
  auxiliary variable's index to the user's original expression string, stamped at
  the four aux-creation sites in `decide_optimizer.cpp` (`<>` indicator, MIN/MAX
  indicator, ABS aux, bilinear aux) and threaded to `PhysicalDecide`.
- **`ColumnProvenance` + `BuildColumnProvenance`** (`ilp_model.hpp` /
  `ilp_model_builder.cpp`). `ColumnKind{USER, AUX, GLOBAL_AUX}`. The builder is
  pure (caller pre-extracts per-decide-var labels + an is-aux flag) and inverts
  `VarIndexer::Get(var, row)` over all rows to produce a `flat column →
  ColumnProvenance` map. The provenance retains the variable's **instance**
  identity (entity id for entity-scoped, row for row-scoped) — the hook the
  unbounded `escaping_instances` characterization resolves to a categorical rule
  set (`unbounded/done.md`). Global-block columns default to GLOBAL_AUX (unnamed).

Tested in `test/common/test_decidb_variable_provenance.cpp`.

## The `diagnose_decide` pragma (consent gate)

Sticky session setting selecting whether/which failed solve is diagnosed. Modes
`none` (default) / `infeasible` / `unbounded` / `slow` / `auto`. **Filter, not
force:** a mode acts only when the solve actually lands in that state, so a
left-on pragma is harmless and `auto` doesn't violate manual-first (setting the
pragma *is* the opt-in).

- Registered as an **extension option** via `RegisterDecideDiagnosticOptions(DBConfig&)`
  (called from `DatabaseInstance::Configure`) — no settings-codegen, no grammar
  change. The set-callback validates the enum, so a typo fails fast at SET time.
  Works with `PRAGMA diagnose_decide=…` and `SET diagnose_decide=…`; `RESET`
  restores `none`.
- Helpers in `decide_diagnostic.cpp`: `GetDiagnoseDecideMode`,
  `DiagnosisApplies(mode, status)`, `DiagnoseModeWantsUnboundedRay`.
- **The gate** in `PhysicalDecide::Finalize`: read the mode before the solve,
  pre-arm `SolveModelOptions::extract_unbounded_ray` only for unbounded/auto
  (default pays nothing); on a non-optimal result, if the mode matches the status
  *and an engine exists* (today: UNBOUNDED only) build + stash the diagnosis and
  throw the short pointer error, else fall through to `ThrowDecideSolveError`.
  Matched-but-unimplemented states (infeasible/slow) fall through until their
  engines land.

Tested in `test/decide/tests/test_query_diagnostics_f4.py` (both backends).

## Diagnosis engine seam

State-specific diagnosis logic lives behind free-function engines rather than as
inline branches in `PhysicalDecide::Finalize`.

- **Unbounded engine:** `DiagnoseUnbounded(const UnboundedDiagnosisInput&)`
  (`src/include/duckdb/decidb/decide_diagnostic_engines.hpp`,
  `src/decidb/utility/decide_diagnostic_engines.cpp`). The input carries the
  `SolverResult`, `VarIndexer`, user labels, aux flags, diagnostic params, and an
  injected `get_candidates(decide_var_idx, total_instances)` callback for categorical
  groupings.
- **Boundary:** the engine is free of DuckDB execution/operator types. It reads the
  ray, maps columns through variable provenance, groups escaping instances by
  DECIDE variable, asks the callback for row/entity categorical candidates, and
  returns a `DecideDiagnostic` only when it produced named variable content.
- **`Finalize` owns orchestration:** read mode, pre-arm unbounded ray extraction,
  call the matching engine for the status, stash + throw only when the engine
  returns a valid diagnosis, otherwise fall through to `ThrowDecideSolveError`.
  The success path still clears the per-connection stash.

Tested in `test/common/test_decidb_diagnostic_engines.cpp`.

## The `decide_diagnostics()` reporting relation

A structured diagnosis a state engine populates, stashed per-connection, surfaced
as a fixed-schema relation.

- **`DecideDiagnostic` / `DiagnosticRow` / `DecideDiagnosticState`**
  (`src/include/duckdb/decidb/decide_diagnostic.hpp`). The state is a
  `ClientContextState` stashed under key `decide_diagnostics`; the failing DECIDE
  mutates it before throwing, so it survives into the next statement **on the same
  connection**. **Lifecycle:** the stash holds *the most recent diagnosed failure
  on the connection, until the next successful solve clears it.* A successful
  DECIDE (`OPTIMAL` in `PhysicalDecide::Finalize`) calls `ClearDecideDiagnostic`,
  which invalidates `latest` (so `decide_diagnostics()` returns 0 rows) — a stale
  diagnosis never lingers after the user fixes the query. The per-connection id
  counter is left intact so ids stay monotonic across solves.
- **`decide_diagnostics()` table function** (registered in
  `system_functions.cpp`, implemented in `decide_diagnostic.cpp`) with fixed schema
  `(diagnosis_id BIGINT, state, subject_kind, subject, attribute, value)`. The
  string columns are VARCHAR; empty string fields render as SQL NULL.
  `diagnosis_id` is stamped at stash time from a per-connection counter and ties
  all rows from one diagnosed failure together.
- **Long-form EAV surface:** `subject_kind` names the entity type (`variable` today,
  `clause` for infeasible later); `subject` is the state-engine-owned identifier;
  `attribute`/`value` carry that engine's facts. This keeps the table function
  schema stable as new states add their own attributes.
- **Why a table function and not a result-schema switch:** DECIDE is a clause on
  SELECT, and the projection binds the decision columns before the solve runs
  (`logical_decide.cpp`, `transform_select_node.cpp`), so a runtime result-schema
  switch is bind-time-blocked. The diagnosis is surfaced via this companion table
  function instead.
- **Today only the unbounded engine populates it** (`BuildUnboundedDiagnostic` — see
  `unbounded/done.md`). It emits one `direction` row for every escaping variable
  and an `escaping_instances` row only when there is instance multiplicity to
  explain (categorical rules / total-escape / count). The forced remedy (add a
  bound) is prescribed in the stderr summary, not a per-row attribute. The
  unbounded characterization adds
  three sticky extension options alongside `diagnose_decide`:
  `diagnose_decide_escape_rate`, `diagnose_decide_categorical_ratio`,
  `diagnose_decide_min_categories` (all in `RegisterDecideDiagnosticOptions`).

Tested in `test/decide/tests/test_query_diagnostics_f5.py` (both backends) and
`test/common/test_decidb_diagnostic_engines.cpp` (a clause-shaped stub row).
