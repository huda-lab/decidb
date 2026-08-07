# Existing Optimizations

Everything currently implemented that reduces ILP size, improves solve time, or transforms the problem before it reaches the solver. This is the "what's already in place" reference; the algebraic rewrites have their own inventory in [../rewrite_passes/done.md](../rewrite_passes/done.md).

---

## 1. WHERE-Clause Filtering (Inherited from DuckDB)

Standard DuckDB predicate pushdown. Rows eliminated by WHERE never enter the constraint/objective matrix. This is not a COP-specific optimization — it is inherited from DuckDB's query pipeline — but it is the single most impactful optimization for most queries, since each surviving row becomes a decision variable.

**Example**: `SELECT ... FROM items WHERE price < 100 DECIDE x(BOOL) ...` — only rows with `price < 100` become decision variables.

**Code**: No DECIDE-specific code; DuckDB's standard filter pushdown handles this before rows reach `PhysicalDecide`.

---

## 2. WHEN-Condition Coefficient Zeroing

When a constraint or objective has a `WHEN` modifier, rows that fail the condition are excluded without removing them from the variable set.

**Mechanism**:
- The WHEN condition is evaluated per row to produce a boolean mask.
- **Aggregate constraints**: coefficients are multiplied by the mask (0 for non-matching rows), so excluded rows contribute nothing to the aggregate.
- **Per-row constraints**: the constraint row is omitted entirely for non-matching rows.
- **Objectives**: the objective coefficient is zeroed for non-matching rows.

This interacts with PER via the unified `row_group_ids` architecture: WHEN-excluded rows get `INVALID_INDEX` in their group assignment, so they fall out of every group's constraint.

**Code pointer**: `src/execution/operator/decide/physical_decide.cpp` (WHEN mask evaluation and the filter-aware `row_group_ids` materialization). See [../../03_expressivity/per/done.md](../../03_expressivity/per/done.md) for the `row_group_ids` design.

---

## 3. Solver Selection (Gurobi / HiGHS Fallback)

Static dispatch: prefer Gurobi, fall back to HiGHS when Gurobi is unavailable.

- **Gurobi**: commercial (free academic license), empirically much faster on large ILPs, uses the Gurobi C API.
- **HiGHS**: open-source, bundled with DeciDB, slower but always available.

Selection is **not** cost-based — Gurobi is always preferred regardless of problem characteristics. The test suite can pin a backend with `DECIDB_FORCE_SOLVER=highs|gurobi`. See [../future_work/todo.md](../future_work/todo.md) for cost-based selection plans.

**Code pointers**:
- Dispatch + force-solver override: `src/decidb/utility/ilp_solver.cpp` (`SolveModel`)
- Gurobi backend: `src/decidb/gurobi/gurobi_solver.cpp` (`GurobiSolver::IsAvailable`, `GurobiSolver::Solve`)
- HiGHS backend: `src/decidb/naive/deterministic_naive.cpp` (`DeterministicNaive::Solve`)
- Model builder: `src/decidb/utility/ilp_model_builder.cpp` (`SolverModel::Build` converts `SolverInput` to the solver-agnostic matrix)

---

## 4. Solver Time Limit (Gurobi)

The Gurobi backend caps solve time (300s default, overridable via `DECIDB_TIME_LIMIT`) so a hard MIQP/QCQP cannot hang the session, returning the best feasible incumbent on timeout. See [../matrix_efficiency/done.md](../matrix_efficiency/done.md) for details. (The HiGHS fallback does not yet set an explicit limit.)

---

## 5. Algebraic Rewrites (DecideOptimizer Pass)

All DECIDE algebraic rewrites run in the **DecideOptimizer** logical-level pass after binding: ABS linearization, bilinear McCormick linearization, MIN/MAX linearization (easy + hard, flat and nested-PER), `<>` disjunction, and AVG → SUM. These reduce a DECIDE expression to a form the solver accepts and, where possible, shrink the problem. The binder validates and binds the expressions; the optimizer rewrites them.

`IN` / `NOT IN` over decision variables is handled earlier, in the symbolic phase (`src/decidb/symbolic/decide_symbolic.cpp`), which expands the membership test before binding.

**Full inventory with code pointers**: [../rewrite_passes/done.md](../rewrite_passes/done.md).
**Entry point**: `src/optimizer/decide/decide_optimizer.cpp` (`DecideOptimizer::OptimizeDecide`); header `src/include/duckdb/optimizer/decide_optimizer.hpp`.
