"""Tests for comparison operators on aggregate constraints.

<= and >= are exercised heavily in other test files. This file covers
the remaining operators:
  - SUM equality: SUM(x) = value
  - Strict less-than: SUM(x * col) < value
  - Strict greater-than: SUM(x) > value
  - Not-equal: SUM(x) <> value
  - Signed coefficient column in aggregate constraint (mixed positive/negative)
"""

import re
import time

import pytest

from decidb_cli import DecidBCliError
from solver.types import VarType, ObjSense, SolverStatus
from comparison.compare import compare_solutions

from ._oracle_helpers import add_ne_indicator

# A strict inequality over a REAL decision is refused at bind time: the declared
# type settles it without reading a row, so the message names the variable and
# quotes the clause. A strict inequality whose variables are all integer but whose
# coefficients are not is a different refusal, raised by the model builder once the
# coefficients have been evaluated.
_STRICT_LT_REAL_MSG = re.compile(
    r"Strict inequality '<' is not supported over the REAL decision"
)
_STRICT_GT_REAL_MSG = re.compile(
    r"Strict inequality '>' is not supported over the REAL decision"
)
_STRICT_LT_COEFF_MSG = re.compile(
    r"Comparison '<' is not supported here: .*can take fractional values"
)
_STRICT_QUADRATIC_MSG = re.compile(
    r"Strict inequality \('<' / '>'\) is not supported on constraints with "
    r"quadratic or bilinear terms"
)
_NE_REAL_MSG = re.compile(
    r"Inequality '<>' is not supported over the REAL decision"
)
_NE_FRACTIONAL_COEFF_MSG = re.compile(
    r"Inequality '<>' is not supported when a column scaling the "
    r"left-hand side holds a non-integer value"
)


@pytest.mark.cons_comparison
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_sum_equality_constraint(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """SUM(x) = exact_value — fix the total count of selected items."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity, x
        FROM lineitem WHERE l_orderkey < 50
        DECIDE x(BOOL)
        SUCH THAT SUM(x) = 10
        MAXIMIZE SUM(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_extendedprice AS DOUBLE),
               CAST(l_quantity AS DOUBLE)
        FROM lineitem WHERE l_orderkey < 50
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("sum_eq")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    oracle_solver.add_constraint(
        {vnames[i]: 1.0 for i in range(len(data))},
        "=", 10.0, name="exact_count",
    )
    oracle_solver.set_objective(
        {vnames[i]: data[i][2] for i in range(len(data))},
        ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_result, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[decidb_cols.index("l_extendedprice")])},
    )

    perf_tracker.record(
        "sum_eq", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.cons_comparison
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_sum_strict_less_than(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """SUM(x * qty) < 100 — strict less-than on aggregate.

    `l_quantity` is `DECIMAL(15,2)`, so the cast is required: the integer-step gate is
    stated on declared types, and a type that admits fractions is refused whatever its
    rows happen to hold. The cast is how the query states that these are whole numbers.
    See test_strict_rejected_on_fractional_column_type for the uncast spelling.
    """
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity, x
        FROM lineitem WHERE l_orderkey < 50
        DECIDE x(BOOL)
        SUCH THAT SUM(x * l_quantity::BIGINT) < 100
        MAXIMIZE SUM(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_extendedprice AS DOUBLE),
               CAST(l_quantity AS DOUBLE)
        FROM lineitem WHERE l_orderkey < 50
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("sum_strict_lt")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    # For integer variables, SUM < 100 ≡ SUM <= 99
    oracle_solver.add_constraint(
        {vnames[i]: data[i][3] for i in range(len(data))},
        "<=", 99.0, name="capacity_strict",
    )
    oracle_solver.set_objective(
        {vnames[i]: data[i][2] for i in range(len(data))},
        ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_result, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[decidb_cols.index("l_extendedprice")])},
    )

    perf_tracker.record(
        "sum_strict_lt", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.cons_comparison
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_sum_strict_greater_than(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """SUM(x) > 5 — strict greater-than on aggregate."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity, x
        FROM lineitem WHERE l_orderkey < 50
        DECIDE x(BOOL)
        SUCH THAT SUM(x) > 5
            AND SUM(x * l_quantity) <= 200
        MAXIMIZE SUM(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_extendedprice AS DOUBLE),
               CAST(l_quantity AS DOUBLE)
        FROM lineitem WHERE l_orderkey < 50
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("sum_strict_gt")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    # For integer variables, SUM(x) > 5 ≡ SUM(x) >= 6
    oracle_solver.add_constraint(
        {vnames[i]: 1.0 for i in range(len(data))},
        ">=", 6.0, name="min_count_strict",
    )
    oracle_solver.add_constraint(
        {vnames[i]: data[i][3] for i in range(len(data))},
        "<=", 200.0, name="capacity",
    )
    oracle_solver.set_objective(
        {vnames[i]: data[i][2] for i in range(len(data))},
        ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_result, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[decidb_cols.index("l_extendedprice")])},
    )

    perf_tracker.record(
        "sum_strict_gt", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 2,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.cons_comparison
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_sum_negative_constant_multiplier(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """``SUM(x * (-10.0)) <= -50`` — negative constant literal as the coefficient.

    Sibling of ``test_sum_negative_coeffs_aggregate`` (which uses a signed
    *column*). Here the negative coefficient is a constant literal. The
    canonicalizer in ``decide_canonicalizer.cpp`` handles literal
    multiplication as a distinct path from column-sourced coefficients; a
    sign-handling bug there would silently flip ``<= -50`` into ``>= -50``
    and widen the feasible set.
    """
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, x
        FROM lineitem WHERE l_orderkey < 20
        DECIDE x(BOOL)
        SUCH THAT SUM(x * (-10.0)) <= -50
        MAXIMIZE SUM(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_extendedprice AS DOUBLE)
        FROM lineitem WHERE l_orderkey < 20
    """).fetchall()
    n = len(data)

    t_build = time.perf_counter()
    oracle_solver.create_model("sum_negative_constant_multiplier")
    vnames = [f"x_{i}" for i in range(n)]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    # Encode the negative form verbatim: SUM(-10 * x) <= -50.
    oracle_solver.add_constraint(
        {vnames[i]: -10.0 for i in range(n)},
        "<=", -50.0, name="neg_literal_floor",
    )
    oracle_solver.set_objective(
        {vnames[i]: data[i][2] for i in range(n)},
        ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    price_idx = decidb_cols.index("l_extendedprice")
    cmp = compare_solutions(
        decidb_rows, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[price_idx])},
    )

    perf_tracker.record(
        "sum_negative_constant_multiplier", decidb_time, build_time,
        result.solve_time_seconds, n, n, 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.cons_comparison
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_sum_negative_coeffs_aggregate(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """Aggregate constraint ``SUM(x * cost) >= 0`` with mixed-sign cost column.

    Exercises coefficient-sign handling during aggregate constraint
    extraction in physical_decide.cpp. Only negative *objective* coefficients
    were previously tested (see `test_edge_cases.py`); a sign error in the
    constraint path would silently flip polarity. The RHS is deliberately
    tight (= 0) so that a sign flip would change the feasible set (swapping
    which rows may be selected), producing a different vector/objective.
    """
    data_sql = (
        "SELECT * FROM (VALUES "
        "(1, -5.0, 10.0), (2, -3.0, 8.0), (3, 7.0, 15.0), (4, -1.0, 5.0)"
        ") t(id, cost, val)"
    )
    decide_sql = f"""
        SELECT id, cost, val, x FROM ({data_sql})
        DECIDE x(BOOL)
        SUCH THAT SUM(x * cost) >= 0
        MAXIMIZE SUM(x * val)
    """
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute(decide_sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute(
        f"SELECT CAST(id AS BIGINT), CAST(cost AS DOUBLE), CAST(val AS DOUBLE) "
        f"FROM ({data_sql})"
    ).fetchall()
    n = len(data)

    t_build = time.perf_counter()
    oracle_solver.create_model("sum_negative_coeffs")
    vnames = [f"x_{i}" for i in range(n)]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    oracle_solver.add_constraint(
        {vnames[i]: data[i][1] for i in range(n)},
        ">=", 0.0, name="signed_cost_floor",
    )
    oracle_solver.set_objective(
        {vnames[i]: data[i][2] for i in range(n)},
        ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    val_idx = decidb_cols.index("val")
    cmp = compare_solutions(
        decidb_rows, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[val_idx])},
    )

    perf_tracker.record(
        "sum_negative_coeffs", decidb_time, build_time,
        result.solve_time_seconds, n, n, 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.cons_comparison
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
def test_sum_not_equal(decidb_cli):
    """SUM(x) <> 5 — not-equal on aggregate (disjunctive, hard for ILP)."""
    result, _ = decidb_cli.execute("""
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity, x
        FROM lineitem WHERE l_orderkey < 50
        DECIDE x(BOOL)
        SUCH THAT SUM(x) <> 5
            AND SUM(x * l_quantity) <= 200
        MAXIMIZE SUM(x * l_extendedprice)
    """)
    assert len(result) > 0
    total = sum(row[4] for row in result)
    assert total != 5, f"SUM(x) = {total}, expected <> 5"


@pytest.mark.cons_comparison
@pytest.mark.var_integer
@pytest.mark.obj_maximize
def test_perrow_not_equal(decidb_cli):
    """x <> 3 — per-row not-equal constraint."""
    result, _ = decidb_cli.execute("""
        SELECT l_orderkey, l_linenumber, x
        FROM lineitem WHERE l_orderkey < 50
        DECIDE x(INT)
        SUCH THAT x <> 3
            AND x <= 5
            AND SUM(x * l_quantity) <= 500
        MAXIMIZE SUM(x * l_extendedprice)
    """)
    assert len(result) > 0
    x_idx = 2
    for row in result:
        assert row[x_idx] != 3, f"x={row[x_idx]}, expected <> 3"


@pytest.mark.cons_comparison
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
def test_sum_not_equal_zero(decidb_cli):
    """SUM(x) <> 0 — forces at least one selected."""
    result, _ = decidb_cli.execute("""
        SELECT l_orderkey, l_linenumber, x
        FROM lineitem WHERE l_orderkey < 50
        DECIDE x(BOOL)
        SUCH THAT SUM(x) <> 0
            AND SUM(x) <= 3
        MAXIMIZE SUM(x * l_extendedprice)
    """)
    assert len(result) > 0
    total = sum(row[2] for row in result)
    assert total != 0, f"SUM(x) = {total}, expected <> 0"
    assert total >= 1, f"SUM(x) must be at least 1"


@pytest.mark.cons_comparison
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.when
@pytest.mark.obj_maximize
def test_sum_not_equal_with_when(decidb_cli):
    """SUM(x) <> 5 WHEN condition — NE with conditional application."""
    result, _ = decidb_cli.execute("""
        SELECT l_orderkey, l_linenumber, l_quantity, x
        FROM lineitem WHERE l_orderkey < 50
        DECIDE x(BOOL)
        SUCH THAT SUM(x) <> 5 WHEN l_quantity > 20
            AND SUM(x * l_quantity) <= 200
        MAXIMIZE SUM(x * l_extendedprice)
    """)
    assert len(result) > 0
    conditional_total = sum(row[3] for row in result if row[2] > 20)
    assert conditional_total != 5, \
        f"SUM(x) WHEN l_quantity > 20 = {conditional_total}, expected <> 5"


@pytest.mark.cons_comparison
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.when
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_sum_not_equal_with_when_binding(decidb_cli, oracle_solver):
    """SUM(x) <> K WHEN cond — expression-level WHEN where NE is binding.
    Oracle emits an NE indicator over the active-row sum only."""
    rows, cols = decidb_cli.execute("""
        SELECT name, value, active, x FROM (
            VALUES ('a', 10, true),
                   ('b', 5, true),
                   ('c', 8, false),
                   ('d', 3, true)
        ) t(name, value, active)
        DECIDE x(BOOL)
        SUCH THAT SUM(x) <> 2 WHEN active
            AND SUM(x) <= 3
        MAXIMIZE SUM(x * value)
    """)

    data = [("a", 10, True), ("b", 5, True), ("c", 8, False), ("d", 3, True)]
    n = len(data)
    oracle_solver.create_model("sum_not_equal_with_when_binding")
    vnames = [f"x_{i}" for i in range(n)]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)
    active_coeffs = {vnames[i]: 1.0 for i in range(n) if data[i][2]}
    add_ne_indicator(oracle_solver, active_coeffs, 2.0, name="ne_active")
    oracle_solver.add_constraint(
        {vnames[i]: 1.0 for i in range(n)}, "<=", 3.0, name="sum_le_3",
    )
    oracle_solver.set_objective(
        {vnames[i]: float(data[i][1]) for i in range(n)}, ObjSense.MAXIMIZE,
    )
    res = oracle_solver.solve()
    assert res.status == SolverStatus.OPTIMAL

    ci = {name: i for i, name in enumerate(cols)}
    decidb_obj = sum(
        int(r[ci["x"]]) * int(r[ci["value"]]) for r in rows
    )
    assert abs(decidb_obj - res.objective_value) <= 1e-6, (
        f"Objective mismatch: DecidB={decidb_obj}, Oracle={res.objective_value}"
    )


@pytest.mark.cons_comparison
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_sum_not_equal_no_when_binding(decidb_cli, oracle_solver):
    """SUM(x) <> K without WHEN where NE is the binding constraint.
    Regression test: aggregate NE without WHEN after the global-z refactor."""
    rows, cols = decidb_cli.execute("""
        SELECT name, value, x FROM (
            VALUES ('a', 10),
                   ('b', 5),
                   ('c', 8),
                   ('d', 3)
        ) t(name, value)
        DECIDE x(BOOL)
        SUCH THAT SUM(x) <> 2
            AND SUM(x) <= 3
            AND SUM(x * value) <= 21
        MAXIMIZE SUM(x * value)
    """)

    data = [("a", 10), ("b", 5), ("c", 8), ("d", 3)]
    n = len(data)
    oracle_solver.create_model("sum_not_equal_no_when_binding")
    vnames = [f"x_{i}" for i in range(n)]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)
    sum_coeffs = {vnames[i]: 1.0 for i in range(n)}
    add_ne_indicator(oracle_solver, sum_coeffs, 2.0, name="ne_sum")
    oracle_solver.add_constraint(sum_coeffs, "<=", 3.0, name="sum_le_3")
    oracle_solver.add_constraint(
        {vnames[i]: float(data[i][1]) for i in range(n)},
        "<=", 21.0, name="value_le_21",
    )
    oracle_solver.set_objective(
        {vnames[i]: float(data[i][1]) for i in range(n)}, ObjSense.MAXIMIZE,
    )
    res = oracle_solver.solve()
    assert res.status == SolverStatus.OPTIMAL

    ci = {name: i for i, name in enumerate(cols)}
    decidb_obj = sum(
        int(r[ci["x"]]) * int(r[ci["value"]]) for r in rows
    )
    assert abs(decidb_obj - res.objective_value) <= 1e-6, (
        f"Objective mismatch: DecidB={decidb_obj}, Oracle={res.objective_value}"
    )


# ---------------------------------------------------------------------------
# Strict `<` / `>` on non-integer-valued LHS: must be rejected.
# The LHS of `SUM(x) < K` with REAL x is continuous, so the integer-step
# rewrite (`< K → <= K-1`) would silently change the feasible set. DecidB
# must raise InvalidInputException rather than produce a wrong answer.
# ---------------------------------------------------------------------------


@pytest.mark.cons_comparison
@pytest.mark.var_real
@pytest.mark.cons_aggregate
def test_real_sum_strict_lt_rejected(decidb_cli):
    """SUM(x) < K with(REAL) — integer-step rewrite is unsafe, must reject."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_quantity, x
        FROM lineitem WHERE l_orderkey < 10
        DECIDE x(REAL)
        SUCH THAT SUM(x) < 5
        MAXIMIZE SUM(x * l_extendedprice)
    """
    with pytest.raises(DecidBCliError) as exc_info:
        decidb_cli.execute(sql)
    assert _STRICT_LT_REAL_MSG.search(exc_info.value.message), (
        f"Unexpected error: {exc_info.value.message}"
    )


@pytest.mark.cons_comparison
@pytest.mark.var_real
@pytest.mark.cons_aggregate
def test_real_sum_strict_gt_rejected(decidb_cli):
    """SUM(x) > K with(REAL) — integer-step rewrite is unsafe, must reject."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, x
        FROM lineitem WHERE l_orderkey < 10
        DECIDE x(REAL)
        SUCH THAT SUM(x) > 1
            AND SUM(x * l_extendedprice) <= 50000
        MAXIMIZE SUM(x * l_extendedprice)
    """
    with pytest.raises(DecidBCliError) as exc_info:
        decidb_cli.execute(sql)
    assert _STRICT_GT_REAL_MSG.search(exc_info.value.message), (
        f"Unexpected error: {exc_info.value.message}"
    )


@pytest.mark.cons_comparison
@pytest.mark.var_real
def test_real_perrow_strict_rejected(decidb_cli):
    """Per-row x < K with(REAL) — same integer-step rewrite applies; must reject."""
    sql = """
        SELECT l_orderkey, l_linenumber, x
        FROM lineitem WHERE l_orderkey < 10
        DECIDE x(REAL)
        SUCH THAT x < 5
        MAXIMIZE SUM(x)
    """
    with pytest.raises(DecidBCliError) as exc_info:
        decidb_cli.execute(sql)
    assert _STRICT_LT_REAL_MSG.search(exc_info.value.message), (
        f"Unexpected error: {exc_info.value.message}"
    )


@pytest.mark.cons_comparison
@pytest.mark.var_real
def test_real_strict_rejection_names_variable_and_clause(decidb_cli):
    """The REAL refusal is structural, so it can quote what the user wrote.

    Nothing about `qty(REAL)` and `<` needs a row, so the refusal happens while
    binding and has the query text in hand. That is the point of rejecting there:
    the message can name the offending decision and echo the clause, which the
    model builder — working on evaluated coefficients — cannot do.
    """
    sql = """
        SELECT l_orderkey, l_linenumber, qty
        FROM lineitem WHERE l_orderkey < 10
        DECIDE qty(REAL)
        SUCH THAT qty < 5
        MAXIMIZE SUM(qty)
    """
    with pytest.raises(DecidBCliError) as exc_info:
        decidb_cli.execute(sql)
    message = exc_info.value.message
    assert _STRICT_LT_REAL_MSG.search(message), f"Unexpected error: {message}"
    assert "'qty'" in message, f"Message does not name the decision: {message}"
    assert "qty < 5" in message, f"Message does not quote the clause: {message}"


@pytest.mark.cons_comparison
@pytest.mark.var_real
@pytest.mark.cons_aggregate
def test_real_strict_accepted_when_l0_count_is_integral(decidb_cli):
    """A REAL decision under norm(..., 0, M) is ACCEPTED: the count is what is compared.

    The refusal is stated on the type of the *compared quantity*, not on every type
    appearing beneath it. `norm(e, 0, M)` counts nonzeros, so its value lands on the
    integer lattice however `e` is declared — that is the definition of L0, not an
    artifact of the 0/1-indicator linearization it receives. The integer step is
    therefore exact, and refusing here would reject a query with a correct encoding.

    Reversed 2026-08-17. The earlier rule refused this, stating the check on the
    declared type of `e` so that acceptance would not depend on a linearization
    choice. That principle stands; L0 integrality just is not a linearization choice.
    Settled together with the `<>` split so both operators answer alike.
    """
    sql = """
        SELECT l_orderkey, l_linenumber, l_quantity, new_qty
        FROM lineitem WHERE l_orderkey < 10
        DECIDE new_qty(REAL)
        SUCH THAT new_qty >= 0 AND new_qty <= 100
            AND norm(new_qty - l_quantity, 0, 1000) < 3
        MAXIMIZE SUM(new_qty)
    """
    strict_rows, _ = decidb_cli.execute(sql)
    assert strict_rows, "norm(..., 0, M) < 3 over a REAL decision should solve"

    # `< 3` and `<= 2` are the same cap on an integer count, so they must agree.
    stepped_rows, _ = decidb_cli.execute(sql.replace("< 3", "<= 2"))
    assert stepped_rows, "norm(..., 0, M) <= 2 should solve"
    assert len(strict_rows) == len(stepped_rows)

    # `<>` follows the same rule — it is the same integer-step argument.
    ne_rows, _ = decidb_cli.execute(sql.replace("< 3", "<> 3"))
    assert ne_rows, "norm(..., 0, M) <> 3 over a REAL decision should solve"


@pytest.mark.cons_comparison
@pytest.mark.var_real
@pytest.mark.cons_aggregate
def test_real_strict_rejected_when_term_stays_continuous(decidb_cli):
    """The L0 exemption is narrow: only p=0 is a count.

    `norm(e, 1)` sums magnitudes and is as continuous as `e`, so a REAL decision
    under one is still refused. This pins that IsIntegerValuedReducer did not open
    the gate for every reducer.
    """
    sql = """
        SELECT l_orderkey, l_linenumber, l_quantity, new_qty
        FROM lineitem WHERE l_orderkey < 10
        DECIDE new_qty(REAL)
        SUCH THAT new_qty >= 0 AND new_qty <= 100
            AND norm(new_qty - l_quantity, 1) < 3
        MAXIMIZE SUM(new_qty)
    """
    with pytest.raises(DecidBCliError) as exc_info:
        decidb_cli.execute(sql)
    assert _STRICT_LT_REAL_MSG.search(exc_info.value.message), (
        f"Unexpected error: {exc_info.value.message}"
    )


@pytest.mark.cons_comparison
@pytest.mark.var_integer
@pytest.mark.cons_aggregate
def test_integer_fractional_coeff_strict_rejected(decidb_cli):
    """SUM(0.5 * x) < K on INTEGER x — the multiplier makes the LHS half-integer; reject.

    Refused at bind time on the literal's declared type, `DECIMAL(2,1)`. A multiplier is
    unlike an additive offset: it rescales the decision off the integer lattice, so every
    factor is checked whether or not it carries a decision.
    """
    sql = """
        SELECT l_orderkey, l_linenumber, x
        FROM lineitem WHERE l_orderkey < 10
        DECIDE x(INT)
        SUCH THAT x <= 4
            AND SUM(0.5 * x) < 5
        MAXIMIZE SUM(x)
    """
    with pytest.raises(DecidBCliError) as exc_info:
        decidb_cli.execute(sql)
    assert _STRICT_LT_COEFF_MSG.search(exc_info.value.message), (
        f"Unexpected error: {exc_info.value.message}"
    )


@pytest.mark.cons_comparison
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
def test_strict_rejected_on_fractional_column_type(decidb_cli):
    """A DECIMAL column with a nonzero scale is refused even when its rows are whole.

    `l_quantity` is `DECIMAL(15,2)` and every row of it holds a whole number, so the
    integer step would in fact be exact today. It is refused anyway, deliberately: a
    value-based rule would make this query's *validity* depend on the table's contents,
    so inserting one fractional row would break a query that worked yesterday. The
    declared type is the contract.

    The message must name the column, its type, and the cast that fixes it — the cast is
    the user stating the assumption the rewrite needs.
    """
    sql = """
        SELECT l_orderkey, l_linenumber, x
        FROM lineitem WHERE l_orderkey < 50
        DECIDE x(BOOL)
        SUCH THAT SUM(x * l_quantity) < 100
        MAXIMIZE SUM(x)
    """
    with pytest.raises(DecidBCliError) as exc_info:
        decidb_cli.execute(sql)
    message = exc_info.value.message
    assert "l_quantity" in message, f"Message must name the column: {message}"
    assert "DECIMAL(15,2)" in message, f"Message must name the type: {message}"
    assert "l_quantity::BIGINT" in message, f"Message must name the cast: {message}"

    # The cast the message suggests is accepted.
    rows, _ = decidb_cli.execute(sql.replace("l_quantity)", "l_quantity::BIGINT)"))
    assert rows, "the suggested cast should solve"


@pytest.mark.cons_comparison
@pytest.mark.var_integer
@pytest.mark.cons_aggregate
def test_decimal_scale_zero_needs_no_cast(decidb_cli):
    """`DECIMAL(p, 0)` carries its scale in the type, so it is provably whole.

    The counterpart to the test above: the rule is "scale > 0, or a floating type",
    not "any DECIMAL".
    """
    rows, _ = decidb_cli.execute("""
        SELECT id, x FROM (
            VALUES (1, 2::DECIMAL(15,0)), (2, 3::DECIMAL(15,0))
        ) t(id, w)
        DECIDE x(INT) SUCH THAT x <= 4 AND SUM(x * w) < 10 MAXIMIZE SUM(x)
    """)
    assert rows, "a scale-0 DECIMAL coefficient should need no cast"


@pytest.mark.cons_comparison
@pytest.mark.var_boolean
@pytest.mark.var_real
@pytest.mark.cons_aggregate
def test_mixed_bool_real_strict_rejected(decidb_cli):
    """Mixed BOOLEAN + REAL in the LHS of SUM(...) < K — reject due to REAL term."""
    sql = """
        SELECT id, x, y FROM (VALUES (1), (2), (3)) t(id)
        DECIDE x(BOOL), y(REAL)
        SUCH THAT y <= 10
            AND SUM(x + y) < 20
        MAXIMIZE SUM(x + y)
    """
    with pytest.raises(DecidBCliError) as exc_info:
        decidb_cli.execute(sql)
    assert _STRICT_LT_REAL_MSG.search(exc_info.value.message), (
        f"Unexpected error: {exc_info.value.message}"
    )


@pytest.mark.cons_comparison
@pytest.mark.var_boolean
@pytest.mark.var_integer
@pytest.mark.cons_aggregate
@pytest.mark.bilinear
@pytest.mark.correctness
def test_bilinear_bool_int_strict_oracle(decidb_cli, oracle_solver):
    """SUM(b * n) < K with BOOLEAN × INTEGER LHS is integer-valued and must solve.

    The product b*n with b∈{0,1} and n∈ℤ takes integer values, so `< 5` is
    semantically equivalent to `<= 4`. Oracle formulates the McCormick
    linearization directly and uses the `<= 4` form; DecidB must produce the
    same objective.
    """
    sql = """
        SELECT id, b, n FROM (VALUES (1), (2), (3)) t(id)
        DECIDE b(BOOL), n(INT)
        SUCH THAT n <= 4
            AND SUM(b * n) < 5
        MAXIMIZE SUM(b * n)
    """
    rows, cols = decidb_cli.execute(sql)

    n_rows = 3
    U = 4.0
    oracle_solver.create_model("bilinear_bool_int_strict")
    bnames = [f"b_{i}" for i in range(n_rows)]
    nnames = [f"n_{i}" for i in range(n_rows)]
    wnames = [f"w_{i}" for i in range(n_rows)]
    for v in bnames:
        oracle_solver.add_variable(v, VarType.BINARY)
    for v in nnames:
        oracle_solver.add_variable(v, VarType.INTEGER, lb=0.0, ub=U)
    for v in wnames:
        oracle_solver.add_variable(v, VarType.CONTINUOUS, lb=0.0, ub=U)
    # McCormick w_i = b_i * n_i
    for i in range(n_rows):
        oracle_solver.add_constraint({wnames[i]: 1.0, bnames[i]: -U}, "<=", 0.0, name=f"mc_bU_{i}")
        oracle_solver.add_constraint({wnames[i]: 1.0, nnames[i]: -1.0}, "<=", 0.0, name=f"mc_wn_{i}")
        oracle_solver.add_constraint(
            {wnames[i]: 1.0, nnames[i]: -1.0, bnames[i]: -U}, ">=", -U, name=f"mc_tight_{i}"
        )
    # Integer-valued SUM(b*n) < 5  ⇔  SUM(w) <= 4
    oracle_solver.add_constraint({w: 1.0 for w in wnames}, "<=", 4.0, name="strict_lt_5")
    oracle_solver.set_objective({w: 1.0 for w in wnames}, ObjSense.MAXIMIZE)
    res = oracle_solver.solve()
    assert res.status == SolverStatus.OPTIMAL

    b_col = cols.index("b")
    n_col = cols.index("n")
    decidb_obj = sum(int(r[b_col]) * int(r[n_col]) for r in rows)
    # Integer-valued objective — no tolerance slack needed.
    assert decidb_obj < 5, f"strict `< 5` violated: DecidB SUM(b*n) = {decidb_obj}"
    assert abs(decidb_obj - res.objective_value) <= 1e-6, (
        f"Objective mismatch: DecidB={decidb_obj}, Oracle={res.objective_value}"
    )


@pytest.mark.cons_comparison
@pytest.mark.var_integer
@pytest.mark.cons_aggregate
@pytest.mark.bilinear
@pytest.mark.correctness
def test_bilinear_int_int_strict_oracle(decidb_cli, oracle_solver):
    """SUM(x * y) < K with INTEGER × INTEGER LHS — integer-valued, must solve.

    No boolean factor, so McCormick cannot linearize; the constraint stays
    quadratic and hits `BuildQuadraticConstraint`. The strict-rewrite must
    still fire there (`< 10 → <= 9`) because x*y with both x, y ∈ ℤ is integer.

    Gurobi-only (quadratic constraints via `GRBaddqconstr`). Under HiGHS, the
    backend rejects the quadratic constraint — we accept that as a pass.
    """
    sql = """
        SELECT id, x, y FROM (VALUES (1), (2)) t(id)
        DECIDE x(INT), y(INT)
        SUCH THAT x <= 4 AND y <= 4
            AND SUM(x * y) < 10
        MAXIMIZE SUM(x * y)
    """
    try:
        rows, cols = decidb_cli.execute(sql)
    except DecidBCliError as e:
        # HiGHS rejects quadratic constraints with a clear message — treat as pass.
        assert re.search(r"[Qq]uadratic|[Gg]urobi|HiGHS", e.message), (
            f"Unexpected error on int×int quadratic constraint: {e.message}"
        )
        return

    n_rows = 2
    U = 4
    oracle_solver.create_model("bilinear_int_int_strict")
    xnames = [f"x_{i}" for i in range(n_rows)]
    ynames = [f"y_{i}" for i in range(n_rows)]
    for v in xnames + ynames:
        oracle_solver.add_variable(v, VarType.INTEGER, lb=0.0, ub=float(U))
    # Integer-valued SUM(x*y) < 10 ⇔ SUM(x*y) <= 9
    oracle_solver.add_quadratic_constraint(
        linear={},
        quadratic={(xnames[i], ynames[i]): 1.0 for i in range(n_rows)},
        sense="<=",
        rhs=9.0,
        name="int_int_strict",
    )
    oracle_solver.set_quadratic_objective(
        linear={},
        quadratic={(xnames[i], ynames[i]): 1.0 for i in range(n_rows)},
        sense=ObjSense.MAXIMIZE,
    )
    res = oracle_solver.solve()
    assert res.status == SolverStatus.OPTIMAL

    x_col = cols.index("x")
    y_col = cols.index("y")
    decidb_obj = sum(int(r[x_col]) * int(r[y_col]) for r in rows)
    assert decidb_obj < 10, f"strict `< 10` violated: DecidB SUM(x*y) = {decidb_obj}"
    assert abs(decidb_obj - res.objective_value) <= 1e-6, (
        f"Objective mismatch: DecidB={decidb_obj}, Oracle={res.objective_value}"
    )


@pytest.mark.cons_comparison
@pytest.mark.var_integer
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_integer_sum_strict_lt_oracle(decidb_cli, duckdb_conn, oracle_solver):
    """SUM(x) < K on pure(INT) vars — the canonical integer-step rewrite case.

    Confirms that the `< K → <= K-1` rewrite still fires after the fix for
    LHS with no boolean factor and no bilinear term. Oracle encodes `<= K-1`.
    """
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, x
        FROM lineitem WHERE l_orderkey < 20
        DECIDE x(INT)
        SUCH THAT x <= 3
            AND SUM(x) < 10
        MAXIMIZE SUM(x * l_extendedprice)
    """
    rows, cols = decidb_cli.execute(sql)

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_extendedprice AS DOUBLE)
        FROM lineitem WHERE l_orderkey < 20
    """).fetchall()
    n = len(data)

    oracle_solver.create_model("integer_sum_strict_lt")
    vnames = [f"x_{i}" for i in range(n)]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.INTEGER, lb=0.0, ub=3.0)
    # Integer-valued SUM(x) < 10 ⇔ SUM(x) <= 9
    oracle_solver.add_constraint({vn: 1.0 for vn in vnames}, "<=", 9.0, name="strict_lt_10")
    oracle_solver.set_objective(
        {vnames[i]: data[i][2] for i in range(n)}, ObjSense.MAXIMIZE,
    )
    res = oracle_solver.solve()
    assert res.status == SolverStatus.OPTIMAL

    x_col = cols.index("x")
    decidb_sum_x = sum(int(r[x_col]) for r in rows)
    assert decidb_sum_x < 10, f"strict `< 10` violated: SUM(x) = {decidb_sum_x}"
    price_col = cols.index("l_extendedprice")
    decidb_obj = sum(int(r[x_col]) * float(r[price_col]) for r in rows)
    assert abs(decidb_obj - res.objective_value) <= 1e-3, (
        f"Objective mismatch: DecidB={decidb_obj}, Oracle={res.objective_value}"
    )


@pytest.mark.cons_comparison
@pytest.mark.var_integer
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_abs_integer_strict_oracle(decidb_cli, oracle_solver):
    """SUM(ABS(x - t)) < K on INTEGER x — ABS preserves integer-valuedness.

    Regression test for a sibling of the original bug: the ABS rewriter
    introduces an auxiliary variable. That aux must be declared INTEGER
    (not DOUBLE) when the inner expression is integer-typed, otherwise the
    LHS-integer check in ilp_model_builder would reject the strict inequality.
    """
    sql = """
        SELECT id, x FROM (VALUES (1), (2), (3)) t(id)
        DECIDE x(INT)
        SUCH THAT x <= 5
            AND SUM(ABS(x - 2)) < 4
        MAXIMIZE SUM(x)
    """
    rows, cols = decidb_cli.execute(sql)

    n = 3
    oracle_solver.create_model("abs_integer_strict")
    xnames = [f"x_{i}" for i in range(n)]
    anames = [f"a_{i}" for i in range(n)]  # a_i = |x_i - 2|
    for xn in xnames:
        oracle_solver.add_variable(xn, VarType.INTEGER, lb=0.0, ub=5.0)
    for an in anames:
        oracle_solver.add_variable(an, VarType.INTEGER, lb=0.0, ub=5.0)
    # ABS linearization: a_i >= x_i - 2, a_i >= -(x_i - 2) ⇔ a_i >= 2 - x_i
    for i in range(n):
        oracle_solver.add_constraint(
            {anames[i]: 1.0, xnames[i]: -1.0}, ">=", -2.0, name=f"abs_pos_{i}"
        )
        oracle_solver.add_constraint(
            {anames[i]: 1.0, xnames[i]: 1.0}, ">=", 2.0, name=f"abs_neg_{i}"
        )
    # Integer-valued SUM(a) < 4  ⇔  SUM(a) <= 3
    oracle_solver.add_constraint({an: 1.0 for an in anames}, "<=", 3.0, name="strict_lt_4")
    oracle_solver.set_objective({xn: 1.0 for xn in xnames}, ObjSense.MAXIMIZE)
    res = oracle_solver.solve()
    assert res.status == SolverStatus.OPTIMAL

    x_col = cols.index("x")
    decidb_obj = sum(int(r[x_col]) for r in rows)
    decidb_sum_abs = sum(abs(int(r[x_col]) - 2) for r in rows)
    assert decidb_sum_abs < 4, f"strict `< 4` violated: SUM(|x-2|) = {decidb_sum_abs}"
    assert abs(decidb_obj - res.objective_value) <= 1e-6, (
        f"Objective mismatch: DecidB={decidb_obj}, Oracle={res.objective_value}"
    )


@pytest.mark.cons_comparison
@pytest.mark.var_integer
@pytest.mark.correctness
def test_integer_perrow_strict_oracle(decidb_cli, duckdb_conn, oracle_solver):
    """Per-row `x < K` on(INT) — confirms per-row rewrite path works end-to-end."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, x
        FROM lineitem WHERE l_orderkey < 20
        DECIDE x(INT)
        SUCH THAT x < 4
            AND SUM(x * l_extendedprice) <= 50000
        MAXIMIZE SUM(x * l_extendedprice)
    """
    rows, cols = decidb_cli.execute(sql)

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_extendedprice AS DOUBLE)
        FROM lineitem WHERE l_orderkey < 20
    """).fetchall()
    n = len(data)

    oracle_solver.create_model("integer_perrow_strict")
    vnames = [f"x_{i}" for i in range(n)]
    for vn in vnames:
        # Integer-valued x < 4 ⇔ x <= 3
        oracle_solver.add_variable(vn, VarType.INTEGER, lb=0.0, ub=3.0)
    oracle_solver.add_constraint(
        {vnames[i]: data[i][2] for i in range(n)},
        "<=", 50000.0, name="budget",
    )
    oracle_solver.set_objective(
        {vnames[i]: data[i][2] for i in range(n)}, ObjSense.MAXIMIZE,
    )
    res = oracle_solver.solve()
    assert res.status == SolverStatus.OPTIMAL

    x_col = cols.index("x")
    for r in rows:
        assert int(r[x_col]) < 4, f"per-row `x < 4` violated: x={r[x_col]}"
    price_col = cols.index("l_extendedprice")
    decidb_obj = sum(int(r[x_col]) * float(r[price_col]) for r in rows)
    assert abs(decidb_obj - res.objective_value) <= 1e-3, (
        f"Objective mismatch: DecidB={decidb_obj}, Oracle={res.objective_value}"
    )


# ---------------------------------------------------------------------------
# Positive regression: non-strict inequalities on REAL LHS still work.
# Confirms the fix only narrowed strict `<` / `>`, not `<=` / `>=`.
# ---------------------------------------------------------------------------


@pytest.mark.cons_comparison
@pytest.mark.var_real
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_real_sum_le_still_works(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """SUM(x) <= K on REAL x still succeeds and matches oracle."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity, x
        FROM lineitem WHERE l_orderkey < 50
        DECIDE x(REAL)
        SUCH THAT SUM(x * l_quantity) <= 100
        MAXIMIZE SUM(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_extendedprice AS DOUBLE),
               CAST(l_quantity AS DOUBLE)
        FROM lineitem WHERE l_orderkey < 50
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("real_sum_le")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.CONTINUOUS, lb=0.0)
    oracle_solver.add_constraint(
        {vnames[i]: data[i][3] for i in range(len(data))},
        "<=", 100.0, name="capacity",
    )
    oracle_solver.set_objective(
        {vnames[i]: data[i][2] for i in range(len(data))},
        ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_result, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[decidb_cols.index("l_extendedprice")])},
    )

    perf_tracker.record(
        "real_sum_le", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.cons_comparison
@pytest.mark.var_real
@pytest.mark.cons_aggregate
def test_real_sum_not_equal_rejected(decidb_cli):
    """SUM(x) <> K with(REAL) — integer-step (±1) rewrite is unsafe, must reject.

    The NE Big-M rewrite expands `SUM(x) <> K` into `SUM(x) <= K-1 OR SUM(x) >= K+1`,
    which wrongly excludes feasible continuous points in the band (K-1, K+1) when
    the LHS is REAL-valued. `{v : v != K}` over the reals is an open set, so unlike
    the strict case there is no exact encoding to fall back to at all.

    Refused at BIND time, so the message can name the variable and quote the clause
    as written; the model builder keeps only the fractional-coefficient half (see
    test_ne_fractional_coeff_rejected).
    """
    sql = """
        SELECT l_orderkey, l_linenumber, x
        FROM lineitem WHERE l_orderkey < 10
        DECIDE x(REAL)
        SUCH THAT x <= 3.33 AND SUM(x) <> 10
        MAXIMIZE SUM(x)
    """
    with pytest.raises(DecidBCliError) as exc_info:
        decidb_cli.execute(sql)
    message = exc_info.value.message
    assert _NE_REAL_MSG.search(message), f"Unexpected error: {message}"
    # The point of rejecting at bind time: the refusal names the decision and echoes
    # the clause with the spelling the user typed, not the internal `!=`.
    assert "'x'" in message, f"Message does not name the decision: {message}"
    assert "<>" in message and "!=" not in message, (
        f"Message should quote the clause as written: {message}"
    )


# ---------------------------------------------------------------------------
# Regression: strict-inequality guard on the quadratic / bilinear constraint
# paths. The `IsEvalConstraintLhsIntegerValued` check was added to both the
# linear path (`ApplyComparisonSense`) and the quadratic path
# (`BuildQuadraticConstraint`). Pure-integer quadratic shapes rewrite via
# integer step and succeed (see `test_bilinear_int_int_strict_oracle`); the
# non-integer-LHS variants below must reject via the quadratic-path guard.
# ---------------------------------------------------------------------------


@pytest.mark.cons_comparison
@pytest.mark.var_real
@pytest.mark.cons_aggregate
def test_strict_lt_rejection_quadratic_real(decidb_cli):
    """SUM(POWER(x, 2)) < K with(REAL) — rejected by the quadratic-path guard.

    Routes through `BuildQuadraticConstraint`; LHS contains a REAL variable so
    `lhs_is_integer=false` and the guard fires with the shared message.
    """
    sql = """
        SELECT id, x FROM (VALUES (1), (2), (3)) t(id)
        DECIDE x(REAL)
        SUCH THAT x <= 10
            AND SUM(POWER(x, 2)) < 5
        MAXIMIZE SUM(x)
    """
    with pytest.raises(DecidBCliError) as exc_info:
        decidb_cli.execute(sql)
    assert _STRICT_LT_REAL_MSG.search(exc_info.value.message), (
        f"Unexpected error: {exc_info.value.message}"
    )


@pytest.mark.cons_comparison
@pytest.mark.var_boolean
@pytest.mark.var_real
@pytest.mark.cons_aggregate
@pytest.mark.bilinear
def test_strict_lt_rejection_bilinear_bool_real(decidb_cli):
    """SUM(b * y) < K with BOOLEAN × REAL — rejected by the bilinear/quadratic guard.

    Bool × Real linearizes via McCormick to an auxiliary CONTINUOUS aux (not
    integer-valued), so the strict-rewrite is unsafe and the guard must fire.
    """
    sql = """
        SELECT id, b, y FROM (VALUES (1), (2), (3)) t(id)
        DECIDE b(BOOL), y(REAL)
        SUCH THAT y <= 10
            AND SUM(b * y) < 5
        MAXIMIZE SUM(b * y)
    """
    with pytest.raises(DecidBCliError) as exc_info:
        decidb_cli.execute(sql)
    assert _STRICT_LT_REAL_MSG.search(exc_info.value.message), (
        f"Unexpected error: {exc_info.value.message}"
    )


@pytest.mark.cons_comparison
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_sum_not_equal_mixed_sign_coeffs(decidb_cli, oracle_solver):
    """SUM(x * cost) <> 0 with mixed-sign cost — NE disjunction must cover both
    sides of zero.

    The unconstrained maximum selects every row (signed sum = 0); NE forbids
    exactly that point, so a correctly-rewritten disjunction (``<= -1`` OR
    ``>= 1``) must drop exactly one contributing row. A one-sided Big-M would
    let the negative branch slip through.
    """
    rows, cols = decidb_cli.execute("""
        SELECT id, cost, val, x FROM (
            VALUES (1, -3, 10),
                   (2, 3, 10),
                   (3, -2, 5),
                   (4, 2, 5)
        ) t(id, cost, val)
        DECIDE x(BOOL)
        SUCH THAT SUM(x * cost) <> 0
        MAXIMIZE SUM(x * val)
    """)

    data = [(1, -3, 10), (2, 3, 10), (3, -2, 5), (4, 2, 5)]
    n = len(data)
    oracle_solver.create_model("sum_not_equal_mixed_sign_coeffs")
    vnames = [f"x_{i}" for i in range(n)]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)
    add_ne_indicator(
        oracle_solver,
        {vnames[i]: float(data[i][1]) for i in range(n)},
        0.0,
        name="ne_signed_sum",
    )
    oracle_solver.set_objective(
        {vnames[i]: float(data[i][2]) for i in range(n)}, ObjSense.MAXIMIZE,
    )
    res = oracle_solver.solve()
    assert res.status == SolverStatus.OPTIMAL

    ci = {name: i for i, name in enumerate(cols)}
    decidb_signed_sum = sum(
        int(r[ci["x"]]) * int(r[ci["cost"]]) for r in rows
    )
    assert decidb_signed_sum != 0, (
        f"NE constraint violated: DecidB SUM(x*cost) = 0"
    )
    decidb_obj = sum(
        int(r[ci["x"]]) * int(r[ci["val"]]) for r in rows
    )
    assert abs(decidb_obj - res.objective_value) <= 1e-6, (
        f"Objective mismatch: DecidB={decidb_obj}, Oracle={res.objective_value}"
    )
