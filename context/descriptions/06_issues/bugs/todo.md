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

## An old DECIDE spelling inside a subquery names an internal function

- **Location:** the parser markers `WHEN_CONSTRAINT_TAG` and
  `QUALIFIED_REDUCER_TAG` (`src/include/duckdb/common/enums/decide.hpp`); no
  DECIDE check looks inside a subquery for them.
- **Observed:** the lexer arms the DECIDE keywords inside a subquery written in
  the clause, so `x <= (SELECT sum(v) WHEN g > 1 FROM t)` and
  `x <= (SELECT sum(t: v) FROM t)` parse, and the subquery's ordinary binder then
  answers *"Catalog Error: Scalar Function with name __when_constraint__ does not
  exist!"* (resp. `__qualified_reducer__`).
- **Expected:** a message in SQL terms naming the spelling, as the new scope
  spellings get from `ValidateDecideNoUnsupportedScope` in the same position.
- **Impact:** the statement is refused either way; only the message is wrong.
  The postfix `WHEN` goes with item 7 of `09_anr_language/todo.md`; `SUM(D: e)`
  stays and keeps the problem until it is fixed.

Resolved behavior is documented by its owning `done.md`.
