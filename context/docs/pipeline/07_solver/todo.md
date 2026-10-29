# Stage 07 — Solver: open work

Open work in the backends and their limits. Finished work is in [done.md](done.md).

## SOL-1 — Bound a HiGHS solve by `DECIDB_TIME_LIMIT`

- **Problem.** `../../issues/bugs.md` § BUG-1. The interrupt poll carries Ctrl-C only (`context.interrupted`), so
  overriding `SetInterruptPoll` in `HighsSession` does not bound the solve; HiGHS stopped checking its own
  `time_limit` in the phase it stalled in.
- **Change.** Find that phase from the vendored source (`third_party/highs`; interrupt callbacks exist for MIP and
  simplex) and propose a deadline callback. The agent finds the cause alone and stops with the proposed fix before
  coding.
- **Done when.** Only the owner's large-tier run (Q2, HiGHS) confirms the fix: the query returns with `TIME_LIMIT`
  near the requested budget.
- **done.md.** [done.md](done.md) § 2, "Time limits".
