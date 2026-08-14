# Variable Type Test Coverage — Done

Covers `BOOL`, `INT`, `REAL`, and multi-variable queries. Tests live in:
- `test/decide/tests/test_var_boolean.py` — BOOL
- `test/decide/tests/test_var_integer.py` — omitted-type rejection (the type is mandatory)
- `test/decide/tests/test_var_real.py` — REAL
- `test/decide/tests/test_var_multi.py` — multiple variables

Note: table-scoped variables (`DECIDE Table.var`) have their own folder at
[../entity_scope/](../entity_scope/).

## Scenarios covered

All oracle-verified:

- **BOOL** (`test_var_boolean.py`, plus many files): classic 0/1 knapsack and a weight-limit variant; broad coverage across all constraint types and MAXIMIZE/MINIMIZE objectives via the rest of the suite.
- **INT** (`test_cons_perrow.py`, `test_cons_mixed.py`, `test_cons_between.py`): explicit `x(INT)`, per-row upper bound + aggregate, column-derived upper bound (`x <= ps_availqty`), BETWEEN with INT. `test_var_integer.py` covers the omitted-type parser error rather than a positive case.
- **REAL** (`test_var_real.py`): basic LP (continuous MAXIMIZE), upper bound on REAL, mixed BOOL + REAL, WHEN and PER on aggregate constraints, MINIMIZE (coefficient-sign path), forced non-integer optimum (readback sanity).
- **Multiple variables** (`test_var_multi.py`, plus interactions elsewhere): two variables with separate constraints (BOOL + INT), two boolean variables (cross-constraint), paired INT + REAL (no BOOL), three variables (BOOL + INT + REAL), mixed BOOL + REAL in the same SUM term and in the same query; mixed BOOL + REAL with ABS (`test_abs_linearization.py`); BOOL + INT under PER grouping (`test_per_interactions.py`).

### Error cases

`test_error_binder.py`: variable name conflicts with column, duplicate DECIDE
variable, unknown variable in constraint. Unknown type annotation is
parser-level (`test_error_parser.py`).

## Feature interactions covered

| Feature A | Feature B | Tested |
|-----------|-----------|--------|
| BOOL | all features (broadly) | ✓ |
| INT | all features (broadly) | ✓ |
| REAL | MAXIMIZE objective | ✓ |
| REAL | MINIMIZE objective | ✓ |
| REAL | PER constraint | ✓ |
| REAL | WHEN constraint (aggregate) | ✓ |
| BOOL + INT | same query | ✓ |
| BOOL + INT | PER grouping | ✓ |
| BOOL + REAL | same query | ✓ |
| BOOL + REAL | ABS linearization | ✓ |
| BOOL + REAL | bilinear (McCormick) | ✓ |
| INT + REAL | QP constraint | ✓ |
