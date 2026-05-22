"""Tests for compound WHEN conditions (AND/OR with parentheses).

Covers:
  - AND compound on aggregate constraint
  - OR compound on aggregate constraint
  - AND compound on per-row constraint
  - AND compound on objective
  - Mixed string + numeric compound condition
  - OR compound on per-row constraint
"""

import time

import pytest

from solver.types import VarType, ObjSense
from comparison.compare import compare_solutions


@pytest.mark.when_compound
@pytest.mark.when_constraint
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_when_compound_and_aggregate(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """AND compound: SUM(x*qty) <= 50 WHEN (returnflag='R' AND linestatus='F')."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity,
               l_returnflag, l_linestatus, x
        FROM lineitem
        WHERE l_orderkey < 100
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * l_quantity) <= 50 WHEN (l_returnflag = 'R' AND l_linestatus = 'F')
        MAXIMIZE SUM(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_extendedprice AS DOUBLE),
               CAST(l_quantity AS DOUBLE),
               l_returnflag,
               l_linestatus
        FROM lineitem WHERE l_orderkey < 100
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("when_cmpd_and_agg")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    # AND: both conditions must hold
    oracle_solver.add_constraint(
        {vnames[i]: data[i][3]
         for i in range(len(data))
         if data[i][4] == 'R' and data[i][5] == 'F'},
        "<=", 50.0, name="capacity_RF",
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
        "when_cmpd_and_agg", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.when_compound
@pytest.mark.when_constraint
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_when_compound_or_aggregate(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """OR compound: SUM(x*qty) <= 100 WHEN (returnflag='R' OR returnflag='A')."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity,
               l_returnflag, x
        FROM lineitem
        WHERE l_orderkey < 100
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * l_quantity) <= 100 WHEN (l_returnflag = 'R' OR l_returnflag = 'A')
        MAXIMIZE SUM(x * l_extendedprice)
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
    oracle_solver.create_model("when_cmpd_or_agg")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    # OR: either condition suffices
    oracle_solver.add_constraint(
        {vnames[i]: data[i][3]
         for i in range(len(data))
         if data[i][4] in ('R', 'A')},
        "<=", 100.0, name="capacity_RA",
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
        "when_cmpd_or_agg", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.when_compound
@pytest.mark.when_perrow
@pytest.mark.var_boolean
@pytest.mark.cons_perrow
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_when_compound_and_perrow(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """AND compound on per-row: x <= 0 WHEN (returnflag='N' AND quantity > 30)."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity,
               l_returnflag, x
        FROM lineitem
        WHERE l_orderkey < 100
        DECIDE x IS BOOLEAN
        SUCH THAT x <= 0 WHEN (l_returnflag = 'N' AND l_quantity > 30)
            AND SUM(x * l_quantity) <= 100
        MAXIMIZE SUM(x * l_extendedprice)
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
    oracle_solver.create_model("when_cmpd_and_perrow")
    vnames = [f"x_{i}" for i in range(len(data))]
    for i, row in enumerate(data):
        if row[4] == 'N' and row[3] > 30:
            oracle_solver.add_variable(vnames[i], VarType.BINARY, lb=0.0, ub=0.0)
        else:
            oracle_solver.add_variable(vnames[i], VarType.BINARY)

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
        "when_cmpd_and_perrow", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.when_compound
@pytest.mark.when_objective
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_when_compound_and_objective(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """AND compound on objective: MAXIMIZE WHEN (returnflag='R' AND discount >= 0.06)."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity,
               l_returnflag, l_discount, x
        FROM lineitem
        WHERE l_orderkey < 100
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * l_quantity) <= 100
        MAXIMIZE SUM(x * l_extendedprice) WHEN (l_returnflag = 'R' AND l_discount >= 0.06)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_extendedprice AS DOUBLE),
               CAST(l_quantity AS DOUBLE),
               l_returnflag,
               CAST(l_discount AS DOUBLE)
        FROM lineitem WHERE l_orderkey < 100
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("when_cmpd_and_obj")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    oracle_solver.add_constraint(
        {vnames[i]: data[i][3] for i in range(len(data))},
        "<=", 100.0, name="capacity",
    )
    # AND compound on objective
    oracle_solver.set_objective(
        {vnames[i]: data[i][2]
         for i in range(len(data))
         if data[i][4] == 'R' and data[i][5] >= 0.06},
        ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    price_idx = decidb_cols.index("l_extendedprice")
    flag_idx = decidb_cols.index("l_returnflag")
    disc_idx = decidb_cols.index("l_discount")
    cmp = compare_solutions(
        decidb_result, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {
            "x": float(row[price_idx])
            if row[flag_idx] == 'R' and float(row[disc_idx]) >= 0.06
            else 0.0,
        },
    )

    perf_tracker.record(
        "when_cmpd_and_obj", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.when_compound
@pytest.mark.when_constraint
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_when_compound_mixed_types(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Mixed string + numeric compound: WHEN (returnflag='A' AND quantity <= 25)."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity,
               l_returnflag, x
        FROM lineitem
        WHERE l_orderkey < 100
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * l_quantity) <= 80 WHEN (l_returnflag = 'A' AND l_quantity <= 25)
        MAXIMIZE SUM(x * l_extendedprice)
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
    oracle_solver.create_model("when_cmpd_mixed")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    # Mixed: string equality + numeric inequality
    oracle_solver.add_constraint(
        {vnames[i]: data[i][3]
         for i in range(len(data))
         if data[i][4] == 'A' and data[i][3] <= 25},
        "<=", 80.0, name="capacity_A_small",
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
        "when_cmpd_mixed", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.when_compound
@pytest.mark.when_perrow
@pytest.mark.var_boolean
@pytest.mark.cons_perrow
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_when_compound_or_perrow(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """OR compound on per-row: x = 1 WHEN (discount >= 0.09 OR quantity < 3)."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity,
               l_discount, x
        FROM lineitem
        WHERE l_orderkey < 100
        DECIDE x IS BOOLEAN
        SUCH THAT x = 1 WHEN (l_discount >= 0.09 OR l_quantity < 3)
            AND SUM(x * l_quantity) <= 5000
        MAXIMIZE SUM(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_extendedprice AS DOUBLE),
               CAST(l_quantity AS DOUBLE),
               CAST(l_discount AS DOUBLE)
        FROM lineitem WHERE l_orderkey < 100
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("when_cmpd_or_perrow")
    vnames = [f"x_{i}" for i in range(len(data))]
    for i, row in enumerate(data):
        if row[4] >= 0.09 or row[3] < 3:
            oracle_solver.add_variable(vnames[i], VarType.BINARY, lb=1.0, ub=1.0)
        else:
            oracle_solver.add_variable(vnames[i], VarType.BINARY)

    oracle_solver.add_constraint(
        {vnames[i]: data[i][3] for i in range(len(data))},
        "<=", 5000.0, name="capacity",
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
        "when_cmpd_or_perrow", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )
