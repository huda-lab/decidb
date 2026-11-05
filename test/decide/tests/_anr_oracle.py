"""Reference evaluator for the full ANR language: variant signatures, reducers nested to
any depth, and frame selectors.

It reads no SQL. A test states the query's result rows, its decisions with their keys and
its clauses as expression trees in plain Python; the evaluator generates the instances the
language defines and then either solves them through the ``OracleSolver`` interface or
judges DeciDB's answer against them. The definitions it follows are the deck's
(``context/New_syntax.pdf`` pp. 25-56, 79-119) under the rulings of
``context/docs/language/decisions.md`` (D5, D67, D68 and D70-D86), read literally, with no
algorithmic shortcut:

- **A scoped expression** ``[when c] [per K]: body`` keeps the rows ``c`` keeps, splits them
  into classes of ``K``, and produces one value per distinct *signature* in each class,
  read at the signature's first kept row (§3.1, §3.2). ``per row`` keeps every tuple
  occurrence.
- **A signature** is the tuple of the body's fields: each column its data terms read, by
  value (D83, deck pp. 81, 110: ``a + b`` is the field ``(a, b)``); for each reducer, its
  factor and its ``by`` value; for each frame, its keys (D82, deck p. 99): its timeline,
  the anchor's position there when it reads relative to the anchor, and its fallback
  where the anchor reaches it.
- **A reducer** reads the kept rows of the enclosing scope that share the current row's
  ``by`` value -- all of them, not only the current class -- evaluates its operand there,
  and combines the operand's values with multiplicity kept (§3.1, §3.3).
- **A frame** partitions the enclosing scope's kept rows by ``within``, orders each
  partition into peers (rows with equal order keys, NULL last as DuckDB's default orders
  it, D74), and navigates by peer position. A missing position has no value. A fallback is
  evaluated at the anchor, and only where a position is missing.
- **No value** (``ABSENT``) is one thing wherever it comes from: a NULL data value (D67), a
  row a NULL key puts in no class, group or timeline (D5), a reducer left with no member
  (D68), a frame that selects nothing. It skips a comparison's instance, drops a reducer's
  member and drops an objective term; ``count`` over no member is 0, and over a range with
  no requested position that exists it has no value (deck p. 50).

Values are symbolic: a linear form over decision instances (``Column``), extremum helpers
(``Extremum``) and absolute values (``Absolute``), with a constant under ``CONSTANT``.
Signatures freeze those forms by identity before any number is substituted, so two
instances that would solve to the same value never collapse. ``solve`` states extrema and
absolute values exactly (pick and sign indicators); ``judge``, ``feasible`` and
``brute_force`` evaluate the same generated instances numerically, so the solved model and
the numeric reading cannot drift apart.

The run-time errors of the language are ``ScopeError`` with the fixed phrase DeciDB's
message carries. A construct the language refuses at bind or plan time (§3.7) raises
``Unsupported``: the oracle has no answer for it. Only some refusals are modelled (a
strict comparison or ``<>`` over a fractional side or a REAL decision); a case must not
state a shape DeciDB refuses for other reasons (a query-wide decision alone in a reducer,
a decision beside MIN/MAX in the objective, a data reducer times a decision, the ``=``
contradiction).

Bounds are a decision's own (``Decision.lb``, ``ub``): the rule that a negative lower
bound written on a bare decision replaces the default floor 0 (D41) is not derived from
the clauses, so a case with one sets ``lb`` itself.
"""

from __future__ import annotations

import itertools
import math
from dataclasses import dataclass, field
from typing import Callable, Iterable, Iterator, Mapping, Sequence

from ._scope_oracle import RelationColumns, key_value  # noqa: F401 -- RelationColumns is part of the statement API
from solver.base import OracleSolver
from solver.types import ObjSense, SolverStatus, VarType

Row = Mapping[str, object]
#: A key: row columns and ``RelationColumns`` (``per D``), or a function of the row
#: returning a tuple. A NULL in a written element puts the row in no class (D5).
Key = object

#: ``per row`` (and an omitted ``per``): one class per tuple occurrence.
PER_ROW = None
#: ``per ()``: one class, or one decision for the whole query.
GLOBAL = ()
#: The constant of a linear form, and the constant entry of a ``Terms`` mapping.
CONSTANT = ""
#: Largest number of positions one range frame may request (``decide_frame_limit``).
FRAME_LIMIT = 100_000

_TOLERANCE = 1e-6
_DIGITS = 9


class ScopeError(Exception):
    """A run-time error of the language, raised before any model is built."""

    #: The fixed phrase DeciDB's message carries for each kind (plan §3.6, §7.3 today).
    PHRASES = {
        "objective_variants": "would be produced more than once",
        "frame_partition": "does not determine the timeline",
        "ambiguous_peer": "a position must hold one row",
    }

    def __init__(self, kind: str, detail: str):
        super().__init__(f"{ScopeError.PHRASES[kind]}: {detail}")
        self.kind = kind
        self.phrase = ScopeError.PHRASES[kind]


class Unsupported(Exception):
    """A construct the language refuses at bind or plan time (D80)."""


# ---------------------------------------------------------------------------
# Expressions
# ---------------------------------------------------------------------------


class Expr:
    """A scalar expression. Python's arithmetic operators build the tree; comparisons are
    built with ``le``, ``ge``, ``eq``, ``lt``, ``gt`` and ``ne``."""

    def __add__(self, other):
        return Add((self, lift(other)))

    def __radd__(self, other):
        return Add((lift(other), self))

    def __sub__(self, other):
        return Add((self, Mul(Const(-1.0), lift(other))))

    def __rsub__(self, other):
        return Add((lift(other), Mul(Const(-1.0), self)))

    def __neg__(self):
        return Mul(Const(-1.0), self)

    def __mul__(self, other):
        return Mul(self, lift(other))

    def __rmul__(self, other):
        return Mul(lift(other), self)

    def __truediv__(self, other):
        if isinstance(other, (int, float)) and not isinstance(other, bool):
            return Mul(self, Const(1.0 / other))
        raise Unsupported("division by an expression")


def lift(value) -> Expr:
    if isinstance(value, Expr):
        return value
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        return Const(float(value))
    raise TypeError(f"not an expression: {value!r}")


@dataclass(frozen=True, eq=False)
class Const(Expr):
    value: float


@dataclass(frozen=True, eq=False)
class Col(Expr):
    """A data value of the row: a column name, or a function of the row."""

    source: object

    def at(self, row: Row):
        return self.source(row) if callable(self.source) else row[self.source]


@dataclass(frozen=True, eq=False)
class Var(Expr):
    """A decision, read through its class at the row."""

    name: str


@dataclass(frozen=True, eq=False)
class Terms(Expr):
    """A linear form given per row: ``fn(row)`` maps decision names to coefficients and
    ``CONSTANT`` to the constant -- the shape ``_scope_oracle`` states its parts in."""

    fn: Callable[[Row], Mapping[str, object]]


@dataclass(frozen=True, eq=False)
class Add(Expr):
    terms: tuple


@dataclass(frozen=True, eq=False)
class Mul(Expr):
    left: Expr
    right: Expr


@dataclass(frozen=True, eq=False)
class Abs(Expr):
    operand: Expr


@dataclass(frozen=True, eq=False)
class Scope:
    """``[when c] [per K]: body``. ``when`` keeps a row when it returns True (False and
    None, SQL's NULL, drop it); ``per`` is ``PER_ROW``, ``GLOBAL`` or a key."""

    body: object
    when: Callable[[Row], object] | None = None
    per: Key = PER_ROW


@dataclass(frozen=True, eq=False)
class Agg(Expr):
    """``kind(operand) by (by)``: ``sum``, ``avg``, ``min``, ``max`` or ``count``."""

    kind: str
    operand: Scope
    by: Key = ()


def reduce(kind: str, body, *, when=None, per=PER_ROW, by=()) -> Agg:
    """``kind([when c] [per K]: body) by (by)``."""
    operand = body if isinstance(body, Scope) else Scope(lift(body), when, per)
    return Agg(kind, operand, by)


@dataclass(frozen=True)
class Position:
    """``first``, ``last``, ``k previous`` or ``k next`` (§3.4)."""

    where: str
    k: int = 1

    def __post_init__(self):
        if self.where not in ("first", "last", "previous", "next"):
            raise ValueError(f"unknown position {self.where!r}")
        if not isinstance(self.k, int) or isinstance(self.k, bool) or self.k < 1:
            raise Unsupported(f"{self.k} must be a positive whole number")  # D77

    @property
    def relative(self) -> bool:
        return self.where in ("previous", "next")

    def lift(self, anchor: int, peers: int) -> int:
        return {"first": 0, "last": peers - 1, "previous": anchor - self.k, "next": anchor + self.k}[self.where]


FIRST = Position("first")
LAST = Position("last")


def previous(k: int = 1) -> Position:
    return Position("previous", k)


def next_(k: int = 1) -> Position:
    return Position("next", k)


@dataclass(frozen=True, eq=False)
class Over:
    """``over (order [desc] [cyclic] [within key])``; ``within ()`` is one timeline."""

    order: object
    desc: bool = False
    cyclic: bool = False
    within: Key = ()


@dataclass(frozen=True, eq=False)
class At(Expr):
    """``at(position [else fallback]: payload) over (...)``."""

    position: Position
    payload: Expr
    over: Over
    fallback: Expr | None = None


@dataclass(frozen=True, eq=False)
class Range(Expr):
    """``kind(from start to end [every d] [else fallback | all]: payload) over (...)``."""

    kind: str
    start: Position
    end: Position
    payload: Expr
    over: Over
    every: int = 1
    fallback: Expr | None = None
    all: bool = False

    def __post_init__(self):
        if not isinstance(self.every, int) or isinstance(self.every, bool) or self.every < 1:
            raise Unsupported(f"{self.every} must be a positive whole number")  # D77
        if self.all and self.fallback is not None:
            raise Unsupported("else and all together")


_FLIP = {"<=": ">=", ">=": "<=", "<": ">", ">": "<", "=": "=", "<>": "<>"}


@dataclass(frozen=True, eq=False)
class Cmp:
    """``lhs sense rhs``, the body of a constraint. ``<``, ``>`` and ``<>`` step onto the
    whole numbers (D60), which is exact only for a whole-numbered decision side."""

    lhs: Expr
    sense: str
    rhs: Expr
    difference: Expr = field(init=False, repr=False)

    def __post_init__(self):
        if self.sense not in _FLIP:
            raise ValueError(f"unknown sense {self.sense!r}")
        object.__setattr__(self, "difference", Add((self.lhs, Mul(Const(-1.0), self.rhs))))


def le(a, b) -> Cmp:
    return Cmp(lift(a), "<=", lift(b))


def ge(a, b) -> Cmp:
    return Cmp(lift(a), ">=", lift(b))


def eq(a, b) -> Cmp:
    return Cmp(lift(a), "=", lift(b))


def lt(a, b) -> Cmp:
    return Cmp(lift(a), "<", lift(b))


def gt(a, b) -> Cmp:
    return Cmp(lift(a), ">", lift(b))


def ne(a, b) -> Cmp:
    return Cmp(lift(a), "<>", lift(b))


def clause(body: Cmp, *, when=None, per=PER_ROW) -> Scope:
    """A constraint ``[when c] [per K]: body``."""
    return Scope(body, when, per)


@dataclass(frozen=True, eq=False)
class Objective:
    """``MAXIMIZE | MINIMIZE [when c]: body``, generated once (``per ()``, D72)."""

    sense: ObjSense
    body: Expr
    when: Callable[[Row], object] | None = None


@dataclass(frozen=True)
class Decision:
    name: str
    #: ``PER_ROW``, ``GLOBAL``, or the row columns the key names.
    key: tuple[str, ...] | None = PER_ROW
    var_type: VarType = VarType.INTEGER
    lb: float = 0.0
    ub: float | None = None


# ---------------------------------------------------------------------------
# Values
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class Column:
    """One instance of a decision: its class among the rows."""

    name: str
    instance: int


@dataclass(frozen=True)
class Extremum:
    """``kind`` over the frozen ``members``: one exact helper per distinct member set."""

    kind: str
    members: frozenset


@dataclass(frozen=True)
class Absolute:
    operand: frozenset


class _Marker:
    def __init__(self, name: str):
        self.name = name

    def __repr__(self) -> str:
        return self.name


#: No value: a NULL (D67), a row in no class of a key (D5), a reducer left with no member
#: (D68), a frame that selects nothing.
ABSENT = _Marker("ABSENT")
#: A row whose value is absent: it joins no signature.
_SKIP = _Marker("SKIP")


def _freeze(form: Mapping) -> frozenset:
    items = ((atom, round(float(coefficient), _DIGITS)) for atom, coefficient in form.items())
    return frozenset((atom, coefficient) for atom, coefficient in items if coefficient != 0.0)


def _thaw(frozen: frozenset) -> dict:
    return {atom: coefficient for atom, coefficient in frozen}


def _is_constant(form: Mapping) -> bool:
    return all(atom == CONSTANT for atom, coefficient in form.items() if coefficient != 0.0)


def _add_into(into: dict, form: Mapping, scale: float = 1.0) -> None:
    for atom, coefficient in form.items():
        into[atom] = into.get(atom, 0.0) + scale * coefficient


def _scaled(form: Mapping, scale: float) -> dict:
    return {atom: scale * coefficient for atom, coefficient in form.items()}


@dataclass
class Instance:
    """One generated comparison: ``form sense 0``, generated from row ``row``."""

    form: dict
    sense: str
    row: int

    def frozen(self) -> tuple:
        return _freeze(self.form), self.sense


@dataclass
class Solution:
    status: SolverStatus
    objective: float | None = None
    values: dict = field(default_factory=dict)


@dataclass
class _Timeline:
    """One frame's timelines over one relation: each partition's peers in order, and
    each row's partition, peer position and order value."""

    peers: dict
    position: dict
    order: dict


# ---------------------------------------------------------------------------
# The model
# ---------------------------------------------------------------------------


def _key_of(row: Row, key) -> tuple | None:
    """The row's value of ``key``; None when the row is in no class of it (D5)."""
    return key_value(row, key)


def _columns_of(expr) -> list[Col]:
    """The columns a data term reads, each once, in reading order (D83); a function of the
    row is a column of its own."""
    if isinstance(expr, Col):
        return [expr]
    if isinstance(expr, Add):
        parts = expr.terms
    elif isinstance(expr, Mul):
        parts = (expr.left, expr.right)
    elif isinstance(expr, Abs):
        parts = (expr.operand,)
    else:
        return []
    columns: list[Col] = []
    for part in parts:
        columns.extend(column for column in _columns_of(part) if all(column.source != c.source for c in columns))
    return columns


class AnrModel:
    """The rows and decisions of one query, and the instances its clauses generate."""

    def __init__(self, rows: Sequence[Row], decisions: Sequence[Decision]):
        self.rows = [dict(row) for row in rows]
        self.decisions = {decision.name: decision for decision in decisions}
        self.classes: dict[str, list[int]] = {}
        self._first_row: dict[str, dict[int, int]] = {}
        for decision in decisions:
            ids, first = self._class_ids(decision.key)
            self.classes[decision.name] = ids
            self._first_row[decision.name] = first
        self._timelines: dict = {}

    def _class_ids(self, key) -> tuple[list[int], dict[int, int]]:
        if key is PER_ROW:
            return list(range(len(self.rows))), {i: i for i in range(len(self.rows))}
        ids: dict[tuple, int] = {}
        out, first = [], {}
        for i, row in enumerate(self.rows):
            value = _key_of(row, key)
            class_id = None if value is None else ids.setdefault(value, len(ids))
            if class_id is not None:
                first.setdefault(class_id, i)
            out.append(class_id)  # None: the decision has no value on the row (D5)
        return out, first

    def representative(self, column: Column) -> int:
        """The first row of the decision instance's class."""
        return self._first_row[column.name][column.instance]

    def columns(self) -> list[Column]:
        """Every decision instance, one solver column each."""
        return [
            Column(name, class_id)
            for name in self.decisions
            for class_id in sorted({c for c in self.classes[name] if c is not None})
        ]

    # -- generation ---------------------------------------------------------

    def instances(self, scoped: Scope) -> list[Instance]:
        """The constraint instances of ``[when c] [per K]: lhs sense rhs``."""
        if not isinstance(scoped.body, Cmp):
            raise TypeError("a constraint's body is a comparison")
        kept = self._root_kept(scoped.when)
        cmp = scoped.body
        return [
            Instance(value, cmp.sense, row)
            for row, value in self._scope_values(cmp.difference, scoped.when, scoped.per, kept, prefiltered=True)
            if value is not ABSENT
        ]

    def emptied(self, scoped: Scope) -> bool:
        """Whether ``[when c] [per K]: lhs sense rhs`` holds instances and none with a value, so it constrains
        nothing: DeciDB warns of it."""
        kept = self._root_kept(scoped.when)
        rows = [t for class_rows in self._split(kept, scoped.per) for t in class_rows]
        return bool(rows) and all(self._form(scoped.body.difference, t, kept) is ABSENT for t in rows)

    def warnings(self, clauses: Sequence[Scope], objective: Objective | None) -> int:
        """How many DECIDE warnings the query prints: one per clause that constrains nothing, one for an objective
        that is 0."""
        return sum(self.emptied(scoped) for scoped in clauses) + self.objective_is_zero(objective)

    def objective_is_zero(self, objective: Objective | None) -> bool:
        """Whether the objective is 0 whatever the decisions: every term has no value or a zero factor. DeciDB
        warns of it."""
        if objective is None:
            return False
        return all(atom == CONSTANT or coefficient == 0 for atom, coefficient in self.objective_form(objective).items())

    def objective_form(self, objective: Objective | None) -> dict:
        """The objective's one value (D72): one class, which must hold one signature. A
        term with no value is dropped (D67, D68), and is one value of the signature."""
        if objective is None:
            return {}
        kept = self._root_kept(objective.when)
        if not kept:
            return {}
        # Produced once, `per ()` (deck p. 62): its one class lies in one timeline of every frame at its root (D75).
        self._check_timelines(objective.body, kept, kept, GLOBAL)
        terms: list = []
        self._terms_of(objective.body, 1.0, terms)
        signatures = {}
        for t in kept:
            signature = tuple(
                ("absent",) if self._form(term, t, kept) is ABSENT else self._fields(term, t, kept) for _, term in terms
            )
            signatures.setdefault(signature, t)
        if len(signatures) > 1:
            raise ScopeError("objective_variants", f"{len(signatures)} signatures")
        at = next(iter(signatures.values()))
        value: dict = {}
        for scale, term in terms:
            form = self._form(term, at, kept)
            if form is not ABSENT:
                _add_into(value, form, scale)
        return value

    def _terms_of(self, expr, scale: float, out: list) -> None:
        """The additive terms of ``expr``, read through ``+``, ``-`` and constant factors."""
        if isinstance(expr, Add):
            for term in expr.terms:
                self._terms_of(term, scale, out)
        elif isinstance(expr, Mul) and isinstance(expr.left, Const):
            self._terms_of(expr.right, scale * expr.left.value, out)
        elif isinstance(expr, Mul) and isinstance(expr.right, Const):
            self._terms_of(expr.left, scale * expr.right.value, out)
        else:
            out.append((scale, expr))

    def _root_kept(self, when) -> list[int]:
        """The rows a clause keeps; a ``when`` that keeps none generates nothing (D68)."""
        return self._kept(list(range(len(self.rows))), when)

    def _kept(self, relation: Sequence[int], when) -> list[int]:
        return [i for i in relation if when is None or when(self.rows[i]) is True]

    def _split(self, relation: Sequence[int], per) -> list[list[int]]:
        if per is PER_ROW:
            return [[i] for i in relation]
        classes: dict[tuple, list[int]] = {}
        for i in relation:
            value = _key_of(self.rows[i], per)
            if value is not None:  # a row in no class of the key generates nothing (D5)
                classes.setdefault(value, []).append(i)
        return list(classes.values())

    def _scope_values(self, body, when, per, relation, *, prefiltered=False) -> list[tuple[int, object]]:
        """``[when] [per]: body`` evaluated on ``relation``: one ``(row, value)`` per
        distinct signature of each class, classes in first-seen order. A row with no value
        joins no signature (D67)."""
        kept = list(relation) if prefiltered else self._kept(relation, when)
        out = []
        for class_rows in self._split(kept, per):
            if per is not PER_ROW:
                self._check_timelines(body, class_rows, kept, per)
            chosen: dict = {}
            for t in class_rows:
                signature = self._signature(body, t, kept)
                if signature is not _SKIP:
                    chosen.setdefault(signature, t)
            for t in chosen.values():
                out.append((t, self._form(body, t, kept)))
        return out

    # -- signatures ---------------------------------------------------------

    def _signature(self, body, t, kept):
        """Row ``t``'s fields, or ``_SKIP`` when its value is absent."""
        if self._form(body, t, kept) is ABSENT:
            return _SKIP
        return self._fields(body, t, kept)

    def _collect(self, expr, scale: float, data: list, other: list) -> None:
        """Flatten one additive context through ``+``, ``-`` and constant factors, as the
        canonical tree does (D83): its data terms are one field, so ``x + a <= b + sum(y)``
        has the data field ``a - b`` beside the decision ``x`` and the reducer's ``by``."""
        if isinstance(expr, Add):
            for term in expr.terms:
                self._collect(term, scale, data, other)
        elif isinstance(expr, Mul) and isinstance(expr.left, Const):
            self._collect(expr.right, scale * expr.left.value, data, other)
        elif isinstance(expr, Mul) and isinstance(expr.right, Const):
            self._collect(expr.left, scale * expr.right.value, data, other)
        elif self._is_data(expr):
            data.append((scale, expr))
        else:
            other.append(expr)

    def _fields(self, expr, t, kept) -> tuple:
        """The fields of one additive context: each other term's fields, then each column
        its data terms read, by value (D83)."""
        data, other = [], []
        self._collect(expr, 1.0, data, other)
        fields = [self._term_fields(node, t, kept) for node in other]
        columns: dict = {}
        for _, node in data:
            for column in _columns_of(node):
                key = column.source if isinstance(column.source, str) else id(column.source)
                columns.setdefault(key, column.at(self.rows[t]))
        fields.extend(("column", key, value) for key, value in sorted(columns.items(), key=lambda item: str(item[0])))
        return tuple(fields)

    def _term_fields(self, node, t, kept) -> tuple:
        """A term that is not data: a decision by its instance, whatever its coefficient
        (D83); a linear form by its every coefficient; a reducer by its ``by`` value; a
        frame by its keys (D82, deck p. 99); and a product or an ``abs`` by the fields of each
        context it holds."""
        if isinstance(node, Var):
            form = self._form(node, t, kept)  # no instance at a row in no class of its key (D5) is one value
            return ("decision", form if form is ABSENT else frozenset(form))
        if isinstance(node, Terms):
            form = self._form(node, t, kept)
            return ("linear", frozenset((atom, round(float(c), _DIGITS)) for atom, c in form.items()))
        if isinstance(node, Agg):
            return ("by", _key_of(self.rows[t], node.by))
        if isinstance(node, (At, Range)):
            return ("frame",) + self._frame_fields(node, t, kept)
        if isinstance(node, Mul):
            return ("mul", self._fields(node.left, t, kept), self._fields(node.right, t, kept))
        if isinstance(node, Abs):
            return ("abs", self._fields(node.operand, t, kept))
        raise TypeError(f"unexpected node {node!r}")

    def _frame_fields(self, node, t, kept) -> tuple:
        """By its keys, as the annotated tree holds a frame (D82, deck p. 99): its timeline, the
        anchor's position there when it reads relative to the anchor, and the fallback where
        the anchor reaches it -- not the value it reads."""
        timeline = self._timeline(node.over, kept)
        within, _ = timeline.position[t]
        fields = [("within", within)]
        relative = node.position.relative if isinstance(node, At) else node.start.relative or node.end.relative
        if relative:
            fields.append(("order", timeline.order[t]))
        if node.fallback is not None and self._missing(node, t, kept):
            fields.append(("fallback", _freeze(self._form(node.fallback, t, kept))))
        return tuple(fields)

    def _is_data(self, expr) -> bool:
        """Free of decisions, reducers and frames: a data term of its context (D83)."""
        if isinstance(expr, (Const, Col)):
            return True
        if isinstance(expr, Add):
            return all(self._is_data(term) for term in expr.terms)
        if isinstance(expr, Mul):
            return self._is_data(expr.left) and self._is_data(expr.right)
        if isinstance(expr, Abs):
            return self._is_data(expr.operand)
        return False

    def _is_pure(self, expr) -> bool:
        """Free of reducers and frames (a maximal subtree of these is a field, D83)."""
        if isinstance(expr, (Const, Col, Var, Terms)):
            return True
        if isinstance(expr, Add):
            return all(self._is_pure(term) for term in expr.terms)
        if isinstance(expr, Mul):
            return self._is_pure(expr.left) and self._is_pure(expr.right)
        if isinstance(expr, Abs):
            return self._is_pure(expr.operand)
        return False

    def _direct_frames(self, expr) -> list:
        if isinstance(expr, (At, Range)):
            return [expr]
        if isinstance(expr, Add):
            return [frame for term in expr.terms for frame in self._direct_frames(term)]
        if isinstance(expr, Mul):
            return self._direct_frames(expr.left) + self._direct_frames(expr.right)
        if isinstance(expr, Abs):
            return self._direct_frames(expr.operand)
        return []

    def _check_timelines(self, body, class_rows, kept, per) -> None:
        """D75: a ``per K`` class must lie in one ``within`` partition of every frame
        written directly in its body; a row in no timeline (D5) lies in none."""
        for frame in self._direct_frames(body):
            partitions = {_key_of(self.rows[t], frame.over.within) for t in class_rows} - {None}
            if len(partitions) > 1:
                raise ScopeError(
                    "frame_partition", f"per {per}: {len(partitions)} partitions {sorted(map(repr, partitions))}"
                )

    # -- forms ----------------------------------------------------------------

    def _column(self, name: str, i: int):
        """The instance of decision ``name`` at row ``i``, or ``ABSENT`` on a row its key
        puts in no class (D5)."""
        if name not in self.decisions:
            raise KeyError(f"undeclared decision {name!r}")
        instance = self.classes[name][i]
        return ABSENT if instance is None else Column(name, instance)

    def _form(self, expr, i, kept):
        """``expr`` at row ``i`` of the scope whose kept rows are ``kept``: a linear form,
        or ``ABSENT``: a NULL, as SQL's arithmetic has it, makes the whole term NULL."""
        row = self.rows[i]
        if isinstance(expr, Const):
            return {CONSTANT: float(expr.value)}
        if isinstance(expr, Col):
            value = expr.at(row)
            return ABSENT if value is None else {CONSTANT: float(value)}
        if isinstance(expr, Var):
            column = self._column(expr.name, i)
            return ABSENT if column is ABSENT else {column: 1.0}
        if isinstance(expr, Terms):
            out: dict = {}
            for name, coefficient in expr.fn(row).items():
                atom = CONSTANT if name == CONSTANT else self._column(name, i)
                if atom is ABSENT or coefficient is None:
                    return ABSENT
                out[atom] = out.get(atom, 0.0) + float(coefficient)
            return out
        if isinstance(expr, Add):
            parts = self._operands(expr.terms, i, kept)
            if parts is ABSENT:
                return ABSENT
            total: dict = {}
            for part in parts:
                _add_into(total, part)
            return total
        if isinstance(expr, Mul):
            parts = self._operands((expr.left, expr.right), i, kept)
            if parts is ABSENT:
                return ABSENT
            left, right = parts
            if _is_constant(left):
                return _scaled(right, left.get(CONSTANT, 0.0))
            if _is_constant(right):
                return _scaled(left, right.get(CONSTANT, 0.0))
            raise Unsupported("a product of decisions")
        if isinstance(expr, Abs):
            operand = self._form(expr.operand, i, kept)
            if operand is ABSENT:
                return ABSENT
            if _is_constant(operand):
                return {CONSTANT: abs(operand.get(CONSTANT, 0.0))}
            return {Absolute(_freeze(operand)): 1.0}
        if isinstance(expr, Agg):
            return self._reducer(expr, i, kept)
        if isinstance(expr, At):
            return self._point(expr, i, kept)
        if isinstance(expr, Range):
            return self._range(expr, i, kept)
        raise TypeError(f"not an expression: {expr!r}")

    def _operands(self, operands, i, kept):
        """Every operand's form, or ``ABSENT`` if one is absent."""
        forms = [self._form(operand, i, kept) for operand in operands]
        return ABSENT if any(form is ABSENT for form in forms) else forms

    # -- reducers -------------------------------------------------------------

    def _reducer(self, node: Agg, i, kept):
        """``node`` for row ``i``: the kept rows sharing ``i``'s ``by`` value, the operand
        evaluated there, its values combined. A row in no group of the ``by`` key (D5) has
        no value for it."""
        if node.kind not in ("sum", "avg", "min", "max", "count"):
            raise Unsupported(f"reducer {node.kind!r}")
        group = _key_of(self.rows[i], node.by)
        if group is None:
            return ABSENT
        relation = [u for u in kept if _key_of(self.rows[u], node.by) == group]
        operand = node.operand
        members = self._scope_values(operand.body, operand.when, operand.per, relation)
        return self._combine(node.kind, [value for _, value in members])

    def _combine(self, kind: str, values: list):
        """The reducer over its members' values. A member with no value is skipped, as
        SQL's aggregates skip a NULL (D67); a reducer left with none has no value (D68),
        except ``count``, which is 0."""
        values = [value for value in values if value is not ABSENT]
        if kind == "count":
            if any(not _is_constant(value) for value in values):
                raise Unsupported("count over decisions")  # COUNT is data only
            return {CONSTANT: float(len(values))}
        if not values:
            return ABSENT
        if kind in ("sum", "avg"):
            total: dict = {}
            for value in values:
                _add_into(total, value, 1.0 if kind == "sum" else 1.0 / len(values))
            return total
        if all(_is_constant(value) for value in values):
            pick = max if kind == "max" else min
            return {CONSTANT: pick(value.get(CONSTANT, 0.0) for value in values)}
        distinct = {_freeze(value) for value in values}
        if len(distinct) == 1:
            return _thaw(next(iter(distinct)))
        return {Extremum(kind, frozenset(distinct)): 1.0}

    # -- frames -----------------------------------------------------------------

    def _timeline(self, over: Over, kept) -> _Timeline:
        cache_key = (id(over), tuple(kept))
        if cache_key in self._timelines:
            return self._timelines[cache_key][1]
        partitions: dict[tuple, list[int]] = {}
        for i in kept:
            within = _key_of(self.rows[i], over.within)
            if within is not None:  # a row in no timeline (D5) has no position
                partitions.setdefault(within, []).append(i)
        peers, position, order = {}, {}, {}
        for within, rows in partitions.items():
            groups: dict = {}
            for i in rows:
                value = Col(over.order).at(self.rows[i])
                order[i] = value
                groups.setdefault(value, []).append(i)
            known = sorted((value for value in groups if value is not None), reverse=over.desc)
            ordered = [groups[value] for value in known]
            if None in groups:
                ordered.append(groups[None])  # NULLS LAST in either direction (D74)
            peers[within] = ordered
            for index, peer in enumerate(ordered):
                for i in peer:
                    position[i] = (within, index)
        timeline = _Timeline(peers, position, order)
        self._timelines[cache_key] = (over, timeline)  # keep ``over`` alive: its id is the key
        return timeline

    @staticmethod
    def _wrap(q: int, peers: int, cyclic: bool) -> int | None:
        if cyclic:
            return q % peers
        return q if 0 <= q < peers else None

    def _requested(self, node: Range, anchor: int, peers: int) -> list[int]:
        start, end = node.start.lift(anchor, peers), node.end.lift(anchor, peers)
        if start > end:
            return []  # a reversed range is empty, cyclic or not
        count = (end - start) // node.every + 1
        if count > FRAME_LIMIT:
            raise Unsupported(f"{count} positions requested, above decide_frame_limit")
        return [start + j * node.every for j in range(count)]

    def _missing(self, node, t, kept) -> int:
        timeline = self._timeline(node.over, kept)
        within, anchor = timeline.position[t]
        peers = len(timeline.peers[within])
        if isinstance(node, At):
            return int(self._wrap(node.position.lift(anchor, peers), peers, node.over.cyclic) is None)
        return sum(1 for q in self._requested(node, anchor, peers) if self._wrap(q, peers, node.over.cyclic) is None)

    def _require_pure(self, node) -> None:
        for part in (node.payload, node.fallback):
            if part is not None and not self._is_pure(part):
                raise Unsupported("a reducer or frame inside a frame")  # D80

    def _point(self, node: At, t, kept):
        """The payload at the selected row; a NULL there, or at a reached fallback, is no
        value (D84), and an anchor in no timeline (D5) selects nothing."""
        self._require_pure(node)
        timeline = self._timeline(node.over, kept)
        if t not in timeline.position:
            return ABSENT
        within, anchor = timeline.position[t]
        peers = timeline.peers[within]
        q = self._wrap(node.position.lift(anchor, len(peers)), len(peers), node.over.cyclic)
        if q is None:
            return ABSENT if node.fallback is None else self._form(node.fallback, t, kept)
        return self._at_position(node, timeline, within, q, kept)

    def _at_position(self, node, timeline, within, q, kept):
        """The payload at position ``q``: its rows hold one signature of it, as the deck's
        frame container holds one per position (D73, deck p. 110) -- each column the payload
        reads by value, each decision by instance -- and it is read once."""
        peers = timeline.peers[within]
        signatures = {self._fields(node.payload, r, kept) for r in peers[q]}
        if len(signatures) > 1:
            raise ScopeError("ambiguous_peer", f"{len(peers[q])} rows at {within}, {timeline.order[peers[q][0]]}")
        return self._form(node.payload, peers[q][0], kept)

    def _range(self, node: Range, t, kept):
        """The reducer over the values the range reads, one per position it visits (D73); a
        NULL among them is skipped, as a reducer skips one (D67, D84)."""
        self._require_pure(node)
        if node.kind not in ("sum", "avg", "min", "max", "count"):
            raise Unsupported(f"range reducer {node.kind!r}")
        timeline = self._timeline(node.over, kept)
        if t not in timeline.position:
            return ABSENT
        within, anchor = timeline.position[t]
        peers = timeline.peers[within]
        selected = [self._wrap(q, len(peers), node.over.cyclic) for q in self._requested(node, anchor, len(peers))]
        missing = sum(1 for p in selected if p is None)
        if node.all and missing:
            return ABSENT  # nothing is read, so no value it would read can fail
        # An `else` with no value at the anchor is no `else` there (D86).
        fallback = self._form(node.fallback, t, kept) if missing and node.fallback is not None else ABSENT
        if missing == len(selected) and fallback is ABSENT:
            return ABSENT  # no requested position exists, and no `else` stands in: `count` too (deck p. 50)
        values = [self._at_position(node, timeline, within, p, kept) for p in selected if p is not None]
        if fallback is not ABSENT:
            values.extend([fallback] * missing)
        return self._combine(node.kind, values)

    # -- reading DeciDB's answer and assignments ----------------------------------

    def values_of(self, answer: Sequence[Row]) -> dict[Column, float]:
        """DeciDB's answer, row-aligned with ``rows``, as one value per decision instance.
        Every row of a class must show one value, whole for a whole-numbered decision and
        within its bounds."""
        assert len(answer) == len(self.rows), f"{len(answer)} result rows, expected {len(self.rows)}"
        values: dict[Column, float] = {}
        for name, decision in self.decisions.items():
            for i, row in enumerate(answer):
                if self.classes[name][i] is None:
                    assert row[name] is None, f"{name} has no value on row {i} (D5), shows {row[name]}"
                    continue
                value = float(row[name])
                column = Column(name, self.classes[name][i])
                seen = values.setdefault(column, value)
                assert abs(seen - value) <= _TOLERANCE, f"{name} shows {seen} and {value} on one class (row {i})"
                if decision.var_type != VarType.CONTINUOUS:
                    assert abs(value - round(value)) <= _TOLERANCE, f"{name} = {value} is not whole"
                assert value >= decision.lb - _TOLERANCE, f"{name} = {value} is below {decision.lb}"
                assert (
                    decision.ub is None or value <= decision.ub + _TOLERANCE
                ), f"{name} = {value} is above {decision.ub}"
        return values

    def evaluate(self, form: Mapping, values: Mapping[Column, float]) -> float:
        """A form's number under one value per decision instance."""
        total = 0.0
        for atom, coefficient in form.items():
            total += coefficient * self._atom_value(atom, values)
        return total

    def _atom_value(self, atom, values) -> float:
        if atom == CONSTANT:
            return 1.0
        if isinstance(atom, Column):
            return float(values[atom])
        if isinstance(atom, Extremum):
            pick = max if atom.kind == "max" else min
            return pick(self.evaluate(_thaw(member), values) for member in atom.members)
        if isinstance(atom, Absolute):
            return abs(self.evaluate(_thaw(atom.operand), values))
        raise TypeError(f"unknown atom {atom!r}")

    @staticmethod
    def holds(lhs: float, sense: str) -> bool:
        """``lhs sense 0``; a whole-numbered side misses a strict bound by at least its
        distance to the next whole number, so strict senses are strict up to tolerance."""
        return {
            "<=": lhs <= _TOLERANCE,
            ">=": lhs >= -_TOLERANCE,
            "=": abs(lhs) <= _TOLERANCE,
            "<": lhs < -_TOLERANCE,
            ">": lhs > _TOLERANCE,
            "<>": abs(lhs) > _TOLERANCE,
        }[sense]

    def generate(self, clauses: Sequence[Scope]) -> list[Instance]:
        return [instance for scoped in clauses for instance in self.instances(scoped)]

    def feasible(self, instances: Sequence[Instance], values: Mapping[Column, float]) -> bool:
        for column, value in values.items():
            decision = self.decisions[column.name]
            if value < decision.lb - _TOLERANCE or (decision.ub is not None and value > decision.ub + _TOLERANCE):
                return False
        return all(self.holds(self.evaluate(instance.form, values), instance.sense) for instance in instances)

    def judge(self, answer: Sequence[Row], clauses: Sequence[Scope], objective: Objective | None) -> float:
        """Check DeciDB's answer against every generated instance and return the
        objective it reaches."""
        values = self.values_of(answer)
        for instance in self.generate(clauses):
            lhs = self.evaluate(instance.form, values)
            assert self.holds(
                lhs, instance.sense
            ), f"a generated clause is violated: {self.render(instance)} (left side - right side = {lhs})"
        return self.evaluate(self.objective_form(objective), values)

    def assignments(self, domains: Mapping[str, Iterable[float]]) -> Iterator[dict[Column, float]]:
        """Every assignment of one value from its decision's domain to every instance."""
        columns = self.columns()
        choices = [list(domains[column.name]) for column in columns]
        for values in itertools.product(*choices):
            yield dict(zip(columns, values))

    def brute_force(self, clauses, objective: Objective | None, domains) -> Solution:
        """The optimum over every assignment of ``domains``, read numerically from the
        same generated instances ``solve`` states."""
        instances = self.generate(clauses)
        form = self.objective_form(objective)
        maximize = objective is None or objective.sense == ObjSense.MAXIMIZE
        best, best_values = None, {}
        for values in self.assignments(domains):
            if not self.feasible(instances, values):
                continue
            value = self.evaluate(form, values)
            if best is None or (value > best + _TOLERANCE if maximize else value < best - _TOLERANCE):
                best, best_values = value, values
        if best is None:
            return Solution(SolverStatus.INFEASIBLE)
        return Solution(SolverStatus.OPTIMAL, best, best_values)

    # -- solving ------------------------------------------------------------------

    def solve(self, solver: OracleSolver, clauses: Sequence[Scope], objective: Objective | None) -> Solution:
        """The optimum of the model the language defines, extrema and absolute values
        stated exactly."""
        instances = self.generate(clauses)
        form = self.objective_form(objective)
        solver.create_model("anr_oracle")
        names: dict = {}
        continuous: set = set()
        for column in self.columns():
            decision = self.decisions[column.name]
            names[column] = f"{column.name}#{column.instance}"
            solver.add_variable(names[column], decision.var_type, decision.lb, decision.ub)
            if decision.var_type == VarType.CONTINUOUS:
                continuous.add(names[column])
        counter = itertools.count()

        def coefficients(form: Mapping) -> tuple[dict, float]:
            out: dict = {}
            for atom, coefficient in form.items():
                if atom == CONSTANT or coefficient == 0.0:
                    continue
                name = variable(atom)
                out[name] = out.get(name, 0.0) + coefficient
            return out, form.get(CONSTANT, 0.0)

        def variable(atom) -> str:
            if atom in names:
                return names[atom]
            if isinstance(atom, Extremum):
                members = [coefficients(_thaw(member)) for member in sorted(atom.members, key=repr)]
                z = f"__extremum_{next(counter)}"
                solver.add_variable(z, VarType.CONTINUOUS, float("-inf"), None)
                picks = []
                for k, (member, constant) in enumerate(members):
                    row = {z: 1.0}
                    _add_into(row, member, -1.0)
                    # z >= member for MAX, z <= member for MIN, and z equals one picked member
                    solver.add_constraint(row, ">=" if atom.kind == "max" else "<=", constant)
                    pick = f"{z}_pick{k}"
                    solver.add_variable(pick, VarType.BINARY)
                    solver.add_indicator_constraint(pick, 1, row, "=", constant)
                    picks.append(pick)
                solver.add_constraint({pick: 1.0 for pick in picks}, "=", 1.0)
                names[atom] = z
                return z
            if isinstance(atom, Absolute):
                operand, constant = coefficients(_thaw(atom.operand))
                a = f"__abs_{next(counter)}"
                sign = f"{a}_sign"
                solver.add_variable(a, VarType.CONTINUOUS, 0.0, None)
                solver.add_variable(sign, VarType.BINARY)
                positive = {a: 1.0}
                _add_into(positive, operand, -1.0)
                negative = {a: 1.0}
                _add_into(negative, operand, 1.0)
                solver.add_indicator_constraint(sign, 1, positive, "=", constant)  # a = e
                solver.add_indicator_constraint(sign, 1, operand, ">=", -constant)  # e >= 0
                solver.add_indicator_constraint(sign, 0, negative, "=", -constant)  # a = -e
                solver.add_indicator_constraint(sign, 0, operand, "<=", -constant)  # e <= 0
                names[atom] = a
                return a
            raise TypeError(f"unknown atom {atom!r}")

        for instance in instances:
            row, constant = coefficients(instance.form)
            rhs = -constant
            if instance.sense in ("<", ">", "<>"):
                row, rhs = self._stepped_side(row, rhs, instance.sense, continuous)
            below = math.ceil(rhs - _TOLERANCE) - 1
            above = math.floor(rhs + _TOLERANCE) + 1
            if instance.sense in ("<=", ">=", "="):
                solver.add_constraint(row, instance.sense, rhs)
            elif instance.sense == "<":
                solver.add_constraint(row, "<=", below)
            elif instance.sense == ">":
                solver.add_constraint(row, ">=", above)
            else:
                side = f"__ne_{next(counter)}"
                solver.add_variable(side, VarType.BINARY)
                solver.add_indicator_constraint(side, 1, row, "<=", below)
                solver.add_indicator_constraint(side, 0, row, ">=", above)
        objective_row, objective_constant = coefficients(form)
        solver.set_objective(objective_row, objective.sense if objective else ObjSense.MAXIMIZE)
        result = solver.solve()
        if result.status != SolverStatus.OPTIMAL:
            return Solution(result.status)
        values = {column: result.variable_values[names[column]] for column in self.columns()}
        return Solution(SolverStatus.OPTIMAL, result.objective_value + objective_constant, values)

    @staticmethod
    def _stepped_side(row: dict, rhs: float, sense: str, continuous: set) -> tuple[dict, float]:
        """A strict comparison or ``<>`` stepped onto whole numbers needs a whole-numbered
        side. ``<>`` over AVG's 1/n coefficients is the SUM with the bound times n (the
        spec's hoist, syntax_reference.md §3); a strict comparison over a fractional side,
        or any of the three over a REAL decision, the language refuses."""
        if any(name in continuous for name in row):
            raise Unsupported(f"{sense} over a REAL decision")
        for scale in range(1, 1001):
            if all(abs(c * scale - round(c * scale)) <= _TOLERANCE for c in row.values()):
                break
        else:
            raise Unsupported(f"{sense} over a side no scaling makes whole")
        if scale != 1 and sense != "<>":
            raise Unsupported(f"{sense} over a fractional side")
        return {name: c * scale for name, c in row.items()}, rhs * scale

    # -- rendering ------------------------------------------------------------------

    def label(self, column: Column) -> str:
        """``x3`` for a per-row decision (its row, from 1), ``x[a]`` for a keyed one (its
        key values), ``x`` for a query-wide one."""
        decision = self.decisions[column.name]
        if decision.key is PER_ROW:
            return f"{column.name}{column.instance + 1}"
        if decision.key == GLOBAL:
            return column.name
        row = self.rows[self._first_row[column.name][column.instance]]
        return f"{column.name}[{', '.join(str(v) for v in _key_of(row, decision.key))}]"

    def _atom_text(self, atom) -> tuple[tuple, str]:
        """An atom's text and its place: decision instances first, by name and class
        (``x2`` before ``x10``), then absolute values, then extrema."""
        if isinstance(atom, Column):
            return (0, atom.name, atom.instance, ""), self.label(atom)
        if isinstance(atom, Absolute):
            text = f"abs({self.render_form(_thaw(atom.operand))})"
            return (1, "", 0, text), text
        if isinstance(atom, Extremum):
            members = sorted((self._terms(_thaw(member)), self.render_form(_thaw(member))) for member in atom.members)
            text = f"{atom.kind}({', '.join(rendered for _, rendered in members)})"
            return (2, "", 0, text), text
        raise TypeError(f"unknown atom {atom!r}")

    def _terms(self, form: Mapping) -> list[tuple[float, str]]:
        terms = [
            (coefficient, self._atom_text(atom))
            for atom, coefficient in _thaw(_freeze(form)).items()
            if atom != CONSTANT
        ]
        return [(coefficient, text) for coefficient, (_, text) in sorted(terms, key=lambda term: term[1][0])]

    @staticmethod
    def _number_text(value: float) -> str:
        value = round(value, _DIGITS) + 0.0
        return str(int(value)) if value == int(value) else repr(value)

    def _join(self, terms: list[tuple[float, str]]) -> str:
        out = ""
        for k, (coefficient, text) in enumerate(terms):
            magnitude = abs(coefficient)
            piece = text if math.isclose(magnitude, 1.0) else f"{self._number_text(magnitude)} * {text}"
            if k == 0:
                out = piece if coefficient > 0 else f"-{piece}"
            else:
                out += f" + {piece}" if coefficient > 0 else f" - {piece}"
        return out

    def render_form(self, form: Mapping) -> str:
        """A form as an expression: its terms in a fixed order, then its constant."""
        terms = self._terms(form)
        constant = round(form.get(CONSTANT, 0.0), _DIGITS)
        text = self._join(terms)
        if not text:
            return self._number_text(constant)
        if constant:
            text += f" + {self._number_text(constant)}" if constant > 0 else f" - {self._number_text(-constant)}"
        return text

    def render(self, instance: Instance) -> str:
        """An instance as ``terms sense bound``, its first term positive (the sense flips
        with the sign), so two spellings of one comparison render alike."""
        terms = self._terms(instance.form)
        bound = -instance.form.get(CONSTANT, 0.0)
        sense = instance.sense
        if terms and terms[0][0] < 0:
            terms = [(-coefficient, text) for coefficient, text in terms]
            bound, sense = -bound, _FLIP[sense]
        return f"{self._join(terms) or '0'} {sense} {self._number_text(bound)}"
