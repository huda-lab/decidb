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
LogicalWindows above the DECIDE source, the shape PERF-3 replaces: each window holds every source column to attach
one value per group. Measure the cost on a 1M-row clause with a data-bound RHS first; if it matters, the fix is
PERF-3's shape, an aggregate over the columns the bound reads on a second read of the source (`DirectSourceReader`),
joined back by its group key.

## PERF-16 — DECIDE-built trees miss constant folding

DuckDB folds constants in `EXPRESSION_REWRITER` (`optimizer.cpp:124`), before `DECIDE_OPTIMIZER` (:147), so the
trees DECIDE builds are never folded. Stage 08 folds the trees it evaluates once (`TransformToChunkExpression`);
direct-solve plans are not folded. One fold must not run on each `ExpressionBuilder::Out` call: `GuardCoefficients`
(`direct_ir.cpp:263`) wraps each IR coefficient in a CASE built there, and a message renders through that CASE, so
folding there prints a written `2 * w` as `2.0 * w`. Proposal: name each coefficient before it is guarded, then fold
the finished direct plan once (`error()` is volatile and never folds). Measure Q1, Q4 and P4 before and after:
DuckDB evaluates a constant subtree once per chunk, so the gain may be under a millisecond; if nothing moves, record
that and close.
