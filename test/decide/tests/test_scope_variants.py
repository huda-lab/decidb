"""Variant signatures (syntax_reference.md §7.1; D64): a `per` key splits the kept rows into
classes, and a class whose rows differ in what the body reads -- a tuple term (a bound, a
factor on a reducer), a decision outside reducers, a reducer's `by` key -- is one
instance per distinct combination of those values, each generated at its first row. An
inner `per` key, and the relations of a colon form, hand their reducer one value per
variant the same way (D65).

These were the determination errors; each case is now an answer, run on data where the
key fixes every value (one variant per class, today's model) and on data where it does
not, and judged by `_scope_oracle.ScopedModel`. The objective is produced once, so a
value with a second value over its kept rows is still an error (D66), naming it in the
user's SQL only (§10.1).
"""

import pytest

from ._output_helpers import assert_no_internal_leak
from ._scope_cases import (DEPOTS, SHIPMENTS, aligned, answer, from_clause, joined, scope_solver)  # noqa: F401
from ._scope_oracle import CONSTANT, Decision, Reduce, Scoped, ScopedModel, ScopedObjective
from solver.types import ObjSense

#: A second East depot whose capacity differs within its region.
EAST_SPLIT = (DEPOTS[0], ("D2", "East", "CA", 40, 12, 0.2, 0.6, 60), DEPOTS[2])
#: Every East depot shares one capacity.
EAST_EVEN = (DEPOTS[0], ("D2", "East", "CA", 25, 12, 0.2, 0.6, 60), DEPOTS[2])

SHIP = lambda r: {"ship": 1}  # noqa: E731
CAPACITY = lambda r: {CONSTANT: r["capacity"]}  # noqa: E731
DEMAND_BOUND = Scoped((SHIP,), "<=", (lambda r: {CONSTANT: r["demand"]},))
MAXIMIZE_SHIP = ScopedObjective(ObjSense.MAXIMIZE, (Reduce("sum", SHIP),))
PRODUCED_TWICE = "so the objective would be produced more than once; move it inside a reducer or fix it with when."


def _constant(value):
    return lambda r: {CONSTANT: value}


def _query(clause: str, depots=DEPOTS, shipments=SHIPMENTS, decide: str = "ship(INT)",
           objective: str = "MAXIMIZE SUM(ship)", select: str = "shipmentID, ship") -> str:
    return f"""
        SELECT {select} {from_clause(shipments, depots)}
        DECIDE {decide}
        SUCH THAT ship <= S.demand AND {clause}
        {objective}
        ORDER BY shipmentID"""


def _answers(cli, solver, clause_sql: str, clauses, depots=DEPOTS, shipments=SHIPMENTS, decide: str = "ship(INT)",
             decisions=(Decision("ship"),)) -> float:
    """DeciDB's optimum of `ship <= S.demand AND clause_sql`, judged against the oracle's
    reading of `clauses`."""
    select = ", ".join(("shipmentID",) + tuple(decision.name for decision in decisions))
    sql = _query(clause_sql, depots, shipments, decide, select=select)
    model = ScopedModel(joined(shipments, depots), list(decisions))
    statements = [DEMAND_BOUND, *clauses]
    expected = model.solve_scoped(solver, statements, MAXIMIZE_SHIP)
    rows = aligned(answer(cli, sql), model)
    assert model.judge_scoped(rows, statements, MAXIMIZE_SHIP) == pytest.approx(expected, abs=1e-6)
    return expected


def _stops(cli, sql: str, message: str) -> None:
    result = cli.execute_raw(sql)
    assert message in result.stderr, result.stderr
    assert "does not determine" not in result.stderr
    assert_no_internal_leak(result, "an objective produced more than once")


#: (clause, oracle statement, data where the key fixes it, data where it does not, the
#: optimum on each). The optimum is the hand count: East's shipments demand 75, West's 25.
VARIANTS = [
    pytest.param("per D.region: sum(ship) by (D.region) <= D.capacity",
                 Scoped((Reduce("sum", SHIP, by=("region",)),), "<=", (CAPACITY,), per=("region",)),
                 EAST_EVEN, EAST_SPLIT, (45, 45), id="bound"),
    pytest.param("per D.region: sum(ship) by (D.depotID) <= 50",
                 Scoped((Reduce("sum", SHIP, by=("depotID",)),), "<=", (_constant(50),), per=("region",)),
                 (DEPOTS[0], ("D2", "North", "CA", 40, 12, 0.2, 0.6, 60), DEPOTS[2]), DEPOTS, (100, 100),
                 id="by-key"),
    pytest.param("per D.region: sum(ship) by (D.region) <= D.maxShipmentShare * sum(S.demand) by (D.region)",
                 Scoped((Reduce("sum", SHIP, by=("region",)),), "<=",
                        (Reduce("sum", lambda r: {CONSTANT: r["demand"]}, by=("region",),
                                factor=lambda r: r["maxShipmentShare"]),), per=("region",)),
                 (DEPOTS[0], ("D2", "East", "CA", 40, 12, 0.2, 0.5, 60), DEPOTS[2]), DEPOTS, (37 + 17, 37 + 17),
                 id="factor"),
    pytest.param("per (): sum(ship) <= D.capacity",
                 Scoped((Reduce("sum", SHIP),), "<=", (CAPACITY,), per=()),
                 (DEPOTS[0], ("D2", "East", "CA", 25, 12, 0.2, 0.6, 60), ("D3", "West", "US", 25, 30, 0.3, 0.7, 60)),
                 DEPOTS, (25, 20), id="per-empty"),
]


@pytest.mark.correctness
@pytest.mark.parametrize("clause_sql, clause, fixed, varying, optima", VARIANTS)
def test_a_value_the_key_does_not_fix_is_a_variant(decidb_cli, scope_solver, clause_sql, clause, fixed, varying,
                                                     optima):
    """Deck pp. 34-39: where the key fixes the value the model is today's; where it does
    not, each value is an instance of its own."""
    assert _answers(decidb_cli, scope_solver, clause_sql, [clause], fixed) == optima[0]
    assert _answers(decidb_cli, scope_solver, clause_sql, [clause], varying) == optima[1]


@pytest.mark.correctness
def test_a_by_key_that_varies_inside_a_class(decidb_cli, scope_solver):
    """Deck p. 27: a depot whose shipments leave on two days reads two `S.dispatchDay`
    groups, one instance each."""
    shipments = SHIPMENTS[:1] + ((2, "D1", 20, False, 2, 3, "active"),) + SHIPMENTS[2:]
    clause = Scoped((Reduce("sum", SHIP, by=("dispatchDay",)),), "<=", (_constant(25),), per=("depotID",))
    sql = "per D.depotID: sum(ship) by (S.dispatchDay) <= 25"
    # Each day holds 25: S1 and S2 on day 1, S3 and S4 on day 2, S5 on day 3.
    assert _answers(decidb_cli, scope_solver, sql, [clause]) == 25 + 25 + 25
    # S2 leaves on day 2 with S3 and S4, so D1 is two instances: day 1 holds S1's 10.
    assert _answers(decidb_cli, scope_solver, sql, [clause], shipments=shipments) == 10 + 25 + 25


@pytest.mark.correctness
def test_a_decision_outside_reducers(decidb_cli, scope_solver):
    """`ship` is per row, so `per D.depotID: ship <= 10` is one instance per row; `r`,
    keyed by the depot, is one instance per depot under `per D.region`."""
    per_row = Scoped((SHIP,), "<=", (_constant(10),), per=("depotID",))
    assert _answers(decidb_cli, scope_solver, "per D.depotID: ship <= 10", [per_row]) == 10 * 5
    decide = "ship(INT), per D.depotID: r(INT), per D.region: g(INT)"
    decisions = (Decision("ship"), Decision("r", ("depotID",)), Decision("g", ("region",)))
    R = lambda r: {"r": 1}  # noqa: E731
    G = lambda r: {"g": 1}  # noqa: E731
    clauses = [Scoped((R,), "<=", (_constant(10),), per=("region",)), Scoped((R,), "<=", (_constant(1),)),
               Scoped((G,), "<=", (_constant(1),))]
    assert _answers(decidb_cli, scope_solver, "per D.region: r <= 10 AND r <= 1 AND g <= 1", clauses,
                    decide=decide, decisions=decisions) == 100


@pytest.mark.correctness
def test_an_inner_per(decidb_cli, scope_solver):
    """D65: `ship` is per row, so East's two capacities and its rows are each a member:
    25 * s1 + 25 * s2 + 40 * s3 + 40 * s4 + 20 * s5 <= 60, at most three shipments of S5."""
    clause = Scoped((Reduce("sum", lambda r: {"ship": r["capacity"]}, per=("region",)),), "<=", (_constant(60),))
    assert _answers(decidb_cli, scope_solver, "sum(per D.region: D.capacity * ship) <= 60", [clause]) == 3


@pytest.mark.correctness
def test_only_kept_rows_are_compared(decidb_cli):
    """D2's capacity differs from D1's, but `when` keeps only D1 and D3 in their regions."""
    rows, _ = decidb_cli.execute(_query("when D.depotID <> 'D2' per D.region: sum(ship) by (D.region) <= D.capacity",
                                        EAST_SPLIT))
    ships = [ship for _, ship in rows]
    assert sum(ships[:2]) == 25 and ships[2:] == [30, 15, 20]


