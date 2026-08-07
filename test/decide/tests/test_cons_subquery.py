"""Tests for subquery on constraint RHS.

Covers:
  - q04_subquery_rhs: SUM(x * price) <= (SELECT AVG(c_acctbal) ...)
"""

import time

import pytest

from solver.types import VarType, ObjSense
from comparison.compare import compare_solutions


@pytest.mark.var_boolean
@pytest.mark.cons_subquery
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.sql_subquery
@pytest.mark.correctness
def test_q04_subquery_rhs(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Subquery RHS: total price <= avg customer acctbal for nation 10."""
    sql = """
        SELECT o_orderkey, o_totalprice, x
        FROM orders
        WHERE o_orderkey < 100
        DECIDE x(BOOL)
        SUCH THAT SUM(x * o_totalprice) <= (SELECT AVG(c_acctbal) FROM customer WHERE c_nationkey = 10)
        MAXIMIZE SUM(x * o_totalprice)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    # Fetch data + resolve subquery via duckdb
    data = duckdb_conn.execute("""
        SELECT CAST(o_orderkey AS BIGINT),
               CAST(o_totalprice AS DOUBLE)
        FROM orders WHERE o_orderkey < 100
    """).fetchall()

    rhs_val = duckdb_conn.execute("""
        SELECT CAST(AVG(c_acctbal) AS DOUBLE) FROM customer WHERE c_nationkey = 10
    """).fetchone()[0]

    t_build = time.perf_counter()
    oracle_solver.create_model("q04_subquery")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    oracle_solver.add_constraint(
        {vnames[i]: data[i][1] for i in range(len(data))},
        "<=", float(rhs_val), name="budget",
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
        "q04_subquery_rhs", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.var_boolean
@pytest.mark.cons_subquery
@pytest.mark.cons_aggregate
@pytest.mark.per_clause
@pytest.mark.obj_maximize
@pytest.mark.sql_subquery
@pytest.mark.correctness
def test_per_constraint_with_subquery_rhs(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """Uncorrelated scalar subquery as the RHS of a PER constraint.

    ``(SELECT 30000)`` must be evaluated once and shared across every PER
    group. A bug that re-evaluates per group, or drops the RHS inside the
    PER partition loop, would either crash or produce inconsistent limits.
    """
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, x
        FROM lineitem WHERE l_orderkey <= 5
        DECIDE x(BOOL)
        SUCH THAT SUM(x * l_extendedprice) <= (SELECT 30000) PER l_orderkey
        MAXIMIZE SUM(x)
    """
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_extendedprice AS DOUBLE)
        FROM lineitem WHERE l_orderkey <= 5
    """).fetchall()
    rhs = duckdb_conn.execute("SELECT CAST(30000 AS DOUBLE)").fetchone()[0]
    n = len(data)

    groups: dict[int, list[int]] = {}
    for i, row in enumerate(data):
        groups.setdefault(row[0], []).append(i)

    t_build = time.perf_counter()
    oracle_solver.create_model("per_subquery_rhs")
    vnames = [f"x_{i}" for i in range(n)]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)
    for g, idxs in groups.items():
        oracle_solver.add_constraint(
            {vnames[i]: data[i][2] for i in idxs},
            "<=", float(rhs), name=f"per_{g}",
        )
    oracle_solver.set_objective(
        {vn: 1.0 for vn in vnames}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    x_idx = decidb_cols.index("x")
    decidb_obj = sum(int(r[x_idx]) for r in decidb_rows)
    assert abs(decidb_obj - result.objective_value) <= 1e-6, (
        f"Objective mismatch: DecidB={decidb_obj}, Oracle={result.objective_value}"
    )

    perf_tracker.record(
        "per_subquery_rhs", decidb_time, build_time,
        result.solve_time_seconds, n, n, len(groups),
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )
