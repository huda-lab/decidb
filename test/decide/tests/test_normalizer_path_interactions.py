"""Cross-product integration tests for the constraint normalizer paths.

`NormalizeComparisonExpr` in `src/decidb/symbolic/decide_symbolic.cpp` has
four mutually-exclusive paths (see the architecture comment at the top of
that file): quadratic LHS bypass, bilinear LHS bypass, composed-MIN/MAX
LHS bypass, and the aggregate-local WHEN path. They are first-match-wins
on `if (...) return cmp.Copy()` early returns.

The bypass conditions aren't disjoint — practical queries can match
multiple. Today this is safe because each bypass is conservative (treats
unrecognized leaves as opaque structural terms) so it never *breaks* a
shape it doesn't fully understand. These tests pin that property down for
the practical combinations:

  - quadratic + WHEN
  - quadratic + WHEN + additive constant offset
  - bilinear + WHEN
  - bilinear + WHEN + additive constant offset
  - composed SUM/MIN with WHEN on the MIN term

If any of these regress (e.g. someone reorders the bypass checks, or
adds a fifth one whose behavior depends on running first), the failures
land here with concrete oracle-comparison diffs rather than as silent
wrong answers in user queries.
"""

import functools
import re
import time

import pytest

from decidb_cli import DecidBCliError
from solver.types import ObjSense, SolverStatus, VarType


def _expect_gurobi(func):
    """Decorator: run test, accept HiGHS rejection (`Gurobi`/`quadratic`
    in the error) as passing. Mirrors the pattern in
    `test_quadratic_constraints.py` so the suite runs against either
    backend without spurious failures."""
    @functools.wraps(func)
    def wrapper(*args, **kwargs):
        try:
            return func(*args, **kwargs)
        except DecidBCliError as e:
            assert re.search(r"[Qq]uadratic|[Gg]urobi|McCormick|bilinear|non-convex", str(e)), \
                f"Unexpected error (expected QP/bilinear backend rejection): {e}"
    return wrapper


def _solve_and_compare(oracle_solver, decidb_obj, tol=5e-2):
    """Solve the oracle, assert its objective matches DecidB's within tol.
    Looser tolerance than the linear suite because QP/QCQP solutions sit
    on the solver's feasibility boundary and can drift by ~1e-6 in
    constraint slack, which propagates into the objective."""
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL, (
        f"Oracle failed to solve: status={result.status}"
    )
    diff = abs(decidb_obj - result.objective_value)
    assert diff <= tol, (
        f"Objective mismatch: DecidB={decidb_obj:.6f}, "
        f"Oracle={result.objective_value:.6f}, diff={diff:.6f}"
    )
    return result


# ===========================================================================
# Quadratic LHS bypass + aggregate-local WHEN
# ===========================================================================
#
# Bypass-order question: WHEN check fires first (line ~997 in
# decide_symbolic.cpp), but the WHEN path's additive walk treats
# `SUM(POWER(x, 2)) WHEN c` as one opaque structural term — no constants
# to peel, no `K * WHEN` to fold. The rebuilt LHS is identical to the
# original. The QP detector downstream still recognizes the
# WHEN-tagged-quadratic-SUM shape and emits the right Q matrix.

@pytest.mark.quadratic
@pytest.mark.when
@pytest.mark.when_objective
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_quadratic_objective_with_when(decidb_cli, oracle_solver):
    """`MINIMIZE SUM(POWER(x - t, 2)) WHEN w` — quadratic objective with
    aggregate-local WHEN. Only w-true rows contribute to Q and linear
    coefficients; w-false rows are unconstrained."""
    sql = """
        SELECT id, t, ROUND(x, 4) AS x FROM (
            VALUES (1, 5.0, true), (2, 8.0, false), (3, 3.0, true)
        ) data(id, t, w)
        DECIDE x IS REAL
        SUCH THAT x >= 0 AND x <= 10
        MINIMIZE SUM(POWER(x - t, 2)) WHEN w
    """
    rows, cols = decidb_cli.execute(sql)
    data = [(1, 5.0, True), (2, 8.0, False), (3, 3.0, True)]

    oracle_solver.create_model("qp_when_obj")
    xnames = [f"x_{i}" for i in range(len(data))]
    for xn in xnames:
        oracle_solver.add_variable(xn, VarType.CONTINUOUS, lb=0.0, ub=10.0)
    linear, quadratic = {}, {}
    for i, row in enumerate(data):
        if row[2]:
            linear[xnames[i]] = -2.0 * row[1]
            quadratic[(xnames[i], xnames[i])] = 1.0
    oracle_solver.set_quadratic_objective(linear, quadratic, ObjSense.MINIMIZE)

    xi = cols.index("x"); ti = cols.index("t")
    decidb_obj = sum(
        ((float(r[xi]) - float(r[ti])) ** 2 - float(r[ti]) ** 2)
        for r in rows if r[cols.index("id")] in (1, 3)  # only w-true rows
    )
    _solve_and_compare(oracle_solver, decidb_obj)


