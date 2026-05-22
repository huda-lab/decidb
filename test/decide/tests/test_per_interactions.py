"""Cross-feature PER interaction tests.

PER composed with auxiliary-variable features (Big-M indicators for hard
MIN/MAX constraints, ABS auxiliary variables, multi-variable coefficient
extraction) is the weakest area in the test suite. Each rewrite must
partition its auxiliary state per PER group; a global-vs-grouped scoping
bug produces silently wrong results.

Each test uses oracle compare to detect optimality bugs. A globally-scoped
auxiliary-variable bug would relax the per-group constraints, producing a
better objective for MAXIMIZE / worse for MINIMIZE than the correct value.

Covered:
  - test_per_max_geq_constraint:     PER + hard MAX(>=K) — Big-M per group
  - test_per_min_leq_constraint:     PER + hard MIN(<=K) — Big-M per group
  - test_per_max_eq_constraint:      PER + equality MAX(=K) — easy + hard combined
  - test_per_abs_aggregate:          PER + SUM(ABS(...)) — ABS aux per group
  - test_per_multi_variable:         PER + (BOOLEAN + INTEGER) — multi-var indexing
  - test_when_per_multi_variable:    WHEN + PER + (BOOLEAN + INTEGER) — WHEN mask per group per variable
  - test_qp_objective_per_constraint: QP objective + PER constraint — QP path alongside PER
  - test_per_single_row_groups:      PER with |group| = 1 — degenerate group cardinality
  - test_per_zero_coefficient_group: PER where one group's aggregate is vacuous (all-zero coeffs)
  - test_per_null_group_with_when:   NULL PER key + WHEN mask — NULL bucket interacts with WHEN→PER empty-skip
"""

import time

import pytest

from solver.types import VarType, ObjSense, SolverStatus


# ============================================================================
# 1.1a — PER + hard MAX(>=K) constraint
# ============================================================================

@pytest.mark.per_clause
@pytest.mark.min_max
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_per_max_geq_constraint(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """MAX(x * l_quantity) >= 30 PER l_returnflag — Big-M indicators must be
    partitioned per group. A global-scoping bug would let one row's indicator
    satisfy all groups simultaneously."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_quantity, l_extendedprice,
               l_returnflag, x
        FROM lineitem WHERE l_orderkey <= 10
        DECIDE x IS BOOLEAN
        SUCH THAT MAX(x * l_quantity) >= 30 PER l_returnflag
        MINIMIZE SUM(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_quantity AS DOUBLE),
               CAST(l_extendedprice AS DOUBLE),
               CAST(l_returnflag AS VARCHAR)
        FROM lineitem WHERE l_orderkey <= 10
        ORDER BY l_orderkey, l_linenumber
    """).fetchall()
    n = len(data)
    K = 30.0

    t_build = time.perf_counter()
    oracle_solver.create_model("per_max_geq")
    xnames = [f"x_{i}" for i in range(n)]
    ynames = [f"y_{i}" for i in range(n)]
    for vn in xnames + ynames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    # Big-M per row: qty_i*x_i - K*y_i >= 0
    # (with M = K, lower bound of x*qty = 0)
    # When y_i=1: qty_i*x_i >= K  → forces x_i=1 AND qty_i >= K
    # When y_i=0: qty_i*x_i >= 0  (trivial)
    for i in range(n):
        qty = data[i][2]
        oracle_solver.add_constraint(
            {xnames[i]: qty, ynames[i]: -K}, ">=", 0.0, name=f"link_{i}",
        )

    # Per-group: SUM(y_i) >= 1
    groups: dict[str, list[int]] = {}
    for i, row in enumerate(data):
        groups.setdefault(row[4], []).append(i)
    for g, idxs in groups.items():
        oracle_solver.add_constraint(
            {ynames[i]: 1.0 for i in idxs}, ">=", 1.0, name=f"grp_{g}",
        )

    oracle_solver.set_objective(
        {xnames[i]: data[i][3] for i in range(n)}, ObjSense.MINIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    assert result.status == SolverStatus.OPTIMAL

    ci = {name: i for i, name in enumerate(decidb_cols)}
    decidb_obj = sum(
        int(row[ci["x"]]) * float(row[ci["l_extendedprice"]])
        for row in decidb_result
    )
    assert abs(decidb_obj - result.objective_value) <= 1.0, (
        f"Objective mismatch: DecidB={decidb_obj:.2f}, "
        f"Oracle={result.objective_value:.2f}"
    )

    # Sanity: each group has at least one row with x=1 AND qty>=30
    by_grp: dict[str, list[tuple[int, float]]] = {}
    for row in decidb_result:
        flag = str(row[ci["l_returnflag"]])
        by_grp.setdefault(flag, []).append(
            (int(row[ci["x"]]), float(row[ci["l_quantity"]]))
        )
    for g, rows in by_grp.items():
        assert any(x == 1 and qty >= K for x, qty in rows), (
            f"Group {g} has no x=1 row with qty>={K} — per-group MAX>=K violated"
        )

    perf_tracker.record(
        "per_max_geq", decidb_time, build_time,
        result.solve_time_seconds, n, n * 2, n + len(groups),
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )


# ============================================================================
# 1.1b — PER + hard MIN(<=K) constraint
# ============================================================================

@pytest.mark.per_clause
@pytest.mark.min_max
@pytest.mark.var_integer
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_per_min_leq_constraint(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """MIN(x * l_quantity) <= 20 PER l_returnflag with INTEGER x in [1,5].
    The lower bound x>=1 prevents the trivial x=0 escape, so the MIN constraint
    actively limits at least one row's x value per group. Big-M indicators must
    be per-group."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_quantity, l_extendedprice,
               l_returnflag, x
        FROM lineitem WHERE l_orderkey <= 10
        DECIDE x IS INTEGER
        SUCH THAT x >= 1 AND x <= 5
            AND MIN(x * l_quantity) <= 20 PER l_returnflag
        MAXIMIZE SUM(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_quantity AS DOUBLE),
               CAST(l_extendedprice AS DOUBLE),
               CAST(l_returnflag AS VARCHAR)
        FROM lineitem WHERE l_orderkey <= 10
        ORDER BY l_orderkey, l_linenumber
    """).fetchall()
    n = len(data)
    K = 20.0
    X_LB, X_UB = 1, 5
    qty_max = max(d[2] for d in data)
    M = X_UB * qty_max - K  # so when y=0, x*qty <= K + M = X_UB*qty_max ✓

    t_build = time.perf_counter()
    oracle_solver.create_model("per_min_leq")
    xnames = [f"x_{i}" for i in range(n)]
    ynames = [f"y_{i}" for i in range(n)]
    for vn in xnames:
        oracle_solver.add_variable(vn, VarType.INTEGER, lb=X_LB, ub=X_UB)
    for vn in ynames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    # Big-M per row: qty_i*x_i + M*y_i <= K + M   (rewrite of x*qty <= K + M*(1-y))
    # When y_i=1: qty_i*x_i <= K
    # When y_i=0: qty_i*x_i <= K + M = X_UB*qty_max  (trivial upper bound)
    for i in range(n):
        qty = data[i][2]
        oracle_solver.add_constraint(
            {xnames[i]: qty, ynames[i]: M}, "<=", K + M, name=f"link_{i}",
        )

    # Per-group: SUM(y_i) >= 1
    groups: dict[str, list[int]] = {}
    for i, row in enumerate(data):
        groups.setdefault(row[4], []).append(i)
    for g, idxs in groups.items():
        oracle_solver.add_constraint(
            {ynames[i]: 1.0 for i in idxs}, ">=", 1.0, name=f"grp_{g}",
        )

    oracle_solver.set_objective(
        {xnames[i]: data[i][3] for i in range(n)}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    assert result.status == SolverStatus.OPTIMAL

    ci = {name: i for i, name in enumerate(decidb_cols)}
    decidb_obj = sum(
        int(row[ci["x"]]) * float(row[ci["l_extendedprice"]])
        for row in decidb_result
    )
    assert abs(decidb_obj - result.objective_value) <= 1.0, (
        f"Objective mismatch: DecidB={decidb_obj:.2f}, "
        f"Oracle={result.objective_value:.2f}"
    )

    # Sanity: each group has at least one row with x*qty <= K
    by_grp: dict[str, list[float]] = {}
    for row in decidb_result:
        flag = str(row[ci["l_returnflag"]])
        x_val = int(row[ci["x"]])
        qty = float(row[ci["l_quantity"]])
        by_grp.setdefault(flag, []).append(x_val * qty)
    for g, vals in by_grp.items():
        assert min(vals) <= K + 1e-6, (
            f"Group {g} MIN(x*qty)={min(vals)} > {K} — per-group MIN<=K violated"
        )

    perf_tracker.record(
        "per_min_leq", decidb_time, build_time,
        result.solve_time_seconds, n, n * 2, n + len(groups),
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )


# ============================================================================
# 1.1c — PER + equality MAX(=K) constraint (easy + hard combined)
# ============================================================================

@pytest.mark.per_clause
@pytest.mark.min_max
@pytest.mark.var_integer
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_per_max_eq_constraint(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """MAX(x * l_quantity) = 30 PER l_returnflag with INTEGER x in [0,5].
    Combines easy direction (per-row x*qty <= 30) with hard direction
    (per-group existence of x*qty = 30). Both must be partitioned per group
    for the equality semantics to hold per group. Uses l_orderkey <= 100 to
    ensure each returnflag group contains at least one (qty,x) factorization
    of 30."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_quantity, l_extendedprice,
               l_returnflag, x
        FROM lineitem WHERE l_orderkey <= 100
        DECIDE x IS INTEGER
        SUCH THAT x >= 0 AND x <= 5
            AND MAX(x * l_quantity) = 30 PER l_returnflag
        MAXIMIZE SUM(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_quantity AS DOUBLE),
               CAST(l_extendedprice AS DOUBLE),
               CAST(l_returnflag AS VARCHAR)
        FROM lineitem WHERE l_orderkey <= 100
        ORDER BY l_orderkey, l_linenumber
    """).fetchall()
    n = len(data)
    K = 30.0
    X_LB, X_UB = 0, 5

    t_build = time.perf_counter()
    oracle_solver.create_model("per_max_eq")
    xnames = [f"x_{i}" for i in range(n)]
    ynames = [f"y_{i}" for i in range(n)]
    for vn in xnames:
        oracle_solver.add_variable(vn, VarType.INTEGER, lb=X_LB, ub=X_UB)
    for vn in ynames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    # Easy direction (per-row): qty_i * x_i <= K
    for i in range(n):
        qty = data[i][2]
        oracle_solver.add_constraint(
            {xnames[i]: qty}, "<=", K, name=f"easy_{i}",
        )

    # Hard direction (per group): exists i with qty_i*x_i >= K
    # Big-M per row: qty_i*x_i - K*y_i >= 0  (M = K, since x>=0 means x*qty>=0)
    for i in range(n):
        qty = data[i][2]
        oracle_solver.add_constraint(
            {xnames[i]: qty, ynames[i]: -K}, ">=", 0.0, name=f"hard_{i}",
        )

    groups: dict[str, list[int]] = {}
    for i, row in enumerate(data):
        groups.setdefault(row[4], []).append(i)
    for g, idxs in groups.items():
        oracle_solver.add_constraint(
            {ynames[i]: 1.0 for i in idxs}, ">=", 1.0, name=f"grp_{g}",
        )

    oracle_solver.set_objective(
        {xnames[i]: data[i][3] for i in range(n)}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    assert result.status == SolverStatus.OPTIMAL

    ci = {name: i for i, name in enumerate(decidb_cols)}
    decidb_obj = sum(
        int(row[ci["x"]]) * float(row[ci["l_extendedprice"]])
        for row in decidb_result
    )
    assert abs(decidb_obj - result.objective_value) <= 1.0, (
        f"Objective mismatch: DecidB={decidb_obj:.2f}, "
        f"Oracle={result.objective_value:.2f}"
    )

    # Sanity: each group has MAX(x*qty) == K
    by_grp: dict[str, list[float]] = {}
    for row in decidb_result:
        flag = str(row[ci["l_returnflag"]])
        x_val = int(row[ci["x"]])
        qty = float(row[ci["l_quantity"]])
        by_grp.setdefault(flag, []).append(x_val * qty)
    for g, vals in by_grp.items():
        assert abs(max(vals) - K) <= 1e-6, (
            f"Group {g} MAX(x*qty)={max(vals)} != {K} — per-group MAX=K violated"
        )

    perf_tracker.record(
        "per_max_eq", decidb_time, build_time,
        result.solve_time_seconds, n, n * 2, n * 2 + len(groups),
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )


# ============================================================================
# 1.2 — PER + ABS in aggregate constraint
# ============================================================================

@pytest.mark.per_clause
@pytest.mark.var_real
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_per_abs_aggregate(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """SUM(ABS(x - target)) <= 5 PER grp — ABS auxiliary variables must be
    partitioned by PER group. A global-scoping bug would let cross-group
    deviations cancel against each other in the aggregate, relaxing the
    per-group bound."""
    sql = """
        WITH data AS (
            SELECT 1 AS id, 'A' AS grp, 10.0 AS target UNION ALL
            SELECT 2, 'A', 15.0 UNION ALL
            SELECT 3, 'B', 20.0 UNION ALL
            SELECT 4, 'B', 25.0
        )
        SELECT id, grp, target, x
        FROM data
        DECIDE x IS REAL
        SUCH THAT x <= 50
            AND SUM(ABS(x - target)) <= 5 PER grp
        MAXIMIZE SUM(x)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    # Mirror the inline CTE for the oracle.
    data = [
        (1, 'A', 10.0),
        (2, 'A', 15.0),
        (3, 'B', 20.0),
        (4, 'B', 25.0),
    ]
    n = len(data)
    K = 5.0

    t_build = time.perf_counter()
    oracle_solver.create_model("per_abs_aggregate")
    xnames = [f"x_{i}" for i in range(n)]
    dnames = [f"d_{i}" for i in range(n)]
    for vn in xnames:
        oracle_solver.add_variable(vn, VarType.CONTINUOUS, lb=0.0, ub=50.0)
    for vn in dnames:
        oracle_solver.add_variable(vn, VarType.CONTINUOUS, lb=0.0)

    # ABS linearization: d_i >= x_i - target_i,  d_i >= -(x_i - target_i)
    for i in range(n):
        target = data[i][2]
        oracle_solver.add_constraint(
            {dnames[i]: 1.0, xnames[i]: -1.0}, ">=", -target, name=f"abs_pos_{i}",
        )
        oracle_solver.add_constraint(
            {dnames[i]: 1.0, xnames[i]: 1.0}, ">=", target, name=f"abs_neg_{i}",
        )

    # Per-group: SUM(d_i) <= K
    groups: dict[str, list[int]] = {}
    for i, row in enumerate(data):
        groups.setdefault(row[1], []).append(i)
    for g, idxs in groups.items():
        oracle_solver.add_constraint(
            {dnames[i]: 1.0 for i in idxs}, "<=", K, name=f"grp_{g}",
        )

    oracle_solver.set_objective(
        {xnames[i]: 1.0 for i in range(n)}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    assert result.status == SolverStatus.OPTIMAL

    ci = {name: i for i, name in enumerate(decidb_cols)}
    decidb_obj = sum(float(row[ci["x"]]) for row in decidb_result)
    assert abs(decidb_obj - result.objective_value) <= 1e-4, (
        f"Objective mismatch: DecidB={decidb_obj:.6f}, "
        f"Oracle={result.objective_value:.6f}"
    )

    # Sanity: per-group SUM(|x - target|) <= K
    by_grp: dict[str, float] = {}
    for row in decidb_result:
        g = str(row[ci["grp"]])
        x_val = float(row[ci["x"]])
        target = float(row[ci["target"]])
        by_grp[g] = by_grp.get(g, 0.0) + abs(x_val - target)
    for g, total in by_grp.items():
        assert total <= K + 1e-4, (
            f"Group {g} SUM|x-target|={total:.6f} > {K} — per-group constraint violated"
        )

    perf_tracker.record(
        "per_abs_aggregate", decidb_time, build_time,
        result.solve_time_seconds, n, n * 2, n * 2 + len(groups),
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )


# ============================================================================
# 1.3 — Multi-variable + PER
# ============================================================================

@pytest.mark.per_clause
@pytest.mark.var_multi
@pytest.mark.var_boolean
@pytest.mark.var_integer
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_per_multi_variable(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Two decision variables (BOOLEAN x, INTEGER y) under PER grouping.
    The variable-indexing layer must produce the right column mapping for
    each group's constraint, with each variable's coefficients partitioned
    correctly. A multi-variable indexing bug under PER could swap or drop
    coefficients silently."""
    sql = """
        WITH data AS (
            SELECT 1 AS id, 'A' AS grp, 10 AS w UNION ALL
            SELECT 2, 'A', 5 UNION ALL
            SELECT 3, 'B', 8 UNION ALL
            SELECT 4, 'B', 3
        )
        SELECT id, grp, w, x, y
        FROM data
        DECIDE x IS BOOLEAN, y IS INTEGER
        SUCH THAT SUM(x * w) <= 12 PER grp
            AND y <= 3
            AND SUM(y) <= 8
        MAXIMIZE SUM(x * w + y)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = [
        (1, 'A', 10),
        (2, 'A', 5),
        (3, 'B', 8),
        (4, 'B', 3),
    ]
    n = len(data)

    t_build = time.perf_counter()
    oracle_solver.create_model("per_multi_var")
    xnames = [f"x_{i}" for i in range(n)]
    ynames = [f"y_{i}" for i in range(n)]
    for vn in xnames:
        oracle_solver.add_variable(vn, VarType.BINARY)
    for vn in ynames:
        oracle_solver.add_variable(vn, VarType.INTEGER, lb=0, ub=3)

    # Per-group: SUM(x_i * w_i) <= 12
    groups: dict[str, list[int]] = {}
    for i, row in enumerate(data):
        groups.setdefault(row[1], []).append(i)
    for g, idxs in groups.items():
        oracle_solver.add_constraint(
            {xnames[i]: data[i][2] for i in idxs}, "<=", 12.0, name=f"grp_{g}",
        )

    # Global: SUM(y_i) <= 8
    oracle_solver.add_constraint(
        {ynames[i]: 1.0 for i in range(n)}, "<=", 8.0, name="sum_y",
    )

    # MAXIMIZE SUM(x_i * w_i + y_i)
    obj: dict[str, float] = {}
    for i in range(n):
        obj[xnames[i]] = float(data[i][2])
        obj[ynames[i]] = 1.0
    oracle_solver.set_objective(obj, ObjSense.MAXIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    assert result.status == SolverStatus.OPTIMAL

    ci = {name: i for i, name in enumerate(decidb_cols)}
    decidb_obj = sum(
        int(row[ci["x"]]) * int(row[ci["w"]]) + int(row[ci["y"]])
        for row in decidb_result
    )
    assert abs(decidb_obj - result.objective_value) <= 0.5, (
        f"Objective mismatch: DecidB={decidb_obj}, "
        f"Oracle={result.objective_value:.0f}"
    )

    # Sanity: per-group SUM(x*w) <= 12 and global SUM(y) <= 8
    by_grp: dict[str, float] = {}
    sum_y = 0
    for row in decidb_result:
        g = str(row[ci["grp"]])
        xw = int(row[ci["x"]]) * int(row[ci["w"]])
        by_grp[g] = by_grp.get(g, 0.0) + xw
        sum_y += int(row[ci["y"]])
    for g, total in by_grp.items():
        assert total <= 12, f"Group {g} SUM(x*w)={total} > 12 — PER violated"
    assert sum_y <= 8, f"SUM(y)={sum_y} > 8 — global constraint violated"

    perf_tracker.record(
        "per_multi_var", decidb_time, build_time,
        result.solve_time_seconds, n, n * 2, len(groups) + 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )


# ============================================================================
# 1.4 — WHEN + PER + multi-variable
# ============================================================================

@pytest.mark.per_clause
@pytest.mark.when_constraint
@pytest.mark.var_multi
@pytest.mark.var_boolean
@pytest.mark.var_integer
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_when_per_multi_variable(decidb_cli, oracle_solver, perf_tracker):
    """WHEN + PER + two decision variables (BOOLEAN x, INTEGER y).

    The WHEN filter applies to the per-group aggregate constraint — only
    active rows contribute to each group's sum. The objective includes all
    rows (no WHEN). A shared coefficient accumulator bug would mix WHEN-masked
    rows of one variable with unmasked rows of the other, silently corrupting
    the per-group aggregate.

    Data layout (row 2 is inactive in group A):
      id=1 grp=A active=T  w=10 v=5  → in WHEN constraint for A
      id=2 grp=A active=F  w=6  v=3  → only in objective (no constraint from WHEN)
      id=3 grp=B active=T  w=8  v=4  → in WHEN constraint for B
      id=4 grp=B active=T  w=7  v=2  → in WHEN constraint for B
    """
    sql = """
        WITH data AS (
            SELECT 1 AS id, 'A' AS grp, true  AS active, 10 AS w, 5 AS v UNION ALL
            SELECT 2, 'A', false, 6, 3 UNION ALL
            SELECT 3, 'B', true,  8, 4 UNION ALL
            SELECT 4, 'B', true,  7, 2
        )
        SELECT id, grp, w, v, x, y FROM data
        DECIDE x IS BOOLEAN, y IS INTEGER
        SUCH THAT y <= 5 AND SUM(x * w + y * v) <= 18 WHEN active PER grp
        MAXIMIZE SUM(x * w + y * v)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    # Inline CTE data: (id, grp, active, w, v)
    data = [
        (1, 'A', True,  10, 5),
        (2, 'A', False,  6, 3),
        (3, 'B', True,   8, 4),
        (4, 'B', True,   7, 2),
    ]
    n = len(data)

    t_build = time.perf_counter()
    oracle_solver.create_model("when_per_multi_var")
    xnames = [f"x_{i}" for i in range(n)]
    ynames = [f"y_{i}" for i in range(n)]
    for vn in xnames:
        oracle_solver.add_variable(vn, VarType.BINARY)
    for vn in ynames:
        # y <= 5 captured by ub=5; default lb=0 from add_variable
        oracle_solver.add_variable(vn, VarType.INTEGER, lb=0, ub=5)

    # Per-group WHEN-filtered aggregate: SUM_{active rows in g} (w_i*x_i + v_i*y_i) <= 18
    groups_active: dict[str, list[int]] = {}
    for i, row in enumerate(data):
        if row[2]:  # active
            groups_active.setdefault(row[1], []).append(i)
    for g, idxs in groups_active.items():
        coeffs: dict[str, float] = {}
        for i in idxs:
            coeffs[xnames[i]] = float(data[i][3])  # w_i
            coeffs[ynames[i]] = float(data[i][4])  # v_i
        oracle_solver.add_constraint(coeffs, "<=", 18.0, name=f"when_per_{g}")

    # Objective: ALL rows (no WHEN filter)
    obj: dict[str, float] = {}
    for i in range(n):
        obj[xnames[i]] = float(data[i][3])  # w_i
        obj[ynames[i]] = float(data[i][4])  # v_i
    oracle_solver.set_objective(obj, ObjSense.MAXIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    assert result.status == SolverStatus.OPTIMAL

    ci = {name: j for j, name in enumerate(decidb_cols)}
    decidb_obj = sum(
        int(row[ci["x"]]) * int(row[ci["w"]]) + int(row[ci["y"]]) * int(row[ci["v"]])
        for row in decidb_result
    )
    assert abs(decidb_obj - result.objective_value) <= 0.5, (
        f"Objective mismatch: DecidB={decidb_obj}, "
        f"Oracle={result.objective_value:.0f}"
    )

    # Sanity: per-group WHEN-filtered sum <= 18
    row_by_id = {int(row[ci["id"]]): row for row in decidb_result}
    by_grp_active: dict[str, float] = {}
    for row_id, grp, active, w, v in data:
        if active:
            row = row_by_id[row_id]
            contrib = int(row[ci["x"]]) * w + int(row[ci["y"]]) * v
            by_grp_active[grp] = by_grp_active.get(grp, 0.0) + contrib
    for g, total in by_grp_active.items():
        assert total <= 18 + 1e-9, (
            f"Group {g} WHEN-filtered SUM(x*w+y*v)={total} > 18 — constraint violated"
        )

    perf_tracker.record(
        "when_per_multi_var", decidb_time, build_time,
        result.solve_time_seconds, n, n * 2, len(groups_active),
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )


# ============================================================================
# 1.5 — QP objective + PER constraint
# ============================================================================

@pytest.mark.per_clause
@pytest.mark.quadratic
@pytest.mark.var_real
@pytest.mark.cons_aggregate
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_qp_objective_per_constraint(
    decidb_cli, oracle_solver, perf_tracker,
):
    """QP objective (MINIMIZE SUM(POWER(x - target, 2))) with a linear PER
    constraint (SUM(x) >= 5 PER grp).

    Oracle-compared via ``set_quadratic_objective`` so both the QP path and
    the PER path are exercised together; a silently-dropped PER constraint
    would produce obj≈0 instead of matching the oracle's ~16.
    """
    sql = """
        WITH data AS (
            SELECT 1 AS id, 'A' AS grp, 0.5 AS target UNION ALL
            SELECT 2, 'A', 0.5 UNION ALL
            SELECT 3, 'B', 0.5 UNION ALL
            SELECT 4, 'B', 0.5
        )
        SELECT id, grp, target, ROUND(x::DOUBLE, 6) AS x
        FROM data
        DECIDE x IS REAL
        SUCH THAT x >= 0 AND x <= 100 AND SUM(x) >= 5 PER grp
        MINIMIZE SUM(POWER(x - target, 2))
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    # Oracle: mirror the SQL with row-indexed continuous vars, a PER-group
    # SUM(x) >= 5 constraint, and the expanded POWER quadratic. Expansion of
    # (x_i - t_i)^2 drops the constant t_i^2 term (DecidB's reported
    # objective does the same), leaving linear = -2*t_i * x_i and
    # quadratic = x_i^2.
    data = [
        (1, "A", 0.5), (2, "A", 0.5), (3, "B", 0.5), (4, "B", 0.5),
    ]
    n = len(data)
    t_build = time.perf_counter()
    oracle_solver.create_model("qp_objective_per_constraint")
    xnames = [f"x_{i}" for i in range(n)]
    for xn in xnames:
        oracle_solver.add_variable(xn, VarType.CONTINUOUS, lb=0.0, ub=100.0)

    groups: dict[str, list[int]] = {}
    for i, row in enumerate(data):
        groups.setdefault(row[1], []).append(i)
    for g, idxs in groups.items():
        oracle_solver.add_constraint(
            {xnames[i]: 1.0 for i in idxs}, ">=", 5.0, name=f"per_{g}",
        )

    linear = {xnames[i]: -2.0 * float(data[i][2]) for i in range(n)}
    quadratic = {(xnames[i], xnames[i]): 1.0 for i in range(n)}
    oracle_solver.set_quadratic_objective(linear, quadratic, ObjSense.MINIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL

    ci = {name: j for j, name in enumerate(decidb_cols)}
    # Decidb-side objective: strip the constant t_i^2 term to match the
    # oracle's constant-free formulation.
    decidb_obj = sum(
        (float(row[ci["x"]]) - float(row[ci["target"]])) ** 2
        - float(row[ci["target"]]) ** 2
        for row in decidb_result
    )
    assert abs(decidb_obj - result.objective_value) <= 1e-3, (
        f"QP+PER objective mismatch: DecidB={decidb_obj:.6f}, "
        f"Oracle={result.objective_value:.6f}"
    )

    # Invariant: PER constraint must hold per group.
    by_grp: dict[str, float] = {}
    for row in decidb_result:
        g = str(row[ci["grp"]])
        by_grp[g] = by_grp.get(g, 0.0) + float(row[ci["x"]])
    for g, total in by_grp.items():
        assert total >= 5 - 1e-6, (
            f"Group {g} SUM(x)={total:.4f} < 5 — PER constraint violated"
        )

    perf_tracker.record(
        "qp_objective_per", decidb_time, build_time,
        result.solve_time_seconds, n, n, 2 + n,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )


# ============================================================================
# Sanity: PER with degenerate group shapes
# ============================================================================

@pytest.mark.per_clause
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_per_single_row_groups(decidb_cli, oracle_solver, perf_tracker):
    """Every PER group has exactly one row — |group| = 1 degenerate case.

    Each group's aggregate has a single-element coefficient vector; the
    per-group constraint degenerates to a per-row bound. The budget RHS
    (60) selects between rows based on val: group B (val=50) fits, groups
    A (100) and C (75) do not.
    """
    sql = """
        SELECT id, grp, val, x FROM (
            VALUES (1, 'A', 100.0), (2, 'B', 50.0), (3, 'C', 75.0)
        ) t(id, grp, val)
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * val) <= 60 PER grp
        MAXIMIZE SUM(x * val)
    """
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = [(1, 'A', 100.0), (2, 'B', 50.0), (3, 'C', 75.0)]
    n = len(data)

    t_build = time.perf_counter()
    oracle_solver.create_model("per_single_row_groups")
    vnames = [f"x_{i}" for i in range(n)]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    groups: dict[str, list[int]] = {}
    for i, row in enumerate(data):
        groups.setdefault(row[1], []).append(i)
    for g, idxs in groups.items():
        oracle_solver.add_constraint(
            {vnames[i]: data[i][2] for i in idxs},
            "<=", 60.0, name=f"per_budget_{g}",
        )
    oracle_solver.set_objective(
        {vnames[i]: data[i][2] for i in range(n)}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL

    val_idx = decidb_cols.index("val")
    x_idx = decidb_cols.index("x")
    decidb_obj = sum(int(r[x_idx]) * float(r[val_idx]) for r in decidb_rows)
    assert abs(decidb_obj - result.objective_value) <= 1e-6, (
        f"Objective mismatch: DecidB={decidb_obj}, Oracle={result.objective_value}"
    )

    perf_tracker.record(
        "per_single_row_groups", decidb_time, build_time,
        result.solve_time_seconds, n, n, len(groups),
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )


@pytest.mark.per_clause
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_per_zero_coefficient_group(decidb_cli, oracle_solver, perf_tracker):
    """One PER group has all-zero coefficients — its aggregate constraint is
    vacuously satisfied regardless of the decision.

    Tests the constraint-builder's handling of a per-group aggregate that
    degenerates to ``0 <= rhs``. Nothing should be added to the model for
    that group, and the other groups' constraints must remain unaffected.
    Group A (both coeffs 0) should leave x_1, x_2 unconstrained → picked by
    MAXIMIZE; group B binds.
    """
    sql = """
        SELECT id, grp, val, x FROM (
            VALUES (1, 'A', 0.0), (2, 'A', 0.0),
                   (3, 'B', 5.0), (4, 'B', 20.0)
        ) t(id, grp, val)
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * val) <= 8 PER grp
        MAXIMIZE SUM(x)
    """
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = [(1, 'A', 0.0), (2, 'A', 0.0), (3, 'B', 5.0), (4, 'B', 20.0)]
    n = len(data)

    t_build = time.perf_counter()
    oracle_solver.create_model("per_zero_coefficient_group")
    vnames = [f"x_{i}" for i in range(n)]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    # Per-group SUM(x * val) <= 8. Group A: 0 <= 8 (vacuous, oracle omits it).
    # Group B: 5 x_3 + 20 x_4 <= 8.
    groups: dict[str, list[int]] = {}
    for i, row in enumerate(data):
        groups.setdefault(row[1], []).append(i)
    for g, idxs in groups.items():
        coeffs = {vnames[i]: data[i][2] for i in idxs}
        # Emit only if not trivially zero (structural equivalent of the
        # "vacuous" group that DecidB may or may not emit; the solver
        # behaves identically either way).
        if any(abs(c) > 0.0 for c in coeffs.values()):
            oracle_solver.add_constraint(
                coeffs, "<=", 8.0, name=f"per_{g}",
            )
    oracle_solver.set_objective(
        {vnames[i]: 1.0 for i in range(n)}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL

    x_idx = decidb_cols.index("x")
    decidb_obj = sum(int(r[x_idx]) for r in decidb_rows)
    assert abs(decidb_obj - result.objective_value) <= 1e-6, (
        f"Objective mismatch: DecidB={decidb_obj}, Oracle={result.objective_value}"
    )

    perf_tracker.record(
        "per_zero_coefficient_group", decidb_time, build_time,
        result.solve_time_seconds, n, n, 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )


@pytest.mark.per_clause
@pytest.mark.when_constraint
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_per_null_group_with_when(decidb_cli, oracle_solver, perf_tracker):
    """Row with NULL PER-key that passes the WHEN mask, combined with a
    group whose only row fails the WHEN mask (empty WHEN-bucket).

    Group NULL: an active row exists → constraint emitted.
    Group 'B': only row is inactive → empty WHEN-bucket → default WHEN→PER
               policy skips the constraint for that group.
    This exercises the combination of NULL-grouping and empty-group skip.
    """
    sql = """
        SELECT id, grp, val, active, x FROM (
            VALUES (1, 'A', 10.0, true),
                   (2, NULL, 5.0, true),
                   (3, 'B', 8.0, false),
                   (4, 'A', 3.0, true),
                   (5, NULL, 12.0, false)
        ) t(id, grp, val, active)
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * val) <= 10 WHEN active PER grp
        MAXIMIZE SUM(x * val)
    """
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = [
        (1, 'A', 10.0, True),
        (2, None, 5.0, True),
        (3, 'B', 8.0, False),
        (4, 'A', 3.0, True),
        (5, None, 12.0, False),
    ]
    n = len(data)

    t_build = time.perf_counter()
    oracle_solver.create_model("per_null_group_with_when")
    vnames = [f"x_{i}" for i in range(n)]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    # Per-group coefficients restricted to WHEN-active rows.
    groups: dict = {}
    for i, row in enumerate(data):
        groups.setdefault(row[1], []).append(i)
    for g, idxs in groups.items():
        active_coeffs = {
            vnames[i]: data[i][2] for i in idxs if data[i][3]
        }
        # Default WHEN→PER policy: skip groups with an empty WHEN-bucket.
        if not active_coeffs:
            continue
        oracle_solver.add_constraint(
            active_coeffs, "<=", 10.0, name=f"per_when_{g}",
        )
    oracle_solver.set_objective(
        {vnames[i]: data[i][2] for i in range(n)}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL

    val_idx = decidb_cols.index("val")
    x_idx = decidb_cols.index("x")
    decidb_obj = sum(int(r[x_idx]) * float(r[val_idx]) for r in decidb_rows)
    assert abs(decidb_obj - result.objective_value) <= 1e-6, (
        f"Objective mismatch: DecidB={decidb_obj}, Oracle={result.objective_value}"
    )

    perf_tracker.record(
        "per_null_group_with_when", decidb_time, build_time,
        result.solve_time_seconds, n, n, 2,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )
