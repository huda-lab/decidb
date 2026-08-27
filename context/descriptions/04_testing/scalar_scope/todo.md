# Query-Wide (`scalar`) Scope Test Coverage — Todo

## Scalar inside `norm(...)` and inside a quadratic objective — now reachable, not covered

Covered now: `ABS`, quadratic constraint, bilinear, `<>` — all in per-row
constraint position.

Not covered: a scalar inside `norm(...)`, and a scalar in a quadratic
**objective**. Both desugar into reducers (`norm(e, 1)` → `SUM(ABS(e))`, QP
objectives are `SUM(POWER(...))`). A body mixing a scalar with row-varying data now
binds and solves:

```sql
SUCH THAT norm(l_linenumber * cap - 3, 1) <= 100      -- binds, solves
MINIMIZE SUM(POWER(l_linenumber * cap - 4, 2))        -- binds, solves
```

What is missing is an **oracle check** that the coefficient the scalar picks up
through those two desugarings is the same `Σ (counted rows' data)` weighting the
plain `SUM(cost * cap)` path was verified to use. The desugaring makes that
plausible, not proven.

**Priority: low.** Add two oracle tests in `test_scalar_scope.py`; no code change is
expected.