@pytest.mark.quadratic
@pytest.mark.when
@pytest.mark.when_constraint
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
@_expect_gurobi
def test_quadratic_constraint_with_when_and_constant_offset(decidb_cli, oracle_solver):
    """`(SUM(POWER(x, 2)) WHEN w) + 3 <= K` — quadratic constraint with
    WHEN AND additive constant. The WHEN path peels `+3` to the RHS;
    the rebuilt LHS is still a recognizable WHEN-tagged quadratic SUM
    that the QCQP pipeline (Gurobi only) handles."""
    sql = """
        SELECT id, x FROM (
            VALUES (1, true), (2, false), (3, true)
        ) data(id, w)
        DECIDE x IS REAL
        SUCH THAT (SUM(POWER(x, 2)) WHEN w) + 3 <= 50
            AND x <= 10
        MAXIMIZE SUM(x)
    """
    rows, cols = decidb_cli.execute(sql)
    data = [(1, True), (2, False), (3, True)]

    oracle_solver.create_model("qcqp_when_offset")
    xnames = [f"x_{i}" for i in range(len(data))]
    for xn in xnames:
        oracle_solver.add_variable(xn, VarType.CONTINUOUS, lb=0.0, ub=10.0)
    # SUM_{w-true}(x_i^2) + 3 <= 50  →  SUM <= 47
    linear: dict = {}
    quadratic = {(xnames[i], xnames[i]): 1.0 for i in range(len(data)) if data[i][1]}
    oracle_solver.add_quadratic_constraint(linear, quadratic, "<=", 47.0, name="qp_off")
    oracle_solver.set_objective({xn: 1.0 for xn in xnames}, ObjSense.MAXIMIZE)

    xi = cols.index("x")
    decidb_obj = sum(float(r[xi]) for r in rows)
    _solve_and_compare(oracle_solver, decidb_obj, tol=1e-2)


# ===========================================================================
# Bilinear LHS bypass + aggregate-local WHEN
# ===========================================================================
#
# Same bypass-order story: WHEN path runs first, treats `SUM(x*y) WHEN c`
# as opaque, returns the LHS unchanged when there's nothing to peel. The
# bilinear pipeline downstream still recognises the McCormick-linearizable
# `Bool * Real` shape inside the WHEN-tagged SUM.

@pytest.mark.bilinear
@pytest.mark.when
@pytest.mark.when_constraint
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_bilinear_constraint_with_when(decidb_cli, oracle_solver):
    """`SUM(x * y) WHEN w <= K` — bilinear (Bool * Real) constraint with
    aggregate-local WHEN. McCormick reformulation must apply only to
    w-true rows; w-false rows are unconstrained."""
    sql = """
        SELECT id, x, y FROM (
            VALUES (1, true), (2, false), (3, true)
        ) data(id, w)
        DECIDE x IS BOOLEAN, y IS REAL
        SUCH THAT SUM(x * y) WHEN w <= 8
            AND y <= 5
        MAXIMIZE SUM(x * y)
    """
    rows, cols = decidb_cli.execute(sql)
    data = [(1, True), (2, False), (3, True)]

    oracle_solver.create_model("bilinear_when")
    n = len(data)
    xnames = [f"x_{i}" for i in range(n)]
    ynames = [f"y_{i}" for i in range(n)]
    pnames = [f"p_{i}" for i in range(n)]  # auxiliary z_i = x_i * y_i
    for xn in xnames:
        oracle_solver.add_variable(xn, VarType.BINARY)
    for yn in ynames:
        oracle_solver.add_variable(yn, VarType.CONTINUOUS, lb=0.0, ub=5.0)
    for pn in pnames:
        oracle_solver.add_variable(pn, VarType.CONTINUOUS, lb=0.0, ub=5.0)
    # McCormick envelope for z_i = x_i * y_i with x∈{0,1}, y∈[0,5]:
    #   z_i <= 5 * x_i,  z_i <= y_i,  z_i >= y_i - 5*(1 - x_i)
    for i in range(n):
        oracle_solver.add_constraint({pnames[i]: 1.0, xnames[i]: -5.0}, "<=", 0.0,
                                      name=f"mc_a_{i}")
        oracle_solver.add_constraint({pnames[i]: 1.0, ynames[i]: -1.0}, "<=", 0.0,
                                      name=f"mc_b_{i}")
        oracle_solver.add_constraint({pnames[i]: 1.0, ynames[i]: -1.0, xnames[i]: -5.0},
                                      ">=", -5.0, name=f"mc_c_{i}")
    # WHEN-filtered: SUM_{w-true}(z_i) <= 8.
    oracle_solver.add_constraint(
        {pnames[i]: 1.0 for i in range(n) if data[i][1]},
        "<=", 8.0, name="bilinear_cap",
    )
    # Objective: MAXIMIZE SUM(z_i) — all rows contribute (the WHEN is
    # only on the constraint, not the objective).
    oracle_solver.set_objective({pn: 1.0 for pn in pnames}, ObjSense.MAXIMIZE)

    xi = cols.index("x"); yi = cols.index("y")
    decidb_obj = sum(float(r[xi]) * float(r[yi]) for r in rows)
    _solve_and_compare(oracle_solver, decidb_obj, tol=1e-3)


@pytest.mark.bilinear
@pytest.mark.when
@pytest.mark.when_constraint
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_bilinear_constraint_with_when_and_constant_offset(decidb_cli, oracle_solver):
    """`(SUM(x * y) WHEN w) + 3 <= K` — bilinear + WHEN + constant offset.
    Verifies the WHEN-path additive walk peels the `+3` cleanly without
    disturbing the bilinear shape inside the SUM body."""
    sql = """
        SELECT id, x, y FROM (
            VALUES (1, true), (2, false), (3, true)
        ) data(id, w)
        DECIDE x IS BOOLEAN, y IS REAL
        SUCH THAT (SUM(x * y) WHEN w) + 3 <= 11
            AND y <= 5
        MAXIMIZE SUM(x * y)
    """
    rows, cols = decidb_cli.execute(sql)
    data = [(1, True), (2, False), (3, True)]

    # Same model as above; just RHS shifts from 8 to 11-3 = 8 (identical
    # constraint after peel).
    oracle_solver.create_model("bilinear_when_offset")
    n = len(data)
    xnames = [f"x_{i}" for i in range(n)]
    ynames = [f"y_{i}" for i in range(n)]
    pnames = [f"p_{i}" for i in range(n)]
    for xn in xnames:
        oracle_solver.add_variable(xn, VarType.BINARY)
    for yn in ynames:
        oracle_solver.add_variable(yn, VarType.CONTINUOUS, lb=0.0, ub=5.0)
    for pn in pnames:
        oracle_solver.add_variable(pn, VarType.CONTINUOUS, lb=0.0, ub=5.0)
    for i in range(n):
        oracle_solver.add_constraint({pnames[i]: 1.0, xnames[i]: -5.0}, "<=", 0.0,
                                      name=f"mc_a_{i}")
        oracle_solver.add_constraint({pnames[i]: 1.0, ynames[i]: -1.0}, "<=", 0.0,
                                      name=f"mc_b_{i}")
        oracle_solver.add_constraint({pnames[i]: 1.0, ynames[i]: -1.0, xnames[i]: -5.0},
                                      ">=", -5.0, name=f"mc_c_{i}")
    oracle_solver.add_constraint(
        {pnames[i]: 1.0 for i in range(n) if data[i][1]},
        "<=", 8.0, name="bilinear_cap_after_peel",
    )
    oracle_solver.set_objective({pn: 1.0 for pn in pnames}, ObjSense.MAXIMIZE)

    xi = cols.index("x"); yi = cols.index("y")
    decidb_obj = sum(float(r[xi]) * float(r[yi]) for r in rows)
    _solve_and_compare(oracle_solver, decidb_obj, tol=1e-3)


# ===========================================================================
# Composed MIN/MAX bypass + aggregate-local WHEN
# ===========================================================================
#
# This combination is the trickiest because the WHEN path runs first
# (line ~997) but the composed MIN/MAX walker is what actually handles
# `SUM + MIN`/`SUM + MAX` shapes. The WHEN path's additive walk doesn't
# peel constants here (there are none) and treats the WHEN-tagged MIN as
# opaque, so the rebuilt LHS is structurally identical to the original.
# The downstream optimizer pass (`RewriteComposedMinMaxInConstraint`)
# then runs as usual on the (effectively unchanged) expression.

@pytest.mark.min_max
@pytest.mark.when
@pytest.mark.when_constraint
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_composed_sum_plus_min_with_when(decidb_cli, oracle_solver):
    """`SUM(x*v) + (MIN(x*v) WHEN w) >= K` — composed MIN with the MIN
    term WHEN-filtered. The MIN here is in the easy direction (MIN >= K
    is enforced by per-row x*v >= K for w-true rows, no Big-M needed)."""
    sql = """
        SELECT id, x FROM (
            VALUES (1, 10.0, true), (2, 5.0, true), (3, 7.0, false)
        ) data(id, v, w)
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * v) + (MIN(x * v) WHEN w) >= 5
        MAXIMIZE SUM(x * v)
    """
    rows, cols = decidb_cli.execute(sql)
    data = [(1, 10.0, True), (2, 5.0, True), (3, 7.0, False)]

    # Hand-encoded composed-MIN: introduce z = MIN_{w-true rows}(x_i * v_i),
    # add per-row links z <= x_i * v_i for each w-true row (easy direction
    # of MIN >= K), then constraint SUM(x*v) + z >= 5.
    #
    # Since x is binary and v is constant per row, x_i * v_i takes values
    # in {0, v_i}; the MIN over w-true rows is min(x_1 * 10, x_2 * 5).
    # The per-row links `z <= x_i * v_i` plus `SUM + z >= 5` give the
    # composed-MIN >= K formulation directly.
    oracle_solver.create_model("composed_min_when")
    n = len(data)
    xnames = [f"x_{i}" for i in range(n)]
    for xn in xnames:
        oracle_solver.add_variable(xn, VarType.BINARY)
    # z bounded by [0, max_w_true v_i]
    z_ub = max(row[1] for row in data if row[2])
    oracle_solver.add_variable("z", VarType.CONTINUOUS, lb=0.0, ub=z_ub)
    # Easy MIN >= K: z <= x_i * v_i for each w-true row.
    for i, row in enumerate(data):
        if row[2]:
            oracle_solver.add_constraint(
                {"z": 1.0, xnames[i]: -row[1]}, "<=", 0.0, name=f"min_link_{i}",
            )
    # Composed: SUM_{all rows}(x_i * v_i) + z >= 5
    cstr = {xnames[i]: data[i][1] for i in range(n)}
    cstr["z"] = 1.0
    oracle_solver.add_constraint(cstr, ">=", 5.0, name="composed_lower")
    # Objective: MAXIMIZE SUM(x*v)
    oracle_solver.set_objective(
        {xnames[i]: data[i][1] for i in range(n)}, ObjSense.MAXIMIZE,
    )

    xi = cols.index("x")
    # Reuse SUM(x*v) from decidb result rows (v is a SELECT-list column? It's
    # not selected here — recover from data by id).
    id_to_v = {row[0]: row[1] for row in data}
    id_idx = cols.index("id")
    decidb_obj = sum(float(r[xi]) * id_to_v[r[id_idx]] for r in rows)
    _solve_and_compare(oracle_solver, decidb_obj, tol=1e-3)
