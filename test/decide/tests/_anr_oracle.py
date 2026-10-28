"""Reference evaluator for the full ANR language: variant signatures, reducers nested to
any depth, and frame selectors.

It reads no SQL. A test states the query's result rows, its decisions with their keys and
its clauses as expression trees in plain Python; the evaluator generates the instances the
language defines and then either solves them through the ``OracleSolver`` interface or
judges DeciDB's answer against them. The definitions it follows are the plan's
(``context/descriptions/09_anr_language/anr_algorithm_and_frames_plan.md`` §3, decisions
D64-D79), read literally, with no algorithmic shortcut:

- **A scoped expression** ``[when c] [per K]: body`` keeps the rows ``c`` keeps, splits them
  into classes of ``K``, and produces one value per distinct *signature* in each class,
  read at the signature's first kept row (§3.1, §3.2). ``per row`` keeps every tuple
  occurrence.
- **A signature** is the tuple of the body's fields: the net value of its reducer-free,
  frame-free additive terms, one field per additive context (D77: the canonical tree, so
  ``a + sum(y) + b`` and ``a + b + sum(y)`` have the same fields); for each reducer, its
  factor and its ``by`` value; for each frame, its ``within`` key, the anchor's order key
  when the selector is relative (D76), and the fallback where the anchor reaches it.
- **A reducer** reads the kept rows of the enclosing scope that share the current row's
  ``by`` value -- all of them, not only the current class -- evaluates its operand there,
  and combines the operand's values with multiplicity kept (§3.1, §3.3).
- **A frame** partitions the enclosing scope's kept rows by ``within``, orders each
  partition into peers (rows with equal order keys, NULL last as DuckDB's default orders
  it, D68), and navigates by peer position (§3.4). A missing position is *absent*: it
  skips a comparison and drops a reducer member (§3.5). A fallback is evaluated at the
  anchor, and only where a position is missing.

Values are symbolic: a linear form over decision instances (``Column``), extremum helpers
(``Extremum``) and absolute values (``Absolute``), with a constant under ``CONSTANT``.
Signatures freeze those forms by identity before any number is substituted, so two
instances that would solve to the same value never collapse. ``solve`` states extrema and
absolute values exactly (pick and sign indicators); ``judge``, ``feasible`` and
``brute_force`` evaluate the same generated instances numerically, so the solved model and
the numeric reading cannot drift apart.

The run-time errors of the language are ``ScopeError`` with the fixed phrase DeciDB's
message carries. A construct the language refuses at bind or plan time (§3.7) raises
``Unsupported``: the oracle has no answer for it.
"""

from __future__ import annotations

import itertools
import math
from dataclasses import dataclass, field
from typing import Callable, Iterable, Iterator, Mapping, Sequence

from solver.base import OracleSolver
from solver.types import ObjSense, SolverStatus, VarType

Row = Mapping[str, object]
#: A key: row columns, or a function of the row returning a tuple.
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
        "kept_no_rows": "matches no rows",
        "reducer_empty": "has no rows",
        "objective_variants": "would be produced more than once",
        "objective_absent": "selects no position",
        "frame_partition": "does not determine the timeline",
        "ambiguous_peer": "a position must hold one row",
        "null_value": "is NULL",
    }

    def __init__(self, kind: str, detail: str):
        super().__init__(f"{ScopeError.PHRASES[kind]}: {detail}")
        self.kind = kind
        self.phrase = ScopeError.PHRASES[kind]


class Unsupported(Exception):
    """A construct the language refuses at bind or plan time (plan §3.7, D74)."""


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
            raise Unsupported(f"{self.k} must be a positive whole number")  # D71

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
            raise Unsupported(f"{self.every} must be a positive whole number")  # D71
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
    """``MAXIMIZE | MINIMIZE [when c]: body``, generated once (``per ()``, D66)."""

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


#: A frame that selects nothing, or a reducer left with no member (§3.5).
ABSENT = _Marker("ABSENT")
#: A NULL data value, kept only where ``count`` reads it.
NULL = _Marker("NULL")
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


def _key_of(row: Row, key) -> tuple:
    return tuple(key(row)) if callable(key) else tuple(row[column] for column in key)


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
            class_id = ids.setdefault(_key_of(row, key), len(ids))
            first.setdefault(class_id, i)
            out.append(class_id)
        return out, first

    def representative(self, column: Column) -> int:
        """The first row of the decision instance's class."""
        return self._first_row[column.name][column.instance]

    def columns(self) -> list[Column]:
        """Every decision instance, one solver column each."""
        return [Column(name, class_id) for name in self.decisions for class_id in sorted(set(self.classes[name]))]

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

    def objective_form(self, objective: Objective | None) -> dict:
        """The objective's one value (D66): one class, which must hold one signature."""
        if objective is None:
            return {}
        kept = self._root_kept(objective.when)
        if not kept:
            return {}
        signatures = {}
        for t in kept:
            signature = self._signature(objective.body, t, kept, null_ok=False)
            if signature is not _SKIP:
                signatures.setdefault(signature, t)
        if len(signatures) > 1:
            raise ScopeError("objective_variants", f"{len(signatures)} signatures")
        at = next(iter(signatures.values()), kept[0])
        value = self._form(objective.body, at, kept)
        if value is ABSENT:
            raise ScopeError("objective_absent", "the objective is produced once")
        return value

    def _root_kept(self, when) -> list[int]:
        everything = list(range(len(self.rows)))
        kept = self._kept(everything, when)
        if when is not None and self.rows and not kept:
            raise ScopeError("kept_no_rows", "when")  # D4
        return kept

    def _kept(self, relation: Sequence[int], when) -> list[int]:
        return [i for i in relation if when is None or when(self.rows[i]) is True]

    def _split(self, relation: Sequence[int], per) -> list[list[int]]:
        if per is PER_ROW:
            return [[i] for i in relation]
        classes: dict[tuple, list[int]] = {}
        for i in relation:
            classes.setdefault(_key_of(self.rows[i], per), []).append(i)
        return list(classes.values())

    def _scope_values(
        self, body, when, per, relation, *, prefiltered=False, reducer=None, null_ok=False
    ) -> list[tuple[int, object]]:
        """``[when] [per]: body`` evaluated on ``relation``: one ``(row, value)`` per
        distinct signature of each class, classes in first-seen order (§3.1, §3.2)."""
        kept = list(relation) if prefiltered else self._kept(relation, when)
        if not kept:
            if reducer is not None:
                raise ScopeError("reducer_empty", f"{reducer.kind} for by value")  # D8
            return []
        out = []
        for class_rows in self._split(kept, per):
            if per is not PER_ROW:
                self._check_timelines(body, class_rows, kept, per)
            chosen: dict = {}
            for t in class_rows:
                signature = self._signature(body, t, kept, null_ok)
                if signature is not _SKIP:
                    chosen.setdefault(signature, t)
            for t in chosen.values():
                out.append((t, self._value(body, t, kept, null_ok)))
        return out

    def _value(self, body, t, kept, null_ok):
        try:
            return self._form(body, t, kept)
        except ScopeError as error:
            if null_ok and error.kind == "null_value":
                return NULL
            raise

    # -- signatures ---------------------------------------------------------

    def _signature(self, body, t, kept, null_ok):
        """Row ``t``'s fields, or ``_SKIP`` when its value is absent and a field it would
        never read is NULL (nothing is computed on a skipped row)."""
        try:
            return self._fields(body, t, kept, null_ok)
        except ScopeError as error:
            if error.kind == "null_value" and self._form(body, t, kept) is ABSENT:
                return _SKIP
            raise

    def _collect(self, expr, scale: float, data: list, other: list) -> None:
        """Flatten one additive context through ``+``, ``-`` and constant factors, as the
        canonical tree does (D77): its data terms are one field, so ``x + a <= b + sum(y)``
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

    def _fields(self, expr, t, kept, null_ok=False) -> tuple:
        """The fields of one additive context: each other term's fields, then the net
        value of the data terms as one field (D77)."""
        data, other = [], []
        self._collect(expr, 1.0, data, other)
        fields = [self._term_fields(node, t, kept, null_ok) for node in other]
        if data:
            total: dict = {}
            try:
                for scale, node in data:
                    _add_into(total, self._form(node, t, kept), scale)
                fields.append(("terms", _freeze(total)))
            except ScopeError as error:
                if not (null_ok and error.kind == "null_value"):
                    raise
                fields.append(("terms", NULL))
        return tuple(fields)

    def _term_fields(self, node, t, kept, null_ok) -> tuple:
        """A term that is not data: a decision by its instance, whatever its coefficient
        (D77); a linear form by its every coefficient; a reducer by its ``by`` value; a
        frame by its keys (D76); and a product or an ``abs`` by the fields of each
        context it holds."""
        if isinstance(node, Var):
            return ("decision", frozenset(self._form(node, t, kept)))
        if isinstance(node, Terms):
            form = self._form(node, t, kept)
            return ("linear", frozenset((atom, round(float(c), _DIGITS)) for atom, c in form.items()))
        if isinstance(node, Agg):
            return ("by", _key_of(self.rows[t], node.by))
        if isinstance(node, (At, Range)):
            return ("frame",) + self._frame_fields(node, t, kept)
        if isinstance(node, Mul):
            return ("mul", self._fields(node.left, t, kept, null_ok), self._fields(node.right, t, kept, null_ok))
        if isinstance(node, Abs):
            return ("abs", self._fields(node.operand, t, kept, null_ok))
        raise TypeError(f"unexpected node {node!r}")

    def _frame_fields(self, node, t, kept) -> tuple:
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
        """Free of decisions, reducers and frames: a data term of its context (D77)."""
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
        """Free of reducers and frames (a maximal subtree of these is a field, D77)."""
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
        """D69: a ``per K`` class must lie in one ``within`` partition of every frame
        written directly in its body."""
        for frame in self._direct_frames(body):
            partitions = {_key_of(self.rows[t], frame.over.within) for t in class_rows}
            if len(partitions) > 1:
                raise ScopeError(
                    "frame_partition", f"per {per}: {len(partitions)} partitions {sorted(map(repr, partitions))}"
                )

    # -- forms ----------------------------------------------------------------

    def _number(self, value) -> float:
        if value is None:
            raise ScopeError("null_value", "a coefficient or bound")
        return float(value)

    def _column(self, name: str, i: int) -> Column:
        if name not in self.decisions:
            raise KeyError(f"undeclared decision {name!r}")
        return Column(name, self.classes[name][i])

    def _form(self, expr, i, kept):
        """``expr`` at row ``i`` of the scope whose kept rows are ``kept``: a linear form,
        or ``ABSENT``. A NULL beside an absent value is never read."""
        row = self.rows[i]
        if isinstance(expr, Const):
            return {CONSTANT: float(expr.value)}
        if isinstance(expr, Col):
            return {CONSTANT: self._number(expr.at(row))}
        if isinstance(expr, Var):
            return {self._column(expr.name, i): 1.0}
        if isinstance(expr, Terms):
            out: dict = {}
            for name, coefficient in expr.fn(row).items():
                atom = CONSTANT if name == CONSTANT else self._column(name, i)
                out[atom] = out.get(atom, 0.0) + self._number(coefficient)
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
        """Every operand's form, or ``ABSENT`` if one is absent. A NULL error is raised
        only when no operand is absent."""
        forms, null_error = [], None
        for operand in operands:
            try:
                forms.append(self._form(operand, i, kept))
            except ScopeError as error:
                if error.kind != "null_value":
                    raise
                null_error = null_error or error
                forms.append(None)
        if any(form is ABSENT for form in forms):
            return ABSENT
        if null_error is not None:
            raise null_error
        return forms

    # -- reducers -------------------------------------------------------------

    def _reducer(self, node: Agg, i, kept):
        """``node`` for row ``i``: the kept rows sharing ``i``'s ``by`` value, the operand
        evaluated there, its values combined (§3.1, §3.3)."""
        if node.kind not in ("sum", "avg", "min", "max", "count"):
            raise Unsupported(f"reducer {node.kind!r}")
        group = _key_of(self.rows[i], node.by)
        relation = [u for u in kept if _key_of(self.rows[u], node.by) == group]
        operand = node.operand
        members = self._scope_values(
            operand.body, operand.when, operand.per, relation, reducer=node, null_ok=node.kind == "count"
        )
        return self._combine(node.kind, [value for _, value in members if value is not ABSENT])

    def _combine(self, kind: str, values: list):
        if kind == "count":
            if any(value is not NULL and not _is_constant(value) for value in values):
                raise Unsupported("count over decisions")  # COUNT is data only (§3.3)
            return {CONSTANT: float(sum(1 for value in values if value is not NULL))}
        if any(value is NULL for value in values):
            raise ScopeError("null_value", "a reducer member")
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
            partitions.setdefault(_key_of(self.rows[i], over.within), []).append(i)
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
                ordered.append(groups[None])  # NULLS LAST in either direction (D68)
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
                raise Unsupported("a reducer or frame inside a frame")  # D74

    def _point(self, node: At, t, kept):
        self._require_pure(node)
        timeline = self._timeline(node.over, kept)
        within, anchor = timeline.position[t]
        peers = timeline.peers[within]
        q = self._wrap(node.position.lift(anchor, len(peers)), len(peers), node.over.cyclic)
        if q is None:
            return ABSENT if node.fallback is None else self._form(node.fallback, t, kept)
        candidates: dict = {}
        for r in peers[q]:
            value = self._form(node.payload, r, kept)
            candidates.setdefault(_freeze(value), value)
        if len(candidates) > 1:
            raise ScopeError("ambiguous_peer", f"{len(peers[q])} rows at {within}, {timeline.order[peers[q][0]]}")
        return next(iter(candidates.values()))

    def _payload(self, node: Range, r, kept):
        """The payload at a selected row; ``count`` skips a NULL rather than reading it
        as a coefficient (D78 makes a NULL payload an error everywhere else)."""
        try:
            return self._form(node.payload, r, kept)
        except ScopeError as error:
            if node.kind == "count" and error.kind == "null_value":
                return NULL
            raise

    def _range(self, node: Range, t, kept):
        self._require_pure(node)
        if node.kind not in ("sum", "avg", "min", "max", "count"):
            raise Unsupported(f"range reducer {node.kind!r}")
        timeline = self._timeline(node.over, kept)
        within, anchor = timeline.position[t]
        peers = timeline.peers[within]
        values, missing = [], 0
        for q in self._requested(node, anchor, len(peers)):
            p = self._wrap(q, len(peers), node.over.cyclic)
            if p is None:
                missing += 1
                continue
            values.extend(self._payload(node, r, kept) for r in peers[p])
        if node.all and missing:
            return ABSENT
        if missing and node.fallback is not None:
            values.extend([self._form(node.fallback, t, kept)] * missing)
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
        for column in self.columns():
            decision = self.decisions[column.name]
            names[column] = f"{column.name}#{column.instance}"
            solver.add_variable(names[column], decision.var_type, decision.lb, decision.ub)
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
