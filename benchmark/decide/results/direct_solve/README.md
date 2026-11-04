# Direct-solve benchmark results for the paper

These are the results behind the paper's direct-solve figure:

- `direct_solve_20261102T124429Z.csv`: the results.
- `direct_solve_20261102T124429Z.json`: the sweep's manifest, with hardware, versions, arguments and query text.

**Question:** how much faster is direct solve (the optimizer rewrites a DECIDE query into a relational plan, and no
solver runs) than building a model and solving it with Gurobi, on the same queries as the problem grows?

**Code:** the script and queries are `benchmark/decide/direct_solve/` at commit `f7549bdc33`, byte-identical to what ran.
The JSON names `e05371fbde` with an uncommitted diff, because the sweep ran before they were committed. That commit
was later regrouped and was never pushed, so use `f7549bdc33`. No file under `src/` or `third_party/` differs between
the two.

## Data

- 60 rows: 5 queries × 12 sizes. Each row holds all modes for that query and size.
- Workload: TPC-H `partsupp`, one decision per row, so decisions = 800,000 × scale factor. Sizes double from 100K
  (sf 0.125) to 204.8M (sf 256).
- 204.8M is the largest size run and the paper's headline size. The JSON's arguments list sf 512 because the sweep was
  stopped by choice before reaching it. No run failed or timed out.

## Queries

All run over `partsupp`.

| Query | Class | Decision | Constraint | Objective |
|---|---|---|---|---|
| `a1` | Separable box | `ship` REAL | `ship BETWEEN 0 AND ps_availqty` | MIN `SUM(POWER(ship - 5000, 2))` |
| `s1` | Uniform matroid, per group | `pick` BOOL | `SUM(pick) by (ps_suppkey) <= 2` | MAX `SUM(ps_availqty * pick)` |
| `s1_global` | Uniform matroid, one bound | `pick` BOOL | `SUM(pick) <= 0.1 * COUNT(*)` | MAX `SUM(ps_availqty * pick)` |
| `r1` | Continuous knapsack, per group | `ship` REAL | `ship BETWEEN 0 AND ps_availqty AND SUM(ship) by (ps_partkey) <= 5000` | MAX `SUM(ps_supplycost * ship)` |
| `r1_global` | Continuous knapsack, one budget | `ship` REAL | `ship BETWEEN 0 AND 1 AND SUM(ship * ps_supplycost) <= 0.1 * SUM(ps_supplycost)` | MAX `SUM(ps_availqty * ship)` |

## Modes

- `direct`: `decide_direct_solve='require'`, 160 threads (the default). The query fails rather than falling back to a
  solver.
- `gurobi`: `decide_direct_solve='off'`, with `DECIDB_FORCE_SOLVER=gurobi`. DeciDB sets no solver thread count, so
  Gurobi uses its default, up to 32 threads according to its logs. MIP gap is 0, DeciDB's default.
- `direct_1t`: direct on 1 thread. The paper reports default performance only, so it does not use this mode.

## Measurement

- Each run is a fresh `decidb` process that opens the stored database, under `/usr/bin/time`.
- Each size's database is read once first, so no mode pays for the first disk read.
- The timed query returns `COUNT(*), SUM(decision), objective`, so times exclude transferring every row to the client.
- The time limit is 6 hours per run.
- Every cell has 3 runs.
- Wall time covers the whole process, including startup, which matters at the smallest sizes.

## Columns

- `query`, `sf`, `decisions`: identify the row.
- `objective`, `agree`: the objective reached. `agree = yes` means every run's objective matched direct's within 1e-6
  relative. It is yes on all 60 rows.
- `<mode> runs`, `median s`, `min s`, `max s`: wall time in seconds. Report the median.
- `<mode> peak MiB`: largest peak resident memory across the runs.
- `gurobi model s`, `gurobi solve s`: median model-build and solve time. They do not sum to wall time; the rest is the
  remainder of the query.
- `speedup vs gurobi`: Gurobi median / direct median, stored as text such as `78.0x`.

## Environment

- Intel Xeon Gold 6230 at 2.10 GHz: 4 sockets, 20 cores each, 80 physical cores, 160 logical CPUs, 4 NUMA nodes,
  376 GiB RAM.
- Linux 6.8.0.
- DeciDB v0.2.1-dev381. DuckDB memory limit 301.2 GiB.
- Gurobi 13.0.1, academic license. HiGHS was not run.
- The machine was otherwise near idle, apart from a ClickHouse server (about 25% of one core) and a security agent.

## Caveats

1. **Different thread counts:** direct uses 160 threads and Gurobi up to 32, because those are their defaults. A figure
   caption must say so.
2. **`r1` and `s1`, Gurobi at 204.8M:** the sweep ran these once because of its 30-minute repeat budget. Two more runs
   were added to the CSV by hand afterwards, with objectives checked against direct's. Wall time and memory cover 3
   runs, but model and solve times are from the first run only.
3. **`r1_global` at 204.8M:** the CSV shows 25×, but later reruns gave 18×, so write 18–25×. Direct's time varied between
   29 and 62 s at this size, and the cause is unknown. The raw plans and solver logs from those reruns are not in git.
4. **`s1_global` at 204.8M:** Gurobi ranges from 583 to 715 s, so write about 6–7×.
5. **`r1` at 100K:** the only cell where direct loses, at 0.9×.
6. **Scope:** direct solve applies only to queries the optimizer proves belong to an implemented class (3 of the 19
   catalogued). Everything else still goes to the solver, so these results do not apply to general DECIDE queries.

## Summary at 204.8M

About 7–277× faster than Gurobi, with 2.6–163× less memory.

| Query | Direct (s) | Gurobi (s) | Speedup |
|---|---|---|---|
| `a1` | 0.95 | 264 | 277× |
| `s1` | 32 | 3,233 | 101× |
| `r1` | 31 | 2,436 | 78× |
| `r1_global` | 30 | 758 | 18–25× |
| `s1_global` | 102 | 696 | about 6–7× |
