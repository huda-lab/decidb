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
unchanged. Done: a key that holds a decision's own key plans no determination check of it (FD reflexivity).

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

## PERF-16 — Constant folding where a tree is built

Stage 08 constant-folds each tree it evaluates (`TransformToChunkExpression`), not where the optimizer finishes
building it (`ExpressionBuilder::Out`), because messages render the built trees: folding there turns the `2 * w` a
message quotes into `2.0 * w`. Direct-solve plans are not folded. The cause is the message, not the fold: a message
that names a value should quote the source text the user wrote (as Clang underlines source rather than reprinting its
tree). Once messages quote source text, the fold moves to `Out` and direct-solve plans get it too.

## PERF-18 — A MIN/MAX member is staged before it becomes a row

Each member of a composed MIN/MAX link is built as an `ExtremumMember` (two vectors and a copied `AuxRange`) and
then copied into a `RawConstraint`: on Q4, after PERF-17, building about 38 ms, freeing about 15 and per-row
allocations about 20 of `execution.formulate`'s 156. Proposal (late materialization): `EmitExtremumLink` takes a member
source, a count and a callback that fills one reused scratch row, and reads the `AuxRange` only for the native arm;
about 7 call sites change. Also: `BuildMinMaxGroups` assigns its dense accumulator (about 8 MB on Q4) once per
call, so a query with many hard MIN/MAX clauses on a large table pays it per clause; measure that before changing it.
