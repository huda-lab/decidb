# Query Diagnostics — Infeasible (remaining work)

The elastic engine is shipped end to end (I1-I5: slack loosening, per-shape slack
placement, `<>` removal, stage-2 achievable objective, lean reporting), plus the T1
scale-normalized editable weights, T2 lexicographic repair ladder, T3 two-mode slack-scope
policy (query vs expanded), T4 objective-best stage-2 removal-set refinement, and T5
lean-reporting polish. See `done.md` for how it currently works. No active implementation
tasks remain here; this file only tracks deferred follow-ups.

---

## Deferred notes

- **Degeneracy guard (deferred, from T1's decision).** A backstop that rejects any elastic edit
  collapsing the user's objective to zero and surfaces the objective-preserving alternative
  instead. Not implemented: T1's scale-normalized weights already steer stage 1 to the
  genuinely-tight constraint, after which stage-2 objective-maximization keeps
  `achievable_objective > 0`, so the guard is a redundant second concern with no failing test.
  Revisit only if a case surfaces where even the geometrically-smallest edit zeroes the objective.
- **Broader repair-tie policy (deferred after T4/A5).** The stage-2b rank tie-break now covers
  every repair kind (removals, editable loosens, data offsets), with and without an objective,
  so both backends name the same clause on the common single-drop / single-loosen ties. Gaps
  that remain. (a) **Rank-sum ties**: the tie-break minimizes a rank-weighted sum, which is not
  a total order over repair *sets* — two different equally-budgeted sets with an equal rank-sum
  (only reachable at cardinality `≥ 2`) can still be solver-arbitrary. A super-increasing
  weighting would totalize it but reintroduces the Big-M scaling the lexicographic passes
  removed; a second frozen pass over a finer key is the clean fix if a case surfaces.
  (b) **Quadratic-objective ties**: the tie-break is skipped when the objective is quadratic
  (freezing it needs a quadratic row), so an exact tie there is still solver-dependent.
  (c) **Absorbed-bound declaration order**: slack ranks follow emission order — matrix rows in
  declaration order, then re-emitted absorbed bounds (their synthetic clause ids start past the
  matrix rows; the bound's original position among the clauses is not recorded anywhere). So on
  a bound-vs-row tie the row wins regardless of which was written first; true source order would
  need a declaration-rank stamp carried from the binder through `user_absorbed_bounds` and
  `ConstraintProvenance`. (d) **User-intent criteria** beyond source order (preserve the more
  meaningful clause, expose multiple equivalent repairs) remain unaddressed.
- **Unbounded-after-fix witness policy (deferred after T4).** A minimal infeasibility repair can
  make the original problem feasible while the restored objective is unbounded. Current behavior
  reports the repair plus `achievable_objective = 'unbounded'`; T4 did not change that. A
  future task could decide whether an unbounded post-fix model should still optimize for a more
  useful finite witness, name which repair unlocks the unbounded direction, or report multiple
  equivalent repairs.

- **A per-row clause's diagnosis does not name which PER group failed (deferred, 2026-08-18).**
  An unreachable bound reduced per group names the clause with its qualifier but not the
  group carrying the failure:

  ```sql
  WITH data AS (SELECT 0 AS g, -1e1000::DOUBLE AS cap UNION ALL SELECT 0, -1e1000::DOUBLE
                UNION ALL SELECT 1, 3.0 UNION ALL SELECT 1, 1.0)
  SELECT g, x FROM data
  DECIDE x(INT) SUCH THAT x >= 0 AND x <= 6 AND MIN(x) <= MAX(cap) PER g MAXIMIZE SUM(x)
  ```

  reports `clause  MIN(x) <= -inf PER g  unreachable_bound  true`. Group `0` carries the
  infinity; group `1`'s bound is finite and fine, and with many groups the user cannot tell
  which to look at. The cause is not in the diagnosis layer —
  `CollectUnreachableClauses` reads `group_label` and emits the `group` EAV row whenever it
  is set, the same as `MakeLoosenEdit`. `ConstraintProvenance::group_label` is stamped only
  on the *aggregate* emission path (`ilp_model_builder.cpp:861`, `:1177`), and the hard
  MIN/MAX `NO_SOLUTION` re-emission sets `lhs_is_aggregate = false`, so it goes down the
  per-row branch, which never stamps a label. The limitation is that branch's, not this
  constraint's, so a fix would change the subject text of other per-row grouped diagnoses
  too — check `_apply_reported_fix` and the golden dump before committing to it. Deferred
  as cosmetic: the finding is correct and the qualifier keeps the clause recognizable, only
  the instance is underspecified.

---

## Open architectural question — should elastic diagnosis be a stage-05 rewrite?

**Raised 2026-08-25, while fixing the composed MIN/MAX misattribution. Recorded, not
scheduled — deliberately out of scope before sign-off, since it reopens stages 07 and 08.**

The elastic program is expressible as a DECIDE query. Given

```sql
SUCH THAT SUM(x) >= 19 AND MIN(x + c) + SUM(x) <= 22
```

its diagnosis is

```sql
DECIDE x(REAL), e1(REAL), e2(REAL)
SUCH THAT SUM(x) + e1 >= 19 AND MIN(x + c) + SUM(x) - e2 <= 22
MINIMIZE e1 + e2
```

Nothing there needs a solver-level facility; it is DECIDE expressing DECIDE. The engine today
instead builds the elastic model at stage 07/08, *after* lowering, where one user clause has
already fanned into an outer row, envelope rows and closing rows. It must therefore reconstruct
which of those rows is the user's editable knob — and that reconstruction is what produced both
halves of the bug fixed in `done.md` (I2.e): the wrong clause name and the wrong repair amount.

Attaching the slack at stage 05, on the canonical bound tree before lowering, makes that
reconstruction unnecessary. `e2` rides through the MIN/MAX lowering as part of the user's
clause, so "which row is the knob" is never asked and the defect class is unrepresentable.

**What the move would cost, and what has to be answered first:**

- **Weights.** `rms_norm` needs each row's coefficient magnitudes, which exist only after the
  model is built. A source-level rewrite needs those weights before it has them.
- **Lexicographic tiers.** DECIDE's `MINIMIZE` is single-objective; the engine runs editable /
  data-offset / removal as ordered tiers. Each becomes a separate solve.
- **The removal tier.** Dropping a clause needs a binary and a Big-M — expressible, but that is
  the `<>` machinery, the most expensive construct in the system.
- **Cost.** Re-binding and re-planning a rewritten query per diagnosis, against mutating a model
  already in memory. `README.md`'s own argument for compiling DuckDB in-process is that "one
  query needs more than one solve: the diagnostic programs are an example."
- **Recursion.** The diagnosis of a DECIDE query is a DECIDE query, which can itself fail.

**Where it would live if taken:** stage 05 owns formulation selection and returns emitted rows
through stage 03's entry points, which is the shape this rewrite has. Stage 07/08 would keep
only the solve and the readback.

**Second defect class it would close (added 2026-08-25, from B3).** A diagnosis is computed
inside the model *as it was built*, and every Big-M and derived column ceiling in that model is
sized from the decision box as the query states it. The repair the engine is looking for is a
**widening** of that box, so an encoding sized at the old width can make the better repair
unrepresentable — and where one arm bakes in nothing and the other does, the same query gets
different advice on different hosts. `done.md` closes the two instances that bit (ABS and
MIN/MAX) with a local rule: a clause demanding more of an auxiliary than the box can supply
sizes that auxiliary itself.

`<>`, bilinear (McCormick) and `norm` have encodings of the same shape and are **not** covered.
They are left alone deliberately rather than patched one at a time: attaching slack before
lowering makes the whole class unrepresentable, exactly as it does for the misattribution class
above. Anyone extending the local rule to a third construct should read that as the signal to
price this rewrite instead.

---

## Suggested batches

- None active.
