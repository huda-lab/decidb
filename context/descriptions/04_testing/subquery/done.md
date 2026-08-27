# Subquery Test Coverage — Done

Tests live in:
- `test/decide/tests/test_cons_subquery.py` — uncorrelated scalar subquery RHS
- `test/decide/tests/test_cons_correlated_subquery.py` — correlated subquery RHS

## Scenarios covered

All oracle-verified:

- **Uncorrelated** (`test_cons_subquery.py`): scalar subquery as constraint RHS, aggregate subquery (AVG, SUM, etc.), scalar subquery as PER constraint RHS (shared across all groups).
- **Correlated** (`test_cons_correlated_subquery.py`): per-row bound, AVG subquery as per-row bound on REAL, boolean gate, objective coefficients, WHEN composition, NULL-producing correlated with COALESCE.

### Error cases

`test_error_binder.py`: correlated aggregate RHS non-scalar, correlated PER
RHS non-scalar, subquery referencing a DECIDE variable (tight match), scalar
subquery returning multiple rows.

## Feature interactions covered

| Feature A | Feature B | Tested |
|-----------|-----------|--------|
| Uncorrelated scalar subquery | constraint RHS | ✓ |
| Uncorrelated scalar subquery | PER constraint RHS | ✓ |
| Correlated subquery | per-row bound | ✓ |
| Correlated subquery | aggregate constraint | ✓ |
| Correlated subquery | objective | ✓ |
| Correlated subquery | WHEN | ✓ |
| Correlated subquery | BOOL | ✓ |
| Correlated subquery | INT | ✓ |
| Correlated subquery | REAL | ✓ |
| Subquery | DECIDE variable (rejected) | ✓ (error test) |

## Caveats

Per [oracle.md](../../02_operations/oracle.md) section 9: subqueries inside
SUCH THAT don't inherit the connection's `search_path`. Tables must be fully
qualified (e.g. `tpch.customer`) when subqueries reference cross-schema data.
