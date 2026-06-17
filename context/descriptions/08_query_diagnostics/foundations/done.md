# Query Diagnostics — Foundations (implemented)

## F1 · Structured solver result — DONE

The solve path now returns a structured result carrying the terminal status, so
callers can branch on the outcome instead of catching an exception. This is the
gate for the rest of the area.

- **`SolverResult` + `SolverStatus`** — new header
  `src/include/duckdb/decidb/solver_result.hpp`. `SolverStatus` =
  `{OPTIMAL, INFEASIBLE, UNBOUNDED, INF_OR_UNBD, TIME_LIMIT, ITERATION_LIMIT, OTHER}`
  (`:23`); `SolverResult { status, solution, ray, raw_status }` (`:37`). `ray` is
  populated by U2 only on the explicit diagnostic ray-extraction path; `raw_status`
  carries the backend-native code for the OTHER message.
- **Backends map-and-return instead of throwing on solver status.**
  `GurobiSolver::Solve` and `DeterministicNaive::Solve` now return `SolverResult`
  (signatures in their headers). The non-optimal status blocks became switch maps
  (`gurobi_solver.cpp` ~`:222-251`, `:274`; `deterministic_naive.cpp` ~`:207-237`,
  `:262`). **HiGHS `kUnboundedOrInfeasible` (status 9) → `INF_OR_UNBD`**
  (`deterministic_naive.cpp:220-224`) — previously dropped into a generic "solver
  status %d" catch-all. Genuine internal/API errors (NaN/Inf, extraction, API
  failures) still throw.
- **`SolveModel` facade returns `SolverResult`** and no longer throws on solver
  status (`ilp_solver.cpp:12`). The default user-facing error text — formerly
  duplicated verbatim across both backends — is now a single helper
  **`ThrowDecideSolveError(const SolverResult &)`** (`ilp_solver.cpp:39`, declared
  in `solver_result.hpp:53`).
- **Throw relocated to the operator (F4-ready).** `PhysicalDecide::Finalize`
  branches on status: non-optimal → `ThrowDecideSolveError` (manual-first); optimal
  → `gstate.ilp_solution = std::move(solve_result.solution)`
  (`physical_decide.cpp:5027-5034`). The F4 pragma will later gate that one call
  site to route the status into diagnosis instead of throwing.
- **Behavior unchanged for users:** same errors, same wording (the `INF_OR_UNBD`
  message already says "infeasible or unbounded"). Covered by dedicated F1
  regressions in `test/decide/tests/test_query_diagnostics_f1.py`: optimal
  solution extraction on both backends; HiGHS infeasible / unbounded /
  `INF_OR_UNBD` paths; and Gurobi infeasible / unbounded paths. The older
  HiGHS-pinned regression remains in
  `test/decide/tests/test_error_infeasible.py::test_highs_milp_unbounded_reports_unbounded`.
- **Deferred (not F1):** timeout incumbent / objective / best-bound / gap fields
  (slow branch, S2); the pragma gate (F4); HiGHS `time_limit` honoring (slow
  branch).

## F2 · Constraint provenance (row → clause) — DONE

Every emitted matrix row now carries where it came from, so diagnosis can report
at the user-clause level instead of at raw rows.

- **`ConstraintProvenance {clause_id, group_key, kind}`** added to `ModelConstraint`
  and `SolverModel::QuadraticConstraint`
  (`src/include/duckdb/decidb/ilp_model.hpp`). `clause_id` = index into
  `SolverInput::constraints` (`DConstants::INVALID_INDEX` for synthetic rows);
  `group_key` = PER/WHEN group id at emission (or row id for per-row), INVALID when
  ungrouped; `kind` = `ConstraintKind{USER, STRUCTURAL}`.
- **Stamped at all 7 builder fan-out sites** in
  `src/decidb/utility/ilp_model_builder.cpp`: linear aggregate-ungrouped /
  aggregate-PER / per-row, the raw global-constraint push (the only site stamped
  **STRUCTURAL**), and the three quadratic analogues. `clause_id` is derived by
  pointer arithmetic into `input.constraints` so it survives the `continue` skips.
