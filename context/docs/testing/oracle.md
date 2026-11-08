# Test oracles

Pytest-based differential testing framework that validates DeciDB's DECIDE clause
by comparing its output against hand-written ILP models formulated in
**gurobipy** (no SQL parsing on the oracle side, no dependence on DeciDB's own
solver or optimizer).

Located at `test/decide/`.

---

## 1. How It Works

```
                       ┌──────────────┐
  DECIDE SQL ─────────►│   DeciDB CLI │──► rows (variable values)
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
1. Runs a DECIDE query through the native DeciDB CLI (`build/release/decidb`)
   via `decidb_cli.execute(sql)` and captures the output `(rows, cols)`.
2. Fetches the same underlying data via `duckdb_conn` (vanilla `duckdb`
   against a separately-generated TPC-H database, `_tpch_oracle.duckdb`).
3. Builds an equivalent ILP in gurobipy via the `oracle_solver` fixture.
4. Solves it.
5. Calls `compare_solutions` which checks (a) that DeciDB's objective
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
│   └── compare.py             # compare_solutions, assert_optimal_match (objective only),
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
    ├── _scope_oracle.py       # Reference evaluator of the per / when / by language (§3.10)
    ├── _scope_cases.py        # The deck's relations and shared helpers of the scope suites
    ├── _brute_force.py        # Enumeration reference for tiny models (§3.10)
    ├── _old_spellings.py      # Scanner behind test_no_old_spellings_remain.py
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

**`gurobi_backend.py`** — Wraps `gurobipy.Model` (gurobipy 12.x). Linear
constraints are added with the `addConstr(tc)` form, since Gurobi 12 has no
expression/sense/rhs overload. Quadratic
objectives build a `QuadExpr`; quadratic constraints go through
`addQConstr`. Indicator constraints use `addGenConstrIndicator` — a
native implication that does not require a hand-picked Big-M.

**`factory.py`** — `get_solver()` returns a `GurobiSolver`. If gurobipy is
not installed or the Gurobi environment cannot start (missing/expired
license), raises `ImportError` with a clear message. HiGHS is not a fallback
on the oracle side; the separate DeciDB CLI-side HiGHS backend documented
in `../pipeline/07_solver/done.md` is unrelated.

### 3.2 Comparison (`comparison/compare.py`)

**`compare_solutions(decidb_rows, decidb_cols, oracle_result, oracle_data, decide_var_names, coeff_fn=None, tolerance=1e-4, decidb_objective_fn=None) -> ComparisonResult`**

Checks both the objective value and the decision-variable vector; every new
correctness test uses it.

- For linear objectives, pass `coeff_fn(row) -> {var_name: coefficient}`. The
  DeciDB objective is computed as `Σ (row[var] × coeff_fn(row)[var])` over
  rows × variables.
- For non-linear objectives (QP/QCQP), pass
  `decidb_objective_fn(rows, cols) -> float` which evaluates the objective
  directly on DeciDB's variable values — e.g. for
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
`POWER(expr, 2) <= 0` (equality-via-quadratic) constraints may need `5e-2`:
the constraint holds only up to Gurobi's feasibility tolerance (~1e-6), which
propagates into the objective, while a real encoding bug differs by orders of
magnitude. `tests/test_quadratic_constraints.py` is the in-place example.

**`assert_optimal_match(...)`** — Objective-only comparison: it does not
compare decision-variable vectors. No test uses it.

**`assert_infeasible(decidb_cli, sql)`** — Asserts that the SQL produces an
`infeasible`/`unbounded` error from DeciDB. Compose it with an independent
gurobipy check (see §3.6) to cross-verify on the oracle side.

### 3.3 Fixtures (`conftest.py`)

| Fixture | Scope | Purpose |
|---------|-------|---------|
| `decidb_db_path` | session | Path to `decidb.db` (TPC-H SF-0.01); skip if missing |
| `decidb_exe_path` | session | Path to `build/release/decidb`; skip if missing |
| `decidb_cli` | session | `DeciDBCli` subprocess wrapper — `execute(sql) -> (rows, cols)`, `assert_error(sql, match=...)`. DeciDB picks its own backend (Gurobi when available). Every run fails its test on an internal error or an internal name in the output, and on an error that carries "is removed" (a removed DECIDE spelling) unless the call passes `removed_spelling=True`, as only `test_scope_removed_syntax.py` does |
| `decidb_cli_gurobi` | session | Same wrapper with `DECIDB_FORCE_SOLVER=gurobi`; skips up front if Gurobi is unavailable on the host |
| `decidb_cli_highs` | session | Same wrapper with `DECIDB_FORCE_SOLVER=highs`, for verifying HiGHS-specific error paths on a host where Gurobi is also linked |
| `_oracle_db_path` | session | Path to `_tpch_oracle.duckdb`; auto-generated on first run via `CALL dbgen(sf=0.01)` against a vanilla duckdb connection |
| `duckdb_conn` | function | Per-test read-only vanilla `duckdb` connection to `_tpch_oracle.duckdb` (no `decidb` Python package involved) |
| `oracle_solver` | function | `CachedOracleSolver` wrapping a `GurobiSolver`; per-test so the cache can disambiguate multiple models per test |
| `perf_tracker` | session | Collects timing; writes JSON + prints table on teardown |

DeciDB and the oracle use two separate databases (`decidb.db` and
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

**GC**: on a full (unfiltered) test run that passes, `save(gc=True)` prunes entries that weren't accessed during the run. Filtered runs (`-k`, `-m`) and failed runs skip GC so unrelated entries survive. A test that skips is not accessed, so its entry is pruned and re-solved on the next run. How xdist workers report to one writer is in [`test/decide/README.md`](../../../test/decide/README.md) § Oracle Cache.

**How it plugs in**: `CachedOracleSolver` wraps the real `OracleSolver` as a drop-in replacement. `create_model()` consults the cache:
- **Hit** — all subsequent model-building calls (`add_variable`, `add_constraint`, `set_objective`, `set_quadratic_objective`, `add_quadratic_constraint`, `add_indicator_constraint`) become no-ops; `solve()` returns the cached `SolverResult` instantly.
- **Miss** — everything is delegated to the real solver; `solve()` stores the result before returning. If no real solver is available and the cache misses, the test is skipped with a clear message.

**Parametric loops**: a single test may call `create_model()` multiple times (e.g. looping over budgets). Each call gets a unique cache key by suffixing `#2`, `#3`, … so earlier solves don't shadow later ones.

**Manual reset**: delete `test/decide/results/oracle_cache.json`. A full rerun will repopulate it.

### 3.5 Shared oracle primitives (`tests/_oracle_helpers.py`)

Tests that encode common DECIDE constructs import helpers from
`tests/_oracle_helpers.py` rather than re-rolling the same Big-M or
indicator patterns in every file. Prefer these over hand-written Big-M
mirrors — see §3.7 on independent semantics. A clause with a `when`, a
`per` key or a `by` key is stated to the scope oracle instead (§3.10).

| Helper | Purpose |
|---|---|
| `group_indices(data, key_fn)` | Group row indices by a key. Rows where `key_fn` returns `None` are in no group, as a NULL written key is (D5). |
| `add_ne_indicator(oracle, coeffs, rhs, name)` | Encode `sum(coeffs) != rhs` over integer-valued terms via a native Gurobi indicator constraint (no Big-M). |
| `add_in_domain(oracle, var_name, domain)` | Restrict `var_name` to a discrete set via SOS1-style indicator encoding (`SUM(z_k) = 1`, `var = SUM(v_k·z_k)`). |
| `add_bool_and(oracle, x, y, z)` | Link binary `z = x ∧ y` with `z ≤ x`, `z ≤ y`, `z ≥ x + y − 1`. Used for Bool × Bool products before summation. |
| `emit_inner_max` / `emit_inner_min` | Per-group auxiliary variable for the *easy* inner-MAX / inner-MIN case of the two-level objective `sum(per col: max(expr) by (col))` / `sum(per col: min(expr) by (col))`. |
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

