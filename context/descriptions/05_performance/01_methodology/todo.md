# Benchmark methodology

Quantify measurement overhead: an instrumented run against an uninstrumented one, isolated from solver logs and stack sampling. None of the current sweep does this.

Decide whether to bring back a lightweight correctness check now that `validation_sql` is gone — even a per-query objective sanity bound would catch a fast wrong answer, which the row-count-only check cannot.

Repeat the sweep on a second host, since every number so far comes from one machine. The large-tier sweep exists on this machine: `benchmark/decide/results/pipeline_profile_large.csv` (written 2026-09-09; 90 rows: 65 `ok`, 10 `solver_limit`, 7 `process_timeout`, 6 `unsupported`, 2 `skipped_memory`), with raw spans in `pipeline_profile_raw_large/` and the console log in `sweep_large.log`. No stage folder cites it yet; `06_issues/bugs/todo.md` cites its raw logs for the HiGHS time-limit bug.
