"""Tests for the two-level objective, `outer(per k: inner(e) by (k))`.

The two-level ILP formulation (per DecidB docs):
  INNER creates per-group auxiliary variables; OUTER creates a global
  auxiliary. Easy / hard classification applies at each level independently.

  - SUM as outer (over groups) is always easy — linear sum of auxiliaries. An
    outer AVG is refused ("not supported yet"); ``sum(per k: ...)`` ranks
    solutions the same way.
  - MIN/MAX as outer splits into easy (``MIN MAX SUM`` / ``MAX MIN SUM``) and
    hard (``MAX MAX SUM`` / ``MIN MIN SUM``) cases.
  - MIN/MAX as inner splits similarly: easy when the inner aggregate is
    bounded in the direction optimization naturally tightens, hard
    otherwise (needs Gurobi indicators).
  - AVG as inner scales per-row coefficients by ``1 / n_g`` — NOT equivalent
    to SUM when groups differ in size.

Every correctness case below builds the same two-level model in gurobipy and
compares DecidB's nested-aggregate objective (evaluated on its own output)
against the oracle's ``objective_value``.
"""

import time
from collections import defaultdict

import pytest

from solver.types import VarType, ObjSense
from comparison.compare import compare_solutions
from ._oracle_helpers import (
    group_indices,
    emit_inner_max, emit_inner_min,
    emit_hard_inner_max, emit_hard_inner_min,
    emit_hard_inner_max_quadratic, emit_hard_inner_min_quadratic,
)


# ---------------------------------------------------------------------------
# Utilities for evaluating nested-aggregate objectives on DecidB output
# ---------------------------------------------------------------------------

def _per_group_selected(rows, cols, per_col, expr_fn, x_col="x"):
    """Group rows by ``per_col``. For each group, collect expr_fn(row)
    values across rows where the decision variable ``x_col`` is selected
    (> 0.5). Returns ``{group_key: [values]}``."""
    xi = cols.index(x_col)
    pi = cols.index(per_col)
    out: dict = defaultdict(list)
    for r in rows:
        if float(r[xi]) > 0.5:
            out[r[pi]].append(float(expr_fn(r)))
    return out


def _per_group_values_if_selected(rows, cols, per_col, expr_fn, x_col="x"):
    """Like ``_per_group_selected`` but includes ``0.0`` for unselected rows,
    so ``max(...)`` reflects ``MAX(x * expr)`` semantics (unselected ⇒ 0)."""
    xi = cols.index(x_col)
    pi = cols.index(per_col)
    out: dict = defaultdict(list)
    for r in rows:
        v = float(expr_fn(r)) if float(r[xi]) > 0.5 else 0.0
        out[r[pi]].append(v)
    return out


# ===========================================================================
# SUM + `per` = no-op
# ===========================================================================

@pytest.mark.per_clause
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_sum_per_noop(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """sum(per col: sum(x * cost) by (col)) ≡ SUM(x * cost)."""
    sql_per = """
        SELECT s_suppkey, s_nationkey, s_acctbal, x FROM supplier
        WHERE s_nationkey < 5
        DECIDE x(BOOL)
        SUCH THAT SUM(x) >= 3
        MINIMIZE sum(per s_nationkey: sum(x * s_acctbal) by (s_nationkey))
    """
    t0 = time.perf_counter()
    rows, cols = decidb_cli.execute(sql_per)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(s_suppkey AS BIGINT),
               CAST(s_nationkey AS BIGINT),
               CAST(s_acctbal AS DOUBLE)
        FROM supplier WHERE s_nationkey < 5
    """).fetchall()
    n = len(data)

    t_build = time.perf_counter()
    oracle_solver.create_model("sum_per_noop")
    vnames = [f"x_{i}" for i in range(n)]
    for v in vnames:
        oracle_solver.add_variable(v, VarType.BINARY)
    oracle_solver.add_constraint({v: 1.0 for v in vnames}, ">=", 3.0, name="total")
    oracle_solver.set_objective(
        {vnames[i]: data[i][2] for i in range(n)}, ObjSense.MINIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        rows, cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[cols.index("s_acctbal")])},
    )
    perf_tracker.record(
        "sum_per_noop", decidb_time, build_time, result.solve_time_seconds,
        n, n, 1, result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.per_clause
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_sum_sum_per_noop(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """sum(per col: sum(x * cost) by (col)) — nested SUMs cancel to flat SUM."""
    sql = """
        SELECT s_suppkey, s_nationkey, s_acctbal, x FROM supplier
        WHERE s_nationkey < 5
        DECIDE x(BOOL)
        SUCH THAT SUM(x) >= 3
        MAXIMIZE sum(per s_nationkey: sum(x * s_acctbal) by (s_nationkey))
    """
    t0 = time.perf_counter()
    rows, cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(s_suppkey AS BIGINT),
               CAST(s_nationkey AS BIGINT),
               CAST(s_acctbal AS DOUBLE)
        FROM supplier WHERE s_nationkey < 5
    """).fetchall()
    n = len(data)

    t_build = time.perf_counter()
    oracle_solver.create_model("sum_sum_per_noop")
    vnames = [f"x_{i}" for i in range(n)]
    for v in vnames:
        oracle_solver.add_variable(v, VarType.BINARY)
    oracle_solver.add_constraint({v: 1.0 for v in vnames}, ">=", 3.0, name="total")
    oracle_solver.set_objective(
        {vnames[i]: data[i][2] for i in range(n)}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        rows, cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[cols.index("s_acctbal")])},
    )
    perf_tracker.record(
        "sum_sum_per_noop", decidb_time, build_time, result.solve_time_seconds,
        n, n, 1, result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


# ---------------------------------------------------------------------------
# Common plumbing for nested-aggregate oracle construction on lineitem data
# ---------------------------------------------------------------------------

def _fetch_lineitem_nested(duckdb_conn, where):
    return duckdb_conn.execute(f"""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_quantity AS DOUBLE),
               CAST(l_returnflag AS VARCHAR)
        FROM lineitem WHERE {where}
    """).fetchall()


def _group_rows(data, key_idx):
    g: dict = defaultdict(list)
    for i, row in enumerate(data):
        g[row[key_idx]].append(i)
    return g


# ===========================================================================
# sum(per col: max(expr) by (col)) — inner MAX
# ===========================================================================

@pytest.mark.per_clause
@pytest.mark.min_max
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_minimize_sum_max_per(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """MINIMIZE sum(per flag: max(x * qty) by (flag)) — easy inner MAX."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_quantity, l_returnflag, x
        FROM lineitem WHERE l_orderkey <= 7
        DECIDE x(BOOL)
        SUCH THAT per l_returnflag: sum(x) by (l_returnflag) >= 1
        MINIMIZE sum(per l_returnflag: max(x * l_quantity) by (l_returnflag))
    """
    t0 = time.perf_counter()
    rows, cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = _fetch_lineitem_nested(duckdb_conn, "l_orderkey <= 7")
    n = len(data)
    groups = _group_rows(data, 3)

    t_build = time.perf_counter()
    oracle_solver.create_model("min_sum_max_per")
    vnames = [f"x_{i}" for i in range(n)]
    for v in vnames:
        oracle_solver.add_variable(v, VarType.BINARY)
    for flag, idxs in groups.items():
        oracle_solver.add_constraint(
            {vnames[i]: 1.0 for i in idxs}, ">=", 1.0, name=f"sum_ge_{flag}",
        )
    # Inner MAX auxiliaries per group (easy case: MINIMIZE over SUM(z_g)
    # naturally pushes z_g *down* toward max of row terms)
    z_names = []
    for flag, idxs in groups.items():
        z = emit_inner_max(
            oracle_solver,
            name_prefix=f"max_{flag}",
            row_coeffs=[{vnames[i]: data[i][2]} for i in idxs],
            row_ub=max(data[i][2] for i in idxs) + 1.0,
        )
        z_names.append(z)
    oracle_solver.set_objective({z: 1.0 for z in z_names}, ObjSense.MINIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    def decidb_obj(rs, cs):
        gs = _per_group_values_if_selected(
            rs, cs, "l_returnflag",
            lambda r: r[cs.index("l_quantity")],
        )
        return sum(max(v) if v else 0.0 for v in gs.values())

    cmp = compare_solutions(
        rows, cols, result, data, ["x"],
        decidb_objective_fn=decidb_obj,
    )
    perf_tracker.record(
        "minimize_sum_max_per", decidb_time, build_time,
        result.solve_time_seconds, n, n + len(groups), len(groups),
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.per_clause
@pytest.mark.min_max
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_maximize_sum_min_per(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """MAXIMIZE sum(per flag: min(x * qty) by (flag)) — easy inner MIN."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_quantity, l_returnflag, x
        FROM lineitem WHERE l_orderkey <= 7
        DECIDE x(BOOL)
        SUCH THAT per l_returnflag: sum(x) by (l_returnflag) >= 1
        MAXIMIZE sum(per l_returnflag: min(x * l_quantity) by (l_returnflag))
    """
    t0 = time.perf_counter()
    rows, cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = _fetch_lineitem_nested(duckdb_conn, "l_orderkey <= 7")
    n = len(data)
    groups = _group_rows(data, 3)

    t_build = time.perf_counter()
    oracle_solver.create_model("max_sum_min_per")
    vnames = [f"x_{i}" for i in range(n)]
    for v in vnames:
        oracle_solver.add_variable(v, VarType.BINARY)
    for flag, idxs in groups.items():
        oracle_solver.add_constraint(
            {vnames[i]: 1.0 for i in idxs}, ">=", 1.0, name=f"sum_ge_{flag}",
        )
    z_names = []
    for flag, idxs in groups.items():
        z = emit_inner_min(
            oracle_solver,
            name_prefix=f"min_{flag}",
            row_coeffs=[{vnames[i]: data[i][2]} for i in idxs],
            row_ub=max(data[i][2] for i in idxs) + 1.0,
        )
        z_names.append(z)
    oracle_solver.set_objective({z: 1.0 for z in z_names}, ObjSense.MAXIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    def decidb_obj(rs, cs):
        gs = _per_group_values_if_selected(
            rs, cs, "l_returnflag",
            lambda r: r[cs.index("l_quantity")],
        )
        return sum(min(v) if v else 0.0 for v in gs.values())

    cmp = compare_solutions(
        rows, cols, result, data, ["x"],
        decidb_objective_fn=decidb_obj,
    )
    perf_tracker.record(
        "maximize_sum_min_per", decidb_time, build_time,
        result.solve_time_seconds, n, n + len(groups), len(groups),
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.per_clause
@pytest.mark.min_max
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_maximize_sum_max_per(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """MAXIMIZE sum(per flag: max(x * qty) by (flag)) — hard inner MAX (needs indicators)."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_quantity, l_returnflag, x
        FROM lineitem WHERE l_orderkey <= 7
        DECIDE x(BOOL)
        SUCH THAT per l_returnflag: sum(x) by (l_returnflag) >= 1
            AND per l_returnflag: sum(x) by (l_returnflag) <= 3
        MAXIMIZE sum(per l_returnflag: max(x * l_quantity) by (l_returnflag))
    """
    t0 = time.perf_counter()
    rows, cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = _fetch_lineitem_nested(duckdb_conn, "l_orderkey <= 7")
    n = len(data)
    groups = _group_rows(data, 3)

    t_build = time.perf_counter()
    oracle_solver.create_model("max_sum_max_per")
    vnames = [f"x_{i}" for i in range(n)]
    for v in vnames:
        oracle_solver.add_variable(v, VarType.BINARY)
    for flag, idxs in groups.items():
        oracle_solver.add_constraint(
            {vnames[i]: 1.0 for i in idxs}, ">=", 1.0, name=f"sum_ge_{flag}",
        )
        oracle_solver.add_constraint(
            {vnames[i]: 1.0 for i in idxs}, "<=", 3.0, name=f"sum_le_{flag}",
        )
    z_names = []
    for flag, idxs in groups.items():
        z = emit_hard_inner_max(
            oracle_solver,
            name_prefix=f"hmax_{flag}",
            row_coeffs=[{vnames[i]: data[i][2]} for i in idxs],
            row_ub=max(data[i][2] for i in idxs) + 1.0,
        )
        z_names.append(z)
    oracle_solver.set_objective({z: 1.0 for z in z_names}, ObjSense.MAXIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    def decidb_obj(rs, cs):
        gs = _per_group_values_if_selected(
            rs, cs, "l_returnflag",
            lambda r: r[cs.index("l_quantity")],
        )
        return sum(max(v) if v else 0.0 for v in gs.values())

    cmp = compare_solutions(
        rows, cols, result, data, ["x"],
        decidb_objective_fn=decidb_obj,
    )
    perf_tracker.record(
        "maximize_sum_max_per", decidb_time, build_time,
        result.solve_time_seconds, n, 2 * n + len(groups), 2 * len(groups),
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.per_clause
@pytest.mark.min_max
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_minimize_sum_min_per(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """MINIMIZE sum(per flag: min(x * qty) by (flag)) — hard inner MIN."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_quantity, l_returnflag, x
        FROM lineitem WHERE l_orderkey <= 7
        DECIDE x(BOOL)
        SUCH THAT per l_returnflag: sum(x) by (l_returnflag) >= 1
            AND per l_returnflag: sum(x) by (l_returnflag) <= 3
        MINIMIZE sum(per l_returnflag: min(x * l_quantity) by (l_returnflag))
    """
    t0 = time.perf_counter()
    rows, cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = _fetch_lineitem_nested(duckdb_conn, "l_orderkey <= 7")
    n = len(data)
    groups = _group_rows(data, 3)

    t_build = time.perf_counter()
    oracle_solver.create_model("min_sum_min_per")
    vnames = [f"x_{i}" for i in range(n)]
    for v in vnames:
        oracle_solver.add_variable(v, VarType.BINARY)
    for flag, idxs in groups.items():
        oracle_solver.add_constraint(
            {vnames[i]: 1.0 for i in idxs}, ">=", 1.0, name=f"sum_ge_{flag}",
        )
        oracle_solver.add_constraint(
            {vnames[i]: 1.0 for i in idxs}, "<=", 3.0, name=f"sum_le_{flag}",
        )
    z_names = []
    for flag, idxs in groups.items():
        z = emit_hard_inner_min(
            oracle_solver,
            name_prefix=f"hmin_{flag}",
            row_coeffs=[{vnames[i]: data[i][2]} for i in idxs],
            row_ub=max(data[i][2] for i in idxs) + 1.0,
        )
        z_names.append(z)
    oracle_solver.set_objective({z: 1.0 for z in z_names}, ObjSense.MINIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    def decidb_obj(rs, cs):
        gs = _per_group_values_if_selected(
            rs, cs, "l_returnflag",
            lambda r: r[cs.index("l_quantity")],
        )
        return sum(min(v) if v else 0.0 for v in gs.values())

    cmp = compare_solutions(
        rows, cols, result, data, ["x"],
        decidb_objective_fn=decidb_obj,
    )
    perf_tracker.record(
        "minimize_sum_min_per", decidb_time, build_time,
        result.solve_time_seconds, n, 2 * n + len(groups), 2 * len(groups),
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


# ===========================================================================
# min/max(per col: sum(expr) by (col)) — easy outer
# ===========================================================================

@pytest.mark.per_clause
@pytest.mark.min_max
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_minimize_max_sum_per(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """MINIMIZE max(per flag: sum(x * qty) by (flag)) — easy outer MAX over per-group SUMs."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_quantity, l_returnflag, x
        FROM lineitem WHERE l_orderkey <= 10
        DECIDE x(BOOL)
        SUCH THAT per l_returnflag: sum(x) by (l_returnflag) >= 2
        MINIMIZE max(per l_returnflag: sum(x * l_quantity) by (l_returnflag))
    """
    t0 = time.perf_counter()
    rows, cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = _fetch_lineitem_nested(duckdb_conn, "l_orderkey <= 10")
    n = len(data)
    groups = _group_rows(data, 3)
    qty_sum_all = sum(r[2] for r in data) + 1.0

    t_build = time.perf_counter()
    oracle_solver.create_model("min_max_sum_per")
    vnames = [f"x_{i}" for i in range(n)]
    for v in vnames:
        oracle_solver.add_variable(v, VarType.BINARY)
    for flag, idxs in groups.items():
        oracle_solver.add_constraint(
            {vnames[i]: 1.0 for i in idxs}, ">=", 2.0, name=f"sum_ge_{flag}",
        )
    # w >= SUM(qty_i * x_i) for each group
    oracle_solver.add_variable("w", VarType.CONTINUOUS, lb=0.0, ub=qty_sum_all)
    for flag, idxs in groups.items():
        link = {vnames[i]: data[i][2] for i in idxs}
        link["w"] = -1.0
        oracle_solver.add_constraint(link, "<=", 0.0, name=f"w_ge_{flag}")
    oracle_solver.set_objective({"w": 1.0}, ObjSense.MINIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    def decidb_obj(rs, cs):
        gs = _per_group_values_if_selected(
            rs, cs, "l_returnflag",
            lambda r: r[cs.index("l_quantity")],
        )
        return max((sum(v) for v in gs.values()), default=0.0)

    cmp = compare_solutions(
        rows, cols, result, data, ["x"],
        decidb_objective_fn=decidb_obj,
    )
    perf_tracker.record(
        "minimize_max_sum_per", decidb_time, build_time,
        result.solve_time_seconds, n, n + 1, len(groups) + 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.per_clause
@pytest.mark.min_max
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_maximize_min_sum_per(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """MAXIMIZE min(per flag: sum(x * qty) by (flag)) — easy outer MIN over per-group SUMs."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_quantity, l_returnflag, x
        FROM lineitem WHERE l_orderkey <= 10
        DECIDE x(BOOL)
        SUCH THAT per l_returnflag: sum(x) by (l_returnflag) >= 2
        MAXIMIZE min(per l_returnflag: sum(x * l_quantity) by (l_returnflag))
    """
    t0 = time.perf_counter()
    rows, cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = _fetch_lineitem_nested(duckdb_conn, "l_orderkey <= 10")
    n = len(data)
    groups = _group_rows(data, 3)
    qty_sum_all = sum(r[2] for r in data) + 1.0

    t_build = time.perf_counter()
    oracle_solver.create_model("max_min_sum_per")
    vnames = [f"x_{i}" for i in range(n)]
    for v in vnames:
        oracle_solver.add_variable(v, VarType.BINARY)
    for flag, idxs in groups.items():
        oracle_solver.add_constraint(
            {vnames[i]: 1.0 for i in idxs}, ">=", 2.0, name=f"sum_ge_{flag}",
        )
    oracle_solver.add_variable("w", VarType.CONTINUOUS, lb=0.0, ub=qty_sum_all)
    for flag, idxs in groups.items():
        link = {vnames[i]: data[i][2] for i in idxs}
        link["w"] = -1.0
        oracle_solver.add_constraint(link, ">=", 0.0, name=f"w_le_{flag}")
    oracle_solver.set_objective({"w": 1.0}, ObjSense.MAXIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    def decidb_obj(rs, cs):
        gs = _per_group_values_if_selected(
            rs, cs, "l_returnflag",
            lambda r: r[cs.index("l_quantity")],
        )
        return min((sum(v) for v in gs.values()), default=0.0)

    cmp = compare_solutions(
        rows, cols, result, data, ["x"],
        decidb_objective_fn=decidb_obj,
    )
    perf_tracker.record(
        "maximize_min_sum_per", decidb_time, build_time,
        result.solve_time_seconds, n, n + 1, len(groups) + 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


# ===========================================================================
# WHEN + nested aggregate
# ===========================================================================

@pytest.mark.per_clause
@pytest.mark.when_constraint
@pytest.mark.min_max
@pytest.mark.correctness
def test_sum_max_when_per(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """MINIMIZE when qty > 10: sum(per flag: max(x * qty) by (flag)) — the when filters
    the rows before the classes form, so a flag with no kept row has no class."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_quantity, l_returnflag, x
        FROM lineitem WHERE l_orderkey <= 10
        DECIDE x(BOOL)
        SUCH THAT per l_returnflag: sum(x) by (l_returnflag) >= 1
        MINIMIZE when l_quantity > 10: sum(per l_returnflag: max(x * l_quantity) by (l_returnflag))
    """
    t0 = time.perf_counter()
    rows, cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = _fetch_lineitem_nested(duckdb_conn, "l_orderkey <= 10")
    n = len(data)
    groups = _group_rows(data, 3)

    t_build = time.perf_counter()
    oracle_solver.create_model("sum_max_when_per")
    vnames = [f"x_{i}" for i in range(n)]
    for v in vnames:
        oracle_solver.add_variable(v, VarType.BINARY)
    for flag, idxs in groups.items():
        oracle_solver.add_constraint(
            {vnames[i]: 1.0 for i in idxs}, ">=", 1.0, name=f"sum_ge_{flag}",
        )
    z_names = []
    for flag, idxs in groups.items():
        qualifying = [i for i in idxs if data[i][2] > 10]
        if not qualifying:
            # No kept row: the when filters the rows before the per classes form,
            # so this flag has no class (no contribution).
            continue
        z = emit_inner_max(
            oracle_solver,
            name_prefix=f"max_{flag}",
            row_coeffs=[{vnames[i]: data[i][2]} for i in qualifying],
            row_ub=max(data[i][2] for i in qualifying) + 1.0,
        )
        z_names.append(z)
    oracle_solver.set_objective({z: 1.0 for z in z_names}, ObjSense.MINIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    def decidb_obj(rs, cs):
        xi = cs.index("x"); qi = cs.index("l_quantity"); fi = cs.index("l_returnflag")
        gs: dict = defaultdict(list)
        for r in rs:
            if float(r[qi]) > 10:
                gs[r[fi]].append(float(r[qi]) if float(r[xi]) > 0.5 else 0.0)
        return sum(max(v) if v else 0.0 for v in gs.values())

    cmp = compare_solutions(
        rows, cols, result, data, ["x"],
        decidb_objective_fn=decidb_obj,
    )
    perf_tracker.record(
        "sum_max_when_per", decidb_time, build_time, result.solve_time_seconds,
        n, n + len(z_names), len(groups) + 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


# ===========================================================================
# Single `per` group = flat aggregate
# ===========================================================================

@pytest.mark.per_clause
@pytest.mark.min_max
@pytest.mark.correctness
def test_single_group(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """All rows share one per key ⇒ sum(per col: max(...) by (col)) === MAX(x*qty)."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_quantity, x
        FROM lineitem WHERE l_orderkey = 1
        DECIDE x(BOOL)
        SUCH THAT SUM(x) >= 2
        MINIMIZE sum(per l_orderkey: max(x * l_quantity) by (l_orderkey))
    """
    t0 = time.perf_counter()
    rows, cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT), CAST(l_linenumber AS BIGINT),
               CAST(l_quantity AS DOUBLE)
        FROM lineitem WHERE l_orderkey = 1
    """).fetchall()
    n = len(data)

    t_build = time.perf_counter()
    oracle_solver.create_model("single_group")
    vnames = [f"x_{i}" for i in range(n)]
    for v in vnames:
        oracle_solver.add_variable(v, VarType.BINARY)
    oracle_solver.add_constraint({v: 1.0 for v in vnames}, ">=", 2.0, name="total")
    z = emit_inner_max(
        oracle_solver,
        name_prefix="max",
        row_coeffs=[{vnames[i]: data[i][2]} for i in range(n)],
        row_ub=max(r[2] for r in data) + 1.0,
    )
    oracle_solver.set_objective({z: 1.0}, ObjSense.MINIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    def decidb_obj(rs, cs):
        xi = cs.index("x"); qi = cs.index("l_quantity")
        vals = [float(r[qi]) if float(r[xi]) > 0.5 else 0.0 for r in rs]
        return max(vals) if vals else 0.0

    cmp = compare_solutions(
        rows, cols, result, data, ["x"],
        decidb_objective_fn=decidb_obj,
    )
    perf_tracker.record(
        "single_group", decidb_time, build_time, result.solve_time_seconds,
        n, n + 1, 2, result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


# ===========================================================================
# AVG as inner / outer
# ===========================================================================

@pytest.mark.per_clause
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_sum_avg_per_unequal_groups(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """sum(per col: avg(x*cost) by (col)) — inner AVG scales coeffs by 1/n_g per group."""
    sql = """
        SELECT s_suppkey, s_nationkey, s_acctbal, x FROM supplier
        WHERE s_nationkey < 5
        DECIDE x(BOOL)
        SUCH THAT per s_nationkey: sum(x) by (s_nationkey) >= 1
        MINIMIZE sum(per s_nationkey: avg(x * s_acctbal) by (s_nationkey))
    """
    t0 = time.perf_counter()
    rows, cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(s_suppkey AS BIGINT),
               CAST(s_nationkey AS BIGINT),
               CAST(s_acctbal AS DOUBLE)
        FROM supplier WHERE s_nationkey < 5
    """).fetchall()
    n = len(data)
    groups = group_indices(data, lambda r: r[1])

    t_build = time.perf_counter()
    oracle_solver.create_model("sum_avg_per_unequal")
    vnames = [f"x_{i}" for i in range(n)]
    for v in vnames:
        oracle_solver.add_variable(v, VarType.BINARY)
    for key, idxs in groups.items():
        oracle_solver.add_constraint(
            {vnames[i]: 1.0 for i in idxs}, ">=", 1.0, name=f"sum_ge_{key}",
        )
    # Inner AVG scales coefficients by 1/n_g; outer SUM is linear.
    obj = {}
    for key, idxs in groups.items():
        n_g = len(idxs)
        for i in idxs:
            obj[vnames[i]] = data[i][2] / n_g
    oracle_solver.set_objective(obj, ObjSense.MINIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    def decidb_obj(rs, cs):
        xi = cs.index("x"); ni = cs.index("s_nationkey"); ai = cs.index("s_acctbal")
        g: dict = defaultdict(lambda: {"sum": 0.0, "n": 0})
        for r in rs:
            g[r[ni]]["n"] += 1
            if float(r[xi]) > 0.5:
                g[r[ni]]["sum"] += float(r[ai])
        return sum(v["sum"] / v["n"] for v in g.values())

    cmp = compare_solutions(
        rows, cols, result, data, ["x"],
        decidb_objective_fn=decidb_obj,
    )
    perf_tracker.record(
        "sum_avg_per_unequal_groups", decidb_time, build_time,
        result.solve_time_seconds, n, n, len(groups),
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.per_clause
@pytest.mark.min_max
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_minimize_max_avg_per(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """MINIMIZE max(per flag: avg(x*qty) by (flag)) — easy outer MAX, inner AVG."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_quantity, l_returnflag, x
        FROM lineitem WHERE l_orderkey <= 10
        DECIDE x(BOOL)
        SUCH THAT per l_returnflag: sum(x) by (l_returnflag) >= 2
        MINIMIZE max(per l_returnflag: avg(x * l_quantity) by (l_returnflag))
    """
    t0 = time.perf_counter()
    rows, cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = _fetch_lineitem_nested(duckdb_conn, "l_orderkey <= 10")
    n = len(data)
    groups = _group_rows(data, 3)
    qty_sum_all = sum(r[2] for r in data) + 1.0

    t_build = time.perf_counter()
    oracle_solver.create_model("min_max_avg_per")
    vnames = [f"x_{i}" for i in range(n)]
    for v in vnames:
        oracle_solver.add_variable(v, VarType.BINARY)
    for flag, idxs in groups.items():
        oracle_solver.add_constraint(
            {vnames[i]: 1.0 for i in idxs}, ">=", 2.0, name=f"sum_ge_{flag}",
        )
    oracle_solver.add_variable("w", VarType.CONTINUOUS, lb=0.0, ub=qty_sum_all)
    for flag, idxs in groups.items():
        n_g = len(idxs)
        link = {vnames[i]: data[i][2] / n_g for i in idxs}
        link["w"] = -1.0
        oracle_solver.add_constraint(link, "<=", 0.0, name=f"w_ge_avg_{flag}")
    oracle_solver.set_objective({"w": 1.0}, ObjSense.MINIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    def decidb_obj(rs, cs):
        xi = cs.index("x"); qi = cs.index("l_quantity"); fi = cs.index("l_returnflag")
        g: dict = defaultdict(lambda: {"sum": 0.0, "n": 0})
        for r in rs:
            g[r[fi]]["n"] += 1
            if float(r[xi]) > 0.5:
                g[r[fi]]["sum"] += float(r[qi])
        return max((v["sum"] / v["n"] for v in g.values()), default=0.0)

    cmp = compare_solutions(
        rows, cols, result, data, ["x"],
        decidb_objective_fn=decidb_obj,
    )
    perf_tracker.record(
        "minimize_max_avg_per", decidb_time, build_time, result.solve_time_seconds,
        n, n + 1, len(groups) + 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.per_clause
@pytest.mark.min_max
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_maximize_min_avg_per(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """MAXIMIZE min(per flag: avg(x*qty) by (flag)) — easy outer MIN, inner AVG."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_quantity, l_returnflag, x
        FROM lineitem WHERE l_orderkey <= 10
        DECIDE x(BOOL)
        SUCH THAT per l_returnflag: sum(x) by (l_returnflag) >= 2
        MAXIMIZE min(per l_returnflag: avg(x * l_quantity) by (l_returnflag))
    """
    t0 = time.perf_counter()
    rows, cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = _fetch_lineitem_nested(duckdb_conn, "l_orderkey <= 10")
    n = len(data)
    groups = _group_rows(data, 3)
    qty_sum_all = sum(r[2] for r in data) + 1.0

    t_build = time.perf_counter()
    oracle_solver.create_model("max_min_avg_per")
    vnames = [f"x_{i}" for i in range(n)]
    for v in vnames:
        oracle_solver.add_variable(v, VarType.BINARY)
    for flag, idxs in groups.items():
        oracle_solver.add_constraint(
            {vnames[i]: 1.0 for i in idxs}, ">=", 2.0, name=f"sum_ge_{flag}",
        )
    oracle_solver.add_variable("w", VarType.CONTINUOUS, lb=0.0, ub=qty_sum_all)
    for flag, idxs in groups.items():
        n_g = len(idxs)
        link = {vnames[i]: data[i][2] / n_g for i in idxs}
        link["w"] = -1.0
        oracle_solver.add_constraint(link, ">=", 0.0, name=f"w_le_avg_{flag}")
    oracle_solver.set_objective({"w": 1.0}, ObjSense.MAXIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    def decidb_obj(rs, cs):
        xi = cs.index("x"); qi = cs.index("l_quantity"); fi = cs.index("l_returnflag")
        g: dict = defaultdict(lambda: {"sum": 0.0, "n": 0})
        for r in rs:
            g[r[fi]]["n"] += 1
            if float(r[xi]) > 0.5:
                g[r[fi]]["sum"] += float(r[qi])
        return min((v["sum"] / v["n"] for v in g.values()), default=0.0)

    cmp = compare_solutions(
        rows, cols, result, data, ["x"],
        decidb_objective_fn=decidb_obj,
    )
    perf_tracker.record(
        "maximize_min_avg_per", decidb_time, build_time, result.solve_time_seconds,
        n, n + 1, len(groups) + 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.per_clause
@pytest.mark.when_constraint
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_sum_avg_when_per(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """MINIMIZE when qty > 10: sum(per flag: avg(x * qty) by (flag)) — the when filters
    each class's AVG; a flag with no kept row has no class."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_quantity, l_returnflag, x
        FROM lineitem WHERE l_orderkey <= 10
        DECIDE x(BOOL)
        SUCH THAT per l_returnflag: sum(x) by (l_returnflag) >= 1
        MINIMIZE when l_quantity > 10: sum(per l_returnflag: avg(x * l_quantity) by (l_returnflag))
    """
    t0 = time.perf_counter()
    rows, cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = _fetch_lineitem_nested(duckdb_conn, "l_orderkey <= 10")
    n = len(data)
    groups = _group_rows(data, 3)

    t_build = time.perf_counter()
    oracle_solver.create_model("sum_avg_when_per")
    vnames = [f"x_{i}" for i in range(n)]
    for v in vnames:
        oracle_solver.add_variable(v, VarType.BINARY)
    for flag, idxs in groups.items():
        oracle_solver.add_constraint(
            {vnames[i]: 1.0 for i in idxs}, ">=", 1.0, name=f"sum_ge_{flag}",
        )
    obj: dict = {}
    for flag, idxs in groups.items():
        qualifying = [i for i in idxs if data[i][2] > 10]
        if not qualifying:
            continue
        n_qual = len(qualifying)
        for i in qualifying:
            obj[vnames[i]] = obj.get(vnames[i], 0.0) + data[i][2] / n_qual
    oracle_solver.set_objective(obj, ObjSense.MINIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    def decidb_obj(rs, cs):
        xi = cs.index("x"); qi = cs.index("l_quantity"); fi = cs.index("l_returnflag")
        g: dict = defaultdict(lambda: {"sum": 0.0, "n": 0})
        for r in rs:
            if float(r[qi]) > 10:
                g[r[fi]]["n"] += 1
                if float(r[xi]) > 0.5:
                    g[r[fi]]["sum"] += float(r[qi])
        return sum(v["sum"] / v["n"] for v in g.values() if v["n"] > 0)

    cmp = compare_solutions(
        rows, cols, result, data, ["x"],
        decidb_objective_fn=decidb_obj,
    )
    perf_tracker.record(
        "sum_avg_when_per", decidb_time, build_time, result.solve_time_seconds,
        n, n, len(groups), result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


# ===========================================================================
# Error cases — no oracle (DecidB is expected to reject these)
# ===========================================================================

@pytest.mark.per_clause
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_sum_avg_per_extreme_unequal_groups(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """sum(per grp: avg(x*cost) by (grp)) with a 1:5 group-size ratio.

    Strengthens the coverage of ``test_sum_avg_per_unequal_groups``: that
    test's ``WHERE s_nationkey < 5`` filter yields a 3:1 max/min ratio, but
    the 1/n_g coefficient scaling is most sensitive when ratios are extreme.
    The singleton group contributes a coefficient of 1.0, while the 5-row
    group's coefficients are each 0.2 — a sign-flip-or-mis-scale bug would
    pick the wrong row between the groups.
    """
    data_sql = (
        "SELECT * FROM (VALUES "
        "(1, 'A', 2.0),"
        " (2, 'B', 3.0),"
        " (3, 'B', 4.0),"
        " (4, 'B', 5.0),"
        " (5, 'B', 6.0),"
        " (6, 'B', 7.0)"
        ") t(id, grp, cost)"
    )
    sql = f"""
        SELECT id, grp, cost, x FROM ({data_sql})
        DECIDE x(BOOL)
        SUCH THAT per grp: sum(x) by (grp) >= 1
        MINIMIZE sum(per grp: avg(x * cost) by (grp))
    """
    t0 = time.perf_counter()
    rows, cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute(
        f"SELECT CAST(id AS BIGINT), CAST(grp AS VARCHAR), CAST(cost AS DOUBLE) "
        f"FROM ({data_sql})"
    ).fetchall()
    n = len(data)
    groups = group_indices(data, lambda r: r[1])
    assert max(len(g) for g in groups.values()) / min(len(g) for g in groups.values()) >= 4, (
        "design-time: group-size ratio must be ≥ 4 to exercise the scaling "
        "coefficient under extreme asymmetry"
    )

    t_build = time.perf_counter()
    oracle_solver.create_model("sum_avg_per_extreme_unequal")
    vnames = [f"x_{i}" for i in range(n)]
    for v in vnames:
        oracle_solver.add_variable(v, VarType.BINARY)
    for key, idxs in groups.items():
        oracle_solver.add_constraint(
            {vnames[i]: 1.0 for i in idxs}, ">=", 1.0, name=f"sum_ge_{key}",
        )
    obj = {}
    for key, idxs in groups.items():
        n_g = len(idxs)
        for i in idxs:
            obj[vnames[i]] = data[i][2] / n_g
    oracle_solver.set_objective(obj, ObjSense.MINIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    def decidb_obj_fn(rs, cs):
        xi = cs.index("x"); gi = cs.index("grp"); ci = cs.index("cost")
        per_group: dict = defaultdict(lambda: {"sum": 0.0, "n": 0})
        for r in rs:
            per_group[r[gi]]["n"] += 1
            if float(r[xi]) > 0.5:
                per_group[r[gi]]["sum"] += float(r[ci])
        return sum(v["sum"] / v["n"] for v in per_group.values())

    cmp = compare_solutions(
        rows, cols, result, data, ["x"],
        decidb_objective_fn=decidb_obj_fn,
    )
    perf_tracker.record(
        "sum_avg_per_extreme_unequal_groups", decidb_time, build_time,
        result.solve_time_seconds, n, n, len(groups),
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


# ===========================================================================
# sum(per col: min/max(power(expr, 2)) by (col)) — hard inner with quadratic
# ===========================================================================
#
# Inner expression is POWER(x - target, 2); outer is SUM over groups. The
# inner aggregate is hard in the direction the outer pushes (MAXIMIZE pushes
# MAX up, MINIMIZE pushes MIN down), so per-row indicators are required —
# exactly the pattern exercised here. The quadratic POWER also means the
# physical layer must preserve squared terms rather than collapsing to
# linear auxiliaries.


_POWER_DATA_SQL = """
    SELECT 1 AS id, 'A' AS grp, 3.0 AS target UNION ALL
    SELECT 2, 'A', 7.0 UNION ALL
    SELECT 3, 'A', 5.0 UNION ALL
    SELECT 4, 'B', 2.0 UNION ALL
    SELECT 5, 'B', 8.0
"""


def _power_data(duckdb_conn):
    return duckdb_conn.execute(
        f"SELECT CAST(id AS BIGINT), CAST(grp AS VARCHAR), "
        f"CAST(target AS DOUBLE) FROM ({_POWER_DATA_SQL})"
    ).fetchall()


def _power_group_obj(rs, cs, reducer):
    """Evaluate SUM_g reducer({POWER(x_i - target_i, 2) : i in group g})
    from DecidB's output rows. Uses the full (x-t)^2 value — no constant
    stripping — so the oracle side must match."""
    xi = cs.index("x")
    gi = cs.index("grp")
    ti = cs.index("target")
    groups: dict = defaultdict(list)
    for r in rs:
        groups[r[gi]].append((float(r[xi]) - float(r[ti])) ** 2)
    return sum(reducer(vs) for vs in groups.values())


@pytest.mark.per_clause
@pytest.mark.min_max
@pytest.mark.quadratic
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_maximize_sum_max_power_per(
    decidb_cli_gurobi, duckdb_conn, oracle_solver, perf_tracker
):
    """MAXIMIZE sum(per grp: max(power(x - target, 2)) by (grp)) — hard inner MAX,
    quadratic per-row expression. Budget couples x's so at most some rows
    can reach the bound extremes; the group MAX must pick among affordable
    pushes.

    Non-convex (MAX of convex, then MAXIMIZE), so it is Gurobi-only and runs
    on ``decidb_cli_gurobi``: the backend is pinned, and the skip is decided
    up front by the fixture from the *environment* rather than by catching a
    rejection from DecidB. Catching the rejection made this test report green
    whether the shape was accepted or newly refused, which is exactly the
    expressivity regression it exists to catch. The HiGHS side of the same
    shape — the documented non-convex rejection message — is pinned in
    ``test_bilinear.py``.
    """
    sql = f"""
        WITH data AS ({_POWER_DATA_SQL})
        SELECT id, grp, target, ROUND(x, 4) AS x FROM data
        DECIDE x(REAL)
        SUCH THAT x >= 0 AND x <= 10 AND SUM(x) <= 20
        MAXIMIZE sum(per grp: max(power(x - target, 2)) by (grp))
    """
    t0 = time.perf_counter()
    rows, cols = decidb_cli_gurobi.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = _power_data(duckdb_conn)
    n = len(data)
    groups: dict = defaultdict(list)
    for i, r in enumerate(data):
        groups[r[1]].append(i)

    t_build = time.perf_counter()
    oracle_solver.create_model("sum_max_power_per")
    xnames = [f"x_{i}" for i in range(n)]
    for xn in xnames:
        oracle_solver.add_variable(xn, VarType.CONTINUOUS, lb=0.0, ub=10.0)
    oracle_solver.add_constraint(
        {xn: 1.0 for xn in xnames}, "<=", 20.0, name="budget",
    )
    # Each row's expression: POWER(x_i - t_i, 2) = x_i^2 - 2 t_i x_i + t_i^2
    # POWER is bounded by max((10 - t)^2, t^2) per row; 100 is a loose bound.
    q_ub = 100.0
    z_names = []
    for grp, idxs in groups.items():
        row_coeffs = []
        for i in idxs:
            t = float(data[i][2])
            row_coeffs.append((
                {xnames[i]: -2.0 * t},
                {(xnames[i], xnames[i]): 1.0},
                t * t,
            ))
        z = emit_hard_inner_max_quadratic(
            oracle_solver, name_prefix=f"hmaxq_{grp}",
            row_coeffs=row_coeffs, q_ub=q_ub,
        )
        z_names.append(z)
    oracle_solver.set_objective({z: 1.0 for z in z_names}, ObjSense.MAXIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        rows, cols, result, data, ["x"],
        decidb_objective_fn=lambda rs, cs: _power_group_obj(rs, cs, max),
    )
    perf_tracker.record(
        "sum_max_power_per", decidb_time, build_time,
        result.solve_time_seconds, n, 3 * n + len(groups), 2 * n + len(groups),
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.per_clause
@pytest.mark.min_max
@pytest.mark.quadratic
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_minimize_sum_min_power_per(
    decidb_cli_gurobi, duckdb_conn, oracle_solver, perf_tracker
):
    """MINIMIZE sum(per grp: min(power(x - target, 2)) by (grp)) — hard inner MIN,
    quadratic per-row expression. INTEGER x with non-integer targets forces
    every row's POWER strictly positive. Target offsets vary per row so
    each row's best-achievable POWER differs, meaning the indicator has to
    pick the *specific* minimum row in each group — a bug that selects the
    wrong row, or emits a linear auxiliary, would change the objective.

    Gurobi-only (MIQP), so it runs on ``decidb_cli_gurobi`` for the reason
    given on ``test_maximize_sum_max_power_per``: the skip belongs to the
    environment, not to what DecidB decided this run.
    """
    data_sql = """
        SELECT 1 AS id, 'A' AS grp, 3.3 AS target UNION ALL
        SELECT 2, 'A', 6.7 UNION ALL
        SELECT 3, 'A', 1.2 UNION ALL
        SELECT 4, 'B', 2.8 UNION ALL
        SELECT 5, 'B', 8.1
    """
    sql = f"""
        WITH data AS ({data_sql})
        SELECT id, grp, target, x FROM data
        DECIDE x(INT)
        SUCH THAT x >= 0 AND x <= 10
        MINIMIZE sum(per grp: min(power(x - target, 2)) by (grp))
    """
    t0 = time.perf_counter()
    rows, cols = decidb_cli_gurobi.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute(
        f"SELECT CAST(id AS BIGINT), CAST(grp AS VARCHAR), "
        f"CAST(target AS DOUBLE) FROM ({data_sql})"
    ).fetchall()
    n = len(data)
    groups: dict = defaultdict(list)
    for i, r in enumerate(data):
        groups[r[1]].append(i)

    t_build = time.perf_counter()
    oracle_solver.create_model("sum_min_power_per")
    xnames = [f"x_{i}" for i in range(n)]
    for xn in xnames:
        oracle_solver.add_variable(xn, VarType.INTEGER, lb=0.0, ub=10.0)
    q_ub = 100.0
    z_names = []
    for grp, idxs in groups.items():
        row_coeffs = []
        for i in idxs:
            t = float(data[i][2])
            row_coeffs.append((
                {xnames[i]: -2.0 * t},
                {(xnames[i], xnames[i]): 1.0},
                t * t,
            ))
        z = emit_hard_inner_min_quadratic(
            oracle_solver, name_prefix=f"hminq_{grp}",
            row_coeffs=row_coeffs, q_ub=q_ub,
        )
        z_names.append(z)
    oracle_solver.set_objective({z: 1.0 for z in z_names}, ObjSense.MINIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        rows, cols, result, data, ["x"],
        decidb_objective_fn=lambda rs, cs: _power_group_obj(rs, cs, min),
    )
    perf_tracker.record(
        "sum_min_power_per", decidb_time, build_time,
        result.solve_time_seconds, n, 3 * n + len(groups), 2 * n + len(groups),
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )
