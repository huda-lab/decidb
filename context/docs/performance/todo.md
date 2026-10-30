# Performance — open work

Open measurement questions, by stage of the solver path. Propose an optimization only where a measured cost supports
it. What is measured is in [findings.md](findings.md).

## PERF-1 — Methodology

Quantify measurement overhead: an instrumented run against an uninstrumented one, isolated from solver logs and stack sampling. None of the current sweep does this.

Decide whether to add a lightweight correctness check: the profiler checks row counts only, and even a per-query objective sanity bound would catch a fast wrong answer, which the row-count-only check cannot.

Repeat the sweep on a second host, since every number so far comes from one machine. The large-tier sweep on this machine: `benchmark/decide/results/pipeline_profile_large.csv` (written 2026-10-30 at d385eca667; 90 rows: 63 `ok`, 18 `solver_limit`, 3 `process_timeout`, 6 `unsupported`), with raw spans in `pipeline_profile_raw_large/` and the console log in `sweep_large.log`. No finding cites it yet. Its 3 `process_timeout` rows are Q1 on HiGHS, whose loops now stop at the limit (`../pipeline/07_solver/done.md`). Q8's non-solver time and peak memory grew fivefold since the 2026-09-09 sweep, mostly scope checks a key always passes, which are no longer built (`../pipeline/04_canonicalizer/done.md`; Q8 1.7 s and 1.1 GiB after, against 0.7 s and 0.7 GiB in 2026-09); the rest is the keyed-decision mapping (229 to 1,169 ms, PERF-3). That sweep is kept in `pipeline_profile_large_20260909/` for comparison; its rows predate the HiGHS limit fixes and the MIP gap of 0, so its HiGHS `ok` rows on Q1 are near-optimal answers, not optima.

## PERF-2 — Planning

Quantify first-use backend discovery versus parsing, binding and rewriting. Distinguish fresh-process costs from repeated-query costs before proposing caching or environment reuse. Do not optimize sub-millisecond noise.

## PERF-3 — Input and extraction

Rank repeated scans and coefficient/RHS work by workload and clause. Compare Q8's keyed-decision mapping, Q1's low-cardinality `per` classes and P4's order-key classes. Propose batching/caching only where supported by measured cost.

`profile_pipeline.py` puts `execution.scope_checks` and `execution.evaluate_value_links` in no stage column, so their time shows only inside `non_solver_ms`. The scope checks (`../language/guide.md` §7.3) run before constraint evaluation on every query with a `when` or a `per` key, one pass over the kept rows per check. Add both spans to the `extraction` stage (each stage names parent spans only, so no time is counted twice), then rank the checks against constraint evaluation. No benchmark query builds a `DETERMINES` check since a key's own elements need none (`../pipeline/04_canonicalizer/done.md` § Scope checks), so measure on a query whose `per` key must determine another column. Record the result in `findings.md`.

## PERF-4 — Model construction

Relate model time and boundary memory jumps to actual row/nonzero expansion and simultaneously live representations. Rank the expensive formulation families. Preserve provenance, diagnosis and backend contracts in any later reduction.

**A helper decision's box, per instance.** Each instance of a helper of the value rewrite (`../language/guide.md` §6.4) is boxed by its own group's reach, and a `MIN`/`MAX` helper's closing Big-M reads that box (`../pipeline/06_model_formulation/done.md`, "Helper decisions of the value rewrite"). Not measured: node count and solve time on a `max(e) by (g)` helper pinned from both sides beside a per-row decision (`x + 1 = max(x * v) by (g)`), two groups whose reach differs by orders of magnitude, against a model boxed by the cover of all groups. Record the finding in `findings.md`.

## PERF-5 — Backend loading

Quantify Gurobi environment and per-row API overhead before considering reuse/bulk loading. For HiGHS, isolate coordinate arrays, compressed packing and backend conversion time/memory before proposing ownership changes.

## PERF-6 — Solver execution

Run scale ladders and repetitions on both backends. Use separate native-log/stack-sample runs for slow cases. Extend solve budgets when that can resolve a concrete question. Do not equate timeouts with infeasibility or extrapolate small-model internal behavior. Keep algorithm/parameter changes deferred.

## PERF-7 — Output and cleanup

Quantify readback, temporary-result writing and destruction after fast solves. Preserve full row production and the keyed-decision mapping. Investigate expensive residual time rather than assigning it to a guessed phase.

## PERF-8 — Memory and scaling

Find the point where each workload and backend stops fitting comfortably, and which stage holds the memory when it does. Relate peak RSS to model size — variables, rows and nonzeros are recorded alongside it in the same CSV row. The large-tier sweep (`benchmark/decide/results/pipeline_profile_large.csv`, 2026-10-30, 90 rows with `peak_rss_mib`; PERF-1) is not yet analysed here.

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
