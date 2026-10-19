"""A factor sitting on a reducer: `2 * SUM(x)`, `2 * MAX(x*v)`, `SUM(x) / 2`.

Canonicalization peels such a factor outward off the reducer and converges every
spelling onto one (`scale * term`, factor on the left). It then **stays outside**:
the physical layer multiplies it into the per-row coefficients for SUM/AVG, or into
the auxiliary's contribution for MIN/MAX.

Nothing pushes it back in, and that is the point. MIN and MAX are order statistics —
``MAX(-2x)`` is ``-2*MIN(x)`` — so pushing a factor through one needs its sign, and a
scalar subquery's sign is not known until the query runs. Kept outside, the sign only
picks which linearization is *cheaper*.

A parsed-level fold used to push the factor in without checking the sign, which is a
silent wrong answer rather than an error: ``MINIMIZE -1 * MAX(x)`` was solved as
``MINIMIZE MAX(-x)``.

Every test here is oracle-verified. "No longer errors" would not distinguish a
correct answer from a sign-flipped one, and three of these shapes did not error
before — they returned the wrong number.

Covers:
  - test_negative_scale_on_max_objective: MINIMIZE -1 * MAX(x)   [was WRONG]
  - test_negative_scale_on_min_objective: MAXIMIZE -1 * MIN(x)   [was WRONG]
  - test_scaled_max_constraint: 2 * MAX(x*v) <= K                [was REJECTED]
  - test_scaled_max_in_composed_constraint: SUM(x) + 2*MAX(x*v)  [was REJECTED]
  - test_reducer_divided_by_constant: SUM(x) / 2 <= K
  - test_nested_multiplier_scale: 2 * (3 * SUM(x)) <= K
  - test_nested_divisor_scale: (SUM(x) / 2) / 3 <= K
  - test_scalar_subquery_scale: (SELECT ...) * SUM(x) <= K   [uncorrelated: legal]
  - rejection tests: row-varying factor, decision factor, CORRELATED subquery

A note on the last one. "One value for the whole query" is decided from evidence
collected before subquery flattening, not from the expression's shape — because
flattening leaves an uncorrelated subquery, a correlated subquery and a plain column
all looking like the same `BoundColumnRefExpression`. The test is an allow-list, so
anything not positively marked as query-wide is treated as row-varying.
"""

import time

import pytest

from solver.types import VarType, ObjSense, SolverStatus
from ._oracle_helpers import emit_hard_inner_max


# ---------------------------------------------------------------------------
# The sign swap. These two shapes returned a wrong answer before B.3.
# ---------------------------------------------------------------------------

@pytest.mark.min_max
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_negative_scale_on_max_objective(decidb_cli):
    """``MINIMIZE -1 * MAX(x)`` with ``SUM(x) <= 4``, ``x in [0,5]``.

    Minimizing ``-MAX(x)`` is maximizing ``MAX(x)``, so one row should take the
    whole budget: x = (4, 0), objective -4. Folding without the swap turns it into
    ``MINIMIZE MAX(-x)`` = ``MAXIMIZE MIN(x)``, which spreads the budget evenly and
    returns x = (2, 2). The two are distinguishable by the x vector alone, which is
    why this test reads the vector rather than only the objective.
    """
    rows, cols = decidb_cli.execute("""
        SELECT id, x FROM (VALUES (1), (2)) t(id)
        DECIDE x(INT)
        SUCH THAT x <= 5 AND SUM(x) <= 4
        MINIMIZE -1 * MAX(x)
    """)
    ci = {c: i for i, c in enumerate(cols)}
    xs = sorted(int(r[ci["x"]]) for r in rows)
    assert xs == [0, 4], f"expected one row to take the whole budget, got {xs}"


@pytest.mark.min_max
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_negative_scale_on_min_objective(decidb_cli):
    """``MAXIMIZE -1 * MIN(x)`` with ``SUM(x) >= 4``, ``x in [0,5]``.

    The mirror image: maximizing ``-MIN(x)`` is minimizing ``MIN(x)``, so one row is
    pushed to 0 and the other carries the requirement, giving ``MIN(x) = 0``.
    Folding without the swap gives ``MAXIMIZE MIN(-x)`` = ``MINIMIZE MAX(x)``, which
    spreads the requirement evenly and yields ``MIN(x) = 2``.

    Assert on ``MIN(x)`` rather than the vector: the surviving row may sit anywhere
    in [4, 5] without changing the objective, so the vector is not unique but the
    quantity being optimized is.
    """
    rows, cols = decidb_cli.execute("""
        SELECT id, x FROM (VALUES (1), (2)) t(id)
        DECIDE x(INT)
        SUCH THAT x <= 5 AND SUM(x) >= 4
        MAXIMIZE -1 * MIN(x)
    """)
    ci = {c: i for i, c in enumerate(cols)}
    xs = [int(r[ci["x"]]) for r in rows]
    assert min(xs) == 0, f"expected MIN(x) driven to 0, got {xs}"
    assert sum(xs) >= 4, f"constraint violated: {xs}"


# ---------------------------------------------------------------------------
# Shapes that used to be rejected outright.
# ---------------------------------------------------------------------------

@pytest.mark.min_max
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_scaled_max_constraint(decidb_cli, oracle_solver, perf_tracker):
    """``2 * MAX(x * v) <= 10`` — rejected before B.3 with "non-aggregate term".

    MAX bounded above is the easy direction, so this is per-row ``2*x_i*v_i <= 10``.
    Oracle-verified because a wrong scale would still look like a plausible answer.
    """
    data = [(1, 2.0), (2, 3.0), (3, 5.0)]
    t0 = time.perf_counter()
    rows, cols = decidb_cli.execute("""
        SELECT id, v, x FROM (VALUES (1, 2.0), (2, 3.0), (3, 5.0)) t(id, v)
        DECIDE x(INT)
        SUCH THAT x <= 9 AND 2 * MAX(x * v) <= 10
        MAXIMIZE SUM(x)
    """)
    decidb_time = time.perf_counter() - t0

    t_build = time.perf_counter()
    oracle_solver.create_model("scaled_max_constraint")
    names = [f"x_{i}" for i in range(len(data))]
    for n in names:
        oracle_solver.add_variable(n, VarType.INTEGER, lb=0.0, ub=9.0)
    # MAX(e) <= K  <=>  e_i <= K for every row; the 2 rides on each row.
    for i, (_, v) in enumerate(data):
        oracle_solver.add_constraint({names[i]: 2.0 * v}, "<=", 10.0, name=f"row_{i}")
    oracle_solver.set_objective({n: 1.0 for n in names}, ObjSense.MAXIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL

    ci = {c: i for i, c in enumerate(cols)}
    decidb_obj = sum(int(r[ci["x"]]) for r in rows)
    assert abs(decidb_obj - result.objective_value) <= 1e-6, (
        f"Objective mismatch: DecidB={decidb_obj}, Oracle={result.objective_value}"
    )
    # And the constraint the user wrote actually holds.
    for r in rows:
        assert 2.0 * int(r[ci["x"]]) * float(r[ci["v"]]) <= 10.0 + 1e-6

    perf_tracker.record(
        "scaled_max_constraint", decidb_time, build_time, result.solve_time_seconds,
        len(data), len(names), len(data), result.objective_value,
        oracle_solver.solver_name(), comparison_status="optimal",
    )


@pytest.mark.min_max
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_scaled_max_in_composed_constraint(decidb_cli, oracle_solver, perf_tracker):
    """``SUM(x) + 2 * MAX(x * v) <= 12`` — rejected before B.3 by the composed
    walker ("does not support scalar multiplication ... of aggregate terms").

    MAX pushed *down* by an upper-bounded row is still the easy direction, but the
    term now shares the row with a SUM, so it becomes a genuine auxiliary. Oracle
    builds z >= x_i*v_i per row and the outer row SUM(x) + 2z <= 12.
    """
    data = [(1, 2.0), (2, 3.0)]
    t0 = time.perf_counter()
    rows, cols = decidb_cli.execute("""
        SELECT id, v, x FROM (VALUES (1, 2.0), (2, 3.0)) t(id, v)
        DECIDE x(INT)
        SUCH THAT x <= 9 AND SUM(x) + 2 * MAX(x * v) <= 12
        MAXIMIZE SUM(x)
    """)
    decidb_time = time.perf_counter() - t0

    t_build = time.perf_counter()
    oracle_solver.create_model("scaled_max_composed")
    names = [f"x_{i}" for i in range(len(data))]
    for n in names:
        oracle_solver.add_variable(n, VarType.INTEGER, lb=0.0, ub=9.0)
    z = "zmax"
    oracle_solver.add_variable(z, VarType.CONTINUOUS, lb=0.0, ub=9.0 * 3.0)
    for i, (_, v) in enumerate(data):
        oracle_solver.add_constraint({names[i]: v, z: -1.0}, "<=", 0.0, name=f"zlb_{i}")
    outer = {n: 1.0 for n in names}
    outer[z] = 2.0
    oracle_solver.add_constraint(outer, "<=", 12.0, name="outer")
    oracle_solver.set_objective({n: 1.0 for n in names}, ObjSense.MAXIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL

    ci = {c: i for i, c in enumerate(cols)}
    decidb_obj = sum(int(r[ci["x"]]) for r in rows)
    assert abs(decidb_obj - result.objective_value) <= 1e-6, (
        f"Objective mismatch: DecidB={decidb_obj}, Oracle={result.objective_value}"
    )
    xs = {int(r[ci["id"]]): int(r[ci["x"]]) for r in rows}
    vs = dict(data)
    lhs = sum(xs.values()) + 2.0 * max(xs[i] * vs[i] for i in xs)
    assert lhs <= 12.0 + 1e-6, f"constraint violated: {lhs} > 12"

    perf_tracker.record(
        "scaled_max_composed", decidb_time, build_time, result.solve_time_seconds,
        len(data), len(names) + 1, len(data) + 1, result.objective_value,
        oracle_solver.solver_name(), comparison_status="optimal",
    )


@pytest.mark.min_max
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_negative_scale_on_max_constraint(decidb_cli):
    """``-2 * MAX(x) >= -8`` — the negative factor flips which direction is easy.

    ``-2*MAX(x) >= -8`` is ``MAX(x) <= 4``, the easy direction, so every row is
    capped at 4 and the budget-free objective drives all rows there. Read without
    the swap it would be ``MAX(-2x) >= -8``, i.e. *some* row satisfies -2x >= -8,
    which leaves the other rows unbounded — a strictly larger feasible set.
    """
    rows, cols = decidb_cli.execute("""
        SELECT id, x FROM (VALUES (1), (2)) t(id)
        DECIDE x(INT)
        SUCH THAT x <= 9 AND -2 * MAX(x) >= -8
        MAXIMIZE SUM(x)
    """)
    ci = {c: i for i, c in enumerate(cols)}
    xs = sorted(int(r[ci["x"]]) for r in rows)
    assert xs == [4, 4], f"every row should be capped at 4, got {xs}"


# ---------------------------------------------------------------------------
# Objectives get no canonicalization, so the fold must match both spellings.
# ---------------------------------------------------------------------------
#
# `DecideCanonicalizer` runs on CONSTRAINTS only. It is what converges
# `SUM(x) * 2` onto `2 * SUM(x)`, so inside a constraint the optimizer's fold only
# ever sees the factor on the left. An objective arrives spelled however the user
# wrote it. Matching one order there rejects the other — which is exactly the
# same-shape-different-outcome asymmetry this work exists to remove.

@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_objective_factor_on_the_right(decidb_cli):
    """``MAXIMIZE SUM(x*p) * 2`` — factor on the right of the reducer.

    Must agree with ``MAXIMIZE 2 * SUM(x*p)``: a positive factor cannot change the
    argmax, so both should spend the budget of 3 on the highest-priced row.
    """
    rows, cols = decidb_cli.execute("""
        SELECT id, p, x FROM (VALUES (1, 10.0), (2, 20.0)) t(id, p)
        DECIDE x(INT)
        SUCH THAT x <= 9 AND SUM(x) <= 3
        MAXIMIZE SUM(x * p) * 2
    """)
    ci = {c: i for i, c in enumerate(cols)}
    picks = {float(r[ci["p"]]): int(r[ci["x"]]) for r in rows}
    assert picks == {10.0: 0, 20.0: 3}, f"expected the budget on p=20, got {picks}"


@pytest.mark.min_max
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_objective_negative_factor_on_the_right(decidb_cli):
    """``MINIMIZE MAX(x) * -1`` — the swap has to fire for this spelling too.

    Same query as ``MINIMIZE -1 * MAX(x)``: minimizing ``-MAX(x)`` maximizes
    ``MAX(x)``, so with ``SUM(x) >= 3`` and ``x <= 9`` every row goes to 9.
    """
    rows, cols = decidb_cli.execute("""
        SELECT id, x FROM (VALUES (1), (2)) t(id)
        DECIDE x(INT)
        SUCH THAT x <= 9 AND SUM(x) >= 3
        MINIMIZE MAX(x) * -1
    """)
    ci = {c: i for i, c in enumerate(cols)}
    xs = [int(r[ci["x"]]) for r in rows]
    assert max(xs) == 9, f"expected MAX(x) driven to 9, got {xs}"


# ---------------------------------------------------------------------------
# Division, and a factor that is not a literal.
# ---------------------------------------------------------------------------

@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_reducer_divided_by_constant(decidb_cli):
    """``SUM(x) / 2 <= 3`` — division is a scale too, and stays a division rather
    than being reciprocated (``SUM(x)/3`` has no exact decimal reciprocal)."""
    rows, cols = decidb_cli.execute("""
        SELECT id, x FROM (VALUES (1), (2)) t(id)
        DECIDE x(INT)
        SUCH THAT x <= 9 AND SUM(x) / 2 <= 3
        MAXIMIZE SUM(x)
    """)
    ci = {c: i for i, c in enumerate(cols)}
    total = sum(int(r[ci["x"]]) for r in rows)
    assert total == 6, f"expected SUM(x) = 6, got {total}"


@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_scalar_subquery_scale(decidb_cli):
    """``(SELECT max(w) FROM t) * SUM(x) <= 12`` — an uncorrelated scalar subquery
    is one value for the whole query, so it is a legal factor even though its
    value is unknown at plan time.

    It survives only because plan_select_node.cpp records, before flattening, which
    table indexes the UNCORRELATED scalar subqueries land on. Shape cannot answer
    this: after flattening the factor is a plain column reference, indistinguishable
    from ``w`` itself and from a correlated subquery (see the rejection test below).
    """
    rows, cols = decidb_cli.execute("""
        SELECT id, x FROM (VALUES (1, 2.0), (2, 3.0)) t(id, w)
        DECIDE x(INT)
        SUCH THAT x <= 5 AND (SELECT max(w) FROM (VALUES (2.0), (3.0)) u(w)) * SUM(x) <= 12
        MAXIMIZE SUM(x)
    """)
    ci = {c: i for i, c in enumerate(cols)}
    total = sum(int(r[ci["x"]]) for r in rows)
    assert total == 4, f"expected 3*SUM(x) <= 12 -> SUM(x) = 4, got {total}"


# ---------------------------------------------------------------------------
# A factor that is not one value for the whole query is rejected.
# ---------------------------------------------------------------------------

@pytest.mark.cons_aggregate
@pytest.mark.error_binder
def test_correlated_subquery_factor_rejected(decidb_cli):
    """A CORRELATED subquery yields a different value per row, so it is no more a
    legal factor than a bare column is.

    This is the case that shape-matching cannot catch and that an earlier version of
    the rule silently accepted. Both an uncorrelated and a correlated scalar
    subquery flatten into a plain ``BoundColumnRefExpression`` on a table index of
    their own, so a rule phrased as "not one of the reduced relation's own bindings"
    admits both. The rule is now an allow-list built from correlation information
    captured before flattening, so anything not positively marked is row-varying.

    Silently accepted, it computed ``SUM(k * x)`` — a per-row coefficient — which is
    exactly what the error tells the user to write explicitly.
    """
    decidb_cli.assert_error("""
        SELECT id, x FROM (VALUES (1, 2.0), (2, 3.0)) m(id, w)
        DECIDE x(INT)
        SUCH THAT x <= 5
            AND (SELECT k FROM (VALUES (1, 10.0), (2, 100.0)) s(sid, k) WHERE s.sid = m.id)
                * SUM(x) <= 100
        MAXIMIZE SUM(x)
    """, match=r"this subquery returns a different value for each row")


@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_correlated_subquery_factor_moved_inside(decidb_cli):
    """The edit the rejection recommends works and keeps the old meaning.

    ``SUM(x * (SELECT ...))`` gives the per-row-coefficient reading the buggy
    acceptance was silently producing: over k = (10, 100) with ``x <= 5``, the cap
    of 100 admits ``x = (5, 0)``.
    """
    rows, cols = decidb_cli.execute("""
        SELECT id, x FROM (VALUES (1, 2.0), (2, 3.0)) m(id, w)
        DECIDE x(INT)
        SUCH THAT x <= 5
            AND SUM(x * (SELECT k FROM (VALUES (1, 10.0), (2, 100.0)) s(sid, k)
                         WHERE s.sid = m.id)) <= 100
        MAXIMIZE SUM(x)
    """)
    ci = {c: i for i, c in enumerate(cols)}
    picks = {int(r[ci["id"]]): int(r[ci["x"]]) for r in rows}
    assert picks == {1: 5, 2: 0}, f"expected x=(5,0), got {picks}"

@pytest.mark.cons_aggregate
@pytest.mark.error_binder
def test_row_varying_factor_rejected(decidb_cli):
    """``w * SUM(x)`` — a reducer collapses many rows to one number, so "which
    row's w scales it?" has no answer. The message names the column and gives the
    edit that expresses the per-row-coefficient reading instead."""
    decidb_cli.assert_error("""
        SELECT id, x FROM (VALUES (1, 2.0), (2, 3.0)) t(id, w)
        DECIDE x(INT)
        SUCH THAT w * SUM(x) <= 4
        MAXIMIZE SUM(x)
    """, match=r"'w' varies per row, so it cannot multiply SUM\(x\)")


@pytest.mark.min_max
@pytest.mark.cons_aggregate
@pytest.mark.error_binder
def test_row_varying_factor_on_minmax_rejected(decidb_cli):
    """Same rule for MIN/MAX, reached by a different route: the simplifier declines
    a MIN/MAX-only LHS, so this shape arrives at canonicalization untouched."""
    decidb_cli.assert_error("""
        SELECT id, x FROM (VALUES (1, 2.0), (2, 3.0)) t(id, w)
        DECIDE x(INT)
        SUCH THAT x <= 9 AND w * MAX(x) <= 4
        MAXIMIZE SUM(x)
    """, match=r"'w' varies per row, so it cannot multiply MAX\(x\)")


@pytest.mark.cons_aggregate
@pytest.mark.error_binder
def test_decision_factor_rejected(decidb_cli):
    """``s * SUM(x)`` where ``s`` is a query-wide decision — a product of two
    decisions is bilinear, not a scale. Before B.3 this produced the unrelated
    "SUM(s) has nothing to aggregate over", because the fold pushed ``s`` into the
    reducer's body."""
    decidb_cli.assert_error("""
        SELECT id, x, s FROM (VALUES (1), (2)) t(id)
        DECIDE x(INT), scalar s(INT)
        SUCH THAT x <= 5 AND s <= 3 AND s * SUM(x) <= 12
        MAXIMIZE SUM(x)
    """, match=r"'s' is a decision, so it cannot multiply SUM\(x\)")


# ---------------------------------------------------------------------------
# A factor whose SIGN is not known until the query runs.
# ---------------------------------------------------------------------------
#
# An uncorrelated scalar subquery is a legal factor — one value for the whole query —
# but its value, and therefore its sign, is unavailable at plan time. That matters
# only for MIN/MAX, which are order statistics: a negative factor turns a MAX into a
# MIN. Keeping the factor OUTSIDE the reducer is what makes this survivable — the
# sign then selects which linearization is *cheaper*, never which one is *correct*.
#
# These are the shapes that used to fail with the internal assertion
# "DECIDE optimizer should rewrite aggregate 'max' to SUM before execution".
#
# They are also where a plausible-looking fix went wrong once. The single-term
# MIN/MAX rewrite has two encodings — "easy" fans the bound out over EVERY row, and
# "hard" asserts SOME row attains it. Those are opposite quantifiers, so defaulting
# to "hard" when the sign is unknown silently drops the ∀ half: `3 * MAX(x) <= 12`
# accepted x = 9 on every row. Each test below therefore asserts the constraint
# actually holds, not merely that the query ran.

@pytest.mark.min_max
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_unknown_sign_factor_on_max_constraint(decidb_cli):
    """``(SELECT max(k)) * MAX(x) <= 12`` with k = 3, so ``MAX(x) <= 4``.

    Maximizing SUM(x) drives every row to the cap. A formulation that only asserted
    "some row attains the max" would leave the other rows free at x = 9.
    """
    rows, cols = decidb_cli.execute("""
        SELECT id, x FROM (VALUES (1), (2)) t(id)
        DECIDE x(INT)
        SUCH THAT x <= 9 AND (SELECT max(k) FROM (VALUES (3.0)) u(k)) * MAX(x) <= 12
        MAXIMIZE SUM(x)
    """)
    ci = {c: i for i, c in enumerate(cols)}
    xs = [int(r[ci["x"]]) for r in rows]
    assert max(xs) <= 4, f"constraint violated: 3 * MAX({xs}) > 12"
    assert xs == [4, 4], f"expected both rows at the cap, got {xs}"


@pytest.mark.min_max
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_unknown_sign_factor_on_min_constraint(decidb_cli):
    """``(SELECT max(k)) * MIN(x) >= 3`` with k = 3, so ``MIN(x) >= 1``."""
    rows, cols = decidb_cli.execute("""
        SELECT id, x FROM (VALUES (1), (2)) t(id)
        DECIDE x(INT)
        SUCH THAT x <= 9 AND (SELECT max(k) FROM (VALUES (3.0)) u(k)) * MIN(x) >= 3
        MAXIMIZE SUM(x)
    """)
    ci = {c: i for i, c in enumerate(cols)}
    xs = [int(r[ci["x"]]) for r in rows]
    assert min(xs) >= 1, f"constraint violated: 3 * MIN({xs}) < 3"
    assert xs == [9, 9], f"expected both rows at their upper bound, got {xs}"


@pytest.mark.min_max
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_unknown_sign_factor_in_composed_constraint(decidb_cli):
    """``SUM(x) + (SELECT max(k)) * MAX(x) <= 12`` with k = 3.

    Feasible set: SUM(x) + 3*MAX(x) <= 12. The optimum of SUM(x) is 4, at (2,2):
    4 + 6 = 10. Any larger total pushes MAX up faster than the budget allows —
    (3,1) gives 4 + 9 = 13.
    """
    rows, cols = decidb_cli.execute("""
        SELECT id, x FROM (VALUES (1), (2)) t(id)
        DECIDE x(INT)
        SUCH THAT x <= 9 AND SUM(x) + (SELECT max(k) FROM (VALUES (3.0)) u(k)) * MAX(x) <= 12
        MAXIMIZE SUM(x)
    """)
    ci = {c: i for i, c in enumerate(cols)}
    xs = [int(r[ci["x"]]) for r in rows]
    assert sum(xs) + 3 * max(xs) <= 12 + 1e-6, f"constraint violated: {xs}"
    assert sum(xs) == 4, f"expected SUM(x) = 4, got {xs}"


@pytest.mark.min_max
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_unknown_sign_factor_on_minmax_objective(decidb_cli):
    """``MINIMIZE (SELECT max(k)) * MAX(x)`` with k = 3 — minimize 3*MAX(x).

    With no lower pressure on x the optimum is all zeros. The flat MIN/MAX objective
    path cannot express this: it replaces the whole objective with its auxiliary at
    coefficient 1.0, leaving nowhere to put the factor, so a scaled objective is
    routed through the composed path instead.
    """
    rows, cols = decidb_cli.execute("""
        SELECT id, x FROM (VALUES (1), (2)) t(id)
        DECIDE x(INT)
        SUCH THAT x <= 9 AND SUM(x) <= 4
        MINIMIZE (SELECT max(k) FROM (VALUES (3.0)) u(k)) * MAX(x)
    """)
    ci = {c: i for i, c in enumerate(cols)}
    xs = [int(r[ci["x"]]) for r in rows]
    assert max(xs) == 0, f"expected MAX(x) driven to 0, got {xs}"


@pytest.mark.min_max
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_scaled_composed_minmax_over_a_join(decidb_cli):
    """The same composed scaled MIN/MAX, but over a JOIN.

    A scale on a composed term is an Expression the optimizer moves out of the
    constraint tree into `ComposedMinMaxTerm`, which opts it out of
    `ColumnBindingResolver` unless that case is updated by hand. An unresolved
    reference still carries a LOGICAL (table_index, column_index), and the physical
    operator reads it as a POSITION in the materialized chunk — the two coincide for
    a single-table source, so single-table tests cannot see the bug. Every other test
    in this file uses VALUES; this one must not.

    `SUM(x) + 2 * MAX(x * v) <= 12` with v from the joined table: v = (2, 3).
    Optimum is SUM(x) = 4 at (2, 2): 4 + 2*max(4,6) = 4 + 12 = 16 > 12, so (2,2) is
    infeasible; (1,2) gives 3 + 2*6 = 15; (2,1) gives 3 + 2*4 = 11 <= 12, SUM = 3.
    """
    rows, cols = decidb_cli.execute("""
        SELECT t.id, s.v, x FROM (VALUES (1), (2)) t(id)
        JOIN (VALUES (1, 2.0), (2, 3.0)) s(sid, v) ON s.sid = t.id
        DECIDE x(INT)
        SUCH THAT x <= 9 AND SUM(x) + 2 * MAX(x * s.v) <= 12
        MAXIMIZE SUM(x)
    """)
    ci = {c: i for i, c in enumerate(cols)}
    pairs = [(float(r[ci["v"]]), int(r[ci["x"]])) for r in rows]
    lhs = sum(x for _, x in pairs) + 2 * max(v * x for v, x in pairs)
    assert lhs <= 12 + 1e-6, f"constraint violated: {pairs} gives {lhs}"
    assert sum(x for _, x in pairs) == 3, f"expected SUM(x) = 3, got {pairs}"


@pytest.mark.obj_maximize
@pytest.mark.error_binder
def test_decision_factor_in_objective_rejected(decidb_cli):
    """``MAXIMIZE s * SUM(x)`` — a decision on both sides of the `*`.

    A factor on a reducer must be one value for the whole query, and a decision is
    not. ``DecideCanonicalizer`` vets objective factors at the planning boundary with
    the same ``PeelScale`` rule it applies to constraints, so the rejection names the
    specific reducer (``SUM(x)``) rather than "an aggregate". The physical extractor
    keeps its own check as a defensive invariant, but it is no longer the first place
    that can say no — without a guard the decision column was read as a data
    coefficient and evaluation crashed with an internal DuckDB assertion.
    """
    decidb_cli.assert_error("""
        SELECT id, x, s FROM (VALUES (1), (2)) t(id)
        DECIDE x(INT), scalar s(INT)
        SUCH THAT x <= 5 AND s <= 3 AND SUM(x) <= 4
        MAXIMIZE s * SUM(x)
    """, match=r"'s' is a decision, so it cannot multiply SUM\(x\)")


@pytest.mark.min_max
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_composed_minmax_term_divided_by_constant(decidb_cli):
    """``SUM(x) + MAX(x*w) / 2 <= 12`` — division on a COMPOSED MIN/MAX term.

    Division reaches the composed path through the same `scale` slot as
    multiplication, with `scale_divides` set; the physical layer inverts it once.
    w = (2,3), x <= 9. The optimum is SUM(x) = 7 at (4,3): 7 + max(8,9)/2 = 11.5.
    """
    rows, cols = decidb_cli.execute("""
        SELECT id, w, x FROM (VALUES (1, 2.0), (2, 3.0)) t(id, w)
        DECIDE x(INT)
        SUCH THAT x <= 9 AND SUM(x) + MAX(x * w) / 2 <= 12
        MAXIMIZE SUM(x)
    """)
    ci = {c: i for i, c in enumerate(cols)}
    pairs = [(float(r[ci["w"]]), int(r[ci["x"]])) for r in rows]
    lhs = sum(x for _, x in pairs) + max(w * x for w, x in pairs) / 2.0
    assert lhs <= 12 + 1e-6, f"constraint violated: {pairs} gives {lhs}"
    assert sum(x for _, x in pairs) == 7, f"expected SUM(x) = 7, got {pairs}"


# ---------------------------------------------------------------------------
# Nested query-wide scales compose into one accepted scale.
# ---------------------------------------------------------------------------

@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_nested_multiplier_scale(decidb_cli):
    """``2 * (3 * SUM(x)) <= 12`` is the composed scale ``6 * SUM(x)``.

    The accepted-shape contract chooses support rather than a late physical
    extractor error.  Integer factors keep typed evaluation unambiguous, so
    this is the smallest positive case for the Step 4 implementation.
    """
    rows, cols = decidb_cli.execute("""
        SELECT id, x FROM (VALUES (1), (2)) t(id)
        DECIDE x(INT)
        SUCH THAT x <= 9 AND 2 * (3 * SUM(x)) <= 12
        MAXIMIZE SUM(x)
    """)
    total = sum(int(r[cols.index("x")]) for r in rows)
    assert total == 2, f"expected 6 * SUM(x) <= 12, got SUM(x)={total}"


@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_nested_divisor_scale(decidb_cli):
    """``(SUM(x) / 2) / 3 <= 1`` composes without reassociating division.

    Evaluating the written typed operations yields ``SUM(x) <= 6``.  The test
    pins support and the arithmetic direction independently of multiplication.
    """
    rows, cols = decidb_cli.execute("""
        SELECT id, x FROM (VALUES (1), (2)) t(id)
        DECIDE x(INT)
        SUCH THAT x <= 9 AND (SUM(x) / 2) / 3 <= 1
        MAXIMIZE SUM(x)
    """)
    total = sum(int(r[cols.index("x")]) for r in rows)
    assert total == 6, f"expected (SUM(x) / 2) / 3 <= 1, got SUM(x)={total}"
