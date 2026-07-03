# Query Diagnostics — Infeasible (remaining work)

The elastic engine is shipped end to end (I1–I5: slack loosening, per-shape slack
placement, `<>` removal, stage-2 achievable objective, lean reporting). See `done.md`
for how it currently works. What remains is a punch-list to make it **production-ready**:
replace the two coarse weighting stand-ins with principled machinery, close two known
granularity gaps, and polish the user-facing output. (T1 — scale-normalized slack
weights — has shipped; see `done.md` "Scale-normalized editable weights".)

Each task below is individually pickable and carries: **Location** (where to work),
**Problem** (what's wrong today), **Decision** (the open choice to settle with the user
before coding), **Test** (the case that proves it), and **Done** (which `done.md` section
to update on ship). Suggested batches are at the bottom.

---

## Weighting architecture

### T2 — Lexicographic ladder replacing the fixed `1 / 1e3 / 1e6` weights

- **Location**: `diagnostic_constants.hpp` (`DIAGNOSTIC_DATA_SLACK_WEIGHT = 1e3`,
  `DIAGNOSTIC_REMOVAL_WEIGHT = 1e6`); consumers in `BuildElasticModel`
  (`decide_diagnostic_engines.cpp:398`, `:533`) and the stage-2 budget row
  (`BuildStage2Model`, ~`:707`).
- **Problem**: the ladder editable(`1`) < data(`1e3`) < removal(`1e6`) is encoded as magic
  constants in one summed objective. This is the classic Big-M brittleness — a real problem
  whose coefficients span more than `1e3` can make a data-slack numerically undercut an
  editable edit and silently invert the intended preference. It is a **separate** problem
  from T1: T1 normalizes competing knobs *within* a tier; T2 makes the ordering *between*
  tiers exact.
- **Decision**: do the full refactor now, or defer? A true lexicographic solve drops the
  weights and runs stage 1 in successive passes (minimize the top tier, freeze it, minimize
  the next). This is the elegant end state and composes cleanly with T1's normalized
  in-tier weights — but it is a bigger change and has **no failing test yet**. If we defer,
  keep it logged here; if we do it, settle the tier order (editable → data → removal) and how
  it interacts with the existing stage-2 budget freeze.
- **Test**: none exists — building one is part of the task. Construct a model whose row
  coefficients span > `1e3` so the summed-weight ladder misorders the fix (data-slack chosen
  over an editable edit), then assert the lexicographic solve orders it correctly.
- **Done**: rewrite the weighting narrative in `done.md` (I2.c editable-knob preference, the
  L0/removal "last-resort weighting" paragraph) and the `diagnostic_constants.hpp` header.

---

## Diagnosis expressivity (virtual knobs for non-literal RHS)

### T3 — Diagnostic policy: invent a user knob when there is no literal RHS to edit

- **Location**: `DiagnoseInfeasible` slack-support partitioning + `ReadElasticEdits`
  (`decide_diagnostic_engines.cpp` ~`:553–620`); `MakeLoosenEdit` / the
  `CONFLICT_SUMMARY` path (~`:284`, `:616`); shape tagging in `ilp_model_builder.cpp`.
- **Problem**: the engine hard-codes one mapping from solver slacks to user edits.
  `SHARED_LITERAL` rows share a slack → `suggested_change`; `PER_ROW_DATA` rows get
  independent slacks and collapse to a `CONFLICT_SUMMARY` — **never an actionable edit** —
  and PER aggregates get one slack per group. When the RHS is data-backed (`x <= col`,
  `x >= demand`, correlated scalar-subquery RHS, `x = target_col`) the user is told *where*
  the conflict is but not *what to change*. Generalize into an explicit policy for inventing
  a **virtual** knob.
- **Decision**: which policies to implement and what the default is:
  - `shared_offset` — one clause-wide offset, diagnose `x <= col` as `x <= col + delta`
    (most actionable when the user can add a tolerance / safety margin).
  - `group_offset` — one offset per PER / grouping bucket (`SUM(x) >= K PER region` reports
    the achievable target per region).
  - `row_profile` — independent row slacks summarized as counts / max violation /
    representative rows (best for bad data / outliers; not directly a query edit).
  - `auto` — keep today's defaults unless the clause shape indicates a better knob.
  Also decide the reporting contract: the relation must **distinguish a virtual edit**
  (`x <= col + delta`) from an actual source literal, and preserve the row/group profile so
  the user can choose between editing the query and fixing the data.
