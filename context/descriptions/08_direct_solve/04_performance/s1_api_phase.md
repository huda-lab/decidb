# S1 API phase and readback measurements

Measured on 2026-10-01–02 UTC on macOS arm64 with 18 GiB RAM, the release
library, four DuckDB threads, and successive uncommitted direct-solve builds over
`616145dec10547a2a89313a65fc7b059de29f6d6`. The
[CTAS scale report](s1_large_scale.md) measures a separate output path. This
API experiment uses [`profile_direct_s1_api.cpp`](../../../../benchmark/decide/profile_direct_s1_api.cpp)
and its [runner](../../../../benchmark/decide/profile_direct_s1_api.py).
The original [narrow raw runs](raw/s1_api_stored_narrow_raw.csv) have two repeats
per configuration; the original [wide raw runs](raw/s1_api_stored_wide_raw.csv)
have one. They predate selective output pruning. All runs in these two files
completed with five million or fewer rows, exactly
10% selected, and equal primary objectives across modes.

## What each timer includes

The program creates an in-memory stored source before the DECIDE query. Its
`setup_s` measures that creation. `query_collect_s` measures
`Connection::Query`: parsing, binding, optimization, the solver or relational
execution, and materializing the full result **inside DuckDB**. The following
`api_readback_s` measures `Fetch()` plus conversion and inspection of every
output value in the same process. It is the client readback cost from an
already materialized result, not network transfer or a streaming query.
The later delivery sweep also uses `Connection::SendQuery`. For both delivery
modes, `submit_s` times the API call and `fetch_s` times all `Fetch()` calls
and value inspection. In streaming mode, execution may continue during
`Fetch()`, so `fetch_s` is not pure client readback. `query_collect_s` and
`api_readback_s` are populated only for materialized mode.
Process peak RSS covers setup, query, and readback. Newer API runs also record
current RSS and process high-water RSS after setup and immediately after the
query; the high-water counter is cumulative since process start. `DECIDB_PROFILE` records
direct analysis/construction, the existing solver model-build, backend-load,
solve, and solver-output-readback spans, plus result-collector append, combine,
and finalize work inside the query timer. Collector append uses a monotonic
clock for each call and sums elapsed time over workers; it is not a separate
query wall timer or a CPU-time measurement. The spans
overlap the enclosing query timer and should not be added to it.

