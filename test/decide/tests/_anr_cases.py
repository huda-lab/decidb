"""The fixtures of the full ANR language (``context/descriptions/04_testing/scope/done.md``,
"``_anr_cases.py``"), each stated twice: as the query in the language
and as ``_anr_oracle`` expressions, with the answer computed by hand.

``Case.phase`` is the first phase whose engine runs ``Case.sql``; until then a case is
checked against the oracle alone (``test_anr_oracle_selfcheck.py``). Each phase's suite
runs the cases of its phase through DeciDB and judges the answer with ``Case.model()``.

Rendered instances (``Case.instances``) are the oracle's spelling of each generated
comparison: a per-row decision ``x`` is ``x1``, ``x2``, ... by row, a keyed one
``x[key value]``, a query-wide one ``x``; terms in a fixed order, the first positive.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Mapping

from ._anr_oracle import (
    FIRST,
    GLOBAL,
    LAST,
    PER_ROW,
    Abs,
    AnrModel,
    At,
    Col,
    Column,
    Const,
    Decision,
    Objective,
    Over,
    Range,
    Scope,
    Var,
    clause,
    ge,
    le,
    next_,
    previous,
    reduce,
)
from ._scope_cases import literal, table
from solver.types import ObjSense, SolverStatus, VarType

MAXIMIZE, MINIMIZE = ObjSense.MAXIMIZE, ObjSense.MINIMIZE


@dataclass(frozen=True)
class Case:
    name: str
    #: The deck pages or the plan decision the case pins.
    source: str
    #: The first phase whose engine runs ``sql``.
    phase: int
    columns: tuple
    data: tuple
    decisions: tuple
    clauses: tuple
    objective: Objective | None
    sql: str
    #: The optimum, computed by hand.
    optimum: float | None = None
    #: Clause index -> the instances it generates, rendered, in generation order.
    instances: Mapping[int, tuple[str, ...]] = field(default_factory=dict)
    #: A ``ScopeError`` kind the data raises, instead of an answer.
    error: str | None = None
    status: SolverStatus = SolverStatus.OPTIMAL
    #: Small domains for an exhaustive cross-check of ``solve``.
    domains: Mapping[str, range] | None = None

    @property
    def rows(self) -> list[dict]:
        return [dict(zip(self.columns, row)) for row in self.data]

    def model(self) -> AnrModel:
        return AnrModel(self.rows, self.decisions)


def _frame_sql(alias: str, columns, data) -> str:
    return table(alias, columns, data)


# ---------------------------------------------------------------------------
# F1 -- the running example (deck pp. 79-97, 111-119)
# ---------------------------------------------------------------------------

F1_COLUMNS = ("tid", "id", "rid", "m", "cid", "p", "d", "c")
F1_DATA = (
    (1, "d1", "East", "Air", "c1", True, 20, 30),
    (2, "d1", "East", "Air", "c1", True, 15, 30),
    (3, "d1", "West", "Ground", "c2", True, 10, 50),
    (4, "d1", "West", "Ground", "c3", False, 5, 50),
    (5, "d2", "West", "Air", "c4", True, 25, 45),
    (6, "d2", "West", "Air", "c4", True, 15, 45),
)
SHIP = Var("ship")


def _f1(inner: str, optimum: float, rendered: tuple[str, ...]) -> Case:
    regional = reduce("sum", reduce(inner, SHIP, by=("cid",)), when=lambda r: r["p"], per=("cid",), by=("rid",))
    allowance = 0.10 * reduce("sum", Col("d"), by=("m",))
    sql = f"""
        SELECT tid, ship {table('t', F1_COLUMNS, F1_DATA)}
        DECIDE ship(INT)
        SUCH THAT ship <= 100 AND
            per t.id: sum(when t.p per t.cid: {inner}(ship) by (t.cid)) by (t.rid)
                      + 0.10 * sum(t.d) by (t.m) <= t.c
        MAXIMIZE sum(ship)
        ORDER BY tid"""
    return Case(
        f"F1_{inner}",
        "deck pp. 97, 115" if inner == "max" else "deck p. 119",
        2,
        F1_COLUMNS,
        F1_DATA,
        (Decision("ship"),),
        (clause(le(SHIP, 100)), clause(le(regional + allowance, Col("c")), per=("id",))),
        Objective(MAXIMIZE, reduce("sum", SHIP)),
        sql,
        optimum,
        {1: rendered},
    )


F1_MAX = _f1(
    "max", 218, ("max(ship1, ship2) <= 22.5", "ship3 + max(ship5, ship6) <= 48.5", "ship3 + max(ship5, ship6) <= 37.5")
)
F1_SUM = _f1("sum", 159, ("ship1 + ship2 <= 22.5", "ship3 + ship5 + ship6 <= 48.5", "ship3 + ship5 + ship6 <= 37.5"))

# ---------------------------------------------------------------------------
# F2 -- previous inventory (deck pp. 99-108)
# ---------------------------------------------------------------------------

F2_COLUMNS = ("tid", "product", "period", "cap")
F2_DATA = ((1, "A", 1, 5), (2, "A", 2, 3), (3, "B", 1, 1), (4, "B", 2, 4), (5, "B", 3, 8), (6, "B", 4, 2))
INVENTORY = Var("inventory")


def _f2(name, frame_sql, over, sense, position, optimum, rendered) -> Case:
    frame = At(position, INVENTORY, over)
    body = ge(INVENTORY, frame) if sense == ">=" else le(INVENTORY, frame)
    selector = "previous" if position.where == "previous" else "next"
    sql = f"""
        SELECT tid, inventory {table('t', F2_COLUMNS, F2_DATA)}
        DECIDE inventory(INT)
        SUCH THAT inventory <= t.cap AND
            inventory {sense} at({selector}: inventory) over ({frame_sql})
        MAXIMIZE sum(inventory)
        ORDER BY tid"""
    return Case(
        name,
        "deck pp. 99-108",
        4,
        F2_COLUMNS,
        F2_DATA,
        (Decision("inventory"),),
        (clause(le(INVENTORY, Col("cap"))), clause(body)),
        Objective(MAXIMIZE, reduce("sum", INVENTORY)),
        sql,
        optimum,
        {1: rendered},
        domains={"inventory": range(0, 6)},
    )


F2_PREVIOUS = _f2(
    "F2_previous",
    "t.period within t.product",
    Over("period", within=("product",)),
    ">=",
    previous(),
    13,
    (
        "inventory1 - inventory2 <= 0",
        "inventory3 - inventory4 <= 0",
        "inventory4 - inventory5 <= 0",
        "inventory5 - inventory6 <= 0",
    ),
)
F2_CYCLIC = _f2(
    "F2_cyclic",
    "t.period cyclic within t.product",
    Over("period", cyclic=True, within=("product",)),
    ">=",
    previous(),
    10,
    (
        "inventory1 - inventory2 >= 0",
        "inventory1 - inventory2 <= 0",
        "inventory3 - inventory6 >= 0",
        "inventory3 - inventory4 <= 0",
        "inventory4 - inventory5 <= 0",
        "inventory5 - inventory6 <= 0",
    ),
)
F2_NEXT = _f2(
    "F2_next",
    "t.period within t.product",
    Over("period", within=("product",)),
    "<=",
    next_(),
    13,
    (
        "inventory1 - inventory2 <= 0",
        "inventory3 - inventory4 <= 0",
        "inventory4 - inventory5 <= 0",
        "inventory5 - inventory6 <= 0",
    ),
)
#: ``previous`` under ``desc`` is ``next`` under ``asc``: x1 >= x2 and x3 >= x4 >= x5 >= x6.
F2_DESC = _f2(
    "F2_desc",
    "t.period desc within t.product",
    Over("period", desc=True, within=("product",)),
    ">=",
    previous(),
    12,
    (
        "inventory1 - inventory2 >= 0",
        "inventory3 - inventory4 >= 0",
        "inventory4 - inventory5 >= 0",
        "inventory5 - inventory6 >= 0",
    ),
)

# ---------------------------------------------------------------------------
# F3 -- variant signatures (deck pp. 34-36)
# ---------------------------------------------------------------------------

F3_COLUMNS = ("depotID", "region", "shipment", "lim")
F3_DATA = (
    ("D1", "East", "S1", 100),
    ("D1", "East", "S2", 90),
    ("D1", "West", "S3", 80),
    ("D1", "West", "S4", 80),
    ("D2", "East", "S5", 50),
    ("D2", "East", "S6", 50),
)


def _f3(name, by, by_sql, optimum, rendered, source) -> Case:
    sql = f"""
        SELECT shipment, ship {table('P', F3_COLUMNS, F3_DATA)}
        DECIDE ship(INT)
        SUCH THAT per P.depotID: sum(ship) by ({by_sql}) <= P.lim
        MAXIMIZE sum(ship)
        ORDER BY shipment"""
    return Case(
        name,
        source,
        1,
        F3_COLUMNS,
        F3_DATA,
        (Decision("ship"),),
        (clause(le(reduce("sum", SHIP, by=by), Col("lim")), per=("depotID",)),),
        Objective(MAXIMIZE, reduce("sum", SHIP)),
        sql,
        optimum,
        {0: rendered},
    )


F3_REGION = _f3(
    "F3_region",
    ("region",),
    "P.region",
    130,
    (
        "ship1 + ship2 + ship5 + ship6 <= 100",
        "ship1 + ship2 + ship5 + ship6 <= 90",
        "ship3 + ship4 <= 80",
        "ship1 + ship2 + ship5 + ship6 <= 50",
    ),
    "deck p. 35",
)
F3_REGION_LIMIT = _f3(
    "F3_region_limit",
    ("region", "lim"),
    "P.region, P.lim",
    320,
    ("ship1 <= 100", "ship2 <= 90", "ship3 + ship4 <= 80", "ship5 + ship6 <= 50"),
    "deck p. 36",
)

# ---------------------------------------------------------------------------
# F4 -- the cyclic ring (deck p. 67)
# ---------------------------------------------------------------------------

F4_COLUMNS = ("ring", "idx", "cap")
F4_DATA = (("L", 1, 1), ("L", 2, 10), ("L", 3, 4))
AMOUNT = Var("amount")


def _f4(cyclic: bool, optimum: float) -> Case:
    over = Over("idx", cyclic=cyclic, within=("ring",))
    sql = f"""
        SELECT idx, amount {table('s', F4_COLUMNS, F4_DATA)}
        DECIDE amount(INT)
        SUCH THAT amount <= s.cap AND
            abs(amount - at(previous: amount) over (s.idx {'cyclic ' if cyclic else ''}within s.ring)) <= 2
        MAXIMIZE sum(amount)
        ORDER BY idx"""
    return Case(
        "F4_cyclic" if cyclic else "F4_open",
        "deck p. 67",
        4,
        F4_COLUMNS,
        F4_DATA,
        (Decision("amount"),),
        (clause(le(AMOUNT, Col("cap"))), clause(le(Abs(AMOUNT - At(previous(), AMOUNT, over)), 2))),
        Objective(MAXIMIZE, reduce("sum", AMOUNT)),
        sql,
        optimum,
        domains={"amount": range(0, 11)},
    )


F4_CYCLIC = _f4(True, 7)
F4_OPEN = _f4(False, 8)

# ---------------------------------------------------------------------------
# F5 -- the weekly step (deck p. 67)
# ---------------------------------------------------------------------------

F5_COLUMNS = ("contract", "dayno")
F5_DATA = tuple(("K", day) for day in range(1, 29))
DELIVERY = Var("delivery")
F5_OVER = Over("dayno", within=("contract",))


def _f5(name, start, start_sql) -> Case:
    weekly = Range("sum", start, LAST, DELIVERY, F5_OVER, every=7)
    sql = f"""
        SELECT dayno, delivery {table('d', F5_COLUMNS, F5_DATA)}
        DECIDE delivery(INT)
        SUCH THAT delivery <= 5 AND
            sum(from {start_sql} to last every 7: delivery) over (d.dayno within d.contract) <= 10
        MAXIMIZE sum(delivery)
        ORDER BY dayno"""
    return Case(
        name,
        "deck p. 67",
        5,
        F5_COLUMNS,
        F5_DATA,
        (Decision("delivery"),),
        (clause(le(DELIVERY, 5)), clause(le(weekly, 10))),
        Objective(MAXIMIZE, reduce("sum", DELIVERY)),
        sql,
        130 if start == FIRST else None,
    )


#: Every anchor generates the same row, days 1, 8, 15 and 22, which collapses to one.
F5_WEEKLY = _f5("F5_weekly", FIRST, "first")
F5_WEEKLY_ROW = "delivery1 + delivery8 + delivery15 + delivery22 <= 10"
#: From day 1, ``from 2 next to last every 7`` samples days 3, 10, 17 and 24.
F5_OFFSET = _f5("F5_offset", next_(2), "2 next")
F5_OFFSET_FIRST_ROW = "delivery3 + delivery10 + delivery17 + delivery24 <= 10"

# ---------------------------------------------------------------------------
# F6 -- rolling demand and its boundary policies (deck pp. 45, 49, 68)
# ---------------------------------------------------------------------------

F6_COLUMNS = ("product", "period")
F6_DATA = (("A", 1), ("A", 2), ("A", 3), ("A", 4))
PRODUCE = Var("produce")


def _f6(name, policy_sql, optimum, **policy) -> Case:
    rolling = Range("sum", previous(), next_(), PRODUCE, Over("period", within=("product",)), **policy)
    sql = f"""
        SELECT period, produce {table('t', F6_COLUMNS, F6_DATA)}
        DECIDE produce(INT)
        SUCH THAT sum(from previous to next{policy_sql}: produce) over (t.period within t.product) >= 6
        MINIMIZE sum(produce)
        ORDER BY period"""
    return Case(
        name,
        "deck pp. 45, 49",
        5,
        F6_COLUMNS,
        F6_DATA,
        (Decision("produce"),),
        (clause(ge(rolling, 6)),),
        Objective(MINIMIZE, reduce("sum", PRODUCE)),
        sql,
        optimum,
        domains={"produce": range(0, 7)},
    )


F6_TRUNCATE = _f6("F6_truncate", "", 12)
F6_ALL = _f6("F6_all", " all", 6, all=True)
F6_ELSE = _f6("F6_else", " else 1", 10, fallback=Const(1.0))

# ---------------------------------------------------------------------------
# F7-F9 -- peers, NULL order keys, k > 1
# ---------------------------------------------------------------------------

F7_COLUMNS = ("id", "product", "period")
F7_DATA = ((1, "A", 1), (2, "A", 1), (3, "A", 2))
X, Y = Var("x"), Var("y")
F7_OVER = Over("period", within=("product",))
F7_POINT = Case(
    "F7_point",
    "D67",
    4,
    F7_COLUMNS,
    F7_DATA,
    (Decision("x"),),
    (clause(ge(X, At(previous(), X, F7_OVER))),),
    Objective(MAXIMIZE, reduce("sum", X)),
    f"""SELECT id, x {table('t', F7_COLUMNS, F7_DATA)} DECIDE x(INT)
        SUCH THAT x <= 3 AND x >= at(previous: x) over (t.period within t.product)
        MAXIMIZE sum(x) ORDER BY id""",
    error="ambiguous_peer",
)
F7_RANGE = Case(
    "F7_range",
    "D67",
    5,
    F7_COLUMNS,
    F7_DATA,
    (Decision("x"),),
    (clause(le(X, 3)), clause(ge(X, Range("sum", previous(), previous(), X, F7_OVER)))),
    Objective(MAXIMIZE, reduce("sum", X)),
    f"""SELECT id, x {table('t', F7_COLUMNS, F7_DATA)} DECIDE x(INT)
        SUCH THAT x <= 3 AND x >= sum(from previous to previous: x) over (t.period within t.product)
        MAXIMIZE sum(x) ORDER BY id""",
    6,
    {1: ("x1 + x2 - x3 <= 0",)},
)

F8_COLUMNS = ("id", "period", "cap")
F8_DATA = ((1, 2, 10), (2, None, 3), (3, 1, 7))


def _f8(name, position, desc, bound) -> Case:
    word = "first" if position == FIRST else "last"
    sql = f"""SELECT id, x {table('t', F8_COLUMNS, F8_DATA)} DECIDE x(INT)
        SUCH THAT x <= at({word}: t.cap) over (t.period{' desc' if desc else ''})
        MAXIMIZE sum(x) ORDER BY id"""
    return Case(
        name,
        "D68",
        4,
        F8_COLUMNS,
        F8_DATA,
        (Decision("x"),),
        (clause(le(X, At(position, Col("cap"), Over("period", desc=desc)))),),
        Objective(MAXIMIZE, reduce("sum", X)),
        sql,
        3 * bound,
        {0: tuple(f"x{i} <= {bound}" for i in (1, 2, 3))},
    )


#: A NULL period sorts last in either direction, so ``last`` reads it (cap 3).
F8_LAST = _f8("F8_last", LAST, False, 3)
F8_LAST_DESC = _f8("F8_last_desc", LAST, True, 3)
F8_FIRST = _f8("F8_first", FIRST, False, 7)
F8_FIRST_DESC = _f8("F8_first_desc", FIRST, True, 10)

F9_COLUMNS = ("id", "period", "v")
F9_DATA = tuple((i, i, i) for i in range(1, 6))
F9 = Case(
    "F9",
    "D71",
    4,
    F9_COLUMNS,
    F9_DATA,
    (Decision("x"), Decision("y")),
    (clause(ge(X, Col("v"))), clause(ge(Y, At(previous(2), X, Over("period"), fallback=Const(0.0))))),
    Objective(MINIMIZE, reduce("sum", Y)),
    f"""SELECT id, x, y {table('t', F9_COLUMNS, F9_DATA)} DECIDE x(INT), y(INT)
        SUCH THAT x >= t.v AND y >= at(2 previous else 0: x) over (t.period)
        MINIMIZE sum(y) ORDER BY id""",
    6,
    {1: ("y1 >= 0", "y2 >= 0", "x1 - y3 <= 0", "x2 - y4 <= 0", "x3 - y5 <= 0")},
)

# ---------------------------------------------------------------------------
# F10, F20 -- scalar fields (D77)
# ---------------------------------------------------------------------------

F10_COLUMNS = ("id", "g", "a", "b")
F10_DATA = ((1, "a", 1, 2), (2, "a", 2, 1))
KEYED_X = Decision("x", ("g",))


def _f10(name, bound, bound_sql, phase, optimum, rendered) -> Case:
    sql = f"""
        SELECT id, x {table('t', F10_COLUMNS, F10_DATA)}
        DECIDE per t.g: x(INT)
        SUCH THAT per t.g: x <= {bound_sql}
        MAXIMIZE sum(x)
        ORDER BY id"""
    return Case(
        name,
        "D77",
        phase,
        F10_COLUMNS,
        F10_DATA,
        (KEYED_X,),
        (clause(le(X, bound), per=("g",)),),
        Objective(MAXIMIZE, reduce("sum", X)),
        sql,
        optimum,
        {0: rendered},
    )


#: ``a + b`` is one field with one value, 3: the clause is determined and runs today.
F10_SUM = _f10("F10_sum", Col("a") + Col("b"), "t.a + t.b", 0, 6, ("x[a] <= 3",))
#: ``a`` takes two values in the class: two instances.
F10_SPLIT = _f10("F10_split", Col("a"), "t.a", 1, 2, ("x[a] <= 1", "x[a] <= 2"))


def _f20(name, bound, bound_sql) -> Case:
    sql = f"""
        SELECT id, x, y {table('t', F10_COLUMNS, F10_DATA)}
        DECIDE per t.g: x(INT), y(INT)
        SUCH THAT y <= 1 AND per t.g: x <= {bound_sql}
        MAXIMIZE sum(x)
        ORDER BY id"""
    return Case(
        name,
        "D77",
        1,
        F10_COLUMNS,
        F10_DATA,
        (KEYED_X, Decision("y")),
        (clause(le(Y, 1)), clause(le(X, bound), per=("g",))),
        Objective(MAXIMIZE, reduce("sum", X)),
        sql,
        10,
        {1: ("x[a] - y1 - y2 <= 3",)},
    )


#: Written associativity does not change the fields: both spellings are one instance.
F20_SPLIT_TERMS = _f20("F20_split_terms", Col("a") + reduce("sum", Y) + Col("b"), "t.a + sum(y) + t.b")
F20_JOINED_TERMS = _f20("F20_joined_terms", Col("a") + Col("b") + reduce("sum", Y), "t.a + t.b + sum(y)")

# ---------------------------------------------------------------------------
# F11 -- join fan-out and the inner per (D65)
# ---------------------------------------------------------------------------

F11_COLUMNS = ("route", "depot", "cost", "unit")
F11_DATA = (("R1", "D1", 10, 2), ("R2", "D1", 10, 3), ("R3", "D2", 20, 1))
OPEN = Var("open")
F11_DECISIONS = (Decision("open", ("depot",), VarType.BINARY), Decision("ship"))
F11_FROM = (
    "FROM (VALUES ('D1', 10), ('D2', 20)) D(depot, cost) "
    "JOIN (VALUES ('R1', 'D1', 2), ('R2', 'D1', 3), ('R3', 'D2', 1)) T(route, depot, unit) USING (depot)"
)
#: ``per D`` keys on every stored column of D.
PER_D = ("depot", "cost")


def _f11(name, agg, body, body_sql, per, per_sql, phase, optimum, rendered) -> Case:
    sql = f"""
        SELECT route, open, ship {F11_FROM}
        DECIDE per D.depot: open(BOOL), ship(INT)
        SUCH THAT ship <= 5 AND {agg}({per_sql}{body_sql}) <= 25
        MAXIMIZE sum(ship)
        ORDER BY route"""
    limit = clause(le(reduce(agg, body, per=per), 25))
    return Case(
        name,
        "D65",
        phase,
        F11_COLUMNS,
        F11_DATA,
        F11_DECISIONS,
        (clause(le(SHIP, 5)), limit),
        Objective(MAXIMIZE, reduce("sum", SHIP)),
        sql,
        optimum,
        {1: rendered},
        domains={"open": range(0, 2), "ship": range(0, 6)},
    )


COST_OPEN = Col("cost") * OPEN
VARYING = Col("cost") * OPEN + Col("unit") * SHIP
#: Each depot once: its body is one value per class.
F11_DETERMINED = _f11(
    "F11_determined",
    "sum",
    COST_OPEN,
    "D.cost * open",
    PER_D,
    "per D: ",
    0,
    15,
    ("10 * open[D1] + 20 * open[D2] <= 25",) * 3,
)
#: Per row, D1's two joined rows count it twice.
F11_PER_ROW = _f11(
    "F11_per_row", "sum", COST_OPEN, "D.cost * open", PER_ROW, "", 0, 15, ("20 * open[D1] + 20 * open[D2] <= 25",) * 3
)
#: The trap: a body that varies within D1 gives two variants, so D1 counts twice again.
F11_VARYING = _f11(
    "F11_varying",
    "sum",
    VARYING,
    "D.cost * open + T.unit * ship",
    PER_D,
    "per D: ",
    1,
    13,
    ("20 * open[D1] + 20 * open[D2] + 2 * ship1 + 3 * ship2 + ship3 <= 25",) * 3,
)
#: AVG divides by the members it receives: three variants, not two classes.
F11_AVERAGE = _f11(
    "F11_average",
    "avg",
    VARYING,
    "D.cost * open + T.unit * ship",
    PER_D,
    "per D: ",
    1,
    None,
    ("6.666666667 * open[D1] + 6.666666667 * open[D2] + 0.666666667 * ship1 + ship2" " + 0.333333333 * ship3 <= 25",)
    * 3,
)

# ---------------------------------------------------------------------------
# F12, F13 -- decisions in signatures, the objective produced once
# ---------------------------------------------------------------------------

#: A per-row decision under a key: one instance per row.
F12 = Case(
    "F12",
    "D64",
    1,
    F3_COLUMNS,
    F3_DATA,
    (Decision("ship"),),
    (clause(le(SHIP, 10), per=("depotID",)),),
    Objective(MAXIMIZE, reduce("sum", SHIP)),
    f"""SELECT shipment, ship {table('P', F3_COLUMNS, F3_DATA)} DECIDE ship(INT)
        SUCH THAT per P.depotID: ship <= 10 MAXIMIZE sum(ship) ORDER BY shipment""",
    60,
    {0: tuple(f"ship{i} <= 10" for i in range(1, 7))},
)

F13_COLUMNS = ("id", "w")
F13_DATA = ((1, 3), (2, 5))
S = Var("s")
F13 = Case(
    "F13",
    "D66",
    1,
    F13_COLUMNS,
    F13_DATA,
    (Decision("x"), Decision("s", GLOBAL)),
    (clause(le(X, 1)), clause(le(S, 1))),
    Objective(MAXIMIZE, reduce("sum", X) - Col("w") * S),
    f"""SELECT id, x, s {table('t', F13_COLUMNS, F13_DATA)} DECIDE x(INT), per (): s(INT)
        SUCH THAT x <= 1 AND s <= 1 MAXIMIZE sum(x) - t.w * s ORDER BY id""",
    error="objective_variants",
)

# ---------------------------------------------------------------------------
# F14, F16 -- nested reducers
# ---------------------------------------------------------------------------

F14_COLUMNS = ("id", "g", "h", "v")
F14_DATA = ((1, "a", "p", 4), (2, "a", "p", 6), (3, "a", "q", 2), (4, "b", "p", 9))
F14 = Case(
    "F14",
    "§3.3",
    2,
    F14_COLUMNS,
    F14_DATA,
    (Decision("x"),),
    (
        clause(
            le(
                reduce("sum", X, by=("g",)),
                reduce("avg", reduce("max", Col("v"), by=("g", "h")), per=("h",), by=("g",)),
            )
        ),
    ),
    Objective(MAXIMIZE, reduce("sum", X)),
    f"""SELECT id, x {table('t', F14_COLUMNS, F14_DATA)} DECIDE x(INT)
        SUCH THAT sum(x) by (t.g) <= avg(per t.h: max(t.v) by (t.g, t.h)) by (t.g)
        MAXIMIZE sum(x) ORDER BY id""",
    13,
    {0: ("x1 + x2 + x3 <= 4",) * 3 + ("x4 <= 9",)},
)

F16_COLUMNS = ("id", "g", "h", "k", "cap")


def _f16(name, caps, ub, optimum, domains=None) -> Case:
    data = tuple((1 + 4 * g + 2 * h + k, g, h, k, caps[g]) for g in (0, 1) for h in (0, 1) for k in (0, 1))
    nested = reduce(
        "sum", reduce("max", reduce("sum", X, by=("g", "h", "k")), per=("k",), by=("g", "h")), per=("h",), by=("g",)
    )
    sql = f"""
        SELECT id, x {table('t', F16_COLUMNS, data)}
        DECIDE x(INT)
        SUCH THAT x <= {ub} AND
            per t.g: sum(per t.h: max(per t.k: sum(x) by (t.g, t.h, t.k)) by (t.g, t.h)) by (t.g) <= t.cap
        MAXIMIZE sum(x)
        ORDER BY id"""
    return Case(
        name,
        "§3.3",
        2,
        F16_COLUMNS,
        data,
        (Decision("x"),),
        (clause(le(X, ub)), clause(le(nested, Col("cap")), per=("g",))),
        Objective(MAXIMIZE, reduce("sum", X)),
        sql,
        optimum,
        {1: (f"max(x1, x2) + max(x3, x4) <= {caps[0]}", f"max(x5, x6) + max(x7, x8) <= {caps[1]}")},
        domains=domains,
    )


F16 = _f16("F16", (12, 7), 10, 38)
F16_SMALL = _f16("F16_small", (4, 3), 3, 14, {"x": range(0, 4)})

# ---------------------------------------------------------------------------
# F15, F17-F21
# ---------------------------------------------------------------------------

F15_COLUMNS = ("product", "period", "d")
F15_DATA = (("A", 1, 2), ("A", 2, 5), ("A", 3, 1), ("A", 4, 4))
F15 = Case(
    "F15",
    "§3.7",
    4,
    F15_COLUMNS,
    F15_DATA,
    (Decision("x"),),
    (clause(ge(X, Col("d"))),),
    Objective(
        MINIMIZE, reduce("sum", Abs(X - At(previous(), X, Over("period", within=("product",)), fallback=Const(0.0))))
    ),
    f"""SELECT period, x {table('t', F15_COLUMNS, F15_DATA)} DECIDE x(INT)
        SUCH THAT x >= t.d
        MINIMIZE sum(abs(x - at(previous else 0: x) over (t.period within t.product))) ORDER BY period""",
    5,
    domains={"x": range(0, 6)},
)

#: F3 with every ship at least 1 and D2's limit 0: D2's East variant cannot hold.
F17_DATA = tuple(row if row[0] == "D1" else row[:3] + (0,) for row in F3_DATA)
F17 = Case(
    "F17",
    "§6.3",
    6,
    F3_COLUMNS,
    F17_DATA,
    (Decision("ship"),),
    (clause(ge(SHIP, 1)), clause(le(reduce("sum", SHIP, by=("region",)), Col("lim")), per=("depotID",))),
    Objective(MAXIMIZE, reduce("sum", SHIP)),
    f"""DIAGNOSE SELECT shipment, ship {table('P', F3_COLUMNS, F17_DATA)} DECIDE ship(INT)
        SUCH THAT ship >= 1 AND per P.depotID: sum(ship) by (P.region) <= P.lim
        MAXIMIZE sum(ship) ORDER BY shipment""",
    status=SolverStatus.INFEASIBLE,
    instances={
        1: (
            "ship1 + ship2 + ship5 + ship6 <= 100",
            "ship1 + ship2 + ship5 + ship6 <= 90",
            "ship3 + ship4 <= 80",
            "ship1 + ship2 + ship5 + ship6 <= 0",
        )
    },
)

F18_COLUMNS = ("id", "product", "period")
F18_DATA = ((1, "A", 1), (2, "A", 2), (3, "A", 3))
F18_FRAME = At(previous(), X, Over("period", within=("product",)))


def _f18(name, extra, extra_sql, optimum, status) -> Case:
    sql = f"""
        SELECT id, x {table('t', F18_COLUMNS, F18_DATA)}
        DECIDE x(INT)
        SUCH THAT at(previous: x) over (t.period within t.product) >= 10 AND x <= 10{extra_sql}
        MAXIMIZE sum(x)
        ORDER BY id"""
    return Case(
        name,
        "§3.5",
        4,
        F18_COLUMNS,
        F18_DATA,
        (Decision("x"),),
        (clause(ge(F18_FRAME, 10)), clause(le(X, 10))) + extra,
        Objective(MAXIMIZE, reduce("sum", X)),
        sql,
        optimum,
        {0: ("x1 >= 10", "x2 >= 10"), 1: ("x1 <= 10", "x2 <= 10", "x3 <= 10")},
        status=status,
    )


F18 = _f18("F18", (), "", 30, SolverStatus.OPTIMAL)
#: The first period skips the frame comparison but keeps its cap, so x1 >= 11 is infeasible.
F18_CAP_KEPT = _f18(
    "F18_cap_kept",
    (clause(ge(X, 11), when=lambda r: r["id"] == 1),),
    " AND when t.id = 1: x >= 11",
    None,
    SolverStatus.INFEASIBLE,
)

F19_COLUMNS = ("product", "period", "demand")
F19_DATA = (("A", 1, 5), ("A", 2, 5), ("A", 3, 5), ("A", 4, 5))
#: The order key is a field of the inner ``per product`` class: three present members,
#: although their values are equal (D76). Comparing frames by value would give 5.
F19 = Case(
    "F19",
    "D76",
    4,
    F19_COLUMNS,
    F19_DATA,
    (Decision("x"),),
    (
        clause(
            le(
                reduce("sum", X),
                reduce("sum", At(previous(), Col("demand"), Over("period", within=("product",))), per=("product",)),
            ),
            per=GLOBAL,
        ),
    ),
    Objective(MAXIMIZE, reduce("sum", X)),
    f"""SELECT period, x {table('t', F19_COLUMNS, F19_DATA)} DECIDE x(INT)
        SUCH THAT per (): sum(x) <= sum(per t.product: at(previous: t.demand) over (t.period within t.product))
        MAXIMIZE sum(x) ORDER BY period""",
    15,
    {0: ("x1 + x2 + x3 + x4 <= 15",)},
)

F21_COLUMNS = ("g", "period", "cap")
F21_DATA = (("a", 1, 9), ("a", 2, 4), ("a", 3, 7))
#: An absent anchor contributes no bound; the group takes the tightest present one (D79).
F21 = Case(
    "F21",
    "D79",
    4,
    F21_COLUMNS,
    F21_DATA,
    (Decision("x"),),
    (clause(le(reduce("sum", X, by=("g",)), At(previous(), Col("cap"), Over("period", within=("g",))))),),
    Objective(MAXIMIZE, reduce("sum", X)),
    f"""SELECT period, x {table('t', F21_COLUMNS, F21_DATA)} DECIDE x(INT)
        SUCH THAT sum(x) by (t.g) <= at(previous: t.cap) over (t.period within t.g)
        MAXIMIZE sum(x) ORDER BY period""",
    4,
    {0: ("x1 + x2 + x3 <= 9", "x1 + x2 + x3 <= 4")},
)

# ---------------------------------------------------------------------------
# Edge cases beyond the F-list: laziness, absence, partitions, repeats, COUNT, errors
# ---------------------------------------------------------------------------

LAZY_COLUMNS = ("id", "period", "s")


def _cast_to_integer(text: str) -> int:
    """SQL's ``CAST(s AS INTEGER)``: a text that is not a number is a conversion error."""
    return int(text)  # ValueError, as DuckDB raises a Conversion Error


def _lazy(name, s, error) -> Case:
    data = tuple((i, i, si) for i, si in zip((1, 2, 3), s))
    fallback = Col(lambda r: _cast_to_integer(r["s"]))
    return Case(
        name,
        "§3.4",
        4,
        LAZY_COLUMNS,
        data,
        (Decision("x"),),
        (clause(le(X, 3)), clause(ge(X, At(previous(), Const(1.0), Over("period"), fallback=fallback)))),
        Objective(MAXIMIZE, reduce("sum", X)),
        f"""SELECT id, x {table('t', LAZY_COLUMNS, data)} DECIDE x(INT)
                    SUCH THAT x <= 3 AND x >= at(previous else CAST(t.s AS INTEGER): 1) over (t.period)
                    MAXIMIZE sum(x) ORDER BY id""",
        None if error else 9,
        error=error,
    )


#: The fallback is reached only at period 1, where s = '1': the text elsewhere is never cast.
LAZY_UNUSED = _lazy("lazy_unused", ("1", "none", "none"), None)
#: Reached at period 1, where s = 'none': the cast fails (``error`` is "conversion").
LAZY_REACHED = _lazy("lazy_reached", ("none", "1", "1"), "conversion")

PERIODS_COLUMNS = ("id", "period", "v")
PERIODS_DATA = ((1, 1, 1), (2, 2, 2), (3, 3, 3))
PERIOD = Over("period")
#: A reversed first/last range selects nothing, so the objective has no value (§3.5).
OBJECTIVE_ABSENT = Case(
    "objective_absent",
    "§3.5",
    5,
    PERIODS_COLUMNS,
    PERIODS_DATA,
    (Decision("x"),),
    (clause(le(X, 1)),),
    Objective(MAXIMIZE, reduce("sum", X) + Range("sum", LAST, FIRST, X, PERIOD)),
    f"""SELECT id, x {table('t', PERIODS_COLUMNS, PERIODS_DATA)} DECIDE x(INT)
        SUCH THAT x <= 1 MAXIMIZE sum(x) + sum(from last to first: x) over (t.period) ORDER BY id""",
    error="objective_absent",
)
#: Cyclic repeats are kept (D70): on three peers, from 4 previous to 4 next is nine positions.
CYCLIC_REPEATS = Case(
    "cyclic_repeats",
    "D70",
    5,
    PERIODS_COLUMNS,
    PERIODS_DATA,
    (Decision("x"),),
    (
        clause(le(X, Range("count", previous(4), next_(4), Col("v"), Over("period", cyclic=True)))),
        clause(le(X, Range("sum", previous(4), next_(4), Col("v"), Over("period", cyclic=True)))),
    ),
    Objective(MAXIMIZE, reduce("sum", X)),
    f"""SELECT id, x {table('t', PERIODS_COLUMNS, PERIODS_DATA)} DECIDE x(INT)
        SUCH THAT x <= count(from 4 previous to 4 next: t.v) over (t.period cyclic)
              AND x <= sum(from 4 previous to 4 next: t.v) over (t.period cyclic)
        MAXIMIZE sum(x) ORDER BY id""",
    27,
    {0: ("x1 <= 9", "x2 <= 9", "x3 <= 9"), 1: ("x1 <= 18", "x2 <= 18", "x3 <= 18")},
)
#: A reversed range is empty, so every instance is skipped; a single peer is not reversed.
REVERSED = Case(
    "reversed",
    "§3.4",
    5,
    PERIODS_COLUMNS,
    PERIODS_DATA,
    (Decision("x"),),
    (clause(le(X, 1)), clause(ge(X, Range("sum", LAST, FIRST, Col("v"), PERIOD)))),
    Objective(MAXIMIZE, reduce("sum", X)),
    f"""SELECT id, x {table('t', PERIODS_COLUMNS, PERIODS_DATA)} DECIDE x(INT)
        SUCH THAT x <= 1 AND x >= sum(from last to first: t.v) over (t.period)
        MAXIMIZE sum(x) ORDER BY id""",
    3,
    {1: ()},
)
#: COUNT over a frame that selects nothing is 0, as SQL counts an empty set.
COUNT_EMPTY = Case(
    "count_empty",
    "§3.3",
    5,
    PERIODS_COLUMNS,
    PERIODS_DATA,
    (Decision("x"),),
    (clause(le(X, Range("count", next_(5), next_(6), Col("v"), PERIOD))),),
    Objective(MAXIMIZE, reduce("sum", X)),
    f"""SELECT id, x {table('t', PERIODS_COLUMNS, PERIODS_DATA)} DECIDE x(INT)
        SUCH THAT x <= count(from 5 next to 6 next: t.v) over (t.period) MAXIMIZE sum(x) ORDER BY id""",
    0,
    {0: ("x1 <= 0", "x2 <= 0", "x3 <= 0")},
)

NULLS_COLUMNS = ("id", "v")
NULLS_DATA = ((1, 5), (2, None), (3, 7))
#: count(e) counts the non-NULL values. Today's engine refuses the NULL as a bound instead
#: (06_issues/bugs/todo.md, "count(e) over a NULL value is refused"); phase 2 rebuilds the
#: bound-side reducers and fixes it.
COUNT_NON_NULL = Case(
    "count_non_null",
    "§3.3",
    2,
    NULLS_COLUMNS,
    NULLS_DATA,
    (Decision("x"),),
    (clause(le(X, reduce("count", Col("v")))),),
    Objective(MAXIMIZE, reduce("sum", X)),
    f"""SELECT id, x {table('t', NULLS_COLUMNS, NULLS_DATA)} DECIDE x(INT)
        SUCH THAT x <= count(t.v) MAXIMIZE sum(x) ORDER BY id""",
    6,
    {0: ("x1 <= 2", "x2 <= 2", "x3 <= 2")},
)
#: A NULL read as a coefficient or bound is an error, as today.
NULL_BOUND = Case(
    "null_bound",
    "§3.5",
    0,
    NULLS_COLUMNS,
    NULLS_DATA,
    (Decision("x"),),
    (clause(le(X, Col("v"))),),
    Objective(MAXIMIZE, reduce("sum", X)),
    f"""SELECT id, x {table('t', NULLS_COLUMNS, NULLS_DATA)} DECIDE x(INT)
        SUCH THAT x <= t.v MAXIMIZE sum(x) ORDER BY id""",
    error="null_value",
)

PARTITION_COLUMNS = ("id", "category", "product", "period", "v")
PARTITION_DATA = ((1, "Food", "A", 1, 4), (2, "Food", "B", 1, 6))


def _partition(name, per, per_sql, error, optimum) -> Case:
    frame = At(FIRST, Col("v"), Over("period", within=("product",)))
    return Case(
        name,
        "D69",
        4,
        PARTITION_COLUMNS,
        PARTITION_DATA,
        (Decision("x"),),
        (clause(le(X, 10)), clause(ge(X, frame), per=per)),
        Objective(MINIMIZE, reduce("sum", X)),
        f"""SELECT id, x {table('t', PARTITION_COLUMNS, PARTITION_DATA)} DECIDE x(INT)
                    SUCH THAT x <= 10 AND per {per_sql}: x >= at(first: t.v) over (t.period within t.product)
                    MINIMIZE sum(x) ORDER BY id""",
        optimum,
        error=error,
    )


#: A category spans two product timelines: no timeline per class (D69).
PARTITION_SPANNED = _partition("partition_spanned", ("category",), "t.category", "frame_partition", None)
PARTITION_HELD = _partition("partition_held", ("product",), "t.product", None, 10)

NESTED_D8_COLUMNS = ("id", "g", "h", "c")
NESTED_D8_DATA = ((1, "a", "p", True), (2, "a", "q", False))
#: The inner reducer's own when empties h = 'q': D8, inside a nested reducer.
NESTED_D8 = Case(
    "nested_d8",
    "D8",
    2,
    NESTED_D8_COLUMNS,
    NESTED_D8_DATA,
    (Decision("x"),),
    (clause(le(reduce("sum", reduce("max", X, when=lambda r: r["c"], by=("h",)), per=("h",), by=("g",)), 5)),),
    Objective(MAXIMIZE, reduce("sum", X)),
    f"""SELECT id, x {table('t', NESTED_D8_COLUMNS, NESTED_D8_DATA)} DECIDE x(INT)
        SUCH THAT sum(per t.h: max(when t.c: x) by (t.h)) by (t.g) <= 5 MAXIMIZE sum(x) ORDER BY id""",
    error="reducer_empty",
)
KEPT_NONE = Case(
    "kept_none",
    "D4",
    0,
    NULLS_COLUMNS,
    NULLS_DATA,
    (Decision("x"),),
    (clause(le(X, 1), when=lambda r: r["id"] > 99),),
    Objective(MAXIMIZE, reduce("sum", X)),
    f"""SELECT id, x {table('t', NULLS_COLUMNS, NULLS_DATA)} DECIDE x(INT)
        SUCH THAT when t.id > 99: x <= 1 MAXIMIZE sum(x) ORDER BY id""",
    error="kept_no_rows",
)

#: Every case, in plan order.
CASES = (
    F1_MAX,
    F1_SUM,
    F2_PREVIOUS,
    F2_CYCLIC,
    F2_NEXT,
    F2_DESC,
    F3_REGION,
    F3_REGION_LIMIT,
    F4_CYCLIC,
    F4_OPEN,
    F5_WEEKLY,
    F5_OFFSET,
    F6_TRUNCATE,
    F6_ALL,
    F6_ELSE,
    F7_POINT,
    F7_RANGE,
    F8_LAST,
    F8_LAST_DESC,
    F8_FIRST,
    F8_FIRST_DESC,
    F9,
    F10_SUM,
    F10_SPLIT,
    F11_DETERMINED,
    F11_PER_ROW,
    F11_VARYING,
    F11_AVERAGE,
    F12,
    F13,
    F14,
    F15,
    F16,
    F16_SMALL,
    F17,
    F18,
    F18_CAP_KEPT,
    F19,
    F20_SPLIT_TERMS,
    F20_JOINED_TERMS,
    F21,
    LAZY_UNUSED,
    LAZY_REACHED,
    OBJECTIVE_ABSENT,
    CYCLIC_REPEATS,
    REVERSED,
    COUNT_EMPTY,
    COUNT_NON_NULL,
    NULL_BOUND,
    PARTITION_SPANNED,
    PARTITION_HELD,
    NESTED_D8,
    KEPT_NONE,
)

# ---------------------------------------------------------------------------
# Running a case through DeciDB with every decision fixed
# ---------------------------------------------------------------------------


def fixing_clauses(model: AnrModel, values: Mapping[Column, float], alias: str, id_column: str) -> str:
    """``values`` as clauses that fix every decision instance: ``when t.id = 3: x = 2``
    for a per-row decision, its key for a keyed one, ``x = 2`` for a query-wide one."""
    parts = []
    for column, value in values.items():
        decision = model.decisions[column.name]
        row = model.rows[model.representative(column)]
        if decision.key == GLOBAL:
            parts.append(f"{column.name} = {literal(value)}")
            continue
        keys = (id_column,) if decision.key is PER_ROW else decision.key
        condition = " and ".join(f"{alias}.{key} = {literal(row[key])}" for key in keys)
        parts.append(f"when {condition}: {column.name} = {literal(value)}")
    return " AND ".join(parts)


def decidb_feasible(cli, sql: str) -> bool:
    """Whether DeciDB finds the query feasible; any other error fails the caller."""
    result = cli.execute_raw(sql)
    if "is infeasible" in result.stderr:
        return False
    assert result.returncode == 0 and not result.stderr.strip(), result.stderr
    return True
