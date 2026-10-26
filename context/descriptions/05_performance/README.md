# Performance: profile before optimizing

Scope: how DeciDB is benchmarked, and what the measurements show about the TPC-H DECIDE pipeline on Gurobi and
HiGHS. The [benchmarking](benchmarking/README.md) folder documents every benchmark, including the direct-solve
benchmark that times direct solve against the forced solver. The stage folders hold the solver-path findings. No
solver tuning is implemented in this work.

## Focus areas

| Folder | Owns |
| --- | --- |
| [benchmarking](benchmarking/README.md) | Every benchmark: what it measures, how to run it, where results go |
| [01_methodology](01_methodology/done.md) | Open methodology questions; the method itself is in `benchmarking/` |
| [02_planning](02_planning/done.md) | Parsing and planning |
| [03_input_and_extraction](03_input_and_extraction/done.md) | Input and coefficient extraction |
| [04_model_construction](04_model_construction/done.md) | Model construction |
| [05_backend_loading](05_backend_loading/done.md) | Backend loading |
| [06_solver_execution](06_solver_execution/done.md) | Solver execution |
| [07_output_and_cleanup](07_output_and_cleanup/done.md) | Output and cleanup |
| [08_memory_and_scaling](08_memory_and_scaling/done.md) | Memory and scaling |

Each stage folder has `done.md` for implemented measurement capabilities and evidence-backed findings, and `todo.md`
for unanswered questions and proposed work. Measuring a bottleneck does not complete its optimization.

## Evidence

- Stage findings come from the [pipeline profiler](benchmarking/pipeline_profiler.md),
  `benchmark/decide/profile_pipeline.py`: Q1 to Q11 and P1 to P4 on Gurobi and HiGHS, on the database its `DATABASE`
  constant names (`large` by default).
- Its results are `benchmark/decide/results/pipeline_profile_{tier}.csv`, one row per (query, backend, repeat), with
  raw spans beside it in `pipeline_profile_raw_{tier}/`. Both are gitignored, so retain them locally.
- Commit-to-commit timings come from the [TPC-H suite](benchmarking/tpch_suite.md); direct-solve speedups from the
  [direct-solve benchmark](benchmarking/direct_solve.md).
- See also [memory and scaling](08_memory_and_scaling/done.md).
