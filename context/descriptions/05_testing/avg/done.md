# AVG Aggregate Test Coverage — Done

Tests live in `test/decide/tests/test_avg.py` (11 tests) plus interactions in
`test_aggregate_local_when.py` and `test_per_objective.py`.

## Scenarios covered

All oracle-verified unless noted:

- **Core** (`test_avg.py`): `AVG(x) op K` constraints, flat AVG objective (= SUM argmax), AVG + WHEN, AVG + PER (per-group average), AVG + WHEN + PER, BOOLEAN and INTEGER variables, AVG in bilinear constraint, `AVG(x) <> K` (Big-M + RHS scaling, plain and with WHEN), AVG with no DECIDE variable (passthrough, no oracle needed).
- **Aggregate-local WHEN** (`test_aggregate_local_when.py`): local WHEN on AVG, and local WHEN + AVG + PER.
- **Nested PER objectives** (`test_per_objective.py`): `SUM(AVG(x * cost)) PER col` with unequal groups, including extreme 1:5 group-size asymmetry; `MAX(AVG(...)) PER col` (easy MAX) and `MIN(AVG(...)) PER col` (easy MIN).

## Feature interactions covered

| Feature A | Feature B | Tested |
|-----------|-----------|--------|
| AVG | BOOLEAN | ✓ |
| AVG | INTEGER | ✓ |
| AVG | WHEN (expression-level) | ✓ |
| AVG | WHEN (aggregate-local) | ✓ |
| AVG | PER | ✓ |
| AVG | WHEN + PER | ✓ |
| AVG (inner) | SUM / MAX / MIN (outer) + PER | ✓ |
| AVG | bilinear constraint | ✓ |
| AVG | `<>` (NE Big-M) | ✓ (denominator hoisted to RHS at execution time; LHS stays integer-valued SUM — see `physical_decide.cpp` `ne_avg_rhs_scale` flag) |
| AVG | entity-scoped | ✓ (`test_entity_scope.py::test_entity_scoped_with_avg`) |
