# Query Diagnostics — Slow (remaining work)

The slow-solve engine shipped (2026-07-02): the checkpoint report (S0–S2), the
in-operator warm-continuation loop with the `decide_on_timeout` pragma (S3), the
success-with-incumbent stop delivery (S4), and the terminal wiring (S5). See `done.md`
for how it all works. Only deferred / out-of-scope items remain below.

## Superseded / deferred (out of scope)

- **Instantaneous Ctrl-C checkpoint: deferred (planned follow-up).** Today `continue`
  mode polls `ClientContext::interrupted` only at each chunk boundary, so Ctrl-C stops at
  the **next** checkpoint (up to one full chunk later), not mid-solve. Interrupting the
  solve *immediately* needs a solver-callback interrupt (Gurobi `GRBterminate` / HiGHS
  `setCallback`+`startCallback`, both confirmed to exist) plus coordination with DuckDB's
  own SIGINT handler. Add this so a long chunk can be cut short on demand.
- **Bucket B elastic-as-classifier: deferred.** Running the elastic engine on a
  no-solution timeout to decide "hard vs infeasible" is a nice future refinement; v1 just
  reports "no solution yet" and offers to continue.
- **Diverging-bound → unbounded hand-off: deferred.** An unbounded MILP that reaches the
  limit (incumbent + bound running to ∞) could route to `unbounded/`. Rare (unbounded is
  normally caught pre-timeout via ray extraction); revisit if it shows up.
