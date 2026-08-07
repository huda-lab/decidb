"""Infeasibility and unboundedness error tests — now cross-verified by gurobipy.

For each problem that DecidB must reject as infeasible / unbounded, the
oracle builds the same model in gurobipy and asserts the corresponding
``SolverStatus``. This guards against DecidB falsely reporting infeasibility
on a feasible problem (or vice versa).
"""

import pytest

from solver.types import ObjSense, SolverStatus, VarType


@pytest.mark.error_infeasible
@pytest.mark.error
class TestInfeasibleModels:
    """DecidB should raise InvalidInputException for infeasible problems,
    and the oracle should independently report INFEASIBLE."""

    def test_contradictory_per_row_bounds(
        self, decidb_cli, duckdb_conn, oracle_solver
    ):
        """Per-row bounds ``x >= 10 AND x <= 5`` contradict each other."""
        decidb_cli.assert_error("""
            SELECT l_quantity, x FROM lineitem WHERE l_orderkey < 10
            DECIDE x(INT)
            SUCH THAT x >= 10 AND x <= 5
            MAXIMIZE SUM(x * l_quantity)
        """, match=r"(?i)infeasible")

        data = duckdb_conn.execute(
            "SELECT CAST(l_quantity AS DOUBLE) FROM lineitem WHERE l_orderkey < 10"
        ).fetchall()
        oracle_solver.create_model("infeas_contradictory_bounds")
        for i in range(len(data)):
            # lb=10 conflicts with ub=5 — Gurobi flags this immediately.
            oracle_solver.add_variable(f"x_{i}", VarType.INTEGER, lb=10.0, ub=5.0)
        oracle_solver.set_objective(
            {f"x_{i}": data[i][0] for i in range(len(data))},
            ObjSense.MAXIMIZE,
        )
        assert oracle_solver.solve().status == SolverStatus.INFEASIBLE

    def test_impossible_sum_constraint(
        self, decidb_cli, duckdb_conn, oracle_solver
    ):
        """``SUM(x) >= 999999`` with BOOLEAN x caps at ``N <= 999999`` rows."""
        decidb_cli.assert_error("""
            SELECT l_quantity, x FROM lineitem WHERE l_orderkey = 1
            DECIDE x(BOOL)
            SUCH THAT SUM(x) >= 999999
            MAXIMIZE SUM(x * l_quantity)
        """, match=r"(?i)infeasible")

        data = duckdb_conn.execute(
            "SELECT CAST(l_quantity AS DOUBLE) FROM lineitem WHERE l_orderkey = 1"
        ).fetchall()
        oracle_solver.create_model("infeas_impossible_sum")
        vnames = [f"x_{i}" for i in range(len(data))]
        for v in vnames:
            oracle_solver.add_variable(v, VarType.BINARY)
        oracle_solver.add_constraint(
            {v: 1.0 for v in vnames}, ">=", 999999.0, name="impossible_lb",
        )
        oracle_solver.set_objective(
            {vnames[i]: data[i][0] for i in range(len(data))},
            ObjSense.MAXIMIZE,
        )
        assert oracle_solver.solve().status == SolverStatus.INFEASIBLE

    def test_negative_sum_upper_bound(
        self, decidb_cli, duckdb_conn, oracle_solver
    ):
        """Non-negative ``x`` cannot have a negative weighted SUM."""
        decidb_cli.assert_error("""
            SELECT l_quantity, x FROM lineitem WHERE l_orderkey < 5
            DECIDE x(BOOL)
            SUCH THAT SUM(x) >= 1 AND SUM(x * l_quantity) <= -1
            MAXIMIZE SUM(x)
        """, match=r"(?i)infeasible")

        data = duckdb_conn.execute(
            "SELECT CAST(l_quantity AS DOUBLE) FROM lineitem WHERE l_orderkey < 5"
        ).fetchall()
        oracle_solver.create_model("infeas_negative_ub")
        vnames = [f"x_{i}" for i in range(len(data))]
        for v in vnames:
            oracle_solver.add_variable(v, VarType.BINARY)
        oracle_solver.add_constraint(
            {v: 1.0 for v in vnames}, ">=", 1.0, name="sum_ge_1",
        )
        oracle_solver.add_constraint(
            {vnames[i]: data[i][0] for i in range(len(data))},
            "<=", -1.0, name="sum_qty_le_neg1",
        )
        oracle_solver.set_objective(
            {v: 1.0 for v in vnames}, ObjSense.MAXIMIZE,
        )
        assert oracle_solver.solve().status == SolverStatus.INFEASIBLE

    def test_infeasible_when_forces_all_zero(
        self, decidb_cli, duckdb_conn, oracle_solver
    ):
        """WHEN forces ``x=0`` for every qualifying row; aggregate still demands SUM(x)>=1."""
        decidb_cli.assert_error("""
            SELECT l_orderkey, l_quantity, l_returnflag, x
            FROM lineitem WHERE l_orderkey < 10
            DECIDE x(BOOL)
            SUCH THAT x <= 0 WHEN l_quantity > 0
                AND SUM(x) >= 1
            MAXIMIZE SUM(x * l_quantity)
        """, match=r"(?i)(infeasible|WHEN conditions)")

        data = duckdb_conn.execute("""
            SELECT CAST(l_quantity AS DOUBLE)
            FROM lineitem WHERE l_orderkey < 10
        """).fetchall()
        oracle_solver.create_model("infeas_when_forces_zero")
        vnames = [f"x_{i}" for i in range(len(data))]
        for i, v in enumerate(vnames):
            # WHEN l_quantity > 0 pins x to 0 via ub=0.0; unqualified rows
            # remain {0, 1}. In TPC-H lineitem qty is always positive so all
            # rows end up pinned, making SUM(x) >= 1 infeasible.
            ub = 0.0 if data[i][0] > 0 else 1.0
            oracle_solver.add_variable(v, VarType.BINARY, lb=0.0, ub=ub)
        oracle_solver.add_constraint(
            {v: 1.0 for v in vnames}, ">=", 1.0, name="sum_ge_1",
        )
        oracle_solver.set_objective(
            {vnames[i]: data[i][0] for i in range(len(data))},
            ObjSense.MAXIMIZE,
        )
        assert oracle_solver.solve().status == SolverStatus.INFEASIBLE

    def test_real_upper_below_nonnegativity_is_rejected(
        self, decidb_cli, oracle_solver
    ):
        """Bug 1 / Part C: a user upper bound below the non-negative REAL floor
        (`x <= -1`, default lower 0) is a type-domain conflict — loosening can never
        help, only the type can. It is rejected with a precise static error naming the
        domain, BEFORE the elastic engine, rather than reaching diagnosis."""
        decidb_cli.assert_error(
            "SELECT x FROM (VALUES (1)) t(id) "
            "DECIDE x(REAL) SUCH THAT x <= -1 MAXIMIZE SUM(x)",
            match=r"(?i)x <= -1 cannot hold.*non-negative",
        )
        oracle_solver.create_model("infeas_real_upper_below_zero")
        oracle_solver.add_variable("x", VarType.CONTINUOUS, lb=0.0, ub=-1.0)
        oracle_solver.set_objective({"x": 1.0}, ObjSense.MAXIMIZE)
        assert oracle_solver.solve().status == SolverStatus.INFEASIBLE

    def test_boolean_lower_above_domain_is_rejected(
        self, decidb_cli, oracle_solver
    ):
        """Bug 1 / Part C: a user lower bound above the BOOLEAN 0/1 ceiling (`x >= 2`)
        is a type-domain conflict, rejected with a precise static error naming the
        domain. The 0/1 box is intrinsic and never loosened."""
        decidb_cli.assert_error(
            "SELECT x FROM (VALUES (1)) t(id) "
            "DECIDE x(BOOL) SUCH THAT x >= 2 MAXIMIZE SUM(x)",
            match=r"(?i)x >= 2 cannot hold.*BOOLEAN",
        )
        oracle_solver.create_model("infeas_boolean_lower_above_one")
        oracle_solver.add_variable("x", VarType.BINARY, lb=0.0, ub=1.0)
        oracle_solver.add_constraint({"x": 1.0}, ">=", 2.0, name="x_ge_2")
        oracle_solver.set_objective({"x": 1.0}, ObjSense.MAXIMIZE)
        assert oracle_solver.solve().status == SolverStatus.INFEASIBLE

    def test_user_overridden_negative_lower_is_feasible(self, decidb_cli):
        """Part C guard: `x <= -1 AND x >= -5` explicitly lowers the floor below 0, so
        the box [-5, -1] is feasible and must NOT trip the non-negativity rejection —
        the maximum is -1."""
        rows, _ = decidb_cli.execute(
            "SELECT x FROM (VALUES (1)) t(id) "
            "DECIDE x(REAL) SUCH THAT x <= -1 AND x >= -5 MAXIMIZE SUM(x)"
        )
        assert rows and rows[0][0] == pytest.approx(-1.0)


@pytest.mark.error_infeasible
@pytest.mark.error
class TestUnboundedModels:
    """DecidB should detect unbounded models (as opposed to infeasible ones).
    The oracle cross-verifies by independently returning UNBOUNDED. Gurobi's
    ambiguous INF_OR_UNBD status is disambiguated on both sides via a
    DualReductions=0 re-solve, so exact statuses are asserted here."""

    def test_unbounded_integer_maximize(
        self, decidb_cli, duckdb_conn, oracle_solver
    ):
        """Integer x >= 1 with no upper bound — unbounded when maximising SUM(x)."""
        decidb_cli.assert_error("""
            SELECT l_orderkey, l_linenumber, x FROM lineitem WHERE l_orderkey <= 5
            DECIDE x(INT)
            SUCH THAT x >= 1
            MAXIMIZE SUM(x)
        """, match=r"(?i)unbounded")

        data = duckdb_conn.execute(
            "SELECT 1 FROM lineitem WHERE l_orderkey <= 5"
        ).fetchall()
        oracle_solver.create_model("unbounded_int_max")
        n = len(data)
        for i in range(n):
            oracle_solver.add_variable(f"x_{i}", VarType.INTEGER, lb=1.0)
        oracle_solver.set_objective(
            {f"x_{i}": 1.0 for i in range(n)}, ObjSense.MAXIMIZE,
        )
        assert oracle_solver.solve().status == SolverStatus.UNBOUNDED

    def test_unbounded_real_maximize(
        self, decidb_cli, duckdb_conn, oracle_solver
    ):
        """REAL x >= 0 with no upper bound — unbounded when maximising SUM(x)."""
        decidb_cli.assert_error("""
            SELECT l_orderkey, l_linenumber, x FROM lineitem WHERE l_orderkey <= 5
            DECIDE x(REAL)
            SUCH THAT x >= 0
            MAXIMIZE SUM(x)
        """, match=r"(?i)unbounded")

        data = duckdb_conn.execute(
            "SELECT 1 FROM lineitem WHERE l_orderkey <= 5"
        ).fetchall()
        oracle_solver.create_model("unbounded_real_max")
        n = len(data)
        for i in range(n):
            oracle_solver.add_variable(f"x_{i}", VarType.CONTINUOUS, lb=0.0)
        oracle_solver.set_objective(
            {f"x_{i}": 1.0 for i in range(n)}, ObjSense.MAXIMIZE,
        )
        assert oracle_solver.solve().status == SolverStatus.UNBOUNDED

    def test_mixed_unbounded_integer_var(
        self, decidb_cli, oracle_solver
    ):
        """Mixed problem where one variable is unconstrained.

        BOOLEAN ``x`` is bounded by ``SUM(x) <= 5``; INTEGER ``y`` appears
        in the objective but in no constraint. Maximising ``SUM(x*val + y)``
        drives ``y`` to infinity. A bounds-propagation bug that treats the
        model as bounded because *some* variable is constrained would miss
        this — hence the separate test from the lone-integer case.
        """
        decidb_cli.assert_error("""
            SELECT id, val, x, y FROM (
                VALUES (1, 10.0), (2, 20.0), (3, 30.0)
            ) t(id, val)
            DECIDE x(BOOL), y(INT)
            SUCH THAT SUM(x) <= 5
            MAXIMIZE SUM(x * val + y)
        """, match=r"(?i)unbounded")

        data = [(1, 10.0), (2, 20.0), (3, 30.0)]
        n = len(data)
        oracle_solver.create_model("mixed_unbounded_int")
        for i in range(n):
            oracle_solver.add_variable(f"x_{i}", VarType.BINARY)
            # INTEGER y with no upper bound mirrors DecidB's default.
            oracle_solver.add_variable(f"y_{i}", VarType.INTEGER, lb=0.0)
        oracle_solver.add_constraint(
            {f"x_{i}": 1.0 for i in range(n)}, "<=", 5.0, name="sum_x_cap",
        )
        obj: dict = {}
        for i in range(n):
            obj[f"x_{i}"] = data[i][1]
            obj[f"y_{i}"] = 1.0
        oracle_solver.set_objective(obj, ObjSense.MAXIMIZE)
        assert oracle_solver.solve().status == SolverStatus.UNBOUNDED

    def test_highs_milp_unbounded_reports_unbounded(self, decidb_cli_highs):
        """HiGHS can report ambiguous kUnboundedOrInfeasible for MILP-unbounded
        models. DeciDB maps that status and runs the zero-objective feasibility
        probe, so this user-facing path still reports an unbounded solve instead
        of leaking the raw backend status.
        """
        decidb_cli_highs.assert_error("""
            SELECT l_orderkey, l_linenumber, x FROM lineitem WHERE l_orderkey <= 5
            DECIDE x(INT)
            SUCH THAT x >= 1
            MAXIMIZE SUM(x)
        """, match=r"(?i)unbounded")
