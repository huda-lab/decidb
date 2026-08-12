# DECIDE Test Framework

Pytest-based testing for DeciDB's DECIDE clause. Each correctness test has a
hand-written Python oracle that builds an ILP model using gurobipy directly —
no SQL parsing in the oracle. Oracle results are cached on disk so the solver
only runs when a test or the database changes.

## Quick Start

```bash
# From the repo root — creates .venv, installs deps, runs all tests:
make decide-test

# Or directly:
./test/decide/run_tests.sh

# Just set up the virtualenv without running tests:
make decide-setup
```

The `run_tests.sh` script automatically creates a virtualenv at
`test/decide/.venv/` on first run, installs all dependencies, and then
invokes pytest.

### Parallelism

Tests run across **8 xdist workers by default** (~25s, vs ~130s serially). The
suite parallelizes well because its cost is dominated by tests that sit on a
wall-clock solver time limit rather than by CPU work — `test_query_diagnostics_slow.py`
alone is ~46% of a serial run.

```bash
DECIDE_TEST_JOBS=4 ./test/decide/run_tests.sh   # 4 workers
DECIDE_TEST_JOBS=0 ./test/decide/run_tests.sh   # serial, for readable output
./test/decide/run_tests.sh -n 2                 # explicit -n always wins
```

Run serially when you need a single test's stdout/stderr uninterleaved, or when
bisecting an order-dependent failure.

## Setup (Manual)

```bash
# Create virtualenv
python3 -m venv test/decide/.venv
source test/decide/.venv/bin/activate

# Install dependencies (vanilla duckdb, gurobipy, pytest)
pip install -r test/decide/requirements.txt
# gurobipy requires a valid Gurobi license; run_tests.sh pre-flights this.

# Ensure decidb executable + decidb.db exist
make                   # build DeciDB executable (build/release/decidb)
# decidb.db should already exist; if not, generate TPC-H data via DeciDB
```

> **Note:** The DeciDB Python package (`tools/pythonpkg`) is **not** required.
> DECIDE queries run via the native CLI executable, and oracle data fetching
> uses vanilla `duckdb` with a separately generated TPC-H database.

## Running Tests

```bash
# Run all tests
make decide-test

# Run by category marker (-m)
./test/decide/run_tests.sh -m var_boolean            # boolean variable tests
./test/decide/run_tests.sh -m var_integer            # integer variable tests
./test/decide/run_tests.sh -m cons_aggregate         # SUM constraint tests
./test/decide/run_tests.sh -m cons_perrow            # per-row bound tests
./test/decide/run_tests.sh -m cons_between           # BETWEEN constraint tests
./test/decide/run_tests.sh -m cons_comparison        # comparison operator tests (=, <, >)
./test/decide/run_tests.sh -m cons_in                # IN domain tests (xfail)
./test/decide/run_tests.sh -m edge_case              # boundary / degenerate input tests
./test/decide/run_tests.sh -m when_constraint        # WHEN on constraints
./test/decide/run_tests.sh -m when_objective         # WHEN on objectives
./test/decide/run_tests.sh -m when_perrow            # WHEN on per-row bounds
./test/decide/run_tests.sh -m when_compound          # compound WHEN (AND/OR)
./test/decide/run_tests.sh -m per_clause             # PER keyword
./test/decide/run_tests.sh -m explain                # EXPLAIN / EXPLAIN ANALYZE output tests
./test/decide/run_tests.sh -m sql_joins              # JOIN tests
./test/decide/run_tests.sh -m query_diagnostics      # DECIDE diagnostic failure/reporting tests
./test/decide/run_tests.sh -m large_scale            # performance / scaling

# Meta markers — combine multiple categories
./test/decide/run_tests.sh -m "correctness"          # all oracle comparison tests
./test/decide/run_tests.sh -m "error"                # all error tests (parser + binder + infeasible)
./test/decide/run_tests.sh -m "not large_scale"      # skip slow tests

# Boolean expressions on markers
./test/decide/run_tests.sh -m "when_constraint or when_objective"   # all WHEN tests on constraints + objectives
./test/decide/run_tests.sh -m "correctness and not large_scale"     # fast correctness tests only

# Run a specific test file or function
./test/decide/run_tests.sh -k test_var_boolean
./test/decide/run_tests.sh -k test_q01_knapsack_binary
```

## Test Categories

### Variable Types

| Marker | File | Tests | Status |
|--------|------|-------|--------|
| `var_boolean` | `test_var_boolean.py` | 2 | passing |
| `var_integer` | `test_var_integer.py` | 1 | passing |
| `var_multi` | `test_var_multi.py` | 2 | xpass (multiple DECIDE variables) |

### Constraints

| Marker | File | Tests | Status |
|--------|------|-------|--------|
| `cons_aggregate` | `test_cons_aggregate.py` | 3 | passing |
| `cons_perrow` | `test_cons_perrow.py` | 2 | passing |
| `cons_mixed` | `test_cons_mixed.py` | 1 | passing |
| `cons_between` | `test_cons_between.py` | 1 | passing |
| `cons_multi` | `test_cons_multi.py` | 1 | passing |
| `cons_subquery` | `test_cons_subquery.py` | 1 | passing |
| `cons_comparison` | `test_cons_comparison.py` | 4 | 3 passing, 1 xfail (`<>` not-equal) |
| `cons_in` | `test_cons_in.py` | 2 | xfail (IN domain constraints) |

### Objectives

| Marker | File | Tests | Status |
|--------|------|-------|--------|
| `obj_maximize` | `test_obj_maximize.py` | — | covered by other files |
| `obj_minimize` | `test_obj_minimize.py` | 3 | passing |
| `obj_complex` | `test_obj_complex_coeffs.py` | 1 | passing |

### WHEN Clause

| Marker | File | Tests | Status |
|--------|------|-------|--------|
| `when_constraint` | `test_when_constraint.py` | 8 | passing |
| `when_objective` | `test_when_objective.py` | 7 | passing |
| `when_perrow` | `test_when_perrow.py` | 5 | passing |
| `when_compound` | `test_when_compound.py` | 6 | passing |

### SQL Features

| Marker | File | Tests | Status |
|--------|------|-------|--------|
| `sql_joins` | `test_sql_joins.py` | 2 | passing |
| `sql_subquery` | `test_sql_subquery.py` | — | covered by `test_cons_subquery.py` |

### Edge Cases

| Marker | File | Tests | Status |
|--------|------|-------|--------|
| `edge_case` | `test_edge_cases.py` | 4 | passing |

### Error Cases

| Marker | File | Tests | Status |
|--------|------|-------|--------|
| `error_parser` | `test_error_parser.py` | 4 | passing |
| `error_binder` | `test_error_binder.py` | 21 | passing |
| `error_infeasible` | `test_error_infeasible.py` | 4 | passing |

### PER Keyword

| Marker | File | Tests | Status |
|--------|------|-------|--------|
| `per_clause` | `test_per_clause.py` | 3 | passing |

### EXPLAIN Output

| Marker | File | Tests | Status |
|--------|------|-------|--------|
| `explain` | `test_explain.py` | 21 | passing |

### Query Diagnostics

| Marker | Coverage |
|--------|----------|
| `query_diagnostics` | Structured solver statuses, `diagnose_decide` settings, `decide_diagnostics()` relation lifecycle/schema, unbounded variable naming, and escaping-instance characterization |

### Scale & Performance

| Marker | File | Tests | Status |
|--------|------|-------|--------|
| `large_scale` | `test_large_scale.py` | 2 | passing |

### Meta Markers

These select across multiple files:

| Marker | What it selects |
|--------|----------------|
| `correctness` | All oracle-comparison correctness tests |
| `error` | All error tests (parser + binder + infeasible) |
| `performance` | All performance measurement tests |

## Solution Comparison

Each correctness test compares DeciDB output against the oracle at two levels:

1. **Objective value** — the total objective must match within tolerance (1e-4).
2. **Decision variable vector** — both sides are sorted by all non-decision
   columns, then variable assignments are compared element-wise.

The comparison produces a status:

| Status | Meaning |
|--------|---------|
| `identical` | Objective AND all variable assignments match |
| `optimal` | Objective matches but at least one assignment differs (alternate optimal) |

Both the status and the full decide vector are stored in the perf JSON and
oracle cache. Both DeciDB (via dynamic-loaded libgurobi in the CLI) and the
oracle (gurobipy) use Gurobi. Most tests report `identical`; the `optimal`
status appears when the problem has multiple optima and the two Gurobi runs
pick different ones — that is not a correctness failure, it's an alternate
optimal solution at the same objective value.

## Oracle Cache

Oracle solver results are cached in `results/oracle_cache.json`. On each
test run, the cache is checked before invoking the real solver:

- **Cache hit** — the solver is skipped entirely; the stored objective and
  variable values are used for comparison. Model-building calls are no-ops.
- **Cache miss** — the solver runs normally and the result (objective +
  variable assignments) is stored for next time.

Invalidation:

- **Per-test**: a hash of the test function's source code + database checksum.
  Changing a query, constraint, or any logic in the test invalidates only that
  entry.
- **Global**: the database file's size and modification time. Rebuilding
  `decidb.db` invalidates the entire cache.
- **GC**: stale entries (deleted/renamed tests) are pruned automatically on
  full (unfiltered) test runs that pass. A filtered run never visits the entries
  it would prune, and a failed run may be missing a worker's report, so neither
  prunes. A test that skips is not "accessed" either, so its entry is pruned and
  re-solved on the next run — cheap, and it keeps the cache honest.

