"""Tests for signed (negative-domain) decision variables.

A decision variable is non-negative by default ([0, +inf)). When the query
gives it an explicit negative lower bound — `x >= -K`, `x BETWEEN -K AND K`, or
a negative literal in an `IN` domain — DeciDB honors it instead of clamping to
0. These tests cover:

  - test_signed_linear:          explicit negative bound (BETWEEN) in a linear LP
  - test_signed_lower_bound_only: one-sided `x >= -K` (no upper bound)
  - test_signed_integer:          plain INTEGER signed variable
  - test_signed_default_preserved: a signed var next to a default var that must
                                   stay >= 0 (regression guard for the default)
  - test_signed_mccormick:        Bool x signed-Real product (McCormick corners)
  - test_signed_mccormick_asymmetric: McCormick with an asymmetric range [-100, 5]
  - test_signed_in_domain:        IN domain containing negative values
  - test_signed_abs_objective:    ABS over a signed variable (audit: sign-safe)
  - test_signed_strict_integer:   strict `>` on a signed-integer aggregate (Big-M)
  - test_signed_norm_l1_parity:   norm(x,1) == SUM(ABS(x)) on a signed variable

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


@pytest.mark.var_real
@pytest.mark.cons_aggregate
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_signed_lower_bound_only(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """One-sided lower bound `x >= -4` (no upper bound). Distinct from BETWEEN:
    only the lower bound is set; the upper stays +inf. MINIMIZE keeps it bounded."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, x
        FROM lineitem
        WHERE l_orderkey < 50
        DECIDE x IS REAL
        SUCH THAT x >= -4
              AND SUM(x) >= 10
        MINIMIZE SUM(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_extendedprice AS DOUBLE)
        FROM lineitem WHERE l_orderkey < 50
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("signed_lower_only")
    n = len(data)
    vnames = [f"x_{i}" for i in range(n)]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.CONTINUOUS, lb=-4.0)  # ub=None -> +inf
    oracle_solver.add_constraint(
        {vnames[i]: 1.0 for i in range(n)}, ">=", 10.0, name="sum_floor",
    )
    oracle_solver.set_objective(
        {vnames[i]: data[i][0] for i in range(n)}, ObjSense.MINIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_result, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[decidb_cols.index("l_extendedprice")])},
    )
    perf_tracker.record(
        "signed_lower_only", decidb_time, build_time,
        result.solve_time_seconds, n, len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.var_integer
@pytest.mark.cons_aggregate
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_signed_integer(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Plain INTEGER signed variable: d >= -5 (negative integer domain)."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, d
        FROM lineitem
        WHERE l_orderkey < 50
        DECIDE d IS INTEGER
        SUCH THAT d >= -5
              AND SUM(d) >= 3
        MINIMIZE SUM(d * l_extendedprice)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_extendedprice AS DOUBLE)
        FROM lineitem WHERE l_orderkey < 50
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("signed_integer")
    n = len(data)
    vnames = [f"d_{i}" for i in range(n)]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.INTEGER, lb=-5.0)
    oracle_solver.add_constraint(
        {vnames[i]: 1.0 for i in range(n)}, ">=", 3.0, name="sum_floor",
    )
    oracle_solver.set_objective(
        {vnames[i]: data[i][0] for i in range(n)}, ObjSense.MINIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_result, decidb_cols, result, data, ["d"],
        coeff_fn=lambda row: {"d": float(row[decidb_cols.index("l_extendedprice")])},
    )
    perf_tracker.record(
        "signed_integer", decidb_time, build_time,
        result.solve_time_seconds, n, len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.var_real
@pytest.mark.cons_aggregate
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_signed_default_preserved(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Regression guard: a signed var `x >= -5` next to a default var `y` (no
    explicit bound). The default var must stay non-negative even though negative
    domains are now reachable — the oracle pins y at lb=0, so a wrongly-signed y
    would mismatch."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, x, y
        FROM lineitem
        WHERE l_orderkey < 40
        DECIDE x IS REAL, y IS REAL
        SUCH THAT x >= -5
              AND SUM(x + y) >= 0
        MINIMIZE SUM((x + y) * l_extendedprice)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_extendedprice AS DOUBLE)
        FROM lineitem WHERE l_orderkey < 40
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("signed_default_preserved")
    n = len(data)
    xs = [f"x_{i}" for i in range(n)]
    ys = [f"y_{i}" for i in range(n)]
    for vn in xs:
        oracle_solver.add_variable(vn, VarType.CONTINUOUS, lb=-5.0)
    for vn in ys:
        oracle_solver.add_variable(vn, VarType.CONTINUOUS, lb=0.0)  # default stays >= 0
    coeffs = {}
    for i in range(n):
        coeffs[xs[i]] = 1.0
        coeffs[ys[i]] = 1.0
    oracle_solver.add_constraint(coeffs, ">=", 0.0, name="sum_floor")
    obj = {}
    for i in range(n):
        obj[xs[i]] = data[i][0]
        obj[ys[i]] = data[i][0]
    oracle_solver.set_objective(obj, ObjSense.MINIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_result, decidb_cols, result, data, ["x", "y"],
        coeff_fn=lambda row: {
            "x": float(row[decidb_cols.index("l_extendedprice")]),
            "y": float(row[decidb_cols.index("l_extendedprice")]),
        },
    )
    # Independent invariant: every default-typed y must be non-negative.
    yi = decidb_cols.index("y")
    assert all(float(r[yi]) >= -1e-6 for r in decidb_result), \
        "default variable y went negative — the non-negative default was not preserved"
    perf_tracker.record(
        "signed_default_preserved", decidb_time, build_time,
        result.solve_time_seconds, n, len(xs) + len(ys), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.var_real
@pytest.mark.var_boolean
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_signed_mccormick_asymmetric(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """McCormick with an asymmetric negative range x in [-100, 5]. Stresses the
    lower corner / aux-bound widening and the max(|lb|,|ub|) magnitude."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, b, x
        FROM lineitem
        WHERE l_orderkey < 40
        DECIDE b IS BOOLEAN, x IS REAL
        SUCH THAT x BETWEEN -100 AND 5
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
    oracle_solver.create_model("signed_mccormick_asym")
    n = len(data)
    bnames = [f"b_{i}" for i in range(n)]
    xnames = [f"x_{i}" for i in range(n)]
    for vn in bnames:
        oracle_solver.add_variable(vn, VarType.BINARY)
    for vn in xnames:
        oracle_solver.add_variable(vn, VarType.CONTINUOUS, lb=-100.0, ub=5.0)
    oracle_solver.add_constraint(
        {bnames[i]: 1.0 for i in range(n)}, "<=", 3.0, name="card",
    )
    quadratic = {(bnames[i], xnames[i]): float(data[i][0]) for i in range(n)}
    oracle_solver.set_quadratic_objective({}, quadratic, ObjSense.MINIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    bi = decidb_cols.index("b")
    xi = decidb_cols.index("x")
    pi = decidb_cols.index("l_extendedprice")

    cmp = compare_solutions(
        decidb_result, decidb_cols, result, data, ["b", "x"],
        decidb_objective_fn=lambda rows, cols: sum(
            float(r[pi]) * float(r[bi]) * float(r[xi]) for r in rows),
    )
    perf_tracker.record(
        "signed_mccormick_asym", decidb_time, build_time,
        result.solve_time_seconds, n, len(bnames) + len(xnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.var_real
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_signed_abs_objective(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """ABS over a signed variable (audit: ABS is already sign-safe). x in [-5, 5]
    with SUM(x) = -3; MINIMIZE SUM(ABS(x))."""
    sql = """
        SELECT l_orderkey, l_linenumber, x
        FROM lineitem
        WHERE l_orderkey < 20
        DECIDE x IS REAL
        SUCH THAT x BETWEEN -5 AND 5
              AND SUM(x) = -3
        MINIMIZE SUM(ABS(x))
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    nrows = duckdb_conn.execute(
        "SELECT COUNT(*) FROM lineitem WHERE l_orderkey < 20").fetchone()[0]

    t_build = time.perf_counter()
    oracle_solver.create_model("signed_abs")
    n = int(nrows)
    xs = [f"x_{i}" for i in range(n)]
    aux = [f"a_{i}" for i in range(n)]
    for vn in xs:
        oracle_solver.add_variable(vn, VarType.CONTINUOUS, lb=-5.0, ub=5.0)
    for vn in aux:
        oracle_solver.add_variable(vn, VarType.CONTINUOUS, lb=0.0)
    for i in range(n):
        oracle_solver.add_constraint({aux[i]: 1.0, xs[i]: -1.0}, ">=", 0.0, name=f"abs_pos_{i}")
        oracle_solver.add_constraint({aux[i]: 1.0, xs[i]: 1.0}, ">=", 0.0, name=f"abs_neg_{i}")
    oracle_solver.add_constraint({xs[i]: 1.0 for i in range(n)}, "=", -3.0, name="sum_eq")
    oracle_solver.set_objective({aux[i]: 1.0 for i in range(n)}, ObjSense.MINIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    xi = decidb_cols.index("x")
    cmp = compare_solutions(
        decidb_result, decidb_cols, result, [(0.0,)] * n, ["x"],
        decidb_objective_fn=lambda rows, cols: sum(abs(float(r[xi])) for r in rows),
    )
    perf_tracker.record(
        "signed_abs", decidb_time, build_time,
        result.solve_time_seconds, n, len(xs) + len(aux), 2 * n + 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.var_integer
@pytest.mark.cons_aggregate
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_signed_strict_integer(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Strict `>` on a signed-integer aggregate. `SUM(d) > -8` with integer d is
    rewritten to `SUM(d) >= -7`; validates the integer-step + Big-M path on a
    negative domain."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, d
        FROM lineitem
        WHERE l_orderkey < 40
        DECIDE d IS INTEGER
        SUCH THAT d BETWEEN -10 AND 0
              AND SUM(d) > -8
        MINIMIZE SUM(d * l_extendedprice)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_extendedprice AS DOUBLE)
        FROM lineitem WHERE l_orderkey < 40
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("signed_strict_integer")
    n = len(data)
    vnames = [f"d_{i}" for i in range(n)]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.INTEGER, lb=-10.0, ub=0.0)
    # SUM(d) > -8 on integer-valued LHS == SUM(d) >= -7 (integer step).
    oracle_solver.add_constraint(
        {vnames[i]: 1.0 for i in range(n)}, ">=", -7.0, name="strict_step",
    )
    oracle_solver.set_objective(
        {vnames[i]: data[i][0] for i in range(n)}, ObjSense.MINIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_result, decidb_cols, result, data, ["d"],
        coeff_fn=lambda row: {"d": float(row[decidb_cols.index("l_extendedprice")])},
    )
    perf_tracker.record(
        "signed_strict_integer", decidb_time, build_time,
        result.solve_time_seconds, n, len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.var_real
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_signed_norm_l1_parity(decidb_cli):
    """norm(x, 1) desugars to SUM(ABS(x)); on a signed variable the two forms
    must reach the same realized L1 value (audit: norm/ABS are sign-safe)."""
    base = """
        SELECT l_orderkey, l_linenumber, x
        FROM lineitem WHERE l_orderkey < 20
        DECIDE x IS REAL
        SUCH THAT x BETWEEN -5 AND 5 AND SUM(x) = -3
        MINIMIZE {obj}
    """
    r1, c1 = decidb_cli.execute(base.format(obj="norm(x, 1)"))
    r2, c2 = decidb_cli.execute(base.format(obj="SUM(ABS(x))"))
    xi1, xi2 = c1.index("x"), c2.index("x")
    v1 = sum(abs(float(r[xi1])) for r in r1)
    v2 = sum(abs(float(r[xi2])) for r in r2)
    assert v1 == pytest.approx(v2, abs=1e-4)
    assert v1 == pytest.approx(3.0, abs=1e-4)  # min L1 with sum fixed at -3
