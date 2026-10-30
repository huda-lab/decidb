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

## PERF-15 — RHS-bound windows hold every source column

The bounds a clause's right-hand side reads from data (`decide_rewrite_rhs_window.cpp`) are a stack of
LogicalWindows above the DECIDE source; each holds every source column to attach one value per group. Measured
(lane AS, large.db, 1M-row lineitem JOIN supplier, medians of 3, bound read from data against the same bound written
as a number): on the direct path R1 and A1 compute a same-key group bound themselves, with no window; the shapes left
(a bound reducer by a coarser key, a row bound read from a reducer) cost +45 to +121 ms and +135 to +178 MiB on a
6-column source, +150 to +181 ms and +540 to +580 MiB on a 23-column one. On the solver path a bound costs +24 to +76
ms before the solve on 6 columns, +136 to +713 ms and 0.56 to 1 GiB on 23, more for each bound on a different key.
Emulated in SQL, a grouped aggregate on a recomputed read joined back by the null-safe key costs +4 to +10 ms and
about +30 MiB on a single-table source; on a join source materialized as a CTE it ties the windows. Design under
review: decorrelate each bound as a scalar-aggregate subquery is unnested (PERF-3's side aggregate on the shared
reader).

## PERF-19 — A1's validation window on a row bound read from data

With PERF-15's side tables, a row bound read from a reducer (`x <= sum(q) by (g) / 20000`) on A1 still costs about
+419 ms against the same bound written as a number (lineitem alone, 23 columns, 1M rows; lane AS): A1 validates the
bound in a window over the source, which a constant bound does not need, so that plan is pipelined. The side table's
join costs about 11 ms of it. Find what the validation window computes and whether a side aggregate (PERF-3's shape)
or the side table itself can carry it.