Under xdist each worker builds its own `OracleCache` over the slice of tests it
was handed, so **no worker writes the shared file**: its GC would prune every
entry the other workers owned, and the last writer would win. Workers hand back
a `worker_report()` (entries solved this session + the set of test IDs they
visited) over xdist's `workeroutput` channel; the controller merges them in
`pytest_sessionfinish` and GCs against the union. The report rides the worker's
shutdown event rather than a file the worker drops on its way out, which is what
makes the GC safe: a worker that crashes and gets replaced delivers no report,
the controller notices the gap and skips pruning. A missing *file* would have
been indistinguishable from a worker that legitimately touched nothing.

To force a full re-solve, delete the cache file:

```bash
rm test/decide/results/oracle_cache.json
```

## Adding a New Test

1. Choose the appropriate test file based on the primary feature being tested
2. Follow the existing pattern:
   - Run the DECIDE query via `decidb_cli` (native executable, subprocess)
   - Fetch the same data via `duckdb_conn` (vanilla duckdb, plain SQL, no DECIDE)
   - Build an oracle model using `oracle_solver`
   - Compare with `compare_solutions` (returns `ComparisonResult` with status and vectors)
   - Record performance with `perf_tracker` (include `comparison_status` and `decide_vector`)
3. Add appropriate markers (feature marker + `correctness` meta marker)
4. The oracle cache updates automatically on the next run

### Never assert on user-facing message text

Error messages, caveats and diagnostic prose are owned by the user-facing-output principle
(see `CLAUDE.md`) and get reworded whenever the wording can be made clearer or less jargon-y.
A test that greps them silently stops matching — it does not fail loudly, it just stops doing
its job. That already happened once: the QCQP retry in `test_quadratic_constraints.py` matched
`solver status 13|[Ss]uboptimal`, the caveat was reworded to *"DECIDE is returning a feasible
solution — the solver could not prove it is the best possible"*, and the file turned into an
intermittent red.

To recognize a **solver terminal**, use the marker channel instead of the prose:

- DeciDB emits `DECIDB_STATUS: <TERMINAL>` on stderr when `DECIDB_STATUS_MARKERS` is set
  (currently `SUBOPTIMAL` only; `physical_decide.cpp`, SOLVED arm). Gated on the env var so
  user-facing output is byte-identical without it.
- `DecidBCli.execute` sets that env var, strips marker lines before classifying stderr, and
  exposes the terminal as `DecidBCliError.status`. Branch on `e.status == "SUBOPTIMAL"`, never
  on `str(e)`.
- `execute_raw` / `execute_script` / `execute_interactive` deliberately do **not** request
  markers: their callers classify raw stderr themselves and would read a marker line as an
  error. If a new terminal needs surfacing there, strip it at the call site first.

Asserting that an error *is about* the right thing (`assert "quadratic" in msg.lower()`) is
still fine — that is a coarse topic check, not a dependency on exact wording.

## Performance Results

After each run, a summary table is printed and a JSON file is saved to
`results/perf_YYYYMMDD_HHMMSS.json`. Each record includes timing data,
the comparison status (`identical`/`optimal`), and the full decide vector.

One run produces exactly one file regardless of worker count: under xdist each
worker returns its records over `workeroutput` and the controller writes the
merged set. These files are never pruned automatically, so `results/` grows by
one file per run — delete `results/perf_*.json` periodically (it reached 549
files / 83MB before its first cleanup).

## Architecture

```
test/decide/
├── conftest.py          # Fixtures (CLI wrapper, duckdb conn, solver, cache, perf)
├── decidb_cli.py        # Subprocess wrapper for build/release/decidb executable
├── oracle_cache.py      # Oracle result cache + CachedOracleSolver wrapper
├── solver/              # Gurobi-only oracle solver abstraction
├── comparison/          # Solution comparison (objective + variable vector)
├── performance/         # Perf tracking and reporting
├── _tpch_oracle.duckdb  # Auto-generated vanilla TPC-H database (gitignored)
├── results/             # JSON output (gitignored)
│   ├── oracle_cache.json
│   └── perf_*.json
└── tests/               # All test files by category
```

### Data Flow

```
┌─────────────────────────────────┐    ┌──────────────────────────────────┐
│  DeciDB (DECIDE queries)        │    │  Oracle (data fetching + ILP)    │
│                                 │    │                                  │
│  build/release/decidb (CLI)     │    │  vanilla duckdb (Python package) │
│  ↕ subprocess                   │    │  ↕ in-process                    │
│  decidb.db (DeciDB format)      │    │  _tpch_oracle.duckdb (vanilla)   │
│  solver: Gurobi (dlopen libgrb) │    │  solver: gurobipy                │
└─────────────────────────────────┘    └──────────────────────────────────┘
         │                                        │
         └──── compare_solutions() ◄──────────────┘
```

Both databases contain identical TPC-H data (same deterministic dbgen
algorithm and scale factor).  The oracle is completely independent of
DeciDB — no `import decidb` anywhere in the test code.
