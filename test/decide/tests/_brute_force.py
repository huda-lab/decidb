"""Exhaustive reference for tiny DECIDE models the scope oracle cannot state: products of
decisions, ``POWER``, an L0 count, ``<>`` over an ``AVG``.

It reads no SQL. A test names each solver column with a small whole-number domain, states
the constraints and the objective as Python over one assignment, and ``optimum``
enumerates every assignment. ``judge`` checks DeciDB's answer against the same statement
and returns its objective, for comparison with the optimum.
"""

from __future__ import annotations

from itertools import product
from typing import Callable, Mapping, Sequence

Assignment = Mapping[str, int]


def optimum(
    domains: Mapping[str, range],
    feasible: Callable[[Assignment], bool],
    objective: Callable[[Assignment], float],
    maximize: bool = True,
) -> float:
    best = None
    names = list(domains)
    for values in product(*(domains[name] for name in names)):
        assignment = dict(zip(names, values))
        if feasible(assignment):
            value = objective(assignment)
            if best is None or (value > best if maximize else value < best):
                best = value
    assert best is not None, "no assignment is feasible"
    return best


def judge(
    assignment: Assignment, feasible: Callable[[Assignment], bool], objective: Callable[[Assignment], float]
) -> float:
    assert all(float(v).is_integer() for v in assignment.values()), f"not whole: {assignment}"
    whole = {name: int(value) for name, value in assignment.items()}
    assert feasible(whole), f"DeciDB's answer violates a constraint: {whole}"
    return objective(whole)


def judge_rows(
    rows: Sequence[Mapping[str, object]],
    domains: Mapping[str, range],
    feasible: Callable[[Assignment], bool],
    objective: Callable[[Assignment], float],
    maximize: bool = True,
) -> None:
    """Judge DeciDB's answer, one dict per result row, against every assignment of
    ``domains``: a per-row decision ``x`` fills the solver columns ``x0``, ``x1``, ...; a
    query-wide one, named bare in ``domains``, must show one value on every row."""
    assignment = {}
    for name in {n.rstrip("0123456789") for n in domains}:
        if name in domains:
            assert len({row[name] for row in rows}) == 1, f"query-wide {name} differs across rows: {rows}"
            assignment[name] = rows[0][name]
        else:
            assignment.update({f"{name}{i}": row[name] for i, row in enumerate(rows)})
    assert judge(assignment, feasible, objective) == optimum(domains, feasible, objective, maximize)
