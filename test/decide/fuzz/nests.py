"""Reducers nested in reducers: ``when``, ``per`` and ``by`` at every level, over decisions and over data.

A query holds a nest over decisions on the left of a clause, or a nest read as a number at every row (``s = nest``):
a nest over data, or one over decisions that data pins. The first kind tries every assignment, so no helper and no
solver stand between the deck's definition (p. 30) and the expected optimum. A clause that does not bind proves little,
so its bound is set from the query's optimum without it. A ``per`` that changes nothing proves as little: a reducer
under one reads the decisions a class holds once, and a clause beside the nest counts one such decision once per class
(``counting``). An optimum moves with few of the values a nest takes, so the pinned kind reads every one of them.
"""

from __future__ import annotations

import math
import random

from solver.types import ObjSense, VarType
from tests._anr_oracle import (
    GLOBAL,
    PER_ROW,
    Abs,
    Add,
    Agg,
    AnrModel,
    Cmp,
    Col,
    Const,
    Decision,
    Mul,
    Objective,
    Var,
    clause,
    eq,
    le,
    reduce,
)

from .runner import Case, Reference, Text, column, inline, number, stored

COLUMNS = (
    ("id", "INT"),
    ("g", "VARCHAR"),
    ("h", "VARCHAR"),
    ("k", "VARCHAR"),
    ("p", "BOOLEAN"),
    ("q", "BOOLEAN"),
    ("v", "INT"),
    ("w", "INT"),
    ("c", "INT"),
    ("cap", "INT"),
)
KINDS = ("sum", "avg", "min", "max")
SENSES = ("<=", ">=", "=", "<>")
#: The largest number of assignments a case tries.
ASSIGNMENTS = 4096
#: The bound of a decision the kinds read at every row state without trying every assignment.
UPPER = 3
#: How many clauses a case draws to find one with an instance.
ATTEMPTS = 4
#: How many random assignments a bound draws to find one that the query's other clauses admit.
DRAWS = 16
#: What the language refuses under ``<>``: an AVG it does not read as ``SUM(e) <> K·n`` makes its side fractional
#: (guide 3) or is refused beside another term or with its own ``when`` or ``per`` (errors.md), and a MIN or MAX over
#: a leaf that is the whole side has no ``<>`` (``RewriteMinMax``).
FRACTIONAL = ("can take fractional values", "AVG in a <> constraint is not supported yet")
EXTREMUM = ("does not support <> comparison with MIN/MAX",)


def decision(name: str) -> Text:
    return Text(name, Var(name))


def parity(row) -> tuple:
    return (row["w"] % 2,)


def total(row) -> tuple:
    return (row["h"], None if row["v"] is None else row["w"] + row["v"])


def negated(row):
    return None if row["p"] is None else not row["p"]


def above_one(row):
    return None if row["v"] is None else row["v"] > 1


WHENS = (
    ("t.p", lambda row: row["p"]),
    ("t.q", lambda row: row["q"]),
    ("NOT t.p", negated),
    ("t.v > 1", above_one),
)
PERS = (("()", GLOBAL), ("t.g", ("g",)), ("t.h", ("h",)), ("t.k", ("k",)), ("t.g, t.h", ("g", "h")), ("row", PER_ROW))
BYS = (
    ((), ()),
    (("t.g",), ("g",)),
    (("t.h",), ("h",)),
    (("t.k",), ("k",)),
    (("t.g", "t.h"), ("g", "h")),
    (("t.g", "t.k"), ("g", "k")),
    (("t.w % 2",), parity),
    (("t.h", "t.w + t.v"), total),
)
DECLARATIONS = {"x": ("x(INT)", PER_ROW), "y": ("per t.g: y(INT)", ("g",)), "z": ("per t.k: z(INT)", ("k",))}
#: A SUM of a keyed decision per class of its own key or of ``()``: a class holds the decision's instance in every row,
#: so the ``per`` counts it once where every row would count it again (deck p. 30).
COUNTS = (("()", GLOBAL, "y"), ("()", GLOBAL, "z"), ("t.g", ("g",), "y"), ("t.k", ("k",), "z"))
#: The data each decision equals in the pinned kind: a value of the row for ``x``, and one its class shares for the
#: keyed two (``a``, ``b`` are 1, 2; ``u``, ``v``, ``w`` are 1, 2, 3).
PINS = {
    "x": column("w"),
    "y": Text("(ascii(t.g) - 96)", Col(lambda row: ord(row["g"]) - 96)),
    "z": Text("(ascii(t.k) - 116)", Col(lambda row: None if row["k"] is None else ord(row["k"]) - 116)),
}


class Counted(tuple):
    """The key of the counting clause's ``per``, read as any key is; ``IgnoresNestPer`` tells it from the nest's."""


def parts(node) -> tuple:
    if isinstance(node, Agg):
        return (node.operand.body,)
    if isinstance(node, Add):
        return node.terms
    if isinstance(node, Mul):
        return node.left, node.right
    return (node.operand,) if isinstance(node, Abs) else ()


def nodes(node):
    """``node`` and every node under it."""
    yield node
    for part in parts(node):
        yield from nodes(part)


def summed(lhs) -> bool:
    """Whether ``lhs <> K`` is read as ``SUM(e) <> K·n`` (guide 3), ``lhs`` holding an AVG: it is the AVG, with no
    ``when`` or ``per`` of its own, over a per-row decision or a constant times one. The CLI refuses the AVG of a
    column times a decision and of a keyed decision with errors.md's message, which lists neither (left for a
    ruling)."""
    if not isinstance(lhs, Agg) or lhs.operand.when is not None or lhs.operand.per is not PER_ROW:
        return False
    body = lhs.operand.body
    if isinstance(body, Mul) and isinstance(body.left, Const):
        body = body.right
    return isinstance(body, Var) and DECLARATIONS[body.name][1] is PER_ROW


def refusals(sense: str, lhs) -> tuple[str, ...]:
    """The refusals ``lhs sense bound`` can raise, ``lhs`` being the side with the decisions: only under ``<>``, from
    an AVG on it that is not ``summed``, and from a MIN or MAX over a leaf that is the whole side. The language reads
    one beside a term (``x + max(x)``, ``t.w * max(x)``) or over a reducer (``max(sum(x) by (t.k))``) under ``<>``."""
    if sense != "<>":
        return ()
    if any(isinstance(node, Agg) and node.kind == "avg" for node in nodes(lhs)):
        return () if summed(lhs) else FRACTIONAL
    extremum = isinstance(lhs, Agg) and lhs.kind in ("min", "max")
    return EXTREMUM if extremum and not any(isinstance(node, Agg) for node in nodes(lhs.operand.body)) else ()


def rows_of(rng: random.Random, count: int) -> list[dict]:
    nulls = rng.random() < 0.5

    def maybe(value):
        return None if nulls and rng.random() < 0.12 else value

    return [
        dict(
            id=i + 1,
            g=rng.choice("ab"),
            h=maybe(rng.choice("pq")),
            k=maybe(rng.choice("uvw")),
            p=maybe(rng.random() < 0.6),
            q=rng.random() < 0.6,
            v=maybe(rng.randint(1, 3)),
            w=rng.randint(1, 3),
            c=rng.choice((-2, -1, 1, 1, 2, 3)),
            cap=rng.randint(2, 14),
        )
        for i in range(count)
    ]


class Draw:
    """The random parts of one query, over its rows and the decisions its leaves read."""

    def __init__(self, rng: random.Random, rows: list[dict], names=("x",)):
        self.rng = rng
        self.rows = rows
        self.names = list(names)
        #: What a leaf reads under a ``per``: a keyed decision is one signature in all the rows of its class, so a class
        #: of the ``per`` holds it once, where a per-row ``x`` is a member of every row whatever the ``per``.
        self.keyed = [name for name in names if DECLARATIONS[name][1] is not PER_ROW] or self.names

    def scope(self, pers=PERS, when_rate: float = 0.35, per_rate: float = 0.5) -> tuple:
        """A ``when`` and a ``per``, each now and then: the text before the colon, and their readings."""
        when = self.rng.choice(WHENS) if self.rng.random() < when_rate else (None, None)
        per = self.rng.choice(pers) if self.rng.random() < per_rate else (None, PER_ROW)
        parts = ([f"when {when[0]}"] if when[0] else []) + ([f"per {per[0]}"] if per[0] else [])
        return ((" ".join(parts) + ": ") if parts else ""), when[1], per[1]

    def reducer(self, operand, *, by=True, counts=False) -> Text:
        """A reducer over ``operand(keyed)``, which reads the keyed decisions when the reducer has a ``per``. A ``per``
        is drawn mostly for a SUM or an AVG, whose members it counts: the classes a MIN or a MAX takes its extreme
        over can change it only by leaving out a row with a NULL key. One that ``counts`` is a SUM under a ``per``
        other than ``per row``, which counts a signature once per class."""
        kind = "sum" if counts else self.rng.choice(KINDS)
        rate = 1.0 if counts else 0.75 if kind in ("sum", "avg") else 0.2
        prefix, when, per = self.scope(PERS[:-1] if counts else PERS, per_rate=rate)
        body = operand(per is not PER_ROW)
        written, key = self.rng.choice(BYS) if by else BYS[0]
        suffix = f" by ({', '.join(written)})" if written else ""
        return Text(f"{kind}({prefix}{body.sql}){suffix}", reduce(kind, body.node, when=when, per=per, by=key))

    def data_nest(self, depth: int) -> Text:
        if depth == 0:
            return self.rng.choice((column("v"), column("w"), column("v") + column("w")))
        return self.reducer(lambda keyed: self.data_nest(depth - 1))

    def leaf(self, keyed: bool) -> Text:
        term = decision(self.rng.choice(self.keyed if keyed else self.names))
        shape = self.rng.randrange(5)
        if shape == 0:
            return column("v") * term
        if shape == 1:
            return number(2) * term
        if shape == 2:
            return term - self.data_nest(1)
        return term

    def body(self, depth: int, keyed: bool = False) -> Text:
        """A reducer's operand: a leaf at the bottom; above it a reducer, alone or composed."""
        if depth == 0:
            return self.leaf(keyed)
        shape = self.rng.random()
        if shape >= 0.40:
            return self.reducer(lambda inner: self.body(depth - 1, inner))
        if shape < 0.08:
            nest = self.reducer(lambda inner: self.body(depth - 1, inner))
            return Text(f"abs({nest.sql})", Abs(nest.node))
        nest = self.reducer(lambda inner: self.body(depth - 1, inner))
        if shape < 0.23:
            return self.leaf(keyed) + nest
        if shape < 0.33:
            return nest + self.reducer(lambda inner: self.body(depth - 1, inner))
        return column("w") * nest


def spellings(rng: random.Random, query: str, rows: list[dict]) -> dict[str, str]:
    """A stored table, and inline VALUES with the rows in another order."""
    shuffled = rng.sample(rows, len(rows))
    return {
        "stored": stored("t", COLUMNS, rows) + query.format(source="t") + "\n",
        "values": query.format(source=inline("t", COLUMNS, shuffled)) + "\n",
    }


def binding(rng: random.Random, model: AnrModel, scoped, goal: Objective, domains, before=()) -> tuple[str, int]:
    """A sense and an offset to the bound that make the clause bite: an inequality or ``<>`` cuts off the optimum of
    the query without it (with the clauses ``before`` it), and an inequality and ``=`` still admit a random
    assignment those clauses admit, so few cases are infeasible. It reads the true model, so a broken oracle judges
    the same query."""
    sense = scoped.body.sense
    instances = model.instances(scoped)
    if not instances:
        return sense, 0
    free = model.brute_force(list(before), goal, domains).values
    form = rng.choice(instances).form
    optimum = model.evaluate(form, free)
    held = model.generate(list(before))
    draws = ({atom: rng.choice(list(domains[atom.name])) for atom in free} for _ in range(DRAWS))
    others = (model.evaluate(form, values) for values in draws if model.feasible(held, values))
    other = next((value for value in others if abs(value - optimum) > 1e-9), optimum)
    if sense == "=":
        return sense, round(other)
    if sense == "<>":
        return sense, round(optimum)
    if other < optimum:
        return "<=", min(math.ceil(other), math.ceil(optimum) - 1)
    return ">=", max(math.floor(other), math.floor(optimum) + 1) if other > optimum else math.floor(optimum)


def with_offset(rhs: Text, offset: int) -> Text:
    if isinstance(rhs.node, Const):
        return number(int(rhs.node.value) + offset)
    return rhs + number(offset) if offset >= 0 else rhs - number(-offset)


def counting(rng: random.Random, rows: list[dict], upper: int, count: tuple) -> tuple[str, object]:
    """``sum(per K: d) <= b`` for one of ``COUNTS``: the clause and its reading. The objective pushes ``d`` up against
    it, so every case reads a ``per``, which its nest seldom does over a few rows."""
    written, key, name = count
    instances = len({row[DECLARATIONS[name][1][0]] for row in rows} - {None})
    bound = rng.randint(1, max(1, instances * upper - 1))
    return f"sum(per {written}: {name}) <= {bound}", clause(le(reduce("sum", Var(name), per=Counted(key)), bound))


def over_decisions(rng: random.Random, model: type) -> tuple[Case, bool]:
    """A case, and whether its nest's clause has an instance."""
    rows = rows_of(rng, rng.randint(3, 6))
    names = rng.choice((["x"], ["x"], ["x"], ["x", "y"], ["x", "z"], ["y"], ["x", "y", "z"]))
    count = rng.choice(COUNTS)
    declared = sorted({*names, "x", count[2]})
    classes = len(rows) + sum(len({row[DECLARATIONS[name][1][0]] for row in rows}) for name in declared[1:])
    upper = max((u for u in (2, 3) if (u + 1) ** classes <= ASSIGNMENTS), default=1)
    decisions = [Decision(name, DECLARATIONS[name][1], VarType.INTEGER, 0.0, float(upper)) for name in declared]
    domains = {name: range(upper + 1) for name in declared}
    draw = Draw(rng, rows, names)

    prefix, when, per = draw.scope(when_rate=0.25)
    depth = rng.randint(0, 2)
    lhs = draw.reducer(lambda keyed: draw.body(depth, keyed))
    shape = rng.random()
    if shape < 0.2:
        lhs = decision("x") + lhs
    elif shape < 0.3:
        lhs = column("w") * lhs

    maximize = rng.random() < 0.5
    goal = Text("sum(t.c * x)", reduce("sum", Col("c") * Var("x")))
    for name in declared[1:]:
        up = name != count[2] or maximize
        goal = Text(
            f"{goal.sql} {'+' if up else '-'} sum({name})", goal.node + (1 if up else -1) * reduce("sum", Var(name))
        )
    if rng.random() < 0.3:
        depth = rng.randint(0, 1)
        nest = draw.reducer(lambda keyed: draw.body(depth, keyed), by=False)
        goal = Text(f"{goal.sql} + {nest.sql}", goal.node + nest.node)
    objective = Objective(ObjSense.MAXIMIZE if maximize else ObjSense.MINIMIZE, goal.node)

    sense = rng.choice(SENSES)
    # Under `=` a bound that varies over a group with no instance raises "more than one value" (Q7 review, needs a
    # ruling), so `=` takes a constant.
    rhs = number(0)
    if sense != "=" and rng.random() < 0.5:
        rhs = rng.choice((column("cap"), column("cap") - draw.data_nest(rng.randint(1, 2))))
    true_model = AnrModel(rows, decisions)
    count_sql, counted = counting(rng, rows, upper, count)
    probe = clause(Cmp(lhs.node, sense, rhs.node), when=when, per=per)
    sense, offset = binding(rng, true_model, probe, objective, domains, [counted])
    rhs = with_offset(rhs, offset)
    scoped = clause(Cmp(lhs.node, sense, rhs.node), when=when, per=per)

    declaration = ", ".join(DECLARATIONS[name][0] for name in declared)
    boxes = " AND ".join(f"{name} <= {upper}" for name in declared)
    query = (
        f"SELECT t.id, {', '.join(declared)} FROM {{source}} DECIDE {declaration} SUCH THAT {boxes} AND "
        f"{count_sql} AND {prefix}{lhs.sql} {sense} {rhs.sql} "
        f"{'MAXIMIZE' if maximize else 'MINIMIZE'} {goal.sql} ORDER BY t.id;"
    )
    reference = Reference(model(rows, decisions), [counted, scoped], objective, domains)
    return Case(reference, spellings(rng, query, rows), refusals(sense, lhs.node)), bool(true_model.instances(scoped))


