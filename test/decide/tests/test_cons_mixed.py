"""Tests for mixed constraints (aggregate + per-row in the same query).

Covers:
  - q02_integer_procurement: per-row x <= ps_availqty + global budget
"""

import time

import pytest

from solver.types import VarType, ObjSense
from comparison.compare import compare_solutions


@pytest.mark.var_integer
@pytest.mark.cons_perrow
@pytest.mark.cons_aggregate
@pytest.mark.cons_mixed
@pytest.mark.cons_multi
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_q02_integer_procurement(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Integer procurement: x <= availqty per row, budget SUM(x*cost) <= 10000."""
    sql = """
        SELECT ps_partkey, ps_suppkey, ps_supplycost, ps_availqty, x
        FROM partsupp
        WHERE ps_partkey < 50
        DECIDE x(INT)
        SUCH THAT x <= ps_availqty
          AND SUM(x * ps_supplycost) <= 10000
        MAXIMIZE SUM(x)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(ps_partkey AS BIGINT),
               CAST(ps_suppkey AS BIGINT),
               CAST(ps_supplycost AS DOUBLE),
               CAST(ps_availqty AS DOUBLE)
        FROM partsupp WHERE ps_partkey < 50
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("q02_procurement")
    vnames = [f"x_{i}" for i in range(len(data))]
    for i, vn in enumerate(vnames):
        # Per-row upper bound: x <= ps_availqty
        oracle_solver.add_variable(vn, VarType.INTEGER, lb=0.0, ub=data[i][3])

    # Budget: SUM(x_i * supplycost_i) <= 10000
    oracle_solver.add_constraint(
        {vnames[i]: data[i][2] for i in range(len(data))},
        "<=", 10000.0, name="budget",
    )
    # Objective: MAXIMIZE SUM(x_i)  (coefficient = 1.0 for each)
    oracle_solver.set_objective(
        {vnames[i]: 1.0 for i in range(len(data))},
        ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    # For MAXIMIZE SUM(x), the objective coefficient per row is 1.0
    cmp = compare_solutions(
        decidb_result, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": 1.0},
    )

    perf_tracker.record(
        "q02_integer_procurement", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 2,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )
