# Query-Wide (`scalar`) Scope Test Coverage — Todo

## Scalar with `PER` — shipped at the canonicalization refactor (2026-08-12)

This area's first draft listed "scalar with PER" as an untested but well-defined
shape; a later correction said it did not bind at all. Both are now history.

```
SUCH THAT SUM(x) <= cap PER c_nationkey
```

used to fail the general "non-reduced RHS on a reduced constraint" check, which
had nothing to do with `PER` — `SUM(x) <= cap` failed the same way without it.
The canonicalization refactor made the binder's constraint gate side-agnostic, so a
decision may be the bound; canonicalization moves `cap` to the model side and the
shape reaching the physical layer is the `SUM(x) - cap <= 0` that B.3 already
handled. The paper's §3.1 constraint
(`demand - sum(ship) <= max_shortfall per regionID`) binds and solves in full,
including the row-varying `demand` that canonicalization sends to the bound side
for B.5's runtime reduction.

Coverage: `test_scalar_scope.py::test_scalar_as_aggregate_rhs` and
`::test_scalar_as_aggregate_rhs_with_per` (the two rejection pins, converted to
oracle-verified positives priced so the optimum is interior rather than all-zero),
`test_canonicalize_side_agnostic.py::test_data_term_left_of_reducer`, and golden
corpus 75/76/77.

What is *not* opened: a bare row-varying **data** column as the bound
(`SUM(x) <= price`). That is still group B of the paper sweep and needs per-tuple
fan-out at the binder; it is now refused identically on either side.

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
