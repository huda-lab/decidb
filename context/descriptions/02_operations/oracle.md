# Oracle-Based DECIDE Testing Framework

Pytest-based differential testing framework that validates DecidB's DECIDE clause
by comparing its output against hand-written ILP models formulated in
**gurobipy** (no SQL parsing on the oracle side, no dependence on DecidB's own
solver or optimizer).

Located at `test/decide/`.

---

## 1. How It Works

```
                       ┌──────────────┐
  DECIDE SQL ─────────►│   DecidB CLI │──► rows (variable values)
                       └──────────────┘
                              │
                              ▼
                       ┌──────────────┐
  same data ─────────►│ gurobipy ILP │──► objective + variable vector
  (via duckdb_conn)    └──────────────┘
                              │
                              ▼
                   compare_solutions(...) — objective AND vector
```

Each correctness test:
1. Runs a DECIDE query through the native DecidB CLI (`build/release/decidb`)
   via `decidb_cli.execute(sql)` and captures the output `(rows, cols)`.
2. Fetches the same underlying data via `duckdb_conn` (vanilla `duckdb`
   against a separately-generated TPC-H database, `_tpch_oracle.duckdb`).
3. Builds an equivalent ILP in gurobipy via the `oracle_solver` fixture.
4. Solves it.
5. Calls `compare_solutions` which checks (a) that DecidB's objective
   (computed from its variable values) matches the oracle's `objective_value`
   within tolerance and (b) that the decision-variable vector matches row
   by row. The returned `ComparisonResult.status` is `"identical"` when both
   match and `"optimal"` when objectives agree but the vectors differ (an
   alternate optimum at the same objective value).

Forbidden: analytical / hand-computed closed-form assertions like
`expected = {1: 10.0, 2: 20.0, 3: 30.0}`. Those only check the instance the
author thought through; an encoding bug that happens to coincide with the
expected answer passes silently. The oracle must be an independent
gurobipy formulation.

---

## 2. Directory Structure

```
test/decide/
├── run_tests.sh               # Virtualenv manager + gurobipy pre-flight + test runner
├── decidb_cli.py              # Subprocess wrapper around build/release/decidb
├── conftest.py                # Fixtures, markers, session hooks
├── oracle_cache.py            # On-disk cache of oracle solve results
├── pytest.ini                 # Marker registration
├── requirements.txt           # duckdb, gurobipy, pytest
├── _tpch_oracle.duckdb        # Vanilla TPC-H DB for duckdb_conn (gitignored)
├── .gitignore
│
├── solver/                    # Gurobi-only oracle solver
│   ├── types.py               #   SolverResult, VarType, ObjSense, SolverStatus
│   ├── base.py                #   OracleSolver ABC
│   ├── gurobi_backend.py      #   gurobipy wrapper (only backend)
│   └── factory.py             #   Returns GurobiSolver; ImportError otherwise
│
├── comparison/
│   └── compare.py             # compare_solutions (modern API), assert_optimal_match (legacy),
│                              # assert_infeasible
│
├── performance/
│   ├── tracker.py             # PerfTracker / PerfRecord
│   └── reporter.py            # CLI table printer, JSON output
│
├── results/                   # JSON perf files + oracle_cache.json (gitignored)
└── tests/
    ├── _oracle_helpers.py     # Shared primitives (group_indices, add_ne_indicator,
    │                          # add_in_domain, add_bool_and,
    │                          # emit_inner_max/min, emit_hard_inner_max/min)
    ├── test_oracle_api_quadratic.py  # Smoke tests for the quadratic oracle API
    └── test_*.py              # Feature test files
```

---

## 3. Infrastructure Components

### 3.1 Solver Abstraction (`solver/`)

**`types.py`** — Shared enums and result type:
- `VarType`: BINARY, INTEGER, CONTINUOUS
- `ObjSense`: MAXIMIZE, MINIMIZE
- `SolverStatus`: OPTIMAL, INFEASIBLE, UNBOUNDED, etc.
- `SolverResult`: status, objective_value, variable_values dict, solve_time_seconds

