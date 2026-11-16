"""Frame selectors, ``at(...)`` and ranges ``agg(from ... to ...)``, over data and over decisions.

Each frame is read as a number: ``y = frame + 1000`` pins ``y`` where the frame has a value, and ``MINIMIZE sum(y)``
sends it to 0 where it has none. Rows tie on the order key, with one payload per position or with two (an error), and
NULLs fall in the partition, the order key, the payload and a column fallback. A frame stands alone, under a clause's
``when``, or inside a reducer.
"""

from __future__ import annotations

import random

from solver.types import ObjSense, VarType
from tests._anr_oracle import (
    PER_ROW,
    AnrModel,
    At,
    Col,
    Const,
    Decision,
    Objective,
    Over,
    Position,
    Range,
    Var,
    clause,
    eq,
    reduce,
)

from .runner import Case, Reference, Text, column, inline, number, stored

COLUMNS = (("id", "INT"), ("p", "VARCHAR"), ("s", "INT"), ("v", "INT"), ("w", "INT"), ("o", "INT"), ("f", "BOOLEAN"))
NULL = Text("NULL", Col(lambda row: None))


def position(rng: random.Random) -> tuple[str, Position]:
    where = rng.choice(("first", "last", "previous", "next"))
    k = rng.choice((1, 1, 2, 3, 4, 7)) if where in ("previous", "next") else 1
    return (f"{k} {where}" if k != 1 else where), Position(where, k)


def frame(rng: random.Random, payload: Text, over_sql: str, over: Over, *, counts: bool, fallbacks) -> Text:
    """A point or a range over ``payload``, with no policy, an ``else`` drawn from ``fallbacks`` or, for a range,
    ``all``. ``counts`` admits ``count``, which reads data only."""
    is_range = rng.random() < 0.6
    policy = rng.choice(("none", "else", "else", "all") if is_range else ("none", "else", "else"))
    fallback = rng.choice(fallbacks) if policy == "else" else None
    written = f" else {fallback.sql}" if fallback else (" all" if policy == "all" else "")
    node_fallback = fallback.node if fallback else None
    if not is_range:
        where_sql, where = position(rng)
        node = At(where, payload.node, over, node_fallback)
        return Text(f"at({where_sql}{written}: {payload.sql}) over ({over_sql})", node)
    kind = rng.choice(("sum", "avg", "min", "max") + (("count",) if counts else ()))
    (start_sql, start), (end_sql, end) = position(rng), position(rng)
    every = rng.choice((1, 1, 2, 3))
    step = f" every {every}" if every != 1 else ""
    node = Range(kind, start, end, payload.node, over, every, node_fallback, policy == "all")
    return Text(f"{kind}(from {start_sql} to {end_sql}{step}{written}: {payload.sql}) over ({over_sql})", node)


def rows_of(rng: random.Random, over_decisions: bool) -> tuple[list[dict], bool]:
    """The rows, and whether each position of the order key holds one payload."""
    count = rng.randint(3, 10)
    tied = rng.random() < 0.5
    orders = rng.choices(range(1, 6), k=count) if tied else rng.sample(range(1, 40), count)
    nulls = rng.random() < 0.4
    one_payload = not tied or rng.random() < 0.8
    payloads: dict = {}
    rows = []
    for i in range(count):
        p = rng.choice(("A", "A", "B", "C", None if nulls else "B"))
        s = None if nulls and rng.random() < 0.15 else orders[i]
        v = None if nulls and not over_decisions and rng.random() < 0.15 else rng.randint(1, 11)
        v, w = payloads.setdefault((p, s), (v, rng.randint(1, 3))) if one_payload else (v, rng.randint(1, 3))
        o = None if nulls and rng.random() < 0.3 else rng.randint(1, 50)
        rows.append(dict(id=i + 1, p=p, s=s, v=v, w=w, o=o, f=rng.random() < 0.7))
    return rows, one_payload


def generate(seed: int, model: type = AnrModel) -> Case:
    rng = random.Random(f"frames:{seed}")
    over_decisions = rng.random() < 0.5
    rows, one_payload = rows_of(rng, over_decisions)
    keyed = over_decisions and one_payload and rng.random() < 0.3
    base = Text("x", Var("x")) if over_decisions else column("v")
    fallbacks = (number(0), number(7), number(2.5), NULL, column("o"))

    decisions, clauses, written = [], [], []
    if over_decisions:
        key = ("p", "s") if keyed else PER_ROW
        decisions.append(Decision("x", key, VarType.CONTINUOUS, 0.0, None))
        clauses.append(clause(eq(Var("x"), Col("v"))))
        written.append("x = t.v")
    for j in range(1 if not one_payload else rng.randint(1, 3)):
        payload = column("w") * base if rng.random() < 0.4 else base
        desc, cyclic, within = rng.random() < 0.3, rng.random() < 0.3, rng.random() < 0.7
        over = Over("s", desc, cyclic, ("p",) if within else ())
        over_sql = "t.s" + (" desc" if desc else "") + (" cyclic" if cyclic else "") + (" within t.p" if within else "")
        read = frame(rng, payload, over_sql, over, counts=not over_decisions, fallbacks=fallbacks)
        if rng.random() < 0.2:
            kind = rng.choice(("sum", "avg", "min", "max"))
            read = Text(f"{kind}({read.sql}) by (t.p)", reduce(kind, read.node, by=("p",)))
        when = rng.random() < 0.25
        name = f"y{j}"
        decisions.append(Decision(name, PER_ROW, VarType.CONTINUOUS, 0.0, 100000.0))
        clauses.append(clause(eq(Var(name), read.node + Const(1000.0)), when=(lambda row: row["f"]) if when else None))
        written.append(f"{name} <= 100000")
        written.append(("when t.f: " if when else "") + f"{name} = {read.sql} + 1000")

    names = [decision.name for decision in decisions]
    ys = [name for name in names if name != "x"]
    declaration = ", ".join(
        ("per t.p, t.s: x(REAL)" if keyed else "x(REAL)") if n == "x" else f"{n}(REAL)" for n in names
    )
    query = (
        f"SELECT t.id, {', '.join(names)} FROM {{source}} DECIDE {declaration} SUCH THAT {' AND '.join(written)} "
        f"MINIMIZE {' + '.join(f'sum({name})' for name in ys)} ORDER BY t.id;"
    )
    goal = reduce("sum", Var(ys[0]))
    for name in ys[1:]:
        goal = goal + reduce("sum", Var(name))
    reference = Reference(model(rows, decisions), clauses, Objective(ObjSense.MINIMIZE, goal))
    shuffled = rng.sample(rows, len(rows))
    scripts = {
        "stored": stored("t", COLUMNS, rows) + query.format(source="t") + "\n",
        "values": query.format(source=inline("t", COLUMNS, shuffled)) + "\n",
    }
    return Case(reference, scripts)


class CyclicIgnored(AnrModel):
    """``cyclic`` read as an open timeline."""

    @staticmethod
    def _wrap(q, peers, cyclic):
        return AnrModel._wrap(q, peers, False)


class OneFurther(AnrModel):
    """Every position read one peer further on."""

    @staticmethod
    def _wrap(q, peers, cyclic):
        return AnrModel._wrap(q + 1, peers, cyclic)


MODEL = AnrModel
BROKEN = {"cyclic-ignored": CyclicIgnored, "one-further": OneFurther}
