# Known bugs

## Open

- **BUG-46 — A data weight on a square gets a message that says its degree is above 2.** `SUM(w * POWER(x, 2)) <= 25`
  is refused with `DECIDE expression contains an unsupported product factor that still references decision variables
  after normalization (total degree > 2 or unexpanded nonlinear product). ...` (`decide_term_split.cpp`, the
  "unsupported product factor" message). The expression has degree 2, and the text does not say that a constant factor
  (`SUM(2 * POWER(x, 2))`) or `POWER(sqrt(w) * x, 2)` is accepted. Repro: that clause over `t(id, w DOUBLE, s DOUBLE)`
  with `DECIDE x(REAL) ... MAXIMIZE SUM(s * x)`. Found 2026-11-06 (N1 review). Fix: `pipeline/04_canonicalizer/todo.md`
  § CAN-1.

- **BUG-48 — DIAGNOSE's `group` column writes a NULL key as the string `'NULL'`.** The finding's `group` reads
  `k = 'NULL'` while its `suggested_change` correctly says `when k IS NULL: ...`. Repro, with
  `DECIDB_FORCE_SOLVER=gurobi`: `DIAGNOSE` of a linear model with `when k IS NOT NULL: x <= 5`, `MAXIMIZE SUM(x)`, over
  rows `(1, 1.0), (1, 2.0), (NULL, 3.0)`. Found 2026-11-06 (UNB-1 lane).

- **BUG-49 — DIAGNOSE of a non-convex unbounded model answers `feasible`.** `DIAGNOSE` of
  `SUM(POWER(x, 2)) BY (k) <= 25 AND x >= 0 MAXIMIZE SUM(POWER(x, 2))`, over rows `(k, s)` where one `k` is NULL (that
  row's `x` has no upper limit), returns `feasible` and prints a "could not prove it is the best" line on stderr. Not
  yet investigated: it may be how Gurobi reports a non-convex model, or how the router reads it. Found 2026-11-06
  (UNB-1 lane).
  - **Cause.** Gurobi answers `INF_OR_UNBD`. The adapter's `DualReductions=0` retry (`gurobi_solver.cpp`) sends a
    non-convex model to spatial branching, which ends `SUBOPTIMAL` with incumbents near 1e19. The router reads
    `SUBOPTIMAL` as solved. A plain solve returns that value too.
  - **Ruling (owner, 2026-11-06).** On a non-convex model the adapter skips the retry and returns `INF_OR_UNBD`. The
    facade's zero-objective probe settles it.

## Limitations (not bugs)

- **A repair exactly at the tolerance on HiGHS's QP path.** An edit whose rows miss a summed bound by exactly the
  feasibility tolerance (1e-6) passes the certificate's LP on both backends, and Gurobi solves the edited query; HiGHS's
  QP path rejects it. HiGHS judges the same violation differently in its LP and QP paths.
- **Gurobi's barrier QP at stage 2.** On a 9,000-row QP repair Gurobi returns a point that breaks a row by 5.7e-5 and an
  objective value 24 above the optimum HiGHS finds (225000010024 against 225000010000). Inside Gurobi.
- **HiGHS on the edited 9,000-row QP.** It reaches its 300 s limit where Gurobi solves it.
- **Gurobi's barrier on many limits on squares.** With a redundant bound on the decision, a feasible, well-scaled query
  with one `SUM(POWER(x, 2))` limit per group can stop with Gurobi's numerical-trouble status and no solution, at
  Gurobi's default settings. With `x <= 50` and a limit of 25, 7 of 20 random inputs fail on 2,000 rows in 100 groups
  and 9 of 20 on 20,000 rows in 1,000 groups. No Gurobi setting tried recovers them. The query raises `DECIDE
  optimization failed: Gurobi could not solve this model (numerical trouble).` Inside Gurobi.

Resolved behavior is documented by its owning `done.md`.
