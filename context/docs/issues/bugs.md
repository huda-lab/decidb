# Known bugs

## Open

None.

## Limitations (not bugs)

- **Gurobi's non-convex tolerance.** On `SUM(POWER(x, 2)) BY (k) <= 25 MAXIMIZE SUM(POWER(x, 2))`, with optimum 50,
  Gurobi returns 50.0000001 and reports it optimal at tolerance 0. Inside Gurobi's spatial branching.
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
- **Gurobi at an objective constant of about 1e200 and above.** `SELECT id, x FROM (VALUES (1, 1e100)) t(id, v) DECIDE
  x(REAL) SUCH THAT x BETWEEN 0 AND 2e100 MINIMIZE SUM(POWER(x - v, 2))` prints the number-range warning, then
  `DECIDE optimization is unbounded`, on a model whose every decision is bounded. Gurobi returns an unbounded status
  for the expanded square's constant `v^2`; HiGHS refuses to load it ("numbers too far apart in size"). Inside the
  backends; direct solve answers `x = v`.

Resolved behavior is documented by its owning `done.md`.
