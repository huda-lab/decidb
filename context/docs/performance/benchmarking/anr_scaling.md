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

Apple M1 Pro, 16 GB, Gurobi 13.0.3, branch `Sami-ANR` with ANR phase 7 (helpers written where they are read, D89),
2026-11-04, `python3 benchmark/decide/anr_scaling.py` (10^3 to 10^5 rows). Own time in ms at 10^5 rows and the growth
exponent between 10^4 and 10^5:

| Workload | Own ms at 10^5 | Exponent | Helpers (inlined) at 10^5 |
|---|---:|---:|---:|
| flat-sum | 11.1 | 0.38 | 0 |
| nested-1 | 55.2 | 0.79 | 2 (0) |
| nested-2 | 130.6 | 0.88 | 3 (0) |
| nested-3 | 230.5 | 1.01 | 4 (0) |
| variants-10 | 24.0 | 0.56 | 1 (0) |
| point | 27.4 | 0.67 | 0 |
| range-w1 | 46.1 | 0.78 | 1 (1) |
| range-w5 | 74.6 | 0.86 | 1 (1) |
| range-w50 | 424.3 | 1.09 | 1 (1) |
| range-step7 | 91.1 | 0.92 | 1 (1) |
| range-max | 149.6 | 0.92 | 1 (0) |
| cyclic-repeats | 59.6 | 0.81 | 1 (1) |

Every workload grows at most linearly. Each range's windows are written into the rows of their anchors (one reader
each), so the model has one column and one row per input row; a `MAX` over windows of several members keeps its
helper, and a nest keeps the helpers its clause rows fold a bound into.
