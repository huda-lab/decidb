# Error Handling Test Coverage — Done

Tests live in:
- `test/decide/tests/test_error_parser.py` — parser-level syntax errors
- `test/decide/tests/test_error_binder.py` — binder-level semantic errors
- `test/decide/tests/test_error_infeasible.py` — infeasible and unbounded model detection

## Parser errors

| Scenario | Where |
|----------|-------|
| DECIDE without SUCH THAT | `test_error_parser.py::test_missing_such_that` |
| DECIDE without a variable name | `test_error_parser.py::test_missing_decide_variable` |
| MAXIMIZE/MINIMIZE without an objective expression | `test_error_parser.py::test_missing_objective_expression` |
| `IS <unknown-type>` | `test_error_parser.py::test_unknown_variable_type` |

All expect `decidb.ParserException`.

## Binder errors

| Category | Tests |
|----------|-------|
| Name/scope conflicts | `test_variable_conflicts_with_column`, `test_duplicate_decide_variables`, `test_unknown_variable_in_constraint` |
| Unsupported syntax in SUCH THAT | `test_is_null_unsupported`, `test_sum_with_in_not_allowed`, `test_non_decide_variable_in_constraint`, `test_non_sum_avg_min_max_function_in_objective` |
| Non-linear / invalid aggregate | `test_no_decide_variable_in_sum`, `test_multiple_decide_variables_in_sum`, `test_nonlinear_decide_variables` |
| RHS shape | `test_between_non_scalar`, `test_decide_between_decide_variable`, `test_in_rhs_with_decide_variable`, `test_sum_rhs_non_scalar`, `test_decide_variable_rhs_with_decide`, `test_sum_equal_non_scalar` |
| Objective rejections | `test_objective_with_addition`, `test_objective_bare_column` |
| Subquery | `test_subquery_rhs_non_scalar`, `test_subquery_rhs_returns_multiple_rows` |
| WHEN restrictions | DECIDE var in WHEN condition (aggregate + compound), correlated subquery non-scalar |
| Aggregate-local WHEN | Mixing expression-level and aggregate-local, DECIDE var in local condition |
| PER on per-row constraint | `test_per_on_perrow_constraint_rejection` |
| Aggregate LHS vs aggregate RHS | `test_aggregate_vs_aggregate_constraint_rejected` |
| Flat MIN/MAX + PER | Rejected via `test_per_objective.py` |

All expect `decidb.InvalidInputException` or `decidb.BinderException`.

## Infeasibility detection

| Scenario | Where |
|----------|-------|
| Contradictory per-row bounds (`x >= 10 AND x <= 5`) | `test_error_infeasible.py::TestInfeasibleModels::test_contradictory_per_row_bounds` |
| Impossible SUM constraint (`SUM(x) >= 1000` with too few rows) | `test_error_infeasible.py::TestInfeasibleModels::test_impossible_sum_constraint` |
| Negative SUM upper bound | `test_error_infeasible.py::TestInfeasibleModels::test_negative_sum_upper_bound` |
| WHEN-forced infeasibility (all rows zero) | `test_error_infeasible.py::TestInfeasibleModels::test_infeasible_when_forces_all_zero` |

All expect `decidb.InvalidInputException` matching `"infeasible"`.

## Unboundedness detection

| Scenario | Where |
|----------|-------|
| Unbounded MAXIMIZE with `IS INTEGER` (only lower bound) | `test_error_infeasible.py::TestUnboundedModels::test_unbounded_integer_maximize` |
| Unbounded MAXIMIZE with `IS REAL` (only lower bound) | `test_error_infeasible.py::TestUnboundedModels::test_unbounded_real_maximize` |

Error message matches `(?i)(unbounded|infeasible)` — accepts either, since
some solvers return `INF_OR_UNBD` when they can't distinguish the two without
further analysis.

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
