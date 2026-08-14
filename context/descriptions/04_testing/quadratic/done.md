# Quadratic Programming Test Coverage — Done

Tests live in:
- `test/decide/tests/test_quadratic.py` — QP/MIQP objectives (41 tests)
- `test/decide/tests/test_quadratic_constraints.py` — QCQP (quadratic constraints, 29 tests)

Covers `POWER(expr, 2)` via Q matrix construction. Convex forms run on both
solvers; non-convex and MIQP require Gurobi. Three syntax forms supported:
`POWER(expr, 2)`, `expr ** 2`, `(expr) * (expr)` self-product, plus negated
variants.

## Scenarios covered (objectives)

All oracle-verified; gurobi-gated cases noted:

- **Convex/concave core** (`test_quadratic.py`): `MINIMIZE SUM(POWER(x - target, 2))` (Q=PSD), `MAXIMIZE SUM(-POWER(x - target, 2))` (Q=NSD), simple squared variable, various coefficient forms, all three syntax forms (`POWER`, `** 2`, self-product), multiple variables, TPC-H data QP, entity-scoped QP objective.
- **Non-convex** `MAXIMIZE SUM(POWER(x, 2))` — gurobi-gated.
- **MIQP** (integer vars + convex QP, `test_maximize_convex_power_integer_case_b`) — gurobi-gated.
- **Mixed linear + quadratic**: same SUM (`SUM(POWER(x-t,2) + c*x)`), sibling SUMs, and negated quadratic + linear.
- **Composition**: QP + WHEN; QP objective + PER constraint (`test_per_interactions.py`).
- **Nested PER objectives**: `SUM(SUM(POWER(x-t,2))) PER grp` (binding per-group cap, unconstrained, and a constant-free regression), `SUM(AVG(POWER(x-t,2))) PER grp` with unequal groups (`test_quadratic.py`); hard-inner `SUM(MAX(POWER))` / `SUM(MIN(POWER))` PER variants in `test_per_objective.py` — gurobi-gated.

## Scenarios covered (constraints — QCQP)

All oracle-verified, in `test_quadratic_constraints.py`:

- **Core shapes**: per-row `POWER(expr, 2) <= K` (gurobi-gated), aggregate `SUM(POWER(expr, 2)) <= K`, zero budget (exact match), binding vs non-binding constraints, multi-variable inner expression, negated `-POWER(expr, 2)`, scaled `K * POWER(expr, 2)`, data-dependent coefficients.
- **Composition**: quadratic constraint + WHEN, + PER groups, + WHEN + PER (mask before group); multiple quadratic constraints per query; QCQP (quadratic objective + quadratic constraint); mixed linear + quadratic constraints; table-scoped (entity) + QP; REAL and INT variables; bilinear + self-product mixed.
- Infeasible quadratic constraint (negative budget) — error test, not oracle-verified.

## Error cases

- Exponent rejections (`test_quadratic.py`): `POWER(x, 3)`, variable exponents.
- Multiple POWER groups in one objective rejected (`test_quadratic.py`).
- Degree > 2 rejections — self-product of POWER, product of two POWERs, variable × POWER — each tested in both objective (`test_quadratic.py`) and constraint (`test_quadratic_constraints.py`) positions.
- HiGHS rejects non-convex QP and MIQP (`test_quadratic.py::TestHighsRejection` + `_expect_gurobi` pattern) and quadratic constraints (`test_quadratic_constraints.py`).
- Infeasible quadratic constraint with tight `match=` (`test_infeasible_negative_budget`).

## Feature interactions covered

| Feature A | Feature B | Tested |
|-----------|-----------|--------|
| QP objective | WHEN | ✓ |
| QP constraint | WHEN | ✓ |
| QP constraint | PER | ✓ |
| QP constraint | WHEN + PER | ✓ |
| QP objective | multiple variables | ✓ |
| QP constraint | multiple variables | ✓ |
| QP | BOOL (MIQP) | ✓ (gurobi-gated) |
| QP | INT (MIQP) | ✓ (gurobi-gated) |
| QP | REAL | ✓ |
| QP | entity-scoped | ✓ |
| QP constraint | bilinear (mixed) | ✓ |
| QP objective | PER constraint | ✓ |
| QP objective | linear terms in same SUM | ✓ |
| QP objective | linear terms in sibling SUM | ✓ |
| Negated QP objective | linear terms | ✓ |
| QP objective | nested PER outer-SUM | ✓ |
| QP objective | nested PER inner-MIN (hard) | ✓ |
| QP objective | nested PER inner-MAX (hard) | ✓ |
