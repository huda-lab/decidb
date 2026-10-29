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

import re
import time

import pytest

from solver.types import VarType, ObjSense, SolverStatus
from ._brute_force import judge_rows
from ._oracle_helpers import emit_hard_inner_max
from ._scope_cases import answer, scope_solver  # noqa: F401 -- scope_solver is a fixture
from ._scope_oracle import CONSTANT, Decision, Reduce, Scoped, ScopedModel, ScopedObjective


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
        f"Objective mismatch: DeciDB={decidb_obj}, Oracle={result.objective_value}"
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
        f"Objective mismatch: DeciDB={decidb_obj}, Oracle={result.objective_value}"
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
# Objectives get no canonicalization, so the fold must match both orders.
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


def _judged_per_row_factor(cli, solver, sql, rows, clauses):
    """Judge `sql` (MAXIMIZE SUM(x) over `rows`, ordered by id) against the scope oracle."""
    model = ScopedModel(rows, [Decision("x")])
    objective = ScopedObjective(ObjSense.MAXIMIZE, (Reduce("sum", lambda r: {"x": 1}),))
    assert model.judge_scoped(answer(cli, sql), clauses, objective) == pytest.approx(
        model.solve_scoped(solver, clauses, objective), abs=1e-4
    )


@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_correlated_subquery_factor_is_read_per_row(decidb_cli, scope_solver):
    """A correlated subquery is row-varying, and a clause with no `per` is generated once
    per row (D30): each row scales `SUM(x)` by its own k, so the tightest row binds."""
    rows = [{"id": 1, "k": 10.0}, {"id": 2, "k": 100.0}]
    clauses = [
        Scoped((lambda r: {"x": 1},), "<=", (lambda r: {CONSTANT: 5},)),
        Scoped((Reduce("sum", lambda r: {"x": 1}, factor=lambda r: r["k"]),), "<=", (lambda r: {CONSTANT: 100},)),
    ]
    _judged_per_row_factor(
        decidb_cli,
        scope_solver,
        """
        SELECT id, x FROM (VALUES (1, 2.0), (2, 3.0)) m(id, w)
        DECIDE x(INT)
        SUCH THAT x <= 5
            AND (SELECT k FROM (VALUES (1, 10.0), (2, 100.0)) s(sid, k) WHERE s.sid = m.id)
                * SUM(x) <= 100
        MAXIMIZE SUM(x)
        ORDER BY id
    """,
        rows,
        clauses,
    )


@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_correlated_subquery_factor_moved_inside(decidb_cli):
    """Moving the factor inside the reducer works and reads it per row.

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
@pytest.mark.correctness
def test_row_varying_factor_is_read_per_row(decidb_cli, scope_solver):
    """``w * SUM(x) <= 4`` is generated once per row (D30): each row's w scales the one
    total, so the largest w binds."""
    rows = [{"id": 1, "w": 2.0}, {"id": 2, "w": 3.0}]
    clauses = [Scoped((Reduce("sum", lambda r: {"x": 1}, factor=lambda r: r["w"]),), "<=", (lambda r: {CONSTANT: 4},))]
    _judged_per_row_factor(
        decidb_cli,
        scope_solver,
        """
        SELECT id, x FROM (VALUES (1, 2.0), (2, 3.0)) t(id, w)
        DECIDE x(INT)
        SUCH THAT w * SUM(x) <= 4
        MAXIMIZE SUM(x)
        ORDER BY id
    """,
        rows,
        clauses,
    )


@pytest.mark.min_max
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_row_varying_factor_on_minmax_is_read_per_row(decidb_cli, scope_solver):
    """The same for MAX: ``w * MAX(x) <= 4`` once per row (D30)."""
    rows = [{"id": 1, "w": 2.0}, {"id": 2, "w": 3.0}]
    clauses = [
        Scoped((lambda r: {"x": 1},), "<=", (lambda r: {CONSTANT: 9},)),
        Scoped((Reduce("max", lambda r: {"x": 1}, factor=lambda r: r["w"]),), "<=", (lambda r: {CONSTANT: 4},)),
    ]
    _judged_per_row_factor(
        decidb_cli,
        scope_solver,
        """
        SELECT id, x FROM (VALUES (1, 2.0), (2, 3.0)) t(id, w)
        DECIDE x(INT)
        SUCH THAT x <= 9 AND w * MAX(x) <= 4
        MAXIMIZE SUM(x)
        ORDER BY id
    """,
        rows,
        clauses,
    )


@pytest.mark.cons_aggregate
@pytest.mark.error_binder
def test_decision_factor_rejected(decidb_cli):
    """``s * SUM(x)`` where ``s`` is a query-wide decision — a product of two
    decisions is bilinear, not a scale. Before B.3 this produced the unrelated
    "SUM(s) has nothing to aggregate over", because the fold pushed ``s`` into the
    reducer's body."""
    decidb_cli.assert_error(
        """
        SELECT id, x, s FROM (VALUES (1), (2)) t(id)
        DECIDE x(INT), per (): s(INT)
        SUCH THAT x <= 5 AND s <= 3 AND s * SUM(x) <= 12
        MAXIMIZE SUM(x)
    """,
        match=r"'s' is a decision, so it cannot multiply SUM\(x\)",
    )


# ---------------------------------------------------------------------------
# A BOOL decision as a factor on a reducer.
# ---------------------------------------------------------------------------
#
# `b * SUM(x)` is a product of two decisions, and a Boolean one is linear: the reducer's
# value lives in a helper decision (the value rewrite) and the product with it is one
# McCormick column, as for any Boolean product. An INT or REAL factor is not convex and
# stays refused (the two tests above).
#
# The scope oracle states no product of decisions, so each answer is judged against every
# assignment of three rows' decisions (`_brute_force`): the optimum, and DeciDB's answer
# against the clause stated in Python.

#: Rows 1 and 2 are group a, row 3 is group b.
_BOOL_FACTOR_ROWS = "(VALUES (1, 'a'), (2, 'a'), (3, 'b')) t(id, g)"
_X_DOMAIN = {f"x{i}": range(4) for i in range(3)}
_QUERY_WIDE_B = ("x(INT), per (): b(BOOL)", "b", {**_X_DOMAIN, "b": range(2)})
_ROW_B = ("x(INT), c(BOOL)", "c", {**_X_DOMAIN, **{f"c{i}": range(2) for i in range(3)}})


def _xs(a):
    return [a["x0"], a["x1"], a["x2"]]


def _bool_factor_sql(decide, column, clause, objective, maximize=True):
    return f"""
        SELECT id, x, {column} FROM {_BOOL_FACTOR_ROWS}
        DECIDE {decide}
        SUCH THAT x <= 3 AND {clause}
        {"MAXIMIZE" if maximize else "MINIMIZE"} {objective}
        ORDER BY id"""


BOOL_FACTOR_CASES = [
    pytest.param(
        _QUERY_WIDE_B,
        "b * SUM(x) <= 4",
        "SUM(x) + 6 * b",
        lambda a: a["b"] * sum(_xs(a)) <= 4,
        lambda a: sum(_xs(a)) + 6 * a["b"],
        True,
        id="sum-below",
    ),
    pytest.param(
        _QUERY_WIDE_B,
        "b * SUM(x) >= 4",
        "SUM(x) - 5 * b",
        lambda a: a["b"] * sum(_xs(a)) >= 4,
        lambda a: sum(_xs(a)) - 5 * a["b"],
        False,
        id="sum-above",
    ),
    pytest.param(
        _QUERY_WIDE_B,
        "-(b * SUM(x)) >= -4",
        "SUM(x) + 6 * b",
        lambda a: -(a["b"] * sum(_xs(a))) >= -4,
        lambda a: sum(_xs(a)) + 6 * a["b"],
        True,
        id="negated-keeps-its-sign",
    ),
    pytest.param(
        _QUERY_WIDE_B,
        "SUM(x) * b <= 4",
        "SUM(x) + 6 * b",
        lambda a: sum(_xs(a)) * a["b"] <= 4,
        lambda a: sum(_xs(a)) + 6 * a["b"],
        True,
        id="factor-written-after",
    ),
    pytest.param(
        _QUERY_WIDE_B,
        "2 * (b * SUM(x)) <= 6",
        "SUM(x) + 8 * b",
        lambda a: 2 * a["b"] * sum(_xs(a)) <= 6,
        lambda a: sum(_xs(a)) + 8 * a["b"],
        True,
        id="data-around-the-product",
    ),
    pytest.param(
        _QUERY_WIDE_B,
        "2 * b * SUM(x) <= 6",
        "SUM(x) + 8 * b",
        lambda a: 2 * a["b"] * sum(_xs(a)) <= 6,
        lambda a: sum(_xs(a)) + 8 * a["b"],
        True,
        id="data-times-the-boolean",
    ),
    pytest.param(
        _QUERY_WIDE_B,
        "b * AVG(x) <= 1",
        "SUM(x) + 7 * b",
        lambda a: a["b"] * sum(_xs(a)) <= 3,
        lambda a: sum(_xs(a)) + 7 * a["b"],
        True,
        id="avg",
    ),
    pytest.param(
        _QUERY_WIDE_B,
        "b * MAX(x) <= 2",
        "SUM(x) + 7 * b",
        lambda a: a["b"] * max(_xs(a)) <= 2,
        lambda a: sum(_xs(a)) + 7 * a["b"],
        True,
        id="max",
    ),
    pytest.param(
        _QUERY_WIDE_B,
        "b * MIN(x) >= 1",
        "SUM(x) - 4 * b",
        lambda a: a["b"] * min(_xs(a)) >= 1,
        lambda a: sum(_xs(a)) - 4 * a["b"],
        False,
        id="min",
    ),
    pytest.param(
        _QUERY_WIDE_B,
        "b * SUM(x) by (t.g) <= 3",
        "SUM(x) + 8 * b",
        lambda a: a["b"] * (a["x0"] + a["x1"]) <= 3 and a["b"] * a["x2"] <= 3,
        lambda a: sum(_xs(a)) + 8 * a["b"],
        True,
        id="one-helper-per-group",
    ),
    pytest.param(
        _QUERY_WIDE_B,
        "when t.id > 1: b * SUM(x) <= 3",
        "SUM(x) + 8 * b",
        lambda a: a["b"] * (a["x1"] + a["x2"]) <= 3,
        lambda a: sum(_xs(a)) + 8 * a["b"],
        True,
        id="the-reducer-reads-the-kept-rows",
    ),
    pytest.param(
        _QUERY_WIDE_B,
        "b * SUM(x) = 3",
        "SUM(x)",
        lambda a: a["b"] * sum(_xs(a)) == 3,
        lambda a: sum(_xs(a)),
        True,
        id="equality",
    ),
    pytest.param(
        _QUERY_WIDE_B,
        "SUM(x) <= 4 AND b * SUM(x) <> 3",
        "SUM(x) + 4 * b",
        lambda a: sum(_xs(a)) <= 4 and a["b"] * sum(_xs(a)) != 3,
        lambda a: sum(_xs(a)) + 4 * a["b"],
        True,
        id="not-equal",
    ),
    pytest.param(
        _QUERY_WIDE_B,
        "b * SUM(x) + SUM(x) <= 7",
        "SUM(x) + 3 * b",
        lambda a: a["b"] * sum(_xs(a)) + sum(_xs(a)) <= 7,
        lambda a: sum(_xs(a)) + 3 * a["b"],
        True,
        id="beside-the-plain-reducer",
    ),
    pytest.param(
        _ROW_B,
        "c * SUM(x) <= 3",
        "SUM(x) + 4 * SUM(c)",
        lambda a: all(a[f"c{i}"] * sum(_xs(a)) <= 3 for i in range(3)),
        lambda a: sum(_xs(a)) + 4 * sum(a[f"c{i}"] for i in range(3)),
        True,
        id="a-boolean-per-row",
    ),
]


@pytest.mark.correctness
@pytest.mark.cons_aggregate
@pytest.mark.parametrize("decision, clause, objective, feasible, value, maximize", BOOL_FACTOR_CASES)
def test_bool_decision_factor_on_a_reducer(
    decidb_cli_gurobi, decidb_cli_highs, decision, clause, objective, feasible, value, maximize
):
    """Each shape answers on both solvers with the oracle's optimum, and the answer satisfies the clause as written."""
    decide, column, domains = decision
    sql = _bool_factor_sql(decide, column, clause, objective, maximize)
    for cli in (decidb_cli_gurobi, decidb_cli_highs):
        judge_rows(answer(cli, sql), domains, feasible, value, maximize)


@pytest.mark.cons_aggregate
@pytest.mark.error_binder
@pytest.mark.parametrize(
    "clause",
    [
        pytest.param("b * (SUM(x) - 1) <= 4", id="a-reducer-inside-another-expression"),
        pytest.param("(b + 1) * SUM(x) <= 4", id="a-sum-in-the-factor"),
    ],
)
def test_bool_decision_factor_stays_on_a_bare_reducer(decidb_cli, clause):
    """The factor is one Boolean decision (under a sign and data) and the term a reducer: anything else is a
    product of decisions the value rewrite does not hold."""
    decidb_cli.assert_error(
        f"""
        SELECT id, x FROM {_BOOL_FACTOR_ROWS}
        DECIDE x(INT), per (): b(BOOL), per (): c(BOOL)
        SUCH THAT x <= 3 AND {clause}
        MAXIMIZE SUM(x)
    """,
        match=r"is a decision, so it cannot multiply",
    )


#: The objective holds the product term directly: the reducer's value is a helper decision and `b * helper` the one
#: query-wide product column, which the objective reads once. The push on the helper comes from the sense and the
#: term's sign. `SUM(x) <= 7` and `SUM(x) >= 2` bound the helper.
BOOL_FACTOR_OBJECTIVES = [
    pytest.param(
        "MAXIMIZE b * SUM(x) - 2 * b + SUM(x)",
        lambda a: sum(_xs(a)) * a["b"] - 2 * a["b"] + sum(_xs(a)),
        True,
        id="sum",
    ),
    pytest.param(
        "MINIMIZE 3 * b * SUM(x) - 9 * b",
        lambda a: 3 * a["b"] * sum(_xs(a)) - 9 * a["b"],
        False,
        id="a-data-factor-with-the-boolean",
    ),
    pytest.param(
        "MINIMIZE SUM(x) - 3 * (b * SUM(x)) + 10",
        lambda a: sum(_xs(a)) - 3 * a["b"] * sum(_xs(a)) + 10,
        False,
        id="a-negative-term-and-a-constant",
    ),
    pytest.param(
        "MAXIMIZE b * AVG(x) + SUM(x)",
        lambda a: a["b"] * sum(_xs(a)) / 3 + sum(_xs(a)),
        True,
        id="avg",
    ),
    pytest.param(
        "MINIMIZE b * MAX(x) - 4 * b + SUM(x)",
        lambda a: a["b"] * max(_xs(a)) - 4 * a["b"] + sum(_xs(a)),
        False,
        id="max",
    ),
    pytest.param(
        "MAXIMIZE b * MIN(x) + 2 * b",
        lambda a: a["b"] * min(_xs(a)) + 2 * a["b"],
        True,
        id="min",
    ),
    pytest.param(
        "MAXIMIZE when t.id > 1: b * SUM(x) - 3 * b",
        lambda a: a["b"] * (a["x1"] + a["x2"]) - 3 * a["b"],
        True,
        id="the-reducer-reads-the-kept-rows",
    ),
]


@pytest.mark.correctness
@pytest.mark.obj_maximize
@pytest.mark.parametrize("objective, value, maximize", BOOL_FACTOR_OBJECTIVES)
def test_bool_decision_factor_on_a_reducer_in_the_objective(
    decidb_cli_gurobi, decidb_cli_highs, objective, value, maximize
):
    """Each objective answers on both solvers with the oracle's optimum, judged against every assignment."""
    sql = f"""
        SELECT id, x, b FROM {_BOOL_FACTOR_ROWS}
        DECIDE x(INT), per (): b(BOOL)
        SUCH THAT x <= 3 AND SUM(x) <= 7 AND SUM(x) >= 2
        {objective}
        ORDER BY id"""
    domains = {**_X_DOMAIN, "b": range(2)}
    for cli in (decidb_cli_gurobi, decidb_cli_highs):
        judge_rows(answer(cli, sql), domains, lambda a: 2 <= sum(_xs(a)) <= 7, value, maximize)


@pytest.mark.correctness
@pytest.mark.obj_maximize
def test_only_the_reducer_under_a_boolean_gets_a_helper_in_the_objective(decidb_cli_highs, tmp_path):
    """`b * SUM(x) + SUM(x)`: the reducer under `b` is held by a helper (and the product with it is one column, read
    once by the objective); the plain `SUM(x)` is still the objective's own sum over `x`."""
    sql = f"""
        SELECT id, x, b FROM {_BOOL_FACTOR_ROWS}
        DECIDE x(INT), per (): b(BOOL)
        SUCH THAT x <= 3 AND SUM(x) <= 7 AND SUM(x) >= 2
        MAXIMIZE b * SUM(x) + SUM(x)"""
    dump = decidb_cli_highs.dump_model(sql, tmp_path / "model.dump")
    assert re.search(r"^num_vars: 6$", dump, re.M), dump  # x, b, the helper and the product
    assert re.findall(r"^col [0-2]: .* obj=(\S+)$", dump, re.M) == ["1", "1", "1"], dump
    assert re.findall(r"^col 5: .* obj=(\S+)$", dump, re.M) == ["1"], dump


#: A BOOL is in {0, 1}, never negative, so it keeps the direction of the term it scales: the helper that holds the
#: reducer is defined on the side the clause (or the sense) pushes it, `v >= sum` or `v <= sum`; only `=`, or a data
#: factor whose sign the rows decide, needs both.
BOOL_FACTOR_SIDES = [
    pytest.param("SUM(x) <= 7 AND b * SUM(x) <= 4", "MAXIMIZE SUM(x)", ">", id="pushed-down"),
    pytest.param("SUM(x) <= 7 AND b * SUM(x) >= 4", "MAXIMIZE SUM(x)", "<", id="pushed-up"),
    pytest.param("SUM(x) <= 7 AND -(b * SUM(x)) >= -4", "MAXIMIZE SUM(x)", ">", id="negated-flips-the-push"),
    pytest.param("SUM(x) <= 7 AND b * SUM(x) = 3", "MAXIMIZE SUM(x)", "=", id="equality-needs-both"),
    pytest.param(
        "SUM(x) <= 7 AND (SELECT 2 - 1) * b * SUM(x) <= 4", "MAXIMIZE SUM(x)", "=", id="a-factor-of-unknown-sign"
    ),
    pytest.param("SUM(x) <= 7 AND SUM(x) >= 2", "MAXIMIZE b * SUM(x)", "<", id="maximized"),
    pytest.param("SUM(x) <= 7 AND SUM(x) >= 2", "MINIMIZE b * SUM(x)", ">", id="minimized"),
]


@pytest.mark.correctness
@pytest.mark.parametrize("clause, objective, sense", BOOL_FACTOR_SIDES)
def test_a_bool_factor_leaves_the_helper_one_sided(decidb_cli_highs, tmp_path, clause, objective, sense):
    """The definition row of the helper (the one with the three `x` and the helper, four entries) is `>=`, `<=` or `=`
    by the push of the clause, and a BOOL factor does not hide the push."""
    sql = f"""
        SELECT id, x, b FROM {_BOOL_FACTOR_ROWS}
        DECIDE x(INT), per (): b(BOOL)
        SUCH THAT x <= 3 AND {clause}
        {objective}"""
    dump = decidb_cli_highs.dump_model(sql, tmp_path / "model.dump")
    assert re.findall(r"^row \d+: sense=(.) rhs=\S+ nnz=4 ", dump, re.M) == [sense], dump


@pytest.mark.obj_maximize
@pytest.mark.error_binder
def test_bool_decision_factor_per_row_in_the_objective_is_refused(decidb_cli):
    """The objective is produced once, so a decision outside a reducer needs one instance over the kept rows: a
    Boolean declared per row multiplying a reducer is refused as it is beside one, not counted once."""
    decidb_cli.assert_error(
        f"""
        SELECT id, x FROM {_BOOL_FACTOR_ROWS}
        DECIDE x(INT), c(BOOL)
        SUCH THAT x <= 3
        MAXIMIZE c * SUM(x)
    """,
        match=r"does not determine the decision c",
    )


@pytest.mark.obj_maximize
@pytest.mark.error_binder
def test_int_decision_factor_in_the_objective_stays_refused(decidb_cli):
    """Only a Boolean factor is lifted in the objective: an INT one is not convex."""
    decidb_cli.assert_error(
        f"""
        SELECT id, x FROM {_BOOL_FACTOR_ROWS}
        DECIDE x(INT), per (): s(INT)
        SUCH THAT x <= 3 AND s <= 3
        MAXIMIZE s * SUM(x)
    """,
        match=r"'s' is a decision, so it cannot multiply SUM\(x\)",
    )


@pytest.mark.cons_aggregate
@pytest.mark.error_binder
def test_bool_decision_factor_names_the_member_without_a_bound(decidb_cli):
    """`b * SUM(x)` needs the reducer's value bounded above, which its members give: with no bound on `x` the
    refusal names `x`, the column to bound, and not the helper that holds the sum."""
    decidb_cli.assert_error(
        f"""
        SELECT id, x FROM {_BOOL_FACTOR_ROWS}
        DECIDE x(INT), per (): b(BOOL)
        SUCH THAT b * SUM(x) <= 4
        MAXIMIZE SUM(x)
    """,
        match=r"finite upper bound on variable 'x'",
    )


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
    decidb_cli.assert_error(
        """
        SELECT id, x, s FROM (VALUES (1), (2)) t(id)
        DECIDE x(INT), per (): s(INT)
        SUCH THAT x <= 5 AND s <= 3 AND SUM(x) <= 4
        MAXIMIZE s * SUM(x)
    """,
        match=r"'s' is a decision, so it cannot multiply SUM\(x\)",
    )


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
