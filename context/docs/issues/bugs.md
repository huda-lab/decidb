# Known bugs

## BUG-35 — A needed repair amount below 1e-5 is dropped

- **Observed:** the absolute edit floor (`diagnostics/infeasible/done.md` § What the widening costs) drops a slack
  below 1e-5 as rounding, so a needed amount below it, such as 1/n spread over more than 100,000 rows, is not reported.
- **Fix (to rule):** with printed amounts certified by a solve, the floor's job is noise; decide whether the
  certificate can replace it.

## BUG-36 — A repair amount of 1e7 or more prints unrounded

- **Observed:** `SnapToPrecision` returns values at or above 1e7 as they are, so an amount like 12345678.90123 prints
  every digit.
- **Fix (to rule):** render at the same significant-figure precision as smaller amounts.

## BUG-37 — A refused row quotes a coefficient the user did not write

- **Observed:** on HiGHS, `x >= 1e29` with `SUM(x + w*y) >= 0` and w = 1e-16 is refused correctly (no scaling places
  the row once `x`'s column is scaled), but the message quotes "1e-16 to 1.07374e+09": the second number is `x`'s
  coefficient after its column scale. The same can happen in the objective through Hessian entries scaled by 2^2e.
- **Fix (to rule):** a refusal whose row holds a scaled column names that decision's bound as the cause.

## Limitations (not bugs)

- **A repair exactly at the tolerance on HiGHS's QP path.** An edit whose rows miss a summed bound by exactly the
  feasibility tolerance (1e-6) passes the certificate's LP on both backends, and Gurobi solves the edited query; HiGHS's
  QP path rejects it. HiGHS judges the same violation differently in its LP and QP paths.
- **Gurobi's barrier QP at stage 2.** On a 9,000-row QP repair Gurobi returns a point that breaks a row by 5.7e-5 and an
  objective value 24 above the optimum HiGHS finds (225000010024 against 225000010000). Inside Gurobi.
- **HiGHS on the edited 9,000-row QP.** It reaches its 300 s limit where Gurobi solves it.

Resolved behavior is documented by its owning `done.md`.
