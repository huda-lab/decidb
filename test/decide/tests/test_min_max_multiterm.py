"""MIN/MAX linking rows whose per-row expression is not a single product term.

A MIN/MAX aggregate over decision variables is linearized into an auxiliary `z`
plus one linking row per active row. Those rows are built from term-indexed
arrays, so the same solver column can reach one row more than once:

  - `(qty + 1) * x` distributes into `qty*x + 1*x` — two terms, one column
  - `qty*x + x` — the same shape written out
  - an entity-scoped variable spans several rows of one PER group, so even a
    single term lands on one column repeatedly

A repeated column index is rejected outright by Gurobi and HiGHS, so every
shape below used to fail before reaching the solver. A bare constant term
(`qty*x + 5`) carries no column at all and used to be read as a variable index.

Each test pins the *objective value* against an independent oracle model, so a
row that is merely accepted but mis-linearized still fails.

Covers:
  - test_flat_min_max_additive_coefficient: MINIMIZE MAX((qty+1)*x), easy
  - test_flat_min_max_repeated_variable: MINIMIZE MAX(qty*x + x), easy
  - test_flat_min_max_constant_term: MINIMIZE MAX(qty*x + 5), constant folding
  - test_flat_max_additive_coefficient_hard: MAXIMIZE MAX((qty+1)*x), hard
  - test_flat_min_additive_coefficient_hard: MINIMIZE MIN((qty+1)*x), hard
  - test_composed_min_max_additive_coefficient: MAX((qty+1)*x) + SUM(x)
  - test_per_outer_min_entity_scoped: MIN(SUM(keepN*acctbal)) PER segment
  - test_per_sum_outer_min_inner_additive_coefficient: SUM(MIN((qty+1)*x)) PER flag
  - test_per_sum_outer_min_inner_repeated_variable: SUM(MIN(qty*x + x)) PER flag
  - test_per_sum_outer_max_inner_additive_coefficient: SUM(MAX((qty+1)*x)) PER flag
"""

import time

import pytest

from solver.types import VarType, ObjSense, SolverStatus
from comparison.compare import compare_solutions
from ._oracle_helpers import (
    emit_hard_inner_max,
    emit_hard_inner_min,
    emit_inner_max,
    emit_inner_min,
    group_indices,
)


LINEITEM_FILTER = "l_orderkey <= 10"


def _fetch_lineitem(conn):
    """Rows of (orderkey, linenumber, quantity) in query order."""
    return conn.execute(f"""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_quantity AS DOUBLE)
        FROM lineitem WHERE {LINEITEM_FILTER}
    """).fetchall()


def _row_max(rows, cols, value_fn):
    """MAX over rows of a per-row expression built from DecidB's output."""
    return max(value_fn(row, cols) for row in rows)


def _row_min(rows, cols, value_fn):
    return min(value_fn(row, cols) for row in rows)


def _fetch_lineitem_with_flag(conn):
    """Rows of (orderkey, linenumber, quantity, returnflag) in query order."""
    return conn.execute(f"""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_quantity AS DOUBLE),
               l_returnflag
        FROM lineitem WHERE {LINEITEM_FILTER}
    """).fetchall()


def _per_group_extreme(rows, cols, pick):
    """`pick` (min/max) applied within each l_returnflag group of DecidB's output."""
    flag_i = cols.index("l_returnflag")
    qty_i = cols.index("l_quantity")
    x_i = cols.index("x")
    per_group = {}
    for row in rows:
        per_group.setdefault(row[flag_i], []).append(
            (float(row[qty_i]) + 1.0) * int(row[x_i])
        )
    return sum(pick(vals) for vals in per_group.values())


# ===========================================================================
# Flat MIN/MAX objective — easy direction
# ===========================================================================

@pytest.mark.min_max
@pytest.mark.var_boolean
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_flat_min_max_additive_coefficient(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """MINIMIZE MAX((l_quantity + 1) * x) — coefficient distributes into two terms.

    Minimizing the worst selected row drives the choice onto the two smallest
    quantities, so the optimum distinguishes a correct linearization from a
    vacuous one.
    """
    sql = """
        SELECT l_orderkey, l_linenumber, l_quantity, x
        FROM lineitem WHERE l_orderkey <= 10
        DECIDE x(BOOL)
        SUCH THAT SUM(x) >= 2
        MINIMIZE MAX((l_quantity + 1) * x)
    """
    t0 = time.perf_counter()
    rows, cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = _fetch_lineitem(duckdb_conn)
    n = len(data)
    ub = max(r[2] for r in data) + 1.0

    t_build = time.perf_counter()
    oracle_solver.create_model("flat_max_additive_coeff")
    vnames = [f"x_{i}" for i in range(n)]
    for v in vnames:
        oracle_solver.add_variable(v, VarType.BINARY)
    oracle_solver.add_constraint(
        {v: 1.0 for v in vnames}, ">=", 2.0, name="sum_ge_2",
    )
    oracle_solver.add_variable("z", VarType.CONTINUOUS, lb=0.0, ub=ub)
    for i in range(n):
        oracle_solver.add_constraint(
            {"z": 1.0, vnames[i]: -(data[i][2] + 1.0)}, ">=", 0.0,
            name=f"z_ge_{i}",
        )
    oracle_solver.set_objective({"z": 1.0}, ObjSense.MINIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    def decidb_obj(rs, cs):
        return _row_max(
            rs, cs,
            lambda r, c: (float(r[c.index("l_quantity")]) + 1.0) * int(r[c.index("x")]),
        )

    cmp = compare_solutions(
        rows, cols, result, data, ["x"], decidb_objective_fn=decidb_obj,
    )
    perf_tracker.record(
        "flat_max_additive_coeff", decidb_time, build_time,
        result.solve_time_seconds, n, n + 1, n + 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.min_max
@pytest.mark.var_boolean
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_flat_min_max_repeated_variable(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """MINIMIZE MAX(l_quantity * x + x) — the same variable in two written terms.

    Algebraically identical to `MAX((l_quantity + 1) * x)`; written out so the
    two terms reach the linking row without an intervening distribution.
    """
    sql = """
        SELECT l_orderkey, l_linenumber, l_quantity, x
        FROM lineitem WHERE l_orderkey <= 10
        DECIDE x(BOOL)
        SUCH THAT SUM(x) >= 2
        MINIMIZE MAX(l_quantity * x + x)
    """
    t0 = time.perf_counter()
    rows, cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = _fetch_lineitem(duckdb_conn)
    n = len(data)
    ub = max(r[2] for r in data) + 1.0

    t_build = time.perf_counter()
    oracle_solver.create_model("flat_max_repeated_var")
    vnames = [f"x_{i}" for i in range(n)]
    for v in vnames:
        oracle_solver.add_variable(v, VarType.BINARY)
    oracle_solver.add_constraint(
        {v: 1.0 for v in vnames}, ">=", 2.0, name="sum_ge_2",
    )
    oracle_solver.add_variable("z", VarType.CONTINUOUS, lb=0.0, ub=ub)
    for i in range(n):
        oracle_solver.add_constraint(
            {"z": 1.0, vnames[i]: -(data[i][2] + 1.0)}, ">=", 0.0,
            name=f"z_ge_{i}",
        )
    oracle_solver.set_objective({"z": 1.0}, ObjSense.MINIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    def decidb_obj(rs, cs):
        return _row_max(
            rs, cs,
            lambda r, c: float(r[c.index("l_quantity")]) * int(r[c.index("x")])
            + int(r[c.index("x")]),
        )

    cmp = compare_solutions(
        rows, cols, result, data, ["x"], decidb_objective_fn=decidb_obj,
    )
    perf_tracker.record(
        "flat_max_repeated_var", decidb_time, build_time,
        result.solve_time_seconds, n, n + 1, n + 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.min_max
@pytest.mark.var_boolean
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_flat_min_max_constant_term(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """MINIMIZE MAX(l_quantity * x + 5) — a bare constant inside the aggregate.

    The constant carries no solver column; it belongs on the linking row's
    bound. `MAX(expr + 5)` is `MAX(expr) + 5`, so the optimum shifts by exactly
    5 against the same selection.
    """
    sql = """
        SELECT l_orderkey, l_linenumber, l_quantity, x
        FROM lineitem WHERE l_orderkey <= 10
        DECIDE x(BOOL)
        SUCH THAT SUM(x) >= 2
        MINIMIZE MAX(l_quantity * x + 5)
    """
    t0 = time.perf_counter()
    rows, cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = _fetch_lineitem(duckdb_conn)
    n = len(data)
    ub = max(r[2] for r in data) + 5.0

    t_build = time.perf_counter()
    oracle_solver.create_model("flat_max_constant_term")
    vnames = [f"x_{i}" for i in range(n)]
    for v in vnames:
        oracle_solver.add_variable(v, VarType.BINARY)
    oracle_solver.add_constraint(
        {v: 1.0 for v in vnames}, ">=", 2.0, name="sum_ge_2",
    )
    oracle_solver.add_variable("z", VarType.CONTINUOUS, lb=0.0, ub=ub)
    for i in range(n):
        # z >= qty_i * x_i + 5
        oracle_solver.add_constraint(
            {"z": 1.0, vnames[i]: -data[i][2]}, ">=", 5.0, name=f"z_ge_{i}",
        )
    oracle_solver.set_objective({"z": 1.0}, ObjSense.MINIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    def decidb_obj(rs, cs):
        return _row_max(
            rs, cs,
            lambda r, c: float(r[c.index("l_quantity")]) * int(r[c.index("x")]) + 5.0,
        )

    cmp = compare_solutions(
        rows, cols, result, data, ["x"], decidb_objective_fn=decidb_obj,
    )
    perf_tracker.record(
        "flat_max_constant_term", decidb_time, build_time,
        result.solve_time_seconds, n, n + 1, n + 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


# ===========================================================================
# Flat MIN/MAX objective — hard direction (indicator + Big-M path)
# ===========================================================================

@pytest.mark.min_max
@pytest.mark.var_boolean
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_flat_max_additive_coefficient_hard(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """MAXIMIZE MAX((l_quantity + 1) * x) — hard direction, multi-term row expr.

    The hard direction routes through the one-hot indicator encoding, a
    separate emission site from the easy direction above.
    """
    sql = """
        SELECT l_orderkey, l_linenumber, l_quantity, x
        FROM lineitem WHERE l_orderkey <= 10
        DECIDE x(BOOL)
        SUCH THAT SUM(x) <= 2
        MAXIMIZE MAX((l_quantity + 1) * x)
    """
    t0 = time.perf_counter()
    rows, cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = _fetch_lineitem(duckdb_conn)
    n = len(data)
    ub = max(r[2] for r in data) + 1.0

    t_build = time.perf_counter()
    oracle_solver.create_model("flat_max_additive_hard")
    vnames = [f"x_{i}" for i in range(n)]
    for v in vnames:
        oracle_solver.add_variable(v, VarType.BINARY)
    oracle_solver.add_constraint(
        {v: 1.0 for v in vnames}, "<=", 2.0, name="sum_le_2",
    )
    z = emit_hard_inner_max(
        oracle_solver, "obj",
        [{vnames[i]: data[i][2] + 1.0} for i in range(n)],
        ub,
    )
    oracle_solver.set_objective({z: 1.0}, ObjSense.MAXIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    def decidb_obj(rs, cs):
        return _row_max(
            rs, cs,
            lambda r, c: (float(r[c.index("l_quantity")]) + 1.0) * int(r[c.index("x")]),
        )

    cmp = compare_solutions(
        rows, cols, result, data, ["x"], decidb_objective_fn=decidb_obj,
    )
    perf_tracker.record(
        "flat_max_additive_hard", decidb_time, build_time,
        result.solve_time_seconds, n, 2 * n + 1, 2 * n + 2,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.min_max
@pytest.mark.var_boolean
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_flat_min_additive_coefficient_hard(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """MINIMIZE MIN((l_quantity + 1) * x) — the other hard direction.

    Forcing every row on (`SUM(x) >= n`) keeps the MIN off the trivial zero
    that an unselected row would otherwise contribute.
    """
    data = _fetch_lineitem(duckdb_conn)
    n = len(data)
    sql = f"""
        SELECT l_orderkey, l_linenumber, l_quantity, x
        FROM lineitem WHERE l_orderkey <= 10
        DECIDE x(BOOL)
        SUCH THAT SUM(x) >= {n}
        MINIMIZE MIN((l_quantity + 1) * x)
    """
    t0 = time.perf_counter()
    rows, cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    ub = max(r[2] for r in data) + 1.0

    t_build = time.perf_counter()
    oracle_solver.create_model("flat_min_additive_hard")
    vnames = [f"x_{i}" for i in range(n)]
    for v in vnames:
        oracle_solver.add_variable(v, VarType.BINARY)
    oracle_solver.add_constraint(
        {v: 1.0 for v in vnames}, ">=", float(n), name="sum_ge_all",
    )
    z = emit_hard_inner_min(
        oracle_solver, "obj",
        [{vnames[i]: data[i][2] + 1.0} for i in range(n)],
        ub,
    )
    oracle_solver.set_objective({z: 1.0}, ObjSense.MINIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    def decidb_obj(rs, cs):
        return _row_min(
            rs, cs,
            lambda r, c: (float(r[c.index("l_quantity")]) + 1.0) * int(r[c.index("x")]),
        )

    cmp = compare_solutions(
        rows, cols, result, data, ["x"], decidb_objective_fn=decidb_obj,
    )
    perf_tracker.record(
        "flat_min_additive_hard", decidb_time, build_time,
        result.solve_time_seconds, n, 2 * n + 1, 2 * n + 2,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


# ===========================================================================
# Composed MIN/MAX objective — MIN/MAX term added to a SUM term
# ===========================================================================

@pytest.mark.min_max
@pytest.mark.var_boolean
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_composed_min_max_additive_coefficient(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """MINIMIZE MAX((l_quantity + 1) * x) + SUM(x) — composed additive objective.

    The composed path allocates its own z per MIN/MAX term and pins it from a
    separate emission site than the flat path.
    """
    sql = """
        SELECT l_orderkey, l_linenumber, l_quantity, x
        FROM lineitem WHERE l_orderkey <= 10
        DECIDE x(BOOL)
        SUCH THAT SUM(x) >= 2
        MINIMIZE MAX((l_quantity + 1) * x) + SUM(x)
    """
    t0 = time.perf_counter()
    rows, cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = _fetch_lineitem(duckdb_conn)
    n = len(data)
    ub = max(r[2] for r in data) + 1.0

    t_build = time.perf_counter()
    oracle_solver.create_model("composed_max_additive_coeff")
    vnames = [f"x_{i}" for i in range(n)]
    for v in vnames:
        oracle_solver.add_variable(v, VarType.BINARY)
    oracle_solver.add_constraint(
        {v: 1.0 for v in vnames}, ">=", 2.0, name="sum_ge_2",
    )
    oracle_solver.add_variable("z", VarType.CONTINUOUS, lb=0.0, ub=ub)
    for i in range(n):
        oracle_solver.add_constraint(
            {"z": 1.0, vnames[i]: -(data[i][2] + 1.0)}, ">=", 0.0,
            name=f"z_ge_{i}",
        )
    obj = {v: 1.0 for v in vnames}
    obj["z"] = 1.0
    oracle_solver.set_objective(obj, ObjSense.MINIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    def decidb_obj(rs, cs):
        worst = _row_max(
            rs, cs,
            lambda r, c: (float(r[c.index("l_quantity")]) + 1.0) * int(r[c.index("x")]),
        )
        return worst + sum(int(r[cs.index("x")]) for r in rs)

    cmp = compare_solutions(
        rows, cols, result, data, ["x"], decidb_objective_fn=decidb_obj,
    )
    perf_tracker.record(
        "composed_max_additive_coeff", decidb_time, build_time,
        result.solve_time_seconds, n, n + 1, n + 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


# ===========================================================================
# PER objective, outer MIN over group sums, entity-scoped variable
# ===========================================================================

@pytest.mark.min_max
@pytest.mark.per_clause
@pytest.mark.var_boolean
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_per_outer_min_entity_scoped(decidb_cli, duckdb_conn, oracle_solver):
    """MAXIMIZE MIN(SUM(keepN * c_acctbal)) PER c_mktsegment, entity-scoped keepN.

    One group spans many rows, and an entity-scoped variable resolves to a
    single solver column across all of them — so one term is enough to place
    that column in the group's linking row repeatedly. No multi-term
    expression is involved.
    """
    sql = """
        SELECT c.c_custkey, n.n_nationkey, c.c_acctbal, c.c_mktsegment, keepN
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0
        DECIDE n.keepN(BOOL)
        SUCH THAT SUM(keepN) <= 150
        MAXIMIZE MIN(SUM(keepN * c.c_acctbal)) PER c.c_mktsegment
    """
    rows, cols = decidb_cli.execute(sql)
    assert len(rows) > 0

    data = duckdb_conn.execute("""
        SELECT CAST(n.n_nationkey AS BIGINT),
               CAST(c.c_acctbal AS DOUBLE),
               CAST(c.c_mktsegment AS VARCHAR)
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0
    """).fetchall()

    nations = sorted({int(r[0]) for r in data})
    segments = sorted({r[2] for r in data})
    bound = sum(abs(r[1]) for r in data) + 1.0

    oracle_solver.create_model("per_outer_min_entity")
    vnames = {nk: f"keepN_{nk}" for nk in nations}
    for nk in nations:
        oracle_solver.add_variable(vnames[nk], VarType.BINARY)

    # SUM(keepN) over join rows: each nation contributes its join-row count.
    counts = {nk: sum(1 for r in data if int(r[0]) == nk) for nk in nations}
    oracle_solver.add_constraint(
        {vnames[nk]: float(counts[nk]) for nk in nations},
        "<=", 150.0, name="sum_le_150",
    )

    # w <= per-segment sum, for every segment; maximize w (easy outer MIN).
    oracle_solver.add_variable("w", VarType.CONTINUOUS, lb=-bound, ub=bound)
    for seg in segments:
        link = {}
        for nk, acctbal, s in ((int(r[0]), r[1], r[2]) for r in data):
            if s != seg:
                continue
            link[vnames[nk]] = link.get(vnames[nk], 0.0) + acctbal
        link["w"] = -1.0
        oracle_solver.add_constraint(link, ">=", 0.0, name=f"w_le_{seg}")
    oracle_solver.set_objective({"w": 1.0}, ObjSense.MAXIMIZE)
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL

    keep_i = cols.index("keepN")
    acct_i = cols.index("c_acctbal")
    seg_i = cols.index("c_mktsegment")
    nkey_i = cols.index("n_nationkey")

    # Entity consistency: one value per nation across all its join rows.
    per_nation = {}
    for row in rows:
        nk = int(row[nkey_i])
        keep = int(row[keep_i])
        assert per_nation.setdefault(nk, keep) == keep, (
            f"Nation {nk} has inconsistent keepN across join rows"
        )

    group_sums = {}
    for row in rows:
        seg = row[seg_i]
        group_sums[seg] = group_sums.get(seg, 0.0) + int(row[keep_i]) * float(row[acct_i])
    decidb_obj = min(group_sums.values())

    assert abs(decidb_obj - result.objective_value) < 1e-3, (
        f"Objective mismatch: DecidB={decidb_obj:.4f}, "
        f"Oracle={result.objective_value:.4f}"
    )


# ===========================================================================
# PER objective, SUM outer, MIN/MAX inner, multi-term row expression
#
# Regression coverage for the `SUM(MIN(expr)) PER col` bind-time bug: the
# outer-SUM nested form used to reject a multi-term inner MIN/MAX argument
# (`MIN((qty+1)*x)`, `MIN(qty*x + x)`) with a binder error naming syntax the
# user never wrote (`sum((x * (min(l.qty) + __MIN__)))`) — the CAS
# distributed the `__MIN__`/`__MAX__` marker across the inner sum before it
# could be reassembled into a whole aggregate. The single-term inner
# (`MIN(qty*x)`, already covered above) and the outer-MIN/MAX-instead-of-SUM
# forms were unaffected; only outer SUM + multi-term inner MIN/MAX hit it.
# See done.md → "SUM(MIN(expr)) PER col rejected a multi-term inner
# MIN/MAX argument at bind time".
# ===========================================================================

@pytest.mark.min_max
@pytest.mark.per_clause
@pytest.mark.var_boolean
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_per_sum_outer_min_inner_additive_coefficient(
    decidb_cli, duckdb_conn, oracle_solver
):
    """MAXIMIZE SUM(MIN((l_quantity + 1) * x)) PER l_returnflag.

    Outer SUM over groups, inner MIN over a two-term row expression — the
    exact shape that used to fail at bind time. Requiring at least one active
    row per group keeps every group's MIN strictly positive, so a scrambled
    linearization (which would silently zero out or misattribute a group's
    contribution) shows up in the objective value, not just in whether the
    query binds at all.
    """
    data = _fetch_lineitem_with_flag(duckdb_conn)
    n = len(data)
    groups = group_indices(data, lambda r: r[3])

    sql = """
        SELECT l_orderkey, l_linenumber, l_quantity, l_returnflag, x
        FROM lineitem WHERE l_orderkey <= 10
        DECIDE x(BOOL)
        SUCH THAT SUM(x) WHEN (l_returnflag = 'A') >= 1
              AND SUM(x) WHEN (l_returnflag = 'N') >= 1
              AND SUM(x) WHEN (l_returnflag = 'R') >= 1
        MAXIMIZE SUM(MIN((l_quantity + 1) * x)) PER l_returnflag
    """
    rows, cols = decidb_cli.execute(sql)

    ub = max(r[2] for r in data) + 1.0
    oracle_solver.create_model("per_sum_outer_min_inner_additive_coeff")
    vnames = [f"x_{i}" for i in range(n)]
    for v in vnames:
        oracle_solver.add_variable(v, VarType.BINARY)
    for flag, idxs in groups.items():
        oracle_solver.add_constraint(
            {vnames[i]: 1.0 for i in idxs}, ">=", 1.0, name=f"sum_ge_1_{flag}",
        )
    z_names = []
    for flag, idxs in groups.items():
        row_coeffs = [{vnames[i]: data[i][2] + 1.0} for i in idxs]
        z_names.append(emit_inner_min(oracle_solver, f"grp_{flag}", row_coeffs, ub))
    oracle_solver.set_objective({z: 1.0 for z in z_names}, ObjSense.MAXIMIZE)
    result = oracle_solver.solve()

    def decidb_obj(rs, cs):
        return _per_group_extreme(rs, cs, min)

    cmp = compare_solutions(
        rows, cols, result, data, ["x"], decidb_objective_fn=decidb_obj,
    )
    assert cmp.status in ("identical", "optimal")


@pytest.mark.min_max
@pytest.mark.per_clause
@pytest.mark.var_boolean
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_per_sum_outer_min_inner_repeated_variable(
    decidb_cli, duckdb_conn, oracle_solver
):
    """MAXIMIZE SUM(MIN(l_quantity * x + x)) PER l_returnflag.

    Algebraically identical to `SUM(MIN((l_quantity + 1) * x)) PER
    l_returnflag` above; written out as two separate terms sharing the same
    variable, since that's the second shape the bug report called out
    (`MIN(l.qty * x + x)`), distinct from the coefficient-distribution shape.
    """
    data = _fetch_lineitem_with_flag(duckdb_conn)
    n = len(data)
    groups = group_indices(data, lambda r: r[3])

    sql = """
        SELECT l_orderkey, l_linenumber, l_quantity, l_returnflag, x
        FROM lineitem WHERE l_orderkey <= 10
        DECIDE x(BOOL)
        SUCH THAT SUM(x) WHEN (l_returnflag = 'A') >= 1
              AND SUM(x) WHEN (l_returnflag = 'N') >= 1
              AND SUM(x) WHEN (l_returnflag = 'R') >= 1
        MAXIMIZE SUM(MIN(l_quantity * x + x)) PER l_returnflag
    """
    rows, cols = decidb_cli.execute(sql)

    ub = max(r[2] for r in data) + 1.0
    oracle_solver.create_model("per_sum_outer_min_inner_repeated_var")
    vnames = [f"x_{i}" for i in range(n)]
    for v in vnames:
        oracle_solver.add_variable(v, VarType.BINARY)
    for flag, idxs in groups.items():
        oracle_solver.add_constraint(
            {vnames[i]: 1.0 for i in idxs}, ">=", 1.0, name=f"sum_ge_1_{flag}",
        )
    z_names = []
    for flag, idxs in groups.items():
        row_coeffs = [{vnames[i]: data[i][2] + 1.0} for i in idxs]
        z_names.append(emit_inner_min(oracle_solver, f"grp_{flag}", row_coeffs, ub))
    oracle_solver.set_objective({z: 1.0 for z in z_names}, ObjSense.MAXIMIZE)
    result = oracle_solver.solve()

    def decidb_obj(rs, cs):
        return _per_group_extreme(rs, cs, min)

    cmp = compare_solutions(
        rows, cols, result, data, ["x"], decidb_objective_fn=decidb_obj,
    )
    assert cmp.status in ("identical", "optimal")


@pytest.mark.min_max
@pytest.mark.per_clause
@pytest.mark.var_boolean
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_per_sum_outer_max_inner_additive_coefficient(
    decidb_cli, duckdb_conn, oracle_solver
):
    """MINIMIZE SUM(MAX((l_quantity + 1) * x)) PER l_returnflag.

    The other direction named in the bug report: outer SUM, inner MAX, sense
    MINIMIZE. Requiring at least one active row per group keeps the optimal
    strategy non-trivial (pick the single lowest-quantity row per group
    rather than collapsing everything to zero).
    """
    data = _fetch_lineitem_with_flag(duckdb_conn)
    n = len(data)
    groups = group_indices(data, lambda r: r[3])

    sql = """
        SELECT l_orderkey, l_linenumber, l_quantity, l_returnflag, x
        FROM lineitem WHERE l_orderkey <= 10
        DECIDE x(BOOL)
        SUCH THAT SUM(x) WHEN (l_returnflag = 'A') >= 1
              AND SUM(x) WHEN (l_returnflag = 'N') >= 1
              AND SUM(x) WHEN (l_returnflag = 'R') >= 1
        MINIMIZE SUM(MAX((l_quantity + 1) * x)) PER l_returnflag
    """
    rows, cols = decidb_cli.execute(sql)

    ub = max(r[2] for r in data) + 1.0
    oracle_solver.create_model("per_sum_outer_max_inner_additive_coeff")
    vnames = [f"x_{i}" for i in range(n)]
    for v in vnames:
        oracle_solver.add_variable(v, VarType.BINARY)
    for flag, idxs in groups.items():
        oracle_solver.add_constraint(
            {vnames[i]: 1.0 for i in idxs}, ">=", 1.0, name=f"sum_ge_1_{flag}",
        )
    z_names = []
    for flag, idxs in groups.items():
        row_coeffs = [{vnames[i]: data[i][2] + 1.0} for i in idxs]
        z_names.append(emit_inner_max(oracle_solver, f"grp_{flag}", row_coeffs, ub))
    oracle_solver.set_objective({z: 1.0 for z in z_names}, ObjSense.MINIMIZE)
    result = oracle_solver.solve()

    def decidb_obj(rs, cs):
        return _per_group_extreme(rs, cs, max)

    cmp = compare_solutions(
        rows, cols, result, data, ["x"], decidb_objective_fn=decidb_obj,
    )
    assert cmp.status in ("identical", "optimal")
