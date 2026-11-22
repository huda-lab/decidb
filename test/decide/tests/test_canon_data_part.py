"""A SUM or AVG keeps the data part of its body on the bound side (CANON-1).

A body that is affine in its decisions splits by linearity: `sum(e)` is `sum(e - e0) + sum(e0)`, where `e0` is the body
with every decision set to 0. The first reducer is the decisions, the second a reducer over data, which the bound
side reads. A reducer the clause holds beside a decision (the value shape) then holds whole numbers wherever the
decisions do, so a strict `<`, `>` or `<>` steps on it exactly. Every query is judged by the scope oracle, or by
enumeration where the oracle does not state the body, on the default plan and with direct solve off.
"""

import re

import pytest

from ._brute_force import judge_rows
from ._scope_cases import answer, const, scope_solver, table, table_rows  # noqa: F401 -- scope_solver is a fixture
from ._scope_oracle import CONSTANT, Decision, Reduce, Scoped, ScopedModel, ScopedObjective
from solver.types import ObjSense

COLUMNS = ("id", "g", "d", "wi", "w")
# `d` has a NULL and so has `wi`, on the row that the group A shares with its first row. `w` is a fractional column.
ROWS = ((1, "A", 0.5, 2, 0.5), (2, "A", None, None, 1.0), (3, "B", 1.5, 3, 1.5), (4, "B", 0.25, 1, 2.0))

MODES = pytest.mark.parametrize("extra_env", [{}, {"DECIDB_TEST_DIRECT_SOLVE": "off"}], ids=["auto", "direct-off"])

X = lambda r: {"x": 1}  # noqa: E731
CAP = Scoped((X,), "<=", (const(4),))
MINIMIZE = ScopedObjective(ObjSense.MINIMIZE, (Reduce("sum", X),))
MAXIMIZE = ScopedObjective(ObjSense.MAXIMIZE, (Reduce("sum", X),))


def _sql(clause, sense, columns="id, x"):
    return f"""SELECT {columns} {table('t', COLUMNS, ROWS)}
        DECIDE x(INT) SUCH THAT x <= 4 AND {clause} {sense} sum(x) ORDER BY id"""


def _judged(cli, solver, clause_sql, sense, clauses):
    """DeciDB's answer to `clause_sql` against the oracle's optimum for `clauses`."""
    model = ScopedModel(table_rows(COLUMNS, ROWS), [Decision("x")])
    objective = MINIMIZE if sense == "MINIMIZE" else MAXIMIZE
    result = answer(cli, _sql(clause_sql, sense))
    clauses = [CAP] + list(clauses)
    assert model.judge_scoped(result, clauses, objective) == pytest.approx(
        model.solve_scoped(solver, clauses, objective), abs=1e-4
    )


def _sum(body, **kwargs):
    return Reduce("sum", body, **kwargs)


