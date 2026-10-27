# Edge Cases & Data Shapes Test Coverage — Done

Tests live in:
- `test/decide/tests/test_edge_cases.py` — boundary conditions and degenerate inputs
- `benchmark/decide/test_large_scale.py`: scale checks; not collected by `pytest.ini` and fails at import, so it does not run (see `05_performance/benchmarking/scale_tests.md`)
- `test/decide/tests/test_sql_joins.py` — JOIN sources
- `test/decide/tests/test_explain.py` — EXPLAIN output

## Boundary conditions

Oracle-verified in `test_edge_cases.py` unless noted: an input with no rows (an
empty result, not an error), single-row input, 10-column linear combination in constraint +
objective, 6 heterogeneous constraints in one query, large-coefficient numeric
stability (1e9 coeffs + NE Big-M), RHS = 0 forcing all variables to zero,
trivially loose constraint (all selected), negative objective coefficients,
feasibility problem (no objective), all-zero objective coefficients
(`MAXIMIZE SUM(x * 0)`), and per-row decisions on a 1-to-many fan-out JOIN
(`test_entity_scope.py::test_row_scoped_vars_on_fanout_join`).

Error tests (not oracle-verified):
- NULL coefficients (with COALESCE hint) — `test_edge_cases.py::test_null_coefficients`
- Unconstrained INT var in objective (mixed w/ bounded BOOL) — `test_error_infeasible.py::TestUnboundedModels::test_mixed_unbounded_integer_var`

Aggregate LHS versus aggregate RHS (`SUM(x*v) <= SUM(y*v)`) is supported and
oracle-verified in `test_canonicalize_side_agnostic.py::test_aggregate_vs_aggregate`.

## Data shapes

All oracle-verified, in `test_when_constraint.py`, `test_per_clause.py`,
`test_per_objective.py`, and `test_per_multi_column.py`: NULL in a `when`
condition column (the row is dropped), NULL in a `per` key (a class of its own),
a single `per` class, a `when` keeping every row, a `when` that leaves a `per`
class without rows (that class generates nothing), mixed conditional +
unconditional constraints, a different `when` on each constraint, several `per`
constraints on different columns.

## Empty row sets

`test_edge_cases.py` pins the two empty-set errors, each with its exact message
(see also `03_expressivity/scope/done.md`):

- A `when` in front that keeps no row is "when c matches no rows." (D4): on
  constraints over an easy or hard `min`/`max`, a `sum` and an `avg`
  (`test_min_geq_constraint_when_empty` and its three mirrors,
  `test_sum_when_empty_rejected`, `test_avg_when_empty_rejected`), and on a flat
  MIN/MAX objective in both directions (`test_maximize_min_objective_when_empty` and
  its three mirrors).
- A reducer whose own `when` keeps no row is "agg(when c: e) has no rows." (D8): in
  hard constraints (`test_max_when_empty_constraint_hard`,
  `test_min_when_empty_constraint_hard`), beside a populated reducer
  (`test_sum_plus_max_when_empty_silently_vacates_constraint`,
  `test_composed_easy_min_when_empty_rejected`,
  `test_mixed_empty_and_populated_when_terms_constraint` and `..._objective`), and
  for AVG (`test_avg_constraint_when_filters_all_rows`).
- A `when` in front that leaves one `per` class without rows generates nothing for
  that class (`test_avg_per_constraint_with_empty_group`,
  `test_maximize_sum_max_per_with_empty_when_group`), while a reducer's own `when`
  that empties a group is "avg(when flag: x * val) has no rows for grp = 'B'."
  (`test_avg_per_constraint_inner_when_with_empty_group_rejected`).

## JOIN sources

All oracle-verified: 2-table and 3-table JOINs (`test_sql_joins.py`),
JOIN + keyed decisions (many tests in `test_entity_scope.py`).

## Scale / performance

`benchmark/decide/test_large_scale.py` holds a 501-row knapsack and a 2204-row order selection,
but the file is not collected and does not run, so neither is verified today. The collected scale case is the 1500-customer problem in
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

EXPLAIN output format is covered in `test_explain.py`.
