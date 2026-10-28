"""Reference evaluator for the DECIDE scoping language (`per`, `when`, `by`).

It reads no SQL. A test states the query's result rows, its decisions with their keys
and its clauses as plain Python, and the evaluator builds the model the language
defines -- one solver column per class of each decision's key -- through the
``OracleSolver`` interface. ``judge`` then checks DeciDB's own answer against the same
definition: every row of a class shows one value, every clause holds, and the objective
it reaches is returned for comparison with the oracle's optimum.

``ScopeModel`` covers keyed declarations (`decide per K: x(TYPE)`): a decision is per
row, per a key of row columns, or one for the whole query; a clause is linear and is
either generated once per row or summed over all rows.

``ScopedModel`` adds the clause scopes (`when`, `per`, `by`, a scope inside a reducer,
the objective's `when` and its two-level form) and follows `ANR Implementation.pdf`
literally: filter the rows, split the kept rows into classes, generate the body once per
class, and evaluate every reducer on the kept rows sharing the class's `by` value, after
its own `when` and one value per class of its own `per`. It raises ``ScopeError`` for
the three run-time errors of the language before any model is built.
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field
from typing import Callable, Mapping, Sequence

from solver.base import OracleSolver
from solver.types import ObjSense, SolverStatus, VarType

Row = Mapping[str, object]
#: One row's linear terms: decision name -> coefficient.
Terms = dict[str, float]

#: No `per`: one decision per result row.
PER_ROW = None
#: `per ()`: one decision for the whole query.
GLOBAL = ()

_TOLERANCE = 1e-6


@dataclass(frozen=True)
class RelationColumns:
    """A key element ``per D`` stands for: D's columns, which compare NULL as a value. A
    plain column name in a key is written, and a NULL there puts the row in no class."""

    columns: tuple[str, ...]


def key_value(row: Row, key) -> tuple | None:
    """The row's value of ``key`` (column names, ``RelationColumns`` or a function of the
    row), or None when the row is in no class: a written element is NULL."""
    if callable(key):
        value = tuple(key(row))
        return None if any(v is None for v in value) else value
    value = []
    for element in key:
        if isinstance(element, RelationColumns):
            value.extend(row[column] for column in element.columns)
        elif row[element] is None:
            return None
        else:
            value.append(row[element])
    return tuple(value)


@dataclass(frozen=True)
class Decision:
    name: str
    #: ``PER_ROW``, ``GLOBAL``, or the key's elements (see ``key_value``).
    key: tuple | None = PER_ROW
    var_type: VarType = VarType.INTEGER
    lb: float = 0.0
    ub: float | None = None


@dataclass(frozen=True)
class RowClause:
    """``terms(row) sense rhs(row)``, generated once per row."""

    terms: Callable[[Row], Terms]
    sense: str
    rhs: Callable[[Row], float]


@dataclass(frozen=True)
class SumClause:
    """``SUM(terms(row)) sense rhs``, one clause over all rows."""

    terms: Callable[[Row], Terms]
    sense: str
    rhs: float


@dataclass(frozen=True)
class Objective:
    """``SUM(rows(row)) + once``: a reducer over the rows plus bare query-wide decisions,
    whose coefficients count once rather than once per row."""

    sense: ObjSense
    rows: Callable[[Row], Terms] = lambda row: {}
    once: Terms = field(default_factory=dict)


def class_ids(rows: Sequence[Row], key) -> list[int | None]:
    """The class of every row under ``key``, numbered in first-seen order. Rows with
    equal key values share a class; a row in no class (``key_value`` None) has no
    decision, and its class is None."""
    if key is None:
        return list(range(len(rows)))
    ids: dict[tuple, int] = {}
    classes = []
    for row in rows:
        value = key_value(row, key)
        classes.append(None if value is None else ids.setdefault(value, len(ids)))
    return classes


def _holds(lhs: float, sense: str, rhs: float) -> bool:
    if sense == "<=":
        return lhs <= rhs + _TOLERANCE
    if sense == ">=":
        return lhs >= rhs - _TOLERANCE
    if sense == "=":
        return abs(lhs - rhs) <= _TOLERANCE
    raise ValueError(f"unknown sense {sense!r}")


class ScopeModel:
    """The rows and decisions of one query, with each decision's classes."""

    def __init__(self, rows: Sequence[Row], decisions: Sequence[Decision]):
        self.rows = list(rows)
        self.decisions = {decision.name: decision for decision in decisions}
        self.classes = {decision.name: class_ids(self.rows, decision.key) for decision in decisions}

    def class_count(self, name: str) -> int:
        """How many decisions ``name`` stands for: one per class among the rows."""
        return len(set(self.classes[name]) - {None})

    def column_count(self) -> int:
        return sum(self.class_count(name) for name in self.decisions)

    def _absent(self, terms: Terms, row_index: int) -> bool:
        """Whether a decision ``terms`` reads has no value on the row: it reads like a NULL."""
        return any(name in self.classes and self.classes[name][row_index] is None for name in terms)

    def _row_coefficients(self, terms: Terms, row_index: int, into: dict[tuple[str, int], float]) -> bool:
        """Add the row's terms; False (nothing added) when a decision they read is absent there."""
        if self._absent(terms, row_index):
            return False
        for name, coefficient in terms.items():
            column = (name, self.classes[name][row_index])
            into[column] = into.get(column, 0.0) + coefficient
        return True

    def _once_coefficients(self, terms: Terms, into: dict[tuple[str, int], float]) -> None:
        for name, coefficient in terms.items():
            if self.decisions[name].key != GLOBAL:
                raise ValueError(f"only a query-wide decision counts once; {name!r} is not one")
            into[(name, 0)] = into.get((name, 0), 0.0) + coefficient

    def solve(self, solver: OracleSolver, clauses: Sequence[RowClause | SumClause], objective: Objective) -> float:
        """The optimum of the model the language defines."""

        def label(column: tuple[str, int]) -> str:
            return f"{column[0]}#{column[1]}"

        solver.create_model("scope_oracle")
        for name, decision in self.decisions.items():
            for class_id in sorted(set(self.classes[name]) - {None}):
                solver.add_variable(label((name, class_id)), decision.var_type, decision.lb, decision.ub)
        for clause in clauses:
            if isinstance(clause, RowClause):
                for row_index, row in enumerate(self.rows):
                    coefficients: dict[tuple[str, int], float] = {}
                    if not self._row_coefficients(clause.terms(row), row_index, coefficients):
                        continue  # an absent decision skips the instance
                    solver.add_constraint(
                        {label(c): v for c, v in coefficients.items()}, clause.sense, float(clause.rhs(row))
                    )
            else:
                coefficients = {}
                for row_index, row in enumerate(self.rows):
                    self._row_coefficients(clause.terms(row), row_index, coefficients)
                solver.add_constraint({label(c): v for c, v in coefficients.items()}, clause.sense, float(clause.rhs))
        coefficients = {}
        for row_index, row in enumerate(self.rows):
            self._row_coefficients(objective.rows(row), row_index, coefficients)
        self._once_coefficients(objective.once, coefficients)
        solver.set_objective({label(c): v for c, v in coefficients.items()}, objective.sense)
        result = solver.solve()
        assert result.status == SolverStatus.OPTIMAL, f"oracle model is not optimal: {result.status}"
        return result.objective_value

    def judge(self, answer: Sequence[Row], clauses: Sequence[RowClause | SumClause], objective: Objective) -> float:
        """Check DeciDB's answer -- its result rows, aligned with ``self.rows`` -- and
        return the objective it reaches. Every row of a class must show one value, a
        whole-numbered decision must be whole, and every clause must hold."""
        assert len(answer) == len(self.rows), f"{len(answer)} result rows, expected {len(self.rows)}"
        values: dict[tuple[str, int], float] = {}
        for name, decision in self.decisions.items():
            for row_index, row in enumerate(answer):
                if self.classes[name][row_index] is None:
                    assert row[name] is None, f"{name} = {row[name]} on row {row_index}, which has no decision"
                    continue
                value = float(row[name])
                column = (name, self.classes[name][row_index])
                seen = values.setdefault(column, value)
                assert abs(seen - value) <= _TOLERANCE, (
                    f"{name} shows {seen} and {value} on rows of one class " f"(key {decision.key}, row {row_index})"
                )
                if decision.var_type != VarType.CONTINUOUS:
                    assert abs(value - round(value)) <= _TOLERANCE, f"{name} = {value} is not whole"
                lower_ok = value >= decision.lb - _TOLERANCE
                upper_ok = decision.ub is None or value <= decision.ub + _TOLERANCE
                assert lower_ok and upper_ok, f"{name} = {value} is outside [{decision.lb}, {decision.ub}]"

        def evaluate(coefficients: dict[tuple[str, int], float]) -> float:
            return sum(coefficient * values[column] for column, coefficient in coefficients.items())

        for clause in clauses:
            if isinstance(clause, RowClause):
                for row_index, row in enumerate(self.rows):
                    coefficients: dict[tuple[str, int], float] = {}
                    if not self._row_coefficients(clause.terms(row), row_index, coefficients):
                        continue  # an absent decision skips the instance
                    lhs = evaluate(coefficients)
                    assert _holds(
                        lhs, clause.sense, float(clause.rhs(row))
                    ), f"row {row_index} violates a clause: {lhs} {clause.sense} {clause.rhs(row)}"
            else:
                coefficients = {}
                for row_index, row in enumerate(self.rows):
                    self._row_coefficients(clause.terms(row), row_index, coefficients)
                lhs = evaluate(coefficients)
                assert _holds(
                    lhs, clause.sense, float(clause.rhs)
                ), f"a sum clause is violated: {lhs} {clause.sense} {clause.rhs}"
        coefficients = {}
        for row_index, row in enumerate(self.rows):
            self._row_coefficients(objective.rows(row), row_index, coefficients)
        self._once_coefficients(objective.once, coefficients)
        return evaluate(coefficients)


# ---------------------------------------------------------------------------
# Scoped clauses: `when`, `per` and `by`
# ---------------------------------------------------------------------------

#: The data part of a ``Terms`` value: ``{CONSTANT: 3.0, "x": 2.0}`` is ``3 + 2 * x``.
CONSTANT = ""

#: A linear expression over model columns: (decision, class) or an auxiliary name, with
#: the constant under ``CONSTANT``.
Linear = dict


class ScopeError(Exception):
    """One of the run-time errors of the language, raised before any model is built."""

    #: The fixed phrase DeciDB's message carries for each kind.
    PHRASES = {
        "not_determined": "does not determine",
    }

    def __init__(self, kind: str, detail: str):
        super().__init__(f"{ScopeError.PHRASES[kind]}: {detail}")
        self.kind = kind
        self.phrase = ScopeError.PHRASES[kind]


@dataclass(frozen=True)
class Reduce:
    """``factor * agg([when c] [per K]: body) by (key)``.

    ``body`` gives one row's value (decisions and a ``CONSTANT``); ``when`` keeps a row
    when it returns True (False and None, SQL's NULL, drop it); ``per`` is ``PER_ROW``,
    ``GLOBAL`` or key columns; ``by`` is key columns or a function of the row returning
    a tuple. ``factor`` is a tuple term, read on the row the class is generated from."""

    agg: str
    body: Callable[[Row], Terms]
    when: Callable[[Row], object] | None = None
    per: tuple[str, ...] | None = PER_ROW
    by: tuple[str, ...] | Callable[[Row], tuple] = ()
    factor: Callable[[Row], float] = lambda row: 1.0


@dataclass(frozen=True)
class Nested:
    """``outer(per key: inner(body) by (key))``: the two-level objective."""

    outer: str
    key: tuple[str, ...]
    inner: str
    body: Callable[[Row], Terms]


#: A part of a clause: a tuple term (a function of the row) or a ``Reduce``.
Part = object


@dataclass(frozen=True)
class Scoped:
    """``[when c] [per K]: lhs sense rhs``, each side the sum of its parts. ``sense`` is
    one of ``<=``, ``>=``, ``=``, ``<``, ``>``, ``<>``; ``<``, ``>`` and ``<>`` step onto the
    whole numbers (``< K`` is ``<= ceil(K) - 1``, ``> K`` is ``>= floor(K) + 1``), which is
    exact only for whole-numbered decision sides (not an AVG)."""

    lhs: tuple
    sense: str
    rhs: tuple = ()
    when: Callable[[Row], object] | None = None
    per: tuple[str, ...] | None = PER_ROW


@dataclass(frozen=True)
class ScopedObjective:
    """``MAXIMIZE|MINIMIZE [when c]: sum of parts``, generated once (``per ()``)."""

    sense: ObjSense
    parts: tuple
    when: Callable[[Row], object] | None = None


def _kept(rows: Sequence[Row], when) -> list[int]:
    return [i for i, row in enumerate(rows) if when is None or when(row) is True]


def _split(rows: Sequence[Row], indices: Sequence[int], key) -> list[list[int]]:
    """``indices`` split into classes of ``key`` in first-seen order; ``PER_ROW`` gives
    one class per row and ``GLOBAL`` one class (none when there is no row). A row in no
    class of ``key`` is in none of them."""
    if key is PER_ROW:
        return [[i] for i in indices]
    classes: dict[tuple, list[int]] = {}
    for i in indices:
        value = key_value(rows[i], key)
        if value is not None:
            classes.setdefault(value, []).append(i)
    return list(classes.values())


def _add(into: Linear, expr: Linear, scale: float = 1.0) -> None:
    for column, coefficient in expr.items():
        into[column] = into.get(column, 0.0) + scale * coefficient


class ScopedModel(ScopeModel):
    """``ScopeModel`` with the clause scopes. Reducer and comparison helpers keep a
    symbolic form (columns plus auxiliaries, for ``solve``) and a numeric one (for
    ``judge``), built by the same generation code so the two cannot drift apart."""

    # -- one row ------------------------------------------------------------

    def _linear(self, terms: Terms, row_index: int) -> Linear | None:
        """``terms`` on row ``row_index``, each decision replaced by its class column; None (SQL's NULL) when a
        coefficient is None or a decision has no value on the row."""
        if any(coefficient is None for coefficient in terms.values()) or self._absent(terms, row_index):
            return None
        expr: Linear = {}
        for name, coefficient in terms.items():
            column = CONSTANT if name == CONSTANT else (name, self.classes[name][row_index])
            expr[column] = expr.get(column, 0.0) + float(coefficient)
        return expr

    def _require_determined(self, class_rows: Sequence[int], key_name: str, value) -> None:
        """Every row of the class shows the same ``value(row_index)``."""
        first = value(class_rows[0])
        for i in class_rows[1:]:
            if value(i) != first:
                raise ScopeError("not_determined", f"per {key_name}, rows {class_rows[0]} and {i}")

    # -- reducers -----------------------------------------------------------

    def _reducer(self, reduce: Reduce, kept: Sequence[int], at: int, emit) -> Linear | None:
        """The value of ``reduce`` for the class generated from row ``at``: the kept rows
        sharing its ``by`` value, then its own ``when``, then one value per class of its
        own ``per``, then the aggregate. A row whose body is NULL is skipped, as SQL's
        aggregates skip one, and a reducer left with no row is NULL (None). ``emit`` turns
        MIN/MAX over decisions into an auxiliary (``solve``) or a number (``judge``)."""
        group = key_value(self.rows[at], reduce.by)
        if group is None:
            return None  # the row is in no group of the `by` key: the reducer has no value there
        read = [i for i in kept if key_value(self.rows[i], reduce.by) == group]
        read = [i for i in read if reduce.when is None or reduce.when(self.rows[i]) is True]
        read = [i for i in read if self._linear(reduce.body(self.rows[i]), i) is not None]
        if not read:
            return None
        members = []
        for inner in _split(self.rows, read, reduce.per):
            self._require_determined(
                inner, f"{reduce.per} inside {reduce.agg}", lambda i: self._linear(reduce.body(self.rows[i]), i)
            )
            members.append(self._linear(reduce.body(self.rows[inner[0]]), inner[0]))
        if reduce.agg in ("sum", "avg"):
            total: Linear = {}
            for member in members:
                _add(total, member, 1.0 if reduce.agg == "sum" else 1.0 / len(members))
            return total
        if reduce.agg in ("min", "max"):
            if all(set(member) <= {CONSTANT} for member in members):
                pick = max if reduce.agg == "max" else min
                return {CONSTANT: pick(member.get(CONSTANT, 0.0) for member in members)}
            return emit(reduce.agg, members)
        raise ValueError(f"unknown reducer {reduce.agg!r}")

    def _part(self, part, kept: Sequence[int], at: int, emit) -> Linear | None:
        """One part's value on the class generated from row ``at``; None when it is NULL."""
        if isinstance(part, Reduce):
            reduced = self._reducer(part, kept, at, emit)
            factor = part.factor(self.rows[at])
            if reduced is None or factor is None:
                return None
            value: Linear = {}
            _add(value, reduced, float(factor))
            return value
        return self._linear(part(self.rows[at]), at)

    def _determined_parts(self, parts, class_rows: Sequence[int], key_name: str) -> None:
        """The determination rule for a generated class: every tuple term, every factor
        and every ``by`` value is one value over the class."""
        for part in parts:
            if isinstance(part, Reduce):
                self._require_determined(class_rows, key_name, lambda i: float(part.factor(self.rows[i])))
                self._require_determined(class_rows, key_name, lambda i: key_value(self.rows[i], part.by))
            else:
                self._require_determined(class_rows, key_name, lambda i: self._linear(part(self.rows[i]), i))

    # -- clauses ------------------------------------------------------------

    def _generate(self, clause: Scoped, emit) -> list[tuple[Linear, str]]:
        """``clause`` as one ``lhs - rhs sense 0`` per class of its ``per`` key. An instance
        with a NULL part is skipped."""
        kept = _kept(self.rows, clause.when)
        generated = []
        for class_rows in _split(self.rows, kept, clause.per):
            if clause.per is not PER_ROW:
                self._determined_parts(clause.lhs + clause.rhs, class_rows, str(clause.per))
            at = class_rows[0]
            sides = [(self._part(part, kept, at, emit), 1.0) for part in clause.lhs]
            sides += [(self._part(part, kept, at, emit), -1.0) for part in clause.rhs]
            if any(value is None for value, _ in sides):
                continue
            expr: Linear = {}
            for value, scale in sides:
                _add(expr, value, scale)
            generated.append((expr, clause.sense))
        return generated

    def _objective(self, objective: ScopedObjective, emit) -> Linear:
        kept = _kept(self.rows, objective.when)
        if not kept:
            return {}
        flat = tuple(part for part in objective.parts if not isinstance(part, Nested))
        self._determined_parts(flat, kept, "()")
        expr: Linear = {}
        for part in objective.parts:
            if isinstance(part, Nested):
                values = []
                for class_rows in _split(self.rows, kept, part.key):
                    inner = Reduce(part.inner, part.body, by=part.key)
                    value = self._reducer(inner, kept, class_rows[0], emit)
                    if value is not None:
                        values.append(value)
                if not values:
                    continue  # an outer reducer over no class is NULL: the term is dropped
                if part.outer in ("sum", "avg"):
                    for value in values:
                        _add(expr, value, 1.0 if part.outer == "sum" else 1.0 / len(values))
                else:
                    _add(expr, emit(part.outer, values))
            else:
                # A NULL term of the objective is dropped.
                value = self._part(part, kept, kept[0], emit)
                if value is not None:
                    _add(expr, value)
        return expr

    def check(self, clauses: Sequence[Scoped], objective: ScopedObjective | None = None) -> None:
        """Raise the ``ScopeError`` the language defines for this data, if any."""

        def numeric(agg, members):
            return {CONSTANT: 0.0}

        for clause in clauses:
            self._generate(clause, numeric)
        if objective is not None:
            self._objective(objective, numeric)

    # -- solving ------------------------------------------------------------

    def solve_scoped(self, solver: OracleSolver, clauses: Sequence[Scoped], objective: ScopedObjective) -> float:
        """The optimum of the model the language defines (``ScopeError`` if the data
        makes the query invalid)."""

        def label(column) -> str:
            return column if isinstance(column, str) else f"{column[0]}#{column[1]}"

        auxiliaries = []

        def emit(agg, members):
            # z = max (min) of the members, exactly: z on the far side of every member,
            # and one chosen member equal to it.
            z = f"__{agg}_{len(auxiliaries)}"
            auxiliaries.append(z)
            solver.add_variable(z, VarType.CONTINUOUS, float("-inf"), None)
            picks = []
            for k, member in enumerate(members):
                coefficients = {label(c): v for c, v in member.items() if c != CONSTANT}
                coefficients[z] = coefficients.get(z, 0.0) - 1.0
                constant = member.get(CONSTANT, 0.0)
                # member - z <= 0 for MAX, >= 0 for MIN
                solver.add_constraint(coefficients, "<=" if agg == "max" else ">=", -constant)
                pick = f"{z}_pick{k}"
                solver.add_variable(pick, VarType.BINARY)
                picks.append(pick)
                solver.add_indicator_constraint(pick, 1, coefficients, "=", -constant)
            solver.add_constraint({pick: 1.0 for pick in picks}, "=", 1.0)
            return {z: 1.0}

        solver.create_model("scoped_oracle")
        for name, decision in self.decisions.items():
            for class_id in sorted(set(self.classes[name]) - {None}):
                solver.add_variable(label((name, class_id)), decision.var_type, decision.lb, decision.ub)
        for clause in clauses:
            for expr, sense in self._generate(clause, emit):
                coefficients = {label(c): v for c, v in expr.items() if c != CONSTANT}
                rhs = -expr.get(CONSTANT, 0.0)
                below = math.ceil(rhs - _TOLERANCE) - 1
                above = math.floor(rhs + _TOLERANCE) + 1
                if sense in ("<=", ">=", "="):
                    solver.add_constraint(coefficients, sense, rhs)
                elif sense == "<":
                    solver.add_constraint(coefficients, "<=", below)
                elif sense == ">":
                    solver.add_constraint(coefficients, ">=", above)
                elif sense == "<>":
                    side = f"__ne_{len(auxiliaries)}"
                    auxiliaries.append(side)
                    solver.add_variable(side, VarType.BINARY)
                    solver.add_indicator_constraint(side, 1, coefficients, "<=", below)
                    solver.add_indicator_constraint(side, 0, coefficients, ">=", above)
                else:
                    raise ValueError(f"unknown sense {sense!r}")
        expr = self._objective(objective, emit)
        solver.set_objective({label(c): v for c, v in expr.items() if c != CONSTANT}, objective.sense)
        result = solver.solve()
        assert result.status == SolverStatus.OPTIMAL, f"oracle model is not optimal: {result.status}"
        return result.objective_value + expr.get(CONSTANT, 0.0)

    def judge_scoped(self, answer: Sequence[Row], clauses: Sequence[Scoped], objective: ScopedObjective) -> float:
        """Check DeciDB's answer against the scoped clauses and return its objective.
        Every row of a decision's class must show one value, a whole-numbered decision
        must be whole, its bounds must hold, and every generated clause must hold."""
        assert len(answer) == len(self.rows), f"{len(answer)} result rows, expected {len(self.rows)}"
        values: dict = {}
        for name, decision in self.decisions.items():
            for row_index, row in enumerate(answer):
                if self.classes[name][row_index] is None:
                    assert row[name] is None, f"{name} = {row[name]} on row {row_index}, which has no decision"
                    continue
                value = float(row[name])
                column = (name, self.classes[name][row_index])
                seen = values.setdefault(column, value)
                assert (
                    abs(seen - value) <= _TOLERANCE
                ), f"{name} shows {seen} and {value} on rows of one class (row {row_index})"
                if decision.var_type != VarType.CONTINUOUS:
                    assert abs(value - round(value)) <= _TOLERANCE, f"{name} = {value} is not whole"
                assert value >= decision.lb - _TOLERANCE, f"{name} = {value} is below {decision.lb}"
                assert (
                    decision.ub is None or value <= decision.ub + _TOLERANCE
                ), f"{name} = {value} is above {decision.ub}"

        def evaluate(expr: Linear) -> float:
            return sum(
                coefficient * (1.0 if column == CONSTANT else values[column]) for column, coefficient in expr.items()
            )

        def emit(agg, members):
            pick = max if agg == "max" else min
            return {CONSTANT: pick(evaluate(member) for member in members)}

        for clause in clauses:
            for expr, sense in self._generate(clause, emit):
                lhs = evaluate(expr)
                # A whole-numbered side misses a strict bound by at least its distance to the
                # next whole number, so the strict senses are strict up to the tolerance.
                ok = {
                    "<=": lhs <= _TOLERANCE,
                    ">=": lhs >= -_TOLERANCE,
                    "=": abs(lhs) <= _TOLERANCE,
                    "<": lhs < -_TOLERANCE,
                    ">": lhs > _TOLERANCE,
                    "<>": abs(lhs) > _TOLERANCE,
                }[sense]
                assert ok, f"a generated clause is violated: {lhs} {sense} 0"
        return evaluate(self._objective(objective, emit))
