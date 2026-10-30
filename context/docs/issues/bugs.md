# Known bugs

- **BUG-41 — A direct plan checks a constant coefficient and names it `1`.** A1 on `MINIMIZE SUM(POWER(s - p / 100,
  2))` plans a NaN/infinite check whose message reads "1 is NaN or infinite on some rows. Skip those rows with 'when
  isfinite(1):'." A constant cannot be NaN on some rows, so the check should not exist, and a message must name what
  the user wrote. Repro: `EXPLAIN SELECT id, s DECIDE s(REAL) FROM t SUCH THAT s BETWEEN 0 AND q * 2 MINIMIZE
  SUM(POWER(s - p / 100, 2))` over any table `t(id, p DOUBLE, q DOUBLE)`. Found 2026-10-30 (lane AT, PERF-16).

## Limitations (not bugs)

- **A repair exactly at the tolerance on HiGHS's QP path.** An edit whose rows miss a summed bound by exactly the
  feasibility tolerance (1e-6) passes the certificate's LP on both backends, and Gurobi solves the edited query; HiGHS's
  QP path rejects it. HiGHS judges the same violation differently in its LP and QP paths.
- **Gurobi's barrier QP at stage 2.** On a 9,000-row QP repair Gurobi returns a point that breaks a row by 5.7e-5 and an
  objective value 24 above the optimum HiGHS finds (225000010024 against 225000010000). Inside Gurobi.
- **HiGHS on the edited 9,000-row QP.** It reaches its 300 s limit where Gurobi solves it.

Resolved behavior is documented by its owning `done.md`.
