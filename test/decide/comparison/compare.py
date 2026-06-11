"""Comparison logic for DecidB DECIDE results vs oracle solver results.

Two levels of comparison:
  1. Objective value comparison (always performed).
  2. Decision-variable vector comparison — sorts both sides by all
     non-decision columns, then checks element-wise equality.

Status classification:
  - "identical": objective AND all variable assignments match.
  - "optimal":   objective matches but at least one assignment differs
                 (alternate optimal solution).
  - AssertionError raised if objectives differ beyond tolerance.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Callable

import pytest

from solver.types import SolverResult, SolverStatus


def _sortable(val):
    """Convert a value to a float if possible, for deterministic sorting."""
    try:
        return float(val)
    except (TypeError, ValueError):
        return val


def compute_decidb_objective(
    decidb_rows: list[tuple],
    decidb_cols: list[str],
    decide_var_names: list[str],
    coeff_fn: Callable[[tuple], dict[str, float]],
) -> float:
    """Compute the achieved objective from DecidB output rows.

    Args:
        decidb_rows: Rows returned by decidb_conn.execute(...).fetchall().
        decidb_cols: Column names from decidb_conn.description.
        decide_var_names: Names of the DECIDE variable columns (e.g. ["x"]).
        coeff_fn: Given a row tuple, returns {var_name: coefficient} for the
            objective.  For a simple ``MAXIMIZE SUM(x * col)``, this would
            return ``{"x": row[col_index]}``.

    Returns:
        The sum of (variable_value * coefficient) across all rows.
    """
    col_idx = {name: i for i, name in enumerate(decidb_cols)}
    total = 0.0
    for row in decidb_rows:
        coeffs = coeff_fn(row)
        for vname in decide_var_names:
            var_val = float(row[col_idx[vname]])
            total += var_val * coeffs.get(vname, 0.0)
    return total


# ---------------------------------------------------------------------------
# Full solution comparison
# ---------------------------------------------------------------------------

@dataclass
class ComparisonResult:
    """Result of comparing DecidB output against the oracle solution."""
    status: str                  # "identical" or "optimal"
    decidb_objective: float
    oracle_objective: float
    decidb_vector: list[float]
    oracle_vector: list[float]


def compare_solutions(
    decidb_rows: list[tuple],
    decidb_cols: list[str],
    oracle_result: SolverResult,
    oracle_data: list[tuple],
    decide_var_names: list[str],
    coeff_fn: Callable[[tuple], dict[str, float]] | None = None,
    tolerance: float = 1e-4,
    decidb_objective_fn: Callable[[list[tuple], list[str]], float] | None = None,
) -> ComparisonResult:
    """Compare DecidB output against the oracle solution.

    1. Asserts that objectives match (raises on mismatch).
    2. Sorts both sides by non-decision columns and compares variable
       assignments element-wise.

    Args:
        decidb_rows: Rows from DecidB query.
        decidb_cols: Column names from DecidB result description.
        oracle_result: Result from the oracle solver (must include
            variable_values for vector comparison).
        oracle_data: Raw data rows from duckdb_conn — same rows the oracle
            model was built from, in original query order.
        decide_var_names: DECIDE variable column names (e.g. ["x"]).
        coeff_fn: Maps a DecidB row to objective coefficients per variable.
        tolerance: Acceptable absolute difference.

    Returns:
        ComparisonResult with status "identical" or "optimal".

    Raises:
        AssertionError: If objectives differ beyond tolerance.
    """
    assert oracle_result.status == SolverStatus.OPTIMAL, (
        f"Oracle did not find optimal solution: {oracle_result.status}"
    )
    assert oracle_result.objective_value is not None

    if decidb_objective_fn is not None:
        decidb_obj = decidb_objective_fn(decidb_rows, decidb_cols)
    else:
        if coeff_fn is None:
            raise ValueError(
                "compare_solutions: pass either coeff_fn (linear) or decidb_objective_fn"
            )
        decidb_obj = compute_decidb_objective(
            decidb_rows, decidb_cols, decide_var_names, coeff_fn
        )
    oracle_obj = oracle_result.objective_value

    obj_diff = abs(decidb_obj - oracle_obj)
    assert obj_diff <= tolerance, (
        f"Objective mismatch: DecidB={decidb_obj:.6f}, "
        f"Oracle={oracle_obj:.6f}, diff={obj_diff:.6f} (tolerance={tolerance})"
    )

    # Sort key: all non-decision columns in DecidB
    var_set = set(decide_var_names)
    key_indices = [i for i, c in enumerate(decidb_cols) if c not in var_set]
    var_indices = [decidb_cols.index(v) for v in decide_var_names]

    decidb_sorted = sorted(
        enumerate(decidb_rows),
        key=lambda pair: tuple(_sortable(pair[1][i]) for i in key_indices),
    )

    oracle_sorted = sorted(
        enumerate(oracle_data),
        key=lambda pair: tuple(_sortable(v) for v in pair[1]),
    )

    decidb_vector = [
        float(row[vi])
        for _, row in decidb_sorted
        for vi in var_indices
    ]

    oracle_vector = [
        float(oracle_result.variable_values.get(f"{vname}_{orig_idx}", 0.0))
        for orig_idx, _ in oracle_sorted
        for vname in decide_var_names
    ]

    vectors_match = (
        len(decidb_vector) == len(oracle_vector)
        and all(
            abs(p - o) <= tolerance
            for p, o in zip(decidb_vector, oracle_vector)
        )
    )

    status = "identical" if vectors_match else "optimal"

    return ComparisonResult(
        status=status,
        decidb_objective=decidb_obj,
        oracle_objective=oracle_obj,
        decidb_vector=decidb_vector,
        oracle_vector=oracle_vector,
    )


# ---------------------------------------------------------------------------
# Backward-compatible wrapper (objective-only, no vector comparison)
# ---------------------------------------------------------------------------

def assert_optimal_match(
    decidb_rows: list[tuple],
    decidb_cols: list[str],
    oracle_result: SolverResult,
    decide_var_names: list[str],
    coeff_fn: Callable[[tuple], dict[str, float]],
    tolerance: float = 1e-4,
) -> None:
    """Assert that DecidB's objective matches the oracle's optimal value.

    Objective-only comparison — kept for backward compatibility.
    Prefer compare_solutions() for new tests.
    """
    assert oracle_result.status == SolverStatus.OPTIMAL, (
        f"Oracle did not find optimal solution: {oracle_result.status}"
    )
    assert oracle_result.objective_value is not None

    decidb_obj = compute_decidb_objective(
        decidb_rows, decidb_cols, decide_var_names, coeff_fn
    )
    oracle_obj = oracle_result.objective_value

    diff = abs(decidb_obj - oracle_obj)
    assert diff <= tolerance, (
        f"Objective mismatch: DecidB={decidb_obj:.6f}, "
        f"Oracle={oracle_obj:.6f}, diff={diff:.6f} (tolerance={tolerance})"
    )


def assert_infeasible(decidb_cli, sql: str) -> None:
    """Assert that executing *sql* on decidb raises an infeasibility error."""
    decidb_cli.assert_error(sql, match=r"(?i)infeasible")
