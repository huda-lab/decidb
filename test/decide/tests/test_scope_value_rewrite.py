"""The value rewrite (language/guide.md §6.4; pipeline/05_optimizer/done.md, "The
value rewrite").

A scoped constraint whose decision side mixes a reducer with something a grouped row
cannot hold -- a per-row or keyed decision beside it, a factor that varies by row, a second
`by` key, a reducer inside another expression -- is built from a helper decision per
reducer: one instance per `by` group, defined once (an equality row for SUM/AVG, an
extremum link for MIN/MAX) and used by one row per class. Deck examples 1, 5 and 8 are this
shape. The helper is whole-numbered when the reducer is, so `<`, `>` and `<>` keep working;
it may be negative; a product of decisions inside such a reducer is refused (§10.2).

Every optimum is judged against `_scope_oracle.ScopedModel`, which follows the ANR
definition literally; model sizes are read from DECIDB_DUMP_MODEL.
"""

import itertools
import re

import pytest

from ._brute_force import judge_rows
from ._diagnose_relation import run as run_diagnose
from ._output_helpers import assert_no_internal_leak
from ._scope_cases import (
    DEPOTS,
    SHIPMENTS,
    aligned,
    answer,
    dump_counts,
    error_of,
    from_clause,
    joined,  # noqa: F401
    scope_solver,
    table,
    table_rows,
)
from ._scope_oracle import CONSTANT, GLOBAL, Decision, Reduce, Scoped, ScopedModel, ScopedObjective
from solver.types import ObjSense, VarType

SHIP = [Decision("ship")]
SHIP_BOUND = Scoped((lambda r: {"ship": 1},), "<=", (lambda r: {CONSTANT: r["demand"]},))
MAXIMIZE_SHIP = ScopedObjective(ObjSense.MAXIMIZE, (Reduce("sum", lambda r: {"ship": 1}),))
MINIMIZE_SHIP = ScopedObjective(ObjSense.MINIMIZE, (Reduce("sum", lambda r: {"ship": 1}),))
#: The decision as a tuple term of a clause.
X = lambda r: {"ship": 1}  # noqa: E731
PRIORITY = lambda r: r["priority"]  # noqa: E731


def _const(value):
    return lambda r: {CONSTANT: value}


def _ship_query(
    clause: str,
    sense: str = "MAXIMIZE",
    shipments=SHIPMENTS,
    depots=DEPOTS,
    decide: str = "ship(INT)",
    columns: str = "shipmentID, ship",
) -> str:
    return f"""
        SELECT {columns} {from_clause(shipments, depots)}
        DECIDE {decide}
        SUCH THAT ship <= S.demand AND {clause}
        {sense} SUM(ship)
        ORDER BY shipmentID"""


def _judged(cli, solver, sql, model, clauses, objective=MAXIMIZE_SHIP):
    result = aligned(answer(cli, sql), model)
    assert model.judge_scoped(result, clauses, objective) == pytest.approx(
        model.solve_scoped(solver, clauses, objective), abs=1e-4
    )
    return [row["ship"] for row in result]


def _judge_apart(rows, group, holds):
    """Judge `rows` one group at a time (`group` reads a joined row) against every assignment
    of the group's ship values, each from 0 to its demand. The clauses bind a group's own
    total and the objective adds the groups up, so the optimum is the sum of the groups'
    optima; `holds(g, ships)` is the clause over group g's values."""
    data = joined()
    for g in dict.fromkeys(group(row) for row in data):
        picked = [i for i, row in enumerate(data) if group(row) == g]
        domains = {f"ship{k}": range(data[i]["demand"] + 1) for k, i in enumerate(picked)}
        judge_rows([rows[i] for i in picked], domains, lambda a: holds(g, list(a.values())), lambda a: sum(a.values()))


def _ship_reduce(agg="sum", by=(), when=None, per=None, factor=1.0):
    scale = factor if callable(factor) else (lambda value: lambda r: value)(factor)
    return Reduce(agg, X, when=when, per=per, by=by, factor=scale)


# ---------------------------------------------------------------------------
# The shapes that take the rewrite
# ---------------------------------------------------------------------------


@pytest.mark.correctness
def test_a_decision_beside_a_reducer(decidb_cli, scope_solver, tmp_path):
    """Deck example 1: no shipment above a fifth of the network total. One helper for the
    total, one definition row, one row per shipment -- `per row:` and `by ()` alike."""
    written = _ship_query("per row: ship <= 0.20 * sum(ship)")
    dump = decidb_cli.dump_model(written, tmp_path / "written.dump")
    assert dump == decidb_cli.dump_model(_ship_query("ship <= 0.20 * sum(ship) by ()"), tmp_path / "by.dump")
    assert dump_counts(dump) == {"num_vars": len(SHIPMENTS) + 1, "num_rows": len(SHIPMENTS) + 1}

    model = ScopedModel(joined(), SHIP)
    clauses = [SHIP_BOUND, Scoped((X,), "<=", (_ship_reduce(factor=0.20),))]
    # A total of 50 lets each shipment take 10; any larger total asks for more than a
    # fifth of itself from some shipment.
    assert _judged(decidb_cli, scope_solver, written, model, clauses) == [10, 10, 10, 10, 10]


@pytest.mark.correctness
def test_two_clauses_share_one_helper(decidb_cli, scope_solver, tmp_path):
    """Two clauses holding the same reducer read one helper with one definition row: one row
    per shipment for each clause and one more for the definition."""
    sql = _ship_query("ship <= 0.25 * sum(ship) AND ship >= 0.1 * sum(ship)")
    assert dump_counts(decidb_cli.dump_model(sql, tmp_path / "model.dump")) == {
        "num_vars": len(SHIPMENTS) + 1,
        "num_rows": 2 * len(SHIPMENTS) + 1,
    }
    model = ScopedModel(joined(), SHIP)
    clauses = [
        SHIP_BOUND,
        Scoped((X,), "<=", (_ship_reduce(factor=0.25),)),
        Scoped((X,), ">=", (_ship_reduce(factor=0.1),)),
    ]
    _judged(decidb_cli, scope_solver, sql, model, clauses)


@pytest.mark.correctness
def test_a_row_varying_factor_on_a_reducer(decidb_cli, scope_solver, tmp_path):
    """Deck example 5: each shipment within its depot's share of the depot's total. The
    factor is read on the shipment's row; the helper is one per depot."""
    sql = _ship_query("ship <= D.maxShipmentShare * sum(ship) by (D.depotID)")
    assert dump_counts(decidb_cli.dump_model(sql, tmp_path / "model.dump")) == {
        "num_vars": len(SHIPMENTS) + len(DEPOTS),
        "num_rows": len(SHIPMENTS) + len(DEPOTS),
    }
    model = ScopedModel(joined(), SHIP)
    clauses = [SHIP_BOUND, Scoped((X,), "<=", (_ship_reduce(by=("depotID",), factor=lambda r: r["maxShipmentShare"]),))]
    # D1 (share 0.5): both shipments equal, 10 each. D2 (0.6): 22 and 15. D3 (0.7): a
    # lone shipment cannot be 70% of itself.
    assert _judged(decidb_cli, scope_solver, sql, model, clauses) == [10, 10, 22, 15, 0]


@pytest.mark.correctness
def test_an_expression_key(decidb_cli, scope_solver):
    """Deck example 8: a shipment against those arriving at its depot on the same day. On
    the deck's data every shipment arrives alone, so a quarter of its group is a quarter
    of itself; with S2 arriving with S1 and a 60% share the pair shares the group."""
    key = "D.depotID, S.dispatchDay + S.transitDays"
    arrival = lambda r: (r["depotID"], r["dispatchDay"] + r["transitDays"])  # noqa: E731
    model = ScopedModel(joined(), SHIP)
    clauses = [SHIP_BOUND, Scoped((X,), "<=", (_ship_reduce(by=arrival, factor=0.25),), per=("shipmentID",))]
    sql = _ship_query(f"per S.shipmentID: ship <= 0.25 * sum(ship) by ({key})")
    assert _judged(decidb_cli, scope_solver, sql, model, clauses) == [0, 0, 0, 0, 0]

    shipments = (SHIPMENTS[0], (2, "D1", 20, False, 1, 2, "active")) + SHIPMENTS[2:]
    model = ScopedModel(joined(shipments), SHIP)
    clauses = [SHIP_BOUND, Scoped((X,), "<=", (_ship_reduce(by=arrival, factor=0.6),), per=("shipmentID",))]
    sql = _ship_query(f"per S.shipmentID: ship <= 0.6 * sum(ship) by ({key})", shipments=shipments)
    assert _judged(decidb_cli, scope_solver, sql, model, clauses) == [10, 15, 0, 0, 0]


@pytest.mark.correctness
def test_a_keyed_decision_beside_a_reducer(decidb_cli, scope_solver):
    """The facility-location link: a depot ships only when open, and two may open. The
    helper holds each depot's total; `open` is the depot's own decision."""
    sql = f"""
        SELECT shipmentID, ship, open {from_clause()}
        DECIDE ship(INT), per D.depotID: open(BOOL)
        SUCH THAT ship <= S.demand
              AND per D.depotID: sum(ship) by (D.depotID) <= D.capacity * open
              AND per (): sum(per D.depotID: open) <= 2
        MAXIMIZE SUM(ship)
        ORDER BY shipmentID"""
    model = ScopedModel(joined(), [Decision("ship"), Decision("open", ("depotID",), VarType.BINARY, ub=1.0)])
    clauses = [
        SHIP_BOUND,
        Scoped((_ship_reduce(by=("depotID",)), lambda r: {"open": -r["capacity"]}), "<=", (), per=("depotID",)),
        Scoped((Reduce("sum", lambda r: {"open": 1}, per=("depotID",)),), "<=", (_const(2),), per=GLOBAL),
    ]
    result = aligned(answer(decidb_cli, sql), model)
    # D1 (capacity 25) and D2 (40) open: 65. D2 and D3 would give 60.
    assert (
        model.judge_scoped(result, clauses, MAXIMIZE_SHIP)
        == pytest.approx(model.solve_scoped(scope_solver, clauses, MAXIMIZE_SHIP))
        == 65
    )
    assert [row["open"] for row in result] == [True, True, True, True, False]


@pytest.mark.correctness
@pytest.mark.parametrize(
    "clause, reducers, bound, helpers",
    [
        pytest.param(
            "sum(ship) by (D.depotID) + sum(ship) by (D.region) <= 50",
            (("depotID",), ("region",)),
            50,
            len(DEPOTS) + 2,
            id="two-keys",
        ),
        pytest.param(
            "sum(ship) by (D.depotID) + sum(ship) by (D.region) + sum(ship) <= 90",
            (("depotID",), ("region",), ()),
            90,
            len(DEPOTS) + 2 + 1,
            id="three-keys",
        ),
    ],
)
def test_reducers_with_different_by_keys(decidb_cli, scope_solver, tmp_path, clause, reducers, bound, helpers):
    """Each reducer reads its own group; the row adds the helpers. One helper per group
    of every key, one definition row each."""
    sql = _ship_query(clause)
    assert dump_counts(decidb_cli.dump_model(sql, tmp_path / "model.dump")) == {
        "num_vars": len(SHIPMENTS) + helpers,
        "num_rows": len(SHIPMENTS) + helpers,
    }
    model = ScopedModel(joined(), SHIP)
    clauses = [SHIP_BOUND, Scoped(tuple(_ship_reduce(by=by) for by in reducers), "<=", (_const(bound),))]
    _judged(decidb_cli, scope_solver, sql, model, clauses)


# ---------------------------------------------------------------------------
# MIN and MAX helpers: the side that pins them follows the clause
# ---------------------------------------------------------------------------

S_GLOBAL = [Decision("ship"), Decision("s", GLOBAL)]
S_LE_3 = Scoped((lambda r: {"s": 1},), "<=", (_const(3),))

MINMAX_CASES = [
    pytest.param(
        "ship(INT)",
        "ship <= 0.5 * max(ship) by (D.region) + 8",
        SHIP,
        [Scoped((X,), "<=", (_ship_reduce("max", by=("region",), factor=0.5), _const(8)))],
        "MAXIMIZE",
        [10, 16, 16, 15, 16],
        id="max-pushed-up-closing",
    ),
    pytest.param(
        "ship(INT)",
        "ship <= min(ship) by (D.region) + 5",
        SHIP,
        [Scoped((X,), "<=", (_ship_reduce("min", by=("region",)), _const(5)))],
        "MAXIMIZE",
        [10, 15, 15, 15, 25],
        id="min-pushed-up-envelope",
    ),
    pytest.param(
        "ship(INT)",
        "ship >= 1 AND ship >= 2 * min(ship) by (D.region) - 1",
        SHIP,
        [
            Scoped((X,), ">=", (_const(1),)),
            Scoped((X,), ">=", (_ship_reduce("min", by=("region",), factor=2), _const(-1))),
        ],
        "MINIMIZE",
        [1, 1, 1, 1, 1],
        id="min-pushed-down-closing",
    ),
    pytest.param(
        "ship(INT)",
        "max(ship) by (D.region) - min(ship) by (D.region) <= 5",
        SHIP,
        [
            Scoped(
                (_ship_reduce("max", by=("region",)), _ship_reduce("min", by=("region",), factor=-1)),
                "<=",
                (_const(5),),
            )
        ],
        "MAXIMIZE",
        [10, 15, 15, 15, 25],
        id="range-with-by",
    ),
    pytest.param(
        "ship(INT)",
        "when S.priority: sum(ship) by (D.region) + max(ship) by (D.region) <= 30",
        SHIP,
        [
            Scoped(
                (_ship_reduce(by=("region",)), _ship_reduce("max", by=("region",))), "<=", (_const(30),), when=PRIORITY
            )
        ],
        "MAXIMIZE",
        [10, 20, 10, 15, 15],
        id="composed-with-by",
    ),
    pytest.param(
        "ship(INT)",
        "when S.priority: sum(ship) + max(ship) by (D.region) <= 30",
        SHIP,
        [Scoped((_ship_reduce(), _ship_reduce("max", by=("region",))), "<=", (_const(30),), when=PRIORITY)],
        "MAXIMIZE",
        None,
        id="composed-with-two-keys",
    ),
    pytest.param(
        "ship(INT)",
        "ship = max(ship) by (D.region)",
        SHIP,
        [Scoped((X,), "=", (_ship_reduce("max", by=("region",)),))],
        "MAXIMIZE",
        [10, 10, 10, 10, 25],
        id="equality-pins-both-sides",
    ),
    pytest.param(
        "ship(INT)",
        "ship <= max(when S.priority: ship) by (D.region)",
        SHIP,
        [Scoped((X,), "<=", (_ship_reduce("max", by=("region",), when=PRIORITY),))],
        "MAXIMIZE",
        [10, 20, 30, 15, 25],
        id="inner-when",
    ),
    pytest.param(
        "ship(INT)",
        "(SELECT 2) * max(ship) by (D.region) <= 40",
        SHIP,
        [Scoped((_ship_reduce("max", by=("region",), factor=2),), "<=", (_const(40),))],
        "MAXIMIZE",
        [10, 20, 20, 15, 20],
        id="unknown-sign-factor-with-by",
    ),
    pytest.param(
        "ship(INT)",
        "max(ship) - min(ship) <= max(S.demand) by (D.region)",
        SHIP,
        [
            Scoped(
                (_ship_reduce("max"), _ship_reduce("min", factor=-1)),
                "<=",
                (Reduce("max", lambda r: {CONSTANT: r["demand"]}, by=("region",)),),
            )
        ],
        "MAXIMIZE",
        [10, 20, 30, 15, 25],
        id="composed-bound-with-by",
    ),
    pytest.param(
        "ship(INT)",
        "max(ship) - min(ship) <= max(per D.depotID: D.capacity)",
        SHIP,
        [
            Scoped(
                (_ship_reduce("max"), _ship_reduce("min", factor=-1)),
                "<=",
                (Reduce("max", lambda r: {CONSTANT: r["capacity"]}, per=("depotID",)),),
            )
        ],
        "MAXIMIZE",
        [10, 20, 30, 15, 25],
        id="composed-bound-with-inner-per",
    ),
    pytest.param(
        "ship(INT), per (): s(INT)",
        "s <= 3 AND when S.priority: max(ship) by (D.region) + s <= 12",
        S_GLOBAL,
        [S_LE_3, Scoped((_ship_reduce("max", by=("region",)), lambda r: {"s": 1}), "<=", (_const(12),), when=PRIORITY)],
        "MAXIMIZE",
        [10, 20, 12, 15, 12],
        id="decision-beside-max-with-by",
    ),
    pytest.param(
        "ship(INT), per (): s(INT)",
        "s <= 3 AND when S.priority: max(ship) + s <= 12",
        S_GLOBAL,
        [S_LE_3, Scoped((_ship_reduce("max"), lambda r: {"s": 1}), "<=", (_const(12),), when=PRIORITY)],
        "MAXIMIZE",
        [10, 20, 12, 15, 12],
        id="decision-beside-max",
    ),
]


@pytest.mark.correctness
@pytest.mark.min_max
@pytest.mark.parametrize("decide, clause, decisions, clauses, sense, ships", MINMAX_CASES)
def test_min_max_helpers(decidb_cli, scope_solver, decide, clause, decisions, clauses, sense, ships):
    """A MIN/MAX helper is pinned on the side the clause would let it escape through
    (both for `=`, `<>` and a factor of unknown sign), and the answer is the oracle's."""
    columns = "shipmentID, " + ", ".join(decision.name for decision in decisions)
    sql = _ship_query(clause, sense, decide=decide, columns=columns)
    model = ScopedModel(joined(), decisions)
    objective = MAXIMIZE_SHIP if sense == "MAXIMIZE" else MINIMIZE_SHIP
    result = _judged(decidb_cli, scope_solver, sql, model, [SHIP_BOUND] + clauses, objective)
    if ships is not None:
        assert result == ships


@pytest.mark.correctness
@pytest.mark.min_max
def test_an_unbounded_member_under_a_closing_link(decidb_cli_highs, decidb_cli_gurobi):
    """`ship <= 0.5 * max(ship) by (D.region) + 8` with no bound on `ship`: the closing side
    needs a Big-M, which the lowering refuses naming the column to bound; the native
    arm states the extremum and answers (16 each, the clause's own fixed point)."""
    sql = f"""
        SELECT shipmentID, ship {from_clause()}
        DECIDE ship(INT)
        SUCH THAT ship <= 0.5 * max(ship) by (D.region) + 8
        MAXIMIZE SUM(ship)
        ORDER BY shipmentID"""
    refused = decidb_cli_highs.execute_raw(sql)
    assert "requires a finite bound on 'ship'" in refused.stderr, refused.stderr
    assert_no_internal_leak(refused, "a MIN/MAX helper over an unbounded member")
    rows, _ = decidb_cli_gurobi.execute(sql)
    assert [ship for _, ship in rows] == [16] * 5


# ---------------------------------------------------------------------------
# Strict bounds, <>, AVG, the reducer's own when and per
# ---------------------------------------------------------------------------


@pytest.mark.correctness
def test_strict_and_not_equal_over_whole_numbered_helpers(decidb_cli, scope_solver):
    """The helper of a SUM over INT is INT: `<` steps the row, `<>` excludes one value."""
    model = ScopedModel(joined(), SHIP)
    east = lambda r: r["region"] == "East"  # noqa: E731
    sql = _ship_query("when D.region = 'East': 2 * ship < sum(ship) by (D.region)")
    clauses = [SHIP_BOUND, Scoped((lambda r: {"ship": 2},), "<", (_ship_reduce(by=("region",)),), when=east)]
    _judged(decidb_cli, scope_solver, sql, model, clauses)

    # D1: `s1 <> s1 + s2 - 10` is `s2 <> 10` and `s2 <> s1 + s2 - 10` is `s1 <> 10`, so the
    # 10-unit shipment gives up one unit; the other depots lose nothing.
    sql = _ship_query("ship <> sum(ship) by (D.depotID) - 10")
    clauses = [SHIP_BOUND, Scoped((X,), "<>", (_ship_reduce(by=("depotID",)), _const(-10)))]
    assert _judged(decidb_cli, scope_solver, sql, model, clauses) == [9, 20, 30, 15, 25]


@pytest.mark.correctness
def test_an_avg_helper(decidb_cli, scope_solver):
    """`ship <= 1.5 * avg(ship) by (D.region)`: East's four shipments average their total
    over four, so a shipment may take 3/8 of the total -- 27 for the 30-unit one."""
    sql = _ship_query("ship <= 1.5 * avg(ship) by (D.region)")
    model = ScopedModel(joined(), SHIP)
    clauses = [SHIP_BOUND, Scoped((X,), "<=", (_ship_reduce("avg", by=("region",), factor=1.5),))]
    assert _judged(decidb_cli, scope_solver, sql, model, clauses) == [10, 20, 27, 15, 25]


@pytest.mark.correctness
def test_a_when_in_front_keeps_rows_for_the_helper_too(decidb_cli, scope_solver):
    """`when S.priority:` the helper sums the priority rows alone, and only they are
    bounded: the lone West priority shipment cannot be half of itself."""
    sql = _ship_query("when S.priority: ship <= 0.5 * sum(ship) by (D.region)")
    model = ScopedModel(joined(), SHIP)
    clauses = [SHIP_BOUND, Scoped((X,), "<=", (_ship_reduce(by=("region",), factor=0.5),), when=PRIORITY)]
    assert _judged(decidb_cli, scope_solver, sql, model, clauses) == [10, 20, 10, 15, 0]


@pytest.mark.correctness
def test_a_helper_over_a_reducer_with_its_own_per(decidb_cli, scope_solver):
    """`sum(per D.depotID: open * D.capacity)` counts each depot once; with one depot open,
    every shipment is bounded by half that depot's capacity -- D2's 40 is the best."""
    sql = f"""
        SELECT shipmentID, ship, open {from_clause()}
        DECIDE ship(INT), per D.depotID: open(BOOL)
        SUCH THAT ship <= S.demand
              AND ship <= 0.5 * sum(per D.depotID: open * D.capacity)
              AND per (): sum(per D.depotID: open) <= 1
        MAXIMIZE SUM(ship)
        ORDER BY shipmentID"""
    model = ScopedModel(joined(), [Decision("ship"), Decision("open", ("depotID",), VarType.BINARY, ub=1.0)])
    clauses = [
        SHIP_BOUND,
        Scoped((X,), "<=", (Reduce("sum", lambda r: {"open": r["capacity"]}, per=("depotID",), factor=lambda r: 0.5),)),
        Scoped((Reduce("sum", lambda r: {"open": 1}, per=("depotID",)),), "<=", (_const(1),), per=GLOBAL),
    ]
    result = aligned(answer(decidb_cli, sql), model)
    assert (
        model.judge_scoped(result, clauses, MAXIMIZE_SHIP)
        == pytest.approx(model.solve_scoped(scope_solver, clauses, MAXIMIZE_SHIP))
        == 85
    )
    assert [row["open"] for row in result] == [False, False, True, True, False]


@pytest.mark.correctness
def test_a_null_helper_key_is_no_group(decidb_cli, scope_solver):
    """Rows without a region are in no group: their bound is NULL, so they take their
    demand; the lone 'b' row cannot be 60% of itself."""
    rows = ((1, "a", 10), (2, "a", 20), (3, "b", 30), (4, None, 15), (5, None, 25))
    columns = ("id", "region", "demand")
    sql = f"""
        SELECT id, ship {table('S', columns, rows)}
        DECIDE ship(INT)
        SUCH THAT ship <= S.demand AND ship <= 0.6 * sum(ship) by (S.region)
        MAXIMIZE SUM(ship)
        ORDER BY id"""
    model = ScopedModel(table_rows(columns, rows), SHIP)
    clauses = [SHIP_BOUND, Scoped((X,), "<=", (_ship_reduce(by=("region",), factor=0.6),))]
    result = aligned(answer(decidb_cli, sql), model, "id")
    assert model.judge_scoped(result, clauses, MAXIMIZE_SHIP) == pytest.approx(
        model.solve_scoped(scope_solver, clauses, MAXIMIZE_SHIP)
    )
    assert [row["ship"] for row in result] == [10, 15, 0, 15, 25]


@pytest.mark.correctness
def test_a_query_wide_decision_beside_a_reducer_in_a_product_row(decidb_cli):
    """`sum(ship) + b * s <= 50` under `when S.priority:`: the product beside the reducer
    is a term of each row (McCormick), the helper holds the priority total. The scope
    oracle states no product, so every assignment is enumerated: the priority shipments
    with `b` and `s`, and apart from them the shipments the clause leaves alone."""
    sql = _ship_query(
        "s <= 3 AND when S.priority: sum(ship) + b * s <= 50",
        decide="ship(INT), per (): b(BOOL), per (): s(INT)",
        columns="shipmentID, ship, b, s",
    )
    rows = answer(decidb_cli, sql)
    priority = [i for i, row in enumerate(joined()) if row["priority"]]
    others = [i for i in range(len(rows)) if i not in priority]
    domains = {f"ship{k}": range(joined()[i]["demand"] + 1) for k, i in enumerate(priority)}
    judge_rows(
        [rows[i] for i in priority],
        dict(domains, b=range(2), s=range(4)),
        lambda a: sum(a[name] for name in domains) + a["b"] * a["s"] <= 50,
        lambda a: sum(a[name] for name in domains),
    )
    free = {f"ship{k}": range(joined()[i]["demand"] + 1) for k, i in enumerate(others)}
    judge_rows([rows[i] for i in others], free, lambda a: True, lambda a: sum(a.values()))


@pytest.mark.correctness
def test_a_row_varying_factor_on_a_decision_beside_a_reducer(decidb_cli, scope_solver):
    """`sum(ship) + D.capacity * s <= 100`: the capacity multiplies the query-wide `s` on
    each priority row; `s` stays 0 and the priority shipments ship in full."""
    sql = _ship_query(
        "s <= 3 AND when S.priority: sum(ship) + D.capacity * s <= 100",
        decide="ship(INT), per (): s(INT)",
        columns="shipmentID, ship, s",
    )
    model = ScopedModel(joined(), S_GLOBAL)
    clauses = [
        SHIP_BOUND,
        S_LE_3,
        Scoped((_ship_reduce(), lambda r: {"s": r["capacity"]}), "<=", (_const(100),), when=PRIORITY),
    ]
    assert _judged(decidb_cli, scope_solver, sql, model, clauses) == [10, 20, 30, 15, 25]


# ---------------------------------------------------------------------------
# A reducer inside another expression
# ---------------------------------------------------------------------------


@pytest.mark.correctness
def test_a_reducer_inside_abs(decidb_cli):
    """`abs(sum(ship) by (D.region) - 20) <= 5`: each region's total within 5 of 20. The
    scope oracle states no `abs`, so every assignment is enumerated, a region at a time;
    with `- 40` no assignment of some region is feasible, and the query is infeasible."""
    rows = answer(decidb_cli, _ship_query("abs(sum(ship) by (D.region) - 20) <= 5"))
    _judge_apart(rows, lambda row: row["region"], lambda region, ships: abs(sum(ships) - 20) <= 5)
    result = decidb_cli.execute_raw(_ship_query("abs(sum(ship) by (D.region) - 40) <= 5"))
    assert "infeasible" in result.stderr, result.stderr
    demands = {}
    for row in joined():
        demands.setdefault(row["region"], []).append(range(row["demand"] + 1))
    assert not all(
        any(abs(sum(ships) - 40) <= 5 for ships in itertools.product(*ranges)) for ranges in demands.values()
    )


@pytest.mark.correctness
@pytest.mark.quadratic
@pytest.mark.parametrize(
    "clause, holds",
    [
        pytest.param(
            "power(sum(ship) by (D.region), 2) <= 900", lambda g, ships: sum(ships) ** 2 <= 900, id="square-of-a-helper"
        ),
        pytest.param(
            "power(sum(ship) by (D.region), 2) <= 50",
            lambda g, ships: sum(ships) ** 2 <= 50,
            id="square-of-a-helper-small",
        ),
        pytest.param(
            "sum(ship) by (D.region) * sum(ship) by (D.region) <= 50",
            lambda g, ships: sum(ships) * sum(ships) <= 50,
            id="product-of-helpers",
        ),
    ],
)
def test_a_reducer_inside_a_square(decidb_cli_gurobi, clause, holds):
    """The helper enters a quadratic row like any decision (Gurobi). The scope oracle is
    linear, so every assignment is enumerated, a region at a time."""
    rows = answer(decidb_cli_gurobi, _ship_query(clause))
    _judge_apart(rows, lambda row: row["region"], holds)


@pytest.mark.correctness
@pytest.mark.quadratic
def test_a_square_of_a_helper_with_an_inner_when(decidb_cli_gurobi):
    """`power(sum(when S.priority: ship), 2) <= 50` bounds the priority shipments only. The
    scope oracle is linear, so every assignment is enumerated, priority and other shipments
    apart."""
    rows = answer(decidb_cli_gurobi, _ship_query("power(sum(when S.priority: ship), 2) <= 50"))
    _judge_apart(rows, lambda row: row["priority"], lambda priority, ships: not priority or sum(ships) ** 2 <= 50)


@pytest.mark.correctness
def test_abs_of_a_helper_bounds_the_total(decidb_cli):
    """`abs(sum(ship) by (D.region)) <= 5`. The scope oracle states no `abs`, so every
    assignment is enumerated, a region at a time."""
    rows = answer(decidb_cli, _ship_query("abs(sum(ship) by (D.region)) <= 5"))
    _judge_apart(rows, lambda row: row["region"], lambda region, ships: abs(sum(ships)) <= 5)


# ---------------------------------------------------------------------------
# One helper per reducer occurrence, its scope over the kept rows, its members
# ---------------------------------------------------------------------------

#: Two groups of `g` with a cap per row.
T_COLUMNS = ("id", "g", "cap")
T_ROWS = ((1, "a", 10), (2, "a", 20), (3, "a", 30), (4, "b", 12), (5, "b", 7))
XV = lambda r: {"x": 1}  # noqa: E731
X_CAP = Scoped((XV,), "<=", (lambda r: {CONSTANT: r["cap"]},))
MAXIMIZE_X = ScopedObjective(ObjSense.MAXIMIZE, (Reduce("sum", XV),))


def _t_query(clause, decide="x(INT)", rows=T_ROWS, columns=T_COLUMNS):
    return f"""
        SELECT id, x {table('t', columns, rows)}
        DECIDE {decide}
        SUCH THAT x <= t.cap AND {clause}
        MAXIMIZE SUM(x)
        ORDER BY id"""


def _brute_force_optimum(rows, holds):
    """MAXIMIZE SUM(x) over 0 <= x <= cap, group of `g` by group, under `holds(xs)` over a
    group's values; None when some group has no feasible point. The oracle for the ABS
    shapes `_scope_oracle` does not model."""
    total = 0
    for g in dict.fromkeys(row["g"] for row in rows):
        caps = [row["cap"] for row in rows if row["g"] == g]
        best = max((sum(xs) for xs in itertools.product(*(range(cap + 1) for cap in caps)) if holds(xs)), default=None)
        if best is None:
            return None
        total += best
    return total


ABS_AROUND_A_HELPER = [
    pytest.param(
        "x <= abs(max(x) by (t.g) - 20) + 1",
        lambda xs: all(x <= abs(max(xs) - 20) + 1 for x in xs),
        id="max-in-the-hard-direction",
    ),
    pytest.param("abs(max(x) by (t.g) - 20) = 10", lambda xs: abs(max(xs) - 20) == 10, id="max-under-an-equality"),
    pytest.param(
        "x <= 2 * abs(min(x) by (t.g) - 10)",
        lambda xs: all(x <= 2 * abs(min(xs) - 10) for x in xs),
        id="min-in-the-hard-direction",
    ),
    pytest.param("abs(min(x) by (t.g)) >= 25", lambda xs: abs(min(xs)) >= 25, id="min-out-of-reach"),
    pytest.param("abs(sum(x) by (t.g) - 40) >= 3", lambda xs: abs(sum(xs) - 40) >= 3, id="sum-in-the-hard-direction"),
]


@pytest.mark.correctness
@pytest.mark.min_max
@pytest.mark.parametrize("clause, holds", ABS_AROUND_A_HELPER)
def test_abs_around_a_helper_in_the_hard_direction(decidb_cli_gurobi, decidb_cli_highs, clause, holds):
    """An ABS splits its clause into an envelope pair, each row holding a copy of the
    reducer: the rows share one helper, pinned on every side either of them needs. Two
    helpers, each pinned on one side, let the pair drift apart -- an answer above the
    optimum, or an answer where there is none. Both arms: native ABS and MIN/MAX on
    Gurobi, the Big-M lowering on HiGHS."""
    expected = _brute_force_optimum(table_rows(T_COLUMNS, T_ROWS), holds)
    for cli in (decidb_cli_gurobi, decidb_cli_highs):
        result = cli.execute_raw(_t_query(clause))
        assert_no_internal_leak(result, clause)
        if expected is None:
            assert "infeasible" in result.stderr, result.stderr
            continue
        assert result.returncode == 0 and not result.stderr.strip(), result.stderr
        assert sum(row["x"] for row in answer(cli, _t_query(clause))) == expected


@pytest.mark.correctness
def test_a_helper_inside_a_hard_abs_is_boxed_with_it(decidb_cli_highs, decidb_cli_gurobi):
    """`abs(sum(x) by (t.i % 2) - 7) >= 3`: the ABS Big-M needs the helper's box and the
    helper's box its members', so the two are derived together; the lowering arm used to
    refuse this query naming the helper. Judged by enumerating every assignment: the scope
    oracle states no `abs`."""
    rows = ((1, 5), (2, 5), (3, 5), (4, 5))
    sql = f"""
        SELECT i, x {table('t', ('i', 'cap'), rows)}
        DECIDE x(INT)
        SUCH THAT x <= t.cap AND abs(sum(x) by (t.i % 2) - 7) >= 3
        MAXIMIZE SUM(x)
        ORDER BY i"""
    domains = {f"x{k}": range(cap + 1) for k, (_, cap) in enumerate(rows)}

    def feasible(a):
        return all(abs(a[f"x{k}"] + a[f"x{k + 2}"] - 7) >= 3 for k in range(2))

    for cli in (decidb_cli_highs, decidb_cli_gurobi):
        judge_rows(answer(cli, sql), domains, feasible, lambda a: sum(a.values()))


@pytest.mark.correctness
def test_the_helper_key_is_read_on_the_kept_rows_only(decidb_cli, scope_solver):
    """`when t.i > 0:` drops the row whose key would not cast: the helper's instances are
    the groups among the kept rows, and the key is never evaluated on any other row."""
    rows = ((0, 9), (1, 10), (2, 20), (3, 30))
    columns = ("i", "cap")
    sql = f"""
        SELECT i, x {table('t', columns, rows)}
        DECIDE x(INT)
        SUCH THAT x <= t.cap
              AND when t.i > 0: x <= 0.5 * sum(x) by (CAST(CASE WHEN t.i = 0 THEN 'oops' ELSE '1' END AS INT))
        MAXIMIZE SUM(x)
        ORDER BY i"""
    model = ScopedModel([dict(row, k=1) for row in table_rows(columns, rows)], [Decision("x")])
    clauses = [
        X_CAP,
        Scoped((XV,), "<=", (Reduce("sum", XV, by=("k",), factor=lambda r: 0.5),), when=lambda r: r["i"] > 0),
    ]
    result = aligned(answer(decidb_cli, sql), model, "i")
    assert (
        model.judge_scoped(result, clauses, MAXIMIZE_X)
        == pytest.approx(model.solve_scoped(scope_solver, clauses, MAXIMIZE_X))
        == 69
    )
    assert [row["x"] for row in result] == [9, 10, 20, 30]


@pytest.mark.correctness
@pytest.mark.min_max
def test_a_min_helper_counts_a_keyed_decision_once(decidb_cli, scope_solver):
    """`min(per t.g: x) by ()` over `per t.g: x`: the reducer reads one row per class, and a
    row it does not read is no member -- left in, it would be a member pinned at 0 and the
    query infeasible. The best `b` is 15: five short of twice the smaller value."""
    rows = ((1, "a", 10), (2, "a", 20), (3, "b", 30))
    sql = _t_query("per row: x <= 2 * min(per t.g: x) by () - 5", decide="per t.g: x(INT)", rows=rows)
    model = ScopedModel(table_rows(T_COLUMNS, rows), [Decision("x", ("g",))])
    clauses = [X_CAP, Scoped((XV,), "<=", (Reduce("min", XV, per=("g",), factor=lambda r: 2), _const(-5)))]
    result = aligned(answer(decidb_cli, sql), model, "id")
    assert (
        model.judge_scoped(result, clauses, MAXIMIZE_X)
        == pytest.approx(model.solve_scoped(scope_solver, clauses, MAXIMIZE_X))
        == 35
    )
    assert [row["x"] for row in result] == [10, 10, 15]


# ---------------------------------------------------------------------------
# Model size, refusals, checks, diagnostics
# ---------------------------------------------------------------------------


@pytest.mark.correctness
def test_the_model_grows_linearly(decidb_cli, tmp_path):
    """Deck example 1 on N rows: N rows of two entries and one definition row of N + 1,
    not N rows of N + 1 entries."""
    n = 40
    sql = f"""
        SELECT i, x FROM range({n}) t(i)
        DECIDE x(INT)
        SUCH THAT x <= 10 AND per row: x <= 0.2 * sum(x)
        MAXIMIZE SUM(x)"""
    dump = decidb_cli.dump_model(sql, tmp_path / "model.dump")
    assert dump_counts(dump) == {"num_vars": n + 1, "num_rows": n + 1}
    assert sum(int(nnz) for nnz in re.findall(r"nnz=(\d+)", dump)) == 3 * n + 1


@pytest.mark.error
@pytest.mark.error_binder
def test_a_product_of_decisions_inside_a_rewritten_reducer_is_refused(decidb_cli):
    """§10.2: a square inside a reducer is accepted only in the aggregate shape."""
    result = decidb_cli.execute_raw(_ship_query("ship <= 0.5 * sum(ship * ship) by (D.region)"))
    assert (
        "DECIDE: 'sum(ship * ship)' is not supported yet in this constraint: a product or square of "
        "decisions inside a reducer is accepted only where the constraint's reducers share one by key, "
        "under a whole-query factor, with nothing beside them but data and query-wide decisions, each on "
        "its own"
    ) in result.stderr, result.stderr
    assert_no_internal_leak(result, "a product inside a rewritten reducer")


@pytest.mark.error
@pytest.mark.parametrize(
    "op, why",
    [
        (">", "stepping the bound is exact only on whole numbers. Compare with '>=' instead."),
        (
            "<>",
            "the rewrite (x <> K becomes x <= K-1 OR x >= K+1) is exact only on whole numbers. Compare with "
            "'<=' or '>=' instead.",
        ),
    ],
)
def test_a_helper_that_can_be_fractional_takes_no_step(decidb_cli, op, why):
    """The helper holds the reducer's whole value, `sum(ship + 0.5)` included, so `<`, `>`
    and `<>` cannot step it; summed in place (the aggregate shape) the constant would move
    to the bound instead. `>=` answers."""
    result = decidb_cli.execute_raw(_ship_query(f"ship + sum(ship + 0.5) by (D.region) {op} 40"))
    assert (
        f"Comparison '{op}' is not supported here: 'sum(ship + 0.5) by (D.region)' can take fractional "
        f"values in this constraint, and {why}"
    ) in result.stderr, result.stderr
    assert_no_internal_leak(result, "a fractional helper under a step")
    assert decidb_cli.execute(_ship_query("ship + sum(ship + 0.5) by (D.region) >= 40"))[0]


@pytest.mark.error
def test_the_scope_checks_still_guard_the_rewritten_clause(decidb_cli):
    """The checks run on the clause as written: a `per` that does not determine the
    decision beside the reducer."""
    assert (
        "DECIDE: per D.region does not determine the decision ship (declared per row). D.region = 'East' "
        "covers more than one ship."
    ) in error_of(decidb_cli, _ship_query("per D.region: ship <= 0.5 * sum(ship) by (D.depotID)"))


@pytest.mark.diagnose
def test_diagnose_names_the_clause_not_the_helper(decidb_cli):
    """An infeasible query with a helper: the diagnosis offers the user's own clause and
    never names the helper or its definition rows. `ship` is bounded above so the
    lowering arm has a Big-M for the link and diagnoses like the native one."""
    sql = f"""
        SELECT shipmentID, ship {from_clause()}
        DECIDE ship(INT)
        SUCH THAT ship >= 5 AND ship <= S.demand AND ship <= 0.1 * max(ship) by (D.depotID)
        MAXIMIZE SUM(ship)"""
    result = run_diagnose(decidb_cli, sql)
    assert "infeasible" in result.stdout and "ship >= 5" in result.stdout, result.stdout
    assert "__value_" not in result.stdout + result.stderr
    assert_no_internal_leak(result, "DIAGNOSE over a value helper")


@pytest.mark.diagnose
def test_diagnose_relaxes_one_of_two_clauses_sharing_a_helper(decidb_cli):
    """The shared definition belongs to no clause: the diagnosis relaxes a clause that reads
    the helper, as written, and never offers the helper. Each of three rows takes at most 10
    and the total at least 30, so every row is 10 and `x >= 0.4 * sum(x)` asks for 12."""
    sql = """
        SELECT id, x FROM (VALUES (1), (2), (3)) v(id)
        DECIDE x(INT)
        SUCH THAT x <= 10 AND sum(x) >= 30 AND x <= 0.5 * sum(x) AND x >= 0.4 * sum(x)
        MAXIMIZE sum(x)"""
    result = run_diagnose(decidb_cli, sql)
    assert "infeasible,x >= 0.4 * sum(x),x >= 0.4 * sum(x) - 2,2.0," in result.stdout, result.stdout
    assert "__value_" not in result.stdout + result.stderr
    assert_no_internal_leak(result, "DIAGNOSE over a shared value helper")


_DROP_CLAUSES = [
    ("x <> sum(x) - 1", lambda xs: all(x != sum(xs) - 1 for x in xs)),
    ("x <> sum(x)", lambda xs: all(x != sum(xs) for x in xs)),
]


def _best_single_drop(maximize):
    """The clause a remove-only repair drops and the objective it reaches, by enumeration: each
    single drop's best sum over every BOOL assignment of two rows, the best drop winning and the
    earliest-written one on a tie."""
    best = None
    for dropped, _ in _DROP_CLAUSES:
        kept = [holds for text, holds in _DROP_CLAUSES if text != dropped]
        sums = [sum(xs) for xs in itertools.product((0, 1), repeat=2) if all(h(list(xs)) for h in kept)]
        if not sums:
            continue
        value = max(sums) if maximize else min(sums)
        if best is None or (value > best[1] if maximize else value < best[1]):
            best = (dropped, value)
    return best


@pytest.mark.diagnose
@pytest.mark.parametrize("sense", ["MAXIMIZE", "MINIMIZE"])
def test_dropping_one_reader_keeps_the_shared_definition(decidb_cli, sense):
    """Two `<>` clauses read one helper and no assignment meets both. The definition belongs to
    neither, so the drop leaves it and the kept clause still reads `sum(x)`. Under MINIMIZE a
    definition dropped with the first clause would free the helper, make (0, 0) feasible and
    report the wrong clause."""
    sql = f"""
        SELECT id, x FROM (VALUES (1), (2)) v(id)
        DECIDE x(BOOL)
        SUCH THAT {" AND ".join(text for text, _ in _DROP_CLAUSES)}
        {sense} sum(x)"""
    dropped, objective = _best_single_drop(sense == "MAXIMIZE")
    result = run_diagnose(decidb_cli, sql)
    assert f"infeasible,{dropped},remove this clause," in result.stdout, result.stdout
    assert f"infeasible,NULL,NULL,{float(objective)},NULL,NULL,achievable_objective," in result.stdout, result.stdout
    assert_no_internal_leak(result, "DIAGNOSE dropping a reader of a shared helper")


@pytest.mark.explain
def test_each_clause_reading_a_shared_helper_accounts_for_it(decidb_cli):
    """Both clauses say in their own plan that `sum(ship)` is a helper decision."""
    out = decidb_cli.execute_raw(f"EXPLAIN {_ship_query('ship <= 0.25 * sum(ship) AND ship >= 0.1 * sum(ship)')}")
    text = re.sub(r"[│\s]", "", out.stdout)
    assert text.count("sum(ship)becomesahelperdecision") == 2, out.stdout


@pytest.mark.explain
def test_explain_echoes_the_clause_as_written(decidb_cli):
    """EXPLAIN shows the clause as written, its canonical reading with the reducer, and
    the rewrite's account; the helper prints as its reducer, never as `__value_0__`."""
    out = decidb_cli.execute_raw(f"EXPLAIN {_ship_query('ship <= D.maxShipmentShare * sum(ship) by (D.depotID)')}")
    # The plan box breaks lines inside tokens, so compare without any whitespace.
    text = re.sub(r"[│\s]", "", out.stdout)
    assert "ship<=D.maxShipmentShare*sum(ship)by(D.depotID)" in text, out.stdout
    assert "≡ship-maxShipmentShare*sum(ship)by(D.depotID)<=0.0" in text, out.stdout
    assert "sum(ship)by(D.depotID)becomesahelperdecisionpergroup" in text, out.stdout
    assert "__value_" not in text, out.stdout
    assert out.returncode == 0 and not out.stderr.strip(), out.stderr


@pytest.mark.correctness
@pytest.mark.parametrize(
    "clause, clauses, ships",
    [
        pytest.param(
            "per row: ship <= 0.20 * sum(ship)",
            [Scoped((X,), "<=", (_ship_reduce(factor=0.20),))],
            [10, 10, 10, 10, 10],
            id="example-1",
        ),
        pytest.param(
            "ship <= D.maxShipmentShare * sum(ship) by (D.depotID)",
            [Scoped((X,), "<=", (_ship_reduce(by=("depotID",), factor=lambda r: r["maxShipmentShare"]),))],
            [10, 10, 22, 15, 0],
            id="example-5",
        ),
        pytest.param(
            "sum(ship) by (D.depotID) + sum(ship) by (D.region) <= 50",
            [Scoped((_ship_reduce(by=("depotID",)), _ship_reduce(by=("region",))), "<=", (_const(50),))],
            None,
            id="two-keys",
        ),
        pytest.param(
            "ship <= 0.5 * max(ship) by (D.region) + 8",
            [Scoped((X,), "<=", (_ship_reduce("max", by=("region",), factor=0.5), _const(8)))],
            [10, 16, 16, 15, 16],
            id="max-closing",
        ),
    ],
)
def test_highs_builds_the_same_answers(decidb_cli_highs, scope_solver, clause, clauses, ships):
    """The helper's rows and links are plain rows and extremum links: HiGHS answers too."""
    model = ScopedModel(joined(), SHIP)
    result = _judged(decidb_cli_highs, scope_solver, _ship_query(clause), model, [SHIP_BOUND] + clauses)
    if ships is not None:
        assert result == ships
