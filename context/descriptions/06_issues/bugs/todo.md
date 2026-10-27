# Known Bugs — Open

## `DECIDB_TIME_LIMIT` is not enforced on HiGHS for large models

- **Location:** `src/decidb/naive/deterministic_naive.cpp` (`HighsSession`);
  contrast `src/decidb/gurobi/gurobi_solver.cpp:125`.
- **Observed:** on Q2 at the large TPC-H tier (~1.5M variables, mixed-integer),
  `highs.optimize` began and never returned — not at the requested 300s
  `DECIDB_TIME_LIMIT`, not by 420s wall clock, when an external harness killed
  the process. Every DeciDB-owned span (parse, rewrite, extraction, model
  build, `highs.load`) completed in under 350ms total; the entire remainder
  was spent inside HiGHS's own `run()`. Seen on the large benchmark tier (query Q2 on
  HiGHS, status `process_timeout`); see `05_performance/06_solver_execution/`.
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

## A DECIDE that refers to an outer query raises an internal error

- **Location:** not traced; DuckDB's subquery decorrelation (`FlattenDependentJoins`), which has no case for the
  `DECIDE` operator.
- **Observed:** with `g(k)` and `t(id, k, v)`,
  `SELECT g.k, q.id, q.x FROM g, LATERAL (FROM t WHERE t.k = g.k DECIDE x(BOOL) SUCH THAT SUM(x) <= 1 MAXIMIZE
  SUM(v*x)) q` raises `INTERNAL Error: Logical operator type "DECIDE" for dependent join`, with `decide_direct_solve`
  `off` and `auto` alike, and with a `PER` clause added. When only the right side refers to the outer query,
  `... LATERAL (FROM t DECIDE x(REAL) SUCH THAT x BETWEEN 0 AND 1 AND SUM(x*w) <= SUM(cap) + g.k MAXIMIZE SUM(w*x)) q`
  raises `INTERNAL Error: Failed to bind column reference "k"`. A scalar subquery whose clause reads the outer row fails the same way:
  `SELECT i, (SELECT MAX(x) FROM range(2) s(j) DECIDE x(INT) SUCH THAT x <= t.i MAXIMIZE SUM(x)) AS m FROM range(3)
  t(i)` raises `INTERNAL Error: Failed to bind column reference "i"` (`ColumnBindingResolver`, when the outer column
  is used only in `SUCH THAT`).
- **Expected:** a correlated DECIDE either runs once per outer row or is rejected at bind time with an ordinary error
  that says a DECIDE cannot refer to an outer query.
- **Impact:** a user who writes one problem per group with `LATERAL` gets an internal error instead of a pointer to
  `PER`.

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
  skip was added. On `medium`, Q10 on HiGHS peaks at 6,525 MiB at 2.5M variables (`05_performance/08_memory_and_scaling/done.md`).
- **Expected:** the query finishes, or fails with an ordinary out-of-memory error; the profiler records a memory kill
  as a memory failure, not a timeout.
- **Impact:** a large HiGHS query can take the whole process down, and the profile misreports why.
- **Reproduce:** `make decide-bench-setup`, remove the entry from `SKIP`, run `profile_pipeline.py`. Memory-heavy.

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

## A negative lower bound under a `when` leaves the decision non-negative

- **Location:** singleton-row folding does not enter a `when` wrapper
  (`FoldSingletonRowsInExpression` in `decide_singleton_row_folding.cpp`), the `IN` rewrite emits its
  negative floor under the same `when` (`RewriteInDomain` in `decide_rewrite_norm_in.cpp`),
  and `PhysicalDecide::Finalize` resolves a floor the query never lowered to 0.
