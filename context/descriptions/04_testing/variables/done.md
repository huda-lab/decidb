# Variable Type Test Coverage — Done

Covers `BOOL`, `INT`, `REAL`, and multi-variable queries. Tests live in:
- `test/decide/tests/test_var_boolean.py` — BOOL
- `test/decide/tests/test_var_integer.py` — omitted-type rejection (the type is mandatory)
- `test/decide/tests/test_var_real.py` — REAL
- `test/decide/tests/test_var_multi.py` — multiple variables

Note: keyed decisions (`per K: x(TYPE)`) are covered in
[../entity_scope/](../entity_scope/done.md) and by `test_scope_declare.py`
([../scope/](../scope/done.md)); query-wide decisions (`per (): x(TYPE)`) in
[../scalar_scope/](../scalar_scope/done.md).

## Scenarios covered

All oracle-verified:

- **BOOL** (`test_var_boolean.py`, plus many files): classic 0/1 knapsack and a weight-limit variant; broad coverage across all constraint types and MAXIMIZE/MINIMIZE objectives via the rest of the suite.
- **INT** (`test_cons_perrow.py`, `test_cons_mixed.py`, `test_cons_between.py`): explicit `x(INT)`, per-row upper bound + aggregate, column-derived upper bound (`x <= ps_availqty`), BETWEEN with INT. `test_var_integer.py` covers the omitted-type parser error rather than a positive case.
- **REAL** (`test_var_real.py`): basic LP (continuous MAXIMIZE), upper bound on REAL, mixed BOOL + REAL, `when` and `per` on aggregate constraints, MINIMIZE (coefficient-sign path), forced non-integer optimum (readback sanity).
- **Multiple variables** (`test_var_multi.py`, plus interactions elsewhere): two variables with separate constraints (BOOL + INT), two boolean variables (cross-constraint), paired INT + REAL (no BOOL), three variables (BOOL + INT + REAL), mixed BOOL + REAL in the same SUM term and in the same query; mixed BOOL + REAL with ABS (`test_abs_linearization.py`); BOOL + INT under a `per` key (`test_per_interactions.py`).

### Error cases

`test_error_binder.py`: variable name conflicts with column, duplicate DECIDE
variable, unknown variable in constraint. Unknown type annotation is
parser-level (`test_error_parser.py`).

`test_decision_names.py`: where a decision's name may stand outside the clauses. A
name a `USING`, `FULL OUTER JOIN USING` or `NATURAL` join merges, in any letter case,
or one two relations share, is the conflict error in both clause orders; a decision with
its own name over a `USING` join answers. A decision in `WHERE` is refused by name,
through a SELECT alias, an aliased expression, `COLUMNS(*)`, a lambda body, the left of
an `IN` subquery, and one or two correlated subqueries down; `WHERE` over data (a
column, a correlated data subquery, a lambda parameter named like a decision) keeps its
rows, and an outer query filters on the solved value, the refusal's suggestion. A decision
read other than by its bare name in a clause is refused: by its position (`#4`, and a data
column's `#2`) in a constraint, the objective, a `when`, a `by` and a frame's order; from
inside a subquery, in a constraint, the objective and a `when`; past a lambda parameter of
its name. A name that binds elsewhere is not the decision: a subquery's own column, a
lambda parameter inside a subquery, a nested DECIDE's own decision.

## Feature interactions covered

| Feature A | Feature B | Tested |
|-----------|-----------|--------|
| BOOL | all features (broadly) | ✓ |
| INT | all features (broadly) | ✓ |
| REAL | MAXIMIZE objective | ✓ |
| REAL | MINIMIZE objective | ✓ |
| REAL | `per` constraint | ✓ |
| REAL | `when` constraint (aggregate) | ✓ |
| BOOL + INT | same query | ✓ |
| BOOL + INT | `per` key | ✓ |
| BOOL + REAL | same query | ✓ |
| BOOL + REAL | ABS linearization | ✓ |
| BOOL + REAL | bilinear (McCormick) | ✓ |
| INT + REAL | QP constraint | ✓ |
