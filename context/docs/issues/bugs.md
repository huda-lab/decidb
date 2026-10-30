# Known bugs

## BUG-33 — HiGHS fails or answers wrongly on a column bound or objective outside its window

- **Observed:** HiGHS refuses `x >= 1e25` and `MINIMIZE SUM(1e16 * POWER(x - 1, 2))` (a load error), which Gurobi
  solves; and `x BETWEEN 0 AND 10 MINIMIZE SUM(1e-12 * POWER(x - 1, 2))` (direct solve off) returns x = 0 on HiGHS,
  1 on Gurobi: the Hessian entries are below HiGHS's small-value threshold and are dropped.
- **Fix (to rule):** scale column bounds and the objective into HiGHS's window, as `ScaleRowsIntoHighsWindow` does for
  rows, and unscale the answer. After the HiGHS upgrade (queue § 1), whose thresholds may differ.

## BUG-35 — A needed repair amount below 1e-5 is dropped

- **Observed:** the absolute edit floor (`diagnostics/infeasible/done.md` § What the widening costs) drops a slack
  below 1e-5 as rounding, so a needed amount below it, such as 1/n spread over more than 100,000 rows, is not reported.
- **Fix (to rule):** with printed amounts certified by a solve, the floor's job is noise; decide whether the
  certificate can replace it.

## BUG-36 — A repair amount of 1e7 or more prints unrounded

- **Observed:** `SnapToPrecision` returns values at or above 1e7 as they are, so an amount like 12345678.90123 prints
  every digit.
- **Fix (to rule):** render at the same significant-figure precision as smaller amounts.

## Limitations (not bugs)

- **A repair exactly at the tolerance on HiGHS's QP path.** An edit whose rows miss a summed bound by exactly the
  feasibility tolerance (1e-6) passes the certificate's LP on both backends, and Gurobi solves the edited query; HiGHS's
  QP path rejects it. HiGHS judges the same violation differently in its LP and QP paths.
- **Gurobi's barrier QP at stage 2.** On a 9,000-row QP repair Gurobi returns a point that breaks a row by 5.7e-5 and an
  objective value 24 above the optimum HiGHS finds (225000010024 against 225000010000). Inside Gurobi.
- **HiGHS on the edited 9,000-row QP.** It reaches its 300 s limit where Gurobi solves it.

Resolved behavior is documented by its owning `done.md`.
