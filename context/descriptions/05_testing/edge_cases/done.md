# Edge Cases & Data Shapes Test Coverage — Done

Tests live in:
- `test/decide/tests/test_edge_cases.py` — boundary conditions and degenerate inputs
- `test/decide/tests/test_large_scale.py` — scale/performance tests
- `test/decide/tests/test_sql_joins.py` — JOIN sources
- `test/decide/tests/test_sql_subquery.py` — SQL subquery features
- `test/decide/tests/test_explain.py` — EXPLAIN output

## Boundary conditions

Oracle-verified in `test_edge_cases.py` unless noted: zero rows matching
(empty result), single-row input, 10-column linear combination in constraint +
objective, 6 heterogeneous constraints in one query, large-coefficient numeric
stability (1e9 coeffs + NE Big-M), RHS = 0 forcing all variables to zero,
trivially loose constraint (all selected), negative objective coefficients,
feasibility problem (no objective), all-zero objective coefficients
(`MAXIMIZE SUM(x * 0)`), and row-scoped variables on a 1-to-many fan-out JOIN
(`test_entity_scope.py::test_row_scoped_vars_on_fanout_join`).

Error tests (not oracle-verified):
- NULL coefficients (with COALESCE hint) — `test_edge_cases.py::test_null_coefficients`
- Aggregate LHS vs aggregate RHS (`SUM(x*v) <= SUM(y*v)`) rejected — `test_error_binder.py::test_aggregate_vs_aggregate_constraint_rejected`
- Unconstrained INT var in objective (mixed w/ bounded BOOL) — `test_error_infeasible.py::TestUnboundedModels::test_mixed_unbounded_integer_var`

## Data shapes

All oracle-verified, in `test_when_constraint.py`, `test_per_clause.py`,
`test_per_objective.py`, and `test_per_multi_column.py`: NULL in WHEN
condition columns, NULL in PER column, single group in PER, WHEN matching
no/all rows, WHEN eliminating an entire PER group, mixed conditional +
unconditional constraints, multiple WHEN conditions on different constraints,
multiple PER constraints on different columns.

## JOIN sources

All oracle-verified: 2-table and 3-table JOINs (`test_sql_joins.py`),
JOIN + entity-scoped (many tests in `test_entity_scope.py`).

## Scale / performance

Oracle-verified in `test_large_scale.py`: 501-row knapsack, 2204-row order
selection. Plus a 1500-customer problem in
`test_entity_scope.py::test_entity_scoped_mixed_when_per` (Gurobi-only).

## Solver coverage

The oracle always picks Gurobi (gurobipy required; oracle fixtures skip if
unavailable). The `_expect_gurobi` decorator in QP/bilinear tests accepts the
rejection message on HiGHS-only hosts. The `DECIDB_FORCE_SOLVER` env var pins
DeciDB's backend for specific tests via the `decidb_cli_highs` and
`decidb_cli_gurobi` fixtures.

| Scenario | Where | Oracle |
|----------|-------|--------|
| HiGHS rejects non-convex QP (MAXIMIZE SUM(POWER)) | `test_quadratic.py::TestHighsRejection::test_highs_nonconvex_qp_rejected` | error test |
| HiGHS rejects MIQP (integer vars + quadratic obj) | `test_quadratic.py::TestHighsRejection::test_highs_miqp_rejected` | error test |
| Gurobi ↔ HiGHS objective agreement on linear IP | `test_edge_cases.py::test_gurobi_highs_agree_on_objective` | cross-check |
| Infeasible QP constraint (tight `match=`) | `test_quadratic_constraints.py::test_infeasible_negative_budget` | error test |

## Infrastructure / meta-tests

EXPLAIN output format (`test_explain.py`); SQL subquery features, non-DECIDE
(`test_sql_subquery.py`).
