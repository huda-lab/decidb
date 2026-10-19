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

## A decision named like a USING column is silently ignored

- **Location:** the name-conflict check in `DecideDeclarationsBinder::BindDeclarations`
  uses `BindContext::GetMatchingBinding`, which skips USING columns.
- **Observed:** `SELECT * FROM (VALUES (1, 10), (2, 20)) A(k, v) JOIN (VALUES (1, 5),
  (2, 6)) B(k, w) USING (k) DECIDE k(INT) SUCH THAT k <= 3 MAXIMIZE SUM(k)` runs; the
  clause binds `k` to the join column, so the model has no row and every decision is 0.
- **Expected:** the conflict error a decision named like any other column gets.
- **Impact:** a silently wrong answer.

## A negative lower bound under a `when` leaves the decision non-negative

- **Location:** bound absorption does not enter a `when` wrapper
  (`AbsorbBoundsInExpression` in `decide_bound_absorption.cpp`), the `IN` rewrite emits its
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

## Any DECIDE query fails with the DECIDE optimizer switched off

- **Location:** `DecideGlobalSinkState`'s constructor (`physical_decide.cpp`, around line
  1570) indexes the decision column box `absorbed_lower_bounds`, which only stage 5's
  `DecideOptimizer::AbsorbVariableBounds` sizes; `05_optimizer/done.md` and
  `plan_decide.cpp` say the path without stage 5 works.
- **Observed:** `SET disabled_optimizers TO 'decide_optimizer'` (or `PRAGMA
  disable_optimizer`, or the unoptimized run of `PRAGMA enable_verification`), then any
  DECIDE query: *"INTERNAL Error: Attempted to access index 0 within vector of size
  0"*, which also invalidates the database.
- **Expected:** the query runs, or a message in SQL terms says the optimizer is needed.
- **Impact:** an internal error on a documented path.

## A DECIDE subquery that reads the outer row fails internally

- **Location:** decorrelation of a dependent join holding a DECIDE
  (`flatten_dependent_join.cpp`), and `ColumnBindingResolver` when the outer column is
  used only in `SUCH THAT`.
- **Observed:** `SELECT i, (SELECT MAX(x) FROM range(2) s(j) DECIDE x(INT) SUCH THAT
  x <= t.i MAXIMIZE SUM(x)) AS m FROM range(3) t(i)` gives *"INTERNAL Error: Failed to
  bind column reference "i""*, and so does a LATERAL subquery whose clause reads `t.i`; a
  reference in the subquery's WHERE gives *"INTERNAL Error: Logical operator type
  "DECIDE" for dependent join"*.
- **Expected:** a bind-time "not supported yet" naming the outer reference, until a
  correlated DECIDE subquery is supported.
- **Impact:** an internal error instead of a message.

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

## A scaled `norm(e, 0)` leaves a constraint the debug verifier calls non-canonical

- **Location:** `RewriteNorm` in `src/optimizer/decide/decide_rewrite_norm_in.cpp` lowers
  the marker in place (`2 * norm(e, 0, M)` becomes `2 * SUM(z)`), so the comparison is not
  canonicalized again, and `VerifyCanonical` at `PhysicalPlanGenerator::CreatePlan`
  finds that it would be.
- **Observed:** on a debug build, `test/decide/tests/test_norm.py::test_norm_scaled_l0_constraint`
  (`2 * norm(new_qty - l_quantity, 0, 1000) <= 6`) raises *"INTERNAL Error: DECIDE
  canonical invariant C4/C6/C7 violated: canonicalizing the tree again changes its
  structure, reducer scale, or provenance tags"*. A release build runs it and answers
  correctly.
- **Expected:** a lowered clause re-enters through `LogicalDecide::AddConstraint` (or is
  canonical as emitted), so the debug check holds.
- **Impact:** debug builds only; release skips the check.

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
- **DIAGNOSE names a NULL slice `g = 'NULL'`:** its suggested change
  `when g = 'NULL': x <= <cap>` matches no row; owned by
  `../../07_query_diagnostics/unbounded/todo.md` §1.
- **Expected:** a refusal or an answer in SQL terms; `g IS NULL` for a NULL slice.
- **Impact:** an internal error, and a suggested change that matches no row.

## Wrong answers or refusals: `IN`, bounds and rounding

Each reproduces on the current build.

- **A negative `IN` value with a negative upper bound is refused:** `x IN (-5, 0, 5) AND
  x <= -3` answers *"x <= -3 cannot hold because x is non-negative"*; the answer is
  `x = -5`. Location: the domain check in `DecideGlobalSinkState`'s constructor
  (`physical_decide.cpp`) rebuilds the box without the removable `IN` floor.
