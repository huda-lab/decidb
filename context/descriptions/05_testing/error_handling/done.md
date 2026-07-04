# Error Handling Test Coverage — Done

Tests live in:
- `test/decide/tests/test_error_parser.py` — parser-level syntax errors
- `test/decide/tests/test_error_binder.py` — binder-level semantic errors
- `test/decide/tests/test_error_infeasible.py` — infeasible and unbounded model detection

## Parser errors

`test_error_parser.py`, all expecting `decidb.ParserException`: DECIDE without
SUCH THAT, DECIDE without a variable name, MAXIMIZE/MINIMIZE without an
objective expression, `IS <unknown-type>`, comma-separated `SUCH THAT`
constraints.

## Binder errors

All expect `decidb.InvalidInputException` or `decidb.BinderException`.
Categories covered in `test_error_binder.py` unless noted:

- Name/scope conflicts: variable conflicts with column, duplicate DECIDE variables, unknown variable in constraint.
- Unsupported syntax in SUCH THAT: IS NULL, `SUM(x) IN (...)`, non-DECIDE variable in constraint, non-SUM/AVG/MIN/MAX function in objective.
- Non-linear / invalid aggregate: no DECIDE variable in SUM, multiple DECIDE variables in SUM, nonlinear DECIDE variables.
- RHS shape: non-scalar BETWEEN / SUM RHS / `SUM = RHS`, DECIDE variable in BETWEEN bounds, IN RHS, or constraint RHS.
- Objective rejections: objective with addition, bare column objective.
- Subquery: non-scalar subquery RHS, scalar subquery returning multiple rows.
- WHEN restrictions: DECIDE var in WHEN condition (aggregate + compound), correlated subquery non-scalar.
- Aggregate-local WHEN: mixing expression-level and aggregate-local, DECIDE var in local condition.
- PER on per-row constraint rejected.
- Aggregate LHS vs aggregate RHS rejected.
- Flat MIN/MAX + PER rejected (via `test_per_objective.py`).

## Infeasibility detection

`test_error_infeasible.py::TestInfeasibleModels`, all expecting
`decidb.InvalidInputException` matching `"infeasible"`: contradictory per-row
bounds (`x >= 10 AND x <= 5`), impossible SUM constraint (`SUM(x) >= 1000`
with too few rows), negative SUM upper bound, WHEN-forced infeasibility (all
rows zero).

## Unboundedness detection

`test_error_infeasible.py::TestUnboundedModels`: unbounded MAXIMIZE with
`IS INTEGER` and with `IS REAL` (only lower bound).

Error messages are matched exactly (`(?i)unbounded` here, `(?i)infeasible`
for the infeasible class). Gurobi's ambiguous `INF_OR_UNBD` status is
disambiguated in the backend via a `DualReductions=0` re-solve (Gurobi's
documented recipe), so DecidB always reports the definitive status; the
gurobipy oracle applies the same recipe.

## Solver-specific error paths

| Scenario | Where |
|----------|-------|
| HiGHS rejects non-convex QP | `test_quadratic.py::TestHighsRejection::test_highs_nonconvex_qp_rejected` (forced-HiGHS tight match) + `test_quadratic.py` (`_expect_gurobi` pattern) |
| HiGHS rejects MIQP | `test_quadratic.py::TestHighsRejection::test_highs_miqp_rejected` (forced-HiGHS tight match) + `test_quadratic.py` (`_expect_gurobi`) |
| HiGHS rejects quadratic constraints | `test_quadratic_constraints.py` |
| HiGHS rejects non-convex bilinear | `test_bilinear.py` |
| Bilinear without upper bound on non-Boolean | `test_bilinear.py` |
| Triple product rejected | `test_bilinear.py` |
| `POWER(x, 3)` and variable exponents rejected | `test_quadratic.py` |
