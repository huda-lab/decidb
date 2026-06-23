"""Tests for signed (negative-domain) decision variables.

A decision variable is non-negative by default ([0, +inf)). When the query
gives it an explicit negative lower bound — `x >= -K`, `x BETWEEN -K AND K`, or
a negative literal in an `IN` domain — DeciDB honors it instead of clamping to
0. These tests cover:

  - test_signed_linear:      explicit negative bound in a linear LP
  - test_signed_mccormick:   Bool x signed-Real product (McCormick lower corner)
  - test_signed_in_domain:   IN domain containing negative values

The oracle is an independent gurobipy formulation with the matching negative
`lb`; for the bilinear case the oracle solves the *true* product (NonConvex=2)
rather than mirroring DeciDB's McCormick linearization.
"""

import time

import pytest

from solver.types import VarType, ObjSense
from comparison.compare import compare_solutions
from ._oracle_helpers import add_in_domain


@pytest.mark.var_real
@pytest.mark.cons_aggregate
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_signed_linear(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Signed continuous variable: x in [-3, 3], minimize weighted sum with a
    sign-mixing aggregate constraint that keeps the problem bounded."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity, x
        FROM lineitem
        WHERE l_orderkey < 50
        DECIDE x IS REAL
        SUCH THAT x BETWEEN -3 AND 3
              AND SUM(x * l_quantity) >= 0
        MINIMIZE SUM(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_extendedprice AS DOUBLE),
               CAST(l_quantity AS DOUBLE)
        FROM lineitem WHERE l_orderkey < 50
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("signed_linear")
    n = len(data)
    vnames = [f"x_{i}" for i in range(n)]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.CONTINUOUS, lb=-3.0, ub=3.0)

    oracle_solver.add_constraint(
        {vnames[i]: data[i][1] for i in range(n)},
        ">=", 0.0, name="sign_mix",
    )
    oracle_solver.set_objective(
        {vnames[i]: data[i][0] for i in range(n)},
        ObjSense.MINIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_result, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[decidb_cols.index("l_extendedprice")])},
    )

    perf_tracker.record(
        "signed_linear", decidb_time, build_time,
        result.solve_time_seconds, n, len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.var_real
@pytest.mark.var_boolean
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_signed_mccormick(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Bool x signed-Real product. x in [-8, 8] is negative-capable, so the
    McCormick lower corner (w >= L*b) and the widened aux bound are exercised.
    The oracle solves the exact product b*x (NonConvex=2), independent of
    DeciDB's McCormick linearization."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, b, x
        FROM lineitem
        WHERE l_orderkey < 40
        DECIDE b IS BOOLEAN, x IS REAL
        SUCH THAT x BETWEEN -8 AND 8
              AND SUM(b) <= 3
        MINIMIZE SUM(l_extendedprice * b * x)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_extendedprice AS DOUBLE)
        FROM lineitem WHERE l_orderkey < 40
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("signed_mccormick")
    n = len(data)
    bnames = [f"b_{i}" for i in range(n)]
    xnames = [f"x_{i}" for i in range(n)]
    for vn in bnames:
        oracle_solver.add_variable(vn, VarType.BINARY)
    for vn in xnames:
        oracle_solver.add_variable(vn, VarType.CONTINUOUS, lb=-8.0, ub=8.0)

    oracle_solver.add_constraint(
        {bnames[i]: 1.0 for i in range(n)}, "<=", 3.0, name="card",
    )
    # Exact product objective: minimize SUM(price_i * b_i * x_i).
    quadratic = {(bnames[i], xnames[i]): float(data[i][0]) for i in range(n)}
    oracle_solver.set_quadratic_objective({}, quadratic, ObjSense.MINIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    bi = decidb_cols.index("b")
    xi = decidb_cols.index("x")
    pi = decidb_cols.index("l_extendedprice")

    def decidb_objective_fn(rows, cols):
        return sum(float(r[pi]) * float(r[bi]) * float(r[xi]) for r in rows)

    cmp = compare_solutions(
        decidb_result, decidb_cols, result, data, ["b", "x"],
        decidb_objective_fn=decidb_objective_fn,
    )

    perf_tracker.record(
        "signed_mccormick", decidb_time, build_time,
        result.solve_time_seconds, n, len(bnames) + len(xnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.var_integer
@pytest.mark.cons_perrow
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_signed_in_domain(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """IN domain containing negative values. Without the bound-widening the
    default lower bound 0 would silently prune the -5 selection."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, x
        FROM lineitem
        WHERE l_orderkey < 40
        DECIDE x IS INTEGER
        SUCH THAT x IN (-5, 0, 5)
        MINIMIZE SUM(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_extendedprice AS DOUBLE)
        FROM lineitem WHERE l_orderkey < 40
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("signed_in_domain")
    n = len(data)
    domain = [-5.0, 0.0, 5.0]
    vnames = [f"x_{i}" for i in range(n)]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.INTEGER, lb=-5.0, ub=5.0)
        add_in_domain(oracle_solver, vn, domain)

    oracle_solver.set_objective(
        {vnames[i]: data[i][0] for i in range(n)},
        ObjSense.MINIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_result, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[decidb_cols.index("l_extendedprice")])},
    )

    perf_tracker.record(
        "signed_in_domain", decidb_time, build_time,
        result.solve_time_seconds, n, len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )
