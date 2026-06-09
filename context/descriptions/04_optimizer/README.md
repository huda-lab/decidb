# DeciDB COP Optimizer

This folder documents the optimization strategies for Constrained Optimization Problem (COP) queries — the DECIDE clause portion of DeciDB. Each area is a **subfolder** with:

- `done.md` — What is implemented today, with code pointers
- `todo.md` — What remains to be built, with design rationale and implementation suggestions

---

## Capstone Priorities

| Priority | Area | Folder | Goal |
|----------|------|--------|------|
| 1 | Matrix efficiency | [matrix_efficiency/](matrix_efficiency/) | Make ILPs smaller and safer (bound conversion, HiGHS timeout) |
| 2 | Rewrite passes | [rewrite_passes/](rewrite_passes/) | Push-down and pull-out rewrites |

---

## All Folders

| Folder | done.md | todo.md |
|--------|---------|---------|
| [existing_optimizations/](existing_optimizations/) | WHERE filtering, WHEN zeroing, solver selection, Gurobi time limit, algebraic rewrites (AVG/ABS/MIN/MAX/`<>`/bilinear/IN) | *(none — reference only)* |
| [matrix_efficiency/](matrix_efficiency/) | Data-driven Big-M + implied-bound propagation, Gurobi solver time limit | Constraint-to-bound conversion, HiGHS time limit |
| [rewrite_passes/](rewrite_passes/) | Algebraic rewrites in the DecideOptimizer pass | Push-down, pull-out |
| [future_work/](future_work/) | *(none)* | Skyband, Progressive Shading, LGS, LP relaxation, cuts, symmetry breaking, softening, hardening, incremental reasoning, cost-based selection, bound tightening |

---

## Implementation Status

| Optimization | Status | Location |
|---|---|---|
| WHERE-clause filtering | **Implemented** | Inherited from DuckDB (no DECIDE-specific code) |
| WHEN-condition coefficient zeroing | **Implemented** | `../01_pipeline/03b_coefficient_evaluation.md` |
| Solver selection (Gurobi/HiGHS fallback) | **Implemented** | `../01_pipeline/03d_solver_backends.md` |
| AVG → SUM rewrite | **Implemented** | `rewrite_passes/done.md` |
| ABS linearization | **Implemented** | `rewrite_passes/done.md` |
| Bilinear McCormick linearization | **Implemented** | `rewrite_passes/done.md` |
| MIN/MAX linearization (easy + hard) | **Implemented** | `rewrite_passes/done.md` |
| `<>` disjunction rewrite | **Implemented** | `rewrite_passes/done.md` |
| IN on decision variables | **Implemented** | `existing_optimizations/done.md` (symbolic phase) |
| DecideOptimizer pass | **Implemented** | `rewrite_passes/done.md` |
| Data-driven Big-M + implied-bound propagation | **Implemented** | `matrix_efficiency/done.md` |
| Solver time limit (Gurobi) | **Implemented** | `matrix_efficiency/done.md` |
| Solver time limit (HiGHS) | **Planned** | `matrix_efficiency/todo.md` |
| Constraint-to-bound conversion | **Planned** | `matrix_efficiency/todo.md` |
| Constraint push-down | **Planned** | `rewrite_passes/todo.md` |
| Constraint pull-out | **Planned** | `rewrite_passes/todo.md` |
| Skyband indexing | **Future** | `future_work/todo.md` |
| Progressive Shading | **Future** | `future_work/todo.md` |
| LP relaxation + rounding | **Future** | `future_work/todo.md` |
| All other items | **Future** | `future_work/todo.md` |
