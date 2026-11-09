"""Shared types for the solver abstraction layer."""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum, auto


class SolverStatus(Enum):
    """Status returned by a solver after optimization."""

    OPTIMAL = auto()
    INFEASIBLE = auto()
    UNBOUNDED = auto()
    INF_OR_UNBD = auto()
    TIME_LIMIT = auto()
    ERROR = auto()


class VarType(Enum):
    """Decision variable type."""

    BINARY = auto()
    INTEGER = auto()
    CONTINUOUS = auto()


class ObjSense(Enum):
    """Objective sense."""

    MAXIMIZE = auto()
    MINIMIZE = auto()


@dataclass
class SolverResult:
    """Result from an oracle solver."""

    status: SolverStatus
    objective_value: float | None = None
    variable_values: dict[str, float] = field(default_factory=dict)
    solve_time_seconds: float = 0.0
