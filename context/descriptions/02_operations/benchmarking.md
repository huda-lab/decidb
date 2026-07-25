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

Eleven queries run at all default database sizes. The set is designed to cover the DECIQL expressivity surface (problem classes, variable types, constraint/objective forms, SQL functions, WHEN/PER) with the **fewest queries** — each query deliberately bundles several features, recorded in a `-- TAGS:` header comment in its `.sql` file. Query files live in `benchmark/decide/queries/*.sql` and use `${NAME}` placeholders for size-specific coefficients; the runner resolves them before execution, fails fast on unresolved `${...}` tokens, and stores both the resolved SQL and coefficient map in each result entry.

**Scale tiers.** The linear / LP / feasibility queries (Q1, Q2, Q4, Q8, Q11) run at full scale (500K–1M rows). The MILP-with-per-row-binaries and QP/QCQP queries are **row-limited** via `${..._ROW_LIMIT}` because their auxiliary variables — Big-M indicators, L0 count indicators, McCormick/QCQP terms — grow with the row count and do not scale to 1M. Q3/Q5/Q10 are moderate (20K–100K); Q6 smaller; Q9 smallest of the MILPs (the hard-MAX indicators couple globally, so it is the least scalable); Q7 (non-convex, Gurobi `NonConvex=2`) is smallest.

| Query | File | Features Exercised |
|-------|------|--------------------|
| Q1 | `q1_ilp_selection.sql` | Boolean ILP selection; MAXIMIZE SUM + WHEN-on-objective; AVG constraint; WHEN + aggregate-local WHEN; multi-column PER |
| Q2 | `q2_integer_domains.sql` | INTEGER + default-type vars; BETWEEN, IN(var), `<>` per-row + aggregate, division, unary-minus, strict `>`; uncorrelated + correlated subqueries; data-only aggregate RHS; `%` fold |
| Q3 | `q3_abs_norms.sql` | REAL signed domain; MINIMIZE; ABS lower-envelope; norm L1 / L∞ / L0 (moderate — L0 is one binary per row) |
| Q4 | `q4_minmax_nested.sql` | MINIMIZE MAX (easy) + nested-PER objective; composed MIN/MAX-in-LHS; easy MAX constraint (full scale — heaviest large query) |
| Q5 | `q5_qp_qcqp.sql` | Convex QP + QCQP; quadratic + mixed linear/quadratic objective; norm L2; aggregate quadratic constraint (moderate, Gurobi) |
| Q6 | `q6_bilinear_mccormick.sql` | MILP; BOOL×REAL bilinear objective + constraint (McCormick); WHEN+PER (row-limited) |
| Q7 | `q7_nonconvex.sql` | MIQP + non-convex QP + non-convex bilinear; per-row quadratic constraint (small, Gurobi `NonConvex=2`) |
| Q8 | `q8_feasibility_join.sql` | Feasibility (no objective); entity-scoped variable over a join; multi-column PER; WHEN+PER; IS NOT NULL |
| Q9 | `q9_maximize_max.sql` | MAXIMIZE MAX (hard, Big-M); equality constraint (small — hard-MAX indicators couple globally) |
| Q10 | `q10_maximize_abs.sql` | MAXIMIZE SUM(ABS) + ABS Path-B `>=` constraint (Big-M; row-limited, but per-row-independent so scales further than Q9) |
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
| `Q3_ROW_LIMIT` | 20000 | 40000 |
| `Q3_ABS_CAP` | 150000 | 300000 |
| `Q4_QTY_CAP` | 2500000 | 5000000 |
| `Q5_ROW_LIMIT` | 50000 | 100000 |
| `Q5_SSE_CAP` | 38700000 | 77400000 |
| `Q6_ROW_LIMIT` | 8192 | 16384 |
| `Q6_PICK_CAP` | 4096 | 8192 |
| `Q6_BILIN_CAP` | 204800 | 409600 |
| `Q6_GRP_CAP` | 80000 | 160000 |
| `Q7_ROW_LIMIT` | 1024 | 2048 |
| `Q7_BILIN_CAP` | 40960 | 81920 |
| `Q7_PICK_CAP` | 512 | 1024 |
| `Q9_ROW_LIMIT` | 5000 | 10000 |
| `Q10_ROW_LIMIT` | 50000 | 100000 |
| `Q10_SUM_CAP` | 750000 | 1500000 |
| `Q10_ABS_FLOOR` | 125000 | 250000 |
| `Q11_BUDGET` | 51000000000 | 102000000000 |

Q8 carries no placeholders (its feasibility bounds are fixed literals); Q4's composed-constraint cap and Q9's cardinality/price bounds are likewise inline literals held constant across sizes.

### Coverage Matrix

Cells mark which query exercises each feature. Queries pack multiple features, so most columns are dense.

**Variable types**

| Feature | Q1 | Q2 | Q3 | Q4 | Q5 | Q6 | Q7 | Q8 | Q9 | Q10 | Q11 |
|---|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:---:|:---:|
| IS BOOLEAN | x | | | x | | x | x | x | x | | |
| IS INTEGER (explicit) | | x | | | | | | | | | |
| IS REAL | | | x | | x | x | x | | | x | x |
| Default type (bare `DECIDE x`) | | x | | | | | | | | | |
| Multiple variables | | x | | | | x | x | | | | |
| Entity/table-scoped | | | | | | | | x | | | |
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
| Nested-PER objective | | | | x | | | | | | | |
| Quadratic objective | | | | | x | | x | | | | |
| Bilinear objective | | | | | | x | | | | | |
| Mixed linear + quadratic | | | | | x | | | | | | |
| WHEN on objective | x | | | | | | | | | | |
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

**WHEN / PER**

| Feature | Q1 | Q2 | Q3 | Q4 | Q5 | Q6 | Q7 | Q8 | Q9 | Q10 | Q11 |
|---|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:---:|:---:|
| WHEN on constraint | x | | | | | x | | x | | | |
| WHEN on objective | x | | | | | | | | | | |
| Aggregate-local WHEN | x | | | | | | | | | | |
| WHEN + PER composition | | | | | | x | | x | | | |
| PER single-column | | | | x | | x | | x | | | |
| PER multi-column | x | | | | | | | x | | | |
| PER on objective (nested) | | | | x | | | | | | | |
| IS NULL / IS NOT NULL | | | | | | | | x | | | |

**Deliberate omissions.** Two objective forms are left uncovered to keep the count at 11: `MAXIMIZE MIN` (easy) — mechanically identical to Q4's `MINIMIZE MAX`; and `MINIMIZE MIN` (hard) — the symmetric twin of Q9's `MAXIMIZE MAX`. Adding either costs a whole query (one objective per query) for a construct whose linearization is already exercised.

## Output

### Visual Output

After each run, `view_results.py` displays colored stage-proportion bars:

```
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
Q1: ilp_selection
  SELECT l_orderkey, l_linenumber, ..., keep
  FROM lineitem
  DECIDE keep IS BOOLEAN
  SUCH THAT SUM(keep * l_quantity) <= 3829286 ...
  MAXIMIZE SUM(keep * l_extendedprice) WHEN l_linestatus = 'F';

  medium  │ 500K rows │ 500K vars │ ... │ 1.73s
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
      "description": "ilp_selection",
      "size": "medium",
      "sql": "SELECT l_orderkey ... SUCH THAT SUM(keep * l_quantity) <= 3829286 ...",
      "coefficients": {
        "Q1_QTY_CAP": 3829286,
        "Q1_GRP_CAP": 62500
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
│   ├── q1_ilp_selection.sql
│   ├── q2_integer_domains.sql
│   ├── q3_abs_norms.sql
│   ├── q4_minmax_nested.sql
│   ├── q5_qp_qcqp.sql
│   ├── q6_bilinear_mccormick.sql
│   ├── q7_nonconvex.sql
│   ├── q8_feasibility_join.sql
│   ├── q9_maximize_max.sql
│   ├── q10_maximize_abs.sql
│   ├── q11_lp_allocation.sql
│   └── manual.sql.example
├── databases/                 # Generated TPC-H DBs (gitignored)
│   ├── medium.db              # SF=0.085, exactly 500K lineitem rows
│   └── large.db               # SF=0.17, exactly 1M lineitem rows
├── results/                   # JSON outputs (gitignored)
└── .gitignore
```
