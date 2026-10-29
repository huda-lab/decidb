# Known bugs

## BUG-1 — `DECIDB_TIME_LIMIT` is not enforced on HiGHS for large models

- **Location:** `src/decidb/naive/deterministic_naive.cpp` (`HighsSession`);
  contrast `src/decidb/gurobi/gurobi_solver.cpp:125`.
- **Observed:** on Q2 at the large TPC-H tier (~1.5M variables, mixed-integer),
  `highs.optimize` began and never returned — not at the requested 300s
  `DECIDB_TIME_LIMIT`, not by 420s wall clock, when an external harness killed
  the process. Every DeciDB-owned span (parse, rewrite, extraction, model
  build, `highs.load`) completed in under 350ms total; the entire remainder
  was spent inside HiGHS's own `run()`. Seen on the large benchmark tier (query Q2 on
  HiGHS, status `process_timeout`); see `../performance/findings.md#solver-execution`.
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
- **Task.** `../pipeline/07_solver/todo.md` § SOL-1.

## BUG-2 — One matrix run of `DECIDB_TEST_DIRECT_SOLVE=off` reported one outcome fewer

- **Observed:** on 2026-10-27 `test/decide/run_direct_matrix.sh` printed `off: 4520 passed, 335 xfailed, 0 failed`
  (4,855 outcomes) where `auto` and `serializer` printed 4,521 + 335. The rerun of the off suite gave 4,521 passed and 0
  skipped. The matrix wrote its logs to a `mktemp -d` folder that was gone, so the missing test is unknown.
- **Next step:** the matrix now keeps its logs (`test/decide/results/matrix_<time>/`) and prints each mode's outcome
  total; when the totals differ again, find the missing test from the logs.

## BUG-3 — Q10 on HiGHS at the large tier is killed for memory, and the profiler records it as a timeout

- **Location:** HiGHS on `benchmark/decide/queries/q10_maximize_abs.sql` at `large`; the skip is
  `SKIP = {("Q10", "highs")}` in `benchmark/decide/profile_pipeline.py`, whose comment says the bug is recorded here.
- **Observed:** in the 2026-09-09 large sweep (`benchmark/decide/results/pipeline_profile_large.csv`,
  `sweep_large.log`), Q10 on Gurobi finishes in all three passes (about 72 to 75 s, peak 6.1 to 6.5 GiB). Q10 on HiGHS
  repeat 0 is recorded as `process_timeout` with an empty `query_s`, and the sweep restarts twice at that cell before the
  skip was added. On `medium`, Q10 on HiGHS peaks at 6,525 MiB at 2.5M variables (`../performance/findings.md#memory-and-scaling`).
- **Expected:** the query finishes, or fails with an ordinary out-of-memory error; the profiler records a memory kill
  as a memory failure, not a timeout.
- **Impact:** a large HiGHS query can take the whole process down, and the profile misreports why.
- **Reproduce:** `make decide-bench-setup`, remove the entry from `SKIP`, run `profile_pipeline.py`. Memory-heavy.

## BUG-14 — Like coefficients that merge past DOUBLE raise an internal error

- **Observed:** the model builder adds like coefficients after each was checked finite, so `SUM(b*x) + SUM(c*x)` with
  b = c = 1e308, or `SUM(POWER(b*x, 2))` with b = 1e200, overflows to infinity and reaches the builder's
  InternalException or a Gurobi API error instead of `DecideInvalidValueMessage`.
- **Fix:** one `AddCoefficient(target, value, written)` helper in the builder checks each sum and raises the message.
  After F4, whose CQ-4 rewrites the builder.

## Limitations (not bugs)

None.

Resolved behavior is documented by its owning `done.md`.
