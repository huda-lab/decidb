# S1 direct solve: benefit report

This report lists where the direct path is materially better than the solver path,
where it is not, and how that bears on the `decide_direct_solve` default. Every comparison below completed on all compared paths and agreed on row
count, selected count, and primary objective. No timeout, solver limit, unsupported
case, or memory skip is counted as a win.

All numbers come from one machine (macOS arm64, 11 cores, 18 GiB RAM, 4 DuckDB
threads) and few repeats. They support a policy; they do not set a threshold.

## How the numbers were taken

- **Query-only memory.** The source table is built once into a database file by a
  `prepare` run. Each measured run is a fresh process that opens the file and runs one
  query, so its peak (`process_peak_through_query_mib`) covers the query only. The
  process baseline after opening is about 17 MiB, and a plain scan of each source peaks
  at 30–385 MiB ([scan floor](raw/s1_perf02_scan_floor_raw.csv)), so the peaks below are
  almost entirely each mode's own working memory. Earlier sweeps built the source in the
  same process, so their high-water marks included table creation.
- **Execution versus collection.** `consume` returns one row of sums over every column
  that `full` returns, so the same relational plan runs with a negligible collector.
  Collection cost is the `full` wall time minus the `consume` wall time for the same
  mode and source. This is a differential, not an exact timer: the collector overlaps
  the plan's final stage, and a collector that runs in parallel has no exact wall time.
- Times are the median of three runs (two for HiGHS). Memory ranges are min–max.

## Where direct wins

Five million stored rows, aggregate output unless noted. Direct and Gurobi time are the
median query time; "×" is Gurobi divided by direct.

| Shape | Direct | Gurobi | × | Direct peak | Gurobi peak |
|---|---:|---:|---:|---:|---:|
| Global upper bound, narrow, full output | 0.169 s | 2.732 s | 16 | 1,005 MiB | 2,351 MiB |
| Global upper bound, 512-byte payload unused (pruned) | 0.155 s | 3.283 s | 21 | 773 MiB | 2,528 MiB |
| 100 `PER` groups, fixed pins | 0.322 s | 33.612 s | 104 | 529 MiB | 3,843 MiB |
| 100 groups, aggregate-local `WHEN` | 0.303 s | 28.383 s | 94 | 535 MiB | 3,001 MiB |
| 100 groups, aggregate-local `WHEN`, source-valued bound | 0.555 s | 27.883 s | 50 | 710 MiB | 3,050 MiB |
| 100 groups, source-valued bounds | 0.505 s | 31.009 s | 61 | 700 MiB | 3,436 MiB |
| 100 groups, paired source-valued bounds | 0.649 s | 31.075 s | 48 | 780 MiB | 3,338 MiB |
| 100 groups, `COALESCE` source-valued bound | 0.568 s | 31.355 s | 55 | 685 MiB | 3,109 MiB |

HiGHS was measured only on the global shapes, where it took about 43 s at this size, so
these ratios understate the gain against it. Raw rows:
[query-only memory sweep](raw/s1_perf02_query_memory_5m_raw.csv).
Direct memory is at least four times lower than Gurobi's in every grouped shape (4.3×
at the closest, 7.3× at the widest gap). The earlier concern
that wide `WHEN` shapes cost more memory than Gurobi came from source-table creation in
the same process; measured query-only, direct is far below Gurobi.

## Where direct helps less or not at all

| Case | Result |
|---|---|
| **Small inputs, narrow** (1,000 to 10,000 rows) | Every path finishes within about 20 ms, so timer resolution limits the comparison ([scale sweep](raw/s1_scale_raw.csv)). |
| **Small inputs, wide, trivial capacity** (100,000 rows, 512-byte payload retained, capacity 0 or all rows) | The one measured case where direct is slower: 196.5 and 193.5 ms against 184.0 and 188.5 ms for Gurobi (about 5% in two-run medians), and about 187 MiB against 140–144 MiB. See [counterexamples](s1_large_scale.md#width-limit-and-counterexamples). |
| **Capacity at the extremes** (5M narrow rows, capacity 0 or all rows) | Direct still wins, but by 3 to 4× instead of 16× (0.39 s versus 1.1–1.3 s for Gurobi). |
| **Wide rows with full output** (5M rows, 512-byte payload returned) | Direct 1.996 s versus Gurobi 3.639 s (1.8×) and HiGHS 43.943 s. With client readback added, 3.94 s versus 5.53 s (1.4×). Readback is about 1.9 s on every path and cannot be avoided. |
| **Result collection on wide rows** | Direct's plan executes in 0.475 s with all columns live, but collecting 5M wide rows costs about 1.5 s more. Gurobi's collection costs 0.46 s and HiGHS's 0.82 s. For wide full output the collector, not the relational plan, is direct's cost. The cause is not isolated (see below). |
| **Wide rows, memory** (5M × 512 B, full output) | Direct 2,993 MiB, Gurobi 3,566 MiB, HiGHS 2,797 MiB. Direct is 16% below Gurobi and 7% above HiGHS. The ranges of the three do not overlap. |
| **Streaming** | Streaming a wide direct result brought the total from 5.6 s to 3.4 s in an earlier sweep, so delivery mode matters as much as the plan. |

### Execution versus collection, wide output

| Mode | `full` | `consume` | Collection (difference) | Client readback |
|---|---:|---:|---:|---:|
| Direct | 1.996 s | 0.475 s | 1.52 s | 1.946 s |
| Gurobi | 3.639 s | 3.177 s | 0.46 s | 1.891 s |
| HiGHS | 43.943 s | 43.120 s | 0.82 s | 1.949 s |

Narrow rows collect in 0.005 s (direct) and show no measurable collection cost for
Gurobi (HiGHS was not run on narrow rows). Run-to-run ranges: direct `full` 1.71–2.00 s and `consume` 0.43–0.49 s;
Gurobi `full` 3.60–3.67 s and `consume` 3.16–3.34 s; HiGHS has two runs, so its
difference carries about half a second of noise. Raw rows:
[collection sweep](raw/s1_perf01_collection_5m_raw.csv).

What this does not establish: why direct's collection is expensive. The window hands rows
back from its sort buffers, which hold 2,804 MiB for the wide plan, and a
`EXPLAIN ANALYZE` of the window shows 1.74 s of operator time for the wide plan versus
0.42 s pruned, both summed over worker threads ([plans](raw/s1_explain_wide_vs_pruned.txt)). That fits a gather cost from the
sorted buffers, but the measurements here do not separate it from other causes.

## What this supports

- For the admitted S1 shapes on large sources (millions of rows), direct is faster by
  tens of times and uses a fraction of the memory when the parent consumes aggregates or
  few columns. This holds for grouped, filtered, pinned, and source-valued forms.
- For wide full output the win is real but modest (1.4 to 1.8×), and direct's own
  collector cost is the largest part of its time.
- On small inputs it is neutral.
- The only measured case where it is slower than a solver is 100,000 wide rows at
  trivial capacity (about 5%, two runs). No completed comparison disagreed on a result.

These results support `auto` as the default for S1-proved shapes, and `auto` became the
default on 2026-10-03. The policy and what users can notice are recorded in
[decisions.md](../00_design/decisions.md#7-selection-policy).

## Reproduce

From the repository root after `make release` and building the API runner (commands in
[the API report](s1_api_phase.md#reproduce)):

```sh
python3 benchmark/decide/profile_direct_s1_api.py --rows 5000000 --widths 0 512 --output-kinds full consume --modes direct gurobi --repeats 3 --db-dir /tmp/s1db --keep-db
python3 benchmark/decide/profile_direct_s1_api.py --rows 5000000 --widths 512 --output-kinds full consume --modes highs --repeats 2 --db-dir /tmp/s1db
python3 benchmark/decide/profile_direct_s1_api.py --rows 5000000 --widths 512 --output-kinds aggregate --modes direct gurobi --scopes global --cardinalities upper --repeats 2 --db-dir /tmp/s1db
python3 benchmark/decide/profile_direct_s1_api.py --rows 5000000 --widths 0 512 --output-kinds aggregate --modes direct gurobi --scopes grouped_local_when --cardinalities interval --bound-kinds constant source --repeats 2 --db-dir /tmp/s1db
python3 benchmark/decide/profile_direct_s1_api.py --rows 5000000 --widths 0 --output-kinds aggregate --modes direct gurobi --scopes grouped --pins fixed --cardinalities interval --repeats 2 --db-dir /tmp/s1db
python3 benchmark/decide/profile_direct_s1_api.py --rows 5000000 --widths 0 --output-kinds aggregate --modes direct gurobi --scopes grouped --cardinalities interval --bound-kinds source source_pair source_coalesce --repeats 2 --db-dir /tmp/s1db
```

The wide database needs about 3 GiB of disk; the Gurobi runs use four threads for about
30 seconds each at this size.
