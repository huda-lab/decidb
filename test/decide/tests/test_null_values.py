"""A NULL data value is skipped where DECIDE reads it (B1, deck pp. 50 and 56).

A reducer skips a row whose summand is NULL, as SQL's aggregates do, and AVG divides by the rows it kept. A NULL that
reaches a constraint unreduced (a bound, a coefficient outside a reducer, a factor on an aggregate) skips that
instance, and a NULL factor on an objective term drops the term. Each test reaches one place the solver path reads a
value: a coefficient on an aggregate, a product of two decisions, a square, a factor on a squared sum, a factor on a
term of the objective (linear and bilinear), AVG, and a factor on a composed MIN/MAX. The rule-specific readings are in
the direct-solve rule files; the scope readings in `_scope_oracle`.

Every test builds the model the rule defines in Python, solves it with the oracle, and checks DeciDB's answer is
feasible for it and reaches its optimum.
"""

import pytest

from solver.types import ObjSense, VarType

# id, c (NULL on row 2), p
ROWS = ((1, 2.0, 5.0), (2, None, 4.0), (3, 1.0, 3.0))
SOURCE = "FROM (VALUES (1, 2.0, 5.0), (2, NULL::DOUBLE, 4.0), (3, 1.0, 3.0)) t(id, c, p)"
KEPT = [(i, c, p) for i, c, p in ROWS if c is not None]
TOLERANCE = 1e-6


def _answer(cli, select, decide):
    rows, columns = cli.execute(f"SELECT id, {select} FROM ({SOURCE} {decide}) q ORDER BY id")
    assert [row[0] for row in rows] == [row[0] for row in ROWS]
    return {row[0]: dict(zip(columns[1:], row[1:])) for row in rows}


def _optimum(oracle_solver, name, variables, build):
    oracle_solver.create_model(name)
    for var, kind, lower, upper in variables:
        oracle_solver.add_variable(var, kind, lower, upper)
    build(oracle_solver)
    return oracle_solver.solve().objective_value


@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_a_null_factor_on_an_aggregate_skips_that_rows_instance(decidb_cli, oracle_solver):
    """`c * SUM(x) <= 4` is one constraint per row; row 2's is NULL and skipped."""
    answer = _answer(decidb_cli, "x", "DECIDE x(INT) SUCH THAT x <= 3 AND c * SUM(x) <= 4 MAXIMIZE SUM(p*x)")
    xs = {i: f"x_{i}" for i, _, _ in ROWS}

    def build(oracle):
        for _, c, _ in KEPT:
            oracle.add_constraint({x: c for x in xs.values()}, "<=", 4.0)
        oracle.set_objective({xs[i]: p for i, _, p in ROWS}, ObjSense.MAXIMIZE)

    optimum = _optimum(
        oracle_solver, "null_factor_on_aggregate", [(x, VarType.INTEGER, 0, 3) for x in xs.values()], build
    )
    total = sum(answer[i]["x"] for i in xs)
    assert all(c * total <= 4.0 + TOLERANCE for _, c, _ in KEPT)
    assert sum(p * answer[i]["x"] for i, _, p in ROWS) == pytest.approx(optimum)


@pytest.mark.cons_perrow
@pytest.mark.correctness
def test_a_null_coefficient_of_a_product_of_decisions_skips_its_instance(decidb_cli, oracle_solver):
    """`c * x * y <= 0.5` on row 2 is NULL and skipped, so row 2 takes both."""
    answer = _answer(decidb_cli, "x, y", "DECIDE x(BOOL), y(BOOL) SUCH THAT c * x * y <= 0.5 MAXIMIZE SUM(p*x + p*y)")
    variables = [(f"{d}_{i}", VarType.BINARY, 0, 1) for i, _, _ in ROWS for d in "xy"]

    def build(oracle):
        for i, c, _ in KEPT:
            oracle.add_quadratic_constraint({}, {(f"x_{i}", f"y_{i}"): c}, "<=", 0.5)
        oracle.set_objective({f"{d}_{i}": p for i, _, p in ROWS for d in "xy"}, ObjSense.MAXIMIZE)

    optimum = _optimum(oracle_solver, "null_bilinear_coefficient", variables, build)
    assert all(c * answer[i]["x"] * answer[i]["y"] <= 0.5 for i, c, _ in KEPT)
    assert sum(p * (answer[i]["x"] + answer[i]["y"]) for i, _, p in ROWS) == pytest.approx(optimum)


