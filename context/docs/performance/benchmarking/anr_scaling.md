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

## Running

```bash
python3 benchmark/decide/anr_scaling.py                  # 10^3 .. 10^5 rows
python3 benchmark/decide/anr_scaling.py --max 1000000    # up to 10^6
python3 benchmark/decide/anr_scaling.py --only range     # the workloads whose name matches
```

Results go to `benchmark/decide/results/anr_scaling.csv`. No database is needed.

## Measured

Apple M1 Pro, 16 GB, Gurobi 13.0.3, branch `Sami-ANR` with the deck's model sizes (D89: a bound on a sum helper a row
it is written into, a group of one member written in), 2026-11-04, `python3 benchmark/decide/anr_scaling.py` (10^3 to
10^5 rows). Own time in ms at 10^5 rows and the growth exponent between 10^4 and 10^5:

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
helper, and a nest writes its outermost sum into the clause row that bounds it and keeps its `MAX` helpers; an inner
sum is written into the `MAX` above it only where that adds no nonzero (D89), which at 10^5 rows it does not.

That holds because every width here is fixed. A range whose width grows with the input reads O(n) positions per window,
so its model grows quadratically: `x <= sum(from first to previous else 0: x) over (t.id) + 1` has 5,049 nonzeros at
100 rows, 20,099 at 200 and 80,199 at 400 (n²/2). A running total written as such a range is quadratic; a prefix-sum
chain would make it linear, and is not built.
