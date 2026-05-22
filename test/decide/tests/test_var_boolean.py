"""Tests for IS BOOLEAN decision variables.

Covers:
  - q01_knapsack_binary: binary knapsack on lineitem
  - knapsack_lineitem: binary knapsack with wider WHERE range
"""

import time

import pytest

from solver.types import VarType, ObjSense, SolverStatus
from comparison.compare import compare_solutions


@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_q01_knapsack_binary(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Binary knapsack: maximize extendedprice, total quantity <= 100."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity, x
        FROM lineitem
        WHERE l_orderkey < 100
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * l_quantity) <= 100
        MAXIMIZE SUM(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    # Oracle data
    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_extendedprice AS DOUBLE),
               CAST(l_quantity AS DOUBLE)
        FROM lineitem WHERE l_orderkey < 100
    """).fetchall()

    # Build oracle model
    t_build = time.perf_counter()
    oracle_solver.create_model("q01_knapsack")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    oracle_solver.add_constraint(
        {vnames[i]: data[i][3] for i in range(len(data))},
        "<=", 100.0, name="capacity",
    )
    oracle_solver.set_objective(
        {vnames[i]: data[i][2] for i in range(len(data))},
        ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    # Compare objectives
    cmp = compare_solutions(
        decidb_result, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[decidb_cols.index("l_extendedprice")])},
    )

    perf_tracker.record(
        "q01_knapsack_binary", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )

