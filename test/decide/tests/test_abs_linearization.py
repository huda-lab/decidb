"""Tests for ABS() linearization over decision variables.

Covers:
  - test_abs_objective_basic: MINIMIZE SUM(ABS(x - col)) with(REAL)
  - test_abs_objective_with_when: ABS in objective with WHEN condition
  - test_abs_objective_with_per: ABS in objective with PER grouping
  - test_abs_constraint_per_row: ABS(x - col) <= tolerance (per-row)
  - test_abs_constraint_aggregate: SUM(ABS(x - col)) <= total_tolerance
  - test_abs_constraint_aggregate_with_when: aggregate constraint with expression-level WHEN
  - test_abs_multiple_terms: two ABS terms in one expression
  - test_abs_no_decide_var: ABS without decide variable (regular SQL)
  - test_abs_mixed_vars: BOOLEAN + REAL with ABS on REAL only
  - test_abs_maximize_objective_basic: MAXIMIZE SUM(ABS(x - col)) with Big-M fix
  - test_abs_maximize_bound_inferred_from_sum: MAXIMIZE SUM(ABS(x - col)) with no
    explicit bound but SUM(x)=K; the bound is inferred from the constraint so it solves
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
        DECIDE new_qty(REAL)
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
        DECIDE new_qty(REAL)
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
        DECIDE new_qty(REAL)
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
        DECIDE new_qty(REAL)
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
        DECIDE new_qty(REAL)
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
        DECIDE new_qty(REAL)
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
        DECIDE x(REAL), y(REAL)
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
        DECIDE s(BOOL), w(REAL)
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
    """``SUM(ABS(x - 2.5)) <= K`` with(REAL) and a non-integer target constant.

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
        DECIDE x(REAL)
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
        DECIDE x(REAL)
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
def test_abs_maximize_bound_inferred_from_sum(decidb_cli, duckdb_conn):
    """MAXIMIZE SUM(ABS(x - l_quantity)) with no explicit bound but SUM(x) = 100.

    x has no declared upper bound, but SUM(x) = 100 with x >= 0 implies x <= 100.
    Implied-bound propagation derives that bound, so the ABS-maximize sign-indicator
    Big-M is finite and the query solves — it no longer needs an explicit bound
    (the previous behavior was to reject this query with a "finite bound" error).

    Ground truth (analytical): maximizing the convex SUM|x_i - q_i| over the simplex
    {SUM(x) = 100, x >= 0} is attained at a vertex (all 100 on one row, rest 0), so
    the optimum is 100 + SUM(q) - 2*min(q) (every q_i < 100 for l_orderkey <= 3).
    """
    sql = """
        SELECT l_quantity, x
        FROM lineitem
        WHERE l_orderkey <= 3
        DECIDE x(REAL)
        SUCH THAT SUM(x) = 100
        MAXIMIZE SUM(ABS(x - l_quantity))
    """
    decidb_result, decidb_cols = decidb_cli.execute(sql)

    qtys = [
        r[0]
        for r in duckdb_conn.execute(
            "SELECT CAST(l_quantity AS DOUBLE) FROM lineitem WHERE l_orderkey <= 3"
        ).fetchall()
    ]
    expected = 100.0 + sum(qtys) - 2.0 * min(qtys)

    decidb_obj = _compute_abs_objective(decidb_result, decidb_cols, "x", "l_quantity")
    assert abs(decidb_obj - expected) <= 1e-4, (
        f"Objective mismatch: DecidB={decidb_obj:.6f}, expected={expected:.6f}"
    )


