# Performance

How DeciDB is benchmarked, and what the measurements show about the TPC-H DECIDE pipeline on Gurobi and HiGHS.

| File | Holds |
|---|---|
| [benchmarking/](benchmarking/README.md) | Every benchmark: what it measures, how to run it, where results go |
| [findings.md](findings.md) | What the pipeline profiler measures at each stage of the solver path, and what its runs show |
| [todo.md](todo.md) | Open measurement questions, by stage |

## Evidence

- Stage findings come from the [../pipeline profiler](benchmarking/pipeline_profiler.md),
  `benchmark/decide/profile_pipeline.py`: Q1 to Q11 and P1 to P4 on Gurobi and HiGHS, on the database its `DATABASE`
  constant names (`large` by default).
- Its results are `benchmark/decide/results/pipeline_profile_{tier}.csv`, one row per (query, backend, repeat), with
  raw spans beside it in `pipeline_profile_raw_{tier}/`. Git ignores both, so keep them locally.
- Commit-to-commit timings come from the [TPC-H suite](benchmarking/tpch_suite.md); direct-solve speedups from the
  [direct-solve benchmark](benchmarking/direct_solve.md).
