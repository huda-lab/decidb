# Known bugs

## Open

- **BUG-45 — A data-derived bound multiplied by a decision is repaired as if it were a literal.** `DIAGNOSE` should relax
  a user's literal before a bound derived from data. It does so for `x <= cap`, but `x <= cap * o` is repaired ahead of
  the literal, whichever clause is written first. Canonicalization moves `cap * o` to the left side, which leaves a
  literal 0 as the right side, so the row looks like a literal and joins the literal tier
  (`diagnostics/decide_diagnostic_engines.cpp`, `is_data_offset`). The finding still reports `virtual_offset`, the kind
  for a data-backed bound. A bound with a column in it should repair in the data tier, with or without a decision.
  The paper's Section 6 states that rule. Its Figure 3 should keep its repair, since 25 on the capacity bound is still
  less than 50 on demand; check this when fixing. Repro over `t(id INT, cap INT)` holding `(1, 60)`: `DIAGNOSE SELECT id,
  x, o DECIDE x(INT), o(BOOL) FROM t SUCH THAT x <= cap * o AND SUM(x) >= 100 MINIMIZE SUM(x)` suggests
  `x <= cap * o + 40`; with `x <= cap` it suggests `sum(x) >= 60`. Found 2026-11-02.

## Limitations (not bugs)

- **A repair exactly at the tolerance on HiGHS's QP path.** An edit whose rows miss a summed bound by exactly the
  feasibility tolerance (1e-6) passes the certificate's LP on both backends, and Gurobi solves the edited query; HiGHS's
  QP path rejects it. HiGHS judges the same violation differently in its LP and QP paths.
- **Gurobi's barrier QP at stage 2.** On a 9,000-row QP repair Gurobi returns a point that breaks a row by 5.7e-5 and an
  objective value 24 above the optimum HiGHS finds (225000010024 against 225000010000). Inside Gurobi.
- **HiGHS on the edited 9,000-row QP.** It reaches its 300 s limit where Gurobi solves it.

Resolved behavior is documented by its owning `done.md`.