- **Reverse index** `BuildClauseToRows(const SolverModel&)` → ordered
  `clause_id → [row positions]` over the linear matrix (structural rows excluded;
  quadratic rows carry provenance but aren't indexed yet).
- **`kind` scope boundary:** F2 stamps only the unambiguous split (USER on the
  EvaluatedConstraint paths, STRUCTURAL on the global-raw push). The **exhaustive**
  STRUCTURAL enumeration across McCormick / Big-M MIN-MAX / ABS / `<>` rewrite paths
  is **F3**, still open — do not assume `kind` is complete.
- Covered by `test/common/test_decidb_constraint_provenance.cpp` (aggregate / PER /
  per-row provenance, distinct clause ids, structural tagging, reverse-index
  round-trip).

## F4 · `PRAGMA diagnose_decide` (manual-first consent gate) — DONE

Sticky session setting selecting whether/which failed solve is diagnosed. Modes
`none`(default) / `infeasible` / `unbounded` / `slow` / `auto`. **Filter, not
force:** a mode acts only when the solve actually lands in that state.

- Registered as an **extension option** (no settings-codegen, no grammar change)
  via `RegisterDecideDiagnosticOptions(DBConfig&)`, called once from
  `DatabaseInstance::Configure` (`src/main/database.cpp`). `AddExtensionOption`'s
  set-callback validates the enum, so a typo fails fast at SET time. Read back with
  `ClientContext::TryGetCurrentSetting`. Works with both `PRAGMA diagnose_decide=…`
  and `SET diagnose_decide=…`; `RESET` restores `none`.
- Helpers in `src/decidb/utility/decide_diagnostic.cpp`: `GetDiagnoseDecideMode`,
  `DiagnosisApplies(mode, status)` (filter predicate), `DiagnoseModeWantsUnboundedRay`.
- **Gate** wired into `PhysicalDecide::Finalize`
  (`src/execution/operator/decide/physical_decide.cpp`): read the mode before the
  solve and pre-arm `SolveModelOptions::extract_unbounded_ray` only for
  unbounded/auto (manual-first — default pays nothing); on a non-optimal result,
  if the mode matches the status **and an engine exists** (this session: UNBOUNDED
  only) build+stash the diagnosis and throw the short pointer error, otherwise fall
  through to the unchanged `ThrowDecideSolveError`. Matched-but-unimplemented states
  (`infeasible`/`slow`) fall through until their engines land.
- Covered by `test/decide/tests/test_query_diagnostics_f4.py` (both backends):
  default `none` reproduces the F1 error; valid modes accepted; invalid mode
  rejected; filter scoping (unbounded mode ignores an infeasible solve and vice
  versa); `auto` routes unbounded.

## F5 · Shared diagnostic reporting surface — DONE

A structured diagnosis a state engine populates, stashed per-connection, surfaced
as a fixed-schema relation.

- **`DecideDiagnostic` / `DiagnosticRow` / `DecideDiagnosticState`**
  (`src/include/duckdb/decidb/decide_diagnostic.hpp`). The state is a
  `ClientContextState` stashed under key `decide_diagnostics`; the failing DECIDE
  mutates it before throwing, so it survives into the next statement on the same
  connection.
- **`decide_diagnostics()` table function** (registered with one line in
  `src/function/table/system_functions.cpp`, implemented in
  `src/decidb/utility/decide_diagnostic.cpp`) with fixed schema
  `(query_id BIGINT, state, variable, direction, group_label, suggested_bound)` —
  `query_id` is BIGINT, the rest VARCHAR; empty string row fields render as SQL NULL.
  The schema is **variable-centric** (one row = one escaping variable): `variable`
  and `group_label` are names (never internal ids), `direction` is the escape
  direction, and `query_id` ties together every row of one failed solve.
  **Output-surface decision:** a runtime result-schema switch is bind-time-blocked
  (DECIDE is a clause on SELECT; the projection binds the decision columns before the
  solve runs — `logical_decide.cpp:23,34`, `transform_select_node.cpp:107`), so the
  diagnosis is surfaced via this companion table function instead.
- **Unbounded content (current):** `BuildUnboundedDiagnostic` emits one row per
  escaping variable carrying its **name** (F6) and the **direction** it escapes (the
  sign of its ray entry; `+∞` in practice, since user vars are bounded below at 0).
  `group_label` and `suggested_bound` are reserved for later enrichment and read NULL
  for now. `query_id` is stamped at stash time from a per-connection counter
  (`DecideDiagnosticState::next_query_id`).
- **Deferred (with the user):** `suggested_bound` (a concrete cap value — left NULL
  to avoid anchoring on a bad number) and `group_label` name resolution (the
  escaping-instance's PER/WHEN group), plus the slack→Δ conversion for the infeasible
  engine. None have a producer yet, so building them now would be speculative.
- Covered by `test/decide/tests/test_query_diagnostics_f5.py` (both backends):
  failing DECIDE + follow-up `SELECT * FROM decide_diagnostics()` returns the named
  row (read as CSV); fixed schema via DESCRIBE; empty when nothing diagnosed / no
  pragma.
- **Note — schema reshaped around unbounded:** the original shared 5-tuple
  `(state, clause, group_key, edit_kind, suggested_change)` was replaced by this
  variable-centric schema. The future infeasible/slow engines, whose subject is a
  *clause* not a *variable*, will need to revisit these columns (see
  `infeasible/todo.md`).

## F6 · Variable provenance (column-side) — DONE

The column-side complement of F2: map every solver column back to the user-facing
thing it represents, so the unbounded diagnosis can **name the escaping variables**
instead of emitting the generic scaffold. Per the "full output" decision this slice
also folded in U3's consuming half (ray → named rows).

- **Aux→source-expression capture (the bulk).** New
  `LogicalDecide::aux_var_expressions` (`vector<pair<idx_t, string>>`,
  `logical_decide.hpp`) maps an auxiliary variable's index in `decide_variables` to
  the user's original expression string. Stamped at the 4 aux-creation sites in
  `src/optimizer/decide/decide_optimizer.cpp` (where the source expression is still
  in hand): `<>` indicator (`comp.left <> comp.right`), MIN/MAX indicator
  (`MAX(inner)`), ABS aux (`ABS(inner)`), bilinear aux (`(b * x)`). Threaded
  LogicalDecide → PhysicalDecide in `plan_decide.cpp` (alongside the sibling aux
  metadata) and serialized as property `232`.
- **`ColumnProvenance` + `BuildColumnProvenance`** (`ilp_model.hpp` /
  `ilp_model_builder.cpp`, next to `ConstraintProvenance` / `BuildClauseToRows`).
  `ColumnKind{USER, AUX, GLOBAL_AUX}`; the builder is **pure** (no Expression
  dependency — caller pre-extracts per-decide-var labels + an is-aux flag) and
  inverts `VarIndexer::Get(var, row)` over all rows to produce a `flat column →
  ColumnProvenance` map sized `indexer.total_vars`. Global-block columns default to
  GLOBAL_AUX (unnamed). Mirrors the solution-readback iteration in
  `physical_decide.cpp`.
- **Naming wired into `BuildUnboundedDiagnostic`** (now takes `(result, columns)`).
  At the `PhysicalDecide::Finalize` diagnosis site, per-decide-var labels are built
  (user → `decide_variables[i]->GetName()`, aux → its captured expression) and run
  through `BuildColumnProvenance`. The diagnosis collects ray columns with
  `|ray[i]| > 1e-8`, resolves each via the map, **dedups by name** (a row-scoped
  `x` escaping across every row reports once), and emits one row per escaping
  variable carrying its **name** and escape **direction** (the sign of its ray
  entry). Falls back to a single detail-less row when no ray is attached (quadratic
  models — U2 extracts none) or only an unnamed global aux escaped (which it flags on
  the stderr summary as a likely model-generation issue). **Never picks the bound.**
- **Empirical scope (verified on both backends):** in current DECIDE formulations
  **only user INTEGER/REAL variables actually escape**. Auxiliary variables are
  structurally bounded — ABS Big-M and bilinear McCormick require finite bounds (they
  error *before* the solver) and MIN/MAX/`<>` indicators are BOOLEAN `[0,1]`. So the
  aux→expression naming is **defensive infrastructure** (it correctly names an aux if
  one ever does escape, e.g. a future feature or a model-gen bug); the practical
  escapers are user vars. This matches P7's "narrow suspects for free": the U2 box-LP
  ray already fixes finite-UB columns to 0, so a non-zero ray entry *is* the filtered
  suspect set — no extra type/sign/bound filter needed.
- Covered by `test/common/test_decidb_variable_provenance.cpp` (the pure builder:
  USER / AUX / GLOBAL_AUX resolution, each linearization-aux kind, entity-scoped
  column collapse) and `test/decide/tests/test_query_diagnostics_f6.py` (both
  backends: REAL var, INTEGER/MILP via the HiGHS `INF_OR_UNBD` path, multi-var
  dedup, `auto` routing, manual-first no-pragma silence).

## Baseline for the remaining foundations (F3)

- **`ModelConstraint` provenance** — landed (F2 above). F3 still owes the exhaustive
  STRUCTURAL stamping across the linearization rewrite paths.
- **HiGHS sets no time limit** (Gurobi reads `DECIDB_TIME_LIMIT`, 300s default,
  `gurobi_solver.cpp:70-81`). Honoring it on HiGHS is a slow-branch item, **not**
  F1.
