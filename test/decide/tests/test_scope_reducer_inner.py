"""`when` and `per` inside a reducer: `agg([when c] [per K]: e) [by (Γ)]`
(language/guide.md §5.3).

Order: the rows `by` selects, then the inner `when`, then one value per variant of each
class of the inner `per` -- per distinct value of what the body reads, each read at its
first row the `when` keeps (D70, D71) -- then the aggregate. The inner `when` filters its
own reducer only; `AVG` divides by the values it receives; `MIN`/`MAX` are unaffected by
an inner `per`. A reducer with no row for some group of the kept rows has no value there
(D68).

Optima are judged against `_scope_oracle.ScopedModel`; model shapes against
DECIDB_DUMP_MODEL.
"""

import pytest

from ._scope_cases import (  # noqa: F401
    DEPOT_COLUMNS,
    DEPOTS,
    MAXIMIZE_SHIP,
    SHIPMENTS,
    aligned,
    answer,
    from_clause,
    joined,
    scope_solver,
)
from ._scope_oracle import CONSTANT, GLOBAL, Decision, Reduce, Scoped, ScopedModel, ScopedObjective
from solver.types import ObjSense

SHIP_OPEN = [Decision("ship"), Decision("open", ("depotID",), lb=0, ub=1)]
#: A depot ships only when open: `ship <= S.demand * open`.
SHIP_IF_OPEN = Scoped((lambda r: {"ship": 1, "open": -r["demand"]},), "<=")


def _open_query(clause: str, shipments=SHIPMENTS, objective: str = "MAXIMIZE SUM(ship)") -> str:
    return f"""
        SELECT shipmentID, ship, open {from_clause(shipments)}
        DECIDE ship(INT), per D.depotID: open(BOOL)
        SUCH THAT ship <= S.demand * open AND {clause}
        {objective}
        ORDER BY shipmentID"""


def _judged(cli, solver, sql, model, clauses, objective):
    result = aligned(answer(cli, sql), model)
    assert model.judge_scoped(result, clauses, objective) == pytest.approx(
        model.solve_scoped(solver, clauses, objective)
    )
    return result


def _capacity_once(per=("depotID",), when=None, by=(), agg="sum"):
    return Reduce(agg, lambda r: {"open": r["capacity"]}, when=when, per=per, by=by)


# ---------------------------------------------------------------------------
# Inner when
# ---------------------------------------------------------------------------


@pytest.mark.correctness
def test_inner_when_filters_its_own_reducer_only(decidb_cli, scope_solver):
    """Priority shipments count twice, the others once. With 79 to share, the optimum
    (57) ships every non-priority shipment in full and 22 of the priority ones."""
    sql = f"""
        SELECT shipmentID, ship {from_clause()}
        DECIDE ship(INT)
        SUCH THAT ship <= S.demand AND sum(when S.priority: ship) + sum(ship) <= 79
        MAXIMIZE SUM(ship)
        ORDER BY shipmentID"""
    model = ScopedModel(joined(), [Decision("ship")])
    clauses = [
        Scoped((lambda r: {"ship": 1},), "<=", (lambda r: {CONSTANT: r["demand"]},)),
        Scoped(
            (Reduce("sum", lambda r: {"ship": 1}, when=lambda r: r["priority"]), Reduce("sum", lambda r: {"ship": 1})),
            "<=",
            (lambda r: {CONSTANT: 79},),
        ),
    ]
    result = _judged(decidb_cli, scope_solver, sql, model, clauses, MAXIMIZE_SHIP)
    assert result[1]["ship"] == 20 and result[3]["ship"] == 15


# ---------------------------------------------------------------------------
# Inner per
# ---------------------------------------------------------------------------


@pytest.mark.correctness
def test_inner_per_charges_a_depot_once_across_its_shipments(decidb_cli, scope_solver):
    """D1 and D2 each have two shipments; their capacity counts once when open."""
    sql = _open_query("sum(per D.depotID: D.capacity * open) <= 60")
    model = ScopedModel(joined(), SHIP_OPEN)
    clauses = [SHIP_IF_OPEN, Scoped((_capacity_once(),), "<=", (lambda r: {CONSTANT: 60},))]
    result = _judged(decidb_cli, scope_solver, sql, model, clauses, MAXIMIZE_SHIP)
    assert [row["open"] for row in result] == [0, 0, 1, 1, 1]


@pytest.mark.correctness
def test_an_inner_per_takes_a_by(decidb_cli):
    """`sum(per D: e) by (Γ)` reads its `by` group, which opens D2 and D3 alone."""
    sql = f"""
        SELECT shipmentID, open {from_clause()}
        DECIDE ship(INT), per D: open(BOOL)
        SUCH THAT ship <= S.demand * open AND sum(per D: capacity * open) by (D.region) <= 50
        MAXIMIZE SUM(ship)
        ORDER BY shipmentID"""
    assert [opened for _, opened in decidb_cli.execute(sql)[0]] == [0, 0, 1, 1, 1]


@pytest.mark.correctness
def test_inner_when_and_per_together_count_a_class_from_its_first_kept_row(decidb_cli, scope_solver):
    """D1's first shipment is not a priority, its second is: D1 is still charged once."""
    shipments = ((1, "D1", 10, False, 1, 2, "active"), (2, "D1", 20, True, 1, 3, "active")) + SHIPMENTS[2:]
    sql = _open_query("sum(when S.priority per D.depotID: D.capacity * open) <= 45", shipments)
    model = ScopedModel(joined(shipments), SHIP_OPEN)
    clauses = [SHIP_IF_OPEN, Scoped((_capacity_once(when=lambda r: r["priority"]),), "<=", (lambda r: {CONSTANT: 45},))]
    result = _judged(decidb_cli, scope_solver, sql, model, clauses, MAXIMIZE_SHIP)
    assert [row["open"] for row in result] == [1, 1, 0, 0, 1]


@pytest.mark.correctness
def test_inner_per_empty_hands_one_value(decidb_cli, scope_solver):
    """`sum(per (): D.networkCapacity)` is 60, the one value every row carries."""
    sql = f"""
        SELECT shipmentID, ship {from_clause()}
        DECIDE ship(INT)
        SUCH THAT ship <= S.demand AND sum(ship) <= sum(per (): D.networkCapacity) - 5
        MAXIMIZE SUM(ship)
        ORDER BY shipmentID"""
    model = ScopedModel(joined(), [Decision("ship")])
    clauses = [
        Scoped((lambda r: {"ship": 1},), "<=", (lambda r: {CONSTANT: r["demand"]},)),
        Scoped(
            (Reduce("sum", lambda r: {"ship": 1}),),
            "<=",
            (Reduce("sum", lambda r: {CONSTANT: r["networkCapacity"]}, per=GLOBAL), lambda r: {CONSTANT: -5}),
        ),
    ]
    result = _judged(decidb_cli, scope_solver, sql, model, clauses, MAXIMIZE_SHIP)
    assert sum(row["ship"] for row in result) == 55


@pytest.mark.correctness
def test_avg_divides_by_the_values_it_receives(decidb_cli, scope_solver):
    """Per region, the average open capacity over depots (inner per), not over shipments:
    D1 has one East shipment and D2 three, so both open average (25 + 40) / 2 = 32.5,
    within 33 -- over shipments it would be (25 + 3 * 40) / 4 = 36.25."""
    shipments = ((1, "D1", 10, True, 1, 2, "active"), (2, "D2", 20, False, 1, 3, "active")) + SHIPMENTS[2:]
    sql = _open_query("per D.region: avg(per D.depotID: D.capacity * open) by (D.region) <= 33", shipments)
    model = ScopedModel(joined(shipments), SHIP_OPEN)
    clauses = [
        SHIP_IF_OPEN,
        Scoped((_capacity_once(by=("region",), agg="avg"),), "<=", (lambda r: {CONSTANT: 33},), per=("region",)),
    ]
    result = _judged(decidb_cli, scope_solver, sql, model, clauses, MAXIMIZE_SHIP)
    assert [row["open"] for row in result] == [1, 1, 1, 1, 1]


@pytest.mark.correctness
def test_inner_per_with_by_and_an_outer_per(decidb_cli, scope_solver):
    """`per D.region: sum(per D.depotID: D.capacity * open) by (D.region) <= 50`: each
    region's open capacity, a depot counted once."""
    sql = _open_query("per D.region: sum(per D.depotID: D.capacity * open) by (D.region) <= 50")
    model = ScopedModel(joined(), SHIP_OPEN)
    clauses = [
        SHIP_IF_OPEN,
        Scoped((_capacity_once(by=("region",)),), "<=", (lambda r: {CONSTANT: 50},), per=("region",)),
    ]
    result = _judged(decidb_cli, scope_solver, sql, model, clauses, MAXIMIZE_SHIP)
    assert [row["open"] for row in result] == [0, 0, 1, 1, 1]


@pytest.mark.correctness
@pytest.mark.min_max
def test_max_is_unaffected_by_an_inner_per(decidb_cli, scope_solver):
    sql = _open_query("per D.region: max(per D.depotID: D.capacity * open) by (D.region) <= 30")
    model = ScopedModel(joined(), SHIP_OPEN)
    clauses = [
        SHIP_IF_OPEN,
        Scoped((_capacity_once(by=("region",), agg="max"),), "<=", (lambda r: {CONSTANT: 30},), per=("region",)),
    ]
    result = _judged(decidb_cli, scope_solver, sql, model, clauses, MAXIMIZE_SHIP)
    assert [row["open"] for row in result] == [1, 1, 0, 0, 1]


@pytest.mark.correctness
def test_inner_per_in_the_objective_counts_a_class_from_its_first_kept_row(decidb_cli, scope_solver):
    """D1's first shipment is not a priority: D1's opening still costs 25."""
    shipments = ((1, "D1", 10, False, 1, 2, "active"), (2, "D1", 20, True, 1, 3, "active")) + SHIPMENTS[2:]
    sql = _open_query(
        "open >= 1", shipments, "MINIMIZE sum(when S.priority per D.depotID: D.capacity * open) - SUM(ship)"
    )
    model = ScopedModel(joined(shipments), SHIP_OPEN)
    clauses = [SHIP_IF_OPEN, Scoped((lambda r: {"open": 1},), ">=", (lambda r: {CONSTANT: 1},))]
    objective = ScopedObjective(
        ObjSense.MINIMIZE, (_capacity_once(when=lambda r: r["priority"]), Reduce("sum", lambda r: {"ship": -1}))
    )
    result = aligned(answer(decidb_cli, sql), model)
    assert (
        model.judge_scoped(result, clauses, objective)
        == pytest.approx(model.solve_scoped(scope_solver, clauses, objective))
        == 25 + 40 + 20 - 100
    )


# ---------------------------------------------------------------------------
# Errors that depend on the data
# ---------------------------------------------------------------------------


@pytest.mark.correctness
def test_an_inner_key_takes_one_value_per_variant(decidb_cli, scope_solver):
    """D71: East's two depots differ in capacity (25, 40) and in `open`, keyed by the
    depot, so `per D.region` hands the sum both: 25 * open[D1] + 40 * open[D2] + 20 *
    open[D3] <= 60, and opening D2 and D3 ships the most (45 + 25). Where every East
    depot has one capacity and `open` is keyed by the region, East is one value."""
    sql = _open_query("sum(per D.region: D.capacity * open) <= 60")
    model = ScopedModel(joined(), SHIP_OPEN)
    clauses = [SHIP_IF_OPEN, Scoped((_capacity_once(per=("region",)),), "<=", (lambda r: {CONSTANT: 60},))]
    result = _judged(decidb_cli, scope_solver, sql, model, clauses, MAXIMIZE_SHIP)
    assert sum(row["ship"] for row in result) == 45 + 25

    depots = (DEPOTS[0], ("D2", "East", "CA", 25, 12, 0.2, 0.6, 60), DEPOTS[2])
    rows, _ = decidb_cli.execute(
        f"""
        SELECT shipmentID, ship {from_clause(SHIPMENTS, depots)}
        DECIDE ship(INT), per D.region: open(BOOL)
        SUCH THAT ship <= S.demand * open AND sum(per D.region: D.capacity * open) <= 30
        MAXIMIZE SUM(ship)
        ORDER BY shipmentID"""
    )
    assert [ship for _, ship in rows] == [10, 20, 30, 15, 0]


@pytest.mark.correctness
def test_a_when_in_front_leaves_the_groups_it_keeps_no_row_of_out(decidb_cli):
    """D1 and D3 have no shipment on hold, so only D2's group is bounded."""
    rows, _ = decidb_cli.execute(_open_query("when S.status = 'hold' per D.depotID: sum(ship) by (D.depotID) <= 3"))
    assert [ship for _, ship, _ in rows] == [10, 20, 30, 3, 25]


@pytest.mark.error
@pytest.mark.error_binder
@pytest.mark.parametrize(
    "clause, at",
    [
        pytest.param("norm(when S.priority: ship, 1) <= 5", "norm(when S.priority: ship, 1)", id="scope-in-norm"),
        # The caret is at the decision, not at the comparison that holds it.
        pytest.param("sum(when ship > 1: ship) <= 5", "ship", id="decision-in-inner-when"),
    ],
)
def test_inner_scope_errors(decidb_cli, clause, at):
    decidb_cli.assert_error_at(_open_query(clause), kind="Binder Error", at=at)


@pytest.mark.correctness
def test_an_inner_relation_key_reads_a_per_row_decision_once_per_row(decidb_cli, scope_solver):
    """D71: `ship` is per row, so each row of a D tuple is a variant of its own and `SUM(per D:
    ship)` is every row's ship, as without the `per`: East ships 75 within 80."""
    sql = _open_query("SUM(per D: ship) by (D.region) <= 80")
    model = ScopedModel(joined(), SHIP_OPEN)
    total = Reduce("sum", lambda r: {"ship": 1}, per=DEPOT_COLUMNS, by=("region",))
    clauses = [SHIP_IF_OPEN, Scoped((total,), "<=", (lambda r: {CONSTANT: 80},))]
    result = _judged(decidb_cli, scope_solver, sql, model, clauses, MAXIMIZE_SHIP)
    assert sum(row["ship"] for row in result) == 100
