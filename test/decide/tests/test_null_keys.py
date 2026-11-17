"""A NULL in a written key column puts the row in no group of that key (B1, deck p. 30).

The row leaves only that key's groups: a reducer over another key still reads it. A keyed decision has no value on
the row (its output is NULL); a reducer over the decision skips the row, and an unreduced use skips that instance.
A relation key (`per D`) compares NULL as a value. Every test is judged by `_scope_oracle`, which states the rule
independently.
"""

import pytest

from ._scope_cases import (  # noqa: F401
    aligned,
    answer,
    const,
    scope_solver,
    table,
    table_rows,
    values,
)
from ._scope_oracle import (
    CONSTANT,
    Decision,
    Objective,
    Reduce,
    RelationColumns,
    RowClause,
    ScopeModel,
    Scoped,
    ScopedModel,
    ScopedObjective,
)
from solver.types import ObjSense, VarType

COLUMNS = ("id", "g", "p")
ROWS = ((1, "a", 5.0), (2, "a", 4.0), (3, None, 3.0), (4, "b", 2.0), (5, None, 1.0))
MAXIMIZE_PX = ScopedObjective(ObjSense.MAXIMIZE, (Reduce("sum", lambda r: {"x": r["p"]}),))


def _judged(cli, solver, sql, model, clauses, objective):
    result = aligned(answer(cli, sql), model, "id")
    assert model.judge_scoped(result, clauses, objective) == pytest.approx(
        model.solve_scoped(solver, clauses, objective)
    )
    return result


@pytest.mark.correctness
def test_a_null_by_key_leaves_only_that_reducer(decidb_cli, scope_solver):
    """`sum(x) by (g) <= 0.5 * sum(x)`: a NULL-`g` row has no limit of its own, and the total still counts it."""
    sql = f"""
        SELECT id, x {table('t', COLUMNS, ROWS)}
        DECIDE x(BOOL) SUCH THAT sum(x) by (t.g) <= 0.5 * sum(x) MAXIMIZE SUM(t.p * x) ORDER BY id"""
    model = ScopedModel(table_rows(COLUMNS, ROWS), [Decision("x", var_type=VarType.BINARY)])
    clauses = [
        Scoped(
            (Reduce("sum", lambda r: {"x": 1}, by=("g",)),),
            "<=",
            (Reduce("sum", lambda r: {"x": 1}, factor=lambda r: 0.5),),
        )
    ]
    _judged(decidb_cli, scope_solver, sql, model, clauses, MAXIMIZE_PX)


@pytest.mark.correctness
def test_a_per_key_that_is_null_on_every_row_has_no_instance(decidb_cli, scope_solver):
    """`per g: sum(x) <= 1` with every `g` NULL has no class, so no instance: every row is picked."""
    rows = tuple((i, None, p) for i, _, p in ROWS)
    sql = f"""
        SELECT id, x {table('t', COLUMNS, rows)}
        DECIDE x(BOOL) SUCH THAT per t.g: sum(x) <= 1 MAXIMIZE SUM(t.p * x) ORDER BY id"""
    model = ScopedModel(table_rows(COLUMNS, rows), [Decision("x", var_type=VarType.BINARY)])
    clauses = [Scoped((Reduce("sum", lambda r: {"x": 1}),), "<=", (lambda r: {CONSTANT: 1},), per=("g",))]
    result = _judged(decidb_cli, scope_solver, sql, model, clauses, MAXIMIZE_PX)
    assert [row["x"] for row in result] == [1] * len(rows)


@pytest.mark.correctness
def test_a_keyed_decision_with_a_null_key_is_null_and_skips_its_instance(decidb_cli, scope_solver):
    """`per g: y` has no value on a NULL-`g` row: the row shows NULL and its `y >= 1` is skipped."""
    sql = f"""
        SELECT id, y {table('t', COLUMNS, ROWS)}
        DECIDE per t.g: y(INT) SUCH THAT y <= 3 AND y >= 1 MAXIMIZE SUM(t.p * y) ORDER BY id"""
    model = ScopedModel(table_rows(COLUMNS, ROWS), [Decision("y", key=("g",))])
    clauses = [
        Scoped((lambda r: {"y": 1},), "<=", (lambda r: {CONSTANT: 3},)),
        Scoped((lambda r: {"y": 1},), ">=", (lambda r: {CONSTANT: 1},)),
    ]
    objective = ScopedObjective(ObjSense.MAXIMIZE, (Reduce("sum", lambda r: {"y": r["p"]}),))
    result = _judged(decidb_cli, scope_solver, sql, model, clauses, objective)
    assert [row["y"] for row in result if row["id"] in (3, 5)] == [None, None]


@pytest.mark.correctness
def test_avg_over_a_keyed_decision_divides_by_the_rows_with_a_decision(decidb_cli, scope_solver):
    """`avg(y) <= 2` reads rows 1, 2 and 4: the NULL-`g` rows have no decision, so AVG skips them."""
    sql = f"""
        SELECT id, y {table('t', COLUMNS, ROWS)}
        DECIDE per t.g: y(INT) SUCH THAT y <= 3 AND avg(y) <= 2 MAXIMIZE SUM(t.p * y) ORDER BY id"""
    model = ScopedModel(table_rows(COLUMNS, ROWS), [Decision("y", key=("g",))])
    clauses = [
        Scoped((lambda r: {"y": 1},), "<=", (lambda r: {CONSTANT: 3},)),
        Scoped((Reduce("avg", lambda r: {"y": 1}),), "<=", (lambda r: {CONSTANT: 2},), per=()),
    ]
    objective = ScopedObjective(ObjSense.MAXIMIZE, (Reduce("sum", lambda r: {"y": r["p"]}),))
    _judged(decidb_cli, scope_solver, sql, model, clauses, objective)


DEPOT_COLUMNS = ("dk", "region")
DEPOT_ROWS = (("D1", "East"), ("D2", None))
SHIP_COLUMNS = ("id", "dk", "g", "cap", "p")
SHIP_ROWS = (
    (1, "D1", "a", 3, 5.0),
    (2, "D1", "a", 2, 4.0),
    (3, "D2", "a", 4, 3.0),
    (4, "D2", None, 1, 2.0),
    (5, "D1", None, 2, 1.0),
)


@pytest.mark.correctness
def test_a_relation_key_compares_null_as_a_value_beside_a_written_column(decidb_cli, scope_solver):
    """`per D, S.g: y`: D2's NULL region is a value of `D`, so row 3 is a class of its own; a NULL `S.g` is no class,
    so rows 4 and 5 have no decision."""
    sql = f"""
        SELECT S.id, y FROM {values(DEPOT_ROWS, 'D', DEPOT_COLUMNS)} JOIN {values(SHIP_ROWS, 'S', SHIP_COLUMNS)}
        ON D.dk = S.dk
        DECIDE per D, S.g: y(INT) SUCH THAT y <= S.cap MAXIMIZE SUM(S.p * y) ORDER BY S.id"""
    depots = dict(DEPOT_ROWS)
    rows = [dict(zip(SHIP_COLUMNS, row), region=depots[row[1]]) for row in SHIP_ROWS]
    model = ScopeModel(rows, [Decision("y", key=(RelationColumns(("dk", "region")), "g"))])
    clauses = [RowClause(lambda r: {"y": 1}, "<=", lambda r: r["cap"])]
    objective = Objective(ObjSense.MAXIMIZE, rows=lambda r: {"y": r["p"]})
    result = aligned(answer(decidb_cli, sql), model, "id")
    assert model.judge(result, clauses, objective) == pytest.approx(model.solve(scope_solver, clauses, objective))
    assert model.class_count("y") == 2
    assert [row["y"] for row in result if row["id"] in (4, 5)] == [None, None]


# A bound known before the query runs, under `per K:`: a row whose written key column is NULL is in no class, so the
# clause gives it no bound (guide §6.2). Folded into the column's box, the bound was the whole column's and capped
# that row too (FoldSingletonRowsInExpression). Row 2's `g` and row 3's `h` are NULL; the rows are judged on both
# backends against the scope oracle, which splits the rows into classes and drops a row in none.
BOUND_COLUMNS = ("id", "g", "h", "v")
BOUND_ROWS = ((1, "a", "p", 3.0), (2, None, "p", 5.0), (3, "b", None, 6.0))
_X = lambda r: {"x": 1}  # noqa: E731
_BOUND_SOLVERS = pytest.mark.parametrize("cli_fixture", ["decidb_cli_gurobi", "decidb_cli_highs"])
_BOUND_CASES = [
    pytest.param("per t.g: x <= 2", [Scoped((_X,), "<=", (const(2),), per=("g",))], id="constant"),
    pytest.param("per t.g, t.h: x <= 2", [Scoped((_X,), "<=", (const(2),), per=("g", "h"))], id="two-columns"),
    pytest.param(
        "per t.g: 2 * x <= 4", [Scoped((lambda r: {"x": 2},), "<=", (const(4),), per=("g",))], id="scaled-coefficient"
    ),
    pytest.param("per t.g: x = 2", [Scoped((_X,), "=", (const(2),), per=("g",))], id="equality"),
    pytest.param(
        "per t.g: x BETWEEN 1 AND 2",
        [Scoped((_X,), ">=", (const(1),), per=("g",)), Scoped((_X,), "<=", (const(2),), per=("g",))],
        id="between",
    ),
]


def _judge_bound(cli, solver, clause, clauses, sense, sign):
    sign_text = "-" if sign < 0 else ""
    sql = f"""
        SELECT id, x {table('t', BOUND_COLUMNS, BOUND_ROWS)}
        DECIDE x(INT) SUCH THAT x <= 10 AND {clause} {sense} SUM({sign_text}t.v * x) ORDER BY id"""
    model = ScopedModel(table_rows(BOUND_COLUMNS, BOUND_ROWS), [Decision("x")])
    clauses = [Scoped((_X,), "<=", (const(10),))] + clauses
    objective = ScopedObjective(
        ObjSense.MAXIMIZE if sign > 0 else ObjSense.MINIMIZE, (Reduce("sum", lambda r: {"x": sign * r["v"]}),)
    )
    return _judged(cli, solver, sql, model, clauses, objective)


@_BOUND_SOLVERS
@pytest.mark.correctness
@pytest.mark.parametrize("clause, clauses", _BOUND_CASES)
def test_a_null_key_row_takes_no_constant_bound_under_per(request, scope_solver, cli_fixture, clause, clauses):
    result = _judge_bound(request.getfixturevalue(cli_fixture), scope_solver, clause, clauses, "MAXIMIZE", 1)
    # The row with a NULL key is bounded by `x <= 10` alone.
    assert result[1]["x"] == 10


@_BOUND_SOLVERS
@pytest.mark.correctness
def test_a_null_key_row_takes_no_constant_bound_under_per_minimized(request, scope_solver, cli_fixture):
    """The MINIMIZE twin: `MINIMIZE SUM(-v * x)` is the same optimum, reached from the other sense."""
    result = _judge_bound(
        request.getfixturevalue(cli_fixture),
        scope_solver,
        "per t.g: x <= 2",
        [Scoped((_X,), "<=", (const(2),), per=("g",))],
        "MINIMIZE",
        -1,
    )
    assert result[1]["x"] == 10


# `per K: x IN (...)` generates rows (indicators, or an equality for one value) that stand beside the clause. They keep
# its `per`: dropped, the NULL-key row was held to the domain. A whole-numbered `x` is in {1, 2} exactly when
# 1 <= x <= 2, and in {2} exactly when x = 2, which is how the oracle states them.
_IN_CASES = [
    pytest.param(
        "x IN (1, 2)",
        [Scoped((_X,), ">=", (const(1),), per=("g",)), Scoped((_X,), "<=", (const(2),), per=("g",))],
        id="indicators",
    ),
    pytest.param("x IN (2)", [Scoped((_X,), "=", (const(2),), per=("g",))], id="one-value"),
]


@_BOUND_SOLVERS
@pytest.mark.correctness
@pytest.mark.parametrize("domain, clauses", _IN_CASES)
def test_a_null_key_row_is_outside_the_domain_of_a_per_in(request, scope_solver, cli_fixture, domain, clauses):
    """The row with a NULL `g` is in no class, so only `x <= 10` bounds it."""
    cli = request.getfixturevalue(cli_fixture)
    result = _judge_bound(cli, scope_solver, f"per t.g: {domain}", clauses, "MAXIMIZE", 1)
    assert [row["x"] for row in result] == [2, 10, 2]


@_BOUND_SOLVERS
@pytest.mark.correctness
def test_a_relation_null_key_keeps_its_constant_bound_in_the_box(request, scope_solver, cli_fixture):
    """`per D: x <= 2` with D2's region NULL: D2 is a class (a relation's columns compare NULL as a value), so every
    row has the bound and it still folds into the column's box, which a written column's NULL does not allow."""
    cli = request.getfixturevalue(cli_fixture)
    sql = f"""
        SELECT S.id, x FROM {values(DEPOT_ROWS, 'D', DEPOT_COLUMNS)} JOIN {values(SHIP_ROWS, 'S', SHIP_COLUMNS)}
        ON D.dk = S.dk
        DECIDE x(INT) SUCH THAT x <= 10 AND per D: x <= 2 MAXIMIZE SUM(S.p * x) ORDER BY S.id"""
    depots = dict(DEPOT_ROWS)
    rows = [dict(zip(SHIP_COLUMNS, row), region=depots[row[1]]) for row in SHIP_ROWS]
    model = ScopedModel(rows, [Decision("x")])
    clauses = [
        Scoped((_X,), "<=", (const(10),)),
        Scoped((_X,), "<=", (const(2),), per=(RelationColumns(("dk", "region")),)),
    ]
    objective = ScopedObjective(ObjSense.MAXIMIZE, (Reduce("sum", lambda r: {"x": r["p"]}),))
    result = aligned(answer(cli, sql), model, "id")
    assert model.judge_scoped(result, clauses, objective) == pytest.approx(
        model.solve_scoped(scope_solver, clauses, objective)
    )
    assert [row["x"] for row in result] == [2] * len(rows)
    # Folded into the box, the clause makes no constraints, which EXPLAIN ANALYZE counts under a clause that does
    # (`per d: 3 constraints from 2 classes`); a clause left to its rows would show that line.
    profile = cli.execute_raw("EXPLAIN ANALYZE " + sql).stdout
    assert "constraints" not in "".join(line.strip("│ ") for line in profile.splitlines()).split("Constraints:")[-1]
