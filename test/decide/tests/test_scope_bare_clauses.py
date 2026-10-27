"""Bare clauses on the scope rules (D30).

A clause with no `when`, `per` or `by` means its fully explicit form: `per row:` in front,
`by ()` on each reducer, and an objective produced once (`per ()`) --
syntax_reference.md §1.1. Each test here states a query whose answer or refusal follows
from that reading, judged by the scope oracle (`_scope_oracle.ScopedModel`) or, for a
shape the oracle cannot state, by enumerating every assignment (`_brute_force`). A test
runs on both solvers; where one cannot load the model, its refusal is asserted instead.
"""

import pytest

from ._brute_force import judge_rows
from ._scope_cases import answer, scope_solver, table, table_rows  # noqa: F401 -- scope_solver is a fixture
from ._scope_oracle import CONSTANT, Decision, Reduce, Scoped, ScopedModel, ScopedObjective
from solver.types import ObjSense, VarType

COLUMNS = ("id", "cap", "g", "w")
ROWS = ((1, 3, "A", 2.0), (2, 5, "A", 4.0), (3, 7, "B", 1.0))
CAPS = [cap for _, cap, _, _ in ROWS]

BOTH_SOLVERS = pytest.mark.parametrize("cli_fixture", ["decidb_cli_gurobi", "decidb_cli_highs"])

X = lambda r: {"x": 1}  # noqa: E731
CAP_BOUND = Scoped((X,), "<=", (lambda r: {CONSTANT: r["cap"]},))
MAXIMIZE_X = ScopedObjective(ObjSense.MAXIMIZE, (Reduce("sum", X),))
MINIMIZE_X = ScopedObjective(ObjSense.MINIMIZE, (Reduce("sum", X),))


def _const(value):
    return lambda r: {CONSTANT: value}


def _query(decisions, clauses, objective, select="id, x"):
    return f"""SELECT {select} {table('t', COLUMNS, ROWS)}
        DECIDE {decisions} SUCH THAT {clauses} {objective} ORDER BY id"""


def _judged(cli, solver, sql, clauses, objective, decisions=(Decision("x"),)):
    model = ScopedModel(table_rows(COLUMNS, ROWS), list(decisions))
    result = answer(cli, sql)
    assert model.judge_scoped(result, clauses, objective) == pytest.approx(
        model.solve_scoped(solver, clauses, objective), abs=1e-4
    )
    return [row["x"] for row in result]


def _enumerated(cli, sql, domains, feasible, objective, maximize=True):
    """Judge DeciDB's answer against every assignment of ``domains`` (solver columns named
    ``x0``, ``x1``, ... for a per-row decision, the bare name for a query-wide one)."""
    judge_rows(answer(cli, sql), domains, feasible, objective, maximize)


# ---------------------------------------------------------------------------
# A strict bound steps onto the whole numbers
# ---------------------------------------------------------------------------

STRICT = [
    pytest.param(
        "x <= 20 AND MAX(x) > 12",
        "MINIMIZE SUM(x)",
        [Scoped((X,), "<=", (_const(20),)), Scoped((Reduce("max", X),), ">", (_const(12),))],
        MINIMIZE_X,
        0.0,
        id="hard-max",
    ),  # not max 12
    pytest.param(
        "x <= cap AND MIN(x) < 3",
        "MAXIMIZE SUM(x)",
        [CAP_BOUND, Scoped((Reduce("min", X),), "<", (_const(3),))],
        MAXIMIZE_X,
        0.0,
        id="hard-min",
    ),  # not min 3
    pytest.param(
        "x <= cap AND SUM(x) + MAX(x) > 12",
        "MINIMIZE SUM(x)",
        [CAP_BOUND, Scoped((Reduce("sum", X), Reduce("max", X)), ">", (_const(12),))],
        MINIMIZE_X,
        0.0,
        id="composed",
    ),  # not solved as >= 12
    pytest.param(
        "x <= cap AND x > 2.5",
        "MINIMIZE SUM(x)",
        [CAP_BOUND, Scoped((X,), ">", (_const(2.5),))],
        MINIMIZE_X,
        0.0,
        id="fractional-absorbed",
    ),  # not x >= 3.5, infeasible on the 3-cap row
    pytest.param(
        "x <= cap AND x > -2.2",
        "MINIMIZE SUM(x)",
        [CAP_BOUND, Scoped((X,), ">", (_const(-2.2),))],
        MINIMIZE_X,
        float("-inf"),
        id="fractional-negative",
    ),  # not x >= -1.2, so -1
]


