"""Tests for the PER keyword.

PER partitions constraints by group:
    SUM(x) <= 5 PER s_nationkey
means a separate SUM(x) <= 5 constraint for each distinct s_nationkey value.

Also covers:
  - PER with <> operator (Big-M disjunction per group)
  - PER with <> and a bound that itself varies within a group (C3): every
    excluded value is kept, not collapsed to one
  - NULL values in PER column (excluded from all groups)
  - Two PER constraints on different grouping columns
"""

import time

import pytest

from solver.types import VarType, ObjSense
from comparison.compare import compare_solutions
from ._oracle_helpers import group_indices as _group_indices, add_ne_bigm as _add_ne_bigm


@pytest.mark.per_clause
@pytest.mark.correctness
def test_per_basic(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """PER keyword should partition constraints by group."""
    sql = """
        SELECT s_suppkey, s_acctbal, s_nationkey, x FROM supplier
        DECIDE x(BOOL)
        SUCH THAT SUM(x) <= 5 PER s_nationkey
        MAXIMIZE SUM(x * s_acctbal)
    """
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(s_suppkey AS BIGINT),
               CAST(s_acctbal AS DOUBLE),
               CAST(s_nationkey AS BIGINT)
        FROM supplier
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("per_basic")
    n = len(data)
    for i in range(n):
        oracle_solver.add_variable(f"x_{i}", VarType.BINARY)
    for group_key, rows in _group_indices(data, lambda r: r[2]).items():
        oracle_solver.add_constraint(
            {f"x_{i}": 1.0 for i in rows},
            "<=", 5.0, name=f"per_{group_key}",
        )
    oracle_solver.set_objective(
        {f"x_{i}": data[i][1] for i in range(n)},
        ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_rows, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[decidb_cols.index("s_acctbal")])},
    )
    perf_tracker.record(
        "per_basic", decidb_time, build_time, result.solve_time_seconds,
        n, n, len(set(r[2] for r in data)),
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.per_clause
@pytest.mark.correctness
def test_per_with_integer_variable(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """PER with integer variables and a weighted constraint."""
    sql = """
        SELECT ps_partkey, ps_suppkey, ps_availqty, ps_supplycost, x
        FROM partsupp WHERE ps_partkey < 50
        DECIDE x(INT)
        SUCH THAT SUM(x * ps_supplycost) <= 1000 PER ps_partkey
        MAXIMIZE SUM(x * ps_availqty)
    """
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(ps_partkey AS BIGINT),
               CAST(ps_suppkey AS BIGINT),
               CAST(ps_availqty AS DOUBLE),
               CAST(ps_supplycost AS DOUBLE)
        FROM partsupp WHERE ps_partkey < 50
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("per_integer")
    n = len(data)
    for i in range(n):
        oracle_solver.add_variable(f"x_{i}", VarType.INTEGER, lb=0.0)
    for group_key, rows in _group_indices(data, lambda r: r[0]).items():
        oracle_solver.add_constraint(
            {f"x_{i}": data[i][3] for i in rows},
            "<=", 1000.0, name=f"per_{group_key}",
        )
    oracle_solver.set_objective(
        {f"x_{i}": data[i][2] for i in range(n)},
        ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_rows, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[decidb_cols.index("ps_availqty")])},
    )
    perf_tracker.record(
        "per_with_integer_variable", decidb_time, build_time,
        result.solve_time_seconds, n, n, len(set(r[0] for r in data)),
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.per_clause
@pytest.mark.when_constraint
@pytest.mark.correctness
def test_per_combined_with_when(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """PER and WHEN used together — group-level constraint with row filter."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity,
               l_returnflag, x
        FROM lineitem WHERE l_orderkey < 100
        DECIDE x(BOOL)
        SUCH THAT SUM(x * l_quantity) <= 50 PER l_returnflag
            AND SUM(x) <= 30
        MAXIMIZE SUM(x * l_extendedprice) WHEN l_returnflag = 'R'
    """
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_extendedprice AS DOUBLE),
               CAST(l_quantity AS DOUBLE),
               CAST(l_returnflag AS VARCHAR)
        FROM lineitem WHERE l_orderkey < 100
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("per_when")
    n = len(data)
    for i in range(n):
        oracle_solver.add_variable(f"x_{i}", VarType.BINARY)
    for group_key, rows in _group_indices(data, lambda r: r[4]).items():
        oracle_solver.add_constraint(
            {f"x_{i}": data[i][3] for i in rows},
            "<=", 50.0, name=f"per_{group_key}",
        )
    oracle_solver.add_constraint(
        {f"x_{i}": 1.0 for i in range(n)},
        "<=", 30.0, name="global_cap",
    )
    oracle_solver.set_objective(
        {
            f"x_{i}": data[i][2] if data[i][4] == "R" else 0.0
            for i in range(n)
        },
        ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_rows, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {
            "x": (
                float(row[decidb_cols.index("l_extendedprice")])
                if row[decidb_cols.index("l_returnflag")] == "R"
                else 0.0
            )
        },
    )
    perf_tracker.record(
        "per_combined_with_when", decidb_time, build_time,
        result.solve_time_seconds, n, n, len(set(r[4] for r in data)) + 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.per_clause
@pytest.mark.cons_comparison
@pytest.mark.correctness
def test_per_not_equal(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """PER with <> operator — Big-M disjunction generated per group."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_returnflag, l_extendedprice, x
        FROM lineitem WHERE l_orderkey < 50
        DECIDE x(BOOL)
        SUCH THAT SUM(x) <> 3 PER l_returnflag
            AND SUM(x) <= 15
        MAXIMIZE SUM(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_returnflag AS VARCHAR),
               CAST(l_extendedprice AS DOUBLE)
        FROM lineitem WHERE l_orderkey < 50
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("per_ne")
    n = len(data)
    for i in range(n):
        oracle_solver.add_variable(f"x_{i}", VarType.BINARY)
    for group_key, rows in _group_indices(data, lambda r: r[2]).items():
        _add_ne_bigm(
            oracle_solver,
            {f"x_{i}": 1.0 for i in rows},
            rhs=3.0, row_count=len(rows), name=f"ne_{group_key}",
        )
    oracle_solver.add_constraint(
        {f"x_{i}": 1.0 for i in range(n)},
        "<=", 15.0, name="global_cap",
    )
    oracle_solver.set_objective(
        {f"x_{i}": data[i][3] for i in range(n)},
        ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_rows, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[decidb_cols.index("l_extendedprice")])},
    )
    perf_tracker.record(
        "per_not_equal", decidb_time, build_time, result.solve_time_seconds,
        n, n, len(set(r[2] for r in data)) + 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.per_clause
@pytest.mark.cons_comparison
@pytest.mark.correctness
def test_per_not_equal_varying_bound_keeps_every_exclusion(decidb_cli, oracle_solver):
    """C3: a `<>` bound that varies within a PER group excludes every value it takes,
    not just the tightest one (standing decision 3).

    Group 'a' has two rows whose `cap` column reads 3 and 7, so the group's own bound
    excludes both: `SUM(x) <> 3 AND SUM(x) <> 7`. Bounding the sum's own range to
    [3, 7] and maximizing isolates the discriminating case — an optimum that sits
    *between* the two excluded values. A wrong reading that collapsed the varying
    bound to its tightest value (as `<=`/`>=` do) would keep only `<> 3` and answer 7;
    keeping both gives 6, the next value down from the excluded ceiling.
    """
    rows, cols = decidb_cli.execute("""
        SELECT id, grp, x FROM (
            VALUES (1, 'a', 3), (2, 'a', 7)
        ) t(id, grp, cap)
        DECIDE x(INT)
        SUCH THAT x BETWEEN 0 AND 10
            AND SUM(x) >= 3 AND SUM(x) <= 7
            AND SUM(x) <> cap PER grp
        MAXIMIZE SUM(x)
    """)
    xi = cols.index("x")
    actual_sum = sum(int(r[xi]) for r in rows)

    oracle_solver.create_model("per_not_equal_varying_bound")
    oracle_solver.add_variable("x_1", VarType.INTEGER, lb=0.0, ub=10.0)
    oracle_solver.add_variable("x_2", VarType.INTEGER, lb=0.0, ub=10.0)
    coeffs = {"x_1": 1.0, "x_2": 1.0}
    oracle_solver.add_constraint(coeffs, ">=", 3.0, name="range_lo")
    oracle_solver.add_constraint(coeffs, "<=", 7.0, name="range_hi")
    _add_ne_bigm(oracle_solver, coeffs, rhs=3.0, row_count=2, name="ne_a_3")
    _add_ne_bigm(oracle_solver, coeffs, rhs=7.0, row_count=2, name="ne_a_7")
    oracle_solver.set_objective(coeffs, ObjSense.MAXIMIZE)
    result = oracle_solver.solve()

    assert actual_sum == pytest.approx(result.objective_value)
    assert actual_sum == 6, f"expected the optimum strictly between 3 and 7, got {actual_sum}"


@pytest.mark.per_clause
@pytest.mark.edge_case
@pytest.mark.correctness
def test_per_null_group_key(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """NULL values in PER column should be excluded from all groups."""
    data_sql = """
        SELECT 1 AS id, 'A' AS grp, 10.0 AS val UNION ALL
        SELECT 2, NULL, 50.0 UNION ALL
        SELECT 3, 'B', 8.0 UNION ALL
        SELECT 4, NULL, 30.0 UNION ALL
        SELECT 5, 'A', 12.0
    """
    sql = f"""
        WITH data AS ({data_sql})
        SELECT id, grp, val, x
        FROM data
        DECIDE x(BOOL)
        SUCH THAT SUM(x) <= 1 PER grp
        MAXIMIZE SUM(x * val)
    """
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute(
        f"SELECT CAST(id AS BIGINT), CAST(grp AS VARCHAR), CAST(val AS DOUBLE) FROM ({data_sql})"
    ).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("per_null_group")
    n = len(data)
    for i in range(n):
        oracle_solver.add_variable(f"x_{i}", VarType.BINARY)
    for group_key, rows in _group_indices(data, lambda r: r[1]).items():
        oracle_solver.add_constraint(
            {f"x_{i}": 1.0 for i in rows},
            "<=", 1.0, name=f"per_{group_key}",
        )
    oracle_solver.set_objective(
        {f"x_{i}": data[i][2] for i in range(n)},
        ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_rows, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[decidb_cols.index("val")])},
    )
    perf_tracker.record(
        "per_null_group_key", decidb_time, build_time, result.solve_time_seconds,
        n, n, 2,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.per_clause
@pytest.mark.correctness
def test_per_different_grouping_columns(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """Two PER constraints on different columns — overlapping group structures."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_returnflag, l_linestatus,
               l_extendedprice, x
        FROM lineitem WHERE l_orderkey < 100
        DECIDE x(BOOL)
        SUCH THAT SUM(x) <= 5 PER l_returnflag
            AND SUM(x) <= 8 PER l_linestatus
        MAXIMIZE SUM(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_returnflag AS VARCHAR),
               CAST(l_linestatus AS VARCHAR),
               CAST(l_extendedprice AS DOUBLE)
        FROM lineitem WHERE l_orderkey < 100
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("per_two_groupings")
    n = len(data)
    for i in range(n):
        oracle_solver.add_variable(f"x_{i}", VarType.BINARY)
    for group_key, rows in _group_indices(data, lambda r: r[2]).items():
        oracle_solver.add_constraint(
            {f"x_{i}": 1.0 for i in rows},
            "<=", 5.0, name=f"per_rflag_{group_key}",
        )
    for group_key, rows in _group_indices(data, lambda r: r[3]).items():
        oracle_solver.add_constraint(
            {f"x_{i}": 1.0 for i in rows},
            "<=", 8.0, name=f"per_lstatus_{group_key}",
        )
    oracle_solver.set_objective(
        {f"x_{i}": data[i][4] for i in range(n)},
        ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_rows, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[decidb_cols.index("l_extendedprice")])},
    )
    perf_tracker.record(
        "per_different_grouping_columns", decidb_time, build_time,
        result.solve_time_seconds, n, n,
        len(set(r[2] for r in data)) + len(set(r[3] for r in data)),
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.per_clause
@pytest.mark.var_real
@pytest.mark.cons_between
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_real_between_per_oracle(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """PER grouping combined with BETWEEN on a REAL variable.

    Regression test for the integer-step rewrite sweep. The PER rewrite
    iterates group-ids but introduces no integer-step discretization of its
    own; BETWEEN desugars to `>=`/`<=`. Together, PER + BETWEEN on a REAL
    variable with fractional bounds must produce the correct per-group
    knapsack-style optimum.
    """
    sql = """
        SELECT l_orderkey, l_linestatus, l_extendedprice, x
        FROM lineitem WHERE l_orderkey < 25
        DECIDE x(REAL)
        SUCH THAT x BETWEEN 0.1 AND 2.9
            AND SUM(x) <= 10 PER l_linestatus
        MAXIMIZE SUM(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               l_linestatus,
               CAST(l_extendedprice AS DOUBLE)
        FROM lineitem WHERE l_orderkey < 25
    """).fetchall()
    n = len(data)

    t_build = time.perf_counter()
    oracle_solver.create_model("real_between_per")
    vnames = [f"x_{i}" for i in range(n)]
    for v in vnames:
        oracle_solver.add_variable(v, VarType.CONTINUOUS, lb=0.1, ub=2.9)
    groups = _group_indices(data, lambda r: r[1])
    for group_key, rows in groups.items():
        oracle_solver.add_constraint(
            {vnames[i]: 1.0 for i in rows},
            "<=", 10.0, name=f"per_{group_key}",
        )
    oracle_solver.set_objective(
        {vnames[i]: data[i][2] for i in range(n)}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_rows, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[decidb_cols.index("l_extendedprice")])},
    )
    perf_tracker.record(
        "real_between_per_oracle", decidb_time, build_time,
        result.solve_time_seconds, n, n, len(groups),
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )
