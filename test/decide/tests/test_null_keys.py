"""A NULL in a written key column puts the row in no group of that key (B1, deck p. 30).

The row leaves only that key's groups: a reducer over another key still reads it. A keyed decision has no value on
the row (its output is NULL); a reducer over the decision skips the row, and an unreduced use skips that instance.
A relation key (`per D`) compares NULL as a value. Every test is judged by `_scope_oracle`, which states the rule
independently.
"""

import pytest

from ._scope_cases import aligned, answer, scope_solver, table, table_rows, values  # noqa: F401
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
