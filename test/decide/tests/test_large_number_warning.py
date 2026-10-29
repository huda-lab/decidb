"""One warning when the model handed to a solver holds a very large number.

A coefficient of 1e6 or more (1 / the integrality tolerance DeciDB sets on the solver: an integer slip times the coefficient
reaches 1), or a right-hand side or bound of about 4.5e9 or more (the feasibility tolerance / the machine epsilon: the
rounding of the number in a double reaches the tolerance), can lose precision in the solver's arithmetic. Coefficients are
constraint, quadratic, indicator and objective coefficients, including the constants DeciDB derives, such as a Big-M.
DeciDB says so once on stderr, solves the model as it is, and returns the answer the solver gives:

    very large number detected in model - numerical precision may make the solution unstable

The check reads the finished solver-neutral model, the one DECIDB_DUMP_MODEL dumps, so it knows nothing of where a
number came from. A query answered by a direct-solve rule builds no model and prints nothing. An open bound
(DECIDE_SOLVER_INFINITY, the default of a variable with no written bound) is not a number.

`DeciDBCli.execute(..., expect_large_numbers=...)` is how a test declares the warning: an undeclared line fails the
test, and so does a declared one that is not printed exactly once per query run. Every case below runs on both
backends through the forced-backend fixtures, which also keep a query on the solver path.

Covers (one test per branch of the check):
  - test_a_number_in_a_linear_position_warns_from_its_threshold: a constraint and an objective coefficient (999999,
    1000000, 2000000); a right-hand side (both signs), lower bound and upper bound (4503599627, 4503599627.370496,
    5000000000)
  - test_a_large_quadratic_objective_coefficient_warns: the Q matrix of the objective
  - test_a_large_quadratic_constraint_number_warns: a quadratic row's Q, linear part and right-hand side (Gurobi)
  - test_a_large_indicator_number_warns: a native `<>` row's coefficients and right-hand side (Gurobi)
  - test_a_big_m_derived_over_the_threshold_warns_where_it_exists: HiGHS lowers `<>` with a Big-M, Gurobi does not
  - test_a_number_just_under_each_threshold_is_silent: 999999 and 4503599627, everywhere a number sits
  - test_open_bounds_are_silent: the default bounds, and written bounds at the solver's infinity
  - test_many_large_numbers_print_one_line: one line per query, however many numbers qualify
  - test_diagnose_prints_once_when_the_solved_model_is_large: the repair model's re-solve does not repeat it
  - test_diagnose_prints_once_for_a_number_only_the_repair_model_holds: the repair model is checked too
  - test_a_direct_solved_query_prints_nothing: no model is built; the same query on the solver path warns
  - test_the_direct_key_declares_a_query_that_warns_only_on_a_solver: the harness key for such a query
  - test_a_row_is_satisfied_within_the_feasibility_tolerance, test_an_integer_column_is_integral_within_the_integrality_
    tolerance: the tolerances the thresholds derive from are the ones the backends run with
"""

import itertools

import pytest

from decidb_cli import DeciDBCliError
from solver.types import ObjSense, SolverStatus, VarType

from ._direct_oracle_common import _allow_direct_path  # noqa: F401  (autouse fixture)
from ._large_numbers import LARGE_COEFFICIENT, LARGE_TOTAL
from ._oracle_helpers import add_ne_indicator

_BACKENDS = ["decidb_cli_highs", "decidb_cli_gurobi"]

_TWO_ROWS = "(VALUES (1), (2)) t(id)"


def _cli(request, fixture):
    return request.getfixturevalue(fixture)


def _values(cli, sql, expect_large_numbers):
    """The decision `x` per row, in row order, after declaring whether the query warns."""
    rows, columns = cli.execute(sql, expect_large_numbers=expect_large_numbers)
    ordered = sorted(rows, key=lambda row: row[columns.index("id")])
    return [row[columns.index("x")] for row in ordered]


def _linear_oracle(oracle_solver, name, *, lower, upper, rows, objective, sense):
    """The same model as the SQL says, built independently: two REAL variables, linear rows and objective."""
    oracle_solver.create_model(name)
    for i in range(2):
        oracle_solver.add_variable(f"x_{i}", VarType.CONTINUOUS, lb=lower, ub=upper)
    for coefficient, comparison, rhs in rows:
        oracle_solver.add_constraint({f"x_{i}": coefficient for i in range(2)}, comparison, rhs)
    oracle_solver.set_objective({f"x_{i}": objective for i in range(2)}, sense)
    return oracle_solver.solve()


# position -> (SQL after the source with the number `{n}` in the position, the oracle's arguments for `n`). Each case holds
# `n` in the one position it names; every other number in it is far under both thresholds.
_COEFFICIENT_POSITIONS = {
    "constraint-coefficient": (
        "DECIDE x(REAL) SUCH THAT x >= 0 AND x <= 10 AND SUM({n} * x) <= 500000 MAXIMIZE SUM(x)",
        lambda n: dict(lower=0.0, upper=10.0, rows=[(n, "<=", 5e5)], objective=1.0, sense=ObjSense.MAXIMIZE),
    ),
    "objective-coefficient": (
        "DECIDE x(REAL) SUCH THAT x >= 0 AND x <= 1 AND SUM(x) <= 1.5 MAXIMIZE SUM({n} * x)",
        lambda n: dict(lower=0.0, upper=1.0, rows=[(1.0, "<=", 1.5)], objective=n, sense=ObjSense.MAXIMIZE),
    ),
}
_TOTAL_POSITIONS = {
    "right-hand-side": (
        "DECIDE x(REAL) SUCH THAT x >= 0 AND x <= 3000000000 AND SUM(x) <= {n} MAXIMIZE SUM(x)",
        lambda n: dict(lower=0.0, upper=3e9, rows=[(1.0, "<=", n)], objective=1.0, sense=ObjSense.MAXIMIZE),
    ),
    "negative-right-hand-side": (
        "DECIDE x(REAL) SUCH THAT x >= 0 AND x <= 5 AND SUM(x) >= -{n} MAXIMIZE SUM(x)",
        lambda n: dict(lower=0.0, upper=5.0, rows=[(1.0, ">=", -n)], objective=1.0, sense=ObjSense.MAXIMIZE),
    ),
    "lower-bound": (
        "DECIDE x(REAL) SUCH THAT x >= -{n} AND x <= 5 AND SUM(x) <= 100 MINIMIZE SUM(x)",
        lambda n: dict(lower=-n, upper=5.0, rows=[(1.0, "<=", 100.0)], objective=1.0, sense=ObjSense.MINIMIZE),
    ),
    "upper-bound": (
        "DECIDE x(REAL) SUCH THAT x >= 0 AND x <= {n} AND SUM(x) >= 100 MINIMIZE SUM(x)",
        lambda n: dict(lower=0.0, upper=n, rows=[(1.0, ">=", 100.0)], objective=1.0, sense=ObjSense.MINIMIZE),
    ),
}

# The numbers written into a position: just under the threshold, at it, and well over. A coefficient warns from
# DECIDE_LARGE_COEFFICIENT (1 / the integrality tolerance); a right-hand side or a bound from DECIDE_LARGE_TOTAL (the
# feasibility tolerance / the machine epsilon, 4503599627.370496). The numbers here are written out, not read from the
# C++ constants, so a change to either constant fails a test.
_COEFFICIENTS = ["999999", "1000000", "2000000"]
_TOTALS = ["4503599627", "4503599627.370496", "5000000000"]
_CASES = [
    (position, n, float(n) >= LARGE_COEFFICIENT) for position in _COEFFICIENT_POSITIONS for n in _COEFFICIENTS
] + [(position, n, float(n) >= LARGE_TOTAL) for position in _TOTAL_POSITIONS for n in _TOTALS]
_POSITIONS = {**_COEFFICIENT_POSITIONS, **_TOTAL_POSITIONS}


@pytest.mark.correctness
@pytest.mark.parametrize("cli_fixture", _BACKENDS)
@pytest.mark.parametrize("position,n,warns", _CASES, ids=[f"{position}-{n}" for position, n, _ in _CASES])
def test_a_number_in_a_linear_position_warns_from_its_threshold(
    request, oracle_solver, position, n, warns, cli_fixture
):
    decide, oracle_arguments = _POSITIONS[position]
    sql = f"SELECT id, x FROM {_TWO_ROWS} {decide.format(n=n)}"
    xs = _values(_cli(request, cli_fixture), sql, expect_large_numbers=warns)
    arguments = oracle_arguments(float(n))
    result = _linear_oracle(oracle_solver, f"number_{position}_{n}", **arguments)

    assert result.status == SolverStatus.OPTIMAL
    assert arguments["objective"] * sum(xs) == pytest.approx(result.objective_value, rel=1e-6, abs=1e-6)


@pytest.mark.correctness
@pytest.mark.parametrize("cli_fixture", _BACKENDS)
def test_a_large_quadratic_objective_coefficient_warns(request, oracle_solver, cli_fixture):
    """Only the Q matrix of the objective is large: 2000000 on x^2, with a linear part of 999999."""
    sql = f"""
        SELECT id, x FROM {_TWO_ROWS}
        DECIDE x(REAL) SUCH THAT x >= 0 AND x <= 1
        MINIMIZE SUM(2000000 * POWER(x, 2) - 999999 * x)
    """
    xs = _values(_cli(request, cli_fixture), sql, expect_large_numbers=True)

    oracle_solver.create_model("large_quadratic_objective")
    for i in range(2):
        oracle_solver.add_variable(f"x_{i}", VarType.CONTINUOUS, lb=0.0, ub=1.0)
    oracle_solver.set_quadratic_objective(
        {f"x_{i}": -999999.0 for i in range(2)}, {(f"x_{i}", f"x_{i}"): 2e6 for i in range(2)}, ObjSense.MINIMIZE
    )
    result = oracle_solver.solve()

    assert result.status == SolverStatus.OPTIMAL
    value = sum(2e6 * x * x - 999999.0 * x for x in xs)
    assert value == pytest.approx(result.objective_value, rel=1e-5)


# name -> (the clause that holds the large number, the oracle's linear part, Q coefficient and right-hand side).
_QUADRATIC_ROWS = {
    "q-coefficient": ("SUM(2000000 * POWER(x, 2)) <= 500000", 0.0, 2e6, 5e5),
    "linear-coefficient": ("SUM(POWER(x, 2) + 2000000 * x) <= 500000", 2e6, 1.0, 5e5),
    "right-hand-side": ("SUM(POWER(x, 2)) <= 5000000000", 0.0, 1.0, 5e9),
}


@pytest.mark.correctness
@pytest.mark.quadratic
@pytest.mark.parametrize("row", sorted(_QUADRATIC_ROWS))
def test_a_large_quadratic_constraint_number_warns(decidb_cli_gurobi, oracle_solver, row):
    # The oracle result is cached under this function's source, not under the table: edit the source when the table changes.
    clause, linear, quadratic, rhs = _QUADRATIC_ROWS[row]
    sql = f"""
        SELECT id, x FROM {_TWO_ROWS}
        DECIDE x(REAL) SUCH THAT x >= 0 AND x <= 10 AND {clause}
        MAXIMIZE SUM(x)
    """
    xs = _values(decidb_cli_gurobi, sql, expect_large_numbers=True)

    oracle_solver.create_model(f"large_quadratic_row_{row}")
    for i in range(2):
        oracle_solver.add_variable(f"x_{i}", VarType.CONTINUOUS, lb=0.0, ub=10.0)
    oracle_solver.add_quadratic_constraint(
        {f"x_{i}": linear for i in range(2)}, {(f"x_{i}", f"x_{i}"): quadratic for i in range(2)}, "<=", rhs
    )
    oracle_solver.set_objective({f"x_{i}": 1.0 for i in range(2)}, ObjSense.MAXIMIZE)
    result = oracle_solver.solve()

    assert result.status == SolverStatus.OPTIMAL
    assert sum(xs) == pytest.approx(result.objective_value, rel=1e-4, abs=1e-6)


# name -> (the clause, the coefficient of each x in the oracle's row, the value the sum must differ from, the sense).
_INDICATOR_ROWS = {
    "coefficient": ("SUM(2000000 * x) <> 0", 2e6, 0.0, "MINIMIZE", ObjSense.MINIMIZE),
    "right-hand-side": ("SUM(x) <> 5000000000", 1.0, 5e9, "MAXIMIZE", ObjSense.MAXIMIZE),
}


@pytest.mark.correctness
@pytest.mark.parametrize("row", sorted(_INDICATOR_ROWS))
def test_a_large_indicator_number_warns(decidb_cli_gurobi, oracle_solver, row):
    """Gurobi states `<>` as indicator rows, so the number is in a row that carries its own coefficients."""
    # The oracle result is cached under this function's source, not under the table: edit the source when the table changes.
    clause, coefficient, differs_from, sense, oracle_sense = _INDICATOR_ROWS[row]
    sql = f"""
        SELECT id, x FROM {_TWO_ROWS}
        DECIDE x(INT) SUCH THAT x >= 0 AND x <= 900000 AND {clause}
        {sense} SUM(x)
    """
    xs = _values(decidb_cli_gurobi, sql, expect_large_numbers=True)

    oracle_solver.create_model(f"large_indicator_{row}")
    for i in range(2):
        oracle_solver.add_variable(f"x_{i}", VarType.INTEGER, lb=0.0, ub=900000.0)
    add_ne_indicator(oracle_solver, {f"x_{i}": coefficient for i in range(2)}, differs_from, name="ne")
    oracle_solver.set_objective({f"x_{i}": 1.0 for i in range(2)}, oracle_sense)
    result = oracle_solver.solve()

    assert result.status == SolverStatus.OPTIMAL
    assert sum(xs) == pytest.approx(result.objective_value, abs=0.5)


@pytest.mark.correctness
@pytest.mark.parametrize("cli_fixture", _BACKENDS)
def test_a_big_m_derived_over_the_threshold_warns_where_it_exists(request, oracle_solver, cli_fixture):
    """Every number the user wrote is under its threshold. HiGHS lowers `<>` with a Big-M of the sum's span (1.8e6), a number
    DeciDB derived. Gurobi states `<>` natively, so its model holds only the user's numbers."""
    sql = f"""
        SELECT id, x FROM {_TWO_ROWS}
        DECIDE x(INT) SUCH THAT x >= 0 AND x <= 900000 AND SUM(x) <> 7
        MAXIMIZE SUM(x)
    """
    xs = _values(_cli(request, cli_fixture), sql, expect_large_numbers={"gurobi": False, "highs": True})

    oracle_solver.create_model("derived_big_m")
    for i in range(2):
        oracle_solver.add_variable(f"x_{i}", VarType.INTEGER, lb=0.0, ub=900000.0)
    add_ne_indicator(oracle_solver, {f"x_{i}": 1.0 for i in range(2)}, 7.0, name="ne")
    oracle_solver.set_objective({f"x_{i}": 1.0 for i in range(2)}, ObjSense.MAXIMIZE)
    result = oracle_solver.solve()

    assert result.status == SolverStatus.OPTIMAL
    assert sum(xs) == pytest.approx(result.objective_value, abs=0.5)


@pytest.mark.correctness
@pytest.mark.parametrize("cli_fixture", _BACKENDS)
def test_a_number_just_under_each_threshold_is_silent(request, oracle_solver, cli_fixture):
    """A coefficient of 999999 in the rows and the objective; a bound of 4503599627 on each side, and a right-hand side of
    4503599627. The thresholds are 1000000 and 4503599627.370496."""
    sql = f"""
        SELECT id, x FROM {_TWO_ROWS}
        DECIDE x(REAL) SUCH THAT x >= -4503599627 AND x <= 4503599627 AND SUM(999999 * x) <= 4503599627
        MAXIMIZE SUM(999999 * x)
    """
    xs = _values(_cli(request, cli_fixture), sql, expect_large_numbers=False)
    result = _linear_oracle(
        oracle_solver,
        "just_under_the_thresholds",
        lower=-4503599627.0,
        upper=4503599627.0,
        rows=[(999999.0, "<=", 4503599627.0)],
        objective=999999.0,
        sense=ObjSense.MAXIMIZE,
    )

    assert result.status == SolverStatus.OPTIMAL
    assert 999999.0 * sum(xs) == pytest.approx(result.objective_value, rel=1e-6)


# name -> (the bounds written, the oracle's lower bound). Nothing else in a case is large, and the row is a floor so that
# it leaves the upper bound open.
_OPEN_BOUNDS = {
    "default": ("x >= 0", 0.0),
    "written-at-the-solvers-infinity": ("x >= -1e30 AND x <= 1e30", float("-inf")),
}


@pytest.mark.correctness
@pytest.mark.parametrize("cli_fixture", _BACKENDS)
@pytest.mark.parametrize("name", sorted(_OPEN_BOUNDS))
def test_open_bounds_are_silent(request, oracle_solver, name, cli_fixture):
    """A variable with no written upper bound has the solver's infinity above it, and a lower bound written at
    -1e30 or an upper at 1e30 is the same infinity. Infinity is no number."""
    bounds, lower = _OPEN_BOUNDS[name]
    sql = f"SELECT id, x FROM {_TWO_ROWS} DECIDE x(REAL) SUCH THAT {bounds} AND SUM(x) >= 1 MINIMIZE SUM(x)"
    xs = _values(_cli(request, cli_fixture), sql, expect_large_numbers=False)
    result = _linear_oracle(
        oracle_solver,
        f"open_bounds_{name}",
        lower=lower,
        upper=None,
        rows=[(1.0, ">=", 1.0)],
        objective=1.0,
        sense=ObjSense.MINIMIZE,
    )

    assert result.status == SolverStatus.OPTIMAL
    assert sum(xs) == pytest.approx(result.objective_value, rel=1e-6)


@pytest.mark.parametrize("cli_fixture", _BACKENDS)
def test_many_large_numbers_print_one_line(request, cli_fixture):
    """A large coefficient, right-hand side, bound and objective coefficient in one model: still one line."""
    sql = f"""
        SELECT id, x FROM {_TWO_ROWS}
        DECIDE x(REAL) SUCH THAT x >= -5000000000 AND x <= 6000000000 AND SUM(2000000 * x) <= 7000000000
        MAXIMIZE SUM(4000000 * x)
    """
    _values(_cli(request, cli_fixture), sql, expect_large_numbers=True)


@pytest.mark.query_diagnostics
@pytest.mark.parametrize("cli_fixture", _BACKENDS)
def test_diagnose_prints_once_when_the_solved_model_is_large(request, cli_fixture):
    """The solved model and the repair model both hold 6000000000; the line is printed for the first."""
    cli = _cli(request, cli_fixture)
    rows, columns = cli.execute(
        f"""
        DIAGNOSE SELECT id, x FROM {_TWO_ROWS}
        DECIDE x(REAL) SUCH THAT x >= 0 AND x <= 5 AND SUM(x) >= 6000000000
        MAXIMIZE SUM(x)
        """,
        expect_large_numbers=True,
    )

    assert {row[columns.index("state")] for row in rows} == {"infeasible"}


@pytest.mark.query_diagnostics
@pytest.mark.parametrize("cli_fixture", _BACKENDS)
def test_diagnose_prints_once_for_a_number_only_the_repair_model_holds(request, cli_fixture):
    """`10000000 * x <= 50000000` is a bound on one decision, so the solved model holds `x <= 5` and no large number;
    the repair model keeps the row, and its coefficient is the large number."""
    cli = _cli(request, cli_fixture)
    sql = f"""
        SELECT id, x FROM (VALUES (1, 5)) t(id, lo)
        DECIDE x(INT) SUCH THAT x <> 5 AND x >= lo AND 10000000 * x <= 50000000
        MINIMIZE SUM(x)
    """
    cli.assert_error(sql, match="(?i)infeasible", expect_large_numbers=False)
    rows, columns = cli.execute("DIAGNOSE " + sql, expect_large_numbers=True)

    assert {row[columns.index("state")] for row in rows} == {"infeasible"}


@pytest.mark.parametrize("cli_fixture", _BACKENDS)
def test_a_direct_solved_query_prints_nothing(request, decidb_cli, cli_fixture):
    """The A1 rule answers `x <= 5000000000` with no solver and no model, so there is no number to name. The same
    query on the solver path (`off`, which a forced backend takes anyway) holds the bound and warns."""
    sql = f"SELECT id, x FROM {_TWO_ROWS} DECIDE x(INT) SUCH THAT x >= 0 AND x <= 5000000000 MAXIMIZE SUM(x)"
    direct, _ = decidb_cli.execute(f"SET decide_direct_solve='require'; {sql}", expect_large_numbers=False)
    solver = _values(_cli(request, cli_fixture), sql, expect_large_numbers=True)

    assert sorted(row[1] for row in direct) == solver == [5000000000, 5000000000]


def test_the_direct_key_declares_a_query_that_warns_only_on_a_solver(decidb_cli, decidb_cli_highs):
    """`{"direct": False, "gurobi": True, "highs": True}` holds in every run. The A1 rule answers this query with no
    model when the run leaves direct solve on (silent), and the solver path warns when `DECIDB_TEST_DIRECT_SOLVE=off`
    sends it there (the backend's key) or when a backend is forced (the forced wrapper's key)."""
    sql = f"SELECT id, x FROM {_TWO_ROWS} DECIDE x(INT) SUCH THAT x >= 0 AND x <= 5000000000 MAXIMIZE SUM(x)"
    declared = {"direct": False, "gurobi": True, "highs": True}
    for cli in (decidb_cli, decidb_cli_highs):
        rows, _ = cli.execute(sql, expect_large_numbers=declared)
        assert sorted(row[1] for row in rows) == [5000000000, 5000000000]


# The tolerances DeciDB sets on both backends (DECIDE_FEASIBILITY_TOLERANCE, DECIDE_INTEGRALITY_TOLERANCE, both 1e-6)
# are the ones the warning's thresholds are derived from. The two tests below fail if a backend stops setting them:
# HiGHS' own `primal_feasibility_tolerance` is 1e-7, and Gurobi's `IntFeasTol` is 1e-5.
@pytest.mark.parametrize("cli_fixture", _BACKENDS)
def test_a_row_is_satisfied_within_the_feasibility_tolerance(request, cli_fixture):
    """`SUM(x) >= 1.5000005` over three columns capped at 0.5 misses by 5e-7, inside 1e-6, so both backends answer
    (HiGHS at its own 1e-7 says infeasible). A miss of 1.5e-6 is outside it."""
    cli = _cli(request, cli_fixture)
    query = "SELECT id, x FROM (VALUES (1), (2), (3)) t(id) DECIDE x(REAL) SUCH THAT x >= 0 AND x <= 0.5 AND SUM(x) >= {n} MINIMIZE SUM(x)"
    xs = _values(cli, query.format(n="1.5000005"), expect_large_numbers=False)
    assert sum(xs) == pytest.approx(1.5000005, abs=1e-9)
    assert max(xs) - 0.5 <= 1e-6
    cli.assert_error(query.format(n="1.5000015"), match="(?i)infeasible", expect_large_numbers=False)


@pytest.mark.correctness
@pytest.mark.parametrize("cli_fixture", _BACKENDS)
def test_an_integer_column_is_integral_within_the_integrality_tolerance(request, cli_fixture):
    """The first row misses the integer point (1, 1, 1) by 4e-6. Gurobi at its own `IntFeasTol` of 1e-5 takes the
    near-integral relaxation vertex for it and answers (1, 1, 1), which breaks the row; at 1e-6 it branches. The best
    whole-number point is found by enumeration, with no solver."""
    rows = [
        ([0.59, 1.105, 0.202], 1.896996),
        ([1.127, 1.649, 1.204], 3.979996),
        ([0.332, 0.299, 0.933], 1.563998),
    ]
    clauses = " AND ".join(
        "SUM(" + " + ".join(f"{a}*v{i}" for i, a in enumerate(coefficients)) + f") <= {limit:.6f}"
        for coefficients, limit in rows
    )
    sql = (
        "SELECT v0, v1, v2 FROM (VALUES (1)) t(id) DECIDE v0(INT), v1(INT), v2(INT) SUCH THAT "
        f"v0 >= 0 AND v0 <= 6 AND v1 >= 0 AND v1 <= 6 AND v2 >= 0 AND v2 <= 6 AND {clauses} "
        "MAXIMIZE SUM(1*v0 + 2*v1 + 2*v2)"
    )
    best = max(
        (
            point
            for point in itertools.product(range(7), repeat=3)
            if all(sum(a * v for a, v in zip(coefficients, point)) <= limit + 1e-9 for coefficients, limit in rows)
        ),
        key=lambda point: point[0] + 2 * point[1] + 2 * point[2],
    )
    result, _ = _cli(request, cli_fixture).execute(sql, expect_large_numbers=False)

    assert result == [best]
