"""A frame's order key as ORDER BY reads it: a collation, given by the column, by ``COLLATE`` in the ``over`` or by
``default_collation``, and NULLs placed by ``default_null_order`` (D74).

Peers are the rows whose keys collate equal, so the payload is one value per collated key. The source comes through a
subquery, a stored table, a CTE, a UNION ALL, a join or a view, each of which must keep the collation, and each spelling
takes the rows in an order of its own.
"""

from __future__ import annotations

import random
import unicodedata

from solver.types import ObjSense, VarType
from tests._anr_oracle import PER_ROW, AnrModel, Col, Decision, Objective, Over, Var, clause, le, reduce

from .frames import frame
from .runner import Case, Reference, Text, inline, number

COLUMNS = (("id", "INT"), ("k", "VARCHAR"), ("v", "DOUBLE"), ("w", "VARCHAR"))
LETTERS = ("a", "A", "b", "B", "c", "C", "d", "D", "é", "e", "E", "É", "Z", "z", "aa", "AA")
SHAPES = ("subquery", "stored", "cte", "over collate", "default collation", "union", "join", "view")


def without_accents(text: str) -> str:
    return "".join(c for c in unicodedata.normalize("NFD", text) if unicodedata.category(c) != "Mn")


COLLATIONS = {
    "NOCASE": str.lower,
    "NOACCENT": without_accents,
    "NOCASE.NOACCENT": lambda text: without_accents(text.lower()),
    "BINARY": lambda text: text,
}
#: Whether NULL comes first, ascending and descending.
NULL_ORDERS = {
    "nulls_last": (False, False),
    "nulls_first": (True, True),
    "nulls_first_on_asc_last_on_desc": (True, False),
    "nulls_last_on_asc_first_on_desc": (False, True),
}


class OrderModel(AnrModel):
    """``_anr_oracle``'s reading, over an order key that sorts as the session's collation and NULL order say."""

    @staticmethod
    def collate(collation: str, text: str) -> str:
        return COLLATIONS[collation](text)

    @staticmethod
    def nulls_first(null_order: str, desc: bool) -> bool:
        return NULL_ORDERS[null_order][desc]

    @classmethod
    def order(cls, collation: str, null_order: str, desc: bool):
        """The key ``Over`` sorts by: a NULL ranks below or above every value, so that the sort, reversed or not,
        puts it where the setting does."""
        null_rank = 2 if cls.nulls_first(null_order, desc) == desc else 0

        def key(row):
            return (null_rank, "") if row["k"] is None else (1, cls.collate(collation, row["k"]))

        return key


def collated(column: str, collation: str) -> str:
    name = f'"{collation}"' if "." in collation else collation
    return column if collation == "BINARY" else f"{column} COLLATE {name}"


def generate(seed: int, model: type = OrderModel) -> Case:
    rng = random.Random(f"order_keys:{seed}")
    shapes = rng.sample(SHAPES, 2)
    collation = rng.choice(list(COLLATIONS))
    if collation == "BINARY" and "default collation" in shapes:
        collation = "NOCASE"  # BINARY is no default_collation
    null_order = rng.choice(list(NULL_ORDERS))
    within = rng.random() < 0.35
    with_null = rng.random() < 0.5
    payloads: dict = {}
    rows = []
    for i in range(rng.randint(1, 12)):
        k = None if with_null and rng.random() < 0.2 else rng.choice(LETTERS)
        w = rng.choice(("p", "q", None)) if within else None
        key = (w, None if k is None else COLLATIONS[collation](k))
        v = payloads.setdefault(key, None if rng.random() < 0.1 else float(rng.randint(1, 40)))
        rows.append(dict(id=i + 1, k=k, v=v, w=w))

    desc, cyclic = rng.random() < 0.4, rng.random() < 0.35
    over = Over(model.order(collation, null_order, desc), desc, cyclic, ("w",) if within else ())
    tail = (" desc" if desc else "") + (" cyclic" if cyclic else "") + (" within t.w" if within else "")
    plain = frame(rng, Text("t.v", Col("v")), "t.k" + tail, over, counts=True, fallbacks=(number(7),))
    in_over = plain.sql.replace("over (t.k", f"over ({collated('t.k', collation)}", 1)

    column = collated("k", collation)
    scripts = {}
    for shape in shapes:
        order = rng.sample(rows, len(rows))
        values = inline("r", COLUMNS, order)
        setup = [f"SET default_null_order='{null_order}';"]
        read, select, by, prefix = plain.sql, "t.id", "t.id", ""
        if shape == "subquery":
            source = f"(SELECT id, {column} AS k, v, w FROM {values}) t"
        elif shape == "stored":
            setup.append(f"CREATE TABLE s(id INT, {collated('k VARCHAR', collation)}, v DOUBLE, w VARCHAR);")
            setup.append(f"INSERT INTO s SELECT * FROM {values};")
            source = "s t"
        elif shape == "cte":
            prefix, source = f"WITH t AS (SELECT id, {column} AS k, v, w FROM {values}) ", "t"
        elif shape == "over collate":
            read, source = in_over, inline("t", COLUMNS, order)
        elif shape == "default collation":
            setup.append(f"SET default_collation='{collation.lower()}';")
            source = inline("t", COLUMNS, order)
        elif shape == "union":
            half = len(order) // 2
            parts = [part for part in (order[:half], order[half:]) if part]
            source = " UNION ALL ".join(
                f"SELECT id, {column} AS k, v, w FROM {inline('r', COLUMNS, part)}" for part in parts
            )
            source = f"({source}) t"
        elif shape == "join":
            keys, read_values = f"SELECT id, {column} AS k, w FROM {values}", f"SELECT id AS id2, v FROM {values}"
            source = f"({keys}) t0 JOIN ({read_values}) s ON t0.id = s.id2"
            read = read.replace("t.k", "t0.k").replace("t.w", "t0.w").replace("t.v", "s.v")
            select, by = "t0.id AS id", "t0.id"
        else:
            setup.append(f"CREATE VIEW s AS SELECT id, {column} AS k, v, w FROM {values};")
            source = "s t"
        query = (
            f"{prefix}SELECT {select}, x FROM {source} DECIDE x(REAL) SUCH THAT x <= 50 AND x <= {read} "
            f"MAXIMIZE sum(x) ORDER BY {by};"
        )
        scripts[shape] = "\n".join(setup + [query]) + "\n"

    decision = Decision("x", PER_ROW, VarType.CONTINUOUS, 0.0, 50.0)
    clauses = [clause(le(Var("x"), plain.node))]
    reference = Reference(model(rows, [decision]), clauses, Objective(ObjSense.MAXIMIZE, reduce("sum", Var("x"))))
    return Case(reference, scripts)


class IgnoresCollation(OrderModel):
    """Keys compared as written."""

    @staticmethod
    def collate(collation, text):
        return text


class NullsAlwaysLast(OrderModel):
    """NULL last whatever ``default_null_order`` says."""

    @staticmethod
    def nulls_first(null_order, desc):
        return False


MODEL = OrderModel
BROKEN = {"ignores-collation": IgnoresCollation, "nulls-always-last": NullsAlwaysLast}
