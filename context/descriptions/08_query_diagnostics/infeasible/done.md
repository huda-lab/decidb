# Query Diagnostics — Infeasible (how it works)

> Router terminal: **failed → infeasible** (`elastic` → report). See `router/README.md`.

The feasible region is empty — no assignment satisfies every `SUCH THAT` constraint at
once. Under `PRAGMA diagnose_decide='auto'`, the infeasible terminal now builds a second
optimization, the **elastic program**, whose optimum is the least-change fix for shipped
I1/I2 shapes: which user constraints to loosen, and by how much. Shared plumbing it builds
on (the pragma gate, provenance, the reporting relation) is in `foundations/done.md`.
Remaining objective/removal/reporting work is tracked in `todo.md`.

## Engine seam: infeasible

The seam is wired and now feeds the elastic engine. On an infeasible solve under `auto`,
`PhysicalDecide::Finalize`'s `DiagnosisTerminal::INFEASIBLE` arm calls `DiagnoseInfeasible`
(`decide_diagnostic_engines.cpp`), mirroring the unbounded arm: a valid diagnosis is
stashed (`StashDecideDiagnostic`) and surfaced (`ThrowDecideDiagnosisReady`); otherwise
control falls through to the static error (`ThrowDecideSolveError`). Under `off` the router
returns `UNDIAGNOSED` and the arm is never reached. A residual `INF_OR_UNBD` (empty ray) is
normalized to `INFEASIBLE` in the arm before the message is built.

**Engine boundary (mirrors `DiagnoseUnbounded`).** `InfeasibleDiagnosisInput`
(`decide_diagnostic_engines.hpp`) is the unbounded input with one structural swap: it
carries the built **`SolverModel`** (the elastic transform will reshape its rows) in place
of a solved ray, alongside the `VarIndexer`, per-variable labels / is-aux flags,
`DecideDiagParams`, and an **injected solve callback**
`std::function<SolverResult(const SolverModel &)>`. The callback lets the engine run the
elastic re-solve without depending on the operator or the solver facade — `Finalize` binds
it to `SolvePreparedModel` on the primary solve's backend (`SelectSolverBackend` is
deterministic, so the re-solve uses the same backend). This keeps the engine
solver-agnostic and unit-testable, the same boundary as the unbounded engine's
`get_candidates` injection. The engine will build its own `ClauseRowIndex` from the model
(`BuildClauseRowIndex` is pure), so that index is not carried in the input.

**Model retention.** `SolveModel` builds the `SolverModel` as a local and discards it —
only the `SolverResult` returns — and `SolverModel::Build` *moves* the global constraints
out of the `SolverInput`, so the base model is gone by the time the arm runs and cannot be
faithfully rebuilt from the gutted input. To give the engine a model to transform,
`SolveModel` takes an optional out-param **`SolverModel *retained_model`**
(`ilp_solver.hpp`): `Finalize` passes it only when diagnosis is armed, and the built model
is moved into it after the solve completes (the post-solve disambiguation / ray helpers
stay file-private inside `ilp_solver.cpp`, so all solve orchestration keeps its single
home). The retained model is freed when `Finalize` returns.

**Label dedup.** Per-decide-variable labels + is-aux flags (for column provenance) are now
built by a shared `build_var_labels` lambda in `Finalize`, used by both the UNBOUNDED and
INFEASIBLE arms (previously inline in the unbounded arm only).

## Elastic engine: stage-1 core (simple shapes) + elastic-infeasible signal

I1 fills the seam: on an infeasible solve under `auto`, `DiagnoseInfeasible` builds and
solves a **second** optimization — the *elastic program* — whose optimum is the
least-change fix. Each relaxable user constraint gets a non-negative slack that lets its
RHS stretch; minimizing the total loosening, the positive-slack support names the
constraints to edit and the slack values are the amounts.

```
stage 1:   min  Σ wᵢ sᵢ              (uniform wᵢ = 1)
           s.t. Aᵢ x ≤ bᵢ + sᵢ ,  sᵢ ≥ 0    (relaxable rows + relaxable single-instance bounds)
                structural / mechanism rows rigid
           →  support {i : s*ᵢ > 0} names the edits; s*ᵢ is the amount
```

**Operator / engine split.** The engine is solver-agnostic and free of DuckDB
`Expression` types, so the work splits across the I0 boundary:

- **Operator (`physical_decide.cpp`).** Knows the `Expression`s and the sink state, so it
  handles **absorbed bounds (decision 1a)**. A user `x <= 10` / `x BETWEEN a AND b` is
  pre-absorbed into the column-bound arrays (never a row), so it carries no provenance and
  the row-based engine can't see it. `TraverseBoundsConstraints` now also records each
  absorbed user bound as a `UserBoundSpec {decide_var_idx, sense, k}` (COMPARISON → one
  spec; **BETWEEN → two**, the previously-untracked side now captured). In the
  `DiagnosisTerminal::INFEASIBLE` arm, for each spec the operator appends a `USER_PARAMETER`
  row (`coeff 1·x sense k`) to the retained model and **relaxes the rigid column bound** for
  that direction (to ±1e30) so the bound is enforced only by the loosenable row. A bound on a
  **multi-instance** variable fans into one row per instance under a single synthetic
  `clause_id`, shape `SHARED_LITERAL`, so the engine collapses them to one shared slack (the
  knob is one literal `K` → report the max overshoot — **I2.a**). Single-instance is the N=1
  case. Default non-negativity (`lower = 0`) and the BOOLEAN `0/1` box stay rigid: a BOOLEAN
  variable is lowered to an INTEGER with synthesized `x >= 0` / `x <= 1` domain constraints
  (so its runtime type is INTEGER — indistinguishable by type from a genuine integer bound),
  so `TraverseBoundsConstraints` consults `op.is_boolean_var` (threaded from `LogicalDecide`)
  and **does not record** the domain bounds in `user_absorbed_bounds`; only genuine user
  bounds are re-emitted. `has_unhandled_user_bounds` therefore stays `false`.
- **Engine (`decide_diagnostic_engines.cpp`, `DiagnoseInfeasible`).** Pure model math.
  Copies the model, rebuilds the objective as `min Σ sᵢ` (zeroes the user objective, drops
  the quadratic objective, `maximize=false`), and adds a slack to every relaxable **linear**
  row (`IsRelaxableForElastic` ⇒ `USER_PARAMETER`). Quadratic rows and
  `STRUCTURAL`/`USER_MECHANISM` rows stay rigid — quadratic-RHS slack is I2; ABS pin rows
  are already `USER_PARAMETER` and are picked up automatically while their envelopes
  (`STRUCTURAL`) stay rigid. **Slack direction (decision 3):** `≤` → `−s` (`Ax − s ≤ b`),
  `≥` → `+s` (`Ax + s ≥ b`), `=` → **two** non-negative slacks `−s⁺ + s⁻` (stays linear,
  uniform `sᵢ ≥ 0`). **Slack type (decision 4):** REAL, `≥ 0`, uncapped, even for an
  integer RHS. It re-solves through the injected `solve_model` callback (same backend as the
  primary solve).

**Reading the result.** On `OPTIMAL`, every slack above `DIAGNOSTIC_RAY_EPSILON` is an
edit; the amount is the slack value (`=` reports the net `s⁺ − s⁻`). Each clause is labelled
by reconstructing its algebra from the **original** row's coefficients over user-facing
column names (`BuildColumnProvenance`) — `x <= 5` → suggestion `x <= 10` — so no source
expression text needs threading. `BuildInfeasibleDiagnostic` emits, per edit, EAV rows
`subject_kind='clause'`, `subject=<clause as written>`, `attribute='suggested_change' |
'amount'`, plus an actionable one-line summary ("the constraints cannot all be satisfied at
once. Loosen `x <= 5` to `x <= 10`.").

**Elastic-infeasible signal.** If the elastic program is *itself* infeasible, the conflict
reaches rigid rows → `BuildElasticInfeasibleDiagnostic` renders a distinct outcome
(`subject_kind='model'`, `attribute='elastic_infeasible'`, summary: loosening your SUCH THAT
limits cannot fix it). This is claimed **only** when every user constraint was actually made
relaxable: the `has_unhandled_user_bounds` flag (set when the operator absorbed a user bound it
could not re-emit) suppresses the verdict and falls through to the static error rather than
wrongly declaring the query unfixable. As of I2.a multi-instance bounds *are* re-emitted, so the
flag stays `false` in practice and remains as a defensive guard. Likewise, when there are no
relaxable rows at all, the engine returns invalid (static error).

**Scope (deferred to later phases).** The achievable-objective re-solve is **I3**. The
L0/removal dial is **I4** (`<>` remove-only depends on it). Full `decide_diagnostics()`
rendering (runnable rewritten query) is **I5**. All per-shape slack placement and unit
conversion (**I2.a–I2.e**) has shipped — see "per-shape slack placement" below.

## Elastic engine: per-shape slack placement (shared blocks, PER, multi-instance bounds)

I2.a/I2.b attach slack at the **editable-knob** level, not the matrix-row level, for the
shapes where one user knob fans into several rows. **This is the reason the engine is
hand-rolled** — Gurobi `feasRelax` is one-slack-per-row and cannot share.

**The elastic transform is a pure function (`BuildElasticModel`, I2.0).** Extracted from
`DiagnoseInfeasible` into `ElasticModel BuildElasticModel(const SolverModel &base)`
(`decide_diagnostic_engines.{hpp,cpp}`): it zeroes the user objective, drops the quadratic
objective, and adds slacks, returning the transformed model + a `vector<BlockSlackRef>`
(`{rows, pos_col, neg_col, sense}`) recording the slack→row wiring. A **size-1 block
reproduces I1 exactly.** `DiagnoseInfeasible` calls it, solves via the injected callback, and
reads each block back. Exposing it is also the structural test seam.

**Shape marker (`ConstraintProvenance::shape`, `decide.hpp::ElasticShape`).** `SHARED_LITERAL`
= the RHS is one editable literal `K` shared across every row the clause emits;
`PER_ROW_DATA` = genuine per-row **data** RHS (`x <= col`). Set in `ilp_model_builder.cpp`:
the per-row builder tags a row `SHARED_LITERAL` when `EvaluatedConstraint::rhs_is_shared_literal`
(set in `physical_decide.cpp` when `rhs_expr` is a `BOUND_CONSTANT`), else `PER_ROW_DATA`;
the **aggregate** sites tag their rows `SHARED_LITERAL` directly (an aggregate's RHS is always a
single scalar knob), so `PER_ROW_DATA` means *exactly* "data RHS"; the absorbed-bound re-emission
tags its rows `SHARED_LITERAL`. Quadratic constraints leave the default and are always treated as
editable (see I2.d below).

**Grouping.** `BuildElasticModel` groups relaxable `SHARED_LITERAL` rows by
`(clause_id, group_key)` into one block → **one shared slack** wired into all N rows
(`=` blocks get one shared `s⁺` and one shared `s⁻` — dual shared slacks). With `eᵢ − s ≤ K`
on every row the single `s` is driven to the **max overshoot**, so the reported edit is the
max (not the per-row sum) and the global "which clause to relax" race is not skewed ≈N×.
This covers **easy MIN/MAX** (`MAX(e) ≤ K` → per-row `eᵢ ≤ K`, absorbed as a shared cap),
**per-row literal** constraints, and **multi-instance bounds** (`x ≤ K` over N instances).
**PER** keys on `(clause_id, group_key)`, so each group is its own block: an aggregate
`SUM(x) ≥ K PER g` emits one row per group, hence one slack per group (clean per-group edits).
**easy-MAX+PER is the exception:** an easy-MAX cap is absorbed as a column bound, which does
**not** preserve the PER grouping, so it re-emits as one *global* shared block — the same
user-facing edit (`K → K + max overshoot`) but without per-group granularity. Per-group
enrichment for absorbed easy-MAX+PER is a later refinement (it would require carrying group
keys through bound absorption).

**Edit conversion (the D3 helper).** Reading the slack support is centralized in
`MakeLoosenEdit(provenance, lhs, rhs, sense, amount)` (`decide_diagnostic_engines.cpp`), the one
place per-shape rendering lives: it renders the sense, re-quotes a strict `<`/`>` against the
typed literal, and formats the suggestion. The LHS is rendered shape-aware — plain `FormatTerms`,
`FormatLhs` (AVG-collapsing), or `FormatQuadraticLhs`. A `ClauseEdit` carries a
`ClauseEditKind` (`LOOSEN` vs `CONFLICT_SUMMARY`, `decide_diagnostic.hpp`) so the builder emits
either a `suggested_change`/`amount` pair or a single `conflict` row.

### I2.c — data-RHS conflict summary + editable-knob preference

A `PER_ROW_DATA` clause (`x <= col`) has a per-row data RHS: there is no single literal to
"loosen to K", so editing the query cannot fix it row-by-row. Two parts:

- **Reporting.** `DiagnoseInfeasible` partitions the positive-slack blocks: data-RHS blocks are
  grouped by `clause_id` and emit **one `CONFLICT_SUMMARY` per clause** — `subject` = the clause
  as written, `attribute='conflict'`, `value="conflicts in M of N rows"` (N = total rows the
  clause emitted) — never a `suggested_change`/`amount`. The user-facing decision was *one
  summary row per clause* (name the clause, no per-row spam).
- **Editable-knob preference.** In a degenerate infeasibility an editable knob and a data row can
  be equally-optimal fixes, and the solver would split arbitrarily (a messy, non-deterministic
  mix). To prefer an *actionable* edit, a data-RHS slack carries a higher objective weight
  (`DIAGNOSTIC_DATA_SLACK_WEIGHT`, `diagnostic_constants.hpp`); the solver loosens editable
  constraints first and reports a data conflict only when editable loosening genuinely cannot
  restore feasibility. (A coarse stand-in for the I3 lexicographic tie-break.)

### I2.d — AVG / strict / quadratic in the user's units

- **AVG.** The AVG→SUM rewrite pre-scales row coefficients by `1/N_g`
  (`ScaleAvgRowCoefficients`), so the slack is **already in AVG units** — reported raw, no
  `/N_g`. A pure-AVG row is tagged `provenance.avg_scaled` (set in `physical_decide.cpp` when
  every linear term is AVG-scaled, propagated at the aggregate provenance sites in
  `ilp_model_builder.cpp`); `FormatLhs` then collapses the `1/N`-scaled terms back to `AVG(x)`
  (inner coeff = stored coeff × the variable's term count) instead of rendering `0.5*x + 0.5*x`.
- **Strict `<` / `>`.** The δ offset (`< K → <= ceil(K)-1`, `> K → >= floor(K)+1`) is baked into
  `rhs` at the δ site (`ApplyComparisonSense`, `ilp_model_builder.cpp`), which now also sets
  `provenance.strict` + `provenance.typed_k` (the user's literal). `MakeLoosenEdit` re-quotes the
  suggestion against `typed_k` and renders `<` / `>`, e.g. `x < 10` → `x < 16`.
- **Quadratic.** `BuildElasticModel` loops `model.quadratic_constraints` and slacks the **linear
  RHS only** (never the Q matrix): `e(x) + xᵀQx ≤ K` loosens to `… ≤ K + s`. The `BlockSlackRef`
  carries a `quadratic` flag (its `rows` then index `quadratic_constraints`); `FormatQuadraticLhs`
  renders `POWER(x, 2)` / `x*y` terms. Quadratic slacks are always editable-weighted (a quadratic
  is always a LOOSEN edit, never a data conflict). QCQP is Gurobi-only.

### I2.e — rigid shapes confirmed

`<>` indicator rows (`USER_MECHANISM`) and McCormick bilinear links (`STRUCTURAL`) are not
relaxable (`IsRelaxableForElastic`), so the elastic transform attaches no slack to them: a `<>`
or bilinear conflict is resolved by loosening an editable bound, and a conflict reachable only
through those rigid rows falls through to the static error (the empty-block guard). `<>` is
remove-only; actual removal is the **I4** L0 dial — I2 only confirms the rigid behavior.

**Tests.** C++ structural (`test_decidb_diagnostic_engines.cpp`): one shared slack spans all N
rows with the correct sign; distinct groups → distinct blocks; data-RHS rows stay independent
size-1 blocks; `USER_MECHANISM`/`STRUCTURAL` rows are never slackened; a penalized data slack
defers to an editable knob; a data-RHS conflict reports a per-clause summary; strict re-quotes
against `typed_k`; an AVG row renders `AVG(x)` with the raw slack; a quadratic constraint slacks
its linear part only with Q untouched. C++ behavioral: a shared block with rigid floors reports
the max overshoot (7), one edit, not the sum (10). Python differential
(`test_query_diagnostics_relation.py`): easy-MAX and multi-instance bound report total = max
overshoot; aggregate `SUM PER g` reports one exact edit per group; a BOOLEAN model loosens its
SUM target, never its 0/1 domain (regression guard); a data-RHS clause reports a conflict
summary; a penalized data row defers to an editable edit; AVG/strict re-quote; quadratic and
McCormick gated to Gurobi; `<>` and McCormick rows stay rigid.

## Elastic engine: column-bound conflicts (intrinsic reset, inverted box, type-domain errors)

A user limit can live in a variable's **column bounds** instead of a matrix row, where the
row-only elastic engine can't relax it. The built `SolverModel`'s `col_lower`/`col_upper` is a
*fused* product — intrinsic domain (REAL/INT `[0,+∞)`, BOOLEAN `[0,1]`) + absorbed user bounds +
implied tightenings from `DecidePropagateImpliedBounds` (a presolve pass with no provenance).
Three mechanisms make every column-derived user limit diagnosable while keeping the intrinsic
domain rigid:

- **Intrinsic reset (the elastic box carries only the intrinsic domain).** In the
  `DiagnosisTerminal::INFEASIBLE` arm, after re-emitting absorbed bounds as slackable rows (the
  decision-1a loop above), a second pass resets every **non-BOOLEAN** decide column's *implied*
  tightenings back to intrinsic: `col_lower > 0 → 0`, `col_upper < +∞ → +∞`. This is safe —
  propagation only ever tightens (raises lower / lowers upper), and every implied tightening has
  a backing row (`USER_PARAMETER` slackable, or `STRUCTURAL` still-rigid) that keeps enforcing it.
  This is what lets `x <= 2+3` (a foldable cap copied into `col_upper`) be loosened by its row
  instead of pinned by the box. **BOOLEAN columns are skipped via `is_boolean_var[var]`, not
  `is_binary[col]`** — a BOOLEAN is lowered to an INTEGER carrying a `[0,1]` box, so `is_binary`
  is `false` for it; gating on `is_binary` would reset its upper to `+∞` and silently make it
  unbounded.
- **Inverted-box survival (`SolverInput::tolerate_infeasible_bounds`).** Two opposite absorbed
  bounds (`x <= 4 AND x >= 10`) invert the box (`col_lower > col_upper`). Without help,
  `SolverModel::Build` throws before `retained_model` is populated, so the engine never sees the
  model. Under diagnosis the flag (set in `Finalize`, same gate as the unbounded-ray extraction)
  makes `Build` keep the inverted box, and `SolveModel` short-circuits to INFEASIBLE **without
  handing the box to the backend** — HiGHS hard-rejects `lb>ub` at load and poisons the session;
  Gurobi tolerates it, so the short-circuit is the solver-agnostic path. The intrinsic reset then
  un-inverts the box for the elastic re-solve, which loosens one of the two bounds.
- **Type-domain conflicts are a static error, not an elastic edit.** A user bound that
  contradicts the *intrinsic* domain — `x <= -1` on a non-negative REAL, `x >= 2` / `x = 2` on a
  BOOLEAN — can't be fixed by loosening (only by changing the type or dropping the bound), so it
  never reaches the engine. A post-absorption check in the sink constructor (right after
  `TraverseBoundsConstraints`) throws a precise static `InvalidInputException`
  (`"x >= 2 cannot hold because x is BOOLEAN (0 or 1). …"`). The guard is `U < 0 AND L >= 0`
  (upper below the non-negativity floor) or `is_boolean AND L > 1` (lower above the 0/1 ceiling),
  so an explicitly-lowered floor (`x <= -1 AND x >= -5`, box `[-5,-1]`) stays feasible and a
  purely user-vs-user inverted box (`x >= 5 AND x <= 1`, both ≥ 0) proceeds to the elastic engine.

A deferred sibling — an uncorrelated scalar-subquery RHS (`x <= (SELECT 5)`), which the optimizer
flattens into a join so it can't be classified as a shared cap without correlation analysis — is
tracked in `todo.md`.

## Tests

`test/common/test_decidb_diagnostic_engines.cpp` — SECTIONs drive `DiagnoseInfeasible`
against the bundled HiGHS backend on one-variable models: a relaxable cap conflicting with
a rigid floor reports the unique minimal loosening (`x <= 5` → `x <= 10`, amount 5); an
equality row loosens via its two-sided slack (`x == 5` → `x == 8`); a rigid-only conflict
with a non-helping relaxable row renders the elastic-infeasible row; the
`has_unhandled_user_bounds` flag suppresses that claim (invalid → static error); and an
all-rigid model returns invalid. End-to-end differential coverage (both backends, vs
`oracle_solver`, including absorbed-bound and BETWEEN cases) is in
`test/decide/tests/test_query_diagnostics_relation.py`.
