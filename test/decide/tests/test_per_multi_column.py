"""Tests for multi-column PER.

PER (col1, col2) partitions constraints by composite groups:
    SUM(x) <= 5 PER (l_returnflag, l_linestatus)
emits one ``SUM(x) <= 5`` constraint per distinct composite tuple. The oracle
mirrors that by grouping the fetched rows in Python and emitting one
``add_constraint`` call per non-empty group.
"""

import time

import pytest

from solver.types import VarType, ObjSense
from comparison.compare import compare_solutions
from ._oracle_helpers import group_indices


def _fetch_lineitem(duckdb_conn, where, cols):
    return duckdb_conn.execute(f"""
        SELECT {cols}
        FROM lineitem WHERE {where}
    """).fetchall()


@pytest.mark.per_clause
@pytest.mark.correctness
def test_multi_column_per_basic(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    sql = """
        SELECT l_orderkey, l_linenumber, l_returnflag, l_linestatus,
               l_extendedprice, x
        FROM lineitem WHERE l_orderkey < 100
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) <= 3 PER (l_returnflag, l_linestatus)
        MAXIMIZE SUM(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = _fetch_lineitem(
        duckdb_conn, "l_orderkey < 100",
        "CAST(l_orderkey AS BIGINT), CAST(l_linenumber AS BIGINT), "
        "CAST(l_returnflag AS VARCHAR), CAST(l_linestatus AS VARCHAR), "
        "CAST(l_extendedprice AS DOUBLE)",
    )
    n = len(data)

    t_build = time.perf_counter()
    oracle_solver.create_model("per_multi_col_basic")
    for i in range(n):
        oracle_solver.add_variable(f"x_{i}", VarType.BINARY)
    for key, idxs in group_indices(data, lambda r: (r[2], r[3])).items():
        oracle_solver.add_constraint(
            {f"x_{i}": 1.0 for i in idxs}, "<=", 3.0,
            name=f"per_{key[0]}_{key[1]}",
        )
    oracle_solver.set_objective(
        {f"x_{i}": data[i][4] for i in range(n)}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_rows, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[decidb_cols.index("l_extendedprice")])},
    )
    perf_tracker.record(
        "multi_column_per_basic", decidb_time, build_time, result.solve_time_seconds,
        n, n, len(set((r[2], r[3]) for r in data)),
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.per_clause
@pytest.mark.correctness
def test_multi_column_per_single_column_in_parens(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """PER (col) with parens should be equivalent to PER col at the objective level."""
    sql_parens = """
        SELECT s_suppkey, s_nationkey, s_acctbal, x FROM supplier
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) <= 5 PER (s_nationkey)
        MAXIMIZE SUM(x * s_acctbal)
    """
    sql_no_parens = """
        SELECT s_suppkey, s_nationkey, s_acctbal, x FROM supplier
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) <= 5 PER s_nationkey
        MAXIMIZE SUM(x * s_acctbal)
    """
    t0 = time.perf_counter()
    decidb_rows_parens, cols_parens = decidb_cli.execute(sql_parens)
    decidb_rows_no_parens, _ = decidb_cli.execute(sql_no_parens)
    decidb_time = time.perf_counter() - t0

    x_idx = cols_parens.index("x")
    sum_parens = sum(int(r[x_idx]) for r in decidb_rows_parens)
    sum_no_parens = sum(int(r[x_idx]) for r in decidb_rows_no_parens)
    assert sum_parens == sum_no_parens, (
        f"PER (col) and PER col differ on count: {sum_parens} vs {sum_no_parens}"
    )

    data = duckdb_conn.execute("""
        SELECT CAST(s_suppkey AS BIGINT),
               CAST(s_nationkey AS BIGINT),
               CAST(s_acctbal AS DOUBLE)
        FROM supplier
    """).fetchall()
    n = len(data)

    t_build = time.perf_counter()
    oracle_solver.create_model("per_paren_vs_bare")
    for i in range(n):
        oracle_solver.add_variable(f"x_{i}", VarType.BINARY)
    for key, idxs in group_indices(data, lambda r: r[1]).items():
        oracle_solver.add_constraint(
            {f"x_{i}": 1.0 for i in idxs}, "<=", 5.0, name=f"per_{key}",
        )
    oracle_solver.set_objective(
        {f"x_{i}": data[i][2] for i in range(n)}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_rows_parens, cols_parens, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[cols_parens.index("s_acctbal")])},
    )
    perf_tracker.record(
        "multi_column_per_single_column_in_parens", decidb_time, build_time,
        result.solve_time_seconds, n, n, len(set(r[1] for r in data)),
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.per_clause
@pytest.mark.when_constraint
@pytest.mark.correctness
def test_multi_column_per_with_when_on_different_column(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """WHEN filters on a column NOT in the PER tuple — only qualifying rows
    feed the grouped constraint."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_returnflag, l_linestatus,
               l_extendedprice, l_quantity, l_discount, x
        FROM lineitem WHERE l_orderkey < 200
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * l_quantity) <= 20 WHEN l_discount > 0.05 PER (l_returnflag, l_linestatus)
            AND SUM(x) <= 30
        MAXIMIZE SUM(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT), CAST(l_linenumber AS BIGINT),
               CAST(l_returnflag AS VARCHAR), CAST(l_linestatus AS VARCHAR),
               CAST(l_extendedprice AS DOUBLE), CAST(l_quantity AS DOUBLE),
               CAST(l_discount AS DOUBLE)
        FROM lineitem WHERE l_orderkey < 200
    """).fetchall()
    n = len(data)

    t_build = time.perf_counter()
    oracle_solver.create_model("per_multi_col_when_different")
    for i in range(n):
        oracle_solver.add_variable(f"x_{i}", VarType.BINARY)
    for key, idxs in group_indices(data, lambda r: (r[2], r[3])).items():
        qualifying = [i for i in idxs if data[i][6] > 0.05]
        if not qualifying:
            continue
        oracle_solver.add_constraint(
            {f"x_{i}": data[i][5] for i in qualifying}, "<=", 20.0,
            name=f"per_{key[0]}_{key[1]}",
        )
    oracle_solver.add_constraint(
        {f"x_{i}": 1.0 for i in range(n)}, "<=", 30.0, name="global_cap",
    )
    oracle_solver.set_objective(
        {f"x_{i}": data[i][4] for i in range(n)}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_rows, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[decidb_cols.index("l_extendedprice")])},
    )
    perf_tracker.record(
        "multi_column_per_with_when_on_different_column", decidb_time, build_time,
        result.solve_time_seconds, n, n,
        len(set((r[2], r[3]) for r in data)) + 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.per_clause
@pytest.mark.when_constraint
@pytest.mark.correctness
def test_multi_column_per_when_overlaps_per_column(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """WHEN filters on a column that IS also in the PER tuple."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_returnflag, l_linestatus,
               l_extendedprice, x
        FROM lineitem WHERE l_orderkey < 200
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) <= 2 WHEN l_returnflag = 'R' PER (l_returnflag, l_linestatus)
            AND SUM(x) <= 50
        MAXIMIZE SUM(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT), CAST(l_linenumber AS BIGINT),
               CAST(l_returnflag AS VARCHAR), CAST(l_linestatus AS VARCHAR),
               CAST(l_extendedprice AS DOUBLE)
        FROM lineitem WHERE l_orderkey < 200
    """).fetchall()
    n = len(data)

    t_build = time.perf_counter()
    oracle_solver.create_model("per_multi_col_when_overlap")
    for i in range(n):
        oracle_solver.add_variable(f"x_{i}", VarType.BINARY)
    for key, idxs in group_indices(data, lambda r: (r[2], r[3])).items():
        if key[0] != "R":
            continue
        oracle_solver.add_constraint(
            {f"x_{i}": 1.0 for i in idxs}, "<=", 2.0,
            name=f"per_{key[0]}_{key[1]}",
        )
    oracle_solver.add_constraint(
        {f"x_{i}": 1.0 for i in range(n)}, "<=", 50.0, name="global_cap",
    )
    oracle_solver.set_objective(
        {f"x_{i}": data[i][4] for i in range(n)}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_rows, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[decidb_cols.index("l_extendedprice")])},
    )
    perf_tracker.record(
        "multi_column_per_when_overlaps_per_column", decidb_time, build_time,
        result.solve_time_seconds, n, n,
        sum(1 for k in set((r[2], r[3]) for r in data) if k[0] == "R") + 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.per_clause
@pytest.mark.when_constraint
@pytest.mark.correctness
def test_multi_column_per_when_eliminates_all_in_group(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """WHEN can eliminate all rows from some composite groups. Those groups
    should simply not appear — no vacuous constraint emitted."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_returnflag, l_linestatus,
               l_extendedprice, l_quantity, x
        FROM lineitem WHERE l_orderkey < 100
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) <= 2 WHEN l_quantity > 40 PER (l_returnflag, l_linestatus)
            AND SUM(x) <= 20
        MAXIMIZE SUM(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT), CAST(l_linenumber AS BIGINT),
               CAST(l_returnflag AS VARCHAR), CAST(l_linestatus AS VARCHAR),
               CAST(l_extendedprice AS DOUBLE), CAST(l_quantity AS DOUBLE)
        FROM lineitem WHERE l_orderkey < 100
    """).fetchall()
    n = len(data)

    t_build = time.perf_counter()
    oracle_solver.create_model("per_multi_col_when_empty_groups")
    for i in range(n):
        oracle_solver.add_variable(f"x_{i}", VarType.BINARY)
    for key, idxs in group_indices(data, lambda r: (r[2], r[3])).items():
        qualifying = [i for i in idxs if data[i][5] > 40]
        if not qualifying:
            continue
        oracle_solver.add_constraint(
            {f"x_{i}": 1.0 for i in qualifying}, "<=", 2.0,
            name=f"per_{key[0]}_{key[1]}",
        )
    oracle_solver.add_constraint(
        {f"x_{i}": 1.0 for i in range(n)}, "<=", 20.0, name="global_cap",
    )
    oracle_solver.set_objective(
        {f"x_{i}": data[i][4] for i in range(n)}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_rows, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[decidb_cols.index("l_extendedprice")])},
    )
    perf_tracker.record(
        "multi_column_per_when_eliminates_all_in_group", decidb_time, build_time,
        result.solve_time_seconds, n, n, 0,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.per_clause
@pytest.mark.correctness
def test_multi_column_per_more_groups(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """Multi-column PER should admit at least as many selections as single-column
    PER with the same per-group cap (finer groups ⇒ more headroom)."""
    sql_single = """
        SELECT l_orderkey, l_linenumber, l_returnflag, l_linestatus,
               l_extendedprice, x
        FROM lineitem WHERE l_orderkey < 100
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) <= 3 PER l_returnflag
        MAXIMIZE SUM(x * l_extendedprice)
    """
    sql_multi = """
        SELECT l_orderkey, l_linenumber, l_returnflag, l_linestatus,
               l_extendedprice, x
        FROM lineitem WHERE l_orderkey < 100
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) <= 3 PER (l_returnflag, l_linestatus)
        MAXIMIZE SUM(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    rows_single, cols_single = decidb_cli.execute(sql_single)
    rows_multi, cols_multi = decidb_cli.execute(sql_multi)
    decidb_time = time.perf_counter() - t0

    x_idx = cols_single.index("x")
    sum_single = sum(int(r[x_idx]) for r in rows_single)
    sum_multi = sum(int(r[x_idx]) for r in rows_multi)
    assert sum_multi >= sum_single, (
        f"Multi-column PER selected {sum_multi} but single-column selected {sum_single}"
    )

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT), CAST(l_linenumber AS BIGINT),
               CAST(l_returnflag AS VARCHAR), CAST(l_linestatus AS VARCHAR),
               CAST(l_extendedprice AS DOUBLE)
        FROM lineitem WHERE l_orderkey < 100
    """).fetchall()
    n = len(data)

    t_build = time.perf_counter()
    oracle_solver.create_model("per_multi_col_more_groups")
    for i in range(n):
        oracle_solver.add_variable(f"x_{i}", VarType.BINARY)
    for key, idxs in group_indices(data, lambda r: (r[2], r[3])).items():
        oracle_solver.add_constraint(
            {f"x_{i}": 1.0 for i in idxs}, "<=", 3.0,
            name=f"per_{key[0]}_{key[1]}",
        )
    oracle_solver.set_objective(
        {f"x_{i}": data[i][4] for i in range(n)}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        rows_multi, cols_multi, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[cols_multi.index("l_extendedprice")])},
    )
    perf_tracker.record(
        "multi_column_per_more_groups", decidb_time, build_time,
        result.solve_time_seconds, n, n, len(set((r[2], r[3]) for r in data)),
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.per_clause
@pytest.mark.correctness
def test_multi_column_per_three_columns(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """PER with three columns."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_returnflag, l_linestatus, l_shipmode,
               l_extendedprice, x
        FROM lineitem WHERE l_orderkey < 200
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) <= 2 PER (l_returnflag, l_linestatus, l_shipmode)
        MAXIMIZE SUM(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT), CAST(l_linenumber AS BIGINT),
               CAST(l_returnflag AS VARCHAR), CAST(l_linestatus AS VARCHAR),
               CAST(l_shipmode AS VARCHAR), CAST(l_extendedprice AS DOUBLE)
        FROM lineitem WHERE l_orderkey < 200
    """).fetchall()
    n = len(data)

    t_build = time.perf_counter()
    oracle_solver.create_model("per_multi_col_three_cols")
    for i in range(n):
        oracle_solver.add_variable(f"x_{i}", VarType.BINARY)
    for key, idxs in group_indices(data, lambda r: (r[2], r[3], r[4])).items():
        oracle_solver.add_constraint(
            {f"x_{i}": 1.0 for i in idxs}, "<=", 2.0,
            name=f"per_{key[0]}_{key[1]}_{key[2]}",
        )
    oracle_solver.set_objective(
        {f"x_{i}": data[i][5] for i in range(n)}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_rows, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[decidb_cols.index("l_extendedprice")])},
    )
    perf_tracker.record(
        "multi_column_per_three_columns", decidb_time, build_time,
        result.solve_time_seconds, n, n,
        len(set((r[2], r[3], r[4]) for r in data)),
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.per_clause
@pytest.mark.correctness
def test_multi_column_per_with_integer_variable(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """Multi-column PER with integer variables and weighted constraints."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_returnflag, l_linestatus,
               l_extendedprice, l_quantity, x
        FROM lineitem WHERE l_orderkey < 100
        DECIDE x IS INTEGER
        SUCH THAT SUM(x * l_quantity) <= 100 PER (l_returnflag, l_linestatus)
            AND x <= 3
        MAXIMIZE SUM(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT), CAST(l_linenumber AS BIGINT),
               CAST(l_returnflag AS VARCHAR), CAST(l_linestatus AS VARCHAR),
               CAST(l_extendedprice AS DOUBLE), CAST(l_quantity AS DOUBLE)
        FROM lineitem WHERE l_orderkey < 100
    """).fetchall()
    n = len(data)

    t_build = time.perf_counter()
    oracle_solver.create_model("per_multi_col_integer")
    for i in range(n):
        oracle_solver.add_variable(f"x_{i}", VarType.INTEGER, lb=0.0, ub=3.0)
    for key, idxs in group_indices(data, lambda r: (r[2], r[3])).items():
        oracle_solver.add_constraint(
            {f"x_{i}": data[i][5] for i in idxs}, "<=", 100.0,
            name=f"per_{key[0]}_{key[1]}",
        )
    oracle_solver.set_objective(
        {f"x_{i}": data[i][4] for i in range(n)}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_rows, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[decidb_cols.index("l_extendedprice")])},
    )
    perf_tracker.record(
        "multi_column_per_with_integer_variable", decidb_time, build_time,
        result.solve_time_seconds, n, n, len(set((r[2], r[3]) for r in data)),
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.per_clause
@pytest.mark.error
def test_unparenthesized_multi_column_per_rejected_with_hint(decidb_cli):
    """`PER a, b` (no parens) is a grammar ambiguity, so it is not a
    supported form. The comma is rejected by the parser with a hint pointing
    at the parenthesized `PER (col1, col2)` form."""
    sql = """
        SELECT id, x FROM (VALUES (1,'EU',2024),(2,'US',2025)) t(id, region, yr)
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) >= 1 PER region, yr
        MAXIMIZE SUM(x)
    """
    decidb_cli.assert_error(
        sql,
        match=r"(?i)comma-separated SUCH THAT constraints are not supported",
    )


@pytest.mark.per_clause
@pytest.mark.correctness
def test_parenthesized_multi_column_per_is_the_supported_form(decidb_cli):
    """The parenthesized composite PER binds and solves end-to-end — the
    supported counterpart to the rejected bare form above."""
    sql = """
        SELECT id, x FROM (VALUES (1,'EU',2024),(2,'US',2025)) t(id, region, yr)
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) >= 1 PER (region, yr)
        MAXIMIZE SUM(x)
    """
    rows, cols = decidb_cli.execute(sql)
    assert len(rows) == 2  # both singleton groups get their x selected
