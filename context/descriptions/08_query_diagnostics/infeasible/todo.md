# Query Diagnostics — Infeasible (remaining work)

The elastic engine is shipped end to end (I1-I5: slack loosening, per-shape slack
placement, `<>` removal, stage-2 achievable objective, lean reporting), plus the T1
scale-normalized editable weights, T2 lexicographic repair ladder, T3 two-mode slack-scope
policy (query vs expanded), and T5 lean-reporting polish. See `done.md` for how it currently
works. What remains is one stage-2 removal-set refinement.

Each task below is individually pickable and carries: **Location** (where to work),
**Problem** (what's wrong today), **Decision** (the open choice to settle with the user
before coding), **Test** (the case that proves it), and **Done** (which `done.md` section
to update on ship). Suggested batches are at the bottom.

---

## Removal-set refinement

### T4 — Let stage 2 re-optimize the removal set for the objective

- **Location**: `BuildStage2Model` (`decide_diagnostic_engines.cpp` ~`:940–958`), which pins
  each removal binary `w` to its stage-1 value. See `done.md` "L0 / removal dial" (~line 452).
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

- **Batch 1 (removal refinement): T4.** Localized stage-2 DROP-set optimality/stability
  trade-off; low urgency, pick up opportunistically.
