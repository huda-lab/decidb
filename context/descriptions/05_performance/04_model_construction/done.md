# Model construction

Spans separate assembly/copies, implied bounds, objective/AVG assembly and indexing; then L0, ABS, not-equal, bilinear, MIN/MAX and construct lowering. Neutral-model phases cover bounds/types, linear/quadratic objectives, constraint expansion, global/general rows and sanity checks. Counters report variables, linear rows/nonzeros, quadratic rows/nonzeros and native constructs.

The old `model_construction_ms` ends before `SolverModel::Build`. The old `solver_ms` includes that neutral build, so those labels alone cannot identify backend time.

## Baseline before the full ANR algorithm

Recorded 2026-10-28 at commit d20d23733b, before any engine change of the full ANR
algorithm (`09_anr_language/done.md`, phases 0–7), so its last phase could check that the
flat workloads did not regress (below, "After ANR phase 7"). `make decide-bench-setup`, then
`DECIDB_BENCH=1 python3 benchmark/decide/run_benchmarks.py --sizes medium --iterations 3`
on the default backend (Gurobi), macOS 13.6 on Apple silicon. Each figure is the median
of three runs; all 33 runs succeeded. The full record is
`benchmark/decide/results/d20d23733b.json`, which is not committed; compare a later run
against it with `--compare d20d23733b`.

Model construction and the optimizer are DeciDB's own time, the part the ANR work
changes. The solver column is Gurobi's and varies from run to run; as the paragraph above
says, it also holds the neutral `SolverModel::Build`.

| Query | Shape | Variables | Optimizer ms | Model construction ms | Solver ms | Wall s | Peak MiB |
|---|---|---|---|---|---|---|---|
| Q1 | ILP selection | 500,000 | 3.2 | 324 | 1,846 | 2.23 | 880 |
| Q2 | integer domains | 765,001 | 3.3 | 59 | 6,385 | 6.50 | 2,152 |
| Q3 | ABS and norms | 420,000 | 3.4 | 13 | 17,884 | 17.96 | 2,602 |
| Q4 | nested MIN/MAX | 500,002 | 4.4 | 622 | 5,776 | 6.45 | 2,240 |
| Q5 | QP and QCQP | 50,000 | 3.5 | 1 | 4,332 | 4.38 | 210 |
| Q6 | bilinear, McCormick | 637,500 | 4.7 | 54 | 20,213 | 20.36 | 3,245 |
| Q7 | nonconvex | 382,500 | 4.3 | 8 | 6,440 | 6.53 | 2,039 |
| Q8 | feasibility over a join | 124,923 | 3.5 | 745 | 66 | 0.90 | 458 |
| Q9 | maximize MAX | 15,001 | 3.3 | 3 | 110 | 0.13 | 147 |
| Q10 | maximize ABS | 2,500,000 | 4.3 | 232 | 41,359 | 41.82 | 3,221 |
| Q11 | LP allocation | 68,000 | 3.4 | 3 | 143 | 0.16 | 140 |

## After ANR phase 2

