# Edge Cases & Data Shapes Test Coverage — Done

Tests live in:
- `test/decide/tests/test_edge_cases.py` — boundary conditions and degenerate inputs
- `test/decide/tests/test_large_scale.py` — scale/performance tests
- `test/decide/tests/test_sql_joins.py` — JOIN sources
- `test/decide/tests/test_sql_subquery.py` — SQL subquery features
- `test/decide/tests/test_explain.py` — EXPLAIN output

## Boundary conditions

| Scenario | Where | Oracle |
|----------|-------|--------|
| Zero rows matching (empty result) | `test_edge_cases.py::test_zero_rows_empty_input` | ✓ |
| Aggregate LHS vs aggregate RHS (`SUM(x*v) <= SUM(y*v)`) rejected | `test_error_binder.py::test_aggregate_vs_aggregate_constraint_rejected` | error test |
| 10-column linear combination in constraint + objective | `test_edge_cases.py::test_many_terms_objective` | ✓ |
| 6 heterogeneous constraints in one query | `test_edge_cases.py::test_five_plus_heterogeneous_constraints` | ✓ |
| Large-coefficient numeric stability (1e9 coeffs + NE Big-M) | `test_edge_cases.py::test_large_coefficient_numeric_stability` | ✓ |
| Row-scoped variables on 1-to-many fan-out JOIN | `test_entity_scope.py::test_row_scoped_vars_on_fanout_join` | ✓ |
| Single row input (trivial problem) | `test_edge_cases.py::test_single_row` | ✓ |
| All variables forced to same value (RHS=0) | `test_edge_cases.py::test_rhs_zero_forces_all_zero` | ✓ |
| Trivially loose constraint (all selected) | `test_edge_cases.py::test_trivial_all_selected` | ✓ |
| Negative objective coefficients | `test_edge_cases.py::test_negative_objective_coefficients` | ✓ |
| NULL coefficients (with COALESCE hint) | `test_edge_cases.py::test_null_coefficients` | error test |
| Feasibility problem (no objective) | `test_edge_cases.py::test_feasibility_no_objective` | ✓ |
| All-zero objective coefficients (`MAXIMIZE SUM(x * 0)`) | `test_edge_cases.py::test_all_zero_objective` | ✓ |
| Unconstrained INTEGER var in objective (mixed w/ bounded BOOLEAN) | `test_error_infeasible.py::TestUnboundedModels::test_mixed_unbounded_integer_var` | error test |

## Data shapes

| Scenario | Where | Oracle |
|----------|-------|--------|
| NULL in WHEN condition columns | `test_when_constraint.py::test_when_null_condition_column` | ✓ |
| NULL in PER column | `test_per_clause.py::test_per_null_group_key` | ✓ |
| Single group in PER | `test_per_objective.py::test_single_group` | ✓ |
| WHEN matching no rows | `test_when_constraint.py::test_when_no_rows_match` | ✓ |
| WHEN matching all rows | `test_when_constraint.py::test_when_all_rows_match` | ✓ |
| WHEN eliminates all in a PER group | `test_per_multi_column.py::test_multi_column_per_when_eliminates_all_in_group` | ✓ |
| Mixed conditional + unconditional | `test_when_constraint.py::test_when_mixed_conditional_and_unconditional` | ✓ |
| Multiple WHEN conditions on different constraints | `test_when_constraint.py::test_when_multiple_categories` | ✓ |
| Multiple PER constraints, different columns | `test_per_clause.py::test_per_different_grouping_columns` | ✓ |

## JOIN sources

| Scenario | Where | Oracle |
|----------|-------|--------|
| 2-table JOIN | `test_sql_joins.py::test_q05_join_decide` | ✓ |
| 3-table JOIN | `test_sql_joins.py` | ✓ |
| JOIN + entity-scoped (many tests) | `test_entity_scope.py` | ✓ |

## Scale / performance

| Scenario | Where | Oracle |
|----------|-------|--------|
| 501-row knapsack | `test_large_scale.py::test_knapsack_large` | ✓ |
| 2204-row order selection | `test_large_scale.py::test_order_selection_large` | ✓ |
| 1500-customer problem | `test_entity_scope.py::test_entity_scoped_mixed_when_per` | ✓ (Gurobi-only) |

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

| Scenario | Where |
|----------|-------|
| EXPLAIN output format | `test_explain.py` |
| SQL subquery features (non-DECIDE) | `test_sql_subquery.py` |