**`base.py`** — `OracleSolver` abstract base class:
```python
create_model(name)
add_variable(name, var_type, lb=0.0, ub=None)
add_constraint(coeffs, sense, rhs, name="")                    # linear, sense ∈ {"<=", ">=", "="}
set_objective(coeffs, sense)                                   # linear
set_quadratic_objective(linear, quadratic, sense, constant=0.0)
add_quadratic_constraint(linear, quadratic, sense, rhs, name="")
add_indicator_constraint(indicator_var, indicator_val,
                         coeffs, sense, rhs, name="")          # Gurobi-native, Big-M-free
solve(time_limit=60.0)                                         # Returns SolverResult
solver_name()                                                  # "Gurobi"
```

`quadratic` is a dict `{(var_i, var_j): coeff}`. Diagonal entries produce
`x²`; off-diagonal produce bilinear `x·y`. Symmetric entries are
de-duplicated by the backend. When any quadratic term is present the
backend automatically sets `model.Params.NonConvex = 2`, which lets Gurobi
handle convex, concave, and non-convex QP/QCQP uniformly.

**`gurobi_backend.py`** — Wraps `gurobipy.Model` (tested against gurobipy
12.x). Linear constraints are added via the modern `addConstr(tc)` form
(the expression/sense/rhs overload was removed in Gurobi 12). Quadratic
objectives build a `QuadExpr`; quadratic constraints go through
`addQConstr`. Indicator constraints use `addGenConstrIndicator` — a
native implication that does not require a hand-picked Big-M.

**`factory.py`** — `get_solver()` returns a `GurobiSolver`. If gurobipy is
not installed or the Gurobi environment cannot start (missing/expired
license), raises `ImportError` with a clear message. HiGHS is not a fallback
on the oracle side; the separate DecidB CLI-side HiGHS backend documented
in `01_pipeline/03d_solver_backends.md` is unrelated.

### 3.2 Comparison (`comparison/compare.py`)

**`compare_solutions(decidb_rows, decidb_cols, oracle_result, oracle_data, decide_var_names, coeff_fn=None, tolerance=1e-4, decidb_objective_fn=None) -> ComparisonResult`**

The modern API. Checks both the objective value and the decision-variable
vector; prefer this for every new correctness test.

- For linear objectives, pass `coeff_fn(row) -> {var_name: coefficient}`. The
  DecidB objective is computed as `Σ (row[var] × coeff_fn(row)[var])` over
  rows × variables.
- For non-linear objectives (QP/QCQP), pass
  `decidb_objective_fn(rows, cols) -> float` which evaluates the objective
  directly on DecidB's variable values — e.g. for
  `MINIMIZE SUM(POWER(x - t, 2))` compute `Σ (x_i - t_i)²` from the rows.
  When the oracle uses `set_quadratic_objective` without a `constant`, the
  oracle's reported `objective_value` omits the `Σ t_i²` term; the
  `decidb_objective_fn` should subtract the same constant so both sides
  match.

Returns a `ComparisonResult` with:
- `status`: `"identical"` (objective + vector match) or `"optimal"`
  (objective matches but vector differs — alternate optimum).
- `decidb_objective`, `oracle_objective`, `decidb_vector`, `oracle_vector`.

Raises `AssertionError` if objectives differ beyond `tolerance`. Typical
tolerance is `1e-4` for linear problems; QCQP with tight
`POWER(expr, 2) <= 0` (equality-via-quadratic) constraints may need `5e-2`
to absorb Gurobi's feasibility tolerance — see
`tests/test_quadratic_constraints.py` for an in-place example.

**`assert_optimal_match(...)`** — Legacy objective-only comparison, retained
for backward compatibility. New tests should not use it.

**`assert_infeasible(decidb_cli, sql)`** — Asserts that the SQL produces an
`infeasible`/`unbounded` error from DecidB. Compose it with an independent
gurobipy check (see §3.6) to cross-verify on the oracle side.

### 3.3 Fixtures (`conftest.py`)

