"""The grid of scope combinations (04_testing/scope/done.md): a `when` or not, `per` omitted / `row` /
`()` / a key, a body with a tuple term only / one reducer / a decision beside a reducer /
two reducers with different `by` keys, the reducer's own scope none / `when` / `per` /
both, SUM / AVG / MIN / MAX, over a per-row decision and one keyed by `h` (the only one a
reducer's `per t.h` can count once) -- every cell against `_scope_oracle.ScopedModel`.

Two outcomes are allowed, and nothing else: the oracle's optimum, or the error the
language defines for the data (the oracle raises it: a `when` that keeps no row, a
reducer with no row for a group), with the same phrase. A class whose rows differ in
what the body reads is one instance per variant (D64), never an error.
Every cell is handled, so no cell is refused; never a wrong answer, never an internal
error.
"""

import pytest

from ._output_helpers import assert_no_internal_leak
from ._scope_cases import aligned, answer, scope_solver, table, table_rows  # noqa: F401 -- fixture
from ._scope_oracle import CONSTANT, GLOBAL, PER_ROW, Decision, Reduce, ScopeError, Scoped, ScopedModel, ScopedObjective
from solver.types import ObjSense

#: Two groups of `g`, two of `h` crossing them, a condition that drops row 2, a per-row
#: bound `v` and a per-`g` constant `cap`.
COLUMNS = ("id", "g", "h", "c", "v", "cap")
ROWS = ((1, "a", "p", True, 10, 20), (2, "a", "q", False, 20, 20), (3, "b", "p", True, 30, 25),
        (4, "b", "q", True, 15, 25))

X = lambda r: {"x": 1}  # noqa: E731
C = lambda r: r["c"]  # noqa: E731
CAP = lambda r: {CONSTANT: r["cap"]}  # noqa: E731
X_BOUND = Scoped((X,), "<=", (lambda r: {CONSTANT: r["v"]},))
MAXIMIZE_X = ScopedObjective(ObjSense.MAXIMIZE, (Reduce("sum", X),))

#: The scope in front: SQL prefix -> (when, per).
FRONTS = {
    "": (None, PER_ROW),
    "when t.c: ": (C, PER_ROW),
    "per row: ": (None, PER_ROW),
    "when t.c per row: ": (C, PER_ROW),
    "per (): ": (None, GLOBAL),
    "when t.c per (): ": (C, GLOBAL),
    "per t.g: ": (None, ("g",)),
    "when t.c per t.g: ": (C, ("g",)),
}
#: The reducer's own scope: SQL prefix of its argument -> (when, per).
INNERS = {
    "": (None, PER_ROW),
    "when t.c: ": (C, PER_ROW),
    "per t.h: ": (None, ("h",)),
    "when t.c per t.h: ": (C, ("h",)),
}
AGGS = ("sum", "avg", "min", "max")
#: The decision: declaration -> oracle decision.
DECISIONS = {"x(INT)": Decision("x"), "per t.h: x(INT)": Decision("x", ("h",))}


def _reduce(agg, inner, by):
    when, per = INNERS[inner]
    return Reduce(agg, X, when=when, per=per, by=by)


#: Body name -> (SQL for (agg, inner), oracle parts (lhs, sense, rhs) for (agg, inner)).
BODIES = {
    "tuple": (lambda agg, inner: "x <= t.cap", lambda agg, inner: ((X,), "<=", (CAP,))),
    "one-reducer": (lambda agg, inner: f"{agg}({inner}x) by (t.g) <= t.cap",
                    lambda agg, inner: ((_reduce(agg, inner, ("g",)),), "<=", (CAP,))),
    "decision-beside": (lambda agg, inner: f"2 * x <= {agg}({inner}x) by (t.g) + t.cap",
                        lambda agg, inner: ((lambda r: {"x": 2},), "<=", (_reduce(agg, inner, ("g",)), CAP))),
    "two-keys": (lambda agg, inner: f"{agg}({inner}x) by (t.g) + {agg}({inner}x) by (t.h) <= 2 * t.cap",
                 lambda agg, inner: ((_reduce(agg, inner, ("g",)), _reduce(agg, inner, ("h",))), "<=",
                                     (lambda r: {CONSTANT: 2 * r["cap"]},))),
}


def _cells():
    for declaration in DECISIONS:
        for front in FRONTS:
            for body in BODIES:
                variants = [("sum", "")] if body == "tuple" else [(agg, inner) for agg in AGGS for inner in INNERS]
                for agg, inner in variants:
                    label = f"{declaration}|{front}{body}:{agg}:{inner}".replace(" ", "_")
                    yield pytest.param(declaration, front, body, agg, inner, id=label)


@pytest.mark.correctness
@pytest.mark.parametrize("declaration, front, body, agg, inner", list(_cells()))
def test_grid_cell(decidb_cli, scope_solver, declaration, front, body, agg, inner):
    sql_of, parts_of = BODIES[body]
    sql = f"""
        SELECT id, x {table('t', COLUMNS, ROWS)}
        DECIDE {declaration}
        SUCH THAT x <= t.v AND {front}{sql_of(agg, inner)}
        MAXIMIZE SUM(x)
        ORDER BY id"""
    when, per = FRONTS[front]
    lhs, sense, rhs = parts_of(agg, inner)
    clauses = [X_BOUND, Scoped(lhs, sense, rhs, when=when, per=per)]
    model = ScopedModel(table_rows(COLUMNS, ROWS), [DECISIONS[declaration]])

    try:
        expected = model.solve_scoped(scope_solver, clauses, MAXIMIZE_X)
    except ScopeError as error:
        expected = error
    result = decidb_cli.execute_raw(sql)
    assert_no_internal_leak(result, f"grid cell {front}{sql_of(agg, inner)}")
    if isinstance(expected, ScopeError):
        assert expected.phrase in result.stderr, f"the language refuses this data ({expected}); got:\n{result.stderr}"
        return
    assert "not supported yet" not in result.stderr, f"items 3 to 5 handle every cell; got:\n{result.stderr}"
    assert result.returncode == 0 and not result.stderr.strip(), result.stderr
    rows = aligned(answer(decidb_cli, sql), model, "id")
    assert model.judge_scoped(rows, clauses, MAXIMIZE_X) == pytest.approx(expected, abs=1e-4)
