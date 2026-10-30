# Known bugs

## BUG-29 — `achievable_objective` leaves out the objective's constant

- **Observed:** on both backends, `x BETWEEN 0 AND 10 AND SUM(x) >= 30 MINIMIZE SUM(POWER(x - 5, 2))` over 2 rows
  reports 150; the repaired optimum is 200. `SUM(x + 7)` reports 30 for 44. The gaps (50 = 2 × 25, 14 = 2 × 7) are
  the constant terms, so the value reads the objective without its constant. A 9,000-row QP reports -1.5e11 for a sum
  of squares.
- **Fix (to rule):** the reported value is the objective as written: find where the repair path reads the objective
  value and add the constant the model builder folds out, at the one place every path reads it.

## BUG-30 — `runaway_-inf` is never reached

- **Observed:** `buy >= -1e30 AND buy <= 100 MINIMIZE SUM(buy)` (unbounded below) returns `undiagnosed`, "the runaway
  variable could not be identified", on both backends. `runaway_+inf` is reached. Repro:
  `benchmark/decide/diagnose_examples/queries/runaway_down.sql`.
- **Fix (to find):** the runaway search's handling of the downward direction.

## BUG-31 — HiGHS misreports `unbounded_after_fix` as no repair

- **Observed:** `benchmark/decide/diagnose_examples/queries/unbounded_after_fix.sql`: Gurobi returns the repair and an
  `unbounded_after_fix` row; HiGHS says "no loosening of the clauses you wrote restores feasibility". The HiGHS
  second-stage status is probably infeasible-or-unbounded, which the repair path does not handle.
- **Fix (to find):** read the stage-2 status the way the first stage does for an infeasible-or-unbounded model.

## BUG-32 — A printed repair amount can fall short

- **Observed:** on 9,000 rows, `x BETWEEN 0 AND cap AND SUM(x) <= -1` suggests `x BETWEEN -0.000111 AND cap`;
  9,000 × 0.000111 = 0.999 < 1, so the edit as printed leaves the query infeasible. The backends' `achievable_objective`
  also differ (10000.00011 against 10024.22).
- **Fix (to rule):** a printed amount rounds in the direction that keeps the repair sufficient (outward), never to
  nearest; find why the repaired optimum differs by backend.

## BUG-33 — HiGHS fails or answers wrongly on a column bound or objective outside its window

- **Observed:** HiGHS refuses `x >= 1e25` and `MINIMIZE SUM(1e16 * POWER(x - 1, 2))` (a load error), which Gurobi
  solves; and `x BETWEEN 0 AND 10 MINIMIZE SUM(1e-12 * POWER(x - 1, 2))` (direct solve off) returns x = 0 on HiGHS,
  1 on Gurobi: the Hessian entries are below HiGHS's small-value threshold and are dropped.
- **Fix (to rule):** scale column bounds and the objective into HiGHS's window, as `ScaleRowsIntoHighsWindow` does for
  rows, and unscale the answer. After the HiGHS upgrade (queue § 1), whose thresholds may differ.

## Limitations (not bugs)

None.

Resolved behavior is documented by its owning `done.md`.
