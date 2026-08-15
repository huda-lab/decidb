"""Tests for SUM-based aggregate constraints.

Covers:
  - q08_marketing_campaign: SUM(x * constant) <= budget
  - order_selection: SUM(x) <= count_limit
"""

import time

import pytest

from solver.types import VarType, ObjSense
from comparison.compare import compare_solutions


@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_q08_marketing_campaign(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Marketing: select customers, cost=10/customer, budget=500."""
    sql = """
        SELECT c_custkey, c_acctbal, x
        FROM customer
        WHERE c_nationkey = 1
        DECIDE x(BOOL)
        SUCH THAT SUM(x * 10) <= 500
        MAXIMIZE SUM(x * c_acctbal)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(c_custkey AS BIGINT),
               CAST(c_acctbal AS DOUBLE)
        FROM customer WHERE c_nationkey = 1
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("q08_marketing")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    # Constraint: SUM(x_i * 10) <= 500  => each coefficient is 10
    oracle_solver.add_constraint(
        {vnames[i]: 10.0 for i in range(len(data))},
        "<=", 500.0, name="budget",
    )
    # Objective: MAXIMIZE SUM(x_i * c_acctbal_i)
    oracle_solver.set_objective(
        {vnames[i]: data[i][1] for i in range(len(data))},
        ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_result, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[decidb_cols.index("c_acctbal")])},
    )

    perf_tracker.record(
        "q08_marketing_campaign", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_order_selection(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Select orders: SUM(x) <= 300, maximize total price."""
    sql = """
        SELECT x, o_orderkey, o_totalprice
        FROM orders
        WHERE o_orderdate >= '1995-01-01' AND o_orderdate < '1995-02-01'
        DECIDE x(BOOL)
        SUCH THAT SUM(x) <= 300
        MAXIMIZE SUM(x * o_totalprice)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(o_orderkey AS BIGINT),
               CAST(o_totalprice AS DOUBLE)
        FROM orders
        WHERE o_orderdate >= '1995-01-01' AND o_orderdate < '1995-02-01'
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("order_selection")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    # SUM(x) <= 300  => each coefficient is 1.0
    oracle_solver.add_constraint(
        {vnames[i]: 1.0 for i in range(len(data))},
        "<=", 300.0, name="count_limit",
    )
    oracle_solver.set_objective(
        {vnames[i]: data[i][1] for i in range(len(data))},
        ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_result, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[decidb_cols.index("o_totalprice")])},
    )

    perf_tracker.record(
        "order_selection", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_sum_gte_with_maximize(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Set-covering pattern: SUM(x) >= 10 with MAXIMIZE objective."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity, x
        FROM lineitem
        WHERE l_orderkey < 50
        DECIDE x(BOOL)
        SUCH THAT SUM(x) >= 10
            AND SUM(x * l_quantity) <= 500
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
    oracle_solver.create_model("sum_gte_maximize")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    oracle_solver.add_constraint(
        {vnames[i]: 1.0 for i in range(len(data))},
        ">=", 10.0, name="min_count",
    )
    oracle_solver.add_constraint(
        {vnames[i]: data[i][3] for i in range(len(data))},
        "<=", 500.0, name="capacity",
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
        "sum_gte_maximize", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 2,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


# --- Aggregate constraint with constant offset on LHS ---
# The symbolic normalizer moves the constant to RHS for aggregate constraints
# (unlike per-row where the ilp_model_builder does the move). This test locks
# in that behavior and guards against a regression where a future refactor
# might skip symbolic normalization and leave an INVALID_INDEX term in the
# aggregate path (where it would still be silently dropped by line 405-411).

@pytest.mark.var_integer
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_aggregate_lhs_with_constant_offset(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """`SUM(x) + 3 <= 10` must give `SUM(x) <= 7`. If the `+3` silently drops
    instead of moving to RHS, decidb would solve `SUM(x) <= 10` and produce
    a larger objective than the oracle."""
    sql = """
        SELECT ps_partkey, ps_availqty, x
        FROM partsupp WHERE ps_partkey < 10
        DECIDE x(INT)
        SUCH THAT SUM(x) + 3 <= 10
            AND x <= 5
        MAXIMIZE SUM(x * ps_availqty)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(ps_partkey AS BIGINT), CAST(ps_availqty AS DOUBLE)
        FROM partsupp WHERE ps_partkey < 10
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("agg_const_offset")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.INTEGER, lb=0.0, ub=5.0)
    # SUM(x) <= 10 - 3 = 7
    oracle_solver.add_constraint(
        {vnames[i]: 1.0 for i in range(len(data))},
        "<=", 7.0, name="sum_bound",
    )
    oracle_solver.set_objective(
        {vnames[i]: data[i][1] for i in range(len(data))}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()
    cmp = compare_solutions(
        decidb_result, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[decidb_cols.index("ps_availqty")])},
    )
    perf_tracker.record(
        "agg_const_offset", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 2,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


def _picked_ids(rows, cols):
    id_idx = cols.index("id")
    x_idx = cols.index("x")
    return {int(row[id_idx]) for row in rows if float(row[x_idx]) > 0.5}


def _picked_pair_ids(rows, cols, left, right):
    id_idx = cols.index("id")
    left_idx = cols.index(left)
    right_idx = cols.index(right)
    return {
        int(row[id_idx])
        for row in rows
        if float(row[left_idx]) > 0.5 and float(row[right_idx]) > 0.5
    }


@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_aggregate_bound_may_subtract(decidb_cli):
    """A bound may subtract, not only add.

    The binder refused `-` on the bound side of a reduced constraint while allowing
    `+`, so `SUM(x*cost) <= 12 - 2` and every other subtraction was rejected as "not
    a scalar". It also caught the minus in `-5.0::DOUBLE`, which is the literal's own
    sign, leaving no way to write a negative bound with a cast. Each spelling below
    is paired with the equivalent literal so a wrong value fails as loudly as a
    rejection would.
    """
    query = """
        SELECT id, cost, value, x
        FROM (
            VALUES (1, 4.0, 10.0), (2, 5.0, 20.0), (3, 2.0, 15.0)
        ) t(id, cost, value)
        DECIDE x(BOOL)
        SUCH THAT SUM(x * cost) <= {bound}
        MAXIMIZE SUM(x * value)
    """
    for bound, same_as in (("12 - 2", "10"), ("-(2) + 12", "10"), ("12.0 - 2.0::DOUBLE", "10")):
        rows, cols = decidb_cli.execute(query.format(bound=bound))
        expected, expected_cols = decidb_cli.execute(query.format(bound=same_as))
        assert _picked_ids(rows, cols) == _picked_ids(expected, expected_cols), \
            f"bound `{bound}` did not behave like `{same_as}`"


@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_sum_body_data_only_offset_plain(decidb_cli):
    """`SUM(x + cost) <= K` subtracts SUM(cost), not just the scalar literals."""
    rows, cols = decidb_cli.execute("""
        SELECT id, cost, value, x
        FROM (
            VALUES (1, 4.0, 10.0), (2, 5.0, 20.0), (3, 2.0, 15.0)
        ) t(id, cost, value)
        DECIDE x(BOOL)
        SUCH THAT SUM(x + cost) <= 12
        MAXIMIZE SUM(x * value)
    """)
    assert _picked_ids(rows, cols) == {2}


@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_sum_body_data_only_offset_weighted_repro_shape(decidb_cli):
    """`SUM(q * (price + x))` keeps the data-only `q*price` aggregate offset."""
    rows, cols = decidb_cli.execute("""
        SELECT id, q, price, x
        FROM (
            VALUES (1, 1.0, 5.0), (2, 2.0, 3.0), (3, 3.0, 1.0)
        ) t(id, q, price)
        DECIDE x(BOOL)
        SUCH THAT SUM(q * (price + x)) <= 18
        MAXIMIZE SUM(x * q)
    """)
    assert _picked_ids(rows, cols) == {1, 3}


@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.bilinear
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_sum_body_data_only_offset_bilinear_constraint(decidb_cli):
    """Bilinear aggregate constraints subtract fixed LHS offsets before model build."""
    rows, cols = decidb_cli.execute("""
        SELECT id, cost, value, x, y
        FROM (
            VALUES (1, 2.0, 10.0), (2, 3.0, 20.0), (3, 1.0, 15.0)
        ) t(id, cost, value)
        DECIDE x(BOOL), y(BOOL)
        SUCH THAT SUM(x * y + cost) <= 8
        MAXIMIZE SUM(x * y * value)
    """)
    assert _picked_pair_ids(rows, cols, "x", "y") == {2, 3}


@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.when
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_sum_body_data_only_offset_outer_when(decidb_cli):
    """Outer WHEN filters both decision and data-only aggregate contributions."""
    rows, cols = decidb_cli.execute("""
        SELECT id, cost, w, value, x
        FROM (
            VALUES (1, 4.0, true, 10.0),
                   (2, 4.0, true, 20.0),
                   (3, 100.0, false, 30.0)
        ) t(id, cost, w, value)
        DECIDE x(BOOL)
        SUCH THAT SUM(x + cost) <= 9 WHEN w
        MAXIMIZE SUM(x * value)
    """)
    assert _picked_ids(rows, cols) == {2, 3}


@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.per_clause
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_sum_body_data_only_offset_per_group(decidb_cli):
    """PER subtracts the data-only aggregate offset separately for each group."""
    rows, cols = decidb_cli.execute("""
        SELECT id, grp, cost, value, x
        FROM (
            VALUES (1, 'A', 2.0, 10.0),
                   (2, 'A', 3.0, 20.0),
                   (3, 'B', 1.0, 5.0),
                   (4, 'B', 1.0, 6.0)
        ) t(id, grp, cost, value)
        DECIDE x(BOOL)
        SUCH THAT SUM(x + cost) <= 6 PER grp
        MAXIMIZE SUM(x * value)
    """)
    assert _picked_ids(rows, cols) == {2, 3, 4}


@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_avg_body_data_only_offset(decidb_cli):
    """AVG applies the denominator to data-only terms before moving them to RHS."""
    rows, cols = decidb_cli.execute("""
        SELECT id, cost, value, x
        FROM (
            VALUES (1, 2.0, 10.0), (2, 4.0, 20.0)
        ) t(id, cost, value)
        DECIDE x(BOOL)
        SUCH THAT AVG(x + cost) <= 3.5
        MAXIMIZE SUM(x * value)
    """)
    assert _picked_ids(rows, cols) == {2}


# --- Aggregate constraint with division coefficients ---
# `/` was previously rejected inside SUM; the SUM-argument validator now
# allows it when the divisor is decide-var-free, which lets users write
# `SUM(x / 2)` and `SUM(x / col)` directly. The downstream symbolic
# normalizer also had to learn to emit a real division expression for
# negative-integer Power exponents (so `x * w^-1` round-trips back to
# `x / w`).

@pytest.mark.var_integer
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_aggregate_sum_divided_by_constant(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """`SUM(x / 2) <= 5` is equivalent to `SUM(x) <= 10` (constant divisor)."""
    sql = """
        SELECT ps_partkey, ps_availqty, x
        FROM partsupp WHERE ps_partkey < 10
        DECIDE x(INT)
        SUCH THAT SUM(x / 2) <= 5
            AND x <= 5
        MAXIMIZE SUM(x * ps_availqty)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(ps_partkey AS BIGINT), CAST(ps_availqty AS DOUBLE)
        FROM partsupp WHERE ps_partkey < 10
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("agg_div_const")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.INTEGER, lb=0.0, ub=5.0)
    # SUM(x_i * 0.5) <= 5 — same shape as `x_i / 2` per row.
    oracle_solver.add_constraint(
        {vnames[i]: 0.5 for i in range(len(data))},
        "<=", 5.0, name="sum_div2",
    )
    oracle_solver.set_objective(
        {vnames[i]: data[i][1] for i in range(len(data))}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()
    cmp = compare_solutions(
        decidb_result, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[decidb_cols.index("ps_availqty")])},
    )
    perf_tracker.record(
        "agg_div_const", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 2,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.var_real
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_aggregate_sum_divided_by_data_column(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """`SUM(x / w) <= K` with per-row divisor w. The coefficient on each
    x_i is 1/w_i; the symbolic normalizer must reconstruct the division
    instead of throwing on the negative exponent it produced internally."""
    sql = """
        WITH data AS (
            SELECT 1 AS id, 2.0 AS w UNION ALL
            SELECT 2, 4.0 UNION ALL
            SELECT 3, 0.5
        )
        SELECT id, w, ROUND(x, 4) AS x FROM data
        DECIDE x(REAL)
        SUCH THAT SUM(x / w) <= 10
            AND x <= 10
        MAXIMIZE SUM(x)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(id AS BIGINT), CAST(w AS DOUBLE) FROM (
            SELECT 1 AS id, 2.0 AS w UNION ALL
            SELECT 2, 4.0 UNION ALL
            SELECT 3, 0.5
        )
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("agg_div_col")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.CONTINUOUS, lb=0.0, ub=10.0)
    oracle_solver.add_constraint(
        {vnames[i]: 1.0 / data[i][1] for i in range(len(data))},
        "<=", 10.0, name="sum_div_w",
    )
    oracle_solver.set_objective(
        {vnames[i]: 1.0 for i in range(len(data))}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()
    cmp = compare_solutions(
        decidb_result, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": 1.0},
    )
    perf_tracker.record(
        "agg_div_col", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 2,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )
