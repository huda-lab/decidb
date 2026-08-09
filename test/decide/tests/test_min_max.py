"""Tests for MIN/MAX aggregate linearization over decision variables.

Covers:
  Easy constraint cases:
  - test_max_leq_constraint: MAX(x) <= K → per-row x <= K
  - test_min_geq_constraint: MIN(x) >= K → per-row x >= K
  - test_max_leq_with_expr: MAX(x * col) <= K

  Hard constraint cases:
  - test_max_geq_constraint: MAX(x) >= K → indicator + Big-M
  - test_min_leq_constraint: MIN(x) <= K → indicator + Big-M
  - test_max_eq_constraint: MAX(x) = K → easy + hard combined
  - test_min_eq_constraint: MIN(x) = K → easy + hard combined

  Easy objective cases:
  - test_minimize_max_objective: MINIMIZE MAX(x * cost)
  - test_maximize_min_objective: MAXIMIZE MIN(x * cost)

  Hard objective cases:
  - test_maximize_max_objective: MAXIMIZE MAX(x * cost)
  - test_minimize_min_objective: MINIMIZE MIN(x * cost)

  Modifiers:
  - test_max_constraint_with_when: MAX(x) <= K WHEN condition
  - test_min_objective_with_when: MAXIMIZE MIN(x * cost) WHEN condition
  - test_max_constraint_with_per: MAX(x) <= K PER column
  - test_min_max_when_per_composition: WHEN + PER combined

  INTEGER variable tests:
  - test_max_leq_integer: MAX(x) <= K with INTEGER variables
  - test_minimize_max_integer: MINIMIZE MAX(x * col) with INTEGER variables

  Combined tests:
  - test_multiple_minmax_constraints: Multiple MIN/MAX constraints in same query
  - test_minmax_constraint_and_objective: MIN/MAX in both constraint and objective

  Error cases:
  - test_max_notequal_error: MAX(x) <> K should error
"""

import time

import pytest

from solver.types import VarType, ObjSense, SolverStatus
from comparison.compare import compare_solutions
from ._oracle_helpers import emit_inner_max, emit_inner_min


# ============================================================================
# Easy Constraint Cases
# ============================================================================

@pytest.mark.min_max
@pytest.mark.var_boolean
@pytest.mark.cons_perrow
@pytest.mark.correctness
def test_max_leq_constraint(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """MAX(x) <= 0 forces all x=0; MAX(x) <= 1 is trivially satisfied for BOOLEAN."""
    # MAX(x) <= 0 → each row: x <= 0 → all x = 0
    sql = """
        SELECT l_orderkey, l_linenumber, x
        FROM lineitem
        WHERE l_orderkey <= 5
        DECIDE x(BOOL)
        SUCH THAT MAX(x) <= 0
        MAXIMIZE SUM(x)
    """
    decidb_result, decidb_cols = decidb_cli.execute(sql)

    # All x should be 0
    ci = {name: i for i, name in enumerate(decidb_cols)}
    for row in decidb_result:
        assert int(row[ci["x"]]) == 0, f"Expected x=0, got {row[ci['x']]}"


@pytest.mark.min_max
@pytest.mark.var_boolean
@pytest.mark.cons_perrow
@pytest.mark.correctness
def test_min_geq_constraint(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """MIN(x) >= 1 forces all x=1 for BOOLEAN."""
    sql = """
        SELECT l_orderkey, l_linenumber, x
        FROM lineitem
        WHERE l_orderkey <= 5
        DECIDE x(BOOL)
        SUCH THAT MIN(x) >= 1
        MAXIMIZE SUM(x)
    """
    decidb_result, decidb_cols = decidb_cli.execute(sql)

    ci = {name: i for i, name in enumerate(decidb_cols)}
    for row in decidb_result:
        assert int(row[ci["x"]]) == 1, f"Expected x=1, got {row[ci['x']]}"


@pytest.mark.min_max
@pytest.mark.var_boolean
@pytest.mark.cons_perrow
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_max_leq_with_expr(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """MAX(x * l_quantity) <= threshold: per-row x * l_quantity <= threshold."""
    threshold = 25.0
    sql = f"""
        SELECT l_orderkey, l_linenumber, l_quantity, x
        FROM lineitem
        WHERE l_orderkey <= 5
        DECIDE x(BOOL)
        SUCH THAT MAX(x * l_quantity) <= {threshold}
        MAXIMIZE SUM(x)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_quantity AS DOUBLE)
        FROM lineitem WHERE l_orderkey <= 5
    """).fetchall()

    # Oracle: per-row x_i * qty_i <= threshold
    n = len(data)
    t_build = time.perf_counter()
    oracle_solver.create_model("max_leq_expr")
    vnames = [f"x_{i}" for i in range(n)]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    for i in range(n):
        qty = data[i][2]
        oracle_solver.add_constraint(
            {vnames[i]: qty}, "<=", threshold, name=f"max_row_{i}",
        )
    oracle_solver.set_objective(
        {vnames[i]: 1.0 for i in range(n)}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    assert result.status == SolverStatus.OPTIMAL
    # Count selected items in DecidB result
    ci = {name: i for i, name in enumerate(decidb_cols)}
    decidb_count = sum(1 for row in decidb_result if int(row[ci["x"]]) == 1)
    assert abs(decidb_count - result.objective_value) <= 0.5, (
        f"Objective mismatch: DecidB selected {decidb_count}, Oracle={result.objective_value:.0f}"
    )

    perf_tracker.record(
        "max_leq_expr", decidb_time, build_time,
        result.solve_time_seconds, n, n, n,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )


# ============================================================================
# Hard Constraint Cases
# ============================================================================

@pytest.mark.min_max
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_max_geq_constraint(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """MAX(x) >= 1 means at least one x must be 1. MINIMIZE SUM(x)."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, x
        FROM lineitem
        WHERE l_orderkey <= 5
        DECIDE x(BOOL)
        SUCH THAT MAX(x) >= 1
        MINIMIZE SUM(x)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    # Oracle: SUM(x) >= 1, MINIMIZE SUM(x) → exactly 1 selected
    ci = {name: i for i, name in enumerate(decidb_cols)}
    decidb_count = sum(1 for row in decidb_result if int(row[ci["x"]]) == 1)
    assert decidb_count == 1, f"Expected exactly 1 selected, got {decidb_count}"


@pytest.mark.min_max
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_min_leq_constraint(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """MIN(x) <= 0 means at least one x must be 0. MAXIMIZE SUM(x)."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, x
        FROM lineitem
        WHERE l_orderkey <= 5
        DECIDE x(BOOL)
        SUCH THAT MIN(x) <= 0
        MAXIMIZE SUM(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    ci = {name: i for i, name in enumerate(decidb_cols)}
    zero_count = sum(1 for row in decidb_result if int(row[ci["x"]]) == 0)
    assert zero_count >= 1, "MIN(x) <= 0 requires at least one x=0"


@pytest.mark.min_max
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_max_eq_constraint(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """MAX(x) = 1 means at least one x=1 AND all x<=1 (trivial for BOOLEAN)."""
    sql = """
        SELECT l_orderkey, l_linenumber, x
        FROM lineitem
        WHERE l_orderkey <= 5
        DECIDE x(BOOL)
        SUCH THAT MAX(x) = 1
        MINIMIZE SUM(x)
    """
    decidb_result, decidb_cols = decidb_cli.execute(sql)

    ci = {name: i for i, name in enumerate(decidb_cols)}
    selected = sum(1 for row in decidb_result if int(row[ci["x"]]) == 1)
    assert selected >= 1, "MAX(x) = 1 requires at least one x=1"
    # MAX(x) = 1 with MINIMIZE SUM(x) → exactly one selected
    assert selected == 1, f"Expected 1 selected (minimize), got {selected}"


@pytest.mark.min_max
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_min_eq_constraint(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """MIN(x) = 0 means at least one x=0 AND all x>=0 (trivial for BOOLEAN)."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, x
        FROM lineitem
        WHERE l_orderkey <= 5
        DECIDE x(BOOL)
        SUCH THAT MIN(x) = 0
        MAXIMIZE SUM(x * l_extendedprice)
    """
    decidb_result, decidb_cols = decidb_cli.execute(sql)

    ci = {name: i for i, name in enumerate(decidb_cols)}
    zeros = sum(1 for row in decidb_result if int(row[ci["x"]]) == 0)
    assert zeros >= 1, "MIN(x) = 0 requires at least one x=0"


# ============================================================================
# Easy Objective Cases
# ============================================================================

@pytest.mark.min_max
@pytest.mark.var_boolean
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_minimize_max_objective(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """MINIMIZE MAX(x * l_quantity): minimize worst-case quantity."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_quantity, x
        FROM lineitem
        WHERE l_orderkey <= 5
        DECIDE x(BOOL)
        SUCH THAT SUM(x) >= 2
        MINIMIZE MAX(x * l_quantity)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_quantity AS DOUBLE)
        FROM lineitem WHERE l_orderkey <= 5
    """).fetchall()

    # Oracle: minimize z, z >= x_i * qty_i for all i, SUM(x) >= 2
    n = len(data)
    t_build = time.perf_counter()
    oracle_solver.create_model("minimize_max")
    vnames = [f"x_{i}" for i in range(n)]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)
    oracle_solver.add_variable("z", VarType.CONTINUOUS, lb=0.0)

    # SUM(x) >= 2
    oracle_solver.add_constraint(
        {vn: 1.0 for vn in vnames}, ">=", 2.0, name="sum_ge_2",
    )
    # z >= x_i * qty_i for each row
    for i in range(n):
        qty = data[i][2]
        oracle_solver.add_constraint(
            {"z": 1.0, vnames[i]: -qty}, ">=", 0.0, name=f"z_ge_{i}",
        )
    oracle_solver.set_objective({"z": 1.0}, ObjSense.MINIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    assert result.status == SolverStatus.OPTIMAL

    # Compute DecidB's MAX(x * qty)
    ci = {name: i for i, name in enumerate(decidb_cols)}
    decidb_max = max(
        int(row[ci["x"]]) * float(row[ci["l_quantity"]])
        for row in decidb_result
    )
    assert abs(decidb_max - result.objective_value) <= 0.5, (
        f"Objective mismatch: DecidB MAX={decidb_max:.2f}, Oracle={result.objective_value:.2f}"
    )

    perf_tracker.record(
        "minimize_max", decidb_time, build_time,
        result.solve_time_seconds, n, n + 1, n + 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )


@pytest.mark.min_max
@pytest.mark.var_boolean
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_maximize_min_objective(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """MAXIMIZE MIN(x * l_quantity): maximize worst-case, all must be selected."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_quantity, x
        FROM lineitem
        WHERE l_orderkey <= 5
        DECIDE x(BOOL)
        SUCH THAT SUM(x) >= 2
        MAXIMIZE MIN(x * l_quantity)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_quantity AS DOUBLE)
        FROM lineitem WHERE l_orderkey <= 5
    """).fetchall()

    # Oracle: maximize z, z <= x_i * qty_i for all i, SUM(x) >= 2
    n = len(data)
    t_build = time.perf_counter()
    oracle_solver.create_model("maximize_min")
    vnames = [f"x_{i}" for i in range(n)]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)
    oracle_solver.add_variable("z", VarType.CONTINUOUS, lb=0.0)

    oracle_solver.add_constraint(
        {vn: 1.0 for vn in vnames}, ">=", 2.0, name="sum_ge_2",
    )
    for i in range(n):
        qty = data[i][2]
        oracle_solver.add_constraint(
            {"z": 1.0, vnames[i]: -qty}, "<=", 0.0, name=f"z_le_{i}",
        )
    oracle_solver.set_objective({"z": 1.0}, ObjSense.MAXIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    assert result.status == SolverStatus.OPTIMAL

    ci = {name: i for i, name in enumerate(decidb_cols)}
    decidb_min = min(
        int(row[ci["x"]]) * float(row[ci["l_quantity"]])
        for row in decidb_result
    )
    assert abs(decidb_min - result.objective_value) <= 0.5, (
        f"Objective mismatch: DecidB MIN={decidb_min:.2f}, Oracle={result.objective_value:.2f}"
    )

    perf_tracker.record(
        "maximize_min", decidb_time, build_time,
        result.solve_time_seconds, n, n + 1, n + 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )


# ============================================================================
# Hard Objective Cases
# ============================================================================

@pytest.mark.min_max
@pytest.mark.var_boolean
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_maximize_max_objective(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """MAXIMIZE MAX(x * l_extendedprice) with SUM(x) <= 3."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, x
        FROM lineitem
        WHERE l_orderkey <= 5
        DECIDE x(BOOL)
        SUCH THAT SUM(x) <= 3
        MAXIMIZE MAX(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_extendedprice AS DOUBLE)
        FROM lineitem WHERE l_orderkey <= 5
    """).fetchall()

    # Oracle: maximize z, z <= x_i * price_i + M*(1-y_i), SUM(y)>=1, SUM(x)<=3
    # Or equivalently: just maximize the max selected price
    n = len(data)
    t_build = time.perf_counter()
    oracle_solver.create_model("maximize_max")
    vnames = [f"x_{i}" for i in range(n)]
    ynames = [f"y_{i}" for i in range(n)]
    M = max(d[2] for d in data) * 2  # Big enough M

    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)
    for yn in ynames:
        oracle_solver.add_variable(yn, VarType.BINARY)
    oracle_solver.add_variable("z", VarType.CONTINUOUS, lb=0.0)

    oracle_solver.add_constraint(
        {vn: 1.0 for vn in vnames}, "<=", 3.0, name="sum_le_3",
    )
    oracle_solver.add_constraint(
        {yn: 1.0 for yn in ynames}, ">=", 1.0, name="sum_y_ge_1",
    )
    for i in range(n):
        price = data[i][2]
        # z <= x_i * price + M*(1-y_i) → z - price*x_i + M*y_i <= M
        oracle_solver.add_constraint(
            {"z": 1.0, vnames[i]: -price, ynames[i]: M}, "<=", M,
            name=f"link_{i}",
        )
    oracle_solver.set_objective({"z": 1.0}, ObjSense.MAXIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    assert result.status == SolverStatus.OPTIMAL

    ci = {name: i for i, name in enumerate(decidb_cols)}
    decidb_max = max(
        int(row[ci["x"]]) * float(row[ci["l_extendedprice"]])
        for row in decidb_result
    )
    assert abs(decidb_max - result.objective_value) <= 1.0, (
        f"Objective mismatch: DecidB MAX={decidb_max:.2f}, Oracle={result.objective_value:.2f}"
    )

    perf_tracker.record(
        "maximize_max", decidb_time, build_time,
        result.solve_time_seconds, n, n * 2 + 1, n + 2,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )


# ============================================================================
# WHEN Modifier
# ============================================================================

@pytest.mark.min_max
@pytest.mark.when_constraint
@pytest.mark.var_boolean
@pytest.mark.correctness
def test_max_constraint_with_when(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """MAX(x) <= 0 WHEN l_quantity > 30: only rows with qty>30 must have x=0."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_quantity, x
        FROM lineitem
        WHERE l_orderkey <= 5
        DECIDE x(BOOL)
        SUCH THAT MAX(x) <= 0 WHEN l_quantity > 30
        MAXIMIZE SUM(x)
    """
    decidb_result, decidb_cols = decidb_cli.execute(sql)

    ci = {name: i for i, name in enumerate(decidb_cols)}
    for row in decidb_result:
        qty = float(row[ci["l_quantity"]])
        x_val = int(row[ci["x"]])
        if qty > 30:
            assert x_val == 0, f"Row with qty={qty} should have x=0, got {x_val}"


# ============================================================================
# Error Cases
# ============================================================================

@pytest.mark.min_max
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_maximize_max_mixed_sign_coefficient(decidb_cli):
    """MAXIMIZE MAX(c*x) where c flips sign across rows — exercises the objective
    Big-M GLOBAL spread, the gap a per-row magnitude would miss.

    Row 1 has c=+1, row 2 has c=-1 with x pinned to 10. The expression reaches
    +10 (row 1, x=10) and -10 (row 2), so the auxiliary-linking Big-M must cover
    a spread of 20; a value derived from a single row's range (10) would wrongly
    cap the optimum. True optimum: MAX(c*x) = 10 (set x=10 on the +1 row).
    """
    sql = """
        WITH data AS (
            SELECT 1 AS id, 1.0 AS c UNION ALL
            SELECT 2 AS id, -1.0 AS c
        )
        SELECT id, c, x
        FROM data
        DECIDE x(INT)
        SUCH THAT x <= 10 AND x >= 10 WHEN c < 0
        MAXIMIZE MAX(c * x)
    """
    rows, cols = decidb_cli.execute(sql)
    ci = {name: i for i, name in enumerate(cols)}
    obj = max(float(r[ci["c"]]) * float(r[ci["x"]]) for r in rows)
    assert abs(obj - 10.0) <= 1e-6, f"expected MAX(c*x)=10, got {obj}"


@pytest.mark.min_max
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_maximize_max_max_per_mixed_sign(decidb_cli):
    """Nested MAXIMIZE MAX(MAX(c*x)) PER g with c flipping sign across rows —
    drives the nested inner + outer hard Big-M paths (not just the flat path)
    with a mixed-sign expression, exercising the objective global-spread M.
    c*x reaches +10 and -10, so the linking M must cover a spread of 20.
    Optimum: MAX over all rows of c*x = 10 (set x=10 on a +1 row)."""
    sql = """
        WITH data AS (
            SELECT 'A' AS g, 1.0 AS c UNION ALL
            SELECT 'A', -1.0 UNION ALL
            SELECT 'B', 1.0 UNION ALL
            SELECT 'B', -1.0
        )
        SELECT g, c, x
        FROM data
        DECIDE x(INT)
        SUCH THAT x <= 10 AND x >= 10 WHEN c < 0
        MAXIMIZE MAX(MAX(c * x)) PER g
    """
    rows, cols = decidb_cli.execute(sql)
    ci = {name: i for i, name in enumerate(cols)}
    obj = max(float(r[ci["c"]]) * float(r[ci["x"]]) for r in rows)
    assert abs(obj - 10.0) <= 1e-6, f"expected MAX(MAX(c*x))=10, got {obj}"


@pytest.mark.min_max
@pytest.mark.var_boolean
@pytest.mark.obj_maximize
@pytest.mark.when_objective
@pytest.mark.correctness
def test_min_objective_with_when(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """MAXIMIZE MIN(x * l_quantity) WHEN l_quantity <= 30 — easy-case flat MIN objective.

    Per CLAUDE.md MIN/MAX rules: MAXIMIZE MIN is an easy case — introduce a
    global `z` with ``z <= expr_i`` for each WHEN-qualifying row, then
    MAXIMIZE z (no indicators, no Big-M). This test oracle-verifies that
    DecidB's flat-MIN-with-WHEN formulation is correctly routed to the easy
    linearization.
    """
    sql = """
        SELECT l_orderkey, l_linenumber, l_quantity, x
        FROM lineitem
        WHERE l_orderkey <= 5
        DECIDE x(BOOL)
        SUCH THAT SUM(x) >= 2
        MAXIMIZE MIN(x * l_quantity) WHEN l_quantity <= 30
    """
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_quantity AS DOUBLE)
        FROM lineitem WHERE l_orderkey <= 5
    """).fetchall()
    n = len(data)

    t_build = time.perf_counter()
    oracle_solver.create_model("min_objective_with_when")
    vnames = [f"x_{i}" for i in range(n)]
    for v in vnames:
        oracle_solver.add_variable(v, VarType.BINARY)

    # SUM(x) >= 2 over all rows (WHEN applies only to the MIN objective).
    oracle_solver.add_constraint(
        {v: 1.0 for v in vnames}, ">=", 2.0, name="sum_ge_2",
    )

    # MAXIMIZE MIN over WHEN-qualifying rows: easy case, global z with
    # z <= l_quantity_i * x_i for each i where l_quantity_i <= 30.
    qualifying_indices = [i for i, row in enumerate(data) if row[2] <= 30.0]
    assert qualifying_indices, "design-time: at least one row must qualify"
    row_coeffs = [
        {vnames[i]: float(data[i][2])} for i in qualifying_indices
    ]
    row_ub = max(float(data[i][2]) for i in qualifying_indices)
    z_name = emit_inner_min(
        oracle_solver, "obj", row_coeffs, row_ub,
    )
    oracle_solver.set_objective({z_name: 1.0}, ObjSense.MAXIMIZE)

    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL

    # MIN(x * l_quantity) over WHEN-qualifying rows gives DecidB's objective.
    def _decidb_min(rows, cols):
        x_idx = cols.index("x")
        q_idx = cols.index("l_quantity")
        vals = [
            int(r[x_idx]) * float(r[q_idx])
            for r in rows
            if float(r[q_idx]) <= 30.0
        ]
        return float(min(vals)) if vals else 0.0

    cmp = compare_solutions(
        decidb_rows, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": 0.0},
        decidb_objective_fn=_decidb_min,
    )
    perf_tracker.record(
        "min_objective_with_when", decidb_time, build_time,
        result.solve_time_seconds, n, n, 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.min_max
@pytest.mark.var_boolean
@pytest.mark.obj_minimize
@pytest.mark.when_objective
@pytest.mark.correctness
def test_max_objective_with_when(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """MINIMIZE MAX(x * l_quantity) WHEN l_quantity <= 30 — easy-case flat MAX objective.

    Mirror of ``test_min_objective_with_when``. Per CLAUDE.md, MINIMIZE MAX is
    an easy case — introduce a global ``z`` with ``z >= expr_i`` for each
    WHEN-qualifying row, then MINIMIZE z (no indicators, no Big-M). Confirms
    DecidB's flat-MAX-with-WHEN formulation is routed to the easy
    linearization.
    """
    sql = """
        SELECT l_orderkey, l_linenumber, l_quantity, x
        FROM lineitem
        WHERE l_orderkey <= 5
        DECIDE x(BOOL)
        SUCH THAT SUM(x) >= 2
        MINIMIZE MAX(x * l_quantity) WHEN l_quantity <= 30
    """
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_quantity AS DOUBLE)
        FROM lineitem WHERE l_orderkey <= 5
    """).fetchall()
    n = len(data)

    t_build = time.perf_counter()
    oracle_solver.create_model("max_objective_with_when")
    vnames = [f"x_{i}" for i in range(n)]
    for v in vnames:
        oracle_solver.add_variable(v, VarType.BINARY)

    oracle_solver.add_constraint(
        {v: 1.0 for v in vnames}, ">=", 2.0, name="sum_ge_2",
    )

    qualifying_indices = [i for i, row in enumerate(data) if row[2] <= 30.0]
    assert qualifying_indices, "design-time: at least one row must qualify"
    row_coeffs = [
        {vnames[i]: float(data[i][2])} for i in qualifying_indices
    ]
    row_ub = max(float(data[i][2]) for i in qualifying_indices)
    z_name = emit_inner_max(
        oracle_solver, "obj", row_coeffs, row_ub,
    )
    oracle_solver.set_objective({z_name: 1.0}, ObjSense.MINIMIZE)

    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL

    def _decidb_max(rows, cols):
        x_idx = cols.index("x")
        q_idx = cols.index("l_quantity")
        vals = [
            int(r[x_idx]) * float(r[q_idx])
            for r in rows
            if float(r[q_idx]) <= 30.0
        ]
        return float(max(vals)) if vals else 0.0

    cmp = compare_solutions(
        decidb_rows, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": 0.0},
        decidb_objective_fn=_decidb_max,
    )
    perf_tracker.record(
        "max_objective_with_when", decidb_time, build_time,
        result.solve_time_seconds, n, n, 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


# ============================================================================
# Hard Objective Cases (continued)
# ============================================================================

@pytest.mark.min_max
@pytest.mark.var_integer
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_minimize_min_objective(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """MINIMIZE MIN(x) with INTEGER vars, x >= 1, SUM(x) >= 10: hard objective, spread values low."""
    sql = """
        SELECT l_orderkey, l_linenumber, x
        FROM lineitem
        WHERE l_orderkey <= 3
        DECIDE x(INT)
        SUCH THAT x >= 1 AND
              x <= 5 AND
              SUM(x) >= 10
        MINIMIZE MIN(x)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT)
        FROM lineitem WHERE l_orderkey <= 3
    """).fetchall()

    # Oracle: minimize z, z >= x_i - M*(1-y_i), SUM(y) >= 1, x_i in [1,5], SUM(x) >= 10
    n = len(data)
    M = 10  # upper bound - lower bound
    t_build = time.perf_counter()
    oracle_solver.create_model("minimize_min")
    vnames = [f"x_{i}" for i in range(n)]
    ynames = [f"y_{i}" for i in range(n)]

    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.INTEGER, lb=1, ub=5)
    for yn in ynames:
        oracle_solver.add_variable(yn, VarType.BINARY)
    oracle_solver.add_variable("z", VarType.CONTINUOUS, lb=1.0, ub=5.0)

    oracle_solver.add_constraint(
        {vn: 1.0 for vn in vnames}, ">=", 10.0, name="sum_ge_10",
    )
    oracle_solver.add_constraint(
        {yn: 1.0 for yn in ynames}, ">=", 1.0, name="sum_y_ge_1",
    )
    for i in range(n):
        # z >= x_i - M*(1-y_i) → z - x_i - M*y_i >= -M
        oracle_solver.add_constraint(
            {"z": 1.0, vnames[i]: -1.0, ynames[i]: -M}, ">=", -M,
            name=f"link_{i}",
        )
    oracle_solver.set_objective({"z": 1.0}, ObjSense.MINIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    assert result.status == SolverStatus.OPTIMAL

    ci = {name: i for i, name in enumerate(decidb_cols)}
    decidb_min_val = min(int(row[ci["x"]]) for row in decidb_result)
    assert abs(decidb_min_val - result.objective_value) <= 0.5, (
        f"Objective mismatch: DecidB MIN={decidb_min_val}, Oracle={result.objective_value:.0f}"
    )

    perf_tracker.record(
        "minimize_min", decidb_time, build_time,
        result.solve_time_seconds, n, n * 2 + 1, n + 2,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )


# ============================================================================
# PER Modifier
# ============================================================================

@pytest.mark.min_max
@pytest.mark.per_clause
@pytest.mark.var_boolean
@pytest.mark.correctness
def test_max_constraint_with_per(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """MAX(x) <= 0 PER l_orderkey: within each order, no item selected.
    Easy case with PER — PER is stripped (redundant since per-row already)."""
    sql = """
        SELECT l_orderkey, l_linenumber, x
        FROM lineitem
        WHERE l_orderkey <= 5
        DECIDE x(BOOL)
        SUCH THAT MAX(x) <= 0 PER l_orderkey
        MAXIMIZE SUM(x)
    """
    decidb_result, decidb_cols = decidb_cli.execute(sql)

    # MAX(x) <= 0 per group → all x = 0 (same as without PER for easy case)
    ci = {name: i for i, name in enumerate(decidb_cols)}
    for row in decidb_result:
        assert int(row[ci["x"]]) == 0, f"Expected x=0, got {row[ci['x']]}"


# ============================================================================
# WHEN + PER Composition
# ============================================================================

@pytest.mark.min_max
@pytest.mark.when_constraint
@pytest.mark.per_clause
@pytest.mark.var_boolean
@pytest.mark.correctness
def test_min_max_when_per_composition(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """MAX(x) <= 0 WHEN l_quantity > 40 PER l_orderkey: zero out high-qty items per order."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_quantity, x
        FROM lineitem
        WHERE l_orderkey <= 10
        DECIDE x(BOOL)
        SUCH THAT MAX(x) <= 0 WHEN l_quantity > 40 PER l_orderkey
        MAXIMIZE SUM(x)
    """
    decidb_result, decidb_cols = decidb_cli.execute(sql)

    ci = {name: i for i, name in enumerate(decidb_cols)}
    for row in decidb_result:
        qty = float(row[ci["l_quantity"]])
        x_val = int(row[ci["x"]])
        if qty > 40:
            assert x_val == 0, f"Row with qty={qty} should have x=0 (WHEN filter), got {x_val}"


# ============================================================================
# INTEGER Variable Tests
# ============================================================================

@pytest.mark.min_max
@pytest.mark.var_integer
@pytest.mark.cons_perrow
@pytest.mark.correctness
def test_max_leq_integer(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """MAX(x) <= 3 with INTEGER variables: each x is bounded to [0, 3]."""
    sql = """
        SELECT l_orderkey, l_linenumber, x
        FROM lineitem
        WHERE l_orderkey <= 3
        DECIDE x(INT)
        SUCH THAT MAX(x) <= 3
        MAXIMIZE SUM(x)
    """
    decidb_result, decidb_cols = decidb_cli.execute(sql)

    ci = {name: i for i, name in enumerate(decidb_cols)}
    for row in decidb_result:
        x_val = int(row[ci["x"]])
        assert x_val <= 3, f"Expected x<=3, got {x_val}"
        # With MAXIMIZE SUM(x) and MAX(x) <= 3, each x should be 3
        assert x_val == 3, f"Expected x=3 (maximize), got {x_val}"


@pytest.mark.min_max
@pytest.mark.var_integer
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_minimize_max_integer(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """MINIMIZE MAX(x) with INTEGER: minimize the largest x value, with sum constraint."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_quantity, x
        FROM lineitem
        WHERE l_orderkey <= 3
        DECIDE x(INT)
        SUCH THAT SUM(x) >= 10 AND
              x <= 5
        MINIMIZE MAX(x)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_quantity AS DOUBLE)
        FROM lineitem WHERE l_orderkey <= 3
    """).fetchall()

    # Oracle: minimize z, z >= x_i for all i, SUM(x) >= 10, x_i <= 5
    n = len(data)
    t_build = time.perf_counter()
    oracle_solver.create_model("minimize_max_int")
    vnames = [f"x_{i}" for i in range(n)]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.INTEGER, lb=0, ub=5)
    oracle_solver.add_variable("z", VarType.CONTINUOUS, lb=0.0)

    oracle_solver.add_constraint(
        {vn: 1.0 for vn in vnames}, ">=", 10.0, name="sum_ge_10",
    )
    for i in range(n):
        oracle_solver.add_constraint(
            {"z": 1.0, vnames[i]: -1.0}, ">=", 0.0, name=f"z_ge_{i}",
        )
    oracle_solver.set_objective({"z": 1.0}, ObjSense.MINIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    assert result.status == SolverStatus.OPTIMAL

    ci = {name: i for i, name in enumerate(decidb_cols)}
    decidb_max = max(int(row[ci["x"]]) for row in decidb_result)
    assert abs(decidb_max - result.objective_value) <= 0.5, (
        f"Objective mismatch: DecidB MAX={decidb_max}, Oracle={result.objective_value:.0f}"
    )

    perf_tracker.record(
        "minimize_max_int", decidb_time, build_time,
        result.solve_time_seconds, n, n + 1, n + 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )


# ============================================================================
# Combined MIN/MAX Tests
# ============================================================================

@pytest.mark.min_max
@pytest.mark.var_boolean
@pytest.mark.cons_multi
@pytest.mark.correctness
def test_multiple_minmax_constraints(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Multiple MIN/MAX constraints in same query: MAX(x) <= 0 WHEN ... AND MIN(x) >= 1 WHEN ..."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_quantity, x
        FROM lineitem
        WHERE l_orderkey <= 5
        DECIDE x(BOOL)
        SUCH THAT MAX(x) <= 0 WHEN l_quantity > 40 AND
              MIN(x) >= 1 WHEN l_quantity < 5
        MAXIMIZE SUM(x)
    """
    decidb_result, decidb_cols = decidb_cli.execute(sql)

    ci = {name: i for i, name in enumerate(decidb_cols)}
    for row in decidb_result:
        qty = float(row[ci["l_quantity"]])
        x_val = int(row[ci["x"]])
        if qty > 40:
            assert x_val == 0, f"qty={qty}>40 should force x=0, got {x_val}"
        if qty < 5:
            assert x_val == 1, f"qty={qty}<5 should force x=1, got {x_val}"


@pytest.mark.min_max
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_minmax_constraint_and_objective(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """MIN/MAX in both constraint and objective: MAX(x) >= 1 with MINIMIZE MAX(x * price)."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, x
        FROM lineitem
        WHERE l_orderkey <= 5
        DECIDE x(BOOL)
        SUCH THAT MAX(x) >= 1
        MINIMIZE MAX(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    ci = {name: i for i, name in enumerate(decidb_cols)}
    selected = [row for row in decidb_result if int(row[ci["x"]]) == 1]
    assert len(selected) >= 1, "MAX(x) >= 1 requires at least one x=1"

    # With MINIMIZE MAX(x*price) and MAX(x)>=1, should select 1 item with lowest price
    if len(selected) == 1:
        sel_price = float(selected[0][ci["l_extendedprice"]])
        all_prices = [float(row[ci["l_extendedprice"]]) for row in decidb_result]
        min_price = min(all_prices)
        assert abs(sel_price - min_price) <= 0.01, (
            f"Should select cheapest item: got {sel_price}, min is {min_price}"
        )


# ============================================================================
# Error Cases
# ============================================================================

@pytest.mark.min_max
@pytest.mark.error
@pytest.mark.error_binder
def test_max_notequal_error(decidb_cli):
    """MAX(x) <> K should produce a binder error."""
    decidb_cli.assert_error("""
        SELECT l_quantity FROM lineitem
        DECIDE x(BOOL)
        SUCH THAT MAX(x) <> 0
        MAXIMIZE SUM(x) LIMIT 1
    """, match=r"does not support <> comparison with MIN/MAX")


# ============================================================================
# Composed MIN/MAX in additive expressions (v1: easy-direction terms only).
# The composed LHS `T1 + T2 + ... <= K` (or >= K) is emitted as a global raw
# ILP constraint with a continuous z_k auxiliary per MIN/MAX term. Each
# auxiliary is pinned by per-row constraints to the row-wise inner value.
# ============================================================================


@pytest.mark.min_max
@pytest.mark.when_constraint
@pytest.mark.correctness
def test_sum_plus_max_leq_composed(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """SUM(x*v) + MAX(x*v) WHEN w <= K — minimum repro for the composed MIN/MAX bug.

    LHS is additive with one SUM and one MAX term. The MAX has a WHEN filter.
    Easy-direction: pushed down by <=, MAX pushed down ⇒ easy.
    """
    sql = """
        SELECT id, v, w, x FROM (
            VALUES (1, 10.0, false),
                   (2, 5.0, true),
                   (3, 7.0, false)
        ) t(id, v, w)
        DECIDE x(BOOL)
        SUCH THAT SUM(x * v) + MAX(x * v) WHEN w <= 12
        MAXIMIZE SUM(x * v)
    """
    t0 = time.perf_counter()
    rows, cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = [(1, 10.0, False), (2, 5.0, True), (3, 7.0, False)]
    n = len(data)
    # Build oracle with explicit z auxiliary for the MAX term.
    t_build = time.perf_counter()
    oracle_solver.create_model("composed_sum_plus_max")
    vnames = [f"x_{i}" for i in range(n)]
    for v in vnames:
        oracle_solver.add_variable(v, VarType.BINARY)
    # z = MAX(x*v) WHEN w. Easy case (pushed down): z >= x*v for each w-matching row.
    z_ub = max(d[1] for d in data if d[2]) + 1.0
    oracle_solver.add_variable("z_max", VarType.CONTINUOUS, lb=0.0, ub=z_ub)
    for i, d in enumerate(data):
        if d[2]:
            oracle_solver.add_constraint(
                {"z_max": 1.0, vnames[i]: -d[1]}, ">=", 0.0, name=f"z_pin_{i}",
            )
    # Outer: SUM(x*v) + z <= 12
    outer_coeffs = {vnames[i]: data[i][1] for i in range(n)}
    outer_coeffs["z_max"] = 1.0
    oracle_solver.add_constraint(outer_coeffs, "<=", 12.0, name="outer")
    oracle_solver.set_objective({vnames[i]: data[i][1] for i in range(n)}, ObjSense.MAXIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        rows, cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[cols.index("v")])},
    )
    perf_tracker.record(
        "composed_sum_plus_max", decidb_time, build_time, result.solve_time_seconds,
        n, n + 1, n + 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.min_max
@pytest.mark.when_constraint
@pytest.mark.correctness
def test_max_plus_max_leq_composed(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """MAX(x*v) WHEN w1 + MAX(x*v) WHEN w2 <= K — two easy-direction MAX terms."""
    sql = """
        SELECT id, v, x FROM (
            VALUES (1, 10.0, true, false),
                   (2, 5.0, false, true),
                   (3, 7.0, false, false)
        ) t(id, v, w1, w2)
        DECIDE x(BOOL)
        SUCH THAT MAX(x * v) WHEN w1 + MAX(x * v) WHEN w2 <= 12
        MAXIMIZE SUM(x * v)
    """
    t0 = time.perf_counter()
    rows, cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = [(1, 10.0, True, False), (2, 5.0, False, True), (3, 7.0, False, False)]
    n = len(data)
    t_build = time.perf_counter()
    oracle_solver.create_model("composed_max_plus_max")
    vnames = [f"x_{i}" for i in range(n)]
    for v in vnames:
        oracle_solver.add_variable(v, VarType.BINARY)
    # z1 = MAX over w1, z2 = MAX over w2
    for name, mask_col in [("z1", 2), ("z2", 3)]:
        ub = max(d[1] for d in data if d[mask_col]) + 1.0 if any(d[mask_col] for d in data) else 1.0
        oracle_solver.add_variable(name, VarType.CONTINUOUS, lb=0.0, ub=ub)
        for i, d in enumerate(data):
            if d[mask_col]:
                oracle_solver.add_constraint(
                    {name: 1.0, vnames[i]: -d[1]}, ">=", 0.0, name=f"{name}_pin_{i}",
                )
    oracle_solver.add_constraint({"z1": 1.0, "z2": 1.0}, "<=", 12.0, name="outer")
    oracle_solver.set_objective({vnames[i]: data[i][1] for i in range(n)}, ObjSense.MAXIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        rows, cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[cols.index("v")])},
    )
    perf_tracker.record(
        "composed_max_plus_max", decidb_time, build_time, result.solve_time_seconds,
        n, n + 2, n + 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.min_max
@pytest.mark.when_constraint
@pytest.mark.correctness
def test_min_plus_min_geq_composed(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """MIN(x*v) WHEN w1 + MIN(x*v) WHEN w2 >= K — two easy-direction MIN terms.

    Easy-direction: pushed up by >=, MIN pushed up ⇒ easy. Forces both
    MIN aggregates to at least K/2 each (roughly) via per-row constraints
    z_k <= v_i * x_i, so the outer z1+z2 >= K can be satisfied.
    """
    # Use positive decision problem: SUM(x) >= 1 PER group so solution exists.
    # Simpler: force x=1 on the binding rows by constraint direction.
    sql = """
        SELECT id, v, x FROM (
            VALUES (1, 10.0, true, false),
                   (2, 8.0, false, true),
                   (3, 7.0, false, false)
        ) t(id, v, w1, w2)
        DECIDE x(BOOL)
        SUCH THAT MIN(x * v) WHEN w1 + MIN(x * v) WHEN w2 >= 15
        MINIMIZE SUM(x * v)
    """
    t0 = time.perf_counter()
    rows, cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = [(1, 10.0, True, False), (2, 8.0, False, True), (3, 7.0, False, False)]
    n = len(data)
    t_build = time.perf_counter()
    oracle_solver.create_model("composed_min_plus_min")
    vnames = [f"x_{i}" for i in range(n)]
    for v in vnames:
        oracle_solver.add_variable(v, VarType.BINARY)
    for name, mask_col in [("z1", 2), ("z2", 3)]:
        ub = max(d[1] for d in data if d[mask_col]) + 1.0 if any(d[mask_col] for d in data) else 1.0
        oracle_solver.add_variable(name, VarType.CONTINUOUS, lb=0.0, ub=ub)
        # MIN easy (pushed up): z <= v_i * x_i for each matching row
        for i, d in enumerate(data):
            if d[mask_col]:
                oracle_solver.add_constraint(
                    {name: 1.0, vnames[i]: -d[1]}, "<=", 0.0, name=f"{name}_pin_{i}",
                )
    oracle_solver.add_constraint({"z1": 1.0, "z2": 1.0}, ">=", 15.0, name="outer")
    oracle_solver.set_objective({vnames[i]: data[i][1] for i in range(n)}, ObjSense.MINIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        rows, cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[cols.index("v")])},
    )
    perf_tracker.record(
        "composed_min_plus_min", decidb_time, build_time, result.solve_time_seconds,
        n, n + 2, n + 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.min_max
@pytest.mark.correctness
def test_composed_minmax_hard_max_constraint(decidb_cli):
    """Hard-direction composed MAX constraint: SUM(x*v) + MAX(x*v) >= K.

    `>=` pushes the MAX term the "wrong" way, so z is pinned to the true MAX by
    the per-row indicator layer (SUM(y)>=1 + Big-M link). Without it z floats to
    +inf and the solver picks nothing. v=[10,20,30]: one row v gives total 2v, so
    v=20 (40) and v=30 (60) are feasible alone, v=10 (20) is not; 0 rows gives
    0 < 40. MINIMIZE SUM(x) => exactly one row, satisfying the bound.
    """
    rows, cols = decidb_cli.execute("""
        SELECT id, v, x FROM (VALUES (1,10.0),(2,20.0),(3,30.0)) t(id,v)
        DECIDE x(BOOL)
        SUCH THAT SUM(x * v) + MAX(x * v) >= 40
        MINIMIZE SUM(x)
    """)
    ci = {c: i for i, c in enumerate(cols)}
    chosen = [float(r[ci["v"]]) for r in rows if int(r[ci["x"]]) == 1]
    assert len(chosen) == 1, f"expected exactly 1 row, got {chosen}"
    assert sum(chosen) + max(chosen) >= 40 - 1e-6


@pytest.mark.min_max
@pytest.mark.error_binder
def test_composed_minmax_subtraction_rejected(decidb_cli):
    """Subtraction in a composed MIN/MAX LHS is rejected in v1."""
    decidb_cli.assert_error("""
        SELECT id, v FROM (VALUES (1, 10.0, true), (2, 5.0, true)) t(id, v, w)
        DECIDE x(BOOL)
        SUCH THAT MAX(x * v) WHEN w - MIN(x * v) WHEN w <= 3
        MAXIMIZE SUM(x * v)
    """, match=r"does not support subtraction")


@pytest.mark.min_max
@pytest.mark.when_constraint
@pytest.mark.correctness
def test_composed_minmax_scalar_mult_hard_min(decidb_cli):
    """`(2 * MIN(x*v) WHEN w) + SUM(x*v) <= K` — hard-direction composed MIN with
    a scalar multiplier.

    The symbolic K*WHEN fold collapses `2 * (MIN(x*v) WHEN w)` into
    `WHEN(MIN(2*x*v), w)`, and the hard-MIN indicator layer pins z to the true MIN
    of the scaled inner expression. Regression for two things at once: hard-
    direction support, and the ExtractCoefficient fix that stopped dropping the
    `2` in the un-normalized `(2*x)*v`.

    rows v=[10,5], w=true. Fold => MIN(2*x*v) + SUM(x*v) <= 20. Both selected:
    MIN(20,10)=10 + SUM 15 = 25 > 20 (infeasible). Row1 alone: MIN(20,0)=0 +
    SUM 10 = 10 (feasible). Row2 alone: obj 5. MAXIMIZE SUM(x*v) => row1 only,
    objective 10.
    """
    rows, cols = decidb_cli.execute("""
        SELECT id, v, x FROM (VALUES (1,10.0,true),(2,5.0,true)) t(id,v,w)
        DECIDE x(BOOL)
        SUCH THAT (2 * MIN(x * v) WHEN w) + SUM(x * v) <= 20
        MAXIMIZE SUM(x * v)
    """)
    ci = {c: i for i, c in enumerate(cols)}
    obj = sum(float(r[ci["v"]]) for r in rows if int(r[ci["x"]]) == 1)
    assert abs(obj - 10.0) <= 1e-6, f"expected obj 10 (row1 only), got {obj}"


@pytest.mark.min_max
@pytest.mark.correctness
def test_composed_minmax_scalar_mult_hard_max(decidb_cli):
    """`SUM(x*v) + MAX(2 * x * v) >= K` — hard-direction composed MAX with a scalar
    factor inside the inner expression. Discriminates the ExtractCoefficient fix:
    the `2` must survive.

    v=[10,5], MINIMIZE SUM(x). With the 2 applied, row1 alone gives
    SUM(x*v)=10 + MAX(2*x*v)=20 = 30 >= 25 — one row suffices. If the 2 were
    dropped, MAX would be 10, row1 gives 20 < 25 and no single row works, forcing
    two rows. So the optimum count is 1 iff the scalar is correctly applied.
    """
    rows, cols = decidb_cli.execute("""
        SELECT id, v, x FROM (VALUES (1,10.0),(2,5.0)) t(id,v)
        DECIDE x(BOOL)
        SUCH THAT SUM(x * v) + MAX(2 * x * v) >= 25
        MINIMIZE SUM(x)
    """)
    ci = {c: i for i, c in enumerate(cols)}
    chosen = [(float(r[ci["v"]]), int(r[ci["x"]])) for r in rows]
    n_sel = sum(xv for _, xv in chosen)
    assert n_sel == 1, f"expected exactly 1 row (scalar 2 applied), got {chosen}"
    sel_v = [v for v, xv in chosen if xv == 1]
    assert sum(sel_v) + 2 * max(sel_v) >= 25 - 1e-6


@pytest.mark.min_max
@pytest.mark.error_binder
def test_composed_minmax_per_wrapper_rejected(decidb_cli):
    """PER on a composed MIN/MAX constraint is rejected in v1."""
    decidb_cli.assert_error("""
        SELECT id, v, g FROM (VALUES (1, 10.0, true, 'A'), (2, 5.0, true, 'B')) t(id, v, w, g)
        DECIDE x(BOOL)
        SUCH THAT SUM(x * v) + MAX(x * v) WHEN w <= 12 PER g
        MAXIMIZE SUM(x * v)
    """, match=r"does not support outer WHEN/PER")


@pytest.mark.min_max
@pytest.mark.error_binder
def test_composed_minmax_nonconst_rhs_subquery_rejected(decidb_cli):
    """Composed MIN/MAX with a scalar-subquery RHS is rejected in v1.

    v1 requires the outer RHS of a composed MIN/MAX constraint to be a
    constant literal (possibly cast-wrapped). A scalar subquery — even
    one that returns a constant — trips the dedicated guard.
    """
    decidb_cli.assert_error("""
        SELECT id, v FROM (VALUES (1, 10.0, true), (2, 5.0, true)) t(id, v, w)
        DECIDE x(BOOL)
        SUCH THAT SUM(x * v) + MAX(x * v) WHEN w <= (SELECT 12)
        MAXIMIZE SUM(x * v)
    """, match=r"Composed MIN/MAX in DECIDE v1 requires a constant RHS")


@pytest.mark.min_max
@pytest.mark.error_binder
def test_composed_minmax_nonconst_rhs_column_rejected(decidb_cli):
    """Composed MIN/MAX with a column-reference RHS is rejected in v1.

    Different rejection path than the subquery shape: the binder fails
    earlier with a generic SUM-comparison error before the composed
    guard runs. Pinned anyway so a future widen of the composed RHS
    grammar surfaces here.
    """
    decidb_cli.assert_error("""
        SELECT id, v FROM (VALUES (1, 10.0, true, 12), (2, 5.0, true, 12)) t(id, v, w, cap)
        DECIDE x(BOOL)
        SUCH THAT SUM(x * v) + MAX(x * v) WHEN w <= cap
        MAXIMIZE SUM(x * v)
    """, match=r"SUM cannot be compared to an expression that is not a scalar or aggregate")


@pytest.mark.min_max
@pytest.mark.error_binder
def test_composed_minmax_outer_when_rejected(decidb_cli):
    """Outer expression-level WHEN on a composed MIN/MAX is rejected in v1.

    Distinct from the PER-rejection pin above: shares the
    "expression-level vs aggregate-local WHEN" guard rather than the
    "outer WHEN/PER on composed" guard, but tests the same v1 limitation
    on outer modifiers. Pinned separately so a widened composed grammar
    that admits one but not the other is detectable.
    """
    decidb_cli.assert_error("""
        SELECT id, v, tier FROM (
            VALUES (1, 10.0, true, 'high'), (2, 5.0, true, 'low')
        ) t(id, v, w, tier)
        DECIDE x(BOOL)
        SUCH THAT (SUM(x * v) + MAX(x * v) WHEN w) <= 12 WHEN (tier = 'high')
        MAXIMIZE SUM(x * v)
    """, match=r"Cannot combine expression-level WHEN with aggregate-local WHEN")


# ----- Composed MIN/MAX in objectives -----


@pytest.mark.min_max
@pytest.mark.obj_minimize
@pytest.mark.when_objective
@pytest.mark.correctness
def test_minimize_sum_plus_max_composed_objective(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """MINIMIZE SUM(x*v) + MAX(x*v) WHEN w — both terms pushed down (easy)."""
    sql = """
        SELECT id, v, w, x FROM (
            VALUES (1, 10.0, true),
                   (2, 5.0, false),
                   (3, 7.0, false)
        ) t(id, v, w)
        DECIDE x(BOOL)
        SUCH THAT SUM(x) >= 1
        MINIMIZE SUM(x * v) + MAX(x * v) WHEN w
    """
    t0 = time.perf_counter()
    rows, cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = [(1, 10.0, True), (2, 5.0, False), (3, 7.0, False)]
    n = len(data)
    t_build = time.perf_counter()
    oracle_solver.create_model("min_sum_plus_max_obj")
    vnames = [f"x_{i}" for i in range(n)]
    for v in vnames:
        oracle_solver.add_variable(v, VarType.BINARY)
    oracle_solver.add_constraint({v: 1.0 for v in vnames}, ">=", 1.0, name="atleastone")
    # z = MAX(x*v) WHEN w, pushed down (easy): z >= x*v per w-matching row
    z_ub = max(d[1] for d in data if d[2]) + 1.0 if any(d[2] for d in data) else 1.0
    oracle_solver.add_variable("z", VarType.CONTINUOUS, lb=0.0, ub=z_ub)
    for i, d in enumerate(data):
        if d[2]:
            oracle_solver.add_constraint(
                {"z": 1.0, vnames[i]: -d[1]}, ">=", 0.0, name=f"z_pin_{i}",
            )
    obj = {vnames[i]: data[i][1] for i in range(n)}
    obj["z"] = 1.0
    oracle_solver.set_objective(obj, ObjSense.MINIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    def decidb_obj_m(rs, cs):
        xi = cs.index("x"); vi = cs.index("v"); wi = cs.index("w")
        sum_part = sum(int(r[xi]) * float(r[vi]) for r in rs)
        max_vals = [float(r[vi]) * int(r[xi]) for r in rs if r[wi]]
        max_part = max(max_vals) if max_vals else 0.0
        return sum_part + max_part

    cmp = compare_solutions(
        rows, cols, result, data, ["x"],
        decidb_objective_fn=decidb_obj_m,
    )
    perf_tracker.record(
        "min_sum_plus_max_obj", decidb_time, build_time, result.solve_time_seconds,
        n, n + 1, 1 + sum(1 for d in data if d[2]),
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.min_max
@pytest.mark.obj_maximize
@pytest.mark.when_objective
@pytest.mark.correctness
def test_maximize_min_plus_sum_composed_objective(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """MAXIMIZE MIN(x*v) WHEN w + SUM(x*v) — MIN pushed up (easy) + SUM pushed up."""
    sql = """
        SELECT id, v, w, x FROM (
            VALUES (1, 10.0, true),
                   (2, 5.0, true),
                   (3, 7.0, false)
        ) t(id, v, w)
        DECIDE x(BOOL)
        SUCH THAT SUM(x) <= 2
        MAXIMIZE MIN(x * v) WHEN w + SUM(x * v)
    """
    t0 = time.perf_counter()
    rows, cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = [(1, 10.0, True), (2, 5.0, True), (3, 7.0, False)]
    n = len(data)
    t_build = time.perf_counter()
    oracle_solver.create_model("max_min_plus_sum_obj")
    vnames = [f"x_{i}" for i in range(n)]
    for v in vnames:
        oracle_solver.add_variable(v, VarType.BINARY)
    oracle_solver.add_constraint({v: 1.0 for v in vnames}, "<=", 2.0, name="budget")
    # z = MIN(x*v) WHEN w, pushed up (easy): z <= x*v per w-matching row
    z_ub = max(d[1] for d in data if d[2]) + 1.0 if any(d[2] for d in data) else 1.0
    oracle_solver.add_variable("z", VarType.CONTINUOUS, lb=0.0, ub=z_ub)
    for i, d in enumerate(data):
        if d[2]:
            oracle_solver.add_constraint(
                {"z": 1.0, vnames[i]: -d[1]}, "<=", 0.0, name=f"z_pin_{i}",
            )
    obj = {vnames[i]: data[i][1] for i in range(n)}
    obj["z"] = 1.0
    oracle_solver.set_objective(obj, ObjSense.MAXIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    def decidb_obj(rs, cs):
        xi = cs.index("x"); vi = cs.index("v"); wi = cs.index("w")
        sum_part = sum(int(r[xi]) * float(r[vi]) for r in rs)
        min_vals = [float(r[vi]) * int(r[xi]) for r in rs if r[wi]]
        min_part = min(min_vals) if min_vals else 0.0
        return sum_part + min_part

    cmp = compare_solutions(
        rows, cols, result, data, ["x"],
        decidb_objective_fn=decidb_obj,
    )
    perf_tracker.record(
        "max_min_plus_sum_obj", decidb_time, build_time, result.solve_time_seconds,
        n, n + 1, 1 + sum(1 for d in data if d[2]),
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.min_max
@pytest.mark.obj_maximize
@pytest.mark.when_objective
@pytest.mark.error_binder
def test_composed_minmax_objective_hard_max(decidb_cli):
    """MAXIMIZE MAX(x*v) WHEN w + SUM(x*v) — MAX pushed UP is the hard objective
    direction. Without the indicator layer z_k floats up and the objective is
    unbounded; the layer pins z_k to the true MAX so the optimum is finite.

    rows v=[10,5], w=true, SUM(x)>=1. Selecting both maximizes both terms:
    SUM(x*v)=15 + MAX(x*v)=10 = 25.
    """
    rows, cols = decidb_cli.execute("""
        SELECT id, v, x FROM (VALUES (1,10.0,true),(2,5.0,true)) t(id,v,w)
        DECIDE x(BOOL)
        SUCH THAT SUM(x) >= 1
        MAXIMIZE MAX(x * v) WHEN w + SUM(x * v)
    """)
    ci = {c: i for i, c in enumerate(cols)}
    chosen = [float(r[ci["v"]]) for r in rows if int(r[ci["x"]]) == 1]
    assert set(chosen) == {10.0, 5.0}, f"expected both rows, got {chosen}"
    assert abs((sum(chosen) + max(chosen)) - 25.0) <= 1e-6


@pytest.mark.min_max
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_composed_minmax_objective_hard_min(decidb_cli):
    """MINIMIZE SUM(x*v) + MIN(x*v) — MIN pushed DOWN is the hard objective
    direction. If z_k were not pinned below the objective would be unbounded
    (z_k -> -inf); the indicator layer pins it to the true MIN.

    v=[10,5,3], SUM(x)>=2 (pick >=2 rows). Any 2-row pick leaves one row
    unselected so MIN(x*v)=0; MINIMIZE then picks the two smallest, {3,5}, for
    objective SUM(x*v)=8 (MIN term 0). Row v=10 stays out.
    """
    rows, cols = decidb_cli.execute("""
        SELECT id, v, x FROM (VALUES (1,10.0),(2,5.0),(3,3.0)) t(id,v)
        DECIDE x(BOOL)
        SUCH THAT SUM(x) >= 2
        MINIMIZE SUM(x * v) + MIN(x * v)
    """)
    ci = {c: i for i, c in enumerate(cols)}
    chosen = [float(r[ci["v"]]) for r in rows if int(r[ci["x"]]) == 1]
    assert set(chosen) == {5.0, 3.0}, f"expected the two smallest, got {chosen}"


# ============================================================================
# Composed MIN/MAX with an entity-scoped variable — KNOWN DEFECT
# ============================================================================

@pytest.mark.min_max
@pytest.mark.correctness
def test_composed_minmax_entity_scoped_multi_row(decidb_cli, perf_tracker):
    """`MINIMIZE SUM(c*open) + MAX(c*open)` over a table-scoped decision on a join.

    Three depots, costs 50/40/30; depot 1 joins two routes, the others one each.
    Charging every row, the objective is
    ``100*o1 + 40*o2 + 30*o3 + max(50*o1, 50*o1, 40*o2, 30*o3)``, minimised at
    ``o3 = 1`` for 60.

    Regression for the composed-term binding bug: the composed MIN/MAX rewrite
    lifts sub-expressions out of the objective into their own vector, and the
    column-binding resolver used to skip that vector — so the operator read each
    coefficient reference's *logical* column index as a position in the
    materialized chunk. Here that made ``D.c`` resolve to ``R.rid``, and the
    query answered depot 2 (objective 80). A single-table source hides it because
    the two indexings coincide, which is why every pre-existing composed test
    passed.

    The oracle is enumeration over the eight assignments rather than a solver
    call, because the instance is small enough to be exhaustive and the point is
    the objective *value*, not an alternate optimum.
    """
    setup = """
        WITH D(did, c) AS (VALUES (1, 50), (2, 40), (3, 30)),
             R(rid, did) AS (VALUES (10, 1), (11, 1), (12, 2), (13, 3))
    """
    rows, cols = decidb_cli.execute(setup + """
        SELECT R.rid, D.did, open
        FROM D JOIN R ON D.did = R.did
        DECIDE D.open(BOOL)
        SUCH THAT SUM(open) >= 1
        MINIMIZE SUM(D.c * open) + MAX(D.c * open)
    """)
    assert len(rows) == 4

    ci = {c: i for i, c in enumerate(cols)}
    cost = {1: 50, 2: 40, 3: 30}
    chosen = {int(r[ci["did"]]): int(r[ci["open"]]) for r in rows}

    row_values = [cost[int(r[ci["did"]])] * int(r[ci["open"]]) for r in rows]
    decidb_obj = sum(row_values) + max(row_values)

    # Exhaustive oracle over the three entity decisions.
    best = None
    for o1 in (0, 1):
        for o2 in (0, 1):
            for o3 in (0, 1):
                if 2 * o1 + o2 + o3 < 1:
                    continue
                vals = [50 * o1, 50 * o1, 40 * o2, 30 * o3]
                total = sum(vals) + max(vals)
                if best is None or total < best:
                    best = total

    assert decidb_obj == best, (
        f"composed MIN/MAX is suboptimal: DeciDB={decidb_obj} (chose {chosen}), "
        f"optimal={best}")