- **Observed:** over `(1, true), (2, false) t(i, p)`, `DECIDE r(INT) SUCH THAT when p: r IN
  (-5, 7, 12) AND r <= 20 MINIMIZE SUM(r)` returns r = 7 on the row where `p` holds; -5 is
  optimal. A query-wide `per (): r` and a keyed `per g: r` return 7 too. Over `(1, 'A'),
  (2, 'B') t(id, g)`, `when g = 'A': x >= -5 AND x <= 10 MINIMIZE SUM(x)` returns 0 on row
  A. Written without the `when`, either bound makes the decision signed;
  `syntax_reference.md` §2 states the `when` case as open.
- **Expected:** -5 on the rows the `when` keeps. Open: does a negative floor under `when`
  make the decision signed on every row (for a keyed decision, its whole class), or only on
  the rows it selects?
- **Impact:** a wrong optimum, silently.

## Key grouping ignores collation

- **Location:** `BuildGroupIds` in `src/execution/operator/decide/physical_decide.cpp`,
  shared by a keyed declaration's classes, a constraint's `per` key and a reducer's `by`
  key.
- **Observed:** over a `VARCHAR COLLATE NOCASE` column holding 'abc', 'ABC', 'Abc' and
  'xyz', `GROUP BY c` sees 2 groups while `DECIDE per C.c: x(INT)` builds 4 decisions, and
  `per c: sum(x) by (c) <= 1` lets each of the three spellings of 'abc' reach 1.
- **Expected:** open — should key equality follow the column's collation, as GROUP BY
  does? Every key compares plain values.
- **Impact:** more classes than SQL grouping would give on collated text keys.

## Messages print a subquery, a condition or a `by` key as DuckDB renders it

- **Location:** a subquery's source fragment, and a `by` key's text, is its parsed
  expression's `ToString` (`TagDecideSourceFragments` in `decide_source_provenance.cpp`,
  `BindReducerBy`), and a condition is re-rendered from its bound form
  (`RenderDecideSource`).
- **Observed:** `when v in (SELECT 1000): x <= 1` gives *"DECIDE: when (v = ANY(SELECT
  1000)) matches no rows."*; `when exists (SELECT 1 WHERE false): ...` gives
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

## Coefficients and keys

Each reproduces on the current build.

- **A lambda in a coefficient:** `SUM(x * list_reduce([cap, b], (a, z) -> a + z)) <= 30`
  raises *"INTERNAL Error: lambda reference expressions are transient, ToString should
  never be called"* (also `when c: sum(...)`, a per-row bound, the objective);
  `SUM(x * len(list_filter([cap, b], q -> q > 1))) <= 12` answers *"Column 'q' must be one
  of the DECIDE variables"*.
- **Expected:** a refusal or an answer in SQL terms.
- **Impact:** an internal error.

## Wrong answers: infinite bounds

Reproduces on the current build.

