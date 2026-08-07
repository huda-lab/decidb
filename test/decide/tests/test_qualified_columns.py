"""Tests for table-qualified data columns inside SUCH THAT / MAXIMIZE.

A `Symbolic` symbol carries only a name, so a qualified column reference used as
a coefficient used to come back out of the symbolic round trip as a bare
`ColumnRefExpression(GetColumnName())` — the qualifier was dropped, and any query
whose FROM had two tables sharing that column name was rejected with a misleading
"Ambiguous reference to column name" error naming the exact syntax the user had
written. `SymbolicTranslationContext::column_map` now keys the symbol by the full
dotted path and restores the original reference by copy.

TPC-H prefixes every column with its table, so the collision is produced here by
self-joining `nation`: `n1` and `n2` both expose `n_nationkey`.
"""

import time

import pytest

from solver.types import VarType, ObjSense
from comparison.compare import compare_solutions


# Nations sharing a region with nation 1 (ARGENTINA, AMERICA). `n2` is pinned to
# a single row, so `n2.n_nationkey` is the constant 1 on every join row while
# `n1.n_nationkey` varies — the two columns therefore disagree, and a query that
# silently used the wrong one would produce a different optimum.
_ROWS_SQL = """
    SELECT CAST(n1.n_nationkey AS BIGINT), CAST(n2.n_nationkey AS BIGINT)
    FROM nation n1 JOIN nation n2 ON n1.n_regionkey = n2.n_regionkey
    WHERE n2.n_nationkey = 1
    ORDER BY n1.n_nationkey
"""


@pytest.mark.var_boolean
@pytest.mark.sql_joins
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_qualified_column_distinguishes_self_join_sides(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """Qualified coefficients bind, and each clause uses its own side of the join.

    The constraint reads `n2.n_nationkey` (constant 1, so it caps the count at 3)
    and the objective reads `n1.n_nationkey` (varying, so it ranks the rows).
    Using `n1` in the constraint instead would cap by nationkey sum, admitting a
    different row set.
    """
    sql = """
        SELECT n1.n_nationkey, x
        FROM nation n1 JOIN nation n2 ON n1.n_regionkey = n2.n_regionkey
        WHERE n2.n_nationkey = 1
        DECIDE x(BOOL)
        SUCH THAT SUM(x * n2.n_nationkey) <= 3
        MAXIMIZE SUM(x * n1.n_nationkey)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute(_ROWS_SQL).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("qualified_column_self_join")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    # SUM(x * n2.n_nationkey) <= 3, with n2.n_nationkey == 1 on every row
    oracle_solver.add_constraint(
        {vnames[i]: float(data[i][1]) for i in range(len(data))},
        "<=", 3.0, name="count_cap",
    )
    # MAXIMIZE SUM(x * n1.n_nationkey)
    oracle_solver.set_objective(
        {vnames[i]: float(data[i][0]) for i in range(len(data))},
        ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_result, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[decidb_cols.index("n_nationkey")])},
    )

    perf_tracker.record(
        "qualified_column_self_join", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.var_boolean
@pytest.mark.sql_joins
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
def test_qualified_column_in_objective_only(decidb_cli, duckdb_conn):
    """A qualified coefficient in the objective alone still binds.

    Guards the FromSymbolic restore path independently of the constraint path:
    the objective is the only place the ambiguous name appears.
    """
    sql = """
        SELECT n1.n_nationkey, x
        FROM nation n1 JOIN nation n2 ON n1.n_regionkey = n2.n_regionkey
        WHERE n2.n_nationkey = 1
        DECIDE x(BOOL)
        SUCH THAT SUM(x) <= 2
        MAXIMIZE SUM(x * n1.n_nationkey)
    """
    decidb_result, decidb_cols = decidb_cli.execute(sql)

    # The two largest n1.n_nationkey values in the region are selected.
    keys = [int(r[0]) for r in duckdb_conn.execute(_ROWS_SQL).fetchall()]
    expected = set(sorted(keys)[-2:])

    key_idx = decidb_cols.index("n_nationkey")
    x_idx = decidb_cols.index("x")
    chosen = {int(r[key_idx]) for r in decidb_result if int(r[x_idx]) == 1}
    assert chosen == expected, f"expected {expected}, got {chosen}"


@pytest.mark.var_boolean
@pytest.mark.sql_joins
@pytest.mark.cons_aggregate
def test_qualified_decide_variable_with_qualified_column(decidb_cli, duckdb_conn):
    """A table-scoped DECIDE variable and a qualified coefficient in one query.

    DECIDE variables are canonicalized to their unqualified name (the form
    `bind_select_node.cpp` always registers) rather than routed through
    `column_map`, so `n1.keep` and `keep` must stay a single symbol while
    `n1.n_nationkey` keeps its qualifier.
    """
    sql = """
        SELECT n1.n_nationkey, keep
        FROM nation n1 JOIN nation n2 ON n1.n_regionkey = n2.n_regionkey
        WHERE n2.n_nationkey = 1
        DECIDE n1.keep(BOOL)
        SUCH THAT SUM(n1.keep * n2.n_nationkey) <= 2
        MAXIMIZE SUM(keep * n1.n_nationkey)
    """
    decidb_result, decidb_cols = decidb_cli.execute(sql)

    keys = [int(r[0]) for r in duckdb_conn.execute(_ROWS_SQL).fetchall()]
    expected = set(sorted(keys)[-2:])

    key_idx = decidb_cols.index("n_nationkey")
    keep_idx = decidb_cols.index("keep")
    chosen = {int(r[key_idx]) for r in decidb_result if int(r[keep_idx]) == 1}
    assert chosen == expected, f"expected {expected}, got {chosen}"
