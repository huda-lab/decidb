"""Tests for quadratic programming (QP) objectives.

Every correctness test formulates the same QP independently via
``oracle_solver.set_quadratic_objective(linear, quadratic, sense)`` and
compares objective values. DecidB's reported objective drops the constant
``t^2`` term from ``POWER(x - t, 2)`` expansion, so the oracle also omits
that constant and the test's decidb-side evaluator strips it on the fly.

Covers:
  - POWER(linear_expr, 2) syntax
  - ** operator form
  - (expr)*(expr) multiplication form
  - MINIMIZE convex QP (standard)
  - MAXIMIZE concave QP (negated quadratic, Case A)
  - MAXIMIZE convex QP (non-convex, Case B — Gurobi only)
  - Mixed linear + quadratic objective
  - REAL/INTEGER variable QP (continuous and MIQP)
  - Error rejection cases
"""

import os
import re
import tempfile
import time

import pytest

from decidb_cli import DecidBCliError
from solver.types import ObjSense, SolverStatus, VarType


# ---------------------------------------------------------------------------
# Common plumbing
# ---------------------------------------------------------------------------

def _solve_and_compare(
    oracle_solver, decidb_rows, decidb_cols, decidb_obj_fn,
):
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL, (
        f"Oracle failed to solve: status={result.status}"
    )
    decidb_obj = decidb_obj_fn(decidb_rows, decidb_cols)
    diff = abs(decidb_obj - result.objective_value)
    assert diff <= 1e-3, (
        f"QP objective mismatch: DecidB={decidb_obj:.6f}, "
        f"Oracle={result.objective_value:.6f}, diff={diff:.6f}"
    )
    return result


def _qp_target_obj(decidb_rows, decidb_cols, *, x_col="x", target_col="target",
                   scale=1.0, mask_fn=None, linear_coeff=None):
    """DecidB-side evaluator for SUM(scale * POWER(x - target, 2)
       (+ linear_coeff * x)), with the t^2 constant stripped to match
       the oracle's ``set_quadratic_objective`` (no constant offset)."""
    xi = decidb_cols.index(x_col)
    ti = decidb_cols.index(target_col)
    total = 0.0
    for row in decidb_rows:
        if mask_fn is not None and not mask_fn(row):
            continue
        x = float(row[xi]); t = float(row[ti])
        total += scale * ((x - t) ** 2 - t ** 2)
        if linear_coeff is not None:
            total += linear_coeff(row) * x
    return total


def _qp_simple_obj(decidb_rows, decidb_cols, *, x_col="x", scale=1.0):
    """DecidB-side evaluator for SUM(scale * POWER(x, 2)) (no target)."""
    xi = decidb_cols.index(x_col)
    return sum(scale * float(row[xi]) ** 2 for row in decidb_rows)


# ===========================================================================
# Basic MINIMIZE QP
# ===========================================================================

@pytest.mark.obj_minimize
@pytest.mark.quadratic
@pytest.mark.correctness
class TestQuadraticBasic:
    """Inline-data QP smoke tests, each oracle-compared."""

    def test_power_syntax_unconstrained(
        self, decidb_cli, duckdb_conn, oracle_solver, perf_tracker
    ):
        """MINIMIZE SUM(POWER(x - target, 2)) with bounds [0, 100]."""
        data_sql = """
            SELECT 1 AS id, 10.0 AS target UNION ALL
            SELECT 2, 20.0 UNION ALL
            SELECT 3, 30.0
        """
        sql = f"""
            WITH data AS ({data_sql})
            SELECT id, target, ROUND(x, 4) AS x
            FROM data
            DECIDE x(REAL)
            SUCH THAT x >= 0 AND x <= 100
            MINIMIZE SUM(POWER(x - target, 2))
        """
        t0 = time.perf_counter()
        rows, cols = decidb_cli.execute(sql)
        decidb_time = time.perf_counter() - t0
        data = duckdb_conn.execute(
            f"SELECT CAST(id AS BIGINT), CAST(target AS DOUBLE) FROM ({data_sql})"
        ).fetchall()

        t_build = time.perf_counter()
        oracle_solver.create_model("qp_unconstrained")
        n = len(data)
        xnames = [f"x_{i}" for i in range(n)]
        for xn in xnames:
            oracle_solver.add_variable(xn, VarType.CONTINUOUS, lb=0.0, ub=100.0)
        linear = {xnames[i]: -2.0 * data[i][1] for i in range(n)}
        quadratic = {(xnames[i], xnames[i]): 1.0 for i in range(n)}
        oracle_solver.set_quadratic_objective(linear, quadratic, ObjSense.MINIMIZE)
        build_time = time.perf_counter() - t_build
        result = _solve_and_compare(
            oracle_solver, rows, cols,
            lambda rs, cs: _qp_target_obj(rs, cs),
        )
        perf_tracker.record(
            "qp_power_unconstrained", decidb_time, build_time,
            result.solve_time_seconds, n, n, 0,
            result.objective_value, oracle_solver.solver_name(),
            comparison_status="optimal",
        )

    def test_starstar_operator(
        self, decidb_cli, duckdb_conn, oracle_solver, perf_tracker
    ):
        """** operator behaves identically to POWER."""
        data_sql = (
            "SELECT 1 AS id, 15.0 AS target UNION ALL SELECT 2, 25.0"
        )
        sql = f"""
            WITH data AS ({data_sql})
            SELECT id, target, ROUND(x, 4) AS x FROM data
            DECIDE x(REAL)
            SUCH THAT x >= 0 AND x <= 100
            MINIMIZE SUM((x - target) ** 2)
        """
        t0 = time.perf_counter()
        rows, cols = decidb_cli.execute(sql)
        decidb_time = time.perf_counter() - t0
        data = duckdb_conn.execute(
            f"SELECT CAST(id AS BIGINT), CAST(target AS DOUBLE) FROM ({data_sql})"
        ).fetchall()

        t_build = time.perf_counter()
        oracle_solver.create_model("qp_starstar")
        n = len(data)
        xnames = [f"x_{i}" for i in range(n)]
        for xn in xnames:
            oracle_solver.add_variable(xn, VarType.CONTINUOUS, lb=0.0, ub=100.0)
        oracle_solver.set_quadratic_objective(
            {xnames[i]: -2.0 * data[i][1] for i in range(n)},
            {(xnames[i], xnames[i]): 1.0 for i in range(n)},
            ObjSense.MINIMIZE,
        )
        build_time = time.perf_counter() - t_build
        result = _solve_and_compare(
            oracle_solver, rows, cols, lambda rs, cs: _qp_target_obj(rs, cs),
        )
        perf_tracker.record(
            "qp_starstar", decidb_time, build_time, result.solve_time_seconds,
            n, n, 0, result.objective_value, oracle_solver.solver_name(),
            comparison_status="optimal",
        )

    def test_multiplication_form(
        self, decidb_cli, duckdb_conn, oracle_solver, perf_tracker
    ):
        """(expr)*(expr) == POWER."""
        data_sql = (
            "SELECT 1 AS id, 7.0 AS target UNION ALL SELECT 2, 42.0"
        )
        sql = f"""
            WITH data AS ({data_sql})
            SELECT id, target, ROUND(x, 4) AS x FROM data
            DECIDE x(REAL)
            SUCH THAT x >= 0 AND x <= 100
            MINIMIZE SUM((x - target) * (x - target))
        """
        t0 = time.perf_counter()
        rows, cols = decidb_cli.execute(sql)
        decidb_time = time.perf_counter() - t0
        data = duckdb_conn.execute(
            f"SELECT CAST(id AS BIGINT), CAST(target AS DOUBLE) FROM ({data_sql})"
        ).fetchall()

        t_build = time.perf_counter()
        oracle_solver.create_model("qp_multform")
        n = len(data)
        xnames = [f"x_{i}" for i in range(n)]
        for xn in xnames:
            oracle_solver.add_variable(xn, VarType.CONTINUOUS, lb=0.0, ub=100.0)
        oracle_solver.set_quadratic_objective(
            {xnames[i]: -2.0 * data[i][1] for i in range(n)},
            {(xnames[i], xnames[i]): 1.0 for i in range(n)},
            ObjSense.MINIMIZE,
        )
        build_time = time.perf_counter() - t_build
        result = _solve_and_compare(
            oracle_solver, rows, cols, lambda rs, cs: _qp_target_obj(rs, cs),
        )
        perf_tracker.record(
            "qp_multform", decidb_time, build_time, result.solve_time_seconds,
            n, n, 0, result.objective_value, oracle_solver.solver_name(),
            comparison_status="optimal",
        )

    def test_qp_with_binding_constraint(
        self, decidb_cli, duckdb_conn, oracle_solver, perf_tracker
    ):
        """Box constraints bind: target=[5, 50] with bounds [10, 40]."""
        data_sql = "SELECT 1 AS id, 5.0 AS target UNION ALL SELECT 2, 50.0"
        sql = f"""
            WITH data AS ({data_sql})
            SELECT id, target, ROUND(x, 4) AS x FROM data
            DECIDE x(REAL)
            SUCH THAT x >= 10 AND x <= 40
            MINIMIZE SUM(POWER(x - target, 2))
        """
        t0 = time.perf_counter()
        rows, cols = decidb_cli.execute(sql)
        decidb_time = time.perf_counter() - t0
        data = duckdb_conn.execute(
            f"SELECT CAST(id AS BIGINT), CAST(target AS DOUBLE) FROM ({data_sql})"
        ).fetchall()

        t_build = time.perf_counter()
        oracle_solver.create_model("qp_binding")
        n = len(data)
        xnames = [f"x_{i}" for i in range(n)]
        for xn in xnames:
            oracle_solver.add_variable(xn, VarType.CONTINUOUS, lb=10.0, ub=40.0)
        oracle_solver.set_quadratic_objective(
            {xnames[i]: -2.0 * data[i][1] for i in range(n)},
            {(xnames[i], xnames[i]): 1.0 for i in range(n)},
            ObjSense.MINIMIZE,
        )
        build_time = time.perf_counter() - t_build
        result = _solve_and_compare(
            oracle_solver, rows, cols, lambda rs, cs: _qp_target_obj(rs, cs),
        )
        perf_tracker.record(
            "qp_binding", decidb_time, build_time, result.solve_time_seconds,
            n, n, 2, result.objective_value, oracle_solver.solver_name(),
            comparison_status="optimal",
        )

    def test_qp_with_aggregate_constraint(
        self, decidb_cli, duckdb_conn, oracle_solver, perf_tracker
    ):
        """SUM(x) = 60 matches unconstrained optimum — constraint not binding."""
        data_sql = """
            SELECT 1 AS id, 10.0 AS target UNION ALL
            SELECT 2, 20.0 UNION ALL
            SELECT 3, 30.0
        """
        sql = f"""
            WITH data AS ({data_sql})
            SELECT id, target, ROUND(x, 4) AS x FROM data
            DECIDE x(REAL)
            SUCH THAT x >= 0 AND x <= 100 AND SUM(x) = 60
            MINIMIZE SUM(POWER(x - target, 2))
        """
        t0 = time.perf_counter()
        rows, cols = decidb_cli.execute(sql)
        decidb_time = time.perf_counter() - t0
        data = duckdb_conn.execute(
            f"SELECT CAST(id AS BIGINT), CAST(target AS DOUBLE) FROM ({data_sql})"
        ).fetchall()

        t_build = time.perf_counter()
        oracle_solver.create_model("qp_aggregate")
        n = len(data)
        xnames = [f"x_{i}" for i in range(n)]
        for xn in xnames:
            oracle_solver.add_variable(xn, VarType.CONTINUOUS, lb=0.0, ub=100.0)
        oracle_solver.add_constraint(
            {xn: 1.0 for xn in xnames}, "=", 60.0, name="sum_fix",
        )
        oracle_solver.set_quadratic_objective(
            {xnames[i]: -2.0 * data[i][1] for i in range(n)},
            {(xnames[i], xnames[i]): 1.0 for i in range(n)},
            ObjSense.MINIMIZE,
        )
        build_time = time.perf_counter() - t_build
        result = _solve_and_compare(
            oracle_solver, rows, cols, lambda rs, cs: _qp_target_obj(rs, cs),
        )
        perf_tracker.record(
            "qp_aggregate_constraint", decidb_time, build_time,
            result.solve_time_seconds, n, n, 1,
            result.objective_value, oracle_solver.solver_name(),
            comparison_status="optimal",
        )

    def test_qp_simple_squared_variable(
        self, decidb_cli, duckdb_conn, oracle_solver, perf_tracker
    ):
        """MINIMIZE SUM(POWER(x, 2)) — pure squared variable, optimum at 0."""
        data_sql = "SELECT 1 AS id UNION ALL SELECT 2 UNION ALL SELECT 3"
        sql = f"""
            WITH data AS ({data_sql})
            SELECT id, ROUND(x, 4) AS x FROM data
            DECIDE x(REAL)
            SUCH THAT x >= 0 AND x <= 100
            MINIMIZE SUM(POWER(x, 2))
        """
        t0 = time.perf_counter()
        rows, cols = decidb_cli.execute(sql)
        decidb_time = time.perf_counter() - t0
        data = duckdb_conn.execute(
            f"SELECT CAST(id AS BIGINT) FROM ({data_sql})"
        ).fetchall()

        t_build = time.perf_counter()
        oracle_solver.create_model("qp_squared")
        n = len(data)
        xnames = [f"x_{i}" for i in range(n)]
        for xn in xnames:
            oracle_solver.add_variable(xn, VarType.CONTINUOUS, lb=0.0, ub=100.0)
        oracle_solver.set_quadratic_objective(
            {}, {(xn, xn): 1.0 for xn in xnames}, ObjSense.MINIMIZE,
        )
        build_time = time.perf_counter() - t_build
        result = _solve_and_compare(
            oracle_solver, rows, cols, lambda rs, cs: _qp_simple_obj(rs, cs),
        )
        perf_tracker.record(
            "qp_squared_simple", decidb_time, build_time,
            result.solve_time_seconds, n, n, 0,
            result.objective_value, oracle_solver.solver_name(),
            comparison_status="optimal",
        )

    def test_qp_with_when(
        self, decidb_cli, duckdb_conn, oracle_solver, perf_tracker
    ):
        """QP with WHEN — only grp='A' rows contribute to the objective."""
        data_sql = """
            SELECT 1 AS id, 10.0 AS target, 'A' AS grp UNION ALL
            SELECT 2, 20.0, 'B' UNION ALL
            SELECT 3, 30.0, 'A' UNION ALL
            SELECT 4, 40.0, 'B'
        """
        sql = f"""
            WITH data AS ({data_sql})
            SELECT id, target, grp, ROUND(x, 4) AS x FROM data
            DECIDE x(REAL)
            SUCH THAT x >= 0 AND x <= 100
            MINIMIZE SUM(POWER(x - target, 2)) WHEN grp = 'A'
        """
        t0 = time.perf_counter()
        rows, cols = decidb_cli.execute(sql)
        decidb_time = time.perf_counter() - t0
        data = duckdb_conn.execute(
            f"SELECT CAST(id AS BIGINT), CAST(target AS DOUBLE), CAST(grp AS VARCHAR) FROM ({data_sql})"
        ).fetchall()

        t_build = time.perf_counter()
        oracle_solver.create_model("qp_when")
        n = len(data)
        xnames = [f"x_{i}" for i in range(n)]
        for xn in xnames:
            oracle_solver.add_variable(xn, VarType.CONTINUOUS, lb=0.0, ub=100.0)
        # Only grp='A' rows appear in linear/quadratic; grp='B' rows are free.
        linear, quadratic = {}, {}
        for i, row in enumerate(data):
            if row[2] == "A":
                linear[xnames[i]] = -2.0 * row[1]
                quadratic[(xnames[i], xnames[i])] = 1.0
        oracle_solver.set_quadratic_objective(linear, quadratic, ObjSense.MINIMIZE)
        build_time = time.perf_counter() - t_build
        grp_idx = cols.index("grp")
        result = _solve_and_compare(
            oracle_solver, rows, cols,
            lambda rs, cs: _qp_target_obj(
                rs, cs, mask_fn=lambda r: r[grp_idx] == "A",
            ),
        )
        perf_tracker.record(
            "qp_with_when", decidb_time, build_time, result.solve_time_seconds,
            n, n, 0, result.objective_value, oracle_solver.solver_name(),
            comparison_status="optimal",
        )

    def test_qp_multiple_variables(
        self, decidb_cli, duckdb_conn, oracle_solver, perf_tracker
    ):
        """Two REAL vars; only x appears in the QP objective. y is unconstrained by Q."""
        data_sql = """
            SELECT 1 AS id, 10.0 AS target, 50.0 AS cap UNION ALL
            SELECT 2, 20.0, 50.0 UNION ALL
            SELECT 3, 30.0, 50.0
        """
        sql = f"""
            WITH data AS ({data_sql})
            SELECT id, target, ROUND(x, 4) AS x, ROUND(y, 4) AS y
            FROM data
            DECIDE x(REAL), y(REAL)
            SUCH THAT x >= 0 AND x <= 100
                AND y >= 0 AND y <= cap
            MINIMIZE SUM(POWER(x - target, 2))
        """
        t0 = time.perf_counter()
        rows, cols = decidb_cli.execute(sql)
        decidb_time = time.perf_counter() - t0
        data = duckdb_conn.execute(
            f"SELECT CAST(id AS BIGINT), CAST(target AS DOUBLE), CAST(cap AS DOUBLE) FROM ({data_sql})"
        ).fetchall()

        t_build = time.perf_counter()
        oracle_solver.create_model("qp_multivar")
        n = len(data)
        for i, row in enumerate(data):
            oracle_solver.add_variable(f"x_{i}", VarType.CONTINUOUS, lb=0.0, ub=100.0)
            oracle_solver.add_variable(f"y_{i}", VarType.CONTINUOUS, lb=0.0, ub=row[2])
        linear = {f"x_{i}": -2.0 * data[i][1] for i in range(n)}
        quadratic = {(f"x_{i}", f"x_{i}"): 1.0 for i in range(n)}
        oracle_solver.set_quadratic_objective(linear, quadratic, ObjSense.MINIMIZE)
        build_time = time.perf_counter() - t_build
        result = _solve_and_compare(
            oracle_solver, rows, cols, lambda rs, cs: _qp_target_obj(rs, cs),
        )
        perf_tracker.record(
            "qp_multivar", decidb_time, build_time, result.solve_time_seconds,
            n, 2 * n, 0, result.objective_value, oracle_solver.solver_name(),
            comparison_status="optimal",
        )

    def test_qp_power_with_constant_division(
        self, decidb_cli, duckdb_conn, oracle_solver, perf_tracker
    ):
        """MINIMIZE SUM(POWER(x/2 - 1, 2)).

        Expansion: (x/2 - 1)^2 = x^2/4 - x + 1, so per-row linear coeff = -1
        and quadratic coeff = 0.25. Unconstrained optimum at x = 2 in [0, 10].
        """
        data_sql = (
            "SELECT 1 AS id UNION ALL SELECT 2 UNION ALL SELECT 3"
        )
        sql = f"""
            WITH data AS ({data_sql})
            SELECT id, ROUND(x, 4) AS x FROM data
            DECIDE x(REAL)
            SUCH THAT x >= 0 AND x <= 10
            MINIMIZE SUM(POWER(x/2 - 1, 2))
        """
        t0 = time.perf_counter()
        rows, cols = decidb_cli.execute(sql)
        decidb_time = time.perf_counter() - t0
        data = duckdb_conn.execute(
            f"SELECT CAST(id AS BIGINT) FROM ({data_sql})"
        ).fetchall()

        t_build = time.perf_counter()
        oracle_solver.create_model("qp_const_div")
        n = len(data)
        xnames = [f"x_{i}" for i in range(n)]
        for xn in xnames:
            oracle_solver.add_variable(xn, VarType.CONTINUOUS, lb=0.0, ub=10.0)
        linear = {xnames[i]: -1.0 for i in range(n)}
        quadratic = {(xnames[i], xnames[i]): 0.25 for i in range(n)}
        oracle_solver.set_quadratic_objective(linear, quadratic, ObjSense.MINIMIZE)
        build_time = time.perf_counter() - t_build

        def decidb_obj(rs, cs):
            xi = cs.index("x")
            # Match oracle: drop the per-row constant +1 (oracle's
            # set_quadratic_objective ignores the additive constant).
            return sum(0.25 * float(r[xi]) ** 2 - float(r[xi]) for r in rs)

        result = _solve_and_compare(oracle_solver, rows, cols, decidb_obj)
        perf_tracker.record(
            "qp_power_const_div", decidb_time, build_time,
            result.solve_time_seconds, n, n, 0,
            result.objective_value, oracle_solver.solver_name(),
            comparison_status="optimal",
        )

    def test_qp_power_with_column_division(
        self, decidb_cli, duckdb_conn, oracle_solver, perf_tracker
    ):
        """MINIMIZE SUM(POWER(x/w - 1, 2)) with per-row data divisor w.

        Per-row expansion: (x/w - 1)^2 = x^2/w^2 - 2x/w + 1, so linear
        coeff = -2/w_i, quadratic coeff = 1/w_i^2. Unconstrained optimum
        at x_i = w_i.
        """
        data_sql = """
            SELECT 1 AS id, 2.0 AS w UNION ALL
            SELECT 2, 4.0 UNION ALL
            SELECT 3, 0.5
        """
        sql = f"""
            WITH data AS ({data_sql})
            SELECT id, w, ROUND(x, 4) AS x FROM data
            DECIDE x(REAL)
            SUCH THAT x >= 0 AND x <= 10
            MINIMIZE SUM(POWER(x/w - 1, 2))
        """
        t0 = time.perf_counter()
        rows, cols = decidb_cli.execute(sql)
        decidb_time = time.perf_counter() - t0
        data = duckdb_conn.execute(
            f"SELECT CAST(id AS BIGINT), CAST(w AS DOUBLE) FROM ({data_sql})"
        ).fetchall()

        t_build = time.perf_counter()
        oracle_solver.create_model("qp_col_div")
        n = len(data)
        xnames = [f"x_{i}" for i in range(n)]
        for xn in xnames:
            oracle_solver.add_variable(xn, VarType.CONTINUOUS, lb=0.0, ub=10.0)
        linear = {xnames[i]: -2.0 / data[i][1] for i in range(n)}
        quadratic = {(xnames[i], xnames[i]): 1.0 / (data[i][1] ** 2) for i in range(n)}
        oracle_solver.set_quadratic_objective(linear, quadratic, ObjSense.MINIMIZE)
        build_time = time.perf_counter() - t_build

        def decidb_obj(rs, cs):
            xi = cs.index("x"); wi = cs.index("w")
            return sum(
                (float(r[xi]) ** 2) / (float(r[wi]) ** 2)
                - 2.0 * float(r[xi]) / float(r[wi])
                for r in rs
            )

        result = _solve_and_compare(oracle_solver, rows, cols, decidb_obj)
        perf_tracker.record(
            "qp_power_col_div", decidb_time, build_time,
            result.solve_time_seconds, n, n, 0,
            result.objective_value, oracle_solver.solver_name(),
            comparison_status="optimal",
        )


# ===========================================================================
# MAXIMIZE quadratic
# ===========================================================================

@pytest.mark.quadratic
@pytest.mark.obj_maximize
@pytest.mark.correctness
class TestQuadraticMaximize:
    """MAXIMIZE with quadratic: Case A (negated, concave) and Case B (non-convex)."""

    def _solve_case_a(self, oracle_solver, data, *, scale=-1.0, linear_target_factor=2.0,
                      bounds=(0.0, 100.0), var_type=VarType.CONTINUOUS,
                      extra_constraint=None):
        """Shared setup for Case A: MAXIMIZE scale * SUM(POWER(x - t, 2))
        expansions. ``scale`` is -1 for the standard concave form, -0.5 and
        -2 for the fractional-coefficient variants."""
        n = len(data)
        xnames = [f"x_{i}" for i in range(n)]
        for xn in xnames:
            oracle_solver.add_variable(xn, var_type, lb=bounds[0], ub=bounds[1])
        if extra_constraint is not None:
            extra_constraint(oracle_solver, xnames)
        linear = {xnames[i]: -scale * linear_target_factor * data[i][1] for i in range(n)}
        quadratic = {(xnames[i], xnames[i]): scale for i in range(n)}
        oracle_solver.set_quadratic_objective(linear, quadratic, ObjSense.MAXIMIZE)
        return xnames

    def test_maximize_negated_power_case_a(
        self, decidb_cli, duckdb_conn, oracle_solver, perf_tracker
    ):
        data_sql = """
            SELECT 1 AS id, 10.0 AS target UNION ALL
            SELECT 2, 20.0 UNION ALL
            SELECT 3, 30.0
        """
        sql = f"""
            WITH data AS ({data_sql})
            SELECT id, target, ROUND(x, 4) AS x FROM data
            DECIDE x(REAL)
            SUCH THAT x >= 0 AND x <= 100
            MAXIMIZE SUM(-POWER(x - target, 2))
        """
        t0 = time.perf_counter()
        rows, cols = decidb_cli.execute(sql)
        decidb_time = time.perf_counter() - t0
        data = duckdb_conn.execute(
            f"SELECT CAST(id AS BIGINT), CAST(target AS DOUBLE) FROM ({data_sql})"
        ).fetchall()

        t_build = time.perf_counter()
        oracle_solver.create_model("qp_max_case_a")
        self._solve_case_a(oracle_solver, data)
        build_time = time.perf_counter() - t_build
        result = _solve_and_compare(
            oracle_solver, rows, cols,
            lambda rs, cs: _qp_target_obj(rs, cs, scale=-1.0),
        )
        perf_tracker.record(
            "qp_max_case_a", decidb_time, build_time, result.solve_time_seconds,
            len(data), len(data), 0,
            result.objective_value, oracle_solver.solver_name(),
            comparison_status="optimal",
        )

    def test_maximize_negated_power_with_binding_constraint(
        self, decidb_cli, duckdb_conn, oracle_solver, perf_tracker
    ):
        data_sql = "SELECT 1 AS id, 5.0 AS target UNION ALL SELECT 2, 50.0"
        sql = f"""
            WITH data AS ({data_sql})
            SELECT id, target, ROUND(x, 4) AS x FROM data
            DECIDE x(REAL)
            SUCH THAT x >= 10 AND x <= 40
            MAXIMIZE SUM(-POWER(x - target, 2))
        """
        t0 = time.perf_counter()
        rows, cols = decidb_cli.execute(sql)
        decidb_time = time.perf_counter() - t0
        data = duckdb_conn.execute(
            f"SELECT CAST(id AS BIGINT), CAST(target AS DOUBLE) FROM ({data_sql})"
        ).fetchall()

        t_build = time.perf_counter()
        oracle_solver.create_model("qp_max_case_a_binding")
        self._solve_case_a(oracle_solver, data, bounds=(10.0, 40.0))
        build_time = time.perf_counter() - t_build
        result = _solve_and_compare(
            oracle_solver, rows, cols,
            lambda rs, cs: _qp_target_obj(rs, cs, scale=-1.0),
        )
        perf_tracker.record(
            "qp_max_case_a_binding", decidb_time, build_time,
            result.solve_time_seconds, len(data), len(data), 2,
            result.objective_value, oracle_solver.solver_name(),
            comparison_status="optimal",
        )

    def test_maximize_negated_starstar_syntax(
        self, decidb_cli, duckdb_conn, oracle_solver, perf_tracker
    ):
        data_sql = "SELECT 1 AS id, 15.0 AS target UNION ALL SELECT 2, 25.0"
        sql = f"""
            WITH data AS ({data_sql})
            SELECT id, target, ROUND(x, 4) AS x FROM data
            DECIDE x(REAL)
            SUCH THAT x >= 0 AND x <= 100
            MAXIMIZE SUM(-((x - target) ** 2))
        """
        t0 = time.perf_counter()
        rows, cols = decidb_cli.execute(sql)
        decidb_time = time.perf_counter() - t0
        data = duckdb_conn.execute(
            f"SELECT CAST(id AS BIGINT), CAST(target AS DOUBLE) FROM ({data_sql})"
        ).fetchall()

        t_build = time.perf_counter()
        oracle_solver.create_model("qp_max_starstar")
        self._solve_case_a(oracle_solver, data)
        build_time = time.perf_counter() - t_build
        result = _solve_and_compare(
            oracle_solver, rows, cols,
            lambda rs, cs: _qp_target_obj(rs, cs, scale=-1.0),
        )
        perf_tracker.record(
            "qp_max_starstar", decidb_time, build_time,
            result.solve_time_seconds, len(data), len(data), 0,
            result.objective_value, oracle_solver.solver_name(),
            comparison_status="optimal",
        )

    def test_maximize_convex_power_case_b(
        self, decidb_cli, duckdb_conn, oracle_solver, perf_tracker
    ):
        """Case B (non-convex, Gurobi-only): MAXIMIZE SUM(POWER(x, 2))."""
        data_sql = "SELECT 1 AS id UNION ALL SELECT 2 UNION ALL SELECT 3"
        sql = f"""
            WITH data AS ({data_sql})
            SELECT id, ROUND(x, 4) AS x FROM data
            DECIDE x(REAL)
            SUCH THAT x >= 0 AND x <= 10
            MAXIMIZE SUM(POWER(x, 2))
        """
        try:
            t0 = time.perf_counter()
            rows, cols = decidb_cli.execute(sql)
            decidb_time = time.perf_counter() - t0
        except DecidBCliError as e:
            assert re.search(r"Non-convex quadratic objectives require Gurobi", e.message)
            return

        data = duckdb_conn.execute(
            f"SELECT CAST(id AS BIGINT) FROM ({data_sql})"
        ).fetchall()
        t_build = time.perf_counter()
        oracle_solver.create_model("qp_max_case_b")
        n = len(data)
        xnames = [f"x_{i}" for i in range(n)]
        for xn in xnames:
            oracle_solver.add_variable(xn, VarType.CONTINUOUS, lb=0.0, ub=10.0)
        oracle_solver.set_quadratic_objective(
            {}, {(xn, xn): 1.0 for xn in xnames}, ObjSense.MAXIMIZE,
        )
        build_time = time.perf_counter() - t_build
        result = _solve_and_compare(
            oracle_solver, rows, cols, lambda rs, cs: _qp_simple_obj(rs, cs),
        )
        perf_tracker.record(
            "qp_max_case_b", decidb_time, build_time, result.solve_time_seconds,
            n, n, 0, result.objective_value, oracle_solver.solver_name(),
            comparison_status="optimal",
        )

    def test_maximize_convex_power_integer_case_b(
        self, decidb_cli, duckdb_conn, oracle_solver, perf_tracker
    ):
        """Non-convex MIQP: MAXIMIZE SUM(POWER(x - 5, 2)) with INTEGER x."""
        data_sql = "SELECT 1 AS id UNION ALL SELECT 2"
        sql = f"""
            WITH data AS ({data_sql})
            SELECT id, x FROM data
            DECIDE x(INT)
            SUCH THAT x >= 0 AND x <= 10
            MAXIMIZE SUM(POWER(x - 5, 2))
        """
        try:
            t0 = time.perf_counter()
            rows, cols = decidb_cli.execute(sql)
            decidb_time = time.perf_counter() - t0
        except DecidBCliError as e:
            assert re.search(r"Non-convex quadratic objectives require Gurobi", e.message)
            return

        data = duckdb_conn.execute(
            f"SELECT CAST(id AS BIGINT) FROM ({data_sql})"
        ).fetchall()
        t_build = time.perf_counter()
        oracle_solver.create_model("qp_max_case_b_int")
        n = len(data)
        xnames = [f"x_{i}" for i in range(n)]
        for xn in xnames:
            oracle_solver.add_variable(xn, VarType.INTEGER, lb=0.0, ub=10.0)
        # POWER(x - 5, 2) = x^2 - 10 x + 25; drop the +25 constant.
        oracle_solver.set_quadratic_objective(
            {xn: -10.0 for xn in xnames},
            {(xn, xn): 1.0 for xn in xnames},
            ObjSense.MAXIMIZE,
        )
        build_time = time.perf_counter() - t_build

        def decidb_obj(rs, cs):
            xi = cs.index("x")
            return sum(
                (float(row[xi]) - 5.0) ** 2 - 25.0 for row in rs
            )

        result = _solve_and_compare(oracle_solver, rows, cols, decidb_obj)
        perf_tracker.record(
            "qp_max_case_b_int", decidb_time, build_time,
            result.solve_time_seconds, n, n, 0,
            result.objective_value, oracle_solver.solver_name(),
            comparison_status="optimal",
        )

    def test_maximize_negated_coefficient_not_one(
        self, decidb_cli, duckdb_conn, oracle_solver, perf_tracker
    ):
        """Coefficient -2 on POWER(x - t, 2) must be preserved."""
        data_sql = """
            SELECT 1 AS id, 10.0 AS target UNION ALL
            SELECT 2, 20.0 UNION ALL
            SELECT 3, 30.0
        """
        sql = f"""
            WITH data AS ({data_sql})
            SELECT id, target, ROUND(x, 4) AS x FROM data
            DECIDE x(REAL)
            SUCH THAT x >= 0 AND x <= 100
            MAXIMIZE SUM((-2) * POWER(x - target, 2))
        """
        t0 = time.perf_counter()
        rows, cols = decidb_cli.execute(sql)
        decidb_time = time.perf_counter() - t0
        data = duckdb_conn.execute(
            f"SELECT CAST(id AS BIGINT), CAST(target AS DOUBLE) FROM ({data_sql})"
        ).fetchall()

        t_build = time.perf_counter()
        oracle_solver.create_model("qp_max_coef_neg2")
        self._solve_case_a(oracle_solver, data, scale=-2.0, linear_target_factor=2.0)
        build_time = time.perf_counter() - t_build
        result = _solve_and_compare(
            oracle_solver, rows, cols,
            lambda rs, cs: _qp_target_obj(rs, cs, scale=-2.0),
        )
        perf_tracker.record(
            "qp_max_coef_neg2", decidb_time, build_time,
            result.solve_time_seconds, len(data), len(data), 0,
            result.objective_value, oracle_solver.solver_name(),
            comparison_status="optimal",
        )

    def test_maximize_half_negated_coefficient(
        self, decidb_cli, duckdb_conn, oracle_solver, perf_tracker
    ):
        """Fractional coefficient -0.5 with binding constraint SUM(x)=90."""
        data_sql = """
            SELECT 1 AS id, 10.0 AS target UNION ALL
            SELECT 2, 20.0 UNION ALL
            SELECT 3, 30.0
        """
        sql = f"""
            WITH data AS ({data_sql})
            SELECT id, target, ROUND(x, 4) AS x FROM data
            DECIDE x(REAL)
            SUCH THAT x >= 0 AND x <= 100 AND SUM(x) = 90
            MAXIMIZE SUM((-0.5) * POWER(x - target, 2))
        """
        t0 = time.perf_counter()
        rows, cols = decidb_cli.execute(sql)
        decidb_time = time.perf_counter() - t0
        data = duckdb_conn.execute(
            f"SELECT CAST(id AS BIGINT), CAST(target AS DOUBLE) FROM ({data_sql})"
        ).fetchall()

        t_build = time.perf_counter()
        oracle_solver.create_model("qp_max_coef_halfneg")

        def add_sum_eq_90(oracle, xns):
            oracle.add_constraint({xn: 1.0 for xn in xns}, "=", 90.0, name="sum_90")

        self._solve_case_a(
            oracle_solver, data, scale=-0.5, linear_target_factor=2.0,
            extra_constraint=add_sum_eq_90,
        )
        build_time = time.perf_counter() - t_build
        result = _solve_and_compare(
            oracle_solver, rows, cols,
            lambda rs, cs: _qp_target_obj(rs, cs, scale=-0.5),
        )
        perf_tracker.record(
            "qp_max_coef_halfneg", decidb_time, build_time,
            result.solve_time_seconds, len(data), len(data), 1,
            result.objective_value, oracle_solver.solver_name(),
            comparison_status="optimal",
        )

    def test_maximize_power_times_neg_one_rhs(
        self, decidb_cli, duckdb_conn, oracle_solver, perf_tracker
    ):
        """POWER(x - t, 2) * (-1) — negation on RHS of multiply."""
        data_sql = "SELECT 1 AS id, 15.0 AS target UNION ALL SELECT 2, 25.0"
        sql = f"""
            WITH data AS ({data_sql})
            SELECT id, target, ROUND(x, 4) AS x FROM data
            DECIDE x(REAL)
            SUCH THAT x >= 0 AND x <= 100
            MAXIMIZE SUM(POWER(x - target, 2) * (-1))
        """
        t0 = time.perf_counter()
        rows, cols = decidb_cli.execute(sql)
        decidb_time = time.perf_counter() - t0
        data = duckdb_conn.execute(
            f"SELECT CAST(id AS BIGINT), CAST(target AS DOUBLE) FROM ({data_sql})"
        ).fetchall()

        t_build = time.perf_counter()
        oracle_solver.create_model("qp_max_neg_rhs")
        self._solve_case_a(oracle_solver, data)
        build_time = time.perf_counter() - t_build
        result = _solve_and_compare(
            oracle_solver, rows, cols,
            lambda rs, cs: _qp_target_obj(rs, cs, scale=-1.0),
        )
        perf_tracker.record(
            "qp_max_neg_rhs", decidb_time, build_time,
            result.solve_time_seconds, len(data), len(data), 0,
            result.objective_value, oracle_solver.solver_name(),
            comparison_status="optimal",
        )

    def test_maximize_negated_multiplication_form(
        self, decidb_cli, duckdb_conn, oracle_solver, perf_tracker
    ):
        """-((x-t) * (x-t)) — negated identical-child multiply."""
        data_sql = "SELECT 1 AS id, 12.0 AS target UNION ALL SELECT 2, 35.0"
        sql = f"""
            WITH data AS ({data_sql})
            SELECT id, target, ROUND(x, 4) AS x FROM data
            DECIDE x(REAL)
            SUCH THAT x >= 0 AND x <= 100
            MAXIMIZE SUM(-((x - target) * (x - target)))
        """
        t0 = time.perf_counter()
        rows, cols = decidb_cli.execute(sql)
        decidb_time = time.perf_counter() - t0
        data = duckdb_conn.execute(
            f"SELECT CAST(id AS BIGINT), CAST(target AS DOUBLE) FROM ({data_sql})"
        ).fetchall()

        t_build = time.perf_counter()
        oracle_solver.create_model("qp_max_neg_mult")
        self._solve_case_a(oracle_solver, data)
        build_time = time.perf_counter() - t_build
        result = _solve_and_compare(
            oracle_solver, rows, cols,
            lambda rs, cs: _qp_target_obj(rs, cs, scale=-1.0),
        )
        perf_tracker.record(
            "qp_max_neg_mult", decidb_time, build_time,
            result.solve_time_seconds, len(data), len(data), 0,
            result.objective_value, oracle_solver.solver_name(),
            comparison_status="optimal",
        )

    def test_maximize_negated_power_integer_case_a(
        self, decidb_cli, duckdb_conn, oracle_solver, perf_tracker
    ):
        """Case A with INTEGER (MIQP) — Gurobi required, HiGHS rejects."""
        data_sql = """
            SELECT 1 AS id, 3 AS target UNION ALL
            SELECT 2, 7 UNION ALL
            SELECT 3, 15
        """
        sql = f"""
            WITH data AS ({data_sql})
            SELECT id, target, x FROM data
            DECIDE x(INT)
            SUCH THAT x >= 0 AND x <= 20
            MAXIMIZE SUM(-POWER(x - target, 2))
        """
        try:
            t0 = time.perf_counter()
            rows, cols = decidb_cli.execute(sql)
            decidb_time = time.perf_counter() - t0
        except DecidBCliError as e:
            assert re.search(
                r"MIQP.*require Gurobi|integer.*require Gurobi",
                e.message, re.IGNORECASE,
            )
            return

        data = duckdb_conn.execute(
            f"SELECT CAST(id AS BIGINT), CAST(target AS DOUBLE) FROM ({data_sql})"
        ).fetchall()
        t_build = time.perf_counter()
        oracle_solver.create_model("qp_max_int_case_a")
        self._solve_case_a(
            oracle_solver, data,
            bounds=(0.0, 20.0), var_type=VarType.INTEGER,
        )
        build_time = time.perf_counter() - t_build
        result = _solve_and_compare(
            oracle_solver, rows, cols,
            lambda rs, cs: _qp_target_obj(rs, cs, scale=-1.0),
        )
        perf_tracker.record(
            "qp_max_int_case_a", decidb_time, build_time,
            result.solve_time_seconds, len(data), len(data), 0,
            result.objective_value, oracle_solver.solver_name(),
            comparison_status="optimal",
        )

    def test_maximize_negated_power_with_sum_constraint(
        self, decidb_cli, duckdb_conn, oracle_solver, perf_tracker
    ):
        """Case A with aggregate constraint SUM(x)=60 (not binding)."""
        data_sql = """
            SELECT 1 AS id, 10.0 AS target UNION ALL
            SELECT 2, 20.0 UNION ALL
            SELECT 3, 30.0
        """
        sql = f"""
            WITH data AS ({data_sql})
            SELECT id, target, ROUND(x, 4) AS x FROM data
            DECIDE x(REAL)
            SUCH THAT x >= 0 AND x <= 100 AND SUM(x) = 60
            MAXIMIZE SUM(-POWER(x - target, 2))
        """
        t0 = time.perf_counter()
        rows, cols = decidb_cli.execute(sql)
        decidb_time = time.perf_counter() - t0
        data = duckdb_conn.execute(
            f"SELECT CAST(id AS BIGINT), CAST(target AS DOUBLE) FROM ({data_sql})"
        ).fetchall()

        t_build = time.perf_counter()
        oracle_solver.create_model("qp_max_case_a_sum")

        def sum60(oracle, xns):
            oracle.add_constraint({xn: 1.0 for xn in xns}, "=", 60.0, name="sum60")

        self._solve_case_a(oracle_solver, data, extra_constraint=sum60)
        build_time = time.perf_counter() - t_build
        result = _solve_and_compare(
            oracle_solver, rows, cols,
            lambda rs, cs: _qp_target_obj(rs, cs, scale=-1.0),
        )
        perf_tracker.record(
            "qp_max_case_a_sum", decidb_time, build_time,
            result.solve_time_seconds, len(data), len(data), 1,
            result.objective_value, oracle_solver.solver_name(),
            comparison_status="optimal",
        )


