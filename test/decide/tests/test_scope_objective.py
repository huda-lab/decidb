"""Objective scope (syntax_reference.md §4): an objective is produced once -- an
omitted `per` is `per ()` -- `when` in front filters the rows for the whole objective,
`when` and `per` inside reducers work as in §5.3, and the two-level form
`outer(per k: inner(e) by (k))` evaluates `inner` once per class of `k` (NULL a class)
and `outer` over those values.

A tuple term must be determined by the empty key: a decision outside the reducers counts
once, which needs a single instance over the kept rows, and a data value beside it must
be one value there. Optima are judged against `_scope_oracle.ScopedModel`.
"""

import itertools

import pytest

from ._output_helpers import assert_no_internal_leak
from ._scope_cases import SHIPMENTS, aligned, answer, from_clause, joined, scope_solver, table, table_rows  # noqa: F401
from ._scope_oracle import CONSTANT, Decision, Nested, Reduce, Scoped, ScopedModel, ScopedObjective
from solver.types import ObjSense

SHIP = [Decision("ship")]
SHIP_BOUND = Scoped((lambda r: {"ship": 1},), "<=", (lambda r: {CONSTANT: r["demand"]},))


def _query(objective: str, clause: str = "ship <= S.demand", decide: str = "ship(INT)") -> str:
    return f"""
        SELECT shipmentID, ship {from_clause()}
        DECIDE {decide}
        SUCH THAT {clause}
        {objective}
        ORDER BY shipmentID"""


@pytest.mark.correctness
def test_omitted_per_is_per_empty(decidb_cli, tmp_path):
    omitted = decidb_cli.dump_model(_query("MAXIMIZE SUM(ship)"), tmp_path / "omitted.dump")
    assert omitted == decidb_cli.dump_model(_query("MAXIMIZE per (): sum(ship)"), tmp_path / "written.dump")


@pytest.mark.correctness
def test_when_in_front_is_the_reducer_when(decidb_cli, tmp_path):
    front = decidb_cli.dump_model(_query("MAXIMIZE when S.priority: sum(S.demand * ship)"), tmp_path / "front.dump")
    inside = decidb_cli.dump_model(_query("MAXIMIZE sum(when S.priority: S.demand * ship)"), tmp_path / "inside.dump")
    assert front == inside


@pytest.mark.correctness
def test_when_in_front_reads_kept_rows_only(decidb_cli, scope_solver):
    """S2 and S4 carry a NULL weight; the `when` drops them, so it is never read."""
    columns = ("id", "w", "cap")
    rows = ((1, 3, 5), (2, None, 4), (3, 2, 6), (4, None, 2))
    sql = f"""
        SELECT id, x {table('t', columns, rows)}
        DECIDE x(INT)
        SUCH THAT x <= t.cap AND sum(x) <= 8
        MAXIMIZE when t.w is not null: sum(t.w * x)
        ORDER BY id"""
    model = ScopedModel(table_rows(columns, rows), [Decision("x")])
    clauses = [
        Scoped((lambda r: {"x": 1},), "<=", (lambda r: {CONSTANT: r["cap"]},)),
        Scoped((Reduce("sum", lambda r: {"x": 1}),), "<=", (lambda r: {CONSTANT: 8},)),
    ]
    objective = ScopedObjective(
        ObjSense.MAXIMIZE, (Reduce("sum", lambda r: {"x": r["w"]}),), when=lambda r: r["w"] is not None
    )
    result = aligned(answer(decidb_cli, sql), model, "id")
    assert model.judge_scoped(result, clauses, objective) == pytest.approx(
        model.solve_scoped(scope_solver, clauses, objective)
    )


# ---------------------------------------------------------------------------
# Tuple terms: counted once, one value over the kept rows
# ---------------------------------------------------------------------------


@pytest.mark.correctness
def test_a_query_wide_decision_beside_a_reducer_counts_once(decidb_cli, scope_solver):
    """`D.networkCapacity` is one value over the kept rows, and `s` counts once -- even
    though the `when` drops the first row."""
    sql = f"""
        SELECT shipmentID, ship, s {from_clause()}
        DECIDE ship(INT), per (): s(INT)
        SUCH THAT ship <= S.demand AND s <= 5 AND sum(ship) + s <= 40
        MAXIMIZE when NOT S.priority: sum(ship) + D.networkCapacity * s
        ORDER BY shipmentID"""
    model = ScopedModel(joined(), [Decision("ship"), Decision("s", ())])
    clauses = [
        SHIP_BOUND,
        Scoped((lambda r: {"s": 1},), "<=", (lambda r: {CONSTANT: 5},)),
        Scoped((Reduce("sum", lambda r: {"ship": 1}), lambda r: {"s": 1}), "<=", (lambda r: {CONSTANT: 40},), per=()),
    ]
    objective = ScopedObjective(
        ObjSense.MAXIMIZE,
        (Reduce("sum", lambda r: {"ship": 1}), lambda r: {"s": r["networkCapacity"]}),
        when=lambda r: not r["priority"],
    )
    result = aligned(answer(decidb_cli, sql), model)
    assert (
        model.judge_scoped(result, clauses, objective)
        == pytest.approx(model.solve_scoped(scope_solver, clauses, objective))
        == 35 + 60 * 5
    )


@pytest.mark.correctness
def test_a_keyed_decision_with_one_instance_counts_once(decidb_cli, scope_solver):
    """`r` is keyed by depot; over D1's two shipments it has one instance, counted once."""
    sql = f"""
        SELECT shipmentID, ship, r {from_clause()}
        DECIDE ship(INT), per D.depotID: r(INT)
        SUCH THAT ship <= S.demand AND r <= 7 AND sum(ship) <= 50
        MAXIMIZE when D.depotID = 'D1': sum(ship) + 3 * r
        ORDER BY shipmentID"""
    model = ScopedModel(joined(), [Decision("ship"), Decision("r", ("depotID",))])
    clauses = [
        SHIP_BOUND,
        Scoped((lambda r: {"r": 1},), "<=", (lambda r: {CONSTANT: 7},)),
        Scoped((Reduce("sum", lambda r: {"ship": 1}),), "<=", (lambda r: {CONSTANT: 50},), per=()),
    ]
    objective = ScopedObjective(
        ObjSense.MAXIMIZE,
        (Reduce("sum", lambda r: {"ship": 1}), lambda r: {"r": 3}),
        when=lambda r: r["depotID"] == "D1",
    )
    result = aligned(answer(decidb_cli, sql), model)
    assert (
        model.judge_scoped(result, clauses, objective)
        == pytest.approx(model.solve_scoped(scope_solver, clauses, objective))
        == 30 + 21
    )


@pytest.mark.error
@pytest.mark.parametrize(
    "objective, message",
    [
        pytest.param(
            "MAXIMIZE when S.priority: sum(ship) + D.capacity * s",
            "DECIDE: per () does not determine D.capacity. The kept rows have more than one value: 25, 40.",
            id="data-value",
        ),
        pytest.param(
            "MAXIMIZE when S.priority: ship",
            "DECIDE: per () does not determine the decision ship (declared per row). The kept rows cover more "
            "than one ship.",
            id="per-row-decision",
        ),
        pytest.param(
            "MAXIMIZE sum(ship) by (D.region)",
            "DECIDE: per () does not determine D.region (in by (D.region)). The kept rows have more than one "
            "value: 'East', 'West'.",
            id="top-level-by",
        ),
        pytest.param(
            "MAXIMIZE when S.priority: sum(ship) + s * s",
            "DECIDE objective: 's * s' multiplies decisions outside a reducer, which is not supported yet",
            id="product-outside-a-reducer",
        ),
    ],
)
def test_objective_tuple_term_errors(decidb_cli, objective, message):
    result = decidb_cli.execute_raw(_query(objective, "ship <= S.demand AND s <= 5", "ship(INT), per (): s(INT)"))
    assert message in result.stderr, result.stderr
    assert_no_internal_leak(result, "an objective tuple term error")


# ---------------------------------------------------------------------------
# The two-level form
# ---------------------------------------------------------------------------

GROUPS = ("id", "g", "cap", "w", "c")
GROUP_ROWS = (
    (1, "a", 5, 1, True),
    (2, "a", 4, -1, False),
    (3, "b", 6, 1, True),
    (4, "b", 2, 2, True),
    (5, None, 3, 1, True),
    (6, None, 9, -2, False),
)


def _two_level_query(outer, inner, sense, clause="x <= t.cap AND sum(x) >= 6", when=""):
    return f"""
        SELECT id, x {table('t', GROUPS, GROUP_ROWS)}
        DECIDE x(INT)
        SUCH THAT {clause}
        {sense} {when}{outer}(per t.g: {inner}(t.w * x) by (t.g))
        ORDER BY id"""


@pytest.mark.correctness
@pytest.mark.parametrize("outer, inner", list(itertools.product(("sum", "min", "max"), repeat=2)))
@pytest.mark.parametrize("sense", ["MINIMIZE", "MAXIMIZE"])
def test_two_level_form(decidb_cli, scope_solver, outer, inner, sense):
    """All nine combinations in both directions, the NULL group a class of its own."""
    sql = _two_level_query(outer, inner, sense)
    model = ScopedModel(table_rows(GROUPS, GROUP_ROWS), [Decision("x")])
    clauses = [
        Scoped((lambda r: {"x": 1},), "<=", (lambda r: {CONSTANT: r["cap"]},)),
        Scoped((Reduce("sum", lambda r: {"x": 1}),), ">=", (lambda r: {CONSTANT: 6},), per=()),
    ]
    objective = ScopedObjective(ObjSense[sense], (Nested(outer, ("g",), inner, lambda r: {"x": r["w"]}),))
    result = aligned(answer(decidb_cli, sql), model, "id")
    assert model.judge_scoped(result, clauses, objective) == pytest.approx(
        model.solve_scoped(scope_solver, clauses, objective)
    )


@pytest.mark.correctness
def test_two_level_form_under_when(decidb_cli, scope_solver):
    """The `when` drops row 2, group a's heaviest weight: the best group is then b, at 8;
    without the `when` it is a, at 21 through row 2."""
    columns = ("id", "g", "cap", "w", "c")
    rows = ((1, "a", 5, 1, True), (2, "a", 4, 5, False), (3, "b", 3, 2, True), (4, "b", 6, 1, True))
    model = ScopedModel(table_rows(columns, rows), [Decision("x")])
    clauses = [
        Scoped((lambda r: {"x": 1},), "<=", (lambda r: {CONSTANT: r["cap"]},)),
        Scoped((Reduce("sum", lambda r: {"x": 1}),), "<=", (lambda r: {CONSTANT: 5},), per=()),
    ]
    nested = (Nested("max", ("g",), "sum", lambda r: {"x": r["w"]}),)
    for when, oracle_when, optimum in (("when t.c: ", lambda r: r["c"], 8), ("", None, 21)):
        sql = f"""
            SELECT id, x {table('t', columns, rows)}
            DECIDE x(INT)
            SUCH THAT x <= t.cap AND sum(x) <= 5
            MAXIMIZE {when}max(per t.g: sum(t.w * x) by (t.g))
            ORDER BY id"""
        objective = ScopedObjective(ObjSense.MAXIMIZE, nested, when=oracle_when)
        result = aligned(answer(decidb_cli, sql), model, "id")
        assert (
            model.judge_scoped(result, clauses, objective)
            == pytest.approx(model.solve_scoped(scope_solver, clauses, objective))
            == optimum
        )


@pytest.mark.error
@pytest.mark.error_binder
@pytest.mark.parametrize(
    "objective, message",
    [
        pytest.param(
            "MAXIMIZE max(per t.g: sum(x) by (t.id))",
            "DECIDE: in max(per t.g: sum(...) by (t.id)) the by key must be the per key; any other is not "
            "supported yet",
            id="different-keys",
        ),
        pytest.param(
            "MAXIMIZE max(per t.g: sum(when t.c: x) by (t.g))",
            "DECIDE: when inside the inner reducer of max(per t.g: sum(...) by (t.g)) is not supported yet; "
            "write it in front of the objective",
            id="inner-when",
        ),
        pytest.param(
            "MAXIMIZE max(when t.c per t.g: sum(x) by (t.g))",
            "DECIDE: when inside the outer reducer of max(per t.g: ...) is not supported yet; write it in "
            "front of the objective",
            id="outer-when",
        ),
        pytest.param(
            "MAXIMIZE sum(per t.g: max(per t.id: sum(x) by (t.id)) by (t.g))",
            "DECIDE: an aggregate inside an aggregate is not supported yet",
            id="three-levels",
        ),
        pytest.param(
            "MAXIMIZE per t.g: sum(x)",
            "an objective is produced once: write per (): or leave per out",
            id="keyed-objective",
        ),
    ],
)
def test_two_level_refusals(decidb_cli, objective, message):
    result = decidb_cli.execute_raw(
        f"""
        SELECT id, x {table('t', GROUPS, GROUP_ROWS)}
        DECIDE x(INT)
        SUCH THAT x <= t.cap
        {objective}"""
    )
    assert message in result.stderr, result.stderr
    assert_no_internal_leak(result, "a two-level objective refusal")


@pytest.mark.error
@pytest.mark.error_binder
def test_a_nested_reducer_in_a_constraint_is_refused(decidb_cli):
    """Deck example 9: nesting is the objective's two-level form only (§10.2)."""
    result = decidb_cli.execute_raw(
        _query("MAXIMIZE SUM(ship)", "ship <= S.demand AND sum(per D.depotID: max(ship) by (D.depotID)) <= 50")
    )
    assert "DECIDE: an aggregate inside an aggregate is not supported yet" in result.stderr, result.stderr


@pytest.mark.error
@pytest.mark.parametrize(
    "objective",
    [
        "MAXIMIZE when S.status = 'lost': sum(ship)",
        "MINIMIZE when S.status = 'lost': max(per D.depotID: sum(ship) by (D.depotID))",
    ],
)
def test_an_objective_when_that_keeps_no_row(decidb_cli, objective):
    result = decidb_cli.execute_raw(_query(objective))
    assert "DECIDE: when S.status = 'lost' matches no rows." in result.stderr, result.stderr


@pytest.mark.error
def test_an_objective_reducer_with_no_row(decidb_cli):
    result = decidb_cli.execute_raw(_query("MAXIMIZE sum(ship) - sum(when S.status = 'lost': ship)"))
    assert "DECIDE: sum(when S.status = 'lost': ship) has no rows." in result.stderr, result.stderr


@pytest.mark.correctness
@pytest.mark.min_max
def test_an_inner_min_counts_every_member_of_its_group(decidb_cli, scope_solver):
    """Row 1's weight is 0, which caps group a's minimum at 0 whatever x1 is: the budget
    belongs to group b. A member is a row the reducer reads, not a row with a nonzero
    coefficient."""
    columns = ("id", "g", "w")
    rows = ((1, "a", 0), (2, "a", 2), (3, "b", 1))
    sql = f"""
        SELECT id, x {table('t', columns, rows)}
        DECIDE x(INT)
        SUCH THAT x <= 3 AND sum(x) <= 3
        MAXIMIZE sum(per t.g: min(t.w * x) by (t.g))
        ORDER BY id"""
    model = ScopedModel(table_rows(columns, rows), [Decision("x")])
    clauses = [
        Scoped((lambda r: {"x": 1},), "<=", (lambda r: {CONSTANT: 3},)),
        Scoped((Reduce("sum", lambda r: {"x": 1}),), "<=", (lambda r: {CONSTANT: 3},), per=()),
    ]
    objective = ScopedObjective(ObjSense.MAXIMIZE, (Nested("sum", ("g",), "min", lambda r: {"x": r["w"]}),))
    result = aligned(answer(decidb_cli, sql), model, "id")
    assert (
        model.judge_scoped(result, clauses, objective)
        == pytest.approx(model.solve_scoped(scope_solver, clauses, objective))
        == 3
    )


@pytest.mark.correctness
@pytest.mark.min_max
def test_a_composed_objective_reads_the_kept_rows(decidb_cli, scope_solver):
    """`when S.priority: min(S.demand * ship) + sum(ship)`: S2, a priority shipment with
    no demand, holds the minimum at 0, so the objective is the priority total."""
    shipments = SHIPMENTS[:1] + ((2, "D1", 0, True, 1, 3, "active"),) + SHIPMENTS[2:]
    sql = f"""
        SELECT shipmentID, ship {from_clause(shipments)}
        DECIDE ship(INT)
        SUCH THAT ship <= 3 AND sum(ship) <= 6
        MAXIMIZE when S.priority: min(S.demand * ship) + sum(ship)
        ORDER BY shipmentID"""
    model = ScopedModel(joined(shipments), SHIP)
    clauses = [
        Scoped((lambda r: {"ship": 1},), "<=", (lambda r: {CONSTANT: 3},)),
        Scoped((Reduce("sum", lambda r: {"ship": 1}),), "<=", (lambda r: {CONSTANT: 6},), per=()),
    ]
    objective = ScopedObjective(
        ObjSense.MAXIMIZE,
        (Reduce("min", lambda r: {"ship": r["demand"]}), Reduce("sum", lambda r: {"ship": 1})),
        when=lambda r: r["priority"],
    )
    result = aligned(answer(decidb_cli, sql), model)
    assert (
        model.judge_scoped(result, clauses, objective)
        == pytest.approx(model.solve_scoped(scope_solver, clauses, objective))
        == 6
    )
