# MIN/MAX Aggregate Test Coverage — Todo

## Missing coverage

### Composed MIN/MAX follow-ups

Hard-direction composed MIN/MAX (constraints and objectives) and scalar-
multiplied composed terms are now **shipped** — see `03_expressivity/such_that/done.md`
→ "Composed MIN/MAX (both directions)". The formerly-negative pins flipped to
oracle-verified positives in `test/decide/tests/test_min_max.py`:

- **Hard-direction** — `test_composed_minmax_hard_max_constraint`,
  `test_composed_minmax_objective_hard_max`, `test_composed_minmax_objective_hard_min`.
- **Scalar-multiplied** (`2 * MIN(...) + SUM(...)`) — `test_composed_minmax_scalar_mult_hard_min`
  (also guards the `ExtractCoefficient` fix that stopped dropping the nested `2`).

Remaining v2 shapes — still rejected at bind time, still pinned by negative tests:

- **Composed MIN/MAX with subtraction** (`MAX - MIN <= K`). Requires
  direction-flipping per term. Pinned by `test_composed_minmax_subtraction_rejected`.
- **Composed MIN/MAX with outer `PER`** (`MIN(...) + MIN(...) <= K PER grp`).
  Pinned by `test_composed_minmax_per_wrapper_rejected`.
- **Composed MIN/MAX with non-constant RHS**. Pinned by two tests —
  `test_composed_minmax_nonconst_rhs_subquery_rejected` (clean DECIDE-specific
  error: `Composed MIN/MAX in DECIDE v1 requires a constant RHS`) and
  `test_composed_minmax_nonconst_rhs_column_rejected` (generic SUM
  comparison error). Both pinned because they trip different binder paths.
- **Composed MIN/MAX with outer `WHEN`**. Pinned by
  `test_composed_minmax_outer_when_rejected` — the rejection path is the
  expression-vs-aggregate-local WHEN guard (distinct from the PER-pin's
  outer-WHEN/PER guard).

## Cross-references

- `PER + hard MIN/MAX` — see also `per/done.md`
- `entity_scope + hard MIN/MAX` — see also `entity_scope/done.md`
- Composed MIN/MAX expressivity — see `03_expressivity/such_that/done.md`
  and `03_expressivity/maximize_minimize/done.md`
