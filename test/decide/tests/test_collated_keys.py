"""A key groups rows as GROUP BY does: under its column's collation (B1).

`COLLATE NOCASE` puts 'a' and 'A' in one group, for a `by` group, a `per K:` class, a bound-side reducer and a keyed
decision; a message quotes the group's least spelling ('A'). The oracle states NOCASE on its own: it groups on the
lowercased key `gk`.
"""

import pytest

from ._scope_cases import aligned, answer, scope_solver, values  # noqa: F401
from ._scope_oracle import CONSTANT, Decision, Reduce, Scoped, ScopedModel, ScopedObjective
from solver.types import ObjSense, VarType

COLUMNS = ("id", "g", "cap", "p")
ROWS = ((1, "a", 1, 5.0), (2, "A", 2, 4.0), (3, "b", 2, 3.0), (4, "B", 3, 2.0), (5, "b", 2, 1.0))
X = Decision("x", var_type=VarType.BINARY)
MAXIMIZE_PX = ScopedObjective(ObjSense.MAXIMIZE, (Reduce("sum", lambda r: {"x": r["p"]}),))


def _source(rows=ROWS, collation="COLLATE NOCASE"):
    return f"FROM (SELECT id, g {collation} AS g, cap, p FROM {values(rows, 'v', COLUMNS)}) t"


def _oracle_rows(rows=ROWS):
    return [dict(zip(COLUMNS, row), gk=row[1].lower()) for row in rows]


def _judged(cli, solver, sql, decisions, clauses, objective):
    model = ScopedModel(_oracle_rows(), decisions)
    result = aligned(answer(cli, sql), model, "id")
    assert model.judge_scoped(result, clauses, objective) == pytest.approx(
        model.solve_scoped(solver, clauses, objective)
    )
    return result


@pytest.mark.correctness
def test_a_by_group_groups_its_key_under_the_collation(decidb_cli, scope_solver):
    """`sum(x) by (g) <= 1`: 'a' and 'A' are one group, so one of rows 1 and 2 is picked, and one of rows 3-5."""
    sql = f"SELECT id, x {_source()} DECIDE x(BOOL) SUCH THAT sum(x) by (t.g) <= 1 MAXIMIZE SUM(t.p * x) ORDER BY id"
    clauses = [Scoped((Reduce("sum", lambda r: {"x": 1}, by=("gk",)),), "<=", (lambda r: {CONSTANT: 1},))]
    result = _judged(decidb_cli, scope_solver, sql, [X], clauses, MAXIMIZE_PX)
    assert sum(row["x"] for row in result) == 2


@pytest.mark.correctness
def test_a_bound_side_reducer_reads_its_group_under_the_collation(decidb_cli, scope_solver):
    """`sum(x) by (g) <= min(cap) by (g)`: the bound is read over 'a' and 'A' together, as the sum is."""
    sql = f"""
        SELECT id, x {_source()}
        DECIDE x(BOOL) SUCH THAT sum(x) by (t.g) <= min(t.cap) by (t.g) MAXIMIZE SUM(t.p * x) ORDER BY id"""
    clauses = [
        Scoped(
            (Reduce("sum", lambda r: {"x": 1}, by=("gk",)),),
            "<=",
            (Reduce("min", lambda r: {CONSTANT: r["cap"]}, by=("gk",)),),
        )
    ]
    _judged(decidb_cli, scope_solver, sql, [X], clauses, MAXIMIZE_PX)


@pytest.mark.correctness
def test_a_keyed_decision_has_one_value_per_collated_key(decidb_cli, scope_solver):
    """`per g: y`: rows 1 and 2 share one decision, rows 3-5 another."""
    sql = f"""
        SELECT id, y {_source()}
        DECIDE per t.g: y(INT) SUCH THAT y <= t.cap AND sum(y) <= 2 MAXIMIZE SUM(t.p * y) ORDER BY id"""
    clauses = [
        Scoped((lambda r: {"y": 1},), "<=", (lambda r: {CONSTANT: r["cap"]},)),
        Scoped((Reduce("sum", lambda r: {"y": 1}),), "<=", (lambda r: {CONSTANT: 2},), per=()),
    ]
    objective = ScopedObjective(ObjSense.MAXIMIZE, (Reduce("sum", lambda r: {"y": r["p"]}),))
    result = _judged(decidb_cli, scope_solver, sql, [Decision("y", key=("gk",))], clauses, objective)
    assert result[0]["y"] == result[1]["y"]


def test_a_message_quotes_the_least_spelling_of_the_group(decidb_cli):
    """`per g` does not determine `cap` once 'a' (cap 1) and 'A' (cap 3) are one class; the message names 'A'."""
    rows = ((1, "a", 1, 5.0), (2, "A", 3, 4.0))
    sql = f"SELECT id, x {_source(rows)} DECIDE x(BOOL) SUCH THAT per t.g: sum(x) <= t.cap MAXIMIZE SUM(t.p * x)"
    decidb_cli.assert_error(sql, match=r"t\.g = 'A' has more than one value")
