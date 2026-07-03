# Query Diagnostics — Slow (remaining work)

The slow-solve engine shipped (2026-07-02): the checkpoint report (S0–S2), the
in-operator warm-continuation loop with the `decide_on_timeout` pragma (S3), the
success-with-incumbent stop delivery (S4), and the terminal wiring (S5). See `done.md`
for how it all works. Only deferred / out-of-scope items remain below.

## Remaining sub-items (small)

- **Mid-solve Ctrl-C: HiGHS + first chunk.** Shipped 2026-07-03 for **`continue` mode on
  Gurobi** (a watcher thread calls `GRBterminate`; see `done.md`). Two gaps remain: **HiGHS**
  stays boundary-only (its interrupt needs the `setCallback`/`startCallback` path, not a
  thread-safe terminate) — the solver-agnostic *fallback*; and the **first chunk** (run inside
  `SolveModel`, before the operator installs the poll) is not interruptible — only continuation
  chunks are. Both are minor: HiGHS is the slow non-recommended backend, and continue users set
  small chunks, so the first chunk is short.
- **Minor: report-block time rendering rounds sub-second limits.** `FormatDuration` prints one
  decimal, so `DECIDB_TIME_LIMIT=0.05` shows "hit the 0.1s time limit" / "elapsed 0.1s". Cosmetic,
  and `FormatDuration` is shared, so leave it unless it misleads.

## Superseded / deferred (out of scope)

- **Bucket B elastic-as-classifier: attempted 2026-07-03, reverted — provably non-viable.**
  The idea was to run the elastic model on a no-solution timeout to classify hard-vs-infeasible.
  Implemented and tested, then removed, because it cannot conclude on the very problems that
  reach this state: a no-incumbent timeout means *finding a feasible point is hard*, and the
  elastic classifier's two possible verdicts each require solving something at least as hard —
  a "feasible" verdict needs a zero-slack point (a feasible point of the original, the exact
  task that just timed out), and an "infeasible" verdict needs to prove min-slack > 0 (prove
  infeasibility, which if quick would have fired the INFEASIBLE terminal *before* the timeout).
  Empirically: an N=40 feasible market-split stayed "undetermined" at 0.3s / 1s / 3s (the
  elastic is exactly as hard as the original). So it added a full extra solve's latency for a
  verdict it essentially never produces. Revisit only if a *cheap* infeasibility certificate
  (not a full elastic re-solve) becomes available.
- **Diverging-bound → unbounded hand-off: deferred — assessed as not a reachable state.**
  The idea was to route an unbounded MILP that reaches the limit (incumbent + bound running
  to ∞) to `unbounded/`. On investigation this does not occur: both backends flag
  unboundedness at presolve / root as `UNBOUNDED` / `INF_OR_UNBD` **independent of the time
  limit**, and the router already sends those (with a ray) to the unbounded terminal — so a
  `TIME_LIMIT` result never carries a non-finite bound. And even if it did, an MILP time-out
  has **no ray**, so the unbounded engine's core (name the escaping variable via the ray)
  could not run; the best a hand-off could do is a generic "looks unbounded" note. Detection
  would be untestable dead code (no way to construct a solve that reaches this state).
  Revisit **only** if a real query is observed hitting `TIME_LIMIT` with a diverging bound.
