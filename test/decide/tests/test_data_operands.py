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


@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize(
    "bound",
    [
        pytest.param("4 * CAST(b > 0 AND b < 5 AS INT)", id="and"),  # IsAllowedDecisionFreeBoundExpression: AND
        pytest.param("4 * CAST(b BETWEEN 1 AND 3 AS INT)", id="between"),  # and a BETWEEN
    ],
)
def test_a_truth_value_over_data_in_a_bound(decidb_cli, oracle_solver, bound):
    rows, cols = decidb_cli.execute(
        f"""
        SELECT id, x FROM {_TABLE} DECIDE x(INT)
        SUCH THAT x <= cap AND SUM(x) <= {bound}
        MAXIMIZE SUM(x * cap)
    """
    )
    assert _objective(rows, cols, caps) == pytest.approx(_oracle(oracle_solver, "truth_bound", caps, ones, 4, caps))


@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_a_conjunction_in_a_reducer_coefficient(decidb_cli, oracle_solver):
    """ValidateSumArgumentInternal reads a truth value over data as a 0/1 coefficient, a conjunction too."""
    rows, cols = decidb_cli.execute(
        f"""
        SELECT id, x FROM {_TABLE} DECIDE x(INT)
        SUCH THAT x <= cap AND SUM(x * CAST(cap > 3 AND b < 3 AS INT)) <= 4
        MAXIMIZE SUM(x)
    """
    )
    coeffs = [int(cap > 3 and b < 3) for _, cap, b in _ROWS]
    assert _objective(rows, cols, ones) == pytest.approx(_oracle(oracle_solver, "and_coef", caps, coeffs, 4, ones))


@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_a_function_over_a_decision_that_folds_to_a_constant(decidb_cli, oracle_solver):
    """`typeof(x)` is known at bind time, so the call folds to 'BIGINT' and `length` of it to 6: a constant that is not
    NULL, which BindFunction lets through (only a NULL it refuses)."""
    rows, cols = decidb_cli.execute(
        f"""
        SELECT id, x FROM {_TABLE} DECIDE x(INT)
        SUCH THAT x <= cap AND SUM(x * length(typeof(x))) <= 18
        MAXIMIZE SUM(x)
    """
    )
    assert _objective(rows, cols, ones) == pytest.approx(
        _oracle(oracle_solver, "typeof", caps, [6] * len(_ROWS), 18, ones)
    )


@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_an_operator_over_data_beside_a_reducer(decidb_cli, oracle_solver):
    """`COALESCE(cap, 0) + SUM(x) <= 10`: the data term moves to the bound, which is the tightest row's (ContainsDecideAggregate
    reads through the COALESCE, an operator, to the reducer beside it)."""
    rows, cols = decidb_cli.execute(
        f"""
        SELECT id, x FROM {_TABLE} DECIDE x(INT)
        SUCH THAT x <= cap AND COALESCE(cap, 0) + SUM(x) <= 10
        MAXIMIZE SUM(x * cap)
    """
    )
    budget = min(10 - cap for _, cap, _ in _ROWS)
    assert _objective(rows, cols, caps) == pytest.approx(
        _oracle(oracle_solver, "coalesce_bound", caps, ones, budget, caps)
    )


@pytest.mark.cons_perrow
@pytest.mark.correctness
def test_a_five_part_by_key_is_a_field_path(decidb_cli, oracle_solver):
    """`t.s.a.b.c` names no relation or column, so a `by` reads it as data: the struct field."""
    rows, cols = decidb_cli.execute(
        """
        SELECT id, x FROM (SELECT i AS id, {'a': {'b': {'c': 1}}} AS s FROM range(3) r(i)) t DECIDE x(INT)
        SUCH THAT x <= 5 AND SUM(x) by (t.s.a.b.c) <= 4
        MAXIMIZE SUM(x)
    """
    )
    got = sum(int(r[cols.index("x")]) for r in rows)
    oracle_solver.create_model("field_path_key")
    names = [f"x_{i}" for i in range(3)]
    for n in names:
        oracle_solver.add_variable(n, VarType.INTEGER, lb=0.0, ub=5.0)
    oracle_solver.add_constraint({n: 1.0 for n in names}, "<=", 4.0, name="one_group")
    oracle_solver.set_objective({n: 1.0 for n in names}, ObjSense.MAXIMIZE)
    assert got == pytest.approx(oracle_solver.solve().objective_value)


@pytest.mark.cons_perrow
@pytest.mark.correctness
def test_a_view_is_a_relation_of_keys(decidb_cli, oracle_solver):
    """A view's column keys a class like a table's (IsStoredColumn reads a catalog entry that is not a table)."""
    rows, cols = decidb_cli.without_verify_serializer().execute(
        f"""
        CREATE TEMP VIEW capped AS SELECT * FROM {_TABLE};
        SELECT id, x FROM capped DECIDE x(INT)
        SUCH THAT x <= cap AND per cap: SUM(x) by (cap) <= 2
        MAXIMIZE SUM(x * cap);
    """
    )
    ub = [min(2, cap) for cap in caps]
    assert _objective(rows, cols, caps) == pytest.approx(_oracle(oracle_solver, "view_key", ub, None, 0, caps))


@pytest.mark.cons_perrow
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_table_macro_parameters_in_scopes_keep_the_written_scopes_out_of_the_source_text(decidb_cli, oracle_solver):
    """A macro argument in the DECIDE tail leaves the tail with no source text, so the scopes' keys, conditions and
    `by` keys are rendered from the parsed tree: a `when` and `per` before a constraint, a `by`, the colon form and an
    objective's `when` all run their text-less branch here."""
    rows, cols = decidb_cli.without_verify_serializer().execute(
        f"""
        CREATE TEMP MACRO scoped(lim, lim2) AS TABLE
            SELECT id, x FROM {_TABLE} DECIDE x(INT)
            SUCH THAT x <= cap AND when b > 1 per cap: SUM(x) <= lim
                AND SUM(x) by (b) <= lim2 AND SUM(t: x) <= 100
            MAXIMIZE when b > 0: SUM(x * cap);
        FROM scoped(2, 3);
    """
    )
    # `when b > 1 per cap: SUM(x) <= 2` reads every kept row (no `by`), so rows 2 and 3 share the budget of 2.
    ub = [min(3, cap) for _, cap, _ in _ROWS]
    coeffs = [int(b > 1) for _, _, b in _ROWS]
    assert _objective(rows, cols, caps) == pytest.approx(_oracle(oracle_solver, "macro_scopes", ub, coeffs, 2, caps))