This guards against DeciDB falsely reporting infeasibility on a feasible
problem (or vice versa). See `tests/test_error_infeasible.py` for the
canonical pattern.

### 3.7 Independent semantics, not Big-M mirrors

For discrete constructs that DeciDB rewrites into Big-M linear programs
(`<>`, IN, etc.), the oracle should encode the **semantics
natively using Gurobi features** — not mirror DeciDB's Big-M reformulation.
Mirroring only detects lockstep bugs (where both implementations agree on
an incorrect rewrite); independent encoding catches DeciDB encoding errors
too.

Concretely:

- **`<>`** — use `add_ne_indicator` (disjunctive branch indicator with native
  implications). Do not mirror DeciDB's `SUM + M·z <= rhs - 1 + M` / `SUM - M·z >= rhs + 1 - M`.
- **IN (…)** — use `add_in_domain` (SOS1 with indicators). This one happens
  to coincide with DeciDB's bind-time rewrite; it's still the right oracle
  shape because it's the canonical formulation, not a mirror.

### 3.8 Performance Tracking (`performance/`)

Each correctness test calls `perf_tracker.record(...)` with:
- DeciDB wall-clock time, oracle build time, oracle solve time
- Problem dimensions (rows, variables, constraints)
- Objective value and solver backend name

On session teardown, the tracker saves a timestamped JSON file to `results/`
and prints a CLI summary table.

### 3.9 Skip audit (`conftest.py`)

Every skip must be declared in advance, in `_DECLARED_SKIPS`, with the reason it
is allowed to vary between runs. On session teardown the controller prints:

```
  DECIDE skip audit: <passed> passed + <skipped> skipped = <outcomes> outcomes
  (compare this total across runs, not the pass count)
```

and fails the run — exit code 1, in serial and under xdist alike — if any skip's
reason matches no declared pattern.

**Why the total, not the pass count.** A structural refactor verifies against the
suite tally, and a run that reports one fewer pass and one more skip looks, at a
glance, exactly like a run that lost a test. The audit prints the sum, which is
stable, and names each skip that made it move.

**What is declared, and what is not.** Environment skips are declared: no usable
Gurobi, no `decidb.db`, no binary, no solver and no oracle cache. One behavioral
skip is declared — `test_quadratic_constraints.py`'s `_expect_gurobi`, where
Gurobi returns SUBOPTIMAL on a non-convex QCQP after three retries. That is real
solver nondeterminism and the audit reports it by name each time it fires.

A skip taken because *DeciDB* refused the query is not declarable. It is a
property of the run rather than of the host, and it makes a test report green
whether the shape is still accepted or was newly refused — an expressivity
regression hiding as a skip. A test whose shape needs a particular backend pins
that backend with `decidb_cli_gurobi` / `decidb_cli_highs` instead, so the skip
decision is made up front, from the environment.

### 3.10 The scope oracle and the brute-force reference

The `per` / `when` / `by` language (`../language/guide.md` §1.1, §2, §4–§7) is judged by
`tests/_scope_oracle.py`, which reads no SQL and builds the model the language defines
through the same `OracleSolver` interface. `tests/_brute_force.py` enumerates the tiny
models the scope oracle cannot state, and `tests/_scope_cases.py` holds the deck's
relations. All three, and the suites that use them, are described in
[scope_suite.md](scope_suite.md).

---

## 4. Running Tests

How to run the suite, select tests by marker and run in parallel is in
[`test/decide/README.md`](../../../test/decide/README.md) § Running Tests.

`run_tests.sh` creates a `.venv/` virtualenv on first run, installs
`duckdb`, `gurobipy`, and `pytest` from `requirements.txt`, pre-flights
gurobipy by starting a Gurobi environment (catches missing/expired
licenses up front), and then invokes pytest. The `decidb` Python package is
not a dependency — DECIDE queries run via the native CLI executable, not
an in-process binding.

