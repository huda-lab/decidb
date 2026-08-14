"""Tests for WHEN clause on aggregate constraints.

Covers:
  - String equality filter on aggregate constraint
  - Multiple WHEN constraints with different categories
  - Numeric comparison in WHEN condition
  - Boundary: all rows match (equivalent to no WHEN)
  - Boundary: no rows match (constraint is trivially satisfied)
  - Mixed conditional + unconditional constraints
  - Constant coefficient in SUM with WHEN
"""

import time

import pytest

from solver.types import VarType, ObjSense
from comparison.compare import compare_solutions


@pytest.mark.when_constraint
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_when_aggregate_string_equality(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Capacity limit on returned items only: SUM(x*qty) <= 100 WHEN returnflag='R'."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity,
               l_returnflag, x
        FROM lineitem
        WHERE l_orderkey < 100
        DECIDE x(BOOL)
        SUCH THAT SUM(x * l_quantity) <= 100 WHEN l_returnflag = 'R'
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
    oracle_solver.create_model("when_agg_str_eq")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    # WHEN filter: only 'R' rows contribute to the capacity constraint
    oracle_solver.add_constraint(
        {vnames[i]: data[i][3] for i in range(len(data)) if data[i][4] == 'R'},
        "<=", 100.0, name="capacity_R",
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
        "when_agg_str_eq", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.when_constraint
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.cons_multi
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_when_multiple_categories(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Different capacity limits per category using multiple WHEN constraints."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity,
               l_returnflag, x
        FROM lineitem
        WHERE l_orderkey < 100
        DECIDE x(BOOL)
        SUCH THAT SUM(x * l_quantity) <= 50 WHEN l_returnflag = 'R'
            AND SUM(x * l_quantity) <= 80 WHEN l_returnflag = 'A'
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
    oracle_solver.create_model("when_multi_cat")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    # Two separate WHEN-filtered constraints
    oracle_solver.add_constraint(
        {vnames[i]: data[i][3] for i in range(len(data)) if data[i][4] == 'R'},
        "<=", 50.0, name="capacity_R",
    )
    oracle_solver.add_constraint(
        {vnames[i]: data[i][3] for i in range(len(data)) if data[i][4] == 'A'},
        "<=", 80.0, name="capacity_A",
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
        "when_multi_cat", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 2,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.when_constraint
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_when_aggregate_numeric_comparison(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Capacity limit on discounted items: SUM(x*price) <= 5000 WHEN discount >= 0.06."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity,
               l_discount, x
        FROM lineitem
        WHERE l_orderkey < 100
        DECIDE x(BOOL)
        SUCH THAT SUM(x * l_extendedprice) <= 5000 WHEN l_discount >= 0.06
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
    oracle_solver.create_model("when_agg_numeric")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    # WHEN filter: only rows with discount >= 0.06
    oracle_solver.add_constraint(
        {vnames[i]: data[i][2] for i in range(len(data)) if data[i][4] >= 0.06},
        "<=", 5000.0, name="price_cap_discounted",
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
        "when_agg_numeric", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.when_constraint
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_when_all_rows_match(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """WHEN condition matches all rows — equivalent to no WHEN."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity, x
        FROM lineitem
        WHERE l_orderkey < 100
        DECIDE x(BOOL)
        SUCH THAT SUM(x * l_quantity) <= 100 WHEN l_quantity > 0
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
    oracle_solver.create_model("when_all_match")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    # All l_quantity > 0 in TPC-H, so this includes all rows
    oracle_solver.add_constraint(
        {vnames[i]: data[i][3] for i in range(len(data)) if data[i][3] > 0},
        "<=", 100.0, name="capacity_all",
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
        "when_all_match", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.when_constraint
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.error_infeasible
def test_when_no_rows_match(decidb_cli):
    """Aggregate constraint with WHEN matching no rows — now rejected pre-solver
    per the "reject all empty aggregate sets" rule."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity,
               l_returnflag, x
        FROM lineitem
        WHERE l_orderkey < 100
        DECIDE x(BOOL)
        SUCH THAT SUM(x * l_quantity) <= 100 WHEN l_returnflag = 'Z'
        MAXIMIZE SUM(x * l_extendedprice)
    """
    decidb_cli.assert_error(sql, match=r"empty|WHEN")


@pytest.mark.when_constraint
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.cons_multi
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_when_mixed_conditional_and_unconditional(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """One WHEN-filtered constraint + one unconditional constraint."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity,
               l_returnflag, x
        FROM lineitem
        WHERE l_orderkey < 100
        DECIDE x(BOOL)
        SUCH THAT SUM(x * l_quantity) <= 50 WHEN l_returnflag = 'R'
            AND SUM(x) <= 20
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
    oracle_solver.create_model("when_mixed")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    # Conditional: capacity on R rows only
    oracle_solver.add_constraint(
        {vnames[i]: data[i][3] for i in range(len(data)) if data[i][4] == 'R'},
        "<=", 50.0, name="capacity_R",
    )
    # Unconditional: at most 20 items total
    oracle_solver.add_constraint(
        {vnames[i]: 1.0 for i in range(len(data))},
        "<=", 20.0, name="count_limit",
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
        "when_mixed", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 2,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.when_constraint
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_when_aggregate_constant_coeff(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Constant coefficient in SUM with WHEN: SUM(x*10) <= 200 WHEN mktsegment='AUTOMOBILE'."""
    sql = """
        SELECT c_custkey, c_acctbal, c_mktsegment, x
        FROM customer
        WHERE c_nationkey = 1
        DECIDE x(BOOL)
        SUCH THAT SUM(x * 10) <= 200 WHEN c_mktsegment = 'AUTOMOBILE'
        MAXIMIZE SUM(x * c_acctbal)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(c_custkey AS BIGINT),
               CAST(c_acctbal AS DOUBLE),
               c_mktsegment
        FROM customer WHERE c_nationkey = 1
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("when_const_coeff")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    # Constant coeff=10 only for AUTOMOBILE customers
    oracle_solver.add_constraint(
        {vnames[i]: 10.0 for i in range(len(data)) if data[i][2] == 'AUTOMOBILE'},
        "<=", 200.0, name="budget_auto",
    )
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
        "when_const_coeff", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.when_constraint
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_when_not_equal(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """WHEN with not-equal operator: SUM(x*qty) <= 80 WHEN returnflag <> 'N'."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity,
               l_returnflag, x
        FROM lineitem
        WHERE l_orderkey < 100
        DECIDE x(BOOL)
        SUCH THAT SUM(x * l_quantity) <= 80 WHEN l_returnflag <> 'N'
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
    oracle_solver.create_model("when_not_eq")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    oracle_solver.add_constraint(
        {vnames[i]: data[i][3] for i in range(len(data)) if data[i][4] != 'N'},
        "<=", 80.0, name="capacity_not_N",
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
        "when_not_eq", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.when_constraint
@pytest.mark.cons_multi
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_when_constraint_ordering_invariance(decidb_cli):
    """WHEN must apply only to its constraint regardless of AND ordering.

    Regression test: `DECIDE_ITEM` precedence must keep "A AND B WHEN C" as
    "A AND (B WHEN C)" so constraint ordering cannot change semantics.
    """
    # Order 1: unconditional BEFORE WHEN
    sql_before = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity,
               l_returnflag, x
        FROM lineitem
        WHERE l_orderkey < 100
        DECIDE x(BOOL)
        SUCH THAT SUM(x) <= 20
            AND SUM(x * l_quantity) <= 50 WHEN l_returnflag = 'R'
        MAXIMIZE SUM(x * l_extendedprice)
    """
    # Order 2: unconditional AFTER WHEN
    sql_after = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity,
               l_returnflag, x
        FROM lineitem
        WHERE l_orderkey < 100
        DECIDE x(BOOL)
        SUCH THAT SUM(x * l_quantity) <= 50 WHEN l_returnflag = 'R'
            AND SUM(x) <= 20
        MAXIMIZE SUM(x * l_extendedprice)
    """
    result_before, cols_before = decidb_cli.execute(sql_before)
    result_after, cols_after = decidb_cli.execute(sql_after)

    assert cols_before == cols_after, "Column names should match"
    assert len(result_before) == len(result_after), "Row counts should match"

    # Sort by non-decision columns for deterministic comparison
    key_indices = [i for i, c in enumerate(cols_before) if c != "x"]
    sort_key = lambda row: tuple(row[i] for i in key_indices)

    sorted_before = sorted(result_before, key=sort_key)
    sorted_after = sorted(result_after, key=sort_key)

    for rb, ra in zip(sorted_before, sorted_after):
        for i in range(len(rb)):
            try:
                assert abs(float(rb[i]) - float(ra[i])) < 1e-6, (
                    f"Ordering changed results: before={rb} vs after={ra}"
                )
            except (ValueError, TypeError):
                assert rb[i] == ra[i], (
                    f"Ordering changed results: before={rb} vs after={ra}"
                )


@pytest.mark.when_constraint
@pytest.mark.edge_case
def test_when_null_condition_column(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """WHEN condition on column with NULL values — NULLs treated as false.

    Rows where the WHEN condition evaluates to NULL should be excluded from
    the constraint (same as non-matching rows).
    """
    sql = """
        WITH data AS (
            SELECT 1 AS id, 10.0 AS val, 'R' AS flag UNION ALL
            SELECT 2, 5.0, NULL UNION ALL
            SELECT 3, 8.0, 'N' UNION ALL
            SELECT 4, 15.0, 'R' UNION ALL
            SELECT 5, 20.0, NULL
        )
        SELECT id, val, flag, x
        FROM data
        DECIDE x(BOOL)
        SUCH THAT SUM(x * val) <= 20 WHEN flag = 'R'
        MAXIMIZE SUM(x * val)
    """
    result, cols = decidb_cli.execute(sql)
    assert len(result) > 0

    id_idx = cols.index("id")
    val_idx = cols.index("val")
    flag_idx = cols.index("flag")
    x_idx = cols.index("x")

    # Verify WHEN constraint: only flag='R' rows contribute
    when_sum = 0.0
    for row in result:
        flag = row[flag_idx]
        if flag == 'R':
            when_sum += int(row[x_idx]) * float(row[val_idx])

    assert when_sum <= 20 + 1e-4, \
        f"WHEN constraint violated: SUM(x*val) WHEN flag='R' = {when_sum} > 20"

    # Verify NULL rows still appear in result (not dropped entirely)
    null_rows = [row for row in result if row[flag_idx] is None]
    assert len(null_rows) == 2, \
        f"Expected 2 NULL-flag rows in result, got {len(null_rows)}"


@pytest.mark.when_constraint
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_when_is_not_null_predicate(decidb_cli, oracle_solver, perf_tracker):
    """WHEN (col IS NOT NULL) — explicit IS NOT NULL predicate in WHEN.

    Companion to ``test_when_null_condition_column`` (which exercises NULL
    *values* in an equality predicate). Here the WHEN condition itself is
    the IS NOT NULL predicate. The parentheses around the predicate are
    required: ``WHEN col IS NOT NULL AND ...`` produces a parser error
    because ``NULL`` + ``AND`` is ambiguous in the DECIDE SUCH THAT grammar.
    """
    sql = """
        SELECT id, val, note, x FROM (
            VALUES (1, 10.0, 'a'), (2, 5.0, NULL),
                   (3, 8.0, 'c'), (4, 15.0, NULL)
        ) t(id, val, note)
        DECIDE x(BOOL)
        SUCH THAT SUM(x * val) <= 15 WHEN (note IS NOT NULL)
            AND SUM(x) <= 3
        MAXIMIZE SUM(x * val)
    """
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = [
        (1, 10.0, 'a'),
        (2, 5.0, None),
        (3, 8.0, 'c'),
        (4, 15.0, None),
    ]
    n = len(data)

    t_build = time.perf_counter()
    oracle_solver.create_model("when_is_not_null")
    vnames = [f"x_{i}" for i in range(n)]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    # WHEN (note IS NOT NULL): rows 1, 3 contribute to the aggregate.
    not_null_coeffs = {
        vnames[i]: data[i][1] for i in range(n) if data[i][2] is not None
    }
    oracle_solver.add_constraint(
        not_null_coeffs, "<=", 15.0, name="when_not_null_budget",
    )
    # Unconditional cap on total selections.
    oracle_solver.add_constraint(
        {vn: 1.0 for vn in vnames}, "<=", 3.0, name="sum_x_cap",
    )
    oracle_solver.set_objective(
        {vnames[i]: data[i][1] for i in range(n)}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    from solver.types import SolverStatus
    assert result.status == SolverStatus.OPTIMAL

    val_idx = decidb_cols.index("val")
    x_idx = decidb_cols.index("x")
    decidb_obj = sum(int(r[x_idx]) * float(r[val_idx]) for r in decidb_rows)
    assert abs(decidb_obj - result.objective_value) <= 1e-6, (
        f"Objective mismatch: DecidB={decidb_obj}, Oracle={result.objective_value}"
    )

    perf_tracker.record(
        "when_is_not_null_predicate", decidb_time, build_time,
        result.solve_time_seconds, n, n, 2,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )
