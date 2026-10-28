# Model construction

Spans separate assembly/copies, implied bounds, objective/AVG assembly and indexing; then L0, ABS, not-equal, bilinear, MIN/MAX and construct lowering. Neutral-model phases cover bounds/types, linear/quadratic objectives, constraint expansion, global/general rows and sanity checks. Counters report variables, linear rows/nonzeros, quadratic rows/nonzeros and native constructs.

The old `model_construction_ms` ends before `SolverModel::Build`. The old `solver_ms` includes that neutral build, so those labels alone cannot identify backend time.

## Baseline before the full ANR algorithm

Recorded 2026-10-28 at commit d20d23733b, before any engine change of the ANR plan
(`09_anr_language/anr_algorithm_and_frames_plan.md`), so its phase 7 can check that the
flat workloads did not regress. `make decide-bench-setup`, then
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
