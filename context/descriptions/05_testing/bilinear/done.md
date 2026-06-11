# Bilinear Term Test Coverage — Done

Tests live in `test/decide/tests/test_bilinear.py` (36 tests) plus interactions
in `test_aggregate_local_when.py` and `test_quadratic_constraints.py`.

Bilinear terms (`x * y`, two different DECIDE variables) split into two
categories:

1. **Boolean × anything**: exact MILP via McCormick envelopes, works with both
   solvers. Bool × Bool uses simpler AND-linearization.
2. **General non-convex** (Real×Real, Int×Int, Int×Real): Q matrix off-diagonal
   entries, Gurobi only (NonConvex=2).

## Scenarios covered

### McCormick (Boolean × anything — both solvers)

All oracle-verified, in `test_bilinear.py` unless noted:

- **Objectives**: Bool × Bool (AND-linearization), Bool × Real, Bool × Int; data-coefficient scaling (`profit * b * x`); MINIMIZE with data coefficient (both McCormick and AND-linearization shapes).
- **Constraints**: Bool × Bool constraints; Bool × Real constraint (McCormick feasibility).
- **Composition**: bilinear + WHEN, + PER (per-group McCormick aux), + WHEN + PER triple; entity-scoped Bool × row-scoped Real; aggregate-local WHEN on bilinear constraint and objective (`test_aggregate_local_when.py`).
- Backward compatibility: existing linear tests still pass.

### Non-convex (Gurobi only)

All oracle-verified; most are gurobi-gated (skipped/rejected on HiGHS-only hosts), in `test_bilinear.py`:

- **Objectives** (gurobi-gated): Real × Real, Int × Int, Int × Real; coefficients from both factor sides and shape-equivalence checks (split shape = flat product, grouped data factor `(a*b)*(x*y)` = `a*b*x*y`); expansion identities `(x+1)*y` = `x*y+y` and `(x+y)*z` = `x*z+y*z`.
- Mixed linear + bilinear objective (not gurobi-gated).
- **Constraints** (`TestBilinearConstraints`): coefficients from both factor sides, split-shape equivalence, data-column coefficients; grouped data-factor aggregate and per-row constraints `(a*b)*(x*y) >= K` (gurobi-gated).

### Error cases

In `test_bilinear.py` (`TestBilinearErrors` and `_expect_gurobi` pattern):
triple product (`x * y * z`) rejected; quad bilinear chain (`(b1*x)*(b2*y)`,
degree-4) rejected; missing upper bound on the non-Boolean factor in
Bool × non-Bool; HiGHS rejects non-convex bilinear.

## Feature interactions covered

| Feature A | Feature B | Tested |
|-----------|-----------|--------|
| Bilinear | Bool × Bool (AND-linearization) | ✓ |
| Bilinear | Bool × Real (McCormick) | ✓ |
| Bilinear | Bool × Int (McCormick) | ✓ |
| Bilinear | Real × Real (Gurobi Q) | ✓ |
| Bilinear | Int × Int (Gurobi Q) | ✓ |
| Bilinear | Int × Real (Gurobi Q) | ✓ |
| Bilinear | WHEN (expression-level) | ✓ |
| Bilinear | WHEN (aggregate-local) | ✓ |
| Bilinear | PER | ✓ |
| Bilinear | WHEN + PER (triple) | ✓ |
| Bilinear | Entity-scoped Boolean factor | ✓ |
| Bilinear | MAXIMIZE objective | ✓ |
| Bilinear | MINIMIZE objective (with data coefficient) | ✓ |
| Bilinear | constraint | ✓ (Bool × Bool, Bool × Real, and Gurobi coefficient regression cases) |
| Bilinear | linear terms (mixed) | ✓ |
| Bilinear | QP self-product (mixed) | ✓ |
