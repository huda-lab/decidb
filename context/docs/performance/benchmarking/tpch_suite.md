# TPC-H suite

`benchmark/decide/run_benchmarks.py` runs a fixed set of DECIDE queries on two generated TPC-H databases and records
wall time, peak memory and per-stage timers for each. Use it to set a baseline before solver-path work and to catch
regressions between commits.

The suite measures the solver path only. Every run sets `decide_direct_solve='off'`, so a query that direct solve
could answer (P4 has that shape) still goes through model construction and the solver. Direct solve has its own
benchmark: [direct_solve.md](direct_solve.md).

## Running

```bash
make decide-bench-setup      # generate medium.db and large.db (once)
make decide-bench            # every query, both sizes, 3 iterations, stage timers on (DECIDB_BENCH=1)
make decide-bench-manual     # queries/manual.sql instead of the suite
make decide-view             # show the newest result file

python3 benchmark/decide/run_benchmarks.py --queries Q1,Q5 --sizes medium    # a subset
python3 benchmark/decide/run_benchmarks.py --iterations 10                   # more repeats
python3 benchmark/decide/run_benchmarks.py --compare                         # against the newest earlier commit's result
python3 benchmark/decide/run_benchmarks.py --compare abc1234                 # against one commit's result
python3 benchmark/decide/run_benchmarks.py --timeout 900                     # seconds per run (default 600)
```

Run the script directly without `DECIDB_BENCH=1` and the stage timers are off; only wall time and memory are recorded.

## Databases

`generate_databases.py` builds two databases in `benchmark/decide/databases/` with DuckDB's `CALL dbgen(sf=...)`, then
replaces `lineitem` with a deterministic prefix ordered by `l_orderkey, l_linenumber`.

| Size | TPC-H SF | Exact `lineitem` rows |
|------|----------|----------------------:|
| medium | 0.085 | 500,000 |
| large | 0.17 | 1,000,000 |

An existing database is reused when its `lineitem` count matches; otherwise it is regenerated.

Only `lineitem` is row-pinned. The other tables scale with the scale factor: about 127.5K orders on `medium` and 255K
on `large`. Queries that touch `orders` (Q2, Q6, Q7, Q8) either carry their own `LIMIT` subquery or use coefficients
sized to the orders count.

## How a run is measured

Each iteration starts a fresh `decidb` process on the database, opened `-readonly`, with the query on stdin, under
`/usr/bin/time`. On macOS the runner uses `time -l` for peak RSS and falls back to `time -p` (wall time only) when a
sandbox blocks resource collection; on Linux it uses `time -v`. The wall time therefore includes process start,
database open and printing the result. A run past `--timeout` is recorded with exit code -1 and the message
`Timeout after Ns`.

Per query and size the runner keeps every run and reports the median wall time, its standard deviation, the median
peak RSS and the median of each stage timer, over the successful runs.

## Stage timers

With `DECIDB_BENCH=1` the engine prints one line per figure to stderr, and the runner parses them into the result:

```
DECIDB_BENCH: optimizer_ms=0.01         # the whole DecideOptimizer stage, on either path
DECIDB_BENCH: model_construction_ms=32  # model building, up to SolverModel::Build
DECIDB_BENCH: solver_ms=1448            # the SolveModel() call, which includes the neutral model build
DECIDB_BENCH: total_variables=9965      # decision variables plus auxiliaries
DECIDB_BENCH: total_constraint_specs=5  # DECIDE clauses handed to the builder
DECIDB_BENCH: total_constraint_rows=19934  # matrix rows the solver received
DECIDB_BENCH: num_rows=9965
```

The two constraint counts are different units; never add them. A spec is one DECIDE clause as the operator hands it to
the builder. A per-row spec expands to one matrix row per data row, and a `per K:` or `by` spec to one row per class
or group, so the row count can be far larger. Read `total_constraint_rows` for matrix pressure and to compare queries; read
`total_constraint_specs` to trace that pressure to a clause. Q4 on `decidb.db` reports 60,182 specs against 120,360
rows: the `per (): max(keep * l_quantity) <= 45` easy-direction rewrite adds a row per data row while counting as one
spec.

Older result files carry a single `total_constraints` that summed the two units. `view_results.py` shows it flagged
`pre-split, mixed unit`; do not compare it with a `total_constraint_rows` figure.

Where each line is printed:

- `src/optimizer/decide/decide_optimizer.cpp`: `optimizer_ms`.
- `src/execution/operator/decide/physical_decide.cpp`: the other six. `total_constraint_rows` is read from
  `SolverResult::model_constraint_rows`, which `src/decidb/solver/ilp_solver.cpp` sets from the built model, since
  only the built model knows the expanded row count.

The timers use DuckDB's `Profiler` class and are gated on `std::getenv("DECIDB_BENCH")`. For a finer, nested
breakdown use the [../../pipeline profiler](pipeline_profiler.md).

## Query set

The runner discovers `queries/q*.sql`: eleven queries, Q1 to Q11, each run at both sizes. (`queries/p*.sql` are run only
by the pipeline profiler.) The set covers the DECIDE expressivity surface (problem classes, variable types, constraint
and objective forms, SQL functions, the `when` / `per` / `by` scope words) with as few queries as possible. Each query
bundles several features and lists them in a `-- TAGS:` header comment.

Query files use `${NAME}` placeholders for size-specific coefficients. The runner fills them from the `COEFFICIENTS`
table in `run_benchmarks.py`, stops on any placeholder left unresolved, and stores the resolved SQL and the
coefficients used in each result entry.

**Scale tiers.** Each query runs at the largest size that completes in reasonable time, set by measurement
(2026-07-26). Three things bound a query's size:

1. **Table cardinality.** Only `lineitem` is pinned to 500K/1M. Queries over `orders` (Q2, Q6, Q7) top out at
   127.5K/255K and the query over `partsupp` (Q11) at 68K/136K. These run their full table.
2. **Solver cost that grows faster than the rows.** Q3 (L0 covering MILP) and Q5 (QCQP barrier) stay row-limited
   through `${..._ROW_LIMIT}`. Q9 is near-linear because its MIN/MAX auxiliary has a derived box, but its
   limit still holds the earlier value until the tier is re-chosen.
3. **Nothing.** Q1, Q4, Q8 and Q10 run the full 500K/1M `lineitem`.

Measured ceilings (largest size completing under 60 s):

| Query | Medium / Large | Wall (med / large) | Bound by |
|---|---|---|---|
| Q1, Q4, Q8 | 500K / 1M | — | full `lineitem` |
| Q10 | 500K / 1M | 30.3s / 73.3s | full `lineitem` (heaviest in the suite) |
| Q2 | 127.5K / 255K | — | full `orders` |
| Q6 | 127.5K / 255K | 15.2s / 39.9s | full `orders` |
| Q7 | 127.5K / 255K | 3.3s / 7.1s | full `orders` |
| Q11 | 68K / 136K | — | full `partsupp` |
| Q3 | 60K / 120K | 13.3s / 26.4s | L0 MILP; 250K exceeds 60s |
| Q5 | 50K / 100K | 3.7s / 9.2s | QCQP barrier; 125K exceeds 60s |
| Q9 | 7.5K / 15K | 0.2s / 0.3s | limit not yet re-tiered; 120K solves in 2.8s |

A full run (3 iterations, 2 sizes) takes about 17 minutes at these settings, mostly Q10, Q6 and Q4. While iterating
on one change, use `--sizes medium --queries <subset>`.

**Raising a limit requires re-measuring.** A superlinear curve is evidence to explain, not a property to design around.
Q7 runs the full `orders` table because its non-convex structure is independent per row: 127.5K rows solve in 3.3 s.
Q9's steep curve (5K 1.7 s, 30K over 60 s) came from a free MIN/MAX auxiliary that starved the root simplex of a box;
boxed, it measures 15K 0.24 s, 30K 0.45 s, 60K 0.95 s, 120K 2.8 s (2026-08-18, solver time). The `-- NOTE:` headers
in each query file record its measured curve.

| Query | File | Features Exercised |
|-------|------|--------------------|
| Q1 | `q1_ilp_selection.sql` | Boolean ILP selection; MAXIMIZE SUM with a `when` in front; AVG constraint; `when` in front of a constraint and inside a reducer; `per ()`; multi-column `per` key with its `by` |
| Q2 | `q2_integer_domains.sql` | `INT` vars; BETWEEN, IN(var), `<>` per-row + aggregate, division, unary-minus, strict `>`; uncorrelated + correlated subqueries; data-only aggregate RHS; `%` fold |
| Q3 | `q3_abs_norms.sql` | REAL signed domain; MINIMIZE; ABS lower-envelope; norm L1 / L∞ / L0 (row-limited: L0 is one binary per row) |
| Q4 | `q4_minmax_nested.sql` | MINIMIZE MAX (easy) as the two-level objective `max(per l_returnflag: sum(...) by (l_returnflag))`; composed MIN/MAX-in-LHS; easy MAX constraint (full scale) |
| Q5 | `q5_qp_qcqp.sql` | Convex QP + QCQP; quadratic + mixed linear/quadratic objective; norm L2; aggregate quadratic constraint (row-limited, Gurobi) |
| Q6 | `q6_bilinear_mccormick.sql` | MILP; BOOL×REAL bilinear objective + constraint (McCormick); `when` + `per` + `by` on one constraint (full `orders` scale) |
| Q7 | `q7_nonconvex.sql` | MIQP + non-convex QP + non-convex bilinear; per-row quadratic constraint (full `orders` scale, Gurobi `NonConvex=2`) |
| Q8 | `q8_feasibility_join.sql` | Feasibility (no objective); keyed decision `per o: assign(BOOL)` over a join; multi-column `per` key; `when` + `per`; IS NOT NULL in a `when` |
| Q9 | `q9_maximize_max.sql` | MAXIMIZE MAX (hard, Big-M); equality constraint (row-limited; the tier is not yet re-chosen) |
| Q10 | `q10_maximize_abs.sql` | MAXIMIZE SUM(ABS) + ABS Path-B `>=` constraint (Big-M; full scale; sign binaries independent per row, 5M vars at 1M rows; heaviest query in the suite) |
| Q11 | `q11_lp_allocation.sql` | Pure LP continuous allocation; MAXIMIZE AVG |

### Coefficients

`${..._ROW_LIMIT}` placeholders set the row prefix for row-limited queries; the remaining placeholders are constraint bounds sized to keep each query feasible and non-trivial at each scale.

| Placeholder | Medium | Large |
|---|---:|---:|
| `Q1_QTY_CAP` | 3829286 | 7660945 |
| `Q1_R_QTY_CAP` | 1260000 | 2520000 |
| `Q1_GRP_CAP` | 62500 | 125000 |
| `Q1_LOCAL_CAP` | 5420000000 | 10900000000 |
| `Q2_NE_SUM` | 127500 | 255000 |
| `Q2_PRICE_CAP` | 9070000000 | 18200000000 |
| `Q2_MOD_CAP` | 1147500 | 2295000 |
| `Q3_ROW_LIMIT` | 60000 | 120000 |
| `Q3_ABS_CAP` | 450000 | 900000 |
| `Q4_QTY_CAP` | 2500000 | 5000000 |
| `Q5_ROW_LIMIT` | 50000 | 100000 |
| `Q5_SSE_CAP` | 38700000 | 77400000 |
| `Q6_ROW_LIMIT` | 127500 | 255000 |
| `Q6_PICK_CAP` | 63750 | 127500 |
| `Q6_BILIN_CAP` | 3187500 | 6375000 |
| `Q6_GRP_CAP` | 1245117 | 2490234 |
| `Q7_ROW_LIMIT` | 127500 | 255000 |
| `Q7_BILIN_CAP` | 5100000 | 10200000 |
| `Q7_PICK_CAP` | 63750 | 127500 |
| `Q9_ROW_LIMIT` | 7500 | 15000 |
| `Q10_ROW_LIMIT` | 500000 | 1000000 |
| `Q10_SUM_CAP` | 7500000 | 15000000 |
| `Q10_ABS_FLOOR` | 1250000 | 2500000 |
| `Q11_BUDGET` | 51000000000 | 102000000000 |
| `P3_BUDGET` (pipeline profiler only) | 5000000 | 10000000 |

Q8 carries no placeholders (its feasibility bounds are fixed literals); Q4's composed-constraint cap and Q9's cardinality/price bounds are likewise inline literals held constant across sizes.

### Coverage matrix

Cells mark which query exercises each feature. Queries pack multiple features, so most columns are dense.

**Variable types**

| Feature | Q1 | Q2 | Q3 | Q4 | Q5 | Q6 | Q7 | Q8 | Q9 | Q10 | Q11 |
|---|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:---:|:---:|
| BOOL | x | | | x | | x | x | x | x | | |
| INT (explicit) | | x | | | | | | | | | |
| REAL | | | x | | x | x | x | | | x | x |
| Multiple variables | | x | | | | x | x | | | | |
| Keyed (`per K: x`) | | | | | | | | x | | | |
| Signed / negative domain | | | x | | | | | | | | |

**Problem classes**

| Feature | Q1 | Q2 | Q3 | Q4 | Q5 | Q6 | Q7 | Q8 | Q9 | Q10 | Q11 |
|---|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:---:|:---:|
| LP | | | | | | | | | | | x |
| ILP | x | x | | x | | | | | x | | |
| MILP | | | x | | | x | | | | x | |
| QP (convex) | | | | | x | | | | | | |
| QP (non-convex) | | | | | | | x | | | | |
| MIQP | | | | | | | x | | | | |
| QCQP | | | | | x | | x | | | | |
| Bilinear (McCormick) | | | | | | x | | | | | |
| Bilinear (non-convex) | | | | | | | x | | | | |
| Feasibility (no objective) | | | | | | | | x | | | |

**Constraint forms**

| Feature | Q1 | Q2 | Q3 | Q4 | Q5 | Q6 | Q7 | Q8 | Q9 | Q10 | Q11 |
|---|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:---:|:---:|
| Equality `=` | | | | | | | | | x | | |
| Strict `>` / `<` | | x | | | | | | | | | |
| `<>` per-row | | x | | | | | | | | | |
| `<>` aggregate | | x | | | | | | | | | |
| BETWEEN | | x | x | | | | | | | x | |
| IN (decision var) | | x | | | | | | | | | |
| Per-row linear LHS (`/`, unary `-`) | | x | | | | | | | | | |
| Uncorrelated subquery | | x | | | | | | | | | |
| Correlated subquery | | x | | | | | | | | | |
| Data-only aggregate RHS | | x | | | | | | | | | |
| Data-only operator fold (`%`) | | x | | | | | | | | | |
| Composed MIN/MAX in LHS | | | | x | | | | | | | |
| Quadratic constraint (aggregate) | | | | | x | | | | | | |
| Quadratic constraint (per-row) | | | | | | | x | | | | |
| Bilinear constraint | | | | | | x | x | | | | |

**Objectives**

| Feature | Q1 | Q2 | Q3 | Q4 | Q5 | Q6 | Q7 | Q8 | Q9 | Q10 | Q11 |
|---|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:---:|:---:|
| MAXIMIZE SUM | x | x | | | | x | | | | | |
| MINIMIZE (SUM / quadratic) | | | x | | x | | | | | | |
| MINIMIZE MAX (easy) | | | | x | | | | | | | |
| MAXIMIZE MAX (hard, Big-M) | | | | | | | | | x | | |
| MAXIMIZE SUM(ABS) | | | | | | | | | | x | |
| Two-level objective | | | | x | | | | | | | |
| Quadratic objective | | | | | x | | x | | | | |
| Bilinear objective | | | | | | x | | | | | |
| Mixed linear + quadratic | | | | | x | | | | | | |
| `when` on objective | x | | | | | | | | | | |
| AVG objective | | | | | | | | | | | x |

**SQL functions**

| Feature | Q1 | Q2 | Q3 | Q4 | Q5 | Q6 | Q7 | Q8 | Q9 | Q10 | Q11 |
|---|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:---:|:---:|
| AVG | x | | | | | | | | | | x |
| MIN / MAX | | | | x | | | | | x | | |
| ABS (lower-envelope) | | | x | | | | | | | | |
| ABS (Big-M) | | | | | | | | | | x | |
| POWER (quadratic) | | | | | x | | x | | | | |
| norm L1 | | | x | | | | | | | | |
| norm L2 | | | | | x | | | | | | |
| norm L∞ | | | x | | | | | | | | |
| norm L0 | | | x | | | | | | | | |
| Bilinear (var × var) | | | | | | x | x | | | | |
| Division | | x | | | | | | | | | |

**when / per / by**

