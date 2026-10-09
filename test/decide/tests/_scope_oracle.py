"""Reference evaluator for the DECIDE scoping language (`per`, `when`, `by`).

It reads no SQL. A test states the query's result rows, its decisions with their keys
and its clauses as plain Python, and the evaluator builds the model the language
defines -- one solver column per class of each decision's key -- through the
``OracleSolver`` interface. ``judge`` then checks DeciDB's own answer against the same
definition: every row of a class shows one value, every clause holds, and the objective
it reaches is returned for comparison with the oracle's optimum.

This first version covers keyed declarations (`decide per K: x(TYPE)`): a decision is
per row, per a key of row columns, or one for the whole query; a clause is linear and
is either generated once per row or summed over all rows.
"""

from __future__ import annotations

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
class Decision:
    name: str
    #: ``PER_ROW``, ``GLOBAL``, or the row columns the key names.
    key: tuple[str, ...] | None = PER_ROW
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


def class_ids(rows: Sequence[Row], key: tuple[str, ...] | None) -> list[int]:
    """The class of every row under ``key``, numbered in first-seen order. Rows with
    equal key values share a class, and NULL (``None``) is a value like any other."""
    if key is None:
        return list(range(len(rows)))
    ids: dict[tuple, int] = {}
    return [ids.setdefault(tuple(row[column] for column in key), len(ids)) for row in rows]


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
        return len(set(self.classes[name]))

    def column_count(self) -> int:
        return sum(self.class_count(name) for name in self.decisions)

    def _row_coefficients(self, terms: Terms, row_index: int, into: dict[tuple[str, int], float]) -> None:
        for name, coefficient in terms.items():
            column = (name, self.classes[name][row_index])
            into[column] = into.get(column, 0.0) + coefficient

    def _once_coefficients(self, terms: Terms, into: dict[tuple[str, int], float]) -> None:
        for name, coefficient in terms.items():
            if self.decisions[name].key != GLOBAL:
                raise ValueError(f"only a query-wide decision counts once; {name!r} is not one")
            into[(name, 0)] = into.get((name, 0), 0.0) + coefficient

    def solve(self, solver: OracleSolver, clauses: Sequence[RowClause | SumClause],
              objective: Objective) -> float:
        """The optimum of the model the language defines."""
        def label(column: tuple[str, int]) -> str:
            return f"{column[0]}#{column[1]}"

        solver.create_model("scope_oracle")
        for name, decision in self.decisions.items():
            for class_id in sorted(set(self.classes[name])):
                solver.add_variable(label((name, class_id)), decision.var_type, decision.lb, decision.ub)
        for clause in clauses:
            if isinstance(clause, RowClause):
                for row_index, row in enumerate(self.rows):
                    coefficients: dict[tuple[str, int], float] = {}
                    self._row_coefficients(clause.terms(row), row_index, coefficients)
                    solver.add_constraint({label(c): v for c, v in coefficients.items()}, clause.sense,
                                          float(clause.rhs(row)))
            else:
                coefficients = {}
                for row_index, row in enumerate(self.rows):
                    self._row_coefficients(clause.terms(row), row_index, coefficients)
                solver.add_constraint({label(c): v for c, v in coefficients.items()}, clause.sense,
                                      float(clause.rhs))
        coefficients = {}
        for row_index, row in enumerate(self.rows):
            self._row_coefficients(objective.rows(row), row_index, coefficients)
        self._once_coefficients(objective.once, coefficients)
        solver.set_objective({label(c): v for c, v in coefficients.items()}, objective.sense)
        result = solver.solve()
        assert result.status == SolverStatus.OPTIMAL, f"oracle model is not optimal: {result.status}"
        return result.objective_value

    def judge(self, answer: Sequence[Row], clauses: Sequence[RowClause | SumClause],
              objective: Objective) -> float:
        """Check DeciDB's answer -- its result rows, aligned with ``self.rows`` -- and
        return the objective it reaches. Every row of a class must show one value, a
        whole-numbered decision must be whole, and every clause must hold."""
        assert len(answer) == len(self.rows), f"{len(answer)} result rows, expected {len(self.rows)}"
        values: dict[tuple[str, int], float] = {}
        for name, decision in self.decisions.items():
            for row_index, row in enumerate(answer):
                value = float(row[name])
                column = (name, self.classes[name][row_index])
                seen = values.setdefault(column, value)
                assert abs(seen - value) <= _TOLERANCE, (
                    f"{name} shows {seen} and {value} on rows of one class "
                    f"(key {decision.key}, row {row_index})")
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
                    self._row_coefficients(clause.terms(row), row_index, coefficients)
                    lhs = evaluate(coefficients)
                    assert _holds(lhs, clause.sense, float(clause.rhs(row))), (
                        f"row {row_index} violates a clause: {lhs} {clause.sense} {clause.rhs(row)}")
            else:
                coefficients = {}
                for row_index, row in enumerate(self.rows):
                    self._row_coefficients(clause.terms(row), row_index, coefficients)
                lhs = evaluate(coefficients)
                assert _holds(lhs, clause.sense, float(clause.rhs)), (
                    f"a sum clause is violated: {lhs} {clause.sense} {clause.rhs}")
        coefficients = {}
        for row_index, row in enumerate(self.rows):
            self._row_coefficients(objective.rows(row), row_index, coefficients)
        self._once_coefficients(objective.once, coefficients)
        return evaluate(coefficients)