@BOTH_SOLVERS
@pytest.mark.correctness
@pytest.mark.parametrize("clauses_sql, objective_sql, clauses, objective, lb", STRICT)
def test_strict_bound_steps_onto_whole_numbers(
    request, scope_solver, cli_fixture, clauses_sql, objective_sql, clauses, objective, lb
):
    cli = request.getfixturevalue(cli_fixture)
    _judged(cli, scope_solver, _query("x(INT)", clauses_sql, objective_sql), clauses, objective, [Decision("x", lb=lb)])


@BOTH_SOLVERS
@pytest.mark.correctness
def test_negative_factor_on_a_hard_max(request, scope_solver, cli_fixture):
    """`-2 * MAX(x) <= -5` is `MAX(x) >= 2.5`: the negative factor reverses the clause, so
    an answer of all zeros violates it."""
    cli = request.getfixturevalue(cli_fixture)
    clauses = [CAP_BOUND, Scoped((Reduce("max", X, factor=lambda r: -2.0),), "<=", (_const(-5),))]
    _judged(
        cli, scope_solver, _query("x(INT)", "x <= cap AND -2 * MAX(x) <= -5", "MINIMIZE SUM(x)"), clauses, MINIMIZE_X
    )


# ---------------------------------------------------------------------------
# A row-varying factor on a reducer, and a data reducer beside a per-row decision
# ---------------------------------------------------------------------------


@BOTH_SOLVERS
@pytest.mark.correctness
@pytest.mark.parametrize(
    "clause_sql, clause",
    [
        pytest.param(
            "cap * sum(x) <= 40",
            Scoped((Reduce("sum", X, factor=lambda r: r["cap"]),), "<=", (_const(40),)),
            id="row-varying-factor",
        ),  # not refused as "'cap' varies per row"
        pytest.param(
            "sum(w) + x <= 10",
            Scoped((Reduce("sum", lambda r: {CONSTANT: r["w"]}), X), "<=", (_const(10),)),
            id="data-reducer-beside-a-row-decision",
        ),  # not refused as "row-scoped decision 'x'"
        pytest.param(
            "x <= avg(cap)",
            Scoped((X,), "<=", (Reduce("avg", lambda r: {CONSTANT: r["cap"]}),)),
            id="data-reducer-bound-of-a-row-decision",
        ),
    ],
)
def test_old_refusal_now_answers(request, scope_solver, cli_fixture, clause_sql, clause):
    cli = request.getfixturevalue(cli_fixture)
    _judged(
        cli,
        scope_solver,
        _query("x(INT)", f"x <= cap AND {clause_sql}", "MAXIMIZE SUM(x)"),
        [CAP_BOUND, clause],
        MAXIMIZE_X,
    )


@BOTH_SOLVERS
@pytest.mark.correctness
def test_max_of_a_decision_bounded_by_max_of_data(request, scope_solver, cli_fixture):
    """`MAX(x) <= MAX(v)` bounds the decision by a data reducer: it once raised an INTERNAL
    error at term extraction."""
    cli = request.getfixturevalue(cli_fixture)
    columns, rows = ("id", "g", "v"), ((1, "b", 9), (2, "b", 1), (3, "a", 2))
    sql = f"""SELECT id, x {table('t', columns, rows)}
        DECIDE x(INT) SUCH THAT x <= 10 AND MAX(x) <= MAX(v) MAXIMIZE SUM(x) ORDER BY id"""
    clauses = [
        Scoped((X,), "<=", (_const(10),)),
        Scoped((Reduce("max", X),), "<=", (Reduce("max", lambda r: {CONSTANT: r["v"]}),)),
    ]
    model = ScopedModel(table_rows(columns, rows), [Decision("x")])
    result = answer(cli, sql)
    assert model.judge_scoped(result, clauses, MAXIMIZE_X) == pytest.approx(
        model.solve_scoped(scope_solver, clauses, MAXIMIZE_X), abs=1e-4
    )


# ---------------------------------------------------------------------------
# The objective is produced once (per ())
# ---------------------------------------------------------------------------


@pytest.mark.parametrize(
    "decisions, objective, message",
    [
        pytest.param(
            "x(INT), per (): s(INT)", "MAXIMIZE SUM(x) - w * s", "per () does not determine w.", id="row-varying-data"
        ),  # not read off row 0's w
        pytest.param(
            "x(INT)", "MAXIMIZE x", "per () does not determine the decision x", id="per-row-decision"
        ),  # not "does not support 'x'(ExpressionClass::COLUMN_REF)"
        pytest.param(
            "x(INT), y(INT)",
            "MAXIMIZE SUM(x) + y",
            "per () does not determine the decision y",
            id="per-row-decision-beside-a-reducer",
        ),  # not "contains a non-aggregate term: y"
    ],
)
def test_objective_term_must_be_one_value(decidb_cli, decisions, objective, message):
    result = decidb_cli.execute_raw(
        _query(decisions, "x <= cap AND s <= 4" if "s(" in decisions else "x <= cap", objective)
    )
    assert message in result.stderr, result.stderr


def test_a_per_row_objective_decision_on_one_row(decidb_cli, scope_solver):
    """`MAXIMIZE x` over one row: `per ()` determines the decision."""
    sql = f"""SELECT id, x {table('t', COLUMNS, ROWS[:1])}
        DECIDE x(INT) SUCH THAT x <= cap MAXIMIZE x ORDER BY id"""
    model = ScopedModel(table_rows(COLUMNS, ROWS[:1]), [Decision("x")])
    objective = ScopedObjective(ObjSense.MAXIMIZE, (X,))
    assert model.judge_scoped(answer(decidb_cli, sql), [CAP_BOUND], objective) == pytest.approx(
        model.solve_scoped(scope_solver, [CAP_BOUND], objective), abs=1e-4
    )


def test_reducer_inside_a_reducer_is_refused(decidb_cli):
    """`SUM(MAX(x))` is refused, never modelled as one reducer."""
    result = decidb_cli.execute_raw(_query("x(INT)", "x <= cap AND SUM(MAX(x)) <= 5", "MAXIMIZE SUM(x)"))
    assert "an aggregate inside an aggregate is not supported yet" in result.stderr, result.stderr


# ---------------------------------------------------------------------------
# D41: an easy MIN/MAX bound is a bound on every row it reads
# ---------------------------------------------------------------------------


@BOTH_SOLVERS
@pytest.mark.correctness
@pytest.mark.parametrize(
    "bound",
    [
        "min(x) >= -5",
        "per row: min(x) >= -5",
        "per (): min(x) >= -5",
        "per g: min(x) >= -5",
        "min(x) by () >= -5",
    ],
)
def test_easy_min_floor_lets_the_decision_go_negative(request, scope_solver, cli_fixture, bound):
    """`min(x) >= -5` is `x >= -5` on every row the reducer reads, a written negative lower
    bound like `x >= -5` (syntax_reference.md §2), whatever the spelling."""
    cli = request.getfixturevalue(cli_fixture)
    _judged(
        cli,
        scope_solver,
        _query("x(INT)", f"x <= cap AND {bound}", "MINIMIZE SUM(x)"),
        [CAP_BOUND, Scoped((Reduce("min", X),), ">=", (_const(-5),))],
        MINIMIZE_X,
        [Decision("x", lb=float("-inf"))],
    )


@pytest.mark.parametrize(
    "bound",
    [
        "max(x) <= 3",
        "per row: max(x) <= 3",
        "per (): max(x) <= 3",
        "per g: max(x) <= 3",
        "max(x) by () <= 3",
    ],
)
def test_easy_max_bound_builds_the_model_of_a_column_bound(decidb_cli, tmp_path, bound):
    """Absorbed into the column box, an easy `max(x) <= 3` adds no row: its model is the one
    `x <= 3` builds."""
    sql = _query("x(INT)", "x <= cap AND {}", "MAXIMIZE SUM(x)")
    assert decidb_cli.dump_model(sql.format(bound), tmp_path / "max.dump") == decidb_cli.dump_model(
        sql.format("x <= 3"), tmp_path / "column.dump"
    )


# ---------------------------------------------------------------------------
# A bound is read by value, not by spelling: the negative floor in every spelling
# ---------------------------------------------------------------------------

_X_TIMES = lambda c: (lambda r: {"x": c})  # noqa: E731
_SIGNED_X = (Decision("x", lb=float("-inf")),)

NEGATIVE_FLOOR_SPELLINGS = [
    pytest.param("x - 1 >= -3", Scoped((X, _const(-1)), ">=", (_const(-3),)), id="x-shift"),
    pytest.param("x >= -2 + 0", Scoped((X,), ">=", (_const(-2),)), id="x-folded-bound"),
    pytest.param("2 * x >= -4", Scoped((_X_TIMES(2),), ">=", (_const(-4),)), id="x-factor"),
    pytest.param("x * 2 >= -4", Scoped((_X_TIMES(2),), ">=", (_const(-4),)), id="x-factor-right"),
    pytest.param("-x <= 2", Scoped((_X_TIMES(-1),), "<=", (_const(2),)), id="x-negated"),
    pytest.param("x / 2 >= -1", Scoped((_X_TIMES(0.5),), ">=", (_const(-1),)), id="x-divided"),
    pytest.param("2 * x > -5", Scoped((_X_TIMES(2),), ">", (_const(-5),)), id="x-strict-factor"),
    pytest.param("min(x) - 1 >= -3", Scoped((Reduce("min", X), _const(-1)), ">=", (_const(-3),)), id="min-shift"),
    pytest.param(
        "2 * min(x) >= -4", Scoped((Reduce("min", X, factor=lambda r: 2.0),), ">=", (_const(-4),)), id="min-factor"
    ),
    pytest.param(
        "-min(x) <= 2", Scoped((Reduce("min", X, factor=lambda r: -1.0),), "<=", (_const(2),)), id="min-negated"
    ),
    pytest.param(
        "min(x) / 2 >= -1", Scoped((Reduce("min", X, factor=lambda r: 0.5),), ">=", (_const(-1),)), id="min-divided"
    ),
    pytest.param(
        "per g: min(x) by (g) - 1 >= -3",
        Scoped((Reduce("min", X, by=("g",)), _const(-1)), ">=", (_const(-3),), per=("g",)),
        id="min-per-shift",
    ),
    pytest.param(
        "2 * (x - 1) >= -6", Scoped((_X_TIMES(2), _const(-2)), ">=", (_const(-6),)), id="x-shift-under-factor"
    ),
    pytest.param("-(x - 1) <= 3", Scoped((_X_TIMES(-1), _const(1)), "<=", (_const(3),)), id="x-shift-under-minus"),
    pytest.param(
        "2 * (min(x) - 1) >= -6",
        Scoped((Reduce("min", X, factor=lambda r: 2.0), _const(-2)), ">=", (_const(-6),)),
        id="min-shift-under-factor",
    ),
    pytest.param(
        "(min(x) - 1) * 2 >= -6",
        Scoped((Reduce("min", X, factor=lambda r: 2.0), _const(-2)), ">=", (_const(-6),)),
        id="min-shift-under-factor-right",
    ),
    pytest.param(
        "(min(x) + 1) / 2 >= -0.5",
        Scoped((Reduce("min", X, factor=lambda r: 0.5), _const(0.5)), ">=", (_const(-0.5),)),
        id="min-shift-under-division",
    ),
    pytest.param(
        "3 - min(x) <= 5",
        Scoped((_const(3), Reduce("min", X, factor=lambda r: -1.0)), "<=", (_const(5),)),
        id="min-subtracted-from-constant",
    ),
    pytest.param("x + x >= -4", Scoped((_X_TIMES(2),), ">=", (_const(-4),)), id="x-repeated"),
    pytest.param("2 * x + x >= -6", Scoped((_X_TIMES(3),), ">=", (_const(-6),)), id="x-repeated-under-factor"),
    pytest.param("x - 3 * x <= 4", Scoped((_X_TIMES(-2),), "<=", (_const(4),)), id="x-repeated-cancelling-to-negative"),
    pytest.param(
        "min(x) + min(x) >= -4",
        Scoped((Reduce("min", X, factor=lambda r: 2.0),), ">=", (_const(-4),)),
        id="min-repeated",
    ),
    pytest.param(
        "2 * min(x) + min(x) >= -6",
        Scoped((Reduce("min", X, factor=lambda r: 3.0),), ">=", (_const(-6),)),
        id="min-repeated-under-factor",
    ),
    pytest.param("3 * x >= x - 4", Scoped((_X_TIMES(3),), ">=", (X, _const(-4))), id="x-repeated-across"),
    pytest.param(
        "3 * min(x) >= min(x) - 4",
        Scoped((Reduce("min", X, factor=lambda r: 3.0),), ">=", (Reduce("min", X), _const(-4))),
        id="min-repeated-across",
    ),
]


@BOTH_SOLVERS
@pytest.mark.correctness
@pytest.mark.parametrize("bound, clause", NEGATIVE_FLOOR_SPELLINGS)
def test_negative_floor_is_read_by_value(request, scope_solver, cli_fixture, bound, clause):
    """Every spelling of the bound `x >= -2` -- shifted, scaled, negated, divided, on `x` or
    on an easy `min(x)` -- lets the decision reach -2 (D41 by value). Before the fix only a
    literal `x >= -2` / `min(x) >= -2` did, and `x - 1 >= -3` kept `x >= 0`."""
    cli = request.getfixturevalue(cli_fixture)
    xs = _judged(
        cli,
        scope_solver,
        _query("x(INT)", f"x <= cap AND {bound}", "MINIMIZE SUM(x)"),
        [CAP_BOUND, clause],
        MINIMIZE_X,
        _SIGNED_X,
    )
    assert min(xs) == -2, xs


@pytest.mark.parametrize(
    "bound",
    [
        "x - 1 >= -3",
        "2 * x >= -4",
        "-x <= 2",
        "2 * (x - 1) >= -6",
        "x + x >= -4",
        "3 * x >= x - 4",
        "min(x) - 1 >= -3",
        "2 * min(x) >= -4",
        "-min(x) <= 2",
        "2 * (min(x) - 1) >= -6",
        "min(x) + min(x) >= -4",
        "3 * min(x) >= min(x) - 4",
    ],
)
def test_affine_bound_builds_the_model_of_a_column_bound(decidb_cli, tmp_path, bound):
    """Absorbed into the column box, a shifted, scaled or negated bound adds no row: its
    model is the one `x >= -2` builds."""
    sql = _query("x(INT)", "x <= cap AND {}", "MINIMIZE SUM(x)")
    assert decidb_cli.dump_model(sql.format(bound), tmp_path / "spelled.dump") == decidb_cli.dump_model(
        sql.format("x >= -2"), tmp_path / "column.dump"
    )


@pytest.mark.parametrize("bound", ["x >= w - 4", "2 * x >= (SELECT -4)", "w * x >= -4", "max(-x) <= 2"])
def test_a_data_bound_stays_a_row(decidb_cli, tmp_path, bound):
    """A side that is data -- a column, a subquery, a column factor -- is not a bound known
    before the query runs, and a MIN/MAX whose member is not a bare decision bounds that
    member: each stays a row, and the decision keeps its default floor."""
    sql = _query("x(INT)", f"x <= cap AND {bound}", "MINIMIZE SUM(x)")
    assert [row["x"] for row in answer(decidb_cli, sql)] == [0, 0, 0]


# ---------------------------------------------------------------------------
# A step on MIN/MAX, and a query-wide decision beside reducers, products and squares
# ---------------------------------------------------------------------------


def _x_domains(**extra):
    return {**{f"x{i}": range(cap + 1) for i, cap in enumerate(CAPS)}, **extra}


def _xs(a):
    return [a[f"x{i}"] for i in range(len(CAPS))]


@pytest.mark.parametrize(
    "decisions, clause, operand, op",
    [
        ("x(INT)", "MAX(x + 0.5) > 3", "max(x + 0.5)", ">"),
        ("x(INT)", "MIN(x - 0.5) < 2", "min(x - 0.5)", "<"),
        ("x(INT)", "2 * MAX(x + 0.25) > 6", "max(x + 0.25)", ">"),
        ("x(INT)", "per g: max(x + 0.5) by (g) > 2", "max(x + 0.5) by (g)", ">"),
        ("x(INT), y(INT)", "y <= 2 AND per row: y + max(x + 0.5) > 8", "max(x + 0.5)", ">"),
        ("x(INT), y(INT)", "y <= 2 AND per row: max(x + 0.5) + y <> 3", "max(x + 0.5)", "<>"),
    ],
)
def test_constant_inside_min_or_max_under_a_step_is_refused(decidb_cli, decisions, clause, operand, op):
    """A strict bound or `<>` on a MIN or MAX steps the extremum itself, where a constant
    inside stays in every member: `MAX(x + 0.5)` is never whole, so the clause is refused
    with the non-strict remedy."""
    stderr = decidb_cli.execute_raw(_query(decisions, f"x <= cap AND {clause}", "MAXIMIZE SUM(x)")).stderr
    assert f"Comparison '{op}' is not supported here: '{operand}' has type DECIMAL" in stderr, stderr


@BOTH_SOLVERS
@pytest.mark.correctness
@pytest.mark.parametrize(
    "clause, holds",
    [
        ("SUM(x) + s <> 5", lambda a: sum(_xs(a)) + a["s"] != 5),
        ("per g: sum(x) by (g) + s <> 4", lambda a: _xs(a)[0] + _xs(a)[1] + a["s"] != 4 and _xs(a)[2] + a["s"] != 4),
    ],
)
def test_query_wide_decision_beside_a_reducer_under_not_equal(request, cli_fixture, clause, holds):
    """`<>` counts a query-wide decision beside the reducers once per group, as `<=`
    does: once per row, `3 + 2 <> 5` would read `3 + 3*2 <> 5` and admit the answer
    the clause excludes."""
    cli = request.getfixturevalue(cli_fixture)
    domains = {**{f"x{i}": range(2) for i in range(len(CAPS))}, "s": range(3)}
    _enumerated(
        cli,
        _query("x(INT), per (): s(INT)", f"x <= 1 AND s <= 2 AND {clause}", "MAXIMIZE SUM(x) + s", select="id, x, s"),
        domains,
        holds,
        lambda a: sum(_xs(a)) + a["s"],
    )


@BOTH_SOLVERS
@pytest.mark.correctness
def test_query_wide_decision_beside_a_boolean_product(request, cli_fixture):
    """`s + SUM(b * x) <= 20` counts `s` once beside the linearized products; the
    objective rewards `b`, so the products bind."""
    cli = request.getfixturevalue(cli_fixture)
    domains = _x_domains(**{f"b{i}": range(2) for i in range(len(CAPS))}, s=range(21))
    _enumerated(
        cli,
        _query(
            "x(INT), b(BOOL), per (): s(INT)",
            "x <= cap AND s <= 20 AND s + SUM(b * x) <= 20",
            "MAXIMIZE SUM(x) + s + 4 * SUM(b)",
            select="id, x, b, s",
        ),
        domains,
        lambda a: a["s"] + sum(a[f"b{i}"] * v for i, v in enumerate(_xs(a))) <= 20,
        lambda a: sum(_xs(a)) + a["s"] + 4 * sum(a[f"b{i}"] for i in range(len(CAPS))),
    )


@pytest.mark.correctness
def test_product_of_query_wide_decisions_in_the_objective(decidb_cli_gurobi, decidb_cli_highs):
    """`MAXIMIZE SUM(x) + s * u` (syntax_reference.md §4: products of decisions in an
    objective). HiGHS cannot load the product and says so."""
    sql = _query(
        "x(INT), per (): s(INT), per (): u(INT)",
        "x <= cap AND s + u <= 6",
        "MAXIMIZE SUM(x) + s * u",
        select="id, x, s, u",
    )
    _enumerated(
        decidb_cli_gurobi,
        sql,
        _x_domains(s=range(7), u=range(7)),
        lambda a: a["s"] + a["u"] <= 6,
        lambda a: sum(_xs(a)) + a["s"] * a["u"],
    )
    assert "needs Gurobi" in decidb_cli_highs.execute_raw(sql).stderr
    # A shared budget makes the product's weight decide: counted once, s * u = 2 loses to
    # three units of x; counted once per row it would win.
    shared = _query(
        "x(INT), per (): s(INT), per (): u(INT)",
        "x <= cap AND per (): sum(x) + s + u <= 3",
        "MAXIMIZE SUM(x) + s * u",
        select="id, x, s, u",
    )
    _enumerated(
        decidb_cli_gurobi,
        shared,
        _x_domains(s=range(4), u=range(4)),
        lambda a: sum(_xs(a)) + a["s"] + a["u"] <= 3,
        lambda a: sum(_xs(a)) + a["s"] * a["u"],
    )
    # The objective's `when` drops the first row; the product still counts, once.
    filtered = _query(
        "x(INT), per (): s(INT), per (): u(INT)",
        "x <= cap AND s <= 4 AND u <= 2",
        "MINIMIZE when id > 1: sum(x) - s * u",
        select="id, x, s, u",
    )
    _enumerated(
        decidb_cli_gurobi,
        filtered,
        _x_domains(s=range(5), u=range(3)),
        lambda a: a["s"] <= 4 and a["u"] <= 2,
        lambda a: sum(_xs(a)[1:]) - a["s"] * a["u"],
        maximize=False,
    )


@pytest.mark.correctness
def test_product_of_query_wide_decisions_inside_a_reducer(decidb_cli_gurobi):
    """`SUM(w * s * u)` weights the product by every row's `w` (Σw = 7): only a product
    outside the reducers counts once. Read off one row it would weigh 2, and `s * u`
    would lose to `- 5 * s`."""
    sql = _query(
        "x(INT), per (): s(INT), per (): u(INT)",
        "x <= cap AND s + u <= 4 AND s <= 3 AND u <= 3",
        "MAXIMIZE SUM(w * s * u) - 5 * s",
        select="id, x, s, u",
    )
    weights = [w for *_, w in ROWS]
    _enumerated(
        decidb_cli_gurobi,
        sql,
        _x_domains(s=range(4), u=range(4)),
        lambda a: a["s"] + a["u"] <= 4,
        lambda a: sum(weights) * a["s"] * a["u"] - 5 * a["s"],
    )


@BOTH_SOLVERS
@pytest.mark.correctness
def test_not_equal_on_an_average_of_boolean_products(request, cli_fixture):
    """`AVG(b * x) <> 5`: the all-in answer averages exactly 5, so one unit or one row
    must give way."""
    cli = request.getfixturevalue(cli_fixture)
    domains = _x_domains(**{f"b{i}": range(2) for i in range(len(CAPS))})
    _enumerated(
        cli,
        _query("x(INT), b(BOOL)", "x <= cap AND AVG(b * x) <> 5", "MAXIMIZE SUM(x) + SUM(b)", select="id, x, b"),
        domains,
        lambda a: sum(a[f"b{i}"] * v for i, v in enumerate(_xs(a))) != 5 * len(CAPS),
        lambda a: sum(_xs(a)) + sum(a[f"b{i}"] for i in range(len(CAPS))),
    )


@pytest.mark.correctness
def test_query_wide_decision_beside_a_squared_reducer(decidb_cli_gurobi, decidb_cli_highs):
    """`SUM(POWER(x, 2)) + s <= 30` counts `s` once, not once per row (syntax_reference.md
    §10.2). HiGHS cannot load a quadratic constraint and says so."""
    sql = _query(
        "x(INT), per (): s(INT)", "x <= 3 AND SUM(POWER(x, 2)) + s <= 30", "MAXIMIZE SUM(x) + s", select="id, x, s"
    )
    domains = {**{f"x{i}": range(4) for i in range(len(CAPS))}, "s": range(31)}
    _enumerated(
        decidb_cli_gurobi,
        sql,
        domains,
        lambda a: sum(v * v for v in _xs(a)) + a["s"] <= 30,
        lambda a: sum(_xs(a)) + a["s"],
    )
    assert "needs Gurobi" in decidb_cli_highs.execute_raw(sql).stderr


# ---------------------------------------------------------------------------
# `by` on norm and count(*) (D34, D35)
# ---------------------------------------------------------------------------


@BOTH_SOLVERS
@pytest.mark.correctness
def test_norm_zero_by_group(request, cli_fixture):
    """`per g: norm(x, 0) by (g) <= 1`: at most one non-zero shipment per group (D35)."""
    cli = request.getfixturevalue(cli_fixture)
    groups = [g for _, _, g, _ in ROWS]
    _enumerated(
        cli,
        _query("x(INT)", "x <= cap AND per g: norm(x, 0) by (g) <= 1", "MAXIMIZE SUM(x)"),
        _x_domains(),
        lambda a: all(sum(1 for v, h in zip(_xs(a), groups) if h == g and v) <= 1 for g in set(groups)),
        lambda a: sum(_xs(a)),
    )


@BOTH_SOLVERS
@pytest.mark.correctness
def test_count_star_by_group(request, scope_solver, cli_fixture):
    """`count(*) by (g)` counts the group's kept rows, as `sum(1) by (g)` does."""
    cli = request.getfixturevalue(cli_fixture)
    clause = Scoped((Reduce("sum", X, by=("g",)),), "<=", (Reduce("sum", _const(1), by=("g",)),), per=("g",))
    _judged(
        cli,
        scope_solver,
        _query("x(INT)", "x <= cap AND per g: sum(x) by (g) <= count(*) by (g)", "MAXIMIZE SUM(x)"),
        [CAP_BOUND, clause],
        MAXIMIZE_X,
    )


# ---------------------------------------------------------------------------
# A refusal reads the same in every spelling
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("spelling", ["", "per row: ", "per (): "])
@pytest.mark.parametrize(
    "clause, verb",
    [
        ("sum(avg(cap) * x) <= 3", "multiply"),
        ("sum(x / avg(cap)) <= 3", "divide"),
        ("sum((x + avg(cap)) / 2) <= 3", "be divided together with"),
    ],
)
def test_data_reducer_scaling_a_decision_inside_a_reducer(decidb_cli, spelling, clause, verb):
    """A reducer over data alone that scales a decision inside a reducer is a coefficient,
    refused with the remedy (pre-compute it) whatever the spelling -- not as a reducer
    inside a reducer."""
    stderr = decidb_cli.execute_raw(_query("x(INT)", f"x <= cap AND {spelling}{clause}", "MAXIMIZE SUM(x)")).stderr
    assert f"cannot {verb} a decision variable. Pre-compute it as a scalar" in stderr, stderr


@pytest.mark.parametrize(
    "clause",
    [
        pytest.param("power(x, 2) <> 9", id="square"),
        pytest.param("sum(power(x, 2)) <> 27", id="sum-of-squares"),
        pytest.param("avg(power(x, 2)) <> 9", id="avg-of-squares"),
        pytest.param("per (): sum(power(x, 2)) + s <> 15", id="query-wide-beside-squares"),
        pytest.param("x * s <> 4", id="product-of-integers"),
    ],
)
def test_not_equal_over_a_product_or_power_is_refused(decidb_cli_gurobi, decidb_cli_highs, clause):
    """`<>` becomes two rows that hold a linear left side only: over a square or a product of
    two decisions neither of which is BOOL it is refused, never solved without those terms
    (it once returned an assignment the clause excludes). HiGHS refuses the product anyway."""
    sql = _query("x(INT), per (): s(INT)", f"x <= 3 AND s <= 3 AND {clause}", "MAXIMIZE SUM(x) + s", select="id, x, s")
    stderr = decidb_cli_gurobi.execute_raw(sql).stderr
    assert "<> over a product or a power of decisions is not supported yet" in stderr, stderr
    assert "Error" in decidb_cli_highs.execute_raw(sql).stderr


@BOTH_SOLVERS
@pytest.mark.correctness
def test_not_equal_over_a_boolean_product_still_answers(request, cli_fixture):
    """A product with a BOOL decision is linear before `<>` is formulated, so it answers."""
    cli = request.getfixturevalue(cli_fixture)
    domains = _x_domains(**{f"b{i}": range(2) for i in range(len(CAPS))})
    _enumerated(
        cli,
        _query("x(INT), b(BOOL)", "x <= cap AND b * x <> 3 AND SUM(b) >= 2", "MAXIMIZE SUM(x)", select="id, x, b"),
        domains,
        lambda a: all(a[f"b{i}"] * v != 3 for i, v in enumerate(_xs(a)))
        and sum(a[f"b{i}"] for i in range(len(CAPS))) >= 2,
        lambda a: sum(_xs(a)),
    )


def test_a_negative_bound_on_an_absolute_value_names_it(decidb_cli):
    """The bound check before solving names an auxiliary decision by what it stands for."""
    stderr = decidb_cli.execute_raw(_query("x(INT)", "abs(x) <= -1", "MAXIMIZE SUM(x)")).stderr
    assert "abs(x) <= -1 cannot hold because abs(x) is never negative" in stderr, stderr
