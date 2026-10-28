# Known bugs

## `DECIDB_TIME_LIMIT` is not enforced on HiGHS for large models

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

## One matrix run of `DECIDB_TEST_DIRECT_SOLVE=off` reported one outcome fewer

- **Observed:** on 2026-10-27 `test/decide/run_direct_matrix.sh` printed `off: 4520 passed, 335 xfailed, 0 failed`
  (4,855 outcomes) where `auto` and `serializer` printed 4,521 + 335. The rerun of the off suite gave 4,521 passed and 0
  skipped. The matrix wrote its logs to a `mktemp -d` folder that was gone, so the missing test is unknown.
- **Next step:** the matrix now keeps its logs (`test/decide/results/matrix_<time>/`) and prints each mode's outcome
  total; when the totals differ again, find the missing test from the logs.

## Q10 on HiGHS at the large tier is killed for memory, and the profiler records it as a timeout

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

## Messages print a subquery, a condition or a `by` key as DuckDB renders it

- **Location:** a subquery's source fragment, and a `by` key's text, is its parsed
  expression's `ToString` (`TagDecideSourceFragments` in `decide_source_provenance.cpp`,
  `BindReducerBy`), and a condition is re-rendered from its bound form
  (`RenderDecideSource`).
- **Observed:** `when v in (SELECT 1000): ...` is quoted as `when (v = ANY(SELECT
  1000))`; `when exists (SELECT 1 WHERE false): ...` gives
  *"... when EXISTS(SELECT 1 WHERE CAST('f' AS BOOLEAN)) ..."*; `when id > 100 and g is
  not null:` gives *"when id > 100 AND (g IS NOT NULL)"*. A `VALUES` list in a subquery
  becomes `(SELECT * FROM (VALUES ('East', 30)) AS valueslist) AS c(region, cap)`, in a
  `when` message and in the NULL-subquery message alike; `by (id % 2)` is quoted as
  `by ((id % 2))`.
- **Expected:** the text as written. That needs each node's source span, which the
  parser does not keep (only its start).
- **Impact:** messages only; they stay recognizable.

## Messages raised while choosing a formulation name columns without their relation

- **Location:** stage 05 has no bind context, so `RenderDecideSource` there (the
  composed walker's *"'s' beside MIN or MAX"* in `decide_rewrite_minmax.cpp`, the
  objective's product refusal in `decide_linear_form.cpp`) cannot qualify a column by its
  relation over a join; stage 04's refusals can (`QuoteMessagesWith`).
- **Observed:** over the deck's join, `MAXIMIZE when S.priority: sum(ship) - max(ship) +
  D.capacity * s` gives *"DECIDE: 'capacity * s' beside MIN or MAX is not supported
  yet"*. A reducer's `by` key keeps its written text either way.
- **Expected:** `'D.capacity * s'`: stage 05 carries the relation names (a `LogicalDecide`
  field, serialized), or the refusal moves to stage 04.
- **Impact:** messages only.

## Wrong answers: solver tolerance

Each reproduces on the current build.

- **Gurobi accepts a row violated by 1 when its coefficients are near 10⁹:** over three
  rows with `x <= cap` (3, 5, 7), `y <= 5`, `per row: y + sum(x * 1000000000::DECIMAL(18,0))
  >= 5000000001` (or `> 5000000000`), `MINIMIZE SUM(x) + SUM(y)` answers `sum(x) = 5`,
  `y = 0` on Gurobi: `5000000000 >= 5000000001` fails. HiGHS answers `sum(x) = 6`. Presolve
  folds the helper's definition into the row and scales it, so the violation falls under
  the feasibility tolerance. The same clause without `y` (every coefficient 1e9) answers 6
  on Gurobi, so the trigger is the span between a clause's coefficients, not a large right
  side. **Ruling (owner, 2026-10-27).** Warn when a clause's coefficients span more
  than the solver resolves, naming the clause. Rescaling the model is a vendor workaround
  and stays out of it.
- **Design (owner, 2026-10-28).**
  - The span is `max|a| / min|a|` over each row a clause makes, in the user's own numbers:
    every auxiliary is replaced by what it stands for (a value helper by its sum, a MIN/MAX
    helper by its members, an ABS by its argument, a product helper by the coefficient on
    the product), an `IN` domain's values count, and so do a quadratic clause's
    coefficients. Numbers DeciDB invents (Big-M, the L0 tolerance) are left out.
  - The check runs over `SolverInput` after `BuildSolverInput`, before the linearizers clear
    the helper definitions, and attributes to the reading clause.
  - The threshold is 1 / the backend's feasibility tolerance (Gurobi 1e-6 → 1e6, HiGHS 1e-7
    → 1e7, the vendor defaults), declared beside the backend's other properties
    (`SolverCapabilities`, not `SolverConstructSupport`).
  - stderr, once per query, one line per clause, quoted as written: *DECIDE warning: the
    numbers in 'per row: y + sum(x * 1000000000) >= 5000000001' range from 1 to
    1000000000, wider than the solver checks exactly; the answer may break this clause
    slightly.*
  - Before committing, count the clauses that warn over the test suite and the TPC-H models.
- **Impact:** a wrong optimum, or a clause violated in the answer.

## Messages: operands, coefficients and keys

Each reproduces on the current build.

- **Two keys over the same columns keep the key text of the one bound first**
  (cosmetic): with `per D.d, D.cost: open(BOOL)` declared, `SUM(D: D.cost * open) <= 7`
  is echoed by EXPLAIN as `sum(D.d, D."cost": "cost" * open) <= 7`, and a determination
  message quotes `(in sum(D.d, D."cost": ...))`; declared `per D: open(BOOL)`, the same
  clause echoes `sum(D: "cost" * open) <= 7`. Both keys name the same columns, so they
  share one entry of the key table (`FindOrCreateDecideKey`, `logical_decide.cpp`), which
  holds one key text. Expected: each clause echoed as written.
- **A column reference is named in two forms:** `KeyElements` (`decide_binder.cpp`) names a key column's
  reference by its SQL spelling (`t.depot`, `t."my col"`, `D."cost"`); every other reference
  carries its bare column name. Anything that prints a reference's name has to tell the two
  apart. The unbounded remedy (`plan_decide.cpp`, the name harvest) does it by comparing the
  name with the binder's source name: equal is bare and is quoted where SQL needs it,
  different is taken as SQL. A reference whose name differs from the source only in case
  is taken as SQL unquoted. Expected: one form on every reference (the bare name, with the
  key's SQL spelling kept beside it, as `DecideKeyElement::name` already keeps it), so no
  printer infers which it holds. Same field as the shared key text above.
- **Impact:** messages only.

## Diagnosis text

Each reproduces on the current build.

- **A `BETWEEN` echoes its lower half only:** `x BETWEEN 18 AND 25` beside `SUM(x) <= 20`
  over two rows reports the clause `x >= 18` and suggests `x >= 10`; pasted back, the upper
  half is lost. The binder splits a BETWEEN into two comparisons that share the clause.
- **A quadratic clause is reported once per row or group in the query scope:** quadratic
  rows never share a slack (I2.d), so `power(x - 20, 2) <= 4` beside `x >= 25` over two rows
  gives two identical query-scope findings with no row to tell them apart; the expanded
  scope names each class (D28).
- **HiGHS's expanded repair can carry a 1e-5 cushion:** `x <> -1 AND ... AND per (): sum(x)
  + max(x) <= -5 AND per row: x >= 18` suggests `per (): sum(x) + max(x) <= 1e-05`
  (amount 5.00001) in the expanded scope where Gurobi and the query scope say `<= 0`; the
  edit over-repairs by 1e-5.
- **Impact:** a suggested change that does not repair the query, or findings the user
  cannot tell apart.

## Limitations (not bugs)

- **A decision as a factor on a reducer is refused:** `b * sum(x) <= 5` with
  `per (): b(BOOL)` (or an INT `s`, in a constraint or in the objective) answers *"'b' is
  a decision, so it cannot multiply SUM(x). Only data can scale
  SUM/AVG/MIN/MAX."* (`decide_linear_form.cpp`); the product written inside the reducer,
  `sum(b * x)`, is a bilinear term and answers. `../language/guide.md` §5 states the
  refusal. **Ruling (owner, 2026-10-27).** Lift it for a `BOOL` factor only: one McCormick helper over
  the reducer's value, as the bilinear rules of §4 linearize any `BOOL` product. An `INT`
  or `REAL` factor stays refused; its product is not convex.
  A lift changes `test_canonicalize_scale.py::test_decision_factor_rejected` and
  `::test_decision_factor_in_objective_rejected`, and §5.
- **A quadratic row refuses a row-varying factor on a query-wide decision that the linear
  row accepts:** with `s` declared `per ()` and `gcap` varying by row,
  `sum(x) + gcap * s <= 30` is generated per row and answers, while
  `sum(power(x, 2)) + gcap * s <= 30` answers *"'sum(power(x, 2))' is not supported yet in
  this constraint: ... with nothing beside them but data and query-wide decisions, each on
  its own"* (`decide_rewrite_scoped.cpp`); `sum(power(x, 2)) + s <= 30` answers. No test
  covers this shape. **Ruling (owner, 2026-10-27).** Lift it: the linear row accepts the same factor,
  so the refusal is an inconsistency, not a policy.

Resolved behavior is documented by its owning `done.md`.
