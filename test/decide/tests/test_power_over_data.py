"""``POWER`` over data columns only: ``SUM(x * POWER(qty, 2))``.

The base references no DECIDE variable, so the whole call is a per-row constant —
the same category as ``mod(id, 97)`` or ``floor(price)``, which the binder has always
accepted as a coefficient. The quadratic machinery has nothing to do here: the
constraint is linear in ``x``.

The binder used to reject it anyway. ``ValidateQuadraticPower`` ran on every ``POWER``
call and required the base to reference a DECIDE variable, so
``SUM(x * POWER(qty, 2))`` failed with *"POWER(..., 2) in DECIDE expression must
reference at least one DECIDE variable"*. That was invisible because the parsed-level
constraint simplifier ran first and lowered ``POWER(qty, 2)`` into ``qty * qty``
through SymbolicC++, so no ``POWER`` node ever reached the binder. Deleting that layer
(the canonicalization refactor) exposed the gate, and the fix is to skip it when the call
contains no DECIDE variable — the rule the named-function arm beside it already used.

Two things changed that these tests pin:

  - The gate itself, for exponent 2.
  - The exponent restriction, for everything else. ``POWER(qty, 3)`` was legal only
    because SymbolicC++ expanded it to ``qty*qty*qty`` before the "only exponent 2 is
    supported" check could see it. It is now legal directly, and the exponent is
    unrestricted for a data-only base.

Oracle-verified rather than asserted against a hand-computed optimum, and the
coefficients are chosen so a mis-evaluated ``POWER`` is visible in the answer: reading
``POWER(qty, 2)`` as ``qty`` admits every row and returns 15 where the correct
optimum is 6.
"""

import time

import pytest

from solver.types import VarType, ObjSense, SolverStatus


# qty^2 = 1, 4, 9, 16 -- with a budget of 10 the best subset is {1, 9}, worth 1+5 = 6.
# qty   = 1, 2, 3, 4  -- if POWER were dropped, all four rows fit and the answer is 15.
_ROWS = [(1, 1.0, 1.0), (2, 2.0, 3.0), (3, 3.0, 5.0), (4, 4.0, 6.0)]
_TABLE = "(VALUES (1, 1.0, 1.0), (2, 2.0, 3.0), (3, 3.0, 5.0), (4, 4.0, 6.0)) t(id, qty, w)"
_CAP = 10.0


def _solve_oracle(oracle_solver, name, coeffs, cap):
    oracle_solver.create_model(name)
    names = [f"x_{i}" for i in range(len(_ROWS))]
    for n in names:
        oracle_solver.add_variable(n, VarType.BINARY, lb=0.0, ub=1.0)
    oracle_solver.add_constraint(
        {names[i]: c for i, c in enumerate(coeffs)}, "<=", cap, name="budget"
    )
    oracle_solver.set_objective(
        {names[i]: w for i, (_, _, w) in enumerate(_ROWS)}, ObjSense.MAXIMIZE
    )
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL
    return result


@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_power_over_data_is_a_coefficient(decidb_cli, oracle_solver, perf_tracker):
    """``SUM(x * POWER(qty, 2)) <= 10`` — linear in x, coefficients are qty squared."""
    t0 = time.perf_counter()
    rows, cols = decidb_cli.execute(f"""
        SELECT id, w, x FROM {_TABLE}
        DECIDE x(BOOL)
        SUCH THAT SUM(x * POWER(qty, 2)) <= {_CAP}
        MAXIMIZE SUM(x * w)
    """)
    decidb_time = time.perf_counter() - t0

    t_build = time.perf_counter()
    result = _solve_oracle(
        oracle_solver, "power_over_data",
        [qty ** 2 for _, qty, _ in _ROWS], _CAP,
    )
    build_time = time.perf_counter() - t_build

    ci = {c: i for i, c in enumerate(cols)}
    decidb_obj = sum(int(r[ci["x"]]) * float(r[ci["w"]]) for r in rows)
    assert abs(decidb_obj - result.objective_value) <= 1e-6, (
        f"Objective mismatch: DecidB={decidb_obj}, Oracle={result.objective_value}"
    )
    # The constraint as written actually holds, with POWER applied.
    lhs = sum(
        int(r[ci["x"]]) * (qty ** 2)
        for r, (_, qty, _) in zip(rows, _ROWS)
    )
    assert lhs <= _CAP + 1e-6, f"user-written constraint violated: {lhs} > {_CAP}"

    perf_tracker.record(
        "power_over_data", decidb_time, build_time, result.solve_time_seconds,
        len(_ROWS), len(_ROWS), 1, result.objective_value,
        oracle_solver.solver_name(), comparison_status="optimal",
    )


@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_power_over_data_matches_explicit_product(decidb_cli):
    """``POWER(qty, 2)`` and ``qty * qty`` are the same coefficient.

    Needs no solver: the two spellings are algebraically identical, so a disagreement
    is a bug whichever one is right. This is what the deleted simplifier used to
    guarantee by rewriting the first spelling into the second.
    """
    def solve(coeff):
        rows, cols = decidb_cli.execute(f"""
            SELECT id, x FROM {_TABLE}
            DECIDE x(BOOL)
            SUCH THAT SUM(x * {coeff}) <= {_CAP}
            MAXIMIZE SUM(x * w)
        """)
        ci = {c: i for i, c in enumerate(cols)}
        return sorted(int(r[ci["id"]]) for r in rows if int(r[ci["x"]]) == 1)

    assert solve("POWER(qty, 2)") == solve("qty * qty")


@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_power_over_data_exponent_three(decidb_cli, oracle_solver):
    """``POWER(qty, 3)`` over data — the exponent gate does not apply to a data base.

    qty^3 = 1, 8, 27, 64. With a budget of 30 the best subset is {1, 8, 27}? No —
    1+8+27 = 36 exceeds it; the oracle decides. The point is only that DecidB and the
    oracle agree, using coefficients that differ sharply from both qty and qty^2.
    """
    cap = 30.0
    rows, cols = decidb_cli.execute(f"""
        SELECT id, w, x FROM {_TABLE}
        DECIDE x(BOOL)
        SUCH THAT SUM(x * POWER(qty, 3)) <= {cap}
        MAXIMIZE SUM(x * w)
    """)
    result = _solve_oracle(
        oracle_solver, "power_over_data_cubed",
        [qty ** 3 for _, qty, _ in _ROWS], cap,
    )
    ci = {c: i for i, c in enumerate(cols)}
    decidb_obj = sum(int(r[ci["x"]]) * float(r[ci["w"]]) for r in rows)
    assert abs(decidb_obj - result.objective_value) <= 1e-6, (
        f"Objective mismatch: DecidB={decidb_obj}, Oracle={result.objective_value}"
    )


@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_power_over_decide_variable_still_gated_in_constraint(decidb_cli):
    """The loosening is scoped to a data-only base — ``POWER(x, 2)`` is unaffected.

    A quadratic constraint term still has to go through the quadratic path, so the
    early-out must not swallow it. Without the ``ExpressionContainsDecideVariable``
    condition this query would take the data route and build a wrong model.
    """
    rows, cols = decidb_cli.execute(f"""
        SELECT id, x FROM {_TABLE}
        DECIDE x(REAL)
        SUCH THAT SUM(POWER(x - 2, 2)) <= 4 AND x <= 9
        MAXIMIZE SUM(x)
    """)
    ci = {c: i for i, c in enumerate(cols)}
    # Four rows, each x within 2 +/- of 2 with the squared deviations summing to <= 4:
    # the symmetric optimum is x_i = 3 for every row.
    for r in rows:
        assert float(r[ci["x"]]) <= 3.0 + 1e-4, (
            f"quadratic constraint not enforced: x={r[ci['x']]}"
        )
