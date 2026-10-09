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

## A query-wide decision in a quadratic constraint is counted once per row

- **Location:** `BuildQuadraticConstraint` in
  `src/decidb/formulation/ilp_model_builder.cpp`, which has no once-only arm for a
  query-wide (`scalar` / `per ()`) term, unlike the objective and the linear aggregate
  paths.
- **Observed:** `SELECT i, x, s FROM range(3) t(i) DECIDE x(REAL), scalar s(REAL)
  SUCH THAT SUM(POWER(x, 2)) + s <= 30 AND s >= 3 MAXIMIZE SUM(x)` dumps
  `qrow 0: sense=< rhs=30 | 3:3 | ...` -- `s` weighted 3, once per row -- and returns
  x = 2.6458 on each row.
- **Expected:** `s` counted once, so Σx² <= 27 and x = 3 on each row.
- **Impact:** a wrong optimum, silently.

## DIAGNOSE re-emits absorbed bounds on the wrong columns for a keyed decision

- **Location:** `EmitAbsorbedUserBoundRows` in
  `src/execution/operator/decide/physical_decide.cpp`: it loops over a decision's
  instances but looks each one up with `VarIndexer::Get(var, inst)`, which takes a row,
  instead of `InstanceColumn(var, inst)`.
- **Observed:** for a `T.x` or `per K:` decision, instance `i` is mapped through row
  `i`'s class, so some classes get the bound twice and others never (code reading; the
  map of 2026-10-09 also saw DIAGNOSE answers change with row order).
- **Expected:** every instance of the decision gets the absorbed bound once.
- **Impact:** DIAGNOSE may suggest edits for a relaxation that is not the one the query
  states.

## A decision named like a USING column is silently ignored

- **Location:** the name-conflict check in `DecideDeclarationsBinder::BindDeclarations`
  uses `BindContext::GetMatchingBinding`, which skips USING columns.
- **Observed:** `SELECT * FROM (VALUES (1, 10), (2, 20)) A(k, v) JOIN (VALUES (1, 5),
  (2, 6)) B(k, w) USING (k) DECIDE k(INT) SUCH THAT k <= 3 MAXIMIZE SUM(k)` runs; the
  clause binds `k` to the join column, so the model has no row and every decision is 0.
- **Expected:** the conflict error a decision named like any other column gets.
- **Impact:** a silently wrong answer.

## A generated column splits the entities of `T.x`

- **Location:** `FindOrCreateEntityScope` in
  `src/planner/expression_binder/decide/decide_binder.cpp` keys `T.x` on every name of
  the table binding, generated columns included. The storage-level cause of the split
  was not traced. `per T:` skips generated columns and is not affected.
- **Observed:** `CREATE TABLE G(a INTEGER, b INTEGER GENERATED ALWAYS AS (a * 2));
  INSERT INTO G (a) VALUES (1), (1), (2), (3);` then `DECIDE G.x(INT)` builds 4
  decisions for 3 distinct tuples.
- **Expected:** 3, one per distinct tuple.
- **Impact:** more decisions than entities, so rows of one entity can disagree.

## `SUM(T: ...)` drops a tuple once its data column leaves the key

- **Location:** the key trimming of `T.x` and `SUM(T: ...)` in
  `DecideDeclarationsBinder::BindDeclarations` (columns the clause reads as data leave
  the key), combined with the qualifier's keep-first-row de-duplication.
- **Observed:** over `(1, 10, 'a'), (1, 20, 'a'), (2, 30, 'b')` as `T(id, cost, tag)`,
  `DECIDE T.x(INT) SUCH THAT x <= 1 MAXIMIZE SUM(T: cost * x)` keys on `(id, tag)` and
  builds objective coefficients 10 and 30: the tuple with cost 20 is never counted.
- **Expected:** every distinct tuple of `T` counted once.
- **Impact:** a wrong objective, silently. `per` keys are not trimmed (item 2 of
  `09_anr_language/todo.md`); the trimming goes with `T.x` in item 7.

## A negative `IN` value under `WHEN` is never allowed

- **Location:** WHEN-guarded comparisons are not folded into a decision's bounds
  (`decide_bound_absorption.cpp`), the `IN` rewrite emits its negative floor under the
  same `WHEN` (`decide_rewrite_norm_in.cpp`), and `PhysicalDecide::Finalize` leaves the
  untouched floor at 0.
- **Observed:** `SELECT i, r FROM (VALUES (1, true), (2, false)) t(i, p) DECIDE r(INT)
  SUCH THAT r IN (-5, 7, 12) WHEN p AND r <= 20 MINIMIZE SUM(r)` returns r = 7 on the
  row where `p` holds; -5 is optimal. Every scope is affected, keyed decisions included.
- **Expected:** -5. Open: does a negative floor under `WHEN` make the decision signed on
  every row (for a keyed decision, its whole class), or only on the rows it selects?
- **Impact:** a wrong optimum, silently.

## Any DECIDE query fails with the DECIDE optimizer switched off

- **Location:** `physical_decide.cpp` (an index into an empty vector, around line 1275);
  `05_optimizer/done.md` and `plan_decide.cpp` say the path without stage 5 works.
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
  bind column reference "i""*; a LATERAL or WHERE reference fails in the decorrelator.
- **Expected:** a bind-time "not supported yet" naming the outer reference, until a
  correlated DECIDE subquery is supported.
- **Impact:** an internal error instead of a message.

## Key grouping ignores collation

- **Location:** `BuildGroupIds` in `src/execution/operator/decide/physical_decide.cpp`,
  shared by `per K:`, `T.x` and the old `PER`.
- **Observed:** over a `VARCHAR COLLATE NOCASE` column holding 'abc', 'ABC', 'Abc' and
  'xyz', `GROUP BY c` sees 2 groups while `DECIDE per C.c: x(INT)` builds 4 decisions.
- **Expected:** open — should key equality follow the column's collation, as GROUP BY
  does? To settle with the shared grouping cache of items 3–4 (`09_anr_language`).
- **Impact:** more classes than SQL grouping would give on collated text keys.

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
