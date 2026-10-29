# Known bugs

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

## BUG-15 — A Ctrl-C before any solution reads as a time limit

- **Observed:** on both backends, interrupting a solve that has found nothing prints `DECIDE stopped at your request
  before finding a solution yet.` and then `DECIDE optimization hit the time limit...` (`ThrowDecideSolveError`,
  `ilp_solver.cpp`), and `decidb -c` exits 0 after it. Repro: the market-split query of
  `test_query_diagnostics_slow.py` with `DECIDB_TIME_LIMIT=20`, SIGINT at 1 s.
- **Needs.** The agent finds the cause and stops with a proposed fix before coding.

## Limitations (not bugs)

None.

Resolved behavior is documented by its owning `done.md`.
