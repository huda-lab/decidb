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

A bare row-varying **data** column as the bound (`SUM(x) <= price`) was listed
here as still refused. Batch C opened it on 2026-08-25: a reduced constraint may be
bounded by a plain data column, and the group's tightest value wins for `<`/`<=`/`>`/`>=`.
Coverage lives in `test_reduced_bound_data_column.py` — see
`../../03_expressivity/such_that/done.md`.

## Scalar inside `norm(...)` and inside a quadratic objective — now reachable, not covered

Covered now: `ABS`, quadratic constraint, bilinear, `<>` — all in per-row
constraint position.

Not covered: a scalar inside `norm(...)`, and a scalar in a quadratic
**objective**. Both desugar into reducers (`norm(e, 1)` → `SUM(ABS(e))`, QP
objectives are `SUM(POWER(...))`), so until batch D these were unreachable — any
reducer containing a scalar was rejected. Batch D replaced that with the
row-invariance rule, so a body mixing a scalar with row-varying data now binds and
solves. Both shapes were confirmed to run on 2026-08-26:

```sql
SUCH THAT norm(l_linenumber * cap - 3, 1) <= 100      -- binds, solves
MINIMIZE SUM(POWER(l_linenumber * cap - 4, 2))        -- binds, solves
```

What is missing is an **oracle check** that the coefficient the scalar picks up
through those two desugarings is the same `Σ (counted rows' data)` weighting the
plain `SUM(cost * cap)` path was verified to use. The desugaring makes that
plausible, not proven.

LOW priority: two oracle tests in `test_scalar_scope.py`, no code expected.
