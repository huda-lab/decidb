# Rewrite Passes — Todo

New optimizer-level rewrites. (The migration of the existing algebraic rewrites from the binder into the optimizer is complete — see `done.md`.)

---

## 1. Constraint Push-Down

**Priority**: High

Push constraint evaluation closer to the data access layer to eliminate rows that cannot participate in any feasible solution.

**Approach**: Analyze constraint bounds to derive row-level implications. If a row's data makes it impossible for that row's decision variable to contribute to any feasible solution, prune it before building the ILP matrix.

**Example**: If `x IS BOOLEAN` and the only constraint involving x is `SUM(x * weight) <= 40`, then any row where `weight > 40` can never have `x = 1` in a feasible solution (a single such row would violate the constraint). These rows can be pruned.

**Requirements**:
- Bound analysis over constraint coefficients
- Must be conservative (only prune rows that are provably infeasible)
- Interacts with WHEN conditions (WHEN-excluded rows are already effectively pruned)

**Benefit**: Reduces number of decision variables and constraint matrix size. Most impactful for queries with tight constraints on large tables.

---

## 2. Constraint Pull-Out

**Priority**: Medium

Extract common sub-expressions from PER-generated constraints. When PER creates K copies of a constraint (one per group), many coefficient computations are shared across groups.

**Example**: `SUM(x * weight) <= 40 PER empID` with 1000 employees. The `weight` column values are the same regardless of which employee group we're computing — we only need to compute the coefficient vector once, then mask it K times.

**Current behavior**: Each group's constraint coefficients are computed independently during ILP model building. With pull-out, shared computation would be factored out.

**Requirements**:
- Identify which coefficient computations are group-independent
- Factor out shared work and apply group masks
- Most valuable when PER cardinality is high (many groups)

**Benefit**: Reduces ILP construction time (not solve time). Becomes critical for high-cardinality PER columns (1000+ distinct values).

---

*(The binder-to-optimizer migration is complete — see `done.md` for the current rewrite inventory.)*
