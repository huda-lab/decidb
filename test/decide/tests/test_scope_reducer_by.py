"""`by (Γ)` after a reducer: which rows it reads (syntax_reference.md §5.2).

`agg(e) by (Γ)` reads every kept row whose Γ equals the current class's Γ, including
rows outside the class; Γ is columns or data expressions, NULL is a group (D5), and an
omitted `by` is `by ()`, every kept row. A clause whose decision reducers share one `by`
key is one model row per group of that key; its bound is the tightest one in the group
(`=` refuses two values, `<>` keeps every value). A data-only reducer on the bound side is
evaluated over its own `by` group, whatever the clause's shape. `MIN` and `MAX` are
accepted wherever `SUM` is (D3): the easy direction bounds every member, the hard one
pins the extremum.

Model size is read from DECIDB_DUMP_MODEL; optima are judged against
`_scope_oracle.ScopedModel`, which follows the ANR definition literally.
"""

import pytest

from ._brute_force import judge_rows
from ._diagnose_relation import rows as diagnose_rows
from ._diagnose_relation import run as run_diagnose
from ._output_helpers import assert_no_internal_leak
from ._scope_cases import (
    DEPOTS,
    SHIPMENTS,
    aligned,
    answer,
    dump_counts,
    from_clause,
    joined,  # noqa: F401
    scope_solver,
    table,
    table_rows,
)
from ._scope_oracle import CONSTANT, Decision, Reduce, Scoped, ScopedModel, ScopedObjective
from solver.types import ObjSense, VarType

SHIP = [Decision("ship")]
SHIP_BOUND = Scoped((lambda r: {"ship": 1},), "<=", (lambda r: {CONSTANT: r["demand"]},))
MAXIMIZE_SHIP = ScopedObjective(ObjSense.MAXIMIZE, (Reduce("sum", lambda r: {"ship": 1}),))
MINIMIZE_SHIP = ScopedObjective(ObjSense.MINIMIZE, (Reduce("sum", lambda r: {"ship": 1}),))


def _ship_query(clause: str, sense: str = "MAXIMIZE", shipments=SHIPMENTS, depots=DEPOTS) -> str:
    return f"""
        SELECT shipmentID, ship {from_clause(shipments, depots)}
        DECIDE ship(INT)
        SUCH THAT ship <= S.demand AND {clause}
        {sense} SUM(ship)
        ORDER BY shipmentID"""


def _judged(cli, solver, sql, model, clauses, objective):
    result = aligned(answer(cli, sql), model)
    assert model.judge_scoped(result, clauses, objective) == pytest.approx(
        model.solve_scoped(solver, clauses, objective)
    )
    return [row["ship"] for row in result]


def _ship_sum(by=(), when=None):
    return Reduce("sum", lambda r: {"ship": 1}, when=when, by=by)


# ---------------------------------------------------------------------------
# Which rows a reducer reads
# ---------------------------------------------------------------------------


@pytest.mark.correctness
def test_omitted_by_is_by_empty(decidb_cli, tmp_path):
    omitted = decidb_cli.dump_model(_ship_query("per (): sum(ship) <= 50"), tmp_path / "omitted.dump")
    assert omitted == decidb_cli.dump_model(_ship_query("per (): sum(ship) by () <= 50"), tmp_path / "empty.dump")


@pytest.mark.correctness
@pytest.mark.parametrize(
    "by, key, groups",
    [
        pytest.param("D.depotID", ("depotID",), 3, id="column"),
        pytest.param("D.region, D.country", ("region", "country"), 3, id="two-columns"),
        pytest.param(
            "S.dispatchDay + S.transitDays", lambda r: (r["dispatchDay"] + r["transitDays"],), 2, id="expression"
        ),
        pytest.param("D", ("depotID",), 3, id="relation"),
    ],
)
def test_by_key(decidb_cli, scope_solver, tmp_path, by, key, groups):
    """One model row per group of the key; each group's sum is bounded."""
    sql = _ship_query(f"sum(ship) by ({by}) <= 22")
    assert dump_counts(decidb_cli.dump_model(sql, tmp_path / "model.dump"))["num_rows"] == groups
    model = ScopedModel(joined(), SHIP)
    clauses = [SHIP_BOUND, Scoped((_ship_sum(key),), "<=", (lambda r: {CONSTANT: 22},))]
    _judged(decidb_cli, scope_solver, sql, model, clauses, MAXIMIZE_SHIP)


@pytest.mark.correctness
def test_a_reducer_reads_rows_outside_its_class(decidb_cli, scope_solver):
    """`per D.depotID: ... by (D.region)`: D1 and D2 each read every East shipment, under
    their own capacity -- the tightest, D1's 25, binds the East total."""
    sql = _ship_query("per D.depotID: sum(ship) by (D.region) <= D.capacity")
    model = ScopedModel(joined(), SHIP)
    clauses = [
        SHIP_BOUND,
        Scoped((_ship_sum(("region",)),), "<=", (lambda r: {CONSTANT: r["capacity"]},), per=("depotID",)),
    ]
    ships = _judged(decidb_cli, scope_solver, sql, model, clauses, MAXIMIZE_SHIP)
    assert sum(ships[:4]) == 25 and ships[4] == 20


@pytest.mark.correctness
def test_a_data_only_reducer_with_by_on_the_bound_side(decidb_cli, scope_solver):
    """Each shipment is bounded by a fifth of its region's demand (East 75, West 25)."""
    sql = _ship_query("ship <= 0.2 * sum(S.demand) by (D.region)")
    model = ScopedModel(joined(), SHIP)
    clauses = [
        SHIP_BOUND,
        Scoped(
            (lambda r: {"ship": 1},),
            "<=",
            (Reduce("sum", lambda r: {CONSTANT: r["demand"]}, by=("region",), factor=lambda r: 0.2),),
        ),
    ]
    assert _judged(decidb_cli, scope_solver, sql, model, clauses, MAXIMIZE_SHIP) == [10, 15, 15, 15, 5]


@pytest.mark.correctness
def test_bound_side_reducers_each_read_their_own_group(decidb_cli, scope_solver):
    """Two bound-side reducers with different `by` keys beside a decision reducer grouped
    by a third: each is evaluated over its own groups of kept rows."""
    sql = _ship_query(
        "when S.status = 'active' per D.depotID: sum(ship) by (D.depotID) "
        "<= 0.5 * max(S.demand) by (D.region) + min(S.demand) by (D.country) - 10"
    )
    model = ScopedModel(joined(), SHIP)
    active = lambda r: r["status"] == "active"  # noqa: E731
    bound = (
        Reduce("max", lambda r: {CONSTANT: r["demand"]}, by=("region",), factor=lambda r: 0.5),
        Reduce("min", lambda r: {CONSTANT: r["demand"]}, by=("country",)),
        lambda r: {CONSTANT: -10},
    )
    clauses = [SHIP_BOUND, Scoped((_ship_sum(("depotID",)),), "<=", bound, when=active, per=("depotID",))]
    _judged(decidb_cli, scope_solver, sql, model, clauses, MAXIMIZE_SHIP)


@pytest.mark.correctness
def test_null_is_a_group_of_by(decidb_cli, scope_solver):
    """Rows 4 and 5 have no region: they form one group of their own (D5)."""
    rows = ((1, "a", 10), (2, "a", 20), (3, "b", 30), (4, None, 15), (5, None, 25))
    columns = ("id", "region", "demand")
    sql = f"""
        SELECT id, ship {table('S', columns, rows)}
        DECIDE ship(INT)
        SUCH THAT ship <= S.demand AND sum(ship) by (S.region) <= 22
        MAXIMIZE SUM(ship)
        ORDER BY id"""
    model = ScopedModel(table_rows(columns, rows), SHIP)
    clauses = [SHIP_BOUND, Scoped((_ship_sum(("region",)),), "<=", (lambda r: {CONSTANT: 22},))]
    result = aligned(answer(decidb_cli, sql), model, "id")
    assert (
        model.judge_scoped(result, clauses, MAXIMIZE_SHIP)
        == pytest.approx(model.solve_scoped(scope_solver, clauses, MAXIMIZE_SHIP))
        == 22 + 22 + 22
    )


@pytest.mark.correctness
@pytest.mark.parametrize("key", ["1", "'all'", "NULL", "1 + 1"])
def test_a_constant_by_key_is_one_group(decidb_cli, scope_solver, key):
    """A literal or folded key holds one value on every row: one group, as `by ()`."""
    sql = _ship_query(f"sum(ship) by ({key}) <= 30")
    model = ScopedModel(joined(), SHIP)
    clauses = [SHIP_BOUND, Scoped((_ship_sum(lambda r: (1,)),), "<=", (lambda r: {CONSTANT: 30},))]
    assert sum(_judged(decidb_cli, scope_solver, sql, model, clauses, MAXIMIZE_SHIP)) == 30


@pytest.mark.correctness
def test_a_constant_by_key_beside_a_when_and_on_the_bound_side(decidb_cli, scope_solver):
    """The priority shipments share 12, and every shipment is bounded by a tenth of the
    total demand, 10."""
    sql = _ship_query("sum(when S.priority: ship) by ('all') <= 12 AND ship <= sum(S.demand) by (1) / 10")
    model = ScopedModel(joined(), SHIP)
    one = lambda r: (1,)  # noqa: E731
    clauses = [
        SHIP_BOUND,
        Scoped((_ship_sum(one, when=lambda r: r["priority"]),), "<=", (lambda r: {CONSTANT: 12},)),
        Scoped(
            (lambda r: {"ship": 1},),
            "<=",
            (Reduce("sum", lambda r: {CONSTANT: r["demand"]}, by=one, factor=lambda r: 0.1),),
        ),
    ]
    ships = _judged(decidb_cli, scope_solver, sql, model, clauses, MAXIMIZE_SHIP)
    assert ships[1] == ships[3] == 10 and ships[0] + ships[2] + ships[4] == 12


