# Query Diagnostics — Slow (remaining work)

The slow-solve engine shipped (2026-07-02): the checkpoint report (S0–S2), the
in-operator warm-continuation loop with the `decide_on_timeout` pragma (S3), the
success-with-incumbent stop delivery (S4), and the terminal wiring (S5). See `done.md`
for how it all works. Only deferred / out-of-scope items remain below.

## Deferred — touches a non-diagnostic path

- **Populate `decide_diagnostics()` on a caveated success (returned unproven incumbent).**
  As of the relation-parity change, a timed-out solve that *fails* (error mode / no-incumbent
  stop) now stashes a `state='slow'` diagnosis and points to `decide_diagnostics()`, mirroring
  the unbounded / infeasible terminals. A stop that *succeeds* by returning the unproven
  incumbent as rows (`ask` `s`, `continue` Ctrl-C) does **not** — the success delivery path
  unconditionally `ClearDecideDiagnostic`s (`physical_decide.cpp`, end of `Finalize`), so the
  quality (best-possible objective, within-%, elapsed) is only on the stderr caveat, not
  queryable. Surfacing it there would mean stashing on a *successful* solve and teaching the
  clear step to spare a slow diagnosis — a change to the success path, not just diagnostics, so
  it is out of scope for the relation-parity change. Revisit if users want to query "how good
  was the solution I got" after a caveated stop.
- **Minor: report-block time rendering rounds sub-second limits.** `FormatDuration` prints one
  decimal, so `DECIDB_TIME_LIMIT=0.05` shows "hit the 0.1s time limit" / "elapsed 0.1s". Cosmetic,
  and `FormatDuration` is shared, so leave it unless it misleads.

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
