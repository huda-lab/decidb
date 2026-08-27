# Constraint Operator Test Coverage — Done

Covers the constraint operators `=`, `<`, `<=`, `>`, `>=`, `<>`, `BETWEEN`, and `IN`, plus structural forms (per-row, aggregate, multi-constraint, mixed). Tests live in:
- `test/decide/tests/test_cons_comparison.py` — all 6 comparison operators
- `test/decide/tests/test_cons_between.py` — BETWEEN
- `test/decide/tests/test_cons_in.py` — IN on decision variables
- `test/decide/tests/test_cons_perrow.py` — per-row constraints
- `test/decide/tests/test_cons_aggregate.py` — aggregate (SUM) constraints
- `test/decide/tests/test_cons_mixed.py` — per-row + aggregate combined
- `test/decide/tests/test_cons_multi.py` — multiple aggregate constraints

## Scenarios covered

### Comparison operators

All 6 operators tested on both per-row and aggregate constraints,
oracle-verified (`test_cons_comparison.py` and others).

- **`<>` (NE Big-M)** (`test_cons_comparison.py`, plus `test_aggregate_local_when.py` for the aggregate-local variant): expression-level and aggregate-local WHEN composition, with/without WHEN binding, and mixed-sign coefficient column (both-sided disjunction). `<>` on REAL LHS is rejected.
- **Strict `<` / `>` rejection coverage** (`test_cons_comparison.py`): rejected on REAL LHS (per-row and aggregate), non-integer coefficient, mixed BOOL + REAL LHS, quadratic path (`SUM(POWER(real, 2))`), and bilinear Bool × Real path.
- **Strict `<` / `>` accepted shapes** (oracle-verified): pure INT SUM (per-row and aggregate), BOOL × INT bilinear LHS, INT × INT bilinear LHS (Gurobi), `SUM(ABS(integer_expr))`.
- Regression: `<=` / `>=` on REAL LHS still work.

### BETWEEN

- **Oracle-verified** (`test_cons_between.py`): per-row BETWEEN, fractional bounds on REAL var, column-derived bound (`x BETWEEN 0 AND col`) combined with multi-constraint + aggregate, aggregate BETWEEN standalone desugar (two constraints from one BETWEEN), and inside aggregate-local WHEN (`test_aggregate_local_when.py`).
- BETWEEN + entity-scoped: `test_entity_scope.py::test_entity_scoped_between_constraint` — constraint only (feasibility, not optimality).
- Error tests (`test_error_binder.py`): non-scalar RHS, DECIDE var in BETWEEN bounds.

### IN

- **Oracle-verified** (`test_cons_in.py`): `x IN (values)` on decision variable, non-integer values on REAL var, `x IN (0, 1)` on BOOL (no-op optimization), single-value IN rewritten to `x = v`, IN + WHEN composition.
- Error tests (`test_error_binder.py`): `SUM(x) IN (...)` rejected, DECIDE var in IN RHS rejected.

### Structural forms

All oracle-verified: per-row bounds (`test_cons_perrow.py`), aggregate
constraints (`test_cons_aggregate.py`), per-row + aggregate combined
(`test_cons_mixed.py`), multiple aggregate constraints (`test_cons_multi.py`),
subquery RHS (`test_cons_subquery.py`), correlated subquery RHS
(`test_cons_correlated_subquery.py`).

### Edge cases

All oracle-verified: RHS = 0 forces all zero, negative objective coefficients
(`test_edge_cases.py`); negative coefficients in aggregate constraints — both
signed column and negative constant literal multiplier
(`test_cons_comparison.py`).

## Feature interactions covered

| Feature A | Feature B | Tested |
|-----------|-----------|--------|
| Comparison operators | aggregate SUM | ✓ (all 6) |
| Comparison operators | per-row | ✓ (all 6) |
| `<>` | PER | ✓ |
| `<>` | entity-scoped | ✓ |
| `<>` | REAL LHS (rejected) | ✓ |
| BETWEEN | entity-scoped | ✓ |
| BETWEEN | aggregate-local WHEN | ✓ |
| BETWEEN | PER + REAL var (fractional bounds) | ✓ (`test_per_clause.py::test_real_between_per_oracle`) |
| IN | WHEN | ✓ |
| IN | BOOL domain restriction | ✓ |
| Negative coefficients | objective | ✓ |
| Negative coefficients | aggregate constraint (signed column) | ✓ |
| Negative coefficients | aggregate constraint (constant literal) | ✓ |
| Multiple constraints | different operators | ✓ |
