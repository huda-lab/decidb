"""Tests for bilinear term support (x * y) in DECIDE objectives and constraints.

Covers:
  - Boolean × Boolean product (AND-linearization, both solvers)
  - Boolean × Real product (McCormick linearization, both solvers)
  - Boolean × Integer product (McCormick linearization, both solvers)
  - Real × Real product (non-convex Q matrix, Gurobi only)
  - Integer × Integer product (non-convex, Gurobi only)
  - Integer × Real product (non-convex, Gurobi only)
  - Bilinear in objectives (all variable type combinations)
  - Bilinear in constraints (QCQP, Gurobi only for non-linearizable)
  - Mixed linear + bilinear objectives
  - Bilinear with WHEN filter
  - Error rejection: triple products, HiGHS non-convex rejection
  - Backward compatibility: existing QP and linear objectives unchanged

Cross-feature interactions (oracle-compared, module-level):
  - test_bilinear_per_group:           bilinear + PER (McCormick aux per group)
  - test_bilinear_when_per_triple:     bilinear + WHEN + PER triple composition
  - test_bilinear_entity_scoped:       entity-scoped Boolean × row-scoped Real
  - test_bilinear_minimize_objective:  MINIMIZE direction with data coefficient
  - test_bilinear_bool_real_constraint: McCormick feasibility for Bool×Real constraint
"""

import re
import time

import pytest

from decidb_cli import DecidBCliError
from solver.types import VarType, ObjSense, SolverStatus

from ._oracle_helpers import add_bool_and


# ===================================================================
# Phase 1: Boolean × anything (McCormick linearization, both solvers)
# ===================================================================


@pytest.mark.correctness
class TestBilinearBooleanObjectives:
    """Bilinear objectives where at least one factor is Boolean (linearizable)."""

    def test_bool_times_bool_objective(self, decidb_cli, oracle_solver, perf_tracker):
        """MAXIMIZE SUM(b1 * b2) — AND-linearization, oracle-compared."""
        sql = """
            WITH data AS (
                SELECT 1 AS id UNION ALL SELECT 2 UNION ALL SELECT 3
            )
            SELECT id, b1, b2
            FROM data
            DECIDE b1(BOOL), b2(BOOL)
            SUCH THAT SUM(b1) <= 3 AND SUM(b2) <= 3
            MAXIMIZE SUM(b1 * b2)
        """
        t0 = time.perf_counter()
        result, cols = decidb_cli.execute(sql)
        decidb_time = time.perf_counter() - t0

        n = 3
        t_build = time.perf_counter()
        oracle_solver.create_model("bool_times_bool_objective")
        b1n = [f"b1_{i}" for i in range(n)]
        b2n = [f"b2_{i}" for i in range(n)]
        wn = [f"w_{i}" for i in range(n)]
        for v in b1n + b2n:
            oracle_solver.add_variable(v, VarType.BINARY)
        for v in wn:
            oracle_solver.add_variable(v, VarType.CONTINUOUS, lb=0.0, ub=1.0)
        for i in range(n):
            _mccormick_link(oracle_solver, wn[i], b1n[i], b2n[i], 1.0, f"mc_{i}")
        oracle_solver.add_constraint({b: 1.0 for b in b1n}, "<=", 3.0, name="cap_b1")
        oracle_solver.add_constraint({b: 1.0 for b in b2n}, "<=", 3.0, name="cap_b2")
        oracle_solver.set_objective({w: 1.0 for w in wn}, ObjSense.MAXIMIZE)
        build_time = time.perf_counter() - t_build
        res = oracle_solver.solve()
        assert res.status == SolverStatus.OPTIMAL

        b1_col = cols.index("b1")
        b2_col = cols.index("b2")
        decidb_obj = sum(int(row[b1_col]) * int(row[b2_col]) for row in result)
        assert abs(decidb_obj - res.objective_value) <= 1e-6, (
            f"Objective mismatch: DecidB={decidb_obj}, Oracle={res.objective_value}"
        )
        perf_tracker.record(
            "bool_times_bool_objective", decidb_time, build_time,
            res.solve_time_seconds, n, n * 3, n * 3 + 2,
            res.objective_value, oracle_solver.solver_name(),
            comparison_status="optimal",
        )

    def test_bool_times_bool_constrained(self, decidb_cli, oracle_solver, perf_tracker):
        """MAXIMIZE SUM(b1 * b2) with SUM(b1) <= 2 — oracle-compared."""
        sql = """
            WITH data AS (
                SELECT 1 AS id UNION ALL SELECT 2 UNION ALL SELECT 3
            )
            SELECT id, b1, b2
            FROM data
            DECIDE b1(BOOL), b2(BOOL)
            SUCH THAT SUM(b1) <= 2 AND SUM(b2) <= 3
            MAXIMIZE SUM(b1 * b2)
        """
        t0 = time.perf_counter()
        result, cols = decidb_cli.execute(sql)
        decidb_time = time.perf_counter() - t0

        n = 3
        t_build = time.perf_counter()
        oracle_solver.create_model("bool_times_bool_constrained")
        b1n = [f"b1_{i}" for i in range(n)]
        b2n = [f"b2_{i}" for i in range(n)]
        wn = [f"w_{i}" for i in range(n)]
        for v in b1n + b2n:
            oracle_solver.add_variable(v, VarType.BINARY)
        for v in wn:
            oracle_solver.add_variable(v, VarType.CONTINUOUS, lb=0.0, ub=1.0)
        for i in range(n):
            _mccormick_link(oracle_solver, wn[i], b1n[i], b2n[i], 1.0, f"mc_{i}")
        oracle_solver.add_constraint({b: 1.0 for b in b1n}, "<=", 2.0, name="cap_b1")
        oracle_solver.add_constraint({b: 1.0 for b in b2n}, "<=", 3.0, name="cap_b2")
        oracle_solver.set_objective({w: 1.0 for w in wn}, ObjSense.MAXIMIZE)
        build_time = time.perf_counter() - t_build
        res = oracle_solver.solve()
        assert res.status == SolverStatus.OPTIMAL

        b1_col = cols.index("b1")
        b2_col = cols.index("b2")
        decidb_obj = sum(int(row[b1_col]) * int(row[b2_col]) for row in result)
        assert abs(decidb_obj - res.objective_value) <= 1e-6, (
            f"Objective mismatch: DecidB={decidb_obj}, Oracle={res.objective_value}"
        )
        perf_tracker.record(
            "bool_times_bool_constrained", decidb_time, build_time,
            res.solve_time_seconds, n, n * 3, n * 3 + 2,
            res.objective_value, oracle_solver.solver_name(),
            comparison_status="optimal",
        )

    def test_bool_times_real_objective(self, decidb_cli, oracle_solver, perf_tracker):
        """MAXIMIZE SUM(b * x) with SUM(b) <= 2, x ∈ [0,100] — McCormick oracle."""
        sql = """
            WITH data AS (
                SELECT 1 AS id, 10.0 AS profit UNION ALL
                SELECT 2, 20.0 UNION ALL
                SELECT 3, 5.0
            )
            SELECT id, b, ROUND(x, 2) AS x
            FROM data
            DECIDE b(BOOL), x(REAL)
            SUCH THAT x >= 0 AND x <= 100 AND SUM(b) <= 2
            MAXIMIZE SUM(b * x)
        """
        t0 = time.perf_counter()
        result, cols = decidb_cli.execute(sql)
        decidb_time = time.perf_counter() - t0

        n = 3
        U = 100.0
        t_build = time.perf_counter()
        oracle_solver.create_model("bool_times_real_objective")
        bnames = [f"b_{i}" for i in range(n)]
        xnames = [f"x_{i}" for i in range(n)]
        wnames = [f"w_{i}" for i in range(n)]
        for v in bnames:
            oracle_solver.add_variable(v, VarType.BINARY)
        for v in xnames:
            oracle_solver.add_variable(v, VarType.CONTINUOUS, lb=0.0, ub=U)
        for v in wnames:
            oracle_solver.add_variable(v, VarType.CONTINUOUS, lb=0.0, ub=U)
        for i in range(n):
            _mccormick_link(oracle_solver, wnames[i], bnames[i], xnames[i], U, f"mc_{i}")
        oracle_solver.add_constraint({b: 1.0 for b in bnames}, "<=", 2.0, name="cap")
        oracle_solver.set_objective({w: 1.0 for w in wnames}, ObjSense.MAXIMIZE)
        build_time = time.perf_counter() - t_build
        res = oracle_solver.solve()
        assert res.status == SolverStatus.OPTIMAL

        b_col = cols.index("b")
        x_col = cols.index("x")
        decidb_obj = sum(int(row[b_col]) * float(row[x_col]) for row in result)
        assert abs(decidb_obj - res.objective_value) <= 1e-3, (
            f"Objective mismatch: DecidB={decidb_obj}, Oracle={res.objective_value}"
        )
        perf_tracker.record(
            "bool_times_real_objective", decidb_time, build_time,
            res.solve_time_seconds, n, n * 3, n * 3 + 1,
            res.objective_value, oracle_solver.solver_name(),
            comparison_status="optimal",
        )

    def test_bool_times_integer_objective(
        self, decidb_cli, oracle_solver, perf_tracker,
    ):
        """MAXIMIZE SUM(b * n) where b(BOOL), n(INT) ∈ [0,5]."""
        sql = """
            WITH data AS (
                SELECT 1 AS id UNION ALL SELECT 2 UNION ALL SELECT 3
            )
            SELECT id, b, n
            FROM data
            DECIDE b(BOOL), n(INT)
            SUCH THAT n >= 0 AND n <= 5 AND SUM(b) <= 2
            MAXIMIZE SUM(b * n)
        """
        t0 = time.perf_counter()
        result, cols = decidb_cli.execute(sql)
        decidb_time = time.perf_counter() - t0

        n_rows = 3
        U = 5.0
        t_build = time.perf_counter()
        oracle_solver.create_model("bool_times_integer_objective")
        bnames = [f"b_{i}" for i in range(n_rows)]
        nnames = [f"n_{i}" for i in range(n_rows)]
        wnames = [f"w_{i}" for i in range(n_rows)]
        for v in bnames:
            oracle_solver.add_variable(v, VarType.BINARY)
        for v in nnames:
            oracle_solver.add_variable(v, VarType.INTEGER, lb=0.0, ub=U)
        for v in wnames:
            oracle_solver.add_variable(v, VarType.CONTINUOUS, lb=0.0, ub=U)
        for i in range(n_rows):
            _mccormick_link(oracle_solver, wnames[i], bnames[i], nnames[i], U, f"mc_{i}")
        oracle_solver.add_constraint({b: 1.0 for b in bnames}, "<=", 2.0, name="cap")
        oracle_solver.set_objective({w: 1.0 for w in wnames}, ObjSense.MAXIMIZE)
        build_time = time.perf_counter() - t_build
        res = oracle_solver.solve()
        assert res.status == SolverStatus.OPTIMAL

        b_col = cols.index("b")
        ncol = cols.index("n")
        decidb_obj = sum(int(row[b_col]) * int(row[ncol]) for row in result)
        assert abs(decidb_obj - res.objective_value) <= 1e-6, (
            f"Objective mismatch: DecidB={decidb_obj}, Oracle={res.objective_value}"
        )
        perf_tracker.record(
            "bool_times_integer_objective", decidb_time, build_time,
            res.solve_time_seconds, n_rows, n_rows * 3, n_rows * 3 + 1,
            res.objective_value, oracle_solver.solver_name(),
            comparison_status="optimal",
        )

    def test_bool_times_real_with_data_coefficient(
        self, decidb_cli, oracle_solver, perf_tracker,
    ):
        """SUM(profit * b * x) parses as (profit * b) * x; bilinear in (b, x)
        with profit as a per-row coefficient. Oracle uses the same McCormick
        aux w = b*x and puts profit on the objective linear coefficient."""
        sql = """
            WITH data AS (
                SELECT 1 AS id, 3.0 AS profit UNION ALL
                SELECT 2, 1.0
            )
            SELECT id, b, ROUND(x, 2) AS x
            FROM data
            DECIDE b(BOOL), x(REAL)
            SUCH THAT x >= 0 AND x <= 10
            MAXIMIZE SUM(profit * b * x)
        """
        t0 = time.perf_counter()
        result, cols = decidb_cli.execute(sql)
        decidb_time = time.perf_counter() - t0

        data = [(1, 3.0), (2, 1.0)]
        n = len(data)
        U = 10.0
        t_build = time.perf_counter()
        oracle_solver.create_model("bool_times_real_with_data_coefficient")
        bnames = [f"b_{i}" for i in range(n)]
        xnames = [f"x_{i}" for i in range(n)]
        wnames = [f"w_{i}" for i in range(n)]
        for v in bnames:
            oracle_solver.add_variable(v, VarType.BINARY)
        for v in xnames:
            oracle_solver.add_variable(v, VarType.CONTINUOUS, lb=0.0, ub=U)
        for v in wnames:
            oracle_solver.add_variable(v, VarType.CONTINUOUS, lb=0.0, ub=U)
        for i in range(n):
            _mccormick_link(oracle_solver, wnames[i], bnames[i], xnames[i], U, f"mc_{i}")
        oracle_solver.set_objective(
            {wnames[i]: data[i][1] for i in range(n)}, ObjSense.MAXIMIZE,
        )
        build_time = time.perf_counter() - t_build
        res = oracle_solver.solve()
        assert res.status == SolverStatus.OPTIMAL

        b_col = cols.index("b")
        x_col = cols.index("x")
        id_col = cols.index("id")
        profit_by_id = dict(data)
        decidb_obj = sum(
            profit_by_id[int(row[id_col])] * int(row[b_col]) * float(row[x_col])
            for row in result
        )
        assert abs(decidb_obj - res.objective_value) <= 1e-3, (
            f"Objective mismatch: DecidB={decidb_obj}, Oracle={res.objective_value}"
        )
        perf_tracker.record(
            "bool_times_real_with_data_coefficient", decidb_time, build_time,
            res.solve_time_seconds, n, n * 3, n * 3,
            res.objective_value, oracle_solver.solver_name(),
            comparison_status="optimal",
        )


# ===================================================================
# Phase 2: General non-convex bilinear (Gurobi only via Q matrix)
# ===================================================================


@pytest.mark.correctness
class TestBilinearNonConvexObjectives:
    """Non-convex bilinear objectives (Real×Real, Int×Int) — Gurobi only."""

    def test_real_times_real_objective(self, decidb_cli, oracle_solver, perf_tracker):
        """MAXIMIZE SUM(x * y) with x, y ∈ [0, 10] — non-convex QP.

        Oracle uses off-diagonal Q entry (x, y) = 1.0; Gurobi's NonConvex=2
        is activated automatically. If DecidB's solver is HiGHS, the query
        is rejected and we exit without running the oracle.
        """
        sql = """
            WITH data AS (SELECT 1 AS id)
            SELECT id, ROUND(x, 2) AS x, ROUND(y, 2) AS y
            FROM data
            DECIDE x(REAL), y(REAL)
            SUCH THAT x >= 0 AND x <= 10 AND y >= 0 AND y <= 10
            MAXIMIZE SUM(x * y)
        """
        try:
            t0 = time.perf_counter()
            result, cols = decidb_cli.execute(sql)
            decidb_time = time.perf_counter() - t0
        except DecidBCliError as e:
            assert re.search(r"Non-convex|require Gurobi", e.message), \
                f"Unexpected error: {e.message}"
            return

        t_build = time.perf_counter()
        oracle_solver.create_model("real_times_real_objective")
        oracle_solver.add_variable("x", VarType.CONTINUOUS, lb=0.0, ub=10.0)
        oracle_solver.add_variable("y", VarType.CONTINUOUS, lb=0.0, ub=10.0)
        oracle_solver.set_quadratic_objective(
            {}, {("x", "y"): 1.0}, ObjSense.MAXIMIZE,
        )
        build_time = time.perf_counter() - t_build
        res = oracle_solver.solve()
        assert res.status == SolverStatus.OPTIMAL

        x_val = float(result[0][cols.index("x")])
        y_val = float(result[0][cols.index("y")])
        decidb_obj = x_val * y_val
        assert abs(decidb_obj - res.objective_value) <= 1e-3, (
            f"Objective mismatch: DecidB={decidb_obj}, Oracle={res.objective_value}"
        )
        perf_tracker.record(
            "real_times_real_objective", decidb_time, build_time,
            res.solve_time_seconds, 1, 2, 0,
            res.objective_value, oracle_solver.solver_name(),
            comparison_status="optimal",
        )

    def test_int_times_int_objective(self, decidb_cli, oracle_solver, perf_tracker):
        """MAXIMIZE SUM(x * y) with x, y INTEGER ∈ [0, 5] — non-convex MIQP."""
        sql = """
            WITH data AS (SELECT 1 AS id)
            SELECT id, x, y
            FROM data
            DECIDE x(INT), y(INT)
            SUCH THAT x >= 0 AND x <= 5 AND y >= 0 AND y <= 5
            MAXIMIZE SUM(x * y)
        """
        try:
            t0 = time.perf_counter()
            result, cols = decidb_cli.execute(sql)
            decidb_time = time.perf_counter() - t0
        except DecidBCliError as e:
            assert re.search(r"Non-convex|require Gurobi", e.message), \
                f"Unexpected error: {e.message}"
            return

        t_build = time.perf_counter()
        oracle_solver.create_model("int_times_int_objective")
        oracle_solver.add_variable("x", VarType.INTEGER, lb=0.0, ub=5.0)
        oracle_solver.add_variable("y", VarType.INTEGER, lb=0.0, ub=5.0)
        oracle_solver.set_quadratic_objective(
            {}, {("x", "y"): 1.0}, ObjSense.MAXIMIZE,
        )
        build_time = time.perf_counter() - t_build
        res = oracle_solver.solve()
        assert res.status == SolverStatus.OPTIMAL

        x_val = int(result[0][cols.index("x")])
        y_val = int(result[0][cols.index("y")])
        decidb_obj = x_val * y_val
        assert abs(decidb_obj - res.objective_value) <= 1e-6, (
            f"Objective mismatch: DecidB={decidb_obj}, Oracle={res.objective_value}"
        )
        perf_tracker.record(
            "int_times_int_objective", decidb_time, build_time,
            res.solve_time_seconds, 1, 2, 0,
            res.objective_value, oracle_solver.solver_name(),
            comparison_status="optimal",
        )

    def test_int_times_real_objective(self, decidb_cli, oracle_solver, perf_tracker):
        """MAXIMIZE SUM(n * x) with n INTEGER ∈ [0,5], x REAL ∈ [0,10] — non-convex."""
        sql = """
            WITH data AS (SELECT 1 AS id)
            SELECT id, n, ROUND(x, 2) AS x
            FROM data
            DECIDE n(INT), x(REAL)
            SUCH THAT n >= 0 AND n <= 5 AND x >= 0 AND x <= 10
            MAXIMIZE SUM(n * x)
        """
        try:
            t0 = time.perf_counter()
            result, cols = decidb_cli.execute(sql)
            decidb_time = time.perf_counter() - t0
        except DecidBCliError as e:
            assert re.search(r"Non-convex|require Gurobi", e.message), \
                f"Unexpected error: {e.message}"
            return

        t_build = time.perf_counter()
        oracle_solver.create_model("int_times_real_objective")
        oracle_solver.add_variable("n", VarType.INTEGER, lb=0.0, ub=5.0)
        oracle_solver.add_variable("x", VarType.CONTINUOUS, lb=0.0, ub=10.0)
        oracle_solver.set_quadratic_objective(
            {}, {("n", "x"): 1.0}, ObjSense.MAXIMIZE,
        )
        build_time = time.perf_counter() - t_build
        res = oracle_solver.solve()
        assert res.status == SolverStatus.OPTIMAL

        n_val = int(result[0][cols.index("n")])
        x_val = float(result[0][cols.index("x")])
        decidb_obj = n_val * x_val
        assert abs(decidb_obj - res.objective_value) <= 1e-3, (
            f"Objective mismatch: DecidB={decidb_obj}, Oracle={res.objective_value}"
        )
        perf_tracker.record(
            "int_times_real_objective", decidb_time, build_time,
            res.solve_time_seconds, 1, 2, 0,
            res.objective_value, oracle_solver.solver_name(),
            comparison_status="optimal",
        )


# ===================================================================
# Mixed objectives
# ===================================================================


@pytest.mark.correctness
class TestBilinearMixedObjectives:
    """Mixed linear + bilinear and bilinear + POWER objectives."""

    def test_linear_plus_bilinear(self, decidb_cli, oracle_solver, perf_tracker):
        """MAXIMIZE SUM(cost + b * x) — mixed linear (cost is a data constant
        per row) plus bilinear (b * x). The cost contribution is constant
        w.r.t. the decision variables, so the oracle drops it and we compare
        only the bilinear portion on both sides."""
        sql = """
            WITH data AS (
                SELECT 1 AS id, 5.0 AS cost UNION ALL
                SELECT 2, 10.0
            )
            SELECT id, b, ROUND(x, 2) AS x
            FROM data
            DECIDE b(BOOL), x(REAL)
            SUCH THAT x >= 0 AND x <= 20
            MAXIMIZE SUM(cost + b * x)
        """
        t0 = time.perf_counter()
        result, cols = decidb_cli.execute(sql)
        decidb_time = time.perf_counter() - t0

        n = 2
        U = 20.0
        t_build = time.perf_counter()
        oracle_solver.create_model("linear_plus_bilinear")
        bnames = [f"b_{i}" for i in range(n)]
        xnames = [f"x_{i}" for i in range(n)]
        wnames = [f"w_{i}" for i in range(n)]
        for v in bnames:
            oracle_solver.add_variable(v, VarType.BINARY)
        for v in xnames:
            oracle_solver.add_variable(v, VarType.CONTINUOUS, lb=0.0, ub=U)
        for v in wnames:
            oracle_solver.add_variable(v, VarType.CONTINUOUS, lb=0.0, ub=U)
        for i in range(n):
            _mccormick_link(oracle_solver, wnames[i], bnames[i], xnames[i], U, f"mc_{i}")
        oracle_solver.set_objective({w: 1.0 for w in wnames}, ObjSense.MAXIMIZE)
        build_time = time.perf_counter() - t_build
        res = oracle_solver.solve()
        assert res.status == SolverStatus.OPTIMAL

        b_col = cols.index("b")
        x_col = cols.index("x")
        decidb_obj = sum(int(row[b_col]) * float(row[x_col]) for row in result)
        assert abs(decidb_obj - res.objective_value) <= 1e-3, (
            f"Objective mismatch (bilinear part): DecidB={decidb_obj}, "
            f"Oracle={res.objective_value}"
        )
        perf_tracker.record(
            "linear_plus_bilinear", decidb_time, build_time,
            res.solve_time_seconds, n, n * 3, n * 3,
            res.objective_value, oracle_solver.solver_name(),
            comparison_status="optimal",
        )


# ===================================================================
# Feature interactions
# ===================================================================


@pytest.mark.correctness
class TestBilinearFeatureInteractions:
    """Bilinear with WHEN, PER, and other features."""

    def test_bilinear_with_when(self, decidb_cli, oracle_solver, perf_tracker):
        """MAXIMIZE SUM(b * x) WHEN category = 'A' — filtered bilinear.

        Oracle includes only rows where category='A' in the objective.
        Rows where category='B' still have their b/x variables in the
        oracle model but are unconstrained by the objective."""
        sql = """
            WITH data AS (
                SELECT 1 AS id, 'A' AS category UNION ALL
                SELECT 2, 'B' UNION ALL
                SELECT 3, 'A'
            )
            SELECT id, category, b, ROUND(x, 2) AS x
            FROM data
            DECIDE b(BOOL), x(REAL)
            SUCH THAT x >= 0 AND x <= 10
            MAXIMIZE SUM(b * x) WHEN category = 'A'
        """
        t0 = time.perf_counter()
        result, cols = decidb_cli.execute(sql)
        decidb_time = time.perf_counter() - t0

        data = [(1, "A"), (2, "B"), (3, "A")]
        n = len(data)
        U = 10.0
        t_build = time.perf_counter()
        oracle_solver.create_model("bilinear_with_when")
        bnames = [f"b_{i}" for i in range(n)]
        xnames = [f"x_{i}" for i in range(n)]
        wnames = [f"w_{i}" for i in range(n)]
        for v in bnames:
            oracle_solver.add_variable(v, VarType.BINARY)
        for v in xnames:
            oracle_solver.add_variable(v, VarType.CONTINUOUS, lb=0.0, ub=U)
        for v in wnames:
            oracle_solver.add_variable(v, VarType.CONTINUOUS, lb=0.0, ub=U)
        for i in range(n):
            _mccormick_link(oracle_solver, wnames[i], bnames[i], xnames[i], U, f"mc_{i}")
        oracle_solver.set_objective(
            {wnames[i]: 1.0 for i in range(n) if data[i][1] == "A"},
            ObjSense.MAXIMIZE,
        )
        build_time = time.perf_counter() - t_build
        res = oracle_solver.solve()
        assert res.status == SolverStatus.OPTIMAL

        cat_col = cols.index("category")
        b_col = cols.index("b")
        x_col = cols.index("x")
        decidb_obj = sum(
            int(row[b_col]) * float(row[x_col])
            for row in result if row[cat_col] == "A"
        )
        assert abs(decidb_obj - res.objective_value) <= 1e-3, (
            f"Objective mismatch: DecidB={decidb_obj}, Oracle={res.objective_value}"
        )
        perf_tracker.record(
            "bilinear_with_when", decidb_time, build_time,
            res.solve_time_seconds, n, n * 3, n * 3,
            res.objective_value, oracle_solver.solver_name(),
            comparison_status="optimal",
        )


# ===================================================================
# Phase 3: Bilinear in constraints
# ===================================================================


@pytest.mark.correctness
class TestBilinearConstraints:
    """Bilinear terms in SUCH THAT constraints."""

    @staticmethod
    def _assert_single_xy_one(result, cols):
        assert len(result) == 1
        row = result[0]
        assert int(row[cols.index("x")]) == 1
        assert int(row[cols.index("y")]) == 1

    @staticmethod
    def _run_coeff_constraint(decidb_cli_gurobi, constraint_expr, data_sql="SELECT 1 AS id", select_cols="id"):
        sql = f"""
            WITH data AS ({data_sql})
            SELECT {select_cols}, x, y
            FROM data
            DECIDE x(INT), y(INT)
            SUCH THAT x <= 1 AND y <= 1 AND {constraint_expr}
        """
        return decidb_cli_gurobi.execute(sql)

    @pytest.mark.bilinear
    @pytest.mark.var_integer
    @pytest.mark.cons_perrow
    def test_bilinear_constraint_coeff_multiplies_both_sides(self, decidb_cli_gurobi):
        """Regression: (2*x)*(3*y) must use coefficient 6, not just one side."""
        result, cols = self._run_coeff_constraint(
            decidb_cli_gurobi, "(2 * x) * (3 * y) >= 4",
        )

        self._assert_single_xy_one(result, cols)

    @pytest.mark.bilinear
    @pytest.mark.var_integer
    @pytest.mark.cons_perrow
    def test_bilinear_constraint_coeff_split_shape_matches_flat_product(self, decidb_cli_gurobi):
        split_result, split_cols = self._run_coeff_constraint(
            decidb_cli_gurobi, "(2 * x) * (3 * y) >= 4",
        )
        flat_result, flat_cols = self._run_coeff_constraint(
            decidb_cli_gurobi, "6 * x * y >= 4",
        )

        self._assert_single_xy_one(split_result, split_cols)
        self._assert_single_xy_one(flat_result, flat_cols)
        assert split_result[0][split_cols.index("x")] == flat_result[0][flat_cols.index("x")]
        assert split_result[0][split_cols.index("y")] == flat_result[0][flat_cols.index("y")]

    @pytest.mark.bilinear
    @pytest.mark.var_integer
    @pytest.mark.cons_perrow
    @pytest.mark.parametrize("constraint_expr", [
        "(2 * x) * y >= 2",
        "x * (3 * y) >= 3",
    ])
    def test_bilinear_constraint_coeff_one_sided(self, decidb_cli_gurobi, constraint_expr):
        result, cols = self._run_coeff_constraint(decidb_cli_gurobi, constraint_expr)

        self._assert_single_xy_one(result, cols)

    @pytest.mark.bilinear
    @pytest.mark.var_integer
    @pytest.mark.cons_perrow
    def test_bilinear_constraint_coeff_multiplies_data_columns(self, decidb_cli_gurobi):
        result, cols = self._run_coeff_constraint(
            decidb_cli_gurobi,
            "(a * x) * (b * y) >= 10",
            data_sql="SELECT 1 AS id, 2 AS a, 5 AS b",
            select_cols="id, a, b",
        )

        self._assert_single_xy_one(result, cols)

    @pytest.mark.bilinear
    @pytest.mark.var_integer
    @pytest.mark.cons_perrow
    def test_bilinear_constraint_coeff_preserves_negative_sign(self, decidb_cli_gurobi):
        result, cols = self._run_coeff_constraint(
            decidb_cli_gurobi, "-((2 * x) * (3 * y)) <= -4",
        )

        self._assert_single_xy_one(result, cols)

    @staticmethod
    def _run_grouped_product_minimize_y(decidb_cli_gurobi, constraint_expr):
        sql = f"""
            WITH data AS (SELECT 1 AS id, 2 AS a, 5 AS b)
            SELECT id, a, b, x, y
            FROM data
            DECIDE x(INT), y(INT)
            SUCH THAT x >= 0 AND x <= 1
                  AND y >= 0 AND y <= 1
                  AND {constraint_expr}
            MINIMIZE SUM(y)
        """
        return decidb_cli_gurobi.execute(sql)

    @pytest.mark.bilinear
    @pytest.mark.var_integer
    @pytest.mark.cons_aggregate
    @pytest.mark.obj_minimize
    def test_bilinear_aggregate_constraint_grouped_product_forces_y(self, decidb_cli_gurobi):
        result, cols = self._run_grouped_product_minimize_y(
            decidb_cli_gurobi, "SUM((a * b) * (x * y)) >= 10",
        )

        self._assert_single_xy_one(result, cols)

    @pytest.mark.bilinear
    @pytest.mark.var_integer
    @pytest.mark.cons_perrow
    @pytest.mark.obj_minimize
    def test_bilinear_per_row_constraint_grouped_product_forces_y(self, decidb_cli_gurobi):
        result, cols = self._run_grouped_product_minimize_y(
            decidb_cli_gurobi, "(a * b) * (x * y) >= 10",
        )

        self._assert_single_xy_one(result, cols)

    def test_bool_bilinear_constraint(self, decidb_cli, oracle_solver, perf_tracker):
        """SUCH THAT SUM(b1 * b2) <= 1 — bilinear constraint, linear objective.

        Oracle encodes z_i = b1_i AND b2_i via McCormick and constrains
        SUM(z_i) <= 1; objective is linear SUM(b1 + b2)."""
        sql = """
            WITH data AS (
                SELECT 1 AS id UNION ALL SELECT 2 UNION ALL SELECT 3
            )
            SELECT id, b1, b2
            FROM data
            DECIDE b1(BOOL), b2(BOOL)
            SUCH THAT SUM(b1 * b2) <= 1
            MAXIMIZE SUM(b1 + b2)
        """
        t0 = time.perf_counter()
        result, cols = decidb_cli.execute(sql)
        decidb_time = time.perf_counter() - t0

        n = 3
        t_build = time.perf_counter()
        oracle_solver.create_model("bool_bilinear_constraint")
        b1n = [f"b1_{i}" for i in range(n)]
        b2n = [f"b2_{i}" for i in range(n)]
        zn = [f"z_{i}" for i in range(n)]
        for v in b1n + b2n:
            oracle_solver.add_variable(v, VarType.BINARY)
        for v in zn:
            oracle_solver.add_variable(v, VarType.CONTINUOUS, lb=0.0, ub=1.0)
        for i in range(n):
            _mccormick_link(oracle_solver, zn[i], b1n[i], b2n[i], 1.0, f"mc_{i}")
        oracle_solver.add_constraint({z: 1.0 for z in zn}, "<=", 1.0, name="cap_z")
        oracle_solver.set_objective(
            {**{b: 1.0 for b in b1n}, **{b: 1.0 for b in b2n}}, ObjSense.MAXIMIZE,
        )
        build_time = time.perf_counter() - t_build
        res = oracle_solver.solve()
        assert res.status == SolverStatus.OPTIMAL

        b1_col = cols.index("b1")
        b2_col = cols.index("b2")
        decidb_sum = sum(int(row[b1_col]) + int(row[b2_col]) for row in result)
        assert abs(decidb_sum - res.objective_value) <= 1e-6, (
            f"Objective mismatch: DecidB={decidb_sum}, Oracle={res.objective_value}"
        )

        total_product = sum(int(row[b1_col]) * int(row[b2_col]) for row in result)
        assert total_product <= 1, f"Constraint violated: SUM(b1*b2)={total_product}"

        perf_tracker.record(
            "bool_bilinear_constraint", decidb_time, build_time,
            res.solve_time_seconds, n, n * 3, n * 3 + 1,
            res.objective_value, oracle_solver.solver_name(),
            comparison_status="optimal",
        )

    def test_bilinear_constraint_and_bilinear_objective_together(
        self, decidb_cli, oracle_solver, perf_tracker,
    ):
        """SUCH THAT SUM(b1 * b2) <= 1, MAXIMIZE SUM(b2 * b3) — a bilinear term in
        SUCH THAT and a *different* bilinear term in the objective, in one query.

        Regression for the constraint-side and objective-side bilinear coefficient
        evaluators sharing one code path (`EvaluateBilinearTerms`): pins that both
        still agree with an independent oracle when exercised together, not just
        individually.
        """
        sql = """
            WITH data AS (
                SELECT 1 AS id UNION ALL SELECT 2 UNION ALL SELECT 3
            )
            SELECT id, b1, b2, b3
            FROM data
            DECIDE b1(BOOL), b2(BOOL), b3(BOOL)
            SUCH THAT SUM(b1 * b2) <= 1
            MAXIMIZE SUM(b2 * b3)
        """
        t0 = time.perf_counter()
        result, cols = decidb_cli.execute(sql)
        decidb_time = time.perf_counter() - t0

        n = 3
        t_build = time.perf_counter()
        oracle_solver.create_model("bilinear_constraint_and_objective_together")
        b1n = [f"b1_{i}" for i in range(n)]
        b2n = [f"b2_{i}" for i in range(n)]
        b3n = [f"b3_{i}" for i in range(n)]
        z1n = [f"z1_{i}" for i in range(n)]  # b1 * b2, constrained
        z2n = [f"z2_{i}" for i in range(n)]  # b2 * b3, objective
        for v in b1n + b2n + b3n:
            oracle_solver.add_variable(v, VarType.BINARY)
        for v in z1n + z2n:
            oracle_solver.add_variable(v, VarType.CONTINUOUS, lb=0.0, ub=1.0)
        for i in range(n):
            _mccormick_link(oracle_solver, z1n[i], b1n[i], b2n[i], 1.0, f"mc1_{i}")
            _mccormick_link(oracle_solver, z2n[i], b2n[i], b3n[i], 1.0, f"mc2_{i}")
        oracle_solver.add_constraint({z: 1.0 for z in z1n}, "<=", 1.0, name="cap_z1")
        oracle_solver.set_objective({z: 1.0 for z in z2n}, ObjSense.MAXIMIZE)
        build_time = time.perf_counter() - t_build
        res = oracle_solver.solve()
        assert res.status == SolverStatus.OPTIMAL

        b1_col = cols.index("b1")
        b2_col = cols.index("b2")
        b3_col = cols.index("b3")
        decidb_obj = sum(int(row[b2_col]) * int(row[b3_col]) for row in result)
        assert abs(decidb_obj - res.objective_value) <= 1e-6, (
            f"Objective mismatch: DecidB={decidb_obj}, Oracle={res.objective_value}"
        )

        total_product = sum(int(row[b1_col]) * int(row[b2_col]) for row in result)
        assert total_product <= 1, f"Constraint violated: SUM(b1*b2)={total_product}"

        perf_tracker.record(
            "bilinear_constraint_and_objective_together", decidb_time, build_time,
            res.solve_time_seconds, n, n * 5, n * 5 + 1,
            res.objective_value, oracle_solver.solver_name(),
            comparison_status="optimal",
        )


# ===================================================================
# Factored products: degree vs. occurrence count
# ===================================================================


@pytest.mark.correctness
@pytest.mark.bilinear
@pytest.mark.var_integer
class TestFactoredProductDegree:
    """A product whose factors are sums is degree 2, not degree 3.

    `(x + y) * z` expands to `x*z + y*z` — every term bilinear, so the whole
    expression is degree 2 and legal wherever bilinear terms are. The binder
    used to count DECIDE variable OCCURRENCES (3 here) rather than compute
    polynomial DEGREE (2), and rejected these as triple products.

    Objectives were masked: the symbolic layer expanded the product before the
    check ran. Constraints were never expanded, so the *same expression* was
    accepted in MAXIMIZE and rejected in SUCH THAT. These tests pin both the
    acceptance and the objective/constraint symmetry.

    Assertions are differential — the factored spelling must agree with the
    hand-expanded spelling — so they test the property at issue without
    depending on a hand-computed optimum. Objective coefficients are distinct
    powers of two so each optimum is unique and ties cannot make the
    comparison flaky.
    """

    OBJECTIVE = "MAXIMIZE SUM(8 * x + 4 * y + 2 * z + w)"

    @classmethod
    def _run_constraint(cls, cli, constraint_expr):
        sql = f"""
            WITH data AS (SELECT 1 AS id, 2.0 AS p)
            SELECT id, x, y, z, w
            FROM data
            DECIDE x(INT), y(INT), z(INT), w(INT)
            SUCH THAT x >= 0 AND x <= 1 AND y >= 0 AND y <= 1
                  AND z >= 0 AND z <= 1 AND w >= 0 AND w <= 1
                  AND {constraint_expr}
            {cls.OBJECTIVE}
        """
        return cli.execute(sql)

    @pytest.mark.cons_aggregate
    @pytest.mark.parametrize("factored,expanded", [
        # Two-term sum times a third variable — the canonical gating shape.
        ("SUM((x + y) * z) <= 1",
         "SUM(x * z + y * z) <= 1"),
        # Three-term sum: 4 occurrences, still degree 2.
        ("SUM((x + y + z) * w) <= 1",
         "SUM(x * w + y * w + z * w) <= 1"),
        # Sum times sum: 4 occurrences, still degree 2.
        ("SUM((x + y) * (z + w)) <= 1",
         "SUM(x * z + x * w + y * z + y * w) <= 1"),
        # Data coefficient on a factored bilinear term.
        ("SUM(p * (x + y) * z) <= 2",
         "SUM(p * x * z + p * y * z) <= 2"),
        # Factored quadratic: the same variable on both sides of the product.
        ("SUM((x + y) * x) <= 1",
         "SUM(x * x + y * x) <= 1"),
    ])
    def test_factored_aggregate_constraint_matches_expanded(
        self, decidb_cli_gurobi, factored, expanded,
    ):
        factored_rows, factored_cols = self._run_constraint(decidb_cli_gurobi, factored)
        expanded_rows, expanded_cols = self._run_constraint(decidb_cli_gurobi, expanded)

        assert factored_cols == expanded_cols
        assert factored_rows == expanded_rows

    @pytest.mark.cons_perrow
    def test_factored_per_row_constraint_matches_expanded(self, decidb_cli_gurobi):
        """The per-row path (no reducer) must accept the factored form too."""
        factored_rows, factored_cols = self._run_constraint(
            decidb_cli_gurobi, "(x + y) * z <= 1",
        )
        expanded_rows, expanded_cols = self._run_constraint(
            decidb_cli_gurobi, "x * z + y * z <= 1",
        )

        assert factored_cols == expanded_cols
        assert factored_rows == expanded_rows

    @pytest.mark.obj_maximize
    @pytest.mark.cons_aggregate
    def test_same_expression_accepted_in_objective_and_constraint(self, decidb_cli_gurobi):
        """`SUM((x + y) * z)` must bind in both clauses.

        Regression for the asymmetry: accepted in MAXIMIZE (symbolic layer
        expanded it) but rejected in SUCH THAT (nothing expanded it).
        """
        as_objective, _ = decidb_cli_gurobi.execute("""
            WITH data AS (SELECT 1 AS id)
            SELECT id, x, y, z
            FROM data
            DECIDE x(INT), y(INT), z(INT)
            SUCH THAT x >= 0 AND x <= 1 AND y >= 0 AND y <= 1 AND z >= 0 AND z <= 1
                  AND SUM(z) = 1
            MAXIMIZE SUM((x + y) * z)
        """)
        assert len(as_objective) == 1

        as_constraint, _ = self._run_constraint(decidb_cli_gurobi, "SUM((x + y) * z) <= 1")
        assert len(as_constraint) == 1

    @pytest.mark.cons_aggregate
    @pytest.mark.parametrize("constraint_expr", [
        # Three distinct variables multiplied: genuinely degree 3.
        "SUM(x * y * z) <= 1",
        # A degree-2 factor multiplied by a third variable: degree 3.
        "SUM((x + y) * z * w) <= 1",
        "SUM((x + y) * (z * w)) <= 1",
    ])
    def test_genuine_degree_three_still_rejected(self, decidb_cli, constraint_expr):
        """The fix must not widen the gate: real degree > 2 still fails."""
        decidb_cli.assert_error(f"""
            WITH data AS (SELECT 1 AS id)
            SELECT id, x, y, z, w
            FROM data
            DECIDE x(INT), y(INT), z(INT), w(INT)
            SUCH THAT x >= 0 AND x <= 1 AND y >= 0 AND y <= 1
                  AND z >= 0 AND z <= 1 AND w >= 0 AND w <= 1
                  AND {constraint_expr}
            {self.OBJECTIVE}
        """, match=r"Triple|higher-order")


# ===================================================================
# Error cases
# ===================================================================


@pytest.mark.correctness
class TestBilinearErrors:
    """Error cases that should be rejected."""

    def test_triple_product_rejected(self, decidb_cli):
        """SUM(a * b * c) with three DECIDE variables should be rejected."""
        decidb_cli.assert_error("""
            WITH data AS (SELECT 1 AS id)
            SELECT id, a, b, c FROM data
            DECIDE a(BOOL), b(BOOL), c(BOOL)
            SUCH THAT a >= 0 AND b >= 0 AND c >= 0
            MAXIMIZE SUM(a * b * c)
        """, match=r"Triple|higher-order")

    def test_quad_bilinear_chain_rejected(self, decidb_cli):
        """SUM((b1 * x) * (b2 * y)) — degree-4 chain of two bilinear terms.

        Each factor is itself a Bool × Real product, so the total shape has
        degree 4. Must be rejected by the same higher-order-product guard
        that catches a * b * c.
        """
        decidb_cli.assert_error("""
            WITH data AS (SELECT 1 AS id)
            SELECT id, b1, b2, x, y FROM data
            DECIDE b1(BOOL), b2(BOOL), x(REAL), y(REAL)
            SUCH THAT x <= 10 AND y <= 10
            MAXIMIZE SUM((b1 * x) * (b2 * y))
        """, match=r"Triple|higher-order")

    def test_missing_upper_bound_rejected(self, decidb_cli):
        """b * x without an upper bound on x should be rejected."""
        decidb_cli.assert_error("""
            WITH data AS (SELECT 1 AS id)
            SELECT id, b, x FROM data
            DECIDE b(BOOL), x(REAL)
            SUCH THAT b >= 0
            MAXIMIZE SUM(b * x)
        """, match=r"finite upper bound|upper bound")

    def test_highs_nonconvex_bilinear_rejected(self, decidb_cli):
        """Real × Real bilinear on HiGHS should error (non-convex)."""
        sql = """
            WITH data AS (SELECT 1 AS id)
            SELECT id, ROUND(x, 2) AS x, ROUND(y, 2) AS y
            FROM data
            DECIDE x(REAL), y(REAL)
            SUCH THAT x >= 0 AND x <= 10 AND y >= 0 AND y <= 10
            MAXIMIZE SUM(x * y)
        """
        try:
            result, cols = decidb_cli.execute(sql)
            # If Gurobi is available, this is fine (non-convex QP)
            pass  # Gurobi handled it
        except DecidBCliError as e:
            # HiGHS: non-convex rejection expected
            assert re.search(r"Non-convex|require Gurobi", e.message), \
                f"Unexpected error: {e.message}"


# ===================================================================
# Backward compatibility
# ===================================================================


@pytest.mark.correctness
class TestBilinearBackwardCompat:
    """Ensure existing QP and linear objectives still work."""

    def test_power_still_works(self, decidb_cli, oracle_solver, perf_tracker):
        """MINIMIZE SUM(POWER(x - target, 2)) — oracle mirrors the expanded QP.

        Expansion of (x_i - t_i)^2 drops the constant t_i^2 term (DecidB's
        reported objective does the same), leaving linear=-2*t_i*x_i and
        quadratic=x_i^2. The decidb-side objective subtracts t_i^2 to match.
        """
        sql = """
            WITH data AS (
                SELECT 1 AS id, 10.0 AS target UNION ALL
                SELECT 2, 20.0
            )
            SELECT id, ROUND(x, 4) AS x
            FROM data
            DECIDE x(REAL)
            SUCH THAT x >= 0 AND x <= 100
            MINIMIZE SUM(POWER(x - target, 2))
        """
        t0 = time.perf_counter()
        result, cols = decidb_cli.execute(sql)
        decidb_time = time.perf_counter() - t0

        data = [(1, 10.0), (2, 20.0)]
        n = len(data)
        t_build = time.perf_counter()
        oracle_solver.create_model("power_still_works")
        xnames = [f"x_{i}" for i in range(n)]
        for xn in xnames:
            oracle_solver.add_variable(xn, VarType.CONTINUOUS, lb=0.0, ub=100.0)
        linear = {xnames[i]: -2.0 * data[i][1] for i in range(n)}
        quadratic = {(xnames[i], xnames[i]): 1.0 for i in range(n)}
        oracle_solver.set_quadratic_objective(linear, quadratic, ObjSense.MINIMIZE)
        build_time = time.perf_counter() - t_build
        res = oracle_solver.solve()
        assert res.status == SolverStatus.OPTIMAL

        x_col = cols.index("x")
        id_col = cols.index("id")
        target_by_id = dict(data)
        decidb_obj = sum(
            (float(row[x_col]) - target_by_id[int(row[id_col])]) ** 2
            - target_by_id[int(row[id_col])] ** 2
            for row in result
        )
        assert abs(decidb_obj - res.objective_value) <= 1e-3, (
            f"Objective mismatch: DecidB={decidb_obj:.4f}, Oracle={res.objective_value:.4f}"
        )
        perf_tracker.record(
            "power_still_works", decidb_time, build_time,
            res.solve_time_seconds, n, n, 0,
            res.objective_value, oracle_solver.solver_name(),
            comparison_status="optimal",
        )

    def test_linear_objective_still_works(
        self, decidb_cli, oracle_solver, perf_tracker,
    ):
        """Simple linear MAXIMIZE SUM(profit * x) — oracle-compared."""
        sql = """
            WITH data AS (
                SELECT 1 AS id, 10.0 AS profit UNION ALL
                SELECT 2, 5.0 UNION ALL
                SELECT 3, 8.0
            )
            SELECT id, profit, x
            FROM data
            DECIDE x(BOOL)
            SUCH THAT SUM(x) <= 2
            MAXIMIZE SUM(profit * x)
        """
        t0 = time.perf_counter()
        result, cols = decidb_cli.execute(sql)
        decidb_time = time.perf_counter() - t0

        data = [(1, 10.0), (2, 5.0), (3, 8.0)]
        n = len(data)
        t_build = time.perf_counter()
        oracle_solver.create_model("linear_objective_still_works")
        xnames = [f"x_{i}" for i in range(n)]
        for xn in xnames:
            oracle_solver.add_variable(xn, VarType.BINARY)
        oracle_solver.add_constraint(
            {xn: 1.0 for xn in xnames}, "<=", 2.0, name="cap",
        )
        oracle_solver.set_objective(
            {xnames[i]: data[i][1] for i in range(n)}, ObjSense.MAXIMIZE,
        )
        build_time = time.perf_counter() - t_build
        res = oracle_solver.solve()
        assert res.status == SolverStatus.OPTIMAL

        x_col = cols.index("x")
        profit_col = cols.index("profit")
        decidb_obj = sum(
            float(row[profit_col]) * int(row[x_col]) for row in result
        )
        assert abs(decidb_obj - res.objective_value) <= 1e-3, (
            f"Objective mismatch: DecidB={decidb_obj}, Oracle={res.objective_value}"
        )
        perf_tracker.record(
            "linear_objective_still_works", decidb_time, build_time,
            res.solve_time_seconds, n, n, 1,
            res.objective_value, oracle_solver.solver_name(),
            comparison_status="optimal",
        )

    def test_identical_multiplication_still_qp(
        self, decidb_cli, oracle_solver, perf_tracker,
    ):
        """(x - target) * (x - target) should still be treated as QP, not bilinear.
        Oracle mirrors the expanded form with diagonal Q entry x_i^2."""
        sql = """
            WITH data AS (
                SELECT 1 AS id, 7.0 AS target UNION ALL
                SELECT 2, 42.0
            )
            SELECT id, ROUND(x, 4) AS x
            FROM data
            DECIDE x(REAL)
            SUCH THAT x >= 0 AND x <= 100
            MINIMIZE SUM((x - target) * (x - target))
        """
        t0 = time.perf_counter()
        result, cols = decidb_cli.execute(sql)
        decidb_time = time.perf_counter() - t0

        data = [(1, 7.0), (2, 42.0)]
        n = len(data)
        t_build = time.perf_counter()
        oracle_solver.create_model("identical_multiplication_still_qp")
        xnames = [f"x_{i}" for i in range(n)]
        for xn in xnames:
            oracle_solver.add_variable(xn, VarType.CONTINUOUS, lb=0.0, ub=100.0)
        linear = {xnames[i]: -2.0 * data[i][1] for i in range(n)}
        quadratic = {(xnames[i], xnames[i]): 1.0 for i in range(n)}
        oracle_solver.set_quadratic_objective(linear, quadratic, ObjSense.MINIMIZE)
        build_time = time.perf_counter() - t_build
        res = oracle_solver.solve()
        assert res.status == SolverStatus.OPTIMAL

        x_col = cols.index("x")
        id_col = cols.index("id")
        target_by_id = dict(data)
        decidb_obj = sum(
            (float(row[x_col]) - target_by_id[int(row[id_col])]) ** 2
            - target_by_id[int(row[id_col])] ** 2
            for row in result
        )
        assert abs(decidb_obj - res.objective_value) <= 1e-3, (
            f"Objective mismatch: DecidB={decidb_obj:.4f}, Oracle={res.objective_value:.4f}"
        )
        perf_tracker.record(
            "identical_multiplication_still_qp", decidb_time, build_time,
            res.solve_time_seconds, n, n, 0,
            res.objective_value, oracle_solver.solver_name(),
            comparison_status="optimal",
        )


# ===================================================================
# Cross-feature interactions (oracle-compared, module-level)
# ===================================================================
#
# Each test mirrors DecidB's McCormick linearization in the oracle:
#   For Bool b ∈ {0,1} and Real x ∈ [0, U]:
#     w = b*x  ⇒  w ∈ [0, U],  w <= U*b,  w <= x,  w >= x - U*(1-b)
# A scoping bug (e.g., aux variable shared across PER groups) would relax
# the feasible region and produce a strictly better objective than the
# oracle — caught by the equality assertion.


def _mccormick_link(oracle, w, b, x, U, prefix):
    """Add the four McCormick constraints linking aux w = b*x with x ∈ [0, U]."""
    oracle.add_constraint({w: 1.0, b: -U}, "<=", 0.0, name=f"{prefix}_wUb")
    oracle.add_constraint({w: 1.0, x: -1.0}, "<=", 0.0, name=f"{prefix}_wx")
    oracle.add_constraint({w: 1.0, x: -1.0, b: -U}, ">=", -U, name=f"{prefix}_wlow")


@pytest.mark.per_clause
@pytest.mark.var_boolean
@pytest.mark.var_real
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_bilinear_per_group(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """SUM(b*x) <= 100 PER l_returnflag with bilinear MAXIMIZE objective.
    McCormick aux variables w_i = b_i*x_i must be partitioned per group; a
    global-scoping bug would let one group's slack absorb another's mass."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_returnflag,
               b, ROUND(x, 4) AS x
        FROM lineitem WHERE l_orderkey <= 10
        DECIDE b(BOOL), x(REAL)
        SUCH THAT x <= 50 AND SUM(b * x) <= 100 PER l_returnflag
        MAXIMIZE SUM(l_extendedprice * b * x)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT), CAST(l_linenumber AS BIGINT),
               CAST(l_extendedprice AS DOUBLE), CAST(l_returnflag AS VARCHAR)
        FROM lineitem WHERE l_orderkey <= 10
        ORDER BY l_orderkey, l_linenumber
    """).fetchall()
    n = len(data)
    U = 50.0
    K = 100.0

    t_build = time.perf_counter()
    oracle_solver.create_model("bilinear_per_group")
    bnames = [f"b_{i}" for i in range(n)]
    xnames = [f"x_{i}" for i in range(n)]
    wnames = [f"w_{i}" for i in range(n)]
    for vn in bnames:
        oracle_solver.add_variable(vn, VarType.BINARY)
    for vn in xnames:
        oracle_solver.add_variable(vn, VarType.CONTINUOUS, lb=0.0, ub=U)
    for vn in wnames:
        oracle_solver.add_variable(vn, VarType.CONTINUOUS, lb=0.0, ub=U)
    for i in range(n):
        _mccormick_link(oracle_solver, wnames[i], bnames[i], xnames[i], U, f"mc_{i}")

    groups: dict[str, list[int]] = {}
    for i, row in enumerate(data):
        groups.setdefault(row[3], []).append(i)
    for g, idxs in groups.items():
        oracle_solver.add_constraint(
            {wnames[i]: 1.0 for i in idxs}, "<=", K, name=f"per_{g}",
        )

    oracle_solver.set_objective(
        {wnames[i]: data[i][2] for i in range(n)}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL

    ci = {name: i for i, name in enumerate(decidb_cols)}
    decidb_obj = sum(
        int(row[ci["b"]]) * float(row[ci["x"]]) * float(row[ci["l_extendedprice"]])
        for row in decidb_result
    )
    assert abs(decidb_obj - result.objective_value) <= 1.0, (
        f"Objective mismatch: DecidB={decidb_obj:.2f}, Oracle={result.objective_value:.2f}"
    )

    by_grp: dict[str, float] = {}
    for row in decidb_result:
        flag = str(row[ci["l_returnflag"]])
        by_grp[flag] = by_grp.get(flag, 0.0) + int(row[ci["b"]]) * float(row[ci["x"]])
    for g, total in by_grp.items():
        assert total <= K + 1e-6, f"Group {g}: SUM(b*x)={total} > {K}"

    perf_tracker.record(
        "bilinear_per_group", decidb_time, build_time,
        result.solve_time_seconds, n, n * 3, n * 3 + len(groups),
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )


@pytest.mark.per_clause
@pytest.mark.when_constraint
@pytest.mark.when_objective
@pytest.mark.var_boolean
@pytest.mark.var_real
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_bilinear_when_per_triple(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """WHEN filter + PER groups + bilinear McCormick — all three must compose.
    Only AIR/RAIL rows contribute to the per-group SUM and the objective."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_returnflag,
               l_shipmode, b, ROUND(x, 4) AS x
        FROM lineitem WHERE l_orderkey <= 10
        DECIDE b(BOOL), x(REAL)
        SUCH THAT x <= 50
            AND SUM(b * x) <= 80 WHEN (l_shipmode = 'AIR' OR l_shipmode = 'RAIL') PER l_returnflag
        MAXIMIZE SUM(l_extendedprice * b * x) WHEN (l_shipmode = 'AIR' OR l_shipmode = 'RAIL')
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT), CAST(l_linenumber AS BIGINT),
               CAST(l_extendedprice AS DOUBLE), CAST(l_returnflag AS VARCHAR),
               CAST(l_shipmode AS VARCHAR)
        FROM lineitem WHERE l_orderkey <= 10
        ORDER BY l_orderkey, l_linenumber
    """).fetchall()
    n = len(data)
    U = 50.0
    K = 80.0
    keep = [row[4] in ("AIR", "RAIL") for row in data]

    t_build = time.perf_counter()
    oracle_solver.create_model("bilinear_when_per_triple")
    bnames = [f"b_{i}" for i in range(n)]
    xnames = [f"x_{i}" for i in range(n)]
    wnames = [f"w_{i}" for i in range(n)]
    for vn in bnames:
        oracle_solver.add_variable(vn, VarType.BINARY)
    for vn in xnames:
        oracle_solver.add_variable(vn, VarType.CONTINUOUS, lb=0.0, ub=U)
    for vn in wnames:
        oracle_solver.add_variable(vn, VarType.CONTINUOUS, lb=0.0, ub=U)
    for i in range(n):
        _mccormick_link(oracle_solver, wnames[i], bnames[i], xnames[i], U, f"mc_{i}")

    # WHEN→PER: groups partition only the kept rows. Empty groups skip (default).
    groups: dict[str, list[int]] = {}
    for i, row in enumerate(data):
        if keep[i]:
            groups.setdefault(row[3], []).append(i)
    for g, idxs in groups.items():
        oracle_solver.add_constraint(
            {wnames[i]: 1.0 for i in idxs}, "<=", K, name=f"per_{g}",
        )

    oracle_solver.set_objective(
        {wnames[i]: data[i][2] for i in range(n) if keep[i]},
        ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL

    ci = {name: i for i, name in enumerate(decidb_cols)}
    decidb_obj = sum(
        int(row[ci["b"]]) * float(row[ci["x"]]) * float(row[ci["l_extendedprice"]])
        for row in decidb_result
        if str(row[ci["l_shipmode"]]) in ("AIR", "RAIL")
    )
    assert abs(decidb_obj - result.objective_value) <= 1.0, (
        f"Objective mismatch: DecidB={decidb_obj:.2f}, Oracle={result.objective_value:.2f}"
    )

    by_grp: dict[str, float] = {}
    for row in decidb_result:
        if str(row[ci["l_shipmode"]]) not in ("AIR", "RAIL"):
            continue
        flag = str(row[ci["l_returnflag"]])
        by_grp[flag] = by_grp.get(flag, 0.0) + int(row[ci["b"]]) * float(row[ci["x"]])
    for g, total in by_grp.items():
        assert total <= K + 1e-6, f"Group {g} (AIR/RAIL only): SUM(b*x)={total} > {K}"

    perf_tracker.record(
        "bilinear_when_per_triple", decidb_time, build_time,
        result.solve_time_seconds, n, n * 3, n * 3 + len(groups),
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )


@pytest.mark.var_boolean
@pytest.mark.var_real
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_bilinear_entity_scoped(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Entity-scoped Boolean keepN (per nation) × row-scoped Real x.
    McCormick aux per join row must use the entity-keyed Boolean — multiple
    rows of the same nation share the same keepN variable."""
    sql = """
        SELECT c.c_custkey, n.n_nationkey, c.c_acctbal, keepN, ROUND(x, 4) AS x
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0 AND c.c_custkey <= 200
        DECIDE n.keepN(BOOL), x(REAL)
        SUCH THAT x <= 100 AND SUM(keepN * x) <= 1000
        MAXIMIZE SUM(keepN * x * c_acctbal)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(c.c_custkey AS BIGINT), CAST(n.n_nationkey AS BIGINT),
               CAST(c.c_acctbal AS DOUBLE)
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0 AND c.c_custkey <= 200
        ORDER BY c.c_custkey, n.n_nationkey
    """).fetchall()
    n = len(data)
    U = 100.0
    K = 1000.0
    nations = sorted({int(row[1]) for row in data})

    t_build = time.perf_counter()
    oracle_solver.create_model("bilinear_entity_scoped")
    knames = {nk: f"keepN_{nk}" for nk in nations}
    xnames = [f"x_{i}" for i in range(n)]
    wnames = [f"w_{i}" for i in range(n)]
    for vn in knames.values():
        oracle_solver.add_variable(vn, VarType.BINARY)
    for vn in xnames:
        oracle_solver.add_variable(vn, VarType.CONTINUOUS, lb=0.0, ub=U)
    for vn in wnames:
        oracle_solver.add_variable(vn, VarType.CONTINUOUS, lb=0.0, ub=U)
    for i, row in enumerate(data):
        nk = int(row[1])
        _mccormick_link(oracle_solver, wnames[i], knames[nk], xnames[i], U, f"mc_{i}")

    oracle_solver.add_constraint(
        {wnames[i]: 1.0 for i in range(n)}, "<=", K, name="cap",
    )
    oracle_solver.set_objective(
        {wnames[i]: float(data[i][2]) for i in range(n)}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL

    ci = {name: i for i, name in enumerate(decidb_cols)}

    # Sanity: same nation → same keepN
    nation_keep: dict[int, int] = {}
    for row in decidb_result:
        nk = int(row[ci["n_nationkey"]])
        kv = int(row[ci["keepN"]])
        if nk in nation_keep:
            assert nation_keep[nk] == kv, f"Nation {nk}: inconsistent keepN"
        else:
            nation_keep[nk] = kv

    decidb_obj = sum(
        int(row[ci["keepN"]]) * float(row[ci["x"]]) * float(row[ci["c_acctbal"]])
        for row in decidb_result
    )
    total_w = sum(
        int(row[ci["keepN"]]) * float(row[ci["x"]]) for row in decidb_result
    )
    assert total_w <= K + 1e-6, f"SUM(keepN*x)={total_w} > {K}"
    assert abs(decidb_obj - result.objective_value) <= 1.0, (
        f"Objective mismatch: DecidB={decidb_obj:.2f}, Oracle={result.objective_value:.2f}"
    )

    perf_tracker.record(
        "bilinear_entity_scoped", decidb_time, build_time,
        result.solve_time_seconds, n, len(nations) + n * 2, n * 3 + 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )


@pytest.mark.var_boolean
@pytest.mark.var_real
@pytest.mark.cons_aggregate
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_bilinear_minimize_objective(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """MINIMIZE SUM(cost * b * x) with x ∈ [2, 10] and SUM(b) >= 2.
    Regression for left-associative parse `(coeff*b)*x`: previously the
    optimizer dropped the `cost` coefficient, causing suboptimal solutions
    that minimized SUM(b*x) instead of SUM(cost*b*x)."""
    sql = """
        WITH data AS (
            SELECT 1 AS id, 5.0 AS cost UNION ALL
            SELECT 2, 10.0 UNION ALL
            SELECT 3, 3.0
        )
        SELECT id, cost, b, ROUND(x, 4) AS x
        FROM data
        DECIDE b(BOOL), x(REAL)
        SUCH THAT x >= 2 AND x <= 10 AND SUM(b) >= 2
        MINIMIZE SUM(cost * b * x)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = [(1, 5.0), (2, 10.0), (3, 3.0)]
    n = len(data)
    L, U = 2.0, 10.0

    t_build = time.perf_counter()
    oracle_solver.create_model("bilinear_minimize")
    bnames = [f"b_{i}" for i in range(n)]
    xnames = [f"x_{i}" for i in range(n)]
    wnames = [f"w_{i}" for i in range(n)]
    for vn in bnames:
        oracle_solver.add_variable(vn, VarType.BINARY)
    for vn in xnames:
        oracle_solver.add_variable(vn, VarType.CONTINUOUS, lb=L, ub=U)
    for vn in wnames:
        oracle_solver.add_variable(vn, VarType.CONTINUOUS, lb=0.0, ub=U)
    # McCormick using x's effective upper bound U=10. With x in [L, U] and
    # DecidB's structural w<=x bound, w can fall below L*b only when b=0
    # (then w=0). When b=1, w>=x-U*0=x>=L. So the link below is exact.
    for i in range(n):
        _mccormick_link(oracle_solver, wnames[i], bnames[i], xnames[i], U, f"mc_{i}")

    oracle_solver.add_constraint(
        {bnames[i]: 1.0 for i in range(n)}, ">=", 2.0, name="card",
    )
    oracle_solver.set_objective(
        {wnames[i]: float(data[i][1]) for i in range(n)}, ObjSense.MINIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL

    ci = {name: i for i, name in enumerate(decidb_cols)}
    decidb_obj = sum(
        float(row[ci["cost"]]) * int(row[ci["b"]]) * float(row[ci["x"]])
        for row in decidb_result
    )
    assert abs(decidb_obj - result.objective_value) <= 1e-3, (
        f"Objective mismatch: DecidB={decidb_obj:.4f}, Oracle={result.objective_value:.4f}"
    )

    n_picked = sum(int(row[ci["b"]]) for row in decidb_result)
    assert n_picked >= 2, f"Cardinality violated: SUM(b)={n_picked} < 2"

    perf_tracker.record(
        "bilinear_minimize_objective", decidb_time, build_time,
        result.solve_time_seconds, n, n * 3, n * 3 + 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )


def _run_split_coefficient_bilinear_query(decidb_cli, objective_expr):
    sql = f"""
        WITH t(id, a, b) AS (
            VALUES (1, 4.0, 4.0), (2, 9.0, 1.0), (3, 1.0, 9.0)
        )
        SELECT id, a, b, x, y
        FROM t
        DECIDE x(INT), y(INT)
        SUCH THAT x >= 0 AND x <= 1 AND y >= 0 AND y <= 1
              AND SUM(x) = 1 AND SUM(y) = 1
        MAXIMIZE SUM({objective_expr})
    """
    return decidb_cli.execute(sql)


def _build_split_coefficient_bilinear_oracle(oracle_solver):
    data = [(1, 4.0, 4.0), (2, 9.0, 1.0), (3, 1.0, 9.0)]
    n = len(data)

    oracle_solver.create_model("bilinear_split_coefficient_objective")
    xnames = [f"x_{i}" for i in range(n)]
    ynames = [f"y_{i}" for i in range(n)]
    for vn in xnames + ynames:
        oracle_solver.add_variable(vn, VarType.INTEGER, lb=0.0, ub=1.0)

    oracle_solver.add_constraint(
        {xnames[i]: 1.0 for i in range(n)}, "=", 1.0, name="pick_x",
    )
    oracle_solver.add_constraint(
        {ynames[i]: 1.0 for i in range(n)}, "=", 1.0, name="pick_y",
    )
    oracle_solver.set_quadratic_objective(
        {},
        {(xnames[i], ynames[i]): data[i][1] * data[i][2] for i in range(n)},
        ObjSense.MAXIMIZE,
    )
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL
    return result


def _decidb_split_coefficient_objective(decidb_rows, decidb_cols):
    ci = {name: i for i, name in enumerate(decidb_cols)}
    return sum(
        float(row[ci["a"]]) * float(row[ci["b"]]) * int(row[ci["x"]]) * int(row[ci["y"]])
        for row in decidb_rows
    )


def _selected_ids_xy(decidb_rows, decidb_cols):
    ci = {name: i for i, name in enumerate(decidb_cols)}
    return {
        int(row[ci["id"]]) for row in decidb_rows
        if int(row[ci["x"]]) == 1 and int(row[ci["y"]]) == 1
    }


@pytest.mark.var_integer
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.bilinear
@pytest.mark.correctness
def test_bilinear_objective_multiplies_both_side_coeffs(decidb_cli_gurobi, oracle_solver):
    """Regression: (a*x)*(b*y) must use coefficient a*b (not just one side)."""
    decidb_rows, decidb_cols = _run_split_coefficient_bilinear_query(
        decidb_cli_gurobi, "(a * x) * (b * y)",
    )
    oracle_result = _build_split_coefficient_bilinear_oracle(oracle_solver)

    decidb_obj = _decidb_split_coefficient_objective(decidb_rows, decidb_cols)
    assert abs(decidb_obj - oracle_result.objective_value) <= 1e-6, (
        f"Objective mismatch: DecidB={decidb_obj}, "
        f"Oracle={oracle_result.objective_value}"
    )
    assert _selected_ids_xy(decidb_rows, decidb_cols) == {1}


@pytest.mark.var_integer
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.bilinear
@pytest.mark.correctness
def test_bilinear_objective_split_shape_matches_flat_product(decidb_cli_gurobi, oracle_solver):
    """(a*x)*(b*y) and a*b*x*y should produce the same optimal objective."""
    split_rows, split_cols = _run_split_coefficient_bilinear_query(
        decidb_cli_gurobi, "(a * x) * (b * y)",
    )
    flat_rows, flat_cols = _run_split_coefficient_bilinear_query(
        decidb_cli_gurobi, "a * b * x * y",
    )
    oracle_result = _build_split_coefficient_bilinear_oracle(oracle_solver)

    split_obj = _decidb_split_coefficient_objective(split_rows, split_cols)
    flat_obj = _decidb_split_coefficient_objective(flat_rows, flat_cols)
    assert abs(split_obj - oracle_result.objective_value) <= 1e-6, (
        f"Split-shape objective mismatch: DecidB={split_obj}, "
        f"Oracle={oracle_result.objective_value}"
    )
    assert abs(flat_obj - oracle_result.objective_value) <= 1e-6, (
        f"Flat-shape objective mismatch: DecidB={flat_obj}, "
        f"Oracle={oracle_result.objective_value}"
    )
    assert abs(split_obj - flat_obj) <= 1e-6, (
        f"Shape mismatch: split={split_obj}, flat={flat_obj}"
    )
    assert _selected_ids_xy(split_rows, split_cols) == {1}
    assert _selected_ids_xy(flat_rows, flat_cols) == {1}


@pytest.mark.var_integer
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.bilinear
@pytest.mark.correctness
def test_bilinear_objective_grouped_data_product_matches_flat_product(decidb_cli_gurobi):
    grouped_rows, grouped_cols = _run_split_coefficient_bilinear_query(
        decidb_cli_gurobi, "(a * b) * (x * y)",
    )
    flat_rows, flat_cols = _run_split_coefficient_bilinear_query(
        decidb_cli_gurobi, "a * b * x * y",
    )

    grouped_obj = _decidb_split_coefficient_objective(grouped_rows, grouped_cols)
    flat_obj = _decidb_split_coefficient_objective(flat_rows, flat_cols)
    assert abs(grouped_obj - flat_obj) <= 1e-6
    assert _selected_ids_xy(grouped_rows, grouped_cols) == {1}
    assert _selected_ids_xy(flat_rows, flat_cols) == {1}


def _run_single_row_bilinear_objective(decidb_cli, objective_expr):
    sql = f"""
        WITH data AS (SELECT 1 AS id)
        SELECT id, x, y, z
        FROM data
        DECIDE x(INT), y(INT), z(INT)
        SUCH THAT x >= 0 AND x <= 1
              AND y >= 0 AND y <= 1
              AND z >= 0 AND z <= 1
              AND SUM(z) = 1
        MAXIMIZE SUM({objective_expr})
    """
    return decidb_cli.execute(sql)


@pytest.mark.var_integer
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.bilinear
@pytest.mark.correctness
def test_bilinear_objective_expands_linear_plus_constant_factor(decidb_cli_gurobi):
    factored_rows, factored_cols = _run_single_row_bilinear_objective(
        decidb_cli_gurobi, "(x + 1) * y",
    )
    expanded_rows, expanded_cols = _run_single_row_bilinear_objective(
        decidb_cli_gurobi, "x * y + y",
    )

    assert factored_rows == expanded_rows
    assert factored_cols == expanded_cols
    assert int(factored_rows[0][factored_cols.index("x")]) == 1
    assert int(factored_rows[0][factored_cols.index("y")]) == 1


@pytest.mark.var_integer
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.bilinear
@pytest.mark.correctness
def test_bilinear_objective_expands_sum_factor(decidb_cli_gurobi):
    factored_rows, factored_cols = _run_single_row_bilinear_objective(
        decidb_cli_gurobi, "(x + y) * z",
    )
    expanded_rows, expanded_cols = _run_single_row_bilinear_objective(
        decidb_cli_gurobi, "x * z + y * z",
    )

    assert factored_rows == expanded_rows
    assert factored_cols == expanded_cols
    assert int(factored_rows[0][factored_cols.index("x")]) == 1
    assert int(factored_rows[0][factored_cols.index("y")]) == 1
    assert int(factored_rows[0][factored_cols.index("z")]) == 1


@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_minimize
@pytest.mark.bilinear
@pytest.mark.correctness
def test_bilinear_bool_bool_coeff_minimize(decidb_cli, oracle_solver):
    """MINIMIZE SUM(cost * b1 * b2) with both factors BOOLEAN.

    Exercises the AND-linearization branch of the 2026-04-15 bilinear
    coefficient-extraction fix. Design is shaped so the bug manifests:

      - SUM(b1) = 4 pins all b1=1, so b1 AND b2 ≡ b2 row-wise.
      - SUM(b2) = 2 forces picking exactly two rows.
      - Correct objective SUM(cost * b2) has a UNIQUE minimum at {b2_1=1,
        b2_3=1} (costs 2 + 3 = 5).
      - Bug-dropped-coeff objective SUM(b2) is degenerate at any pair,
        so a solver returning e.g. {0, 1} yields decidb_obj = 7 + 2 = 9
        and fails the oracle comparison against 5.

    AND-linearization is encoded in the oracle via `add_bool_and`
    (`_oracle_helpers.py:211`).
    """
    sql = """
        WITH data AS (
            SELECT 1 AS id, 7.0 AS cost UNION ALL
            SELECT 2, 2.0 UNION ALL
            SELECT 3, 5.0 UNION ALL
            SELECT 4, 3.0
        )
        SELECT id, cost, b1, b2
        FROM data
        DECIDE b1(BOOL), b2(BOOL)
        SUCH THAT SUM(b1) = 4 AND SUM(b2) = 2
        MINIMIZE SUM(cost * b1 * b2)
    """
    decidb_result, decidb_cols = decidb_cli.execute(sql)

    costs = [7.0, 2.0, 5.0, 3.0]
    n = len(costs)

    oracle_solver.create_model("bilinear_bool_bool_coeff_minimize")
    for i in range(n):
        oracle_solver.add_variable(f"b1_{i}", VarType.BINARY)
        oracle_solver.add_variable(f"b2_{i}", VarType.BINARY)
    for i in range(n):
        add_bool_and(oracle_solver, f"b1_{i}", f"b2_{i}", f"z_{i}")

    oracle_solver.add_constraint(
        {f"b1_{i}": 1.0 for i in range(n)}, "=", 4.0, name="pin_b1",
    )
    oracle_solver.add_constraint(
        {f"b2_{i}": 1.0 for i in range(n)}, "=", 2.0, name="pin_b2",
    )
    oracle_solver.set_objective(
        {f"z_{i}": costs[i] for i in range(n)}, ObjSense.MINIMIZE,
    )
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL

    ci = {name: i for i, name in enumerate(decidb_cols)}
    decidb_obj = sum(
        float(row[ci["cost"]]) * int(row[ci["b1"]]) * int(row[ci["b2"]])
        for row in decidb_result
    )
    assert abs(decidb_obj - result.objective_value) <= 1e-6, (
        f"Objective mismatch: DecidB={decidb_obj}, "
        f"Oracle={result.objective_value}"
    )

    n_b1 = sum(int(row[ci["b1"]]) for row in decidb_result)
    n_b2 = sum(int(row[ci["b2"]]) for row in decidb_result)
    assert n_b1 == 4, f"SUM(b1)={n_b1} != 4"
    assert n_b2 == 2, f"SUM(b2)={n_b2} != 2"


@pytest.mark.var_boolean
@pytest.mark.var_real
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_bilinear_bool_real_constraint(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Bool×Real bilinear in a SUCH THAT constraint (not an objective).
    McCormick must produce the correct feasible region for the constraint
    SUM(b*x) <= 15 while maximizing SUM(x). Three rows, x ∈ [0, 10]."""
    sql = """
        WITH data AS (
            SELECT 1 AS id UNION ALL SELECT 2 UNION ALL SELECT 3
        )
        SELECT id, b, ROUND(x, 4) AS x
        FROM data
        DECIDE b(BOOL), x(REAL)
        SUCH THAT x <= 10 AND SUM(b * x) <= 15
        MAXIMIZE SUM(x)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    n = 3
    U = 10.0
    K = 15.0

    t_build = time.perf_counter()
    oracle_solver.create_model("bilinear_bool_real_constraint")
    bnames = [f"b_{i}" for i in range(n)]
    xnames = [f"x_{i}" for i in range(n)]
    wnames = [f"w_{i}" for i in range(n)]
    for vn in bnames:
        oracle_solver.add_variable(vn, VarType.BINARY)
    for vn in xnames:
        oracle_solver.add_variable(vn, VarType.CONTINUOUS, lb=0.0, ub=U)
    for vn in wnames:
        oracle_solver.add_variable(vn, VarType.CONTINUOUS, lb=0.0, ub=U)
    for i in range(n):
        _mccormick_link(oracle_solver, wnames[i], bnames[i], xnames[i], U, f"mc_{i}")

    oracle_solver.add_constraint(
        {wnames[i]: 1.0 for i in range(n)}, "<=", K, name="cap",
    )
    oracle_solver.set_objective(
        {xnames[i]: 1.0 for i in range(n)}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL

    ci = {name: i for i, name in enumerate(decidb_cols)}
    decidb_x_sum = sum(float(row[ci["x"]]) for row in decidb_result)
    decidb_bx_sum = sum(
        int(row[ci["b"]]) * float(row[ci["x"]]) for row in decidb_result
    )
    assert decidb_bx_sum <= K + 1e-6, f"SUM(b*x)={decidb_bx_sum} > {K}"
    assert abs(decidb_x_sum - result.objective_value) <= 1e-3, (
        f"Objective mismatch: DecidB={decidb_x_sum:.4f}, Oracle={result.objective_value:.4f}"
    )

    perf_tracker.record(
        "bilinear_bool_real_constraint", decidb_time, build_time,
        result.solve_time_seconds, n, n * 3, n * 3 + 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )
