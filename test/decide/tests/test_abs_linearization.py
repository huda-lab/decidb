"""Tests for ABS() linearization over decision variables.

Covers:
  - test_abs_objective_basic: MINIMIZE SUM(ABS(x - col)) with IS REAL
  - test_abs_objective_with_when: ABS in objective with WHEN condition
  - test_abs_objective_with_per: ABS in objective with PER grouping
  - test_abs_constraint_per_row: ABS(x - col) <= tolerance (per-row)
  - test_abs_constraint_aggregate: SUM(ABS(x - col)) <= total_tolerance
  - test_abs_constraint_aggregate_with_when: aggregate constraint with expression-level WHEN
  - test_abs_multiple_terms: two ABS terms in one expression
  - test_abs_no_decide_var: ABS without decide variable (regular SQL)
  - test_abs_mixed_vars: BOOLEAN + REAL with ABS on REAL only
  - test_abs_maximize_objective_basic: MAXIMIZE SUM(ABS(x - col)) with Big-M fix
  - test_abs_maximize_no_bound_raises: MAXIMIZE SUM(ABS(x - col)) without upper bound raises
"""

import time

import pytest

from solver.types import VarType, ObjSense, SolverStatus


def _compute_abs_objective(decidb_rows, decidb_cols, var_name, ref_col):
    """Compute SUM(ABS(var - ref_col)) from DecidB output."""
    ci = {name: i for i, name in enumerate(decidb_cols)}
    return sum(
        abs(float(row[ci[var_name]]) - float(row[ci[ref_col]]))
        for row in decidb_rows
    )


@pytest.mark.var_real
@pytest.mark.cons_aggregate
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_abs_objective_basic(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """MINIMIZE SUM(ABS(new_qty - l_quantity)) with aggregate constraint."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_quantity, new_qty
        FROM lineitem
        WHERE l_orderkey <= 5
        DECIDE new_qty IS REAL
        SUCH THAT SUM(new_qty) = 100
        MINIMIZE SUM(ABS(new_qty - l_quantity))
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

    n = len(data)
    t_build = time.perf_counter()
    oracle_solver.create_model("abs_objective_basic")
    vnames = [f"new_qty_{i}" for i in range(n)]
    dnames = [f"d_{i}" for i in range(n)]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.CONTINUOUS, lb=0.0)
    for dn in dnames:
        oracle_solver.add_variable(dn, VarType.CONTINUOUS, lb=0.0)

    # SUM(new_qty) = 100
    oracle_solver.add_constraint(
        {vnames[i]: 1.0 for i in range(n)}, "=", 100.0, name="sum_eq",
    )
    # ABS linearization: d_i >= new_qty_i - qty_i, d_i >= -(new_qty_i - qty_i)
    for i in range(n):
        qty = data[i][2]
        oracle_solver.add_constraint(
            {dnames[i]: 1.0, vnames[i]: -1.0}, ">=", -qty, name=f"abs_pos_{i}",
        )
        oracle_solver.add_constraint(
            {dnames[i]: 1.0, vnames[i]: 1.0}, ">=", qty, name=f"abs_neg_{i}",
        )
    oracle_solver.set_objective(
        {dnames[i]: 1.0 for i in range(n)}, ObjSense.MINIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    assert result.status == SolverStatus.OPTIMAL
    decidb_obj = _compute_abs_objective(decidb_result, decidb_cols, "new_qty", "l_quantity")
    assert abs(decidb_obj - result.objective_value) <= 1e-4, (
        f"Objective mismatch: DecidB={decidb_obj:.6f}, Oracle={result.objective_value:.6f}"
    )

    perf_tracker.record(
        "abs_objective_basic", decidb_time, build_time,
        result.solve_time_seconds, n, n * 2, 1 + n * 2,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )


@pytest.mark.var_real
@pytest.mark.when_constraint
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_abs_objective_with_when(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """MINIMIZE SUM(ABS(new_qty - l_quantity)) WHEN l_returnflag = 'R'."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_quantity, l_returnflag, new_qty
        FROM lineitem
        WHERE l_orderkey <= 5
        DECIDE new_qty IS REAL
        SUCH THAT SUM(new_qty) = 100
        MINIMIZE SUM(ABS(new_qty - l_quantity)) WHEN l_returnflag = 'R'
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_quantity AS DOUBLE),
               l_returnflag
        FROM lineitem WHERE l_orderkey <= 5
    """).fetchall()

    n = len(data)
    t_build = time.perf_counter()
    oracle_solver.create_model("abs_objective_when")
    vnames = [f"new_qty_{i}" for i in range(n)]
    dnames = [f"d_{i}" for i in range(n)]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.CONTINUOUS, lb=0.0)
    for dn in dnames:
        oracle_solver.add_variable(dn, VarType.CONTINUOUS, lb=0.0)

    oracle_solver.add_constraint(
        {vnames[i]: 1.0 for i in range(n)}, "=", 100.0, name="sum_eq",
    )
    # ABS linearization for ALL rows (constraints are unconditional)
    for i in range(n):
        qty = data[i][2]
        oracle_solver.add_constraint(
            {dnames[i]: 1.0, vnames[i]: -1.0}, ">=", -qty, name=f"abs_pos_{i}",
        )
        oracle_solver.add_constraint(
            {dnames[i]: 1.0, vnames[i]: 1.0}, ">=", qty, name=f"abs_neg_{i}",
        )
    # Objective only on rows where l_returnflag = 'R'
    obj = {dnames[i]: 1.0 for i in range(n) if data[i][3] == 'R'}
    if obj:
        oracle_solver.set_objective(obj, ObjSense.MINIMIZE)
    else:
        oracle_solver.set_objective({dnames[0]: 0.0}, ObjSense.MINIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    assert result.status == SolverStatus.OPTIMAL
    ci = {name: i for i, name in enumerate(decidb_cols)}
    decidb_obj = sum(
        abs(float(row[ci["new_qty"]]) - float(row[ci["l_quantity"]]))
        for row in decidb_result
        if row[ci["l_returnflag"]] == 'R'
    )
    assert abs(decidb_obj - result.objective_value) <= 1e-4, (
        f"Objective mismatch: DecidB={decidb_obj:.6f}, Oracle={result.objective_value:.6f}"
    )

    perf_tracker.record(
        "abs_objective_when", decidb_time, build_time,
        result.solve_time_seconds, n, n * 2, 1 + n * 2,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )


@pytest.mark.var_real
@pytest.mark.per_clause
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_abs_objective_with_per(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """MINIMIZE SUM(ABS(new_qty - l_quantity)) with PER l_orderkey constraint."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_quantity, new_qty
        FROM lineitem
        WHERE l_orderkey <= 5
        DECIDE new_qty IS REAL
        SUCH THAT SUM(new_qty) = 20 PER l_orderkey
        MINIMIZE SUM(ABS(new_qty - l_quantity))
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

    groups = {}
    for i, row in enumerate(data):
        groups.setdefault(row[0], []).append(i)

    n = len(data)
    t_build = time.perf_counter()
    oracle_solver.create_model("abs_objective_per")
    vnames = [f"new_qty_{i}" for i in range(n)]
    dnames = [f"d_{i}" for i in range(n)]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.CONTINUOUS, lb=0.0)
    for dn in dnames:
        oracle_solver.add_variable(dn, VarType.CONTINUOUS, lb=0.0)

    # PER l_orderkey: SUM(new_qty) = 20 per group
    for orderkey, row_indices in groups.items():
        oracle_solver.add_constraint(
            {vnames[i]: 1.0 for i in row_indices}, "=", 20.0,
            name=f"per_{orderkey}",
        )
    # ABS linearization
    for i in range(n):
        qty = data[i][2]
        oracle_solver.add_constraint(
            {dnames[i]: 1.0, vnames[i]: -1.0}, ">=", -qty, name=f"abs_pos_{i}",
        )
        oracle_solver.add_constraint(
            {dnames[i]: 1.0, vnames[i]: 1.0}, ">=", qty, name=f"abs_neg_{i}",
        )
    oracle_solver.set_objective(
        {dnames[i]: 1.0 for i in range(n)}, ObjSense.MINIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    assert result.status == SolverStatus.OPTIMAL
    decidb_obj = _compute_abs_objective(decidb_result, decidb_cols, "new_qty", "l_quantity")
    assert abs(decidb_obj - result.objective_value) <= 1e-4, (
        f"Objective mismatch: DecidB={decidb_obj:.6f}, Oracle={result.objective_value:.6f}"
    )

    perf_tracker.record(
        "abs_objective_per", decidb_time, build_time,
        result.solve_time_seconds, n, n * 2, len(groups) + n * 2,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )


@pytest.mark.var_real
@pytest.mark.cons_perrow
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_abs_constraint_per_row(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Per-row constraint: ABS(new_qty - l_quantity) <= 5."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_quantity, l_extendedprice, new_qty
        FROM lineitem
        WHERE l_orderkey <= 5
        DECIDE new_qty IS REAL
        SUCH THAT ABS(new_qty - l_quantity) <= 5
        MAXIMIZE SUM(new_qty * l_extendedprice)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_quantity AS DOUBLE),
               CAST(l_extendedprice AS DOUBLE)
        FROM lineitem WHERE l_orderkey <= 5
    """).fetchall()

    n = len(data)
    t_build = time.perf_counter()
    oracle_solver.create_model("abs_constraint_perrow")
    vnames = [f"new_qty_{i}" for i in range(n)]
    dnames = [f"d_{i}" for i in range(n)]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.CONTINUOUS, lb=0.0)
    for dn in dnames:
        oracle_solver.add_variable(dn, VarType.CONTINUOUS, lb=0.0)

    for i in range(n):
        qty = data[i][2]
        # ABS linearization: d_i >= new_qty_i - qty, d_i >= qty - new_qty_i
        oracle_solver.add_constraint(
            {dnames[i]: 1.0, vnames[i]: -1.0}, ">=", -qty, name=f"abs_pos_{i}",
        )
        oracle_solver.add_constraint(
            {dnames[i]: 1.0, vnames[i]: 1.0}, ">=", qty, name=f"abs_neg_{i}",
        )
        # d_i <= 5
        oracle_solver.add_constraint(
            {dnames[i]: 1.0}, "<=", 5.0, name=f"abs_bound_{i}",
        )
    oracle_solver.set_objective(
        {vnames[i]: data[i][3] for i in range(n)}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    assert result.status == SolverStatus.OPTIMAL
    # Verify per-row ABS constraint
    ci = {name: i for i, name in enumerate(decidb_cols)}
    for row in decidb_result:
        deviation = abs(float(row[ci["new_qty"]]) - float(row[ci["l_quantity"]]))
        assert deviation <= 5.0 + 1e-6, f"ABS constraint violated: deviation={deviation}"

    # Compare objectives
    decidb_obj = sum(
        float(row[ci["new_qty"]]) * float(row[ci["l_extendedprice"]])
        for row in decidb_result
    )
    assert abs(decidb_obj - result.objective_value) <= 1e-2, (
        f"Objective mismatch: DecidB={decidb_obj:.6f}, Oracle={result.objective_value:.6f}"
    )

    perf_tracker.record(
        "abs_constraint_perrow", decidb_time, build_time,
        result.solve_time_seconds, n, n * 2, n * 3,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )


@pytest.mark.var_real
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_abs_constraint_aggregate(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Aggregate constraint: SUM(ABS(new_qty - l_quantity)) <= 50."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_quantity, l_extendedprice, new_qty
        FROM lineitem
        WHERE l_orderkey <= 5
        DECIDE new_qty IS REAL
        SUCH THAT SUM(ABS(new_qty - l_quantity)) <= 50
        MAXIMIZE SUM(new_qty * l_extendedprice)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_quantity AS DOUBLE),
               CAST(l_extendedprice AS DOUBLE)
        FROM lineitem WHERE l_orderkey <= 5
    """).fetchall()

    n = len(data)
    t_build = time.perf_counter()
    oracle_solver.create_model("abs_constraint_agg")
    vnames = [f"new_qty_{i}" for i in range(n)]
    dnames = [f"d_{i}" for i in range(n)]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.CONTINUOUS, lb=0.0)
    for dn in dnames:
        oracle_solver.add_variable(dn, VarType.CONTINUOUS, lb=0.0)

    # ABS linearization
    for i in range(n):
        qty = data[i][2]
        oracle_solver.add_constraint(
            {dnames[i]: 1.0, vnames[i]: -1.0}, ">=", -qty, name=f"abs_pos_{i}",
        )
        oracle_solver.add_constraint(
            {dnames[i]: 1.0, vnames[i]: 1.0}, ">=", qty, name=f"abs_neg_{i}",
        )
    # SUM(d_i) <= 50
    oracle_solver.add_constraint(
        {dnames[i]: 1.0 for i in range(n)}, "<=", 50.0, name="total_abs",
    )
    oracle_solver.set_objective(
        {vnames[i]: data[i][3] for i in range(n)}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    assert result.status == SolverStatus.OPTIMAL
    ci = {name: i for i, name in enumerate(decidb_cols)}
    # Verify aggregate ABS constraint
    total_dev = sum(
        abs(float(row[ci["new_qty"]]) - float(row[ci["l_quantity"]]))
        for row in decidb_result
    )
    assert total_dev <= 50.0 + 1e-4, f"Aggregate ABS constraint violated: {total_dev}"

    decidb_obj = sum(
        float(row[ci["new_qty"]]) * float(row[ci["l_extendedprice"]])
        for row in decidb_result
    )
    assert abs(decidb_obj - result.objective_value) <= 1e-2, (
        f"Objective mismatch: DecidB={decidb_obj:.6f}, Oracle={result.objective_value:.6f}"
    )

    perf_tracker.record(
        "abs_constraint_agg", decidb_time, build_time,
        result.solve_time_seconds, n, n * 2, 1 + n * 2,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )


@pytest.mark.var_real
@pytest.mark.cons_aggregate
@pytest.mark.when_constraint
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_abs_constraint_aggregate_with_when(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """SUM(ABS(new_qty - l_quantity)) <= 30 WHEN l_returnflag = 'R'.

    Exercises WHEN mask propagation to ABS-linearization auxiliaries in an
    aggregate constraint. The d_i auxiliaries exist for all rows (their linking
    constraints are unconditional), but only WHEN-matching rows contribute to
    SUM(d_i). A bug that sums d_i over all rows would over-constrain the
    problem and produce a different optimum.
    """
    sql = """
        SELECT l_orderkey, l_linenumber, l_quantity, l_extendedprice, l_returnflag,
               ROUND(new_qty, 4) AS new_qty
        FROM lineitem
        WHERE l_orderkey <= 5
        DECIDE new_qty IS REAL
        SUCH THAT new_qty <= 60
            AND SUM(ABS(new_qty - l_quantity)) <= 30 WHEN l_returnflag = 'R'
        MAXIMIZE SUM(new_qty * l_extendedprice)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_quantity AS DOUBLE),
               CAST(l_extendedprice AS DOUBLE),
               l_returnflag
        FROM lineitem WHERE l_orderkey <= 5
    """).fetchall()

    n = len(data)
    t_build = time.perf_counter()
    oracle_solver.create_model("abs_constraint_agg_when")
    vnames = [f"new_qty_{i}" for i in range(n)]
    dnames = [f"d_{i}" for i in range(n)]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.CONTINUOUS, lb=0.0, ub=60.0)
    for dn in dnames:
        oracle_solver.add_variable(dn, VarType.CONTINUOUS, lb=0.0)

    # ABS linearization is unconditional (d_i >= |new_qty_i - qty_i| for every row)
    for i in range(n):
        qty = data[i][2]
        oracle_solver.add_constraint(
            {dnames[i]: 1.0, vnames[i]: -1.0}, ">=", -qty, name=f"abs_pos_{i}",
        )
        oracle_solver.add_constraint(
            {dnames[i]: 1.0, vnames[i]: 1.0}, ">=", qty, name=f"abs_neg_{i}",
        )

    # Aggregate upper bound only over WHEN-matching (R) rows
    r_rows = {dnames[i]: 1.0 for i in range(n) if data[i][4] == 'R'}
    if r_rows:
        oracle_solver.add_constraint(r_rows, "<=", 30.0, name="when_abs_sum")

    oracle_solver.set_objective(
        {vnames[i]: data[i][3] for i in range(n)}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    assert result.status == SolverStatus.OPTIMAL
    ci = {name: i for i, name in enumerate(decidb_cols)}

    # Sanity check: DecidB actually respected the WHEN-filtered ABS bound
    r_total_dev = sum(
        abs(float(row[ci["new_qty"]]) - float(row[ci["l_quantity"]]))
        for row in decidb_result
        if row[ci["l_returnflag"]] == 'R'
    )
    assert r_total_dev <= 30.0 + 1e-4, (
        f"WHEN-filtered ABS sum exceeded 30: {r_total_dev}"
    )

    decidb_obj = sum(
        float(row[ci["new_qty"]]) * float(row[ci["l_extendedprice"]])
        for row in decidb_result
    )
    assert abs(decidb_obj - result.objective_value) <= 1e-2, (
        f"Objective mismatch: DecidB={decidb_obj:.6f}, Oracle={result.objective_value:.6f}"
    )

    perf_tracker.record(
        "abs_constraint_agg_when", decidb_time, build_time,
        result.solve_time_seconds, n, n * 2, 1 + n * 2,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )


@pytest.mark.var_real
@pytest.mark.cons_aggregate
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_abs_multiple_terms(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Two ABS terms: MINIMIZE SUM(ABS(x - a) + ABS(y - b))."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_quantity, l_extendedprice, x, y
        FROM lineitem
        WHERE l_orderkey <= 3
        DECIDE x IS REAL, y IS REAL
        SUCH THAT SUM(x) = 50
            AND SUM(y) = 10000
        MINIMIZE SUM(ABS(x - l_quantity) + ABS(y - l_extendedprice))
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_quantity AS DOUBLE),
               CAST(l_extendedprice AS DOUBLE)
        FROM lineitem WHERE l_orderkey <= 3
    """).fetchall()

    n = len(data)
    t_build = time.perf_counter()
    oracle_solver.create_model("abs_multiple")
    xnames = [f"x_{i}" for i in range(n)]
    ynames = [f"y_{i}" for i in range(n)]
    dx_names = [f"dx_{i}" for i in range(n)]
    dy_names = [f"dy_{i}" for i in range(n)]
    for xn in xnames:
        oracle_solver.add_variable(xn, VarType.CONTINUOUS, lb=0.0)
    for yn in ynames:
        oracle_solver.add_variable(yn, VarType.CONTINUOUS, lb=0.0)
    for dn in dx_names:
        oracle_solver.add_variable(dn, VarType.CONTINUOUS, lb=0.0)
    for dn in dy_names:
        oracle_solver.add_variable(dn, VarType.CONTINUOUS, lb=0.0)

    oracle_solver.add_constraint(
        {xnames[i]: 1.0 for i in range(n)}, "=", 50.0, name="sum_x",
    )
    oracle_solver.add_constraint(
        {ynames[i]: 1.0 for i in range(n)}, "=", 10000.0, name="sum_y",
    )
    for i in range(n):
        qty, price = data[i][2], data[i][3]
        # dx_i >= x_i - qty, dx_i >= qty - x_i
        oracle_solver.add_constraint(
            {dx_names[i]: 1.0, xnames[i]: -1.0}, ">=", -qty, name=f"absx_pos_{i}",
        )
        oracle_solver.add_constraint(
            {dx_names[i]: 1.0, xnames[i]: 1.0}, ">=", qty, name=f"absx_neg_{i}",
        )
        # dy_i >= y_i - price, dy_i >= price - y_i
        oracle_solver.add_constraint(
            {dy_names[i]: 1.0, ynames[i]: -1.0}, ">=", -price, name=f"absy_pos_{i}",
        )
        oracle_solver.add_constraint(
            {dy_names[i]: 1.0, ynames[i]: 1.0}, ">=", price, name=f"absy_neg_{i}",
        )
    obj = {}
    for i in range(n):
        obj[dx_names[i]] = 1.0
        obj[dy_names[i]] = 1.0
    oracle_solver.set_objective(obj, ObjSense.MINIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    assert result.status == SolverStatus.OPTIMAL
    ci = {name: i for i, name in enumerate(decidb_cols)}
    decidb_obj = sum(
        abs(float(row[ci["x"]]) - float(row[ci["l_quantity"]]))
        + abs(float(row[ci["y"]]) - float(row[ci["l_extendedprice"]]))
        for row in decidb_result
    )
    assert abs(decidb_obj - result.objective_value) <= 1e-2, (
        f"Objective mismatch: DecidB={decidb_obj:.6f}, Oracle={result.objective_value:.6f}"
    )

    perf_tracker.record(
        "abs_multiple", decidb_time, build_time,
        result.solve_time_seconds, n, n * 4, 2 + n * 4,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )


@pytest.mark.correctness
def test_abs_no_decide_var(decidb_cli, duckdb_conn):
    """ABS without DECIDE variable is regular SQL — should not be rewritten."""
    sql = """
        SELECT l_orderkey, ABS(l_quantity - 25) AS deviation
        FROM lineitem
        WHERE l_orderkey <= 3
    """
    decidb_result, decidb_cols = decidb_cli.execute(sql)

    expected = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               ABS(l_quantity - 25) AS deviation
        FROM lineitem WHERE l_orderkey <= 3
    """).fetchall()

    assert len(decidb_result) == len(expected)
    for pr, er in zip(
        sorted(decidb_result, key=lambda r: r[0]),
        sorted(expected, key=lambda r: r[0]),
    ):
        assert abs(float(pr[1]) - float(er[1])) <= 1e-6