@pytest.mark.cons_perrow
@pytest.mark.correctness
def test_a_null_coefficient_in_a_square_skips_its_instance(decidb_cli_gurobi, oracle_solver):
    """`POWER(c*x, 2) <= 4` on row 2 is NULL and skipped, so row 2 takes its cap. A quadratic row needs Gurobi."""
    answer = _answer(decidb_cli_gurobi, "x", "DECIDE x(REAL) SUCH THAT x <= 5 AND POWER(c*x, 2) <= 4 MAXIMIZE SUM(p*x)")

    def build(oracle):
        for i, c, _ in KEPT:
            oracle.add_quadratic_constraint({}, {(f"x_{i}", f"x_{i}"): c * c}, "<=", 4.0)
        oracle.set_objective({f"x_{i}": p for i, _, p in ROWS}, ObjSense.MAXIMIZE)

    optimum = _optimum(
        oracle_solver, "null_square_coefficient", [(f"x_{i}", VarType.CONTINUOUS, 0, 5) for i, _, _ in ROWS], build
    )
    assert all((c * answer[i]["x"]) ** 2 <= 4.0 + TOLERANCE for i, c, _ in KEPT)
    assert sum(p * answer[i]["x"] for i, _, p in ROWS) == pytest.approx(optimum, rel=1e-5)


@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_a_null_factor_on_a_squared_sum_skips_the_clause(decidb_cli_gurobi, oracle_solver):
    """The clause's one instance is NULL, so only the boxes hold. A quadratic row needs Gurobi."""
    answer = _answer(
        decidb_cli_gurobi,
        "x",
        "DECIDE x(REAL) SUCH THAT x <= 5 AND (SELECT NULL::DOUBLE) * SUM(POWER(x, 2)) <= 4 MAXIMIZE SUM(p*x)",
    )

    def build(oracle):
        oracle.set_objective({f"x_{i}": p for i, _, p in ROWS}, ObjSense.MAXIMIZE)

    optimum = _optimum(
        oracle_solver, "null_squared_sum_factor", [(f"x_{i}", VarType.CONTINUOUS, 0, 5) for i, _, _ in ROWS], build
    )
    assert sum(p * answer[i]["x"] for i, _, p in ROWS) == pytest.approx(optimum)


@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_avg_divides_by_the_rows_it_keeps(decidb_cli, oracle_solver):
    """Row 2's `c*x` is NULL, so AVG reads rows 1 and 3 and divides by 2."""
    answer = _answer(decidb_cli, "x", "DECIDE x(REAL) SUCH THAT x <= 5 AND AVG(c*x) <= 1.5 MAXIMIZE SUM(p*x)")

    def build(oracle):
        oracle.add_constraint({f"x_{i}": c / len(KEPT) for i, c, _ in KEPT}, "<=", 1.5)
        oracle.set_objective({f"x_{i}": p for i, _, p in ROWS}, ObjSense.MAXIMIZE)

    optimum = _optimum(
        oracle_solver, "null_avg_denominator", [(f"x_{i}", VarType.CONTINUOUS, 0, 5) for i, _, _ in ROWS], build
    )
    assert sum(c * answer[i]["x"] for i, c, _ in KEPT) / len(KEPT) <= 1.5 + TOLERANCE
    assert sum(p * answer[i]["x"] for i, _, p in ROWS) == pytest.approx(optimum)


@pytest.mark.obj_maximize
@pytest.mark.correctness
@pytest.mark.parametrize(
    "term",
    [
        pytest.param("(SELECT NULL::DOUBLE) * SUM(p*x)", id="linear"),
        pytest.param("(SELECT NULL::DOUBLE) * SUM(p*x*y)", id="bilinear"),
    ],
)
def test_a_null_factor_on_an_objective_term_drops_the_term(decidb_cli, oracle_solver, term):
    """The NULL term is dropped, and the other term is what the query maximizes."""
    answer = _answer(
        decidb_cli,
        "x, y",
        f"DECIDE x(BOOL), y(BOOL) SUCH THAT SUM(x) <= 1 AND SUM(y) <= 1 MAXIMIZE {term} + SUM(p*x)",
    )
    variables = [(f"{d}_{i}", VarType.BINARY, 0, 1) for i, _, _ in ROWS for d in "xy"]

    def build(oracle):
        for d in "xy":
            oracle.add_constraint({f"{d}_{i}": 1.0 for i, _, _ in ROWS}, "<=", 1.0)
        oracle.set_objective({f"x_{i}": p for i, _, p in ROWS}, ObjSense.MAXIMIZE)

    optimum = _optimum(oracle_solver, f"null_objective_factor", variables, build)
    assert sum(answer[i]["x"] for i, _, _ in ROWS) <= 1 and sum(answer[i]["y"] for i, _, _ in ROWS) <= 1
    assert sum(p * answer[i]["x"] for i, _, p in ROWS) == pytest.approx(optimum)


@pytest.mark.min_max
@pytest.mark.correctness
def test_a_null_factor_on_a_composed_extremum_skips_the_clause(decidb_cli, oracle_solver):
    """`NULL * MAX(x) + MIN(x) <= 3` is NULL, so only the boxes hold."""
    answer = _answer(
        decidb_cli,
        "x",
        "DECIDE x(INT) SUCH THAT x <= 5 AND (SELECT NULL::DOUBLE) * MAX(x) + MIN(x) <= 3 MAXIMIZE SUM(p*x)",
    )

    def build(oracle):
        oracle.set_objective({f"x_{i}": p for i, _, p in ROWS}, ObjSense.MAXIMIZE)

    optimum = _optimum(
        oracle_solver, "null_composed_factor", [(f"x_{i}", VarType.INTEGER, 0, 5) for i, _, _ in ROWS], build
    )
    assert sum(p * answer[i]["x"] for i, _, p in ROWS) == pytest.approx(optimum)


@pytest.mark.min_max
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_a_null_factor_on_a_composed_objective_term_drops_the_term(decidb_cli, oracle_solver):
    """`MINIMIZE NULL * MAX(x) - SUM(x)` minimizes `-SUM(x)` alone."""
    answer = _answer(decidb_cli, "x", "DECIDE x(INT) SUCH THAT x <= 5 MINIMIZE (SELECT NULL::DOUBLE) * MAX(x) - SUM(x)")

    def build(oracle):
        oracle.set_objective({f"x_{i}": -1.0 for i, _, _ in ROWS}, ObjSense.MINIMIZE)

    optimum = _optimum(
        oracle_solver, "null_composed_objective", [(f"x_{i}", VarType.INTEGER, 0, 5) for i, _, _ in ROWS], build
    )
    assert -sum(answer[i]["x"] for i, _, _ in ROWS) == pytest.approx(optimum)


@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_a_null_factor_on_an_unreduced_objective_term_drops_only_that_term(decidb_cli, oracle_solver):
    """`NULL*u + 2*u` keeps `2*u`: the NULL term is dropped before like terms merge."""
    rows, _ = decidb_cli.execute(
        f"SELECT id, u FROM ({SOURCE} DECIDE per (): u(INT) SUCH THAT u <= 3 "
        "MAXIMIZE (SELECT NULL::DOUBLE)*u + 2*u) q ORDER BY id"
    )

    def build(oracle):
        oracle.set_objective({"u": 2.0}, ObjSense.MAXIMIZE)

    optimum = _optimum(oracle_solver, "null_unreduced_objective", [("u", VarType.INTEGER, 0, 3)], build)
    assert len({row[1] for row in rows}) == 1
    assert 2.0 * rows[0][1] == pytest.approx(optimum)
