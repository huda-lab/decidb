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
  - test_negated_extremum_*: a lone negated MIN/MAX in a constraint is a term's sign, not a factor of -1
"""

import time

import pytest

from decidb_cli import DeciDBCliError
from solver.types import VarType, ObjSense, SolverStatus
from ._brute_force import judge_rows
from ._oracle_helpers import emit_hard_inner_max, emit_hard_inner_min
from ._scope_cases import answer, dump_counts, scope_solver, table  # noqa: F401
from ._scope_oracle import CONSTANT, Decision, Reduce, Scoped, ScopedModel, ScopedObjective


def _add_exact_abs(oracle, d_name, x_name, offset, big_m, tag):
    """Pin ``d = |x - offset|`` exactly (lower envelope + Big-M upper envelope)."""
    y = f"{tag}_y"
    oracle.add_variable(d_name, VarType.CONTINUOUS, lb=0.0)
    oracle.add_variable(y, VarType.BINARY)
    two_m = 2.0 * big_m
    oracle.add_constraint({d_name: 1.0, x_name: -1.0}, ">=", -offset, name=f"{tag}_lo1")
    oracle.add_constraint({d_name: 1.0, x_name: 1.0}, ">=", offset, name=f"{tag}_lo2")
    oracle.add_constraint(
        {d_name: 1.0, x_name: -1.0, y: two_m},
        "<=",
        -offset + two_m,
        name=f"{tag}_hi1",
    )
    oracle.add_constraint(
        {d_name: 1.0, x_name: 1.0, y: -two_m},
        "<=",
        offset,
        name=f"{tag}_hi2",
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

    data = duckdb_conn.execute(
        """
        SELECT CAST(l_quantity AS DOUBLE)
        FROM lineitem WHERE l_orderkey <= 3
    """
    ).fetchall()
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
            {f"da_{i}": 1.0, f"db_{i}": -1.0},
            "<=",
            0.0,
            name=f"row_{i}",
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
    decidb_obj = sum(float(r[ci["a"]]) + float(r[ci["b"]]) for r in decidb_rows)
    assert (
        abs(decidb_obj - result.objective_value) <= 1e-4
    ), f"Objective mismatch: DeciDB={decidb_obj:.6f}, Oracle={result.objective_value:.6f}"
    # The sign-blind formulation is feasible-but-wrong rather than an error, so
    # assert the constraint actually holds row by row.
    for r in decidb_rows:
        av, bv, qv = float(r[ci["a"]]), float(r[ci["b"]]), float(r[ci["l_quantity"]])
        assert abs(av - 3.0) <= abs(bv - qv) + 1e-4, f"constraint violated: |{av}-3| > |{bv}-{qv}|"

    perf_tracker.record(
        "abs_negated_term_needs_bigm",
        decidb_time,
        build_time,
        result.solve_time_seconds,
        n * 2,
        n * 4,
        n * 5,
        result.objective_value,
        oracle_solver.solver_name(),
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

    n = duckdb_conn.execute("SELECT COUNT(*) FROM lineitem WHERE l_orderkey <= 3").fetchone()[0]
    assert n > 0

    t_build = time.perf_counter()
    oracle_solver.create_model("composed_minmax_negated")
    for i in range(n):
        oracle_solver.add_variable(f"x_{i}", VarType.CONTINUOUS, lb=0.0, ub=5.0)
        oracle_solver.add_variable(f"y_{i}", VarType.CONTINUOUS, lb=0.0, ub=7.0)
    z = emit_hard_inner_max(
        oracle_solver,
        "maxy",
        [{f"y_{i}": 1.0} for i in range(n)],
        row_ub=7.0,
    )
    outer = {f"x_{i}": 1.0 for i in range(n)}
    outer[z] = -1.0
    oracle_solver.add_constraint(outer, "<=", 0.0, name="outer")
    oracle_solver.set_objective(
        {f"x_{i}": 1.0 for i in range(n)},
        ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL

    ci = {c: i for i, c in enumerate(decidb_cols)}
    sum_x = sum(float(r[ci["x"]]) for r in decidb_rows)
    max_y = max(float(r[ci["y"]]) for r in decidb_rows)
    assert (
        abs(sum_x - result.objective_value) <= 1e-4
    ), f"Objective mismatch: DeciDB={sum_x:.6f}, Oracle={result.objective_value:.6f}"
    assert sum_x <= max_y + 1e-4, f"constraint violated: SUM(x)={sum_x} > MAX(y)={max_y}"

    perf_tracker.record(
        "composed_minmax_negated_reducer",
        decidb_time,
        build_time,
        result.solve_time_seconds,
        n * 2,
        n,
        2,
        result.objective_value,
        oracle_solver.solver_name(),
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

    n = duckdb_conn.execute("SELECT COUNT(*) FROM lineitem WHERE l_orderkey <= 3").fetchone()[0]
    assert n > 0

    t_build = time.perf_counter()
    oracle_solver.create_model("leading_negative_reducer")
    for i in range(n):
        oracle_solver.add_variable(f"x_{i}", VarType.CONTINUOUS, lb=0.0, ub=9.0)
    z = emit_hard_inner_max(
        oracle_solver,
        "maxx",
        [{f"x_{i}": 1.0} for i in range(n)],
        row_ub=9.0,
    )
    oracle_solver.add_constraint({z: -1.0}, "<=", -3.0, name="outer")
    oracle_solver.set_objective(
        {f"x_{i}": 1.0 for i in range(n)},
        ObjSense.MINIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL

    ci = {c: i for i, c in enumerate(decidb_cols)}
    sum_x = sum(float(r[ci["x"]]) for r in decidb_rows)
    max_x = max(float(r[ci["x"]]) for r in decidb_rows)
    assert (
        abs(sum_x - result.objective_value) <= 1e-4
    ), f"Objective mismatch: DeciDB={sum_x:.6f}, Oracle={result.objective_value:.6f}"
    assert max_x >= 3.0 - 1e-4, f"constraint violated: MAX(x)={max_x} < 3"

    perf_tracker.record(
        "leading_negative_reducer",
        decidb_time,
        build_time,
        result.solve_time_seconds,
        n,
        n,
        2,
        result.objective_value,
        oracle_solver.solver_name(),
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
    decidb_rows, decidb_cols = decidb_cli.execute(
        """
        SELECT id, v, x FROM (VALUES (1, 10.0), (2, 12.0)) t(id, v)
        DECIDE x(BOOL)
        SUCH THAT SUM(x) <= 2
        MAXIMIZE MAX(x * v) - MIN(x * v)
    """
    )
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
        {f"x_{i}": 1.0 for i in range(n)},
        "<=",
        2.0,
        name="card",
    )
    oracle_solver.set_objective({zmax: 1.0, zmin: -1.0}, ObjSense.MAXIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL

    ci = {c: i for i, c in enumerate(decidb_cols)}
    picked = [float(r[ci["v"]]) * int(r[ci["x"]]) for r in decidb_rows]
    decidb_obj = max(picked) - min(picked)
    assert (
        abs(decidb_obj - result.objective_value) <= 1e-4
    ), f"Objective mismatch: DeciDB={decidb_obj:.6f}, Oracle={result.objective_value:.6f}"

    perf_tracker.record(
        "composed_minmax_objective_subtraction",
        decidb_time,
        build_time,
        result.solve_time_seconds,
        n,
        n * 2,
        1,
        result.objective_value,
        oracle_solver.solver_name(),
        comparison_status="optimal",
    )


# ---------------------------------------------------------------------------
# A lone negated MIN or MAX: the sign is the term's, never a factor of -1.
# ---------------------------------------------------------------------------
#
# `-MAX(x) >= -4` and `-1 * MAX(x) >= -4` are one problem. The first reaches the MIN/MAX rewrite as a negated
# reducer, the second as a reducer under the factor -1; the rewrite reads the direction as the comparison's times the
# term's sign times its factor's sign (WalkComposedLhs does the same), so both build one model.

_SIGN_COLUMNS = ("id", "g", "w")
_SIGN_ROWS = ((1, "a", 2), (2, "a", 3), (3, "b", 4))


def _sign_sql(clause: str, sense: str) -> str:
    return f"""
        SELECT id, x {table('t', _SIGN_COLUMNS, _SIGN_ROWS)}
        DECIDE x(INT)
        SUCH THAT x <= 6 AND {clause}
        {sense} SUM(x * t.w)
        ORDER BY id"""


# (clause, aggregate, the term's total multiplier, relation, bound, objective sense). The objective pushes against
# the clause: MAXIMIZE binds a cap on every member (the easy direction), MINIMIZE a reach by some member (the hard).
_NEGATED_EXTREMUM_CASES = [
    pytest.param("-MAX(x) >= -4", "max", -1, ">=", -4, "MAXIMIZE", id="sign-easy-max"),
    pytest.param("-MAX(x) <= -3", "max", -1, "<=", -3, "MINIMIZE", id="sign-hard-max"),
    pytest.param("-MIN(x) <= -2", "min", -1, "<=", -2, "MINIMIZE", id="sign-easy-min"),
    pytest.param("-MIN(x) >= -2", "min", -1, ">=", -2, "MAXIMIZE", id="sign-hard-min"),
    pytest.param("-(2 * MAX(x)) >= -8", "max", -2, ">=", -8, "MAXIMIZE", id="sign-positive-factor-easy"),
    pytest.param("-(2 * MAX(x)) <= -6", "max", -2, "<=", -6, "MINIMIZE", id="sign-positive-factor-hard"),
    pytest.param("-(-2 * MAX(x)) <= 8", "max", 2, "<=", 8, "MAXIMIZE", id="sign-negative-factor-easy"),
    pytest.param("-(-2 * MIN(x)) <= 4", "min", 2, "<=", 4, "MAXIMIZE", id="sign-negative-factor-hard"),
    pytest.param("-MAX(x) = -4", "max", -1, "=", -4, "MAXIMIZE", id="sign-equality"),
    pytest.param("-(2 * MIN(x)) = -4", "min", -2, "=", -4, "MAXIMIZE", id="sign-factor-equality"),
    pytest.param("-((SELECT 2) * MAX(x)) >= -8", "max", -2, ">=", -8, "MAXIMIZE", id="unknown-factor-easy"),
    pytest.param("-((SELECT 2) * MAX(x)) <= -6", "max", -2, "<=", -6, "MINIMIZE", id="unknown-factor-hard"),
]


@pytest.mark.min_max
@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize("clause, agg, multiplier, relation, bound, sense", _NEGATED_EXTREMUM_CASES)
def test_negated_extremum_answers(decidb_cli, scope_solver, clause, agg, multiplier, relation, bound, sense):
    """A negated MIN/MAX answers as the oracle does, in every direction its sign, its factor's sign and the relation
    give: a cap on each member (easy), a reach by one member (hard), both for `=`, and a factor of unknown sign (the
    composed formulation, which reads the sign as a term's)."""
    model = ScopedModel(
        [dict(zip(_SIGN_COLUMNS, row)) for row in _SIGN_ROWS],
        [Decision("x", var_type=VarType.INTEGER, ub=6)],
    )
    reducer = Reduce(agg, lambda r: {"x": 1.0}, factor=lambda r: float(multiplier))
    clauses = [Scoped((reducer,), relation, (lambda r: {CONSTANT: bound},))]
    objective = ScopedObjective(ObjSense[sense], (Reduce("sum", lambda r: {"x": r["w"]}),))
    result = answer(decidb_cli, _sign_sql(clause, sense))
    assert model.judge_scoped(result, clauses, objective) == pytest.approx(
        model.solve_scoped(scope_solver, clauses, objective), abs=1e-4
    )


@pytest.mark.min_max
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_negated_extremum_per_group(decidb_cli, scope_solver):
    """`per g: -max(x) by (g) <= -3`: some member of each group reaches 3. The sign rides with the group key."""
    model = ScopedModel(
        [dict(zip(_SIGN_COLUMNS, row)) for row in _SIGN_ROWS],
        [Decision("x", var_type=VarType.INTEGER, ub=6)],
    )
    reducer = Reduce("max", lambda r: {"x": 1.0}, by=("g",), factor=lambda r: -1.0)
    clauses = [Scoped((reducer,), "<=", (lambda r: {CONSTANT: -3},), per=("g",))]
    objective = ScopedObjective(ObjSense.MINIMIZE, (Reduce("sum", lambda r: {"x": r["w"]}),))
    result = answer(decidb_cli, _sign_sql("per g: -max(x) by (g) <= -3", "MINIMIZE"))
    assert model.judge_scoped(result, clauses, objective) == pytest.approx(
        model.solve_scoped(scope_solver, clauses, objective), abs=1e-4
    )


# The written factor `-1 *` and the sign build one model: the dump is the model after presolve and formulation.
_SAME_MODEL_CASES = [
    pytest.param("-MAX(x * t.w) <= -2", "-1 * MAX(x * t.w) <= -2", id="hard-max"),
    pytest.param("-MIN(x * t.w) >= -5", "-1 * MIN(x * t.w) >= -5", id="hard-min"),
    pytest.param("-MAX(x * t.w) >= -5", "-1 * MAX(x * t.w) >= -5", id="easy-max"),
    pytest.param("-(2 * MIN(x * t.w)) = -4", "-2 * MIN(x * t.w) = -4", id="equality-with-a-factor"),
    pytest.param("-(-2 * MAX(x * t.w)) >= 4", "2 * MAX(x * t.w) >= 4", id="sign-over-a-negative-factor"),
]


@pytest.mark.min_max
@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize("negated, factored", _SAME_MODEL_CASES)
def test_negated_extremum_builds_the_model_of_its_factor(decidb_cli, tmp_path, negated, factored):
    """One problem, two spellings, one model: a sign and a factor of -1 give the same rows."""

    def dump(clause: str, name: str) -> str:
        return decidb_cli.dump_model(
            f"""
            SELECT id, x FROM (VALUES (1, 2.0), (2, 3.0), (3, 5.0)) t(id, w)
            DECIDE x(REAL)
            SUCH THAT x <= t.w AND {clause}
            MINIMIZE SUM(x)""",
            tmp_path / name,
        )

    assert dump(negated, "negated.txt") == dump(factored, "factored.txt")


@pytest.mark.min_max
@pytest.mark.cons_aggregate
@pytest.mark.quadratic
@pytest.mark.correctness
def test_negated_extremum_member_is_bounded_per_row(decidb_cli_gurobi, tmp_path):
    """`-MAX(POWER(x - w, 2)) >= -4` is the cheap direction (every squared deviation at most 4), so each member is a
    quadratic row and no definition of the body is needed: the model has one column per row. Read with the sign
    dropped it would be the expensive direction, which defines the body in a column of its own."""
    rows = ((1, 1), (2, 3), (3, 5))
    sql = f"""
        SELECT id, x {table('t', ('id', 'w'), rows)}
        DECIDE x(INT)
        SUCH THAT x <= 6 AND -MAX(POWER(x - t.w, 2)) >= -4
        MAXIMIZE SUM(x)
        ORDER BY id"""
    answers = answer(decidb_cli_gurobi, sql)
    assert dump_counts(decidb_cli_gurobi.dump_model(sql, tmp_path / "model.txt"))["num_vars"] == 3
    judge_rows(
        answers,
        {f"x{i}": range(7) for i in range(3)},
        lambda a: all((a[f"x{i}"] - rows[i][1]) ** 2 <= 4 for i in range(3)),
        lambda a: sum(a.values()),
    )


# ---------------------------------------------------------------------------
# An objective's sense is a sign: `MAXIMIZE f` and `MINIMIZE -f` are one problem.
# ---------------------------------------------------------------------------

_SAME_OBJECTIVE_CASES = [
    pytest.param("MAXIMIZE MIN(x * t.w)", "MINIMIZE -MIN(x * t.w)", "min", id="flat-min"),
    pytest.param("MAXIMIZE MAX(x * t.w)", "MINIMIZE -MAX(x * t.w)", "max", id="flat-max"),
    pytest.param(
        "MAXIMIZE when t.g = 'a': MIN(x * t.w)", "MINIMIZE when t.g = 'a': -MIN(x * t.w)", "min", id="under-when"
    ),
]


@pytest.mark.min_max
@pytest.mark.obj_maximize
@pytest.mark.correctness
@pytest.mark.parametrize("maximized, negated, agg", _SAME_OBJECTIVE_CASES)
def test_maximize_an_extremum_and_minimize_its_negation_are_one_problem(
    decidb_cli, scope_solver, tmp_path, maximized, negated, agg
):
    """Both spellings build one model and answer as the oracle does, on the flat MIN/MAX path the sign selects."""
    when = (lambda r: r["g"] == "a") if "when" in maximized else None
    model = ScopedModel(
        [dict(zip(_SIGN_COLUMNS, row)) for row in _SIGN_ROWS],
        [Decision("x", var_type=VarType.INTEGER, ub=6)],
    )
    clauses = [Scoped((Reduce("sum", lambda r: {"x": 1.0}),), "<=", (lambda r: {CONSTANT: 9},), per=())]
    objective = ScopedObjective(ObjSense.MAXIMIZE, (Reduce(agg, lambda r: {"x": r["w"]}),), when=when)
    expected = model.solve_scoped(scope_solver, clauses, objective)

    def sql(objective_text):
        return f"""
            SELECT id, x {table('t', _SIGN_COLUMNS, _SIGN_ROWS)}
            DECIDE x(INT)
            SUCH THAT x <= 6 AND SUM(x) <= 9
            {objective_text}
            ORDER BY id"""

    for text in (maximized, negated):
        assert model.judge_scoped(answer(decidb_cli, sql(text)), clauses, objective) == pytest.approx(
            expected, abs=1e-4
        )
    assert decidb_cli.dump_model(sql(maximized), tmp_path / "maximized.txt") == decidb_cli.dump_model(
        sql(negated), tmp_path / "negated.txt"
    )


# ---------------------------------------------------------------------------
# A minus over a factor that reads a decision is the term's sign: `-b * SUM(x)` is `-(b * SUM(x))`.
# ---------------------------------------------------------------------------
#
# The minus lives only in the model's algebra there, so moving it is an identity. A minus over data is DuckDB's to
# evaluate, and it can raise, so it stays where it is.

_BOOL_FACTOR_DOMAINS = {**{f"x{i}": range(4) for i in range(3)}, "b": range(2)}


def _bool_factor_sql(clause, objective):
    return f"""
        SELECT id, x, b FROM (VALUES (1), (2), (3)) t(id)
        DECIDE x(INT), per (): b(BOOL)
        SUCH THAT x <= 3 AND {clause}
        {objective}
        ORDER BY id"""


def _bool_factor_sum(a):
    return a["x0"] + a["x1"] + a["x2"]


@pytest.mark.min_max
@pytest.mark.obj_maximize
@pytest.mark.correctness
@pytest.mark.parametrize(
    "maximized, minimized, scale",
    [
        pytest.param("-b * SUM(x) + 4 * b", "b * SUM(x) - 4 * b", 1, id="bare-minus"),
        pytest.param("2 * -b * SUM(x) + 8 * b", "2 * b * SUM(x) - 8 * b", 2, id="minus-inside-a-product"),
    ],
)
def test_maximize_a_negated_boolean_factor_and_minimize_the_factor_are_one_problem(
    decidb_cli_highs, tmp_path, maximized, minimized, scale
):
    """`MAXIMIZE -b * SUM(x) + 4 * b` is `MINIMIZE b * SUM(x) - 4 * b`: both build one model, and each answers as every
    assignment of the three rows says (the objective is `b * (4 - SUM(x))`, best at b = 1 and SUM(x) = 2). The minus
    is taken off the factor whether it stands alone or inside a product."""
    clause = "SUM(x) >= 2 AND SUM(x) <= 7"
    maximized_sql = _bool_factor_sql(clause, f"MAXIMIZE {maximized}")
    minimized_sql = _bool_factor_sql(clause, f"MINIMIZE {minimized}")
    feasible = lambda a: 2 <= _bool_factor_sum(a) <= 7  # noqa: E731
    for sql, maximize in ((maximized_sql, True), (minimized_sql, False)):
        # The written objective: `scale * b * (4 - SUM(x))` maximized, or its negation minimized.
        value = lambda a: (1 if maximize else -1) * scale * a["b"] * (4 - _bool_factor_sum(a))  # noqa: E731
        judge_rows(answer(decidb_cli_highs, sql), _BOOL_FACTOR_DOMAINS, feasible, value, maximize)
    assert decidb_cli_highs.dump_model(maximized_sql, tmp_path / "maximized.txt") == decidb_cli_highs.dump_model(
        minimized_sql, tmp_path / "minimized.txt"
    )


@pytest.mark.min_max
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_a_constraint_with_a_negated_boolean_factor_answers_as_the_oracle(decidb_cli_highs):
    """`-b * SUM(x) >= -4` bounds b * SUM(x) by 4: b = 1 only while the sum stays at 4 or below."""
    sql = _bool_factor_sql("SUM(x) >= 2 AND -b * SUM(x) >= -4", "MAXIMIZE SUM(x) + 6 * b")
    judge_rows(
        answer(decidb_cli_highs, sql),
        _BOOL_FACTOR_DOMAINS,
        lambda a: _bool_factor_sum(a) >= 2 and -a["b"] * _bool_factor_sum(a) >= -4,
        lambda a: _bool_factor_sum(a) + 6 * a["b"],
    )


@pytest.mark.error
@pytest.mark.correctness
def test_a_minus_over_data_stays_in_the_data(decidb_cli_highs):
    """`-c * SUM(x)` with c the INT minimum: DuckDB evaluates `-c` and raises, as it does for the query written alone.
    Moving that minus onto the term's sign would drop the error."""
    sql = """
        SET decide_direct_solve='off';
        SELECT id, x FROM (VALUES (1, (-2147483647 - 1)::INTEGER), (2, 3)) t(id, c)
        DECIDE x(INT)
        SUCH THAT x <= 3 AND -c * SUM(x) <= 5
        MAXIMIZE SUM(x)"""
    with pytest.raises(DeciDBCliError, match="Overflow in negation"):
        decidb_cli_highs.execute(sql)
