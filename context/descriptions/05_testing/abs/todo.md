# ABS Linearization Test Coverage — Todo

## Missing coverage

### Oracle tests for hard-direction ABS (Big-M sign-indicator path)

When the proper Big-M fix landed (see `07_issues/bugs/done.md` → "ABS Hard-Direction Constraints — Proper Big-M Fix"), C33–C37 in `stress_queries/01_constraints.sql` were added as smoke coverage for `ABS >= K`, `ABS = K`, `MIN(ABS) >= K`, `SUM(ABS) >= K`, and `ABS BETWEEN`. These exercise the new code path on real data but don't oracle-verify the optimum against an independent computation.

`test/decide/tests/test_abs_linearization.py` has oracle-verified positive tests for the core Path-B shapes:

- per-row `ABS(expr) >= K` — `test_abs_constraint_per_row_hard_ge` (closed-form: K > every coefficient forces the far branch on every row).
- per-row `ABS(expr) = K` — `test_abs_constraint_per_row_hard_eq` (both Big-M bounds active; lower-root vs upper-root selection).
- `SUM(ABS) >= K` aggregate hard direction — `test_abs_constraint_aggregate_hard_ge` (sign-indicator Big-M pins each `d_i = |·|`; oracle mirrors the maximize-objective model).

Remaining Path-B shapes still lacking oracle tests:

- `MIN(ABS) >= K` (easy-MIN strip → per-row hard)
- `MAX(ABS) >= K` aggregate hard direction (Big-M on each aux *and* the outer MAX big-M indicator from the existing hard-MAX path — interaction is non-trivial)
- `ABS BETWEEN a AND b` (both bounds at once)
- ABS on both sides of a comparison (`ABS(e1) <= ABS(e2)`)

Also worth: Path-B + WHEN (verify the unconditional per-row Big-M is correct under WHEN-filtered aggregates) and Path-B + PER (per-group aggregates over Path-B-pinned auxes).
