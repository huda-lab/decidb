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

## PERF-19 — A1's validation window on a row bound read from data

Every A1 plan whose values vary by row (a bound or an objective read from data) ends in DirectValidationBreaker: a
projection raises a bad row's error and a `COUNT(*) OVER ()` window holds the whole input, so a bad last row raises
even under an outer LIMIT 1. Late materialization keeps the window narrow by moving the variable-width columns out of
it, but only over a plan of projections, windows, filters and scans (`CollectPlanOperators`); a bound's side table
(PERF-15) adds a join and a CTE scan, so the window holds every column. Measured (lane AU, large.db lineitem alone,
16 columns, 1M rows, `MAXIMIZE sum(x * l_extendedprice)`, medians of 3, wall ms / peak MiB): bound a number 180 / 388,
a plain column 201 / 387, `sum(l_quantity) by (l_suppkey) / 20000` 592 / 362; a constant problem, pipelined, 39 ms.
The side table's own join and aggregate cost about 15 ms of CPU. The window scales negatively with threads on wide
rows (592 ms at 11 threads, 429 at 1). Design under review: verify, then stream (PERF-3's gated verdict on the
shared reader) in place of the breaker.
