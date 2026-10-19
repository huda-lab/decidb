# AVG Aggregate Test Coverage — Done

Tests live in `test/decide/tests/test_avg.py` plus interactions in
`test_aggregate_local_when.py` and `test_per_objective.py`.

## Scenarios covered

All oracle-verified unless noted:

- **Core** (`test_avg.py`): `AVG(x) op K` constraints, flat AVG objective (= SUM argmax), `when c: avg(...)` (divides by the kept rows), `per g: avg(...) by (g)` (per-group average), `when c per g: avg(...) by (g)`, BOOL and INT variables, AVG in bilinear constraint, `AVG(x) <> K` (Big-M + RHS scaling, plain and under a `when`), AVG with no DECIDE variable (passthrough, no oracle needed).
- **A reducer's own `when`** (`test_aggregate_local_when.py`): `avg(when c: e)`, alone and under `per g: ... by (g)`.
- **Two-level objectives** (`test_per_objective.py`): `sum(per col: avg(x * cost) by (col))` with unequal groups, including extreme 1:5 group-size asymmetry; `max(per col: avg(...) by (col))` (easy MAX) and `min(per col: avg(...) by (col))` (easy MIN).

## Feature interactions covered

| Feature A | Feature B | Tested |
|-----------|-----------|--------|
| AVG | BOOL | ✓ |
| AVG | INT | ✓ |
| AVG | `when` in front | ✓ |
| AVG | a reducer's own `when` | ✓ |
| AVG | `per` | ✓ |
| AVG | `when` + `per` | ✓ |
| AVG (inner) | SUM / MAX / MIN (outer) in the two-level objective | ✓ |
| AVG | bilinear constraint | ✓ |
| AVG | `<>` (NE Big-M) | ✓ (denominator hoisted to RHS at execution time; LHS stays integer-valued SUM — see `physical_decide.cpp` `ne_avg_rhs_scale` flag) |
| AVG | keyed decision | ✓ (`test_entity_scope.py::test_entity_scoped_with_avg`) |