def over_data(rng: random.Random, model: type) -> Case:
    """The nest's value at every row, read exactly: ``s = nest`` pins it, and ``s`` falls to its floor on a row where
    the nest has no value."""
    rows = rows_of(rng, rng.randint(3, 8))
    draw = Draw(rng, rows)
    depth = rng.randint(1, 2)
    nest = draw.reducer(lambda keyed: draw.data_nest(depth))
    count = rng.choice(COUNTS)
    name = count[2]
    count_sql, counted = counting(rng, rows, UPPER, count)
    query = (
        f"SELECT t.id, s, {name} FROM {{source}} DECIDE s(REAL), {DECLARATIONS[name][0]} SUCH THAT s >= -1000 AND "
        f"s <= 1000 AND {name} <= {UPPER} AND {count_sql} AND s = {nest.sql} MINIMIZE sum(s) - sum({name}) "
        "ORDER BY t.id;"
    )
    decisions = [
        Decision("s", PER_ROW, VarType.CONTINUOUS, -1000.0, 1000.0),
        Decision(name, DECLARATIONS[name][1], VarType.INTEGER, 0.0, float(UPPER)),
    ]
    clauses = [counted, clause(eq(Var("s"), nest.node))]
    objective = Objective(ObjSense.MINIMIZE, reduce("sum", Var("s")) - reduce("sum", Var(name)))
    return Case(Reference(model(rows, decisions), clauses, objective), spellings(rng, query, rows))


def over_pinned(rng: random.Random, model: type) -> Case:
    """A nest over decisions read at every row, as ``over_data`` reads one over data: ``PINS`` fix every decision, so
    ``s = nest`` holds the nest's value there, and a ``per``, ``by`` or ``when`` read wrong anywhere in it moves an
    ``s``."""
    rows = rows_of(rng, rng.randint(6, 12))
    draw = Draw(rng, rows, tuple(DECLARATIONS))
    depth = rng.randint(1, 2)
    nest = draw.reducer(lambda keyed: draw.body(depth, keyed), counts=True)
    pins = " AND ".join(f"{name} <= {UPPER} AND {name} = {PINS[name].sql}" for name in DECLARATIONS)
    query = (
        f"SELECT t.id, s, {', '.join(DECLARATIONS)} FROM {{source}} DECIDE s(REAL), "
        f"{', '.join(text for text, _ in DECLARATIONS.values())} SUCH THAT s >= -1000 AND s <= 1000 AND "
        f"{pins} AND s = {nest.sql} MINIMIZE sum(s) ORDER BY t.id;"
    )
    decisions = [Decision("s", PER_ROW, VarType.CONTINUOUS, -1000.0, 1000.0)] + [
        Decision(name, key, VarType.INTEGER, 0.0, float(UPPER)) for name, (_, key) in DECLARATIONS.items()
    ]
    clauses = [clause(eq(Var(name), pin.node)) for name, pin in PINS.items()] + [clause(eq(Var("s"), nest.node))]
    objective = Objective(ObjSense.MINIMIZE, reduce("sum", Var("s")))
    return Case(Reference(model(rows, decisions), clauses, objective), spellings(rng, query, rows))


def generate(seed: int, model: type = AnrModel) -> Case:
    rng = random.Random(f"nests:{seed}")
    kind = rng.random()
    if kind < 0.1:
        return over_data(rng, model)
    if kind < 0.35:
        return over_pinned(rng, model)
    # A clause whose `when`s leave it no instance says little, so most draws take another.
    for _ in range(ATTEMPTS):
        case, has_instance = over_decisions(rng, model)
        if has_instance:
            break
    return case


class DropsFirstMember(AnrModel):
    """A reducer that loses its first member."""

    def _combine(self, kind, values):
        return super()._combine(kind, values[1:])


class IgnoresPer(AnrModel):
    """Every row its own class, whatever the ``per``."""

    def _split(self, relation, per):
        return super()._split(relation, PER_ROW)


class IgnoresNestPer(AnrModel):
    """Every row its own class in the nest's clause and the objective, whatever their ``per``; the counting clause
    keeps its classes, so only a ``per`` the nest reads can show the mismatch."""

    def _split(self, relation, per):
        return super()._split(relation, per if isinstance(per, Counted) else PER_ROW)


MODEL = AnrModel
BROKEN = {"drops-first-member": DropsFirstMember, "ignores-per": IgnoresPer, "ignores-nest-per": IgnoresNestPer}
