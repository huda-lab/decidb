"""Tests for BETWEEN constraints.

Covers:
  - q10_logic_dependency: per-row x BETWEEN 0 AND 5, with aggregate constraint
  - test_aggregate_between_standalone: SUM(x*w) BETWEEN lo AND hi — desugar
    path without aggregate-local WHEN (distinct from test_aggregate_local_when.py)
"""

import time

import pytest

from solver.types import VarType, ObjSense
from comparison.compare import compare_solutions


@pytest.mark.var_integer
@pytest.mark.cons_between
@pytest.mark.cons_multi
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_q10_logic_dependency(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """BETWEEN constraint: x BETWEEN 0 AND 5, maximize count."""
    sql = """
        SELECT l_orderkey, l_extendedprice, x
        FROM lineitem
        WHERE l_orderkey < 20
        DECIDE x IS INTEGER
        SUCH THAT x BETWEEN 0 AND 5
          AND SUM(x * l_extendedprice) <= 10000
        MAXIMIZE SUM(x)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_extendedprice AS DOUBLE)
        FROM lineitem WHERE l_orderkey < 20
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("q10_logic")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        # BETWEEN 0 AND 5 => lb=0, ub=5
        oracle_solver.add_variable(vn, VarType.INTEGER, lb=0.0, ub=5.0)

    # SUM(x * l_extendedprice) <= 10000
    oracle_solver.add_constraint(
        {vnames[i]: data[i][1] for i in range(len(data))},
        "<=", 10000.0, name="price_limit",
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
        "q10_logic_dependency", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 2,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.var_boolean
@pytest.mark.cons_between
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_aggregate_between_standalone(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """Standalone aggregate BETWEEN: ``SUM(x * weight) BETWEEN lo AND hi``.

    The only existing aggregate BETWEEN coverage lives nested inside
    aggregate-local WHEN (test_aggregate_local_when.py). Here we exercise
    the plain desugaring path, where the binder must emit two aggregate
    constraints — ``SUM(x * weight) >= lo`` and ``SUM(x * weight) <= hi``
    — from a single BETWEEN expression. A dropped sign or swapped bound
    would silently cut off feasible solutions.

    Data is chosen so the band [10, 15] is a non-trivial slice: several
    subsets fit, a few are below lo, a few are above hi. The optimum
    selects {1, 2, 3} for total weight 15 and value 33.
    """
    data_sql = (
        "SELECT * FROM (VALUES "
        "(1, 3.0, 10.0), (2, 5.0, 8.0), (3, 7.0, 15.0), (4, 2.0, 4.0), (5, 4.0, 7.0)"
        ") t(id, weight, value)"
    )
    decide_sql = f"""
        SELECT id, weight, value, x FROM ({data_sql})
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * weight) BETWEEN 10 AND 15
        MAXIMIZE SUM(x * value)
    """
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute(decide_sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute(
        f"SELECT CAST(id AS BIGINT), CAST(weight AS DOUBLE), CAST(value AS DOUBLE) "
        f"FROM ({data_sql})"
    ).fetchall()
    n = len(data)

    t_build = time.perf_counter()
    oracle_solver.create_model("aggregate_between_standalone")
    vnames = [f"x_{i}" for i in range(n)]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    weights = {vnames[i]: data[i][1] for i in range(n)}
    oracle_solver.add_constraint(dict(weights), ">=", 10.0, name="weight_lo")
    oracle_solver.add_constraint(dict(weights), "<=", 15.0, name="weight_hi")
    oracle_solver.set_objective(
        {vnames[i]: data[i][2] for i in range(n)},
        ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    value_idx = decidb_cols.index("value")
    cmp = compare_solutions(
        decidb_rows, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[value_idx])},
    )

    perf_tracker.record(
        "aggregate_between_standalone", decidb_time, build_time,
        result.solve_time_seconds, n, n, 2,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.var_real
@pytest.mark.cons_between
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_real_between_oracle(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Per-row BETWEEN with non-integer bounds on a REAL variable.

    Regression test for the integer-step rewrite sweep. BETWEEN desugars into
    `>=` and `<=` (no ±1 band), so REAL LHS with fractional bounds must solve
    correctly. This guards against future regressions if BETWEEN desugaring is
    changed to use an integer-step shortcut.
    """
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, x
        FROM lineitem WHERE l_orderkey < 20
        DECIDE x IS REAL
        SUCH THAT x BETWEEN 0.25 AND 3.75
            AND SUM(x) <= 40
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
    oracle_solver.create_model("real_between")
    vnames = [f"x_{i}" for i in range(n)]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.CONTINUOUS, lb=0.25, ub=3.75)
    oracle_solver.add_constraint(
        {vnames[i]: 1.0 for i in range(n)}, "<=", 40.0, name="sum_cap",
    )
    oracle_solver.set_objective(
        {vnames[i]: data[i][2] for i in range(n)}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_rows, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[decidb_cols.index("l_extendedprice")])},
    )

    perf_tracker.record(
        "real_between_oracle", decidb_time, build_time,
        result.solve_time_seconds, n, n, 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )
