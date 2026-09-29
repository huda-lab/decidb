# Performance: profile before optimizing

Current scope: characterize the existing TPC-H DECIDE pipeline on **Gurobi and HiGHS**. No tuning, optimization or direct-solver comparison is implemented in this work.

## Focus areas

| Folder | Owns |
| --- | --- |
| [01_methodology](01_methodology/done.md) | Benchmark methodology |
| [02_planning](02_planning/done.md) | Parsing and planning |
| [03_input_and_extraction](03_input_and_extraction/done.md) | Input and coefficient extraction |
| [04_model_construction](04_model_construction/done.md) | Model construction |
| [05_backend_loading](05_backend_loading/done.md) | Backend loading |
| [06_solver_execution](06_solver_execution/done.md) | Solver execution |
| [07_output_and_cleanup](07_output_and_cleanup/done.md) | Output and cleanup |
| [08_memory_and_scaling](08_memory_and_scaling/done.md) | Memory and scaling |

Each folder has `done.md` for implemented measurement capabilities and evidence-backed findings, and `todo.md` for unanswered questions and proposed work. Measuring a bottleneck does not complete its optimization.

## Evidence

- Runner: `benchmark/decide/profile_pipeline.py` (Q1–Q11 and P1–P4 on Gurobi and HiGHS, medium database).
- Results: `benchmark/decide/results/pipeline_profile_{tier}.csv`, one row per (query, backend, repeat); raw spans beside it in `pipeline_profile_raw_{tier}/`. Both are gitignored, so retain them locally.
- [Benchmark operations](../02_operations/benchmarking.md), [methodology](01_methodology/done.md), [memory and scaling](08_memory_and_scaling/done.md).

The old commit-by-commit log is replaced by these topic-owned documents. Measured priorities will be recorded once the sweep has run.