# ===========================================================================
# Error cases (no oracle)
# ===========================================================================

@pytest.mark.error
@pytest.mark.quadratic
class TestQuadraticErrors:
    """Queries rejected by the binder or physical operator."""

    def test_power_exponent_3_rejected(self, decidb_cli):
        decidb_cli.assert_error("""
            WITH data AS (SELECT 1 AS id, 10.0 AS target)
            SELECT id, x FROM data
            DECIDE x(REAL)
            SUCH THAT x >= 0 AND x <= 100
            MINIMIZE SUM(POWER(x - target, 3))
        """, match=r"Only POWER\(expr, 2\) is supported|Higher powers are not allowed")

    def test_product_of_different_vars_now_supported(
        self, decidb_cli, duckdb_conn, oracle_solver, perf_tracker
    ):
        """x * y bilinear: now accepted (Gurobi) or rejected (HiGHS)."""
        sql = """
            WITH data AS (SELECT 1 AS id, 10.0 AS val)
            SELECT id, ROUND(x, 2) AS x, ROUND(y, 2) AS y FROM data
            DECIDE x(REAL), y(REAL)
            SUCH THAT x >= 0 AND x <= 10 AND y >= 0 AND y <= 10
            MINIMIZE SUM(x * y)
        """
        try:
            t0 = time.perf_counter()
            rows, cols = decidb_cli.execute(sql)
            decidb_time = time.perf_counter() - t0
        except DecidBCliError as e:
            assert re.search(r"Non-convex|require Gurobi", e.message)
            return

        # Oracle: minimize x*y with both in [0,10].
        t_build = time.perf_counter()
        oracle_solver.create_model("bilinear_min")
        oracle_solver.add_variable("x", VarType.CONTINUOUS, lb=0.0, ub=10.0)
        oracle_solver.add_variable("y", VarType.CONTINUOUS, lb=0.0, ub=10.0)
        oracle_solver.set_quadratic_objective(
            {}, {("x", "y"): 1.0}, ObjSense.MINIMIZE,
        )
        build_time = time.perf_counter() - t_build

        def decidb_obj(rs, cs):
            xi = cs.index("x"); yi = cs.index("y")
            return float(rs[0][xi]) * float(rs[0][yi])

        result = _solve_and_compare(oracle_solver, rows, cols, decidb_obj)
        perf_tracker.record(
            "qp_bilinear_min", decidb_time, build_time,
            result.solve_time_seconds, 1, 2, 0,
            result.objective_value, oracle_solver.solver_name(),
            comparison_status="optimal",
        )

    def test_minimize_negated_power_nonconvex(
        self, decidb_cli, duckdb_conn, oracle_solver, perf_tracker
    ):
        """MINIMIZE -POWER(x, 2) is non-convex; Gurobi pushes x to boundary."""
        sql = """
            WITH data AS (
                SELECT 1 AS id UNION ALL SELECT 2 UNION ALL SELECT 3
            )
            SELECT id, ROUND(x, 4) AS x FROM data
            DECIDE x(REAL)
            SUCH THAT x >= 0 AND x <= 10
            MINIMIZE SUM(-POWER(x, 2))
        """
        try:
            t0 = time.perf_counter()
            rows, cols = decidb_cli.execute(sql)
            decidb_time = time.perf_counter() - t0
        except DecidBCliError as e:
            assert re.search(r"Non-convex quadratic objectives require Gurobi", e.message)
            return

        t_build = time.perf_counter()
        oracle_solver.create_model("qp_min_neg_nonconvex")
        n = len(rows)
        xnames = [f"x_{i}" for i in range(n)]
        for xn in xnames:
            oracle_solver.add_variable(xn, VarType.CONTINUOUS, lb=0.0, ub=10.0)
        oracle_solver.set_quadratic_objective(
            {}, {(xn, xn): -1.0 for xn in xnames}, ObjSense.MINIMIZE,
        )
        build_time = time.perf_counter() - t_build

        def decidb_obj(rs, cs):
            xi = cs.index("x")
            return sum(-float(row[xi]) ** 2 for row in rs)

        result = _solve_and_compare(oracle_solver, rows, cols, decidb_obj)
        perf_tracker.record(
            "qp_min_neg_nonconvex", decidb_time, build_time,
            result.solve_time_seconds, n, n, 0,
            result.objective_value, oracle_solver.solver_name(),
            comparison_status="optimal",
        )

    def test_power_with_variable_exponent_rejected(self, decidb_cli):
        decidb_cli.assert_error("""
            WITH data AS (SELECT 1 AS id, 10.0 AS target, 2 AS exp)
            SELECT id, x FROM data
            DECIDE x(REAL)
            SUCH THAT x >= 0 AND x <= 100
            MINIMIZE SUM(POWER(x - target, exp))
        """, match=r"POWER exponent.*must be a constant integer")

    def test_qp_multiple_quadratic_groups_rejected(self, decidb_cli):
        decidb_cli.assert_error("""
            WITH data AS (
                SELECT 1 AS id, 1.0 AS a, 2.0 AS b UNION ALL
                SELECT 2, 3.0, 4.0
            )
            SELECT id, ROUND(x, 4) AS x, ROUND(y, 4) AS y
            FROM data
            DECIDE x(REAL), y(REAL)
            SUCH THAT x >= 0 AND x <= 100 AND y >= 0 AND y <= 100
            MINIMIZE SUM(POWER(x - a, 2)) + SUM(POWER(y - b, 2))
        """, match=r"multiple quadratic")

    def test_qp_self_product_of_power_rejected(self, decidb_cli):
        # POWER(x, 2) * POWER(x, 2) = x^4 — identical-children self-product with
        # a quadratic inner must be rejected, not silently reduced to x^2.
        decidb_cli.assert_error("""
            WITH data AS (SELECT 1 AS id)
            SELECT id, ROUND(x, 4) AS x FROM data
            DECIDE x(REAL)
            SUCH THAT x >= 0 AND x <= 3
            MINIMIZE SUM(POWER(x - 1, 2) * POWER(x - 1, 2)) + SUM(-2 * x)
        """, match=r"self-product .* non-linear|degree > 2")

    def test_qp_product_of_two_powers_rejected(self, decidb_cli):
        # POWER(x, 2) * POWER(y, 2) = x^2 y^2 — falls into the bilinear branch
        # of ExtractLinearAndBilinearTerms with a degree-2 "coefficient"; must
        # be rejected before the coefficient evaluator touches it.
        decidb_cli.assert_error("""
            WITH data AS (SELECT 1 AS id)
            SELECT id, x, y FROM data
            DECIDE x(REAL), y(REAL)
            SUCH THAT x >= 0 AND x <= 100 AND y >= 0 AND y <= 100
            MINIMIZE SUM(POWER(x, 2) * POWER(y, 2))
        """, match=r"degree > 2")

    def test_qp_variable_times_power_rejected(self, decidb_cli):
        # a * POWER(x, 2) with a a DECIDE var — total degree 3; same bilinear
        # misclassification as (2) and previously crashed the engine.
        decidb_cli.assert_error("""
            WITH data AS (SELECT 1 AS id)
            SELECT id, a, x FROM data
            DECIDE a(REAL), x(REAL)
            SUCH THAT a >= 0 AND a <= 5 AND x >= 0 AND x <= 5
            MAXIMIZE SUM(a * POWER(x, 2))
        """, match=r"degree > 2")


# ===========================================================================
# TPC-H based QP tests — oracle-compared
# ===========================================================================

@pytest.mark.quadratic
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_qp_minimize_squared_deviation_tpch(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """Minimize squared deviation from ps_supplycost on partsupp (ps_partkey<=10)."""
    sql = """
        SELECT ps_partkey, ps_suppkey, ps_supplycost, ROUND(x, 4) AS x
        FROM partsupp
        WHERE ps_partkey <= 10
        DECIDE x(REAL)
        SUCH THAT x >= 0 AND x <= 1000
        MINIMIZE SUM(POWER(x - ps_supplycost, 2))
    """
    t0 = time.perf_counter()
    rows, cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0
    data = duckdb_conn.execute("""
        SELECT CAST(ps_partkey AS BIGINT), CAST(ps_suppkey AS BIGINT),
               CAST(ps_supplycost AS DOUBLE)
        FROM partsupp WHERE ps_partkey <= 10
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("qp_tpch_deviation")
    n = len(data)
    xnames = [f"x_{i}" for i in range(n)]
    for xn in xnames:
        oracle_solver.add_variable(xn, VarType.CONTINUOUS, lb=0.0, ub=1000.0)
    linear = {xnames[i]: -2.0 * data[i][2] for i in range(n)}
    quadratic = {(xnames[i], xnames[i]): 1.0 for i in range(n)}
    oracle_solver.set_quadratic_objective(linear, quadratic, ObjSense.MINIMIZE)
    build_time = time.perf_counter() - t_build

    cost_col = cols.index("ps_supplycost"); x_col = cols.index("x")

    def decidb_obj(rs, cs):
        return sum(
            (float(r[x_col]) - float(r[cost_col])) ** 2 - float(r[cost_col]) ** 2
            for r in rs
        )

    result = _solve_and_compare(oracle_solver, rows, cols, decidb_obj)
    perf_tracker.record(
        "qp_tpch_deviation", decidb_time, build_time, result.solve_time_seconds,
        n, n, 0, result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )


@pytest.mark.quadratic
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_qp_with_sum_constraint_tpch(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """Inflate total cost by 10% via SUM(x) >= 1.1 * total_cost."""
    total_cost = duckdb_conn.execute("""
        SELECT SUM(CAST(ps_supplycost AS DOUBLE))
        FROM partsupp WHERE ps_partkey <= 10
    """).fetchone()[0]
    inflated = round(total_cost * 1.1, 2)

    sql = f"""
        SELECT ps_partkey, ps_suppkey, ps_supplycost, ROUND(x, 4) AS x
        FROM partsupp
        WHERE ps_partkey <= 10
        DECIDE x(REAL)
        SUCH THAT x >= 0 AND x <= 10000 AND SUM(x) >= {inflated}
        MINIMIZE SUM(POWER(x - ps_supplycost, 2))
    """
    t0 = time.perf_counter()
    rows, cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0
    data = duckdb_conn.execute("""
        SELECT CAST(ps_partkey AS BIGINT), CAST(ps_suppkey AS BIGINT),
               CAST(ps_supplycost AS DOUBLE)
        FROM partsupp WHERE ps_partkey <= 10
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("qp_tpch_inflated")
    n = len(data)
    xnames = [f"x_{i}" for i in range(n)]
    for xn in xnames:
        oracle_solver.add_variable(xn, VarType.CONTINUOUS, lb=0.0, ub=10000.0)
    oracle_solver.add_constraint(
        {xn: 1.0 for xn in xnames}, ">=", inflated, name="sum_inflate",
    )
    linear = {xnames[i]: -2.0 * data[i][2] for i in range(n)}
    quadratic = {(xnames[i], xnames[i]): 1.0 for i in range(n)}
    oracle_solver.set_quadratic_objective(linear, quadratic, ObjSense.MINIMIZE)
    build_time = time.perf_counter() - t_build

    cost_col = cols.index("ps_supplycost"); x_col = cols.index("x")

    def decidb_obj(rs, cs):
        return sum(
            (float(r[x_col]) - float(r[cost_col])) ** 2 - float(r[cost_col]) ** 2
            for r in rs
        )

    result = _solve_and_compare(oracle_solver, rows, cols, decidb_obj)
    perf_tracker.record(
        "qp_tpch_sum_inflate", decidb_time, build_time, result.solve_time_seconds,
        n, n, 1, result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )


# ===========================================================================
# Mixed linear + quadratic objective — already oracle-verified, unchanged
# ===========================================================================
#
# DecidB extracts a mixed objective SUM(POWER(x-t, 2) + c*x) into
# ``squared_terms`` (Q matrix) and ``terms`` (linear c vector) simultaneously.
# The oracle mirrors this via ``set_quadratic_objective(linear, quadratic)``.

@pytest.mark.quadratic
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_qp_mixed_linear_quadratic(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """MINIMIZE SUM(POWER(x - target, 2) + penalty * x) — mixed inside one SUM."""
    sql = """
        WITH data AS (
            SELECT 1 AS id, 10.0 AS target, 4.0 AS penalty UNION ALL
            SELECT 2, 20.0, -6.0 UNION ALL
            SELECT 3, 30.0, 0.0
        )
        SELECT id, target, penalty, ROUND(x, 6) AS x
        FROM data
        DECIDE x(REAL)
        SUCH THAT x >= 0 AND x <= 100
        MINIMIZE SUM(POWER(x - target, 2) + penalty * x)
    """
    t0 = time.perf_counter()
    rows, cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0
    data = duckdb_conn.execute("""
        SELECT CAST(id AS BIGINT), CAST(target AS DOUBLE), CAST(penalty AS DOUBLE)
        FROM (
            SELECT 1 AS id, 10.0 AS target, 4.0 AS penalty UNION ALL
            SELECT 2, 20.0, -6.0 UNION ALL
            SELECT 3, 30.0, 0.0
        )
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("qp_mixed_linear_quadratic")
    n = len(data)
    xnames = [f"x_{i}" for i in range(n)]
    for xn in xnames:
        oracle_solver.add_variable(xn, VarType.CONTINUOUS, lb=0.0, ub=100.0)
    linear = {xnames[i]: (-2.0 * data[i][1] + data[i][2]) for i in range(n)}
    quadratic = {(xnames[i], xnames[i]): 1.0 for i in range(n)}
    oracle_solver.set_quadratic_objective(linear, quadratic, ObjSense.MINIMIZE)
    build_time = time.perf_counter() - t_build

    pen_col = cols.index("penalty")

    def decidb_obj(rs, cs):
        return _qp_target_obj(
            rs, cs, linear_coeff=lambda r: float(r[pen_col]),
        )

    result = _solve_and_compare(oracle_solver, rows, cols, decidb_obj)
    perf_tracker.record(
        "qp_mixed_linear_quadratic", decidb_time, build_time,
        result.solve_time_seconds, n, n, 0,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )


@pytest.mark.quadratic
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_qp_mixed_separate_sums(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """Same as test_qp_mixed_linear_quadratic but split across sibling SUMs."""
    sql = """
        WITH data AS (
            SELECT 1 AS id, 10.0 AS target, 4.0 AS penalty UNION ALL
            SELECT 2, 20.0, -6.0 UNION ALL
            SELECT 3, 30.0, 0.0
        )
        SELECT id, target, penalty, ROUND(x, 6) AS x
        FROM data
        DECIDE x(REAL)
        SUCH THAT x >= 0 AND x <= 100
        MINIMIZE SUM(POWER(x - target, 2)) + SUM(penalty * x)
    """
    t0 = time.perf_counter()
    rows, cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0
    data = duckdb_conn.execute("""
        SELECT CAST(id AS BIGINT), CAST(target AS DOUBLE), CAST(penalty AS DOUBLE)
        FROM (
            SELECT 1 AS id, 10.0 AS target, 4.0 AS penalty UNION ALL
            SELECT 2, 20.0, -6.0 UNION ALL
            SELECT 3, 30.0, 0.0
        )
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("qp_mixed_separate_sums")
    n = len(data)
    xnames = [f"x_{i}" for i in range(n)]
    for xn in xnames:
        oracle_solver.add_variable(xn, VarType.CONTINUOUS, lb=0.0, ub=100.0)
    linear = {xnames[i]: (-2.0 * data[i][1] + data[i][2]) for i in range(n)}
    quadratic = {(xnames[i], xnames[i]): 1.0 for i in range(n)}
    oracle_solver.set_quadratic_objective(linear, quadratic, ObjSense.MINIMIZE)
    build_time = time.perf_counter() - t_build

    pen_col = cols.index("penalty")

    def decidb_obj(rs, cs):
        return _qp_target_obj(
            rs, cs, linear_coeff=lambda r: float(r[pen_col]),
        )

    result = _solve_and_compare(oracle_solver, rows, cols, decidb_obj)
    perf_tracker.record(
        "qp_mixed_separate_sums", decidb_time, build_time,
        result.solve_time_seconds, n, n, 0,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )


@pytest.mark.quadratic
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_qp_mixed_negated_quadratic(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """MAXIMIZE SUM(-POWER(x - target, 2) + penalty * x) — concave mixed."""
    sql = """
        WITH data AS (
            SELECT 1 AS id, 10.0 AS target, 4.0 AS penalty UNION ALL
            SELECT 2, 20.0, -6.0 UNION ALL
            SELECT 3, 30.0, 2.0
        )
        SELECT id, target, penalty, ROUND(x, 6) AS x
        FROM data
        DECIDE x(REAL)
        SUCH THAT x >= 0 AND x <= 100
        MAXIMIZE SUM(-POWER(x - target, 2) + penalty * x)
    """
    t0 = time.perf_counter()
    rows, cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0
    data = duckdb_conn.execute("""
        SELECT CAST(id AS BIGINT), CAST(target AS DOUBLE), CAST(penalty AS DOUBLE)
        FROM (
            SELECT 1 AS id, 10.0 AS target, 4.0 AS penalty UNION ALL
            SELECT 2, 20.0, -6.0 UNION ALL
            SELECT 3, 30.0, 2.0
        )
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("qp_mixed_negated_quadratic")
    n = len(data)
    xnames = [f"x_{i}" for i in range(n)]
    for xn in xnames:
        oracle_solver.add_variable(xn, VarType.CONTINUOUS, lb=0.0, ub=100.0)
    linear = {xnames[i]: (2.0 * data[i][1] + data[i][2]) for i in range(n)}
    quadratic = {(xnames[i], xnames[i]): -1.0 for i in range(n)}
    oracle_solver.set_quadratic_objective(linear, quadratic, ObjSense.MAXIMIZE)
    build_time = time.perf_counter() - t_build

    pen_col = cols.index("penalty")

    def decidb_obj(rs, cs):
        return _qp_target_obj(
            rs, cs, scale=-1.0,
            linear_coeff=lambda r: float(r[pen_col]),
        )

    result = _solve_and_compare(oracle_solver, rows, cols, decidb_obj)
    perf_tracker.record(
        "qp_mixed_negated_quadratic", decidb_time, build_time,
        result.solve_time_seconds, n, n, 0,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )


# ===========================================================================
# Nested outer-SUM of aggregate-of-quadratic (PER on QP objective)
# ===========================================================================
# These tests exercise the nested-aggregate quadratic shape. Before the fix,
# MINIMIZE SUM(SUM(POWER(x - const, 2)))
# PER grp was rejected by the binder because POWER expansion produced a
# __SUM__ placeholder that leaked through the nested-SUM validator. After the
# fix, normalization preserves the raw AST and the post-bind optimizer's
# nested-aggregate strip flattens the form.
#
# Semantically SUM(SUM(expr)) PER grp ≡ SUM(expr), and SUM(AVG(expr)) PER grp
# weights each row by 1/n_g (group size). SUM(MIN/MAX(expr)) PER grp needs
# quadratic per-row auxiliary constraints downstream; those shapes now bind
# successfully but their physical-layer correctness is tracked separately.


@pytest.mark.quadratic
@pytest.mark.per_clause
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_qp_nested_sum_sum_per_binding(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """MINIMIZE SUM(SUM(POWER(x - target, 2))) PER grp with a binding PER upper
    bound that forces x below each row's target.

    The nested form is algebraically equivalent to flat SUM(POWER(x - t, 2)),
    so the oracle is formulated as the flat QP with the same per-group SUM(x)
    <= K constraints.
    """
    data_sql = """
        SELECT 1 AS id, 'A' AS grp, 10.0 AS target UNION ALL
        SELECT 2, 'A', 12.0 UNION ALL
        SELECT 3, 'B', 30.0 UNION ALL
        SELECT 4, 'B', 32.0
    """
    # Caps chosen below per-group target sums (22 for A, 62 for B) so the
    # constraint binds and pulls each group's optimum away from x = target.
    sql = f"""
        WITH data AS ({data_sql})
        SELECT id, grp, target, ROUND(x, 4) AS x FROM data
        DECIDE x(REAL)
        SUCH THAT x >= 0 AND x <= 100
            AND SUM(x) <= 10 WHEN grp = 'A'
            AND SUM(x) <= 40 WHEN grp = 'B'
        MINIMIZE SUM(SUM(POWER(x - target, 2))) PER grp
    """
    t0 = time.perf_counter()
    rows, cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0
    data = duckdb_conn.execute(f"""
        SELECT CAST(id AS BIGINT), CAST(grp AS VARCHAR), CAST(target AS DOUBLE)
        FROM ({data_sql})
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("qp_nested_sum_sum_per_binding")
    n = len(data)
    xnames = [f"x_{i}" for i in range(n)]
    for xn in xnames:
        oracle_solver.add_variable(xn, VarType.CONTINUOUS, lb=0.0, ub=100.0)
    # Flat QP (outer SUM is stripped by the optimizer).
    linear = {xnames[i]: -2.0 * data[i][2] for i in range(n)}
    quadratic = {(xnames[i], xnames[i]): 1.0 for i in range(n)}
    oracle_solver.set_quadratic_objective(linear, quadratic, ObjSense.MINIMIZE)
    # Per-group upper bounds matching the SQL.
    for grp, cap in (("A", 10.0), ("B", 40.0)):
        idxs = [i for i in range(n) if data[i][1] == grp]
        oracle_solver.add_constraint(
            {xnames[i]: 1.0 for i in idxs}, "<=", cap, name=f"cap_{grp}",
        )
    build_time = time.perf_counter() - t_build

    result = _solve_and_compare(
        oracle_solver, rows, cols, lambda rs, cs: _qp_target_obj(rs, cs),
    )
    perf_tracker.record(
        "qp_nested_sum_sum_per_binding", decidb_time, build_time,
        result.solve_time_seconds, n, n, 2,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )


@pytest.mark.quadratic
@pytest.mark.per_clause
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_qp_nested_sum_sum_per_unconstrained(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """Unconstrained nested-PER QP: each x_i = target_i, objective = -Σ t²
    (after DecidB's constant-stripping convention). Sanity test confirming
    the nested form degenerates to flat SUM(POWER(x-t,2)) semantically.
    """
    data_sql = """
        SELECT 1 AS id, 'A' AS grp, 10.0 AS target UNION ALL
        SELECT 2, 'A', 15.0 UNION ALL
        SELECT 3, 'B', 20.0 UNION ALL
        SELECT 4, 'B', 25.0
    """
    sql = f"""
        WITH data AS ({data_sql})
        SELECT id, grp, target, ROUND(x, 4) AS x FROM data
        DECIDE x(REAL)
        SUCH THAT x >= 0 AND x <= 100
        MINIMIZE SUM(SUM(POWER(x - target, 2))) PER grp
    """
    t0 = time.perf_counter()
    rows, cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0
    data = duckdb_conn.execute(f"""
        SELECT CAST(id AS BIGINT), CAST(grp AS VARCHAR), CAST(target AS DOUBLE)
        FROM ({data_sql})
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("qp_nested_sum_sum_per_uncon")
    n = len(data)
    xnames = [f"x_{i}" for i in range(n)]
    for xn in xnames:
        oracle_solver.add_variable(xn, VarType.CONTINUOUS, lb=0.0, ub=100.0)
    linear = {xnames[i]: -2.0 * data[i][2] for i in range(n)}
    quadratic = {(xnames[i], xnames[i]): 1.0 for i in range(n)}
    oracle_solver.set_quadratic_objective(linear, quadratic, ObjSense.MINIMIZE)
    build_time = time.perf_counter() - t_build

    result = _solve_and_compare(
        oracle_solver, rows, cols, lambda rs, cs: _qp_target_obj(rs, cs),
    )
    perf_tracker.record(
        "qp_nested_sum_sum_per_unconstrained", decidb_time, build_time,
        result.solve_time_seconds, n, n, 0,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )


@pytest.mark.quadratic
@pytest.mark.per_clause
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_qp_nested_sum_avg_per_binding(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """MINIMIZE SUM(AVG(POWER(x - target, 2))) PER grp with unequal group
    sizes (A: 2 rows, B: 3 rows). Inner AVG scales each row's contribution
    by 1/n_g. Verifies the per_inner_was_avg path in decide_optimizer.cpp
    together with the nested-aggregate-of-quadratic binding fix.
    """
    data_sql = """
        SELECT 1 AS id, 'A' AS grp, 10.0 AS target UNION ALL
        SELECT 2, 'A', 20.0 UNION ALL
        SELECT 3, 'B', 30.0 UNION ALL
        SELECT 4, 'B', 35.0 UNION ALL
        SELECT 5, 'B', 40.0
    """
    # Per-group caps chosen below target sums to force a binding interior
    # optimum distinct from x = target.
    sql = f"""
        WITH data AS ({data_sql})
        SELECT id, grp, target, ROUND(x, 4) AS x FROM data
        DECIDE x(REAL)
        SUCH THAT x >= 0 AND x <= 100
            AND SUM(x) <= 20 WHEN grp = 'A'
            AND SUM(x) <= 60 WHEN grp = 'B'
        MINIMIZE SUM(AVG(POWER(x - target, 2))) PER grp
    """
    t0 = time.perf_counter()
    rows, cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0
    data = duckdb_conn.execute(f"""
        SELECT CAST(id AS BIGINT), CAST(grp AS VARCHAR), CAST(target AS DOUBLE)
        FROM ({data_sql})
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("qp_nested_sum_avg_per_binding")
    n = len(data)
    xnames = [f"x_{i}" for i in range(n)]
    for xn in xnames:
        oracle_solver.add_variable(xn, VarType.CONTINUOUS, lb=0.0, ub=100.0)
    # Count rows per group for the 1/n_g scaling.
    from collections import Counter
    grp_size = Counter(row[1] for row in data)
    linear, quadratic = {}, {}
    for i, row in enumerate(data):
        w = 1.0 / grp_size[row[1]]
        linear[xnames[i]] = -2.0 * row[2] * w
        quadratic[(xnames[i], xnames[i])] = w
    oracle_solver.set_quadratic_objective(linear, quadratic, ObjSense.MINIMIZE)
    for grp, cap in (("A", 20.0), ("B", 60.0)):
        idxs = [i for i in range(n) if data[i][1] == grp]
        oracle_solver.add_constraint(
            {xnames[i]: 1.0 for i in idxs}, "<=", cap, name=f"cap_{grp}",
        )
    build_time = time.perf_counter() - t_build

    grp_col = cols.index("grp")
    x_col = cols.index("x")
    target_col = cols.index("target")

    def decidb_obj(rs, cs):
        # Mirror DecidB's per-group 1/n_g weighting with constant t² stripped.
        counts = Counter(r[grp_col] for r in rs)
        total = 0.0
        for r in rs:
            w = 1.0 / counts[r[grp_col]]
            x = float(r[x_col]); t = float(r[target_col])
            total += w * ((x - t) ** 2 - t ** 2)
        return total

    result = _solve_and_compare(oracle_solver, rows, cols, decidb_obj)
    perf_tracker.record(
        "qp_nested_sum_avg_per_binding", decidb_time, build_time,
        result.solve_time_seconds, n, n, 2,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )


@pytest.mark.quadratic
@pytest.mark.per_clause
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_qp_nested_sum_sum_per_constant_free_regression(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """Regression: MINIMIZE SUM(SUM(POWER(x, 2))) PER grp — no constant term
    in POWER. This form accidentally passed validation pre-fix (the factoring
    didn't leak a __SUM__ placeholder because there was no data-only sum to
    fold). Post-fix it takes the nested quadratic path instead;
    end-to-end result must be identical.
    """
    data_sql = """
        SELECT 1 AS id, 'A' AS grp UNION ALL
        SELECT 2, 'A' UNION ALL
        SELECT 3, 'B' UNION ALL
        SELECT 4, 'B'
    """
    sql = f"""
        WITH data AS ({data_sql})
        SELECT id, grp, ROUND(x, 4) AS x FROM data
        DECIDE x(REAL)
        SUCH THAT x >= 0 AND x <= 100
            AND SUM(x) >= 4 WHEN grp = 'A'
            AND SUM(x) >= 6 WHEN grp = 'B'
        MINIMIZE SUM(SUM(POWER(x, 2))) PER grp
    """
    t0 = time.perf_counter()
    rows, cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0
    data = duckdb_conn.execute(f"""
        SELECT CAST(id AS BIGINT), CAST(grp AS VARCHAR) FROM ({data_sql})
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("qp_nested_sum_sum_per_cf")
    n = len(data)
    xnames = [f"x_{i}" for i in range(n)]
    for xn in xnames:
        oracle_solver.add_variable(xn, VarType.CONTINUOUS, lb=0.0, ub=100.0)
    oracle_solver.set_quadratic_objective(
        {}, {(xn, xn): 1.0 for xn in xnames}, ObjSense.MINIMIZE,
    )
    for grp, floor in (("A", 4.0), ("B", 6.0)):
        idxs = [i for i in range(n) if data[i][1] == grp]
        oracle_solver.add_constraint(
            {xnames[i]: 1.0 for i in idxs}, ">=", floor, name=f"floor_{grp}",
        )
    build_time = time.perf_counter() - t_build

    result = _solve_and_compare(
        oracle_solver, rows, cols, lambda rs, cs: _qp_simple_obj(rs, cs),
    )
    perf_tracker.record(
        "qp_nested_sum_sum_per_constant_free", decidb_time, build_time,
        result.solve_time_seconds, n, n, 2,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )


# ===========================================================================
# Entity-scoped QP objective
# ===========================================================================

@pytest.mark.obj_minimize
@pytest.mark.quadratic
@pytest.mark.correctness
def test_qp_entity_scoped_objective(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """MINIMIZE SUM(POWER(x - target, 2)) with entity-scoped ``items.x``
    and a cross-entity resource cap.

    Convex minimize QP, so runs on both Gurobi and HiGHS (placed in
    ``test_quadratic.py`` rather than ``test_quadratic_constraints.py`` to
    avoid the Gurobi-only `@_expect_gurobi` marker). Each distinct item gets
    one REAL variable; the per-row squared deviation terms contribute to
    that item's linear and quadratic coefficients. An aggregate ``SUM(x) <=
    50`` cap couples entities — ``SUM(x)`` over join rows equals
    ``3*x_A + 2*x_B + x_C`` (row-count-weighted by entity), so the cap
    forces a trade-off across items rather than a decoupled per-entity
    least-squares fit.
    """
    data_sql = """
        SELECT 'A' AS item, 10.0 AS target UNION ALL
        SELECT 'A', 12.0 UNION ALL
        SELECT 'A', 14.0 UNION ALL
        SELECT 'B', 20.0 UNION ALL
        SELECT 'B', 22.0 UNION ALL
        SELECT 'C', 5.0
    """
    sql = f"""
        WITH items AS ({data_sql})
        SELECT item, target, ROUND(x, 4) AS x FROM items
        DECIDE items.x(REAL)
        SUCH THAT x >= 0 AND x <= 100
            AND SUM(x) <= 50
        MINIMIZE SUM(POWER(x - target, 2))
    """
    t0 = time.perf_counter()
    rows, cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute(
        f"SELECT CAST(item AS VARCHAR), CAST(target AS DOUBLE) FROM ({data_sql})"
    ).fetchall()

    # Entity consistency: every row with the same item must have the same x.
    item_idx = cols.index("item")
    x_idx = cols.index("x")
    item_x = {}
    for row in rows:
        it = row[item_idx]
        xv = float(row[x_idx])
        if it in item_x:
            assert item_x[it] == pytest.approx(xv, abs=1e-3), (
                f"item {it} has inconsistent x: {item_x[it]} vs {xv}"
            )
        else:
            item_x[it] = xv

    t_build = time.perf_counter()
    oracle_solver.create_model("qp_entity_scoped_objective")
    items = sorted({r[0] for r in data})
    for it in items:
        oracle_solver.add_variable(
            f"x_{it}", VarType.CONTINUOUS, lb=0.0, ub=100.0,
        )

    linear: dict = {}
    quadratic: dict = {}
    row_count = {it: 0 for it in items}
    for row in data:
        it = row[0]
        t = float(row[1])
        key = f"x_{it}"
        linear[key] = linear.get(key, 0.0) - 2.0 * t
        quadratic[(key, key)] = quadratic.get((key, key), 0.0) + 1.0
        row_count[it] += 1
    # SUM(x) over join rows = sum(row_count[it] * x_{it}).
    oracle_solver.add_constraint(
        {f"x_{it}": float(row_count[it]) for it in items},
        "<=", 50.0, name="entity_budget",
    )
    oracle_solver.set_quadratic_objective(
        linear, quadratic, ObjSense.MINIMIZE,
    )
    build_time = time.perf_counter() - t_build

    n = len(data)
    result = _solve_and_compare(
        oracle_solver, rows, cols,
        lambda rs, cs: _qp_target_obj(rs, cs, x_col="x", target_col="target"),
    )
    perf_tracker.record(
        "qp_entity_scoped_objective", decidb_time, build_time,
        result.solve_time_seconds, n, len(items), 0,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )


# ---------------------------------------------------------------------------
# HiGHS-forced rejection tests (require DECIDB_FORCE_SOLVER=highs)
# ---------------------------------------------------------------------------


@pytest.mark.correctness
@pytest.mark.quadratic
class TestHighsRejection:
    """Model-class refusals when the solver is pinned to HiGHS.

    Three model classes have no lowering into plain linear rows -- quadratic
    constraints, a non-convex objective, and MIQP -- so a backend that cannot
    take one has no answer but refusal. HiGHS declares all three unsupported,
    and the refusal happens at PLAN time (``RequireDecideSolverSupport``,
    ``src/optimizer/decide/decide_solver_gate.cpp``), before the query reads a
    row. Each message names what the query does in SQL terms, blames the host
    rather than the query, and names the solver to install.

    On Gurobi-linked hosts the unforced suite would take the Gurobi path and
    succeed on all three; ``decidb_cli_highs`` sets ``DECIDB_FORCE_SOLVER=highs``.
    """

    def test_highs_nonconvex_qp_rejected(self, decidb_cli_highs):
        """MAXIMIZE SUM(POWER(x, 2)) with x(REAL) is non-convex and HiGHS-rejected."""
        sql = """
            WITH data AS (SELECT 1 AS id UNION ALL SELECT 2)
            SELECT id, x FROM data
            DECIDE x(REAL)
            SUCH THAT x >= 0 AND x <= 10
            MAXIMIZE SUM(POWER(x, 2))
        """
        decidb_cli_highs.assert_error(
            sql, match=r"MAXIMIZEs squared decision terms.*Gurobi",
        )

    def test_highs_miqp_rejected(self, decidb_cli_highs):
        """MINIMIZE SUM(POWER(x, 2)) with x(INT) triggers MIQP, HiGHS-rejected."""
        sql = """
            WITH data AS (
                SELECT 1 AS id, 3.0 AS target UNION ALL
                SELECT 2, 7.0
            )
            SELECT id, x FROM data
            DECIDE x(INT)
            SUCH THAT x <= 10
            MINIMIZE SUM(POWER(x - target, 2))
        """
        decidb_cli_highs.assert_error(
            sql, match=r"squares decision variables that are not continuous.*Gurobi",
        )

    def test_highs_quadratic_constraint_rejected(self, decidb_cli_highs):
        """A squared term in SUCH THAT is a quadratic constraint (QCQP), HiGHS-rejected."""
        sql = """
            WITH data AS (SELECT 1 AS id UNION ALL SELECT 2)
            SELECT id, x FROM data
            DECIDE x(REAL)
            SUCH THAT POWER(x, 2) <= 4
            MAXIMIZE SUM(x)
        """
        decidb_cli_highs.assert_error(
            sql, match=r"SUCH THAT clause squares or multiplies decision variables.*Gurobi",
        )

    def test_highs_refusal_happens_before_any_row_is_read(self, decidb_cli_highs):
        """The gate is at plan time, so a refused query never builds a model.

        Proven by the model dump: ``DECIDB_DUMP_MODEL`` writes one block per
        model actually built, and a plan-time refusal writes none. This is what
        separates the gate from the old model-load rejection, which refused only
        after a full scan and build.
        """
        sql = """
            WITH data AS (SELECT 1 AS id UNION ALL SELECT 2)
            SELECT id, x FROM data
            DECIDE x(REAL)
            SUCH THAT POWER(x, 2) <= 4
            MAXIMIZE SUM(x)
        """
        with tempfile.TemporaryDirectory() as tmp:
            dump_path = os.path.join(tmp, "model.dump")
            cli = decidb_cli_highs.with_env({"DECIDB_DUMP_MODEL": dump_path})
            cli.assert_error(sql, match=r"SUCH THAT clause squares")
            assert not os.path.exists(dump_path), (
                "a plan-time refusal must not build a model, but a dump was written"
            )
