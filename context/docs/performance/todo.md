# Performance — open work

Open measurement questions, by stage of the solver path. Propose an optimization only where a measured cost supports
it. What is measured is in [findings.md](findings.md).

## PERF-1 — Methodology

Repeat the sweep on a second host, since every number so far comes from one machine. The large-tier sweep on this
machine: `benchmark/decide/results/pipeline_profile_large.csv` (written 2026-10-30 at d385eca667; 90 rows: 63 `ok`,
18 `solver_limit`, 3 `process_timeout`, 6 `unsupported`), with raw spans in `pipeline_profile_raw_large/` and the
console log in `sweep_large.log`. Its 3 `process_timeout` rows are Q1 on HiGHS, whose loops now stop at the limit
(`../pipeline/07_solver/done.md`). The 2026-09-09 sweep is kept in `pipeline_profile_large_20260909/` for comparison;
its rows predate the HiGHS limit fixes and the MIP gap of 0, so its HiGHS `ok` rows on Q1 are near-optimal answers,
not optima.

## PERF-6 — Solver execution

Run scale ladders and repetitions on both backends. Use separate native-log/stack-sample runs for slow cases. Extend solve budgets when that can resolve a concrete question. Do not equate timeouts with infeasibility or extrapolate small-model internal behavior. Keep algorithm/parameter changes deferred.

## PERF-8 — Memory and scaling

Find the point where each workload and backend stops fitting comfortably, and which stage holds the memory when it does. Relate peak RSS to model size — variables, rows and nonzeros are recorded alongside it in the same CSV row. The large-tier sweep (`benchmark/decide/results/pipeline_profile_large.csv`, 2026-10-30, 90 rows with `peak_rss_mib`; PERF-1) is not yet analysed here.

## PERF-22 — A pipelined plan over a copy of the source holds its wide columns

Late materialization is switched off for a pipelined plan (A1's stream), which holds no rows. But a stored source read
three times or more is copied (`DirectSourceCopyIsCheaper`), and the copy holds every row it keeps, the variable-width
columns the parent reads included; the copy decision also counted its width without them, because the reader takes the
driver's `stored_columns` before the rule says it pipelines. A1 with a bound read from a reducer is three reads. Measure
first (A1 with a reducer on large.db lineitem, a parent that reads every column), then make the rule "late
materialization applies when the plan holds rows, in a rank or a copy" rather than "when the plan is not pipelined".
