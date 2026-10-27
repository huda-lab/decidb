# Direct-solve benchmark

`benchmark/decide/direct_solve/run.py` times direct solve against the forced solver, Gurobi and HiGHS, on the same
DECIDE query as the data grows. Direct solve is the optimizer rewrite that answers some DECIDE problems with relational
operators instead of a solver ([`../../direct_solve/README.md`](../../direct_solve/README.md)). The numbers feed the
paper's direct-solve section. There is one query per direct-solve rule that works today: S1, A1 and R1.

## Running

```bash
python3 benchmark/decide/direct_solve/run.py                                  # every query, every size, every mode
python3 benchmark/decide/direct_solve/run.py --sf 0.00125 0.0125 --queries r1  # a subset
```

| Option | Default | Meaning |
|---|---|---|
| `--sf` | `0.00125 0.0125 0.125 2` | TPC-H scale factors, run smallest first |
| `--queries` | every file in `queries/` | `s1`, `a1`, `r1` |
| `--modes` | `direct gurobi highs` | must include `direct`, the baseline for every speedup and objective |
| `--repeats` | 3 | runs of every mode per cell |
| `--timeout` | 600 | seconds before a run is killed |
| `--solver-budget` | 300 | a solver runs a size only if its median at the previous size was at most this |
| `--db-dir` | `benchmark/decide/databases/direct_solve/` | where the TPC-H databases are cached |

It needs the release CLI and a Gurobi license. The full sweep at the largest size runs the solvers on 1.6M decisions;
start with the small scale factors.

## Data

Each scale factor is one TPC-H database built once with DuckDB's `CALL dbgen(sf=...)` and cached as
`tpch_sf{sf}.db` in `--db-dir` (gitignored). A stored database is reused as is. Every query reads `partsupp`, one
decision per row:

| Scale factor | `partsupp` rows (decisions) | Suppliers | Parts |
|---:|---:|---:|---:|
| 0.00125 | 800 | 10 | 200 |
| 0.0125 | 9,600 | 120 | 2,400 |
| 0.125 | 100,000 | 1,250 | 25,000 |
| 2 | 1,600,000 | 20,000 | 400,000 |

dbgen drops the fractional part of a scale factor above 1 (`sf=1.25` generates `sf=1`), so the largest size is a whole
number.

**Why TPC-H.** The tables of the paper's Figure 1 (Depots, Regions, Routes) have no standard generator. TPC-H does, and
its tables play the same roles: a supplier is a depot, a `partsupp` row is a route, `ps_availqty` is the route's
capacity and `ps_supplycost` its unit cost.

## Queries

Each query uses one table, `partsupp`, and only SELECT, DECIDE, FROM, SUCH THAT and the objective: no joins, filters
or subqueries. The time then measures the optimization and not other operators.

| Query | Rule | DECIDE part | What it asks |
|---|---|---|---|
| `s1.sql` | S1, uniform matroid | `pick(BOOL)`, `SUM(pick) by (ps_suppkey) <= 2`, `MAXIMIZE SUM(ps_availqty * pick)` | each supplier keeps its two offers with the most stock |
| `a1.sql` | A1, separable box | `ship(REAL)`, `ship BETWEEN 0 AND ps_availqty`, `MINIMIZE SUM(POWER(ship - 5000, 2))` | ship each offer as close to 5,000 units as its stock allows |
| `r1.sql` | R1, continuous knapsack | `ship(REAL)`, `ship BETWEEN 0 AND ps_availqty AND SUM(ship) by (ps_partkey) <= 5000`, `MAXIMIZE SUM(ps_supplycost * ship)` | each part ships at most 5,000 units across its suppliers, at the highest total cost |

Direct solve does not read a reducer's `by` key yet, so `s1.sql` and `r1.sql` fail in `direct` mode and run on the
solver path alone until it does.

A1 minimizes a square so that each row has an optimum inside its range and the solver faces a QP, not a problem
whose answer sits at a bound.

