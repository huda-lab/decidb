"""`when` and `per` inside a reducer: `agg([when c] [per K]: e) [by (Γ)]`
(language/guide.md §5.3).

Order: the rows `by` selects, then the inner `when`, then one value per class of the
inner `per` -- the first row the `when` keeps stands for its class, since the key
determines the body (a scope check) -- then the aggregate. The inner `when` filters its
own reducer only; `AVG` divides by the values it receives; `MIN`/`MAX` are unaffected by
an inner `per`. A reducer with no row for some group of the kept rows is an error naming
the group (D8, D18). The colon form `SUM(D: e)` is `sum(per D: e)` and takes the same checks (S4).

Optima are judged against `_scope_oracle.ScopedModel`; model shapes against
DECIDB_DUMP_MODEL.
"""

import pytest

from ._brute_force import judge, optimum
from ._output_helpers import assert_no_internal_leak
from ._scope_cases import DEPOTS, SHIPMENTS, aligned, answer, from_clause, joined, scope_solver  # noqa: F401
from ._scope_oracle import CONSTANT, GLOBAL, Decision, Reduce, Scoped, ScopedModel, ScopedObjective
from solver.types import ObjSense

SHIP_OPEN = [Decision("ship"), Decision("open", ("depotID",), lb=0, ub=1)]
#: A depot ships only when open: `ship <= S.demand * open`.
SHIP_IF_OPEN = Scoped((lambda r: {"ship": 1, "open": -r["demand"]},), "<=")
MAXIMIZE_SHIP = ScopedObjective(ObjSense.MAXIMIZE, (Reduce("sum", lambda r: {"ship": 1}),))


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
def test_inner_per_matches_the_colon_form(decidb_cli, tmp_path):
    """`sum(per D: e)` is `SUM(D: e)` (D1): the same model."""

    def query(clause):
        return f"""
            SELECT shipmentID, ship {from_clause()}
            DECIDE ship(INT), per D: open(BOOL)
            SUCH THAT ship <= S.demand * open AND {clause}
            MAXIMIZE SUM(ship)"""

    colon = decidb_cli.dump_model(query("SUM(D: capacity * open) <= 60"), tmp_path / "colon.dump")
    assert colon == decidb_cli.dump_model(query("sum(per D: D.capacity * open) <= 60"), tmp_path / "per.dump")


@pytest.mark.correctness
def test_the_colon_form_takes_a_by(decidb_cli, tmp_path):
    """`SUM(D: e) by (Γ)` reads its `by` group: the model of `sum(per D: e) by (Γ)`, which opens D2 and D3 alone."""

    def query(clause):
        return f"""
            SELECT shipmentID, open {from_clause()}
            DECIDE ship(INT), per D: open(BOOL)
            SUCH THAT ship <= S.demand * open AND {clause}
            MAXIMIZE SUM(ship)
            ORDER BY shipmentID"""

    colon = query("SUM(D: capacity * open) by (D.region) <= 50")
    dump = decidb_cli.dump_model(colon, tmp_path / "colon.dump")
    assert dump == decidb_cli.dump_model(
        query("sum(per D: D.capacity * open) by (D.region) <= 50"), tmp_path / "per.dump"
    )
    assert [opened for _, opened in decidb_cli.execute(colon)[0]] == [0, 0, 1, 1, 1]


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


@pytest.mark.error
def test_an_inner_key_must_determine_the_body(decidb_cli, scope_solver):
    """`per D.region` does not determine D.capacity (East has 25 and 40); `per D.depotID`
    does, and the same reducer runs."""
    result = decidb_cli.execute_raw(_open_query("sum(per D.region: D.capacity * open) <= 60"))
    assert (
        "DECIDE: per D.region does not determine D.capacity (in sum(per D.region: D.capacity * open)). "
        "D.region = 'East' has more than one value: 25, 40." in result.stderr
    ), result.stderr
    assert_no_internal_leak(result, "an inner key that does not determine the body")

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
def test_a_when_in_front_leaves_the_groups_it_keeps_no_row_of_out(decidb_cli, scope_solver):
    """D1 and D3 have no shipment on hold, so only D2's group is bounded."""
    rows, _ = decidb_cli.execute(_open_query("when S.status = 'hold' per D.depotID: sum(ship) by (D.depotID) <= 3"))
    assert [ship for _, ship, _ in rows] == [10, 20, 30, 3, 25]


def _per_depot(result) -> dict:
    """The one value `open` shows on every row of each depot."""
    values = {}
    for row in result:
        assert values.setdefault(row["depotID"], row["open"]) == row["open"], f"one depot, two values: {result}"
    return values


_DEPOT_DOMAINS = {depot[0]: range(6) for depot in DEPOTS}
_ROWS_PER_DEPOT = {depot[0]: sum(1 for s in SHIPMENTS if s[1] == depot[0]) for depot in DEPOTS}


@pytest.mark.correctness
@pytest.mark.parametrize(
    "cli_fixture, clause, objective, feasible, value, maximize",
    [
        pytest.param(
            cli,
            "per (): norm(D: open, 'inf') <= 1",
            "MAXIMIZE SUM(D: open)",
            lambda a: max(a.values()) <= 1,
            lambda a: sum(a.values()),
            True,
            id=f"inf-{cli}",
        )
        for cli in ("decidb_cli_gurobi", "decidb_cli_highs")
    ]
    + [
        pytest.param(
            cli,
            "norm(D: open, 0) <= 2",
            "MAXIMIZE SUM(D: open)",
            lambda a: sum(1 for v in a.values() if v) <= 2,
            lambda a: sum(a.values()),
            True,
            id=f"zero-{cli}",
        )
        for cli in ("decidb_cli_gurobi", "decidb_cli_highs")
    ]
    + [
        pytest.param(
            cli,
            "open <= 5",
            "MAXIMIZE SUM(D: 4 * open) - norm(D: 3 * open - 6, 1)",
            lambda a: True,
            lambda a: sum(4 * v - abs(3 * v - 6) for v in a.values()),
            True,
            id=f"objective-{cli}",
        )
        for cli in ("decidb_cli_gurobi", "decidb_cli_highs")
    ]
    + [
        # A square of a decision in a constraint needs Gurobi.
        pytest.param(
            "decidb_cli_gurobi",
            "when NOT S.priority: norm(D: open, 2) <= 2",
            "MAXIMIZE SUM(D: open)",
            lambda a: a["D1"] ** 2 + a["D2"] ** 2 <= 2,
            lambda a: sum(a.values()),
            True,
            id="under-a-when",
        )
    ],
)
def test_norm_with_a_relation_qualifier_reads_each_depot_once(
    request, cli_fixture, clause, objective, feasible, value, maximize
):
    """`norm(D: e, p)` is the norm over D's rows, each depot once, as `SUM(D: e)` is. D1 and
    D2 have two shipments each: counting their rows would change the optimum of orders 0, 1 and
    2; 'inf' reads a largest value, which a repeated row leaves alone. The `when` keeps D1 and
    D2 only, so D3 is free."""
    assert _ROWS_PER_DEPOT == {"D1": 2, "D2": 2, "D3": 1}
    cli = request.getfixturevalue(cli_fixture)
    result = answer(
        cli,
        f"""
        SELECT shipmentID, depotID, open {from_clause()}
        DECIDE per D: open(INT)
        SUCH THAT open <= 5 AND {clause}
        {objective}
        ORDER BY shipmentID""",
    )
    assert judge(_per_depot(result), feasible, value) == optimum(_DEPOT_DOMAINS, feasible, value, maximize)


@pytest.mark.error
@pytest.mark.error_binder
@pytest.mark.parametrize(
    "clause, message",
    [
        pytest.param(
            "norm(when S.priority: ship, 1) <= 5",
            "DECIDE: when and per inside a call need SUM, AVG, MIN or MAX; write the scope in front of the "
            "constraint or inside a reducer",
            id="scope-in-norm",
        ),
        pytest.param(
            "sum(when ship > 1: ship) <= 5",
            "DECIDE: ship is a decision; when, per and by may only use data",
            id="decision-in-inner-when",
        ),
        pytest.param(
            "SUM(D: ship) by (D.region) <= 100",
            "DECIDE: per D does not determine the decision ship (in sum(D: ship))",
            id="colon-form-takes-the-data-check",
        ),
    ],
)
def test_inner_scope_errors(decidb_cli, clause, message):
    result = decidb_cli.execute_raw(_open_query(clause))
    assert message in result.stderr, result.stderr
    assert_no_internal_leak(result, "an inner scope error")
