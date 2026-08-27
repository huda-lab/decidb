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
  feasible ⇒ rewrite to `UNBOUNDED`, infeasible ⇒ `INFEASIBLE`. Under `DIAGNOSE`, a
  genuinely-unbounded solve therefore reaches the normal unbounded engine via this
  rewrite; an unprefixed query reports the normalized state without running an engine.
  - **Residual `INF_OR_UNBD` is a router fallback, not a broader status policy.** When the
    probe *itself* returns neither OPTIMAL nor INFEASIBLE (a zero-objective model
    can't be unbounded, so this means the solver could not decide feasibility at all
    — error/limit), the status stays `INF_OR_UNBD`. Under `DIAGNOSE`, the router uses
    the ray signal: a present ray routes to the unbounded terminal and no ray routes
    to the infeasible terminal. Without the prefix it reports the ambiguous state and
    stops.
- **Pre-solve model-builder infeasibility is normalized.** Both contradictory
  accumulated bounds and a violated coefficient-free row are decided inside
  `SolverModel::Build` and reported as `SolverResult{status = INFEASIBLE}`, so a
  fast contradiction such as `x >= 5 AND x <= 1` reaches the same
  `PhysicalDecide::Finalize` gate as backend-reported infeasibility instead of
  bypassing diagnostics with a direct builder error. The two differ in whether a
  model comes back. A violated coefficient-free row (`SUM(0 * x) <= -1`,
  `x - x <= -1`) is **kept** in the model and sets `build_proven_infeasible`, so
  diagnosis runs normally and names the clause. Contradictory bounds still throw
  `DecideInfeasibleModelException` for an unprefixed query, abandoning the half-built
  model; under `DIAGNOSE` the inverted box is kept instead.
- **An unretained model becomes an `undiagnosed` finding.** When
  `SolveModel` returns INFEASIBLE from a throw, `retained_model` is left
  default-constructed — no columns, no rows. The infeasible arm checks
  `num_vars == 0` up front and reports one `edit_source='undiagnosed'` finding rather
  than indexing an empty model. `SolverModel::num_vars` is default-initialized to 0
  for exactly this reason: the check is only meaningful if "never populated" is
  representable.
- **The status policy lives in the operator.** `PhysicalDecide::Finalize`
  (`physical_decide.cpp`) branches on status: optimal → store the solution
  (`SUBOPTIMAL` also stores it, with a "not proven best" caveat); other
  non-optimal → the statement's `diagnose` flag decides whether to run an engine and
  report findings or call `ThrowDecideSolveError`. This is the single trigger.

An unprefixed query gets the concise state-only error plus the instruction to rerun
with `DIAGNOSE`; it pays for no model retention, ray extraction, or elastic solve.

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
ray routes through the unbounded engine. If that engine cannot name a variable, its
`undiagnosed` finding retains `state='infeasible or unbounded'` because feasibility
was not established (`router/README.md`).

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
    (not `kOk`) at the time limit. `HighsSession::RunAndReadback` throws only on
    `HighsStatus::kError`, so `kWarning` falls through to the model-status switch and
    `kTimeLimit` maps to `TIME_LIMIT` (previously the `!= kOk` guard threw an INTERNAL
    error first, crashing the diagnosable timeout). Both
    backends now produce `TIME_LIMIT` with a readable incumbent / bound / gap.
  - **Manual interrupt API — Gurobi wired, HiGHS not.** Ctrl-C is a peer trigger into the
    slow branch on *any* armed solve (first solve included), decoupled from the time limit —
    see `../../01_pipeline/08_execution/slow_solves.md` ("Ctrl-C as a peer trigger"). Gurobi uses **`GRBterminate`** from a
    watcher thread (thread-safe), mapping `GRB_INTERRUPTED → TIME_LIMIT` with a
    `user_interrupted` flag for the wording. HiGHS's interrupt would need the
    `setCallback` / `startCallback` path (no thread-safe terminate), which is **not** wired,
    so HiGHS stays boundary-only: a mid-solve Ctrl-C is seen only at the chunk boundary and
    reported as a time-limit stop — the solver-agnostic fallback.
  - **Warm-start API exists on both.** Gurobi `Start` attribute; HiGHS `setSolution`.
    (The anytime objective→constraint ladder that would have used it was dropped — see
    `../../01_pipeline/08_execution/slow_solves_todo.md`.)

## Constraint provenance (row → clause)

Every emitted matrix row records where it came from, so diagnosis reports at the
user-clause level instead of raw rows.

- **`ConstraintProvenance {clause_id, group_key, kind, shape, avg_scaled, strict,
  typed_k}`** on `ModelConstraint` and `SolverModel::QuadraticConstraint`
  (`src/include/duckdb/decidb/ilp_model.hpp`). `clause_id` indexes
  `SolverInput::constraints` (`INVALID_INDEX` for synthetic rows); `group_key` is the
  PER/WHEN group id at emission (or row id for per-row, INVALID when ungrouped); `kind`
  is the row role. The trailing fields (added in I2) drive the elastic engine's slack
  placement: `shape` (`ElasticShape::UNSET` / `SHARED_SCALAR` / `PER_ROW_DATA`) decides
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
  user parameter. The six evaluated-constraint sites share the field-setting through
  two free functions, `StampConstraintProvenance` (the fields common to all six:
  `source_clause_id`, `repair_group_id`, `kind`, `shape`/`rhs_label`, and `group_key`/
  `group_label` where grouped) and `StampAggregateProvenance` (`is_aggregate` /
  `qualifier`, the four aggregate sites only) — see
  `../../01_pipeline/06_model_formulation/done.md` §7 for why `avg_scaled` /
  `weight_labels` / `folded_terms` stay linear-only rather than becoming a third
  shared field.
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
  unbounded escape characterization resolves to categorical slice findings
  set (`unbounded/done.md`). Global-block columns default to GLOBAL_AUX (unnamed)
  unless named through `SolverInput::global_variable_labels` (aggregate `<>`
  indicators, composed MIN/MAX z's — see `infeasible/done.md`).

Tested in `test/common/test_decidb_variable_provenance.cpp`.

## The trigger — the `DIAGNOSE` prefix

Nothing starts a diagnosis except the statement prefix. There is no session setting for
it: `diagnose_decide` and `decide_on_timeout` were deleted in batch H, together with the
whole idea of an automatic path.

- **Where it comes from.** `DIAGNOSE <select>` is a grammar alternative on
  `VariableShowStmt` (`third_party/libpg_query/grammar/statements/variable_show.y`),
  mirroring `SUMMARIZE`: one rule plus an `is_diagnose` flag on
  `PGVariableShowSelectStmt`. The transformer turns it into a `ShowRef` with
  `ShowType::DIAGNOSE`, so `select_with_parens`' existing `'(' VariableShowStmt ')'`
  production makes `SELECT * FROM (DIAGNOSE …)` compose for free.
- **How it travels.** `Binder::BindDiagnose` (`bind_showref.cpp`) binds the inner query
  unchanged, finds the one `LogicalDecide` in the resulting plan, and sets
  `LogicalDecide::diagnose = true`. `plan_decide.cpp` copies it to
  `PhysicalDecide::diagnose`. It is a property of the STATEMENT, carried parser → binder
  → logical plan → stage 08, and never read back out of a setting.
- **What it arms.** `PhysicalDecide::FinalizeSolveResult` reads the flag once as
  `diagnosis_armed` and pre-arms the shared diagnosis prep it gates:
  `SolveModelOptions::extract_unbounded_ray`, `tolerate_infeasible_bounds`, and
  retained-model capture. An unprefixed query pays for none of it.
- **The gate helper.** `DiagnosisApplies(bool armed, SolverStatus)`
  (`decide_diagnostic.cpp`) — true only under the prefix, and only for INFEASIBLE /
  UNBOUNDED / INF_OR_UNBD. `DiagnoseModeArmsDiagnosis` and `GetDiagnoseDecideMode` are
  gone with the setting they read.
- **A query with no DECIDE clause is rejected**, at bind time, by `BindDiagnose`:
  DIAGNOSE reports on an optimization run, and there is none.

Tested in `test/decide/tests/test_diagnose_trigger.py` (both backends).

**A slow solve is not covered here.** A time-limit stop, and Ctrl-C, are ordinary
execution behaviour and happen with or without the prefix — see
`../../01_pipeline/08_execution/slow_solves.md`.

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
- **`Finalize` owns orchestration:** read the statement's armed flag, call the matching
  engine for the routed status, and write its findings to the statement-scoped handoff.
  If an engine cannot produce named/actionable content, the operator writes one
  `undiagnosed` finding instead. Only an unprefixed failure falls through to
  `ThrowDecideSolveError`; success clears the handoff before the operator above emits
  its `feasible` finding.
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
are the user-facing contract and live in `00_project_overview/syntax_reference.md` §8;
this section is the mechanics.

- **`DecideDiagnostic` / `DiagnosticFinding` / `DecideDiagnosticState`**
  (`src/include/duckdb/decidb/decide_diagnostic.hpp`). A `DecideDiagnostic` is one
  `state` plus a vector of findings; a `DiagnosticFinding` is one row of the relation,
  with real columns and real types (`amount` DOUBLE, `row` BIGINT, `has_amount` /
  `has_row` for NULL).
- **A statement-scoped handoff, not a stash to read later.** `DecideDiagnosticState` is
  still a `ClientContextState` under key `decide_diagnostics`, but its job changed:
  `PhysicalDecide` writes it during its `Finalize`, and `PhysicalDecideDiagnose` — the
  operator directly above it in the same plan — consumes it in its own `Finalize` via
  `TakeDecideDiagnostic`, which clears it on the way out. Nothing may read it twice, and
  nothing outside the statement reads it at all. A successful solve calls
  `ClearDecideDiagnostic`, so a `DIAGNOSE` over a query that worked finds nothing waiting
  and reports the single `feasible` finding instead. Before batch H this was a
  cross-statement stash that `decide_diagnostics()` read back on a later statement, with
  the whole lifecycle problem (stale diagnoses, monotonic ids) that implies; that problem
  is now structurally absent.
- **`LogicalDecideDiagnose` / `PhysicalDecideDiagnose`** (`planner/operator/`,
  `execution/operator/decide/`) are the prefix's own operator pair. The logical one
  resolves its types from `GetDecideDiagnoseSchema` — the single definition of the
  columns — and the physical one is a sink that swallows the query's rows (DIAGNOSE
  reports on the run, it does not return the run's output) and a source that emits the
  findings via `RenderDecideDiagnostic`. Both are tiny: the engines already produce typed
  findings, so this is a reshape of where they are written, not new machinery.
- **`decide_diagnostics()` is deleted** — the table function, its EAV bind schema
  (`diagnosis_id, state, subject_kind, subject, attribute, value`, all VARCHAR), and its
  registration in `system_functions.cpp`. The relation it returned needed a `PIVOT` recipe
  to be read one-row-per-clause; the flat shape is that row directly, and it composes as
  a subquery instead of being a second statement.
- **Why an operator and not a result-schema switch:** DECIDE is a clause on SELECT, and
  the projection binds the decision columns before the solve runs
  (`logical_decide.cpp`, `transform_select_node.cpp`), so a runtime result-schema switch
  is bind-time-blocked. The prefix declares the schema at bind time instead, which is
  exactly what makes it a different statement rather than a different outcome of the same
  one.
- **The unbounded engine populates runaway findings** (`BuildUnboundedDiagnostic` — see
  `unbounded/done.md`). One finding per escaping variable, or one per categorical slice
  when the escape is characterized: `clause` names the variable, `edit_source` carries
  the direction, `group` names the slice, `amount` counts it, and
  `suggested_change` prescribes the forced remedy without inventing the cap. The
  unbounded characterization adds
  three sticky extension options:
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
