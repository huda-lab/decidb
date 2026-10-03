# Performance — completed work

The [S1 large-scale report](s1_large_scale.md) includes reproducible commands
and recorded raw measurements for 1,000 through 5,000,000 rows, both solver
backends, capacities 0 through row count, score patterns with varying ties,
and source widths 0 and 512 bytes. Every measured run completed; the large
narrow sweep used three repeats. At five million narrow rows and 10% capacity,
the direct path's median was 0.403 seconds versus 2.853 seconds for Gurobi and
42.978 seconds for HiGHS on mixed scores. Wide rows reduce the advantage and
can make direct peak memory higher than either solver path.

A separate narrow stored-table sweep at one and five million rows excludes
source-table creation from the query timer. At five million rows with mixed
scores, direct took a median 0.204 seconds versus 2.833 seconds for Gurobi and
42.652 seconds for HiGHS over two runs. All modes returned the same row count,
selected count, and primary objective within floating-point summation error.
Single wide stored-table runs at five million rows took 2.451 seconds direct,
4.818 seconds Gurobi, and 44.515 seconds HiGHS, with peak RSS 3,536,
3,890, and 3,687 MiB respectively. This differs from the computed wide source,
where direct peak RSS was higher; source evaluation and layout matter.
At five million narrow computed rows, single capacity-zero and full-capacity
runs also favored direct, but by smaller factors than the 10% case.

