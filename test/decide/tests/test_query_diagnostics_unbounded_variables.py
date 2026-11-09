"""Unbounded variable diagnostics: name each escaping variable.

Every solver column maps back to the user-facing thing it represents: user
variable name, or (for an auxiliary linearization column) the user's original
source expression. Folded into the unbounded diagnosis, the DIAGNOSE
relation reports each escaping variable by NAME with the DIRECTION it escapes
(+inf, or -inf for a variable whose floor the query removed).

These tests run `DIAGNOSE <query>`, which is the only thing that starts the engine,
reading the relation as CSV so single-letter variable names assert unambiguously. Run
under both backends.

Empirical scope note: in current DECIDE formulations only *user* INTEGER/REAL
variables can actually escape to infinity. Auxiliary variables are structurally
bounded — ABS Big-M and bilinear McCormick require finite bounds (they error
before the solver) and MIN/MAX/`<>` indicators are BOOLEAN [0,1]. So the e2e
escaping-variable naming exercises user vars; the aux→expression resolution is
covered by the C++ unit test (test_decidb_variable_provenance.cpp).
"""

import os
import tempfile

import pytest

from solver.types import ObjSense, SolverStatus, VarType

from . import _diagnose_relation


_BACKENDS = ["decidb_cli_highs", "decidb_cli_gurobi"]


_diagnose = _diagnose_relation.run


_rows = _diagnose_relation.eav_rows


def _attrs(rows, subject):
    return {r["attribute"]: r["value"] for r in rows if r["subject_kind"] == "variable" and r["subject"] == subject}


@pytest.mark.query_diagnostics
class TestUnboundedVariableDiagnostics:
    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_unbounded_real_var_named(self, request, cli_fixture):
        """A single escaping REAL user variable is named in the relation."""
        cli = request.getfixturevalue(cli_fixture)
        sql = "SELECT id, x FROM (VALUES (1), (2)) t(id) " "DECIDE x(REAL) SUCH THAT x >= 0 MAXIMIZE SUM(x)"
        result = _diagnose(cli, sql)

        # DIAGNOSE returns the finding; it does not raise.
        assert not result.stderr.strip(), result.stderr
        rows = _rows(result)
        assert _attrs(rows, "x")["grows_toward"] == "+inf"
        assert {r["state"] for r in rows} == {"unbounded"}
        # A4: it prescribes the forced remedy (add a bound) without inventing the cap.
        flat = _diagnose_relation.rows(result)
        assert [f["suggested_change"] for f in flat] == ["x <= <cap>"]

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_unbounded_integer_var_named(self, request, cli_fixture):
        """An INTEGER var is a MILP — on HiGHS this is the INF_OR_UNBD (status 9)
        path that the zero-objective probe disambiguates — and is still named
        correctly."""
        cli = request.getfixturevalue(cli_fixture)
        sql = "SELECT id, n FROM (VALUES (1), (2)) t(id) " "DECIDE n(INT) SUCH THAT n >= 0 MAXIMIZE SUM(n)"
        result = _diagnose(cli, sql)
        rows = _rows(result)
        assert _attrs(rows, "n")["grows_toward"] == "+inf"
        assert {r["state"] for r in rows} == {"unbounded"}

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_multiple_escaping_vars_each_named_once(self, request, cli_fixture):
        """Two escaping vars are each named once, in one diagnosis."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x, y FROM (VALUES (1), (2)) t(id) "
            "DECIDE x(REAL), y(REAL) SUCH THAT x >= 0 AND y >= 0 "
            "MAXIMIZE SUM(x + y)"
        )
        rows = _rows(_diagnose(cli, sql))
        assert _attrs(rows, "x")["grows_toward"] == "+inf"
        assert _attrs(rows, "y")["grows_toward"] == "+inf"
        # Every instance of each escapes, so the count covers the whole variable and no
        # categorical slice is named.
        assert _attrs(rows, "x")["escaping_instances"] == "2"
        assert _attrs(rows, "y")["escaping_instances"] == "2"
        assert {r["subject"] for r in rows if r["subject_kind"] == "variable" and r["attribute"] == "grows_toward"} == {
            "x",
            "y",
        }
        assert {r["state"] for r in rows} == {"unbounded"}

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_minimize_unbounded_var_named_from_improving_direction(self, request, cli_fixture):
        """A MINIMIZE objective that improves toward -inf still names x escaping +inf."""
        cli = request.getfixturevalue(cli_fixture)
        sql = "SELECT id, x FROM (VALUES (1), (2)) t(id) " "DECIDE x(REAL) SUCH THAT x >= 0 MINIMIZE SUM(x * -1)"
        result = _diagnose(cli, sql)
        rows = _rows(result)
        assert _attrs(rows, "x")["grows_toward"] == "+inf"

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_a_decision_falling_without_bound_is_named_with_a_floor(self, request, cli_fixture):
        """BUG-30: `buy >= -1e30` is no floor (a bound of 1e20 or more is none), so MINIMIZE
        drives every `buy` toward -inf. The ray search reads a column's box in both
        directions, so it names the variable falling, and the remedy is the floor it lacks."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, buy FROM (VALUES (1), (2)) t(id) "
            "DECIDE buy(REAL) SUCH THAT buy >= -1e30 AND buy <= 100 MINIMIZE SUM(buy)"
        )
        result = _diagnose(cli, sql)
        rows = _rows(result)
        assert _attrs(rows, "buy")["grows_toward"] == "-inf"
        assert _attrs(rows, "buy")["escaping_instances"] == "2"
        assert {r["state"] for r in rows} == {"unbounded"}
        flat = _diagnose_relation.rows(result)
        assert [f["suggested_change"] for f in flat] == ["buy >= <floor>"]

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_without_the_prefix_no_variable_is_named(self, request, cli_fixture):
        """The unprefixed query reports its state and stops: no variable, no remedy,
        and no second solve to work either out."""
        cli = request.getfixturevalue(cli_fixture)
        sql = "SELECT id, x FROM (VALUES (1), (2)) t(id) " "DECIDE x(REAL) SUCH THAT x >= 0 MAXIMIZE SUM(x)"
        result = cli.execute_script(f".mode csv\n{sql};\n")
        err = result.stderr.lower()
        assert "decide optimization is unbounded" in err
        assert "variable x" not in err
        # A1: it points the user at the prefix that WOULD name it.
        assert "diagnose" in err


_flat = _diagnose_relation.rows


def _dump_of(cli, sql):
    """The solver-neutral dump of the model the diagnosed query builds."""
    with tempfile.TemporaryDirectory() as tmp:
        return cli.dump_model(f"DIAGNOSE {sql}", os.path.join(tmp, "model.dump"))


def _named(flat):
    return [(f["state"], f["clause"], f["edit_source"]) for f in flat]


def _build(oracle_solver, name, variables, *, quadratic_rows=(), rows=(), indicators=(), objective):
    """State a maximization over `variables` = {name: (type, lb, ub)} independently of DeciDB; return its status."""
    oracle_solver.create_model(name)
    for var, (var_type, lb, ub) in variables.items():
        oracle_solver.add_variable(var, var_type, lb=lb, ub=ub)
    for i, (linear, quadratic, sense, rhs) in enumerate(quadratic_rows):
        oracle_solver.add_quadratic_constraint(linear, quadratic, sense, rhs, name=f"square_row_{i}")
    for i, (coeffs, sense, rhs) in enumerate(rows):
        oracle_solver.add_constraint(coeffs, sense, rhs, name=f"row_{i}")
    for i, (binary, value, coeffs, sense, rhs) in enumerate(indicators):
        oracle_solver.add_indicator_constraint(binary, value, coeffs, sense, rhs, name=f"indicator_{i}")
    oracle_solver.set_objective(objective, ObjSense.MAXIMIZE)
    return oracle_solver.solve().status


_REAL = VarType.CONTINUOUS
_FREE = float("-inf")