| Feature | Q1 | Q2 | Q3 | Q4 | Q5 | Q6 | Q7 | Q8 | Q9 | Q10 | Q11 |
|---|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:---:|:---:|
| `when` on constraint | x | | | | | x | | x | | | |
| `when` on objective | x | | | | | | | | | | |
| `when` inside a reducer | x | | | | | | | | | | |
| `when` + `per` on one constraint | x | | | | | x | | x | | | |
| `per ()` on constraint | x | | | x | | x | | | | | |
| `per` single-column | | | | x | | x | | x | | | |
| `per` multi-column | x | | | | | | | x | | | |
| `by (G)` after a reducer | x | | | x | | x | | x | | | |
| Two-level objective `outer(per k: inner(e) by (k))` | | | | x | | | | | | | |
| IS NULL / IS NOT NULL in a `when` | | | | | | | | x | | | |

**Deliberate omissions.** Two objective forms are left uncovered to keep the count at 11: `MAXIMIZE MIN` (easy), which is mechanically identical to Q4's `MINIMIZE MAX`, and `MINIMIZE MIN` (hard), the symmetric twin of Q9's `MAXIMIZE MAX`. Adding either costs a whole query (one objective per query) for a construct whose linearization is already exercised.

## Output

### Result file

Each run writes `benchmark/decide/results/{commit}.json`, or `dirty.json` when the working tree has uncommitted
changes, or `manual.json` in manual mode. A second run on the same commit overwrites the file.

```json
{
  "commit": "6b56b35",
  "timestamp": "...",
  "system": {"platform": "...", "cpu": "...", "python": "..."},
  "iterations": 3,
  "sizes": ["medium", "large"],
  "queries": [
    {
      "query": "Q1",
      "description": "ilp_selection",
      "size": "medium",
      "sql": "SELECT l_orderkey ... SUCH THAT per (): sum(keep * l_quantity) <= 3829286 ...",
      "coefficients": {"Q1_QTY_CAP": 3829286, "Q1_GRP_CAP": 62500},
      "runs": [...],
      "stats": {
        "total_runs": 3,
        "successful_runs": 3,
        "median_wall_time_s": 0.45,
        "stddev_wall_time_s": 0.0,
        "median_peak_rss_kb": 32000,
        "stages": {"optimizer_ms": 0.01, "model_construction_ms": 5.2, "solver_ms": 420.0, "...": "..."}
      }
    }
  ]
}
```

`system` also holds `ram_gb` on Linux, read from `/proc/meminfo`.

### Viewer

After each run the runner calls `view_results.py`, which prints each query's SQL, a metrics line (rows, variables,
constraint rows and specs, peak memory, median time) and a 60-character bar of the time split into optimizer (blue),
model construction (yellow), solver (red) and the rest (grey):

```
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
Q1: ilp_selection
  SELECT l_orderkey, l_linenumber, ..., keep
  ...
  medium  │ 500K rows │ 500K vars │ ... │ 1.73s
  ██░░░░░░░░░░░░████████████████████████████████████░░░░░░
   opt       model              solver              other
```

```bash
python3 benchmark/decide/view_results.py           # newest .json in results/
python3 benchmark/decide/view_results.py {hash}    # one commit
python3 benchmark/decide/view_results.py dirty
python3 benchmark/decide/view_results.py manual
```

With no argument the viewer takes the newest `.json` in `results/` by modification time. The archived S1 profilers
also write JSON there, so after one of them runs, name the result file. The direct-solve benchmark writes to
`results/direct_solve/` and does not interfere.

### Comparison

`--compare` prints, per query and size, the previous and current median, the change in seconds and percent, and the
change in peak memory. With no argument it walks the last 50 commits of `git log` and takes the newest one that has a
result file; with a hash it reads `results/{hash}.json`.

### Manual queries

1. Copy `queries/manual.sql.example` to `queries/manual.sql` (gitignored).
2. Edit the query.
3. Run `make decide-bench-manual`.

Manual mode runs on the selected sizes and writes `results/manual.json`, overwriting it. It does not fill `${...}`
placeholders, so write the constants into the query.

## Files

```
benchmark/decide/
├── generate_databases.py      # builds databases/medium.db and large.db
├── run_benchmarks.py          # the runner; also holds the COEFFICIENTS table
├── view_results.py            # the viewer
├── queries/
│   ├── q1_ilp_selection.sql ... q11_lp_allocation.sql
│   ├── p1_lp_row_bound.sql ... p4_per_high_cardinality.sql   # pipeline profiler only
│   └── manual.sql.example
├── databases/                 # gitignored
└── results/                   # gitignored
```
