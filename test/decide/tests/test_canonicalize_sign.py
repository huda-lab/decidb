"""Sign-awareness of the two plan-time analyses canonicalization made reachable.

Canonicalization moves every decision-bearing term onto the LHS, and a term that
crosses the relation arrives negated. Two analyses used to classify a term by the
side it was written on rather than by the sign it carries:

  - ABS Big-M pinning (``ClassifyAbsConstraints``) — a negated auxiliary is *not*
    pinned by an upper-bounded row, because raising it makes the row easier. Read
    side-wise, it looked pinned, and the constraint became vacuous.
  - composed MIN/MAX easy-vs-hard (``WalkComposedLhs``) — used to reject a
    subtracted term outright, so a negated reducer was a binder error.

Each test pins the objective against an independent oracle model: a shape that is
merely *accepted* but mis-classified still fails, which is the whole point — the
sign bugs produce a feasible, optimal-looking answer to the wrong model.

Covers:
  - test_abs_negated_term_needs_bigm: ABS(a - k) - ABS(b - col) <= 0
  - test_composed_minmax_negated_reducer: SUM(x) - MAX(y) <= 0
  - test_leading_negative_reducer: 3 - MAX(x) <= 0 (the `0 - term` rebuild idiom)
  - test_composed_minmax_objective_subtraction: MAXIMIZE MAX(x*v) - MIN(x*v)
"""

import time

import pytest

from solver.types import VarType, ObjSense, SolverStatus
from ._oracle_helpers import emit_hard_inner_max, emit_hard_inner_min


def _add_exact_abs(oracle, d_name, x_name, offset, big_m, tag):
    """Pin ``d = |x - offset|`` exactly (lower envelope + Big-M upper envelope)."""
    y = f"{tag}_y"
    oracle.add_variable(d_name, VarType.CONTINUOUS, lb=0.0)
    oracle.add_variable(y, VarType.BINARY)
    two_m = 2.0 * big_m
    oracle.add_constraint({d_name: 1.0, x_name: -1.0}, ">=", -offset, name=f"{tag}_lo1")
    oracle.add_constraint({d_name: 1.0, x_name: 1.0}, ">=", offset, name=f"{tag}_lo2")
    oracle.add_constraint(
        {d_name: 1.0, x_name: -1.0, y: two_m}, "<=", -offset + two_m, name=f"{tag}_hi1",
    )
    oracle.add_constraint(
        {d_name: 1.0, x_name: 1.0, y: -two_m}, "<=", offset, name=f"{tag}_hi2",
    )


@pytest.mark.var_real
@pytest.mark.var_multi
@pytest.mark.cons_perrow
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_abs_negated_term_needs_bigm(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """``ABS(a - 3) - ABS(b - l_quantity) <= 0`` — the subtracted ABS needs Big-M.

    The second auxiliary enters with coefficient -1, so the row upper-bounds
    ``|a-3| - aux_b``, not ``aux_b``. Without Big-M, ``aux_b`` floats upward and
    the constraint admits any ``a``; the objective would then report the free
    optimum ``a = 20`` on every row.
    """
    sql = """
        SELECT l_orderkey, l_linenumber, l_quantity, a, b
        FROM lineitem
        WHERE l_orderkey <= 3
        DECIDE a(REAL), b(REAL)
        SUCH THAT a <= 20 AND b <= 20 AND ABS(a - 3) - ABS(b - l_quantity) <= 0
        MAXIMIZE SUM(a) + SUM(b)
    """
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_quantity AS DOUBLE)
        FROM lineitem WHERE l_orderkey <= 3
    """).fetchall()
    n = len(data)
    assert n > 0

    t_build = time.perf_counter()
    oracle_solver.create_model("abs_negated_term")
    big_m = 20.0 + max(row[0] for row in data) + 3.0
    for i in range(n):
        qty = data[i][0]
        oracle_solver.add_variable(f"a_{i}", VarType.CONTINUOUS, lb=0.0, ub=20.0)
        oracle_solver.add_variable(f"b_{i}", VarType.CONTINUOUS, lb=0.0, ub=20.0)
        _add_exact_abs(oracle_solver, f"da_{i}", f"a_{i}", 3.0, big_m, f"da{i}")
        _add_exact_abs(oracle_solver, f"db_{i}", f"b_{i}", qty, big_m, f"db{i}")
        oracle_solver.add_constraint(
            {f"da_{i}": 1.0, f"db_{i}": -1.0}, "<=", 0.0, name=f"row_{i}",
        )
    obj = {}
    for i in range(n):
        obj[f"a_{i}"] = 1.0
        obj[f"b_{i}"] = 1.0
    oracle_solver.set_objective(obj, ObjSense.MAXIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL

    ci = {c: i for i, c in enumerate(decidb_cols)}
    decidb_obj = sum(
        float(r[ci["a"]]) + float(r[ci["b"]]) for r in decidb_rows
    )
    assert abs(decidb_obj - result.objective_value) <= 1e-4, (
        f"Objective mismatch: DecidB={decidb_obj:.6f}, Oracle={result.objective_value:.6f}"
    )
    # The sign-blind formulation is feasible-but-wrong rather than an error, so
    # assert the constraint actually holds row by row.
    for r in decidb_rows:
        av, bv, qv = float(r[ci["a"]]), float(r[ci["b"]]), float(r[ci["l_quantity"]])
        assert abs(av - 3.0) <= abs(bv - qv) + 1e-4, (
            f"constraint violated: |{av}-3| > |{bv}-{qv}|"
        )

    perf_tracker.record(
        "abs_negated_term_needs_bigm", decidb_time, build_time,
        result.solve_time_seconds, n * 2, n * 4, n * 5,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )


@pytest.mark.var_real
@pytest.mark.var_multi
@pytest.mark.cons_aggregate
@pytest.mark.min_max
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_composed_minmax_negated_reducer(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """``SUM(x) - MAX(y) <= 0`` — MAX arrives with sign -1, flipping easy to hard.

    ``-MAX(y)`` bounded above means ``MAX(y)`` is pushed *up*, which is the hard
    direction: the auxiliary must be pinned to some row's value by an indicator,
    not merely bounded below by every row. Rejected outright before Phase A.
    """
    sql = """
        SELECT l_orderkey, l_linenumber, x, y
        FROM lineitem
        WHERE l_orderkey <= 3
        DECIDE x(REAL), y(REAL)
        SUCH THAT SUM(x) - MAX(y) <= 0 AND x <= 5 AND y <= 7
        MAXIMIZE SUM(x)
    """
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    n = duckdb_conn.execute(
        "SELECT COUNT(*) FROM lineitem WHERE l_orderkey <= 3"
    ).fetchone()[0]
    assert n > 0

    t_build = time.perf_counter()
    oracle_solver.create_model("composed_minmax_negated")
    for i in range(n):
        oracle_solver.add_variable(f"x_{i}", VarType.CONTINUOUS, lb=0.0, ub=5.0)
        oracle_solver.add_variable(f"y_{i}", VarType.CONTINUOUS, lb=0.0, ub=7.0)
    z = emit_hard_inner_max(
        oracle_solver, "maxy", [{f"y_{i}": 1.0} for i in range(n)], row_ub=7.0,
    )
    outer = {f"x_{i}": 1.0 for i in range(n)}
    outer[z] = -1.0
    oracle_solver.add_constraint(outer, "<=", 0.0, name="outer")
    oracle_solver.set_objective(
        {f"x_{i}": 1.0 for i in range(n)}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL

    ci = {c: i for i, c in enumerate(decidb_cols)}
    sum_x = sum(float(r[ci["x"]]) for r in decidb_rows)
    max_y = max(float(r[ci["y"]]) for r in decidb_rows)
    assert abs(sum_x - result.objective_value) <= 1e-4, (
        f"Objective mismatch: DecidB={sum_x:.6f}, Oracle={result.objective_value:.6f}"
    )
    assert sum_x <= max_y + 1e-4, f"constraint violated: SUM(x)={sum_x} > MAX(y)={max_y}"

    perf_tracker.record(
        "composed_minmax_negated_reducer", decidb_time, build_time,
        result.solve_time_seconds, n * 2, n, 2,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )


@pytest.mark.var_real
@pytest.mark.cons_aggregate
@pytest.mark.min_max
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_leading_negative_reducer(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """``3 - MAX(x) <= 0`` — the reducer is the only LHS term and it is negated.

    Canonicalizes to ``0 - MAX(x) <= -3``: the ``0 - term`` idiom the additive
    rebuild emits for a leading negative term, with a bound side that is an
    expression rather than a literal.
    """
    sql = """
        SELECT l_orderkey, l_linenumber, x
        FROM lineitem
        WHERE l_orderkey <= 3
        DECIDE x(REAL)
        SUCH THAT 3 - MAX(x) <= 0 AND x <= 9
        MINIMIZE SUM(x)
    """
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    n = duckdb_conn.execute(
        "SELECT COUNT(*) FROM lineitem WHERE l_orderkey <= 3"
    ).fetchone()[0]
    assert n > 0

    t_build = time.perf_counter()
    oracle_solver.create_model("leading_negative_reducer")
    for i in range(n):
        oracle_solver.add_variable(f"x_{i}", VarType.CONTINUOUS, lb=0.0, ub=9.0)
    z = emit_hard_inner_max(
        oracle_solver, "maxx", [{f"x_{i}": 1.0} for i in range(n)], row_ub=9.0,
    )
    oracle_solver.add_constraint({z: -1.0}, "<=", -3.0, name="outer")
    oracle_solver.set_objective(
        {f"x_{i}": 1.0 for i in range(n)}, ObjSense.MINIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL

    ci = {c: i for i, c in enumerate(decidb_cols)}
    sum_x = sum(float(r[ci["x"]]) for r in decidb_rows)
    max_x = max(float(r[ci["x"]]) for r in decidb_rows)
    assert abs(sum_x - result.objective_value) <= 1e-4, (
        f"Objective mismatch: DecidB={sum_x:.6f}, Oracle={result.objective_value:.6f}"
    )
    assert max_x >= 3.0 - 1e-4, f"constraint violated: MAX(x)={max_x} < 3"

    perf_tracker.record(
        "leading_negative_reducer", decidb_time, build_time,
        result.solve_time_seconds, n, n, 2,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )


@pytest.mark.var_boolean
@pytest.mark.min_max
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_composed_minmax_objective_subtraction(decidb_cli, oracle_solver, perf_tracker):
    """``MAXIMIZE MAX(x*v) - MIN(x*v)`` — the spread of the selected values.

    The composed walker is shared between constraints and objectives, so the
    same sign descent unblocks this. Under MAXIMIZE both terms are hard: the MAX
    is pushed up, and the MIN carries sign -1 so it is pushed down.

    rows v=[10,12], `SUM(x) <= 2`. Selecting both gives 12-10 = 2. Selecting only
    the larger leaves the other row's `x*v` at 0, so MIN=0 and the spread is 12 —
    an optimum that requires the MIN auxiliary to be pinned rather than merely
    bounded.
    """
    vals = [10.0, 12.0]
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute("""
        SELECT id, v, x FROM (VALUES (1, 10.0), (2, 12.0)) t(id, v)
        DECIDE x(BOOL)
        SUCH THAT SUM(x) <= 2
        MAXIMIZE MAX(x * v) - MIN(x * v)
    """)
    decidb_time = time.perf_counter() - t0

    n = len(vals)
    t_build = time.perf_counter()
    oracle_solver.create_model("composed_minmax_obj_subtraction")
    for i in range(n):
        oracle_solver.add_variable(f"x_{i}", VarType.BINARY)
    rows = [{f"x_{i}": vals[i]} for i in range(n)]
    zmax = emit_hard_inner_max(oracle_solver, "spreadmax", rows, row_ub=max(vals))
    zmin = emit_hard_inner_min(oracle_solver, "spreadmin", rows, row_ub=max(vals))
    oracle_solver.add_constraint(
        {f"x_{i}": 1.0 for i in range(n)}, "<=", 2.0, name="card",
    )
    oracle_solver.set_objective({zmax: 1.0, zmin: -1.0}, ObjSense.MAXIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL

    ci = {c: i for i, c in enumerate(decidb_cols)}
    picked = [float(r[ci["v"]]) * int(r[ci["x"]]) for r in decidb_rows]
    decidb_obj = max(picked) - min(picked)
    assert abs(decidb_obj - result.objective_value) <= 1e-4, (
        f"Objective mismatch: DecidB={decidb_obj:.6f}, Oracle={result.objective_value:.6f}"
    )

    perf_tracker.record(
        "composed_minmax_objective_subtraction", decidb_time, build_time,
        result.solve_time_seconds, n, n * 2, 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )
