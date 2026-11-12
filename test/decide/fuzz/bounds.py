"""A frame on the bound of a clause over groups: each group takes its tightest value among the anchors where the frame
has one, and under ``=`` two values are an error (D85).

Every query is run twice more with the bound precomputed as a column, ``t.fv``, read from the frame by a reading of its
own: a frame bound must answer as a column bound does.
"""

from __future__ import annotations

import random

from solver.types import ObjSense, VarType
from tests._anr_oracle import (
    ABSENT,
    CONSTANT,
    PER_ROW,
    AnrModel,
    At,
    Cmp,
    Col,
    Decision,
    Extremum,
    Objective,
    Over,
    Position,
    ScopeError,
    Var,
    clause,
    reduce,
)

from .runner import Case, Reference, inline, stored

COLUMNS = (
    ("id", "INT"),
    ("g", "VARCHAR"),
    ("h", "VARCHAR"),
    ("k", "INT"),
    ("s", "INT"),
    ("c", "DOUBLE"),
    ("w", "DOUBLE"),
    ("fv", "DOUBLE"),
)
UPPER = 100.0
TWO_VALUES = "takes more than one value"
#: A reducer's own ``when``; one keeps no row.
MEMBER_WHENS = (
    None,
    ("t.c > 3", lambda row: row["c"] is not None and row["c"] > 3),
    ("t.s <= 2", lambda row: row["s"] <= 2),
    ("t.s > 100", lambda row: False),
    ("t.g = 'a'", lambda row: row["g"] == "a"),
)
CLAUSE_WHENS = (
    None,
    ("t.s > 1", lambda row: row["s"] > 1),
    ("t.c < 8", lambda row: row["c"] is not None and row["c"] < 8),
    ("t.g <> 'b'", lambda row: row["g"] is not None and row["g"] != "b"),
)
KEYS = ((), ("g",), ("h",), ("g", "h"))
#: The bound as a function of the frame, written and read.
BOUNDS = (
    ("{F}", lambda read: read),
    ("2 * {F} + 1", lambda read: 2 * read + 1),
    ("{F} - 1", lambda read: read - 1),
    ("20 - {F}", lambda read: 20 - read),
)


class TwoValues(ScopeError):
    def __init__(self):
        Exception.__init__(self, TWO_VALUES)
        self.kind, self.phrase = "two_values", TWO_VALUES


class BoundModel(AnrModel):
    """``_anr_oracle``'s reading, where ``=`` refuses a group bound by two values (D85). A MIN or MAX under ``=`` says
    infeasible instead, as its rewrite splits ``=`` in two (Q7 review, older false errors, finding 12)."""

    def generate(self, clauses):
        instances = super().generate(clauses)
        bounds: dict = {}
        for instance in instances:
            terms = frozenset((atom, c) for atom, c in instance.form.items() if atom != CONSTANT)
            if instance.sense != "=" or any(isinstance(atom, Extremum) for atom, _ in terms):
                continue
            if bounds.setdefault(terms, instance.form.get(CONSTANT, 0.0)) != instance.form.get(CONSTANT, 0.0):
                raise TwoValues()
        return instances

    @staticmethod
    def frame_values(rows, where: str, within) -> dict:
        """``at(where: t.c) over (t.s within ...)`` at every row of ``rows``, read without the oracle: the column a
        bound is precomputed into."""
        timelines: dict = {}
        for row in rows:
            key = tuple(row[name] for name in within)
            if None not in key:
                timelines.setdefault(key, []).append(row)
        values = {row["id"]: None for row in rows}
        for timeline in timelines.values():
            timeline.sort(key=lambda row: row["s"])
            for i, row in enumerate(timeline):
                j = {"previous": i - 1, "next": i + 1, "first": 0, "last": len(timeline) - 1}[where]
                values[row["id"]] = timeline[j]["c"] if 0 <= j < len(timeline) else None
        return values


def spans_timelines(rows, scoped) -> bool:
    try:
        AnrModel(rows, [Decision("x")]).instances(scoped)
    except ScopeError as error:
        return error.kind == "frame_partition"
    return False


def rows_of(rng: random.Random) -> list[dict]:
    count = rng.randint(4, 8)
    orders = rng.sample(range(1, count + 1), count)
    weights = rng.sample([round(1 + 0.137 * i + rng.random() * 0.05, 3) for i in range(count)], count)
    return [
        dict(
            id=i + 1,
            g=rng.choice(("a", "a", "b", "b", "c", None)),
            h=rng.choice(("p", "q", "q", None)),
            k=rng.choice((1, 2, 3, None)),
            s=orders[i],
            c=None if rng.random() < 0.2 else float(rng.randint(1, 9)),
            w=weights[i],
            fv=None,
        )
        for i in range(count)
    ]


def generate(seed: int, model: type = BoundModel) -> Case:
    rng = random.Random(f"bounds:{seed}")
    rows = rows_of(rng)
    where, within = rng.choice(("previous", "next", "first", "last")), rng.choice(KEYS)
    lhs_kind = rng.choice(("sum", "avg", "max", "min", "maxsum"))
    sense = rng.choice(("<=", ">=", "=", "<=", ">=", "=", "<", ">"))
    if lhs_kind == "avg" and sense in ("<", ">"):
        sense = "<="
    integral = sense in ("<", ">") or rng.random() < 0.25
    # Under `=` a column bound raises on a group with no instance, a frame bound does not (Q7 review, needs a
    # ruling): every row is in a group, which keeps its members and its readers.
    by = rng.choice([key for key in KEYS if sense != "=" or all(row[name] is not None for name in key for row in rows)])
    member_when = rng.choice(MEMBER_WHENS) if sense != "=" else None
    per = rng.choice((None, None, "k")) if sense != "=" else None
    clause_when = rng.choice(CLAUSE_WHENS)
    bound_sql, bound_of = rng.choice(BOUNDS)
    maximize = rng.random() < 0.5

    # A frame's anchors are the rows its clause's `when` keeps.
    when = clause_when and clause_when[1]
    kept = [row for row in rows if when is None or when(row)]
    values = model.frame_values(kept, where, within)
    for row in rows:
        row["fv"] = values.get(row["id"])

    def side(kind: str):
        member = f"when {member_when[0]}: x" if member_when else "x"
        suffix = f" by ({', '.join('t.' + name for name in by)})" if by else ""
        return f"{kind}({member}){suffix}", reduce(kind, Var("x"), when=member_when and member_when[1], by=by)

    if lhs_kind == "maxsum":
        (max_sql, max_node), (sum_sql, sum_node) = side("max"), side("sum")
        lhs_sql, lhs = f"{max_sql} + {sum_sql}", max_node + sum_node
    else:
        lhs_sql, lhs = side(lhs_kind)
    bound = bound_of(At(Position(where), Col("c"), Over("s", within=within)))
    scoped = clause(Cmp(lhs, sense, bound), when=when, per=(per,) if per else PER_ROW)
    # A `per` steps aside from a class over two timelines, an error of the frame alone (D75), which its column cannot
    # raise.
    if per and spans_timelines(rows, scoped):
        per = None
        scoped = clause(Cmp(lhs, sense, bound), when=when)

    over = f"over (t.s{' within ' + ', '.join('t.' + name for name in within) if within else ''})"
    prefix = " ".join(([f"when {clause_when[0]}"] if clause_when else []) + ([f"per t.{per}"] if per else []))
    prefix = prefix + ": " if prefix else ""

    def query(bound_text: str, source: str) -> str:
        return (
            f"SELECT t.id, x FROM {source} DECIDE x({'INT' if integral else 'REAL'}) SUCH THAT x <= {UPPER:g} AND "
            f"{prefix}{lhs_sql} {sense} {bound_text} {'MAXIMIZE' if maximize else 'MINIMIZE'} sum(x * t.w) "
            "ORDER BY t.id;\n"
        )

    frame_sql, column_sql = bound_sql.format(F=f"at({where}: t.c) {over}"), bound_sql.format(F="t.fv")
    decision = Decision("x", PER_ROW, VarType.INTEGER if integral else VarType.CONTINUOUS, 0.0, UPPER)
    objective = Objective(ObjSense.MAXIMIZE if maximize else ObjSense.MINIMIZE, reduce("sum", Var("x") * Col("w")))
    reference = Reference(model(rows, [decision]), [scoped], objective)
    table = stored("t", COLUMNS, rows[::-1] if rng.random() < 0.5 else rows)
    source = inline("t", COLUMNS, rng.sample(rows, len(rows)))
    scripts = {
        "frame, stored": table + query(frame_sql, "t"),
        "column, stored": table + query(column_sql, "t"),
        "frame, values": query(frame_sql, source),
        "column, values": query(column_sql, source),
    }
    return Case(reference, scripts)


class AbsentIsZero(BoundModel):
    """A frame that selects nothing read as 0 at an anchor in a timeline, so the anchor bounds its group."""

    def _point(self, node, t, kept):
        value = super()._point(node, t, kept)
        missing = value is ABSENT and t in self._timeline(node.over, kept).position
        return {CONSTANT: 0.0} if missing else value


class ColumnOneFurther(BoundModel):
    """Each row's precomputed value taken from the row with the next id: the column spellings then ask another query."""

    @staticmethod
    def frame_values(rows, where, within):
        values = BoundModel.frame_values(rows, where, within)
        ids = sorted(values)
        return {row: values[ids[(i + 1) % len(ids)]] for i, row in enumerate(ids)}


MODEL = BoundModel
BROKEN = {"absent-is-zero": AbsentIsZero, "column-one-further": ColumnOneFurther}
