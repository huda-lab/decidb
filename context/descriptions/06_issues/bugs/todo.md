# Known Bugs — Open

## `DECIDB_TIME_LIMIT` is not enforced on HiGHS for large models

- **Location:** `src/decidb/naive/deterministic_naive.cpp` (`HighsSession`);
  contrast `src/decidb/gurobi/gurobi_solver.cpp:125`.
- **Observed:** on Q2 at the large TPC-H tier (~1.5M variables, mixed-integer),
  `highs.optimize` began and never returned — not at the requested 300s
  `DECIDB_TIME_LIMIT`, not by 420s wall clock, when an external harness killed
  the process. Every DeciDB-owned span (parse, rewrite, extraction, model
  build, `highs.load`) completed in under 350ms total; the entire remainder
  was spent inside HiGHS's own `run()`. Reproduced during the
  `pipeline_profile.py` large-tier sweep
  (`benchmark/decide/results/pipeline_profile_raw_large/Q2_highs_0/`,
  status `process_timeout`); see `05_performance/06_solver_execution/`.
- **Cause:** `HighsSession` inherits `SolverSession::SetInterruptPoll`'s
  no-op base instead of overriding it, so a HiGHS solve has no external
  cancellation path — the only thing bounding it is HiGHS's own internal
  check of the `time_limit` option, and that check's granularity was too
  coarse to return within any reasonable margin of the budget on this model.
  Gurobi does override `SetInterruptPoll` and is not affected.
- **Expected:** `DECIDB_TIME_LIMIT` bounds a HiGHS solve the same way it
  bounds Gurobi's — the query returns with a `TIME_LIMIT` status near the
  requested budget, not an unbounded hang.
- **Impact:** a user who sets a time limit expecting a bounded wait can have
  the query hang indefinitely on HiGHS once the model is large enough; there
  is no DeciDB-level safety net.

## A `BOOL` decision takes a value outside 0 and 1 under a bound with no `WHEN`

- **Location:** not traced; the solver path's reading of a variable bound.
- **Observed:** a bound on a `BOOL` decision with no `WHEN` that lies outside 0 and 1 is read as a bound on the variable,
  not as Boolean arithmetic. A lower bound below 0 (`x >= -3`, `x > -3`, `x = -1`) makes `x` signed, so the result holds
  -3, -1 or -2. A bound that cannot hold (`x >= 2`, `x <= -1`, `x < 0`, `x > 1.5`) raises a message naming the clause
  instead of DECIDE's infeasible error. The same bound with a `WHEN` is plain arithmetic and behaves correctly.
- **Expected:** a `BOOL` decision only ever takes 0 or 1. A bound outside that range is Boolean arithmetic: one that
  always holds changes nothing, and one that cannot hold makes the problem infeasible with DECIDE's usual error.
- **Impact:** a user can get a non-Boolean value back from a `BOOL` decision. Direct solve declines these bounds
  (`08_direct_solve/rules/s1/done.md` § Explicitly out of scope) and can admit them once this is fixed.

Resolved behavior is documented by its owning `done.md`.
