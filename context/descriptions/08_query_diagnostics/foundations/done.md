# Query Diagnostics — Foundations (how it works)

Shared plumbing every diagnosis state consumes. This doc describes the shipped
infrastructure, topic by topic. Remaining foundation work (F3 relaxability
tagging) is in `todo.md`.

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
  INVALID when ungrouped); `kind = ConstraintKind{USER, STRUCTURAL}`.
- **Stamped at every builder fan-out site** in `ilp_model_builder.cpp` (linear
  aggregate-ungrouped / aggregate-PER / per-row, the raw global push, and the three
  quadratic analogues). `clause_id` is derived by pointer arithmetic into
  `input.constraints` so it survives `continue` skips.
- **`BuildClauseToRows(const SolverModel&)`** is the reverse index: `clause_id →
  [row positions]` over the linear matrix.
- **`kind` scope:** the unambiguous split is stamped (USER on the evaluated-constraint
  paths, STRUCTURAL on the global-raw push). The exhaustive STRUCTURAL enumeration
  across the linearization rewrite paths is F3 (`todo.md`) — `kind` is not yet
  complete.

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

## The `decide_diagnostics()` reporting relation

A structured diagnosis a state engine populates, stashed per-connection, surfaced
as a fixed-schema relation.

- **`DecideDiagnostic` / `DiagnosticRow` / `DecideDiagnosticState`**
  (`src/include/duckdb/decidb/decide_diagnostic.hpp`). The state is a
  `ClientContextState` stashed under key `decide_diagnostics`; the failing DECIDE
  mutates it before throwing, so it survives into the next statement **on the same
  connection**.
- **`decide_diagnostics()` table function** (registered in
  `system_functions.cpp`, implemented in `decide_diagnostic.cpp`) with fixed schema
  `(query_id BIGINT, state, variable, direction, escaping_instances, suggested_bound)`
  — `query_id` BIGINT, the rest VARCHAR; empty string fields render as SQL NULL. The
  schema is **variable-centric** (one row = one escaping variable). `query_id` is
  stamped at stash time from a per-connection counter.
- **Why a table function and not a result-schema switch:** DECIDE is a clause on
  SELECT, and the projection binds the decision columns before the solve runs
  (`logical_decide.cpp`, `transform_select_node.cpp`), so a runtime result-schema
  switch is bind-time-blocked. The diagnosis is surfaced via this companion table
  function instead.
- **Today only the unbounded engine populates it** (`BuildUnboundedDiagnostic` — see
  `unbounded/done.md`). `escaping_instances` characterizes which instances of a
  variable escape (categorical rules / total-escape / count); `suggested_bound`
  reads NULL (DeciDB never picks the bound). The unbounded characterization adds
  three sticky extension options alongside `diagnose_decide`:
  `diagnose_decide_escape_rate`, `diagnose_decide_categorical_ratio`,
  `diagnose_decide_min_categories` (all in `RegisterDecideDiagnosticOptions`).

Tested in `test/decide/tests/test_query_diagnostics_f5.py` (both backends).

**Schema is currently shaped around unbounded.** The infeasible/slow engines,
whose subject is a *clause* not a *variable*, will need to revisit these columns
(re-add `clause`/`edit_kind`/`suggested_change`, or generalize the relation) before
they can report — see `infeasible/todo.md`.