@pytest.mark.query_diagnostics
class TestUnboundedWithNonLinearParts:
    """UNB-1: every column of a non-linear part stays still along the ray, so a runaway that sits only in linear terms
    is named even when squares, native MIN/MAX or indicator constraints appear elsewhere. Gurobi only: HiGHS takes
    none of these constructs natively."""

    def test_a_runaway_outside_a_square_row_is_named(self, decidb_cli_gurobi, oracle_solver):
        """Branch: a quadratic row no longer declines the ray LP; its columns are held still, the other rows stay.

        The NULL-key row is in no `by (k)` ball, so its `x` has no limit. The oracle states the same model, finds it
        unbounded, and finds it bounded once that row's `x` has a cap."""
        free = {"x1": (_REAL, _FREE, None), "x2": (_REAL, _FREE, None), "x3": (_REAL, _FREE, None)}
        ball = [({}, {("x1", "x1"): 1.0, ("x2", "x2"): 1.0}, "<=", 25.0)]
        objective = {"x1": 1.0, "x2": 2.0, "x3": 3.0}
        # Gurobi's barrier cannot split "infeasible or unbounded" for a quadratic row. The capped model below is
        # feasible and only adds a cap, so the free model is feasible, which leaves unbounded.
        assert _build(oracle_solver, "ball_with_a_free_row", free, quadratic_rows=ball, objective=objective) in (
            SolverStatus.UNBOUNDED,
            SolverStatus.INF_OR_UNBD,
        )
        capped = {**free, "x3": (_REAL, _FREE, 10.0)}
        assert (
            _build(oracle_solver, "ball_with_a_capped_row", capped, quadratic_rows=ball, objective=objective)
            == SolverStatus.OPTIMAL
        )

        sql = (
            "SELECT k, s, x FROM (VALUES (1, 1.0), (1, 2.0), (NULL, 3.0)) t(k, s) "
            "DECIDE x(REAL) SUCH THAT SUM(POWER(x, 2)) BY (k) <= 25 MAXIMIZE SUM(s * x)"
        )
        flat = _flat(_diagnose(decidb_cli_gurobi, sql))
        assert _named(flat) == [("unbounded", "x", "runaway_+inf")]
        assert flat[0]["suggested_change"] == "when k IS NULL: x <= <cap>"
        assert (flat[0]["amount"], flat[0]["total"]) == ("1.0", "1")

    def test_the_linear_part_of_a_square_row_limits_the_ray(self, decidb_cli_gurobi, oracle_solver):
        """Branch: the linear part of a quadratic row joins the ray LP with a right-hand side of 0.

        `POWER(x, 2) + y <= 25` with `y >= 0` keeps `y` below 25, so only `z` runs away. Without the row in the ray
        LP, `y` would be named too. The oracle finds the model unbounded, and bounded once `z` has a cap."""
        variables = {
            "x": (_REAL, 0.0, None),
            "y": (_REAL, 0.0, None),
            "z": (_REAL, 0.0, None),
        }
        square_row = [({"y": 1.0}, {("x", "x"): 1.0}, "<=", 25.0)]
        objective = {"y": 1.0, "z": 1.0}
        assert (
            _build(oracle_solver, "square_row_free_z", variables, quadratic_rows=square_row, objective=objective)
            == SolverStatus.UNBOUNDED
        )
        capped = {**variables, "z": (_REAL, 0.0, 10.0)}
        assert (
            _build(oracle_solver, "square_row_capped_z", capped, quadratic_rows=square_row, objective=objective)
            == SolverStatus.OPTIMAL
        )

        sql = (
            "SELECT id, x, y, z FROM (VALUES (1), (2)) t(id) "
            "DECIDE x(REAL), y(REAL), z(REAL) SUCH THAT POWER(x, 2) + y <= 25 "
            "AND x >= 0 AND y >= 0 AND z >= 0 MAXIMIZE SUM(y + z)"
        )
        assert _named(_flat(_diagnose(decidb_cli_gurobi, sql))) == [("unbounded", "z", "runaway_+inf")]

    def test_a_runaway_inside_a_square_stays_undiagnosed(self, decidb_cli_gurobi, oracle_solver):
        """Branch: a column of the objective's quadratic part is held still, so a runaway that sits only there finds
        no ray and keeps the non-linear reason. The linear `+ x` would name it if the square did not hold it still.
        The oracle finds the model unbounded."""
        oracle_solver.create_model("open_square_objective")
        oracle_solver.add_variable("x", _REAL, lb=-10.0)
        oracle_solver.set_quadratic_objective({"x": 1.0}, {("x", "x"): 1.0}, ObjSense.MAXIMIZE)
        assert oracle_solver.solve().status == SolverStatus.UNBOUNDED

        sql = (
            "SELECT id, x FROM (VALUES (1), (2)) t(id) "
            "DECIDE x(REAL) SUCH THAT x >= -10 MAXIMIZE SUM(POWER(x, 2) + x)"
        )
        flat = _flat(_diagnose(decidb_cli_gurobi, sql))
        assert [(f["state"], f["edit_source"]) for f in flat] == [("unbounded", "undiagnosed")]
        assert flat[0]["suggested_change"] == "a non-linear term prevents naming the variable."

    def test_a_runaway_inside_a_quadratic_constraint_stays_undiagnosed(self, decidb_cli_gurobi, oracle_solver):
        """Branch: a column of a quadratic constraint is held still, so a runaway `x` there finds no ray and the model
        keeps the non-linear reason (it holds a quadratic constraint, not a quadratic objective). The oracle finds
        `x1^2 + x2^2 >= 1` with `MAXIMIZE x1 + x2` unbounded."""
        oracle_solver.create_model("open_square_constraint")
        oracle_solver.add_variable("x1", _REAL, lb=0.0)
        oracle_solver.add_variable("x2", _REAL, lb=0.0)
        oracle_solver.add_quadratic_constraint({}, {("x1", "x1"): 1.0, ("x2", "x2"): 1.0}, ">=", 1.0, name="outside")
        oracle_solver.set_objective({"x1": 1.0, "x2": 1.0}, ObjSense.MAXIMIZE)
        assert oracle_solver.solve().status == SolverStatus.UNBOUNDED

        sql = (
            "SELECT id, x FROM (VALUES (1), (2)) t(id) "
            "DECIDE x(REAL) SUCH THAT SUM(POWER(x, 2)) >= 1 MAXIMIZE SUM(x)"
        )
        flat = _flat(_diagnose(decidb_cli_gurobi, sql))
        assert [(f["state"], f["edit_source"]) for f in flat] == [("unbounded", "undiagnosed")]
        assert flat[0]["suggested_change"] == "a non-linear term prevents naming the variable."

    def test_a_runaway_outside_a_native_max_is_named(self, decidb_cli_gurobi, oracle_solver):
        """Branch: a general constraint's result and argument columns are held still.

        `MAX(x) >= 5` over an unbounded `x` has no Big-M, so Gurobi states it as a native MAX (the dump shows one),
        and the runaway `y`, which sits only in a linear term, is named. The oracle states the same condition as
        `x_i >= 5 * b_i` with `sum(b) >= 1` (some row reaches 5), finds the model unbounded, and bounded once `y`
        has a cap."""
        sql = (
            "SELECT id, x, y FROM (VALUES (1), (2)) t(id) "
            "DECIDE x(REAL), y(REAL) SUCH THAT x >= 0 AND y >= 0 AND MAX(x) >= 5 "
            "MINIMIZE SUM(x) - SUM(y)"
        )
        assert "num_genconstrs: 1" in _dump_of(decidb_cli_gurobi, sql)

        def status(name, y_cap):
            variables = {
                "x1": (_REAL, 0.0, None),
                "x2": (_REAL, 0.0, None),
                "y1": (_REAL, 0.0, y_cap),
                "y2": (_REAL, 0.0, y_cap),
                "b1": (VarType.BINARY, 0.0, 1.0),
                "b2": (VarType.BINARY, 0.0, 1.0),
            }
            rows = [
                ({"x1": 1.0, "b1": -5.0}, ">=", 0.0),
                ({"x2": 1.0, "b2": -5.0}, ">=", 0.0),
                ({"b1": 1.0, "b2": 1.0}, ">=", 1.0),
            ]
            objective = {"x1": -1.0, "x2": -1.0, "y1": 1.0, "y2": 1.0}
            return _build(oracle_solver, name, variables, rows=rows, objective=objective)

        assert status("native_max_free_y", None) == SolverStatus.UNBOUNDED
        assert status("native_max_capped_y", 10.0) == SolverStatus.OPTIMAL

        assert _named(_flat(_diagnose(decidb_cli_gurobi, sql))) == [("unbounded", "y", "runaway_+inf")]

    def test_a_runaway_inside_a_native_max_stays_undiagnosed(self, decidb_cli_gurobi):
        """Branch: the argument column of a native MAX is held still, so a runaway `x` there finds no ray and keeps
        the non-linear reason."""
        sql = (
            "SELECT id, x FROM (VALUES (1), (2)) t(id) "
            "DECIDE x(REAL) SUCH THAT x >= 0 AND MAX(x) >= 5 MAXIMIZE SUM(x)"
        )
        assert "num_genconstrs: 1" in _dump_of(decidb_cli_gurobi, sql)
        flat = _flat(_diagnose(decidb_cli_gurobi, sql))
        assert [(f["state"], f["edit_source"]) for f in flat] == [("unbounded", "undiagnosed")]
        assert flat[0]["suggested_change"] == "a non-linear term prevents naming the variable."

    def test_a_runaway_outside_an_indicator_constraint_is_named(self, decidb_cli_gurobi, oracle_solver):
        """Branch: an indicator constraint's binary and row columns are held still.

        Gurobi states `x <> 5` as indicator rows on a binary (the dump shows them), and the runaway `y`, which sits
        only in a linear term, is named. The oracle states the same model with indicator constraints, finds it
        unbounded, and bounded once `y` has a cap."""
        sql = (
            "SELECT id, x, y FROM (VALUES (1), (2)) t(id) "
            "DECIDE x(INT), y(REAL) SUCH THAT x <= 9 AND x >= 0 AND x <> 5 AND y >= 0 MAXIMIZE SUM(x + y)"
        )
        assert "num_indconstrs: 4" in _dump_of(decidb_cli_gurobi, sql)

        def status(name, y_cap):
            variables = {}
            indicators = []
            for i in (1, 2):
                variables[f"x{i}"] = (VarType.INTEGER, 0.0, 9.0)
                variables[f"y{i}"] = (_REAL, 0.0, y_cap)
                variables[f"z{i}"] = (VarType.BINARY, 0.0, 1.0)
                indicators.append((f"z{i}", 0, {f"x{i}": 1.0}, "<=", 4.0))
                indicators.append((f"z{i}", 1, {f"x{i}": 1.0}, ">=", 6.0))
            objective = {"x1": 1.0, "x2": 1.0, "y1": 1.0, "y2": 1.0}
            return _build(oracle_solver, name, variables, indicators=indicators, objective=objective)

        assert status("indicator_free_y", None) == SolverStatus.UNBOUNDED
        assert status("indicator_capped_y", 10.0) == SolverStatus.OPTIMAL

        assert _named(_flat(_diagnose(decidb_cli_gurobi, sql))) == [("unbounded", "y", "runaway_+inf")]

    def test_a_runaway_through_an_indicator_column_stays_undiagnosed(self, decidb_cli_gurobi, oracle_solver):
        """Branch: the columns of an indicator row stay still, so a ray that needs `x` (here `y <= x`, with `x`
        in the `x <> 5` indicator rows) finds none and keeps the non-linear reason. If `x` moved, the ray would
        name both. The oracle finds the model unbounded."""
        sql = (
            "SELECT id, x, y FROM (VALUES (1), (2)) t(id) "
            "DECIDE x(INT), y(REAL) SUCH THAT x >= 0 AND x <> 5 AND y >= 0 AND y <= x MAXIMIZE SUM(y)"
        )
        assert "num_indconstrs: 4" in _dump_of(decidb_cli_gurobi, sql)

        variables = {}
        indicators = []
        rows = []
        for i in (1, 2):
            variables[f"x{i}"] = (VarType.INTEGER, 0.0, None)
            variables[f"y{i}"] = (_REAL, 0.0, None)
            variables[f"z{i}"] = (VarType.BINARY, 0.0, 1.0)
            indicators.append((f"z{i}", 0, {f"x{i}": 1.0}, "<=", 4.0))
            indicators.append((f"z{i}", 1, {f"x{i}": 1.0}, ">=", 6.0))
            rows.append(({f"y{i}": 1.0, f"x{i}": -1.0}, "<=", 0.0))
        objective = {"y1": 1.0, "y2": 1.0}
        status = _build(
            oracle_solver, "indicator_with_y_under_x", variables, rows=rows, indicators=indicators, objective=objective
        )
        assert status == SolverStatus.UNBOUNDED

        flat = _flat(_diagnose(decidb_cli_gurobi, sql))
        assert [(f["state"], f["edit_source"]) for f in flat] == [("unbounded", "undiagnosed")]
        assert flat[0]["suggested_change"] == "a non-linear term prevents naming the variable."


# The NULL-key row is in no `by (k)` ball, so its `x` has no limit and the squares in the objective grow without bound.
_NONCONVEX_UNBOUNDED_SQL = (
    "SELECT k, s, x FROM (VALUES (1, 3.0), (1, 4.0), (NULL, 5.0)) t(k, s) "
    "DECIDE x(REAL) SUCH THAT SUM(POWER(x, 2)) BY (k) <= 25 AND x >= 0 MAXIMIZE SUM(POWER(x, 2))"
)


@pytest.mark.query_diagnostics
class TestNonConvexInfOrUnbd:
    """BUG-49: the Gurobi adapter does not retry an INF_OR_UNBD with dual reductions off. On a non-convex model that
    retry switched Gurobi to spatial branching, where it ended SUBOPTIMAL with an arbitrary incumbent and the query
    answered `feasible`. The facade's zero-objective probe settles the status instead. Gurobi only: HiGHS takes no
    non-convex model."""

    def _assert_model_is_unbounded_unless_capped(self, oracle_solver):
        """The oracle states the same model: it cannot call it solved as written, and solves it once the NULL row's
        `x` has a cap. Gurobi's nonconvex search cannot split "infeasible or unbounded" here, so either status is the
        same finding; the capped model is feasible, so the free one is not infeasible."""

        def status(name, x3_cap):
            variables = {
                "x1": (_REAL, 0.0, None),
                "x2": (_REAL, 0.0, None),
                "x3": (_REAL, 0.0, x3_cap),
            }
            ball = [({}, {("x1", "x1"): 1.0, ("x2", "x2"): 1.0}, "<=", 25.0)]
            oracle_solver.create_model(name)
            for var, (var_type, lb, ub) in variables.items():
                oracle_solver.add_variable(var, var_type, lb=lb, ub=ub)
            for i, (linear, quadratic, sense, rhs) in enumerate(ball):
                oracle_solver.add_quadratic_constraint(linear, quadratic, sense, rhs, name=f"ball_{i}")
            oracle_solver.set_quadratic_objective(
                {}, {("x1", "x1"): 1.0, ("x2", "x2"): 1.0, ("x3", "x3"): 1.0}, ObjSense.MAXIMIZE
            )
            return oracle_solver.solve().status

        assert status("nonconvex_free_row", None) in (SolverStatus.UNBOUNDED, SolverStatus.INF_OR_UNBD)
        assert status("nonconvex_capped_row", 10.0) == SolverStatus.OPTIMAL

    def test_diagnose_names_unbounded_when_the_retry_is_skipped(self, decidb_cli_gurobi, oracle_solver):
        """Branch: a non-convex model that reaches INF_OR_UNBD keeps that status past the adapter, the zero-objective
        probe finds it feasible, and DIAGNOSE answers `unbounded`, not `feasible`. A runaway inside a square has no
        ray, so the finding is `undiagnosed` with the non-linear reason."""
        self._assert_model_is_unbounded_unless_capped(oracle_solver)
        flat = _flat(_diagnose(decidb_cli_gurobi, _NONCONVEX_UNBOUNDED_SQL))
        assert [(f["state"], f["edit_source"]) for f in flat] == [("unbounded", "undiagnosed")]
        assert flat[0]["suggested_change"] == "a non-linear term prevents naming the variable."

    def test_without_the_prefix_the_query_raises_unbounded(self, decidb_cli_gurobi, oracle_solver):
        """Branch: the same query without DIAGNOSE raises the unbounded error and returns no rows, not an arbitrary
        incumbent with a caveat."""
        self._assert_model_is_unbounded_unless_capped(oracle_solver)
        result = decidb_cli_gurobi.execute_script(f".mode csv\n{_NONCONVEX_UNBOUNDED_SQL};\n")
        assert "decide optimization is unbounded" in result.stderr.lower()
        assert "k,s,x" not in result.stdout
