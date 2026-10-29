"""The determination rule (language/guide.md §7.1): a `per` key must determine
everything its body reads once per class -- every tuple term (a bound, a factor on a
reducer), every decision outside reducers, every reducer's `by` key -- and an inner
`per` key the body of its reducer.

It is checked against the data, over the kept rows, before solving, so each case is a
pair: the query runs on data where the key determines the value, and stops with the
named error on data where it does not. The message quotes the key, the expression and
one class holding two values, in the user's SQL only (§10.1). Without `per` the same clause
is legal: one constraint per row, which is the tightest bound per group (S3).
"""

import pytest

from ._output_helpers import assert_no_internal_leak
from ._scope_cases import DEPOTS, SHIPMENTS, aligned, answer, from_clause, joined, scope_solver  # noqa: F401
from ._scope_oracle import CONSTANT, Decision, Reduce, Scoped, ScopedModel, ScopedObjective, ScopeError
from solver.types import ObjSense

#: A second East depot whose capacity differs within its region.
EAST_SPLIT = (DEPOTS[0], ("D2", "East", "CA", 40, 12, 0.2, 0.6, 60), DEPOTS[2])
#: Every East depot shares one capacity.
EAST_EVEN = (DEPOTS[0], ("D2", "East", "CA", 25, 12, 0.2, 0.6, 60), DEPOTS[2])


def _query(
    clause: str, depots=DEPOTS, shipments=SHIPMENTS, decide: str = "ship(INT)", objective: str = "MAXIMIZE SUM(ship)"
) -> str:
    return f"""
        SELECT shipmentID, ship {from_clause(shipments, depots)}
        DECIDE {decide}
        SUCH THAT ship <= S.demand AND {clause}
        {objective}
        ORDER BY shipmentID"""


def _stops(cli, sql: str, message: str) -> None:
    result = cli.execute_raw(sql)
    assert message in result.stderr, result.stderr
    assert "does not determine" in result.stderr
    assert_no_internal_leak(result, "a determination error")


PAIRS = [
    pytest.param(
        "per D.region: sum(ship) by (D.region) <= D.capacity",
        EAST_EVEN,
        EAST_SPLIT,
        "DECIDE: per D.region does not determine D.capacity. D.region = 'East' has more than one value: " "25, 40.",
        id="bound",
    ),
    pytest.param("per D.depotID: sum(ship) by (S.dispatchDay) <= 500", DEPOTS, DEPOTS, None, id="by-key-holds"),
    pytest.param(
        "per D.region: sum(ship) by (D.depotID) <= 50",
        (DEPOTS[0], ("D2", "North", "CA", 40, 12, 0.2, 0.6, 60), DEPOTS[2]),
        DEPOTS,
        "DECIDE: per D.region does not determine D.depotID (in by (D.depotID)). D.region = 'East' has "
        "more than one value: 'D1', 'D2'.",
        id="by-key",
    ),
    pytest.param(
        "per D.region: sum(ship) by (D.region) <= D.maxShipmentShare * sum(S.demand) by (D.region)",
        (DEPOTS[0], ("D2", "East", "CA", 40, 12, 0.2, 0.5, 60), DEPOTS[2]),
        DEPOTS,
        "DECIDE: per D.region does not determine D.maxShipmentShare. D.region = 'East' has more than one "
        "value: 0.5, 0.6.",
        id="factor",
    ),
    pytest.param(
        "per (): sum(ship) <= D.capacity",
        (DEPOTS[0], ("D2", "East", "CA", 25, 12, 0.2, 0.6, 60), ("D3", "West", "US", 25, 30, 0.3, 0.7, 60)),
        DEPOTS,
        "DECIDE: per () does not determine D.capacity. The kept rows have more than one value: 20, 40.",
        id="per-empty",
    ),
]


@pytest.mark.error
@pytest.mark.parametrize("clause, holds, fails, message", PAIRS)
def test_a_per_key_must_determine_the_body(decidb_cli, clause, holds, fails, message):
    rows, _ = decidb_cli.execute(_query(clause, holds))
    assert len(rows) == len(SHIPMENTS)
    if message is not None:
        _stops(decidb_cli, _query(clause, fails), message)


@pytest.mark.error
def test_a_by_key_that_varies_inside_a_class(decidb_cli):
    """Deck p. 27: a depot whose shipments leave on two days does not have one
    `S.dispatchDay` group."""
    shipments = SHIPMENTS[:1] + ((2, "D1", 20, False, 2, 3, "active"),) + SHIPMENTS[2:]
    _stops(
        decidb_cli,
        _query("per D.depotID: sum(ship) by (S.dispatchDay) <= 500", shipments=shipments),
        "DECIDE: per D.depotID does not determine S.dispatchDay (in by (S.dispatchDay)). D.depotID = 'D1' has "
        "more than one value: 1, 2.",
    )


@pytest.mark.error
def test_a_decision_outside_reducers(decidb_cli):
    """`ship` is per row and D1 has two shipments; a decision keyed by the depot is
    determined, and one keyed by the region is not."""
    _stops(
        decidb_cli,
        _query("per D.depotID: ship <= 10"),
        "DECIDE: per D.depotID does not determine the decision ship (declared per row). D.depotID = 'D1' "
        "covers more than one ship.",
    )
    decide = "ship(INT), per D.depotID: r(INT), per D.region: g(INT)"
    rows, _ = decidb_cli.execute(_query("per D.depotID: r <= D.capacity AND r <= 100 AND g <= 1", decide=decide))
    assert len(rows) == len(SHIPMENTS)
    # g is keyed by region, coarser than the class: one g per depot.
    rows, _ = decidb_cli.execute(_query("per D.depotID: g <= D.capacity AND r <= 1 AND g <= 100", decide=decide))
    assert len(rows) == len(SHIPMENTS)
    _stops(
        decidb_cli,
        _query("per D.region: r <= 10 AND r <= 1 AND g <= 1", decide=decide),
        "DECIDE: per D.region does not determine the decision r (declared per D.depotID). D.region = 'East' "
        "covers more than one r.",
    )


@pytest.mark.error
def test_an_objective_term(decidb_cli, scope_solver):
    """A per-row decision outside the objective's reducers is legal only with a single
    instance over the kept rows (§4)."""
    rows, _ = decidb_cli.execute(_query("ship <= 7", objective="MAXIMIZE when S.shipmentID = 3: ship"))
    assert [ship for _, ship in rows][2] == 7
    _stops(
        decidb_cli,
        _query("ship <= 7", objective="MAXIMIZE when S.priority: ship"),
        "DECIDE: per () does not determine the decision ship (declared per row). The kept rows cover more "
        "than one ship.",
    )


@pytest.mark.error
def test_an_inner_per(decidb_cli):
    _stops(
        decidb_cli,
        _query("sum(per D.region: D.capacity * ship) <= 60"),
        "DECIDE: per D.region does not determine D.capacity (in sum(per D.region: D.capacity * ship)). "
        "D.region = 'East' has more than one value: 25, 40.",
    )


@pytest.mark.error
def test_only_kept_rows_are_compared(decidb_cli):
    """D2's capacity differs from D1's, but `when` keeps only D1 and D3 in their regions."""
    rows, _ = decidb_cli.execute(
        _query("when D.depotID <> 'D2' per D.region: sum(ship) by (D.region) <= D.capacity", EAST_SPLIT)
    )
    ships = [ship for _, ship in rows]
    assert sum(ships[:2]) == 25 and ships[2:] == [30, 15, 20]


@pytest.mark.error
@pytest.mark.correctness
def test_null_is_one_value(decidb_cli, scope_solver):
    """Two NULL capacities in one class are one value, so the key determines the bound; the NULL bound then skips
    East's instance (B1), which only its demands hold. A NULL beside a number is two values, and the error quotes
    NULL."""
    depots = (("D1", "East", "US", None, 8, 0.1, 0.5, 60), ("D2", "East", "CA", None, 12, 0.2, 0.6, 60), DEPOTS[2])
    sql = _query("per D.region: sum(ship) by (D.region) <= D.capacity", depots)
    model = ScopedModel(joined(depots=depots), [Decision("ship")])
    clauses = [
        Scoped((lambda r: {"ship": 1},), "<=", (lambda r: {CONSTANT: r["demand"]},)),
        Scoped(
            (Reduce("sum", lambda r: {"ship": 1}, by=("region",)),),
            "<=",
            (lambda r: {CONSTANT: r["capacity"]},),
            per=("region",),
        ),
    ]
    objective = ScopedObjective(ObjSense.MAXIMIZE, (Reduce("sum", lambda r: {"ship": 1}),))
    result = aligned(answer(decidb_cli, sql), model)
    assert model.judge_scoped(result, clauses, objective) == pytest.approx(
        model.solve_scoped(scope_solver, clauses, objective)
    )
    depots = (("D1", "East", "US", None, 8, 0.1, 0.5, 60), DEPOTS[1], DEPOTS[2])
    _stops(
        decidb_cli,
        _query("per D.region: sum(ship) by (D.region) <= D.capacity", depots),
        "DECIDE: per D.region does not determine D.capacity. D.region = 'East' has more than one value: " "40, NULL.",
    )


@pytest.mark.correctness
def test_without_per_the_clause_is_legal_and_takes_the_tightest_bound(decidb_cli, scope_solver):
    """S3: the same clause with no `per` is one constraint per row -- East's total under
    its tightest capacity, 25."""
    sql = _query("sum(ship) by (D.region) <= D.capacity", EAST_SPLIT)
    model = ScopedModel(joined(depots=EAST_SPLIT), [Decision("ship")])
    clauses = [
        Scoped((lambda r: {"ship": 1},), "<=", (lambda r: {CONSTANT: r["demand"]},)),
        Scoped((Reduce("sum", lambda r: {"ship": 1}, by=("region",)),), "<=", (lambda r: {CONSTANT: r["capacity"]},)),
    ]
    objective = ScopedObjective(ObjSense.MAXIMIZE, (Reduce("sum", lambda r: {"ship": 1}),))
    result = aligned(answer(decidb_cli, sql), model)
    assert (
        model.judge_scoped(result, clauses, objective)
        == pytest.approx(model.solve_scoped(scope_solver, clauses, objective))
        == 25 + 20
    )
    with pytest.raises(ScopeError, match="does not determine"):
        model.check([Scoped(clauses[1].lhs, "<=", clauses[1].rhs, per=("region",))])


@pytest.mark.error
def test_raised_under_diagnose(decidb_cli):
    """Nothing was solved, so there is nothing to diagnose (§10.1)."""
    _stops(
        decidb_cli,
        "DIAGNOSE " + _query("per D.region: sum(ship) by (D.region) <= D.capacity", EAST_SPLIT),
        "DECIDE: per D.region does not determine D.capacity.",
    )


@pytest.mark.correctness
@pytest.mark.parametrize(
    "clause",
    [
        pytest.param("when S.status = 'lost': ship <= 1", id="when-keeps-no-row"),
        pytest.param(
            "per D.depotID: sum(when S.status = 'hold': ship) by (D.depotID) <= 3",
            id="reducer-with-no-row",
        ),
    ],
)
def test_empty_sets_under_diagnose_are_no_instance(decidb_cli, clause):
    """A `when` that keeps no row, or a group a reducer reads no row of, has no instance: the query is feasible."""
    rows, columns = decidb_cli.execute("DIAGNOSE " + _query(clause))
    assert [row[columns.index("state")] for row in rows] == ["feasible"], rows


@pytest.mark.error
def test_messages_quote_the_sql_only(decidb_cli):
    """A subquery and a cast in the key's body are quoted as written."""
    _stops(
        decidb_cli,
        _query("per D.region: sum(ship) by (D.region) <= D.capacity + (SELECT 1)", EAST_SPLIT),
        "DECIDE: per D.region does not determine D.capacity + (SELECT 1). D.region = 'East' has more than "
        "one value: 26, 41.",
    )
    _stops(
        decidb_cli,
        _query("per D.region: sum(ship) by (D.region) <= CAST(D.capacity AS DOUBLE)", EAST_SPLIT),
        "DECIDE: per D.region does not determine CAST(D.capacity AS DOUBLE). D.region = 'East' has more than "
        "one value: 25.0, 40.0.",
    )
