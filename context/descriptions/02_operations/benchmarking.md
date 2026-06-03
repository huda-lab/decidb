# Performance Benchmarking

This document describes the benchmarking infrastructure for measuring DeciDB DECIDE query performance.

## Overview

The benchmark suite measures **wall-clock time**, **peak memory (RSS, when the platform exposes it)**, and **per-stage breakdowns** across a fixed set of DECIDE queries at deterministic database sizes. It is designed to:

- Establish baselines before optimization work
- Validate that optimizations produce measurable improvements
- Identify bottlenecks across solver time, model construction, and optimizer rewrites
- Track regressions across commits

## Running Benchmarks

```bash
make decide-bench-setup                           # Generate databases (one-time)
make decide-bench                                  # Full run (all queries, all sizes, stage timers)
make decide-bench-manual                           # Run manual query
make decide-view                                   # View latest results

# Subset of queries or sizes
python3 benchmark/decide/run_benchmarks.py --queries Q1,Q5 --sizes medium
python3 benchmark/decide/run_benchmarks.py --queries Q1,Q5 --sizes medium,large

# More iterations for statistical confidence
python3 benchmark/decide/run_benchmarks.py --iterations 10

# Compare against previous commit (auto-detect from git log)
python3 benchmark/decide/run_benchmarks.py --compare

# Compare against specific commit
python3 benchmark/decide/run_benchmarks.py --compare abc1234

# View specific results
python3 benchmark/decide/view_results.py {hash}
python3 benchmark/decide/view_results.py dirty
python3 benchmark/decide/view_results.py manual
```

## Database Sizes

The default DECIDE benchmark uses two generated TPC-H databases. The generator creates the usual TPC-H tables at the configured scale factor, then replaces `lineitem` with a deterministic prefix ordered by `l_orderkey, l_linenumber`.

| Size | TPC-H SF | Exact `lineitem` rows | Purpose |
|------|----------|----------------------:|---------|
| medium | 0.085 | 500,000 | Moderate stress testing |
| large | 0.17 | 1,000,000 | Full benchmark run |

Generate with `make decide-bench-setup` (runs `generate_databases.py`). Databases are stored in `benchmark/decide/databases/` (gitignored). Existing `medium.db` and `large.db` files are reused only when their `lineitem` count matches the expected exact count; otherwise they are regenerated.

**Only `lineitem` is row-pinned.** Other TPC-H tables (`orders`, `customer`, `part`, etc.) still scale with the configured TPC-H scale factor. For the defaults above that means ≈127.5K orders on `medium` (SF=0.085) and ≈255K orders on `large` (SF=0.17). Queries that touch `orders` (Q6, Q7, Q8) either include their own `LIMIT` subquery or rely on size-specific coefficients that scale with the orders cardinality.

## Stage Timers

When `DECIDB_BENCH=1` is set (automatically by `make decide-bench`), the DECIDE pipeline emits per-stage timing to stderr:

```
DECIDB_BENCH: optimizer_ms=0.01         # DecideOptimizer rewrite passes
DECIDB_BENCH: model_construction_ms=32  # ILP/QP model building
DECIDB_BENCH: solver_ms=1448            # SolveModel() call (Gurobi or HiGHS)
DECIDB_BENCH: total_variables=9965      # num_rows * num_decide_vars plus auxiliaries
DECIDB_BENCH: total_constraints=5       # per-row + global constraints
DECIDB_BENCH: num_rows=9965
```

The Python runner parses these lines and includes them in the result JSON. It wraps DeciDB with `/usr/bin/time`; on macOS it uses `time -l` when available for RSS, and falls back to `time -p` when sandbox restrictions prevent resource collection.

**Source locations:**
- `src/execution/operator/decide/physical_decide.cpp` - model_construction_ms, solver_ms, total_variables, total_constraints, num_rows
- `src/optimizer/decide/decide_optimizer.cpp` - optimizer_ms

Timers use DuckDB's `Profiler` class (`src/include/duckdb/common/profiler.hpp`). They are gated behind `std::getenv("DECIDB_BENCH")`.

## Benchmark Query Set

Nine queries run at all default database sizes. Query files live in `benchmark/decide/queries/*.sql` and use `${NAME}` placeholders for size-specific coefficients. The runner resolves placeholders before execution, fails fast on unresolved `${...}` tokens, and stores both the resolved SQL and coefficient map in each result entry.

| Query | File | Features Exercised |
|-------|------|--------------------|
| Q1 | `q1_linear_knapsack.sql` | Boolean knapsack, SUM constraint, linear objective |
| Q2 | `q2_abs_minmax.sql` | REAL decision variable, ABS linearization, hard MAX condition |
| Q3 | `q3_avg_when.sql` | AVG rewrite, WHEN-filtered aggregate, continuous decision variable |
| Q4 | `q4_nested_per_objective.sql` | PER grouping, nested aggregate objective, MINIMIZE MAX |
| Q5 | `q5_linear_stress.sql` | Boolean multi-constraint knapsack, larger linear stress case |
| Q6 | `q6_domain_ne_between.sql` | INTEGER domain, IN, BETWEEN, not-equal aggregate |
| Q7 | `q7_entity_scope_join.sql` | Join input, entity-scoped DECIDE variable, PER by order priority |
| Q8 | `q8_boolean_bilinear.sql` | Two Boolean decision variables, bilinear terms over an ordered orders prefix |
| Q9 | `q9_quadratic_gurobi.sql` | REAL variable, quadratic norm constraint, quadratic objective |

### Coefficients

| Placeholder | Medium | Large |
|---|---:|---:|
| `Q1_QTY_CAP` | 833 | 1667 |
| `Q2_ABS_CAP` | 333 | 667 |
| `Q3_R_QTY_CAP` | 333 | 667 |
| `Q5_QTY_CAP` | 8333 | 16667 |
| `Q5_COUNT_CAP` | 833 | 1667 |
| `Q5_DISCOUNT_CAP` | 83333 | 166667 |
| `Q6_PRICE_CAP` | 90691505 | 182337938 |
| `Q6_NE_SUM` | 255 | 510 |
| `Q7_QTY_CAP` | 5000 | 10000 |
| `Q7_PRIORITY_CAP` | 100 | 200 |
| `Q8_CHOOSE_CAP` | 128 | 255 |
| `Q8_EXPEDITE_CAP` | 64 | 128 |
| `Q8_PAIR_PRICE_CAP` | 13603726 | 27350691 |
| `Q8_ROW_LIMIT` | 2048 | 4096 |
| `Q9_SSE_CAP` | 752545 | 1504661 |

### Coverage Matrix

| Feature | Q1 | Q2 | Q3 | Q4 | Q5 | Q6 | Q7 | Q8 | Q9 |
|---------|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|
| IS BOOLEAN | x | | | x | x | | x | x | |
| IS INTEGER | | | | | | x | | | |
| IS REAL | | x | | | | | | | x |
| ABS linearization | | x | | | | | | | |
| MIN/MAX | | x | | x | | | | | |
| AVG rewrite | | | x | | | | | | |
| WHEN filtering | | | x | | | | | | |
| PER grouping | | | | x | | | x | | |
| Join input | | | | | | | x | | |
| Domain constraints | | | | | | x | | | |
| Not-equal aggregate | | | | | | x | | | |
| Bilinear terms | | | | | | | | x | |
| Quadratic terms | | | | | | | | | x |
| Multiple constraints | | x | x | x | x | x | x | x | x |

## Output

### Visual Output

After each run, `view_results.py` displays colored stage-proportion bars:

```
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
Q1: linear_knapsack
  SELECT l_orderkey, l_linenumber, l_quantity, l_extendedprice, x
  FROM lineitem
  DECIDE x IS BOOLEAN
  SUCH THAT SUM(x * l_quantity) <= 833
  MAXIMIZE SUM(x * l_extendedprice);

  medium  │ 500K rows │ 500K vars │ 3 constraints │ 0.45s
  ██░░░░░░░░░░░░████████████████████████████████████░░░░░░
   opt       model              solver              other
```

Stage colors: optimizer (blue), model construction (yellow), solver (red), overhead (grey). Bar width is 60 characters, proportional to time share.

The viewer can be run standalone:

```bash
python3 benchmark/decide/view_results.py           # latest result
python3 benchmark/decide/view_results.py {hash}    # specific commit
python3 benchmark/decide/view_results.py dirty     # dirty result
python3 benchmark/decide/view_results.py manual    # manual result
```

### JSON Output

Saved to `benchmark/decide/results/{commit}.json` (or `dirty.json` for uncommitted changes, `manual.json` for manual queries).

```json
{
  "commit": "6b56b35",
  "timestamp": "...",
  "system": {...},
  "iterations": 1,
  "sizes": ["medium", "large"],
  "queries": [
    {
      "query": "Q1",
      "description": "linear_knapsack",
      "size": "medium",
      "sql": "SELECT l_orderkey ... SUCH THAT SUM(x * l_quantity) <= 833 ...",
      "coefficients": {
        "Q1_QTY_CAP": 833,
        "Q2_ABS_CAP": 333
      },
      "runs": [...],
      "stats": {
        "median_wall_time_s": 0.45,
        "stddev_wall_time_s": 0.0,
        "median_peak_rss_kb": 32000,
        "stages": {
          "optimizer_ms": 0.01,
          "model_construction_ms": 5.2,
          "solver_ms": 420.0,
          "total_variables": 500000,
          "total_constraints": 3,
          "num_rows": 500000
        }
      }
    }
  ]
}
```

Running on the same commit overwrites the previous result file. Dirty worktrees write to `dirty.json`.

### Manual Queries

For ad-hoc benchmarking:

1. Copy `queries/manual.sql.example` to `queries/manual.sql`
2. Edit the query
3. Run `make decide-bench-manual`

Manual mode runs against the selected default sizes and saves to `results/manual.json` (always overwritten). It does not apply the standard query coefficient substitution.

### Comparison

Use `--compare` to see deltas between the current run and a previous one:

```bash
# Auto-detect: walks git log to find the most recent commit with results
python3 benchmark/decide/run_benchmarks.py --compare

# Explicit: compare against a specific commit hash
python3 benchmark/decide/run_benchmarks.py --compare abc1234
```

### Recording Results

Whenever a commit is made for the purpose of improving performance, a corresponding entry MUST be added to `context/descriptions/06_performance/`. One file per optimization batch, named `{NNN}_{baseline_commit}_{evaluated_commit}.md` where `NNN` is a zero-padded sequential log number (e.g., `002_9c3a53fb62_6bc8ae1412.md`), recording the change set, hypothesis, and measured outcome (with references to the commit hash and the benchmark JSON files used for the comparison). The `06_performance/README.md` index lists all entries. The log is append-only — superseded entries get a new entry referencing the old one rather than being rewritten.

## Makefile Targets

| Target | Description |
|--------|-------------|
| `make decide-bench-setup` | Generate deterministic TPC-H databases (`medium`, `large`) |
| `make decide-bench` | Run full benchmark suite with stage timers |
| `make decide-bench-manual` | Run manual query benchmark |
| `make decide-view` | View latest benchmark results |

## File Layout

```
benchmark/decide/
├── generate_databases.py      # Database generation script
├── run_benchmarks.py          # Orchestration script
├── view_results.py            # Visual results viewer
├── queries/
│   ├── q1_linear_knapsack.sql
│   ├── q2_abs_minmax.sql
│   ├── q3_avg_when.sql
│   ├── q4_nested_per_objective.sql
│   ├── q5_linear_stress.sql
│   ├── q6_domain_ne_between.sql
│   ├── q7_entity_scope_join.sql
│   ├── q8_boolean_bilinear.sql
│   ├── q9_quadratic_gurobi.sql
│   └── manual.sql.example
├── databases/                 # Generated TPC-H DBs (gitignored)
│   ├── medium.db              # SF=0.085, exactly 500K lineitem rows
│   └── large.db               # SF=0.17, exactly 1M lineitem rows
├── results/                   # JSON outputs (gitignored)
└── .gitignore
```