@pytest.mark.error
def test_a_null_variant_is_the_null_error(decidb_cli):
    """NULL is one value of a field, and the variant holding it reads a NULL bound: the
    NULL error of §3, whether or not another variant shares the class."""
    query = "per D.region: sum(ship) by (D.region) <= D.capacity"
    both_null = (("D1", "East", "US", None, 8, 0.1, 0.5, 60), ("D2", "East", "CA", None, 12, 0.2, 0.6, 60), DEPOTS[2])
    one_null = (("D1", "East", "US", None, 8, 0.1, 0.5, 60), DEPOTS[1], DEPOTS[2])
    for depots in (both_null, one_null):
        result = decidb_cli.execute_raw(_query(query, depots))
        assert 'column "capacity" is NULL' in result.stderr, result.stderr
        assert_no_internal_leak(result, "a NULL bound")


@pytest.mark.correctness
def test_per_and_no_per_give_the_tightest_bound_alike(decidb_cli, scope_solver):
    """S3 and D64: with no `per` the clause is one instance per row, each under its own
    capacity; under `per D.region` East's two capacities are two instances. Both bound
    East's total by its tightest capacity, 25."""
    total = Reduce("sum", SHIP, by=("region",))
    for per, prefix in ((None, ""), (("region",), "per D.region: ")):
        clause = Scoped((total,), "<=", (CAPACITY,), per=per)
        assert _answers(decidb_cli, scope_solver, f"{prefix}sum(ship) by (D.region) <= D.capacity", [clause],
                        EAST_SPLIT) == 25 + 20


# ---------------------------------------------------------------------------
# The objective is produced once (D66)
# ---------------------------------------------------------------------------


@pytest.mark.error
def test_an_objective_decision(decidb_cli):
    """A per-row decision outside the objective's reducers is legal only with a single
    instance over the kept rows (§4)."""
    rows, _ = decidb_cli.execute(_query("ship <= 7", objective="MAXIMIZE when S.shipmentID = 3: ship"))
    assert [ship for _, ship in rows][2] == 7
    _stops(decidb_cli, _query("ship <= 7", objective="MAXIMIZE when S.priority: ship"),
           "DECIDE objective: the decision 'ship' (declared per row) has more than one instance over the kept rows, "
           + PRODUCED_TWICE)


@pytest.mark.error
def test_an_objective_value_and_by_key(decidb_cli):
    """A factor and a `by` key are named as written, with the first two values they take."""
    decide = "ship(INT), per (): s(INT)"
    _stops(decidb_cli, _query("s <= 1", decide=decide, objective="MAXIMIZE sum(ship) - D.capacity * s"),
           "DECIDE objective: 'D.capacity' takes more than one value over the kept rows (25, 40), " + PRODUCED_TWICE)
    _stops(decidb_cli, _query("s <= 1", decide=decide, objective="MAXIMIZE sum(ship) by (D.region)"),
           "DECIDE objective: 'D.region' in by (D.region) takes more than one value over the kept rows ('East', "
           "'West'), " + PRODUCED_TWICE)


@pytest.mark.error
def test_objective_messages_quote_the_sql_only(decidb_cli):
    """A subquery and a cast in the value are quoted as written."""
    decide = "ship(INT), per (): s(INT)"
    _stops(decidb_cli, _query("s <= 1", decide=decide, objective="MAXIMIZE sum(ship) - (D.capacity + (SELECT 1)) * s"),
           "DECIDE objective: 'D.capacity + (SELECT 1)' takes more than one value over the kept rows (26, 41), "
           + PRODUCED_TWICE)
    _stops(decidb_cli,
           _query("s <= 1", decide=decide, objective="MAXIMIZE sum(ship) - CAST(D.capacity AS DOUBLE) * s"),
           "DECIDE objective: 'CAST(D.capacity AS DOUBLE)' takes more than one value over the kept rows (25.0, "
           "40.0), " + PRODUCED_TWICE)


@pytest.mark.error
def test_the_objective_error_is_raised_under_diagnose(decidb_cli):
    """Nothing was solved, so there is nothing to diagnose (§10.1)."""
    _stops(decidb_cli, "DIAGNOSE " + _query("ship <= 7", objective="MAXIMIZE when S.priority: ship"),
           "DECIDE objective: the decision 'ship' (declared per row) has more than one instance")


@pytest.mark.error
@pytest.mark.parametrize("clause, message", [
    pytest.param("when S.status = 'lost': ship <= 1", "DECIDE: when S.status = 'lost' matches no rows.",
                 id="when-keeps-no-row"),
    pytest.param("per D.depotID: sum(when S.status = 'hold': ship) by (D.depotID) <= 3",
                 "DECIDE: sum(when S.status = 'hold': ship) has no rows for D.depotID = 'D1'.",
                 id="reducer-with-no-row"),
])
def test_empty_sets_raised_under_diagnose(decidb_cli, clause, message):
    """D4 and D8 are data errors too: DIAGNOSE raises them rather than diagnosing."""
    result = decidb_cli.execute_raw("DIAGNOSE " + _query(clause))
    assert message in result.stderr, result.stderr
    assert_no_internal_leak(result, "an empty set under DIAGNOSE")