@pytest.mark.correctness
def test_a_key_that_is_null_on_every_row_is_one_group(decidb_cli, scope_solver):
    """`g` holds only NULL, so `g || h` is NULL on every row: one group, which row 2 is in."""
    columns = ("id", "g", "h", "v")
    rows = ((1, None, None, 7), (2, None, "q", 9))
    sql = f"""
        SELECT id, x {table('t', columns, rows)}
        DECIDE x(INT)
        SUCH THAT x <= 20 AND x <= max(when t.id > 1: t.v) by (t.g || t.h)
        MAXIMIZE SUM(x)
        ORDER BY id"""
    model = ScopedModel(table_rows(columns, rows), [Decision("x")])
    clauses = [
        Scoped((lambda r: {"x": 1},), "<=", (lambda r: {CONSTANT: 20},)),
        Scoped(
            (lambda r: {"x": 1},),
            "<=",
            (Reduce("max", lambda r: {CONSTANT: r["v"]}, when=lambda r: r["id"] > 1, by=lambda r: (None,)),),
        ),
    ]
    objective = ScopedObjective(ObjSense.MAXIMIZE, (Reduce("sum", lambda r: {"x": 1}),))
    result = aligned(answer(decidb_cli, sql), model, "id")
    assert (
        model.judge_scoped(result, clauses, objective)
        == pytest.approx(model.solve_scoped(scope_solver, clauses, objective))
        == 18
    )


@pytest.mark.correctness
@pytest.mark.parametrize(
    "key, oracle_key",
    [
        pytest.param("D.region = (SELECT 'East')", lambda r: (r["region"] == "East",), id="uncorrelated"),
        pytest.param(
            "(SELECT count(*) FROM (VALUES (1), (2)) v(k) WHERE v.k <= S.dispatchDay)",
            lambda r: (min(r["dispatchDay"], 2),),
            id="correlated",
        ),
    ],
)
def test_a_by_key_with_a_subquery(decidb_cli, scope_solver, key, oracle_key):
    """A key written twice in one clause is one key, a subquery in it included."""
    sql = _ship_query(f"sum(ship) by ({key}) + sum(when S.priority: ship) by ({key}) <= 60")
    model = ScopedModel(joined(), SHIP)
    clauses = [
        SHIP_BOUND,
        Scoped(
            (_ship_sum(oracle_key), _ship_sum(oracle_key, when=lambda r: r["priority"])),
            "<=",
            (lambda r: {CONSTANT: 60},),
        ),
    ]
    _judged(decidb_cli, scope_solver, sql, model, clauses, MAXIMIZE_SHIP)


@pytest.mark.correctness
def test_tightest_bound_per_group_without_per(decidb_cli, scope_solver):
    """With no `per`, a varying bound is one constraint per row, which is the group's
    tightest bound (§6.2); `=` cannot hold two values, and `<>` excludes every one."""
    sql = _ship_query("sum(ship) by (D.region) <= D.capacity")
    model = ScopedModel(joined(), SHIP)
    clauses = [SHIP_BOUND, Scoped((_ship_sum(("region",)),), "<=", (lambda r: {CONSTANT: r["capacity"]},))]
    ships = _judged(decidb_cli, scope_solver, sql, model, clauses, MAXIMIZE_SHIP)
    assert sum(ships[:4]) == 25

    result = decidb_cli.execute_raw(_ship_query("sum(ship) by (D.region) = D.capacity"))
    assert "an `=` bound cannot hold two different values at once" in result.stderr, result.stderr

    sql = _ship_query("ship <= 9 AND sum(ship) by (D.region) <> D.capacity - 2")
    model_ne = ScopedModel(joined(), SHIP)
    clauses = [
        SHIP_BOUND,
        Scoped((lambda r: {"ship": 1},), "<=", (lambda r: {CONSTANT: 9},)),
        Scoped((_ship_sum(("region",)),), "<>", (lambda r: {CONSTANT: r["capacity"] - 2},)),
    ]
    ships = _judged(decidb_cli, scope_solver, sql, model_ne, clauses, MAXIMIZE_SHIP)
    assert sum(ships[:4]) not in (23, 38) and ships[4] == 9


# ---------------------------------------------------------------------------
# MIN and MAX, AVG
# ---------------------------------------------------------------------------


@pytest.mark.correctness
@pytest.mark.min_max
@pytest.mark.parametrize(
    "clause, sense, oracle_clause",
    [
        pytest.param(
            "per D.depotID: max(ship) by (D.depotID) <= D.capacity - 12",
            "MAXIMIZE",
            Scoped(
                (Reduce("max", lambda r: {"ship": 1}, by=("depotID",)),),
                "<=",
                (lambda r: {CONSTANT: r["capacity"] - 12},),
                per=("depotID",),
            ),
            id="max-easy",
        ),
        pytest.param(
            "per D.depotID: min(ship) by (D.depotID) >= 8",
            "MINIMIZE",
            Scoped(
                (Reduce("min", lambda r: {"ship": 1}, by=("depotID",)),),
                ">=",
                (lambda r: {CONSTANT: 8},),
                per=("depotID",),
            ),
            id="min-easy",
        ),
        pytest.param(
            "per D.depotID: max(ship) by (D.depotID) >= 12",
            "MINIMIZE",
            Scoped(
                (Reduce("max", lambda r: {"ship": 1}, by=("depotID",)),),
                ">=",
                (lambda r: {CONSTANT: 12},),
                per=("depotID",),
            ),
            id="max-hard",
        ),
        pytest.param(
            "per D.region: min(ship) by (D.region) <= 4",
            "MAXIMIZE",
            Scoped(
                (Reduce("min", lambda r: {"ship": 1}, by=("region",)),),
                "<=",
                (lambda r: {CONSTANT: 4},),
                per=("region",),
            ),
            id="min-hard",
        ),
        pytest.param(
            "per D.depotID: max(ship) by (D.depotID) = 12",
            "MINIMIZE",
            Scoped(
                (Reduce("max", lambda r: {"ship": 1}, by=("depotID",)),),
                "=",
                (lambda r: {CONSTANT: 12},),
                per=("depotID",),
            ),
            id="max-equal",
        ),
        pytest.param(
            "max(when S.priority: ship) by (D.region) <= 9",
            "MAXIMIZE",
            Scoped(
                (Reduce("max", lambda r: {"ship": 1}, when=lambda r: r["priority"], by=("region",)),),
                "<=",
                (lambda r: {CONSTANT: 9},),
            ),
            id="max-inner-when",
        ),
        pytest.param(
            "when S.priority: min(ship) by (D.region) >= 5",
            "MINIMIZE",
            Scoped(
                (Reduce("min", lambda r: {"ship": 1}, by=("region",)),),
                ">=",
                (lambda r: {CONSTANT: 5},),
                when=lambda r: r["priority"],
            ),
            id="min-front-when",
        ),
    ],
)
def test_min_and_max(decidb_cli, scope_solver, clause, sense, oracle_clause):
    sql = _ship_query(clause, sense)
    model = ScopedModel(joined(), SHIP)
    objective = MAXIMIZE_SHIP if sense == "MAXIMIZE" else MINIMIZE_SHIP
    _judged(decidb_cli, scope_solver, sql, model, [SHIP_BOUND, oracle_clause], objective)


@pytest.mark.correctness
@pytest.mark.min_max
@pytest.mark.parametrize(
    "clause, oracle_clause, xs",
    [
        pytest.param(
            "per (): min(per t.g: x) <= 12",
            Scoped((Reduce("min", lambda r: {"x": 1}, per=("g",)),), "<=", (lambda r: {CONSTANT: 12},), per=()),
            [20, 20, 12],
            id="hard-min",
        ),
        pytest.param(
            "per (): max(per t.g: x) >= 18",
            Scoped((Reduce("max", lambda r: {"x": 1}, per=("g",)),), ">=", (lambda r: {CONSTANT: 18},), per=()),
            [20, 20, 20],
            id="hard-max",
        ),
        pytest.param(
            "per (): max(per t.g: x) <= 12",
            Scoped((Reduce("max", lambda r: {"x": 1}, per=("g",)),), "<=", (lambda r: {CONSTANT: 12},), per=()),
            [12, 12, 12],
            id="easy-max",
        ),
    ],
)
def test_a_hard_min_or_max_counts_a_keyed_decision_once(decidb_cli, scope_solver, clause, oracle_clause, xs):
    """`min(per t.g: x)` over `per t.g: x` reads one row per class of `g`: a row it does not
    read is no member. Left in with a zero coefficient, it was a member pinned at 0, which
    `min(...) <= 12` took for the class below 12 and let both classes stay at 20."""
    columns = ("id", "g")
    rows = ((1, "a"), (2, "a"), (3, "b"))
    sql = f"""
        SELECT id, x {table('t', columns, rows)}
        DECIDE per t.g: x(INT)
        SUCH THAT x >= 10 AND x <= 20 AND {clause}
        MAXIMIZE SUM(x)
        ORDER BY id"""
    x = lambda r: {"x": 1}  # noqa: E731
    model = ScopedModel(table_rows(columns, rows), [Decision("x", ("g",))])
    clauses = [
        Scoped((x,), ">=", (lambda r: {CONSTANT: 10},)),
        Scoped((x,), "<=", (lambda r: {CONSTANT: 20},)),
        oracle_clause,
    ]
    objective = ScopedObjective(ObjSense.MAXIMIZE, (Reduce("sum", x),))
    result = aligned(answer(decidb_cli, sql), model, "id")
    assert (
        model.judge_scoped(result, clauses, objective)
        == pytest.approx(model.solve_scoped(scope_solver, clauses, objective))
        == sum(xs)
    )
    assert [row["x"] for row in result] == xs


@pytest.mark.correctness
@pytest.mark.min_max
def test_easy_max_reduces_the_bound_over_every_kept_row(decidb_cli, scope_solver):
    """The reducer's own `when` picks the members, S1 and S3 of D1 (capacity 40); the
    bound is still the tightest over every kept row of the group -- D2's 25, on the
    non-members S2 and S4, caps East's members at 20. Over the members only it would be
    35, and S3 would ship 30."""
    shipments = (
        (1, "D1", 10, True, 1, 2, "active"),
        (2, "D2", 20, False, 1, 3, "active"),
        (3, "D1", 30, True, 2, 1, "active"),
        (4, "D2", 15, False, 2, 2, "hold"),
        SHIPMENTS[4],
    )
    depots = (("D1", "East", "US", 40, 8, 0.1, 0.5, 60), ("D2", "East", "CA", 25, 12, 0.2, 0.6, 60), DEPOTS[2])
    sql = _ship_query("max(when S.priority: ship) by (D.region) <= D.capacity - 5", shipments=shipments, depots=depots)
    model = ScopedModel(joined(shipments, depots), SHIP)
    clauses = [
        SHIP_BOUND,
        Scoped(
            (Reduce("max", lambda r: {"ship": 1}, when=lambda r: r["priority"], by=("region",)),),
            "<=",
            (lambda r: {CONSTANT: r["capacity"] - 5},),
        ),
    ]
    assert _judged(decidb_cli, scope_solver, sql, model, clauses, MAXIMIZE_SHIP) == [10, 20, 20, 15, 15]


FACTOR_COLUMNS = ("id", "g", "v", "cap")
FACTOR_ROWS = ((1, "a", 10, 5), (2, "a", 20, 5), (3, "b", 30, 8), (4, None, 40, 2), (5, None, 50, 2))


def _x(agg, factor=1, by=("g",)):
    return Reduce(agg, lambda r: {"x": 1}, by=by, factor=lambda r: factor)


def _cap(scale=1):
    return lambda r: {CONSTANT: scale * r["cap"]}


@pytest.mark.correctness
@pytest.mark.min_max
@pytest.mark.parametrize(
    "clause, sense, oracle_clause, optimum",
    [
        pytest.param(
            "per t.g: -2 * max(x) by (t.g) <= -2 * t.cap",
            "MINIMIZE",
            Scoped((_x("max", -2),), "<=", (_cap(-2),), per=("g",)),
            15,
            id="negative-factor-hard-max",
        ),
        pytest.param(
            "per t.g: -2 * max(x) by (t.g) = -2 * t.cap",
            "MINIMIZE",
            Scoped((_x("max", -2),), "=", (_cap(-2),), per=("g",)),
            15,
            id="negative-factor-equal",
        ),
        pytest.param(
            "per t.g: -2 * min(x) by (t.g) >= -2 * t.cap",
            "MAXIMIZE",
            Scoped((_x("min", -2),), ">=", (_cap(-2),), per=("g",)),
            85,
            id="negative-factor-hard-min",
        ),
        pytest.param(
            "per (): -2 * max(x) <= -10",
            "MINIMIZE",
            Scoped((_x("max", -2, ()),), "<=", (lambda r: {CONSTANT: -10},), per=()),
            5,
            id="negative-factor-one-group",
        ),
        pytest.param(
            "per t.g: max(x) by (t.g) > t.cap",
            "MINIMIZE",
            Scoped((_x("max"),), ">", (_cap(),), per=("g",)),
            18,
            id="strict-hard-max",
        ),
        pytest.param(
            "per t.g: min(x) by (t.g) < t.cap",
            "MAXIMIZE",
            Scoped((_x("min"),), "<", (_cap(),), per=("g",)),
            82,
            id="strict-hard-min",
        ),
        pytest.param(
            "per t.g: 2 * max(x) by (t.g) > 2 * t.cap",
            "MINIMIZE",
            Scoped((_x("max", 2),), ">", (_cap(2),), per=("g",)),
            18,
            id="strict-under-a-factor",
        ),
        pytest.param(
            "per t.g: -max(x) by (t.g) <= -t.cap",
            "MINIMIZE",
            Scoped((_x("max", -1),), "<=", (_cap(-1),), per=("g",)),
            15,
            id="negated-hard-max",
        ),
        pytest.param(
            "per t.g: t.cap - max(x) by (t.g) <= 0",
            "MINIMIZE",
            Scoped((_cap(), _x("max", -1)), "<=", (), per=("g",)),
            15,
            id="negated-beside-data",
        ),
        pytest.param(
            "per t.g: -max(x) by (t.g) >= -t.cap",
            "MAXIMIZE",
            Scoped((_x("max", -1),), ">=", (_cap(-1),), per=("g",)),
            22,
            id="negated-easy-max",
        ),
        pytest.param(
            "per t.g: -min(x) by (t.g) >= -t.cap",
            "MAXIMIZE",
            Scoped((_x("min", -1),), ">=", (_cap(-1),), per=("g",)),
            85,
            id="negated-hard-min",
        ),
        pytest.param(
            "-max(x) by (t.id % 2) <= -5",
            "MINIMIZE",
            Scoped((_x("max", -1, lambda r: (r["id"] % 2,)),), "<=", (lambda r: {CONSTANT: -5},)),
            10,
            id="negated-without-per",
        ),
        pytest.param(
            "per t.g: -(-max(x) by (t.g)) >= t.cap",
            "MINIMIZE",
            Scoped((_x("max"),), ">=", (_cap(),), per=("g",)),
            15,
            id="negated-twice",
        ),
        pytest.param(
            "-(-max(x) by (t.id % 2)) <= 3",
            "MAXIMIZE",
            Scoped((_x("max", 1, lambda r: (r["id"] % 2,)),), "<=", (lambda r: {CONSTANT: 3},)),
            15,
            id="negated-twice-without-per",
        ),
        pytest.param(
            "when t.id >= 0: -(-min(x)) >= t.id",
            "MINIMIZE",
            Scoped((_x("min", 1, ()),), ">=", (lambda r: {CONSTANT: r["id"]},), when=lambda r: r["id"] >= 0),
            25,
            id="negated-twice-against-a-row-bound",
        ),
        pytest.param(
            "per t.g: max(x + t.cap) by (t.g) > 12",
            "MINIMIZE",
            Scoped(
                (Reduce("max", lambda r: {"x": 1, CONSTANT: r["cap"]}, by=("g",)),),
                ">",
                (lambda r: {CONSTANT: 12},),
                per=("g",),
            ),
            24,
            id="strict-with-a-whole-data-offset",
        ),
    ],
)
def test_a_min_or_max_under_a_factor_or_a_strict_bound(decidb_cli, scope_solver, clause, sense, oracle_clause, optimum):
    """A factor holds the reducer's value: `-2 * max(e)` is `min(-2 * e)`, so a negative
    one turns the extremum and the direction around; `-max(e)` is the factor -1. `<` and
    `>` step a whole-numbered bound by one."""
    sql = f"""
        SELECT id, x {table('t', FACTOR_COLUMNS, FACTOR_ROWS)}
        DECIDE x(INT)
        SUCH THAT x <= t.v AND {clause}
        {sense} SUM(x)
        ORDER BY id"""
    model = ScopedModel(table_rows(FACTOR_COLUMNS, FACTOR_ROWS), [Decision("x")])
    clauses = [Scoped((lambda r: {"x": 1},), "<=", (lambda r: {CONSTANT: r["v"]},)), oracle_clause]
    objective = ScopedObjective(ObjSense[sense], (Reduce("sum", lambda r: {"x": 1}),))
    result = aligned(answer(decidb_cli, sql), model, "id")
    assert (
        model.judge_scoped(result, clauses, objective)
        == pytest.approx(model.solve_scoped(scope_solver, clauses, objective))
        == optimum
    )


@pytest.mark.correctness
@pytest.mark.min_max
@pytest.mark.parametrize(
    "clause, sense, lhs, cmp, bound, optimum",
    [
        pytest.param(
            "when t.id >= 0: sum(x) + max(x) > 30",
            "MINIMIZE",
            (Reduce("sum", lambda r: {"x": 1}), Reduce("max", lambda r: {"x": 1})),
            ">",
            30,
            21,
            id="greater",
        ),
        pytest.param(
            "when t.id >= 0: sum(x) - min(x) > 15",
            "MINIMIZE",
            (Reduce("sum", lambda r: {"x": 1}), Reduce("min", lambda r: {"x": 1}, factor=lambda r: -1)),
            ">",
            15,
            16,
            id="greater-minus-min",
        ),
        pytest.param(
            "when t.id >= 0: sum(x) + max(x) < 26",
            "MAXIMIZE",
            (Reduce("sum", lambda r: {"x": 1}), Reduce("max", lambda r: {"x": 1})),
            "<",
            26,
            18,
            id="less",
        ),
    ],
)
def test_a_strict_bound_on_a_composed_clause(decidb_cli, scope_solver, clause, sense, lhs, cmp, bound, optimum):
    """A MIN or MAX beside another term, against a strict bound: `> 30` is `>= 31`. Each
    bound is one where the non-strict reading has another optimum (20, 15, 19)."""
    columns = ("id",)
    rows = ((0,), (1,), (2,))
    sql = f"""
        SELECT id, x {table('t', columns, rows)}
        DECIDE x(INT)
        SUCH THAT x <= 10 AND {clause}
        {sense} SUM(x)
        ORDER BY id"""
    model = ScopedModel(table_rows(columns, rows), [Decision("x")])
    clauses = [
        Scoped((lambda r: {"x": 1},), "<=", (lambda r: {CONSTANT: 10},)),
        Scoped(lhs, cmp, (lambda r: {CONSTANT: bound},), when=lambda r: r["id"] >= 0),
    ]
    objective = ScopedObjective(ObjSense[sense], (Reduce("sum", lambda r: {"x": 1}),))
    result = aligned(answer(decidb_cli, sql), model, "id")
    assert (
        model.judge_scoped(result, clauses, objective)
        == pytest.approx(model.solve_scoped(scope_solver, clauses, objective))
        == optimum
    )


@pytest.mark.query_diagnostics
@pytest.mark.parametrize(
    "clause, written, suggested",
    [
        pytest.param("per (): min(x) < 0", "per (): min(x) < 0", "per (): min(x) < 1", id="min"),
        pytest.param("max(x) by (t.g) > 3", "max(x) by (t.g) > 3", "max(x) by (t.g) > 2", id="max-by"),
        pytest.param(
            "per (): sum(x) + max(x) > 12",
            "per (): sum(x) + max(x) > 12",
            "per (): sum(x) + max(x) > 11",
            id="composed",
        ),
    ],
)
def test_diagnose_quotes_a_strict_min_or_max_bound_as_written(decidb_cli, clause, written, suggested):
    """The model steps a strict bound (`> 3` is `>= 4`); a diagnosis still quotes `> 3` and
    suggests the next value the user would write. (The cap is a data column, which a repair
    edits only after the clauses' own literals, so the clause under test is the edit.)"""
    findings = diagnose_rows(
        run_diagnose(
            decidb_cli,
            f"""
        SELECT id, x {table('t', ('id', 'g', 'cap'), ((0, 'a', 3), (1, 'a', 3), (2, 'b', 3)))}
        DECIDE x(INT)
        SUCH THAT x <= t.cap AND {clause}
        MINIMIZE SUM(x)""",
        )
    )
    assert any(row["clause"] == written and row["suggested_change"] == suggested for row in findings), findings


