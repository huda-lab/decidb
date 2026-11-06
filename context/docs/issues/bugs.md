# Known bugs

## Open

- **BUG-50 — A COALESCE over a decision is refused without a caret.** `SUM(COALESCE(x, 0)) <= 5` is refused by the
  shape check (`DECIDE: operator 'COALESCE' cannot take a DECIDE variable. In the SUCH THAT clause ...`), and the
  message underlines no term. The transformer gives a parsed COALESCE no source location (`transform_coalesce.cpp` and
  the two-argument form in `transform_function.cpp` build the operator without one), so the bound node has none for
  the refusal to point at, and the gap spans several transformer sites. Every other shape-check refusal carries a
  caret. Repro: that clause over `t(id)` with
  `DECIDE x(REAL) ... MAXIMIZE SUM(x)`. Found 2026-11-06 (CAN-1). Fix: set the location on each parsed special form in
  the transformer.

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