Each file carries two header comments that `run.py` reads: `-- decision: <column>` and `-- objective: <expression>`.
The runner wraps the query as `SELECT COUNT(*), SUM(<decision>), <objective> FROM (<query>) q`, so every run returns
the decision count and the objective it reached, and the time does not include sending every row to the client.

## Modes

| Mode | Setting | Effect |
|---|---|---|
| `direct` | `SET decide_direct_solve='require'` | direct solve answers the query, or the query fails |
| `gurobi` | `'off'`, `DECIDB_FORCE_SOLVER=gurobi` | the solver path on Gurobi |
| `highs` | `'off'`, `DECIDB_FORCE_SOLVER=highs` | the solver path on HiGHS |

`require` means a query the rule does not take fails instead of falling back to the solver, so a reported speedup can
never be the solver timed against itself.

## Method

- Every run is a fresh `decidb` process under `/usr/bin/time -l` that opens the stored database `-readonly` and runs
  one statement. Its wall time and peak resident set size are the query's own, plus process start and database open.
- Every mode runs `--repeats` times per (query, scale factor) cell. The result reports the median wall time with the
  minimum and maximum, and the largest peak memory.
- **Agreement.** At every size, every finished run's objective in every mode must match direct solve's within 1e-6
  relative (`|a - b| <= 1e-6 * max(1, |a|, |b|)`). A mismatch is printed, written to the CSV as `agree = NO`, and
  stops the sweep with a non-zero exit, so no wrong answer is timed further.
- **Timeouts and errors.** A run past `--timeout` is killed with its whole process group (`time` and the `decidb` it
  started) and reported as `timeout`. A run that fails is reported as `error`. Neither is ever reported as a time.
- **Machine-heat rule.** Sizes run smallest first. A solver runs the next size of a query only when its median at the
  previous size was within `--solver-budget`; otherwise the cell reads `skipped (...)` with the reason.

## Output

Each sweep writes two files to `benchmark/decide/results/direct_solve/` (gitignored), named by the UTC start time. They
sit in their own folder, as the archived S1 profilers' do, because `view_results.py` opens the newest JSON in
`results/`.

- `direct_solve_<stamp>.csv`: one row per (query, scale factor) with the decision count, direct solve's objective,
  the agreement flag, each mode's median, minimum and maximum seconds and peak MiB, and the speedup of direct solve
  over each solver (solver median divided by direct median).
- `direct_solve_<stamp>.json`: a manifest with the commit, whether `src/`, `third_party/` or
  `benchmark/decide/direct_solve/` has uncommitted changes, the binary and its build time, the DeciDB, Gurobi and
  HiGHS versions, the thread count, the CPU, core count, memory, platform and the arguments.

The script also prints the table as Markdown while it runs.

## Known limits

- Times include process start and database open, about 20 ms. Below about 10K decisions both paths take that long,
  so the smaller sizes show no speedup.
- TPC-H group sizes are fixed across scale factors: 80 offers per supplier and 4 suppliers per part. Only the number
  of groups grows, never their size.
- Each query runs at one parameter setting: K = 2 for S1, a target of 5,000 for A1 and a cap of 5,000 per part for R1.
- HiGHS cannot solve A1 beyond 800 decisions. Its QP solver stops with "Large nullspace" once more than 4,000
  variables end strictly inside their bounds, and about half of A1's do (`../../pipeline/07_solver/done.md`, HiGHS
  backend). The cell reads `error` at 9,600 and the larger sizes are skipped. This is a limit of HiGHS's QP method,
  not of the model: the bounds reach HiGHS as column bounds, not rows.
- Gurobi and HiGHS solve to a gap of 0 (`../../pipeline/07_solver/done.md`, Optimality gap), so on S1 every path
  returns the same optimum; Gurobi's default gap of 1e-4 returned a different, slightly lower one on each run.

## Latest results

TODO: fill with the numbers, commit, machine and manifest of the rerun.