Measured 2026-10-28 the same way, the phase-1 build (d626957dc8) and the phase-2 one
back to back, medians of three runs. Every flat workload builds as fast as before (within
a few milliseconds, or faster). Q4, whose objective is the nested
`max(per l_returnflag: sum(keep * l_extendedprice) by (l_returnflag))`, builds in 752 ms
against 583: each group sum is a helper decision now, defined by one equality per group
that stage 06 assembles row by row through the extremum members' path
(`BuildMinMaxGroups`), where the two-level path fed the group sums to the extremum
directly; the model is three columns and three rows larger (1,000,013 rows). Its wall
time is 6.13 s against 7.15 (the solver's share varies from run to run). Inlining a SUM
helper into the one reader that sums it is phase 7 of the plan.

## After ANR phase 5

Measured 2026-10-30 the same way (medium, Gurobi, medians of three runs, all 33
succeeded), on the phase-5 working tree over commit 3649855796, against the d20d23733b
baseline above (`--compare d20d23733b`). Phases 3 to 5 changed no flat workload's model:
variables and rows are the baseline's everywhere but Q4, whose three extra columns and rows
are phase 2's helpers. DeciDB's own time fell from 2,064 to 1,877 ms over the suite; the
solver's share is unchanged within its run-to-run spread (104.6 s against 101.2 s).

| Query | Model construction ms | Solver ms | Wall s | Peak MiB |
|---|---|---|---|---|
| Q1 | 250 (324) | 1,970 | 2.29 | 724 |
| Q2 | 61 (59) | 6,252 | 6.37 | 2,121 |
| Q3 | 13 (13) | 17,340 | 17.45 | 2,701 |
| Q4 | 760 (622) | 4,899 | 5.72 | 2,071 |
| Q5 | 1 (1) | 4,327 | 4.38 | 221 |
| Q6 | 27 (54) | 19,709 | 19.82 | 3,300 |
| Q7 | 8 (8) | 6,426 | 6.52 | 2,089 |
| Q8 | 527 (745) | 66 | 0.68 | 442 |
| Q9 | 3 (3) | 122 | 0.15 | 146 |
| Q10 | 225 (232) | 39,943 | 40.39 | 3,583 |
| Q11 | 3 (3) | 140 | 0.16 | 146 |

The baseline's model construction is in parentheses. Q4 is phase 2's cost, as recorded
above (752 ms then, 760 now): phase 5's listed members cost it nothing. Q10's peak memory
is the solver's and spreads from 2,977 to 3,796 MiB across its three runs (the
baseline's from 3,052 to 3,235).

## After ANR phase 7

Measured 2026-10-30 the same way (medium, Gurobi, medians of three runs, all 33
succeeded), on the phase-7 working tree over commit d9a579e1fb, against the d20d23733b
baseline. Every query builds the baseline's model exactly -- the same columns and rows, Q4's
three helper columns and rows of phase 2 gone, its group sums written into its MAX's
envelope (D83) -- and DeciDB's own time over the suite is 1,675 ms against 2,105 (the
solver's 100.7 s against 104.6 s, within its spread).

| Query | Model construction ms | Solver ms | Wall s | Peak MiB |
|---|---|---|---|---|
| Q1 | 247 (324) | 1,897 | 2.20 | 796 |
| Q2 | 60 (59) | 6,230 | 6.34 | 2,072 |
| Q3 | 14 (13) | 17,302 | 17.39 | 2,860 |
| Q4 | 599 (622) | 5,528 | 6.19 | 2,142 |
| Q5 | 1 (1) | 4,299 | 4.35 | 216 |
| Q6 | 26 (54) | 19,576 | 19.68 | 3,476 |
| Q7 | 8 (8) | 6,205 | 6.29 | 2,041 |
| Q8 | 479 (745) | 74 | 0.63 | 455 |
| Q9 | 2 (3) | 107 | 0.13 | 146 |
| Q10 | 198 (232) | 39,352 | 39.77 | 3,702 |
| Q11 | 3 (3) | 139 | 0.16 | 125 |

The baseline's model construction is in parentheses. Q4 had built in 790 ms at phase 6:
not its model -- three group sums cost three columns -- but two passes doing needless work
on a helper of half a million members, the backward bound pass (`DeriveReducerMemberBounds`,
122 ms on a query with no ABS) and the definitions (117 ms, a hash map per member); both
now skip or accumulate (`../../01_pipeline/06_model_formulation/done.md`), and an extremum
member's row scans while short, which also took a third off Q4's composed MIN/MAX. Q10 and
Q3 had regressed in phase 6, an exact ABS's argument row built afresh per data row (Q10 300
ms); one row reused brings them to 198 and 14.

### How the ANR workloads grow

`benchmark/decide/anr_scaling.py --max 1000000` (`../../02_operations/benchmarking.md`): each
workload over a synthetic table of 10³ to 10⁶ rows at fixed key cardinalities, depth, variant
count and frame width. DeciDB's own time (optimizer and model construction) at 10⁶ rows,
the model's size there, and the growth exponent between 10⁵ and 10⁶ rows (1 is linear):

| Workload | Own ms at 10⁶ | Columns | Rows | Exponent |
|---|---|---|---|---|
| `flat-sum` (no helper) | 66 | 1,000,000 | 10 | 0.83 |
| `nested-1` (SUM of MAX) | 506 | 1,000,110 | 1,000,010 | 0.99 |
| `nested-2` | 1,178 | 1,001,110 | 1,001,010 | 0.99 |
| `nested-3` | 2,203 | 2,011,110 | 3,011,010 | 0.96 |
| `variants-10` (ten per class) | 238 | 1,000,011 | 110 | 0.97 |
| `point` | 208 | 1,000,000 | 1,000,000 | 1.01 |
| `range-w1` | 347 | 1,000,000 | 1,000,000 | 1.02 |
| `range-w5` (AVG) | 588 | 1,000,000 | 1,000,000 | 1.02 |
| `range-w50` (AVG) | 3,709 | 1,000,000 | 1,000,000 | 1.04 |
| `range-step7` | 685 | 1,000,000 | 1,000,000 | 1.00 |
| `range-max` (MAX of five) | 1,705 | 2,000,000 | 5,999,000 | 1.07 |
| `cyclic-repeats` | 407 | 1,000,000 | 1,000,000 | 0.98 |

Every range over decisions is written into its anchor's row: a million rows over the
million decisions, where a helper per window had doubled both. The flat workloads sit under
1 because a fixed cost still shows at 10⁵. `range-max` keeps its helpers (a MAX of several
members), six million rows whose provenance makes the model memory-bound
(`../08_memory_and_scaling/todo.md`); building its extremum links one window at a time took
it from 1.18 to 1.07. `range-w50` reads fifty million values at 10⁶ rows, above the default
`decide_frame_limit`, which the script raises.