- **Gurobi calls an infinite floor on a decision the objective does not read unbounded:**
  `y >= 'inf'::DOUBLE` on such a decision -- per row, keyed (`per t.depot: y >=
  'inf'::DOUBLE` on `per t.depot: y`) or query-wide -- answers *"DECIDE optimization is
  unbounded"*, and DIAGNOSE suggests a cap (`y <= <cap>`, a runaway) instead of naming the
  bound no value reaches. On a decision the objective reads, `x >= 'inf'::DOUBLE` is
  reported infeasible; HiGHS refuses the bound in SQL terms.
- **Impact:** a refused feasible query, and a misleading verdict.

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
  sit. Inside a reducer, `sum(x * CAST(cap > 2 AS INT))` answers *"Unsupported expression
  of type ExpressionClass::COMPARISON inside DECIDE SUM expression"*; `CAST(cap IN (3, 4)
  AS INT)` is accepted. Expected: the comparison read as a 0/1 data value, or a refusal in
  SQL terms.
- **An invalid coefficient names a row by an internal position:** `x / w <= 1` with a
  `w = 0` row answers *"... contains invalid value (NaN or Infinity) at row 1."*; the number
  is the row's 0-based place in the materialized input (`physical_decide.cpp:410`, the
  count of values read so far), which a parallel scan fills in a different order each run.
  Expected: the row named by its values, or no number.
- **A reducer over a constant is called a query-wide decision:** `sum(1) <= 5` (or
  `sum(per t.g: 1) <= 5`) answers *"1 is a query-wide decision, so SUM(1) has nothing to
  aggregate over; use 1 on its own"*: `ClassifyReducerCall` (`decide_binder.cpp`) tests the
  body for row invariance, not for a decision. `sum(cap) <= 5` gets *"SUM expression must
  reference at least one DECIDE variable"*. Expected: that message for any decision-free
  body.
- **Two keys over the same columns keep the key text of the one bound first**
  (cosmetic): with `per D.d, D.cost: open(BOOL)` declared, `SUM(D: D.cost * open) <= 7`
  is echoed by EXPLAIN as `sum(D.d, D."cost": "cost" * open) <= 7`, and a determination
  message quotes `(in sum(D.d, D."cost": ...))`; declared `per D: open(BOOL)`, the same
  clause echoes `sum(D: "cost" * open) <= 7`. Both keys name the same columns, so they
  share one exact scope (`FindOrCreateExactScope`, `decide_binder.cpp`), which holds one
  key text. Expected: each clause echoed as written.
- **Impact:** messages, and one refused query the language could answer (the comparison
  inside an operand).

## Messages: nested DECIDE and refusals

Each reproduces on the current build.

- **A refusal inside a nested DECIDE is masked:** `x <= (SELECT max(y) FROM (SELECT y
  FROM t DECIDE y(INT) SUCH THAT y <= cap OR y <= 1 MAXIMIZE SUM(y)))` answers
  *"Referenced column "y" not found in FROM clause!"*, not the inner refusal; the same for
  any binder refusal of the inner clause.
- **Refusals name engine enums:** `NOT (x <= 1 OR x >= 3)`, `x NOT IN (1, 2)` and `x NOT
  BETWEEN 1 AND 2` answer *"SUCH THAT constraint clause does not support '...'
  (ExpressionType::OPERATOR_NOT)"* (`COMPARE_NOT_IN`), and `MAXIMIZE 3` answers
  *"[MAXIMIZE|MINIMIZE] clause does not support '3'(ExpressionClass::CONSTANT)"*.
  Location: the default arms of `DecideConstraintsBinder::BindOperator` and of
  `decide_objective_binder.cpp`.
- **The objective quotes a bound tree:** `MAXIMIZE (SUM(x) + 3) / 2` answers *"contains a
  non-aggregate term: "/"(CAST((sum(x) + CAST(3 AS HUGEINT)) AS DOUBLE), ...)"*, a
  decision-free `count(*)` term is quoted `'COUNT_STAR(...)'` (`UserFacingName`,
  `decide_canonicalizer.cpp`), and `MAXIMIZE count(*)` names the function `'count_star'`.
- **Impact:** messages only.

## Diagnosis text

Each reproduces on the current build.

- **A quadratic bound is echoed with the constant canonicalization moved:** `x >= 12 AND
  SUM(POWER(x - 5, 2)) <= 3` over three rows (also `per (): norm(x - 5, 2) <= 3`) echoes
  `sum(power(x - 5, 2)) <= -72` and suggests `<= -63`, a bound no assignment meets.
  Location: `SourceAwareQuadraticLhs` (`decide_diagnostic_render.cpp`) pairs the written
  body with the row's moved bound.
- **An amount can round down and under-repair:** `SnapDiagnosticValue` rounds to the
  nearest integer within 1e-6 relative, so over one row `x(REAL)`, `x <= 1000000.16 AND
  SUM(x) >= 2000000.5` suggests `x <= 2000000.16` (amount 1000000, the gap is 1000000.34)
  and the pasted query is still infeasible. Expected: round toward the repair.
- **An unreachable upper bound at minus infinity says "lower this bound":** `MIN(x) <=
  -1e1000::DOUBLE` answers *"lower this bound — no assignment can reach it"*; it has to be
  raised.
- **A `BETWEEN` echoes its lower half only:** `x BETWEEN 18 AND 25` beside `SUM(x) <= 20`
  over two rows reports the clause `x >= 18` and suggests `x >= 10`; pasted back, the upper
  half is lost. The binder splits a BETWEEN into two comparisons that share the clause.
- **The unbounded remedy names a plain column unquoted:** the cap names a column as the
  clause's own reference does -- a `per`/`by` key column as SQL already (`t.depot`), any
  other by its bare name -- so a column whose name needs quotes (`"my col"`) prints as
  `when my col = 'v': x <= <cap>`. The harvested names (`plan_decide.cpp`) mix the two.
- **A quadratic clause is reported once per row or group in the query scope:** quadratic
  rows never share a slack (I2.d), so `power(x - 20, 2) <= 4` beside `x >= 25` over two rows
  gives two identical query-scope findings with no row to tell them apart; the expanded
  scope names each class (D28).
- **HiGHS's expanded repair can carry a 1e-5 cushion:** `x <> -1 AND ... AND per (): sum(x)
  + max(x) <= -5 AND per row: x >= 18` suggests `per (): sum(x) + max(x) <= 1e-05`
  (amount 5.00001) in the expanded scope where Gurobi and the query scope say `<= 0`; the
  edit over-repairs by 1e-5 (the rounding item above).
- **Impact:** a suggested change that does not repair the query, or names the wrong
  direction.

## A decision named in WHERE filters on an input column

- **Location:** WHERE binding sees the DECIDE binding that `BindDeclarations` adds
  (`src/planner/binder/query_node/bind_select_node.cpp`); the filter is planned below the
  DECIDE operator, where the decision's column index reads an input column.
- **Observed:** `SELECT id, cap FROM (VALUES (1,3,'A'),(2,5,'A'),(3,7,'B')) t(id,cap,g)
  WHERE x > 1 DECIDE x(INT) SUCH THAT x <= cap MAXIMIZE SUM(x)` returns ids 2 and 3:
  EXPLAIN shows `FILTER (col0 > 2)` for `WHERE x > 2`, a filter on `id`. With two
  decisions, the second reads the second input column.
- **Expected:** a refusal -- a decision has no value before the solve, so WHERE takes
  data only (*"DECIDE: x is a decision; WHERE may only use data"*).
- **Impact:** a silent wrong answer. Older than the scoping language.

## Parameters of a prepared statement or a table macro fail in a DECIDE clause

- **Observed:** `PREPARE p AS SELECT id, x FROM t DECIDE x(INT) SUCH THAT x <= $1 AND
  x <= cap MAXIMIZE sum(x)` answers *"SUCH THAT clause does not support
  '$1'(ExpressionClass::PARAMETER)"*; `sum(x) <= $1` answers *"SUM cannot be compared to
  an expression that is not a scalar or aggregate without DECIDE variables"*. A table
  macro's parameter in a clause answers *"Referenced column "lim" not found in FROM
  clause!"*.
- **Expected:** a parameter is data, bound like a constant.
- **Impact:** a prepared or macro DECIDE query cannot take a value; the message names an
  internal enum.

## A scaled `AVG` leaves a constraint the debug verifier calls non-canonical

- **Location:** `RewriteAvgInExpression` (`src/optimizer/decide/decide_rewrite_notequal_avg.cpp`)
  swaps AVG for SUM in place, under the factor the canonicalizer peeled, without going
  back through `LogicalDecide::AddConstraint`; the same class as the scaled `norm(e, 0)`
  entry above.
- **Observed:** on a debug build, `2 * avg(x) >= 4` (also `2 * avg(x) = 4`,
  `2 * avg(x) <> 4`, `-avg(x) <> -2`, `per (): 2 * avg(x) >= 4`) raises *"INTERNAL Error:
  DECIDE canonical invariant C4/C6/C7 violated"*. `avg(2 * x) <> 4` and `avg(x) <> 2`
  pass. A release build answers correctly.
- **Expected:** the lowered clause is canonical, or re-enters through `AddConstraint`.
- **Impact:** debug builds only; no test covers a scaled `AVG` on the decision side.

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
