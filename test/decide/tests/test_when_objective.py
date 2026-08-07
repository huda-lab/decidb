"""Tests for WHEN clause on objectives.

Covers:
  - MAXIMIZE with WHEN filter
  - MINIMIZE with WHEN filter
  - Boundary: no rows match (objective is zero)
  - Boundary: all rows match (equivalent to no WHEN)
  - Same WHEN condition on both constraint and objective
  - Different WHEN conditions on constraint vs objective
  - Unconditional constraint + WHEN objective
"""

import time

import pytest

from solver.types import VarType, ObjSense
from comparison.compare import compare_solutions


@pytest.mark.when_objective
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_when_objective_maximize(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Only returned items contribute to profit: MAXIMIZE SUM(x*price) WHEN flag='R'."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity,
               l_returnflag, x
        FROM lineitem
        WHERE l_orderkey < 100
        DECIDE x(BOOL)
        SUCH THAT SUM(x * l_quantity) <= 100
        MAXIMIZE SUM(x * l_extendedprice) WHEN l_returnflag = 'R'
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_extendedprice AS DOUBLE),
               CAST(l_quantity AS DOUBLE),
               l_returnflag
        FROM lineitem WHERE l_orderkey < 100
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("when_obj_max")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    # Constraint applies to ALL rows (no WHEN)
    oracle_solver.add_constraint(
        {vnames[i]: data[i][3] for i in range(len(data))},
        "<=", 100.0, name="capacity",
    )
    # Objective: only 'R' rows contribute
    oracle_solver.set_objective(
        {vnames[i]: data[i][2] for i in range(len(data)) if data[i][4] == 'R'},
        ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    price_idx = decidb_cols.index("l_extendedprice")
    flag_idx = decidb_cols.index("l_returnflag")
    cmp = compare_solutions(
        decidb_result, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {
            "x": float(row[price_idx]) if row[flag_idx] == 'R' else 0.0,
        },
    )

    perf_tracker.record(
        "when_obj_max", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.when_objective
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_when_objective_minimize(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Minimize accepted-item quantity: MINIMIZE SUM(x*qty) WHEN flag='A'."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity,
               l_returnflag, x
        FROM lineitem
        WHERE l_orderkey < 100
        DECIDE x(BOOL)
        SUCH THAT SUM(x) >= 50
        MINIMIZE SUM(x * l_quantity) WHEN l_returnflag = 'A'
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_extendedprice AS DOUBLE),
               CAST(l_quantity AS DOUBLE),
               l_returnflag
        FROM lineitem WHERE l_orderkey < 100
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("when_obj_min")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    # Must select at least 50 items
    oracle_solver.add_constraint(
        {vnames[i]: 1.0 for i in range(len(data))},
        ">=", 50.0, name="min_count",
    )
    # Objective: minimize quantity contribution from 'A' rows only
    oracle_solver.set_objective(
        {vnames[i]: data[i][3] for i in range(len(data)) if data[i][4] == 'A'},
        ObjSense.MINIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    qty_idx = decidb_cols.index("l_quantity")
    flag_idx = decidb_cols.index("l_returnflag")
    cmp = compare_solutions(
        decidb_result, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {
            "x": float(row[qty_idx]) if row[flag_idx] == 'A' else 0.0,
        },
    )

    perf_tracker.record(
        "when_obj_min", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.when_objective
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.error_infeasible
def test_when_objective_no_match(decidb_cli):
    """Aggregate objective with WHEN matching no rows — now rejected pre-solver."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity,
               l_returnflag, x
        FROM lineitem
        WHERE l_orderkey < 100
        DECIDE x(BOOL)
        SUCH THAT SUM(x) <= 10
        MAXIMIZE SUM(x * l_extendedprice) WHEN l_returnflag = 'Z'
    """
    decidb_cli.assert_error(sql, match=r"empty|WHEN")


@pytest.mark.when_objective
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_when_objective_all_match(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """WHEN matches all rows on objective — equivalent to no WHEN."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity, x
        FROM lineitem
        WHERE l_orderkey < 100
        DECIDE x(BOOL)
        SUCH THAT SUM(x * l_quantity) <= 100
        MAXIMIZE SUM(x * l_extendedprice) WHEN l_quantity > 0
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_extendedprice AS DOUBLE),
               CAST(l_quantity AS DOUBLE)
        FROM lineitem WHERE l_orderkey < 100
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("when_obj_all_match")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    oracle_solver.add_constraint(
        {vnames[i]: data[i][3] for i in range(len(data))},
        "<=", 100.0, name="capacity",
    )
    # All l_quantity > 0 → all rows contribute to objective
    oracle_solver.set_objective(
        {vnames[i]: data[i][2] for i in range(len(data)) if data[i][3] > 0},
        ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_result, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[decidb_cols.index("l_extendedprice")])},
    )

    perf_tracker.record(
        "when_obj_all_match", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.when_objective
@pytest.mark.when_constraint
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_when_constraint_and_objective_same_condition(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker,
):
    """Same WHEN on both constraint and objective — non-R rows are invisible."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity,
               l_returnflag, x
        FROM lineitem
        WHERE l_orderkey < 100
        DECIDE x(BOOL)
        SUCH THAT SUM(x * l_quantity) <= 50 WHEN l_returnflag = 'R'
        MAXIMIZE SUM(x * l_extendedprice) WHEN l_returnflag = 'R'
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_extendedprice AS DOUBLE),
               CAST(l_quantity AS DOUBLE),
               l_returnflag
        FROM lineitem WHERE l_orderkey < 100
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("when_same_cond")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    # Both constraint and objective filter on 'R'
    oracle_solver.add_constraint(
        {vnames[i]: data[i][3] for i in range(len(data)) if data[i][4] == 'R'},
        "<=", 50.0, name="capacity_R",
    )
    oracle_solver.set_objective(
        {vnames[i]: data[i][2] for i in range(len(data)) if data[i][4] == 'R'},
        ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    price_idx = decidb_cols.index("l_extendedprice")
    flag_idx = decidb_cols.index("l_returnflag")
    cmp = compare_solutions(
        decidb_result, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {
            "x": float(row[price_idx]) if row[flag_idx] == 'R' else 0.0,
        },
    )

    perf_tracker.record(
        "when_same_cond", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.when_objective
@pytest.mark.when_constraint
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_when_constraint_and_objective_different_conditions(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker,
):
    """Constraint filters 'A' rows, objective rewards 'R' rows — disjoint filters."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity,
               l_returnflag, x
        FROM lineitem
        WHERE l_orderkey < 100
        DECIDE x(BOOL)
        SUCH THAT SUM(x * l_quantity) <= 100 WHEN l_returnflag = 'A'
        MAXIMIZE SUM(x * l_extendedprice) WHEN l_returnflag = 'R'
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_extendedprice AS DOUBLE),
               CAST(l_quantity AS DOUBLE),
               l_returnflag
        FROM lineitem WHERE l_orderkey < 100
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("when_diff_cond")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    # Constraint on 'A' rows only
    oracle_solver.add_constraint(
        {vnames[i]: data[i][3] for i in range(len(data)) if data[i][4] == 'A'},
        "<=", 100.0, name="capacity_A",
    )
    # Objective on 'R' rows only
    oracle_solver.set_objective(
        {vnames[i]: data[i][2] for i in range(len(data)) if data[i][4] == 'R'},
        ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    price_idx = decidb_cols.index("l_extendedprice")
    flag_idx = decidb_cols.index("l_returnflag")
    cmp = compare_solutions(
        decidb_result, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {
            "x": float(row[price_idx]) if row[flag_idx] == 'R' else 0.0,
        },
    )

    perf_tracker.record(
        "when_diff_cond", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.when_objective
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.cons_perrow
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_when_objective_with_unconditional_constraint(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker,
):
    """Unconditional constraints + WHEN on objective only."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity,
               l_returnflag, x
        FROM lineitem
        WHERE l_orderkey < 100
        DECIDE x(BOOL)
        SUCH THAT SUM(x * l_quantity) <= 100
            AND x <= 1
        MAXIMIZE SUM(x * l_extendedprice) WHEN l_returnflag = 'R'
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_extendedprice AS DOUBLE),
               CAST(l_quantity AS DOUBLE),
               l_returnflag
        FROM lineitem WHERE l_orderkey < 100
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("when_obj_uncon")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        # x <= 1 is inherent for BINARY, but models the per-row constraint
        oracle_solver.add_variable(vn, VarType.BINARY)

    oracle_solver.add_constraint(
        {vnames[i]: data[i][3] for i in range(len(data))},
        "<=", 100.0, name="capacity",
    )
    # Objective: only 'R' rows
    oracle_solver.set_objective(
        {vnames[i]: data[i][2] for i in range(len(data)) if data[i][4] == 'R'},
        ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    price_idx = decidb_cols.index("l_extendedprice")
    flag_idx = decidb_cols.index("l_returnflag")
    cmp = compare_solutions(
        decidb_result, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {
            "x": float(row[price_idx]) if row[flag_idx] == 'R' else 0.0,
        },
    )

    perf_tracker.record(
        "when_obj_uncon", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )
