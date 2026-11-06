# Known bugs

## Open

- **BUG-46 — A data weight on a square gets a message that says its degree is above 2.** `SUM(w * POWER(x, 2)) <= 25`
  is refused with `DECIDE expression contains an unsupported product factor that still references decision variables
  after normalization (total degree > 2 or unexpanded nonlinear product). ...` (`decide_term_split.cpp`, the
  "unsupported product factor" message). The expression has degree 2, and the text does not say that a constant factor
  (`SUM(2 * POWER(x, 2))`) or `POWER(sqrt(w) * x, 2)` is accepted. Repro: that clause over `t(id, w DOUBLE, s DOUBLE)`
  with `DECIDE x(REAL) ... MAXIMIZE SUM(s * x)`. Found 2026-11-06 (N1 review). Fix: `pipeline/04_canonicalizer/todo.md`
  § CAN-1.

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

Resolved behavior is documented by its owning `done.md`.
