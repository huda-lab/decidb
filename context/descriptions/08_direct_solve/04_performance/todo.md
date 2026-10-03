# Performance — open work

The [large-scale sweeps](s1_large_scale.md) are exploratory while the
production selection policy remains open. Use the same phase and workload
boundaries for further measurements. The first-slice
[correctness gate](../03_correctness/done.md) is complete.
The existing pipeline profiling area is
[`05_performance/`](../../05_performance/README.md); direct-solve reports should
use compatible workload, backend, and readback boundaries.

- [ ] **PERF-01 — End-to-end phase accounting.** Depends: VAL-05. Separate
  direct analysis, plan construction, relational execution, and result
  readback. Compare against solver model construction, backend loading, solve,
  and readback on both supported backends. Total CTAS time, peak RSS, direct
  analysis/construction, and solver phases are measured. The
  [API experiment](s1_api_phase.md) separately measures client readback of
  every row and collector append/combine/finalize operation time. The wide
  full-output case shows much higher direct collector append work than either
  solver path. Streaming uses a bounded result buffer and can continue
  execution during fetch; it is a useful end-to-end alternative but not an
  exact phase subtraction. Isolate relational execution from in-engine result
  collection in wall time without changing their semantics.
- [ ] **PERF-02 — Scale and memory sweep.** Depends: PERF-01. Vary input rows,
  capacity, sign mix, tie density, and source width. Record peak memory,
  intermediate cardinality, planning overhead, and output liveness costs.
  The synthetic, stored-table, and stored-join sweeps have raw data through
  five million rows and 512-byte
  sources. A 100,000-row `EXPLAIN ANALYZE` shows full cardinality through the
  rank; a five-million-row pair shows the payload adds both scan and window
  work. The [aggregate API sweep](s1_api_phase.md#selective-output-pruning)
  shows that safe pruning removes an unused stored payload from the scan and
  cuts the five-million-row wide query/collection time substantially. The
  [window-buffer counter](s1_api_phase.md#window-sort-storage-and-joined-source-pruning)
  measures 2,804 MiB for wide full output versus 191 MiB for an aggregate,
  and verifies that a safe inner-join passthrough can achieve the same
  pruning. Computed outputs remain live unless their evaluation can be proved
  skippable. The [five-million-row interval sweep](s1_api_phase.md#global-cardinality-intervals)
  shows that a positive lower-bound count shares the rank window on the
  tested stored source without materially increasing its query time or sort
  storage. A [grouped interval sweep](s1_api_phase.md#grouped-cardinality-intervals)
  covers 100 `PER` groups at five million rows with matching direct/Gurobi
  results. Its current direct plan adds a global active-count window for the
  solver's empty-aggregate outcome before score evaluation; the corrected
  direct median is 0.272–0.279
  seconds, with a per-group sort-buffer snapshot. Measure peak window and
  merge memory, and add further
  representative source plans; keep source query, configuration, and raw
  measurements reproducible.
  A [fixed-pin grouped sweep](s1_api_phase.md#fixed-boolean-pins-at-scale)
  gives 0.326–0.330-second direct medians versus 36.256–37.517 seconds for
  Gurobi at five million rows. It still needs a query-only peak-memory
  measurement. A [source-valued bound sweep](s1_api_phase.md#source-valued-group-bounds-at-scale)
  varies cap within each of 100 groups at five million rows; direct medians
  are 0.453–0.457 seconds versus 32.018–32.670 seconds for Gurobi, with
  exact selected-count and objective agreement. Query-only peak memory remains
  unmeasured. A [paired dynamic-bound sweep](s1_api_phase.md#paired-source-bounds-at-scale)
  tests two varying source columns per group at five million rows and retains
  a large direct benefit; its external peak RSS is also unmeasured. A
  [nullable source-expression sweep](s1_api_phase.md#source-bound-expressions-at-scale)
  tests `COALESCE` caps at the same size and also retains the benefit, while
  query-only peak memory remains unmeasured. A
  [grouped aggregate-local `WHEN` sweep](s1_api_phase.md#aggregate-local-when-at-scale)
  retains a large query-time benefit with 10% inactive rows, but its direct
  in-process peak memory exceeds Gurobi on the tested wide source. A
  [source-valued local-`WHEN` sweep](s1_api_phase.md#source-valued-aggregate-local-when-at-scale)
  also retains the query-time gain; its wide in-process peak varies around
  Gurobi's, so this memory tradeoff still needs a decision-grade measurement.
  An [additive-score sweep](s1_api_phase.md#additive-linear-scores-at-scale)
  confirms a large query-time gain for two source coefficient terms with 100
  grouped intervals at five million rows; its full-process wide benefit is
  smaller because source setup dominates.
- [ ] **PERF-03 — Selection policy evidence.** Depends: PERF-02. Determine when
  direct is materially better and when it is not. The sweep shows both large
  narrow wins and wide/zero-capacity counterexamples; the API sweep shows
  output readback can erase much of a direct gain. Exact and paired intervals
  improved substantially over Gurobi on measured five-million-row global
  and grouped sources, but broader source and output shapes still need
  coverage. Keep the default `off`
  while the broader policy is unproved. A cost model is warranted only when
  correct alternatives genuinely compete.

**Exit gate:** retain/promote the rule only with a complete benefit report that
does not count timeouts, solver limits, unsupported cases, or memory skips as
successful comparisons.
