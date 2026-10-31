"""Nested reducers in the engine (``syntax_reference.md`` §5.5; D73-D75).

A reducer nests in another to any depth, with `when`, `per` and `by` at every level. A
nested reducer reads, at the row it stands for, the rows its path keeps -- the clause's
`when` and every enclosing reducer's -- grouped by its path key: every enclosing `by` key
and its own. Every reducer of a nest is a helper decision keyed by that path, defined
once per group, and read by the reducers around it. Every phase-2 fixture of
``_anr_cases`` runs through DeciDB on both solvers and is judged by ``_anr_oracle``,
which reads no SQL; a grid of nests at depth 2 and 3 is judged cell by cell; F1's model
is pinned; and the refusals of D74 are named.
"""

import itertools

import pytest

from ._anr_cases import CASES, F1_MAX, F1_SUM
from ._anr_oracle import PER_ROW, AnrModel, Col, Decision, Objective, ScopeError, Var, clause, eq, ge, le, reduce
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
    reached = model.judge(answer(cli, sql), clauses, objective)
    assert reached == pytest.approx(expected.objective, abs=1e-6)
    return reached


# ---------------------------------------------------------------------------
# The phase-2 fixtures (§6.3), judged by the oracle, on both solvers
# ---------------------------------------------------------------------------


@BOTH_SOLVERS
@pytest.mark.correctness
@pytest.mark.parametrize("case", _named(c for c in CASES if c.phase == 2))
def test_the_engine_answers_each_fixture(request, scope_solver, cli_fixture, case):
    """F1 (both inner reducers), F14, F16, F16_small, count(e) over a NULL, and D8 inside
    a nest: the hand-computed optimum, or the error the data raises."""
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
@pytest.mark.parametrize("case, counts", [pytest.param(F1_SUM, {"num_vars": 6, "num_rows": 3}, id="F1_sum"),
                                          pytest.param(F1_MAX, {"num_vars": 10, "num_rows": 8}, id="F1_max")])
def test_the_running_example_is_a_helper_per_path_group(decidb_cli, tmp_path, case, counts):
    """F1 (deck pp. 111-119): six shipments; the inner reducer a helper per (rid, cid)
    group among the rows `when t.p` keeps -- three, and a spare column for the row it
    drops, which no row reads -- and the outer one a helper per region, read by three
    clause rows (d1 reads East and West, d2 West). Inlined where it adds no nonzero
    (phase 7), as the deck writes it: for F1_sum, each inner sum into its region's and
    each region's into its rows (West's three terms into two rows, p. 119), the six
    shipments and three rows alone. For F1_max, each region's sum into its rows (p. 115);
    the inner MAX keeps its four columns -- two of its groups have two members -- the
    lone c2 defined by an equality (p. 112), the others by an envelope row per member:
    ten columns, three rows and five."""
    dump = decidb_cli.with_env({"DECIDB_NATIVE_CONSTRUCTS": "off"}).dump_model(case.sql, tmp_path / "f1.dump")
    assert dump_counts(dump) == counts
    # A MAX pushed down needs only its envelope: no indicator, no Big-M (D73).
    assert "bin=1" not in dump, dump


@pytest.mark.error
def test_d8_names_the_path_group(decidb_cli):
    """The inner `when` keeps no row of the (g, h) = ('a', 'q') group: the D8 error names
    the reducer and its path group, both keys."""
    case = next(c for c in CASES if c.name == "nested_d8")
    result = decidb_cli.execute_raw(case.sql)
    assert "has no rows for t.g = 'a', t.h = 'q'" in result.stderr, result.stderr


@pytest.mark.error
def test_d8_quotes_the_inner_by_as_written(decidb_cli):
    """The D8 message leaves out the empty reducer's own `by` -- it names the group apart --
    but keeps every reducer nested in it as written, `by` included."""
    result = decidb_cli.execute_raw("""
        SELECT id, x FROM (VALUES (1, 'a', 'p', true), (2, 'a', 'q', false)) t(id, g, h, c)
        DECIDE x(INT)
        SUCH THAT x <= 3 AND per t.g: sum(when t.c: max(x) by (t.h)) by (t.g, t.h) <= 5
        MAXIMIZE sum(x)""")
    assert "DECIDE: sum(when c: max(x) by (t.h)) has no rows for t.g = 'a', t.h = 'q'." in result.stderr, result.stderr


# ---------------------------------------------------------------------------
# The grid: depth 2 and depth 3, every level's kind and scope (§6.5)
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


@pytest.mark.correctness
@pytest.mark.parametrize("cli_fixture, native", [
    pytest.param("decidb_cli_gurobi", "on", id="gurobi"),
    pytest.param("decidb_cli_highs", "on", id="highs"),
    pytest.param("decidb_cli_highs", "off", id="highs-lowered"),
])
@pytest.mark.parametrize("nest_sql, sense, bound", [
    pytest.param("sum(max(x) by (t.g, t.h)) by (t.g)", ">=", 50, id="sum-max-above-reach"),
    pytest.param("sum(max(x) by (t.g, t.h)) by (t.g)", "=", 50, id="sum-max-equal-above-reach"),
    pytest.param("avg(min(x) by (t.g, t.h)) by (t.g)", ">=", 50, id="avg-min-above-reach"),
    pytest.param("max(sum(x) by (t.g, t.h)) by (t.g)", ">=", 50, id="max-sum-above-reach"),
    pytest.param("min(avg(x) by (t.g, t.h)) by (t.g)", "=", 50, id="min-avg-equal-above-reach"),
])
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
@pytest.mark.parametrize("outer, inner, sense, bound", [
    pytest.param("sum", "sum", "<=", -4, id="sum-sum"),
    pytest.param("max", "sum", "<=", -4, id="max-sum"),
    pytest.param("sum", "min", "<=", -4, id="sum-min"),
    pytest.param("avg", "min", "=", -5, id="avg-min-equal"),
])
def test_a_negative_bound_on_a_nest_over_negative_terms(request, scope_solver, cli_fixture, outer, inner, sense,
                                                        bound):
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


# ---------------------------------------------------------------------------
# Members, and what a level reads
# ---------------------------------------------------------------------------


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
@pytest.mark.parametrize("objective_sql, objective, optimum", [
    pytest.param("sum(per t.g: avg(x) by (t.g)) - avg(x)",
                 reduce("sum", reduce("avg", X, by=("g",)), per=("g",)) - reduce("avg", X), 10 - 10 / 3,
                 id="minus-a-whole-average"),
    pytest.param("avg(x) + sum(per t.g: avg(x) by (t.g))",
                 reduce("avg", X) + reduce("sum", reduce("avg", X, by=("g",)), per=("g",)), 10 / 3 + 10,
                 id="plus-a-whole-average"),
])
def test_an_inlined_group_average_stays_apart_from_another(request, scope_solver, cli_fixture, objective_sql,
                                                           objective, optimum):
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
# What stays refused (D74)
# ---------------------------------------------------------------------------


@pytest.mark.error
@pytest.mark.error_binder
@pytest.mark.parametrize(
    "clause_sql, message",
    [
        pytest.param("sum(norm(x, 1)) <= 5", "DECIDE: norm inside a reducer is not supported yet", id="norm"),
        pytest.param(
            "sum(max(x * x) by (t.g)) <= 5",
            "DECIDE: 'max(x * x)' squares or multiplies decisions inside nested reducers, which is not "
            "supported yet",
            id="square",
        ),
        pytest.param(
            "sum(x + max(t.id) by (t.g)) <= 50",
            "DECIDE: max(...) over data alone inside a reducer over decisions is not supported yet",
            id="data",
        ),
        pytest.param(
            "sum(x + count(*) by (t.g)) <= 50",
            "DECIDE: count(...) over data alone inside a reducer over decisions is not supported yet",
            id="data-count",
        ),
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