| Fixture | Scope | Purpose |
|---------|-------|---------|
| `decidb_db_path` | session | Path to `decidb.db` (TPC-H SF-0.01); skip if missing |
| `decidb_exe_path` | session | Path to `build/release/decidb`; skip if missing |
| `decidb_cli` | session | `DecidBCli` subprocess wrapper — `execute(sql) -> (rows, cols)`, `assert_error(sql, match=...)` |
| `_oracle_db_path` | session | Path to `_tpch_oracle.duckdb`; auto-generated on first run via `CALL dbgen(sf=0.01)` against a vanilla duckdb connection |
| `duckdb_conn` | function | Per-test read-only vanilla `duckdb` connection to `_tpch_oracle.duckdb` (no `decidb` Python package involved) |
| `oracle_solver` | function | `CachedOracleSolver` wrapping a `GurobiSolver`; per-test so the cache can disambiguate multiple models per test |
| `perf_tracker` | session | Collects timing; writes JSON + prints table on teardown |

DecidB and the oracle use two separate databases (`decidb.db` and
`_tpch_oracle.duckdb`) generated with the same deterministic `dbgen`
algorithm at the same scale factor, so the data is identical. The oracle
side never touches the `decidb` Python package.

### 3.4 Oracle Cache (`oracle_cache.py`)

Oracle solves are deterministic for a fixed (test source, database) pair, so the framework caches solver results on disk to amortize the oracle cost across reruns.

**File**: `test/decide/results/oracle_cache.json` (gitignored, generated on first run).

**Keying**: each entry is keyed by pytest node ID, with an `input_hash` derived from:
- `inspect.getsource(test_fn)` — hash of the test function's source code.
- DB checksum — `sha256("{size}:{mtime_ns}")[:16]` of `decidb.db`.

Changing either the test body or the underlying database invalidates only the affected entries.

**Global invalidation**: the cache file stores the DB checksum at the top level. If the DB changes, the entire cache is dropped on next load.

**GC**: on a full (unfiltered) test run, `save(gc=True)` prunes entries that weren't accessed during the run. Filtered runs (`-k`, `-m`) skip GC so unrelated entries survive.

**How it plugs in**: `CachedOracleSolver` wraps the real `OracleSolver` as a drop-in replacement. `create_model()` consults the cache:
- **Hit** — all subsequent model-building calls (`add_variable`, `add_constraint`, `set_objective`, `set_quadratic_objective`, `add_quadratic_constraint`, `add_indicator_constraint`) become no-ops; `solve()` returns the cached `SolverResult` instantly.
- **Miss** — everything is delegated to the real solver; `solve()` stores the result before returning. If no real solver is available and the cache misses, the test is skipped with a clear message.

**Parametric loops**: a single test may call `create_model()` multiple times (e.g. looping over budgets). Each call gets a unique cache key by suffixing `#2`, `#3`, … so earlier solves don't shadow later ones.

**Manual reset**: delete `test/decide/results/oracle_cache.json`. A full rerun will repopulate it.

### 3.5 Shared oracle primitives (`tests/_oracle_helpers.py`)

Tests that encode common DECIDE constructs import helpers from
`tests/_oracle_helpers.py` rather than re-rolling the same Big-M or
indicator patterns in every file. Prefer these over hand-written Big-M
mirrors — see §3.7 on independent semantics.

