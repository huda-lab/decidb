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

## PERF-3 — A passing determination check materializes every row

In progress (lane AN). A `per` key that must determine another column gets a planned determination check
(`../language/guide.md` §7.3). On 1M rows (`per l.l_orderkey: assign <= o.o_shippriority`, `findings.md` § Input and
extraction) the check costs about 480 ms of wall time and 860 MiB of peak memory while it passes, five times
constraint evaluation: three window operators each materialize every row with every column, for the per-class
count/min/max (569 ms CPU), a dense rank of all rows by the key to pick the least failing class (877 ms) and a global
min for the message (125 ms). Ruled fix: a side aggregate over only the columns the check reads; the message is
unchanged.

## PERF-6 — Solver execution

Run scale ladders and repetitions on both backends. Use separate native-log/stack-sample runs for slow cases. Extend solve budgets when that can resolve a concrete question. Do not equate timeouts with infeasibility or extrapolate small-model internal behavior. Keep algorithm/parameter changes deferred.

## PERF-8 — Memory and scaling

Find the point where each workload and backend stops fitting comfortably, and which stage holds the memory when it does. Relate peak RSS to model size — variables, rows and nonzeros are recorded alongside it in the same CSV row. The large-tier sweep (`benchmark/decide/results/pipeline_profile_large.csv`, 2026-10-30, 90 rows with `peak_rss_mib`; PERF-1) is not yet analysed here.

## PERF-13 — Q8's key values

`BuildGroupIds` keeps one `Value` per class per key column as each class's representative (`out_rep_keys`): 250k
classes × 9 columns for Q8's `per o`. They cost 143 ms to build in `execution.keyed_mapping`, 45 ms to destroy in
`solver.cleanup` and 360 MiB of peak memory (`findings.md` § Input and extraction: against `per o.o_orderkey` on the
same query). Proposal: keep each group's representative row (`group_rows`) instead of its values, order the groups
by a sort over the key columns, and format values only when a message names a group (late materialization).

## PERF-14 — Q4's composed MIN/MAX evaluation

Q4's `execution.evaluate_composed` takes 495 ms on Gurobi and on HiGHS, the largest extraction cost on the benchmark
(`findings.md` § Input and extraction). No cause is found yet: find where the time goes inside the evaluation before
proposing a change.

## Regressions since the 2026-09-09 sweep

The 2026-10-30 large sweep (PERF-1) against the 2026-09-09 one, on Gurobi, where every run finishes in both: total
time fell 3 % (smaller models: per-row bounds fold into column boxes), and the non-solver stages grew in a few
queries. The scope checks a key always passes caused most of it and are fixed (Q8 4.0 s and 3.95 GiB → 1.7 s and
1.14 GiB); so are the group ids and the `when` scan (PERF-9, PERF-11 closed: `findings.md`). Each task: find
the cause with the profile spans (`profile_pipeline.py`
raw `profile.json`, one query at `large`), fix it if it is a defect, else record why the cost is needed. Record in
`findings.md`, and in the owning stage's `done.md` if code changes.

### PERF-10 — Implied bounds, the admitted interval and setup still slower than September

After the extraction fixes (`findings.md` § Regressions since 2026-09-09), measured alone at `large`: implied bounds Q1
13.6 → 49 ms, Q4 14 → 49, P4 7 → 25; Q4's `extraction.rhs` 74 ms per 1M rows; Q1's linear coefficients 31 ms against
15.6; a grouped clause's group view 13-19 ms against about 7.5. Causes and proposals, each to rule:

- Implied bounds (78ef6ec5bf): every emitted row goes through `AccumulateClauseRow` and `Flush` (Q1: accumulate 14,
  flush 10, bound loop 12, instance loop 5 ms); the CSR moved here from the builder, whose time fell about as much.
  Proposal: build each clause's emitted rows once as a CSR shared by implied bounds, the `<>` expansion and the builder
  (late materialization). Changes no result.
- The admitted interval (5fdd4659c5): an easy MAX's `AdmittedInterval` is two CASE trees evaluated per row with
  nothing shared. Proposal: evaluate both ends in one projection through DuckDB's common-subexpression elimination.
- The NULL test: about 5 ms per clause scanning for it, and the objective sites still carry identity NULL-test filters
  (Q1 `evaluate_objective` 24 → 32 ms). Proposal: drop the test at plan time where DuckDB's statistics prove the column
  never NULL, as `StatisticsPropagator` folds `IS NOT NULL`; the objective sites treat an identity filter as none, as
  the constraint sites now do.
- Term filters read on the kept rows only (`KeepChunkRowsWhere` / `ExecuteOnReadRows`, a per-row `vector<bool>`
  selection): 9-30 ms. A `SelectionVector` path is faster, but DuckDB's conjunction `Select` reorders AND/OR operands
  adaptively, and DECIDE reads them left to right; a design trade-off.
- The `masks[i].size() != num_rows` InternalException in `EvaluateConstraints` cannot fire: delete it.
