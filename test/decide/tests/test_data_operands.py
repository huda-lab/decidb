"""An operand that reads no decision is data, whatever its shape.

Below a clause's top, DuckDB binds a decision-free operand by its own rules: a
comparison read as 0/1 (``CAST(cap > 2 AS INT)``), a lambda and its parameters, a
prepared statement's ``$1``, a table macro's parameter. The DECIDE binders used to
refuse every shape they did not list, so each of these failed: a lambda with an
internal error, a comparison as *"Column 'cap' must be one of the DECIDE variables"*,
a parameter as *"SUCH THAT does not support '$1'"*, and a macro parameter as
*"Referenced column not found"*, because macro substitution did not visit DECIDE's
clauses.

Each test solves the same knapsack with the operand written out and with its value
computed in Python, then checks DeciDB's objective against the oracle's.
"""

import pytest

from solver.types import VarType, ObjSense, SolverStatus


_ROWS = [(1, 3, 1), (2, 5, 2), (3, 7, 3)]  # (id, cap, b)
_TABLE = "(VALUES (1, 3, 1), (2, 5, 2), (3, 7, 3)) t(id, cap, b)"


def _oracle(oracle_solver, name, ub, coeffs, budget, weights):
    """max sum(weights * x) s.t. 0 <= x_i <= ub_i integer, sum(coeffs * x) <= budget."""
    oracle_solver.create_model(name)
    names = [f"x_{i}" for i in range(len(_ROWS))]
    for n, u in zip(names, ub):
        oracle_solver.add_variable(n, VarType.INTEGER, lb=0.0, ub=float(u))
    if coeffs is not None:
        oracle_solver.add_constraint({n: float(c) for n, c in zip(names, coeffs)}, "<=", float(budget), name="budget")
    oracle_solver.set_objective({n: float(w) for n, w in zip(names, weights)}, ObjSense.MAXIMIZE)
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL
    return result.objective_value


def _objective(rows, cols, weights):
    ci = {c: i for i, c in enumerate(cols)}
    by_id = {int(r[ci["id"]]): int(r[ci["x"]]) for r in rows}
    return sum(w * by_id[i] for (i, _, _), w in zip(_ROWS, weights))


caps = [cap for _, cap, _ in _ROWS]
ones = [1] * len(_ROWS)


@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_lambda_in_a_reducer_coefficient(decidb_cli, oracle_solver):
    rows, cols = decidb_cli.execute(
        f"""
        SELECT id, x FROM {_TABLE} DECIDE x(INT)
        SUCH THAT x <= cap AND SUM(x * list_reduce([cap, b], (a, z) -> a + z)) <= 30
        MAXIMIZE SUM(x)
    """
    )
    coeffs = [cap + b for _, cap, b in _ROWS]
    assert _objective(rows, cols, ones) == pytest.approx(_oracle(oracle_solver, "lambda_coef", caps, coeffs, 30, ones))


@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_comparison_inside_a_lambda(decidb_cli, oracle_solver):
    rows, cols = decidb_cli.execute(
        f"""
        SELECT id, x FROM {_TABLE} DECIDE x(INT)
        SUCH THAT x <= cap AND SUM(x * len(list_filter([cap, b], q -> q > 1))) <= 12
        MAXIMIZE SUM(x)
    """
    )
    coeffs = [len([v for v in (cap, b) if v > 1]) for _, cap, b in _ROWS]
    assert _objective(rows, cols, ones) == pytest.approx(_oracle(oracle_solver, "lambda_cmp", caps, coeffs, 12, ones))


@pytest.mark.cons_perrow
@pytest.mark.correctness
def test_lambda_in_a_per_row_bound(decidb_cli, oracle_solver):
    rows, cols = decidb_cli.execute(
        f"""
        SELECT id, x FROM {_TABLE} DECIDE x(INT)
        SUCH THAT x <= list_reduce([cap, b], (a, z) -> a - z)
        MAXIMIZE SUM(x)
    """
    )
    ub = [cap - b for _, cap, b in _ROWS]
    assert _objective(rows, cols, ones) == pytest.approx(_oracle(oracle_solver, "lambda_row", ub, None, 0, ones))


@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_lambda_in_the_objective(decidb_cli, oracle_solver):
    rows, cols = decidb_cli.execute(
        f"""
        SELECT id, x FROM {_TABLE} DECIDE x(INT)
        SUCH THAT x <= cap AND SUM(x) <= 8
        MAXIMIZE SUM(x * list_reduce([cap, b], (a, z) -> a * z))
    """
    )
    weights = [cap * b for _, cap, b in _ROWS]
    expected = _oracle(oracle_solver, "lambda_obj", caps, ones, 8, weights)
    assert _objective(rows, cols, weights) == pytest.approx(expected)


@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_lambda_in_a_reduced_bound(decidb_cli, oracle_solver):
    rows, cols = decidb_cli.execute(
        f"""
        SELECT id, x FROM {_TABLE} DECIDE x(INT)
        SUCH THAT x <= cap AND SUM(x) <= list_reduce([2, 4], (a, z) -> a + z)
        MAXIMIZE SUM(x * cap)
    """
    )
    assert _objective(rows, cols, caps) == pytest.approx(_oracle(oracle_solver, "lambda_bound", caps, ones, 6, caps))


@pytest.mark.cons_perrow
@pytest.mark.correctness
def test_comparison_in_a_per_row_bound(decidb_cli, oracle_solver):
    rows, cols = decidb_cli.execute(
        f"""
        SELECT id, x FROM {_TABLE} DECIDE x(INT)
        SUCH THAT x <= 4 * CAST((cap > 2 AND cap < 6) AS INT)
        MAXIMIZE SUM(x)
    """
    )
    ub = [4 * int(2 < cap < 6) for _, cap, _ in _ROWS]
    assert _objective(rows, cols, ones) == pytest.approx(_oracle(oracle_solver, "cmp_row", ub, None, 0, ones))


@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_comparison_in_a_reducer_coefficient(decidb_cli, oracle_solver):
    rows, cols = decidb_cli.execute(
        f"""
        SELECT id, x FROM {_TABLE} DECIDE x(INT)
        SUCH THAT x <= cap AND SUM(x * CAST(cap > 4 AS INT)) <= 4
        MAXIMIZE SUM(x)
    """
    )
    coeffs = [int(cap > 4) for _, cap, _ in _ROWS]
    assert _objective(rows, cols, ones) == pytest.approx(_oracle(oracle_solver, "cmp_coef", caps, coeffs, 4, ones))


@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_comparison_in_a_reduced_bound(decidb_cli, oracle_solver):
    rows, cols = decidb_cli.execute(
        f"""
        SELECT id, x FROM {_TABLE} DECIDE x(INT)
        SUCH THAT x <= cap AND SUM(x) <= 5 * CAST(2 > 1 AS INT)
        MAXIMIZE SUM(x * cap)
    """
    )
    assert _objective(rows, cols, caps) == pytest.approx(_oracle(oracle_solver, "cmp_bound", caps, ones, 5, caps))


@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
@pytest.mark.parametrize("limit,weight", [(2, 3), (4, 1)])
def test_prepared_statement_parameters(decidb_cli, oracle_solver, limit, weight):
    """`$1` in a per-row bound, `$2` in a reducer, its bound and the objective; two
    EXECUTEs of one plan take different values."""
    rows, cols = decidb_cli.execute(
        f"""
        PREPARE p AS SELECT id, x FROM {_TABLE} DECIDE x(INT)
        SUCH THAT x <= $1 AND x <= cap AND SUM(x * $2) <= 20 AND SUM(x) <= $2 + 4
        MAXIMIZE SUM(x * $2 * cap);
        EXECUTE p({limit}, {weight});
    """
    )
    ub = [min(limit, cap) for cap in caps]
    weights = [weight * cap for cap in caps]
    oracle_solver.create_model(f"prepared_{limit}_{weight}")
    names = [f"x_{i}" for i in range(len(_ROWS))]
    for n, u in zip(names, ub):
        oracle_solver.add_variable(n, VarType.INTEGER, lb=0.0, ub=float(u))
    oracle_solver.add_constraint({n: float(weight) for n in names}, "<=", 20.0, name="weighted")
    oracle_solver.add_constraint({n: 1.0 for n in names}, "<=", float(weight + 4), name="count")
    oracle_solver.set_objective({n: float(w) for n, w in zip(names, weights)}, ObjSense.MAXIMIZE)
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL
    assert _objective(rows, cols, weights) == pytest.approx(result.objective_value)


@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_table_macro_parameters(decidb_cli, oracle_solver):
    """A table macro's parameters in a constraint and in the objective.

    Without the serializer round trip, which cannot replay the macro's CREATE; the plan
    the macro expands to holds only constants.
    """
    rows, cols = decidb_cli.without_verify_serializer().execute(
        f"""
        CREATE TEMP MACRO capped(lim, w) AS TABLE
            SELECT id, x FROM {_TABLE} DECIDE x(INT)
            SUCH THAT x <= cap AND SUM(x) <= lim
            MAXIMIZE SUM(x * cap * w);
        FROM capped(6, 2);
    """
    )
    weights = [2 * cap for cap in caps]
    expected = _oracle(oracle_solver, "macro", caps, ones, 6, weights)
    assert _objective(rows, cols, weights) == pytest.approx(expected)
