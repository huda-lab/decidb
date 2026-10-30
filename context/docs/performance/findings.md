# Performance findings

What the pipeline profiler measures at each stage of the solver path, and what its runs show. The method is in
[benchmarking/pipeline_profiler.md](benchmarking/pipeline_profiler.md); open questions are in [todo.md](todo.md).
Measuring a bottleneck does not optimize it; no solver tuning is implemented.

## Methodology

Unless marked otherwise, a number below is from build dc809bdb5f on database `large`, one query at a time through
`profile_pipeline.run_one` with `SOLVER_SECONDS` 3, the median of three interleaved runs. Load is the 1-minute load
average during the runs. Numbers marked *2026-10-30 sweep* are read from the large sweep at d385eca667
(`results/pipeline_profile_large.csv`, raw spans in `pipeline_profile_raw_large/`).

**Instrumentation overhead.** The same CTAS in fresh processes with `DECIDB_PROFILE=1 DECIDB_BENCH=1` against no
`DECIDB_*` variable, with no solver log and no sampling, interleaved in alternating order; a full solve (limit 300 s)
on queries whose solve is short. Load 2.9-3.4.

| Query | on (ms) | off (ms) | diff | RSS on/off (MiB) |
|---|---|---|---|---|
| Q11 Gurobi | 164 | 163 | +1.0 (0.6 %) | 118 / 118 |
| Q9 Gurobi | 213 | 209 | +4.0 (1.9 %) | 259 / 260 |
| P1 Gurobi | 338 | 336 | +2.0 (0.6 %) | 388 / 388 |
| P2 Gurobi | 276 | 269 | +7.0 (2.6 %) | 534 / 534 |
| P4 Gurobi | 613 | 612 | +1.0 (0.2 %) | 673 / 676 |
| P3 Gurobi | 1012 | 1008 | +4.0 (0.4 %) | 743 / 743 |
| Q8 Gurobi | 700 | 684 | +16 (2.3 %) | 855 / 856 |
| P1 HiGHS | 252 | 253 | -1.0 | 250 / 250 |
| Q11 HiGHS | 140 | 141 | -1.0 | 131 / 131 |
| Q8 HiGHS | 844 | 855 | -11 | 938 / 937 |

A query emits 260-340 profile lines. The overhead (-1.3 % to +2.6 %, no consistent sign) is inside run-to-run noise
and peak memory is unchanged, so the sweep's numbers need no correction for it.

