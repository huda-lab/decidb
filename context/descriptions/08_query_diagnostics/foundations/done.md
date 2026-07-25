# Query Diagnostics — Foundations (how it works)

Shared plumbing every diagnosis state consumes. This doc describes the shipped
infrastructure, topic by topic. Remaining foundation work is in `todo.md`.

## Structured solver result

The solve path returns a structured result instead of throwing on solver status,
so callers branch on the outcome. This gates the whole area.

- **`SolverResult` + `SolverStatus`** (`src/include/duckdb/decidb/solver_result.hpp`).
  `SolverStatus = {OPTIMAL, INFEASIBLE, UNBOUNDED, INF_OR_UNBD, TIME_LIMIT,
  SUBOPTIMAL, ITERATION_LIMIT, OTHER}`; `SolverResult { status, solution, objective_value, ray,
  diagnostic_timed_out, raw_status, has_solution, best_bound, gap }`.
  `ray` is populated only on the unbounded ray-extraction path; `raw_status` carries
  the backend-native code for the `OTHER` message. `diagnostic_timed_out` is set only
  when an internal diagnostic helper solve (the `INF_OR_UNBD` probe or ray fallback)
  hits its helper budget before producing a usable result, so the operator can say
  diagnosis ran out of time instead of silently falling to the static error.
  - **`SUBOPTIMAL` (feasible-but-unproven).** Gurobi's `GRB_SUBOPTIMAL` (status 13): a
    feasible solution exists but the solver stopped without proving optimality — in
    practice a numerically hard barrier on a QCP/SOC reformulation that stalls before
    satisfying optimality tolerances. It carries the incumbent exactly like the timeout
    fields below (`solution` / `objective_value` / `has_solution`), and the router
    (`decide_router.cpp`) sends it to the **SOLVED** success terminal so the operator
    delivers the rows with a plain "not proven best" stderr caveat rather than erroring.
    Gurobi-only — HiGHS rejects quadratic constraints upstream, so it never reaches here.
    (Historically such statuses were rewritten to `OPTIMAL`; the current soundness rule
    never relabels a non-optimal termination as optimal — it carries a distinct status.)
  - **Timeout incumbent fields (S1).** `solution` + `objective_value` carry the proven
    optimum at `OPTIMAL` **and** the best-so-far incumbent at `TIME_LIMIT` / `SUBOPTIMAL`
    (when one was found) — so a caller must branch on `status` / `has_solution`, not on
    solution emptiness, to know whether a value is proven best. `has_solution` is true iff the
    backend found a feasible incumbent (Gurobi `SolCount > 0` / HiGHS
    `primal_solution_status == kSolutionStatusFeasible`); it **gates** the incumbent reads
    (`solution` / `objective_value` / `gap`) because with no incumbent those attributes
    return solver sentinels (`-1e100` / `inf` / `nan`). `best_bound` (Gurobi `ObjBound` /
    HiGHS `mip_dual_bound`) and `gap` **default to NaN ("unavailable")** and are only
    populated when a proven bound actually exists — i.e. on **MIP** timeouts. On an LP/QP
    timeout Gurobi's `ObjBound` reads back the ±1e100 infinity sentinel (finite, so it
    would pass an `isfinite` guard downstream) and HiGHS's `mip_dual_bound` holds its 0
    default; both backends therefore reject sentinel/non-finite values (Gurobi: magnitude
    `>= EFFECTIVE_INFINITY`; HiGHS additionally gates on the model having an integer
    variable) and leave NaN. Report writers must skip bound/gap output when
    `!std::isfinite`. `gap` (Gurobi `MIPGap` / HiGHS `mip_gap`) is a **fraction** on
    *both* backends — HiGHS's `mip_gap` doc string reads "(%)" but the stored value is
    `|primal − dual| / |primal|`, not a percentage. Populated in the `GRB_TIME_LIMIT` /
    `kTimeLimit` branches (`gurobi_solver.cpp`, `deterministic_naive.cpp`).
- **Backends map-and-return** instead of throwing on solver status
  (`gurobi_solver.cpp`, `deterministic_naive.cpp`). HiGHS `kUnboundedOrInfeasible`
  (status 9) maps to `INF_OR_UNBD`. Genuine internal/API errors (NaN/Inf,
  extraction, API failures) still throw.