- **Test**: extend `test_query_diagnostics_relation.py` / `..._tpch.py` — a `x <= col` case
  that today reports `conflict` should, under `shared_offset`, report a virtual
  `x <= col + delta` with the delta differential-checked against a re-solve.
- **Done**: new "diagnostic policy / virtual knobs" section in `done.md`; update the I2.c
  data-RHS-conflict narrative to point at the policy.

---

## Known granularity gaps (deferred in `done.md`)

### T4 — Per-group edits for absorbed easy-MAX + PER

- **Location**: absorbed easy-MAX bound path (`physical_decide.cpp` bound absorption) +
  block grouping in `BuildElasticModel` (`decide_diagnostic_engines.cpp`). See `done.md`
  "per-shape slack placement" (~line 164-166).
- **Problem**: `MAX(e) <= K PER g` is absorbed as a **column bound**, which does not preserve
  the PER grouping, so it re-emits as one *global* shared block — the user-facing edit
  (`K → K + max overshoot`) is correct but loses per-group granularity that the non-absorbed
  PER aggregate path has.
- **Decision**: carry group keys through bound absorption (extra plumbing on `UserBoundSpec`),
  or accept the global collapse as a documented limitation? Weigh the plumbing cost against
  how often easy-MAX+PER appears.
- **Test**: `test_query_diagnostics_relation.py` — an easy-MAX+PER case asserting one edit
  per failing group (currently one global edit).
- **Done**: update the easy-MAX+PER paragraph in `done.md` "per-shape slack placement."

### T5 — Let stage 2 re-optimize the removal set for the objective

- **Location**: `BuildStage2Model` (`decide_diagnostic_engines.cpp` ~`:686–710`), which pins
  each removal binary `w` to its stage-1 value. See `done.md` "L0 / removal dial" (~line 366).
- **Problem**: stage 2 **freezes** the DROP set at the stage-1 choice, so the reported drop
  set is stable but need not be the one best for the user's objective. Among equally-minimal
  removal sets, a different `<>` to drop might yield a higher achievable objective.
- **Decision**: let `w` re-optimize in stage 2 (drop set may then differ from stage 1 — is a
  drop set that changes between stages acceptable to report?), or keep the freeze for stability
  and document it? This is a genuine stability-vs-optimality trade-off to settle with the user.
- **Test**: a two-`<>` model where the stage-1 drop and the objective-best drop differ.
- **Done**: update the stage-2 composition paragraph in `done.md` "L0 / removal dial."

---

## Output polish

### T6 — Tighten default stderr wording and labels

- **Location**: `BuildInfeasibleDiagnostic` and the label/provenance formatting
  (`decide_diagnostic.cpp`).
- **Problem**: the engine emits correct diagnoses, but some default output is awkward for SQL
  users — grouped/PER subjects where the subject text and the `group` attribute duplicate
  context, and multi-edit summaries that read as a flat list of equally-minimal alternatives
  when the engine actually found a combined repair set.
- **Decision**: mostly wording — settle the grouped-subject phrasing and how a multi-edit
  repair set is summarized (combined set vs. list of alternatives) without losing the richer
  relation rows. Keep to the project's user-facing voice (name the clause + smallest edit,
  no solver jargon).
- **Test**: string assertions in `test_query_diagnostics_relation.py` headline checks.
- **Done**: refresh the I5 "lean cue summary" examples in `done.md`.

---

## Deferred notes

- **Degeneracy guard (deferred, from T1's decision).** A backstop that rejects any elastic edit
  collapsing the user's objective to zero and surfaces the objective-preserving alternative
  instead. Not implemented: T1's scale-normalized weights already steer stage 1 to the
  genuinely-tight constraint, after which stage-2 objective-maximization keeps
  `achievable_objective > 0`, so the guard is a redundant second concern with no failing test.
  Revisit only if a case surfaces where even the geometrically-smallest edit zeroes the objective.

---

## Suggested batches

- **Batch 1 (user-facing): T3 + T6.** The virtual-knob policy is the biggest expressivity
  win (turns dead-end conflict summaries into actionable edits) and pairs naturally with the
  wording polish, since both touch the reporting layer.
- **Batch 2 (weighting refactor): T2.** Normalized in-tier weights (T1, shipped) and the
  lexicographic between-tier ladder (T2) compose; with T1 in, T2 is a clean swap. No failing
  test forces T2, so it can trail.
- **Batch 3 (granularity): T4 + T5.** Both are localized refinements to shipped machinery;
  low urgency, pick up opportunistically.


