# Benchmarking

Every benchmark DeciDB has, what each one answers, and how to run it. All of them live under `benchmark/decide/`,
need the release CLI (`make release`, which builds `build/release/decidb`), and write to `benchmark/decide/results/`.
Databases and results are gitignored (`benchmark/decide/.gitignore`), so keep the files a finding cites.

| Benchmark | Answers | Code | Doc |
|---|---|---|---|
| TPC-H suite | How long does each DECIDE query in a fixed set take on the solver path, and did a commit change that? | `run_benchmarks.py`, `queries/q*.sql` | [tpch_suite.md](tpch_suite.md) |
| Pipeline profiler | Where inside the solver path does the time and memory go, stage by stage, on Gurobi and on HiGHS? | `profile_pipeline.py`, `queries/q*.sql`, `queries/p*.sql` | [pipeline_profiler.md](pipeline_profiler.md) |
| Direct-solve benchmark | How much faster is direct solve than the forced solver on the same query, as the data grows? | `direct_solve/run.py`, `direct_solve/queries/*.sql` | [direct_solve.md](direct_solve.md) |
| Archived direct-solve profilers | The measurements each direct-solve rule's `done.md` cites | `direct_solve/archive/` | [archive_profilers.md](archive_profilers.md) |
| Oracle-checked scale tests | Does DECIDE still match an independent solver on a few hundred to a few thousand rows? | `test_large_scale.py` | [scale_tests.md](scale_tests.md) |

## Which one to use

- A change to the solver path (rewrites, model construction, a backend): run the TPC-H suite before and after, with
  `--compare`. Narrow it with `--queries` and `--sizes medium` while iterating.
- A question about which stage is slow or holds the memory: run the pipeline profiler.
- A change to a direct-solve rule, or numbers for the paper's direct-solve section: run the direct-solve benchmark.
- A deeper question about one rule (thread scaling, operator profile, API delivery): start from the archived profiler
  that rule's `done.md` cites.

## Run commands

```bash
make decide-bench-setup && make decide-bench                    # TPC-H suite: databases once, then every query
python3 benchmark/decide/profile_pipeline.py                     # pipeline profiler (constants at the top of the file)
python3 benchmark/decide/direct_solve/run.py                     # direct solve against Gurobi and HiGHS
python3 benchmark/decide/direct_solve/archive/<profiler>.py -h   # one archived profiler; see archive_profilers.md
```

The pipeline profiler and the direct-solve benchmark force each backend in turn, so both need a Gurobi license.
HiGHS is built into the CLI.

## Recording results

Findings belong in the stage folders of [`05_performance/`](../README.md) or in the rule folders of
[`08_direct_solve/rules/`](../../08_direct_solve/README.md). Put measured, evidence-backed findings in `done.md` and
open questions in `todo.md`. Cite the exact command, the commit, the machine and the result file. Measuring a
bottleneck does not complete its optimization.
