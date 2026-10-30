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

## PERF-20 — S1 and R1 lose late materialization under a side table or a row-check gate

Late materialization moves a stored source's variable-width columns out of the operators that hold rows and joins
them back by row id, but only over a plan of projections, windows, filters and scans (`CollectPlanOperators`,
`direct_late_materialization.cpp`). A bound's side table (a SINGLE join and a CTE scan, PERF-15) or the row checks'
gate (a filter with a `projection_map` over a cross product, PERF-3) makes it refuse, so S1's and R1's held rows carry
every column. Measure first: S1 and R1 on large.db lineitem (16 columns) with a bound read from data and with a
planned row check, against the same query without them. A fix follows the row id's binding through any operator
rather than widening the list (A1 holds no rows since PERF-19, so it is not affected).

## PERF-21 — A materialized copy of the source keeps every column

The source reader copies a source once when the plan reads it often enough (`DirectSourceCopyIsCheaper`, three reads
or more) or when it cannot be re-scanned (a join). The copy is a materialized CTE's definition, which unused-column
removal keeps whole because it does not follow CTE refs, so it holds every source column whatever the reads use.
Measured (lane AU, large.db lineitem, 16 columns, 1M rows, A1 with a bound read from a reducer: three reads): 248 ms
and 512 MiB copied against 277 ms and 272 MiB recomputed (forced by `memory_limit`), so the copy buys about 30 ms for
about 240 MiB; lineitem JOIN supplier rises 160 MiB the same way. Find the root fix: prune a CTE definition to the
columns its refs read (DuckDB's `ColumnLifetimeAnalyzer` treats a materialized CTE the same blanket way), or count
the copy's width in the copy decision.
