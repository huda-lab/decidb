"""Tests for IS INTEGER (and default-type) decision variables.

Covers:
  - simple_test: default variable type (INTEGER), single aggregate constraint
"""

import time

import pytest

from solver.types import VarType, ObjSense, SolverStatus
from comparison.compare import compare_solutions


@pytest.mark.var_integer
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_simple_test(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Default integer variable: maximize extendedprice, sum <= 10000."""
    sql = """
        SELECT x, l_orderkey, l_linenumber, l_extendedprice, l_tax
        FROM LINEITEM
        WHERE l_orderkey <= 5
        DECIDE x
        SUCH THAT SUM(x * l_extendedprice) <= 10000
        MAXIMIZE SUM(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_extendedprice AS DOUBLE),
               CAST(l_tax AS DOUBLE)
        FROM LINEITEM WHERE l_orderkey <= 5
    """).fetchall()

    # Oracle: default type = non-negative integer.
    # Constraint: SUM(x_i * price_i) <= 10000
    # Objective:  MAXIMIZE SUM(x_i * price_i)
    # Since objective == constraint LHS and both use extendedprice, the optimal
    # is to set each x to the largest integer that still satisfies the sum.
    # The solver will figure this out.
    t_build = time.perf_counter()
    oracle_solver.create_model("simple_test")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.INTEGER, lb=0.0)

    oracle_solver.add_constraint(
        {vnames[i]: data[i][2] for i in range(len(data))},
        "<=", 10000.0, name="budget",
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
        "simple_test", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )
