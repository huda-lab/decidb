"""Tests for JOINs in FROM clause with DECIDE.

Covers:
  - q05_join_decide: join orders with customer, filter by segment
  - q06 also uses joins — see test_cons_multi.py
"""

import time

import pytest

from solver.types import VarType, ObjSense
from comparison.compare import compare_solutions


@pytest.mark.var_boolean
@pytest.mark.sql_joins
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_q05_join_decide(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Join: select orders from BUILDING segment, maximize total price."""
    sql = """
        SELECT o.o_orderkey, o.o_totalprice, c.c_mktsegment, x
        FROM orders o, customer c
        WHERE o.o_custkey = c.c_custkey
          AND c.c_mktsegment = 'BUILDING'
          AND o.o_orderkey < 1000
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * o.o_totalprice) <= 100000
        MAXIMIZE SUM(x * o.o_totalprice)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(o.o_orderkey AS BIGINT),
               CAST(o.o_totalprice AS DOUBLE),
               c.c_mktsegment
        FROM orders o, customer c
        WHERE o.o_custkey = c.c_custkey
          AND c.c_mktsegment = 'BUILDING'
          AND o.o_orderkey < 1000
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("q05_join")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    # SUM(x * totalprice) <= 100000
    oracle_solver.add_constraint(
        {vnames[i]: data[i][1] for i in range(len(data))},
        "<=", 100000.0, name="budget",
    )
    # MAXIMIZE SUM(x * totalprice)
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
        "q05_join_decide", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.var_boolean
@pytest.mark.sql_joins
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_three_way_join(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Three-table join: lineitem + orders + customer."""
    sql = """
        SELECT l.l_orderkey, l.l_linenumber, l.l_extendedprice,
               o.o_totalprice, c.c_mktsegment, x
        FROM lineitem l, orders o, customer c
        WHERE l.l_orderkey = o.o_orderkey
          AND o.o_custkey = c.c_custkey
          AND c.c_mktsegment = 'BUILDING'
          AND l.l_orderkey < 100
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * l.l_extendedprice) <= 50000
        MAXIMIZE SUM(x * l.l_extendedprice)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l.l_orderkey AS BIGINT),
               CAST(l.l_linenumber AS BIGINT),
               CAST(l.l_extendedprice AS DOUBLE),
               CAST(o.o_totalprice AS DOUBLE),
               c.c_mktsegment
        FROM lineitem l, orders o, customer c
        WHERE l.l_orderkey = o.o_orderkey
          AND o.o_custkey = c.c_custkey
          AND c.c_mktsegment = 'BUILDING'
          AND l.l_orderkey < 100
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("three_way_join")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    oracle_solver.add_constraint(
        {vnames[i]: data[i][2] for i in range(len(data))},
        "<=", 50000.0, name="budget",
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
        "three_way_join", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )
