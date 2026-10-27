"""Deck examples 1-8 (syntax_reference.md §6.5), as written and in their fully
explicit form.

Each runs over the deck's query -- `FROM Shipment S JOIN Depot D USING (depotID)` with
`DECIDE ship(INT), per D.depotID: reserve(REAL)` -- and is checked four ways: both forms
build the same model (DECIDB_DUMP_MODEL), the model has one row per generated class (plus,
for examples 1, 5 and 8, one definition row per helper the value rewrite adds; examples 3
and 6 bound one decision, so their classes fold into the reserve column's box), the
optimum is the oracle's (`_scope_oracle.ScopedModel`, which follows the ANR definition
literally), and the answer satisfies every clause the language generates. Example 7 is
also run on data where one depot has no priority shipment: the reducer has no row for
that class, which is an error naming it (D8, D18).

Example 1 is written bare, `ship <= 0.20 * sum(ship)`: a clause with no `when`, `per` or
`by` follows the same rules (D30).
"""

import pytest

from ._scope_cases import (
    SHIPMENTS,
    aligned,
    answer,
    dump_counts,
    from_clause,
    joined,
    scope_solver,
)  # noqa: F401 -- scope_solver is a fixture
from ._scope_oracle import CONSTANT, Decision, Reduce, Scoped, ScopedModel, ScopedObjective
from ._output_helpers import assert_no_internal_leak
from solver.types import ObjSense, VarType

DECISIONS = [Decision("ship"), Decision("reserve", ("depotID",), VarType.CONTINUOUS)]

#: `ship <= S.demand`: bounds every shipment, folded into its column's box (no row).
SHIP_BOUND = Scoped((lambda r: {"ship": 1},), "<=", (lambda r: {CONSTANT: r["demand"]},))


def _query(clause: str, objective: str, shipments=SHIPMENTS) -> str:
    return f"""
        SELECT shipmentID, ship, reserve {from_clause(shipments)}
        DECIDE ship(INT), per D.depotID: reserve(REAL)
        SUCH THAT ship <= S.demand AND {clause}
        {objective}
        ORDER BY shipmentID"""


MAXIMIZE_SHIP = ("MAXIMIZE SUM(ship)", ScopedObjective(ObjSense.MAXIMIZE, (Reduce("sum", lambda r: {"ship": 1}),)))
#: Shipments as large as allowed, each depot's reserve as small as allowed.
SHIP_LESS_RESERVE = (
    "MAXIMIZE SUM(ship) - SUM(reserve)",
    ScopedObjective(ObjSense.MAXIMIZE, (Reduce("sum", lambda r: {"ship": 1, "reserve": -1}),)),
)


def _demand(row):
    return {CONSTANT: row["demand"]}


EXAMPLES = [
    pytest.param(
        "ship <= 0.20 * sum(ship)",
        "per row: ship <= 0.20 * sum(per row: ship) by ()",
        Scoped((lambda r: {"ship": 1},), "<=", (Reduce("sum", lambda r: {"ship": 1}, factor=lambda r: 0.20),)),
        5 + 1,
        MAXIMIZE_SHIP,
        id="1",
    ),  # one row per shipment, one definition row for the total
    pytest.param(
        "per (): sum(ship) <= networkCapacity",
        "per (): sum(per row: ship) by () <= networkCapacity",
        Scoped((Reduce("sum", lambda r: {"ship": 1}),), "<=", (lambda r: {CONSTANT: r["networkCapacity"]},), per=()),
        1,
        MAXIMIZE_SHIP,
        id="2",
    ),
    pytest.param(
        "per D.depotID: reserve >= D.networkReserveShare * sum(S.demand)",
        "per D.depotID: reserve >= D.networkReserveShare * sum(per row: S.demand) by ()",
        Scoped(
            (lambda r: {"reserve": 1},),
            ">=",
            (Reduce("sum", _demand, factor=lambda r: r["networkReserveShare"]),),
            per=("depotID",),
        ),
        0,
        SHIP_LESS_RESERVE,
        id="3",
    ),  # each depot's bound is its reserve column's lower bound
    pytest.param(
        "per D.depotID: sum(ship) by (D.depotID) <= D.capacity",
        "per D.depotID: sum(per row: ship) by (D.depotID) <= D.capacity",
        Scoped(
            (Reduce("sum", lambda r: {"ship": 1}, by=("depotID",)),),
            "<=",
            (lambda r: {CONSTANT: r["capacity"]},),
            per=("depotID",),
        ),
        3,
        MAXIMIZE_SHIP,
        id="4",
    ),
    pytest.param(
        "per D.depotID: reserve >= 0.10 * sum(S.demand) by (D.region) + 0.02 * sum(S.demand) by (D.country)",
        "per D.depotID: reserve >= 0.10 * sum(per row: S.demand) by (D.region) "
        "+ 0.02 * sum(per row: S.demand) by (D.country)",
        Scoped(
            (lambda r: {"reserve": 1},),
            ">=",
            (
                Reduce("sum", _demand, by=("region",), factor=lambda r: 0.10),
                Reduce("sum", _demand, by=("country",), factor=lambda r: 0.02),
            ),
            per=("depotID",),
        ),
        0,
        SHIP_LESS_RESERVE,
        id="6",
    ),  # each depot's bound is its reserve column's lower bound
    pytest.param(
        "ship <= D.maxShipmentShare * sum(ship) by (D.depotID)",
        "per row: ship <= D.maxShipmentShare * sum(per row: ship) by (D.depotID)",
        Scoped(
            (lambda r: {"ship": 1},),
            "<=",
            (Reduce("sum", lambda r: {"ship": 1}, by=("depotID",), factor=lambda r: r["maxShipmentShare"]),),
        ),
        5 + 3,
        MAXIMIZE_SHIP,
        id="5",
    ),  # one row per shipment, one definition row per depot
    pytest.param(
        "per D.depotID: sum(when S.priority: ship) by (D.depotID) <= D.priorityCapacity",
        "per D.depotID: sum(when S.priority per row: ship) by (D.depotID) <= D.priorityCapacity",
        Scoped(
            (Reduce("sum", lambda r: {"ship": 1}, when=lambda r: r["priority"], by=("depotID",)),),
            "<=",
            (lambda r: {CONSTANT: r["priorityCapacity"]},),
            per=("depotID",),
        ),
        3,
        MAXIMIZE_SHIP,
        id="7",
    ),
    pytest.param(
        "per S.shipmentID: ship <= 0.25 * sum(ship) by (D.depotID, S.dispatchDay + S.transitDays)",
        "per S.shipmentID: ship <= 0.25 * sum(per row: ship) by (D.depotID, S.dispatchDay + S.transitDays)",
        Scoped(
            (lambda r: {"ship": 1},),
            "<=",
            (
                Reduce(
                    "sum",
                    lambda r: {"ship": 1},
                    by=lambda r: (r["depotID"], r["dispatchDay"] + r["transitDays"]),
                    factor=lambda r: 0.25,
                ),
            ),
            per=("shipmentID",),
        ),
        5 + 5,
        MAXIMIZE_SHIP,
        id="8",
    ),  # one row per shipment, each arriving alone: one definition row each
]


@pytest.mark.correctness
@pytest.mark.parametrize("written, explicit, clause, classes, objective", EXAMPLES)
def test_deck_example(decidb_cli, scope_solver, tmp_path, written, explicit, clause, classes, objective):
    sql_objective, oracle_objective = objective
    sql = _query(written, sql_objective)
    dump = decidb_cli.dump_model(sql, tmp_path / "written.dump")
    assert dump == decidb_cli.dump_model(_query(explicit, sql_objective), tmp_path / "explicit.dump")
    # One row per generated class for the example (and one definition row per helper of the
    # value rewrite, examples 1, 5 and 8); `ship <= S.demand` is the ship columns' bound.
    assert dump_counts(dump)["num_rows"] == classes

    model = ScopedModel(joined(), DECISIONS)
    clauses = [SHIP_BOUND, clause]
    result = aligned(answer(decidb_cli, sql), model)
    assert model.judge_scoped(result, clauses, oracle_objective) == pytest.approx(
        model.solve_scoped(scope_solver, clauses, oracle_objective), abs=1e-4
    )


@pytest.mark.correctness
def test_deck_example_7_on_a_depot_without_priority_shipments(decidb_cli, scope_solver):
    """D3's only shipment is not a priority: `sum(when S.priority: ship) by (D.depotID)`
    reads no row for it, an error naming the group (D8, D18). Written in front, the
    same condition leaves D3 out instead (§5.3)."""
    shipments = SHIPMENTS[:4] + ((5, "D3", 25, False, 3, 1, "active"),)
    sql = _query(
        "per D.depotID: sum(when S.priority: ship) by (D.depotID) <= D.priorityCapacity",
        "MAXIMIZE SUM(ship)",
        shipments,
    )
    result = decidb_cli.execute_raw(sql)
    assert "DECIDE: sum(when S.priority: ship) has no rows for D.depotID = 'D3'." in result.stderr, result.stderr
    assert_no_internal_leak(result, "deck example 7 on a depot without priority shipments")

    moved = _query(
        "when S.priority per D.depotID: sum(ship) by (D.depotID) <= D.priorityCapacity", "MAXIMIZE SUM(ship)", shipments
    )
    model = ScopedModel(joined(shipments), DECISIONS)
    clauses = [
        SHIP_BOUND,
        Scoped(
            (Reduce("sum", lambda r: {"ship": 1}, by=("depotID",)),),
            "<=",
            (lambda r: {CONSTANT: r["priorityCapacity"]},),
            when=lambda r: r["priority"],
            per=("depotID",),
        ),
    ]
    objective = MAXIMIZE_SHIP[1]
    rows = aligned(answer(decidb_cli, moved), model)
    assert model.judge_scoped(rows, clauses, objective) == pytest.approx(
        model.solve_scoped(scope_solver, clauses, objective)
    )
    assert rows[4]["ship"] == 25  # D3 has no constraint: its shipment takes its whole demand
