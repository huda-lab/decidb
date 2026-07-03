# Query Diagnostics — Infeasible (remaining work)

The elastic engine is shipped end to end (I1-I5: slack loosening, per-shape slack
placement, `<>` removal, stage-2 achievable objective, lean reporting), plus the T3
two-mode slack-scope policy (query vs expanded) and its lean-reporting polish (T5). See
`done.md` for how it currently works. What remains is a punch-list to make it
**production-ready**: replace the coarse between-tier weighting stand-in with principled
machinery, and finish one stage-2 removal-set refinement. (T1 - scale-normalized slack
weights - and T3/T5 - the slack-scope policy - have shipped; see `done.md`
"Scale-normalized editable weights" and "Slack-scope policy: query vs expanded".)

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

## Removal-set refinement

### T4 — Let stage 2 re-optimize the removal set for the objective

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

## Deferred notes

- **Degeneracy guard (deferred, from T1's decision).** A backstop that rejects any elastic edit
  collapsing the user's objective to zero and surfaces the objective-preserving alternative
  instead. Not implemented: T1's scale-normalized weights already steer stage 1 to the
  genuinely-tight constraint, after which stage-2 objective-maximization keeps
  `achievable_objective > 0`, so the guard is a redundant second concern with no failing test.
  Revisit only if a case surfaces where even the geometrically-smallest edit zeroes the objective.

---

## Suggested batches

- **Batch 1 (user-facing): T3 + T5. — SHIPPED.** The two-mode slack-scope policy landed:
  `query` (default) turns dead-end data conflicts into actionable virtual offsets and folds
  every PER clause into one SQL-literal edit; `expanded` exposes the per-row / per-group
  fanout profile. The stderr headline stays a lean clause pointer. See `done.md`
  "Slack-scope policy: query vs expanded".
- **Batch 2 (weighting refactor): T2.** Normalized in-tier weights (T1, shipped) and the
  lexicographic between-tier ladder (T2) compose; with T1 in, T2 is a clean swap. No failing
  test forces T2, so it can trail.
- **Batch 3 (removal refinement): T4.** Localized stage-2 DROP-set optimality/stability
  trade-off; low urgency, pick up opportunistically.
