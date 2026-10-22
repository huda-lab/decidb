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

## Direct solve fails to bind any source column a parent reads when the source ends in `ORDER BY`

- **Location:** not traced; the direct-solve plan or its unused-column pruning
  (`src/optimizer/decide/direct/`). The error is raised by `ColumnBindingResolver`.
- **Observed:** with a source subquery that ends in `ORDER BY` and a parent that reads any source column (one the rule
  uses, such as `score`, as well as one it does not), direct solve raises an internal error, for S1 and A1 alike and
  under the default `auto`:
  `SELECT SUM(length(payload)) FROM (SELECT * FROM s ORDER BY id DESC) t DECIDE x(BOOL) SUCH THAT SUM(x) <= 3 MAXIMIZE SUM(score * x)`
  gives `INTERNAL Error: Failed to bind column reference "payload"`. The same query with `decide_direct_solve = 'off'`
  answers, and so does the direct path when the parent reads only `x` or the source has no `ORDER BY`. The parent asks
  for the subquery's output bindings (`[19.0]`) while the plan offers only the table's (`#[1.0]`, `#[1.1]`, ...): the
  projection above the `ORDER BY` appears to be skipped without remapping the parent's references (not traced). Late
  materialization of a stored table does not apply to this source (an `ORDER BY` is neither a projection nor a filter), and the error
  also occurs with that step disabled.
- **Expected:** the same answer as the solver path.
- **Impact:** a valid query fails with an internal error in the default mode instead of answering.

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

Resolved behavior is documented by its owning `done.md`.
