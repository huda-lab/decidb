# Variable Type Test Coverage — Done

Covers `IS BOOLEAN`, `IS INTEGER`, `IS REAL`, and multi-variable queries. Tests live in:
- `test/decide/tests/test_var_boolean.py` — IS BOOLEAN
- `test/decide/tests/test_var_integer.py` — default/IS INTEGER
- `test/decide/tests/test_var_real.py` — IS REAL
- `test/decide/tests/test_var_multi.py` — multiple variables

Note: table-scoped variables (`DECIDE Table.var`) have their own folder at
[../entity_scope/](../entity_scope/).

## Scenarios covered

All oracle-verified:

- **IS BOOLEAN** (`test_var_boolean.py`, plus many files): classic 0/1 knapsack and a weight-limit variant; broad coverage across all constraint types and MAXIMIZE/MINIMIZE objectives via the rest of the suite.
- **IS INTEGER** (`test_var_integer.py`, `test_cons_perrow.py`, `test_cons_mixed.py`, `test_cons_between.py`): default type (no annotation) and explicit `IS INTEGER`, per-row upper bound + aggregate, column-derived upper bound (`x <= ps_availqty`), BETWEEN with INTEGER.
- **IS REAL** (`test_var_real.py`): basic LP (continuous MAXIMIZE), upper bound on REAL, mixed BOOLEAN + REAL, WHEN and PER on aggregate constraints, MINIMIZE (coefficient-sign path), forced non-integer optimum (readback sanity).
- **Multiple variables** (`test_var_multi.py`, plus interactions elsewhere): two variables with separate constraints (BOOL + INTEGER), two boolean variables (cross-constraint), paired INTEGER + REAL (no BOOLEAN), three variables (BOOL + INTEGER + REAL), mixed BOOLEAN + REAL in the same SUM term and in the same query; mixed BOOLEAN + REAL with ABS (`test_abs_linearization.py`); BOOLEAN + INTEGER under PER grouping (`test_per_interactions.py`).

### Error cases

`test_error_binder.py`: variable name conflicts with column, duplicate DECIDE
variable, unknown variable in constraint. Unknown type annotation is
parser-level (`test_error_parser.py`).

## Feature interactions covered

| Feature A | Feature B | Tested |
|-----------|-----------|--------|
| BOOLEAN | all features (broadly) | ✓ |
| INTEGER | all features (broadly) | ✓ |
| REAL | MAXIMIZE objective | ✓ |
| REAL | MINIMIZE objective | ✓ |
| REAL | PER constraint | ✓ |
| REAL | WHEN constraint (aggregate) | ✓ |
| BOOLEAN + INTEGER | same query | ✓ |
| BOOLEAN + INTEGER | PER grouping | ✓ |
| BOOLEAN + REAL | same query | ✓ |
| BOOLEAN + REAL | ABS linearization | ✓ |
| BOOLEAN + REAL | bilinear (McCormick) | ✓ |
| INTEGER + REAL | QP constraint | ✓ |
