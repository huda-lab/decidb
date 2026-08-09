# Query-Wide (`scalar`) Scope Test Coverage — Todo

## Scalar with `PER` — blocked on group B, not merely untested

This area's first draft listed "scalar with PER" as an untested but well-defined
shape. That was wrong, and the correction is worth keeping: the shape **does not
bind**.

```
SUCH THAT SUM(x) <= cap PER c_nationkey
→ Binder Error: SUM cannot be compared to an expression that is not a scalar or
  aggregate without DECIDE variables
```

The rejection has nothing to do with `PER` — `SUM(x) <= cap` fails the same way
without it. It is the general "non-reduced RHS on a reduced constraint" check,
filed as **B1 / B2 / B4** in `context/descriptions/todo.md`. This matters more
than an ordinary gap because it is the shape the **paper writes with `scalar`**:
§3.1's `demand - sum(ship) <= max_shortfall per regionID`.

Both rejections are pinned by `test_scalar_as_aggregate_rhs_rejected` and
`test_scalar_as_aggregate_rhs_with_per_rejected`, so these tests flip when group
B lands and are the natural place to add the positive coverage then.

## Scalar composed with the remaining rewrites

Covered now: `ABS`, quadratic constraint, bilinear, `<>` — all in per-row
constraint position, which is the only position a scalar can reach them from,
since a scalar inside a reducer is rejected (A3) and objectives must be
aggregates.

Not covered, and not currently reachable: a scalar inside `norm(...)`, or in a
quadratic **objective**. Both desugar into reducers (`norm(e, 1)` → `SUM(ABS(e))`,
QP objectives are `SUM(POWER(...))`), so they hit the same A3 rejection. Nothing
to test until that rule changes — and if it ever does, the coefficient question
it was protecting against (1 vs. `n`) has to be answered first.

LOW priority: these are consequences of a settled decision, not gaps.