@pytest.mark.correctness
@pytest.mark.avg_rewrite
@pytest.mark.parametrize(
    "clause, oracle_clause, priority_east",
    [
        pytest.param(
            "avg(when S.priority: ship) by (D.region) <= 5",
            Scoped(
                (Reduce("avg", lambda r: {"ship": 1}, when=lambda r: r["priority"], by=("region",)),),
                "<=",
                (lambda r: {CONSTANT: 5},),
            ),
            10,
            id="inner-when",
        ),
        pytest.param(
            "when S.priority: avg(ship) by (D.region) <= 5",
            Scoped(
                (Reduce("avg", lambda r: {"ship": 1}, by=("region",)),),
                "<=",
                (lambda r: {CONSTANT: 5},),
                when=lambda r: r["priority"],
            ),
            10,
            id="front-when",
        ),
        pytest.param(
            "when S.priority: avg(ship) <= 5",
            Scoped(
                (Reduce("avg", lambda r: {"ship": 1}),), "<=", (lambda r: {CONSTANT: 5},), when=lambda r: r["priority"]
            ),
            None,
            id="front-when-every-group",
        ),
    ],
)
def test_avg_divides_by_the_rows_its_when_keeps(decidb_cli, scope_solver, clause, oracle_clause, priority_east):
    """East holds four shipments, two of them priority: an average over the priority ones
    divides by 2, so they ship 10 between them -- over all four it would be 20."""
    sql = _ship_query(clause)
    model = ScopedModel(joined(), SHIP)
    ships = _judged(decidb_cli, scope_solver, sql, model, [SHIP_BOUND, oracle_clause], MAXIMIZE_SHIP)
    if priority_east is not None:
        assert ships[0] + ships[2] == priority_east
    assert ships[1] == 20 and ships[3] == 15


@pytest.mark.correctness
@pytest.mark.avg_rewrite
def test_avg_by_group(decidb_cli, scope_solver):
    sql = _ship_query("per D.region: avg(ship) by (D.region) <= 12")
    model = ScopedModel(joined(), SHIP)
    clauses = [
        SHIP_BOUND,
        Scoped(
            (Reduce("avg", lambda r: {"ship": 1}, by=("region",)),), "<=", (lambda r: {CONSTANT: 12},), per=("region",)
        ),
    ]
    ships = _judged(decidb_cli, scope_solver, sql, model, clauses, MAXIMIZE_SHIP)
    assert sum(ships[:4]) == 48 and ships[4] == 12


@pytest.mark.correctness
@pytest.mark.avg_rewrite
def test_avg_not_equal(decidb_cli):
    """`<>` over a plain AVG excludes the value from each group's average. The scope
    oracle steps a whole-numbered side, which an average is not, so every assignment is
    enumerated."""
    sql = _ship_query("ship <= 2 AND per D.region: avg(ship) by (D.region) <> 2")
    regions = [row["region"] for row in joined()]

    def feasible(a):
        totals = {}
        for i, region in enumerate(regions):
            total, count = totals.get(region, (0, 0))
            totals[region] = (total + a[f"ship{i}"], count + 1)
        return all(total != 2 * count for total, count in totals.values())

    domains = {f"ship{i}": range(3) for i in range(len(regions))}
    judge_rows(answer(decidb_cli, sql), domains, feasible, lambda a: sum(a.values()))


@pytest.mark.correctness
@pytest.mark.quadratic
def test_a_square_in_a_grouped_reducer(decidb_cli_gurobi):
    """`POWER(…, 2)` inside reducers sharing one `by` key is the aggregate shape (§10.2).
    A quadratic constraint needs Gurobi, and the scope oracle is linear, so every
    assignment is enumerated."""
    rows = ((1, "a", 5), (2, "a", 4), (3, "b", 6), (4, "b", 2))
    columns = ("id", "g", "cap")
    sql = f"""
        SELECT id, x {table('t', columns, rows)}
        DECIDE x(INT)
        SUCH THAT x <= t.cap AND per t.g: sum(x * x) by (t.g) <= 20
        MAXIMIZE SUM(x)
        ORDER BY id"""

    def feasible(a):
        return a["x0"] ** 2 + a["x1"] ** 2 <= 20 and a["x2"] ** 2 + a["x3"] ** 2 <= 20

    domains = {f"x{i}": range(cap + 1) for i, (_, _, cap) in enumerate(rows)}
    judge_rows(answer(decidb_cli_gurobi, sql), domains, feasible, lambda a: sum(a.values()))


# ---------------------------------------------------------------------------
# Refused shapes and errors. The shapes the value rewrite answers -- a decision beside a
# reducer, reducers with different by keys, MIN/MAX beside another term with a by or a
# varying bound, a reducer inside another expression -- run in test_scope_value_rewrite.py.
# ---------------------------------------------------------------------------


@pytest.mark.error
@pytest.mark.error_binder
@pytest.mark.parametrize(
    "clause, message",
    [
        pytest.param(
            "avg(ship) by (D.region) + sum(ship) by (D.region) <> 3",
            "DECIDE: AVG in a <> constraint is not supported yet beside another term or with its own when " "or per",
            id="avg-beside-a-term-under-not-equal",
        ),
        pytest.param(
            "avg(when S.priority: ship) <> 3",
            "DECIDE: AVG in a <> constraint is not supported yet beside another term or with its own when " "or per",
            id="avg-with-its-own-when-under-not-equal",
        ),
        pytest.param(
            "abs(ship) by (D.region) <= 5",
            "DECIDE: by (...) must directly follow SUM, AVG, MIN, MAX, norm or count",
            id="by-after-abs",
        ),
        pytest.param(
            "sum(ship) by (ship) <= 5",
            "DECIDE: ship is a decision; when, per and by may only use data",
            id="decision-in-by",
        ),
        pytest.param(
            "sum(max(ship) by (D.region)) <= 5",
            "DECIDE: an aggregate inside an aggregate is not supported yet",
            id="nested",
        ),
        pytest.param(
            "per D.region: avg(ship) by (D.region) < 3",
            "Comparison '<' is not supported here: 'avg(ship) by (D.region)' has type DOUBLE",
            id="strict-avg-with-by",
        ),
        pytest.param(
            "avg(when S.priority: ship) by (D.region) > 3",
            "Comparison '>' is not supported here: 'avg(when S.priority: ship) by (D.region)' has type DOUBLE",
            id="strict-avg-with-a-when",
        ),
        pytest.param(
            "avg(per D.depotID: ship) by (D.region) < 3",
            "Comparison '<' is not supported here: 'avg(per D.depotID: ship) by (D.region)' has type DOUBLE",
            id="strict-avg-with-an-inner-per",
        ),
        pytest.param(
            "per D.region: max(ship + 0.5) by (D.region) > 3",
            "Comparison '>' is not supported here: 'max(ship + 0.5) by (D.region)' has type DECIMAL",
            id="strict-max-of-a-fraction",
        ),
        pytest.param(
            "per (): min(ship - D.networkReserveShare) < 3",
            "Comparison '<' is not supported here: 'min(ship - D.networkReserveShare)' has type DECIMAL",
            id="strict-min-of-a-fractional-column",
        ),
        pytest.param(
            "when S.priority: sum(ship) + max(ship + 0.5) > 30",
            "Comparison '>' is not supported here: 'max(ship + 0.5)' has type DECIMAL",
            id="strict-composed-max-of-a-fraction",
        ),
    ],
)
def test_refused_shapes(decidb_cli, clause, message):
    result = decidb_cli.execute_raw(_ship_query(clause))
    assert message in result.stderr, result.stderr
    assert_no_internal_leak(result, "a refused by shape")


@pytest.mark.error
@pytest.mark.error_binder
@pytest.mark.parametrize(
    "decide, clause, objective, message",
    [
        pytest.param(
            "ship(INT), per D.depotID: r(INT)",
            "r <= 9 AND avg(per D.depotID: r) < 3",
            "MAXIMIZE SUM(ship)",
            "Comparison '<' is not supported here: 'avg(per D.depotID: r)' has type DOUBLE",
            id="strict-avg-keyed",
        ),
        pytest.param(
            "ship(INT), per (): s(INT)",
            "s <= 7",
            "MAXIMIZE when S.priority: sum(ship) - max(ship) + s",
            "DECIDE: 's' beside MIN or MAX is not supported yet",
            id="decision-beside-max-in-the-objective",
        ),
    ],
)
def test_refused_shapes_beside_other_decisions(decidb_cli, decide, clause, objective, message):
    result = decidb_cli.execute_raw(
        f"""
        SELECT shipmentID, ship {from_clause()}
        DECIDE {decide}
        SUCH THAT ship <= S.demand AND {clause}
        {objective}"""
    )
    assert message in result.stderr, result.stderr
    assert_no_internal_leak(result, "a refused shape beside another decision")


@pytest.mark.correctness
@pytest.mark.min_max
def test_a_composed_clause_reads_the_kept_rows(decidb_cli, scope_solver):
    """`when S.priority: sum(ship) + max(ship) <= 30` sums and maxes the priority rows."""
    sql = _ship_query("when S.priority: sum(ship) + max(ship) <= 30")
    model = ScopedModel(joined(), SHIP)
    priority = lambda r: r["priority"]  # noqa: E731
    clauses = [
        SHIP_BOUND,
        Scoped((_ship_sum(), Reduce("max", lambda r: {"ship": 1})), "<=", (lambda r: {CONSTANT: 30},), when=priority),
    ]
    ships = _judged(decidb_cli, scope_solver, sql, model, clauses, MAXIMIZE_SHIP)
    assert ships[1] == 20 and ships[3] == 15


@pytest.mark.correctness
def test_real_bound_side_values(decidb_cli, scope_solver):
    """A REAL decision against reducers that read only part of the rows."""
    sql = f"""
        SELECT shipmentID, reserve {from_clause()}
        DECIDE per D.depotID: reserve(REAL)
        SUCH THAT per D.depotID: reserve >= 0.10 * sum(S.demand) by (D.region) + avg(S.demand) by (D.country)
        MINIMIZE SUM(reserve)
        ORDER BY shipmentID"""
    model = ScopedModel(joined(), [Decision("reserve", ("depotID",), VarType.CONTINUOUS)])
    demand = lambda r: {CONSTANT: r["demand"]}  # noqa: E731
    clauses = [
        Scoped(
            (lambda r: {"reserve": 1},),
            ">=",
            (Reduce("sum", demand, by=("region",), factor=lambda r: 0.10), Reduce("avg", demand, by=("country",))),
            per=("depotID",),
        )
    ]
    objective = ScopedObjective(ObjSense.MINIMIZE, (Reduce("sum", lambda r: {"reserve": 1}),))
    result = aligned(answer(decidb_cli, sql), model)
    assert model.judge_scoped(result, clauses, objective) == pytest.approx(
        model.solve_scoped(scope_solver, clauses, objective), abs=1e-4
    )
