"""An `INT` decision must be able to return the answer the solver found.

`x(INT)` used to bind to DuckDB `INTEGER`, a 32-bit column. Nothing in the pipeline
held a decision to that range: the solver works in doubles, and the bound a decision
is actually subject to need not be a number anyone wrote. So a query whose optimum
exceeded 2147483647 solved correctly and was then truncated on the way out, and the
user got 2147483647 — the type's limit presented as the answer, with no error and no
diagnostic. A saturating readback also makes the returned row violate the query's own
constraints, which is the one thing a DECIDE result is supposed to guarantee.

Rejecting an out-of-range bound at bind time cannot fix this, in either direction.
It is too narrow: `SUM(x) <= 5000000000` bounds no single decision, so there is no
bound to inspect, and `x <= cap` takes its limit from a column that binding has not
read. And it is too wide: a large bound does not imply a large answer, so refusing
`x <= 5000000000 AND SUM(x) <= 10` would refuse a query whose answer is 10.

`INT` therefore binds to `BIGINT`, which is also what DuckDB itself returns for
generated integers (`range()`). The limit that remains is the double's, not the
column's: past 2^53 a double stops counting consecutively, so the solver never
distinguished the value and no wider integer type recovers it. That is refused by
name rather than rounded.

Covers:
  - test_a_declared_bound_beyond_int32_returns_the_optimum: the reported case
  - test_a_bound_read_from_data_beyond_int32_returns_the_optimum: no literal to inspect
  - test_an_aggregate_row_beyond_int32_returns_the_optimum: no per-variable bound at all
  - test_a_large_bound_with_a_small_answer_still_answers: the over-wide refusal's case
  - test_the_int32_boundary_reads_back_exactly: the old ceiling is not a special value
  - test_an_int_decision_is_a_bigint_column: the widening, as the user sees it
  - test_a_bool_decision_is_unaffected: a 0/1 domain cannot overflow and did not move
  - test_a_value_past_exact_integer_range_is_refused_in_sql_terms: the honest limit
  - test_the_backends_agree_beyond_int32: no answer depends on the host's solver
"""

import pytest

from solver.types import VarType, ObjSense, SolverStatus


# Every case below is the same one-row problem — maximize a single INT decision against
# a limit of 5e9 — differing only in HOW the limit reaches the variable. That is the
# axis the bug lives on: the value is identical, and what changes is whether anything
# before readback could have known it would not fit.
_LIMIT = 5000000000

_DECLARED_BOUND = f"""
    SELECT id, x FROM (VALUES (1)) t(id)
    DECIDE x(INT) SUCH THAT x >= 0 AND x <= {_LIMIT} MAXIMIZE SUM(x)
"""

_BOUND_FROM_DATA = f"""
    SELECT id, x FROM (VALUES (1, {_LIMIT})) t(id, cap)
    DECIDE x(INT) SUCH THAT x >= 0 AND x <= cap MAXIMIZE SUM(x)
"""

_AGGREGATE_ROW = f"""
    SELECT id, x FROM (VALUES (1)) t(id)
    DECIDE x(INT) SUCH THAT SUM(x) <= {_LIMIT} MAXIMIZE SUM(x)
"""


def _single_int_oracle(oracle, name, ub):
    """The model all three spellings build: one integer decision, maximized against `ub`."""
    oracle.create_model(name)
    oracle.add_variable("x", VarType.INTEGER, lb=0.0, ub=float(ub))
    oracle.set_objective({"x": 1.0}, ObjSense.MAXIMIZE)
    return oracle.solve()


@pytest.mark.edge_case
@pytest.mark.var_integer
@pytest.mark.correctness
def test_a_declared_bound_beyond_int32_returns_the_optimum(decidb_cli, oracle_solver):
    """`x <= 5000000000` written as a literal — the case the bug was reported on.

    This one a bind-time range check could have caught. The two below are why one
    would not have been enough.
    """
    rows, _ = decidb_cli.execute(_DECLARED_BOUND)
    result = _single_int_oracle(oracle_solver, "declared_bound", _LIMIT)

    assert result.status == SolverStatus.OPTIMAL
    assert rows[0][1] == result.objective_value == _LIMIT


@pytest.mark.edge_case
@pytest.mark.var_integer
@pytest.mark.correctness
def test_a_bound_read_from_data_beyond_int32_returns_the_optimum(decidb_cli, oracle_solver):
    """`x <= cap` takes its limit from a column, so there is no literal to inspect.

    The binder sees a column reference. What it holds is not known until the relation
    is executed, by which point the bind-time refusal the bug report proposed is long
    past.
    """
    rows, _ = decidb_cli.execute(_BOUND_FROM_DATA)
    result = _single_int_oracle(oracle_solver, "bound_from_data", _LIMIT)

    assert result.status == SolverStatus.OPTIMAL
    assert rows[0][1] == result.objective_value == _LIMIT


@pytest.mark.edge_case
@pytest.mark.var_integer
@pytest.mark.cons_aggregate
def test_an_aggregate_row_beyond_int32_returns_the_optimum(decidb_cli, oracle_solver):
    """`SUM(x) <= 5000000000` bounds the total, and no single decision at all.

    The decisive case. `x` carries no bound here — over one row the aggregate row is
    what pins it, and over many rows a degenerate optimum may pile the whole total
    onto one of them. There is nothing a binder could reject, so the readback is the
    only layer that can be right about this.
    """
    rows, _ = decidb_cli.execute(_AGGREGATE_ROW)
    result = _single_int_oracle(oracle_solver, "aggregate_row", _LIMIT)

    assert result.status == SolverStatus.OPTIMAL
    assert rows[0][1] == result.objective_value == _LIMIT


@pytest.mark.edge_case
@pytest.mark.var_integer
def test_a_large_bound_with_a_small_answer_still_answers(decidb_cli, oracle_solver):
    """A bound out of int32 range with an answer well inside it.

    The case that rules out refusing the bound: what has to fit the column is the
    value, and `SUM(x) <= 10` decides that here. Rejecting `x <= 5000000000` on sight
    would refuse a query whose answer is 10.
    """
    rows, _ = decidb_cli.execute(f"""
        SELECT id, x FROM (VALUES (1)) t(id)
        DECIDE x(INT)
        SUCH THAT x >= 0 AND x <= {_LIMIT} AND SUM(x) <= 10
        MAXIMIZE SUM(x)
    """)
    oracle_solver.create_model("large_bound_small_answer")
    oracle_solver.add_variable("x", VarType.INTEGER, lb=0.0, ub=float(_LIMIT))
    oracle_solver.add_constraint({"x": 1.0}, "<=", 10.0, name="total")
    oracle_solver.set_objective({"x": 1.0}, ObjSense.MAXIMIZE)
    result = oracle_solver.solve()

    assert result.status == SolverStatus.OPTIMAL
    assert rows[0][1] == result.objective_value == 10


@pytest.mark.edge_case
@pytest.mark.var_integer
def test_the_int32_boundary_reads_back_exactly(decidb_cli, oracle_solver):
    """2147483647 was the value every oversized answer used to collapse onto.

    It is an ordinary answer, and must read back as itself rather than as evidence
    that something was clamped.
    """
    boundary = 2147483647
    rows, _ = decidb_cli.execute(f"""
        SELECT id, x FROM (VALUES (1)) t(id)
        DECIDE x(INT) SUCH THAT x >= 0 AND x <= {boundary} MAXIMIZE SUM(x)
    """)
    result = _single_int_oracle(oracle_solver, "int32_boundary", boundary)

    assert result.status == SolverStatus.OPTIMAL
    assert rows[0][1] == result.objective_value == boundary


@pytest.mark.edge_case
@pytest.mark.var_integer
def test_an_int_decision_is_a_bigint_column(decidb_cli):
    """The widening as the user meets it: `DECIDE x(INT)` yields a BIGINT column."""
    rows, _ = decidb_cli.execute("""
        SELECT id, typeof(x) AS declared FROM (VALUES (1)) t(id)
        DECIDE x(INT) SUCH THAT x <= 3 MAXIMIZE SUM(x)
    """)

    assert rows[0][1] == "BIGINT"


@pytest.mark.edge_case
@pytest.mark.var_boolean
def test_a_bool_decision_is_unaffected(decidb_cli):
    """`BOOL` did not widen, and did not need to.

    Its domain is 0/1, so it cannot reach the range that broke `INT`; widening it
    would have changed an output column type for nothing. Asserted alongside an `INT`
    in the same query, because the two sharing a query is what makes the mixed width
    observable at all.
    """
    rows, _ = decidb_cli.execute("""
        SELECT id, typeof(b) AS b_type, typeof(x) AS x_type, b, x
        FROM (VALUES (1)) t(id)
        DECIDE b(BOOL), x(INT)
        SUCH THAT b + x <= 5
        MAXIMIZE SUM(b + x)
    """)

    assert rows[0][1] == "INTEGER"
    assert rows[0][2] == "BIGINT"
    assert rows[0][3] + rows[0][4] == 5


@pytest.mark.edge_case
@pytest.mark.var_integer
@pytest.mark.error
def test_a_value_past_exact_integer_range_is_refused_in_sql_terms(decidb_cli):
    """Past 2^53 a double no longer counts consecutively, so the value is not there.

    No wider integer type recovers a number the solver never distinguished, which is
    what separates this refusal from the truncation it replaced: there is no correct
    answer being withheld. The message names the variable and an edit.
    """
    decidb_cli.assert_error(
        """
        SELECT id, x FROM (VALUES (1)) t(id)
        DECIDE x(INT) SUCH THAT SUM(x) <= 90071992547409920 MAXIMIZE SUM(x)
        """,
        match="beyond the range DeciDB can return as a whole number",
    )


@pytest.mark.edge_case
@pytest.mark.var_integer
@pytest.mark.parametrize("sql", [
    _DECLARED_BOUND, _BOUND_FROM_DATA, _AGGREGATE_ROW,
], ids=["declared-bound", "bound-from-data", "aggregate-row"])
def test_the_backends_agree_beyond_int32(decidb_cli_highs, decidb_cli_gurobi, sql):
    """Readback is backend-neutral, so neither solver may disagree about the value."""
    highs_rows, _ = decidb_cli_highs.execute(sql)
    gurobi_rows, _ = decidb_cli_gurobi.execute(sql)

    assert highs_rows == gurobi_rows
    assert highs_rows[0][1] == _LIMIT
