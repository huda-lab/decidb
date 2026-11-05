# Direct-solve benchmark

`benchmark/decide/direct_solve/run.py` times direct solve against the forced solver, Gurobi and HiGHS, on the same
DECIDE query as the data grows. Direct solve is the optimizer rewrite that answers some DECIDE problems with relational
operators instead of a solver ([`../../direct_solve/README.md`](../../direct_solve/README.md)). The numbers feed the
paper's direct-solve section. There is a query for each direct-solve rule that works today: A1, and S1, R1 and N1 in two
forms each, many small groups and one bound over every row.

## Running

```bash
python3 benchmark/decide/direct_solve/run.py                             # every query, every size: an overnight run
python3 benchmark/decide/direct_solve/run.py --sf 0.125 0.25 --queries r1  # a subset
```

| Option | Default | Meaning |
|---|---|---|
| `--sf` | `0.125` doubling to `512` | TPC-H scale factors, run smallest first |
| `--queries` | every file in `queries/` | `s1`, `s1_global`, `a1`, `r1`, `r1_global`, `n1`, `n1_groups` |
| `--modes` | `direct direct_1t gurobi` | must include `direct`, the baseline for every speedup and objective; `highs` is also available |
| `--repeats` | 3 | runs of every mode per cell |
| `--repeat-budget` | 1800 | a run past this many seconds is its cell's last, so the `runs` column can read less than `--repeats` |
| `--timeout` | 21600 | seconds before a run is killed |
| `--solver-budget` | 7200 | a solver runs a size only if its median at the previous size was at most this |
| `--db-dir` | `benchmark/decide/databases/direct_solve/` | where the TPC-H databases are cached |

It needs the release CLI and a Gurobi license. A row is written as its cell finishes, so a sweep can be stopped at any
point and keeps what it measured. The databases up to `sf=512` take about 270 GB of disk.

## Data

Each scale factor is one TPC-H database built once with DuckDB's `CALL dbgen(sf=...)`, when the sweep reaches it, and
cached as `tpch_sf{sf}.db` in `--db-dir` (gitignored). A stored database is reused as is. Every query reads `partsupp`, one
decision per row:

| Scale factor | `partsupp` rows (decisions) | Suppliers | Parts |
|---:|---:|---:|---:|
| 0.125 | 100,000 | 1,250 | 25,000 |
| 1 | 800,000 | 10,000 | 200,000 |
| 512 | 409,600,000 | 5,120,000 | 102,400,000 |

Each scale factor between these doubles the one before it.

dbgen drops the fractional part of a scale factor above 1 (`sf=1.25` generates `sf=1`), so the larger sizes are whole
numbers.

**Why TPC-H.** The tables of the paper's Figure 1 (Depots, Regions, Routes) have no standard generator. TPC-H does, and
its tables play the same roles: a supplier is a depot, a `partsupp` row is a route, `ps_availqty` is the route's
capacity and `ps_supplycost` its unit cost.

## Queries

Each query uses one table, `partsupp`, and only SELECT, DECIDE, FROM, SUCH THAT and the objective: no joins, filters
or subqueries. The time then measures the optimization and not other operators.

| Query | Rule | DECIDE part | What it asks |
|---|---|---|---|
| `s1.sql` | S1, uniform matroid | `pick(BOOL)`, `SUM(pick) by (ps_suppkey) <= 2`, `MAXIMIZE SUM(ps_availqty * pick)` | each supplier keeps its two offers with the most stock |
| `s1_global.sql` | S1, uniform matroid | `pick(BOOL)`, `SUM(pick) <= 0.1 * COUNT(*)`, `MAXIMIZE SUM(ps_availqty * pick)` | keep the tenth of all offers with the most stock |
| `a1.sql` | A1, separable box | `ship(REAL)`, `ship BETWEEN 0 AND ps_availqty`, `MINIMIZE SUM(POWER(ship - 5000, 2))` | ship each offer as close to 5,000 units as its stock allows |
| `r1.sql` | R1, continuous knapsack | `ship(REAL)`, `ship BETWEEN 0 AND ps_availqty AND SUM(ship) by (ps_partkey) <= 5000`, `MAXIMIZE SUM(ps_supplycost * ship)` | each part ships at most 5,000 units across its suppliers, at the highest total cost |
| `r1_global.sql` | R1, continuous knapsack | `ship(REAL)`, `ship BETWEEN 0 AND 1 AND SUM(ship * ps_supplycost) <= 0.1 * SUM(ps_supplycost)`, `MAXIMIZE SUM(ps_availqty * ship)` | take a fraction of each offer, spending at most a tenth of the total cost, to secure the most stock |
| `n1.sql` | N1, Euclidean ball | `ship(REAL)`, `SUM(POWER(ship, 2)) <= 1000000`, `MAXIMIZE SUM(ps_supplycost * ship)` | ship along each offer's cost, with one limit on the sum of squared amounts over every offer |
| `n1_groups.sql` | N1, Euclidean ball | `ship(REAL)`, `SUM(POWER(ship, 2)) by (ps_suppkey) <= 1000000`, `MAXIMIZE SUM(ps_supplycost * ship)` | the same, with a limit for each supplier over its 80 offers |

A1 minimizes a square so that each row has an optimum inside its range and the solver faces a QP, not a problem
whose answer sits at a bound.

Each file carries two header comments that `run.py` reads: `-- decision: <column>` and `-- objective: <expression>`.
The runner wraps the query as `SELECT COUNT(*), SUM(<decision>), <objective> FROM (<query>) q`, so every run returns
the decision count and the objective it reached, and the time does not include sending every row to the client.

## Modes

| Mode | Setting | Effect |
|---|---|---|
| `direct` | `SET decide_direct_solve='require'` | direct solve answers the query, or the query fails |
| `direct_1t` | `'require'`, `SET threads=1` | the same, on one thread, so a speedup can be told apart from the core count |
| `gurobi` | `'off'`, `DECIDB_FORCE_SOLVER=gurobi` | the solver path on Gurobi |
| `highs` | `'off'`, `DECIDB_FORCE_SOLVER=highs` | the solver path on HiGHS |

A solver mode also sets `DECIDB_TIME_LIMIT` above `--timeout`, because DeciDB otherwise stops a solve at 300 s and the
query fails, and `DECIDB_BENCH`, which gives the model-build and solve split.

`require` means a query the rule does not take fails instead of falling back to the solver, so a reported speedup can
never be the solver timed against itself.

## Method

- Every run is a fresh `decidb` process under `/usr/bin/time` that opens the stored database `-readonly` and runs
  one statement. Its wall time and peak resident set size are the query's own, plus process start and database open.
- A size's database is read once before its first run, so no mode pays for the first read from disk.
- Every mode runs `--repeats` times per (query, scale factor) cell, stopping early after a run past `--repeat-budget`.
  The result reports the median wall time with the minimum and maximum, and the largest peak memory.
- **Agreement.** At every size, every finished run's objective in every mode must match direct solve's within 1e-6
  relative (`|a - b| <= 1e-6 * max(1, |a|, |b|)`). A mismatch is printed, written to the CSV as `agree = NO`, and
  stops the sweep with a non-zero exit, so no wrong answer is timed further. A size no solver finished reads
  `agree = n/a`.
- **Timeouts and errors.** A run past `--timeout` is killed with its whole process group (`time` and the `decidb` it
  started) and reported as `timeout`. A run that fails is reported as `error`. Neither is ever reported as a time.
- **Machine-heat rule.** Sizes run smallest first. A solver runs the next size of a query only when its median at the
  previous size was within `--solver-budget`; otherwise the cell reads `skipped (...)` with the reason.

## Output

Each sweep writes two files to `benchmark/decide/results/direct_solve/` (gitignored), named by the UTC start time. They
sit in their own folder, as the archived S1 profilers' do, because `view_results.py` opens the newest JSON in
`results/`.

- `direct_solve_<stamp>.csv`: one row per (query, scale factor) with the decision count, direct solve's objective,
  the agreement flag, each mode's runs, median, minimum and maximum seconds and peak MiB, each solver's model-build and
  solve seconds, and the speedup of direct solve over each solver (solver median divided by direct median).
- `direct_solve_<stamp>.json`: a manifest with the commit, whether `src/`, `third_party/` or
  `benchmark/decide/direct_solve/` has uncommitted changes and their diff, the binary and its build time, the DeciDB, Gurobi and
  HiGHS versions, the thread count, the CPU, core count, memory, platform and the arguments.

The script also prints the table as Markdown while it runs.

## Known limits

- Times include process start and database open, about 20 ms.
- Direct solve runs on DuckDB's default of 160 threads on the benchmark machine, Gurobi on its own default. `direct_1t`
  is there to show the difference.
- TPC-H group sizes are fixed across scale factors: 80 offers per supplier and 4 suppliers per part. Only the number
  of groups grows, never their size.
- Each query runs at one parameter setting: K = 2 per supplier and a tenth of all rows for S1, a target of 5,000 for A1,
  a cap of 5,000 per part and a tenth of the total cost for R1, and a limit of 1,000,000 for N1.
- HiGHS has no quadratic constraints, so its N1 cells read `error`.
- HiGHS (1.15.1, as 1.11) cannot solve A1 beyond 800 decisions. Its QP solver stops with "Large nullspace" once more than 4,000
  variables end strictly inside their bounds, and about half of A1's do (`../../pipeline/07_solver/done.md`, HiGHS
  backend). The cell reads `error` at 9,600 and the larger sizes are skipped. This is a limit of HiGHS's QP method,
  not of the model: the bounds reach HiGHS as column bounds, not rows.
- Gurobi and HiGHS solve to a gap of 0 (`../../pipeline/07_solver/done.md`, Optimality gap), so on S1 every path
  returns the same optimum; Gurobi's default gap of 1e-4 returned a different, slightly lower one on each run.

## Latest results

TODO: fill with the numbers, commit, machine and manifest of the rerun.