@pytest.mark.var_boolean
@pytest.mark.var_real
@pytest.mark.cons_aggregate
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_abs_mixed_vars(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Mixed BOOLEAN + REAL with ABS on REAL variable only."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity, s, w
        FROM lineitem
        WHERE l_orderkey <= 5
        DECIDE s IS BOOLEAN, w IS REAL
        SUCH THAT SUM(s) >= 5
            AND SUM(w) = 100
        MINIMIZE SUM(ABS(w - l_quantity))
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_extendedprice AS DOUBLE),
               CAST(l_quantity AS DOUBLE)
        FROM lineitem WHERE l_orderkey <= 5
    """).fetchall()

    n = len(data)
    t_build = time.perf_counter()
    oracle_solver.create_model("abs_mixed")
    snames = [f"s_{i}" for i in range(n)]
    wnames = [f"w_{i}" for i in range(n)]
    dnames = [f"d_{i}" for i in range(n)]
    for sn in snames:
        oracle_solver.add_variable(sn, VarType.BINARY)
    for wn in wnames:
        oracle_solver.add_variable(wn, VarType.CONTINUOUS, lb=0.0)
    for dn in dnames:
        oracle_solver.add_variable(dn, VarType.CONTINUOUS, lb=0.0)

    oracle_solver.add_constraint(
        {snames[i]: 1.0 for i in range(n)}, ">=", 5.0, name="min_selected",
    )
    oracle_solver.add_constraint(
        {wnames[i]: 1.0 for i in range(n)}, "=", 100.0, name="sum_w",
    )
    for i in range(n):
        qty = data[i][3]
        oracle_solver.add_constraint(
            {dnames[i]: 1.0, wnames[i]: -1.0}, ">=", -qty, name=f"abs_pos_{i}",
        )
        oracle_solver.add_constraint(
            {dnames[i]: 1.0, wnames[i]: 1.0}, ">=", qty, name=f"abs_neg_{i}",
        )
    oracle_solver.set_objective(
        {dnames[i]: 1.0 for i in range(n)}, ObjSense.MINIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    assert result.status == SolverStatus.OPTIMAL
    ci = {name: i for i, name in enumerate(decidb_cols)}
    decidb_obj = sum(
        abs(float(row[ci["w"]]) - float(row[ci["l_quantity"]]))
        for row in decidb_result
    )
    assert abs(decidb_obj - result.objective_value) <= 1e-4, (
        f"Objective mismatch: DecidB={decidb_obj:.6f}, Oracle={result.objective_value:.6f}"
    )
    # Check boolean constraint
    s_sum = sum(1 for row in decidb_result if float(row[ci["s"]]) >= 0.5)
    assert s_sum >= 5, f"Boolean constraint violated: SUM(s) = {s_sum}"

    perf_tracker.record(
        "abs_mixed", decidb_time, build_time,
        result.solve_time_seconds, n, n * 3, 2 + n * 2,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )


@pytest.mark.var_real
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_real_abs_fractional_target_oracle(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """``SUM(ABS(x - 2.5)) <= K`` with IS REAL and a non-integer target constant.

    Regression test for the integer-step rewrite sweep. ABS linearization uses
    bounded-auxiliary constraints (``d >= x - 2.5`` and ``d >= -(x - 2.5)``) —
    no ±1 step — so a fractional constant inside the ABS must not trigger any
    coefficient-integrality shortcut. Distinct from the existing
    ``test_abs_constraint_aggregate`` which uses a column reference (integer-
    valued in the TPC-H fixture) rather than an explicit fractional offset.
    """
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, x
        FROM lineitem WHERE l_orderkey < 20
        DECIDE x IS REAL
        SUCH THAT x <= 5.0
            AND SUM(ABS(x - 2.5)) <= 10
        MAXIMIZE SUM(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_extendedprice AS DOUBLE)
        FROM lineitem WHERE l_orderkey < 20
    """).fetchall()
    n = len(data)

    t_build = time.perf_counter()
    oracle_solver.create_model("real_abs_fractional")
    vnames = [f"x_{i}" for i in range(n)]
    dnames = [f"d_{i}" for i in range(n)]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.CONTINUOUS, lb=0.0, ub=5.0)
    for dn in dnames:
        oracle_solver.add_variable(dn, VarType.CONTINUOUS, lb=0.0)
    # d_i >= x_i - 2.5 ; d_i >= -(x_i - 2.5)
    for i in range(n):
        oracle_solver.add_constraint(
            {dnames[i]: 1.0, vnames[i]: -1.0}, ">=", -2.5, name=f"abs_pos_{i}",
        )
        oracle_solver.add_constraint(
            {dnames[i]: 1.0, vnames[i]: 1.0}, ">=", 2.5, name=f"abs_neg_{i}",
        )
    oracle_solver.add_constraint(
        {dnames[i]: 1.0 for i in range(n)}, "<=", 10.0, name="abs_cap",
    )
    oracle_solver.set_objective(
        {vnames[i]: data[i][2] for i in range(n)}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    assert result.status == SolverStatus.OPTIMAL
    ci = {name: i for i, name in enumerate(decidb_cols)}
    total_dev = sum(abs(float(row[ci["x"]]) - 2.5) for row in decidb_rows)
    assert total_dev <= 10.0 + 1e-4, f"ABS cap violated: {total_dev}"

    decidb_obj = sum(
        float(row[ci["x"]]) * float(row[ci["l_extendedprice"]]) for row in decidb_rows
    )
    assert abs(decidb_obj - result.objective_value) <= 1e-2, (
        f"Objective mismatch: DecidB={decidb_obj:.6f}, Oracle={result.objective_value:.6f}"
    )

    perf_tracker.record(
        "real_abs_fractional_target", decidb_time, build_time,
        result.solve_time_seconds, n, n * 2, 1 + n * 2,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )


@pytest.mark.var_real
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_abs_maximize_objective_basic(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """MAXIMIZE SUM(ABS(x - l_quantity)): Big-M reformulation must return correct optimum.

    Regression test for the unsound MAXIMIZE+ABS linearization bug. Previously
    the optimizer emitted only aux >= inner and aux >= -inner (lower-envelope),
    which left aux unbounded under MAXIMIZE, producing a spurious 'unbounded' error.
    The fix adds two Big-M upper-bound constraints that pin aux = |inner| exactly.
    """
    sql = """
        SELECT l_orderkey, l_linenumber, l_quantity, x
        FROM lineitem
        WHERE l_orderkey <= 3
        DECIDE x IS REAL
        SUCH THAT SUM(x) = 100 AND x <= 50
        MAXIMIZE SUM(ABS(x - l_quantity))
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

    n = len(data)
    max_qty = max(row[2] for row in data)
    M = 50.0 + max_qty  # safe upper bound on |x_i - qty_i|: x in [0,50], qty in data

    t_build = time.perf_counter()
    oracle_solver.create_model("abs_maximize_basic")
    xnames = [f"x_{i}" for i in range(n)]
    dnames = [f"d_{i}" for i in range(n)]
    ynames = [f"y_{i}" for i in range(n)]
    for xn in xnames:
        oracle_solver.add_variable(xn, VarType.CONTINUOUS, lb=0.0, ub=50.0)
    for dn in dnames:
        oracle_solver.add_variable(dn, VarType.CONTINUOUS, lb=0.0)
    for yn in ynames:
        oracle_solver.add_variable(yn, VarType.BINARY)

    # SUM(x) = 100
    oracle_solver.add_constraint(
        {xnames[i]: 1.0 for i in range(n)}, "=", 100.0, name="sum_eq",
    )
    # ABS Big-M formulation: all four constraints per row
    for i in range(n):
        qty = data[i][2]
        two_M = 2.0 * M
        # Lower bounds (same as MINIMIZE)
        oracle_solver.add_constraint(
            {dnames[i]: 1.0, xnames[i]: -1.0}, ">=", -qty, name=f"abs_pos_{i}",
        )
        oracle_solver.add_constraint(
            {dnames[i]: 1.0, xnames[i]: 1.0}, ">=", qty, name=f"abs_neg_{i}",
        )
        # Upper bounds (new for MAXIMIZE): pin d_i = |x_i - qty_i|
        # d_i <= (x_i - qty_i) + 2M*(1-y_i)  =>  d_i - x_i + 2M*y_i <= -qty_i + 2M
        oracle_solver.add_constraint(
            {dnames[i]: 1.0, xnames[i]: -1.0, ynames[i]: two_M}, "<=",
            -qty + two_M, name=f"abs_ub1_{i}",
        )
        # d_i <= -(x_i - qty_i) + 2M*y_i  =>  d_i + x_i - 2M*y_i <= qty_i
        oracle_solver.add_constraint(
            {dnames[i]: 1.0, xnames[i]: 1.0, ynames[i]: -two_M}, "<=",
            qty, name=f"abs_ub2_{i}",
        )
    oracle_solver.set_objective(
        {dnames[i]: 1.0 for i in range(n)}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    assert result.status == SolverStatus.OPTIMAL
    decidb_obj = _compute_abs_objective(decidb_result, decidb_cols, "x", "l_quantity")
    assert abs(decidb_obj - result.objective_value) <= 1e-3, (
        f"Objective mismatch: DecidB={decidb_obj:.6f}, Oracle={result.objective_value:.6f}"
    )

    perf_tracker.record(
        "abs_maximize_basic", decidb_time, build_time,
        result.solve_time_seconds, n, n * 3, 1 + n * 4,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )


@pytest.mark.var_real
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_abs_maximize_no_bound_raises(decidb_cli):
    """MAXIMIZE SUM(ABS(x - l_quantity)) without an upper bound on x must raise.

    The Big-M computation requires a finite upper bound on the decision variable
    to pin aux = |inner|. Without it, there is no valid M and DecidB must raise
    an InvalidInputException naming the unbounded variable.
    """
    sql = """
        SELECT l_quantity, x
        FROM lineitem
        WHERE l_orderkey <= 3
        DECIDE x IS REAL
        SUCH THAT SUM(x) = 100
        MAXIMIZE SUM(ABS(x - l_quantity))
    """
    with pytest.raises(Exception, match="finite bound"):
        decidb_cli.execute(sql)
