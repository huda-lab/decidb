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

from decidb_cli import DeciDBCliError
from solver.types import ObjSense, SolverStatus, VarType
from comparison.compare import compare_solutions
from ._oracle_helpers import add_in_domain
from ._scope_cases import answer, scope_solver  # noqa: F401 -- scope_solver is a fixture
from ._scope_oracle import CONSTANT, Decision, Reduce, Scoped, ScopedModel, ScopedObjective


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
        DECIDE x(REAL)
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
        DECIDE b(BOOL), x(REAL)
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
        DECIDE x(INT)
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
        DECIDE x(REAL)
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
        DECIDE d(INT)
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
        DECIDE x(REAL), y(REAL)
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
        DECIDE b(BOOL), x(REAL)
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
        DECIDE x(REAL)
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
        DECIDE d(INT)
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
        DECIDE x(REAL)
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


@pytest.mark.var_integer
@pytest.mark.correctness
@pytest.mark.parametrize("cli_fixture", ["decidb_cli_gurobi", "decidb_cli_highs"])
@pytest.mark.parametrize("bound", [-2.5, -2.7, -0.5])
def test_signed_integer_fractional_lower_bound(request, scope_solver, cli_fixture, bound):
    """An INTEGER decision with a fractional negative lower bound and an open upper side
    (`x <= cap` is a row, not a column bound): `x >= -2.5` minimizes to -2. HiGHS was handed
    `lb = -2.5` on an integer column and answered -3, below the bound."""
    cli = request.getfixturevalue(cli_fixture)
    rows = [{"id": 1, "cap": 9}, {"id": 2, "cap": 9}]
    x = lambda r: {"x": 1}  # noqa: E731
    clauses = [
        Scoped((x,), "<=", (lambda r: {CONSTANT: r["cap"]},)),
        Scoped((x,), ">=", (lambda r: {CONSTANT: bound},)),
    ]
    objective = ScopedObjective(ObjSense.MINIMIZE, (Reduce("sum", x),))
    model = ScopedModel(rows, [Decision("x", lb=float("-inf"))])
    result = answer(
        cli,
        f"""SELECT id, x FROM (VALUES (1, 9), (2, 9)) t(id, cap)
        DECIDE x(INT) SUCH THAT x <= cap AND x >= {bound} MINIMIZE SUM(x) ORDER BY id""",
    )
    assert model.judge_scoped(result, clauses, objective) == pytest.approx(
        model.solve_scoped(scope_solver, clauses, objective), abs=1e-4
    )


@pytest.mark.var_integer
@pytest.mark.correctness
@pytest.mark.parametrize("cli_fixture", ["decidb_cli_gurobi", "decidb_cli_highs"])
def test_integer_bounds_without_a_whole_number(request, cli_fixture):
    """`2.3 <= x <= 2.7` holds no integer: infeasible on both solvers, and DIAGNOSE still
    names a bound to change (HiGHS refuses an inverted box on load, so the rounded box
    never reaches it)."""
    cli = request.getfixturevalue(cli_fixture)
    query = """SELECT id, x FROM (VALUES (1), (2)) t(id)
        DECIDE x(INT) SUCH THAT x >= 2.3 AND x <= 2.7 MINIMIZE SUM(x)"""
    assert "DECIDE optimization is infeasible" in cli.execute_raw(query).stderr
    diagnosis = cli.execute_raw(f"DIAGNOSE {query}")
    assert diagnosis.stdout.count("infeasible") >= 1, diagnosis.stdout + diagnosis.stderr


@pytest.mark.var_integer
@pytest.mark.correctness
@pytest.mark.parametrize("cli_fixture", ["decidb_cli_gurobi", "decidb_cli_highs"])
@pytest.mark.parametrize("shift", [0, 4])
def test_signed_extremum_bounded_by_a_column(request, scope_solver, cli_fixture, shift):
    """A signed decision whose upper side is a column bound (`x <= cap - shift`, a row per
    data row) still gives the MIN/MAX rewrite a finite box on HiGHS: a row over one
    decision bounds it whatever its sign, by a bound that may be negative. HiGHS refused
    "requires a finite bound on 'x'"."""
    cli = request.getfixturevalue(cli_fixture)
    rows = [{"id": 1, "cap": 3}, {"id": 2, "cap": 5}, {"id": 3, "cap": 2}]
    x = lambda r: {"x": 1}  # noqa: E731
    clauses = [
        Scoped((x,), "<=", (lambda r: {CONSTANT: r["cap"] - shift},)),
        Scoped((Reduce("max", x),), ">", (lambda r: {CONSTANT: 0},)),
    ]
    objective = ScopedObjective(ObjSense.MINIMIZE, (Reduce("sum", x),))
    model = ScopedModel(rows, [Decision("x", lb=-2.0)])
    result = answer(
        cli,
        f"""SELECT id, x FROM (VALUES (1, 3), (2, 5), (3, 2)) t(id, cap)
        DECIDE x(INT) SUCH THAT x >= -2 AND x <= cap - {shift} AND MAX(x) > 0 MINIMIZE SUM(x) ORDER BY id""",
    )
    assert model.judge_scoped(result, clauses, objective) == pytest.approx(
        model.solve_scoped(scope_solver, clauses, objective), abs=1e-4
    )


# ---------------------------------------------------------------------------
# A negative lower bound under a `when` lowers the floor on the rows the `when` keeps
# ---------------------------------------------------------------------------
#
# A constant lower bound replaces the default floor of 0 on the instances whose rows it keeps. A decision shared by
# several rows (keyed or query-wide) is signed when any row the `when` keeps lowers its floor. A data bound under a
# `when` still only intersects with the default, and so does a bound beside a floor written without a `when`.
#
# The oracle states the ruling as rows of its own: one column per instance of the decision, lowered to -inf when some
# row lowers it, with a constraint `x >= floor` for each kept row, and 0 otherwise. It reads no DeciDB bound.

_LIFT_BACKENDS = pytest.mark.parametrize("cli_fixture", ["decidb_cli_gurobi", "decidb_cli_highs"])
_LIFT_MODES = ("off", "auto")
_LIFT_COLUMNS = ("id", "g", "p", "lo")
_LIFT_ROWS = [
    {"id": 1, "g": "A", "p": True, "lo": 3},
    {"id": 2, "g": "A", "p": False, "lo": 3},
    {"id": 3, "g": "B", "p": False, "lo": 3},
    {"id": 4, "g": "C", "p": True, "lo": 3},
]


class _Lift:
    """One query and the model the ruling gives it.

    ``instance`` maps a row to the decision instance it reads (its id, its key, or ``()`` for a query-wide decision).
    ``floors`` are the constant lower bounds as ``row -> value`` (None where the row is not kept), ``lowers`` and
    ``uppers`` the bounds that only intersect, ``domain`` an ``IN`` list on the kept rows. ``once`` makes the objective
    count each instance once (a bare query-wide decision) instead of once per row (``SUM(x)``).
    """

    def __init__(
        self, sql, rows, instance, floors, uppers=(), lowers=(), domain=None, once=False, kind=VarType.INTEGER
    ):
        self.sql, self.rows, self.instance = sql, rows, instance
        self.floors, self.uppers, self.lowers, self.domain = floors, uppers, lowers, domain
        self.once, self.kind = once, kind

    def instances(self):
        return list(dict.fromkeys(self.instance(row) for row in self.rows))

    def solve(self, solver):
        solver.create_model("lifted_floor")
        name = {instance: f"x{index}" for index, instance in enumerate(self.instances())}
        cost = {}
        for instance, column in name.items():
            rows = [r for r in self.rows if self.instance(r) == instance]
            lifted = any(floor(r) is not None for r in rows for floor in self.floors)
            solver.add_variable(column, self.kind, float("-inf") if lifted else 0.0, None)
            cost[column] = 1.0 if self.once else float(len(rows))
        for row in self.rows:
            column = name[self.instance(row)]
            for bounds, sense in ((self.floors, ">="), (self.lowers, ">="), (self.uppers, "<=")):
                for bound in bounds:
                    if bound(row) is not None:
                        solver.add_constraint({column: 1.0}, sense, float(bound(row)))
            if self.domain and self.domain(row) is not None:
                add_in_domain(solver, column, self.domain(row), name_prefix=f"in_{row['id']}")
        solver.set_objective(cost, ObjSense.MINIMIZE)
        return solver.solve()

    def judge(self, rows, columns, result):
        """DeciDB's answer reaches the oracle's optimum and gives every row of an instance one value."""
        shown = dict(zip((r[columns.index("id")] for r in rows), (r[columns.index("x")] for r in rows)))
        seen = {}
        for row in self.rows:
            value = seen.setdefault(self.instance(row), shown[row["id"]])
            assert shown[row["id"]] == pytest.approx(value), shown
        total = sum(seen.values()) if self.once else sum(shown[r["id"]] for r in self.rows)
        assert total == pytest.approx(result.objective_value, abs=1e-4), shown


def _lift_check(cli, scope_solver, case):
    result = case.solve(scope_solver)
    for mode in _LIFT_MODES:
        sql = f"SET decide_direct_solve='{mode}'; {case.sql}"
        if result.status == SolverStatus.INFEASIBLE:
            with pytest.raises(
                DeciDBCliError, match=r"DECIDE optimization is infeasible\. Prefix the query with DIAGNOSE"
            ):
                cli.execute(sql)
            continue
        assert result.status == SolverStatus.OPTIMAL, result.status
        rows, columns = cli.execute(sql)
        case.judge(rows, columns, result)


def _lift_query(rows, decide, columns=_LIFT_COLUMNS, objective="MINIMIZE SUM(x)"):
    def literal(value):
        if isinstance(value, bool):
            return "true" if value else "false"
        return "NULL" if value is None else repr(value)

    tuples = ", ".join("(" + ", ".join(literal(r[c]) for c in columns) + ")" for r in rows)
    return f"SELECT id, x FROM (VALUES {tuples}) t({', '.join(columns)}) {decide} {objective} ORDER BY id"


_KEPT_P = lambda r: -5 if r["p"] is True else None  # noqa: E731
_KEPT_A = lambda r: -5 if r["g"] == "A" else None  # noqa: E731
_ANY = lambda r: 10  # noqa: E731
_ROW = lambda r: r["id"]  # noqa: E731
_BY_G = lambda r: r["g"]  # noqa: E731
_ONE = lambda r: ()  # noqa: E731
_REAL = VarType.CONTINUOUS

_LIFT_CASES = {
    # The floor lowers the instances of the rows the `when` keeps, and the others keep 0: -5 on A, 0 on B and C.
    "row_scoped_real": lambda: _Lift(
        _lift_query(_LIFT_ROWS, "DECIDE x(REAL) SUCH THAT when g = 'A': x >= -5 AND x <= 10"),
        _LIFT_ROWS, _ROW, [_KEPT_A], uppers=[_ANY], kind=_REAL),
    # A keyed decision is lowered by one kept row: class A has a kept row (id 1) and an unkept one (id 2).
    "keyed_class_lowered_by_one_kept_row": lambda: _Lift(
        _lift_query(_LIFT_ROWS, "DECIDE per g: x(REAL) SUCH THAT when p: x >= -5 AND x <= 10"),
        _LIFT_ROWS, _BY_G, [_KEPT_P], uppers=[_ANY], kind=_REAL),
    # A query-wide decision is lowered when any row the `when` keeps lowers it.
    "query_wide_lowered_by_one_kept_row": lambda: _Lift(
        _lift_query(_LIFT_ROWS, "DECIDE per (): x(REAL) SUCH THAT when p: x >= -5 AND x <= 10", objective="MINIMIZE x"),
        _LIFT_ROWS, _ONE, [_KEPT_P], uppers=[_ANY], once=True, kind=_REAL),
    # Two floors keep one row: the tighter one decides.
    "two_floors_on_one_row": lambda: _Lift(
        _lift_query(_LIFT_ROWS, "DECIDE x(REAL) SUCH THAT when p: x >= -5 AND when p: x >= -3 AND x <= 10"),
        _LIFT_ROWS, _ROW, [_KEPT_P, lambda r: -3 if r["p"] is True else None], uppers=[_ANY], kind=_REAL),
    # A floor written without a `when` leaves the decision with a lower bound written: a floor under a `when` only
    # intersects with it, on every row.
    "unconditional_floor_beside_a_conditional_one": lambda: _Lift(
        _lift_query(_LIFT_ROWS, "DECIDE x(REAL) SUCH THAT x >= -2 AND when p: x >= -5 AND x <= 10"),
        _LIFT_ROWS, _ROW, [lambda r: -2, _KEPT_P], uppers=[_ANY], kind=_REAL),
    # A bound of data is a row and does not replace the default floor.
    "data_bound_under_a_when_intersects": lambda: _Lift(
        _lift_query(_LIFT_ROWS, "DECIDE x(REAL) SUCH THAT when p: x >= lo - 8 AND x <= 10"),
        _LIFT_ROWS, _ROW, [], lowers=[lambda r: r["lo"] - 8 if r["p"] is True else None], uppers=[_ANY], kind=_REAL),
    # A NULL `when` keeps no row: the default holds on row 2, and row 3 is kept.
    "null_when_keeps_the_default": lambda: _Lift(
        _lift_query(
            [{"id": 1, "g": "A", "p": None, "lo": 3}, {"id": 2, "g": "A", "p": True, "lo": 3}],
            "DECIDE x(REAL) SUCH THAT when p: x >= -5 AND x <= 10"),
        [{"id": 1, "g": "A", "p": None, "lo": 3}, {"id": 2, "g": "A", "p": True, "lo": 3}],
        _ROW, [_KEPT_P], uppers=[_ANY], kind=_REAL),
    # A `when` that keeps no row lowers nothing.
    "when_that_keeps_no_row": lambda: _Lift(
        _lift_query([dict(r, p=False) for r in _LIFT_ROWS], "DECIDE x(REAL) SUCH THAT when p: x >= -5 AND x <= 10"),
        [dict(r, p=False) for r in _LIFT_ROWS], _ROW, [_KEPT_P], uppers=[_ANY], kind=_REAL),
    # An easy MIN reads each member as `x >= K`, under the clause's `when` and under its own filter.
    "easy_min_under_a_when": lambda: _Lift(
        _lift_query(_LIFT_ROWS, "DECIDE x(INT) SUCH THAT when p: min(x) >= -5 AND x <= 10"),
        _LIFT_ROWS, _ROW, [_KEPT_P], uppers=[_ANY]),
    "easy_min_with_a_reducer_filter": lambda: _Lift(
        _lift_query(_LIFT_ROWS, "DECIDE x(INT) SUCH THAT min(when p: x) >= -5 AND x <= 10"),
        _LIFT_ROWS, _ROW, [_KEPT_P], uppers=[_ANY]),
    # The recorded bug: a negative value in an IN domain under a `when`.
    "in_domain_under_a_when": lambda: _Lift(
        _lift_query(_LIFT_ROWS[:3], "DECIDE x(INT) SUCH THAT when p: x IN (-5, 7, 12) AND x <= 20"),
        _LIFT_ROWS[:3], _ROW, [_KEPT_P], uppers=[lambda r: 20],
        domain=lambda r: [-5, 7, 12] if r["p"] is True else None),
    # A positive floor under a `when` replaces the default of 0 on the rows it keeps, which a normal solve cannot tell
    # from an intersection; test_diagnose_repairs_a_conditional_floor_as_it_repairs_an_unconditional_one does.
    "positive_floor_under_a_when": lambda: _Lift(
        _lift_query(_LIFT_ROWS, "DECIDE x(INT) SUCH THAT when p: x >= 3 AND x <= 10"),
        _LIFT_ROWS, _ROW, [lambda r: 3 if r["p"] is True else None], uppers=[_ANY]),
    # `x <= -2` beside `when p: x >= -5`: every row kept leaves -5..-2 to the solve, and an unkept row (id 2) holds the
    # default floor of 0, which `x <= -2` rules out. The solve says so, not the early check of the domain.
    "upper_below_zero_every_row_kept": lambda: _Lift(
        _lift_query([dict(r, p=True) for r in _LIFT_ROWS], "DECIDE x(REAL) SUCH THAT x <= -2 AND when p: x >= -5"),
        [dict(r, p=True) for r in _LIFT_ROWS], _ROW, [_KEPT_P], uppers=[lambda r: -2], kind=_REAL),
    "upper_below_zero_some_row_unkept": lambda: _Lift(
        _lift_query(_LIFT_ROWS, "DECIDE x(REAL) SUCH THAT x <= -2 AND when p: x >= -5"),
        _LIFT_ROWS, _ROW, [_KEPT_P], uppers=[lambda r: -2], kind=_REAL),
}


@_LIFT_BACKENDS
@pytest.mark.var_real
@pytest.mark.var_integer
@pytest.mark.when
@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(_LIFT_CASES))
def test_negative_lower_bound_under_a_when_lowers_the_floor_on_the_rows_it_keeps(
    request, scope_solver, cli_fixture, name
):
    _lift_check(request.getfixturevalue(cli_fixture), scope_solver, _LIFT_CASES[name]())


@_LIFT_BACKENDS
@pytest.mark.var_real
@pytest.mark.when
@pytest.mark.correctness
def test_upper_bound_below_zero_with_an_unkept_row_is_left_to_the_solve(request, cli_fixture):
    """The early check that an upper bound below 0 contradicts the default floor counts a floor under a `when` as
    lowering it, so it leaves the query to the solve and the message is the solve's."""
    cli = request.getfixturevalue(cli_fixture)
    error = cli.execute_raw(_LIFT_CASES["upper_below_zero_some_row_unkept"]().sql).stderr
    assert "DECIDE optimization is infeasible" in error and "non-negative" not in error, error


@pytest.mark.var_integer
@pytest.mark.when
@pytest.mark.correctness
def test_diagnose_quotes_the_conditional_floor_and_not_the_default_of_other_rows(decidb_cli):
    """`SUM(x) = -11` over two kept rows needs more than the floors give (-5 each). The one repair is the clause
    `when p: x >= -5` (to -6); the unkept row 2 keeps its default floor of 0, which no clause states, so it is neither
    quoted nor widened to make the sum reachable."""
    script = (
        ".mode csv\n"
        "DIAGNOSE SELECT id, p, x FROM (VALUES (1, true), (2, false), (3, true)) t(id, p) "
        "DECIDE x(INT) SUCH THAT when p: x >= -5 AND SUM(x) = -11 AND x <= 4 MINIMIZE SUM(x);\n"
    )
    proc = decidb_cli.execute_script(script)
    findings = [line.split(",") for line in proc.stdout.splitlines()[1:]]
    clauses = [f for f in findings if f[0] == "infeasible" and f[1] != "NULL"]
    assert [(f[1], f[2]) for f in clauses] == [("when p: x >= -5", "when p: x >= -6")], proc.stdout + proc.stderr


@pytest.mark.var_integer
@pytest.mark.when
@pytest.mark.correctness
@pytest.mark.parametrize("floor_clause", ["x >= 3", "when p: x >= 3"])
def test_diagnose_repairs_a_conditional_floor_as_it_repairs_an_unconditional_one(decidb_cli, floor_clause):
    """`SUM(x) <= -2` over two rows holding a floor of 3 is repaired by the one edit that lowers the floor, with or
    without a `when`. The `when` must not leave a hidden floor of 0 on the rows it keeps, which would need a second edit
    (`sum(x) <= 0`) as well."""
    script = (
        ".mode csv\n"
        "DIAGNOSE SELECT id, p, x FROM (VALUES (1, true), (2, true)) t(id, p) "
        f"DECIDE x(INT) SUCH THAT {floor_clause} AND SUM(x) <= -2 AND x <= 9 MINIMIZE SUM(x);\n"
    )
    proc = decidb_cli.execute_script(script)
    findings = [line.split(",") for line in proc.stdout.splitlines()[1:]]
    clauses = [f for f in findings if f[0] == "infeasible" and f[1] != "NULL"]
    repaired = floor_clause.replace("3", "-1")
    assert [(f[1], f[2]) for f in clauses] == [(floor_clause, repaired)], proc.stdout + proc.stderr


@pytest.mark.var_real
@pytest.mark.when
@pytest.mark.correctness
def test_upper_bound_below_zero_beside_a_non_negative_conditional_floor_is_the_domain_error(decidb_cli):
    """A floor of 0 or more under a `when` cannot make `x <= -2` feasible, so the early domain check still names it."""
    error = decidb_cli.execute_raw(
        "SELECT id, x FROM (VALUES (1, true), (2, true)) t(id, p) "
        "DECIDE x(REAL) SUCH THAT x <= -2 AND when p: x >= 3 MINIMIZE SUM(x)"
    ).stderr
    assert "non-negative" in error, error
