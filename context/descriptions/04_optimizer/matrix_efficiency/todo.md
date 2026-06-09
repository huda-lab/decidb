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

---

## 2. HiGHS Time Limit

**Priority**: Medium

**Motivation**: The Gurobi backend now caps solve time (see `done.md` — 300s default, overridable via `DECIDB_TIME_LIMIT`). The HiGHS backend recognizes the `kTimeLimit` status when reporting results, but does not set an explicit `time_limit` option, so it runs with the HiGHS default (effectively unbounded). A hard problem on the HiGHS fallback path can still hang the session.

**Implementation**:
- HiGHS: `highs.setOptionValue("time_limit", seconds)` before `highs.run()`
- Read the same `DECIDB_TIME_LIMIT` env override the Gurobi backend uses, so both backends share one knob and one default
- HiGHS already returns the best feasible solution on `kTimeLimit`; only the option needs to be set

**Code locations to modify**:
- `src/decidb/naive/deterministic_naive.cpp` — add `highs.setOptionValue("time_limit", ...)` call alongside the existing `log_to_console` setup