The in-engine result materialization is still fused with relational execution
in `query_collect_s`. This experiment isolates **API readback** and records
collector operation time, but does not claim a separate wall-clock time or peak
memory for the rank operator, relational execution, or in-engine collection.
The [plan cardinality check](s1_large_scale.md#stored-table-input) shows all rows reach the rank on
one smaller fixture; it does not measure the rank's intermediate bytes.

## Stored source results

The source has an `i` ID and a mixed-sign DOUBLE score. Wide runs also retain
a 512-byte string in every output row. Capacity is 10% of rows. Times below
are seconds; RSS is process peak MiB. Narrow values are medians of two fresh
processes. Wide values are single observations.

| Rows | Payload | Mode | Query and internal collection | API readback | Peak RSS |
|---:|---:|---|---:|---:|---:|
| 1,000,000 | 0 B | Direct | 0.039 | 0.047 | 227 |
| 1,000,000 | 0 B | Gurobi | 0.495 | 0.047 | 520 |
| 1,000,000 | 0 B | HiGHS | 8.405 | 0.048 | 810 |
| 5,000,000 | 0 B | Direct | 0.186 | 0.236 | 1,094 |
| 5,000,000 | 0 B | Gurobi | 2.776 | 0.239 | 2,440 |
| 5,000,000 | 0 B | HiGHS | 42.832 | 0.237 | 2,709 |
| 1,000,000 | 512 B | Direct | 0.283 | 0.344 | 1,523 |
| 1,000,000 | 512 B | Gurobi | 0.574 | 0.331 | 2,026 |
| 1,000,000 | 512 B | HiGHS | 8.498 | 0.341 | 2,184 |
| 5,000,000 | 512 B | Direct | 2.382 | 1.886 | 3,627 |
| 5,000,000 | 512 B | Gurobi | 4.080 | 1.889 | 3,033 |
| 5,000,000 | 512 B | HiGHS | 43.548 | 1.938 | 3,773 |

At five million narrow rows, direct query/collection was about 15 times
faster than Gurobi; including the separately measured API readback reduces
that ratio to about seven times. API readback was 0.236 seconds for direct,
larger than its 0.186-second query/collection. The five-million-row wide
query/collection ratio was about 1.7 times, or about 1.4 times including
readback. Direct peak RSS on that wide API run was about 595 MiB above
Gurobi's. These figures support the large, narrow S1 benefit and show that
output width and consumption can dominate end-to-end cost once solver work is
removed. Single wide runs and a single machine do not establish a general
selection threshold.

For five million narrow rows, median direct analysis and construction were
0.020 and 0.018 ms. The Gurobi model-build, backend-load, solve, and
solver-readback spans were 109, 468, 2,021, and 54 ms; the HiGHS spans were
111, 236, 42,299, and 62 ms. The internal solver readback span is distinct
from the later API readback of all rows. The optimizer spans are tiny on this
case; the large win primarily avoids backend loading and optimization.

## Unused wide source column before pruning

A separate [pre-pruning aggregate sweep](raw/s1_api_stored_aggregate_raw.csv) uses the
same stored source and 10% capacity, but asks the outer query only for
`COUNT(*)`, `SUM(x)`, and `SUM(score*x)`. The 512-byte payload is not in the
returned result. All eight direct/Gurobi runs completed, returned one
aggregate row, and agreed on count, selected count, and objective. These are
single observations per configuration.

| Rows | Source payload | Mode | Query and internal collection | Peak RSS |
|---:|---:|---|---:|---:|
| 1,000,000 | 0 B | Direct | 0.053 s | 226 MiB |
| 1,000,000 | 0 B | Gurobi | 0.565 s | 518 MiB |
| 1,000,000 | 512 B | Direct | 0.147 s | 1,328 MiB |
| 1,000,000 | 512 B | Gurobi | 0.554 s | 1,540 MiB |
| 5,000,000 | 0 B | Direct | 0.173 s | 1,032 MiB |
| 5,000,000 | 512 B | Direct | 0.968 s | 2,238 MiB |
| 5,000,000 | 0 B | Gurobi | 2.905 s | 1,819 MiB |
| 5,000,000 | 512 B | Gurobi | 3.635 s | 2,640 MiB |

At five million rows, the unused wide payload made direct query/collection
about 5.6 times slower than on the narrow source. An `EXPLAIN` on the same
aggregate shape at 100,000 rows shows `score`, `i`, and `payload` in the
`SEQ_SCAN` projections, followed by the score guard and global `WINDOW`.
The original result boundary pinned every source output, so the payload
remained live through the rank even when the parent did not request it.
Process peak RSS includes the stored input table and cannot be attributed
solely to the rank. The query timer excludes table creation, so the time gap
does demonstrate a cost in the DECIDE query path.

Inspect the current plan shape with:

```sql
SET decide_direct_solve='require';
CREATE TEMP TABLE source AS
  SELECT i, i::DOUBLE AS score, rpad(i::VARCHAR, 512, 'p') AS payload
  FROM range(100000) t(i);
EXPLAIN SELECT COUNT(*), SUM(x), SUM(score*x) FROM (
  FROM source DECIDE x(BOOL)
  SUCH THAT SUM(x)<=10000 MAXIMIZE SUM(score*x)
) q;
```

The earlier all-column scan was observed before the pruning change. The
current plan for this query scans only `score`, as measured below.

## Selective output pruning

The shared result boundary now drops an unreferenced output dependency only
when the source plan proves that omitting its evaluation is safe: a stored
table column, a constant, or a passthrough alias through a projection, filter,
or inner comparison join.
The decision output can also be omitted; the hidden rank dependency still
forces full-input score validation. Computed source outputs without that proof
stay live. This matters for correctness: an unused computed payload containing
`error('payload boom')` raises on the solver path and must also raise on the
direct path. The new direct/solver regression checks this case.

With the safe pruning build, two fresh five-million-row, 512-byte stored-source
aggregate runs took 0.218 and 0.190 seconds for direct query/collection
(median 0.204 seconds), versus 0.968 seconds in the single original run.
The [post-pruning raw runs](raw/s1_api_stored_aggregate_safe_pruned_raw.csv) returned
the same count, selected count, and objective as the original run. The wide
aggregate's `EXPLAIN` now scans only `score`; it retains the global window and
score guard. This is strong evidence for removing the unnecessary payload
scan. The before observation and two after observations are too few for a
precise universal speedup estimate.

Peak RSS for these in-memory stored-source processes did not decrease
consistently: source-table creation is included in the process peak and varies
between runs. A window sort-buffer measurement appears below; peak rank
memory still needs separate evidence.
The focused S1 suite, full serializer-verification suite, and C++ DECIDE
regressions pass with the safe pruning rule.

A [two-repeat five-million-row aggregate memory sweep](raw/s1_api_memory_aggregate_raw.csv)
adds snapshots around the timed query. Every run returned one aggregate row
and the same count, selected count, and objective. Narrow setup current RSS
was about 118 MiB, and the process high-water through the query reached 862
MiB in both runs. Wide setup current RSS was 1,899 and 2,070 MiB; the
high-water through the query reached 1,954 and 2,167 MiB. The wide setup
itself had already reached 1,928 and 2,167 MiB. Source-table allocation
dominates wide process RSS, and query work may reuse memory allocated during
setup. Subtracting setup RSS or peak RSS from the later high-water would not
measure the rank's peak intermediate footprint.

## Collector phase check on full output

A fresh [profiled six-run sweep](raw/s1_api_phase_components_raw.csv) on the safe
pruning build returned the full result for five million stored rows, once per
width and mode. Every run completed and matched row count, selected count,
objective, and output bytes. The 512-byte payload therefore remained live in
this experiment. Times below are single observations. Collector append is the
sum of work on parallel worker threads; it can exceed query wall time.

| Payload | Mode | Query and internal collection | Collector append work | API readback |
|---:|---|---:|---:|---:|
| 0 B | Direct | 0.243 s | 0.016 worker-s | 0.230 s |
| 0 B | Gurobi | 2.756 s | 0.014 worker-s | 0.239 s |
| 0 B | HiGHS | 42.237 s | 0.014 worker-s | 0.237 s |
| 512 B | Direct | 1.901 s | 3.870 worker-s | 1.974 s |
| 512 B | Gurobi | 4.209 s | 0.631 worker-s | 1.897 s |
| 512 B | HiGHS | 44.318 s | 0.598 worker-s | 1.986 s |

Collector combine and finalize each totaled less than 0.001 seconds per run.
The wide direct plan carries `payload` through a full-input global `WINDOW`
before the result collector; the solver plan has a `DECIDE` operator over the
source scan. The direct collector's measured append work is much larger than
the solver collectors' for the same returned values. This identifies an
output-materialization cost associated with the current wide direct plan;
the measurements do not isolate how much of that difference comes from the
window's vector layout, memory pressure, or another upstream effect. The
separate relational and collector wall times and query-only peak memory were
measured later with a consuming-aggregate control and a file-backed source; see
[done.md](done.md#execution-versus-collection-perf-01).

A separate five-million-row [`EXPLAIN ANALYZE` pair](raw/s1_explain_wide_vs_pruned.sql)
and its [recorded plans](raw/s1_explain_wide_vs_pruned.txt) compare
`SELECT i, score, payload, x` with `SELECT i, score, x`. Both rank all five
million rows. The wide plan scanned the
payload and reported 1.74 seconds of `WINDOW` operator time and 0.69 seconds
of scan time; the pruned plan scanned only ID and score and reported 0.42 and
0.00 seconds respectively. Whole `EXPLAIN ANALYZE` times were 0.753 and
0.186 seconds. Operator times accumulate across workers, and `EXPLAIN
ANALYZE` does not materialize the five-million-row API result. This shows
that carrying payload through the rank is costly upstream of result collection
as well; its timers are separate from the API sweep and cannot be subtracted
from that sweep's wall time.

## Window sort storage and joined-source pruning

The release build now profiles the distinct buffer-manager allocations held
by a `WINDOW` sort at sink finalization. This is a window-specific storage
snapshot: it excludes later merge buffers, output collection, and the source
table, so it is **not** a peak-memory estimate. Every row below passed through
the window and produced the same selected count and objective within its
source kind. The [stored-source raw runs](raw/s1_window_storage_5m_raw.csv) each
ran once at five million rows:

| Stored source | Requested output | Window input | Window sort buffers | Query and internal collection |
|---|---|---:|---:|---:|
| Narrow | Full rows | 5,000,000 | 229 MiB | 0.182 s |
| Narrow | Aggregate | 5,000,000 | 191 MiB | 0.159 s |
| 512-byte payload | Full rows | 5,000,000 | 2,804 MiB | 2.331 s |
| 512-byte payload | Aggregate | 5,000,000 | 191 MiB | 0.158 s |

The wide full-output rank owns about 2,614 MiB more sort buffers than the
wide aggregate rank. The latter no longer reads or carries the unused payload.
The full and aggregate queries have different output collection costs, so
their total query times are not a rank-only comparison.

The first [joined-source runs before join passthrough pruning](raw/s1_window_join_aggregate_before_raw.csv)
showed a remaining liveness gap. A stored 512-byte payload passed through an
inner join and was unused by the outer aggregate, yet the window held 2,804
MiB and the direct query took 0.754 seconds. The S1 source-output proof
now follows a binding through an inner comparison join only when exactly one
child supplies it and that child's output is already proved safe to skip.
It still rejects computed outputs and other join kinds. Two
[post-change runs](raw/s1_window_join_aggregate_after_raw.csv) of the same wide
joined aggregate took 0.218 and 0.218 seconds, with 191 MiB of window sort
buffers and the same row count, selected count, and objective. The narrow
joined aggregate remained around 0.19–0.22 seconds. A permanent differential
test covers the pruned plan, full output bindings, serializer round trip, and
an unused computed payload that must still raise on direct and solver paths.

## Materialized and streamed full output

A [five-million-row delivery sweep](raw/s1_api_delivery_5m_raw.csv) ran both API
delivery modes on narrow and 512-byte stored rows, once per solver mode. All
twelve runs completed and agreed on row count, selected count, objective, and
payload bytes. Times below add `submit_s` and `fetch_s`; source creation is
outside the sum.

| Payload | Mode | Materialized total | Streamed total |
|---:|---|---:|---:|
| 0 B | Direct | 0.496 s | 0.456 s |
| 0 B | Gurobi | 3.324 s | 3.104 s |
| 0 B | HiGHS | 44.051 s | 42.662 s |
| 512 B | Direct | 4.873 s | 3.448 s |
| 512 B | Gurobi | 5.892 s | 5.390 s |
| 512 B | HiGHS | 46.401 s | 45.662 s |

Three additional [paired wide direct repeats](raw/s1_api_delivery_direct_wide_repeats_raw.csv)
gave median totals of 5.648 seconds materialized and 3.421 seconds streamed.
Materialized result-collector append work varied from 8.25 to 19.24 summed
worker-seconds in those trials; streaming avoided that collector and used a
bounded buffer. Process peak RSS did not improve consistently: the paired
wide runs had median peaks of 3,555 and 3,518 MiB, with substantial
variation in source setup RSS. Streaming can resume the query during fetch,
so this comparison measures a different delivery path with the same SQL
result; it does not isolate relational execution from collection wall time.

## Global cardinality intervals

The current S1 rule also admits exact immutable global lower, equality, and
paired lower/upper bounds. It keeps the same global sort and adds
`COUNT(*) OVER` to the window only when the lower bound is positive. The
count guards nonempty inputs with too few rows; the rank selects the first
`L` rows plus improving rows through `U`. An `EXPLAIN` of the paired form
shows one `WINDOW` containing both `ROW_NUMBER` and the full-partition count.

The [narrow stored-source sweep](raw/s1_api_cardinality_5m_narrow_raw.csv) used
five million rows and aggregate output, two fresh runs per form and mode.
Upper used 10% capacity; lower and equality required 60%; the paired interval
was 60–70%. Every run completed. Direct and Gurobi agreed on selected count
and primary objective within each form. Query times below are medians in
seconds; the source setup and later API readback are excluded.

| Constraint | Chosen | Direct | Gurobi | Direct rank sort buffers |
|---|---:|---:|---:|---:|
| Upper only | 500,000 | 0.170 | 2.853 | 191 MiB |
| Lower only | 3,000,000 | 0.184 | 2.819 | 191 MiB |
| Exact | 3,000,000 | 0.181 | 9.403 | 191 MiB |
| Paired interval | 3,000,000 | 0.182 | 11.459 | 191 MiB |

The lower-bound count did not materially increase the measured direct query
time or sort-buffer snapshot on this source. Gurobi's exact and paired cases
were substantially slower than its upper and lower cases in these runs; this
is a measured workload effect, not a rule-selection threshold.

A separate [512-byte stored-source interval sweep](raw/s1_api_cardinality_5m_wide_raw.csv)
ran once per output shape and mode at five million rows. Full output took
2.176 seconds direct query/collection plus 1.954 seconds API readback,
versus 12.632 plus 1.893 seconds for Gurobi. Direct window sort buffers
were 2,804 MiB. Aggregate output took 0.185 seconds direct versus 12.122
seconds Gurobi; pruning the unused payload left 191 MiB in the direct
window sort. Both output shapes selected 3,000,000 rows with objective
601,317,410.6 in both modes. The wide figures are single observations;
process peak RSS includes source creation and does not isolate query memory.

## Grouped cardinality intervals

The scoped S1 proof now admits source-column `PER` keys and an optional
top-level deterministic source-only `WHEN`. It requires paired bounds to use
identical membership. The relational plan partitions its rank and count by
eligible group; rows with NULL keys or a false `WHEN` remain unconstrained.
The solver reports an empty-aggregate error when a nonempty input has no
eligible row for the constraint, including an all-NULL `PER` input. The
relational plan now checks that case with a global count of eligible rows
*before evaluating scores*, matching the solver's error order when both the
active set is empty and a score is invalid.
Small independent-enumeration and solver-differential tests check those
semantics. This performance sweep exercises the `PER` path on 100 groups,
without a `WHEN` filter.

The [paired 5M-row aggregate sweep](raw/s1_api_grouped_interval_5m_raw.csv) uses
stored rows with `dept = id % 100` and a 60–70% cardinality interval in each
group. It ran twice per width and mode in fresh processes. All eight runs
completed, but the direct version in that sweep lacked the empty-active-set
check. Its direct timings are superseded. A [new two-repeat direct sweep](raw/s1_api_grouped_interval_5m_guard_order_raw.csv)
uses the corrected plan on the same stored source. All four new direct runs
completed; both modes selected 3,000,000 rows with primary objective
601,317,410.6. Source creation and API readback are outside the query timer.
The values are medians in seconds; direct and Gurobi come from separate runs
of the same SQL and source generator:

| Payload | Direct query/collection | Gurobi query/collection |
|---:|---:|---:|
| 0 B | 0.272 | 34.356 |
| 512 B, unused by aggregate | 0.279 | 33.562 |

The corrected direct plan took 0.282/0.263 seconds on narrow rows and
0.281/0.277 seconds on the unused-wide source. It measured 282–300 MiB of
distinct rank-sort buffers, summed once as each hash group finished sorting.
The global active-count window does not add sort buffers, but it makes the
profile's summed window input-row counter 10 million for five million source
rows. The sort-buffer counter is an allocation snapshot across groups, not a
simultaneous peak-memory measurement. Source setup and allocator reuse affect
process RSS, so the peak RSS columns cannot isolate query memory.

The [earlier direct sort-counter sweep](raw/s1_api_grouped_interval_5m_sort_raw.csv)
recorded lower timings but also lacked the active-set check. The original
paired sweep's zero sort-buffer values were a separate sampling error: it
sampled before grouped sorting began. Neither old direct result is a current
S1 timing. A later [intermediate active-guard sweep](raw/s1_api_grouped_interval_5m_active_guard_raw.csv)
put the empty-set guard after score evaluation and therefore had the wrong
error order when both failed; its 0.309/0.325-second medians are also
superseded.

### How to read the grouped sweeps below

Every sweep below uses five million stored rows, 100 `PER dept` groups, aggregate output, and a
512-byte payload the aggregate does not read ("512 B" rows). Times are medians of two fresh direct
processes against one or two fresh Gurobi processes, in seconds, for `query_collect_s`. Source
creation and client readback are outside that timer. "Whole process" adds the source creation,
about 7.6 s for the wide source. Memory figures are in-process high-water MiB through the query.
The "sort buffers" figure sums the rank windows' allocation snapshots; it is not a simultaneous
peak. External peak RSS was not measured in these sweeps: the sandbox denied the `sysctl` call in
`/usr/bin/time`, so the executable was launched directly with `DECIDB_PROFILE=1`. Every completed
run returned five million rows, and direct and Gurobi agreed on the selected count and primary
objective. These are one-machine, few-repeat observations; they do not set a selection threshold.

### Fixed Boolean pins at scale

Source: a 60–70% interval per group, with 1% of rows fixed selected and a disjoint 1% fixed zero.
The plan counts fixed and free rows per group, subtracts the fixed selections from the interval,
and ranks free rows first. [Raw runs](raw/s1_api_grouped_interval_5m_fixed_raw.csv); 3,000,000
selections, objective 588,324,437.9.

| Unused payload | Direct | Gurobi |
|---:|---:|---:|
| 0 B | 0.330 | 36.256 |
| 512 B | 0.326 | 37.517 |

Sort buffers 276–282 MiB. The [current free-row sweep](raw/s1_api_grouped_interval_5m_free_current_raw.csv)
gave direct medians of 0.268 and 0.276. Pin queries add flag columns and count windows, so the times
measure the whole admitted shape, not one operator.

### Source-valued group bounds at scale

Source: `SUM(x)>=30000 PER dept` and `SUM(x)<=cap PER dept`; half of each group stores `cap=35000`
and half `35001`, so the upper bound needs a within-group MIN. 3,000,000 selections, objective
601,317,410.6. [Raw runs](raw/s1_api_grouped_source_bound_5m_raw.csv).

| Unused payload | Direct | Gurobi |
|---:|---:|---:|
| 0 B | 0.453 | 32.018 |
| 512 B | 0.457 | 32.670 |

The plan validates the bound column on every row, then reduces eligible values before scoring and
ranking. Sort buffers 550–578 MiB. The constant-bound interval sweep above used a different stored
schema, so it does not isolate the cost of the reduction.

```sh
python3 benchmark/decide/profile_direct_s1_api.py --rows 5000000 --widths 0 512 \
  --source-kinds stored --output-kinds aggregate --modes direct gurobi \
  --deliveries materialized --cardinalities interval --scopes grouped \
  --pins free --bound-kinds source --repeats 2 --timeout 180
```

### Numeric source-bound widening

After admitting fractional, infinite, and wider numeric source columns, a
[rerun of the integer-cap workload](raw/s1_api_numeric_source_bound_5m_raw.csv) measured direct at
0.5025 s (no payload) and 0.5357 s (512 B), against 0.453 and 0.457 before the widening. The wider
handling costs a little; direct is still far ahead of the 32 s Gurobi runs. Sort buffers 550–568 MiB.

The [fractional DOUBLE-cap sweep](raw/s1_api_double_source_bound_5m_raw.csv) alternates each
group's cap between `35000.5` and `35001.5`. The inclusive upper count is still 35000; the tighter
value requires MIN after DOUBLE conversion.

| Unused payload | Direct | Gurobi |
|---:|---:|---:|
| 0 B | 0.4954 | 32.6941 |
| 512 B | 0.5281 | 33.4266 |

Reproduce one run with the executable directly (replace `source` with `source_double` for the
fractional caps; the other forms below swap in `source_pair`, `source_coalesce`, or the `additive`
objective the same way):

```sh
DECIDB_PROFILE=1 benchmark/decide/results/profile_direct_s1_api \
  5000000 0 stored direct aggregate materialized interval grouped free source
```

For the solver side, replace `direct` with `gurobi` and set `DECIDB_FORCE_SOLVER=gurobi`.

### Paired source bounds at scale

Source: an upper column alternating 35000/35001 and a lower column alternating 30000/29999, so the
interval is [30000, 35000] per group once both columns are reduced. One bound-window stage serves
both clauses. [Raw runs](raw/s1_api_paired_source_bound_5m_raw.csv); 3,000,000 selections.

| Unused payload | Direct | Gurobi |
|---:|---:|---:|
| 0 B | 0.6549 | 31.8961 |
| 512 B | 0.6783 | 33.0310 |

Sort buffers 631–656 MiB.

### Source-bound expressions at scale

Source: `COALESCE(cap,35001)` as the upper bound, with 25% NULL, 25% 35000 and 50% 35001 caps per
group, so each group's limit is 35000; a constant lower bound of 30000. 3,000,000 selections.
[Raw runs](raw/s1_api_source_expression_5m_raw.csv).

| Unused payload | Direct | Gurobi | Direct whole process | Gurobi whole process |
|---:|---:|---:|---:|---:|
| 0 B | 0.5824 | 32.1601 | 0.7323 | 32.3474 |
| 512 B | 0.5792 | 32.4024 | 8.2455 | 40.1063 |

Sort buffers 548–572 MiB.

### Aggregate-local WHEN at scale

Source: a Boolean `active` column, 10% of each group inactive. Both constant bounds use
`SUM(x) WHEN active` (30000 lower, 35000 upper). Inactive rows stay free to be selected. 3,000,000
active selections, 3,250,121 overall, objective 563,866,342.3.
[Raw runs](raw/s1_api_local_when_5m_raw.csv).

| Unused payload | Direct | Gurobi | Direct whole process | Gurobi whole process |
|---:|---:|---:|---:|---:|
| 0 B | 0.3018 | 28.8805 | 0.4446 | 29.0565 |
| 512 B | 0.2979 | 30.4114 | 7.9870 | 38.2439 |

Sort buffers 274–294 MiB. Memory through the query: 666–689 MiB direct against 2,922 MiB Gurobi on
the narrow source, but 3,069–3,149 MiB direct against 2,972 MiB Gurobi with the 512-byte payload.
On the wide fixture direct pays a memory cost even though its query time is lower.

```sh
DECIDB_PROFILE=1 benchmark/decide/results/profile_direct_s1_api \
  5000000 0 stored direct aggregate materialized interval grouped_local_when free constant
```

### Source-valued aggregate-local WHEN at scale

Same rows and 10% inactive share. The lower bound is 30000; the upper comes from a cap column
alternating 35000/35001. `SUM(x) WHEN active` counts only active rows, while the cap's group minimum
includes inactive rows. 3,000,000 active selections, 3,250,121 overall, objective 563,866,342.3.
[Raw runs](raw/s1_api_local_when_source_bound_5m_raw.csv). Replace `constant` with `source` in the
command above to reproduce one.

| Unused payload | Direct | Gurobi | Direct whole process | Gurobi whole process |
|---:|---:|---:|---:|---:|
| 0 B | 0.5664 | 28.8267 | 0.7402 | 29.0349 |
| 512 B | 0.5736 | 29.6506 | 8.2712 | 37.3544 |

Sort buffers 511–538 MiB. Memory through the query: 855–861 MiB direct on narrow rows, 2,936–3,372
MiB direct on wide rows, against 2,639 and 3,116 MiB for Gurobi. These whole-run figures vary too
much to settle the wide-row tradeoff; the query-only sweep in the
[benefit report](s1_benefit_report.md) does: direct is 16% below Gurobi on wide full output.

### Additive linear scores at scale

Source: a 30000–35000 interval per group and the objective `SUM(score*x)+SUM(i*x)`, so each row's
rank score adds two source coefficients in the solver's DOUBLE order. The parent returns one
aggregate row, so the unused payload can be pruned. 3,500,000 selections, objective
11,374,999,356,200. [Raw runs](raw/s1_api_additive_objective_5m_raw.csv).

| Unused payload | Direct | Gurobi | Direct whole process | Gurobi whole process |
|---:|---:|---:|---:|---:|
| 0 B | 0.3051 | 13.9740 | 0.4156 | 14.1100 |
| 512 B | 0.3192 | 15.0499 | 7.9498 | 22.7124 |

The profiled Gurobi runs spent 219–231 ms building the neutral model, 522–536 ms loading the
backend, and 12.8–13.3 s optimizing; direct analysis and construction were each under 0.04 ms.
Memory through the query: 680–693 MiB direct against 2,943 MiB Gurobi on narrow rows; 3,165–3,193
MiB against 3,341 MiB on wide rows, where the source had already reached about 2,643 MiB during
setup. Sort buffers 323–338 MiB.

## Reproduce

Two runner options isolate what the older rows below could not. `--db-dir DIR` builds each
source once into a database file and measures every query in a fresh process that only
opens it, so `process_peak_through_query_mib` is query-only. `--output-kinds consume`
returns one row of sums over every column `full` returns; `full` minus `consume` wall time
estimates result-collection cost. The commands for those sweeps are in the
[benefit report](s1_benefit_report.md#reproduce).

From the repository root after `make release`:

```sh
clang++ -std=c++17 -O2 -I src/include benchmark/decide/profile_direct_s1_api.cpp \
  -L build/release/src -lduckdb -Wl,-rpath,$PWD/build/release/src \
  -o benchmark/decide/results/profile_direct_s1_api
python3 benchmark/decide/profile_direct_s1_api.py --rows 1000000 5000000 --widths 0 --source-kinds stored --repeats 2 --timeout 180
python3 benchmark/decide/profile_direct_s1_api.py --rows 1000000 5000000 --widths 512 --source-kinds stored --repeats 1 --timeout 180
python3 benchmark/decide/profile_direct_s1_api.py --rows 1000000 5000000 --widths 0 512 --source-kinds stored --output-kinds aggregate --modes direct gurobi --repeats 1 --timeout 150
python3 benchmark/decide/profile_direct_s1_api.py --rows 5000000 --widths 512 --source-kinds stored --output-kinds aggregate --modes direct --repeats 2 --timeout 150
python3 benchmark/decide/profile_direct_s1_api.py --rows 5000000 --widths 0 512 --source-kinds stored --output-kinds full --repeats 1 --timeout 180
python3 benchmark/decide/profile_direct_s1_api.py --rows 5000000 --widths 0 512 --source-kinds stored --output-kinds aggregate --modes direct --repeats 2 --timeout 180
python3 benchmark/decide/profile_direct_s1_api.py --rows 5000000 --widths 0 512 --source-kinds stored --output-kinds full aggregate --modes direct --deliveries materialized --repeats 1 --timeout 180
python3 benchmark/decide/profile_direct_s1_api.py --rows 5000000 --widths 0 512 --source-kinds stored_join --output-kinds aggregate --modes direct --deliveries materialized --repeats 2 --timeout 180
python3 benchmark/decide/profile_direct_s1_api.py --rows 5000000 --widths 0 512 --source-kinds stored --output-kinds full --modes direct gurobi highs --deliveries materialized stream --repeats 1 --timeout 180
python3 benchmark/decide/profile_direct_s1_api.py --rows 5000000 --widths 512 --source-kinds stored --output-kinds full --modes direct --deliveries materialized stream --repeats 3 --timeout 180
python3 benchmark/decide/profile_direct_s1_api.py --rows 5000000 --widths 0 --source-kinds stored --output-kinds aggregate --modes direct gurobi --deliveries materialized --cardinalities upper lower exact interval --repeats 2 --timeout 180
python3 benchmark/decide/profile_direct_s1_api.py --rows 5000000 --widths 512 --source-kinds stored --output-kinds full aggregate --modes direct gurobi --deliveries materialized --cardinalities interval --repeats 1 --timeout 180
python3 benchmark/decide/profile_direct_s1_api.py --rows 5000000 --widths 0 512 --source-kinds stored --output-kinds aggregate --modes direct gurobi --deliveries materialized --cardinalities interval --scopes grouped --repeats 2 --timeout 180
python3 benchmark/decide/profile_direct_s1_api.py --rows 5000000 --widths 0 512 --source-kinds stored --output-kinds aggregate --modes direct --deliveries materialized --cardinalities interval --scopes grouped --repeats 2 --timeout 180
python3 benchmark/decide/profile_direct_s1_api.py --rows 5000000 --widths 0 512 --source-kinds stored --output-kinds aggregate --modes direct gurobi --deliveries materialized --cardinalities interval --scopes grouped --pins fixed --repeats 2 --timeout 180
python3 benchmark/decide/profile_direct_s1_api.py --rows 5000000 --widths 0 512 --source-kinds stored --output-kinds aggregate --modes direct --deliveries materialized --cardinalities interval --scopes grouped --pins free --repeats 2 --timeout 180
build/release/decidb -f context/descriptions/08_direct_solve/04_performance/raw/s1_explain_wide_vs_pruned.sql
```

The runner starts a fresh process per mode and uses `/usr/bin/time -l` for
macOS peak RSS. It records raw stdout/stderr and a JSON manifest beside each
CSV in `benchmark/decide/results/`. The checked-in raw CSVs above retain the
measurements used in this report.