- **The `SolveModel` facade** returns `SolverResult` and no longer throws on solver
  status (`ilp_solver.cpp`). The default user-facing error text is a single helper,
  `ThrowDecideSolveError(const SolverResult &)`.
- **Diagnostic helper solves are capped and interrupt-aware.** `ResolveDecideDiagnosticTimeLimit`
  (`solver_config.hpp`) returns `min(60s, primary_solve_limit)`. The prepared-model
  overload `SolvePreparedModel(model, backend, options)` applies that budget and the
  same interrupt poll as the primary solve. It is used by the zero-objective
  `INF_OR_UNBD` probe, the portable box-LP ray fallback, and the infeasible engine's
  elastic re-solves (tier passes, stage 2, and tie-break), so diagnosis cannot add
  several full 300s chunks after an already-slow failed solve. A helper `TIME_LIMIT`
  is reported as "diagnosis ran out of time" rather than being hidden behind the
  generic static error.
- **`INF_OR_UNBD` first goes through the existing feasibility probe.** A ray (improving
  recession direction) is *necessary but not sufficient* for unboundedness — the
  region must also be feasible (an infeasible problem can still admit an improving
  ray, e.g. `x − y ≤ −10 AND y − x ≤ −10`). So `DisambiguateInfOrUnbd`
  (`ilp_solver.cpp`) re-solves a zero-objective copy (`MakeZeroObjectiveProbeModel`):
  feasible ⇒ rewrite to `UNBOUNDED`, infeasible ⇒ `INFEASIBLE`. A genuinely-unbounded
  solve therefore reaches the normal unbounded diagnosis under `auto` via this
  rewrite.
  - **Residual `INF_OR_UNBD` is a router fallback, not a broader status policy.** When the
    probe *itself* returns neither OPTIMAL nor INFEASIBLE (a zero-objective model
    can't be unbounded, so this means the solver could not decide feasibility at all
    — error/limit), the status stays `INF_OR_UNBD`. Under `auto`, the router runs
    check-ray: ray present reuses the unbounded terminal with the caveat `It may
    instead be infeasible.`, and no ray routes to the infeasible terminal. Under
    `off`, it still falls to the static `ThrowDecideSolveError` `INF_OR_UNBD` branch.
- **Pre-solve model-builder infeasibility is normalized.** Contradictory normalized
  rows and contradictory accumulated bounds throw `DecideInfeasibleModelException`
  inside `SolverModel::Build`; `SolveModel` catches that internal exception and
  returns `SolverResult{status = INFEASIBLE}`. A fast contradiction such as
  `x >= 5 AND x <= 1` therefore reaches the same `PhysicalDecide::Finalize` gate as
  backend-reported infeasibility, instead of bypassing diagnostics with a direct
  builder error.
- **The throw lives in the operator.** `PhysicalDecide::Finalize`
  (`physical_decide.cpp`) branches on status: optimal → store the solution
  (`SUBOPTIMAL` also stores it, with a "not proven best" caveat); other
  non-optimal → the pragma gate decides whether to diagnose or call
  `ThrowDecideSolveError`. This is the single gated call site.

Routing and behavior are unchanged for users with no pragma. (The static error
*wording* was later tightened — concise, one line + the smallest fix, no jargon —
under the "user-facing output is for SQL users" principle; see `unbounded/done.md`.)

## Solver behavior (backend reference)

Empirical facts about how the two backends behave on the cases diagnostics cares
about. Durable reference — the status disambiguation, ray extraction, and the router all
rely on these.

- **Versions.** Gurobi is runtime-loaded (dlopen), supporting 9.5–13.0 — the version
  is parsed from the lib filename, so there is no fixed Gurobi version. HiGHS is
  vendored 1.11.0 (`third_party/highs/HConfig.h`).
- **Terminal status by case** (constructed minimal models):

  | Case | Gurobi | HiGHS |
  | --- | --- | --- |
  | Unbounded (REAL / LP) | definitive UNBOUNDED | definitive UNBOUNDED (`kUnbounded`) |
  | Infeasible (aggregate) | INFEASIBLE | INFEASIBLE (`kInfeasible`) |
  | Unbounded (INTEGER / MILP) | definitive UNBOUNDED | ambiguous `kUnboundedOrInfeasible` → mapped to `INF_OR_UNBD` |

  Gurobi is definitive on both LP and MILP. HiGHS is definitive only for LP; on
  MILP-unbounded it returns the ambiguous status and has no `DualReductions`-style
  knob to disambiguate in a re-solve — which is why the portable zero-objective probe
  exists.
- **Zero-objective disambiguation.** Re-solving with the objective replaced by `0`
  (`MakeZeroObjectiveProbeModel`) cleanly separates the ambiguous case on both
  backends: feasible ⇒ the original was unbounded, infeasible ⇒ infeasible (verified
  on LP and MILP, both backends). This is the portable analogue of Gurobi's
  `DualReductions=0`, and is what `DisambiguateInfOrUnbd` runs.
- **Native ray APIs (optional accelerators, not the default).** Gurobi `UnbdRay`
  (needs `InfUnbdInfo=1`) returns a ray for continuous models only — it errors on a
  MIP. HiGHS `getPrimalRay` returns a ray for LP and MIP (even under the ambiguous
  status, via the LP relaxation), at the cost of an extra LP solve. Neither is
  uniformly better; DeciDB uses the portable box-LP (`unbounded/done.md`) as the
  solver-agnostic default and treats native rays as optional accelerators.

These facts are the evidence behind the router's inf/unb branch — the ambiguous
status is real (HiGHS MILP), the feasibility probe stays first, and a residual
ray is reported with an explicit caveat because feasibility was not established
(`router/README.md`).

- **Time-limit behavior (incumbent / bound / gap at timeout).** Probed 2026-07-02
  on constructed hard MILPs (equality market-split for the no-incumbent case; a
  trivially-feasible multi-dim knapsack for the with-incumbent case) with
  `DECIDB_TIME_LIMIT` set to 1–2 s. **Both backends expose everything the slow
  router needs at the time limit** — a feasible incumbent (when one was found), its
  objective, the best bound, and the gap — so Bucket A is *not* reduced on HiGHS.

  | Field | Gurobi (at `GRB_TIME_LIMIT`) | HiGHS (at `kTimeLimit`) |
  | --- | --- | --- |
  | incumbent present? | `SolCount > 0` | `primal_solution_status == 2` (feasible) |
  | incumbent vector | `X` array (only when `SolCount > 0`) | `getSolution().col_value` (only when feasible) |
  | incumbent objective | `ObjVal` | `getInfo().objective_function_value` |
  | best bound | `ObjBound` — finite/meaningful **for MIPs** | `getInfo().mip_dual_bound` — finite/meaningful **for MIPs** |
  | gap | `MIPGap` (only when incumbent, MIP) | `getInfo().mip_gap` (only when incumbent, MIP) |

  - **The best bound is the reliable routing key on both backends *for MIPs*** —
    meaningful in both buckets (e.g. the same market-split model reports bound `27` on
    Gurobi *and* HiGHS). This is what S3's "route by best bound" reads. On **LP/QP**
    timeouts neither backend proves a bound: Gurobi's `ObjBound` read succeeds but
    returns the ±1e100 infinity sentinel, and HiGHS's `mip_dual_bound` silently holds
    its 0 default (probed 2026-07-04, 200k-var LP at `DECIDB_TIME_LIMIT=0.001`). The
    backends keep the NaN "unavailable" default in `SolverResult` for those cases.
  - **Sentinels must be gated on the incumbent-presence flag, not the read's error
    code.** With no incumbent, Gurobi's `ObjVal`/`MIPGap` reads *succeed* (error 0)
    but return `-1e100` / `1e100`; HiGHS returns `objective_function_value = inf` and
    `mip_gap = nan`, and `col_value` is present but garbage. Read incumbent/objective/gap
    *only* when `SolCount > 0` (Gurobi) / `primal_solution_status == 2` (HiGHS).
  - **HiGHS timeout mapping (fixed, S0).** `highs.run()` returns `HighsStatus::kWarning`
    (not `kOk`) at the time limit. `DeterministicNaive::Solve` now throws only on
    `HighsStatus::kError`, so `kWarning` falls through to the model-status switch and
    `kTimeLimit` maps to `TIME_LIMIT` (previously the `!= kOk` guard threw an INTERNAL
    error first, crashing the diagnosable timeout — see `07_issues/bugs/done.md`). Both
    backends now produce `TIME_LIMIT` with a readable incumbent / bound / gap.
  - **Manual interrupt API — Gurobi wired, HiGHS not.** Ctrl-C is a peer trigger into the
    slow branch on *any* armed solve (first solve included), decoupled from the time limit —
    see `slow/done.md` ("Ctrl-C as a peer trigger"). Gurobi uses **`GRBterminate`** from a
    watcher thread (thread-safe), mapping `GRB_INTERRUPTED → TIME_LIMIT` with a
    `user_interrupted` flag for the wording. HiGHS's interrupt would need the
    `setCallback` / `startCallback` path (no thread-safe terminate), which is **not** wired,
    so HiGHS stays boundary-only: a mid-solve Ctrl-C is seen only at the chunk boundary and
    reported as a time-limit stop — the solver-agnostic fallback.
  - **Warm-start API exists on both.** Gurobi `Start` attribute; HiGHS `setSolution`.
    (The anytime objective→constraint ladder that would have used it was dropped — see
    `slow/todo.md`.)

## Constraint provenance (row → clause)

Every emitted matrix row records where it came from, so diagnosis reports at the
user-clause level instead of raw rows.

- **`ConstraintProvenance {clause_id, group_key, kind, shape, avg_scaled, strict,
  typed_k}`** on `ModelConstraint` and `SolverModel::QuadraticConstraint`
  (`src/include/duckdb/decidb/ilp_model.hpp`). `clause_id` indexes
  `SolverInput::constraints` (`INVALID_INDEX` for synthetic rows); `group_key` is the
  PER/WHEN group id at emission (or row id for per-row, INVALID when ungrouped); `kind`
  is the row role. The trailing fields (added in I2) drive the elastic engine's slack
  placement: `shape` (`ElasticShape::UNSET` / `SHARED_LITERAL` / `PER_ROW_DATA`) decides
  whether a clause's rows share **one** slack; `UNSET` is the invalid default, and
  `BuildElasticModel` asserts that any relaxable user-clause row has been explicitly
  stamped before diagnosis reads it. `avg_scaled` / `strict` / `typed_k` carry the
  per-shape unit info for reporting (see `infeasible/done.md`).
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
Tested in `test/common/test_decidb_constraint_provenance.cpp`. (A standalone
clause-row reverse index shipped with F2 but no consumer ever materialized — the
elastic engine walks `provenance` directly — so it was removed; consumers that need
clause → row lookup group by `provenance.clause_id` inline.)

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
  unbounded `affected_rows` characterization resolves to a categorical rule
  set (`unbounded/done.md`). Global-block columns default to GLOBAL_AUX (unnamed)
  unless named through `SolverInput::global_variable_labels` (aggregate `<>`
  indicators, composed MIN/MAX z's — see `infeasible/done.md`).

Tested in `test/common/test_decidb_variable_provenance.cpp`.

## The `diagnose_decide` pragma

Sticky session setting selecting whether a failed solve is diagnosed. Two modes:
`auto` (default) and `off`. Under `auto`, diagnosis runs whenever the solve
*actually* lands in a failed state an engine covers; on a successful solve it costs
nothing. `off` suppresses diagnosis entirely and reproduces the plain static solver
error. (The earlier per-state filter modes `infeasible`/`unbounded`/`slow` and the
opt-in `none` default were removed: they were filters, not forces, so `auto`
subsumes every useful case while staying silent on success.)

- Registered as an **extension option** via `RegisterDecideDiagnosticOptions(DBConfig&)`
  (called from `DatabaseInstance::Configure`) — no settings-codegen, no grammar
  change. The set-callback validates the enum, so a typo fails fast at SET time.
  Works with `PRAGMA diagnose_decide=…` and `SET diagnose_decide=…`; `RESET`
  restores `auto`.
- Helpers in `decide_diagnostic.cpp`: `GetDiagnoseDecideMode`,
  `DiagnosisApplies(mode, status)` (true under `auto` for INFEASIBLE / UNBOUNDED /
  TIME_LIMIT), `DiagnoseModeArmsDiagnosis` (true under `auto`; controls shared
  pre-solve diagnosis prep).
- **The gate** in `PhysicalDecide::Finalize`: read the mode before the solve,
  pre-arm `SolveModelOptions::extract_unbounded_ray`, infeasible-bound tolerance,
  and retained-model capture under `auto` (`off` pays nothing). On a non-optimal
  result, `RouteSolveResult` dispatches to the UNBOUNDED / INFEASIBLE / TIME_LIMIT
  terminal when available; the terminal builds + stashes the diagnosis and throws the
  short pointer error, else falls through to `ThrowDecideSolveError`.

Tested in `test/decide/tests/test_query_diagnostics_pragmas.py` (both backends).

## The `decide_on_timeout` pragma

Sticky session setting governing what a **time-limit** stop does — but only under
`diagnose_decide='auto'` (`off` is a master mute: TIME_LIMIT routes to `UNDIAGNOSED` →
plain error, so `decide_on_timeout` never fires). Three modes, registered the same way
alongside `diagnose_decide` in `RegisterDecideDiagnosticOptions`, validated by a
set-callback, read via `GetDecideOnTimeoutMode` (default `ask`):

- `ask` (default) — print the report, then prompt to keep solving on the warm solver at
  an interactive terminal; **falls back to `error` when stdin is not a TTY** (tests,
  pipes, benchmarks, C-API) so it never blocks on an unanswerable prompt.
- `error` — print the report, then error (never returns the incumbent).
- `continue` — auto-resume each chunk until the solver finishes; Ctrl-C
  (`ClientContext::interrupted`) breaks at the next chunk boundary.

The full loop, warm-resume session, and stop delivery live in `slow/done.md`; the
terminal wiring is in `router/done.md` ("Terminals: time_limit").

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
- **Infeasible engine (I1/I2):** `DiagnoseInfeasible(const InfeasibleDiagnosisInput&)`
  carries the built `SolverModel` (the elastic transform `BuildElasticModel` reshapes
  it) plus an injected `solve_model` callback, and a `has_unhandled_user_bounds` flag
  that keeps the elastic-infeasible verdict honest when a user bound could not be
  re-emitted. The flag is computed by the operator's re-emission loop: it turns `true`
  only if a recorded bound's column is missing from the retained model — expected never
  in practice, since every absorbed user bound is re-emitted (multi-instance bounds as a
  shared-slack block per I2.a, BOOLEAN pins like `x <= 0` / `x >= 1` / `x = 1` with the
  column opened only to its intrinsic `[0,1]`). Only bounds that merely restate a
  variable's intrinsic domain (the BOOLEAN 0/1 box, default non-negativity) are excluded
  at recording time via `op.is_boolean_var`. Engine internals are in `infeasible/done.md`.

Tested in `test/common/test_decidb_diagnostic_engines.cpp`.

## Shared diagnostic constants

The box-LP ray-fallback path spans three translation units — the model builder
(`diagnostic_solves.cpp`, which opens bounds), the solver facade (`ilp_solver.cpp`,
which confirms an improving ray), and the unbounded engine
(`decide_diagnostic_engines.cpp`, which filters escaping columns). Its "free suspect
filter" invariant only holds if these all use the *same* two thresholds, so they are
defined once in `src/include/duckdb/decidb/diagnostic_constants.hpp`:

- `EFFECTIVE_INFINITY` (`1e20`) — a finite bound at/above this magnitude is treated
  as unbounded in that direction (which upper bounds the fallback LP opens).
- `DIAGNOSTIC_RAY_EPSILON` (`1e-8`) — a ray / objective-improvement component at/below
  this is treated as zero (both the improving-ray check and the escape filter).

Previously each file redefined its own copy (`EFFECTIVE_INFINITY`,
`UNBOUNDED_RAY_IMPROVEMENT_EPSILON`, `RAY_ESCAPE_EPSILON`); the shared header removes
the drift risk.

## The `decide_diagnostics()` reporting relation

A structured diagnosis a state engine populates, stashed per-connection, surfaced
as a fixed-schema relation.

- **`DecideDiagnostic` / `DiagnosticRow` / `DecideDiagnosticState`**
  (`src/include/duckdb/decidb/decide_diagnostic.hpp`). The state is a
  `ClientContextState` stashed under key `decide_diagnostics`; the failing DECIDE
  mutates it before throwing, so it survives into the next statement **on the same
  connection**. **Lifecycle:** the stash holds *the diagnosis of the most recent
  DECIDE failure on the connection that actually produced one — otherwise it is
  empty.* A successful DECIDE (`OPTIMAL` in `PhysicalDecide::Finalize`) calls
  `ClearDecideDiagnostic`, which invalidates `latest` (so `decide_diagnostics()`
  returns 0 rows) — a stale diagnosis never lingers after the user fixes the query.
  The same clear runs on every failure path that throws *without* stashing a new
  diagnosis — `diagnose_decide='off'`, a status no engine covers, an unbounded solve
  the engine can't name a variable for, an infeasible solve the elastic engine
  declines to report on, and a `continue`-mode timeout resume that lands on a
  definitive INFEASIBLE/UNBOUNDED — so an earlier failure's diagnosis can never be
  misread as belonging to a later, undiagnosed one (`PhysicalDecide::FinalizeInternal`,
  `physical_decide.cpp`). The per-connection id counter is left intact so ids stay
  monotonic across solves.
- **`decide_diagnostics()` table function** (registered in
  `system_functions.cpp`, implemented in `decide_diagnostic.cpp`) with fixed schema
  `(diagnosis_id BIGINT, state, subject_kind, subject, attribute, value)`. The
  string columns are VARCHAR; empty string fields render as SQL NULL.
  `diagnosis_id` is stamped at stash time from a per-connection counter and ties
  all rows from one diagnosed failure together.
- **Long-form EAV surface:** `subject_kind` names the entity type (`variable` for
  unbounded, `clause`/`model` for infeasible); `subject` is the state-engine-owned identifier;
  `attribute`/`value` carry that engine's facts. This keeps the table function
  schema stable as new states add their own attributes.
- **Scannable one-row-per-subject view (a `PIVOT` recipe, not a second function).** The EAV
  shape lists one row per `attribute`, so an infeasible loosen clause spans three rows
  (`edit_kind` / `suggested_change` / `amount`). To read it as one row per clause, pivot it —
  the columns adapt to whichever attributes the state emitted (loosen, drop, or conflict), so a
  single recipe covers every infeasible shape without a parallel state-specific table function
  that would have to duplicate the deliberately-flexible schema:

  ```sql
  PIVOT (SELECT subject, attribute, value FROM decide_diagnostics() WHERE subject_kind = 'clause')
  ON attribute USING first(value) GROUP BY subject ORDER BY subject;
  --  subject │ amount │ edit_kind │ suggested_change
  --  x <= 1  │ 4      │ loosen    │ x <= 5
  ```
- **Why a table function and not a result-schema switch:** DECIDE is a clause on
  SELECT, and the projection binds the decision columns before the solve runs
  (`logical_decide.cpp`, `transform_select_node.cpp`), so a runtime result-schema
  switch is bind-time-blocked. The diagnosis is surfaced via this companion table
  function instead.
- **The unbounded engine populates variable rows** (`BuildUnboundedDiagnostic` — see
  `unbounded/done.md`). It emits one `grows_toward` row for every escaping variable
  and an `affected_rows` / `affected_entities` row only when there is instance
  multiplicity to explain (self-describing categorical rules / total-escape / count).
  The forced remedy (add a bound) is prescribed in the stderr summary, not a per-row
  attribute. The unbounded characterization adds
  three sticky extension options alongside `diagnose_decide`:
  `diagnose_decide_escape_rate`, `diagnose_decide_categorical_ratio`,
  `diagnose_decide_min_categories` (all in `RegisterDecideDiagnosticOptions`).
  The infeasible engine populates clause/model rows (`BuildInfeasibleDiagnostic` —
  see `infeasible/done.md`) for loosen/drop edits and achievable-objective
  metadata. It adds two more options: `diagnose_decide_removal_bigm` (DOUBLE,
  default `0` = auto-derive, `>= 0`) — the Big-M used to neutralize a dropped `<>`
  in the I4 removal dial, threaded via `DecideDiagParams::removal_bigm`; and
  `diagnose_decide_infeasible_slack_scope` (VARCHAR, `query` default / `expanded`)
  — the T3 slack-scope policy selecting a folded SQL-level edit vs. the per-row /
  per-group profile, threaded via `DecideDiagParams::slack_scope` (see
  `infeasible/done.md`).

Tested in `test/decide/tests/test_query_diagnostics_relation.py` (both backends) and
`test/common/test_decidb_diagnostic_engines.cpp` (a clause-shaped stub row).
