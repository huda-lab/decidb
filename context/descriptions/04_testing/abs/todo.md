# ABS Linearization Test Coverage — Todo

## Missing coverage

### Oracle tests for hard-direction ABS (Big-M sign-indicator path)

When the proper Big-M fix landed, C33–C37 in `stress_queries/01_constraints.sql` were added as smoke coverage for `ABS >= K`, `ABS = K`, `MIN(ABS) >= K`, `SUM(ABS) >= K`, and `ABS BETWEEN`. These exercise the new code path on real data but don't oracle-verify the optimum against an independent computation.

`test/decide/tests/test_abs_linearization.py` has oracle-verified positive tests for every Path-B shape:

- per-row `ABS(expr) >= K` — `test_abs_constraint_per_row_hard_ge` (closed-form: K > every coefficient forces the far branch on every row).
- per-row `ABS(expr) = K` — `test_abs_constraint_per_row_hard_eq` (both Big-M bounds active; lower-root vs upper-root selection).
- `SUM(ABS) >= K` aggregate hard direction — `test_abs_constraint_aggregate_hard_ge` (sign-indicator Big-M pins each `d_i = |·|`; oracle mirrors the maximize-objective model).
- `MIN(ABS) >= K` — `test_abs_min_geq_per_row_hard` (easy-MIN strip → per-row hard; all rows forced to the far branch).
- `MAX(ABS) >= K` — `test_abs_max_geq_aggregate_hard` (outer hard-MAX indicator `SUM(y) >= 1` stacked on each ABS Big-M; exactly one row must satisfy).
- `ABS BETWEEN a AND b` — `test_abs_between_both_bounds` (easy upper + hard lower together).
- ABS on both sides (`ABS(e1) <= ABS(e2)`) — `test_abs_on_both_sides` (feasible half-line up to the midpoint).

## Remaining

- Path-B + WHEN (verify the unconditional per-row Big-M is correct under WHEN-filtered aggregates) and Path-B + PER (per-group aggregates over Path-B-pinned auxes). Lower priority — the core Big-M formulations are now oracle-covered above; these check the WHEN/PER composition on top.
