"""Nested reducers in the engine (language/guide.md §5.5; D79-D81).

A reducer nests in another to any depth, with `when`, `per` and `by` at every level. A
nested reducer reads, at the row it stands for, the rows its path keeps -- the clause's
`when` and every enclosing reducer's -- grouped by its path key: every enclosing `by` key
and its own. Every reducer of a nest is a helper decision keyed by that path, defined
once per group, and read by the reducers around it. Every phase-2 fixture of
``_anr_cases`` runs through DeciDB on both solvers and is judged by ``_anr_oracle``,
which reads no SQL; a grid of nests at depth 2 and 3 is judged cell by cell; F1's model
is pinned; and the refusals of D80 are named.
"""

import itertools

import pytest

from ._anr_cases import CASES, F1_MAX, F1_SUM
from ._anr_oracle import PER_ROW, Abs, AnrModel, Col, Decision, Objective, ScopeError, Var, clause, eq, ge, le, reduce
from ._output_helpers import assert_no_internal_leak
from ._scope_cases import answer, dump_counts, scope_solver, table, table_rows  # noqa: F401 -- fixture
from solver.types import ObjSense, SolverStatus, VarType

X = Var("x")
MAXIMIZE_X = Objective(ObjSense.MAXIMIZE, reduce("sum", X))
BOTH_SOLVERS = pytest.mark.parametrize("cli_fixture", ["decidb_cli_gurobi", "decidb_cli_highs"])


def _named(cases):
    return [pytest.param(case, id=case.name) for case in cases]


def _judged(cli, solver, sql, rows, decisions, clauses, objective=MAXIMIZE_X):
    """DeciDB's answer, judged by the oracle; returns the optimum both reach."""
    model = AnrModel(rows, decisions)
    expected = model.solve(solver, clauses, objective)
    assert expected.status == SolverStatus.OPTIMAL, expected.status
    reached = model.judge(answer(cli, sql, model.warnings(clauses, objective)), clauses, objective)
    assert reached == pytest.approx(expected.objective, abs=1e-6)
    return reached


# ---------------------------------------------------------------------------
# The phase-2 fixtures, judged by the oracle, on both solvers
# ---------------------------------------------------------------------------


@BOTH_SOLVERS
@pytest.mark.correctness
@pytest.mark.parametrize("case", _named(c for c in CASES if c.phase == 2))
def test_the_engine_answers_each_fixture(request, scope_solver, cli_fixture, case):
    """F1 (both inner reducers), F14, F16, F16_small, count(e) over a NULL, and an empty
    group inside a nest (D68): the hand-computed optimum, or the error the data raises."""
    cli = request.getfixturevalue(cli_fixture)
    result = cli.execute_raw(case.sql)
    assert_no_internal_leak(result, case.name)
    if case.error:
        assert ScopeError.PHRASES[case.error] in result.stderr, result.stderr
        return
    assert result.returncode == 0 and not result.stderr.strip(), result.stderr
    model = case.model()
    reached = model.judge(answer(cli, case.sql), case.clauses, case.objective)
    expected = case.optimum
    if expected is None:
        expected = model.solve(scope_solver, case.clauses, case.objective).objective
    assert reached == pytest.approx(expected, abs=1e-6)


@pytest.mark.correctness
@pytest.mark.parametrize(
    "case, counts",
    [
        pytest.param(F1_SUM, {"num_vars": 6, "num_rows": 3}, id="F1_sum"),
        pytest.param(F1_MAX, {"num_vars": 8, "num_rows": 7}, id="F1_max"),
    ],
)
def test_the_running_example_is_a_helper_per_path_group(decidb_cli, tmp_path, case, counts):
    """F1 is the deck's model (pp. 111-119). Each region's sum is written into the three
    clause rows that read it -- East once, West twice -- as each costs no more nonzeros there
    than defined (D89), the bound a row on it rather than its box. For F1_sum the inner sums
    are written into the regions in turn: the six shipments and three rows (p. 119). For
    F1_max the inner MAX of c1 and of c4, two members each, keep their columns and an
    envelope row per member; c2's one member is the MAX (p. 112), and the group `when t.p`
    leaves empty, c3, has no value and no column: eight columns, four envelope rows and the
    three clause rows (p. 115)."""
    dump = decidb_cli.with_env({"DECIDB_NATIVE_CONSTRUCTS": "off"}).dump_model(case.sql, tmp_path / "f1.dump")
    assert dump_counts(dump) == counts
    # A MAX pushed down needs only its envelope: no indicator, no Big-M (D79).
    assert "bin=1" not in dump, dump


# ---------------------------------------------------------------------------
# The grid: depth 2 and depth 3, every level's kind and scope
# ---------------------------------------------------------------------------

#: Two groups of g, two of h crossing them, two of k: every (g, h, k) group a row the
#: condition keeps (rows 3 and 8 it drops), a per-row bound v and a per-g cap.
COLUMNS = ("id", "g", "h", "k", "c", "v", "cap")
ROWS = (
    (1, "a", "p", "m", True, 6, 12),
    (2, "a", "p", "n", True, 4, 12),
    (3, "a", "q", "m", False, 5, 12),
    (4, "a", "q", "m", True, 3, 12),
    (5, "b", "p", "m", True, 7, 9),
    (6, "b", "p", "n", True, 2, 9),
    (7, "b", "q", "n", True, 6, 9),
    (8, "b", "q", "n", False, 4, 9),
)
C = lambda r: r["c"]  # noqa: E731
KINDS = ("sum", "avg", "min", "max")
#: A level's own scope: SQL prefix of its argument -> (when, per), `{key}` its `per` key.
SCOPES = {
    "": (None, PER_ROW),
    "when t.c: ": (C, PER_ROW),
    "per t.{key}: ": (None, "{key}"),
    "when t.c per t.{key}: ": (C, "{key}"),
}
DECISIONS = {"x(INT)": Decision("x"), "per t.h: x(INT)": Decision("x", ("h",))}


def _level(kind, scope, key, operand, by):
    """One reducer of a nest, as SQL and as the oracle states it."""
    when, per = SCOPES[scope]
    sql_scope = scope.format(key=key)
    sql = f"{kind}({sql_scope}{operand[0]}) by ({', '.join('t.' + b for b in by)})"
    return sql, reduce(kind, operand[1], when=when, per=(key,) if per == "{key}" else per, by=by)


def _depth2(outer, outer_scope, inner, inner_scope):
    nest = _level(inner, inner_scope, "k", ("x", X), ("g", "h"))
    return _level(outer, outer_scope, "h", nest, ("g",))


def _depth3(outer, outer_scope, middle, middle_scope, inner, inner_scope):
    nest = _level(inner, inner_scope, "id", ("x", X), ("g", "h", "k"))
    nest = _level(middle, middle_scope, "k", nest, ("g", "h"))
    return _level(outer, outer_scope, "h", nest, ("g",))


def _cells():
    # Depth 2: every kind and scope at both levels, over either decision.
    for declaration, outer, outer_scope, inner, inner_scope in itertools.product(
        DECISIONS, KINDS, SCOPES, KINDS, SCOPES
    ):
        label = f"2|{declaration}|{outer}:{outer_scope}|{inner}:{inner_scope}".replace(" ", "_")
        yield pytest.param(declaration, _depth2(outer, outer_scope, inner, inner_scope), id=label)
    # Depth 3: a sample that still crosses every middle kind and scope.
    for outer, outer_scope, middle, middle_scope, inner, inner_scope in itertools.product(
        ("sum", "max"), ("", "per t.{key}: "), KINDS, SCOPES, ("sum", "max"), ("", "when t.c: ")
    ):
        label = f"3|{outer}:{outer_scope}|{middle}:{middle_scope}|{inner}:{inner_scope}".replace(" ", "_")
        yield pytest.param("x(INT)", _depth3(outer, outer_scope, middle, middle_scope, inner, inner_scope), id=label)


@pytest.mark.correctness
@pytest.mark.parametrize("declaration, nest", list(_cells()))
def test_grid_cell(decidb_cli, scope_solver, declaration, nest):
    """`per t.g: <nest> <= t.cap`: the oracle's optimum, or the error the data raises, with
    the same phrase -- never a refusal, never a wrong answer, never an internal error."""
    nest_sql, nest_oracle = nest
    sql = f"""
        SELECT id, x {table('t', COLUMNS, ROWS)}
        DECIDE {declaration}
        SUCH THAT x <= t.v AND per t.g: {nest_sql} <= t.cap
        MAXIMIZE SUM(x)
        ORDER BY id"""
    clauses = [clause(le(X, Col("v"))), clause(le(nest_oracle, Col("cap")), per=("g",))]
    model = AnrModel(table_rows(COLUMNS, ROWS), [DECISIONS[declaration]])
    try:
        expected = model.solve(scope_solver, clauses, MAXIMIZE_X)
    except ScopeError as error:
        expected = error
    result = decidb_cli.execute_raw(sql)
    assert_no_internal_leak(result, f"grid cell {nest_sql}")
    if isinstance(expected, ScopeError):
        assert expected.phrase in result.stderr, f"the language refuses this data ({expected}); got:\n{result.stderr}"
        return
    assert result.returncode == 0 and not result.stderr.strip(), result.stderr
    assert model.judge(answer(decidb_cli, sql), clauses, MAXIMIZE_X) == pytest.approx(expected.objective, abs=1e-4)


def _data_cells():
    """A reducer over data alone inside one over decisions, every kind and scope at both
    levels, and `count`, which takes no scope of its own. Its own `by (t.h)` is under the
    outer `by (t.g)`, so its path key is `(t.g, t.h)`, and the outer `when` keeps the rows
    it reads."""
    for outer, outer_scope, inner, inner_scope in itertools.product(KINDS, SCOPES, KINDS + ("count",), SCOPES):
        if inner == "count" and inner_scope:
            continue
        data = _level(inner, inner_scope, "k", ("t.v", Col("v")), ("h",))
        nest = _level(outer, outer_scope, "h", (f"x - {data[0]}", X - data[1]), ("g",))
        yield pytest.param(nest, id=f"{outer}:{outer_scope}|{inner}:{inner_scope}".replace(" ", "_"))


@pytest.mark.correctness
@pytest.mark.parametrize("nest", list(_data_cells()))
def test_a_data_reducer_inside_a_nest(decidb_cli, scope_solver, nest):
    """A reducer over data alone inside one over decisions is a value of its path's group,
    computed as a bound's is (language/guide.md §5.5): `per t.g: <nest> <= 0`, judged by
    the oracle."""
    nest_sql, nest_oracle = nest
    sql = f"""
        SELECT id, x {table('t', COLUMNS, ROWS)}
        DECIDE x(INT)
        SUCH THAT x <= t.v AND per t.g: {nest_sql} <= 0
        MAXIMIZE SUM(x)
        ORDER BY id"""
    clauses = [clause(le(X, Col("v"))), clause(le(nest_oracle, 0), per=("g",))]
    model = AnrModel(table_rows(COLUMNS, ROWS), [Decision("x")])
    expected = model.solve(scope_solver, clauses, MAXIMIZE_X)
    result = decidb_cli.execute_raw(sql)
    assert_no_internal_leak(result, f"data nest {nest_sql}")
    assert result.returncode == 0 and not result.stderr.strip(), result.stderr
    assert model.judge(answer(decidb_cli, sql), clauses, MAXIMIZE_X) == pytest.approx(expected.objective, abs=1e-4)


#: A NULL `h` puts row 1 in no group of it (D5); `q` keeps no row a `when t.v > 4` reads.
NULL_COLUMNS = ("id", "g", "h", "v")
NULL_ROWS = ((1, "a", None, 3), (2, "a", "q", 4), (3, "b", "p", 5), (4, "b", "q", None))


@BOTH_SOLVERS
@pytest.mark.correctness
@pytest.mark.parametrize(
    "clause_sql, body",
    [
        pytest.param(
            "sum(x - avg(t.v) by (t.h)) <= 10",
            le(reduce("sum", X - reduce("avg", Col("v"), by=("h",))), 10),
            id="no-group",
        ),
        pytest.param(
            "sum(x - max(when t.v > 4: t.v) by (t.h)) <= 10",
            le(
                reduce("sum", X - reduce("max", Col("v"), when=lambda r: r["v"] is not None and r["v"] > 4, by=("h",))),
                10,
            ),
            id="no-row-kept",
        ),
        pytest.param(
            "per t.g: sum(x + max(t.v) by (t.h)) by (t.g) <= 12",
            le(reduce("sum", X + reduce("max", Col("v"), by=("h",)), by=("g",)), 12),
            id="per-path-group",
        ),
    ],
)
def test_a_data_reducer_with_no_value_drops_its_member(request, scope_solver, cli_fixture, clause_sql, body):
    """A reducer over data alone has no value where its path group has none -- a NULL key
    puts the row in no group, a `when` may keep no row of one -- and the reducer around it
    skips that member, as it skips any NULL it reads (deck p. 50)."""
    cli = request.getfixturevalue(cli_fixture)
    sql = f"""
        SELECT id, x {table('t', NULL_COLUMNS, NULL_ROWS)}
        DECIDE x(INT)
        SUCH THAT x <= 9 AND {clause_sql}
        MAXIMIZE SUM(x)
        ORDER BY id"""
    per = ("g",) if clause_sql.startswith("per") else PER_ROW
    clauses = [clause(le(X, 9)), clause(body, per=per)]
    _judged(cli, scope_solver, sql, table_rows(NULL_COLUMNS, NULL_ROWS), [Decision("x")], clauses)


#: Row 3, which the condition drops, moves its group's mean from 3 to 12.
MEAN_COLUMNS = ("id", "g", "c", "v")
MEAN_ROWS = ((1, "a", True, 2), (2, "a", True, 4), (3, "a", False, 30), (4, "b", True, 5))


@BOTH_SOLVERS
@pytest.mark.correctness
@pytest.mark.parametrize(
    "objective_sql, objective",
    [
        pytest.param(
            "MINIMIZE sum(abs(x - avg(t.v) by (t.g)))",
            Objective(ObjSense.MINIMIZE, reduce("sum", Abs(X - reduce("avg", Col("v"), by=("g",))))),
            id="deviation-from-the-group-mean",
        ),
        pytest.param(
            "MINIMIZE when t.c: sum(abs(x - avg(t.v) by (t.g)))",
            Objective(ObjSense.MINIMIZE, reduce("sum", Abs(X - reduce("avg", Col("v"), by=("g",))), when=C)),
            id="under-the-objectives-when",
        ),
    ],
)
def test_a_data_reducer_inside_the_objective(request, scope_solver, cli_fixture, objective_sql, objective):
    """In the objective too, a reducer over data alone inside one over decisions is a value
    of its path's group, the objective's `when` among what its path keeps: under `when t.c`
    group a's mean is 3, not 12."""
    cli = request.getfixturevalue(cli_fixture)
    sql = f"""
        SELECT id, x {table('t', MEAN_COLUMNS, MEAN_ROWS)}
        DECIDE x(INT)
        SUCH THAT x <= 9
        {objective_sql}
        ORDER BY id"""
    _judged(cli, scope_solver, sql, table_rows(MEAN_COLUMNS, MEAN_ROWS), [Decision("x")], [clause(le(X, 9))], objective)


@pytest.mark.error
def test_a_data_reducer_read_as_a_coefficient_must_be_finite(decidb_cli):
    """Inside a reducer over decisions a data reducer's value is a coefficient, so what it
    reads must be finite, and the message names what it reads; on a bound, infinity is a
    value (no bound)."""
    rows = ((1, "a", 3), (2, "b", 4))
    query = f"SELECT id, x {table('t', ('id', 'g', 'v'), rows)} DECIDE x(INT) SUCH THAT x <= 9 AND {{}} MAXIMIZE sum(x)"
    result = decidb_cli.execute_raw(query.format("sum(x + max(t.v / 0.0) by (t.g)) <= 30"))
    assert (
        "DECIDE: t.v / 0.0 is NaN or infinite on some rows. Skip those rows with 'when isfinite(t.v / 0.0):'"
        in result.stderr
    ), result.stderr
    assert_no_internal_leak(result, "an infinite data reducer")
    bound = decidb_cli.execute_raw(query.format("sum(x) <= max(t.v / 0.0) by (t.g)"))
    assert bound.returncode == 0 and not bound.stderr.strip(), bound.stderr


#: A comparison by its SQL sense.
SENSES = {"<=": le, ">=": ge, "=": eq}


@pytest.mark.correctness
@pytest.mark.parametrize("sense", list(SENSES))
@pytest.mark.parametrize("outer, inner", list(itertools.product(KINDS, KINDS)))
def test_a_constant_bound_on_a_nest(decidb_cli, scope_solver, sense, outer, inner):
    """`per t.g: outer(inner(x) by (t.g, t.h)) by (t.g) <sense> 4`: rewritten, the clause
    is a bound on the outer helper alone, which bound absorption folds into its box, and
    the box must keep it -- a helper's default floor is freed, a written one is not. The
    objective pushes against the bound (MINIMIZE under `>=` and `=`), so a lost bound shows
    as a wrong answer, and an `=` the data cannot meet as the infeasibility it is."""
    nest_sql = f"{outer}({inner}(x) by (t.g, t.h)) by (t.g)"
    nest = reduce(outer, reduce(inner, X, by=("g", "h")), by=("g",))
    goal = Objective(ObjSense.MAXIMIZE if sense == "<=" else ObjSense.MINIMIZE, reduce("sum", X))
    sql = f"""
        SELECT id, x {table('t', COLUMNS, ROWS)}
        DECIDE x(INT)
        SUCH THAT x <= t.v AND per t.g: {nest_sql} {sense} 4
        {'MAXIMIZE' if sense == '<=' else 'MINIMIZE'} SUM(x)
        ORDER BY id"""
    clauses = [clause(le(X, Col("v"))), clause(SENSES[sense](nest, 4), per=("g",))]
    model = AnrModel(table_rows(COLUMNS, ROWS), [Decision("x")])
    solved = model.solve(scope_solver, clauses, goal)
    result = decidb_cli.execute_raw(sql)
    assert_no_internal_leak(result, f"a constant bound on {nest_sql}")
    if solved.status != SolverStatus.OPTIMAL:
        assert "is infeasible" in result.stderr, (solved.status, result.stderr)
        return
    assert result.returncode == 0 and not result.stderr.strip(), result.stderr
    assert model.judge(answer(decidb_cli, sql), clauses, goal) == pytest.approx(solved.objective, abs=1e-4)


# ---------------------------------------------------------------------------
# Members, and what a level reads
# ---------------------------------------------------------------------------


@pytest.mark.correctness
@pytest.mark.parametrize(
    "cli_fixture, native",
    [
        pytest.param("decidb_cli_gurobi", "on", id="gurobi"),
        pytest.param("decidb_cli_highs", "on", id="highs"),
        pytest.param("decidb_cli_highs", "off", id="highs-lowered"),
    ],
)
@pytest.mark.parametrize(
    "nest_sql, sense, bound",
    [
        pytest.param("sum(max(x) by (t.g, t.h)) by (t.g)", ">=", 50, id="sum-max-above-reach"),
        pytest.param("sum(max(x) by (t.g, t.h)) by (t.g)", "=", 50, id="sum-max-equal-above-reach"),
        pytest.param("avg(min(x) by (t.g, t.h)) by (t.g)", ">=", 50, id="avg-min-above-reach"),
        pytest.param("max(sum(x) by (t.g, t.h)) by (t.g)", ">=", 50, id="max-sum-above-reach"),
        pytest.param("min(avg(x) by (t.g, t.h)) by (t.g)", "=", 50, id="min-avg-equal-above-reach"),
    ],
)
def test_a_nest_no_value_can_meet_is_infeasible(request, cli_fixture, native, nest_sql, sense, bound):
    """A bound past a nest's reach makes the boxes its helpers derive from each other
    cross, and each pass pushed their ends further apart: the query is infeasible, as for
    any empty box -- once an internal error -- and DIAGNOSE still names the clause."""
    cli = request.getfixturevalue(cli_fixture).with_env({"DECIDB_NATIVE_CONSTRUCTS": native})
    sql = f"""
        SELECT id, x {table('t', COLUMNS, ROWS)}
        DECIDE x(INT)
        SUCH THAT x <= t.v AND per t.g: {nest_sql} {sense} {bound}
        MAXIMIZE SUM(x)"""
    result = cli.execute_raw(sql)
    assert_no_internal_leak(result, nest_sql)
    assert "DECIDE optimization is infeasible" in result.stderr, result.stderr
    diagnosis = cli.execute_raw("DIAGNOSE " + sql)
    assert_no_internal_leak(diagnosis, "DIAGNOSE " + nest_sql)
    assert diagnosis.returncode == 0 and "infeasible" in diagnosis.stdout, diagnosis.stderr


@BOTH_SOLVERS
@pytest.mark.correctness
@pytest.mark.parametrize(
    "outer, inner, sense, bound",
    [
        pytest.param("sum", "sum", "<=", -4, id="sum-sum"),
        pytest.param("max", "sum", "<=", -4, id="max-sum"),
        pytest.param("sum", "min", "<=", -4, id="sum-min"),
        pytest.param("avg", "min", "=", -5, id="avg-min-equal"),
    ],
)
def test_a_negative_bound_on_a_nest_over_negative_terms(request, scope_solver, cli_fixture, outer, inner, sense, bound):
    """A helper holds a reducer's value, negative where its terms are: `sum(sum(t.w * x)
    ...) <= -4` over a negative `w` is a bound to meet, not one that contradicts a
    non-negative decision (the static check an ABS auxiliary takes once refused it as
    infeasible)."""
    cli = request.getfixturevalue(cli_fixture)
    columns = ("id", "g", "h", "w", "v")
    rows = ((1, "a", "p", -2, 5), (2, "a", "q", -2, 5), (3, "b", "p", -1, 6), (4, "b", "q", -3, 6))
    sql = f"""
        SELECT id, x {table('t', columns, rows)}
        DECIDE x(INT)
        SUCH THAT x <= t.v AND per t.g: {outer}({inner}(t.w * x) by (t.g, t.h)) by (t.g) {sense} {bound}
        MINIMIZE SUM(x)
        ORDER BY id"""
    nest = reduce(outer, reduce(inner, Col("w") * X, by=("g", "h")), by=("g",))
    clauses = [clause(le(X, Col("v"))), clause(SENSES[sense](nest, bound), per=("g",))]
    goal = Objective(ObjSense.MINIMIZE, reduce("sum", X))
    model = AnrModel(table_rows(columns, rows), [Decision("x")])
    expected = model.solve(scope_solver, clauses, goal)
    assert expected.status == SolverStatus.OPTIMAL, expected.status
    result = cli.execute_raw(sql)
    assert_no_internal_leak(result, "a negative bound on a nest")
    assert result.returncode == 0 and not result.stderr.strip(), result.stderr
    assert model.judge(answer(cli, sql), clauses, goal) == pytest.approx(expected.objective, abs=1e-6)


@BOTH_SOLVERS
@pytest.mark.correctness
def test_a_max_objective_over_group_averages(request, scope_solver, cli_fixture):
    """`MAXIMIZE max(avg(t.w * x) by (t.g))`: a helper per group average and one over them,
    whose Big-M closing rows (1/3 coefficients) left HiGHS's optimum 1e-6 off a row: HiGHS
    then called it a solve error, which leaked as an internal one. It is solved again
    with integrality held tighter, and answers on both backends."""
    cli = request.getfixturevalue(cli_fixture)
    columns = ("id", "g", "v", "w")
    rows = ((1, "b", 3, 1), (2, "a", 1, 1), (3, "b", 8, 4), (4, "a", 6, 4), (5, "a", 3, -2), (6, "b", 3, 0))
    sql = f"""
        SELECT id, x {table('t', columns, rows)}
        DECIDE x(REAL)
        SUCH THAT x <= t.v
        MAXIMIZE max(avg(t.w * x) by (t.g))
        ORDER BY id"""
    goal = Objective(ObjSense.MAXIMIZE, reduce("max", reduce("avg", Col("w") * X, by=("g",))))
    clauses = [clause(le(X, Col("v")))]
    model = AnrModel(table_rows(columns, rows), [Decision("x", var_type=VarType.CONTINUOUS)])
    result = cli.execute_raw(sql)
    assert_no_internal_leak(result, "a MAX objective over group averages")
    assert result.returncode == 0 and not result.stderr.strip(), result.stderr
    # group b: (3 + 4 * 8 + 0) / 3, above group a's (1 + 4 * 6 - 2 * 0) / 3
    assert model.judge(answer(cli, sql), clauses, goal) == pytest.approx(35 / 3, abs=1e-6)


@pytest.mark.correctness
def test_an_inner_avg_divides_by_its_members(decidb_cli, scope_solver):
    """`avg(per t.h: sum(x) by (t.g, t.h)) by (t.g)`: group 'a' holds two h classes, so the
    average is half the two sums, whatever the rows; 'b' one class, its sum."""
    rows = ((1, "a", "p", 10), (2, "a", "p", 10), (3, "a", "q", 10), (4, "b", "p", 10))
    columns = ("id", "g", "h", "v")
    sql = f"""
        SELECT id, x {table('t', columns, rows)}
        DECIDE x(INT)
        SUCH THAT x <= t.v AND per t.g: avg(per t.h: sum(x) by (t.g, t.h)) by (t.g) <= 6
        MAXIMIZE SUM(x)
        ORDER BY id"""
    nest = reduce("avg", reduce("sum", X, by=("g", "h")), per=("h",), by=("g",))
    clauses = [clause(le(X, Col("v"))), clause(le(nest, 6), per=("g",))]
    assert _judged(decidb_cli, scope_solver, sql, table_rows(columns, rows), [Decision("x")], clauses) == 12 + 6


@pytest.mark.correctness
def test_an_inner_count_counts_members(decidb_cli, scope_solver):
    """`sum(per t.g: count(*) by (t.g))` on the bound side: one member per class of g, each
    its group's row count -- the three rows, once."""
    rows = ((1, "a", 9), (2, "a", 9), (3, "b", 9))
    columns = ("id", "g", "v")
    sql = f"""
        SELECT id, x {table('t', columns, rows)}
        DECIDE x(INT)
        SUCH THAT x <= sum(per t.g: count(*) by (t.g))
        MAXIMIZE SUM(x)
        ORDER BY id"""
    clauses = [clause(le(X, reduce("sum", reduce("count", Col("v"), by=("g",)), per=("g",))))]
    assert _judged(decidb_cli, scope_solver, sql, table_rows(columns, rows), [Decision("x")], clauses) == 3 * 3


@pytest.mark.correctness
def test_an_inner_by_reads_the_outer_group_only(decidb_cli, scope_solver):
    """`sum(max(x) by (t.g)) by (t.h)`: the inner maximum is per (h, g) pair -- the path
    key -- not per g across the h groups (the path rule). Row 1 and row 3 share h = 'p' but not g, so
    each reads its own row, and x1 + x3 <= 5 is all the clause says of them."""
    rows = ((1, "a", "p", 3), (2, "a", "q", 4), (3, "b", "p", 5))
    columns = ("id", "g", "h", "v")
    sql = f"""
        SELECT id, x {table('t', columns, rows)}
        DECIDE x(INT)
        SUCH THAT x <= t.v AND sum(max(x) by (t.g)) by (t.h) <= 5
        MAXIMIZE SUM(x)
        ORDER BY id"""
    clauses = [clause(le(X, Col("v"))), clause(le(reduce("sum", reduce("max", X, by=("g",)), by=("h",)), 5))]
    assert _judged(decidb_cli, scope_solver, sql, table_rows(columns, rows), [Decision("x")], clauses) == 4 + 5


# ---------------------------------------------------------------------------
# Bounds through a nest (§4.6)
# ---------------------------------------------------------------------------

BOUND_COLUMNS = ("id", "g", "h")
BOUND_ROWS = ((1, "a", "p"), (2, "a", "p"), (3, "a", "q"), (4, "b", "p"))


def _bound_query(clause_sql):
    return f"""
        SELECT id, x {table('t', BOUND_COLUMNS, BOUND_ROWS)}
        DECIDE x(INT)
        SUCH THAT {clause_sql}
        MAXIMIZE SUM(x)
        ORDER BY id"""


@pytest.mark.correctness
@pytest.mark.min_max
@pytest.mark.parametrize(
    "nest_sql, nest, bound, optimum",
    [
        # Each row reads its (g, h) maximum: 2 * max(x1, x2) + x3 = 10 for 'a', x4 = 10 for 'b'.
        pytest.param(
            "sum(max(x) by (t.g, t.h)) by (t.g)",
            reduce("sum", reduce("max", X, by=("g", "h")), by=("g",)),
            10,
            20,
            id="sum-of-maxima",
        ),
        pytest.param(
            "sum(per t.h: max(x) by (t.g, t.h)) by (t.g)",
            reduce("sum", reduce("max", X, by=("g", "h")), per=("h",), by=("g",)),
            10,
            30,
            id="sum-per-class",
        ),
        pytest.param(
            "max(max(x) by (t.g, t.h)) by (t.g)",
            reduce("max", reduce("max", X, by=("g", "h")), by=("g",)),
            7,
            28,
            id="maximum-of-maxima",
        ),
    ],
)
def test_a_root_bound_reaches_a_nested_closing_link(decidb_cli_highs, scope_solver, nest_sql, nest, bound, optimum):
    """An `=` pins every MAX of the nest from both sides, and nothing bounds `x` but the
    root: the inner helper's Big-M is read off the outer one's box, which the row bounds
    (DeriveReducerMemberBounds). Lowered, on HiGHS, it answers."""
    cli = decidb_cli_highs.with_env({"DECIDB_NATIVE_CONSTRUCTS": "force"})
    clause_sql = f"per t.g: {nest_sql} = {bound}"
    clauses = [clause(eq(nest, bound), per=("g",))]
    model = AnrModel(table_rows(BOUND_COLUMNS, BOUND_ROWS), [Decision("x", ub=50)])
    expected = model.solve(scope_solver, clauses, MAXIMIZE_X)
    assert expected.objective == pytest.approx(optimum)
    assert model.judge(answer(cli, _bound_query(clause_sql)), clauses, MAXIMIZE_X) == pytest.approx(optimum)


@pytest.mark.error
@pytest.mark.min_max
def test_a_minimum_with_no_bound_above_is_refused_by_name(decidb_cli_highs):
    """`min(sum(x) by (t.g, t.h)) by (t.g) = 10` caps the smallest group sum only: the
    others reach any value, so the MIN's closing side has no Big-M, and the lowering names
    the inner reducer to bound, as written."""
    cli = decidb_cli_highs.with_env({"DECIDB_NATIVE_CONSTRUCTS": "force"})
    result = cli.execute_raw(_bound_query("per t.g: min(sum(x) by (t.g, t.h)) by (t.g) = 10"))
    assert (
        "requires a finite bound on 'sum(x) by (t.g, t.h)'. Add constraints 'sum(x) by (t.g, t.h) >= <lower>' "
        "and 'sum(x) by (t.g, t.h) <= <upper>'"
    ) in result.stderr, result.stderr
    assert_no_internal_leak(result, "an underivable nested closing link")


@BOTH_SOLVERS
@pytest.mark.correctness
@pytest.mark.min_max
def test_a_bound_through_two_helper_levels(request, scope_solver, cli_fixture):
    """`max(max(x) by (t.g, t.h)) by (t.g) <= 10`: both MAX levels pushed down take their
    envelopes alone, which bound every x by 10 through two helpers."""
    cli = request.getfixturevalue(cli_fixture)
    clauses = [clause(le(reduce("max", reduce("max", X, by=("g", "h")), by=("g",)), 10), per=("g",))]
    sql = _bound_query("per t.g: max(max(x) by (t.g, t.h)) by (t.g) <= 10")
    assert _judged(cli, scope_solver, sql, table_rows(BOUND_COLUMNS, BOUND_ROWS), [Decision("x", ub=50)], clauses) == 40


@pytest.mark.correctness
def test_abs_around_a_reducer_on_a_trivial_path_answers(decidb_cli, scope_solver):
    """`sum(abs(x - avg(x)))`: no enclosing `when`, `per` or `by` chooses the inner AVG's
    rows, so the ABS rewrite reads them as the path rule does, and it answers."""
    columns = ("id", "v")
    rows = ((1, 3), (2, 5), (3, 4))
    sql = f"""
        SELECT id, x {table('t', columns, rows)}
        DECIDE x(INT)
        SUCH THAT x <= t.v AND sum(abs(x - avg(x))) <= 4
        MAXIMIZE SUM(x)
        ORDER BY id"""
    from ._anr_oracle import Abs

    clauses = [clause(le(X, Col("v"))), clause(le(reduce("sum", Abs(X - reduce("avg", X))), 4))]
    assert _judged(decidb_cli, scope_solver, sql, table_rows(columns, rows), [Decision("x")], clauses) == 12


@BOTH_SOLVERS
@pytest.mark.correctness
def test_a_weighted_sum_of_one_helper_settles(request, scope_solver, cli_fixture):
    """`sum(t.w * max(x)) = 9` over weights 2 and 1 reads one helper twice: the backward pass
    sums its occurrences (3m = 9) rather than bounding each by the other's ends, which
    closed in on m = 3 by halves and never settled (an internal error)."""
    cli = request.getfixturevalue(cli_fixture)
    columns = ("id", "w", "v")
    rows = ((1, 2, 9), (2, 1, 9))
    sql = f"""
        SELECT id, x {table('t', columns, rows)}
        DECIDE x(INT)
        SUCH THAT x <= t.v AND sum(t.w * max(x)) = 9
        MAXIMIZE SUM(x)
        ORDER BY id"""
    clauses = [clause(le(X, Col("v"))), clause(eq(reduce("sum", Col("w") * reduce("max", X)), 9))]
    assert _judged(cli, scope_solver, sql, table_rows(columns, rows), [Decision("x")], clauses) == 6


@BOTH_SOLVERS
@pytest.mark.correctness
@pytest.mark.parametrize(
    "objective_sql, objective, optimum",
    [
        pytest.param(
            "sum(per t.g: avg(x) by (t.g)) - avg(x)",
            reduce("sum", reduce("avg", X, by=("g",)), per=("g",)) - reduce("avg", X),
            10 - 10 / 3,
            id="minus-a-whole-average",
        ),
        pytest.param(
            "avg(x) + sum(per t.g: avg(x) by (t.g))",
            reduce("avg", X) + reduce("sum", reduce("avg", X, by=("g",)), per=("g",)),
            10 / 3 + 10,
            id="plus-a-whole-average",
        ),
    ],
)
def test_an_inlined_group_average_stays_apart_from_another(
    request, scope_solver, cli_fixture, objective_sql, objective, optimum
):
    """The inlined `sum(per K: avg(e) by (G))` divides by each group's size, another AVG of
    the same decision by the relation's: two terms, not one like term (they were summed
    into one with the first's divisor, and answered x = 0, or one solver's guess)."""
    cli = request.getfixturevalue(cli_fixture)
    columns = ("id", "g", "v")
    rows = ((1, "a", 10), (2, "a", 10), (3, "b", 10))
    sql = f"""
        SELECT id, x {table('t', columns, rows)}
        DECIDE x(INT)
        SUCH THAT x <= t.v AND sum(x) <= 10
        MAXIMIZE {objective_sql}
        ORDER BY id"""
    goal = Objective(ObjSense.MAXIMIZE, objective)
    clauses = [clause(le(X, Col("v"))), clause(le(reduce("sum", X), 10))]
    model = AnrModel(table_rows(columns, rows), [Decision("x")])
    assert model.judge(answer(cli, sql), clauses, goal) == pytest.approx(optimum, abs=1e-6)


# ---------------------------------------------------------------------------
# What stays refused (D80)
# ---------------------------------------------------------------------------


@pytest.mark.error
@pytest.mark.error_binder
@pytest.mark.parametrize(
    "clause_sql, message",
    [
        pytest.param("sum(norm(x, 1)) <= 5", "DECIDE: norm inside a reducer is not supported yet", id="norm"),
        # Each is rewritten before the value rewrite reads the path rule, so the inner
        # reducer would lose the enclosing reducer's `when` and `by`: once answered wrongly.
        pytest.param(
            "per t.g: sum(abs(max(x) by (t.h))) by (t.g) <= 5",
            "DECIDE: abs over a reducer inside another reducer is not supported yet",
            id="abs-over-a-nested-reducer",
        ),
        pytest.param(
            "per t.g: norm(max(x) by (t.h), 1) by (t.g) <= 5",
            "DECIDE: a reducer inside norm is not supported yet",
            id="reducer-in-norm",
        ),
        pytest.param(
            "norm(max(x) by (t.g), 0) <= 1",
            "DECIDE: a reducer inside norm is not supported yet",
            id="reducer-in-l0-norm",
        ),
        pytest.param(
            "sum((x - 1) * max(x) by (t.g)) <= 50",
            "multiplies a reducer by a decision; only data can scale a reducer",
            id="decision-times-a-nested-reducer",
        ),
    ],
)
def test_refused_nests(decidb_cli, clause_sql, message):
    result = decidb_cli.execute_raw(_bound_query(f"x <= 9 AND {clause_sql}"))
    assert message in result.stderr, result.stderr
    assert_no_internal_leak(result, "a refused nest")


@pytest.mark.correctness
@pytest.mark.quadratic
def test_a_square_inside_a_nest_is_defined_where_a_max_reads_it(decidb_cli_gurobi, decidb_cli_highs):
    """`sum(max(x * x) by (t.g)) <= 5`: each row reads its group's largest square, so
    3 * max_a + max_b <= 5 holds every x at 1. The square is a helper at least its value per
    row (DefineNonlinearReducerBodies), which a MAX reads natively: Gurobi answers, and
    HiGHS, which states no MIN/MAX, is refused by name."""
    sql = _bound_query("x <= 9 AND sum(max(x * x) by (t.g)) <= 5")
    assert [row["x"] for row in answer(decidb_cli_gurobi, sql)] == [1, 1, 1, 1]
    result = decidb_cli_highs.execute_raw(sql)
    assert "'max(x * x) by (t.g)' takes the maximum of a squared or multiplied expression" in result.stderr
    assert_no_internal_leak(result, "a square under a MAX on HiGHS")


@pytest.mark.error
@pytest.mark.quadratic
def test_a_square_a_helper_would_define_is_refused(decidb_cli):
    """In the objective, a nest of sums that is not a sum of group sums reads its inner sum
    through a helper, whose definition would be a quadratic row (D80)."""
    sql = f"""
        SELECT id, x {table('t', BOUND_COLUMNS, BOUND_ROWS)}
        DECIDE x(INT)
        SUCH THAT x <= 9 AND x >= 1
        MINIMIZE sum(sum(x * x) by (t.g))
        ORDER BY id"""
    result = decidb_cli.execute_raw(sql)
    message = "'sum(x * x) by (t.g)' squares or multiplies decisions inside nested reducers"
    assert message in result.stderr, result.stderr
    assert_no_internal_leak(result, "a square a helper would define")


# ---------------------------------------------------------------------------
# Defects the review of the port found, each answer worked by hand from the deck
# ---------------------------------------------------------------------------


def _review_query(columns, rows, decide, clause_sql, objective="MAXIMIZE SUM(x)"):
    return f"""
        SELECT id, x {table('t', columns, rows)}
        DECIDE {decide}
        SUCH THAT x <= 10 AND {clause_sql}
        {objective}
        ORDER BY id"""


@BOTH_SOLVERS
@pytest.mark.correctness
@pytest.mark.parametrize(
    "columns, rows, decide, clause_sql, expected",
    [
        pytest.param(
            ("id", "g", "w"),
            ((1, "a", 1), (2, "a", None)),
            "x(INT)",
            "per (): sum(t.w * sum(x) by (t.g)) <= 5",
            [0, 5],
            id="null-data",
        ),
        pytest.param(
            ("id", "g", "h"),
            ((1, "a", "h1"), (2, "a", None)),
            "x(INT), per t.h: y(INT)",
            "y <= 10 AND per (): sum(y + max(x) by (t.g)) <= 5",
            [5, 5],
            id="absent-decision",
        ),
        pytest.param(
            ("id", "g", "w", "c"),
            ((1, "a", 1, True), (2, "a", None, True)),
            "x(INT)",
            "per (): sum(when t.c: t.w * sum(x) by (t.g)) <= 5",
            [0, 5],
            id="beside-a-when",
        ),
    ],
)
def test_an_outer_null_drops_the_outer_member_only(request, cli_fixture, columns, rows, decide, clause_sql, expected):
    """Row 2's NULL `w`, or its decision `y` whose key is NULL, drops row 2's member of the
    outer sum alone (D67): the reducer inside reads its group whole (deck p. 30), the rows
    the outer `when` keeps, so the clause is x1 + x2 <= 5 -- max(x1, x2) <= 5 -- not x1
    alone. The reducer's NULL tests had been carried into the path of every reducer inside
    it."""
    sql = _review_query(columns, rows, decide, clause_sql)
    result = answer(request.getfixturevalue(cli_fixture), sql)
    assert sum(row["x"] for row in result) == sum(expected), result
    if "max" in clause_sql:
        assert [row["x"] for row in result] == expected


@BOTH_SOLVERS
@pytest.mark.correctness
@pytest.mark.parametrize(
    "clause_sql, objective, expected",
    [
        pytest.param("per (): min(sum(per t.k: x) by (t.g)) = 5", "MAXIMIZE SUM(x)", [10, 5], id="min-of-groups"),
        pytest.param("per (): max(sum(per t.k: x) by (t.g)) >= 5", "MINIMIZE SUM(x)", [0, 5], id="max-of-groups"),
        pytest.param("per (): avg(sum(per t.k: x) by (t.g)) = 5", "MINIMIZE SUM(x)", [0, 5], id="avg-of-groups"),
        pytest.param("x + sum(per t.k: x) by (t.g) <= 5", "MAXIMIZE SUM(x)", [10, 2], id="one-level"),
    ],
)
def test_a_group_of_no_class_has_no_value(request, cli_fixture, clause_sql, objective, expected):
    """Row 1's NULL `k` puts it in no class of the inner `per t.k` (D5), so group a's sum has
    no member and no value (D68): the reducer around it skips it, and a row reading it
    skips its instance. Its helper had been left a group with no definition -- free -- or
    read where it was absent (an internal error)."""
    sql = _review_query(("id", "g", "k"), ((1, "a", None), (2, "b", "m")), "x(INT)", clause_sql, objective)
    assert [row["x"] for row in answer(request.getfixturevalue(cli_fixture), sql)] == expected


@BOTH_SOLVERS
@pytest.mark.correctness
@pytest.mark.parametrize(
    "columns, rows, objective",
    [
        pytest.param(
            ("id", "g", "w"),
            ((1, "a", 1), (2, None, 5), (3, "b", 2)),
            "sum(per t.g: sum(t.w * x) by (t.g))",
            id="null-per-key",
        ),
        pytest.param(
            ("id", "g", "h", "w"),
            ((1, "a", "u", 1), (2, "a", None, 5), (3, "b", "u", 2)),
            "sum(per t.g: sum(t.w * x) by (t.g, t.h))",
            id="null-by-key",
        ),
    ],
)
def test_a_sum_of_group_sums_skips_a_row_in_no_group(request, cli_fixture, columns, rows, objective):
    """Row 2 is in no group of the inner `by` (D5), so it is no member of the objective's sum
    of group sums: the best use of `SUM(x) <= 10` is row 3's weight 2. Read in place as one
    sum (D81), the objective had counted row 2's weight 5."""
    sql = _review_query(columns, rows, "x(INT)", "SUM(x) <= 10", f"MAXIMIZE {objective}")
    assert [row["x"] for row in answer(request.getfixturevalue(cli_fixture), sql)] == [0, 0, 10]


@BOTH_SOLVERS
@pytest.mark.correctness
def test_groups_holding_one_value_are_two_variants(request, cli_fixture):
    """`sum(per (): max(t.v) by (t.g))` over data: groups a and b are two variants -- a
    reducer's `by` key is a signature field (D70, deck p. 37) -- although their maxima are
    both 4, so the bound is 8. The bound side had grouped the variants by value."""
    sql = f"""
        SELECT id, x {table('t', ('id', 'g', 'v'), ((1, 'a', 4), (2, 'b', 4)))}
        DECIDE x(REAL)
        SUCH THAT x <= sum(per (): max(t.v) by (t.g))
        MAXIMIZE sum(x)
        ORDER BY id"""
    assert [row["x"] for row in answer(request.getfixturevalue(cli_fixture), sql)] == [8, 8]


@BOTH_SOLVERS
@pytest.mark.correctness
@pytest.mark.parametrize(
    "clause_sql",
    [
        pytest.param("per (): sum(max(when NOT t.c: x) by (t.g)) >= 5", id="nest"),
        pytest.param("x + max(max(when NOT t.c: x) by (t.g)) >= 5", id="nest-in-a-row"),
        pytest.param("x + sum(when NOT t.c: x) >= 5", id="one-level"),
    ],
)
def test_a_reducer_left_with_no_member_skips_its_instance(request, cli_fixture, clause_sql):
    """Every row has `c`, so the inner `when NOT t.c` keeps none: each group is empty, the
    reducer over them has no member and no value, and every instance reading it is skipped
    (D68, D87), so the clause constrains nothing, which stderr says. A query-wide helper had
    no way to be absent: the nest was called infeasible, and a row reading the helper raised
    an internal error."""
    sql = _review_query(("id", "g", "c"), ((1, "a", True), (2, "b", True)), "x(INT)", clause_sql, "MINIMIZE SUM(x)")
    warnings = [f"'{clause_sql}' constrains nothing"]
    assert [row["x"] for row in answer(request.getfixturevalue(cli_fixture), sql, warnings)] == [0, 0]


@BOTH_SOLVERS
@pytest.mark.correctness
def test_a_product_with_a_factor_of_no_value_has_none(request, cli_fixture):
    """Group b keeps no row for `sum(when t.c: x)`, so it has no value, and nor has the
    product `b * sum(...)` on row 2 (D88): row 2's comparison is skipped and x2 keeps its cap
    of 9. Row 1 reads x1 + b1 * x1 <= 5, best at b1 = 0, x1 = 5. The product had read the
    empty group as 0, holding x2 to 5."""
    sql = f"""
        SELECT id, x, b {table('t', ('id', 'g', 'c'), ((1, 'a', True), (2, 'b', False)))}
        DECIDE x(INT), b(BOOL)
        SUCH THAT x <= 9 AND x + b * sum(when t.c: x) by (t.g) <= 5
        MAXIMIZE sum(x) + sum(b)
        ORDER BY id"""
    assert [row["x"] for row in answer(request.getfixturevalue(cli_fixture), sql)] == [5, 9]


@pytest.mark.correctness
def test_an_objective_average_counts_the_members_with_a_value(decidb_cli):
    """Rows 3 and 4 read group b's empty `max(when t.c: x)`, so their `abs` has no value and
    the objective's average drops them (D67, D88): it is |x1 - x2| / 2, which pays 2.5 for
    x1 = 5 against 1.5 of the penalty. Over all four rows it would be 1.25, and every x 0."""
    sql = f"""
        SELECT id, x {table('t', ('id', 'g', 'c'), ((1, 'a', True), (2, 'a', True), (3, 'b', False), (4, 'b', False)))}
        DECIDE x(REAL)
        SUCH THAT x <= 5
        MAXIMIZE avg(abs(x - max(when t.c: x) by (t.g))) - 0.3 * sum(x)
        ORDER BY id"""
    assert sum(row["x"] for row in answer(decidb_cli, sql)) == pytest.approx(5)


@BOTH_SOLVERS
@pytest.mark.correctness
def test_a_bound_reads_each_column_as_a_field(request, cli_fixture):
    """`sum(per (): t.a + t.b)` over data reads the fields `t.a` and `t.b` (D83, deck pp. 81,
    110): (1, 2) and (2, 1) are two instances, 3 + 3, although their sums agree. The bound
    side had grouped the instances by the sum."""
    sql = f"""
        SELECT id, x {table('t', ('id', 'g', 'a', 'b'), ((1, 'a', 1, 2), (2, 'a', 2, 1)))}
        DECIDE x(REAL)
        SUCH THAT x <= sum(per (): t.a + t.b)
        MAXIMIZE sum(x)
        ORDER BY id"""
    assert [row["x"] for row in answer(request.getfixturevalue(cli_fixture), sql)] == [6, 6]


@pytest.mark.correctness
@pytest.mark.parametrize(
    "columns, rows, clause_sql, expected",
    [
        pytest.param(
            ("id", "depot", "customer", "dispatch", "transit", "demand"),
            ((1, "d1", "c1", 1, 2, 4), (2, "d1", "c1", 2, 1, 4)),
            "x <= sum(per t.customer: max(t.demand) by (t.customer)) by (t.depot, t.dispatch + t.transit)",
            [4, 4],
            id="deck-p24-arrival-day",
        ),
        pytest.param(
            ("id", "k", "g", "v"),
            ((1, "u", "a", 4), (2, "u", "A", 4)),
            "x <= sum(per t.k: max(t.v) by (t.k)) by (upper(t.g))",
            [4, 4],
            id="function-of-one-column",
        ),
        pytest.param(
            ("id", "k", "w", "v"),
            ((1, "u", 1, 5), (2, "u", 3, 5), (3, "v", 2, 7), (4, "w", 5, 9)),
            "per (): sum(x - sum(per t.k: max(t.v) by (t.k))) by (t.w % 2) <= 20",
            [0, 0, 27, 62],
            id="inside-a-decision-reducer",
        ),
    ],
)
def test_a_nested_reducer_is_one_field_its_key(decidb_cli, columns, rows, clause_sql, expected):
    """A reducer nested in a data reducer with a `per` is a signature field by its key alone
    (deck p. 81), however that key is computed: customer c1 is one class of the group (d1, day
    3), and k = u one class of the odd `t.w % 2` group (rows w = 1, 3, 5), so each counts once.
    The nested value's NULL test, `(t.dispatch + t.transit) IS NULL`, had added the columns it
    reads as fields, so a class counted once per column value: x <= 8 on the deck's p. 24
    shape, and 5 + 5 + 9 for the odd group instead of 5 + 9 (x1 + x2 + x4 <= 77, not 62)."""
    sql = f"""
        SELECT id, x {table('t', columns, rows)}
        DECIDE x(INT)
        SUCH THAT x <= 100 AND {clause_sql}
        MAXIMIZE sum(t.id * x)
        ORDER BY id"""
    assert [row["x"] for row in answer(decidb_cli, sql)] == expected