**Objective check.** The profiler compares the optima of a query's solved runs
([pipeline_profiler.md § Correctness signal](benchmarking/pipeline_profiler.md#correctness-signal)). On the sweeps on
disk: the 2026-10-30 large sweep has no disagreement; the 2026-09-08 medium sweep flags Q1, Q2 and Q6 and the
2026-09-09 large sweep flags Q1, the HiGHS near-optimal answers of the 1e-4 MIP gap those builds used. The widest
agreeing spread is Q11, Gurobi 2734.3439230 against HiGHS 2734.3439758 (1.9e-8 relative).

## Planning

Opt-in spans cover parsing, SELECT binding, comparison/objective canonicalization, backend selection, DECIDE rewrite families and physical DECIDE planning. DuckDB detailed JSON supplies the broader planner/optimizer timings. Statement markers exclude setup/validation from the target query's custom profile.

The first Gurobi availability check loads the library and probes an environment/license during planning; `gurobi.availability_probe` is distinct from actual model loading.

**Fresh process against repeated query.** One process runs the same CTAS three times; three processes per query and
mode (forced Gurobi, forced HiGHS, no forcing). Load 2.2-2.6.

- An empty process (open `large.db`, `SELECT 1`): 18 ms wall.
- Parse, bind and canonicalize: 0.1-0.3 ms on every statement, first or not.
- `optimizer.decide`: 5.1-5.3 ms on a process's first statement when Gurobi can be chosen (forced Gurobi or no
  forcing), 0.15-0.25 ms after. The difference is `gurobi.availability_probe`, 4.8-5.1 ms once per process. Forced
  HiGHS never probes. Without forcing, P1 is answered by direct solve (119 ms first, 113-114 after).
- `gurobi.environment`: 0.6-0.8 ms on every statement (one environment per query). `gurobi.new_model`, also per
  query, scales with columns: 11 ms at 136k, 85 ms at 1M.

| Whole statement (ms) | Gurobi first / repeats | HiGHS first / repeats |
|---|---|---|
| Q11 | 161 / 150-152 | 139 / 132-133 |
| P1 | 340 / 315-321 | 251 / 246-256 |
| Q8 | 664 / 632-634 | 827 / 780-795 |

The first statement costs 5-47 ms more: 5 ms is the probe, the rest is spread over execution (warm caches and the
allocator) in no single span. Planning is sub-millisecond and first use costs one 5 ms probe per process; reusing the
environment would save under 1 ms a query, so nothing is cached.

## Input and extraction

DuckDB operator profiles expose scans, joins, sorting and output-table writing. Per-chunk accumulators measure sink append and solution-to-row mapping without logging every chunk. Sink-combine timing includes its lock wait.

Finalize spans separate the keyed-decision mapping (`execution.keyed_mapping`), the run-time scope checks (`execution.scope_checks`), constraint evaluation, objective evaluation, composed MIN/MAX evaluation and the helper decisions' members (`execution.evaluate_value_links`). Constraint subspans isolate setup, linear coefficients, the `when` / `per` / `by` grouping with a reducer's inner `per` and colon form (`extraction.when_per_and_qualifiers`), RHS and quadratic coefficients. Nested group-id and Boolean-mask spans identify shared helpers; constraint ordinals identify clauses.

**Ranking.** Gurobi, load 2.2-3.0; HiGHS within 2 % of these. ms:

| Query | Top extraction costs |
|---|---|
| Q4 | `evaluate_composed` 495 (21 after PERF-14); `formulate` 312 (composed MIN/MAX 240, MIN/MAX objective 73); constraints 156 (RHS 82) |
| Q8 | group ids 304 (inside the keyed mapping 255 and the constraints); constraints 152 (`when`/`per` 106) |
| Q1 | constraints 217: linear coefficients 63, `when`/`per` 63, setup 59, RHS 31, group ids 23; objective 32 |
| P4 | constraints 121: `when`/`per` 114, of it group ids 95 (250k order-key classes); objective 11.5 |
| P1 | constraints 43 (RHS 39); objective 11.5 |

Q4's composed MIN/MAX evaluation is the largest extraction cost on the benchmark and has no known cause
([todo.md § PERF-14](todo.md#perf-14--q4s-composed-minmax-evaluation)).

**Q8's key width.** Q8's keyed mapping depends on the key's width: `per o` (9 columns) against `per o.o_orderkey` on
the same query (load 2.2): keyed mapping 252 → 109 ms, `solver.cleanup` 45 → 0.7 ms, peak 858 → 498 MiB, query
666 → 474 ms. The difference is one `Value` per class per key column (250k × 9) kept as each class's representative
(`BuildGroupIds`'s `out_rep_keys`), built in the mapping and destroyed with the model
([todo.md § PERF-13](todo.md#perf-13--q8s-key-values)).

**Scope checks against constraint evaluation.** A join of `lineitem` and `orders` (1M rows), `DECIDE per o:
assign(BOOL)`, one varying clause plus `per o.o_orderpriority: sum(assign) by (o.o_orderpriority) >= 20`, MAXIMIZE
`sum(assign * l.l_extendedprice)`; Gurobi; operator times from DuckDB's JSON profile in separate runs; span times in
ms. Load 1.9-2.1.

| Clause | Checks | Query (ms) | Peak (MiB) | `scope_checks` | constraints | Window ops (CPU ms) |
|---|---|---|---|---|---|---|
| `per o: assign <= o.o_shippriority` | none needed | 571 | 838 | 15.5 | 97 | — |
| `per l.l_orderkey: assign <= 1` | the operator's keyed check | 589 | 807 | 109 | 46 | — |
| `per l.l_orderkey: assign <= o.o_shippriority` | keyed + a planned check | 1069 | 1667 | 87 | 97 | 569 + 877 + 125 |

The operator's keyed check costs about twice constraint evaluation (109 against 46 ms). A planned determination check
costs about 480 ms of wall time and 860 MiB of peak memory on 1M rows while passing, five times constraint
evaluation: three window operators each materialize every row with every column, for the per-class count/min/max
(569 ms CPU), a dense rank of all rows by the key to pick the least failing class (877 ms) and a global min for the
message (125 ms) ([todo.md § PERF-3](todo.md#perf-3--a-passing-determination-check-materializes-every-row)).

## Model construction

Spans separate assembly/copies, implied bounds, objective/AVG assembly and indexing; then L0, ABS, not-equal, bilinear, MIN/MAX and construct lowering. Neutral-model phases cover bounds/types, linear/quadratic objectives, constraint expansion, global/general rows and sanity checks. Counters report variables, linear rows/nonzeros, quadratic rows/nonzeros and native constructs.

The `model_construction_ms` column ends before `SolverModel::Build`, and `solver_ms` includes that neutral build, so those two columns alone cannot identify backend time.

**Ranking by time** (*2026-10-30 sweep*, Gurobi medians; Q1, Q4, Q8, P1 and P4 within 20 % on dc809bdb5f):

| Query | Formulation (ms) | Neutral build (ms) | Shape |
|---|---|---|---|
| Q10 | 308 (ABS bounds 306) | 657 (rows 417, sanity 85, validate 129) | 5M vars, 6M rows, 14M nnz |
| Q4 | 293 (composed MIN/MAX 232, MIN/MAX objective 62) | 153 (rows 100, global rows 30) | 1M vars, 1M rows, 6M nnz |
| Q3 | 9 | 195 (rows 165) | 0.84M vars, 1.56M rows, 3.2M nnz |
| Q2 | 50 (`<>` 49) | 182 (rows 132) | 1.5M vars, 0.5M rows |
| Q6 | 154 (bilinear 153) | 112 (global rows 66) | 1.3M vars, 2.3M rows |
| Q7 | 0.5 | 98 (quadratic rows 76) | |
| P1 | 2 | 92 (rows 80: 1M per-row bounds folded into the column box) | 1M vars, 0 rows |

Formulation families by cost: ABS > composed MIN/MAX > bilinear > `<>` > MIN/MAX objective. The neutral build takes
26-60 ns per nonzero.

**Memory jumps** (RSS change across the span; *2026-10-30 sweep*, repeat 0, dc809bdb5f in brackets): Q10 formulate
+939 MiB, build +2,114, Gurobi load +696; Q6 formulate +1,308, build +570; Q3 build +460; Q2 formulate +217, build
+304; Q4 formulate (+366), build (+264); Q8 keyed mapping (+386, the key `Value`s of § Input and extraction). Q10's
build adds about 340 bytes per model row beyond 12 bytes per nonzero, so the per-row structure (`ModelConstraint`: two
heap vectors and a `ConstraintProvenance` with strings and vectors) holds the memory, not the coefficients. Not
measured further.

**A helper decision's box, per instance.** Two groups of 200 rows, `x(REAL)` in [0, 10]; group 1 `v` in [1, 10],
group 2 in [1e4, 1e5], so the MAX helper's reach is [0, 99.2] and [0, 988,340]. The cover model comes from an
uncommitted switch that sets every instance's reach to the cover (its model dump shows a helper's `ub` 99.2 → 988,340
and its closing Big-M with it). Three repeats, load 2.5-2.7.

| Clause | Backend | Own group's box | Cover |
|---|---|---|---|
| `x + 1 = max(x * v) by (g)`, MAXIMIZE `sum(w * x)` | Gurobi | 0 nodes, 0.7 ms | 0 nodes, 0.7 ms |
| | HiGHS | 0 nodes, 0.9 ms | 0 nodes, 0.9 ms |
| `x + 1 <= max(x * v) by (g)`, MINIMIZE `sum(w * x)` | Gurobi | 1 node, 19.2 ms | 1 node, 19.4 ms |
| | HiGHS | 3 nodes, 170 ms | 1 node, 69 ms |

The pinned clause has one solution per group (every `x` of a group equal, `1 / (max v - 1)`), which presolve finds,
so the box cannot matter there. On the one-sided clause Gurobi is unchanged and HiGHS takes more nodes with the
tighter box. Objectives agree across boxes and backends within 1e-6. On this pair the per-instance box shows no
benefit in nodes or time; one pair is not evidence against it, and a model where the closing Big-M decides the
relaxation (many groups, an objective on the helper) is not measured.

## Backend loading

Gurobi spans cover environment creation, variable arrays, model creation, individual linear-row submission, quadratic/general/indicator rows and quadratic objectives. No loading strategy is changed.

HiGHS spans cover variable types, the rows packed into HiGHS's compressed row matrix (`highs.pack_rows`), coefficient
scaling (`highs.scale_rows`), the columns (`highs.pack_columns`), `passModel` (`highs.pass_model`) and the Hessian
(`highs.hessian`). The model's rows are already row-wise, so they pack once into the `HighsLp`, which moves into
`passModel`; HiGHS builds its own internal copy inside `passModel`. Boundary RSS and total process peak are recorded
independently from optimization.

**Gurobi** (*2026-10-30 sweep*, medians). Environment 0.6-0.8 ms per query. `GRBnewmodel` about 85 ns per column (P1,
1M columns, 85 ms; Q10, 5M, 416 ms). One `GRBaddconstr` per row, about 110 ns per row (Q4, 1M rows, 121 ms; Q6, 2.3M,
267 ms; Q10, 6M, 660 ms); general constraints Q10 192 ms; indicators Q2 74 ms. Load is under 2 % of the solve on every
query that solves in over 5 s. On the fast ones (P1 91 ms against a 34 ms solve, P4 121 against 216) `GRBnewmodel`
dominates, and it is already one call. Bulk `GRBaddconstrs` could touch only the per-row part (at most 660 ms, Q10,
against a 72 s solve), so no change is proposed.

**HiGHS**, dc809bdb5f → 49bf2dfb1c (the single row pack above), forced HiGHS, 3 s limit, load 2.5-5.2. The same model
on both builds: goldens identical on both backends.

| Query | nnz | `highs.load` | RSS jump in load (MiB) | Peak after load (MiB) | Process peak (MiB) |
|---|---|---|---|---|---|
| Q4 | 6.0M | 306 → 219 ms | 523 → 226 | 1,413 → 1,117 | 2,214 → 2,113 |
| Q6 | 6.1M | 304 → 185 ms | 575 → 215 | 2,573 → 2,213 | 3,694 → 3,602 |
| Q10 | 22M | 1,164 → 699 ms | 1,816 → 1,026 | 4,794 → 3,874 | 4,963 → 4,533 |

## Solver execution

Both backends are forced explicitly; no silent fallback is accepted. Gurobi `optimize` and HiGHS `run` are timed separately from loading/readback. The Gurobi watcher join has its own span.

Optional native logs expose versions, reductions and progress where available. Counters include iteration/node counts and objectives; Gurobi additionally reports bound/gap where available. Native solver internals are not all separately timed by these API-level spans.

Q5/Q7 require quadratic constraints and are rejected by the current forced-HiGHS contract before execution; they are capability exclusions. P2/P3 cover supported convex QP on both. The corrected 1,000-row pilot validated all 28 supported pairs, with two expected HiGHS refusals; it is preliminary, not the final baseline.

## Output and cleanup

Backend result extraction is separate from optimization. DECIDE's output accumulator measures complete solution-to-row mapping and integer conversion. CTAS forces full output without terminal formatting millions of rows; it is a different consumer from the original CLI benchmark.

The facade times postprocessing and cleanup separately. Gurobi model/environment destruction is instrumented. Finalize self time includes remaining local destruction; process wall/resource measurements additionally include validation and shutdown.

**Measured** on the runs of § Input and extraction, wall time against the spans; DuckDB's operator profile for the
CTAS write.

- Backend readback (`*.status_and_readback`): 0.3-3 ms. `execution.output_readback` (solution to rows, every row
  produced): 0.4 ms (Q11, 136k rows) to 17.8 ms (Q8, 1M rows).
- Writing the temporary result: DuckDB's `CREATE_TABLE_AS` operator, 5.7 ms CPU for 1M rows (the scope-check join).
- `solver.cleanup` (the SolverModel and the session destroyed): 0.7-17 ms, except Q8 at 45-48 ms on both backends,
  which is the 250k × 9 representative key `Value`s (`per o.o_orderkey`: 0.7 ms; PERF-13). `gurobi.free_model` at
  most 3.2 ms (Q1).
- Wall time outside every span: under 10 ms on every query (P1 7.9, Q8 2.7, Q11 2.2). On queries with a long finalize
  the per-thread `sink_append` total exceeds the wall time it overlaps, so nothing is left unassigned.

Output and cleanup are small; Q8's cleanup is the one cost that stands out, and it belongs to the keyed mapping's
representative values.

## Memory and scaling

Every run records its peak resident set size, taken from `/usr/bin/time -l`, as the `peak_rss_mib` column. It is recorded and never acted on: a large number is a finding, not a reason to stop a run.

The profiler carries no memory guard, cap or early stop. A machine-wide counter such as `vm_stat` swapins covers every process since boot, not the run being measured, so it cannot judge a run; nothing here is a claim about swap.

The medium-tier sweep (`results/pipeline_profile_medium.csv`) peaks at 6,525 MiB, on Q10/HiGHS at 2.5M variables; the median across all 90 runs is 668 MiB. The machine has 18 GiB, so this scale fits with room to spare.

## Regressions since 2026-09-09

Measured 2026-10-30 on Gurobi with `DECIDB_TIME_LIMIT=3`, medians of three interleaved runs beside other work (load
8-18, single spans swing up to 3x).

- **Q8's keyed mapping (PERF-9).** Group ids compared each row with its group's representative `Value` by `Value`,
  built a label string for every group, and copied an uncollated key into a label column. Group ids now come from
  DuckDB's aggregate hash table, labels are formatted only for the groups a message names, and a key keeps one copy
  of an uncollated column. `execution.keyed_mapping` 1,169 → 257 ms; `extraction.group_ids` 868 → 309 ms and
  extraction 1,028 → 453 ms on Q8; Q1's group ids 104 → 25-37 ms. Q8's peak RSS 1,172 → about 860 MiB.
- **A `when` read once per part (PERF-10, part).** `KeepRowsWhere` scanned the data once per AND or OR part; it now
  scans once. Q1 `when_per_and_qualifiers` 218 → 165-174 ms.
- **Extraction passes since September (PERF-10).** Measured alone, medians of 3 at `large` on Gurobi, two interleaved
  rounds against the build before. A NULL-skip filter that keeps every row the clause reads (a NULL test on data with no
  NULL) now counts as no filter, so an ungrouped clause no longer carries a 1M-row group list or a CSR for it (DuckDB's
  ValidityMask idea: allocate nothing while every row is valid); a `per K:` class check reads the cached grouping and
  stops at the first kept row with a class instead of renumbering every row; a clause's term conditions are read in one
  scan; the kept rows' groups are built only under DIAGNOSE, their one reader. `extraction.constraint` Q1 312 → 217 ms,
  Q4 210 → 156, P4 134 → 123; `assembly.implied_bounds` Q1 61 → 49, Q4 and P4 unchanged.
- **Rows, bounds and NULL tests built once (PERF-10, closed).** Each clause's emitted rows are one CSR shared by
  implied bounds, the `<>` expansion and the builder; a bound's admitted ends read `a` and `r` evaluated once per row
  (two projections) and stage 08 folds constants once; a NULL test the statistics prove always TRUE folds at plan time
  and no filter is read for it, and the objective sites treat an identity filter as none. Medians of 3 at `large` on
  Gurobi, two interleaved rounds (load 5-14), Q1 / Q4 / P4: `extraction.constraint` 216 → 202, 156 → 120,
  122 → 120 ms; `assembly.implied_bounds` 52 → 38, 51 → 42, 26 → 24; `extraction.rhs` Q4 83 → 55;
  `extraction.constraint_setup` Q1 60 → 43, Q4 8 → 0; `execution.evaluate_objective` 32 → 20, 26 → 17, 11 → 7;
  `model.build` 53 → 46, 157 → 128, 42 → 43. Term filters still select kept rows with a per-row `vector<bool>`: a
  `SelectionVector` path would let DuckDB's conjunction `Select` reorder AND/OR operands, and DECIDE reads them left
  to right.
- **Composed MIN/MAX coefficients read a chunk at a time (PERF-14, closed).** `EvaluateComposedClauses` read each
  inner term's coefficient row by row: it boxed a `Value`, cast it to `DOUBLE` through a fresh `Vector` and unboxed
  it, one scan per inner term (a stack sample put the phase's whole time in the cast). It now reads a term's inner
  coefficients in one scan through `EvaluateTermColumns`, the vectorized reader every other clause uses. Q4
  `execution.evaluate_composed` 489 → 21 ms on Gurobi, 21 ms on HiGHS; no other span moved.
- **A group keeps its row, not its key values (PERF-13, closed).** `BuildGroupIds` kept one `Value` per group per
  key column (250k groups × 9 columns on Q8's `per o`). A group now keeps its first row (for a collated column, the
  row of its least spelling); groups number through DuckDB's sort key instead of a `Value` comparator, and a message
  reads a group's values at its row when it names it (late materialization). Q8, medians of 3, two interleaved
  rounds (load 10-13): `execution.keyed_mapping` 264 → 167 ms, `extraction.group_ids` 314 → 219, `solver.cleanup`
  52 → 0, peak RSS 834 → 526 MiB. The keyed mapping left is the hash-table build (about 100 ms) and the sort of
  250k groups (about 80 ms).
- **MIN/MAX link rows summed without a hash map (PERF-17, closed).** Each MIN/MAX link row summed its columns in its
  own `std::unordered_map` (`MinMaxLinkRow::AddColumn`, and the composed outer row's `outer_accum`): a bucket array
  and a node per member, millions of allocations on Q4's 1M members, and rows grown from capacity 1. The rows now sum
  in the model builder's `SparseCoeffAccumulator` (dense scatter, first-touch order) and are sized once. Q4, medians
  of 3 (load 11-16): `execution.formulate` 346 → 172 ms; same model (1,000,002 columns, 1,000,010 rows, 6,000,004
  nonzeros).
- **A MIN/MAX member built into the row it becomes (PERF-18, closed).** Each member was built as an `ExtremumMember`
  (two vectors and a copied `AuxRange`) and then copied into the solver row. `EmitExtremumLink` now asks an
  `ExtremumMemberSource` for each member as it emits the row, into one reused scratch row, and reads a member's range
  only where the native arm pins a column (late materialization). Q4, medians of 3 (load 15): `execution.formulate`
  167 → 122 ms; same model. The dense accumulator `BuildMinMaxGroups` assigns per call costs 1.4 ms cold, 0.12 ms
  warm: left as is.
- **A reserve that grew a shared buffer one row at a time (BUG-40, fixed).** `SparseCoeffAccumulator::Flush` reserved
  exactly `size + touched` on each call. Since `ClauseEmittedRows` (PERF-10) appends every row of a clause into one
  CSR buffer, each flush reallocated and copied the whole buffer: quadratic in the clause's rows. d1 (1M-row join,
  `per l.l_orderkey` over 250k kept rows) spent 5 s in `assembly.implied_bounds`. The reserve is gone and the vectors
  grow geometrically: d1 passing 6.2 → 0.98 s (three runs each, load 6-10). Q1, Q4 and P4 were too small to show it.
- **Row lists built twice.** Implied bounds and the builder each built a grouped clause's row lists; `ClauseRows`
  builds them once. Q1 model build about 15 ms lower.
- **Group keys formatted when printed (PERF-12).** A grouped clause formatted every group's key text when its data
  was read and copied it into each model row; a row now carries a group number and the text is formatted only for a
  finding that prints it. On a clause of 200,000 groups over 1M rows, `extraction.constraint` 150 → 103 ms (medians of
  three interleaved runs, load ~17); Q1 and Q8, whose grouped clauses have 4-6 groups, unchanged. DIAGNOSE on 20,000
  groups: same time and peak RSS.
- **Peak memory on Q3 and Q10 (PERF-11).** The peak is inside Gurobi's solve, after DeciDB hands over the model; the
  stages before it hold less than the solve adds. Not a DeciDB cost.
