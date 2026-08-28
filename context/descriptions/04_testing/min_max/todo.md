# MIN/MAX Aggregate Test Coverage — Todo

No open coverage gaps. Shipped coverage is recorded in `done.md`.

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
- **Outer equality** (`SUM(...) + MAX(...) = K`) —
  `test_composed_minmax_outer_equality_rejected`. `BETWEEN K AND K` expresses
  the same bound and is accepted; see `done.md`.

## Cross-references

- `PER + hard MIN/MAX` — see also `per/done.md`
- `entity_scope + hard MIN/MAX` — see also `entity_scope/done.md`
- Composed MIN/MAX expressivity — see `03_expressivity/such_that/done.md`
  and `03_expressivity/maximize_minimize/done.md`
