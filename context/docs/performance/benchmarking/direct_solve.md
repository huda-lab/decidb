# Direct-solve benchmark

`benchmark/decide/direct_solve/run.py` times direct solve against the forced solver, Gurobi and HiGHS, on the same
DECIDE query as the data grows. Direct solve is the optimizer rewrite that answers some DECIDE problems with relational
operators instead of a solver ([`../../direct_solve/README.md`](../../direct_solve/README.md)). The numbers feed the
paper's direct-solve section. There is a query for each direct-solve rule that works today: A1, O1, A2 in four forms,
and S1, R1 and N1 in two forms each, many small groups and one bound over every row.

## Running

```bash
python3 benchmark/decide/direct_solve/run.py                             # every query, every size: an overnight run
python3 benchmark/decide/direct_solve/run.py --sf 0.125 0.25 --queries r1  # a subset
```

| Option | Default | Meaning |
|---|---|---|
| `--sf` | `0.125` doubling to `512` | TPC-H scale factors, run smallest first |
| `--queries` | every file in `queries/` | the file names in `queries/`, such as `s1`, `a2_keyed`, `o1` |
| `--modes` | `direct direct_1t gurobi` | must include `direct`, the baseline for every speedup and objective; `highs` is also available |
| `--repeats` | 3 (5 under `--compare`) | runs of every mode per cell, or of each side under `--compare` |
| `--repeat-budget` | 1800 | a run past this many seconds is its cell's last, so the `runs` column can read less than `--repeats` |
| `--timeout` | 21600 | seconds before a run is killed |
| `--solver-budget` | 7200 | a solver runs a size only if its median at the previous size was at most this |
| `--db-dir` | `benchmark/decide/databases/direct_solve/` | where the TPC-H databases are cached |
| `--compare` | off | run each query's direct plan beside its hand-written comparator instead of the sweep (§ Comparing with a hand-written plan); `--modes` is not used |

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
or subqueries. The time then measures the optimization and not other operators. O1 is the exception: its decisions are
the pairs of a cross product, so it reads `supplier` twice, and its decision count is the square of the supplier count
(10,000 × sf).

| Query | Rule | DECIDE part | What it asks |
|---|---|---|---|
| `s1.sql` | S1, uniform matroid | `pick(BOOL)`, `SUM(pick) by (ps_suppkey) <= 2`, `MAXIMIZE SUM(ps_availqty * pick)` | each supplier keeps its two offers with the most stock |
| `s1_global.sql` | S1, uniform matroid | `pick(BOOL)`, `SUM(pick) <= 0.1 * COUNT(*)`, `MAXIMIZE SUM(ps_availqty * pick)` | keep the tenth of all offers with the most stock |
| `a1.sql` | A1, separable box | `ship(REAL)`, `ship BETWEEN 0 AND ps_availqty`, `MINIMIZE SUM(POWER(ship - 5000, 2))` | ship each offer as close to 5,000 units as its stock allows |
| `r1.sql` | R1, continuous knapsack | `ship(REAL)`, `ship BETWEEN 0 AND ps_availqty AND SUM(ship) by (ps_partkey) <= 5000`, `MAXIMIZE SUM(ps_supplycost * ship)` | each part ships at most 5,000 units across its suppliers, at the highest total cost |
| `r1_global.sql` | R1, continuous knapsack | `ship(REAL)`, `ship BETWEEN 0 AND 1 AND SUM(ship * ps_supplycost) <= 0.1 * SUM(ps_supplycost)`, `MAXIMIZE SUM(ps_availqty * ship)` | take a fraction of each offer, spending at most a tenth of the total cost, to secure the most stock |
| `n1.sql` | N1, Euclidean ball | `ship(REAL)`, `SUM(POWER(ship, 2)) <= 1000000`, `MAXIMIZE SUM(ps_supplycost * ship)` | ship along each offer's cost, with one limit on the sum of squared amounts over every offer |
| `o1.sql` | O1, Monge assignment | `x(BOOL)` over `supplier l CROSS JOIN supplier r`, `per l: SUM(x) by (l) = 1 AND per r: SUM(x) by (r) = 1`, `MINIMIZE SUM(l.s_acctbal * r.s_acctbal * x)` | pair every supplier with one partner, richest with poorest, for the least total product of balances |
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

## Comparing with a hand-written plan

`run.py --compare` answers a different question from the sweep: how close is the plan a direct-solve rule builds to the
plan an expert writes by hand in SQL for the same answer? The sweep times direct solve against a solver. This mode times
it against the cheapest correct relational plan, so the remaining gap is the plan's shape.

```bash
python3 benchmark/decide/direct_solve/run.py --compare --sf 1 --queries a1 n1 r1
```

**The comparators.** `hand/<query>.sql` holds one comparator for each file in `queries/`, with a header comment that
says which obligations of the rule it carries. A comparator that skipped an obligation would be cheaper for a reason
that is not the plan, so each carries what its rule owes:

| Comparator | What it carries |
|---|---|
| `a1.sql` | the verdict first (an empty box raises infeasible, an answer that is not finite raises unbounded), no row released before every row has passed it, so two reads of the table; the target clamped into its box; `-0.0` turned into `0.0` |
| `a2*.sql` | the class value computed once from a first read and checked finite (the checks of the class's sums and of its answer); the value clamped into its box; `-0.0` turned into `0.0` |
| `n1.sql`, `n1_groups.sql` | each target and its square checked finite while `h` is summed; `h` checked finite; the scale `(value / sqrt(h)) * radius`; the warning for an `h` below the normal range on a row that has a value; the answer 0 for an `h` of 0; one ball per group, joined back by the key |
| `r1.sql`, `r1_global.sql` | the price ranking by one window; offers at the same price share what the budget leaves them in proportion to their room, so the answer is the same on every run (a `ROWS` frame would split a tie by the order the rows arrive); the box clamp; a budget read from the data comes from a first read |
| `o1.sql` | each side ranked by one window, the right side descending, and equal ranks selected; supplier rows are distinct and both sides are one table, so the rule's distinct classes, one-copy window and class-count check have nothing to do, and a product of two balances is always finite |
| `s1.sql`, `s1_global.sql` | the ranking by one window and the cut at the bound; for a bound read from the data, the check that it is not below the lower bound 0 (infeasible) |

A comparator is cheaper than the rule's plan only where the plan does work the comparator does not need. Where a check
has nothing to raise on the benchmark data (every box is finite and starts at 0), the comparator's header says so and
leaves it out. `test_direct_comparators.py` checks that every query has exactly one comparator and that each gives the
answer of its direct plan on a small TPC-H database, to 1e-9.

**The measure.** Both sides run in one `decidb` process under `PRAGMA enable_profiling='json'`, one statement per
repeat, best of `--repeats`. The first run of each side is not timed: it reads the table in. Each query runs once on all
threads and once on one thread.

- **Summed operator CPU** is the measure: the sum of every operator's time, whatever the thread count. It compares the
  work of the two plans and does not depend on how well either one parallelizes.
- **Latency** is reported beside it, for each side.
- **DECIDE planning** is reported apart. The `optimizer.direct.*` spans (`DECIDB_PROFILE`) are time the direct side
  spends planning, so they are in its latency and in no operator's time. Reporting them apart keeps the CPU ratio a
  comparison of plans and shows the planning cost on its own.
- **Answers.** The run on one thread, where a sum is deterministic, compares the two sides' `COUNT(*)`,
  `SUM(<decision>)` and objective. A difference is shown. A relative difference beyond 1e-9 stops the run with a
  non-zero exit.

The table has one row for each query and scale factor: the two CPU times and their ratio, the two latencies, the
planning, the ratio of the one-thread latency to the all-thread latency for each side (what each side gains from the
threads), and whether the answers are identical.

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