@pytest.mark.var_real
@pytest.mark.cons_perrow
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_abs_constraint_per_row_hard_ge(decidb_cli, duckdb_conn):
    """Per-row hard direction ``ABS(x - l_quantity) >= K`` (Big-M sign-indicator path).

    Oracle-verified by closed form. K is chosen strictly greater than every
    l_quantity, so the near branch (``x <= qty - K``) is negative and infeasible
    for every row; each row is forced onto the far branch ``x >= qty + K``.
    MINIMIZE SUM(x) then pins ``x_i = qty_i + K`` exactly. A wrong Big-M sign
    would admit ``x_i`` inside the forbidden band and undershoot the optimum.
    Complements C33-C37 in stress_queries (smoke-only) with an oracle check.
    """
    qtys = [
        r[0]
        for r in duckdb_conn.execute(
            "SELECT CAST(l_quantity AS DOUBLE) FROM lineitem WHERE l_orderkey <= 3"
        ).fetchall()
    ]
    k = max(qtys) + 10.0
    ub = 2.0 * max(qtys) + k + 10.0
    sql = f"""
        SELECT l_quantity, x
        FROM lineitem WHERE l_orderkey <= 3
        DECIDE x(REAL)
        SUCH THAT x <= {ub} AND ABS(x - l_quantity) >= {k}
        MINIMIZE SUM(x)
    """
    rows, cols = decidb_cli.execute(sql)
    ci = {name: i for i, name in enumerate(cols)}
    for row in rows:
        qty = float(row[ci["l_quantity"]])
        x = float(row[ci["x"]])
        assert abs(x - (qty + k)) <= 1e-4, (
            f"expected x={qty + k:.4f}, got {x:.4f} (qty={qty})"
        )
        assert abs(x - qty) >= k - 1e-4, f"hard ABS constraint violated: |{x}-{qty}| < {k}"


@pytest.mark.var_real
@pytest.mark.cons_perrow
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_abs_constraint_per_row_hard_eq(decidb_cli, duckdb_conn):
    """Per-row ``ABS(x - l_quantity) = K`` exactly (both Big-M bounds active).

    Equality pins ``x_i`` to one of ``qty_i +/- K``. MINIMIZE SUM(x) selects the
    lower root ``qty_i - K`` where it stays non-negative, else the upper root
    ``qty_i + K`` (rows with ``qty < K``, e.g. the qty=2 lineitem in order 3).
    Verifies the equality path exercises both the lower- and upper-envelope
    Big-M constraints, not just one direction.
    """
    k = 5.0
    qtys = [
        r[0]
        for r in duckdb_conn.execute(
            "SELECT CAST(l_quantity AS DOUBLE) FROM lineitem WHERE l_orderkey <= 3"
        ).fetchall()
    ]
    ub = max(qtys) + k + 10.0
    sql = f"""
        SELECT l_quantity, x
        FROM lineitem WHERE l_orderkey <= 3
        DECIDE x(REAL)
        SUCH THAT x <= {ub} AND ABS(x - l_quantity) = {k}
        MINIMIZE SUM(x)
    """
    rows, cols = decidb_cli.execute(sql)
    ci = {name: i for i, name in enumerate(cols)}
    for row in rows:
        qty = float(row[ci["l_quantity"]])
        x = float(row[ci["x"]])
        expected = qty - k if qty - k >= 0.0 else qty + k
        assert abs(x - expected) <= 1e-4, (
            f"expected x={expected:.4f}, got {x:.4f} (qty={qty})"
        )
        assert abs(abs(x - qty) - k) <= 1e-4, f"ABS equality violated: ||{x}-{qty}| - {k}|"


@pytest.mark.var_real
@pytest.mark.cons_aggregate
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_abs_constraint_aggregate_hard_ge(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Aggregate hard direction ``SUM(ABS(x - l_quantity)) >= K``.

    This is the sign-indicator path where each ``d_i`` must be pinned to exactly
    ``|x_i - qty_i|`` (both Big-M bounds), otherwise the aggregate lower bound is
    trivially satisfiable by letting the auxiliaries float up. K is set above the
    dispersion at ``x = 0`` (``SUM(qty)``), so the constraint binds and the solver
    must push some ``x_i`` away from their quantities. Oracle mirrors the pinned
    Big-M model from ``test_abs_maximize_objective_basic``.
    """
    data = duckdb_conn.execute("""
        SELECT CAST(l_quantity AS DOUBLE)
        FROM lineitem WHERE l_orderkey <= 3
    """).fetchall()
    qtys = [r[0] for r in data]
    n = len(qtys)
    ub = 500.0
    k = sum(qtys) + 100.0  # strictly above the x=0 dispersion, so the bound binds
    m = ub + max(qtys)  # safe bound on |x_i - qty_i| for x in [0, ub]

    sql = f"""
        SELECT l_quantity, x
        FROM lineitem WHERE l_orderkey <= 3
        DECIDE x(REAL)
        SUCH THAT x <= {ub} AND SUM(ABS(x - l_quantity)) >= {k}
        MINIMIZE SUM(x)
    """
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    t_build = time.perf_counter()
    oracle_solver.create_model("abs_constraint_agg_hard_ge")
    xnames = [f"x_{i}" for i in range(n)]
    dnames = [f"d_{i}" for i in range(n)]
    ynames = [f"y_{i}" for i in range(n)]
    for xn in xnames:
        oracle_solver.add_variable(xn, VarType.CONTINUOUS, lb=0.0, ub=ub)
    for dn in dnames:
        oracle_solver.add_variable(dn, VarType.CONTINUOUS, lb=0.0)
    for yn in ynames:
        oracle_solver.add_variable(yn, VarType.BINARY)

    two_m = 2.0 * m
    for i in range(n):
        qty = qtys[i]
        # Lower envelope: d_i >= |x_i - qty_i|
        oracle_solver.add_constraint(
            {dnames[i]: 1.0, xnames[i]: -1.0}, ">=", -qty, name=f"abs_pos_{i}",
        )
        oracle_solver.add_constraint(
            {dnames[i]: 1.0, xnames[i]: 1.0}, ">=", qty, name=f"abs_neg_{i}",
        )
        # Upper envelope (sign indicator): pin d_i = |x_i - qty_i|
        oracle_solver.add_constraint(
            {dnames[i]: 1.0, xnames[i]: -1.0, ynames[i]: two_m}, "<=",
            -qty + two_m, name=f"abs_ub1_{i}",
        )
        oracle_solver.add_constraint(
            {dnames[i]: 1.0, xnames[i]: 1.0, ynames[i]: -two_m}, "<=",
            qty, name=f"abs_ub2_{i}",
        )
    # SUM(d_i) >= K
    oracle_solver.add_constraint(
        {dnames[i]: 1.0 for i in range(n)}, ">=", k, name="agg_hard",
    )
    oracle_solver.set_objective(
        {xnames[i]: 1.0 for i in range(n)}, ObjSense.MINIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    assert result.status == SolverStatus.OPTIMAL
    ci = {name: i for i, name in enumerate(decidb_cols)}
    # DecidB must respect the aggregate hard bound
    total_dev = sum(
        abs(float(row[ci["x"]]) - float(row[ci["l_quantity"]])) for row in decidb_rows
    )
    assert total_dev >= k - 1e-3, f"aggregate ABS lower bound violated: {total_dev} < {k}"

    decidb_obj = sum(float(row[ci["x"]]) for row in decidb_rows)
    assert abs(decidb_obj - result.objective_value) <= 1e-2, (
        f"Objective mismatch: DecidB={decidb_obj:.6f}, Oracle={result.objective_value:.6f}"
    )

    perf_tracker.record(
        "abs_constraint_agg_hard_ge", decidb_time, build_time,
        result.solve_time_seconds, n, n * 3, 1 + n * 4,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )


@pytest.mark.var_real
@pytest.mark.cons_aggregate
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_abs_min_geq_per_row_hard(decidb_cli):
    """``MIN(ABS(x - 5)) >= K`` — easy-MIN strip to per-row hard ABS.

    ``MIN(...) >= K`` means *every* row's ``|x - 5| >= K``. With ``x in [4, 20]``
    the near branch (``x <= 2``) is excluded, so each row is forced to ``x >= 8``;
    MINIMIZE SUM(x) pins every ``x`` at exactly 8 (``|8 - 5| = 3``). Exercises the
    MIN-strip → per-row Big-M sign-indicator path over ABS auxiliaries.
    """
    rows, cols = decidb_cli.execute("""
        SELECT id, x FROM (VALUES (1),(2),(3)) t(id)
        DECIDE x(REAL)
        SUCH THAT x >= 4 AND x <= 20 AND MIN(ABS(x - 5)) >= 3
        MINIMIZE SUM(x)
    """)
    ci = {c: i for i, c in enumerate(cols)}
    xs = [float(r[ci["x"]]) for r in rows]
    assert xs, "no rows returned"
    for xv in xs:
        assert abs(xv - 8.0) <= 1e-4, f"expected all x=8, got {xs}"
        assert abs(xv - 5.0) >= 3.0 - 1e-4


@pytest.mark.var_real
@pytest.mark.cons_aggregate
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_abs_max_geq_aggregate_hard(decidb_cli):
    """``MAX(ABS(x - 5)) >= K`` — aggregate hard MAX layered over ABS Big-M.

    ``MAX(...) >= K`` means *at least one* row's ``|x - 5| >= K``. This stacks the
    outer hard-MAX indicator (``SUM(y) >= 1``) on top of each row's ABS
    sign-indicator — the non-trivial interaction. With ``x in [4, 20]`` only one
    row must reach ``x = 8`` (``|.| = 3``); MINIMIZE SUM(x) keeps the other two at
    4, so SUM = 8 + 4 + 4 = 16 and exactly one row satisfies ``|x - 5| >= 3``.
    """
    rows, cols = decidb_cli.execute("""
        SELECT id, x FROM (VALUES (1),(2),(3)) t(id)
        DECIDE x(REAL)
        SUCH THAT x >= 4 AND x <= 20 AND MAX(ABS(x - 5)) >= 3
        MINIMIZE SUM(x)
    """)
    ci = {c: i for i, c in enumerate(cols)}
    xs = [float(r[ci["x"]]) for r in rows]
    assert abs(sum(xs) - 16.0) <= 1e-3, f"expected SUM=16, got {xs}"
    satisfied = [xv for xv in xs if abs(xv - 5.0) >= 3.0 - 1e-4]
    assert len(satisfied) == 1, f"expected exactly one row with |x-5|>=3, got {xs}"
    assert max(abs(xv - 5.0) for xv in xs) >= 3.0 - 1e-4


@pytest.mark.var_real
@pytest.mark.cons_perrow
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_abs_between_both_bounds(decidb_cli):
    """``ABS(x - 5) BETWEEN 2 AND 8`` — both bounds at once.

    The upper bound ``|x - 5| <= 8`` is easy (two linear rows); the lower bound
    ``|x - 5| >= 2`` is hard (disjunction → Big-M sign indicator). Feasible set is
    ``x in [-3, 3] U [7, 13]``; with ``x <= 20`` and non-negativity, MAXIMIZE
    SUM(x) pins every ``x`` at the top of the upper band, ``x = 13``.
    """
    rows, cols = decidb_cli.execute("""
        SELECT id, x FROM (VALUES (1),(2)) t(id)
        DECIDE x(REAL)
        SUCH THAT x <= 20 AND ABS(x - 5) BETWEEN 2 AND 8
        MAXIMIZE SUM(x)
    """)
    ci = {c: i for i, c in enumerate(cols)}
    xs = [float(r[ci["x"]]) for r in rows]
    assert xs, "no rows returned"
    for xv in xs:
        assert abs(xv - 13.0) <= 1e-4, f"expected x=13, got {xv}"
        dev = abs(xv - 5.0)
        assert 2.0 - 1e-4 <= dev <= 8.0 + 1e-4


@pytest.mark.var_real
@pytest.mark.cons_perrow
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_abs_on_both_sides(decidb_cli):
    """``ABS(x - 3) <= ABS(x - 9)`` — ABS on both sides of the comparison.

    Feasible where ``x`` is at least as close to 3 as to 9, i.e. ``x <= 6`` (the
    midpoint). MAXIMIZE SUM(x) with ``x <= 20`` pins ``x = 6``
    (``|6 - 3| = |6 - 9| = 3``).
    """
    rows, cols = decidb_cli.execute("""
        SELECT id, x FROM (VALUES (1),(2)) t(id)
        DECIDE x(REAL)
        SUCH THAT x <= 20 AND ABS(x - 3) <= ABS(x - 9)
        MAXIMIZE SUM(x)
    """)
    ci = {c: i for i, c in enumerate(cols)}
    xs = [float(r[ci["x"]]) for r in rows]
    assert xs, "no rows returned"
    for xv in xs:
        assert abs(xv - 6.0) <= 1e-4, f"expected x=6, got {xv}"
        assert abs(xv - 3.0) <= abs(xv - 9.0) + 1e-4


@pytest.mark.var_real
@pytest.mark.cons_aggregate
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_abs_data_column_coefficient_may_be_negative(decidb_cli, oracle_solver):
    """SUM(w * ABS(x - t)) <= K where w is a column that goes negative.

    A column's sign is not known at plan time. On a row where w < 0, enlarging
    that row's ABS auxiliary makes the constraint easier to satisfy, so an
    auxiliary held only by the cheap one-sided bounds (d >= u, d >= -u) drifts
    above |x - t| and the constraint stops meaning what it says. Such an
    auxiliary needs the Big-M encoding that pins d to the exact absolute value.

    Regression pin: while the sign walk assumed an unknown factor was positive,
    DecidB answered x = 0 on every row here — the constraint was satisfied
    entirely by letting the auxiliary float, without moving x at all.
    """
    rows_data = [(-1.0, 3.0), (2.0, 1.0)]  # (w, t) per row
    decidb_rows, cols = decidb_cli.execute("""
        SELECT id, x FROM (VALUES (1, -1, 3), (2, 2, 1)) t(id, w, tgt)
        DECIDE x(REAL)
        SUCH THAT SUM(w * ABS(x - tgt)) <= -5 AND x <= 10 AND x >= 0
        MINIMIZE SUM(x)
    """)
    ci = {name: i for i, name in enumerate(cols)}
    decidb_obj = sum(float(r[ci["x"]]) for r in decidb_rows)

    n = len(rows_data)
    big_m = 40.0  # any M >= max|x - t| = 9 over the declared box
    oracle_solver.create_model("abs_negative_data_coefficient")
    for i in range(n):
        oracle_solver.add_variable(f"x_{i}", VarType.CONTINUOUS, lb=0.0, ub=10.0)
        oracle_solver.add_variable(f"d_{i}", VarType.CONTINUOUS, lb=0.0, ub=big_m)
        oracle_solver.add_variable(f"y_{i}", VarType.BINARY)

    # d_i = |x_i - t_i| exactly: two lower bounds, plus two indicator-selected
    # upper bounds so the auxiliary cannot drift upward.
    for i, (_, t) in enumerate(rows_data):
        oracle_solver.add_constraint(
            {f"d_{i}": 1.0, f"x_{i}": -1.0}, ">=", -t, name=f"lo_pos_{i}")
        oracle_solver.add_constraint(
            {f"d_{i}": 1.0, f"x_{i}": 1.0}, ">=", t, name=f"lo_neg_{i}")
        oracle_solver.add_constraint(
            {f"d_{i}": 1.0, f"x_{i}": -1.0, f"y_{i}": big_m}, "<=", -t + big_m,
            name=f"up_pos_{i}")
        oracle_solver.add_constraint(
            {f"d_{i}": 1.0, f"x_{i}": 1.0, f"y_{i}": -big_m}, "<=", t,
            name=f"up_neg_{i}")

    oracle_solver.add_constraint(
        {f"d_{i}": rows_data[i][0] for i in range(n)}, "<=", -5.0,
        name="weighted_abs")
    oracle_solver.set_objective(
        {f"x_{i}": 1.0 for i in range(n)}, ObjSense.MINIMIZE)
    result = oracle_solver.solve()

    assert result.status == SolverStatus.OPTIMAL
    assert abs(decidb_obj - result.objective_value) <= 1e-4, (
        f"Objective mismatch: DecidB={decidb_obj:.6f}, "
        f"Oracle={result.objective_value:.6f}"
    )


# ===========================================================================
# Path-B (sign-indicator Big-M) composed with WHEN and PER
#
# The Big-M envelope that pins each `d_i` to exactly `|expr_i|` is emitted
# unconditionally, per row. WHEN and PER act on the *aggregate* that reads those
# auxiliaries — WHEN masks which ones enter the sum, PER partitions them into
# group sums. The two tests below check that composition: the pinning stays
# per-row while the aggregate is filtered or grouped.
# ===========================================================================


def _pin_abs_aux(oracle_solver, xname, dname, yname, target, two_m, tag):
    """Add `d = |x - target|` to the oracle: lower envelope plus the
    sign-indicator upper envelope that pins it (Path-B)."""
    oracle_solver.add_variable(dname, VarType.CONTINUOUS, lb=0.0)
    oracle_solver.add_variable(yname, VarType.BINARY)
    oracle_solver.add_constraint(
        {dname: 1.0, xname: -1.0}, ">=", -target, name=f"abs_pos_{tag}",
    )
    oracle_solver.add_constraint(
        {dname: 1.0, xname: 1.0}, ">=", target, name=f"abs_neg_{tag}",
    )
    oracle_solver.add_constraint(
        {dname: 1.0, xname: -1.0, yname: two_m}, "<=", -target + two_m,
        name=f"abs_ub1_{tag}",
    )
    oracle_solver.add_constraint(
        {dname: 1.0, xname: 1.0, yname: -two_m}, "<=", target,
        name=f"abs_ub2_{tag}",
    )


@pytest.mark.var_real
@pytest.mark.cons_aggregate
@pytest.mark.when_constraint
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_abs_constraint_aggregate_hard_ge_with_when(decidb_cli, duckdb_conn,
                                                    oracle_solver, perf_tracker):
    """`SUM(ABS(x - l_quantity)) >= K WHEN l_linenumber <= 2`.

    The hard direction needs each `d_i` pinned by the sign-indicator Big-M, and
    that pinning is per-row and unconditional; WHEN only decides which `d_i`
    enter the aggregate. K is set just above the masked rows' dispersion at
    `x = 0` so the bound binds, but *below* the dispersion the unmasked rows
    would add — so an implementation that summed every `d_i` would find the
    constraint already satisfied and leave every `x` at 0.
    """
    data = duckdb_conn.execute("""
        SELECT CAST(l_linenumber AS BIGINT), CAST(l_quantity AS DOUBLE)
        FROM lineitem WHERE l_orderkey <= 3
    """).fetchall()
    n = len(data)
    masked = [i for i in range(n) if data[i][0] <= 2]
    ub = 500.0
    k = sum(data[i][1] for i in masked) + 100.0
    assert k < sum(row[1] for row in data), \
        "K must sit below the all-rows dispersion, or the mask would not matter"
    m = ub + max(row[1] for row in data)

    sql = f"""
        SELECT l_orderkey, l_linenumber, l_quantity, x
        FROM lineitem WHERE l_orderkey <= 3
        DECIDE x(REAL)
        SUCH THAT x <= {ub}
            AND SUM(ABS(x - l_quantity)) >= {k} WHEN l_linenumber <= 2
        MINIMIZE SUM(x)
    """
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0
    assert len(decidb_rows) == n, "oracle and DecidB must see the same rows"

    t_build = time.perf_counter()
    oracle_solver.create_model("abs_agg_hard_ge_when")
    for i in range(n):
        oracle_solver.add_variable(f"x_{i}", VarType.CONTINUOUS, lb=0.0, ub=ub)
        _pin_abs_aux(oracle_solver, f"x_{i}", f"d_{i}", f"y_{i}",
                     data[i][1], 2.0 * m, str(i))
    oracle_solver.add_constraint(
        {f"d_{i}": 1.0 for i in masked}, ">=", k, name="agg_hard_masked",
    )
    oracle_solver.set_objective(
        {f"x_{i}": 1.0 for i in range(n)}, ObjSense.MINIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL

    ci = {name: i for i, name in enumerate(decidb_cols)}
    masked_dev = sum(
        abs(float(row[ci["x"]]) - float(row[ci["l_quantity"]]))
        for row in decidb_rows if int(row[ci["l_linenumber"]]) <= 2
    )
    assert masked_dev >= k - 1e-3, \
        f"masked aggregate ABS lower bound violated: {masked_dev} < {k}"

    decidb_obj = sum(float(row[ci["x"]]) for row in decidb_rows)
    assert decidb_obj > 1e-3, \
        "every x stayed at 0 — the aggregate summed the unmasked auxiliaries too"
    assert abs(decidb_obj - result.objective_value) <= 1e-2, (
        f"Objective mismatch: DecidB={decidb_obj:.6f}, "
        f"Oracle={result.objective_value:.6f}"
    )

    perf_tracker.record(
        "abs_agg_hard_ge_when", decidb_time, build_time,
        result.solve_time_seconds, n, n * 3, 1 + n * 4,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )


@pytest.mark.var_real
@pytest.mark.cons_aggregate
@pytest.mark.per_clause
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_abs_constraint_aggregate_hard_ge_with_per(decidb_cli, oracle_solver,
                                                   perf_tracker):
    """`SUM(ABS(x - target)) >= 50 PER grp`.

    Each group carries its own hard lower bound over its own pinned auxiliaries.
    At `x = 0` group A's dispersion is 25 and group B's is 45, so *both* groups
    bind and each has to push one `x` past its target. The pooled reading is
    25 + 45 = 70, already above 50, so a global-scoping bug would leave every
    `x` at 0 and report objective 0 instead of 90.
    """
    data = [(1, 'A', 10.0), (2, 'A', 15.0), (3, 'B', 20.0), (4, 'B', 25.0)]
    n = len(data)
    ub = 100.0
    k = 50.0

    sql = f"""
        WITH data AS (
            SELECT 1 AS id, 'A' AS grp, 10.0 AS target UNION ALL
            SELECT 2, 'A', 15.0 UNION ALL
            SELECT 3, 'B', 20.0 UNION ALL
            SELECT 4, 'B', 25.0
        )
        SELECT id, grp, target, x
        FROM data
        DECIDE x(REAL)
        SUCH THAT x <= {ub} AND SUM(ABS(x - target)) >= {k} PER grp
        MINIMIZE SUM(x)
    """
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0
    assert len(decidb_rows) == n

    m = ub + max(row[2] for row in data)
    t_build = time.perf_counter()
    oracle_solver.create_model("abs_agg_hard_ge_per")
    for i in range(n):
        oracle_solver.add_variable(f"x_{i}", VarType.CONTINUOUS, lb=0.0, ub=ub)
        _pin_abs_aux(oracle_solver, f"x_{i}", f"d_{i}", f"y_{i}",
                     data[i][2], 2.0 * m, str(i))
    for g in ("A", "B"):
        members = [i for i in range(n) if data[i][1] == g]
        # Every group must clear K on its own; pooling them would not.
        assert sum(data[i][2] for i in members) < k, \
            f"group {g} must not already clear K at x = 0"
        oracle_solver.add_constraint(
            {f"d_{i}": 1.0 for i in members}, ">=", k, name=f"agg_hard_{g}",
        )
    assert sum(row[2] for row in data) >= k, \
        "the pooled dispersion must clear K, or the bug would not be visible"
    oracle_solver.set_objective(
        {f"x_{i}": 1.0 for i in range(n)}, ObjSense.MINIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL

    ci = {name: i for i, name in enumerate(decidb_cols)}
    per_group = {}
    for row in decidb_rows:
        g = row[ci["grp"]]
        dev = abs(float(row[ci["x"]]) - float(row[ci["target"]]))
        per_group[g] = per_group.get(g, 0.0) + dev
    for g, dev in per_group.items():
        assert dev >= k - 1e-3, \
            f"group {g}: dispersion {dev} below the per-group bound {k}"

    decidb_obj = sum(float(row[ci["x"]]) for row in decidb_rows)
    assert abs(decidb_obj - result.objective_value) <= 1e-3, (
        f"Objective mismatch: DecidB={decidb_obj:.6f}, "
        f"Oracle={result.objective_value:.6f}"
    )

    perf_tracker.record(
        "abs_agg_hard_ge_per", decidb_time, build_time,
        result.solve_time_seconds, n, n * 3, 2 + n * 4,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )
