"""Tests for WHEN clause on per-row constraints.

Covers:
  - Force zero: x <= 0 WHEN condition (block selection of matching rows)
  - Force select: x = 1 WHEN condition (require matching rows)
  - Numeric condition on per-row bound
  - Boundary: no rows match (per-row constraint is inert)
  - Boundary: all rows match (equivalent to unconditional per-row)
  - test_when_perrow_real: REAL variable (no implicit [0,1] cap) + WHEN
    per-row — continuous skip-constraint path
"""

import time

import pytest

from solver.types import VarType, ObjSense
from comparison.compare import compare_solutions


@pytest.mark.when_perrow
@pytest.mark.var_boolean
@pytest.mark.cons_perrow
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_when_perrow_force_zero(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Block non-returned items: x <= 0 WHEN returnflag='N', plus capacity."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity,
               l_returnflag, x
        FROM lineitem
        WHERE l_orderkey < 100
        DECIDE x IS BOOLEAN
        SUCH THAT x <= 0 WHEN l_returnflag = 'N'
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
    oracle_solver.create_model("when_perrow_zero")
    vnames = [f"x_{i}" for i in range(len(data))]
    for i, row in enumerate(data):
        if row[4] == 'N':
            # WHEN matches: x <= 0 → fix at 0
            oracle_solver.add_variable(vnames[i], VarType.BINARY, lb=0.0, ub=0.0)
        else:
            oracle_solver.add_variable(vnames[i], VarType.BINARY)

    # Aggregate constraint applies to all rows
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
        "when_perrow_zero", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.when_perrow
@pytest.mark.var_boolean
@pytest.mark.cons_perrow
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_when_perrow_force_select(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Force selection of high-discount items: x = 1 WHEN discount >= 0.09."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity,
               l_discount, x
        FROM lineitem
        WHERE l_orderkey < 100
        DECIDE x IS BOOLEAN
        SUCH THAT x = 1 WHEN l_discount >= 0.09
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
    oracle_solver.create_model("when_perrow_select")
    vnames = [f"x_{i}" for i in range(len(data))]
    for i, row in enumerate(data):
        if row[4] >= 0.09:
            # WHEN matches: x = 1 → fix at 1
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
        "when_perrow_select", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.when_perrow
@pytest.mark.var_boolean
@pytest.mark.cons_perrow
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_when_perrow_numeric_condition(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Exclude large-quantity items: x <= 0 WHEN quantity > 40."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity, x
        FROM lineitem
        WHERE l_orderkey < 100
        DECIDE x IS BOOLEAN
        SUCH THAT x <= 0 WHEN l_quantity > 40
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
               CAST(l_quantity AS DOUBLE)
        FROM lineitem WHERE l_orderkey < 100
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("when_perrow_numeric")
    vnames = [f"x_{i}" for i in range(len(data))]
    for i, row in enumerate(data):
        if row[3] > 40:
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
        "when_perrow_numeric", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.when_perrow
@pytest.mark.var_boolean
@pytest.mark.cons_perrow
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_when_perrow_no_matches(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """WHEN matches nothing — per-row constraint is inert, only aggregate limits."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity,
               l_returnflag, x
        FROM lineitem
        WHERE l_orderkey < 100
        DECIDE x IS BOOLEAN
        SUCH THAT x <= 0 WHEN l_returnflag = 'Z'
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
    oracle_solver.create_model("when_perrow_none")
    vnames = [f"x_{i}" for i in range(len(data))]
    # No rows match 'Z', so all variables get default bounds
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

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
        "when_perrow_none", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.when_perrow
@pytest.mark.var_integer
@pytest.mark.cons_perrow
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_when_perrow_all_match(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """WHEN matches all rows — equivalent to unconditional per-row bound."""
    sql = """
        SELECT ps_partkey, ps_availqty, x
        FROM partsupp
        WHERE ps_partkey < 20
        DECIDE x IS INTEGER
        SUCH THAT x <= 3 WHEN ps_availqty > 0
            AND SUM(x) <= 50
        MAXIMIZE SUM(x * ps_availqty)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(ps_partkey AS BIGINT),
               CAST(ps_availqty AS DOUBLE)
        FROM partsupp WHERE ps_partkey < 20
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("when_perrow_all")
    vnames = [f"x_{i}" for i in range(len(data))]
    for i, row in enumerate(data):
        # All ps_availqty > 0 in TPC-H → all match → x <= 3 for every row
        if row[1] > 0:
            oracle_solver.add_variable(vnames[i], VarType.INTEGER, lb=0.0, ub=3.0)
        else:
            oracle_solver.add_variable(vnames[i], VarType.INTEGER)

    oracle_solver.add_constraint(
        {vnames[i]: 1.0 for i in range(len(data))},
        "<=", 50.0, name="total_limit",
    )
    oracle_solver.set_objective(
        {vnames[i]: data[i][1] for i in range(len(data))},
        ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_result, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[decidb_cols.index("ps_availqty")])},
    )

    perf_tracker.record(
        "when_perrow_all", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.when_perrow
@pytest.mark.var_real
@pytest.mark.cons_perrow
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_when_perrow_real(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Per-row WHEN bound with IS REAL — continuous skip-constraint path.

    All existing per-row WHEN tests use BOOLEAN or INTEGER variables, both
    of which have implicit or defaulted upper bounds. REAL variables have
    no implicit cap, so the code path that decides "WHEN false → skip the
    per-row constraint" for continuous variables is untested. If the
    skip-path left a stale or default bound in place on non-matching rows,
    the aggregate cap could no longer pick a high-priced uncapped row to
    concentrate mass on.

    The test caps 'N' rows at x <= 10 via WHEN while leaving non-'N' rows
    uncapped per-row. The aggregate ``SUM(x) <= 30`` is the only cap on
    non-'N' rows, so the optimum loads 30 units onto the highest-priced
    non-'N' row.
    """
    sql = """
        SELECT l_orderkey, l_linenumber, l_returnflag, l_extendedprice,
               ROUND(x, 6) AS x
        FROM lineitem
        WHERE l_orderkey <= 5
        DECIDE x IS REAL
        SUCH THAT x <= 10 WHEN l_returnflag = 'N'
            AND SUM(x) <= 30
        MAXIMIZE SUM(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               l_returnflag,
               CAST(l_extendedprice AS DOUBLE)
        FROM lineitem WHERE l_orderkey <= 5
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("when_perrow_real")
    vnames = [f"x_{i}" for i in range(len(data))]
    for i, row in enumerate(data):
        if row[2] == 'N':
            oracle_solver.add_variable(vnames[i], VarType.CONTINUOUS, lb=0.0, ub=10.0)
        else:
            # Non-matching rows inherit only the aggregate cap — no per-row ub.
            oracle_solver.add_variable(vnames[i], VarType.CONTINUOUS, lb=0.0)

    oracle_solver.add_constraint(
        {vnames[i]: 1.0 for i in range(len(data))},
        "<=", 30.0, name="aggregate_cap",
    )
    oracle_solver.set_objective(
        {vnames[i]: data[i][3] for i in range(len(data))},
        ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    price_idx = decidb_cols.index("l_extendedprice")
    cmp = compare_solutions(
        decidb_result, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[price_idx])},
    )

    perf_tracker.record(
        "when_perrow_real", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )
