"""Tests for correlated subqueries in DECIDE constraints and objectives.

Covers:
  - Per-row constraint with correlated subquery: x <= (SELECT ... WHERE correlation)
  - Correlated subquery as boolean gate via COALESCE
  - Correlated subquery providing per-row objective coefficients
  - WHEN + correlated subquery composition
  - NULL-producing correlated subquery handled via COALESCE
  - Verifies decorrelation into JOIN and per-row RHS evaluation.
"""

import time

import pytest

from solver.types import VarType, ObjSense
from comparison.compare import compare_solutions


@pytest.mark.var_integer
@pytest.mark.cons_perrow
@pytest.mark.cons_aggregate
@pytest.mark.cons_subquery
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_correlated_subquery_perrow_bound(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Correlated subquery provides per-row upper bound from another table.

    x <= (SELECT p_size FROM part WHERE p_partkey = ps_partkey) bounds each
    partsupp row's x by the corresponding part's size. Combined with a global
    budget constraint on supplycost.
    """
    sql = """
        SELECT ps_partkey, ps_suppkey, ps_supplycost, x
        FROM partsupp
        WHERE ps_partkey < 20
        DECIDE x(INT)
        SUCH THAT x <= (SELECT CAST(p_size AS INTEGER) FROM part WHERE p_partkey = ps_partkey)
          AND SUM(x * ps_supplycost) <= 5000
        MAXIMIZE SUM(x)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(ps_partkey AS BIGINT),
               CAST(ps_suppkey AS BIGINT),
               CAST(ps_supplycost AS DOUBLE),
               CAST(p_size AS DOUBLE)
        FROM partsupp
        JOIN part ON p_partkey = ps_partkey
        WHERE ps_partkey < 20
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("correlated_subquery_perrow")
    vnames = [f"x_{i}" for i in range(len(data))]
    for i, vn in enumerate(vnames):
        oracle_solver.add_variable(vn, VarType.INTEGER, lb=0.0, ub=data[i][3])

    oracle_solver.add_constraint(
        {vnames[i]: data[i][2] for i in range(len(data))},
        "<=", 5000.0, name="budget",
    )
    oracle_solver.set_objective(
        {vnames[i]: 1.0 for i in range(len(data))},
        ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_result, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": 1.0},
    )

    perf_tracker.record(
        "correlated_subquery_perrow", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 2,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.var_boolean
@pytest.mark.cons_perrow
@pytest.mark.cons_subquery
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_correlated_subquery_boolean_filter(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Correlated subquery as a boolean gate: only select items whose supplier has positive balance.

    x <= (SELECT 1 FROM supplier WHERE s_suppkey = ps_suppkey AND s_acctbal > 0)
    effectively filters which rows can have x=1.
    """
    sql = """
        SELECT ps_partkey, ps_suppkey, ps_supplycost, x
        FROM partsupp
        WHERE ps_partkey < 10
        DECIDE x(BOOL)
        SUCH THAT x <= COALESCE((SELECT 1 FROM supplier WHERE s_suppkey = ps_suppkey AND s_acctbal > 0), 0)
          AND SUM(x * ps_supplycost) <= 2000
        MAXIMIZE SUM(x)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(ps_partkey AS BIGINT),
               CAST(ps_suppkey AS BIGINT),
               CAST(ps_supplycost AS DOUBLE),
               CASE WHEN s_acctbal > 0 THEN 1.0 ELSE 0.0 END AS eligible
        FROM partsupp
        LEFT JOIN supplier ON s_suppkey = ps_suppkey
        WHERE ps_partkey < 10
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("correlated_subquery_boolean")
    vnames = [f"x_{i}" for i in range(len(data))]
    for i, vn in enumerate(vnames):
        oracle_solver.add_variable(vn, VarType.BINARY, ub=data[i][3])

    oracle_solver.add_constraint(
        {vnames[i]: data[i][2] for i in range(len(data))},
        "<=", 2000.0, name="budget",
    )
    oracle_solver.set_objective(
        {vnames[i]: 1.0 for i in range(len(data))},
        ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_result, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": 1.0},
    )

    perf_tracker.record(
        "correlated_subquery_boolean", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 2,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.cons_subquery
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_correlated_subquery_objective(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Correlated subquery provides per-row objective coefficients.

    MAXIMIZE SUM(x * (SELECT p_retailprice ...)) uses a correlated subquery
    to fetch each partsupp row's retail price as the objective coefficient.
    """
    sql = """
        SELECT ps_partkey, ps_suppkey, ps_supplycost,
               (SELECT CAST(p_retailprice AS DOUBLE) FROM part
                WHERE p_partkey = ps_partkey) AS retail_price,
               x
        FROM partsupp
        WHERE ps_partkey < 10
        DECIDE x(BOOL)
        SUCH THAT SUM(x * ps_supplycost) <= 3000
        MAXIMIZE SUM(x * (SELECT CAST(p_retailprice AS DOUBLE) FROM part
                          WHERE p_partkey = ps_partkey))
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(ps_partkey AS BIGINT),
               CAST(ps_suppkey AS BIGINT),
               CAST(ps_supplycost AS DOUBLE),
               CAST(p_retailprice AS DOUBLE)
        FROM partsupp
        JOIN part ON p_partkey = ps_partkey
        WHERE ps_partkey < 10
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("correlated_subquery_objective")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    oracle_solver.add_constraint(
        {vnames[i]: data[i][2] for i in range(len(data))},
        "<=", 3000.0, name="budget",
    )
    oracle_solver.set_objective(
        {vnames[i]: data[i][3] for i in range(len(data))},
        ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_result, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[decidb_cols.index("retail_price")])},
    )

    perf_tracker.record(
        "correlated_subquery_objective", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.var_boolean
@pytest.mark.cons_perrow
@pytest.mark.cons_aggregate
@pytest.mark.cons_subquery
@pytest.mark.when_constraint
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_correlated_subquery_when_composition(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """WHEN filter combined with correlated subquery on per-row bound.

    x <= COALESCE((SELECT 1 FROM supplier ...), 0) WHEN ps_supplycost < 500
    Only rows with supplycost < 500 get the correlated eligibility check;
    rows with supplycost >= 500 are unconstrained (BOOLEAN caps at 1).
    """
    sql = """
        SELECT ps_partkey, ps_suppkey, ps_supplycost, x
        FROM partsupp
        WHERE ps_partkey < 10
        DECIDE x(BOOL)
        SUCH THAT x <= COALESCE(
                    (SELECT 1 FROM supplier
                     WHERE s_suppkey = ps_suppkey AND s_acctbal > 0), 0)
                  WHEN ps_supplycost < 500
            AND SUM(x * ps_supplycost) <= 2000
        MAXIMIZE SUM(x)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(ps_partkey AS BIGINT),
               CAST(ps_suppkey AS BIGINT),
               CAST(ps_supplycost AS DOUBLE),
               CASE WHEN s_acctbal > 0 THEN 1.0 ELSE 0.0 END AS eligible
        FROM partsupp
        LEFT JOIN supplier ON s_suppkey = ps_suppkey
        WHERE ps_partkey < 10
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("correlated_subquery_when")
    vnames = [f"x_{i}" for i in range(len(data))]
    for i, vn in enumerate(vnames):
        # WHEN ps_supplycost < 500: x <= eligible (0 or 1)
        # WHEN ps_supplycost >= 500: no per-row bound (BOOLEAN ub = 1)
        supplycost = data[i][2]
        eligible = data[i][3]
        ub = eligible if supplycost < 500 else 1.0
        oracle_solver.add_variable(vn, VarType.BINARY, ub=ub)

    oracle_solver.add_constraint(
        {vnames[i]: data[i][2] for i in range(len(data))},
        "<=", 2000.0, name="budget",
    )
    oracle_solver.set_objective(
        {vnames[i]: 1.0 for i in range(len(data))},
        ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_result, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": 1.0},
    )

    perf_tracker.record(
        "correlated_subquery_when", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 2,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.var_integer
@pytest.mark.cons_perrow
@pytest.mark.cons_aggregate
@pytest.mark.cons_subquery
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_correlated_subquery_null_coalesce(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Correlated subquery returning NULL for some rows, handled via COALESCE.

    (SELECT p_size FROM part WHERE p_partkey = ps_partkey AND p_size > 30)
    returns NULL for parts with size <= 30. COALESCE converts that to 0,
    effectively forcing x=0 for those rows.
    """
    sql = """
        SELECT ps_partkey, ps_suppkey, ps_supplycost, x
        FROM partsupp
        WHERE ps_partkey < 20
        DECIDE x(INT)
        SUCH THAT x <= COALESCE(
                    (SELECT CAST(p_size AS INTEGER) FROM part
                     WHERE p_partkey = ps_partkey AND p_size > 30), 0)
            AND SUM(x * ps_supplycost) <= 5000
        MAXIMIZE SUM(x)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(ps_partkey AS BIGINT),
               CAST(ps_suppkey AS BIGINT),
               CAST(ps_supplycost AS DOUBLE),
               CASE WHEN p_size > 30 THEN CAST(p_size AS DOUBLE) ELSE 0.0 END AS bound
        FROM partsupp
        JOIN part ON p_partkey = ps_partkey
        WHERE ps_partkey < 20
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("correlated_subquery_null")
    vnames = [f"x_{i}" for i in range(len(data))]
    for i, vn in enumerate(vnames):
        oracle_solver.add_variable(vn, VarType.INTEGER, lb=0.0, ub=data[i][3])

    oracle_solver.add_constraint(
        {vnames[i]: data[i][2] for i in range(len(data))},
        "<=", 5000.0, name="budget",
    )
    oracle_solver.set_objective(
        {vnames[i]: 1.0 for i in range(len(data))},
        ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_result, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": 1.0},
    )

    perf_tracker.record(
        "correlated_subquery_null", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 2,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.var_real
@pytest.mark.cons_perrow
@pytest.mark.cons_aggregate
@pytest.mark.cons_subquery
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_correlated_subquery_is_real(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """Correlated subquery providing a fractional per-row upper bound on(REAL).

    Decorrelation emits per-row RHS values; with `AVG(...)` on a continuous
    (non-integer) LHS those bounds are fractional in general. This exercises
    the LP / continuous-variable path through decorrelation — previous
    correlated-subquery tests all use BOOLEAN or INTEGER variables.

    An aggregate budget forces the optimum to be non-trivial (not simply
    `x_i = ub_i` for every row).
    """
    sql = """
        SELECT l.l_orderkey, l.l_linenumber, l.l_extendedprice, l.l_quantity, x
        FROM lineitem l
        WHERE l.l_orderkey < 30
        DECIDE x(REAL)
        SUCH THAT x <= (SELECT CAST(AVG(l2.l_quantity) AS DOUBLE)
                        FROM lineitem l2
                        WHERE l2.l_orderkey = l.l_orderkey)
              AND SUM(x * l.l_quantity) <= 100
        MAXIMIZE SUM(x * l.l_extendedprice)
    """
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l.l_orderkey AS BIGINT),
               CAST(l.l_linenumber AS BIGINT),
               CAST(l.l_extendedprice AS DOUBLE),
               CAST(l.l_quantity AS DOUBLE),
               CAST(avg_q.avg_quantity AS DOUBLE)
        FROM lineitem l
        JOIN (
            SELECT l2.l_orderkey, AVG(l2.l_quantity) AS avg_quantity
            FROM lineitem l2
            GROUP BY l2.l_orderkey
        ) avg_q ON avg_q.l_orderkey = l.l_orderkey
        WHERE l.l_orderkey < 30
    """).fetchall()
    n = len(data)

    t_build = time.perf_counter()
    oracle_solver.create_model("correlated_subquery_is_real")
    vnames = [f"x_{i}" for i in range(n)]
    for i, vn in enumerate(vnames):
        # ub from decorrelated AVG → fractional in general
        oracle_solver.add_variable(vn, VarType.CONTINUOUS, lb=0.0, ub=data[i][4])

    oracle_solver.add_constraint(
        {vnames[i]: data[i][3] for i in range(n)},
        "<=", 100.0, name="budget",
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
        "correlated_subquery_is_real", decidb_time, build_time,
        result.solve_time_seconds, n, n, 2,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )
