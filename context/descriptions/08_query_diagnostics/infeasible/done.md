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
  bounds are re-emitted. Reversed user bounds such as `5 >= x` are flipped by the
  constraint binder before this pass, so they follow the same absorption and re-emission
  path as `x <= 5`. `has_unhandled_user_bounds` therefore stays `false`.
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
'amount'`, plus an actionable one-line summary. The summary names the concrete edit(s)
inline, mirroring the unbounded terminal: with up to three actionable edits it quotes them
("… — loosen `x <= 5` to `x <= 10`." / "… remove `x <> 1`." / Oxford-joined with "or"); with
four or more it falls back to a kind-named cue ("loosen one of your SUCH THAT limits") and
defers the per-clause detail to the relation. A data-RHS-only conflict keeps its own cue
("the conflict is in per-row data, with no single limit to loosen").

**Elastic-infeasible signal.** If the elastic program is *itself* infeasible, the conflict
reaches rigid rows → `BuildElasticInfeasibleDiagnostic` renders a distinct outcome
(`subject_kind='model'`, `attribute='elastic_infeasible'`, summary: loosening your SUCH THAT
limits cannot fix it). This is claimed **only** when every user constraint was actually made
relaxable: the `has_unhandled_user_bounds` flag (set when the operator absorbed a user bound it
could not re-emit) suppresses the verdict and falls through to the static error rather than
wrongly declaring the query unfixable. As of I2.a multi-instance bounds *are* re-emitted, so the
flag stays `false` in practice and remains as a defensive guard. Likewise, when there are no
relaxable rows at all, the engine returns invalid (static error).

**Scope (deferred to later phases).** The achievable-objective re-solve (**I3**) has
shipped — see "stage-2 achievable objective (freeze-budget)" below. The L0/removal dial is
**I4** (`<>` remove-only depends on it). Full `decide_diagnostics()` rendering (runnable
rewritten query) is **I5**. All per-shape slack placement and unit conversion
(**I2.a–I2.e**) has shipped — see "per-shape slack placement" below.

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
typed literal, and formats the suggestion. The LHS is rendered shape-aware by `FormatLhs` —
plain `FormatTerms`, AVG-collapsing (`FormatAvgLhs`), or SUM-collapsing (`FormatSumLhs`) —
or `FormatQuadraticLhs` for a quadratic constraint. A `ClauseEdit` carries a
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
- **SUM.** An aggregate `SUM` over rows emits one solver column per row for the same decide
  variable, so a plain reconstruction reads `x + x + x`. For an **ungrouped** aggregate
  (`provenance.group_key == INVALID`), `FormatSumLhs` collapses the per-row fan-out back to
  `SUM(c*var)` — uniform coefficient only (`SUM(price * x)`, data-varying, falls back to the raw
  reconstruction); a variable appearing once stays as written. PER-grouped aggregates are left
  expanded (their differing row counts are what distinguishes one group's edit from another's in
  the relation today) until group identity is surfaced as its own field.
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
summary; a penalized data row defers to an editable edit; reversed bounds are reported as their
canonical absorbed form; AVG/strict re-quote (including strict quadratic); quadratic and
McCormick gated to Gurobi; `<>` and McCormick rows stay rigid.

## Elastic engine: L0 / removal dial (`<>` removal)

I4 adds the **removal dial** for clauses that cannot be *loosened*, only *removed* — the
flagship case is `<>` (not-equal). A `x <> 3` compiles to two rigid `USER_MECHANISM` Big-M
disjunction rows (`x − M·z ≤ K−1`, `x − M·z ≥ K+1−M`) sharing one binary disjunction
selector `z`; there is no scalar RHS to stretch, so the loosening passes skip them. I4 lets
the engine *drop* such a clause and reports it as a `DROP` edit ("Remove `x <> 3`").

**Scope.** Removal applies to both **per-row `<>`** (`x <> 3`) and **aggregate `<>`**
(`SUM(x) <> K`). `STRUCTURAL` rows (McCormick links) stay rigid — a definition row cannot be
meaningfully dropped. The two `<>` shapes differ only in *where* the disjunction binary and
its label live (per-row = row-scoped column labeled via `var_labels`; aggregate =
global-block column labeled via the `global_variable_labels` channel — see below); the
removal mechanic, weighting, and reporting are identical.

**Marker = `ConstraintProvenance::indicator_col`** (`ilp_model.hpp`). A flat-column field set
at the two `<>` mechanism sites (per-row *and* aggregate), so it does triple duty: it marks a
row as removable (`!= INVALID`), groups the disjunction pair (rows sharing one `z` = one `<>`
instance), and sources both the removal Big-M and the label.
- **Per-row.** It carries the `<>`'s indicator decide-var on the expanded rows:
  `physical_decide.cpp` stamps `ec1.ne_indicator_idx = ec2.ne_indicator_idx =
  indicator_var_idx` on the two disjunction rows, and the per-row builder site
  (`ilp_model_builder.cpp`) resolves `provenance.indicator_col = indexer.Get(ne_indicator_idx,
  row)`.
- **Aggregate.** The deferred aggregate-`<>` expansion (`physical_decide.cpp`) allocates one
  global binary `z` per group and emits its two rows as `SolverInput::RawConstraint`s with
  `indicator_col = z_idx`. The global-constraint copy in `ilp_model_builder.cpp` propagates
  `raw.indicator_col → constr.provenance.indicator_col`, so the global rows group exactly like
  per-row rows.

The previously-INVALID clause_id of these rows is untouched, so no existing clause-id consumer
is disturbed.

**Label channel for global-block indicators (aggregate `<>`).** A per-row `<>` indicator is a
decide-var column, so `BuildColumnProvenance` already names it from `var_labels`. An aggregate
`<>` binary lives in the global block, which `BuildColumnProvenance` otherwise leaves unnamed —
so a dropped aggregate `<>` would have an empty subject. `SolverInput::global_variable_labels`
(parallel to `global_variable_types`) carries the clause text `SUM(x) <> K` — looked up from
`aux_var_expressions` at the aggregate-`<>` site and empty for every other global aux (MIN/MAX,
McCormick). It is reconciled to the final global-var count with one `resize(num_global_vars)`
before `SolveModel` (the aggregate-`<>` globals are allocated first, so the labels form a
contiguous prefix and the trailing MIN/MAX globals pad with ""). `BuildColumnProvenance` takes
it as an optional argument and writes the labels onto the global-block columns; the infeasible
engine forwards it through `InfeasibleDiagnosisInput::global_variable_labels`. The DROP edit
then reads `columns[indicator_col].label` uniformly for both shapes.

**Mechanic (all-or-nothing binary, no separate gate row).** Because `<>` removal is binary,
`BuildElasticModel` (`decide_diagnostic_engines.cpp`), after the loosening + quadratic
passes, groups relaxable rows by `indicator_col` and adds **one binary `w` per `<>`**, wired
into each disjunction row with a `±M₂` coefficient (sign by sense, reusing the slack
convention: `<` → `−M₂`, `>` → `+M₂`). `w = 1` makes both rows vacuous — the clause is
dropped. **M₂** defaults to the clause's own disjunction Big-M (`|row coeff on indicator_col|`,
provably enough to neutralize whichever side `z` selects), overridable by the
`diagnose_decide_removal_bigm` pragma. The wiring is recorded in a `RemovalRef {rows, w_col,
indicator_col}` on `ElasticModel`.

**Last-resort weighting (B1).** `w` is penalized `DIAGNOSTIC_REMOVAL_WEIGHT` (`1e6`,
`diagnostic_constants.hpp`), stacked **above** the editable (`1`) and data (`1e3`) slack
weights, all in the one stage-1 objective (no extra solve). So the solver prefers any
loosening and drops a `<>` only when loosening genuinely cannot restore feasibility; the
support `{i : wᵢ = 1}` is the minimum-cardinality removal set. Like the data-slack weight
this is a coarse weighted stand-in that rides the future lexicographic-tier conversion (see
"Notes to revisit" in `todo.md`).

**Reading + reporting.** `ReadElasticEdits` reads each `RemovalRef`: `w > 0.5` emits a
`ClauseEdit{kind = DROP, label = columns[indicator_col].label}` (the F6 `x <> 3` string).
The F6 label is built by `DiagnosisComparand` (`decide_optimizer.cpp`), which unwraps the
implicit `CAST` the binder inserts around a literal and drops the outer parens, so the clause
reads `x <> 1`, not `(x <> CAST(1 AS INTEGER))`. `BuildInfeasibleDiagnostic` renders a DROP as
a dedicated EAV row `subject_kind='clause'`, `subject='x <> 3'`, `attribute='edit_kind'`,
`value='drop'` (distinct from the LOOSEN `suggested_change`/`amount` pair), and names the fix
inline in the summary ("… remove `x <> 1`."). The
`DiagnoseInfeasible` empty-guard now bails only when **both** slacks and removals are empty,
so a removal-only model is still diagnosed.

**Stage-2 composition (I3).** Stage 2 runs when there is an objective **and** an actionable
fix (`HasLoosenEdit || HasRemoval`). `BuildStage2Model` **freezes the removal set** by pinning
each `w` to its stage-1 value (`col_lower = col_upper`) and extends the budget row over the
`w` columns too, so the cap stays exactly `S* = stage-1 objective` (which included the
removal penalty). The reported DROP set is therefore stable between stages; letting `w`
re-optimize the dropped set is deferred to I5.

**Pragma.** `diagnose_decide_removal_bigm` (DOUBLE, default `0` = auto-derive, `>= 0`,
`decide_diagnostic.cpp`) threads through `DecideDiagParams::removal_bigm` into
`BuildElasticModel`. The objective weight `W` stays internal.

**Tests.** C++ structural (`test_decidb_diagnostic_engines.cpp`): a `<>` pair gets one binary
`w` wired `−M₂`/`+M₂` with `obj = DIAGNOSTIC_REMOVAL_WEIGHT` and a `RemovalRef`; the pragma
override replaces M₂; a pinned-`<>` must-drop reports `edit_kind='drop'`; a loosenable
conflict prefers the LOOSEN and never drops; an **aggregate `<>`** whose disjunction binary is
a global-block column is named via the `global_variable_labels` channel (not `var_labels`) and
reported as a drop. Python differential (`test_query_diagnostics_relation.py`, both backends):
`x <> 0 AND x <> 1` on a BOOLEAN drops exactly one `<>` (min cardinality), with the achievable
objective differential-checked against a re-solve of the fixed query; `SUM(x) <> 0 AND SUM(x)
<> 1` (aggregate) drops exactly one *named* aggregate `<>`, also differential-checked; `x <> 5
AND 5 ≤ x ≤ 5` prefers loosening; the pragma override produces the same drop and a negative
value is rejected at SET time.

## Elastic engine: stage-2 achievable objective (freeze-budget)

I3 reports the **achievable objective** the user gets after the minimal fix — and the
specific edit that achieves it. Stage 1 (above) finds the least *total* loosening `S*`, but
when that minimum is non-unique it returns an **arbitrary** minimizer whose edit need not be
the one best for the user's objective. Stage 2 is a second lexicographic tier: among all
min-loosening fixes, pick the one that maximizes the user's original objective, and report
**that** edit so the edit and the objective agree.

**Method (freeze the budget, not the amounts).** `BuildStage2Model`
(`decide_diagnostic_engines.cpp`) starts from the stage-1 elastic model (slacks already
wired) and:
- adds a rigid **budget row** `Σ wᵢ sᵢ ≤ S*` over the slack columns. The weights are read
  back from the elastic model's objective coefficients (editable `1`, data
  `DIAGNOSTIC_DATA_SLACK_WEIGHT`), so the row freezes *exactly* the stage-1 objective. `S*`
  is `stage1.objective_value` (the stage-1 solve minimized `Σ wᵢ sᵢ`, so its objective value
  *is* the total loosening). The cap is exactly `S*`, no explicit ε: the stage-1 optimum sits
  on the boundary and the **backend feasibility tolerance** supplies the cushion that keeps
  the re-solve feasible (this is decision 1 — "pick against the backend feasibility
  tolerance" — realized by letting that tolerance *be* the tolerance).
- **restores the user's original objective** (linear `obj_coeffs`, the quadratic objective
  `q_*` / `has_quadratic_obj` / `nonconvex_quadratic`, and the `maximize` sense), with the
  slack columns set to zero objective weight so the reported objective is exactly `cᵀx` over
  the user's variables. Stage 1 had zeroed and dropped the objective; stage 2 needs it back.

`DiagnoseInfeasible` runs stage 2 only when there is a **real objective** (`HasObjective`)
**and** an editable `LOOSEN` edit (`HasLoosenEdit`): a constant objective has nothing to
report, and a data-RHS-only conflict has no actionable edit, so an achievable-objective
number would be misleading (those cases fall back to the stage-1 edit list unchanged).

**Reading the result.** On `OPTIMAL`, the stage-2 slacks are re-read into the edit list
(`ReadElasticEdits`, the shared stage-1/stage-2 reader) and the objective value is reported.
Because the budget is enforced only to the feasibility tolerance, a maximizing re-solve can
ride that tolerance (a clean `10` arriving as `10.000001`), so stage-2 amounts and the
objective are snapped by `SnapDiagnosticValue` — snap to the nearest integer when within a
relative tolerance of one (recovering integer bounds at **any** magnitude; significant-figure
rounding used to mangle `1234567890` into `1234570001`), else trim to a fixed absolute
precision. The stage-1 read is exact and is **not** snapped.

**Objective value plumbing.** `SolverResult` carries `objective_value`, populated on
`OPTIMAL` by both backends in the model's own sense (`gurobi_solver.cpp` via
`GRB_DBL_ATTR_OBJVAL` — a new `getdblattr` scalar getter was added to the loader;
`deterministic_naive.cpp` via HiGHS `getInfo().objective_function_value`). The engine reads
it for both `S*` (stage 1) and the achievable objective (stage 2) — ground truth, no `cᵀx`
re-derivation and no quadratic-convention guessing.

**Reporting.** `BuildInfeasibleDiagnostic` gained two optional arguments
(`achievable_objective`, `unbounded_after_fix`). When set it appends "After this change, the
best achievable objective is `<value>`." to the summary and emits one EAV row
`subject_kind='model'`, `attribute='achievable_objective'`, `value=<number>`. `S*` itself is
**not** surfaced (decision 4) — the headline fact is the objective, not the internal total.

**Stage-2 unbounded (decision 2).** If the relaxed region is unbounded in the objective
direction (`UNBOUNDED` / `INF_OR_UNBD`), there is no finite optimum: the engine keeps the
stage-1 edit (still valid) and reports "After this change, the objective is unbounded." with
the model row value `'unbounded'`. It does **not** hand off to the unbounded engine — the
edit is the actionable part, and composing two diagnoses would muddy the message.

**Tests.** C++ (`test_decidb_diagnostic_engines.cpp`): a non-unique-minimizer 2-var case
(caps `x ≤ 0`, `y ≤ 0` feeding rigid `x + y ≥ 10`, `MAXIMIZE x`) reports the x-cap edit and
objective `10` — loosening `y` would give `0`; a stage-2-unbounded case (`MAXIMIZE y` with
`y` free) reports the edit plus `achievable_objective = 'unbounded'`. Python differential
(`test_query_diagnostics_relation.py`): the same shape end-to-end on both backends, with the
reported objective checked against an independent re-solve of the fixed query (the edit
applied), never a hand-computed value. Existing infeasible tests that carry an objective now
exercise stage 2 transparently — their reported edits are unchanged.

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

## Infeasibility reporting: lean cue summary + frozen vocabulary (I5)

I5 is the final reporting step. The structured EAV edit list was already emitted by I1–I4; I5
makes two refinements and **locks the vocabulary**.

**Lean cue summary (mirrors the unbounded terminal).** `BuildInfeasibleDiagnostic`
(`decide_diagnostic.cpp`) no longer inlines the *specific* edit (`"Loosen x <= 5 to x <= 10."`).
The headline is a one-clause cue naming the *kind* of fix; the specific clause/amount live in the
table (`decide_diagnostics()`), and every thrown message already appends
`Details: SELECT * FROM decide_diagnostics();`. The cue is adaptive over which edit kinds the
solve produced:
- LOOSEN present → `"…; a possible edit was found to make it feasible — loosen one of your SUCH THAT limits."`
- DROP (no loosen) → `"… — remove one of your SUCH THAT constraints."`
- both → `"… — loosen or remove one of your SUCH THAT constraints."`
- CONFLICT_SUMMARY only (no actionable editable edit) → `"…; the conflict is in per-row data, with no single limit to loosen."`

The word *"a possible edit"* (not "the fix") is the honesty framing — the slacks give **one**
hitting set, not the complete conflict collection — so no separate caveat row is needed. The I3
achievable-objective sentence is **kept inline** after the cue (`"After this change, the best
achievable objective is …."` / `"… the objective is unbounded."`). `BuildElasticInfeasibleDiagnostic`
(the "loosening cannot fix it" path) is untouched — the cue does not apply when no edit was found.

**Why no runnable rewritten query.** The original plan envisioned emitting a copy-paste rewritten
DECIDE query. It was deliberately dropped: the engine has no access to the original SQL text (it
works on `SolverModel` rows, and clause labels are *reconstructed* + canonicalized — the user's
`5 >= x` comes back as `x <= 5`), so a faithful full query is unbuildable and a reconstructed
partial would duplicate what the table already carries. The table is the source of truth; the
headline is a lean pointer, exactly like the unbounded terminal.

**Frozen EAV vocabulary.** Every edit row now carries a uniform `attribute='edit_kind'`, so
filtering `attribute='edit_kind'` enumerates all edits and their kinds — the relation is
self-describing:
- `subject_kind='clause'`: `edit_kind` ∈ {`loosen`, `drop`, `conflict`}; plus `suggested_change`
  + `amount` (LOOSEN only), `conflict` = `"conflicts in M of N rows"` (CONFLICT_SUMMARY only).
- `subject_kind='model'`: `achievable_objective` (the I3 number, or `'unbounded'`),
  `elastic_infeasible` (`'true'`, the loosening-cannot-fix-it verdict).

These strings are stable. Previously only DROP carried `edit_kind`; I5 added `loosen` and
`conflict` so the three edit kinds are uniform.

**Tests.** C++ (`test_decidb_diagnostic_engines.cpp`): the loosen section asserts the cue wording
(`"a possible edit was found"` + `"loosen one of your SUCH THAT limits"`, and that the old inline
`"Loosen x <= 5 to x <= 10"` is gone) and `edit_kind='loosen'`; the data-conflict section asserts
`edit_kind='conflict'` and the data-conflict cue (no "possible edit" claim). Python differential
(`test_query_diagnostics_relation.py`): a loosen case asserts `edit_kind='loosen'`; the data-RHS
conflict asserts `edit_kind='conflict'`.

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
