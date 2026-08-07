# PER Clause Test Coverage — Done

Tests live in:
- `test/decide/tests/test_per_clause.py` — basic PER on constraints
- `test/decide/tests/test_per_multi_column.py` — multi-column PER
- `test/decide/tests/test_per_objective.py` — PER on objectives (nested aggregates)
- `test/decide/tests/test_per_interactions.py` — PER composed with auxiliary-variable features (hard MIN/MAX, ABS, multi-variable)

## Scenarios covered

### PER on constraints

All oracle-verified:

- **Basic** (`test_per_clause.py`, `test_per_multi_column.py`): single-column PER with aggregate constraint, multi-column PER (2 and 3 columns), INT variables, PER + WHEN, PER + `<>`, NULL group keys (excluded), two PER constraints on different columns, WHEN filtering out an entire PER group.
- **Auxiliary-variable interactions** (`test_per_interactions.py`): hard `MAX(>=K)` / hard `MIN(<=K)` (Big-M per group), equality `MAX(=K)` (easy + hard combined per group), ABS in aggregate constraint (ABS aux per group), multi-variable BOOL + INT, WHEN + PER + multi-variable (WHEN mask per group), QP objective + PER constraint.
- **Degenerate / edge shapes**: single-row PER groups, zero-coefficient group (one group's aggregate vacuous), NULL PER-key + WHEN mask (NULL bucket with WHEN→PER empty-skip) — all `test_per_interactions.py`; PER equality constraint (two-sided bounds per group, `test_abs_linearization.py`); feasibility (no objective) + PER (`test_edge_cases.py`); uncorrelated scalar subquery as PER constraint RHS (`test_cons_subquery.py`).

### PER on objectives (nested aggregate syntax)

All 16+ combinations of outer/inner ∈ {SUM, MIN, MAX, AVG} tested in
`test_per_objective.py`: `SUM(SUM(...))` no-op equivalence, all
`SUM(MAX/MIN)` and `MAX/MIN(SUM)` directions (including hard-outer +
easy-inner shapes), `SUM(AVG)` with unequal groups, `MAX(AVG)` / `MIN(AVG)`,
nested + WHEN (`SUM(MAX(...)) WHEN ... PER col`), and the single-group
degenerate case.

### Error cases

- Flat `MIN/MAX + PER` (ambiguous) rejected — `test_per_objective.py`.
- `PER` on per-row constraint (`x <= 5 PER col`) rejected — `test_error_binder.py::TestBinderErrors::test_per_on_perrow_constraint_rejection`.

## Feature interactions covered

| Feature A | Feature B | Tested |
|-----------|-----------|--------|
| PER | WHEN (expression-level) | ✓ |
| PER | WHEN (aggregate-local) | ✓ |
| PER | multi-column | ✓ |
| PER | INT variables | ✓ |
| PER | NULL group keys | ✓ |
| PER | `<>` | ✓ |
| PER | MIN/MAX (easy, stripped) | ✓ |
| PER | MIN/MAX (objective, nested) | ✓ |
| PER | AVG | ✓ |
| PER | entity-scoped | ✓ |
| PER | quadratic constraint | ✓ |
| PER | quadratic constraint + WHEN | ✓ |
| PER | QP objective | ✓ |
| PER | WHEN + entity-scoped (triple) | ✓ |
| PER | WHEN + MIN/MAX (triple) | ✓ |
| PER | WHEN + AVG (triple) | ✓ |
| PER | hard MIN/MAX constraints (Big-M per group) | ✓ |
| PER | ABS in aggregate constraint | ✓ |
| PER | multi-variable (BOOL + INT) | ✓ |
| PER | WHEN + multi-variable | ✓ |
| PER | equality constraint | ✓ |
| PER | feasibility (no objective) | ✓ |
