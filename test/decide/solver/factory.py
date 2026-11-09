"""Instantiate the Gurobi oracle solver."""

from __future__ import annotations

from .base import OracleSolver


def get_solver() -> OracleSolver:
    """Return a Gurobi-backed oracle solver.

    Raises ImportError if gurobipy is not installed or no valid license is
    available — every DECIDE oracle test depends on Gurobi.
    """
    try:
        import gurobipy  # noqa: F401
    except ImportError as exc:
        raise ImportError(
            "gurobipy is required for the DECIDE oracle. "
            "Install it with: pip install 'gurobipy>=12.0' "
            "(requires a valid Gurobi license)."
        ) from exc

    from .gurobi_backend import GurobiSolver

    return GurobiSolver()