---

## 5. Markers

Markers and their descriptions are registered in `test/decide/pytest.ini`; a
correctness test carries its feature markers and the `correctness` meta marker.

---

## 6. Test Reference

The test files in `test/decide/tests/` are the source of truth. The feature areas and
their key files are in [README.md](README.md#feature-areas); what each area's tests
cover is in [coverage.md](coverage.md).

---

## 7. Adding a New Test

### Step 1: Choose the test file

Pick the file that matches the primary feature being tested (§6).
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
        DECIDE x(BOOL)
        SUCH THAT SUM(x * col1) <= 100
        MAXIMIZE SUM(x * col2)
    """
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    # 2. Fetch the same data via vanilla duckdb (independent of DeciDB).
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

For aggregate operators with non-trivial semantics (`per` / `by` groups, `<>`,
bilinear, MIN/MAX hard cases, the two-level objective), import helpers from
`tests/_oracle_helpers.py` rather than re-rolling the logic. See §3.5. A
clause with a `when`, a `per` key or a `by` key is easier to state to
`_scope_oracle.ScopedModel` (§3.10).

For quadratic objectives / constraints, use
`oracle_solver.set_quadratic_objective(linear, quadratic, sense)` and
`oracle_solver.add_quadratic_constraint(linear, quadratic, sense, rhs)`.
Pass `decidb_objective_fn` to `compare_solutions` since DeciDB's objective
cannot be recovered via a linear `coeff_fn`.

### Step 3: Run

```bash
bash run_tests.sh -k "test_my_new_query" -v
```

---

## 8. Known Caveats

1. **Absolute tolerance on large objectives.** Comparison uses `|decidb - oracle| <= 1e-4`.
   For objectives in the millions, a relative tolerance might be more appropriate.
   No failures observed in practice.

2. **Session-scoped stateful solver.** The `oracle_solver` fixture is per test, but
   the gurobipy solver it wraps (`_raw_oracle_solver`) is session-scoped. Each
   test calls `create_model()`, which resets state, but if a test crashes mid-model
   the next test could inherit stale state.

3. **A crash stays in its query.** `decidb_cli` shells out to a fresh subprocess per
   call, so a DeciDB crash or internal error leaves no shared state behind.

4. **DECIDE subquery search path.** Subqueries inside SUCH THAT don't inherit
   the connection's `search_path`. Tables must be fully qualified
   (e.g. `tpch.customer`).

5. **Wall-clock performance numbers.** Timings reflect wall-clock time
   including Python overhead and subprocess startup, not CPU time. They
   are useful for relative comparisons across runs but not for absolute
   benchmarking.

6. **TPC-H scale factor sensitivity.** Tests are tuned for SF-0.01
   (`decidb.db` and `_tpch_oracle.duckdb`). Changing the scale factor may
   make some queries infeasible or shift constraint boundaries. Example:
   q09 uses `s_nationkey <= 5` to ensure 20+ suppliers.

7. **sys.path manipulation in conftest.** `conftest.py` inserts the
   test/decide directory onto `sys.path` so that `solver/`, `comparison/`,
   `performance/`, and the top-level `decidb_cli` and `oracle_cache`
   modules are importable. This is a convenience hack; a proper package
   install would be cleaner.

## 9. Golden model dumps

A characterization oracle over the model DeciDB *builds*, as opposed to the answer it
returns.

### Why it exists

Two different models routinely share an optimum. A refactor can change which rows are
emitted, how a Big-M is scaled, or how an auxiliary is boxed, and every test in the
pytest suite still passes — because every test asks what the answer was. The suite
cannot see the change at all.

This harness makes that visible. `DECIDB_DUMP_MODEL` writes the exact matrix handed to
the backend — columns with their boxes and integrality, rows with senses and
coefficients, general constraints, indicator constraints — for every query in
`corpus.sql`. Diffing that against a committed baseline turns "this refactor changes no
model" from a claim into a check.

### Layout

| File | What it is |
|---|---|
| `corpus.sql` | 112 queries under entries 01–110 (with 21b and 32b), each present for its constraint *shape*, not its answer |
| `capture.sh --solver <gurobi\|highs>` | Writes `baseline.<solver>.dump` and `.results` |
| `check.sh` | Diffs today's model against both baselines; the read-only half |
| `baseline.<solver>.dump` | The committed reference, one per backend |
| `baseline.<solver>.dump.results` | Query output, diffed separately |

The fixture is four rows on purpose: shapes carrying a Big-M encoding stay instant.

### One baseline per backend

The backend is part of the answer, not an ambient fact about the host. Gurobi states
ABS, MIN/MAX and `<>` natively while HiGHS lowers all three, so the two build genuinely
different models from identical SQL — 112 models against 109. Entries 86 and 87 (MIN/MAX
over a range open on one side) reach a model only where MIN/MAX is stated natively, and
entry 33 (a quadratic constraint) needs Gurobi; on HiGHS those three are the refusals
recorded in `baseline.highs.dump.results`.

A baseline with no backend in its name would record whatever backend the capturing
host has, and a diff could not tell drift from signal. `--solver` is required, and `capture.sh`
validates the name itself rather than trusting `DECIDB_FORCE_SOLVER` to complain — an
unrecognized name there falls through to auto-selection by design, which would record
the host default under a name promising otherwise.

### The configuration is pinned too, not just the backend

`DECIDB_NATIVE_CONSTRUCTS` is the A/B switch between stating a construct natively and
lowering it to Big-M. `capture.sh` pins it to `on`, the default, for the same reason it
pins `DECIDB_FORCE_SOLVER`: a baseline records the model DeciDB builds by default, never
one arm of a comparison.

Left ambient, `DECIDB_NATIVE_CONSTRUCTS=force make decide-test` would fail before pytest
runs: the check would capture the forced arm and diff it against the default baseline,
and every dump with a MIN or MAX would report `MODEL CHANGED` (`num_genconstrs` 0 to 1,
the Big-M rows and binaries gone). That is the switch working, not the model moving.
HiGHS declares no native MIN/MAX, so the switch is a no-op there.

Three corpus queries also give different *results* under the switch, and all are tied optima: `MAX(x) >= 3 ...
MINIMIZE SUM(x)` has four equally good answers and `max(per grp: sum(x) by (grp))` has two, so
the two encodings break the tie differently while the objective is unchanged. Pinning
the switch keeps those ties out of the baseline, where they would freeze an arbitrary choice.

### Running it

`make decide-test` runs `check.sh` first and **fails on any difference**, before pytest.
`make decide-golden` runs it alone.

A failure is not automatically a bug; it means the built model moved and the move has to
be read. When it is intended, regenerate and commit the baselines with the change that
caused them:

```bash
test/decide/golden/capture.sh --solver gurobi
test/decide/golden/capture.sh --solver highs
```

A backend missing from the host is skipped — which solvers are installed is a fact about
the machine. A baseline that was never captured is a failure, not a skip: passing
silently on a missing reference would let a baseline go stale unseen.

### What the corpus covers

Per-row and aggregate constraint shapes, canonicalization spellings (side-swapped,
leading-negative, scaled reducers), `when` and `per` in front of a constraint, a
reducer's own `when` and `per` (including a relation key, `sum(per i: keep)`), `by`
keys (including `count(*) by (grp)`, entry 62), keyed and query-wide decisions, the
two-level objective, subquery
right-hand sides, ABS, MIN/MAX in all four hard forms, `<>` including both range
collapses and a bound that varies within a `by` group (entry 94), and the value
rewrite (entries 95–101: deck examples 1, 5 and 8, two helpers with different `by`
keys, a MAX helper pinned on one or both sides, a keyed decision beside a reducer).
Entries 86–88 box auxiliaries over a range open on one side, on both the constraint
and the objective path; without them every MIN/MAX query in the corpus carries a
finite bound, so a model that discarded a derivable bound would diff clean.