The [stored-join sweep](s1_large_scale.md#stored-table-join-input) adds a
second source plan at one and five million rows. Direct/Gurobi CTAS medians
at five million rows were 0.215/3.423 seconds; both HiGHS trials reached the
150-second process limit and are incomplete evidence. The [API phase
experiment](s1_api_phase.md) separately times in-process readback of every
result value. At five million narrow stored rows, direct query/collection
and API readback medians were 0.186 and 0.236 seconds. The wide source took
2.382 and 1.886 seconds in one direct run. A separate aggregate-output sweep
measured 0.173 seconds direct on a five-million-row narrow source versus
0.968 seconds with an unused 512-byte payload. `EXPLAIN` shows the wide
payload remained in the original scan and global-rank plan. Safe output
pruning now omits stored columns that the parent never reads, while retaining
computed columns that may raise errors. Two new five-million-row wide
aggregate trials took 0.218 and 0.190 seconds for direct query/collection,
with the same count, selected count, and objective. Peak RSS includes creation
of the stored source and did not fall consistently. The
[API report](s1_api_phase.md#selective-output-pruning) records the raw data and
plan evidence.
A follow-up [memory snapshot sweep](s1_api_phase.md#selective-output-pruning)
records current RSS after setup and process high-water RSS through the query
on the five-million-row aggregate. Wide source creation and allocator reuse
prevent those process counters from identifying rank intermediate memory.

The runners record end-to-end CTAS or API query time, process peak RSS, direct
analysis/construction spans, and solver model-build/load/solve/readback spans.
A [five-million-row full-output phase sweep](s1_api_phase.md#collector-phase-check-on-full-output)
also records materialized-result collector append, combine, and finalize work.
With a 512-byte payload, direct collector appends summed to 3.870 worker-seconds
across parallel workers, versus 0.631 and 0.598 for Gurobi and HiGHS, for
the same returned values. The query wall timers were 1.901, 4.209, and 44.318
seconds. A [paired `EXPLAIN ANALYZE`](s1_api_phase.md#collector-phase-check-on-full-output)
shows that carrying the payload through the global rank also increases window
and scan operator time.

A [window sort-buffer snapshot](s1_api_phase.md#window-sort-storage-and-joined-source-pruning)
now measures distinct buffer allocations owned by the rank at sink
finalization. On five million stored rows it recorded 2,804 MiB for full
512-byte output versus 191 MiB when the parent requested only aggregates.
An inner-join passthrough proof extends that safe pruning to stored payloads
from a joined source: its wide aggregate changed from 2,804 to 191 MiB of
window buffers, and two post-change queries took 0.218 seconds versus 0.754
seconds before. A separate [streaming comparison](s1_api_phase.md#materialized-and-streamed-full-output)
shows a median 5.648-second materialized versus 3.421-second streamed direct
total on three wide five-million-row pairs; process peak RSS was variable.
The [global interval sweep](s1_api_phase.md#global-cardinality-intervals)
extends large-data evidence to lower, equality, and paired bounds. At five
million narrow stored rows, two runs per form gave direct query/collection
medians of 0.170–0.184 seconds versus 2.819–11.459 seconds for Gurobi,
with matching selected counts and objectives. The positive-lower count shared
the rank window and its sort-buffer snapshot stayed at 191 MiB. One wide
full-output paired-interval run took 2.176 seconds direct query/collection
plus 1.954 seconds API readback versus 12.632 plus 1.893 seconds for
Gurobi; unused wide output pruned to 191 MiB of rank sort buffers for an
aggregate result.
A [five-million-row grouped interval sweep](s1_api_phase.md#grouped-cardinality-intervals)
tested 100 `PER` groups and a 60–70% interval on stored narrow and unused
512-byte payload sources. A corrected direct plan checks the solver's
empty-active-set error before score evaluation. Its two-repeat
query/collection medians were 0.272 seconds narrow and 0.279 seconds
unused-wide; the earlier paired Gurobi runs
on the same SQL/source had medians of 34.356 and 33.562 seconds. The corrected
plan recorded 282–300 MiB of summed per-group sort-buffer allocations. That
counter is neither simultaneous peak sort memory nor full query memory. The
earlier faster direct results lacked the required empty-active-set behavior,
and an intermediate guard had the wrong error order; all are retained only as
superseded evidence.
A [fixed-pin five-million-row sweep](s1_api_phase.md#fixed-boolean-pins-at-scale)
uses the same grouped interval with 1% fixed selected and 1% fixed zero rows.
Two-repeat direct query/collection medians were 0.330 seconds narrow and 0.326
seconds with an unused 512-byte payload, versus 36.256 and 37.517 seconds for
Gurobi. Every run matched on selected count and primary objective. A current
unpinned direct sweep measured 0.268 and 0.276 seconds. The sort-buffer
snapshot was 276–282 MiB for fixed pins, not simultaneous peak query memory.
A [source-valued group-bound sweep](s1_api_phase.md#source-valued-group-bounds-at-scale)
used five million stored rows and two cap values within every one of 100
groups. Direct query/collection medians were 0.453 seconds narrow and 0.457
seconds with an unused 512-byte payload, versus 32.018 and 32.670 seconds for
Gurobi. All eight runs agreed on three million selections and primary
objective 601,317,410.6. Summed window sort-buffer snapshots of 550–578 MiB
are not query peak memory.
A [rerun after numeric source-bound widening](s1_api_phase.md#numeric-source-bound-widening)
returned the same count and objective in four direct runs, with query/collection
medians of 0.5025 seconds narrow and 0.5357 seconds with an unused 512-byte
payload. It used the executable's own timing because the sandbox denied the
external `/usr/bin/time` wrapper's `sysctl` call; external peak RSS was not
measured in that rerun.
A [fractional DOUBLE-cap sweep](s1_api_phase.md#numeric-source-bound-widening)
also varied values within every group at five million rows. Two direct runs
per width took 0.4954 seconds narrow and 0.5281 seconds with unused payload;
one Gurobi run per width took 32.6941 and 33.4266 seconds. Every run agreed
on three million selections and objective 601,317,410.6.
A [paired dynamic-bound sweep](s1_api_phase.md#paired-source-bounds-at-scale)
measured two independently varying source columns per group at five million
rows. Direct query/collection medians were 0.6549 seconds narrow and 0.6783
seconds with unused payload; Gurobi took 31.8961 and 33.0310 seconds. Every
run agreed on three million selections and objective 601,317,410.6. Summed
sort-buffer snapshots of 631–656 MiB are not query peak memory.
A [nullable `COALESCE` source-bound sweep](s1_api_phase.md#source-bound-expressions-at-scale)
measured five million rows with 100 groups. Direct query/collection medians
were 0.5824 seconds narrow and 0.5792 seconds with unused 512-byte payload;
Gurobi took 32.1601 and 32.4024 seconds. All six runs agreed on three
million selections and objective 601,317,410.6. Whole-process medians,
including source creation and readback, were 0.7323 and 8.2455 seconds for
direct versus 32.3474 and 40.1063 seconds for Gurobi.
A [grouped aggregate-local `WHEN` sweep](s1_api_phase.md#aggregate-local-when-at-scale)
measured five million rows with 10% inactive rows per group. All six runs
agreed on three million active selections, 3,250,121 total selections, and
objective 563,866,342.3. Direct query/collection medians were 0.3018 seconds
narrow and 0.2979 seconds with unused 512-byte payload, versus 28.8805 and
30.4114 seconds for Gurobi. Direct in-process query high-water memory was
666–689 MiB narrow versus Gurobi's 2,922 MiB, but 3,069–3,149 MiB wide
versus Gurobi's 2,972 MiB. Rank-only peak memory remains unmeasured.
A [source-valued aggregate-local `WHEN` sweep](s1_api_phase.md#source-valued-aggregate-local-when-at-scale)
kept the same five-million-row active quota with varying group caps. All six
runs agreed on active count, total count, and objective. Direct query/collection
medians were 0.5664 seconds narrow and 0.5736 seconds wide versus 28.8267
and 29.6506 seconds for Gurobi. Wide direct in-process high-water memory
varied from 2,936 to 3,372 MiB versus 3,116 MiB for Gurobi, so the memory
tradeoff remains unresolved.
A [five-million-row additive-score sweep](s1_api_phase.md#additive-linear-scores-at-scale)
tested two source coefficient terms with 100 grouped intervals. Direct
query/collection medians were 0.3051 seconds narrow and 0.3192 seconds with
an unused 512-byte payload; Gurobi took 13.9740 and 15.0499 seconds. All six
runs agreed on 3,500,000 selections and the reported objective. The in-process
high-water through the query was 680–693 MiB direct versus 2,943 MiB Gurobi
narrow, and 3,165–3,193 MiB direct versus 3,341 MiB Gurobi wide.
PERF-01 through PERF-03 remain open for valid execution and collection wall
accounting, peak rank memory, and a production selection policy.
