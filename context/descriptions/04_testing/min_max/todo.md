# MIN/MAX Aggregate Test Coverage — Todo

## Missing coverage

### Composed MIN/MAX follow-ups

Remaining v2 shapes — still rejected at bind time, still pinned by negative tests:

- **Composed MIN/MAX with outer `PER`** (`MIN(...) + MIN(...) <= K PER grp`).
  Pinned by `test_composed_minmax_per_wrapper_rejected`.
- **Composed MIN/MAX with non-constant RHS**. Pinned by two tests —
  `test_composed_minmax_nonconst_rhs_subquery_rejected` (clean DECIDE-specific
  error: `Composed MIN/MAX in DECIDE v1 requires a constant RHS`) and
  `test_composed_minmax_nonconst_rhs_column_rejected` (the same dedicated
  rejection after bare data-column bounds became legal generally).
- **Composed MIN/MAX with outer `WHEN`**. Pinned by
  `test_composed_minmax_outer_when_rejected` — the rejection path is the
  expression-vs-aggregate-local WHEN guard (distinct from the PER-pin's
  outer-WHEN/PER guard).
- **Composed MIN/MAX with outer equality** (`SUM(...) + MAX(...) = K`). The
  additive composed path currently accepts only directional comparisons. The
  rejection is not yet pinned by a focused test.

## Cross-references

- `PER + hard MIN/MAX` — see also `per/done.md`
- `entity_scope + hard MIN/MAX` — see also `entity_scope/done.md`
- Composed MIN/MAX expressivity — see `03_expressivity/such_that/done.md`
  and `03_expressivity/maximize_minimize/done.md`
