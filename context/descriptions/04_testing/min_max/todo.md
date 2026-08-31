# MIN/MAX Aggregate Test Coverage — Todo

## Make composed equality semantics consistent

The engine rejects an outer equality such as
`SUM(x * v) + MAX(x * v) = K`, but currently accepts the equivalent
`SUM(x * v) + MAX(x * v) BETWEEN K AND K`. That split is surprising and the
equal-endpoint spelling must not be accepted as a workaround.

Reject equal endpoints for composed MIN/MAX while direct equality remains
unsupported. Preserve ordinary non-degenerate `BETWEEN low AND high` support.
Because the parser currently lowers `BETWEEN` to two directional comparisons
before the composed rewrite sees it, the fix must retain or reconstruct enough
source provenance to distinguish this spelling without rejecting unrelated
conjunctions accidentally.

Keep `test_composed_minmax_outer_equality_rejected` and
`test_composed_minmax_equality_via_degenerate_between` as the current-behavior
pins until that rejection is implemented. Public documentation should describe
direct equality as unsupported without directing users around the guard.

## Not a coverage gap: v2 expressivity still deferred

These composed MIN/MAX shapes are rejected at bind time by design, and each
rejection is pinned by a negative test — so nothing is untested here. They are
listed only so the deferral stays visible; widening the grammar is an
expressivity decision, not a testing one.

- **Outer `PER`** (`MIN(...) + MIN(...) <= K PER grp`) —
  `test_composed_minmax_per_wrapper_rejected`.
- **Non-constant RHS** — `test_composed_minmax_nonconst_rhs_subquery_rejected`
  and `test_composed_minmax_nonconst_rhs_column_rejected`.
- **Outer `WHEN`** — `test_composed_minmax_outer_when_rejected`, which trips the
  expression-vs-aggregate-local WHEN guard rather than the outer WHEN/PER one.
- **Outer equality** is the open semantic inconsistency above, not merely a
  deferred expressivity shape.

## Cross-references

- `PER + hard MIN/MAX` — see also `per/done.md`
- `entity_scope + hard MIN/MAX` — see also `entity_scope/done.md`
- Composed MIN/MAX expressivity — see `03_expressivity/such_that/done.md`
  and `03_expressivity/maximize_minimize/done.md`
