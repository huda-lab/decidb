"""A product with an additive factor inside a reducer body: ``SUM(q * (price + x))``.

The physical layer cannot hand such a body to the coefficient classifier as written —
``(price + x)`` is neither a data factor nor a bare decision variable — so it
distributes first, into ``SUM(q*price) + SUM(q*x)``. Distribution rebuilds the ``*``
node from a flattened factor list, and the rebuild is where this used to go wrong.

The old rebuild reused the *original* multiply's bound function, return type and bind
data over the new children. Those no longer agree: the original ``*`` was resolved for
``q * (price + x)``, and the addition widens its operand, so the function expects a
wider DECIMAL than the plain ``price`` it is now handed. DuckDB does not check this. It
reinterprets the children's physical representation, which yields a garbage coefficient
and reads past the end of a narrower vector — the bound came out around ``-6.4e+10``
instead of ``4``, and a feasible query was reported infeasible.

Two properties make this worth pinning rather than trusting:

  - It is **silent**. Nothing throws; a plausible-looking number reaches the solver.
  - It is **DECIMAL-only and width-sensitive**. The same query over DOUBLE columns, or
    over DECIMAL(18,2) columns (already the same physical width on both sides), was
    always correct. So a test written over DOUBLE or over wide decimals passes against
    the bug.

The fix binds each rebuilt product through ``FunctionBinder``, which resolves the
implementation, return type and casts for the operands actually present. Same reasoning
applies to ``ExtractCoefficientWithoutVariable``, which rebuilds a product after
*dropping* a factor and so loses the alignment between children and signature.

Every test here is oracle-verified. The failure returns a number rather than an error,
so "it solved" does not distinguish a correct bound from a garbage one.

The shapes below were originally chosen because each reached the rebuild through a
different decline of the parsed-level constraint simplifier, which distributed the body
first in the common case. That layer was deleted at canonicalize.md C.4, so all of them
now reach the rebuild directly.

Covers:
  - test_nested_product_matches_expanded_spelling: the two spellings agree
  - test_nested_product_oracle: SUM(q*(price+x)) <= K, oracle-verified
  - test_nested_product_subtraction: SUM(q*(price-x)), the `-` distribution arm
  - test_nested_product_scaled_reducer: 2 * SUM(q*(price+x)), a second decline path
  - test_nested_product_narrow_decimal_widths: the widths that actually differ
"""

import time

import pytest

from solver.types import VarType, ObjSense, SolverStatus


# The columns are deliberately DECIMAL(2,1): `price + x` widens to DECIMAL(3,1),
# so the rebuilt `q * price` is handed operands narrower than the original
# multiply's signature. Wider decimals do not reproduce the bug.
_ROWS = [(1, 1.0, 5.0), (2, 2.0, 3.0), (3, 3.0, 1.0)]
_TABLE = "(VALUES (1, 1.0, 5.0), (2, 2.0, 3.0), (3, 3.0, 1.0)) t(id, q, price)"

# Sum of q*price over every row = 1*5 + 2*3 + 3*1 = 14.
_DATA_OFFSET = 14.0
_CAP = 18.0


@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_nested_product_matches_expanded_spelling(decidb_cli):
    """``SUM(q*(price+x))`` and ``SUM(q*price + q*x)`` are the same constraint.

    This is the sharpest form of the check and needs no solver: the two spellings are
    algebraically identical, so any disagreement is a bug regardless of which one is
    right. Before the fix the nested spelling reported the query infeasible while the
    expanded one returned {1, 3}.
    """
    def solve(body):
        rows, cols = decidb_cli.execute(f"""
            SELECT id, x FROM {_TABLE}
            DECIDE x(BOOL)
            SUCH THAT SUM({body}) <= {_CAP}
            MAXIMIZE SUM(x * q)
        """)
        ci = {c: i for i, c in enumerate(cols)}
        return sorted(int(r[ci["id"]]) for r in rows if int(r[ci["x"]]) == 1)

    nested = solve("q * (price + x)")
    expanded = solve("q * price + q * x")
    assert nested == expanded, (
        f"same constraint, different answers: nested={nested}, expanded={expanded}"
    )


@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_nested_product_oracle(decidb_cli, oracle_solver, perf_tracker):
    """``SUM(q*(price+x)) <= 18`` maximizing ``SUM(x*q)``.

    Distributing gives ``SUM(q*price) + SUM(q*x) <= 18``. The data half is a constant
    14, so the model is ``SUM(q*x) <= 4`` with coefficients equal to the q values.
    Oracle-verified because the wrong bound is still a number the solver accepts.
    """
    t0 = time.perf_counter()
    rows, cols = decidb_cli.execute(f"""
        SELECT id, q, x FROM {_TABLE}
        DECIDE x(BOOL)
        SUCH THAT SUM(q * (price + x)) <= {_CAP}
        MAXIMIZE SUM(x * q)
    """)
    decidb_time = time.perf_counter() - t0

    t_build = time.perf_counter()
    oracle_solver.create_model("nested_product_in_reducer")
    names = [f"x_{i}" for i in range(len(_ROWS))]
    for n in names:
        oracle_solver.add_variable(n, VarType.BINARY, lb=0.0, ub=1.0)
    # SUM(q*price) is data: it moves to the bound.
    oracle_solver.add_constraint(
        {names[i]: q for i, (_, q, _) in enumerate(_ROWS)},
        "<=", _CAP - _DATA_OFFSET, name="budget",
    )
    oracle_solver.set_objective(
        {names[i]: q for i, (_, q, _) in enumerate(_ROWS)}, ObjSense.MAXIMIZE
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL

    ci = {c: i for i, c in enumerate(cols)}
    decidb_obj = sum(int(r[ci["x"]]) * float(r[ci["q"]]) for r in rows)
    assert abs(decidb_obj - result.objective_value) <= 1e-6, (
        f"Objective mismatch: DecidB={decidb_obj}, Oracle={result.objective_value}"
    )
    # And the constraint as the user wrote it actually holds.
    lhs = sum(
        float(r[ci["q"]]) * (price + int(r[ci["x"]]))
        for r, (_, _, price) in zip(rows, _ROWS)
    )
    assert lhs <= _CAP + 1e-6, f"user-written constraint violated: {lhs} > {_CAP}"

    perf_tracker.record(
        "nested_product_in_reducer", decidb_time, build_time, result.solve_time_seconds,
        len(_ROWS), len(names), 1, result.objective_value,
        oracle_solver.solver_name(), comparison_status="optimal",
    )


@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_nested_product_subtraction(decidb_cli, oracle_solver, perf_tracker):
    """``SUM(q*(price-x)) >= 11`` — the ``-`` arm of the same distribution.

    Distributing gives ``SUM(q*price) - SUM(q*x) >= 11``, i.e. ``SUM(q*x) <= 3`` after
    the constant 14 moves right. Worth covering separately: the subtraction arm builds
    its addends through the same rebuild but flips a sign, so a rebuild that silently
    returns garbage would be masked by the sign check passing.
    """
    bound = 11.0
    t0 = time.perf_counter()
    rows, cols = decidb_cli.execute(f"""
        SELECT id, q, x FROM {_TABLE}
        DECIDE x(BOOL)
        SUCH THAT SUM(q * (price - x)) >= {bound}
        MAXIMIZE SUM(x * q)
    """)
    decidb_time = time.perf_counter() - t0

    t_build = time.perf_counter()
    oracle_solver.create_model("nested_product_subtraction")
    names = [f"x_{i}" for i in range(len(_ROWS))]
    for n in names:
        oracle_solver.add_variable(n, VarType.BINARY, lb=0.0, ub=1.0)
    # 14 - SUM(q*x) >= 11  <=>  SUM(q*x) <= 3
    oracle_solver.add_constraint(
        {names[i]: q for i, (_, q, _) in enumerate(_ROWS)},
        "<=", _DATA_OFFSET - bound, name="budget",
    )
    oracle_solver.set_objective(
        {names[i]: q for i, (_, q, _) in enumerate(_ROWS)}, ObjSense.MAXIMIZE
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL

    ci = {c: i for i, c in enumerate(cols)}
    decidb_obj = sum(int(r[ci["x"]]) * float(r[ci["q"]]) for r in rows)
    assert abs(decidb_obj - result.objective_value) <= 1e-6, (
        f"Objective mismatch: DecidB={decidb_obj}, Oracle={result.objective_value}"
    )
    lhs = sum(
        float(r[ci["q"]]) * (price - int(r[ci["x"]]))
        for r, (_, _, price) in zip(rows, _ROWS)
    )
    assert lhs >= bound - 1e-6, f"user-written constraint violated: {lhs} < {bound}"

    perf_tracker.record(
        "nested_product_subtraction", decidb_time, build_time, result.solve_time_seconds,
        len(_ROWS), len(names), 1, result.objective_value,
        oracle_solver.solver_name(), comparison_status="optimal",
    )


@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_nested_product_scaled_reducer(decidb_cli):
    """``2 * SUM(q*(price+x)) <= 36`` — a scaled reducer over the same body.

    When this was written, the parsed-level simplifier distributed such a body before
    binding, which is why the bug hid for so long; a scaled reducer was one of the
    shapes it declined, so this reached the rebuild from a second direction. That layer
    was deleted at canonicalize.md C.4, so every test in this file now reaches the
    rebuild directly. Kept because the scaled-reducer path multiplies the factor in at
    the physical layer, which the others do not exercise.

    ``2 * (14 + SUM(q*x)) <= 36`` is ``SUM(q*x) <= 4`` — the same model as
    ``test_nested_product_oracle``, so the same answer is expected.
    """
    rows, cols = decidb_cli.execute(f"""
        SELECT id, q, x FROM {_TABLE}
        DECIDE x(BOOL)
        SUCH THAT 2 * SUM(q * (price + x)) <= 36
        MAXIMIZE SUM(x * q)
    """)
    ci = {c: i for i, c in enumerate(cols)}
    obj = sum(int(r[ci["x"]]) * float(r[ci["q"]]) for r in rows)
    assert abs(obj - 4.0) <= 1e-6, f"expected objective 4, got {obj}"


@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
@pytest.mark.parametrize("decl", ["DECIMAL(2,1)", "DECIMAL(9,2)", "DECIMAL(18,2)", "DOUBLE"])
def test_nested_product_narrow_decimal_widths(decidb_cli, decl):
    """The same query across DECIMAL widths and DOUBLE.

    The bug was a physical-width mismatch, so it only appeared where the operand and
    the signature landed on different physical storage types. DECIMAL(2,1) reproduced
    it; DECIMAL(18,2) and DOUBLE did not. Parametrizing pins the whole range, so a
    future rebuild that is correct for one width cannot quietly break another.
    """
    rows, cols = decidb_cli.execute(f"""
        SELECT id, q, x FROM (
            SELECT id, q::{decl} AS q, price::{decl} AS price FROM {_TABLE}
        ) s
        DECIDE x(BOOL)
        SUCH THAT SUM(q * (price + x)) <= {_CAP}
        MAXIMIZE SUM(x * q)
    """)
    ci = {c: i for i, c in enumerate(cols)}
    obj = sum(int(r[ci["x"]]) * float(r[ci["q"]]) for r in rows)
    assert abs(obj - 4.0) <= 1e-6, f"{decl}: expected objective 4, got {obj}"
