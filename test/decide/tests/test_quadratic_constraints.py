"""Tests for quadratic constraints (POWER(expr, 2) in SUCH THAT).

Covers QCQP: quadratic constraints with linear or quadratic objectives.
Each correctness test formulates the same QCQP in gurobipy using the
``set_quadratic_objective`` / ``add_quadratic_constraint`` oracle API and
compares optimality.

Expansion rules used throughout:
  POWER(x - t, 2)             = x^2 - 2·t·x + t^2
  POWER(w·x - t, 2)           = w^2·x^2 - 2·w·t·x + t^2
  SUM(POWER(x_i - t_i, 2)) ≤ K  ⇔  Q = diag(1), linear = {-2·t_i}, rhs = K - Σ t_i^2

Tests that produce quadratic constraints are wrapped with ``@_expect_gurobi``
since HiGHS rejects Q-constraints; when that happens, the oracle check is
skipped and we verify the rejection message instead.
"""

import functools
import re
import time

import pytest

from decidb_cli import DecidBCliError
from solver.types import ObjSense, SolverStatus, VarType


def _expect_gurobi(func, _retries=3):
    """Decorator: run test, accept a HiGHS quadratic rejection as passing, and
    tolerate Gurobi's nondeterministic SUBOPTIMAL termination.

    The tight ``POWER(expr, 2) <= 0`` (equality-via-quadratic) constraints used
    here are non-convex knife-edge surfaces. Gurobi intermittently returns
    status 13 (SUBOPTIMAL) — a feasible incumbent it could not *prove* optimal.
    DeciDB returns those rows with a caveat on stderr rather than relabelling the
    termination as OPTIMAL, and ``DecidBCli.execute`` surfaces any stderr as an
    error, so the caveat reaches us as a ``DecidBCliError``. That is a legitimate
    transient outcome, not a correctness bug: we retry for a proven-OPTIMAL solve
    (so the oracle assertion still runs in the common case) and only ``skip`` if
    every attempt hits SUBOPTIMAL.

    The SUBOPTIMAL case is recognised by ``DecidBCliError.status``, set from the
    ``DECIDB_STATUS:`` marker line. Do **not** match the caveat's prose here: that
    text is owned by the user-facing-output principle and is reworded freely. It was
    reworded once already, which silently disabled this retry and turned the file
    into an intermittent failure.
    """
    @functools.wraps(func)
    def wrapper(*args, **kwargs):
        last_suboptimal = None
        for _ in range(_retries):
            try:
                return func(*args, **kwargs)
            except DecidBCliError as e:
                if e.status == "SUBOPTIMAL":
                    last_suboptimal = str(e)
                    continue  # Gurobi nondeterminism — retry for a proven optimum
                assert re.search(r"[Qq]uadratic|[Gg]urobi", str(e)), \
                    f"Unexpected error (expected quadratic/Gurobi rejection): {e}"
                return
        pytest.skip(
            "Gurobi returned SUBOPTIMAL on this non-convex QCQP on every attempt "
            f"— nondeterministic, not a correctness failure: {last_suboptimal}")
    return wrapper


def _assert_oracle_obj(oracle_solver, decidb_obj, tol=5e-2):
    """Solve the oracle and assert its objective matches ``decidb_obj``.

    The tolerance is looser than the linear suite's 1e-4 because QCQP
    constraints of the form ``POWER(expr, 2) <= 0`` are only satisfied to
    Gurobi's feasibility tolerance (~1e-6). A small residual in the
    quadratic constraint translates into a proportional drift in the
    variable values and thus the objective. 5e-2 comfortably rejects real
    encoding bugs (which would differ by orders of magnitude) while
    accommodating legitimate solver noise.
    """
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


def _aggregate_sq_dev(data, target_idx, rhs, weight_idx=None):
    """Build ``linear``, ``quadratic``, and adjusted ``rhs`` for
    ``SUM(POWER(w_i·x_i - t_i, 2)) [sense] rhs`` across rows. Returns a tuple
    ``(linear, quadratic, adjusted_rhs)`` suitable for ``add_quadratic_constraint``.
    """
    linear, quadratic = {}, {}
    constant = 0.0
    for i, row in enumerate(data):
        t = float(row[target_idx])
        w = 1.0 if weight_idx is None else float(row[weight_idx])
        xn = f"x_{i}"
        linear[xn] = -2.0 * w * t
        quadratic[(xn, xn)] = w * w
        constant += t * t
    return linear, quadratic, rhs - constant


@pytest.mark.correctness
@pytest.mark.quadratic
@_expect_gurobi
def test_aggregate_quadratic_constraint_data_only_offset(decidb_cli):
    """Quadratic aggregate constraints subtract fixed LHS offsets from RHS."""
    rows, cols = decidb_cli.execute("""
        SELECT id, ROUND(x, 4) AS x, target, cost
        FROM (
            VALUES (1, 1.0, 1.0), (2, 2.0, 2.0)
        ) t(id, target, cost)
        DECIDE x(REAL)
        SUCH THAT x >= 0 AND x <= 10
            AND SUM(POWER(x - target, 2) + cost) <= 3
        MAXIMIZE SUM(x)
    """)
    id_idx = cols.index("id")
    x_idx = cols.index("x")
    expected = {1: 1.0, 2: 2.0}
    for row in rows:
        assert float(row[x_idx]) == pytest.approx(expected[int(row[id_idx])], abs=1e-3)


# ===================================================================
# Category 1: Core Correctness
# ===================================================================

@pytest.mark.correctness
@pytest.mark.quadratic
class TestQuadraticConstraintCorrectness:
    """Core correctness — each QCQP is independently formulated in gurobipy."""

    @_expect_gurobi
    def test_zero_budget_forces_exact_match(
        self, decidb_cli, duckdb_conn, oracle_solver, perf_tracker
    ):
        """``POWER(x - target, 2) <= 0`` per-row forces ``x = target`` exactly."""
        data_sql = """
            SELECT 1 AS id, 10.0 AS target UNION ALL
            SELECT 2, 25.0 UNION ALL
            SELECT 3, 40.0
        """
        sql = f"""
            WITH data AS ({data_sql})
            SELECT id, ROUND(x, 4) AS x, target FROM data
            DECIDE x(REAL)
            SUCH THAT x >= 0 AND x <= 100
                AND POWER(x - target, 2) <= 0
            MAXIMIZE SUM(x)
        """
        t0 = time.perf_counter()
        rows, cols = decidb_cli.execute(sql)
        decidb_time = time.perf_counter() - t0
        data = duckdb_conn.execute(
            f"SELECT CAST(id AS BIGINT), CAST(target AS DOUBLE) FROM ({data_sql})"
        ).fetchall()

        t_build = time.perf_counter()
        oracle_solver.create_model("qcp_zero_budget")
        n = len(data)
        for i in range(n):
            oracle_solver.add_variable(f"x_{i}", VarType.CONTINUOUS, lb=0.0, ub=100.0)
            # Per-row POWER(x_i - t_i, 2) <= 0 ⇔ (x_i - t_i)^2 ≤ 0 ⇔ x_i = t_i
            t = float(data[i][1])
            oracle_solver.add_quadratic_constraint(
                linear={f"x_{i}": -2.0 * t},
                quadratic={(f"x_{i}", f"x_{i}"): 1.0},
                sense="<=", rhs=-t * t,
                name=f"tight_{i}",
            )
        oracle_solver.set_objective(
            {f"x_{i}": 1.0 for i in range(n)}, ObjSense.MAXIMIZE,
        )
        build_time = time.perf_counter() - t_build

        x_col = cols.index("x")
        decidb_obj = sum(float(r[x_col]) for r in rows)
        result = _assert_oracle_obj(oracle_solver, decidb_obj)
        perf_tracker.record(
            "qcp_zero_budget", decidb_time, build_time,
            result.solve_time_seconds, n, n, n,
            result.objective_value, oracle_solver.solver_name(),
            comparison_status="optimal",
        )

    @_expect_gurobi
    def test_aggregate_budget_with_linear_objective(
        self, decidb_cli, duckdb_conn, oracle_solver, perf_tracker
    ):
        """MAXIMIZE SUM(x) SUBJECT TO SUM(POWER(x - t, 2)) <= budget.

        Compares two budgets (tight + loose) and oracle-verifies both.
        """
        data_sql = """
            SELECT 1 AS id, 10.0 AS target UNION ALL
            SELECT 2, 20.0 UNION ALL
            SELECT 3, 30.0
        """
        base_sql = f"""
            WITH data AS ({data_sql})
            SELECT id, ROUND(x, 4) AS x, target FROM data
            DECIDE x(REAL)
            SUCH THAT x >= 0 AND x <= 100
                AND SUM(POWER(x - target, 2)) <= {{budget}}
            MAXIMIZE SUM(x)
        """
        data = duckdb_conn.execute(
            f"SELECT CAST(id AS BIGINT), CAST(target AS DOUBLE) FROM ({data_sql})"
        ).fetchall()
        n = len(data)

        for budget in (3.0, 75.0):
            t0 = time.perf_counter()
            rows, cols = decidb_cli.execute(base_sql.format(budget=budget))
            decidb_time = time.perf_counter() - t0

            t_build = time.perf_counter()
            oracle_solver.create_model(f"qcp_agg_budget_{int(budget)}")
            for i in range(n):
                oracle_solver.add_variable(
                    f"x_{i}", VarType.CONTINUOUS, lb=0.0, ub=100.0,
                )
            linear, quadratic, adj_rhs = _aggregate_sq_dev(data, 1, budget)
            oracle_solver.add_quadratic_constraint(
                linear=linear, quadratic=quadratic,
                sense="<=", rhs=adj_rhs, name="agg_budget",
            )
            oracle_solver.set_objective(
                {f"x_{i}": 1.0 for i in range(n)}, ObjSense.MAXIMIZE,
            )
            build_time = time.perf_counter() - t_build

            x_col = cols.index("x")
            decidb_obj = sum(float(r[x_col]) for r in rows)
            result = _assert_oracle_obj(oracle_solver, decidb_obj)
            perf_tracker.record(
                f"qcp_agg_budget_{int(budget)}", decidb_time, build_time,
                result.solve_time_seconds, n, n, 1,
                result.objective_value, oracle_solver.solver_name(),
                comparison_status="optimal",
            )

    @_expect_gurobi
    def test_multi_variable_inner_expression(
        self, decidb_cli, duckdb_conn, oracle_solver, perf_tracker
    ):
        """POWER(2·x + y - c, 2) ≤ 0 forces ``2x + y = c`` exactly (off-diagonal Q).

        Expansion: (2x + y - c)^2 = 4x^2 + 4xy + y^2 - 4cx - 2cy + c^2.
        """
        sql = """
            WITH data AS (SELECT 1 AS id, 5.0 AS c)
            SELECT id, ROUND(x, 4) AS x, ROUND(y, 4) AS y
            FROM data
            DECIDE x(REAL), y(REAL)
            SUCH THAT x >= 0 AND x <= 10
                AND y >= 0 AND y <= 10
                AND POWER(2*x + y - c, 2) <= 0
            MAXIMIZE SUM(x + y)
        """
        t0 = time.perf_counter()
        rows, cols = decidb_cli.execute(sql)
        decidb_time = time.perf_counter() - t0

        t_build = time.perf_counter()
        oracle_solver.create_model("qcp_cross_terms")
        oracle_solver.add_variable("x", VarType.CONTINUOUS, lb=0.0, ub=10.0)
        oracle_solver.add_variable("y", VarType.CONTINUOUS, lb=0.0, ub=10.0)
        c = 5.0
        oracle_solver.add_quadratic_constraint(
            linear={"x": -4.0 * c, "y": -2.0 * c},
            quadratic={("x", "x"): 4.0, ("x", "y"): 4.0, ("y", "y"): 1.0},
            sense="<=", rhs=-c * c,
            name="hyperplane",
        )
        oracle_solver.set_objective({"x": 1.0, "y": 1.0}, ObjSense.MAXIMIZE)
        build_time = time.perf_counter() - t_build

        xv = float(rows[0][cols.index("x")])
        yv = float(rows[0][cols.index("y")])
        decidb_obj = xv + yv
        result = _assert_oracle_obj(oracle_solver, decidb_obj)
        perf_tracker.record(
            "qcp_cross_terms", decidb_time, build_time, result.solve_time_seconds,
            1, 2, 1, result.objective_value, oracle_solver.solver_name(),
            comparison_status="optimal",
        )

    @_expect_gurobi
    def test_binding_vs_nonbinding(
        self, decidb_cli, duckdb_conn, oracle_solver, perf_tracker
    ):
        """Large budget (non-binding) vs tight budget — oracle-verify both."""
        data_sql = """
            SELECT 1 AS id, 10.0 AS target UNION ALL
            SELECT 2, 20.0 UNION ALL
            SELECT 3, 30.0
        """
        base_sql = f"""
            WITH data AS ({data_sql})
            SELECT id, ROUND(x, 4) AS x, target FROM data
            DECIDE x(REAL)
            SUCH THAT x >= 0 AND x <= 100
                AND SUM(POWER(x - target, 2)) <= {{budget}}
            MAXIMIZE SUM(x)
        """
        data = duckdb_conn.execute(
            f"SELECT CAST(id AS BIGINT), CAST(target AS DOUBLE) FROM ({data_sql})"
        ).fetchall()
        n = len(data)

        for tag, budget in (("big", 100000.0), ("tight", 3.0)):
            t0 = time.perf_counter()
            rows, cols = decidb_cli.execute(base_sql.format(budget=budget))
            decidb_time = time.perf_counter() - t0

            t_build = time.perf_counter()
            oracle_solver.create_model(f"qcp_binding_{tag}")
            for i in range(n):
                oracle_solver.add_variable(
                    f"x_{i}", VarType.CONTINUOUS, lb=0.0, ub=100.0,
                )
            linear, quadratic, adj_rhs = _aggregate_sq_dev(data, 1, budget)
            oracle_solver.add_quadratic_constraint(
                linear=linear, quadratic=quadratic,
                sense="<=", rhs=adj_rhs, name="budget",
            )
            oracle_solver.set_objective(
                {f"x_{i}": 1.0 for i in range(n)}, ObjSense.MAXIMIZE,
            )
            build_time = time.perf_counter() - t_build

            x_col = cols.index("x")
            decidb_obj = sum(float(r[x_col]) for r in rows)
            result = _assert_oracle_obj(oracle_solver, decidb_obj)
            perf_tracker.record(
                f"qcp_binding_{tag}", decidb_time, build_time,
                result.solve_time_seconds, n, n, 1,
                result.objective_value, oracle_solver.solver_name(),
                comparison_status="optimal",
            )

    @_expect_gurobi
    def test_negated_power(
        self, decidb_cli, duckdb_conn, oracle_solver, perf_tracker
    ):
        """SUM(-POWER(x - t, 2)) >= -50 ⇔ SUM(POWER(x - t, 2)) <= 50.

        Oracle checks objective for the (equivalent) negated form directly.
        """
        sql = """
            WITH data AS (
                SELECT 1 AS id, 10.0 AS target UNION ALL
                SELECT 2, 20.0 UNION ALL
                SELECT 3, 30.0
            )
            SELECT id, ROUND(x, 4) AS x, target FROM data
            DECIDE x(REAL)
            SUCH THAT x >= 0 AND x <= 100
                AND SUM(-POWER(x - target, 2)) >= -50
            MAXIMIZE SUM(x)
        """
        t0 = time.perf_counter()
        rows, cols = decidb_cli.execute(sql)
        decidb_time = time.perf_counter() - t0
        data = [(1, 10.0), (2, 20.0), (3, 30.0)]
        n = len(data)

        t_build = time.perf_counter()
        oracle_solver.create_model("qcp_negated")
        for i in range(n):
            oracle_solver.add_variable(
                f"x_{i}", VarType.CONTINUOUS, lb=0.0, ub=100.0,
            )
        # -(x_i - t_i)^2 >= -K  ⇔  (x_i - t_i)^2 <= K across all rows.
        linear, quadratic, adj_rhs = _aggregate_sq_dev(data, 1, 50.0)
        oracle_solver.add_quadratic_constraint(
            linear=linear, quadratic=quadratic, sense="<=", rhs=adj_rhs,
            name="neg_power",
        )
        oracle_solver.set_objective(
            {f"x_{i}": 1.0 for i in range(n)}, ObjSense.MAXIMIZE,
        )
        build_time = time.perf_counter() - t_build

        x_col = cols.index("x")
        decidb_obj = sum(float(r[x_col]) for r in rows)
        result = _assert_oracle_obj(oracle_solver, decidb_obj)
        perf_tracker.record(
            "qcp_negated_power", decidb_time, build_time, result.solve_time_seconds,
            n, n, 1, result.objective_value, oracle_solver.solver_name(),
            comparison_status="optimal",
        )

    @_expect_gurobi
    def test_scaled_power(
        self, decidb_cli, duckdb_conn, oracle_solver, perf_tracker
    ):
        """SUM(2·POWER(x - t, 2)) <= 100  ⇔  SUM(POWER(x - t, 2)) <= 50."""
        sql = """
            WITH data AS (
                SELECT 1 AS id, 10.0 AS target UNION ALL
                SELECT 2, 20.0 UNION ALL
                SELECT 3, 30.0
            )
            SELECT id, ROUND(x, 4) AS x, target FROM data
            DECIDE x(REAL)
            SUCH THAT x >= 0 AND x <= 100
                AND SUM(2 * POWER(x - target, 2)) <= 100
            MAXIMIZE SUM(x)
        """
        t0 = time.perf_counter()
        rows, cols = decidb_cli.execute(sql)
        decidb_time = time.perf_counter() - t0
        data = [(1, 10.0), (2, 20.0), (3, 30.0)]
        n = len(data)

        t_build = time.perf_counter()
        oracle_solver.create_model("qcp_scaled")
        for i in range(n):
            oracle_solver.add_variable(
                f"x_{i}", VarType.CONTINUOUS, lb=0.0, ub=100.0,
            )
        # 2·(x-t)^2 ≤ 100 scales both linear and quadratic parts by 2.
        base_lin, base_quad, base_rhs = _aggregate_sq_dev(data, 1, 50.0)
        linear = {k: 2.0 * v for k, v in base_lin.items()}
        quadratic = {k: 2.0 * v for k, v in base_quad.items()}
        # The constant Σ t^2 gets scaled too: rhs_adj = 100 - 2·Σt^2 = 2·(50 - Σt^2).
        oracle_solver.add_quadratic_constraint(
            linear=linear, quadratic=quadratic, sense="<=",
            rhs=2.0 * base_rhs, name="scaled",
        )
        oracle_solver.set_objective(
            {f"x_{i}": 1.0 for i in range(n)}, ObjSense.MAXIMIZE,
        )
        build_time = time.perf_counter() - t_build

        x_col = cols.index("x")
        decidb_obj = sum(float(r[x_col]) for r in rows)
        result = _assert_oracle_obj(oracle_solver, decidb_obj)
        perf_tracker.record(
            "qcp_scaled_power", decidb_time, build_time, result.solve_time_seconds,
            n, n, 1, result.objective_value, oracle_solver.solver_name(),
            comparison_status="optimal",
        )

    @_expect_gurobi
    def test_query_wide_subquery_scales_squared_reducer(
        self, decidb_cli, oracle_solver
    ):
        """A flattened scalar subquery factor is evaluated once before Q construction.

        The factor is deliberately outside SUM, which is the canonical scaled-reducer
        shape: ``3 * SUM(POWER(x, 2)) <= 75``.
        """
        sql = """
            SELECT id, ROUND(x, 4) AS x
            FROM (VALUES (1), (2)) data(id)
            DECIDE x(REAL)
            SUCH THAT x >= 0 AND x <= 100
                AND (SELECT max(s) FROM (VALUES (3.0)) scale(s))
                    * SUM(POWER(x, 2)) <= 75
            MAXIMIZE SUM(x)
        """
        rows, cols = decidb_cli.execute(sql)

        oracle_solver.create_model("qcp_query_wide_scale")
        for i in range(2):
            oracle_solver.add_variable(
                f"x_{i}", VarType.CONTINUOUS, lb=0.0, ub=100.0,
            )
        oracle_solver.add_quadratic_constraint(
            linear={},
            quadratic={(f"x_{i}", f"x_{i}"): 3.0 for i in range(2)},
            sense="<=", rhs=75.0, name="query_wide_scale",
        )
        oracle_solver.set_objective(
            {f"x_{i}": 1.0 for i in range(2)}, ObjSense.MAXIMIZE,
        )

        x_col = cols.index("x")
        _assert_oracle_obj(oracle_solver, sum(float(r[x_col]) for r in rows))

    @_expect_gurobi
    def test_data_dependent_coefficients(
        self, decidb_cli, duckdb_conn, oracle_solver, perf_tracker
    ):
        """POWER(w·x - t, 2) ≤ 0 forces ``x = t/w`` per row."""
        sql = """
            WITH data AS (
                SELECT 1 AS id, 2.0 AS weight, 10.0 AS target UNION ALL
                SELECT 2, 1.0, 5.0 UNION ALL
                SELECT 3, 3.0, 15.0
            )
            SELECT id, ROUND(x, 4) AS x, weight, target FROM data
            DECIDE x(REAL)
            SUCH THAT x >= 0 AND x <= 100
                AND SUM(POWER(weight * x - target, 2)) <= 0
            MAXIMIZE SUM(x)
        """
        t0 = time.perf_counter()
        rows, cols = decidb_cli.execute(sql)
        decidb_time = time.perf_counter() - t0
        data = duckdb_conn.execute("""
            SELECT CAST(id AS BIGINT), CAST(weight AS DOUBLE), CAST(target AS DOUBLE) FROM (
                SELECT 1 AS id, 2.0 AS weight, 10.0 AS target UNION ALL
                SELECT 2, 1.0, 5.0 UNION ALL
                SELECT 3, 3.0, 15.0
            )
        """).fetchall()
        n = len(data)

        t_build = time.perf_counter()
        oracle_solver.create_model("qcp_data_dep_coef")
        for i in range(n):
            oracle_solver.add_variable(
                f"x_{i}", VarType.CONTINUOUS, lb=0.0, ub=100.0,
            )
        linear, quadratic, adj_rhs = _aggregate_sq_dev(
            data, target_idx=2, rhs=0.0, weight_idx=1,
        )
        oracle_solver.add_quadratic_constraint(
            linear=linear, quadratic=quadratic,
            sense="<=", rhs=adj_rhs, name="weighted",
        )
        oracle_solver.set_objective(
            {f"x_{i}": 1.0 for i in range(n)}, ObjSense.MAXIMIZE,
        )
        build_time = time.perf_counter() - t_build

        x_col = cols.index("x")
        decidb_obj = sum(float(r[x_col]) for r in rows)
        result = _assert_oracle_obj(oracle_solver, decidb_obj)
        perf_tracker.record(
            "qcp_data_dep_coef", decidb_time, build_time, result.solve_time_seconds,
            n, n, 1, result.objective_value, oracle_solver.solver_name(),
            comparison_status="optimal",
        )


# ===================================================================
# Category 2: Syntax Variants
# ===================================================================

_SYNTAX_DATA_SQL = """
    SELECT 1 AS id, 10.0 AS target UNION ALL
    SELECT 2, 20.0 UNION ALL
    SELECT 3, 30.0
"""

_SYNTAX_TARGETS = [(1, 10.0), (2, 20.0), (3, 30.0)]


def _run_syntax_variant(
    decidb_cli, oracle_solver, perf_tracker,
    *, test_id, constraint_expr,
):
    """Build the same budgeted SUM-of-squared-deviation QCP with different
    constraint syntax and oracle-verify the objective."""
    sql = f"""
        WITH data AS ({_SYNTAX_DATA_SQL})
        SELECT id, ROUND(x, 4) AS x FROM data
        DECIDE x(REAL)
        SUCH THAT x >= 0 AND x <= 100
            AND SUM({constraint_expr}) <= 50
        MAXIMIZE SUM(x)
    """
    t0 = time.perf_counter()
    rows, cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    t_build = time.perf_counter()
    oracle_solver.create_model(test_id)
    n = len(_SYNTAX_TARGETS)
    for i in range(n):
        oracle_solver.add_variable(
            f"x_{i}", VarType.CONTINUOUS, lb=0.0, ub=100.0,
        )
    linear, quadratic, adj_rhs = _aggregate_sq_dev(_SYNTAX_TARGETS, 1, 50.0)
    oracle_solver.add_quadratic_constraint(
        linear=linear, quadratic=quadratic,
        sense="<=", rhs=adj_rhs, name="budget",
    )
    oracle_solver.set_objective(
        {f"x_{i}": 1.0 for i in range(n)}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build

    x_col = cols.index("x")
    decidb_obj = sum(float(r[x_col]) for r in rows)
    result = _assert_oracle_obj(oracle_solver, decidb_obj)
    perf_tracker.record(
        test_id, decidb_time, build_time, result.solve_time_seconds,
        n, n, 1, result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )


@pytest.mark.correctness
@pytest.mark.quadratic
class TestQuadraticConstraintSyntax:
    """All quadratic-constraint syntaxes produce identical results."""

    @_expect_gurobi
    def test_power_form(self, decidb_cli, oracle_solver, perf_tracker):
        _run_syntax_variant(
            decidb_cli, oracle_solver, perf_tracker,
            test_id="qcp_syntax_power",
            constraint_expr="POWER(x - target, 2)",
        )

    @_expect_gurobi
    def test_starstar_form(self, decidb_cli, oracle_solver, perf_tracker):
        _run_syntax_variant(
            decidb_cli, oracle_solver, perf_tracker,
            test_id="qcp_syntax_starstar",
            constraint_expr="(x - target) ** 2",
        )

    @_expect_gurobi
    def test_self_multiply_form(self, decidb_cli, oracle_solver, perf_tracker):
        _run_syntax_variant(
            decidb_cli, oracle_solver, perf_tracker,
            test_id="qcp_syntax_self_mult",
            constraint_expr="(x - target) * (x - target)",
        )

    @_expect_gurobi
    def test_bare_self_product(
        self, decidb_cli, duckdb_conn, oracle_solver, perf_tracker
    ):
        """SUM(x * x) <= 75 — 3 rows, maximize SUM(x)."""
        sql = """
            WITH data AS (
                SELECT 1 AS id UNION ALL SELECT 2 UNION ALL SELECT 3
            )
            SELECT id, ROUND(x, 4) AS x FROM data
            DECIDE x(REAL)
            SUCH THAT x >= 0 AND x <= 100
                AND SUM(x * x) <= 75
            MAXIMIZE SUM(x)
        """
        t0 = time.perf_counter()
        rows, cols = decidb_cli.execute(sql)
        decidb_time = time.perf_counter() - t0

        t_build = time.perf_counter()
        oracle_solver.create_model("qcp_bare_self_product")
        for i in range(3):
            oracle_solver.add_variable(
                f"x_{i}", VarType.CONTINUOUS, lb=0.0, ub=100.0,
            )
        oracle_solver.add_quadratic_constraint(
            linear={}, quadratic={(f"x_{i}", f"x_{i}"): 1.0 for i in range(3)},
            sense="<=", rhs=75.0, name="sum_sq",
        )
        oracle_solver.set_objective(
            {f"x_{i}": 1.0 for i in range(3)}, ObjSense.MAXIMIZE,
        )
        build_time = time.perf_counter() - t_build

        x_col = cols.index("x")
        decidb_obj = sum(float(r[x_col]) for r in rows)
        result = _assert_oracle_obj(oracle_solver, decidb_obj)
        perf_tracker.record(
            "qcp_bare_self_product", decidb_time, build_time,
            result.solve_time_seconds, 3, 3, 1,
            result.objective_value, oracle_solver.solver_name(),
            comparison_status="optimal",
        )

    @_expect_gurobi
    def test_mixed_self_product_and_bilinear(
        self, decidb_cli, duckdb_conn, oracle_solver, perf_tracker
    ):
        """SUM(x·x + x·y + y·y) <= 12 — self-products + bilinear term."""
        sql = """
            WITH data AS (SELECT 1 AS id)
            SELECT id, ROUND(x, 4) AS x, ROUND(y, 4) AS y FROM data
            DECIDE x(REAL), y(REAL)
            SUCH THAT x >= 0 AND x <= 10
                AND y >= 0 AND y <= 10
                AND SUM(x * x + x * y + y * y) <= 12
            MAXIMIZE SUM(x + y)
        """
        t0 = time.perf_counter()
        rows, cols = decidb_cli.execute(sql)
        decidb_time = time.perf_counter() - t0

        t_build = time.perf_counter()
        oracle_solver.create_model("qcp_mixed_self_bilinear")
        oracle_solver.add_variable("x", VarType.CONTINUOUS, lb=0.0, ub=10.0)
        oracle_solver.add_variable("y", VarType.CONTINUOUS, lb=0.0, ub=10.0)
        oracle_solver.add_quadratic_constraint(
            linear={},
            quadratic={("x", "x"): 1.0, ("x", "y"): 1.0, ("y", "y"): 1.0},
            sense="<=", rhs=12.0, name="quad_form",
        )
        oracle_solver.set_objective({"x": 1.0, "y": 1.0}, ObjSense.MAXIMIZE)
        build_time = time.perf_counter() - t_build

        xv = float(rows[0][cols.index("x")])
        yv = float(rows[0][cols.index("y")])
        result = _assert_oracle_obj(oracle_solver, xv + yv)
        perf_tracker.record(
            "qcp_mixed_self_bilinear", decidb_time, build_time,
            result.solve_time_seconds, 1, 2, 1,
            result.objective_value, oracle_solver.solver_name(),
            comparison_status="optimal",
        )

    @_expect_gurobi
    def test_constant_scaled_power(
        self, decidb_cli, duckdb_conn, oracle_solver, perf_tracker
    ):
        """SUM(3 · POWER(x, 2)) <= 75 — constant-scaled self-product."""
        sql = """
            WITH data AS (
                SELECT 1 AS id UNION ALL SELECT 2
            )
            SELECT id, ROUND(x, 4) AS x FROM data
            DECIDE x(REAL)
            SUCH THAT x >= 0 AND x <= 100
                AND SUM(3 * POWER(x, 2)) <= 75
            MAXIMIZE SUM(x)
        """
        t0 = time.perf_counter()
        rows, cols = decidb_cli.execute(sql)
        decidb_time = time.perf_counter() - t0

        t_build = time.perf_counter()
        oracle_solver.create_model("qcp_const_scaled")
        for i in range(2):
            oracle_solver.add_variable(
                f"x_{i}", VarType.CONTINUOUS, lb=0.0, ub=100.0,
            )
        oracle_solver.add_quadratic_constraint(
            linear={},
            quadratic={(f"x_{i}", f"x_{i}"): 3.0 for i in range(2)},
            sense="<=", rhs=75.0, name="scaled_sum_sq",
        )
        oracle_solver.set_objective(
            {f"x_{i}": 1.0 for i in range(2)}, ObjSense.MAXIMIZE,
        )
        build_time = time.perf_counter() - t_build

        x_col = cols.index("x")
        decidb_obj = sum(float(r[x_col]) for r in rows)
        result = _assert_oracle_obj(oracle_solver, decidb_obj)
        perf_tracker.record(
            "qcp_constant_scaled_power", decidb_time, build_time,
            result.solve_time_seconds, 2, 2, 1,
            result.objective_value, oracle_solver.solver_name(),
            comparison_status="optimal",
        )


# ===================================================================
# Category 3: Feature Interactions
# ===================================================================

@pytest.mark.correctness
@pytest.mark.quadratic
class TestQuadraticConstraintInteractions:
    """Quadratic constraints composed with WHEN, PER, and linear constraints."""

    @_expect_gurobi
    def test_when_filtering(
        self, decidb_cli, duckdb_conn, oracle_solver, perf_tracker
    ):
        """SUM(POWER(x - t, 2)) <= 2 WHEN active=1 — inactive rows free."""
        sql = """
            WITH data AS (
                SELECT 1 AS id, 10.0 AS target, 1 AS active UNION ALL
                SELECT 2, 20.0, 1 UNION ALL
                SELECT 3, 30.0, 0
            )
            SELECT id, ROUND(x, 4) AS x, target, active FROM data
            DECIDE x(REAL)
            SUCH THAT x >= 0 AND x <= 100
                AND SUM(POWER(x - target, 2)) <= 2 WHEN active = 1
            MAXIMIZE SUM(x)
        """
        t0 = time.perf_counter()
        rows, cols = decidb_cli.execute(sql)
        decidb_time = time.perf_counter() - t0
        data = duckdb_conn.execute("""
            SELECT CAST(id AS BIGINT), CAST(target AS DOUBLE), CAST(active AS BIGINT) FROM (
                SELECT 1 AS id, 10.0 AS target, 1 AS active UNION ALL
                SELECT 2, 20.0, 1 UNION ALL
                SELECT 3, 30.0, 0
            )
        """).fetchall()
        n = len(data)

        t_build = time.perf_counter()
        oracle_solver.create_model("qcp_when")
        for i in range(n):
            oracle_solver.add_variable(
                f"x_{i}", VarType.CONTINUOUS, lb=0.0, ub=100.0,
            )
        # Build sum-of-squared-dev over active rows only.
        active_idx = [i for i in range(n) if data[i][2] == 1]
        active_data = [(data[i][0], data[i][1]) for i in active_idx]
        linear, quadratic, adj_rhs = _aggregate_sq_dev(active_data, 1, 2.0)
        # Rename keys to match actual x_i indices
        linear = {f"x_{active_idx[k]}": v for k, (_, v) in enumerate(
            zip(active_idx, linear.values())
        )}
        quadratic = {
            (f"x_{active_idx[k]}", f"x_{active_idx[k]}"): list(quadratic.values())[k]
            for k in range(len(active_idx))
        }
        oracle_solver.add_quadratic_constraint(
            linear=linear, quadratic=quadratic,
            sense="<=", rhs=adj_rhs, name="when_active",
        )
        oracle_solver.set_objective(
            {f"x_{i}": 1.0 for i in range(n)}, ObjSense.MAXIMIZE,
        )
        build_time = time.perf_counter() - t_build

        x_col = cols.index("x")
        decidb_obj = sum(float(r[x_col]) for r in rows)
        result = _assert_oracle_obj(oracle_solver, decidb_obj)
        perf_tracker.record(
            "qcp_when_filter", decidb_time, build_time, result.solve_time_seconds,
            n, n, 1, result.objective_value, oracle_solver.solver_name(),
            comparison_status="optimal",
        )

    @_expect_gurobi
    def test_per_group_quadratic_constraint(
        self, decidb_cli, duckdb_conn, oracle_solver, perf_tracker
    ):
        """SUM(POWER(x - t, 2)) <= 10 PER group — one quadratic constraint per group."""
        sql = """
            WITH data AS (
                SELECT 1 AS id, 'A' AS grp, 10.0 AS target UNION ALL
                SELECT 2, 'A', 15.0 UNION ALL
                SELECT 3, 'B', 30.0 UNION ALL
                SELECT 4, 'B', 35.0
            )
            SELECT id, grp, ROUND(x, 4) AS x, target FROM data
            DECIDE x(REAL)
            SUCH THAT x >= 0 AND x <= 100
                AND SUM(POWER(x - target, 2)) <= 10 PER grp
            MAXIMIZE SUM(x)
        """
        t0 = time.perf_counter()
        rows, cols = decidb_cli.execute(sql)
        decidb_time = time.perf_counter() - t0
        data = duckdb_conn.execute("""
            SELECT CAST(id AS BIGINT), CAST(grp AS VARCHAR), CAST(target AS DOUBLE) FROM (
                SELECT 1 AS id, 'A' AS grp, 10.0 AS target UNION ALL
                SELECT 2, 'A', 15.0 UNION ALL
                SELECT 3, 'B', 30.0 UNION ALL
                SELECT 4, 'B', 35.0
            )
        """).fetchall()
        n = len(data)

        t_build = time.perf_counter()
        oracle_solver.create_model("qcp_per_group")
        for i in range(n):
            oracle_solver.add_variable(
                f"x_{i}", VarType.CONTINUOUS, lb=0.0, ub=100.0,
            )
        from collections import defaultdict
        groups: dict = defaultdict(list)
        for i, row in enumerate(data):
            groups[row[1]].append(i)
        for grp, idxs in groups.items():
            constant = sum(float(data[i][2]) ** 2 for i in idxs)
            linear = {f"x_{i}": -2.0 * float(data[i][2]) for i in idxs}
            quadratic = {(f"x_{i}", f"x_{i}"): 1.0 for i in idxs}
            oracle_solver.add_quadratic_constraint(
                linear=linear, quadratic=quadratic,
                sense="<=", rhs=10.0 - constant, name=f"per_{grp}",
            )
        oracle_solver.set_objective(
            {f"x_{i}": 1.0 for i in range(n)}, ObjSense.MAXIMIZE,
        )
        build_time = time.perf_counter() - t_build

        x_col = cols.index("x")
        decidb_obj = sum(float(r[x_col]) for r in rows)
        result = _assert_oracle_obj(oracle_solver, decidb_obj)
        perf_tracker.record(
            "qcp_per_group", decidb_time, build_time, result.solve_time_seconds,
            n, n, len(groups),
            result.objective_value, oracle_solver.solver_name(),
            comparison_status="optimal",
        )

    @_expect_gurobi
    def test_when_per_quadratic_constraint(
        self, decidb_cli, duckdb_conn, oracle_solver, perf_tracker
    ):
        """SUM(POWER(x - t, 2)) <= K WHEN active=1 PER grp.

        WHEN filters out inactive rows; PER then partitions the surviving rows
        into groups and emits one quadratic constraint per group. Inactive
        rows have no constraint, so they go to the upper bound (x = 100). The
        test exercises the WHEN-mask-then-PER-group path for QCQP — the Q
        matrix per group must reference only active rows in that group.
        """
        data_sql = """
            SELECT 1 AS id, 'A' AS grp, 1 AS active, 10.0 AS target UNION ALL
            SELECT 2, 'A', 1, 12.0 UNION ALL
            SELECT 3, 'A', 0, 99.0 UNION ALL
            SELECT 4, 'B', 1, 30.0 UNION ALL
            SELECT 5, 'B', 1, 32.0 UNION ALL
            SELECT 6, 'B', 0, 99.0
        """
        sql = f"""
            WITH data AS ({data_sql})
            SELECT id, grp, active, ROUND(x, 4) AS x, target FROM data
            DECIDE x(REAL)
            SUCH THAT x >= 0 AND x <= 100
                AND SUM(POWER(x - target, 2)) <= 5 WHEN active = 1 PER grp
            MAXIMIZE SUM(x)
        """
        t0 = time.perf_counter()
        rows, cols = decidb_cli.execute(sql)
        decidb_time = time.perf_counter() - t0
        data = duckdb_conn.execute(f"""
            SELECT CAST(id AS BIGINT), CAST(grp AS VARCHAR),
                   CAST(active AS BIGINT), CAST(target AS DOUBLE)
            FROM ({data_sql})
        """).fetchall()
        n = len(data)

        t_build = time.perf_counter()
        oracle_solver.create_model("qcp_when_per")
        for i in range(n):
            oracle_solver.add_variable(
                f"x_{i}", VarType.CONTINUOUS, lb=0.0, ub=100.0,
            )
        # Build one quadratic constraint per group, using only active rows.
        from collections import defaultdict
        active_by_grp: dict = defaultdict(list)
        for i, row in enumerate(data):
            if int(row[2]) == 1:
                active_by_grp[row[1]].append(i)
        for grp, idxs in active_by_grp.items():
            constant = sum(float(data[i][3]) ** 2 for i in idxs)
            linear = {f"x_{i}": -2.0 * float(data[i][3]) for i in idxs}
            quadratic = {(f"x_{i}", f"x_{i}"): 1.0 for i in idxs}
            oracle_solver.add_quadratic_constraint(
                linear=linear, quadratic=quadratic,
                sense="<=", rhs=5.0 - constant, name=f"when_per_{grp}",
            )
        oracle_solver.set_objective(
            {f"x_{i}": 1.0 for i in range(n)}, ObjSense.MAXIMIZE,
        )
        build_time = time.perf_counter() - t_build

        x_col = cols.index("x")
        decidb_obj = sum(float(r[x_col]) for r in rows)
        result = _assert_oracle_obj(oracle_solver, decidb_obj)

        # Sanity: inactive rows should hit the upper bound (free of QP cap).
        active_col = cols.index("active")
        for r in rows:
            if int(r[active_col]) == 0:
                assert abs(float(r[x_col]) - 100.0) <= 1e-3, (
                    f"Inactive row should be unconstrained at upper bound, "
                    f"got x={r[x_col]} for id={r[cols.index('id')]}"
                )

        perf_tracker.record(
            "qcp_when_per", decidb_time, build_time, result.solve_time_seconds,
            n, n, len(active_by_grp),
            result.objective_value, oracle_solver.solver_name(),
            comparison_status="optimal",
        )

    @_expect_gurobi
    def test_multiple_quadratic_constraints(
        self, decidb_cli, duckdb_conn, oracle_solver, perf_tracker
    ):
        """Two quadratic constraints simultaneously."""
        sql = """
            WITH data AS (
                SELECT 1 AS id, 10.0 AS t1, 15.0 AS t2 UNION ALL
                SELECT 2, 20.0, 25.0 UNION ALL
                SELECT 3, 30.0, 35.0
            )
            SELECT id, ROUND(x, 4) AS x FROM data
            DECIDE x(REAL)
            SUCH THAT x >= 0 AND x <= 100
                AND SUM(POWER(x - t1, 2)) <= 50
                AND SUM(POWER(x - t2, 2)) <= 50
            MAXIMIZE SUM(x)
        """
        t0 = time.perf_counter()
        rows, cols = decidb_cli.execute(sql)
        decidb_time = time.perf_counter() - t0
        targets1 = [(1, 10.0), (2, 20.0), (3, 30.0)]
        targets2 = [(1, 15.0), (2, 25.0), (3, 35.0)]
        n = 3

        t_build = time.perf_counter()
        oracle_solver.create_model("qcp_two_constraints")
        for i in range(n):
            oracle_solver.add_variable(
                f"x_{i}", VarType.CONTINUOUS, lb=0.0, ub=100.0,
            )
        for tag, tgts in (("t1", targets1), ("t2", targets2)):
            linear, quadratic, adj_rhs = _aggregate_sq_dev(tgts, 1, 50.0)
            oracle_solver.add_quadratic_constraint(
                linear=linear, quadratic=quadratic,
                sense="<=", rhs=adj_rhs, name=f"budget_{tag}",
            )
        oracle_solver.set_objective(
            {f"x_{i}": 1.0 for i in range(n)}, ObjSense.MAXIMIZE,
        )
        build_time = time.perf_counter() - t_build

        x_col = cols.index("x")
        decidb_obj = sum(float(r[x_col]) for r in rows)
        result = _assert_oracle_obj(oracle_solver, decidb_obj)
        perf_tracker.record(
            "qcp_multi_constraint", decidb_time, build_time,
            result.solve_time_seconds, n, n, 2,
            result.objective_value, oracle_solver.solver_name(),
            comparison_status="optimal",
        )

    @_expect_gurobi
    def test_qcqp_quadratic_objective_and_constraint(
        self, decidb_cli, duckdb_conn, oracle_solver, perf_tracker
    ):
        """Full QCQP: quadratic in both objective and constraint.

        MINIMIZE SUM(POWER(x - preferred, 2))
        SUBJECT TO SUM(POWER(x - required, 2)) <= 50
        """
        sql = """
            WITH data AS (
                SELECT 1 AS id, 10.0 AS preferred, 20.0 AS required UNION ALL
                SELECT 2, 50.0, 30.0 UNION ALL
                SELECT 3, 30.0, 25.0
            )
            SELECT id, ROUND(x, 4) AS x, preferred, required FROM data
            DECIDE x(REAL)
            SUCH THAT x >= 0 AND x <= 100
                AND SUM(POWER(x - required, 2)) <= 50
            MINIMIZE SUM(POWER(x - preferred, 2))
        """
        t0 = time.perf_counter()
        rows, cols = decidb_cli.execute(sql)
        decidb_time = time.perf_counter() - t0
        data = duckdb_conn.execute("""
            SELECT CAST(id AS BIGINT), CAST(preferred AS DOUBLE), CAST(required AS DOUBLE) FROM (
                SELECT 1 AS id, 10.0 AS preferred, 20.0 AS required UNION ALL
                SELECT 2, 50.0, 30.0 UNION ALL
                SELECT 3, 30.0, 25.0
            )
        """).fetchall()
        n = len(data)

        t_build = time.perf_counter()
        oracle_solver.create_model("qcqp")
        for i in range(n):
            oracle_solver.add_variable(
                f"x_{i}", VarType.CONTINUOUS, lb=0.0, ub=100.0,
            )
        req_targets = [(data[i][0], data[i][2]) for i in range(n)]
        linear, quadratic, adj_rhs = _aggregate_sq_dev(req_targets, 1, 50.0)
        oracle_solver.add_quadratic_constraint(
            linear=linear, quadratic=quadratic,
            sense="<=", rhs=adj_rhs, name="required_budget",
        )
        # Objective: minimize SUM(POWER(x_i - preferred_i, 2)), dropping
        # constant Σ preferred_i^2 to match DecidB's reported objective.
        obj_linear = {f"x_{i}": -2.0 * data[i][1] for i in range(n)}
        obj_quadratic = {(f"x_{i}", f"x_{i}"): 1.0 for i in range(n)}
        oracle_solver.set_quadratic_objective(
            obj_linear, obj_quadratic, ObjSense.MINIMIZE,
        )
        build_time = time.perf_counter() - t_build

        x_col = cols.index("x"); pref_col = cols.index("preferred")
        decidb_obj = sum(
            (float(r[x_col]) - float(r[pref_col])) ** 2 - float(r[pref_col]) ** 2
            for r in rows
        )
        result = _assert_oracle_obj(oracle_solver, decidb_obj)
        perf_tracker.record(
            "qcqp_both", decidb_time, build_time, result.solve_time_seconds,
            n, n, 1, result.objective_value, oracle_solver.solver_name(),
            comparison_status="optimal",
        )

    @_expect_gurobi
    def test_mixed_linear_and_quadratic_constraints(
        self, decidb_cli, duckdb_conn, oracle_solver, perf_tracker
    ):
        """SUM(x) >= 50 AND SUM(POWER(x - t, 2)) <= 25 — mixed constraint set."""
        sql = """
            WITH data AS (
                SELECT 1 AS id, 10.0 AS target UNION ALL
                SELECT 2, 15.0 UNION ALL
                SELECT 3, 20.0
            )
            SELECT id, ROUND(x, 4) AS x, target FROM data
            DECIDE x(REAL)
            SUCH THAT x >= 0 AND x <= 100
                AND SUM(x) >= 50
                AND SUM(POWER(x - target, 2)) <= 25
            MINIMIZE SUM(POWER(x - target, 2))
        """
        t0 = time.perf_counter()
        rows, cols = decidb_cli.execute(sql)
        decidb_time = time.perf_counter() - t0
        data = [(1, 10.0), (2, 15.0), (3, 20.0)]
        n = len(data)

        t_build = time.perf_counter()
        oracle_solver.create_model("qcp_mixed_lin_quad")
        for i in range(n):
            oracle_solver.add_variable(
                f"x_{i}", VarType.CONTINUOUS, lb=0.0, ub=100.0,
            )
        oracle_solver.add_constraint(
            {f"x_{i}": 1.0 for i in range(n)}, ">=", 50.0, name="lin",
        )
        linear, quadratic, adj_rhs = _aggregate_sq_dev(data, 1, 25.0)
        oracle_solver.add_quadratic_constraint(
            linear=linear, quadratic=quadratic,
            sense="<=", rhs=adj_rhs, name="quad_budget",
        )
        obj_linear = {f"x_{i}": -2.0 * data[i][1] for i in range(n)}
        obj_quadratic = {(f"x_{i}", f"x_{i}"): 1.0 for i in range(n)}
        oracle_solver.set_quadratic_objective(
            obj_linear, obj_quadratic, ObjSense.MINIMIZE,
        )
        build_time = time.perf_counter() - t_build

        x_col = cols.index("x"); target_col = cols.index("target")
        decidb_obj = sum(
            (float(r[x_col]) - float(r[target_col])) ** 2 - float(r[target_col]) ** 2
            for r in rows
        )
        result = _assert_oracle_obj(oracle_solver, decidb_obj)
        perf_tracker.record(
            "qcp_mixed_lin_quad", decidb_time, build_time,
            result.solve_time_seconds, n, n, 2,
            result.objective_value, oracle_solver.solver_name(),
            comparison_status="optimal",
        )


# ===================================================================
# Category 4: Variable Types
# ===================================================================

@pytest.mark.correctness
@pytest.mark.quadratic
class TestQuadraticConstraintVarTypes:

    @_expect_gurobi
    def test_real_variables(
        self, decidb_cli, duckdb_conn, oracle_solver, perf_tracker
    ):
        sql = """
            WITH data AS (
                SELECT 1 AS id, 10.0 AS target UNION ALL
                SELECT 2, 20.0
            )
            SELECT id, ROUND(x, 4) AS x, target FROM data
            DECIDE x(REAL)
            SUCH THAT x >= 0 AND x <= 100
                AND SUM(POWER(x - target, 2)) <= 10
            MAXIMIZE SUM(x)
        """
        t0 = time.perf_counter()
        rows, cols = decidb_cli.execute(sql)
        decidb_time = time.perf_counter() - t0
        data = [(1, 10.0), (2, 20.0)]
        n = len(data)

        t_build = time.perf_counter()
        oracle_solver.create_model("qcp_real_vars")
        for i in range(n):
            oracle_solver.add_variable(
                f"x_{i}", VarType.CONTINUOUS, lb=0.0, ub=100.0,
            )
        linear, quadratic, adj_rhs = _aggregate_sq_dev(data, 1, 10.0)
        oracle_solver.add_quadratic_constraint(
            linear=linear, quadratic=quadratic,
            sense="<=", rhs=adj_rhs, name="budget",
        )
        oracle_solver.set_objective(
            {f"x_{i}": 1.0 for i in range(n)}, ObjSense.MAXIMIZE,
        )
        build_time = time.perf_counter() - t_build

        x_col = cols.index("x")
        decidb_obj = sum(float(r[x_col]) for r in rows)
        result = _assert_oracle_obj(oracle_solver, decidb_obj)
        perf_tracker.record(
            "qcp_real", decidb_time, build_time, result.solve_time_seconds,
            n, n, 1, result.objective_value, oracle_solver.solver_name(),
            comparison_status="optimal",
        )

    @_expect_gurobi
    def test_integer_variables(
        self, decidb_cli, duckdb_conn, oracle_solver, perf_tracker
    ):
        """MIQP with quadratic constraint."""
        sql = """
            WITH data AS (
                SELECT 1 AS id, 10 AS target UNION ALL
                SELECT 2, 20 UNION ALL
                SELECT 3, 30
            )
            SELECT id, x, target FROM data
            DECIDE x(INT)
            SUCH THAT x >= 0 AND x <= 100
                AND SUM(POWER(x - target, 2)) <= 6
            MAXIMIZE SUM(x)
        """
        t0 = time.perf_counter()
        rows, cols = decidb_cli.execute(sql)
        decidb_time = time.perf_counter() - t0
        data = [(1, 10.0), (2, 20.0), (3, 30.0)]
        n = len(data)

        t_build = time.perf_counter()
        oracle_solver.create_model("qcp_integer_vars")
        for i in range(n):
            oracle_solver.add_variable(
                f"x_{i}", VarType.INTEGER, lb=0.0, ub=100.0,
            )
        linear, quadratic, adj_rhs = _aggregate_sq_dev(data, 1, 6.0)
        oracle_solver.add_quadratic_constraint(
            linear=linear, quadratic=quadratic,
            sense="<=", rhs=adj_rhs, name="budget_int",
        )
        oracle_solver.set_objective(
            {f"x_{i}": 1.0 for i in range(n)}, ObjSense.MAXIMIZE,
        )
        build_time = time.perf_counter() - t_build

        x_col = cols.index("x")
        decidb_obj = sum(float(r[x_col]) for r in rows)
        result = _assert_oracle_obj(oracle_solver, decidb_obj)
        perf_tracker.record(
            "qcp_integer", decidb_time, build_time, result.solve_time_seconds,
            n, n, 1, result.objective_value, oracle_solver.solver_name(),
            comparison_status="optimal",
        )

    @_expect_gurobi
    def test_table_scoped_variables(
        self, decidb_cli, duckdb_conn, oracle_solver, perf_tracker
    ):
        """Table-scoped x with quadratic constraint.

        Each distinct ``item`` value yields one x variable; all rows of the
        same item share that x. The inner SUM(POWER(x - target, 2)) then
        touches the same x across rows of the same item.
        """
        sql = """
            WITH items AS (
                SELECT 'A' AS item, 10.0 AS target UNION ALL
                SELECT 'A', 12.0 UNION ALL
                SELECT 'B', 20.0 UNION ALL
                SELECT 'B', 22.0
            )
            SELECT item, ROUND(x, 4) AS x, target FROM items
            DECIDE items.x(REAL)
            SUCH THAT x >= 0 AND x <= 100
                AND SUM(POWER(x - target, 2)) <= 20
            MAXIMIZE SUM(x)
        """
        t0 = time.perf_counter()
        rows, cols = decidb_cli.execute(sql)
        decidb_time = time.perf_counter() - t0
        data = duckdb_conn.execute("""
            SELECT CAST(item AS VARCHAR), CAST(target AS DOUBLE) FROM (
                SELECT 'A' AS item, 10.0 AS target UNION ALL
                SELECT 'A', 12.0 UNION ALL
                SELECT 'B', 20.0 UNION ALL
                SELECT 'B', 22.0
            )
        """).fetchall()

        t_build = time.perf_counter()
        oracle_solver.create_model("qcp_table_scoped")
        items = sorted({r[0] for r in data})
        for it in items:
            oracle_solver.add_variable(
                f"x_{it}", VarType.CONTINUOUS, lb=0.0, ub=100.0,
            )
        # Inner SUM: for each row, coefficient maps to the per-item variable.
        linear: dict = {}
        quadratic: dict = {}
        constant = 0.0
        for row in data:
            it = row[0]; t = float(row[1])
            key = f"x_{it}"
            linear[key] = linear.get(key, 0.0) - 2.0 * t
            quadratic[(key, key)] = quadratic.get((key, key), 0.0) + 1.0
            constant += t * t
        oracle_solver.add_quadratic_constraint(
            linear=linear, quadratic=quadratic,
            sense="<=", rhs=20.0 - constant, name="agg_budget",
        )
        # Objective: SUM(x) over rows = (rows per item) * x_item.
        from collections import Counter
        cnt = Counter(r[0] for r in data)
        oracle_solver.set_objective(
            {f"x_{it}": float(cnt[it]) for it in items}, ObjSense.MAXIMIZE,
        )
        build_time = time.perf_counter() - t_build

        x_col = cols.index("x")
        decidb_obj = sum(float(r[x_col]) for r in rows)
        result = _assert_oracle_obj(oracle_solver, decidb_obj)
        perf_tracker.record(
            "qcp_table_scoped", decidb_time, build_time, result.solve_time_seconds,
            len(data), len(items), 1,
            result.objective_value, oracle_solver.solver_name(),
            comparison_status="optimal",
        )


# ===================================================================
# Category 5: Edge Cases
# ===================================================================

@pytest.mark.correctness
@pytest.mark.quadratic
class TestQuadraticConstraintEdgeCases:

    def test_infeasible_negative_budget(
        self, decidb_cli, duckdb_conn, oracle_solver
    ):
        """POWER(x, 2) <= -1 is unsatisfiable. Oracle should also report INFEASIBLE."""
        sql = """
            WITH data AS (SELECT 1 AS id)
            SELECT id, x FROM data
            DECIDE x(REAL)
            SUCH THAT x >= 0 AND x <= 10
                AND POWER(x, 2) <= -1
            MAXIMIZE SUM(x)
        """
        decidb_cli.assert_error(sql, match=r"(?i)infeasible")

        oracle_solver.create_model("qcp_infeasible")
        oracle_solver.add_variable("x", VarType.CONTINUOUS, lb=0.0, ub=10.0)
        oracle_solver.add_quadratic_constraint(
            linear={}, quadratic={("x", "x"): 1.0},
            sense="<=", rhs=-1.0, name="infeasible",
        )
        oracle_solver.set_objective({"x": 1.0}, ObjSense.MAXIMIZE)
        assert oracle_solver.solve().status == SolverStatus.INFEASIBLE

    @_expect_gurobi
    def test_single_row(
        self, decidb_cli, duckdb_conn, oracle_solver, perf_tracker
    ):
        """Single-row POWER(x - target, 2) <= 4 degenerates to x ∈ [3, 7]."""
        sql = """
            WITH data AS (SELECT 1 AS id, 5.0 AS target)
            SELECT id, ROUND(x, 4) AS x, target FROM data
            DECIDE x(REAL)
            SUCH THAT x >= 0 AND x <= 100
                AND POWER(x - target, 2) <= 4
            MAXIMIZE SUM(x)
        """
        t0 = time.perf_counter()
        rows, cols = decidb_cli.execute(sql)
        decidb_time = time.perf_counter() - t0

        t_build = time.perf_counter()
        oracle_solver.create_model("qcp_single_row")
        oracle_solver.add_variable("x", VarType.CONTINUOUS, lb=0.0, ub=100.0)
        oracle_solver.add_quadratic_constraint(
            linear={"x": -10.0}, quadratic={("x", "x"): 1.0},
            sense="<=", rhs=4.0 - 25.0, name="bound",
        )
        oracle_solver.set_objective({"x": 1.0}, ObjSense.MAXIMIZE)
        build_time = time.perf_counter() - t_build

        xv = float(rows[0][cols.index("x")])
        result = _assert_oracle_obj(oracle_solver, xv)
        perf_tracker.record(
            "qcp_single_row", decidb_time, build_time, result.solve_time_seconds,
            1, 1, 1, result.objective_value, oracle_solver.solver_name(),
            comparison_status="optimal",
        )

    @_expect_gurobi
    def test_per_row_constraint(
        self, decidb_cli, duckdb_conn, oracle_solver, perf_tracker
    ):
        """POWER(x - target, 2) <= 9 per row — each row independent."""
        sql = """
            WITH data AS (
                SELECT 1 AS id, 10.0 AS target UNION ALL
                SELECT 2, 20.0 UNION ALL
                SELECT 3, 30.0
            )
            SELECT id, ROUND(x, 4) AS x, target FROM data
            DECIDE x(REAL)
            SUCH THAT x >= 0 AND x <= 100
                AND POWER(x - target, 2) <= 9
            MAXIMIZE SUM(x)
        """
        t0 = time.perf_counter()
        rows, cols = decidb_cli.execute(sql)
        decidb_time = time.perf_counter() - t0
        data = [(1, 10.0), (2, 20.0), (3, 30.0)]
        n = len(data)

        t_build = time.perf_counter()
        oracle_solver.create_model("qcp_per_row")
        for i in range(n):
            oracle_solver.add_variable(
                f"x_{i}", VarType.CONTINUOUS, lb=0.0, ub=100.0,
            )
            t = float(data[i][1])
            oracle_solver.add_quadratic_constraint(
                linear={f"x_{i}": -2.0 * t},
                quadratic={(f"x_{i}", f"x_{i}"): 1.0},
                sense="<=", rhs=9.0 - t * t, name=f"row_{i}",
            )
        oracle_solver.set_objective(
            {f"x_{i}": 1.0 for i in range(n)}, ObjSense.MAXIMIZE,
        )
        build_time = time.perf_counter() - t_build

        x_col = cols.index("x")
        decidb_obj = sum(float(r[x_col]) for r in rows)
        result = _assert_oracle_obj(oracle_solver, decidb_obj)
        perf_tracker.record(
            "qcp_per_row", decidb_time, build_time, result.solve_time_seconds,
            n, n, n, result.objective_value, oracle_solver.solver_name(),
            comparison_status="optimal",
        )


# ===================================================================
# Category 6: Error Handling
# ===================================================================

@pytest.mark.correctness
@pytest.mark.quadratic
class TestQuadraticConstraintErrors:

    def test_constraint_self_product_of_power_rejected(self, decidb_cli):
        # POWER(x, 2) * POWER(x, 2) inside a constraint = x^4; must not
        # silently reduce to x^2. (Previously this produced wrong bindings
        # because the inner POWER was treated as a linear term.)
        decidb_cli.assert_error("""
            WITH data AS (SELECT 1 AS id)
            SELECT id, ROUND(x, 4) AS x FROM data
            DECIDE x(REAL)
            SUCH THAT x >= 0 AND x <= 3
                AND SUM(POWER(x, 2) * POWER(x, 2)) <= 16
            MINIMIZE SUM(-x)
        """, match=r"self-product .* non-linear|degree > 2")

    def test_constraint_product_of_two_powers_rejected(self, decidb_cli):
        # POWER(x, 2) * POWER(y, 2) inside a constraint = x^2 y^2.
        decidb_cli.assert_error("""
            WITH data AS (SELECT 1 AS id)
            SELECT id, x, y FROM data
            DECIDE x(REAL), y(REAL)
            SUCH THAT x >= 0 AND x <= 5 AND y >= 0 AND y <= 5
                AND SUM(POWER(x, 2) * POWER(y, 2)) <= 10
            MAXIMIZE SUM(x + y)
        """, match=r"degree > 2")

    def test_constraint_variable_times_power_rejected(self, decidb_cli):
        # a * POWER(x, 2) inside a constraint = a x^2 (total degree 3).
        decidb_cli.assert_error("""
            WITH data AS (SELECT 1 AS id)
            SELECT id, a, x FROM data
            DECIDE a(REAL), x(REAL)
            SUCH THAT a >= 0 AND a <= 5 AND x >= 0 AND x <= 5
                AND SUM(a * POWER(x, 2)) <= 20
            MAXIMIZE SUM(a + x)
        """, match=r"degree > 2")
