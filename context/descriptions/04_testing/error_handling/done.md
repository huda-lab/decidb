# Error Handling Test Coverage — Done

Tests live in:
- `test/decide/tests/test_error_parser.py` — parser-level syntax errors
- `test/decide/tests/test_error_binder.py` — binder-level semantic errors
- `test/decide/tests/test_error_infeasible.py` — infeasible and unbounded model detection

## Parser errors

`test_error_parser.py`, all expecting `decidb.ParserException`: DECIDE without
SUCH THAT, DECIDE without a variable name, MAXIMIZE/MINIMIZE without an
objective expression, an unknown type, comma-separated `SUCH THAT`
constraints ("use AND between constraints").

The removed spellings (`00_project_overview/syntax_reference.md` §10.3) are parser
errors whose message carries "is removed" and names the replacement —
`test_scope_removed_syntax.py`. The scoping language's own
parse errors (a `per` key or `per row` on the objective, a parenthesized key, an
expression in a key) are pinned in `test/decidb/test_scope_parser.test`. See
[`../scope/done.md`](../scope/done.md).

## Binder errors

All expect `decidb.InvalidInputException` or `decidb.BinderException`.
Categories covered in `test_error_binder.py` unless noted:

- Name/scope conflicts: variable conflicts with column, duplicate DECIDE variables, unknown variable in constraint; a name a join merges and a decision in `WHERE` in `test_decision_names.py` (`../variables/done.md`).
- Unsupported syntax in SUCH THAT: IS NULL, `SUM(x) IN (...)`, non-DECIDE variable in constraint, non-SUM/AVG/MIN/MAX function in objective.
- Non-linear / invalid aggregate: no DECIDE variable in SUM, multiple DECIDE variables in SUM, nonlinear DECIDE variables.
- RHS shape: non-scalar BETWEEN / SUM RHS / `SUM = RHS`, DECIDE variable in BETWEEN bounds, IN RHS, or constraint RHS.
- Objective: a constant added to the objective is accepted
  (`test_objective_with_addition_succeeds`); a bare data column is rejected.
- Subquery: non-scalar subquery RHS, scalar subquery returning multiple rows.
- A decision in a `when` condition, plain and compound, and in a reducer's own `when`
  (`test_aggregate_local_when.py`): "x is a decision; when, per and by may only use
  data".
- A correlated subquery bound that takes two values under `=`
  (`test_correlated_subquery_aggregate_equality_rhs_rejected`).
- `OR` between constraints, in every scope spelling, and data-only aggregates as
  divisors (`TestRefusalsInEverySpelling`).
- The scope errors — an objective produced more than once, a `when` that keeps no row,
  a reducer with no row for a group, key errors — are in the `test_scope_*.py` suites
  ([`../scope/done.md`](../scope/done.md)).

## Infeasibility detection

`test_error_infeasible.py::TestInfeasibleModels`, expecting
`decidb.InvalidInputException` matching `"infeasible"`: contradictory per-row
bounds (`x >= 10 AND x <= 5`), impossible SUM constraint (`SUM(x) >= 1000`
with too few rows), negative SUM upper bound, a `when` that forces every row to
zero beside `SUM(x) >= 1` (`when l_quantity > 0: x <= 0`), and
`sum(x) by (grp) = cap` with two caps in one group
(`test_equal_bound_varying_per_group_is_infeasible`). Its `per grp:` form is two
variants of the class, which name the same contradiction
(`test_equal_bound_varying_within_a_per_class_is_two_instances`).

## Unboundedness detection

`test_error_infeasible.py::TestUnboundedModels`: unbounded MAXIMIZE with
`INT` and with `REAL` (only lower bound).

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