# Each body is `x` or a multiple of it plus a data part, and the clause holds the reducer beside a decision. The
# bound is chosen where stepping on the whole value would answer differently from stepping on the decisions
# (`x + 0.3` over four rows is 1.2 beyond the decisions: `> 4` is `>= 3` of them, not `>= 4`).
@MODES
@pytest.mark.correctness
@pytest.mark.parametrize(
    "clause, sense, parts",
    [
        pytest.param(
            "sum(x + 0.3) + x > 4",
            "MINIMIZE",
            ((_sum(lambda r: {"x": 1, CONSTANT: 0.3}), X), ">", 4),
            id="constant-addend",
        ),
        pytest.param(
            "sum((x + 0.3) * 3) + x < 11",
            "MAXIMIZE",
            ((_sum(lambda r: {"x": 3, CONSTANT: 0.9}), X), "<", 11),
            id="constant-in-a-product",
        ),
        pytest.param(
            "sum((x + d) * 2) + x > 8",
            "MINIMIZE",
            ((_sum(lambda r: {"x": 2, CONSTANT: None if r["d"] is None else 2 * r["d"]}), X), ">", 8),
            id="data-column-in-a-product-with-a-null",
        ),
        pytest.param(
            "sum(x + d) + x > 4",
            "MINIMIZE",
            ((_sum(lambda r: {"x": 1, CONSTANT: r["d"]}), X), ">", 4),
            id="data-column-with-a-null",
        ),
        pytest.param(
            "sum(x * wi + 0.3) + x > 7",
            "MINIMIZE",
            ((_sum(lambda r: {"x": r["wi"], CONSTANT: 0.3}), X), ">", 7),
            id="null-in-the-coefficient-skips-the-data-part",
        ),
        pytest.param(
            "sum(x + d) by (g) + x > 2",
            "MINIMIZE",
            ((_sum(lambda r: {"x": 1, CONSTANT: r["d"]}, by=("g",)), X), ">", 2),
            id="by-group-with-a-null",
        ),
        pytest.param(
            "sum(when g = 'A': x + 0.3) by (g) + x > 2",
            "MINIMIZE",
            (
                (_sum(lambda r: {"x": 1, CONSTANT: 0.3}, when=lambda r: r["g"] == "A", by=("g",)), X),
                ">",
                2,
            ),
            id="empty-group-of-a-sum",
        ),
    ],
)
def test_a_data_part_beside_a_decision_agrees_with_the_oracle(request, scope_solver, extra_env, clause, sense, parts):
    """The reducer's rows are the oracle's: a NULL in the data part or in a coefficient skips the row from both halves,
    and a group the reducer's own `when` empties has no instance."""
    cli = request.getfixturevalue("decidb_cli").with_env(extra_env)
    terms, op, bound = parts
    _judged(cli, scope_solver, clause, sense, [Scoped(terms, op, (const(bound),))])


@MODES
@pytest.mark.correctness
@pytest.mark.parametrize(
    "clause, parts",
    [
        pytest.param(
            "avg(x + d) by (g) + x >= 3",
            ((Reduce("avg", lambda r: {"x": 1, CONSTANT: r["d"]}, by=("g",)), X), ">=", 3),
            id="avg-with-a-null",
        ),
        pytest.param(
            "avg(when g = 'A': x + 0.3) by (g) + x >= 1",
            (
                (Reduce("avg", lambda r: {"x": 1, CONSTANT: 0.3}, when=lambda r: r["g"] == "A", by=("g",)), X),
                ">=",
                1,
            ),
            id="empty-group-of-an-avg",
        ),
    ],
)
def test_an_avg_with_a_data_part_reads_the_rows_its_halves_share(request, scope_solver, extra_env, clause, parts):
    cli = request.getfixturevalue("decidb_cli").with_env(extra_env)
    terms, op, bound = parts
    _judged(cli, scope_solver, clause, "MINIMIZE", [Scoped(terms, op, (const(bound),))])


@MODES
@pytest.mark.correctness
@pytest.mark.parametrize(
    "body, total",
    [
        pytest.param("abs(x) + 0.3", lambda xs: sum(abs(x) + 0.3 for x in xs), id="abs"),
        pytest.param("max(x) + 0.3", lambda xs: sum(max(xs) + 0.3 for _ in xs), id="a-reducer-over-decisions"),
    ],
)
def test_a_body_with_a_node_beyond_a_sum_splits_its_data_part_too(request, extra_env, body, total):
    """`sum(abs(x) + 0.3)` is `sum(abs(x)) + sum(0.3)`: `abs(x)` stays under the first, whole, and the constant is
    the bound's. Judged by enumeration: the oracle does not state `abs`."""
    cli = request.getfixturevalue("decidb_cli").with_env(extra_env)
    bound = 9 if body.startswith("max") else 3
    sql = _sql(f"sum({body}) + x > {bound}", "MINIMIZE")
    count = len(ROWS)
    judge_rows(
        answer(cli, sql),
        {f"x{i}": range(5) for i in range(count)},
        lambda a: all(total([a[f"x{j}"] for j in range(count)]) + a[f"x{i}"] > bound for i in range(count)),
        lambda a: sum(a.values()),
        False,
    )


@MODES
@pytest.mark.correctness
def test_a_decision_with_no_value_on_a_row_leaves_both_halves(request, scope_solver, extra_env):
    """`per g: y` has no value on a NULL-`g` row, so `sum(y + 0.3)` skips it: the constant is added for the three rows
    with a decision, not the five."""
    cli = request.getfixturevalue("decidb_cli").with_env(extra_env)
    columns = ("id", "g", "p")
    rows = ((1, "a", 5.0), (2, "a", 4.0), (3, None, 3.0), (4, "b", 2.0), (5, None, 1.0))
    sql = f"""SELECT id, y {table('t', columns, rows)}
        DECIDE per t.g: y(INT) SUCH THAT y <= 3 AND sum(y + 0.3) + y > 6 MINIMIZE SUM(t.p * y) ORDER BY id"""
    model = ScopedModel(table_rows(columns, rows), [Decision("y", key=("g",))])
    clauses = [
        Scoped((lambda r: {"y": 1},), "<=", (const(3),)),
        Scoped((_sum(lambda r: {"y": 1, CONSTANT: 0.3}), lambda r: {"y": 1}), ">", (const(6),)),
    ]
    objective = ScopedObjective(ObjSense.MINIMIZE, (_sum(lambda r: {"y": r["p"]}),))
    result = answer(cli, sql)
    assert model.judge_scoped(result, clauses, objective) == pytest.approx(
        model.solve_scoped(scope_solver, clauses, objective), abs=1e-4
    )


@pytest.mark.correctness
@pytest.mark.explain
def test_a_data_part_that_folds_to_zero_is_not_split(decidb_cli):
    """`x + 0` has no data part to move: the canonical clause is the written one. Only a body with a data part has a
    canonical layer of its own, `sum(x + 0.3 - 0.3) + x > -sum(0.3) + 4`."""
    plan = decidb_cli.execute_raw("EXPLAIN " + _sql("sum(x + 0) + x > 4", "MINIMIZE")).stdout
    assert "sum(x + 0) + x > 4" in plan and "≡" not in plan, plan
    split = decidb_cli.execute_raw("EXPLAIN " + _sql("sum(x + 0.3) + x > 4", "MINIMIZE")).stdout
    assert "≡" in split, split
    # A reducer over data alone reads no decision, so it has no data part to move either.
    data = re.sub(
        r"[\s│┌┐└┘─┬┴├┤]+",
        "",
        decidb_cli.execute_raw("EXPLAIN " + _sql("sum(d + 0.3) + sum(x) > 4", "MINIMIZE")).stdout,
    )
    assert "sum(d+0.3)" in data and "sum(d+0.3-" not in data, data


# A constant inside a data factor is part of the coefficient: `x * (w + 0.5)` is `x * w + 0.5 * x`, off the whole
# numbers, as `x * 0.5` is. Nothing moves to the bound.
@pytest.mark.error
@pytest.mark.parametrize("clause", ["sum(x * (w + 0.5)) > 2", "sum(x * (w + 0.5)) + x > 2"])
def test_a_constant_in_a_data_factor_takes_no_strict_step(decidb_cli, clause):
    """DecideSideIsWholeNumbered, the product's decision-free factor: its addends count."""
    decidb_cli.assert_error_at(
        _sql(clause, "MINIMIZE"), kind="Not implemented Error", at=clause, word="'>' compares 'x * (w + 0.5)'"
    )


@pytest.mark.correctness
def test_a_whole_data_factor_with_a_constant_takes_the_step(decidb_cli):
    """`wi + 1` is whole, so `x * (wi + 1)` is: the same branch, taken the other way."""
    assert decidb_cli.execute(_sql("sum(x * (wi + 1)) > 2", "MINIMIZE"))[0]
