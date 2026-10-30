# Performance findings

What the pipeline profiler measures at each stage of the solver path, and what its runs show. The method is in
[benchmarking/pipeline_profiler.md](benchmarking/pipeline_profiler.md); open questions are in [todo.md](todo.md).
Measuring a bottleneck does not optimize it; no solver tuning is implemented.

## Planning

Opt-in spans cover parsing, SELECT binding, comparison/objective canonicalization, backend selection, DECIDE rewrite families and physical DECIDE planning. DuckDB detailed JSON supplies the broader planner/optimizer timings. Statement markers exclude setup/validation from the target query's custom profile.

The first Gurobi availability check loads the library and probes an environment/license during planning; `gurobi.availability_probe` is distinct from actual model loading.

## Input and extraction

DuckDB operator profiles expose scans, joins, sorting and output-table writing. Per-chunk accumulators measure sink append and solution-to-row mapping without logging every chunk. Sink-combine timing includes its lock wait.

Finalize spans separate the keyed-decision mapping (`execution.keyed_mapping`), the run-time scope checks (`execution.scope_checks`), constraint evaluation, objective evaluation, composed MIN/MAX evaluation and the helper decisions' members (`execution.evaluate_value_links`). Constraint subspans isolate setup, linear coefficients, the `when` / `per` / `by` grouping with a reducer's inner `per` and colon form (`extraction.when_per_and_qualifiers`), RHS and quadratic coefficients. Nested group-id and Boolean-mask spans identify shared helpers; constraint ordinals identify clauses.

## Model construction

Spans separate assembly/copies, implied bounds, objective/AVG assembly and indexing; then L0, ABS, not-equal, bilinear, MIN/MAX and construct lowering. Neutral-model phases cover bounds/types, linear/quadratic objectives, constraint expansion, global/general rows and sanity checks. Counters report variables, linear rows/nonzeros, quadratic rows/nonzeros and native constructs.

The `model_construction_ms` column ends before `SolverModel::Build`, and `solver_ms` includes that neutral build, so those two columns alone cannot identify backend time.

## Backend loading

Gurobi spans cover environment creation, variable arrays, model creation, individual linear-row submission, quadratic/general/indicator rows and quadratic objectives. No loading strategy is changed.

HiGHS spans cover variable types, row/range coordinate arrays, coefficient scaling, CSR packing, `passModel` and Hessian loading. Existing temporary arrays and copies remain. Boundary RSS and total process peak are recorded independently from optimization.

## Solver execution

Both backends are forced explicitly; no silent fallback is accepted. Gurobi `optimize` and HiGHS `run` are timed separately from loading/readback. The Gurobi watcher join has its own span.

Optional native logs expose versions, reductions and progress where available. Counters include iteration/node counts and objectives; Gurobi additionally reports bound/gap where available. Native solver internals are not all separately timed by these API-level spans.

Q5/Q7 require quadratic constraints and are rejected by the current forced-HiGHS contract before execution; they are capability exclusions. P2/P3 cover supported convex QP on both. The corrected 1,000-row pilot validated all 28 supported pairs, with two expected HiGHS refusals; it is preliminary, not the final baseline.

## Output and cleanup

Backend result extraction is separate from optimization. DECIDE's output accumulator measures complete solution-to-row mapping and integer conversion. CTAS forces full output without terminal formatting millions of rows; it is a different consumer from the original CLI benchmark.

The facade times postprocessing and cleanup separately. Gurobi model/environment destruction is instrumented. Finalize self time includes remaining local destruction; process wall/resource measurements additionally include validation and shutdown.

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
- **Row lists built twice.** Implied bounds and the builder each built a grouped clause's row lists; `ClauseRows`
  builds them once. Q1 model build about 15 ms lower.
- **Group keys formatted when printed (PERF-12).** A grouped clause formatted every group's key text when its data
  was read and copied it into each model row; a row now carries a group number and the text is formatted only for a
  finding that prints it. On a clause of 200,000 groups over 1M rows, `extraction.constraint` 150 → 103 ms (medians of
  three interleaved runs, load ~17); Q1 and Q8, whose grouped clauses have 4-6 groups, unchanged. DIAGNOSE on 20,000
  groups: same time and peak RSS.
- **Peak memory on Q3 and Q10 (PERF-11).** The peak is inside Gurobi's solve, after DeciDB hands over the model; the
  stages before it hold less than the solve adds. Not a DeciDB cost.
