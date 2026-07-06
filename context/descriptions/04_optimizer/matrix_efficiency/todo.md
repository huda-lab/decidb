# Matrix Efficiency — Todo

Optimizations that make the ILP matrix smaller, tighter, or safer to solve.

---

## 1. Constraint-to-Bound Conversion

**Priority**: Medium

**Motivation**: Many user-written constraints are equivalent to simple variable bounds (e.g., `SUM(x) <= 10` when there's only one row, or `x <= 5` as a per-row constraint on a single variable). Solvers handle bounds much more efficiently than matrix constraints — bounds are O(1) per variable, while each matrix constraint adds a row to the LP tableau.

**Detection rules**:
- `x <= K` (single variable, no coefficient or coefficient = 1) → upper bound on x
- `x >= K` (single variable) → lower bound on x
- `x = K` (single variable) → fixed variable
- Only when the constraint applies to **all rows** (no WHEN or PER modifier), otherwise it's a conditional constraint that can't be expressed as a simple bound

**Implementation approach**:
1. After binding, scan constraints for single-variable patterns
2. Extract bound information and apply directly to `SolverInput` variable bounds
3. Remove the constraint from the matrix
4. Handle interactions: if multiple constraints bound the same variable, take the tightest

**Benefit**: Reduces matrix rows (fewer constraints for solver to process) and improves solver numerical stability.
