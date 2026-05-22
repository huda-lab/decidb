"""Shared utilities for oracle-verified DECIDE tests.

Keep helpers here that are needed by more than one test file. Per-test
logic (building coefficients, shaping constraints) stays in the test.

Conventions:
  - Row-scoped DECIDE variable ``v`` for row ``i`` is named ``"v_{i}"``.
  - Entity-scoped ``t.v`` uses ``f"{var}_{entity_key}"``.
  - Group indices refer to positions in the original data list, not
    DecidB's output (which may be reordered).
"""

from __future__ import annotations

from collections import defaultdict
from typing import Callable, Hashable

from solver.types import VarType


def group_indices(
    data: list[tuple],
    key_fn: Callable[[tuple], Hashable | None],
) -> dict[Hashable, list[int]]:
    """Return {group_value: [row_index, ...]} in order of first appearance.

    Rows where ``key_fn`` returns ``None`` are dropped — matching DECIDE's
    PER semantics, which excludes NULL-keyed rows from every group.
    """
    groups: dict = defaultdict(list)
    for i, row in enumerate(data):
        k = key_fn(row)
        if k is None:
            continue
        groups[k].append(i)
    return groups


def add_ne_indicator(
    oracle,
    coeffs: dict[str, float],
    rhs: float,
    name: str,
) -> None:
    """Encode ``sum(coeffs) != rhs`` independently via Gurobi indicator constraints.

    Introduces one binary branch variable ``z`` and uses native indicator
    constraints (no hand-picked Big-M):

      z = 1  ⇒  S <= rhs - 1        (lower branch)
      z = 0  ⇒  S >= rhs + 1        (upper branch)

    Applicable when the LHS is integer-valued. For continuous sums a strict
    disequality is not ILP-expressible; callers handle that explicitly.
    """
    z_name = f"{name}__z"
    oracle.add_variable(z_name, VarType.BINARY)
    oracle.add_indicator_constraint(
        z_name, 1, coeffs, "<=", rhs - 1.0, name=f"{name}_lo",
    )
    oracle.add_indicator_constraint(
        z_name, 0, coeffs, ">=", rhs + 1.0, name=f"{name}_hi",
    )


# Backward-compatible alias kept briefly while call sites migrate to the
# indicator form; prefer ``add_ne_indicator`` in new code.
def add_ne_bigm(oracle, coeffs, rhs, row_count, name):  # noqa: ARG001
    add_ne_indicator(oracle, coeffs, rhs, name)


def add_in_domain(
    oracle,
    var_name: str,
    domain: list[float],
    name_prefix: str | None = None,
) -> list[str]:
    """Restrict ``var_name`` to a discrete set of values using binary indicators.

    For each value ``v_k`` in ``domain`` introduces a binary ``z_k`` with
      SUM(z_k) = 1             (exactly one selected)
      var = SUM(v_k * z_k)    (linking)

    Mirrors DecidB's bind-time rewrite for ``x IN (v1, ..., vK)``. Returns
    the list of indicator variable names.
    """
    prefix = name_prefix or f"{var_name}_in"
    zs: list[str] = []
    for k, _ in enumerate(domain):
        z = f"{prefix}_{k}"
        oracle.add_variable(z, VarType.BINARY)
        zs.append(z)
    oracle.add_constraint({z: 1.0 for z in zs}, "=", 1.0, name=f"{prefix}_card")
    link = {z: -float(v) for z, v in zip(zs, domain)}
    link[var_name] = 1.0
    oracle.add_constraint(link, "=", 0.0, name=f"{prefix}_link")
    return zs


def emit_inner_max(
    oracle,
    name_prefix: str,
    row_coeffs: list[dict[str, float]],
    row_ub: float,
) -> str:
    """Introduce an auxiliary ``z = MAX(<per-row linear expr>)`` variable.

    Adds ``z`` and per-row constraints ``z >= expr_i``. ``z``'s upper bound is
    set to ``row_ub`` (a loose upper bound on any row's expression value) so
    the MIP stays well-posed. Returns ``z``'s variable name.

    Use for the *easy* inner-MAX case inside ``MINIMIZE SUM(MAX(expr)) PER col``
    — minimizing over SUM(z) naturally drives each z down to the tight bound.
    """
    z_name = f"{name_prefix}_zmax"
    oracle.add_variable(z_name, VarType.CONTINUOUS, lb=0.0, ub=row_ub)
    for i, coeffs in enumerate(row_coeffs):
        expr = dict(coeffs)
        expr[z_name] = -1.0
        oracle.add_constraint(expr, "<=", 0.0, name=f"{name_prefix}_ge_row_{i}")
    return z_name


def emit_inner_min(
    oracle,
    name_prefix: str,
    row_coeffs: list[dict[str, float]],
    row_ub: float,
) -> str:
    """Introduce an auxiliary ``z = MIN(<per-row linear expr>)`` variable.

    Adds ``z`` with per-row constraints ``z <= expr_i``. Use for the *easy*
    inner-MIN case inside ``MAXIMIZE SUM(MIN(expr)) PER col``.
    """
    z_name = f"{name_prefix}_zmin"
    oracle.add_variable(z_name, VarType.CONTINUOUS, lb=0.0, ub=row_ub)
    for i, coeffs in enumerate(row_coeffs):
        expr = dict(coeffs)
        expr[z_name] = -1.0
        oracle.add_constraint(expr, ">=", 0.0, name=f"{name_prefix}_le_row_{i}")
    return z_name


def emit_hard_inner_max(
    oracle,
    name_prefix: str,
    row_coeffs: list[dict[str, float]],
    row_ub: float,
) -> str:
    """Inner MAX auxiliary for the *hard* case (when MAX is being pushed *up*).

    Uses Gurobi indicator constraints: for each row add a binary y_i with
      y_i = 1  ⇒  z <= expr_i
    plus ``SUM(y_i) >= 1``. Combined with the upper bounds from easy-form
    ``z >= expr_i`` per row, this pins z to exactly one row's value — the
    row that "is" the max. Returns z's name.
    """
    z_name = f"{name_prefix}_zmax_hard"
    oracle.add_variable(z_name, VarType.CONTINUOUS, lb=0.0, ub=row_ub)
    y_names = []
    for i, coeffs in enumerate(row_coeffs):
        # easy-form upper bound: z >= expr_i
        expr = dict(coeffs)
        expr[z_name] = -1.0
        oracle.add_constraint(expr, "<=", 0.0, name=f"{name_prefix}_lb_{i}")
        # hard-form tight lock: y_i=1 ⇒ z <= expr_i
        y = f"{name_prefix}_y_{i}"
        oracle.add_variable(y, VarType.BINARY)
        up = dict(coeffs)
        up[z_name] = -1.0
        oracle.add_indicator_constraint(
            y, 1, up, ">=", 0.0, name=f"{name_prefix}_tight_{i}",
        )
        y_names.append(y)
    oracle.add_constraint(
        {y: 1.0 for y in y_names}, ">=", 1.0, name=f"{name_prefix}_sel",
    )
    return z_name


def emit_hard_inner_min(
    oracle,
    name_prefix: str,
    row_coeffs: list[dict[str, float]],
    row_ub: float,
) -> str:
    """Inner MIN auxiliary for the *hard* case (when MIN is being pushed *down*)."""
    z_name = f"{name_prefix}_zmin_hard"
    oracle.add_variable(z_name, VarType.CONTINUOUS, lb=0.0, ub=row_ub)
    y_names = []
    for i, coeffs in enumerate(row_coeffs):
        # easy-form lower bound: z <= expr_i
        expr = dict(coeffs)
        expr[z_name] = -1.0
        oracle.add_constraint(expr, ">=", 0.0, name=f"{name_prefix}_ub_{i}")
        # hard-form tight lock: y_i=1 ⇒ z >= expr_i
        y = f"{name_prefix}_y_{i}"
        oracle.add_variable(y, VarType.BINARY)
        up = dict(coeffs)
        up[z_name] = -1.0
        oracle.add_indicator_constraint(
            y, 1, up, "<=", 0.0, name=f"{name_prefix}_tight_{i}",
        )
        y_names.append(y)
    oracle.add_constraint(
        {y: 1.0 for y in y_names}, ">=", 1.0, name=f"{name_prefix}_sel",
    )
    return z_name


def emit_hard_inner_max_quadratic(
    oracle,
    name_prefix: str,
    row_coeffs: list[tuple[dict[str, float], dict[tuple[str, str], float], float]],
    q_ub: float,
) -> str:
    """Quadratic variant of ``emit_hard_inner_max``.

    Each row contributes a quadratic expression ``expr_i = L_i(x) + Q_i(x) + c_i``
    where ``row_coeffs[i] == (L_i, Q_i, c_i)``. The helper emits a per-row
    auxiliary ``q_i = expr_i`` via a single quadratic equality constraint, then
    applies the standard easy+indicator pattern linearly on ``q_i``. This
    detour is required because Gurobi's ``addGenConstrIndicator`` accepts only
    linear bodies — quadratic expressions must be materialised into a linear
    proxy first.
    """
    z_name = f"{name_prefix}_zmax_hardq"
    oracle.add_variable(z_name, VarType.CONTINUOUS, lb=0.0, ub=q_ub)
    y_names = []
    for i, (lin_i, quad_i, const_i) in enumerate(row_coeffs):
        q = f"{name_prefix}_q_{i}"
        oracle.add_variable(q, VarType.CONTINUOUS, lb=0.0, ub=q_ub)
        eq_lin = dict(lin_i)
        eq_lin[q] = eq_lin.get(q, 0.0) - 1.0
        oracle.add_quadratic_constraint(
            eq_lin, quad_i, "=", -const_i, name=f"{name_prefix}_qeq_{i}",
        )
        oracle.add_constraint(
            {z_name: 1.0, q: -1.0}, ">=", 0.0, name=f"{name_prefix}_lb_{i}",
        )
        y = f"{name_prefix}_y_{i}"
        oracle.add_variable(y, VarType.BINARY)
        oracle.add_indicator_constraint(
            y, 1, {z_name: 1.0, q: -1.0}, "<=", 0.0,
            name=f"{name_prefix}_tight_{i}",
        )
        y_names.append(y)
    oracle.add_constraint(
        {y: 1.0 for y in y_names}, ">=", 1.0, name=f"{name_prefix}_sel",
    )
    return z_name


def emit_hard_inner_min_quadratic(
    oracle,
    name_prefix: str,
    row_coeffs: list[tuple[dict[str, float], dict[tuple[str, str], float], float]],
    q_ub: float,
) -> str:
    """Quadratic variant of ``emit_hard_inner_min`` (mirror of the max variant)."""
    z_name = f"{name_prefix}_zmin_hardq"
    oracle.add_variable(z_name, VarType.CONTINUOUS, lb=0.0, ub=q_ub)
    y_names = []
    for i, (lin_i, quad_i, const_i) in enumerate(row_coeffs):
        q = f"{name_prefix}_q_{i}"
        oracle.add_variable(q, VarType.CONTINUOUS, lb=0.0, ub=q_ub)
        eq_lin = dict(lin_i)
        eq_lin[q] = eq_lin.get(q, 0.0) - 1.0
        oracle.add_quadratic_constraint(
            eq_lin, quad_i, "=", -const_i, name=f"{name_prefix}_qeq_{i}",
        )
        oracle.add_constraint(
            {z_name: 1.0, q: -1.0}, "<=", 0.0, name=f"{name_prefix}_ub_{i}",
        )
        y = f"{name_prefix}_y_{i}"
        oracle.add_variable(y, VarType.BINARY)
        oracle.add_indicator_constraint(
            y, 1, {z_name: 1.0, q: -1.0}, ">=", 0.0,
            name=f"{name_prefix}_tight_{i}",
        )
        y_names.append(y)
    oracle.add_constraint(
        {y: 1.0 for y in y_names}, ">=", 1.0, name=f"{name_prefix}_sel",
    )
    return z_name


def add_bool_and(
    oracle,
    x_name: str,
    y_name: str,
    z_name: str,
) -> None:
    """Link a binary ``z`` to the AND of two binaries: ``z = x ∧ y``.

    Pure linear encoding (no Big-M):
      z <= x
      z <= y
      z >= x + y - 1
    Used to linearize Bool × Bool products before summation.
    """
    oracle.add_variable(z_name, VarType.BINARY)
    oracle.add_constraint({z_name: 1.0, x_name: -1.0}, "<=", 0.0, name=f"{z_name}_le_x")
    oracle.add_constraint({z_name: 1.0, y_name: -1.0}, "<=", 0.0, name=f"{z_name}_le_y")
    oracle.add_constraint(
        {z_name: 1.0, x_name: -1.0, y_name: -1.0}, ">=", -1.0, name=f"{z_name}_ge_xy",
    )


