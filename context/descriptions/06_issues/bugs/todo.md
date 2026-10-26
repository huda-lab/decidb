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

## An order-0 norm over a reducer or over data raises an internal error

- **Location:** not traced; the solver path's linearization of `norm(x, 0)` (its indicator variables) when the norm sits
  inside another reducer. Direct solve declines this form, so the error comes from the solver path under every setting.
- **Observed:** on `CREATE TABLE t AS SELECT i AS id, (i%3)::DOUBLE AS v FROM range(6) r(i)`,
  `FROM t DECIDE x(REAL) SUCH THAT x BETWEEN 0 AND 1 AND SUM(MAX(norm(x,0))) <= 2 MAXIMIZE SUM(v*x)` raises
  `INTERNAL Error: DECIDE term 'max(sum(__l0auto_ind_0__))' holds a decision variable under an expression that is not
  linear algebra`, with `decide_direct_solve` `off` and `auto` alike. `MINIMIZE SUM(MAX(norm(x,0))) PER id` fails the
  same way. On the right side over data, `SUM(x*w) <= norm(cap, 0)` raises `INTERNAL Error: DECIDE constraint reached
  term extraction with invalid aggregate/per-row homogeneity: '(sum((x * w)) <= sum(__l0auto_ind_0__))'` on both
  paths. The right-hand-side rewrite declines the order-0 norm tag, so that decline has no passing test until this is
  fixed.
- **Expected:** either the model is solved or the user gets an ordinary error that names the form it does not support;
  never an internal error.
- **Impact:** a query a user can write crashes with an internal error that points at generated names.

## A DECIDE that refers to an outer query raises an internal error

- **Location:** not traced; DuckDB's subquery decorrelation (`FlattenDependentJoins`), which has no case for the
  `DECIDE` operator.
- **Observed:** with `g(k)` and `t(id, k, v)`,
  `SELECT g.k, q.id, q.x FROM g, LATERAL (FROM t WHERE t.k = g.k DECIDE x(BOOL) SUCH THAT SUM(x) <= 1 MAXIMIZE
  SUM(v*x)) q` raises `INTERNAL Error: Logical operator type "DECIDE" for dependent join`, with `decide_direct_solve`
  `off` and `auto` alike, and with a `PER` clause added. When only the right side refers to the outer query,
  `... LATERAL (FROM t DECIDE x(REAL) SUCH THAT x BETWEEN 0 AND 1 AND SUM(x*w) <= SUM(cap) + g.k MAXIMIZE SUM(w*x)) q`
  raises `INTERNAL Error: Failed to bind column reference "k"`.
- **Expected:** a correlated DECIDE either runs once per outer row or is rejected at bind time with an ordinary error
  that says a DECIDE cannot refer to an outer query.
- **Impact:** a user who writes one problem per group with `LATERAL` gets an internal error instead of a pointer to
  `PER`.

## `COUNT` of a text column on the right side raises a conversion error

- **Location:** not traced; the solver path's reading of a data-only `COUNT` on the right side of a constraint.
- **Observed:** with `g` a `VARCHAR` column, `FROM t DECIDE x(REAL) SUCH THAT x BETWEEN 0 AND 1 AND SUM(x*w) <=
  COUNT(g) MAXIMIZE SUM(w*x)` raises `Conversion Error: Could not convert string 'a' to DOUBLE`, with
  `decide_direct_solve` `off` and `auto` alike. `COUNT` of a numeric column answers.
- **Expected:** `COUNT(g)` counts the non-null values of `g` whatever its type.
- **Impact:** a valid query fails with an error that points at the column's values, not at DECIDE.

## Q10 on HiGHS at the large tier is killed for memory, and the profiler records it as a timeout

- **Location:** HiGHS on `benchmark/decide/queries/q10_maximize_abs.sql` at `large`; the skip is
  `SKIP = {("Q10", "highs")}` in `benchmark/decide/profile_pipeline.py`, whose comment says the bug is recorded here.
- **Observed:** in the 2026-09-09 large sweep (`benchmark/decide/results/pipeline_profile_large.csv`,
  `sweep_large.log`), Q10 on Gurobi finishes in all three passes (about 72 to 75 s, peak 6.1 to 6.5 GiB). Q10 on HiGHS
  repeat 0 is recorded as `process_timeout` with an empty `query_s`, and the sweep restarts twice at that cell before the
  skip was added. On `medium`, Q10 on HiGHS peaks at 6,525 MiB at 2.5M variables (`05_performance/08_memory_and_scaling/done.md`).
- **Expected:** the query finishes, or fails with an ordinary out-of-memory error; the profiler records a memory kill
  as a memory failure, not a timeout.
- **Impact:** a large HiGHS query can take the whole process down, and the profile misreports why.
- **Reproduce:** `make decide-bench-setup`, remove the entry from `SKIP`, run `profile_pipeline.py`. Memory-heavy.

## `test_large_scale.py` is never collected

- **Location:** `benchmark/decide/test_large_scale.py`, moved out of `test/decide/tests/` in `9943450ca1`.
- **Observed:** `test/decide/pytest.ini` collects only `tests/`, so `run_tests.sh -m large_scale` selects no test. Run
  directly (`test/decide/.venv/bin/python -m pytest --setup-plan benchmark/decide/test_large_scale.py`), it fails with
  `ModuleNotFoundError: No module named 'solver'`: it imports `solver.types` and `comparison.compare` and uses fixtures
  from `test/decide/conftest.py`, which apply only under `test/decide/`.
- **Expected:** the two scale cases (501 and 2,204 rows) run with the suite, or the file is deleted.
- **Impact:** the scale cases are unverified while docs list them.

## `make decide-view` opens the newest JSON in `results/`, whichever tool wrote it

- **Location:** `benchmark/decide/view_results.py`, `find_result_file`.
- **Observed:** the archived S1 profilers (`benchmark/decide/direct_solve/archive/profile_direct_s1.py`,
  `profile_direct_s1_api.py`) write `direct_s1_*.json` manifests into `benchmark/decide/results/`. After one runs,
  `make decide-view` with no argument tries to render that manifest as a suite result.
- **Expected:** the viewer picks only suite results (by name or by shape), or the profilers write elsewhere, as
  `direct_solve/run.py` does (`results/direct_solve/`).
- **Impact:** the default view shows the wrong file after a profiler run.

## With a gap of 0, HiGHS cannot finish an entity-scoped MIP the test suite runs

- **Location:** `HighsSession::Load` in `src/decidb/naive/deterministic_naive.cpp`, which sets `mip_rel_gap` to 0
  (`01_pipeline/07_solver/done.md`, Optimality gap); the test is
  `test/decide/tests/test_entity_scope.py::test_entity_scoped_mixed_when_per`.
- **Observed:** under `DECIDB_FORCE_SOLVER=highs` (the matrix's `highs` run), HiGHS reaches incumbent 74,999.84 against
  bound 75,000 within seconds, a gap of 0.0002%, and then fails to close it before the test's 120 s timeout. With HiGHS's
  default gap of 1e-4 the test passed. Gurobi closes the same model at gap 0. The test is unchanged.
- **Expected:** a query answers with the optimum by default, and a user who accepts a near-optimal answer can say so.
  The proposed fix is a `decide_mip_gap` setting, default 0, passed to both backends in place of the hard-coded 0;
  this test would then set 1e-4 on HiGHS.
- **Impact:** the `highs` matrix run has a second failure besides `test_norm_combined_l1_l2_objective`. A HiGHS user
  with a hard MIP can wait far longer than before for a proof the answer did not need.

Resolved behavior is documented by its owning `done.md`.
