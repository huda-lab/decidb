"""Tests for multiple constraints in one query.

Covers:
  - q06_multi_constraint: weight (quantity) + volume (size) constraints with join
"""

import time

import pytest

from solver.types import VarType, ObjSense
from comparison.compare import compare_solutions


@pytest.mark.var_boolean
@pytest.mark.cons_multi
@pytest.mark.cons_aggregate
@pytest.mark.sql_joins
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_q06_multi_constraint(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Multiple constraints: weight (quantity) <= 500 AND volume (size) <= 1000."""
    sql = """
        SELECT l.l_orderkey, l.l_quantity, p.p_size, x
        FROM lineitem l, part p
        WHERE l.l_partkey = p.p_partkey
          AND l.l_orderkey < 50
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * l_quantity) <= 500
          AND SUM(x * p.p_size) <= 1000
        MAXIMIZE SUM(x)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l.l_orderkey AS BIGINT),
               CAST(l.l_quantity AS DOUBLE),
               CAST(p.p_size AS DOUBLE)
        FROM lineitem l, part p
        WHERE l.l_partkey = p.p_partkey
          AND l.l_orderkey < 50
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("q06_multi")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    # Constraint 1: SUM(x * quantity) <= 500
    oracle_solver.add_constraint(
        {vnames[i]: data[i][1] for i in range(len(data))},
        "<=", 500.0, name="weight",
    )
    # Constraint 2: SUM(x * size) <= 1000
    oracle_solver.add_constraint(
        {vnames[i]: data[i][2] for i in range(len(data))},
        "<=", 1000.0, name="volume",
    )
    # MAXIMIZE SUM(x)
    oracle_solver.set_objective(
        {vnames[i]: 1.0 for i in range(len(data))},
        ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_result, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": 1.0},
    )

    perf_tracker.record(
        "q06_multi_constraint", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 2,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )
