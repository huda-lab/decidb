"""Tests for complex coefficient arithmetic in objectives/constraints.

Covers:
  - q03_complex_coeffs: SUM(x * (price * (1 - discount) * (1 + tax)))
"""

import time

import pytest

from solver.types import VarType, ObjSense
from comparison.compare import compare_solutions


@pytest.mark.var_boolean
@pytest.mark.obj_complex
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_q03_complex_coeffs(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Complex coefficients: discounted price with tax calculation."""
    sql = """
        SELECT l_orderkey, l_extendedprice, l_discount, l_tax, x
        FROM lineitem
        WHERE l_orderkey < 50
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * (l_extendedprice * (1 - l_discount) * (1 + l_tax))) <= 50000
        MAXIMIZE SUM(x)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_extendedprice AS DOUBLE),
               CAST(l_discount AS DOUBLE),
               CAST(l_tax AS DOUBLE)
        FROM lineitem WHERE l_orderkey < 50
    """).fetchall()

    # Compute the effective coefficient per row:
    # coeff_i = price_i * (1 - discount_i) * (1 + tax_i)
    coefficients = [
        row[1] * (1.0 - row[2]) * (1.0 + row[3]) for row in data
    ]

    t_build = time.perf_counter()
    oracle_solver.create_model("q03_complex")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    # SUM(x_i * coeff_i) <= 50000
    oracle_solver.add_constraint(
        {vnames[i]: coefficients[i] for i in range(len(data))},
        "<=", 50000.0, name="budget",
    )
    # MAXIMIZE SUM(x)  (just count)
    oracle_solver.set_objective(
        {vnames[i]: 1.0 for i in range(len(data))},
        ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    # Objective is SUM(x), so coefficient per row is 1.0
    cmp = compare_solutions(
        decidb_result, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": 1.0},
    )

    perf_tracker.record(
        "q03_complex_coeffs", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


# ===========================================================================
# Objective constant offset + scalar multiplier: peeled in the normalizer.
# ===========================================================================
# `MAXIMIZE SUM(x) + 3` adds a constant that doesn't affect argmax; the
# normalizer peels it onto LogicalDecide.objective_constant_offset so the
# model the solver sees is just `SUM(x)`. Similarly, `MAXIMIZE 2 * SUM(x)`
# folds the scalar into the SUM body: `SUM(2 * x)`. Both used to fail with
# "DECIDE objective contains a non-aggregate term" before the objective
# normalizer learned to peel/fold.

@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_objective_with_constant_offset(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """`MAXIMIZE SUM(x * cost) + 100` — additive constant peeled away."""
    sql = """
        SELECT c_custkey, c_acctbal, x FROM customer
        WHERE c_nationkey = 1
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) <= 5
        MAXIMIZE SUM(x * c_acctbal) + 100
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(c_custkey AS BIGINT), CAST(c_acctbal AS DOUBLE)
        FROM customer WHERE c_nationkey = 1
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("obj_const_offset")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)
    oracle_solver.add_constraint(
        {vnames[i]: 1.0 for i in range(len(data))},
        "<=", 5.0, name="count_limit",
    )
    # The +100 is ignored by argmax; oracle maximizes just SUM(x * c_acctbal).
    oracle_solver.set_objective(
        {vnames[i]: data[i][1] for i in range(len(data))}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()
    cmp = compare_solutions(
        decidb_result, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[decidb_cols.index("c_acctbal")])},
    )
    perf_tracker.record(
        "obj_const_offset", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_objective_with_scalar_multiplier(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """`MAXIMIZE 2 * SUM(x * cost)` — constant multiplier folded into the
    SUM body. Doesn't change argmax (2 > 0) but must not throw."""
    sql = """
        SELECT c_custkey, c_acctbal, x FROM customer
        WHERE c_nationkey = 1
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) <= 5
        MAXIMIZE 2 * SUM(x * c_acctbal)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(c_custkey AS BIGINT), CAST(c_acctbal AS DOUBLE)
        FROM customer WHERE c_nationkey = 1
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("obj_scalar_mult")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)
    oracle_solver.add_constraint(
        {vnames[i]: 1.0 for i in range(len(data))},
        "<=", 5.0, name="count_limit",
    )
    # Fold turns `2 * SUM(x * c_acctbal)` into `SUM(2 * x * c_acctbal)`.
    oracle_solver.set_objective(
        {vnames[i]: 2.0 * data[i][1] for i in range(len(data))}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()
    cmp = compare_solutions(
        decidb_result, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": 2.0 * float(row[decidb_cols.index("c_acctbal")])},
    )
    perf_tracker.record(
        "obj_scalar_mult", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )
