# Subquery Test Coverage — Done

Tests live in:
- `test/decide/tests/test_cons_subquery.py` — uncorrelated scalar subquery RHS
- `test/decide/tests/test_cons_correlated_subquery.py` — correlated subquery RHS

## Scenarios covered

All oracle-verified:

- **Uncorrelated** (`test_cons_subquery.py`): scalar subquery as constraint RHS, aggregate subquery (AVG, SUM, etc.), an uncorrelated scalar subquery as the bound of a `per` constraint (one value for every class).
- **Correlated** (`test_cons_correlated_subquery.py`): per-row bound, AVG subquery as per-row bound on REAL, boolean gate, objective coefficients, under a `when`, NULL-producing correlated with COALESCE.
- **A correlated bound that varies across rows** (`test_cons_correlated_subquery.py`):
  with no `per` key every row bounds the reducer, so the tightest bound applies
  (`test_correlated_subquery_aggregate_rhs_tightest_bound`; per class under
  `per ps_suppkey:`, where every class has one row, in
  `test_correlated_subquery_aggregate_per_rhs_tightest_per_group`); a reducer's own
  `when` does not narrow the rows the bound is read on
  (`test_correlated_subquery_rhs_ignores_aggregate_local_when`). Under a `per` key each
  value a bound takes within a class is a variant of its own (`test_scope_variants.py`).

### Error cases

`test_error_binder.py`: a correlated bound that takes two values under `=`
(`test_correlated_subquery_aggregate_equality_rhs_rejected`), a subquery bound
referencing a DECIDE variable (tight match, `test_subquery_rhs_non_scalar`), a
scalar subquery returning multiple rows.

## Feature interactions covered

| Feature A | Feature B | Tested |
|-----------|-----------|--------|
| Uncorrelated scalar subquery | constraint RHS | ✓ |
| Uncorrelated scalar subquery | `per` constraint bound | ✓ |
| Correlated subquery | per-row bound | ✓ |
| Correlated subquery | aggregate constraint | ✓ |
| Correlated subquery | objective | ✓ |
| Correlated subquery | `when` | ✓ |
| Correlated subquery | BOOL | ✓ |
| Correlated subquery | INT | ✓ |
| Correlated subquery | REAL | ✓ |
| Subquery | DECIDE variable (rejected) | ✓ (error test) |

## Caveats

Per [oracle.md](../../02_operations/oracle.md) §8, caveat 5: subqueries inside
SUCH THAT don't inherit the connection's `search_path`. Tables must be fully
qualified (e.g. `tpch.customer`) when subqueries reference cross-schema data.
