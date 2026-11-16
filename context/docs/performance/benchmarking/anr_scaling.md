# ANR scaling

`benchmark/decide/anr_scaling.py` asks whether DeciDB's own time grows linearly with the input on the ANR workloads:
reducers nested to depth 1, 2 and 3 at ten keys a level, ten variants per class (D70), a point frame, ranges of width
1, 5 and 50 and of step 7, a `MAX` over a range, and a cyclic range with repeats (D76). Each is one DECIDE query over a
synthetic `range(n)` table whose key cardinalities, nesting depth, variant count and frame width are fixed, so every
helper, window and member grows with `n` alone. Every decision is `REAL`, so the solve is an LP: what is measured is the
model, not a branch-and-bound.

DeciDB's own time is the optimizer's and the model construction's (`DECIDB_BENCH`), the solve left out. Beside it the
script records the model's size and the ANR counters `DECIDB_PROFILE` prints (`model.variables`, `model.linear_rows`,
`model.linear_nonzeros`, `anr.frame_windows`, `anr.frame_positions`, `anr.helpers`, `anr.helper_instances`,
`anr.members`, `anr.member_terms`, `anr.inlined_helpers`; `../../pipeline/08_execution/done.md`), and prints each
workload's growth exponent: the slope of log(own time) against log(n) between the two largest sizes, about 1 when it
grows linearly. Direct solve is off (`decide_direct_solve = 'off'`), since it answers a flat sum without a model, and
`decide_frame_limit` is raised so a range of fifty over a million anchors is not refused.

It also records the peak memory of the process that ran the query, `peak_rss_mb`: its whole peak resident set, so the
solver's share is in it. It is what `wait4` reports for that child alone; `getrusage(RUSAGE_CHILDREN)` keeps the largest
child so far, so a small workload after a large one would report the large one's peak. `ru_maxrss` counts bytes on macOS
and KiB on Linux; only the macOS count has been checked against `/usr/bin/time -l`.

Each workload runs three times at each size (`--runs`), and every time and the peak are stored as their median over
those runs. One run moves by more than the 1.5x the evaluator rewrite's gates allow: five runs at 10^5 rows on a loaded
host gave `range-w5` 88 to 300 ms of own time and 258 to 293 MB of peak. Each measure is its own median, so `own_ms`
need not equal the stored `optimizer_ms` plus `model_ms`. A run that fails is stored as the row, and that size is not
run again.

## Running

```bash
python3 benchmark/decide/anr_scaling.py                  # 10^3 .. 10^5 rows
python3 benchmark/decide/anr_scaling.py --max 1000000    # up to 10^6
python3 benchmark/decide/anr_scaling.py --only range     # the workloads whose name matches
python3 benchmark/decide/anr_scaling.py --runs 5         # the median of five runs
python3 benchmark/decide/anr_scaling.py --phase P0       # store the numbers as P0's
python3 benchmark/decide/anr_scaling.py --phase P2 --against P0
```

Results go to `benchmark/decide/results/anr_scaling.csv`, or with `--phase P` to `anr_scaling_P.csv`. No database is
needed. Each row holds the `phase`, the workload and size, `status`, `own_ms`, `peak_rss_mb`, `optimizer_ms`,
`model_ms`, `solver_ms`, `runs`, the counters above, the exponent and any `error`.

A phase is a stored baseline that a later phase of the evaluator rewrite is held to: P0 is taken before the rewrite,
and its gates allow a later phase at most 1.5x P0's peak memory and own time. A run with `--phase` replaces only the
workloads and sizes it measured and keeps the rest, so `--only` runs add up to one phase. `--against P` prints each
workload's time and peak as a multiple of P's, read before this run writes, so `--phase P --against P` compares a rerun
with P's stored numbers.

## Measured

Apple M1 Pro, 16 GB, Gurobi 13.0.3, branch `Sami-ANR` with the deck's model sizes (D89: a bound on a sum helper a row
it is written into, a group of one member written in), 2026-11-04, `python3 benchmark/decide/anr_scaling.py` (10^3 to
10^5 rows), one run per size, before peak memory was recorded. Own time in ms at 10^5 rows and the growth exponent
between 10^4 and 10^5:

| Workload | Own ms at 10^5 | Exponent | Helpers (inlined) at 10^5 |
|---|---:|---:|---:|
| flat-sum | 14.7 | 0.49 | 0 |
| nested-1 | 57.2 | 0.77 | 2 (1) |
| nested-2 | 142.7 | 0.91 | 3 (1) |
| nested-3 | 238.5 | 1.02 | 4 (1) |
| variants-10 | 24.0 | 0.57 | 1 (0) |
| point | 29.3 | 0.68 | 0 |
| range-w1 | 45.5 | 0.76 | 1 (1) |
| range-w5 | 73.1 | 0.83 | 1 (1) |
| range-w50 | 416.7 | 1.08 | 1 (1) |
| range-step7 | 87.7 | 0.92 | 1 (1) |
| range-max | 162.1 | 0.91 | 1 (0) |
| cyclic-repeats | 59.5 | 0.81 | 1 (1) |

Every workload grows at most linearly. Each range's windows are written into the rows of their anchors (one reader
each), so the model has one column and one row per input row; a `MAX` over windows of several members keeps its
helper, and a nest writes its sums into the clause row that bounds it and keeps its `MAX` helpers; an inner sum under
a `MAX` is written into the `MAX` above it only where that adds no nonzero (D89), which at 10^5 rows it does not.

That holds because every width here is fixed. A range whose width grows with the input reads O(n) positions per window,
so its model grows quadratically: `x <= sum(from first to previous else 0: x) over (t.id) + 1` has 5,049 nonzeros at
100 rows, 20,099 at 200 and 80,199 at 400 (n²/2). A running total written as such a range is quadratic; a prefix-sum
chain would make it linear, and is not built.