| Helper | Purpose |
|---|---|
| `group_indices(data, key_fn)` | Group row indices by a PER key. Rows where `key_fn` returns `None` are dropped (matches DECIDE's exclusion of NULL-keyed rows from every group). |
| `add_ne_indicator(oracle, coeffs, rhs, name)` | Encode `sum(coeffs) != rhs` over integer-valued terms via a native Gurobi indicator constraint (no Big-M). |
| `add_in_domain(oracle, var_name, domain)` | Restrict `var_name` to a discrete set via SOS1-style indicator encoding (`SUM(z_k) = 1`, `var = SUM(v_k·z_k)`). |
| `add_bool_and(oracle, x, y, z)` | Link binary `z = x ∧ y` with `z ≤ x`, `z ≤ y`, `z ≥ x + y − 1`. Used for Bool × Bool products before summation. |
| `emit_inner_max` / `emit_inner_min` | Per-group auxiliary variable for the *easy* inner-MAX / inner-MIN case of `SUM(MAX(expr)) PER col` / `SUM(MIN(expr)) PER col` inside objectives. |
| `emit_hard_inner_max` / `emit_hard_inner_min` | Same for the *hard* case — uses per-row Gurobi indicators to tightly pin the auxiliary to one row's value. |

### 3.6 Cross-verifying infeasibility

For infeasible / unbounded tests, the oracle independently formulates the
same ILP in gurobipy and asserts the solver's status matches:

```python
decidb_cli.assert_error(sql, match=r"(?i)(infeasible|unbounded)")

# Independent cross-check:
oracle_solver.create_model("infeas_case")
# ... build the same ILP ...
assert oracle_solver.solve().status in (
    SolverStatus.INFEASIBLE, SolverStatus.UNBOUNDED,
)
```

This guards against DecidB falsely reporting infeasibility on a feasible
problem (or vice versa). See `tests/test_error_infeasible.py` for the
canonical pattern.

### 3.7 Independent semantics, not Big-M mirrors

For discrete constructs that DecidB rewrites into Big-M linear programs
(`<>`, IN, etc.), the oracle should encode the **semantics
natively using Gurobi features** — not mirror DecidB's Big-M reformulation.
Mirroring only detects lockstep bugs (where both implementations agree on
an incorrect rewrite); independent encoding catches DecidB encoding errors
too.

Concretely:

- **`<>`** — use `add_ne_indicator` (disjunctive branch indicator with native
  implications). Do not mirror DecidB's `SUM + M·z <= rhs - 1 + M` / `SUM - M·z >= rhs + 1 - M`.
- **IN (…)** — use `add_in_domain` (SOS1 with indicators). This one happens
  to coincide with DecidB's bind-time rewrite; it's still the right oracle
  shape because it's the canonical formulation, not a mirror.

### 3.8 Performance Tracking (`performance/`)

Each correctness test calls `perf_tracker.record(...)` with:
- DecidB wall-clock time, oracle build time, oracle solve time
- Problem dimensions (rows, variables, constraints)
- Objective value and solver backend name

On session teardown, the tracker saves a timestamped JSON file to `results/`
and prints a CLI summary table.

---

## 4. Running Tests

```bash
cd test/decide
bash run_tests.sh                          # All tests
bash run_tests.sh -k "test_q01"            # Single test
bash run_tests.sh -m "var_boolean"         # By marker
bash run_tests.sh -m "correctness"         # All oracle comparisons
bash run_tests.sh -m "error"              # All error tests
bash run_tests.sh -m "not large_scale"    # Skip perf tests
```

`run_tests.sh` creates a `.venv/` virtualenv on first run, installs
`duckdb`, `gurobipy`, and `pytest` from `requirements.txt`, pre-flights
gurobipy by starting a Gurobi environment (catches missing/expired
licenses up front), and then invokes pytest. The `decidb` Python package is
not a dependency — DECIDE queries run via the native CLI executable, not
an in-process binding.

---

## 5. Test Categories & Markers

| Marker | Description |
|--------|-------------|
| `var_boolean` | IS BOOLEAN decision variables |
| `var_integer` | IS INTEGER / default type |
| `cons_aggregate` | SUM-based aggregate constraints |
| `cons_perrow` | Per-row bounds (`x <= 5`, `x <= col`) |
| `cons_mixed` | Aggregate + per-row combined |
| `cons_between` | BETWEEN ... AND ... |
| `cons_subquery` | Subquery on constraint RHS |
| `cons_multi` | Multiple constraints |
| `obj_maximize` | MAXIMIZE objective |
| `obj_minimize` | MINIMIZE objective |
| `obj_complex` | Complex coefficient arithmetic |
| `sql_joins` | JOINs with DECIDE |
| `sql_subquery` | SQL subquery features |
| `error_parser` | Parser-level syntax errors |
| `error_binder` | Binder-level semantic errors |
| `error_infeasible` | Infeasible models |
| `per_clause` | PER keyword |
| `large_scale` | Performance / scale tests |
| `correctness` | Meta: all oracle comparison tests |
| `error` | Meta: all error tests |
| `performance` | Meta: all performance tests |

---

## 6. Correctness Test Reference

### 6.1 Variable Type Tests (`test_var_boolean.py`, `test_var_integer.py`)

**test_q01_knapsack_binary** — Classic 0/1 knapsack on lineitem.
```sql
SELECT ... FROM lineitem WHERE l_orderkey < 100
DECIDE x IS BOOLEAN
SUCH THAT SUM(x * l_quantity) <= 100
MAXIMIZE SUM(x * l_extendedprice)
```
Oracle: binary variables, one capacity constraint, maximize total price.

**test_knapsack_lineitem** — Variant with weight limit 500 on l_orderkey <= 200.

**test_simple_test** — Default (INTEGER) variable type, bounded 0-10.
```sql
SELECT ... FROM lineitem WHERE l_orderkey <= 10
DECIDE x
SUCH THAT SUM(x * l_quantity) <= 500 AND x <= 10
MAXIMIZE SUM(x * l_extendedprice)
```
Oracle: integer variables with upper bound 10, one aggregate constraint.

### 6.2 Constraint Tests

**test_q08_marketing_campaign** (`test_cons_aggregate.py`) — Boolean selection
with budget constraint: `SUM(x * c_acctbal) <= 5000`.

**test_order_selection** (`test_cons_aggregate.py`) — Select orders under weight
limit: `SUM(x * o_totalprice) <= 50000`.

**test_q07_row_wise_bounds** (`test_cons_perrow.py`) — Per-row + aggregate:
```sql
DECIDE x IS INTEGER
SUCH THAT x <= 5 AND SUM(x * ps_supplycost) <= 1000
MAXIMIZE SUM(x * ps_availqty)
```
Oracle: integer vars with `ub=5`, aggregate budget constraint.

**test_q02_integer_procurement** (`test_cons_mixed.py`) — Per-row + aggregate:
```sql
DECIDE x IS INTEGER
SUCH THAT x <= ps_availqty AND SUM(x * ps_supplycost) <= 5000
MAXIMIZE SUM(x * ps_availqty)
```
Oracle: upper bound per variable from `ps_availqty` column.

**test_q10_logic_dependency** (`test_cons_between.py`) — BETWEEN + multi-constraint:
```sql
DECIDE x IS INTEGER
SUCH THAT x BETWEEN 0 AND 5
  AND SUM(x) <= 100
  AND SUM(x * l_extendedprice) <= 100000
MAXIMIZE SUM(x * l_extendedprice * (1 - l_discount))
```
Oracle: variables bounded [0, 5], two aggregate constraints.

**test_q04_subquery_rhs** (`test_cons_subquery.py`) — Subquery as constraint bound:
```sql
SUCH THAT SUM(x * o_totalprice) <= (SELECT AVG(c_acctbal) FROM tpch.customer WHERE c_nationkey = 10)
```
Oracle: resolves the subquery via plain SQL first, then uses the scalar value.

**test_q06_multi_constraint** (`test_cons_multi.py`) — Two constraints with join:
```sql
FROM lineitem, orders WHERE l_orderkey = o_orderkey AND l_orderkey <= 50
SUCH THAT SUM(x * l_quantity) <= 200 AND SUM(x * o_totalprice) <= 50000
```
Oracle: two aggregate constraints on different columns from joined data.

### 6.3 Objective Tests

**test_q09_minimize_cost** (`test_obj_minimize.py`) — Minimize selection:
```sql
FROM supplier WHERE s_nationkey <= 5
DECIDE x IS BOOLEAN
SUCH THAT SUM(x) >= 10
MINIMIZE SUM(x * s_acctbal)
```
Oracle: select at least 10, minimize total acctbal.

**test_min_cost_supplier** (`test_obj_minimize.py`) — Minimize cost to meet demand:
```sql
SUCH THAT SUM(x * ps_availqty) >= 1000
MINIMIZE SUM(x * ps_supplycost)
```

**test_q03_complex_coeffs** (`test_obj_complex_coeffs.py`) — Multi-column arithmetic:
```sql
MAXIMIZE SUM(x * l_extendedprice * (1 - l_discount) * (1 + l_tax))
```
Oracle: pre-computes `price * (1 - discount) * (1 + tax)` per row.

### 6.4 SQL Feature Tests

**test_q05_join_decide** (`test_sql_joins.py`) — JOIN in FROM clause:
```sql
FROM orders, customer
WHERE o_custkey = c_custkey AND c_nationkey = 10
```
Oracle: fetches joined result set, builds model on joined rows.

### 6.5 Scale Tests (`test_large_scale.py`)

**test_knapsack_large** — 501 rows (lineitem, orderkey <= 100).
**test_order_selection_large** — 2204 rows (orders, orderkey <= 3000).

Both compare objective values and record timing.

---

## 7. Error Test Reference

### 7.1 Parser Errors (`test_error_parser.py`) — 4 tests

| Test | What it checks |
|------|----------------|
| `test_missing_such_that` | DECIDE without SUCH THAT |
| `test_missing_variable_name` | DECIDE IS BOOLEAN (no name) |
| `test_missing_decide_keyword` | SUCH THAT without DECIDE |
| `test_empty_constraint` | SUCH THAT (empty) |

All expect `decidb.ParserException`.

### 7.2 Binder Errors (`test_error_binder.py`) — 16 tests

| Test | Error |
|------|-------|
| `test_variable_conflicts_with_column` | Name clashes with existing column |
| `test_duplicate_decide_variables` | Same variable declared twice |
| `test_unknown_variable_in_constraint` | Undeclared var in IN list |
| `test_is_null_unsupported` | IS NULL in SUCH THAT |
| `test_sum_with_in_not_allowed` | SUM(x) IN (...) |
| `test_non_decide_variable_in_constraint` | Regular column constrained |
| `test_non_sum_function_in_constraint` | AVG(x) instead of SUM |
| `test_no_decide_variable_in_sum` | SUM(col) without decide var |
| `test_multiple_decide_variables_in_sum` | x*x is quadratic |
| `test_nonlinear_decide_variables` | Non-linearity caught at execution |
| `test_between_non_scalar` | SUM BETWEEN with non-scalar bound |
| `test_decide_between_decide_variable` | DECIDE var in BETWEEN bounds |
| `test_in_rhs_with_decide_variable` | DECIDE var in IN list values |
| `test_sum_rhs_non_scalar` | SUM compared to non-scalar |
| `test_decide_variable_rhs_with_decide` | Var compared to var |
| `test_sum_equal_non_scalar` | SUM = non-scalar |
| `test_objective_with_addition` | SUM(...)+3 in objective |
| `test_objective_bare_column` | Bare column as objective |
| `test_subquery_rhs_non_scalar` | Subquery referencing DECIDE var |

### 7.3 Infeasibility Tests (`test_error_infeasible.py`) — 3 tests

| Test | Contradiction |
|------|---------------|
| `test_contradictory_bounds` | x >= 10 AND x <= 5 |
| `test_impossible_sum` | SUM(x) >= 1000, but only 6 boolean vars |
| `test_conflicting_aggregate` | SUM(x) >= 100 AND SUM(x) <= 1 |

All expect `decidb.InvalidInputException` matching "infeasible".

---

## 8. Adding a New Test

### Step 1: Choose the test file

Pick the file that matches the primary feature being tested (see section 5).
Create a new file if none fits.

### Step 2: Write the test function

```python
import time
import pytest

from solver.types import VarType, ObjSense
from comparison.compare import compare_solutions


@pytest.mark.var_boolean          # variable type
@pytest.mark.cons_aggregate       # constraint category
@pytest.mark.obj_maximize         # objective type
@pytest.mark.correctness          # required for oracle-verified tests
def test_my_new_query(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    # 1. Run DECIDE via the native CLI. decidb_cli.execute returns (rows, cols).
    sql = """
        SELECT col1, col2, x
        FROM my_table WHERE some_filter
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * col1) <= 100
        MAXIMIZE SUM(x * col2)
    """
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    # 2. Fetch the same data via vanilla duckdb (independent of DecidB).
    data = duckdb_conn.execute("""
        SELECT CAST(col1 AS DOUBLE), CAST(col2 AS DOUBLE)
        FROM my_table WHERE some_filter
    """).fetchall()

    # 3. Formulate the equivalent ILP in gurobipy.
    t_build = time.perf_counter()
    oracle_solver.create_model("my_test")
    n = len(data)
    vnames = [f"x_{i}" for i in range(n)]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    oracle_solver.add_constraint(
        {vnames[i]: data[i][0] for i in range(n)},
        "<=", 100.0, name="capacity",
    )
    oracle_solver.set_objective(
        {vnames[i]: data[i][1] for i in range(n)},
        ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    # 4. Compare objective + decision vector. For non-linear objectives,
    #    pass decidb_objective_fn=lambda rows, cols: ... instead of coeff_fn.
    cmp = compare_solutions(
        decidb_rows, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[decidb_cols.index("col2")])},
    )

    # 5. Record performance and comparison status.
    perf_tracker.record(
        "my_test", decidb_time, build_time,
        result.solve_time_seconds, n, len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )
```

For aggregate operators with non-trivial semantics (PER, `<>`,
bilinear, MIN/MAX hard cases, nested aggregates), import helpers from
`tests/_oracle_helpers.py` rather than re-rolling the logic. See §3.5.

For quadratic objectives / constraints, use
`oracle_solver.set_quadratic_objective(linear, quadratic, sense)` and
`oracle_solver.add_quadratic_constraint(linear, quadratic, sense, rhs)`.
Pass `decidb_objective_fn` to `compare_solutions` since DecidB's objective
cannot be recovered via a linear `coeff_fn`.

### Step 3: Run

```bash
bash run_tests.sh -k "test_my_new_query" -v
```

---

## 9. Known Caveats

1. **Absolute tolerance on large objectives.** Comparison uses `|decidb - oracle| <= 1e-4`.
   For objectives in the millions, a relative tolerance might be more appropriate.
   No failures observed in practice.

2. **Session-scoped stateful solver.** The `oracle_solver` fixture is session-scoped.
   Each test calls `create_model()` which resets state, but if a test crashes mid-model
   the next test could inherit stale state.

3. **Legacy `assert_optimal_match` is objective-only.** The legacy wrapper
   checks objective equality but does not compare decision-variable vectors.
   `compare_solutions` (the modern API, §3.2) does both — prefer it for all
   new tests.

4. **Fatal error cascading.** `test_nonlinear_decide_variables` triggers an
   `InternalException`. `decidb_cli` shells out to a fresh subprocess per
   call, so there's no shared state to corrupt — unlike the old in-process
   `decidb_conn` fixture this replaced.

5. **DECIDE subquery search path.** Subqueries inside SUCH THAT don't inherit
   the connection's `search_path`. Tables must be fully qualified
   (e.g. `tpch.customer`).

6. **Oracle side is fully independent of DecidB.** `duckdb_conn` uses vanilla
   `duckdb` against a separately-generated `_tpch_oracle.duckdb`; the
   `decidb` Python package is not imported anywhere in the test code. Both
   databases are seeded with the same deterministic `dbgen` scale factor
   so the data is identical.

7. **Wall-clock performance numbers.** Timings reflect wall-clock time
   including Python overhead and subprocess startup, not CPU time. They
   are useful for relative comparisons across runs but not for absolute
   benchmarking.

8. **Gurobi is mandatory on the oracle side.** If gurobipy isn't installed or
   no valid Gurobi license is available, `run_tests.sh` pre-flights the
   failure and exits with a clear error. There is no HiGHS fallback on the
   oracle side; the DecidB CLI's HiGHS fallback (`01_pipeline/03d_solver_backends.md`)
   is unrelated.

9. **TPC-H scale factor sensitivity.** Tests are tuned for SF-0.01
   (`decidb.db` and `_tpch_oracle.duckdb`). Changing the scale factor may
   make some queries infeasible or shift constraint boundaries. Example:
   q09 uses `s_nationkey <= 5` to ensure 20+ suppliers.

10. **sys.path manipulation in conftest.** `conftest.py` inserts the
    test/decide directory onto `sys.path` so that `solver/`, `comparison/`,
    `performance/`, and the top-level `decidb_cli` and `oracle_cache`
    modules are importable. This is a convenience hack; a proper package
    install would be cleaner.

11. **QCQP tolerance.** `POWER(expr, 2) <= 0` (equality-via-quadratic) only
    holds up to Gurobi's feasibility tolerance (~1e-6), which propagates
    into the objective. `test_quadratic_constraints.py` relaxes its
    comparison tolerance to `5e-2` for this reason; real encoding bugs
    differ by orders of magnitude, not by solver noise.
