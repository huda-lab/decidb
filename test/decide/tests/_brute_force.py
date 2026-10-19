"""Exhaustive reference for tiny DECIDE models the scope oracle cannot state: products of
decisions, ``POWER``, an L0 count, ``<>`` over an ``AVG``.

It reads no SQL. A test names each solver column with a small whole-number domain, states
the constraints and the objective as Python over one assignment, and ``optimum``
enumerates every assignment. ``judge`` checks DeciDB's answer against the same statement
and returns its objective, for comparison with the optimum.
"""

from __future__ import annotations

from itertools import product
from typing import Callable, Mapping

Assignment = Mapping[str, int]


def optimum(domains: Mapping[str, range], feasible: Callable[[Assignment], bool],
            objective: Callable[[Assignment], float], maximize: bool = True) -> float:
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


def judge(assignment: Assignment, feasible: Callable[[Assignment], bool],
          objective: Callable[[Assignment], float]) -> float:
    assert all(float(v).is_integer() for v in assignment.values()), f"not whole: {assignment}"
    whole = {name: int(value) for name, value in assignment.items()}
    assert feasible(whole), f"DeciDB's answer violates a constraint: {whole}"
    return objective(whole)
