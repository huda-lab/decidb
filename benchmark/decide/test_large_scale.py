"""Large-scale performance tests.

These tests exercise DECIDE on bigger datasets to measure scaling behavior.
They use wider WHERE ranges to increase the number of decision variables.
"""

import time

import pytest

from solver.types import VarType, ObjSense
from comparison.compare import compare_solutions


@pytest.mark.large_scale
@pytest.mark.performance
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_knapsack_large(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Knapsack on a larger range (l_orderkey < 500)."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity, x
        FROM lineitem
        WHERE l_orderkey < 500
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * l_quantity) <= 1000
        MAXIMIZE SUM(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_extendedprice AS DOUBLE),
               CAST(l_quantity AS DOUBLE)
        FROM lineitem WHERE l_orderkey < 500
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("knapsack_large")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    oracle_solver.add_constraint(
        {vnames[i]: data[i][3] for i in range(len(data))},
        "<=", 1000.0, name="capacity",
    )
    oracle_solver.set_objective(
        {vnames[i]: data[i][2] for i in range(len(data))},
        ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_result, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[decidb_cols.index("l_extendedprice")])},
    )

    perf_tracker.record(
        "knapsack_large", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.large_scale
@pytest.mark.performance
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_order_selection_large(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Order selection on a wider date range (full year 1995)."""
    sql = """
        SELECT o_orderkey, o_totalprice, x
        FROM orders
        WHERE o_orderdate >= '1995-01-01' AND o_orderdate < '1996-01-01'
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) <= 1000
        MAXIMIZE SUM(x * o_totalprice)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(o_orderkey AS BIGINT),
               CAST(o_totalprice AS DOUBLE)
        FROM orders
        WHERE o_orderdate >= '1995-01-01' AND o_orderdate < '1996-01-01'
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("order_selection_large")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    oracle_solver.add_constraint(
        {vnames[i]: 1.0 for i in range(len(data))},
        "<=", 1000.0, name="count_limit",
    )
    oracle_solver.set_objective(
        {vnames[i]: data[i][1] for i in range(len(data))},
        ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_result, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[decidb_cols.index("o_totalprice")])},
    )

    perf_tracker.record(
        "order_selection_large", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )
