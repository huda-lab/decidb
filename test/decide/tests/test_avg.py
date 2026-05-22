"""Tests for AVG() over decision variables.

``AVG(expr) op K`` rewrites to ``SUM(expr) op K*N`` at bind time, where the
divisor N is:
  - total rows for a vanilla AVG
  - the count of WHEN-qualifying rows for AVG(..) WHEN cond
  - the per-group size for AVG(..) PER col
  - the intersection for AVG(..) WHEN cond PER col

Objectives share argmax/argmin with SUM (N is a positive constant) so an
AVG objective is compared against an ordinary linear objective.
"""

import time
from collections import defaultdict

import pytest

from decidb_cli import DecidBCliError
from solver.types import VarType, ObjSense
from comparison.compare import compare_solutions
from ._oracle_helpers import add_bool_and, add_ne_indicator, group_indices


_VALUES_4 = "VALUES ('a', 10), ('b', 5), ('c', 8), ('d', 3)"
_DATA_SQL_4 = f"SELECT * FROM ({_VALUES_4}) t(name, value)"


def _fetch(duckdb_conn, sql):
    return duckdb_conn.execute(sql).fetchall()


@pytest.mark.avg_rewrite
@pytest.mark.correctness
def test_avg_objective(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """MAXIMIZE AVG(x * col) should give the same argmax as MAXIMIZE SUM(x * col)."""
    base = """
        SELECT name, value, x FROM ({data_sql})
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) <= 2
        {objective}
    """
    sql_avg = base.format(data_sql=_DATA_SQL_4, objective="MAXIMIZE AVG(x * value)")
    sql_sum = base.format(data_sql=_DATA_SQL_4, objective="MAXIMIZE SUM(x * value)")

    t0 = time.perf_counter()
    rows_avg, cols_avg = decidb_cli.execute(sql_avg)
    rows_sum, cols_sum = decidb_cli.execute(sql_sum)
    decidb_time = time.perf_counter() - t0

    ci_avg = {c: i for i, c in enumerate(cols_avg)}
    ci_sum = {c: i for i, c in enumerate(cols_sum)}
    selected_avg = {r[ci_avg["name"]] for r in rows_avg if r[ci_avg["x"]] == 1}
    selected_sum = {r[ci_sum["name"]] for r in rows_sum if r[ci_sum["x"]] == 1}
    assert selected_avg == selected_sum, (
        f"AVG and SUM objectives should select same rows: {selected_avg} vs {selected_sum}"
    )

    data = _fetch(
        duckdb_conn,
        f"SELECT CAST(name AS VARCHAR), CAST(value AS DOUBLE) FROM ({_DATA_SQL_4})",
    )
    n = len(data)
    t_build = time.perf_counter()
    oracle_solver.create_model("avg_objective_sum_equiv")
    for i in range(n):
        oracle_solver.add_variable(f"x_{i}", VarType.BINARY)
    oracle_solver.add_constraint(
        {f"x_{i}": 1.0 for i in range(n)}, "<=", 2.0, name="sum_cap",
    )
    oracle_solver.set_objective(
        {f"x_{i}": data[i][1] for i in range(n)}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        rows_sum, cols_sum, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[cols_sum.index("value")])},
    )
    perf_tracker.record(
        "avg_objective", decidb_time, build_time, result.solve_time_seconds,
        n, n, 1, result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.avg_rewrite
@pytest.mark.correctness
def test_avg_constraint(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """AVG(x*value) <= 5 with N=4 rows ⇒ SUM(x*value) <= 20."""
    sql = f"""
        SELECT name, value, x FROM ({_DATA_SQL_4})
        DECIDE x IS BOOLEAN
        SUCH THAT AVG(x * value) <= 5
        MAXIMIZE SUM(x * value)
    """
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = _fetch(
        duckdb_conn,
        f"SELECT CAST(name AS VARCHAR), CAST(value AS DOUBLE) FROM ({_DATA_SQL_4})",
    )
    n = len(data)
    t_build = time.perf_counter()
    oracle_solver.create_model("avg_constraint")
    for i in range(n):
        oracle_solver.add_variable(f"x_{i}", VarType.BINARY)
    oracle_solver.add_constraint(
        {f"x_{i}": data[i][1] for i in range(n)},
        "<=", 5.0 * n, name="avg_cap",
    )
    oracle_solver.set_objective(
        {f"x_{i}": data[i][1] for i in range(n)}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_rows, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[decidb_cols.index("value")])},
    )
    perf_tracker.record(
        "avg_constraint", decidb_time, build_time, result.solve_time_seconds,
        n, n, 1, result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.avg_rewrite
@pytest.mark.correctness
def test_avg_with_when(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """AVG(x*value) <= 6 WHEN tier='high' uses N_when (number of high rows)."""
    values = (
        "VALUES ('a', 10, 'high'), ('b', 5, 'low'), "
        "('c', 8, 'high'), ('d', 3, 'low')"
    )
    data_sql = f"SELECT * FROM ({values}) t(name, value, tier)"
    sql = f"""
        SELECT name, value, tier, x FROM ({data_sql})
        DECIDE x IS BOOLEAN
        SUCH THAT AVG(x * value) <= 6 WHEN tier = 'high'
        MAXIMIZE SUM(x * value)
    """
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = _fetch(
        duckdb_conn,
        f"SELECT CAST(name AS VARCHAR), CAST(value AS DOUBLE), CAST(tier AS VARCHAR) FROM ({data_sql})",
    )
    n = len(data)
    high_idx = [i for i, r in enumerate(data) if r[2] == "high"]

    t_build = time.perf_counter()
    oracle_solver.create_model("avg_when")
    for i in range(n):
        oracle_solver.add_variable(f"x_{i}", VarType.BINARY)
    oracle_solver.add_constraint(
        {f"x_{i}": data[i][1] for i in high_idx},
        "<=", 6.0 * len(high_idx), name="avg_when_cap",
    )
    oracle_solver.set_objective(
        {f"x_{i}": data[i][1] for i in range(n)}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_rows, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[decidb_cols.index("value")])},
    )
    perf_tracker.record(
        "avg_with_when", decidb_time, build_time, result.solve_time_seconds,
        n, n, 1, result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.avg_rewrite
@pytest.mark.correctness
def test_avg_with_per(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """AVG(x*value) <= 4 PER grp uses N_g per group."""
    values = (
        "VALUES ('a', 10, 'A'), ('b', 5, 'B'), ('c', 8, 'A'), ('d', 3, 'B')"
    )
    data_sql = f"SELECT * FROM ({values}) t(name, value, grp)"
    sql = f"""
        SELECT name, value, grp, x FROM ({data_sql})
        DECIDE x IS BOOLEAN
        SUCH THAT AVG(x * value) <= 4 PER grp
        MAXIMIZE SUM(x * value)
    """
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = _fetch(
        duckdb_conn,
        f"SELECT CAST(name AS VARCHAR), CAST(value AS DOUBLE), CAST(grp AS VARCHAR) FROM ({data_sql})",
    )
    n = len(data)

    t_build = time.perf_counter()
    oracle_solver.create_model("avg_per")
    for i in range(n):
        oracle_solver.add_variable(f"x_{i}", VarType.BINARY)
    for grp, idxs in group_indices(data, lambda r: r[2]).items():
        oracle_solver.add_constraint(
            {f"x_{i}": data[i][1] for i in idxs},
            "<=", 4.0 * len(idxs), name=f"avg_per_{grp}",
        )
    oracle_solver.set_objective(
        {f"x_{i}": data[i][1] for i in range(n)}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_rows, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[decidb_cols.index("value")])},
    )
    perf_tracker.record(
        "avg_with_per", decidb_time, build_time, result.solve_time_seconds,
        n, n, 2, result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.avg_rewrite
@pytest.mark.correctness
def test_avg_with_when_per(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """AVG with both WHEN and PER: filter WHEN first, then group by PER."""
    values = (
        "VALUES ('a', 10, 'A', 'high'), ('b', 5, 'B', 'high'),"
        " ('c', 8, 'A', 'low'),  ('d', 3, 'B', 'low'),"
        " ('e', 6, 'A', 'high'), ('f', 4, 'B', 'high')"
    )
    data_sql = f"SELECT * FROM ({values}) t(name, value, grp, tier)"
    sql = f"""
        SELECT name, value, grp, tier, x FROM ({data_sql})
        DECIDE x IS BOOLEAN
        SUCH THAT AVG(x * value) <= 5 WHEN tier = 'high' PER grp
        MAXIMIZE SUM(x * value)
    """
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = _fetch(
        duckdb_conn,
        f"SELECT CAST(name AS VARCHAR), CAST(value AS DOUBLE), "
        f"CAST(grp AS VARCHAR), CAST(tier AS VARCHAR) FROM ({data_sql})",
    )
    n = len(data)

    t_build = time.perf_counter()
    oracle_solver.create_model("avg_when_per")
    for i in range(n):
        oracle_solver.add_variable(f"x_{i}", VarType.BINARY)
    for grp, idxs in group_indices(data, lambda r: r[2]).items():
        qualifying = [i for i in idxs if data[i][3] == "high"]
        if not qualifying:
            continue
        oracle_solver.add_constraint(
            {f"x_{i}": data[i][1] for i in qualifying},
            "<=", 5.0 * len(qualifying), name=f"avg_when_per_{grp}",
        )
    oracle_solver.set_objective(
        {f"x_{i}": data[i][1] for i in range(n)}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_rows, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[decidb_cols.index("value")])},
    )
    perf_tracker.record(
        "avg_with_when_per", decidb_time, build_time, result.solve_time_seconds,
        n, n, 2, result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.avg_rewrite
@pytest.mark.correctness
def test_avg_boolean(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """AVG(x) <= 0.5 with 4 BOOLEAN rows ⇒ SUM(x) <= 2."""
    sql = f"""
        SELECT name, value, x FROM ({_DATA_SQL_4})
        DECIDE x IS BOOLEAN
        SUCH THAT AVG(x) <= 0.5
        MAXIMIZE SUM(x * value)
    """
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = _fetch(
        duckdb_conn,
        f"SELECT CAST(name AS VARCHAR), CAST(value AS DOUBLE) FROM ({_DATA_SQL_4})",
    )
    n = len(data)

    t_build = time.perf_counter()
    oracle_solver.create_model("avg_boolean")
    for i in range(n):
        oracle_solver.add_variable(f"x_{i}", VarType.BINARY)
    oracle_solver.add_constraint(
        {f"x_{i}": 1.0 for i in range(n)}, "<=", 0.5 * n, name="avg_x_cap",
    )
    oracle_solver.set_objective(
        {f"x_{i}": data[i][1] for i in range(n)}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_rows, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[decidb_cols.index("value")])},
    )
    perf_tracker.record(
        "avg_boolean", decidb_time, build_time, result.solve_time_seconds,
        n, n, 1, result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.avg_rewrite
@pytest.mark.correctness
def test_avg_integer(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """AVG(x) <= 3 with 3 INTEGER rows (x <= 5) ⇒ SUM(x) <= 9."""
    values = "VALUES ('a', 10), ('b', 5), ('c', 8)"
    data_sql = f"SELECT * FROM ({values}) t(name, value)"
    sql = f"""
        SELECT name, value, x FROM ({data_sql})
        DECIDE x
        SUCH THAT x <= 5 AND AVG(x) <= 3
        MAXIMIZE SUM(x * value)
    """
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = _fetch(
        duckdb_conn,
        f"SELECT CAST(name AS VARCHAR), CAST(value AS DOUBLE) FROM ({data_sql})",
    )
    n = len(data)

    t_build = time.perf_counter()
    oracle_solver.create_model("avg_integer")
    for i in range(n):
        oracle_solver.add_variable(f"x_{i}", VarType.INTEGER, lb=0.0, ub=5.0)
    oracle_solver.add_constraint(
        {f"x_{i}": 1.0 for i in range(n)}, "<=", 3.0 * n, name="avg_cap",
    )
    oracle_solver.set_objective(
        {f"x_{i}": data[i][1] for i in range(n)}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_rows, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[decidb_cols.index("value")])},
    )
    perf_tracker.record(
        "avg_integer", decidb_time, build_time, result.solve_time_seconds,
        n, n, 1, result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.avg_rewrite
@pytest.mark.correctness
def test_avg_bilinear_constraint(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """AVG(x*y) <= 0.5 with 3 rows, both x,y BOOLEAN. AND-linearize the product."""
    data_sql = "SELECT * FROM (VALUES (1), (2), (3)) t(value)"
    sql = f"""
        SELECT value, x, y FROM ({data_sql})
        DECIDE x IS BOOLEAN, y IS BOOLEAN
        SUCH THAT AVG(x * y) <= 0.5
        MAXIMIZE SUM(x + y)
    """
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = _fetch(
        duckdb_conn,
        f"SELECT CAST(value AS BIGINT) FROM ({data_sql})",
    )
    n = len(data)

    t_build = time.perf_counter()
    oracle_solver.create_model("avg_bilinear")
    for i in range(n):
        oracle_solver.add_variable(f"x_{i}", VarType.BINARY)
        oracle_solver.add_variable(f"y_{i}", VarType.BINARY)
        add_bool_and(oracle_solver, f"x_{i}", f"y_{i}", f"z_{i}")
    oracle_solver.add_constraint(
        {f"z_{i}": 1.0 for i in range(n)}, "<=", 0.5 * n, name="avg_xy_cap",
    )
    obj = {}
    for i in range(n):
        obj[f"x_{i}"] = 1.0
        obj[f"y_{i}"] = 1.0
    oracle_solver.set_objective(obj, ObjSense.MAXIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_rows, decidb_cols, result, data, ["x", "y"],
        coeff_fn=lambda row: {"x": 1.0, "y": 1.0},
    )
    perf_tracker.record(
        "avg_bilinear_constraint", decidb_time, build_time, result.solve_time_seconds,
        n, 3 * n, 1, result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.avg_rewrite
@pytest.mark.correctness
def test_avg_not_equal_boolean(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """AVG(x) <> 0.5 with 4 BOOLEAN rows ⇒ SUM(x) <> 2 (Big-M disjunction).

    Exercises the composition of AVG→SUM RHS scaling and the NE Big-M
    disjunction. Dataset is tuned so the unconstrained optimum is
    SUM(x)=2 (profit 190, pick {a,b} — the only feasible pair under
    the weight budget), i.e. the one value AVG(x)<>0.5 rules out. A
    silent no-op on the <> constraint would diverge from the oracle;
    wrong RHS scaling would drop the wrong SUM band.
    """
    values = "VALUES ('a', 100, 9), ('b', 90, 7), ('c', 10, 2), ('d', 5, 1)"
    data_sql = f"SELECT * FROM ({values}) t(name, profit, weight)"
    sql = f"""
        SELECT name, profit, weight, x FROM ({data_sql})
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * weight) <= 16
              AND AVG(x) <> 0.5
        MAXIMIZE SUM(x * profit)
    """
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = _fetch(
        duckdb_conn,
        f"SELECT CAST(name AS VARCHAR), CAST(profit AS DOUBLE), "
        f"CAST(weight AS DOUBLE) FROM ({data_sql})",
    )
    n = len(data)

    t_build = time.perf_counter()
    oracle_solver.create_model("avg_not_equal_boolean")
    for i in range(n):
        oracle_solver.add_variable(f"x_{i}", VarType.BINARY)
    oracle_solver.add_constraint(
        {f"x_{i}": data[i][2] for i in range(n)},
        "<=", 16.0, name="budget",
    )
    # AVG(x) <> 0.5 with n=4 ⇒ SUM(x) <> 2 (integer step).
    add_ne_indicator(
        oracle_solver, {f"x_{i}": 1.0 for i in range(n)},
        rhs=0.5 * n, name="avg_ne_05",
    )
    oracle_solver.set_objective(
        {f"x_{i}": data[i][1] for i in range(n)}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_rows, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[decidb_cols.index("profit")])},
    )
    perf_tracker.record(
        "avg_not_equal_boolean", decidb_time, build_time, result.solve_time_seconds,
        n, n, 2, result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.avg_rewrite
@pytest.mark.correctness
def test_avg_not_equal_with_when(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """AVG(x) <> 0.5 WHEN tier='high' — N scales with WHEN-qualifying rows.

    6 rows, 4 tagged 'high'; the AVG denominator becomes 4, so the
    constraint is SUM(x_i for high i) <> 2. Dataset is tuned so the
    unconstrained optimum picks exactly 2 high rows ({a,b,e}: profit
    240) — the forbidden value; constrained optimum shifts to 3 high
    rows ({a,b,d,f}: profit 235).
    """
    values = (
        "VALUES ('a', 100, 9, 'high'), ('b', 90, 7, 'high'),"
        "       ('c',  10, 2, 'high'), ('d',  5, 1, 'high'),"
        "       ('e',  50, 4, 'low'),  ('f', 40, 3, 'low')"
    )
    data_sql = f"SELECT * FROM ({values}) t(name, profit, weight, tier)"
    sql = f"""
        SELECT name, profit, weight, tier, x FROM ({data_sql})
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * weight) <= 20
              AND AVG(x) <> 0.5 WHEN tier = 'high'
        MAXIMIZE SUM(x * profit)
    """
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = _fetch(
        duckdb_conn,
        f"SELECT CAST(name AS VARCHAR), CAST(profit AS DOUBLE), "
        f"CAST(weight AS DOUBLE), CAST(tier AS VARCHAR) FROM ({data_sql})",
    )
    n = len(data)
    high_idx = [i for i, r in enumerate(data) if r[3] == "high"]

    t_build = time.perf_counter()
    oracle_solver.create_model("avg_not_equal_with_when")
    for i in range(n):
        oracle_solver.add_variable(f"x_{i}", VarType.BINARY)
    oracle_solver.add_constraint(
        {f"x_{i}": data[i][2] for i in range(n)},
        "<=", 20.0, name="budget",
    )
    # AVG(x) <> 0.5 WHEN high ⇒ SUM(x_i for high) <> 0.5 * |high| = 2.
    add_ne_indicator(
        oracle_solver, {f"x_{i}": 1.0 for i in high_idx},
        rhs=0.5 * len(high_idx), name="avg_when_ne_05",
    )
    oracle_solver.set_objective(
        {f"x_{i}": data[i][1] for i in range(n)}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_rows, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[decidb_cols.index("profit")])},
    )
    perf_tracker.record(
        "avg_not_equal_with_when", decidb_time, build_time, result.solve_time_seconds,
        n, n, 2, result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.avg_rewrite
def test_avg_no_decide_var(decidb_cli):
    """AVG(col) without decide variables must pass through to normal DuckDB.

    This is a pass-through test — no decision problem means no oracle to
    compare against. Kept as DecidB-only behavior verification.
    """
    rows, cols = decidb_cli.execute("""
        SELECT AVG(value) as avg_val FROM (
            VALUES (10), (5), (8), (3)
        ) t(value)
    """)
    ci = {c: i for i, c in enumerate(cols)}
    assert abs(rows[0][ci["avg_val"]] - 6.5) < 1e-6, \
        f"Expected AVG=6.5, got {rows[0][ci['avg_val']]}"