- **Gurobi calls an infinite floor on a decision the objective does not read unbounded:**
  `y >= 'inf'::DOUBLE` on such a decision -- per row, keyed (`per t.depot: y >=
  'inf'::DOUBLE` on `per t.depot: y`) or query-wide -- answers *"DECIDE optimization is
  unbounded"*, and DIAGNOSE suggests a cap (`y <= <cap>`, a runaway) instead of naming the
  bound no value reaches. On a decision the objective reads, `x >= 'inf'::DOUBLE` is
  reported infeasible; HiGHS refuses the bound in SQL terms. With no upper bound on the decision, Gurobi answers
  `x >= 'inf'::DOUBLE MINIMIZE SUM(x)` with `x = 1e+100` for a REAL `x` (the row's `+inf` meets the open column at
  Gurobi's infinity, 1e100) and fails with *"DECIDE optimization failed (solver status 6)"* for an INT `x`
  (found 2026-10-27; direct solve reports it infeasible).
- **Impact:** a misleading verdict, and an answer to a bound no value meets.

## Wrong answers: MIN/MAX bodies and solver tolerance

Each reproduces on the current build.

- **A MIN/MAX over a squared or multiplied body is modelled wrongly** (Gurobi; the user
  chose to log it, 2026-10-18): over two INT rows with `x <= 7`, `max(power(x, 2)) >= 49`
  is reported infeasible (x = 7 holds it); `min(power(x, 2)) <= 4` answers x = (7, 7) and
  `sum(x) + max(power(x, 2)) <= 20` answers (6, 7), both violating the clause (the latter
  on HiGHS too). In the objective, `MAXIMIZE max(power(x, 2))` and `sum(per g: max(power(x,
  2)) by (g))` are solved as the sum of the squares: no auxiliary, no indicator, only the
  Q matrix. Only the easy direction (`max(power(x, 2)) <= 16`) is right, in every scope.
  `test_per_objective.py::test_maximize_sum_max_power_per` and
  `test_minimize_sum_min_power_per` pass only because their rows are independent.
  Expected: an exact formulation (an epigraph for the convex side, indicators for the
  hard side) or a refusal.
- **Gurobi accepts a row violated by 1 when its coefficients are near 10⁹:** over three
  rows with `x <= cap` (3, 5, 7), `y <= 5`, `per row: y + sum(x * 1000000000::DECIMAL(18,0))
  >= 5000000001` (or `> 5000000000`), `MINIMIZE SUM(x) + SUM(y)` answers `sum(x) = 5`,
  `y = 0` on Gurobi: `5000000000 >= 5000000001` fails. HiGHS answers `sum(x) = 6`. Presolve
  folds the helper's definition into the row and scales it, so the violation falls under
  the feasibility tolerance. Expected: an answer that holds, or a warning that the
  coefficients exceed what the solver resolves.
- **Impact:** a wrong optimum, or a clause violated in the answer.

## Messages: operands, coefficients and keys

Each reproduces on the current build.

- **A data comparison inside an operand is bound as a constraint:** `x <= CAST((cap > 2
  AND cap < 6) AS INT)` (or `CAST(cap > 2 AS INT)`) answers *"SUCH THAT clause: Column
  'cap' must be one of the DECIDE variables"*: `DecideConstraintsBinder` binds a
  conjunction's children (`BindConjunction`) and a comparison as constraints wherever they
  sit. Inside a reducer, `sum(x * CAST(cap > 2 AS INT))` answers *"SUM expression does not
  support '(cap > 2)'"*; `CAST(cap IN (3, 4) AS INT)` is accepted. Expected: the comparison
  read as a 0/1 data value.
- **Two keys over the same columns keep the key text of the one bound first**
  (cosmetic): with `per D.d, D.cost: open(BOOL)` declared, `SUM(D: D.cost * open) <= 7`
  is echoed by EXPLAIN as `sum(D.d, D."cost": "cost" * open) <= 7`, and a determination
  message quotes `(in sum(D.d, D."cost": ...))`; declared `per D: open(BOOL)`, the same
  clause echoes `sum(D: "cost" * open) <= 7`. Both keys name the same columns, so they
  share one exact scope (`FindOrCreateExactScope`, `decide_binder.cpp`), which holds one
  key text. Expected: each clause echoed as written.
- **A column reference is named in two forms:** `BindKeyColumns` names a key column's
  reference by its SQL spelling (`t.depot`, `t."my col"`, `D."cost"`); every other reference
  carries its bare column name. Anything that prints a reference's name has to tell the two
  apart. The unbounded remedy (`plan_decide.cpp`, the name harvest) does it by comparing the
  name with the binder's source name: equal is bare and is quoted where SQL needs it,
  different is taken as SQL. A reference whose name differs from the source only in case
  is taken as SQL unquoted. Expected: one form on every reference (the bare name, with the
  key's SQL spelling kept beside it, as `group_keys[].names` already keeps it), so no
  printer infers which it holds. Same field as the shared key text above.
- **Impact:** messages, and one refused query the language could answer (the comparison
  inside an operand).

## Messages: nested DECIDE and refusals

Each reproduces on the current build.

- **A refusal inside a nested DECIDE is masked:** `x <= (SELECT max(y) FROM (SELECT y
  FROM t DECIDE y(INT) SUCH THAT y <= cap OR y <= 1 MAXIMIZE SUM(y)))` answers
  *"Referenced column "y" not found in FROM clause!"*, not the inner refusal; the same for
  any binder refusal of the inner clause.
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

## Parameters of a prepared statement or a table macro fail in a DECIDE clause

- **Observed:** `PREPARE p AS SELECT id, x FROM t DECIDE x(INT) SUCH THAT x <= $1 AND
  x <= cap MAXIMIZE sum(x)` answers *"SUCH THAT does not support '$1'"*; `sum(x) <= $1` answers *"SUM cannot be compared to
  an expression that is not a scalar or aggregate without DECIDE variables"*. A table
  macro's parameter in a clause answers *"Referenced column "lim" not found in FROM
  clause!"*.
- **Expected:** a parameter is data, bound like a constant.
- **Impact:** a prepared or macro DECIDE query cannot take a value.

## Direct solve reads an integer bound just below a whole number more strictly than both backends

- **Location:** `DirectClauseBounds` and the box rounding in `DirectReadBounds`
  (`src/optimizer/decide/direct/direct_bound_reader.cpp`); the solver path writes the bound into the column box
  unrounded (`TryFoldPerRowBound`, `FoldSingletonRows`) and leaves the integer reading to the backend.
- **Observed:** `FROM (VALUES (1, 4.3)) t(id, c) DECIDE x(INT) SUCH THAT x <= c/0.1 AND x >= 0 MAXIMIZE SUM(x)`
  answers 43 on Gurobi and HiGHS and 42 on direct solve (A1). `c/0.1` is 42.99999999999999 in a double. A literal
  `x <= 42.9999999` gives the same split: both backends round the bound up within their integrality tolerance,
  direct solve takes the floor. `0.1*x <= c` agrees on 43 (direct checks the row `0.1 * 43 <= 4.3` in doubles).
- **Expected:** one rule for which integers a bound admits, used by the model builder, the folding pass and direct
  solve (`decide_numeric_policy.hpp`): the code-quality entry "A bound on one decision is classified three times".
- **Impact:** a whole-number answer can differ by one between `auto` and `off` for a bound within about 1e-6 below a
  whole number. The code predates the Sami-Active merge.

## Limitations (not bugs)

- **A decision as a factor on a reducer is refused:** `b * sum(x) <= 5` with
  `per (): b(BOOL)` (or an INT `s`, in a constraint or in the objective) answers *"'b' is
  a decision, so it cannot multiply SUM(x). Only data can scale
  SUM/AVG/MIN/MAX."* (`decide_linear_form.cpp`); the product written inside the reducer,
  `sum(b * x)`, is a bilinear term and answers. `syntax_reference.md` §5 states the
  refusal. Decision needed: lift it (for a `BOOL` factor, one McCormick helper over the
  reducer's value, as the bilinear rules of §4 linearize any `BOOL` product) or keep it.
  A lift changes `test_canonicalize_scale.py::test_decision_factor_rejected` and
  `::test_decision_factor_in_objective_rejected`, and §5.
- **A quadratic row refuses a row-varying factor on a query-wide decision that the linear
  row accepts:** with `s` declared `per ()` and `gcap` varying by row,
  `sum(x) + gcap * s <= 30` is generated per row and answers, while
  `sum(power(x, 2)) + gcap * s <= 30` answers *"'sum(power(x, 2))' is not supported yet in
  this constraint: ... with nothing beside them but data and query-wide decisions, each on
  its own"* (`decide_rewrite_scoped.cpp`); `sum(power(x, 2)) + s <= 30` answers. No test
  covers this shape.

Resolved behavior is documented by its owning `done.md`.
