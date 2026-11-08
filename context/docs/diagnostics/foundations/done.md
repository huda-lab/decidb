# Query Diagnostics — Foundations (how it works)

Shared plumbing every diagnosis state consumes, topic by topic.

## Structured solver result

The solve path returns a structured result instead of throwing on solver status,
so callers branch on the outcome. This gates the whole area.

- **`SolverResult` + `SolverStatus`** (`src/include/duckdb/decidb/solver/solver_result.hpp`).
  `SolverStatus = {OPTIMAL, INFEASIBLE, UNBOUNDED, INF_OR_UNBD, TIME_LIMIT,
  SUBOPTIMAL, ITERATION_LIMIT, OTHER}`; `SolverResult { status, solution, objective_value, ray,
  diagnostic_timed_out, status_text, has_solution, best_bound, gap, user_interrupted, model_constraint_rows }`.
  `ray` is populated only on the unbounded ray-extraction path; `status_text` carries
  the backend's own words for the `OTHER` message. `diagnostic_timed_out` is set only
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
    A non-optimal termination is never relabelled `OPTIMAL`.
  - **Timeout incumbent fields.** `solution` + `objective_value` carry the proven
    optimum at `OPTIMAL` **and** the best-so-far incumbent at `TIME_LIMIT` / `SUBOPTIMAL`
    (when one was found) — so a caller must branch on `status` / `has_solution`, not on
    solution emptiness, to know whether a value is proven best. `has_solution` is true iff the
    backend found a feasible incumbent and **gates** the incumbent reads (`solution` /
    `objective_value` / `gap`). `best_bound` and `gap` **default to NaN ("unavailable")** and
    are populated only when a proven bound exists — on **MIP** timeouts; report writers skip
    them when `!std::isfinite`. `gap` is a **fraction** on *both* backends — HiGHS's `mip_gap`
    doc string reads "(%)" but the stored value is `|primal − dual| / |primal|`. They are set
    in the `GRB_TIME_LIMIT` / `kTimeLimit` branches (`gurobi_solver.cpp`,
    `deterministic_naive.cpp`); the per-backend reads and sentinels are under "Time-limit
    behavior" below. `user_interrupted` marks a Ctrl-C stop, which reuses `TIME_LIMIT`.
- **Backends map-and-return** instead of throwing on solver status
  (`gurobi_solver.cpp`, `deterministic_naive.cpp`). HiGHS `kUnboundedOrInfeasible`
  (status 9) maps to `INF_OR_UNBD`. Genuine internal/API errors (NaN/Inf,
  extraction, API failures) still throw.
- **The `SolveModel` facade** returns `SolverResult` and does not throw on solver
  status (`ilp_solver.cpp`). The default user-facing error text is a single helper,
  `ThrowDecideSolveError(const SolverResult &, SolverBackend)`.
- **Diagnostic helper solves are capped and interrupt-aware.** `ResolveDecideDiagnosticTimeLimit`
  (`solver_config.hpp`) returns `min(60s, primary_solve_limit)`.
  `SolvePreparedModel(model, backend, options)` applies that budget and the
  same interrupt poll as the primary solve. It is used by the zero-objective
  `INF_OR_UNBD` probe, the portable box-LP ray fallback, and the infeasible engine's
  elastic re-solves (tier passes, stage 2, and tie-break), so diagnosis cannot add
  several full 300s chunks after an already-slow failed solve. A helper `TIME_LIMIT`
  is reported as "diagnosis ran out of time" rather than being hidden behind the
  generic static error.
- **`INF_OR_UNBD` first goes through a feasibility probe.** A ray (improving
  recession direction) is *necessary but not sufficient* for unboundedness — the
  region must also be feasible (an infeasible problem can still admit an improving
  ray, e.g. `x − y ≤ −10 AND y − x ≤ −10`). So `DisambiguateInfOrUnbd`
  (`ilp_solver.cpp`), the one route every solve settles its status through (the primary solve's
  `SolveModel` and every diagnostic solve's `SolvePreparedModel`),
  re-solves a zero-objective copy (`MakeZeroObjectiveProbeModel`):
  feasible ⇒ rewrite to `UNBOUNDED`, infeasible ⇒ `INFEASIBLE`. Under `DIAGNOSE`, a
  genuinely-unbounded solve therefore reaches the normal unbounded engine via this
  rewrite; an unprefixed query reports the normalized state without running an engine.
  - **Residual `INF_OR_UNBD` is a router fallback, not a broader status policy.** When the
    probe *itself* returns neither OPTIMAL nor INFEASIBLE (a zero-objective model
    can't be unbounded, so this means the solver could not decide feasibility at all
    — error/limit), the status stays `INF_OR_UNBD`, and the router routes it by the ray
    signal (`../router/done.md`).
- **Pre-solve model-builder infeasibility is normalized.** Contradictory bounds (`x >= 5
  AND x <= 1`, an inverted column box) and a violated coefficient-free row (`SUM(0 * x) <=
  -1`, `x - x <= -1`) stay in the built model, and `SolverModel::ProvesInfeasible` answers
  it as `SolverResult{status = INFEASIBLE}` before any backend reads it. The model is
  retained, so it reaches the same `PhysicalDecide::Finalize` gate as backend-reported
  infeasibility, and diagnosis runs normally and names the clause.
- **The status policy lives in the operator.** `PhysicalDecide::Finalize` routes the
  status once through the router (`../router/done.md`). An unprefixed query gets the
  concise state-only error plus the instruction to rerun with `DIAGNOSE`; it pays for no
  model retention, ray extraction, or elastic solve.

## Solver behavior (backend reference)

Empirical facts about how the two backends behave on the cases diagnostics cares
about. Durable reference — the status disambiguation, ray extraction, and the router all
rely on these.

- **Versions.** Gurobi is runtime-loaded (dlopen), supporting 9.5–13.0 — the version
  is parsed from the lib filename, so there is no fixed Gurobi version. HiGHS is
  vendored 1.15.1 (`third_party/highs/HConfig.h`).
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
  on LP and MILP, both backends). This is what `DisambiguateInfOrUnbd` runs, for every
  backend; Gurobi's `DualReductions=0` re-solve is not used, because on a non-convex
  model it switches to spatial branching and does not settle the status.
- **Native ray APIs (optional accelerators, not the default).** Gurobi `UnbdRay`
  (needs `InfUnbdInfo=1`) returns a ray for continuous models only — it errors on a
  MIP. HiGHS `getPrimalRay` returns a ray for LP and MIP (even under the ambiguous
  status, via the LP relaxation), at the cost of an extra LP solve. Neither is
  uniformly better; DeciDB uses the portable box-LP (`../unbounded/done.md`) as the
  solver-agnostic default and treats native rays as optional accelerators.

These facts are the evidence behind the router's inf/unb branch (`../router/done.md`):
the ambiguous status is real (HiGHS MILP), so the feasibility probe stays first.

- **Time-limit behavior (incumbent / bound / gap at timeout).** Measured on constructed
  hard MILPs (equality market-split for the no-incumbent case; a
  trivially-feasible multi-dim knapsack for the with-incumbent case) with
  `DECIDB_TIME_LIMIT` set to 1–2 s. **Both backends expose everything a time-limit
  report needs** — a feasible incumbent (when one was found), its objective, the best
  bound, and the gap.

  | Field | Gurobi (at `GRB_TIME_LIMIT`) | HiGHS (at `kTimeLimit`) |
  | --- | --- | --- |
  | incumbent present? | `SolCount > 0` | `primal_solution_status == 2` (feasible) |
  | incumbent vector | `X` array (only when `SolCount > 0`) | `getSolution().col_value` (only when feasible) |
  | incumbent objective | `ObjVal` | `getInfo().objective_function_value` |
  | best bound | `ObjBound` — finite/meaningful **for MIPs** | `getInfo().mip_dual_bound` — finite/meaningful **for MIPs** |
  | gap | `MIPGap` (only when incumbent, MIP) | `getInfo().mip_gap` (only when incumbent, MIP) |

  - **The best bound is the reliable routing key on both backends *for MIPs*** —
    with or without an incumbent (e.g. the same market-split model reports bound `27` on
    Gurobi *and* HiGHS). On **LP/QP** timeouts neither backend proves a bound: Gurobi's
    `ObjBound` read succeeds but returns the ±1e100 infinity sentinel (finite, so it would
    pass an `isfinite` guard), and HiGHS's `mip_dual_bound` silently holds its 0 default
    (a 200k-variable LP at `DECIDB_TIME_LIMIT=0.001`). Both backends therefore reject a
    value at or above `DECIDE_EFFECTIVE_INFINITY` in magnitude, HiGHS also requires an
    integer variable in the model, and the NaN "unavailable" default stays.
  - **Sentinels must be gated on the incumbent-presence flag, not the read's error
    code.** With no incumbent, Gurobi's `ObjVal`/`MIPGap` reads *succeed* (error 0)
    but return `-1e100` / `1e100`; HiGHS returns `objective_function_value = inf` and
    `mip_gap = nan`, and `col_value` is present but garbage. Read incumbent/objective/gap
    *only* when `SolCount > 0` (Gurobi) / `primal_solution_status == 2` (HiGHS).
  - **HiGHS timeout mapping.** `highs.run()` returns `HighsStatus::kWarning`
    (not `kOk`) at the time limit. `HighsSession::RunAndReadback` reports only
    `HighsStatus::kError` as `OTHER`, so `kWarning` falls through to the model-status switch
    and `kTimeLimit` maps to `TIME_LIMIT`. Both backends produce `TIME_LIMIT` with a readable
    incumbent / bound / gap.
  - **Manual interrupt on both backends.** Ctrl-C stops a solve with or without the
    prefix, decoupled from the time limit (`../../pipeline/08_execution/slow_solves.md`).
    Gurobi uses **`GRBterminate`** from a watcher thread (thread-safe), mapping
    `GRB_INTERRUPTED → TIME_LIMIT`. HiGHS has no thread-safe terminate; its interrupt
    callbacks (`kCallbackMipInterrupt`, `kCallbackSimplexInterrupt`, `kCallbackIpmInterrupt`)
    answer from the same poll beside HiGHS's own time-limit checks, and `kInterrupt` maps to
    `TIME_LIMIT`. Both set `user_interrupted` for the wording.
  - **Warm-start API exists on both.** Gurobi `Start` attribute; HiGHS `setSolution`.
    At a timeout DeciDB resumes the original warm solve; it does not launch the elastic
    engine as a feasibility classifier or infer unboundedness from incumbent progress. See
    `../../pipeline/08_execution/slow_solves.md` ("Settled timeout policy").

## Constraint provenance (row → clause)

Every emitted matrix row records where it came from, so diagnosis reports at the
user-clause level instead of raw rows.

- **`ConstraintProvenance`** on `ModelConstraint` and `SolverModel::QuadraticConstraint`
  (`src/include/duckdb/decidb/formulation/ilp_model.hpp`). Three separate identities:
  `source_clause_id` indexes the source display registry `SolverModel::constraint_sources`,
  `repair_group_id` is the elastic grouping identity, and `removal_group_id` the atomic DROP
  identity (`INVALID_INDEX` for synthetic rows); `group_key` is the
  group the row was emitted for — a reducer's `by` group in the aggregate shape, a
  `per K:` class in the per-row shape, the one group of a clause with a `when` — and
  INVALID when the clause is ungrouped; `kind` is the row role. Beside it, `data_row`
  names the data row a per-row row came from and `class_set` the classes a group's row
  stands for (D28), which the expanded slack scope reports one by one. The remaining fields
  drive the elastic engine's slack placement: `shape` (`ElasticShape::UNSET`
  / `SHARED_SCALAR` / `PER_ROW_DATA`) decides whether a clause's rows share **one** slack;
  `UNSET` is the invalid default, and `BuildElasticModel` asserts that any relaxable
  user-clause row has been explicitly stamped before diagnosis reads it. `avg_scaled` /
  `strict` / `typed_k` carry the per-shape unit info for reporting (see
  `../infeasible/done.md`).
- **Single row-role enum:** `ConstraintKind = { USER_PARAMETER, USER_MECHANISM,
  STRUCTURAL }` (`duckdb/common/enums/decide.hpp`). `USER_PARAMETER` carries a
  user-editable parameter/RHS and is relaxable. `USER_MECHANISM` is a rigid helper
  row attached to a user clause (for example generated `<>` and hard MIN/MAX
  mechanism rows). `STRUCTURAL` is a synthesized definition/linking row. The
  elastic predicate is one equality: `IsRelaxableForElastic(kind)` is true only for
  `USER_PARAMETER`.
- **Stamped at every builder fan-out site** in `ilp_model_builder.cpp` (linear
  aggregate rows, ungrouped or one per `by` group; per-row rows; the raw global push;
  and the three quadratic analogues). `clause_id` is derived by pointer arithmetic into
  `input.constraints` so it survives `continue` skips. Evaluated constraints default
  to `USER_PARAMETER` unless the planner/operator stamps a rigid role; raw generated
  constraints default to `STRUCTURAL` unless explicitly marked as user mechanism or
  user parameter. The six evaluated-constraint sites share the field-setting through
  three free functions: `StampConstraintProvenance` (the fields common to all six:
  `source_clause_id`, `repair_group_id`, `removal_group_id`, `kind`, `shape`/`rhs_label`,
  `rhs_mechanism_offset`, and `group_key`/`group_name` where grouped),
  `StampAggregateProvenance` (`is_aggregate`, the four aggregate sites only) and
  `StampDataRow` (`data_row` and `per_kept_row`, the two per-row sites) — see
  `../../pipeline/06_model_formulation/done.md` §7 for why `avg_scaled` /
  `weight_labels` / `folded_terms` stay linear-only rather than becoming a shared
  field. The builder stamps no clause text: a clause's `when c per K:` qualifier lives
  in the source registry alone (`ConstraintSourceInfo::qualifier`, built by
  `InitializeConstraintSourceInfo` in `decide_source_provenance.cpp`), and the
  diagnosis renderer reads it there through `FindConstraintSource`.
- **Rigid rewrite rows are stamped at the source.** The optimizer tags structural
  rewrites that re-enter constraint parsing with `STRUCTURAL_CONSTRAINT_TAG`, and
  `PhysicalDecide` stamps generated McCormick / ABS envelope rows as `STRUCTURAL`;
  generated `<>` and hard MIN/MAX mechanism rows as `USER_MECHANISM`; and composed
  MIN/MAX outer user pins as `USER_PARAMETER`.
Tested in `test/common/test_decidb_constraint_provenance.cpp`. There is no separate
clause → row index: the elastic engine walks `provenance` directly, and a consumer that
needs the lookup groups by `provenance.source_clause_id`.

## Variable provenance (column → name / instance)

The column-side complement: map every solver column back to the user-facing thing
it represents, so the unbounded diagnosis names escaping variables.

- **Aux→source-expression capture.** `LogicalDecide::aux_var_expressions` maps an
  auxiliary variable's index to the user's original expression string, stamped by
  `decide_rewrite::AddAuxiliaryDecision` (`decide_optimizer.cpp`), which every
  aux-creating rewrite calls, and threaded to `PhysicalDecide`.
- **`ColumnProvenance` + `BuildColumnProvenance`** (`ilp_model.hpp` /
  `ilp_model_builder.cpp`). `ColumnKind{USER, AUX, GLOBAL_AUX}`. The builder is
  pure (caller pre-extracts per-decide-var labels + an is-aux flag) and inverts
  `VarIndexer::Get(var, row)` over all rows to produce a `flat column →
  ColumnProvenance` map. The provenance retains the variable's **instance**
  identity (the class id for a keyed decision, the row for a decision per row) — the
  hook the unbounded escape characterization resolves to categorical slice findings
  (`../unbounded/done.md`). Global-block columns default to GLOBAL_AUX (unnamed)
  unless named through `SolverInput::global_variable_labels` (aggregate `<>`
  indicators, composed MIN/MAX z's — see `../infeasible/done.md`).

Tested in `test/common/test_decidb_variable_provenance.cpp`.

## The trigger — the `DIAGNOSE` prefix

Nothing starts a diagnosis except the statement prefix. There is no session setting for
it and no automatic path.

- **Where it comes from.** `DIAGNOSE <select>` is a grammar alternative on
  `VariableShowStmt` (`third_party/libpg_query/grammar/statements/variable_show.y`),
  mirroring `SUMMARIZE`: one rule plus an `is_diagnose` flag on
  `PGVariableShowSelectStmt`. The transformer turns it into a `ShowRef` with
  `ShowType::DIAGNOSE`, so `select_with_parens`' existing `'(' VariableShowStmt ')'`
  production makes `SELECT * FROM (DIAGNOSE …)` compose for free.
- **How it travels.** `Binder::BindDiagnose` (`bind_showref.cpp`) binds the inner query
  unchanged, collects the `LogicalDecide` operators in the resulting plan, and sets
  `LogicalDecide::diagnose = true` on the one it requires there to be. `plan_decide.cpp` copies it to
  `PhysicalDecide::diagnose`. It is a property of the STATEMENT, carried parser → binder
  → logical plan → stage 08, and never read back out of a setting.
- **What it arms.** `PhysicalDecide::FinalizeSolveResult` reads the flag once as
  `diagnosis_armed` and pre-arms the shared diagnosis prep it gates:
  `SolveModelOptions::extract_unbounded_ray`, `keep_singleton_rows` (data-valued singleton rows stay rows), and
  retained-model capture. An unprefixed query pays for none of it.
- **The gate.** The router's classifier takes the armed flag and routes a failure to an
  engine only under the prefix (`../router/done.md`).
- **A query with no DECIDE clause is rejected**, at bind time, by `BindDiagnose`:
  DIAGNOSE reports on an optimization run, and there is none.
- **A query with more than one DECIDE clause is rejected too**, at bind time, in the
  same place. Such a query runs several solves and one diagnosis relation cannot say
  which one it describes, so the prefix refuses and names the way forward: isolate
  each decision query and run `DIAGNOSE` on each separately. This covers both shapes
  that put two operators in the plan — a DECIDE subquery nested inside a DECIDE
  clause, and two DECIDE subqueries joined side by side; both are already flattened
  into the operator tree by the time `BindDiagnose` walks it. **Only the prefix is
  refused**: the same queries solve unprefixed.

Tested in `test/decide/tests/test_diagnose_trigger.py` (both backends).

## Diagnosis engine seam

State-specific diagnosis logic lives behind free-function engines rather than as
inline branches in `PhysicalDecide::Finalize`.

- **Unbounded engine:** `DiagnoseUnbounded(const UnboundedDiagnosisInput&)`
  (`src/include/duckdb/decidb/diagnostics/decide_diagnostic_engines.hpp`,
  `src/decidb/diagnostics/decide_diagnostic_engines.cpp`). The input carries the
  `SolverResult`, `VarIndexer`, user labels, aux flags, diagnostic params, and an
  injected `get_candidates(decide_var_idx)` callback for categorical groupings.
- **Boundary:** the engine is free of DuckDB execution/operator types. It reads the
  ray, maps columns through variable provenance, groups escaping instances by
  DECIDE variable, asks the callback for row or keyed-class categorical candidates, and
  returns a `DecideDiagnostic` only when it produced named variable content.
- **`Finalize` owns orchestration:** read the statement's armed flag, call the matching
  engine for the routed status, and write its findings to the statement-scoped handoff.
  If an engine cannot produce named/actionable content, the operator writes one
  `undiagnosed` finding instead. Only an unprefixed failure falls through to
  `ThrowDecideSolveError`; success clears the handoff before the operator above emits
  its `feasible` finding.
- **Infeasible engine:** `DiagnoseInfeasible(const InfeasibleDiagnosisInput&)`
  carries the repair model `PhysicalDecide::FormulateElasticModel` builds against the
  widened column box (the elastic transform `BuildElasticModel` then reshapes it) plus an
  injected `solve_model` callback. Every bound the optimizer folded into a column box
  reaches that model through its own clause's rows. Engine internals are in
  `../infeasible/done.md`.

Tested in `test/common/test_decidb_diagnostic_engines.cpp`.

## Shared diagnostic constants

The box-LP ray-fallback path spans three translation units — the model builder
(`probe_models.cpp`, which opens bounds), the solver facade (`ilp_solver.cpp`,
which confirms an improving ray), and the unbounded engine
(`decide_diagnostic_engines.cpp`, which filters escaping columns). Its "free suspect
filter" invariant only holds if these all use the *same* two thresholds, so each is
defined once and no file keeps its own copy:

- `DECIDE_EFFECTIVE_INFINITY` (`1e20`, `src/include/duckdb/common/decide_numeric_policy.hpp`)
  — a finite bound at/above this magnitude is treated as unbounded in that direction
  (which bounds the fallback LP opens).
- `DIAGNOSTIC_RAY_EPSILON` (`1e-8`, `src/include/duckdb/decidb/diagnostics/diagnostic_constants.hpp`)
  — a ray / objective-improvement component at/below this is treated as zero (both the
  improving-ray check and the escape filter).

## Diagnosis invariants (shared test helper)

A diagnostics defect does not fail a test the way a wrong answer does. The solve still fails,
the diagnosis still appears, and its numbers are often still right — what breaks is *which line
of the user's query gets blamed*. The suite stays green while the feature's promise stops
holding. `test/decide/tests/_diagnostic_invariants.py` asserts that promise mechanically, on
every diagnostics test, rather than per construct: there are nine auxiliary kinds that reach the
elastic engine (`<>` per-row and aggregate, ABS, MIN/MAX hard and composed, `norm` 0/1/inf,
bilinear), and a hand-written assertion only ever guards the one its author had in mind.

1. **A reported edit is made of the user's own text** (`assert_edits_are_users_text`). Every
   identifier in a blamed clause, and every coefficient on its left-hand side, must occur in the
   query the user typed. The two sides are held to different standards on purpose: a left-side
   number multiplies a term and is structure no lowering may invent, while the right-side bound
   is one canonicalization legitimately *computes* (`x + 2 <= 7` folds to `x <= 5`, and 5 was
   never typed). A `suggested_change` is grounded on names only — proposing a new bound value is
   what a repair *is*.
2. **Ties break on achievable objective** (`assert_backends_agree`), in its observable form.
   Gurobi and HiGHS state some constructs differently (native vs lowered), so they hand the
   engine different row sets; a choice not fixed by a stated rule falls out differently and a
   user moving hosts is told to edit a different line of their own query.
3. **A construct is reported in the user's spelling** (`assert_no_internal_names`). `ABS(x) >= 5`
   reports as `ABS(x) >= 5`, never as the rows it lowers to. An internal column (`col3`) or a
   pipeline tag (`__…__`) in a blamed clause means the user is reading our bookkeeping.

The helper deliberately does **not** consult the `subject_to_sql` map that `_apply_reported_fix`
takes. That map exists so a test can *apply* a fix whose rendering differs from the typed SQL (a
`BETWEEN` split in two, a reversed bound, a composed extremum). Honoring it would let a future
author silence a fabricated clause by adding a map entry — the guard would be defeatable by the
same edit that makes a test pass. Token grounding needs no such hatch, because a legitimate
re-spelling reuses the query's own names and numbers.

Rule 1 is wired into `_apply_reported_fix`, so every clause-edit test in
`test_query_diagnostics_relation.py` checks it without opting in. It is the release-build
counterpart to `assert_blamable_row`, whose `D_ASSERT` does not run in a release binary.

## The reporting relation — what `DIAGNOSE` returns

A structured diagnosis a state engine populates, handed to the operator above it, and
rendered as a flat, fixed-schema relation. The schema and its `edit_source` vocabulary
are the user-facing contract and live in `../../language/guide.md` §8;
this section is the mechanics.

- **`DecideDiagnostic` / `DiagnosticFinding` / `DecideDiagnosticState`**
  (`src/include/duckdb/decidb/diagnostics/decide_diagnostic.hpp`). A `DecideDiagnostic` is one
  `state` plus a vector of findings; a `DiagnosticFinding` is one row of the relation,
  with real columns and real types (`amount` DOUBLE, `row` BIGINT, `has_amount` /
  `has_row` for NULL).
- **A statement-scoped handoff, not a stash to read later.** `DecideDiagnosticState` is
  a `ClientContextState` under key `decide_diagnostics`.
  `PhysicalDecide` writes it during its `Finalize`, and `PhysicalDecideDiagnose` — the
  operator directly above it in the same plan — consumes it in its own `Finalize` via
  `TakeDecideDiagnostic`, which clears it on the way out. Nothing may read it twice, and
  nothing outside the statement reads it at all. A successful solve calls
  `ClearDecideDiagnostic`, so a `DIAGNOSE` over a query that worked finds nothing waiting
  and reports the single `feasible` finding instead. The handoff lives for one statement,
  so there are no stale diagnoses and no ids to manage.
- **`LogicalDecideDiagnose` / `PhysicalDecideDiagnose`** (`planner/operator/`,
  `execution/operator/decide/`) are the prefix's own operator pair. The logical one
  resolves its types from `GetDecideDiagnoseSchema` — the single definition of the
  columns — and the physical one is a sink that swallows the query's rows (DIAGNOSE
  reports on the run, it does not return the run's output) and a source that emits the
  findings via `RenderDecideDiagnostic`. Both are small: the engines already produce typed
  findings. The flat relation is one row per finding directly, and it composes as a
  subquery instead of being a second statement.
- **Why an operator and not a result-schema switch:** DECIDE is a clause on SELECT, and
  the projection binds the decision columns before the solve runs
  (`logical_decide.cpp`, `transform_select_node.cpp`), so a runtime result-schema switch
  is bind-time-blocked. The prefix declares the schema at bind time instead, which is
  exactly what makes it a different statement rather than a different outcome of the same
  one.
- **The engines populate the findings.** `BuildUnboundedDiagnostic` writes one runaway
  finding per escaping variable or slice (`../unbounded/done.md`); `BuildInfeasibleDiagnostic`
  writes loosen/drop edits and achievable-objective rows (`../infeasible/done.md`). Their
  sticky tuning options (`diagnose_decide_escape_rate`, `diagnose_decide_categorical_ratio`,
  `diagnose_decide_min_categories`, `diagnose_decide_infeasible_slack_scope`) are registered
  in `RegisterDecideDiagnosticOptions` and read into `DecideDiagParams`; each engine's doc
  says what its options do.

Tested in `test/decide/tests/test_query_diagnostics_relation.py` (both backends) and
`test/common/test_decidb_diagnostic_engines.cpp` (a clause-shaped stub row).
