# DeciDB COP Optimizer

This folder documents the optimization strategies for Constrained Optimization Problem (COP) queries — the DECIDE clause portion of DeciDB. Each area is a **subfolder** with:

- `done.md` — What is implemented today, with code pointers
- `todo.md` — What remains to be built, with design rationale and implementation suggestions

---

## Capstone Priorities

| Priority | Area | Folder | Goal |
|----------|------|--------|------|
| 1 | Matrix efficiency | [matrix_efficiency/](matrix_efficiency/) | Make ILPs smaller and safer (bound conversion, solver timeout) |
| 2 | Partition-solve | [partition_solve/](partition_solve/) | Decompose PER queries into independent sub-ILPs |
| 3 | Rewrite passes | [rewrite_passes/](rewrite_passes/) | Push-down, pull-out, migrate binder rewrites to optimizer |

---

## All Folders

| Folder | done.md | todo.md |
|--------|---------|---------|
| [existing_optimizations/](existing_optimizations/) | WHERE filtering, WHEN zeroing, solver selection, binder algebraic rewrites (AVG/ABS/MIN/MAX/`<>`/IN), DecideOptimizer pass | *(none — reference only)* |
| [matrix_efficiency/](matrix_efficiency/) | Data-driven Big-M + implied-bound propagation | Constraint-to-bound conversion, solver time limit |
| [partition_solve/](partition_solve/) | `row_group_ids` foundation from PER | PER decomposition into K independent ILPs |
| [rewrite_passes/](rewrite_passes/) | Current binder + optimizer rewrite locations | Push-down, pull-out, binder-to-optimizer migration |
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
| MIN/MAX linearization (easy + hard) | **Implemented** | `rewrite_passes/done.md` |
| `<>` disjunction rewrite | **Implemented** | `rewrite_passes/done.md` |
| IN on decision variables | **Implemented** | `rewrite_passes/done.md` |
| DecideOptimizer pass | **Implemented** | `rewrite_passes/done.md` |
| Constraint-to-bound conversion | **Planned** | `matrix_efficiency/todo.md` |
| Solver time limit | **Planned** | `matrix_efficiency/todo.md` |
| Partition-solve (PER decomposition) | **Planned** (foundation ready) | `partition_solve/todo.md` |
| Constraint push-down | **Planned** | `rewrite_passes/todo.md` |
| Constraint pull-out | **Planned** | `rewrite_passes/todo.md` |
| Binder-to-optimizer migration | **Implemented** | `rewrite_passes/done.md` |
| Skyband indexing | **Future** | `future_work/todo.md` |
| Progressive Shading | **Future** | `future_work/todo.md` |
| LP relaxation + rounding | **Future** | `future_work/todo.md` |
| All other items | **Future** | `future_work/todo.md` |
