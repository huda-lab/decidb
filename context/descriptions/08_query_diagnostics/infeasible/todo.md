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

---

## Suggested batches

- None active.
