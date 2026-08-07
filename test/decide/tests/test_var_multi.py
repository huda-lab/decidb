"""Tests for multiple decision variables (DECIDE x, y, ...).

Multiple DECIDE variables allow modeling richer problems where rows have
more than one decision per tuple.
"""

import time

import pytest

from solver.types import VarType, ObjSense
from comparison.compare import compare_solutions


@pytest.mark.var_multi
@pytest.mark.var_boolean
@pytest.mark.var_integer
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_two_variables_separate_constraints(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """DECIDE x(BOOL), y(INT) with independent constraints."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity, x, y
        FROM lineitem WHERE l_orderkey < 50
        DECIDE x(BOOL), y(INT)
        SUCH THAT SUM(x * l_quantity) <= 100
            AND y <= 5
            AND SUM(y) <= 20
        MAXIMIZE SUM(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_extendedprice AS DOUBLE),
               CAST(l_quantity AS DOUBLE)
        FROM lineitem WHERE l_orderkey < 50
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("var_multi_bool_int")
    n = len(data)
    for i in range(n):
        oracle_solver.add_variable(f"x_{i}", VarType.BINARY)
        oracle_solver.add_variable(f"y_{i}", VarType.INTEGER, lb=0.0, ub=5.0)

    oracle_solver.add_constraint(
        {f"x_{i}": data[i][3] for i in range(n)},
        "<=", 100.0, name="x_quantity_cap",
    )
    oracle_solver.add_constraint(
        {f"y_{i}": 1.0 for i in range(n)},
        "<=", 20.0, name="y_sum_cap",
    )
    oracle_solver.set_objective(
        {f"x_{i}": data[i][2] for i in range(n)},
        ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_rows, decidb_cols, result, data, ["x", "y"],
        coeff_fn=lambda row: {
            "x": float(row[decidb_cols.index("l_extendedprice")]),
            "y": 0.0,
        },
    )

    perf_tracker.record(
        "two_variables_separate_constraints", decidb_time, build_time,
        result.solve_time_seconds, n, 2 * n, 3,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.var_multi
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_two_boolean_variables(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """Two boolean variables with a cross-constraint."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity, x, y
        FROM lineitem WHERE l_orderkey < 50
        DECIDE x(BOOL), y(BOOL)
        SUCH THAT SUM(x * l_quantity) <= 100
            AND SUM(y * l_quantity) <= 200
        MAXIMIZE SUM(x * l_extendedprice + y * l_quantity)
    """
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_extendedprice AS DOUBLE),
               CAST(l_quantity AS DOUBLE)
        FROM lineitem WHERE l_orderkey < 50
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("var_multi_bool_bool")
    n = len(data)
    for i in range(n):
        oracle_solver.add_variable(f"x_{i}", VarType.BINARY)
        oracle_solver.add_variable(f"y_{i}", VarType.BINARY)

    oracle_solver.add_constraint(
        {f"x_{i}": data[i][3] for i in range(n)},
        "<=", 100.0, name="x_quantity_cap",
    )
    oracle_solver.add_constraint(
        {f"y_{i}": data[i][3] for i in range(n)},
        "<=", 200.0, name="y_quantity_cap",
    )
    objective = {}
    for i in range(n):
        objective[f"x_{i}"] = data[i][2]
        objective[f"y_{i}"] = data[i][3]
    oracle_solver.set_objective(objective, ObjSense.MAXIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_rows, decidb_cols, result, data, ["x", "y"],
        coeff_fn=lambda row: {
            "x": float(row[decidb_cols.index("l_extendedprice")]),
            "y": float(row[decidb_cols.index("l_quantity")]),
        },
    )

    perf_tracker.record(
        "two_boolean_variables", decidb_time, build_time,
        result.solve_time_seconds, n, 2 * n, 2,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.var_multi
@pytest.mark.var_integer
@pytest.mark.var_real
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_integer_real_paired(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """DECIDE x(INT), y(REAL) without a BOOLEAN in the mix.

    Guards the variable-type flag loop: with no boolean the solver must still
    set ``is_integer`` only on ``x`` (leaving ``y`` continuous). A regression
    that accidentally treats all vars as continuous would fail the x-vector
    integrality check; one that treats all as integer would fail fractional y.
    """
    data_sql = (
        "SELECT * FROM (VALUES "
        "(1, 3.0, 1.5), (2, 5.0, 2.5), (3, 2.0, 3.0), (4, 4.0, 1.0)"
        ") t(id, val_a, val_b)"
    )
    sql = f"""
        SELECT id, val_a, val_b, x, ROUND(y, 4) AS y FROM ({data_sql})
        DECIDE x(INT), y(REAL)
        SUCH THAT x <= 5 AND y <= 10 AND SUM(x + y) <= 20
        MAXIMIZE SUM(x * val_a + y * val_b)
    """
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute(
        f"SELECT id, CAST(val_a AS DOUBLE), CAST(val_b AS DOUBLE) FROM ({data_sql})"
    ).fetchall()
    n = len(data)

    t_build = time.perf_counter()
    oracle_solver.create_model("integer_real_paired")
    xnames = [f"x_{i}" for i in range(n)]
    ynames = [f"y_{i}" for i in range(n)]
    for xn in xnames:
        oracle_solver.add_variable(xn, VarType.INTEGER, lb=0.0, ub=5.0)
    for yn in ynames:
        oracle_solver.add_variable(yn, VarType.CONTINUOUS, lb=0.0, ub=10.0)

    combined = {}
    for i in range(n):
        combined[xnames[i]] = 1.0
        combined[ynames[i]] = 1.0
    oracle_solver.add_constraint(combined, "<=", 20.0, name="sum_x_plus_y_cap")

    obj = {}
    for i in range(n):
        obj[xnames[i]] = data[i][1]
        obj[ynames[i]] = data[i][2]
    oracle_solver.set_objective(obj, ObjSense.MAXIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_rows, decidb_cols, result, data, ["x", "y"],
        coeff_fn=lambda row: {
            "x": float(row[decidb_cols.index("val_a")]),
            "y": float(row[decidb_cols.index("val_b")]),
        },
    )
    perf_tracker.record(
        "integer_real_paired", decidb_time, build_time,
        result.solve_time_seconds, n, 2 * n, 3,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.var_multi
@pytest.mark.var_boolean
@pytest.mark.var_integer
@pytest.mark.var_real
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_three_decide_variables(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """Three-variable stress test for VarIndexer / physical_decide loop bounds.

    Hard-coded ``i < 2`` or analogous off-by-one bugs would only surface with
    three or more decision variables. One per type keeps the flag-setting
    combinatorics exercised.
    """
    data_sql = (
        "SELECT * FROM (VALUES "
        "(1, 1.0, 2.0, 3.0, 5.0, 2.0, 4.5),"
        "(2, 2.0, 1.0, 2.0, 3.0, 4.0, 2.5),"
        "(3, 1.5, 1.5, 1.5, 2.0, 2.0, 3.5),"
        "(4, 3.0, 0.5, 1.0, 4.0, 1.5, 1.0)"
        ") t(id, a, b, c, val_a, val_b, val_c)"
    )
    sql = f"""
        SELECT id, a, b, c, val_a, val_b, val_c, x, y, ROUND(z, 4) AS z
        FROM ({data_sql})
        DECIDE x(BOOL), y(INT), z(REAL)
        SUCH THAT SUM(x) <= 3
            AND y <= 5
            AND z <= 10.0
            AND SUM(x * a + y * b + z * c) <= 100
        MAXIMIZE SUM(x * val_a + y * val_b + z * val_c)
    """
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute(
        f"""SELECT id,
                   CAST(a AS DOUBLE), CAST(b AS DOUBLE), CAST(c AS DOUBLE),
                   CAST(val_a AS DOUBLE), CAST(val_b AS DOUBLE), CAST(val_c AS DOUBLE)
            FROM ({data_sql})"""
    ).fetchall()
    n = len(data)

    t_build = time.perf_counter()
    oracle_solver.create_model("three_decide_variables")
    xnames = [f"x_{i}" for i in range(n)]
    ynames = [f"y_{i}" for i in range(n)]
    znames = [f"z_{i}" for i in range(n)]
    for xn in xnames:
        oracle_solver.add_variable(xn, VarType.BINARY)
    for yn in ynames:
        oracle_solver.add_variable(yn, VarType.INTEGER, lb=0.0, ub=5.0)
    for zn in znames:
        oracle_solver.add_variable(zn, VarType.CONTINUOUS, lb=0.0, ub=10.0)

    oracle_solver.add_constraint(
        {xn: 1.0 for xn in xnames}, "<=", 3.0, name="sum_x_cap",
    )
    budget = {}
    for i in range(n):
        budget[xnames[i]] = data[i][1]
        budget[ynames[i]] = data[i][2]
        budget[znames[i]] = data[i][3]
    oracle_solver.add_constraint(budget, "<=", 100.0, name="weighted_budget")

    obj = {}
    for i in range(n):
        obj[xnames[i]] = data[i][4]
        obj[ynames[i]] = data[i][5]
        obj[znames[i]] = data[i][6]
    oracle_solver.set_objective(obj, ObjSense.MAXIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_rows, decidb_cols, result, data, ["x", "y", "z"],
        coeff_fn=lambda row: {
            "x": float(row[decidb_cols.index("val_a")]),
            "y": float(row[decidb_cols.index("val_b")]),
            "z": float(row[decidb_cols.index("val_c")]),
        },
    )
    perf_tracker.record(
        "three_decide_variables", decidb_time, build_time,
        result.solve_time_seconds, n, 3 * n, 4,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.var_multi
@pytest.mark.var_boolean
@pytest.mark.var_real
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_mixed_types_same_aggregate_term(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """BOOLEAN + REAL user-declared variables in the same SUM(...) term.

    The implicit mixed case (ABS auxiliary REAL alongside user BOOLEAN) is
    already exercised via `test_abs_linearization.py`. This test locks in
    the *explicit* user-declared variant of the same shape.
    """
    data_sql = (
        "SELECT * FROM (VALUES "
        "(1, 2.0, 1.5, 10.0, 3.0), (2, 1.0, 2.5, 5.0, 4.0), "
        "(3, 3.0, 0.5, 8.0, 2.5), (4, 2.5, 1.0, 7.0, 1.5)"
        ") t(id, col_a, col_b, val_x, val_y)"
    )
    sql = f"""
        SELECT id, col_a, col_b, val_x, val_y, x, ROUND(y, 4) AS y
        FROM ({data_sql})
        DECIDE x(BOOL), y(REAL)
        SUCH THAT y <= 10
            AND SUM(x * col_a + y * col_b) <= 50
        MAXIMIZE SUM(x * val_x + y * val_y)
    """
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute(
        f"""SELECT id,
                   CAST(col_a AS DOUBLE), CAST(col_b AS DOUBLE),
                   CAST(val_x AS DOUBLE), CAST(val_y AS DOUBLE)
            FROM ({data_sql})"""
    ).fetchall()
    n = len(data)

    t_build = time.perf_counter()
    oracle_solver.create_model("mixed_types_same_term")
    xnames = [f"x_{i}" for i in range(n)]
    ynames = [f"y_{i}" for i in range(n)]
    for xn in xnames:
        oracle_solver.add_variable(xn, VarType.BINARY)
    for yn in ynames:
        oracle_solver.add_variable(yn, VarType.CONTINUOUS, lb=0.0, ub=10.0)

    term = {}
    for i in range(n):
        term[xnames[i]] = data[i][1]
        term[ynames[i]] = data[i][2]
    oracle_solver.add_constraint(term, "<=", 50.0, name="mixed_term_cap")

    obj = {}
    for i in range(n):
        obj[xnames[i]] = data[i][3]
        obj[ynames[i]] = data[i][4]
    oracle_solver.set_objective(obj, ObjSense.MAXIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_rows, decidb_cols, result, data, ["x", "y"],
        coeff_fn=lambda row: {
            "x": float(row[decidb_cols.index("val_x")]),
            "y": float(row[decidb_cols.index("val_y")]),
        },
    )
    perf_tracker.record(
        "mixed_types_same_aggregate_term", decidb_time, build_time,
        result.solve_time_seconds, n, 2 * n, 2,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )
