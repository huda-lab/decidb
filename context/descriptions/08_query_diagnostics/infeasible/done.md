# Query Diagnostics — Infeasible (how it works)

> Router terminal: **failed → infeasible** (`elastic` → report). See `router/README.md`.

The feasible region is empty — no assignment satisfies every `SUCH THAT` constraint at
once. Under `PRAGMA diagnose_decide='auto'`, the infeasible terminal now builds a second
optimization, the **elastic program**, whose optimum is the least-change fix for shipped
I1/I2 shapes: which user constraints to loosen, and by how much. Shared plumbing it builds
on (the pragma gate, provenance, the reporting relation) is in `foundations/done.md`.
The engine itself is shipped, including the T2 lexicographic repair ladder, T4
objective-best DROP-set re-optimization, and the stage-2b rank tie-break over **every**
repair kind — removals, editable loosens, and data offsets, with or without an objective —
so both backends name the same clause on a repair tie; deferred follow-ups are tracked in
`todo.md`.

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
it to the prepared-model solver on the primary solve's backend (`SelectSolverBackend` is
deterministic, so the re-solve uses the same backend), with the diagnostic helper budget
`min(60s, primary solve limit)` and the same interrupt poll as the primary solve. If any
elastic pass hits that helper budget, the operator reports that diagnosis ran out of time
instead of hiding the attempt behind the generic static error. This keeps the engine
solver-agnostic and unit-testable, the same boundary as the unbounded engine's
`get_candidates` injection. No clause→row index is carried in the input — the engine reads
each row's `provenance` directly off the model.

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
RHS stretch; `<>` clauses get binary removal switches. Stage 1 solves the repair in
lexicographic passes: first minimize removal cardinality, then data-RHS virtual offsets, then
editable source-literal loosening. A tier with no repair knobs is skipped (no solve) — the
common editable-only conflict runs a single pass. The final positive repair support names the
constraints to edit and the slack/switch values are the amounts.

```
stage 1R:  R* = min Σ wᵢ                         (remove-only <> switches)
stage 1D:  D* = min Σ s_data       subject to R ≤ R*
stage 1E:  E* = min Σ αᵢ s_edit    subject to R ≤ R*, D ≤ D*

           Aᵢ x ≤ bᵢ + sᵢ ,  sᵢ ≥ 0              (relaxable rows + bounds)
           structural / mechanism rows rigid unless covered by a removal switch
           →  final repair support names the edits; s*ᵢ / w*ᵢ are the amounts
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
  and skips recording a BOOLEAN bound **only when it merely restates the domain** (an upper
  `>= 1` or a lower `<= 0` after integer-strict normalization). A genuine BOOLEAN **pin**
  (`x <= 0`, `x >= 1`, `x = 1`) IS recorded and re-emitted like any other user bound — its
  column is opened only back to the intrinsic `[0,1]` (never to ±1e30), so the pin becomes
  loosenable while the 0/1 domain itself stays rigid. (Erasing pins wholesale made the
  elastic model diverge from the user's query: silently missing diagnoses, or edits that
  left the real query infeasible.) Reversed user bounds such as `5 >= x` are flipped by the
  constraint binder before this pass, so they follow the same absorption and re-emission
  path as `x <= 5`. `has_unhandled_user_bounds` is now computed for real by the re-emission
  loop: it turns `true` only if a recorded bound's column is missing from the retained model
  (expected never in practice), keeping the elastic-infeasible verdict honest.
- **Engine (`decide_diagnostic_engines.cpp`, `DiagnoseInfeasible`).** Pure model math.
  Copies the model, clears the user objective for the repair passes (zeroes linear objective,
  drops the quadratic objective, `maximize=false`), and adds a slack to every relaxable **linear**
  row (`IsRelaxableForElastic` ⇒ `USER_PARAMETER`). The active repair objective is installed
  per lexicographic tier. Quadratic rows and
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
'amount' | 'edit_source' | 'offset_scope'`, plus a one-line summary. The summary only points
to the relevant clause target (for example, "diagnosis points to clause `x <= 5`"); suggested
changes, amounts, edit sources, groups, and achievable-objective facts stay in
`decide_diagnostics()`. How a fanout clause (data RHS, PER aggregate) reads back is governed
by the slack-scope pragma — see "Slack-scope policy: query vs expanded" below.

**Elastic-infeasible signal.** If the elastic program is *itself* infeasible, the conflict
reaches rigid rows → `BuildElasticInfeasibleDiagnostic` renders a distinct outcome
(`subject_kind='model'`, `attribute='elastic_infeasible'`, summary: loosening your SUCH THAT
limits cannot fix it). This is claimed **only** when every user constraint was actually made
relaxable: the `has_unhandled_user_bounds` flag (set when the operator absorbed a user bound it
could not re-emit) suppresses the verdict and falls through to the static error rather than
wrongly declaring the query unfixable. As of I2.a multi-instance bounds *are* re-emitted, so the
flag stays `false` in practice and remains as a defensive guard. Likewise, when there are no
relaxable rows at all, the engine returns invalid (static error).

**Scope.** The achievable-objective re-solve (**I3**, "stage-2 achievable objective
(freeze-budget)" below), the L0/removal dial (**I4**, `<>` remove-only), lean
`decide_diagnostics()` reporting (**I5**), and all per-shape slack placement and unit
conversion (**I2.a–I2.e**, "per-shape slack placement" below) are shipped.

## Slack-scope policy: query vs expanded (T3)

The `diagnose_decide_infeasible_slack_scope` session pragma (VARCHAR, default `query`,
validated at SET time in `decide_diagnostic.cpp`; read into `DecideDiagParams::slack_scope`)
answers two different user questions with the same engine:

- **`query` (default)** — *"What single SQL rule do I change?"* Every knob that fans out
  (a PER clause across its groups, a data-backed RHS across its rows, a multi-instance cap)
  folds into **one** slack. The reported edit corresponds to the single literal the user
  would edit. A data RHS with no literal to loosen reports a **virtual offset**.
- **`expanded`** — *"Which generated rows/groups are actually tight?"* PER/aggregate groups
  get one slack each (`edit_source='expanded_group'`, `offset_scope='group'`); a data RHS
  stays per-row (`edit_source='expanded_row'`, `offset_scope='row'`), each conflicting row
  exposing its exact overshoot. A debug/profile view, not necessarily a pasteable SQL edit.

**Mechanism (one folding rule, two block keys).** The policy lives entirely in
`BuildElasticModel` + `ReadElasticEdits` (`decide_diagnostic_engines.cpp`); the operator and
the rest of the engine are unchanged. `BuildElasticModel(base, removal_bigm, slack_scope)`:

- **`folds()`** — a `SHARED_LITERAL` knob always folds; a `PER_ROW_DATA` row folds **only in
  query mode** (in expanded mode its rows stay independent, one slack each).
- **`block_key()`** — query mode keys a block by `clause_id` alone (so all PER groups of a
  clause collapse into one slack = the single literal edit); expanded mode keys by
  `(clause_id, group_key)` (so each PER group is its own slack). A folded data block still
  belongs to the data-offset lexicographic tier in both modes — only the *grouping* changes by
  mode, never the tier.

**Readback (`ReadElasticEdits`, threaded with `slack_scope`).** Per block:

- **query** — one clause-level edit with the group identity dropped (`offset_scope='clause'`,
  no `group` row). A data RHS → a `virtual_offset` LOOSEN (`MakeVirtualOffsetEdit`:
  `x <= col + delta` / `>=` → `col - delta`, where `col` is `ConstraintProvenance::rhs_label`,
  the RHS column name carried from the binder — fall back to the numeric representative RHS
  when absent). Any other literal knob → `source_literal`.
- **expanded** — a data RHS → `expanded_row` per conflicting row; a PER-grouped literal →
  `expanded_group` per group (keeping the group key); an ungrouped literal → `source_literal`.

**Reporting contract (frozen EAV vocabulary, additive to I5).** Every LOOSEN edit carries
`edit_source ∈ {source_literal, virtual_offset, expanded_row, expanded_group}` and
`offset_scope ∈ {clause, row, group}` (emitted by `BuildInfeasibleDiagnostic` when set). The
old `CONFLICT_SUMMARY` edit kind (the data-RHS `"conflicts in M of N rows"` dead-end) is
**removed** — a data conflict is now an actionable `virtual_offset` (query) or an
`expanded_row` profile (expanded). The stderr headline stays a lean clause pointer (**T5**):
in query mode it points to the folded clause (`diagnosis points to clause \`SUM(x) >= 5 PER
grp\``), never a per-group list; the per-group breakdown and all amounts/sources live in
`decide_diagnostics()`.

**easy-MAX + PER is one global cap, correctly.** `MAX(x) <= K PER g` is mathematically
`x <= K` for **every** row, so the optimizer's easy-MIN/MAX rewrite strips the (vacuous) PER
wrapper (`decide_optimizer.cpp`, `RewriteMinMaxInConstraint`) and the cap is absorbed as one
uniform column bound. There is genuinely no per-group *cap* to break out — the diagnosis
reports the single global edit (`x <= K` → `x <= K + max overshoot`) in **both** modes, which
is exactly right. (This resolves the former "easy-MAX+PER collapses" note: it was not a bug,
just the uniform cap surfacing. Preserving a per-group *overshoot profile* would require
carrying the stripped PER key as a diagnosis-only side channel through the optimizer — not
done, since it risks the empty-group/WHEN semantics the strip protects.)

**Tests.** C++ structural (`test_decidb_diagnostic_engines.cpp`): `BuildElasticModel` folds a
PER `SHARED_LITERAL` clause into one block in query mode and one-per-group in expanded; a data
RHS folds to one block in query and stays independent per-row in expanded. Python differential
(`test_query_diagnostics_relation.py`, both backends): data-RHS query virtual offset (named
column) + expanded per-row profile; PER-aggregate query fold (one clause edit) + expanded
per-group; option validation for the pragma. TPC-H (`test_query_diagnostics_tpch.py`): the
`SUM(x) >= 5 PER l_orderkey` case folds in query and breaks out per failing order in expanded;
`x >= l_quantity` reports the named virtual offset in query and per-row in expanded.

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

**Shape marker (`ConstraintProvenance::shape`, `decide.hpp::ElasticShape`).** `UNSET` is the
invalid default; `BuildElasticModel` asserts that every relaxable row with a user `clause_id`
has been stamped explicitly. `SHARED_LITERAL` = the RHS is one editable literal `K` shared
across every row the clause emits; `PER_ROW_DATA` = genuine per-row **data** RHS (`x <= col`).
Set in `ilp_model_builder.cpp`: the per-row builder tags a row `SHARED_LITERAL` when
`EvaluatedConstraint::rhs_is_shared_literal` (set in `physical_decide.cpp` when `rhs_expr` is a
`BOUND_CONSTANT`, is foldable, or carries the `SHARED_SCALAR_SUBQUERY_TAG` alias — see
"uncorrelated scalar-subquery RHS" below), else `PER_ROW_DATA`; the **aggregate** sites tag
their rows `SHARED_LITERAL` directly (an aggregate's RHS is always a single scalar knob), so
`PER_ROW_DATA` means *exactly* "data RHS"; the absorbed-bound re-emission tags its rows
`SHARED_LITERAL`. Quadratic aggregate rows are stamped `SHARED_LITERAL`; quadratic per-row rows
are stamped from the same shared-literal-vs-data RHS rule before I2.d slacks the linear RHS.

**Grouping (mode-aware — see "Slack-scope policy" above).** `BuildElasticModel` groups
relaxable rows into blocks via `folds()` + `block_key()`. A `SHARED_LITERAL` block gets **one
shared slack** wired into all N rows (`=` blocks get one shared `s⁺` and one shared `s⁻`); with
`eᵢ − s ≤ K` on every row the single `s` is driven to the **max overshoot**, so the reported
edit is the max (not the per-row sum) and the "which clause to relax" race is not skewed ≈N×.
This covers **easy MIN/MAX** (`MAX(e) ≤ K` → per-row `eᵢ ≤ K`, absorbed as a shared cap),
**per-row literal** constraints, and **multi-instance bounds** (`x ≤ K` over N instances).
For **PER**, the block key depends on the mode: **query** keys by `clause_id` alone, so all
groups of `SUM(x) ≥ K PER g` fold into one slack = the single literal edit; **expanded** keys
by `(clause_id, group_key)`, so each group is its own slack (per-group profile). **easy-MAX +
PER** re-emits as one global cap in both modes — correctly, because the easy rewrite makes the
cap uniform `x ≤ K` across groups (the optimizer strips the vacuous PER; see "Slack-scope
policy").

**Edit conversion (the D3 helper).** Reading the slack support is centralized in
`MakeLoosenEdit(provenance, lhs, rhs, sense, amount)` (a literal knob) and
`MakeVirtualOffsetEdit(provenance, lhs, rhs_text, sense, delta)` (a query-mode data offset,
`x <= col + delta`) in `decide_diagnostic_engines.cpp` — the one place per-shape rendering
lives: they render the sense, re-quote a strict `<`/`>` against the typed literal, and format
the suggestion. The LHS is rendered shape-aware by `FormatLhs` — plain `FormatTerms`,
AVG-collapsing (`FormatAvgLhs`), or SUM-collapsing (`FormatSumLhs`) — or `FormatQuadraticLhs`
for a quadratic constraint. A `ClauseEdit` carries a `ClauseEditKind` (`LOOSEN` vs `DROP`,
`decide_diagnostic.hpp`) plus `edit_source` / `offset_scope` provenance.

### I2.c — data-RHS editable-knob preference

A `PER_ROW_DATA` clause (`x <= col`) has a per-row data RHS with no single literal to
"loosen to K". Under the T3 slack-scope policy it reports a **virtual offset** in query mode
(`x <= col + delta`) or a **per-row profile** in expanded mode (see "Slack-scope policy"), and
the data offset lives in a separate lexicographic tier, so an editable literal is always
preferred when it can restore feasibility. Two parts:

- **Reporting.** `ReadElasticEdits` reads a data-RHS block per the slack-scope mode: query mode
  emits **one `virtual_offset` LOOSEN** for the folded clause (`x <= col + delta`), expanded mode
  emits **one `expanded_row` LOOSEN per conflicting row**. Either way it names the clause, not
  per-row spam. (This replaces the former `CONFLICT_SUMMARY` "conflicts in M of N rows" dead-end,
  which is gone — a data conflict is now actionable.)
- **Editable-knob preference.** In a degenerate infeasibility an editable knob and a data row can
  be equally-optimal fixes, and the solver would split arbitrarily (a messy, non-deterministic
  mix). To prefer an *actionable* edit, stage 1 solves data offsets before editable loosening:
  it first freezes the minimum removal count, then freezes the minimum data-offset total, then
  minimizes editable loosening. Data conflicts surface only when editable loosening cannot
  restore feasibility with data offset total `D = 0`.
- **Scale-normalized editable weights (T1).** Within the editable tier the stage-1 objective sums
  slacks across constraints in *incomparable units* — e.g. `SUM(buy) >= 30` (count) vs
  `SUM(buy*l_extendedprice) <= 100` (dollars). With a uniform weight of 1 the raw sum makes the
  small-magnitude row look cheapest, so the solver gutted the count floor to `>= 0` (select
  nothing, `achievable_objective = 0`) instead of loosening the genuinely-tight budget — a
  degenerate "require nothing" edit. Each **editable** slack is now weighted `ref / rms(Aᵢ)`
  (`BuildElasticModel`, `decide_diagnostic_engines.cpp`), where `rms(Aᵢ) = √(Σcⱼ²/nnz)` is the
  row's **root-mean-square coefficient** — its *typical* magnitude. Dividing by it makes the
  objective track how far the constraint boundary moves *per unit of decision*, so the
  large-coefficient budget (barely off in normalized terms) is the cheaper edit. RMS (not the plain
  L2 norm `‖Aᵢ‖₂`) is deliberate: L2 grows with the number of terms, so it would make a
  many-variable aggregate floor (`x + y >= 10`, √2) numerically cheaper to loosen than a
  single-variable cap (`x <= 0`, 1) purely for having more terms — reintroducing the degenerate
  "gut the floor" preference. RMS is invariant to term count (both those rows have RMS 1), so it
  removes that bias while still capturing the data-magnitude mismatch E needs. `ref` = the smallest
  editable RMS; being a common factor it never changes *which* row wins (only conditioning), keeps
  every editable weight in `(0, 1]`, and leaves the usual all-equal-coefficient model at weight 1.
  This is a **within-editable-tier** normalization only: data offsets and removals are separate
  lexicographic tiers, while editable slack refs carry the `ref / rms(Aᵢ)` coefficient used by the
  editable pass and the stage-2 `E*` budget. Why not RHS-norm (`1/|b|`): the budget needs a *large absolute*
  loosening (100 → thousands), so dividing by the small RHS leaves it expensive and does not fix the
  degeneracy; coefficient-magnitude does. Structural weight assertion in
  `test_decidb_diagnostic_engines.cpp`; end-to-end flip on real TPC-H data in
  `test_query_diagnostics_tpch.py::test_E_loosen_should_not_be_degenerate` (both backends).
- **Boolean bound-absorption revert (bundled with T1).** T1's weights are only *reachable* once the
  diagnosis model can actually exercise the loosened constraint. `DecidePropagateImpliedBounds`
  tightens a variable's column box by absorbing a user row — e.g. `SUM(buy * price) <= 100` implies
  `buyᵢ ≤ 100/priceᵢ`. For a BOOLEAN (lowered to INTEGER with a `[0,1]` box) a fractional upper
  (`≤ 0.1`) silently pins the variable to **0**, so the (now-relaxable) budget can never be
  exercised and the only "fix" is gutting the *other* clause — the same degenerate symptom, from a
  different cause. The diagnosis bound-reset loop (`physical_decide.cpp`) already reverts absorbed
  tightenings for non-boolean columns; it now also reverts them for booleans — resetting the box to
  `[0,1]` (upper → 1 if `< 1`, lower → 0 if `> 0`) instead of skipping them — so the slackable row
  is the sole enforcer. Never opens past 1 (that would unbound the variable). With this, E loosens
  the budget and reaches a nonzero objective; without it, T1's normalized weights point at the right
  clause but the model still can't buy anything.
- **Data-weighted SUM renders symbolically (`SUM(buy * l_extendedprice)`).** A data-VARYING summed
  term has no single literal coefficient to quote, so it used to fall back to the raw per-row
  numeric fan-out (`24710*buy + 56688*buy + …`). The coefficient's source-column name is now carried
  from the binder (`Term::coefficient->GetName()` at evaluation in `physical_decide.cpp`) through
  `EvaluatedConstraint::coefficient_labels` → `ConstraintProvenance::weight_labels`
  (`ilp_model_builder.cpp`, stamped at the aggregate emission sites) → `FormatSumLhs`, which renders
  `SUM(var * label)` when a fanned variable is data-varying and a label is present (else it still
  falls back to the raw form). Uniform-coefficient sums are unaffected (they still fold to
  `SUM(c*var)`).

### I2.d — AVG / strict / quadratic in the user's units

- **AVG.** The AVG→SUM rewrite pre-scales row coefficients by `1/N_g`
  (`ScaleAvgRowCoefficients`), so the slack is **already in AVG units** — reported raw, no
  `/N_g`. A pure-AVG row is tagged `provenance.avg_scaled` (set in `physical_decide.cpp` when
  every linear term is AVG-scaled, propagated at the aggregate provenance sites in
  `ilp_model_builder.cpp`); `FormatLhs` then collapses the `1/N`-scaled terms back to `AVG(x)`
  (inner coeff = stored coeff × the variable's term count) instead of rendering `0.5*x + 0.5*x`.
- **SUM.** An aggregate `SUM` over rows emits one solver column per row for the same decide
  variable, so a plain reconstruction reads `x + x + x`. `FormatSumLhs` collapses the per-row
  fan-out back to `SUM(c*var)` for a uniform coefficient, and to `SUM(var * col)` for a
  **data-varying** coefficient (`SUM(buy * l_extendedprice)`) using the symbolic column name carried
  in `provenance.weight_labels` (see the "data-weighted SUM renders symbolically" note above); it
  only falls back to the raw numeric reconstruction when a data-varying term has *no* label.
  **PER-grouped aggregates fold too** (the old `group_key == INVALID` gate is gone): `SUM(x) >= 5
  PER grp` renders both groups as `SUM(x) >= 5 PER grp`, kept distinguishable by the `group` EAV row
  + `PER grp` qualifier (see "PER-group identity" below). A **single-row / WHEN aggregate group** has
  no fan-out for `FormatSumLhs` to fold, so `FormatLhs` wraps the reconstruction in `SUM(...)`
  explicitly when `provenance.is_aggregate` and there is no multi-column fan (`HasVarFan`) —
  `SUM(x) >= 99 WHEN g='a'` renders `SUM(x) >= 99 …`, not a bare `x >= 99`.
- **Strict `<` / `>`.** The δ offset (`< K → <= ceil(K)-1`, `> K → >= floor(K)+1`) is baked into
  `rhs` at the δ site (`ApplyComparisonSense`, `ilp_model_builder.cpp`), which now also sets
  `provenance.strict` + `provenance.typed_k` (the user's literal). `MakeLoosenEdit` re-quotes the
  suggestion against `typed_k` and renders `<` / `>`, e.g. `x < 10` → `x < 16`.
- **Quadratic.** `BuildElasticModel` loops `model.quadratic_constraints` and slacks the **linear
  RHS only** (never the Q matrix): `e(x) + xᵀQx ≤ K` loosens to `… ≤ K + s`. The `BlockSlackRef`
  carries a `quadratic` flag (its `rows` then index `quadratic_constraints`); `FormatQuadraticLhs`
  renders `POWER(x, 2)` / `x*y` terms. Quadratic slacks are always editable-weighted (a quadratic
  is always a LOOSEN edit, never a data conflict). QCQP is Gurobi-only.

### PER-group identity (group label + qualifier; unblocks the SUM fold for PER)

A PER-grouped aggregate (`SUM(x) >= 5 PER grp`) emits one row per group, so each group can get
its own edit. Each group's edit is now **self-identifying**, which is what lets the SUM fold
above apply to PER without the two groups colliding in the relation. Three additive channels,
all carried on `ConstraintProvenance` (no change to PER solve logic):

- **`group_label`** — the group's printable key (`'a'`, or `EU, 2024` for a composite key).
  `LookupOrBuildPerGroupIds` (`physical_decide.cpp`) now requests `BuildGroupIds`'s `rep_keys`
  out-param and stores the per-group representative values on the cache entry; the post-WHEN
  remap to consecutive `0..K'` reindexes the labels in lockstep (a new `mapped` id is pushed at
  exactly `out_group_labels.size()`, so labels stay aligned with the dense ordinals).
  `FormatPerGroupKey` joins composite columns with `, `; NULL renders `"NULL"` and an actual
  empty-string key renders `"''"` so it is not confused with "ungrouped." Threaded through
  `EvaluatedConstraint::group_labels` → stamped on `provenance.group_label` at the aggregate-PER
  emission sites (`ilp_model_builder.cpp`). The diagnosis uses it twice: every EAV row for the
  edit gets a self-identifying subject (`SUM(x) >= 5 PER grp [group: a]`), and the structured
  `group` row (`attribute='group'`, `value='a'`) is still emitted. This avoids relying on relation
  row order to associate `suggested_change`/`amount` with a group.
- **`is_aggregate`** — set from `eval_const.lhs_is_aggregate` at the aggregate emission sites;
  drives the single-row / WHEN `SUM(...)` wrapper in `FormatLhs` (see I2.d SUM above).
- **`qualifier`** — the `PER grp` / `PER (region, year)` / `WHEN <pred>` text, computed once per clause at the
  WHEN/PER eval site (`physical_decide.cpp`) and appended to the reconstructed label by
  `MakeLoosenEdit`. PER keys reuse the expression `GetName()` the EXPLAIN path uses; a WHEN
  predicate is rendered by `RenderWhenPredicate`, which unwraps the binder's implicit literal
  CASTs and drops redundant outer parens (`grp = 'a'`, not `(grp = CAST('a' AS VARCHAR))`) and
  handles comparisons + AND/OR.

The objective PER path passes a throwaway labels vector (objective groups are not diagnosed by
clause key). **Composite PER keys** (`PER (region, year)`) are handled by the join and reachable
through `SUCH THAT` in the parenthesized form. The **unparenthesized** comma-list form
(`PER region, year`) is rejected by the parser; use `PER (region, year)`. **easy-MAX + PER** correctly
re-emits as one global cap: the easy rewrite makes `MAX(x) <= K PER g` the uniform `x <= K`
(the optimizer strips the vacuous PER), so there is no per-group cap to break out — see
"Slack-scope policy: query vs expanded" above.

### I2.e — rigid shapes confirmed

`<>` indicator rows (`USER_MECHANISM`) and McCormick bilinear links (`STRUCTURAL`) are not
relaxable (`IsRelaxableForElastic`), so the elastic transform attaches no slack to them: a `<>`
or bilinear conflict is resolved by loosening an editable bound, and a conflict reachable only
through those rigid rows falls through to the static error (the empty-block guard). `<>` is
remove-only; actual removal is the **I4** L0 dial — I2 only confirms the rigid behavior.

**Tests.** C++ structural (`test_decidb_diagnostic_engines.cpp`): one shared slack spans all N
rows with the correct sign; a PER `SHARED_LITERAL` clause folds to one block in query and
one-per-group in expanded; a data RHS folds to one block in query and stays independent per-row
in expanded; `USER_MECHANISM`/`STRUCTURAL` rows are never slackened; a data-offset tier
defers to an editable knob; a query-mode data RHS folds to one virtual offset; strict re-quotes
against `typed_k`; an AVG row renders `AVG(x)` with the raw slack; a quadratic constraint slacks
its linear part only with Q untouched. C++ behavioral: a shared block with rigid floors reports
the max overshoot (7), one edit, not the sum (10). Python differential
(`test_query_diagnostics_relation.py`): easy-MAX and multi-instance bound report total = max
overshoot; aggregate `SUM(x) >= 5 PER grp` folds to one `SUM(x) >= 5 PER grp` edit in query mode
and breaks out per group (`[group: a]` / `[group: b]` with `group` rows) in expanded mode;
a single-row `SUM(x) >= 99 WHEN grp='a'` keeps its `SUM(...)` wrapper and clean WHEN qualifier; a
BOOLEAN model loosens its SUM target, never its 0/1 domain (regression guard); a data-RHS clause
reports a named virtual offset (query) / per-row profile (expanded); a data-offset row defers
to an editable edit; reversed bounds are
reported as their canonical absorbed form; AVG/strict re-quote (single-row SUM keeps its wrapper:
`SUM(x) > 10`, including strict quadratic); quadratic and McCormick gated to Gurobi; `<>` and
McCormick rows stay rigid.

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

**Label channel for global-block columns (aggregate `<>`, composed MIN/MAX).** A per-row `<>`
indicator is a decide-var column, so `BuildColumnProvenance` already names it from `var_labels`.
An aggregate `<>` binary lives in the global block, which `BuildColumnProvenance` otherwise
leaves unnamed — so a dropped aggregate `<>` would have an empty subject.
`SolverInput::global_variable_labels` (parallel to `global_variable_types`) carries the user
source text of a labeled global: the clause text `SUM(x) <> K` for an aggregate-`<>` binary
(looked up from `aux_var_expressions` at that site), and the aggregate text `MAX(x)` for a
composed-MIN/MAX z (both the constraint and objective allocation sites in
`physical_decide.cpp` build it from the term's `agg_name` + `inner_expr->ToString()`, padding
the label vector to the z's ordinal first since earlier allocation sites may be unlabeled) —
so the composed outer pin renders `SUM(x) + MAX(x) <= -1`, never an internal `colN`. Other
global aux (single-term MIN/MAX objective z, McCormick) stay unlabeled. The vector is
reconciled to the final global-var count with one `resize(num_global_vars)` before
`SolveModel`. `BuildColumnProvenance` takes it as an optional argument and writes the labels
onto the global-block columns; the infeasible engine forwards it through
`InfeasibleDiagnosisInput::global_variable_labels`. The DROP edit then reads
`columns[indicator_col].label` uniformly for both shapes.

**Mechanic (all-or-nothing binary, no separate gate row).** Because `<>` removal is binary,
`BuildElasticModel` (`decide_diagnostic_engines.cpp`), after the loosening + quadratic
passes, groups relaxable rows by `indicator_col` and adds **one binary `w` per `<>`**, wired
into each disjunction row with a `±M₂` coefficient (sign by sense, reusing the slack
convention: `<` → `−M₂`, `>` → `+M₂`). `w = 1` makes both rows vacuous — the clause is
dropped. **M₂** defaults to the clause's own disjunction Big-M (`|row coeff on indicator_col|`,
provably enough to neutralize whichever side `z` selects), overridable by the
`diagnose_decide_removal_bigm` pragma. The wiring is recorded in a `RemovalRef {rows, w_col,
indicator_col}` on `ElasticModel`.

**Removal tier (B1/T2).** `w` has no baked-in objective coefficient in the elastic model. Stage
1 first minimizes `R = Σ wᵢ`, so the support `{i : wᵢ = 1}` is a minimum-cardinality removal
set before the engine considers data offsets or editable slack. The next passes freeze
`R = R*`, minimize data offsets `D`, freeze `D = D*`, and finally minimize editable loosening
`E`. So dropping a `<>` is a last resort without a fixed Big-M-style objective weight.

**Reading + reporting.** `ReadElasticEdits` reads each `RemovalRef`: `w > 0.5` emits a
`ClauseEdit{kind = DROP, label = columns[indicator_col].label}` (the F6 `x <> 3` string).
The F6 label is built by `DiagnosisComparand` (`decide_optimizer.cpp`), which unwraps the
implicit `CAST` the binder inserts around a literal and drops the outer parens, so the clause
reads `x <> 1`, not `(x <> CAST(1 AS INTEGER))`. `BuildInfeasibleDiagnostic` renders a DROP as
a dedicated EAV row `subject_kind='clause'`, `subject='x <> 3'`, `attribute='edit_kind'`,
`value='drop'` (distinct from the LOOSEN `suggested_change`/`amount` pair), and points to the
dropped clause in the summary. A single written `<>` that expands per row has one indicator
(hence one `RemovalRef`) per row, all sharing the same clause label; `ReadElasticEdits`
**dedupes DROP edits by label** (a `std::set<string>` of already-emitted labels) so the
relation carries one DROP per user clause, not one per row. The
`DiagnoseInfeasible` empty-guard now bails only when **both** slacks and removals are empty,
so a removal-only model is still diagnosed.

**Stage-2 composition (I3).** Stage 2 runs when there is an objective **and** an actionable
fix (`HasLoosenEdit || HasRemoval`). `BuildStage2Model` freezes the solved tier budgets
`R ≤ R*`, `D ≤ D*`, and `E ≤ E*`, but does **not** pin individual repair variables. For
remove-only `<>` clauses, stage 2 (2a) can therefore re-optimize the DROP set under the
minimum-cardinality removal budget and report the objective-best equally minimal DROP set.

**Stage-2b rank tie-break (solver-agnostic determinism, all repair kinds).** When the repair
choice is *indifferent* between two equally-minimal repairs — two DROP sets, or an LP that can
put the same loosening on either of two clauses (or split it fractionally across both where
one edit suffices) — the solver picks arbitrarily, and Gurobi and HiGHS can name **different**
clauses. `BuildTieBreakModel` removes that ambiguity with one extra pass over every repair
knob: when a stage-2a objective exists, freeze it at its optimum with a one-sided row (`obj ≥
Z*` for MAXIMIZE / `≤ Z*` for MINIMIZE — the rhs is **exact**, no tolerance cushion: any eps of
objective room is monetized by the rank objective, which would shave eps of repair onto a
lower-ranked slack and split the edit; the stage-2a point attains `Z*` to machine precision,
and a rounding pathology just makes the pass non-optimal, keeping the stage-2a repair). Then
minimize the rank-weighted repair sum: each removal `w` gets its 1-based rank, each slack gets
its tier coefficient (editable slacks: the T1 scale weight) times its 1-based rank, ranks
ascending in emission order (`removals` by indicator column; `slacks` by row — matrix rows in
declaration order, then re-emitted absorbed bounds, whose original declaration position is not
recorded). The tier budget rows carried into the model pin each tier's total, so the pass only
reselects *which* clauses carry the repair — concentrating it on the lowest-ranked clause is
the unique optimum, and both backends report the same edit. On the **no-objective path** (no
user objective, or a data-only repair that skips stage 2) the same pass runs directly on the
budget-frozen stage-1 model with no freeze row; its solution is re-read with `snap=false` like
the stage-1 read. The stage-2a achievable objective `Z*` is what gets reported (the tie-break's
own objective is just the ranking sum). The pass is skipped — keeping the prior result — when
no tier owns `≥ 2` knobs (budgets pin cross-tier trades, so no naming tie is possible) or, on
the freeze path, when the objective is quadratic (freezing it would need a quadratic row; an
exact tie there is rarer still). Residual: two *different* min-cardinality sets with an equal
rank-sum (only possible at cardinality `≥ 2`) can still tie, since linear ranks are not a total
order over sets; the common single-drop and single-loosen ties are fully deterministic.

**Pragma.** `diagnose_decide_removal_bigm` (DOUBLE, default `0` = auto-derive, `>= 0`,
`decide_diagnostic.cpp`) threads through `DecideDiagParams::removal_bigm` into
`BuildElasticModel`.

**Tests.** C++ structural (`test_decidb_diagnostic_engines.cpp`): a `<>` pair gets one binary
`w` wired `−M₂`/`+M₂` with zero baked-in objective coefficient and a `RemovalRef`; the pragma
override replaces M₂; a pinned-`<>` must-drop reports `edit_kind='drop'`; a loosenable
conflict prefers the LOOSEN and never drops; an **aggregate `<>`** whose disjunction binary is
a global-block column is named via the `global_variable_labels` channel (not `var_labels`) and
reported as a drop. Python differential (`test_query_diagnostics_relation.py`, both backends):
`x <> 0 AND x <> 1` on a BOOLEAN drops exactly one `<>` (min cardinality) and chooses the
objective-best drop (`x <> 0` for `MINIMIZE SUM(x)`, `x <> 1` for `MAXIMIZE SUM(x)`), with the
reported objective differential-checked against an independent re-solve of the fixed query;
`SUM(x) <> 0 AND SUM(x) <> 1` (aggregate) does the same while keeping the dropped aggregate
`<>` named; **two independent BOOLEANs** each with `<> 0 AND <> 1` drop the objective-best `<>`
per variable (a minimum-cardinality-2 set, differential-checked); an **objective-indifferent
tie** (`x <> 0 AND x <> 1 AND y ≤ 5 MAXIMIZE SUM(y)`, where the objective only touches the free
`y`) drops the earliest-declared `x <> 0` on **both** backends — the solver-agnostic tie-break
guarantee; `x <> 5 AND 5 ≤ x ≤ 5` prefers loosening; the pragma override produces the same drop
and a negative value is rejected at SET time.

## Elastic engine: stage-2 achievable objective (freeze-budget)

I3 reports the **achievable objective** the user gets after the minimal fix — and the
specific edit that achieves it. Stage 1 (above) finds the lexicographically least repair
budgets `(R*, D*, E*)`, but when that minimum is non-unique it returns an **arbitrary**
minimizer whose edit need not be the one best for the user's objective. Stage 2 is a final
lexicographic tier: among all fixes within those repair budgets, pick the one that maximizes
the user's original objective, and report **that** edit so the edit and the objective agree.

**Method (freeze the budget, not the amounts).** `BuildStage2Model`
(`decide_diagnostic_engines.cpp`) starts from the stage-1 elastic model (slacks already
wired) and:
- adds rigid **budget rows** for every active stage-1 tier: `R ≤ R*` over removal columns,
  `D ≤ D*` over data-offset slack columns, and `E ≤ E*` over editable slack columns using the
  same `ref / rms(Aᵢ)` coefficients as the editable pass. Equality blocks count both
  directional slacks (`s⁺ + s⁻`). The caps are exact stage-1 tier optima; the backend
  feasibility tolerance supplies the only cushion that keeps the re-solve feasible.
- **restores the user's original objective** (linear `obj_coeffs`, the quadratic objective
  `q_*` / `has_quadratic_obj` / `nonconvex_quadratic`, and the `maximize` sense), with the
  slack columns set to zero objective weight so the reported objective is exactly `cᵀx` over
  the user's variables. Stage 1 had zeroed and dropped the objective; stage 2 needs it back.

`DiagnoseInfeasible` runs stage 2 only when there is a **real objective** (`HasObjective`)
**and** a `LOOSEN` or `DROP` edit (`HasLoosenEdit || HasRemoval`): a constant objective has
nothing to report, so it falls back to the stage-1 edit list unchanged. A data-RHS clause is
now an actionable `LOOSEN` (a query-mode virtual offset or an expanded per-row edit), so it
*does* exercise stage 2 and reports an achievable objective — the former data-only carve-out
(when data reported a non-actionable conflict) is gone.

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
`deterministic_naive.cpp` via HiGHS `getInfo().objective_function_value`). The engine still
reads the stage-2 achievable objective directly from this field; stage-1 tier budgets are
computed from the solved repair variables so the same coefficient logic feeds readback and
the `R*`/`D*`/`E*` freezes.

**Reporting.** `BuildInfeasibleDiagnostic` has two optional arguments
(`achievable_objective`, `unbounded_after_fix`). When set it emits one EAV row
`subject_kind='model'`, `attribute='achievable_objective'`, `value=<number>`. The repair
budgets themselves are **not** surfaced (decision 4), and the stderr headline remains a
clause pointer rather than an objective report.

**Stage-2 unbounded (decision 2).** If the relaxed region is unbounded in the objective
direction (`UNBOUNDED` / `INF_OR_UNBD`), there is no finite optimum: the engine keeps the
stage-1 edit (still valid) and emits model row value `'unbounded'`. It does **not** hand off
to the unbounded engine — the edit is the actionable part, and composing two diagnoses would
muddy the message.

**Tests.** C++ (`test_decidb_diagnostic_engines.cpp`): a non-unique-minimizer 2-var case
(caps `x ≤ 0`, `y ≤ 0` feeding rigid `x + y ≥ 10`, `MAXIMIZE x`) reports the x-cap edit and
objective `10` — loosening `y` would give `0`; a stage-2-unbounded case (`MAXIMIZE y` with
`y` free) reports the edit plus `achievable_objective = 'unbounded'`. Python differential
(`test_query_diagnostics_relation.py`): the same shape end-to-end on both backends, with the
reported objective checked against an independent re-solve of the fixed query (the edit
applied), never a hand-computed value. Existing infeasible tests that carry an objective now
exercise stage 2 transparently — their reported edits are unchanged. The **no-objective
loosen tie** (`x <= 0 AND y <= 0 AND x + y >= 10` with no MAXIMIZE — Gurobi and HiGHS used to
name different clauses) reports the same single floor edit on both backends
(`test_infeasible_no_objective_loosen_tie_is_solver_agnostic`), the stage-2b guarantee for
loosen repairs.

## Elastic engine: column-bound conflicts (intrinsic reset, inverted box, type-domain errors)

A user limit can live in a variable's **column bounds** instead of a matrix row, where the
row-only elastic engine can't relax it. The built `SolverModel`'s `col_lower`/`col_upper` is a
*fused* product — intrinsic domain (REAL/INT `[0,+∞)`, BOOLEAN `[0,1]`) + absorbed user bounds +
implied tightenings from `DecidePropagateImpliedBounds` (a presolve pass with no provenance).
Three mechanisms make every column-derived user limit diagnosable while keeping the intrinsic
domain rigid:

- **Intrinsic reset (the elastic box carries only the intrinsic domain).** In the
  `DiagnosisTerminal::INFEASIBLE` arm, after re-emitting absorbed bounds as slackable rows (the
  decision-1a loop above), a second pass resets every decide column's *implied* tightenings back
  to intrinsic — non-BOOLEAN: `col_lower > 0 → 0`, `col_upper < +∞ → +∞`; BOOLEAN: back to
  `[0,1]` (`col_lower > 0 → 0`, `col_upper < 1 → 1`), never past 1. This is safe —
  propagation only ever tightens (raises lower / lowers upper), and every implied tightening has
  a backing row (`USER_PARAMETER` slackable, or `STRUCTURAL` still-rigid) that keeps enforcing it.
  This is what lets `x <= 2+3` (a foldable cap copied into `col_upper`) be loosened by its row
  instead of pinned by the box. **BOOLEAN columns are detected via `is_boolean_var[var]`, not
  `is_binary[col]`** — a BOOLEAN is lowered to an INTEGER carrying a `[0,1]` box, so `is_binary`
  is `false` for it; gating on `is_binary` would reset its upper to `+∞` and silently make it
  unbounded. The decision-1a re-emission loop uses the same signal: an absorbed BOOLEAN pin's
  column opens only to `[0,1]` for the pinned direction (a non-BOOLEAN opens to ±1e30).
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

### Uncorrelated scalar-subquery RHS (`x <= (SELECT 5)`)

A per-row bound whose RHS is an **uncorrelated** scalar subquery is one shared editable cap, but
`PlanSubqueries` flattens it into a cross-joined column ref that is structurally indistinguishable
from genuine per-row data — `IsFoldable()` is false, so naive classification would mis-tag it
`PER_ROW_DATA` and report a data conflict instead of an editable cap. The correlation information
exists only *before* flattening, so `plan_select_node.cpp` (right before its `PlanSubqueries` call,
at the single DECIDE-owned flatten site) scans the bound constraint tree for comparisons whose RHS
is a scalar `BoundSubqueryExpression` with no correlated columns (`!BoundSubqueryExpression::IsCorrelated()`,
unwrapping casts), holds raw pointers to those comparison nodes (which survive the in-place rewrite),
and after flattening stamps the rewritten RHS column-ref alias with `SHARED_SCALAR_SUBQUERY_TAG`
(`decide.hpp`). The tag rides through the optimizer and into the `DecideConstraint` (alias is
preserved by `Copy()`); `physical_decide.cpp` ORs `IsSharedScalarSubqueryTag(rhs_expr->GetAlias())`
into `rhs_is_shared_literal`, so the elastic engine collapses the rows to one shared slack and
reports `Loosen x <= 5 to x <= 10`. **Correlated** subqueries are left untagged and stay
`PER_ROW_DATA` (genuinely row-varying). Tests: `test_query_diagnostics_relation.py::
test_infeasible_uncorrelated_subquery_cap_is_editable` / `…_correlated_subquery_cap_stays_per_row`.

## Infeasibility reporting: lean cue summary + frozen vocabulary (I5)

I5 is the final reporting step. The structured EAV edit list was already emitted by I1–I4; I5
makes two refinements and **locks the vocabulary**.

**Lean cue summary.** `BuildInfeasibleDiagnostic` (`decide_diagnostic.cpp`) keeps the thrown
stderr headline short: it points to the relevant clause target(s) and leaves the actual edits to
`decide_diagnostics()`. This avoids implying that one listed clause is independently sufficient
when the engine found a combined repair set.
- Single target → `"…; diagnosis points to clause \`x <= 5\`."` (a data RHS folds to its
  clause too, e.g. `"…; diagnosis points to clause \`x >= hi\`."` in query mode)
- Multiple targets → `"…; diagnosis points to clause \`SUM(x) >= 1\` and clause \`SUM(5*x) <= -1\`."`
- PER groups (**expanded mode only** — query mode folds a PER clause to one clause pointer with
  no group list) → `"…; diagnosis points to grouped clause \`SUM(x) >= 5 PER grp\` for groups
  \`a\` and \`b\`."` The group list is **capped** (`QuotedGroupList`, `HEADLINE_GROUP_CAP = 3`):
  with more than three failing groups the headline shows the first three then `… and N more (see
  decide_diagnostics())`. The relation still carries one row per failing group.

Every thrown message still appends `Details: SELECT * FROM decide_diagnostics();`. The table is
the source of truth for `edit_kind`, `suggested_change`, `amount`, `edit_source`, `offset_scope`,
`group`, and `achievable_objective`. `BuildElasticInfeasibleDiagnostic` (the "loosening cannot
fix it" path) is untouched — the cue does not apply when no edit was found.

**Why no runnable rewritten query.** The original plan envisioned emitting a copy-paste rewritten
DECIDE query. It was deliberately dropped: the engine has no access to the original SQL text (it
works on `SolverModel` rows, and clause labels are *reconstructed* + canonicalized — the user's
`5 >= x` comes back as `x <= 5`), so a faithful full query is unbuildable and a reconstructed
partial would duplicate what the table already carries. The table is the source of truth; the
headline is only a lean pointer.

**Frozen EAV vocabulary.** Every edit row carries a uniform `attribute='edit_kind'`, so
filtering `attribute='edit_kind'` enumerates all edits and their kinds — the relation is
self-describing:
- `subject_kind='clause'`: `edit_kind` ∈ {`loosen`, `drop`}; plus `suggested_change` + `amount`
  (LOOSEN only), `edit_source` ∈ {`source_literal`, `virtual_offset`, `expanded_row`,
  `expanded_group`} and `offset_scope` ∈ {`clause`, `row`, `group`} (T3, the slack-scope
  provenance), `group` = the PER key value (expanded-mode PER edits only — disambiguates
  per-group edits that share the same folded `subject`).
- `subject_kind='model'`: `achievable_objective` (the I3 number, or `'unbounded'`),
  `elastic_infeasible` (`'true'`, the loosening-cannot-fix-it verdict).

These strings are stable. The former `conflict` edit kind (data-RHS dead-end) was **removed** by
T3 — a data conflict is now a `loosen` with `edit_source='virtual_offset'` (query) or
`'expanded_row'` (expanded).

**Tests.** C++ (`test_decidb_diagnostic_engines.cpp`): the loosen section asserts the clause
pointer (`"diagnosis points to clause \`x <= 5\`"`) and `edit_kind='loosen'`; the must-drop
section asserts the pointer to `"(x <> 3)"`; the query-mode data section asserts a
`virtual_offset` edit and the expanded section asserts per-row `expanded_row` edits. Python
differential (`test_query_diagnostics_relation.py`): a loosen case asserts `edit_kind='loosen'`;
the data-RHS query case asserts `edit_source='virtual_offset'`, the expanded case
`edit_source='expanded_row'`.

## Tests

`test/common/test_decidb_diagnostic_engines.cpp` — SECTIONs drive `DiagnoseInfeasible`
against the bundled HiGHS backend on one-variable models: a relaxable cap conflicting with
a rigid floor reports the unique minimal loosening (`x <= 5` → `x <= 10`, amount 5); an
equality row loosens via its two-sided slack (`x == 5` → `x == 8`); a rigid-only conflict
with a non-helping relaxable row renders the elastic-infeasible row; the
`has_unhandled_user_bounds` flag suppresses that claim (invalid → static error); and an
all-rigid model returns invalid. End-to-end differential coverage (both backends, vs
`oracle_solver`, including absorbed-bound and BETWEEN cases) is in
`test/decide/tests/test_query_diagnostics_relation.py`. That file's `_apply_reported_fix`
harness (E1) guards the core least-change promise mechanically: for each asserted infeasible
diagnosis it applies every reported edit to the SQL (loosen → replace the clause with its
`suggested_change`; drop → remove the clause) and asserts the edited query actually solves.
BOOLEAN user-pin coverage (E3: `x >= 1`, `x = 1`, `x <= 0` pins, plus a domain-restatement
guard asserting `x <= 1 AND x >= 0` never becomes an editable knob) lives there too.
