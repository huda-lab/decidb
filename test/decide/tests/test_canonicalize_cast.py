"""Exact cast descent in the canonicalizer's additive spine.

A bound expression tree is typed, and DuckDB inserts casts wherever a scalar
function's children do not already match the resolved signature
(``FunctionBinder::CastToFunctionArguments``), plus one around each whole
comparison side when the two sides' types differ. So a DECIDE constraint's
additive spine routinely arrives sealed inside a cast. Treating that cast as a
term boundary makes the whole side one opaque term, and the canonicalizer
declines.

``Decompose`` descends **widening numeric** casts and re-applies them per term
on rebuild. Widening is the load-bearing word: ``CAST(1.6 + 1.6 AS INTEGER)`` is
3 while ``CAST(1.6 AS INTEGER) + CAST(1.6 AS INTEGER)`` is 4, so a narrowing
cast must stay a boundary.

Cast descent is legal only when the target type represents every source value
exactly.  The oracle cases below deliberately cross the ``2^53`` boundary:
DuckDB allows ``BIGINT`` and high-precision ``DECIMAL`` to cast implicitly to
``DOUBLE``, but those conversions are not exact.  Moving terms through such a
cast changes the feasible set.  Exact DECIMAL widening remains a positive
control.

Covers:
  - test_cast_lid_mixed_placement: reducer (LEFT) beside a scalar subquery
    (RIGHT) under a cast lid -- the shape where descent changes the pass's
    decision, not merely its reach
  - test_cast_lid_negated_reducer: two aggregate-local WHEN reducers, one
    negated, under a cast lid
  - test_bigint_to_double_cast_lid_preserves_sql_semantics: the exactness cliff
    immediately around 2^53
  - test_decimal_cast_lid_exactness: exact DECIMAL widening versus an inexact
    DECIMAL-to-DOUBLE conversion
"""

import time

import pytest

from solver.types import VarType, ObjSense, SolverStatus


def _solve_cast_lid_case(decidb_cli, duckdb_conn, value_sql, limit_sql):
    """Return DeciDB's maximum and DuckDB's direct-expression maximum.

    ``x + value <= limit`` binds a cast over the whole additive LHS whenever
    ``value`` and ``limit`` require different comparison types.  The vanilla
    DuckDB query evaluates that written expression for each candidate without
    DECIDE canonicalization, so it is an independent semantic oracle.
    """
    sql = f"""
        SELECT id, value, lim, x
        FROM (VALUES (1, {value_sql}, {limit_sql})) t(id, value, lim)
        DECIDE x(INT)
        SUCH THAT x <= 3 AND x + value <= lim
        MAXIMIZE SUM(x)
    """
    rows, cols = decidb_cli.execute(sql)
    actual = int(rows[0][cols.index("x")])

    expected = duckdb_conn.execute(f"""
        SELECT MAX(candidate)
        FROM range(0, 4) candidates(candidate),
             (VALUES ({value_sql}, {limit_sql})) t(value, lim)
        WHERE candidate + value <= lim
    """).fetchone()[0]
    return actual, int(expected)


@pytest.mark.var_integer
@pytest.mark.cons_aggregate
@pytest.mark.cons_mixed
@pytest.mark.when_constraint
@pytest.mark.cons_subquery
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_cast_lid_mixed_placement(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """``(SUM(x) WHEN (q > 25)) + (scalar subquery) <= 60``.

    The LHS binds to ``CAST(CAST(sum(x) FILTER ... AS DECIMAL) + CAST(sub AS
    DECIMAL) AS DOUBLE)`` -- one cast lid over a two-term spine whose terms want
    opposite sides. A numeric literal would not exercise this: the parsed-level
    simplifier peels a numeric offset before binding, so it never arrives
    sealed. A subquery survives to the canonicalizer intact.
    """
    sql = """
        SELECT l_orderkey, l_linenumber, l_quantity, x
        FROM lineitem
        WHERE l_orderkey <= 5
        DECIDE x(INT)
        SUCH THAT x <= 4
          AND (SUM(x) WHEN (l_quantity > 25))
              + (SELECT MAX(l_quantity) FROM lineitem WHERE l_orderkey = 1) <= 60
        MAXIMIZE SUM(x)
    """
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_quantity AS DOUBLE)
        FROM lineitem WHERE l_orderkey <= 5
    """).fetchall()
    bound = duckdb_conn.execute("""
        SELECT CAST(MAX(l_quantity) AS DOUBLE) FROM lineitem WHERE l_orderkey = 1
    """).fetchone()[0]
    n = len(data)
    assert n > 0
    # The shape is only meaningful if the WHEN filter actually excludes rows.
    assert 0 < sum(1 for row in data if row[0] > 25.0) < n

    t_build = time.perf_counter()
    oracle_solver.create_model("cast_lid_mixed_placement")
    agg = {}
    obj = {}
    for i in range(n):
        oracle_solver.add_variable(f"x_{i}", VarType.INTEGER, lb=0.0, ub=4.0)
        obj[f"x_{i}"] = 1.0
        if data[i][0] > 25.0:
            agg[f"x_{i}"] = 1.0
    oracle_solver.add_constraint(agg, "<=", 60.0 - bound, name="agg")
    oracle_solver.set_objective(obj, ObjSense.MAXIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL

    ci = {c: i for i, c in enumerate(decidb_cols)}
    decidb_obj = sum(float(r[ci["x"]]) for r in decidb_rows)
    assert abs(decidb_obj - result.objective_value) <= 1e-4, (
        f"Objective mismatch: DecidB={decidb_obj:.6f}, Oracle={result.objective_value:.6f}"
    )
    # The bound migrated across the relation, so check it holds as written.
    filtered = sum(
        float(r[ci["x"]]) for r in decidb_rows if float(r[ci["l_quantity"]]) > 25.0
    )
    assert filtered + bound <= 60.0 + 1e-4, (
        f"constraint violated: {filtered} + {bound} > 60"
    )

    perf_tracker.record(
        "cast_lid_mixed_placement", decidb_time, build_time,
        result.solve_time_seconds, n, 1, n,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )


@pytest.mark.var_integer
@pytest.mark.cons_aggregate
@pytest.mark.cons_mixed
@pytest.mark.when_constraint
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_cast_lid_negated_reducer(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """``(SUM(x) WHEN c) - (SUM(x * q) WHEN c) + 2 <= 8`` under a cast lid.

    Every term is decision-bearing here, so placement does not change -- what is
    pinned is that the spine is reached at all, with the subtraction's sign
    carried through to the negated reducer.
    """
    sql = """
        SELECT l_orderkey, l_linenumber, l_quantity, x
        FROM lineitem
        WHERE l_orderkey <= 5
        DECIDE x(INT)
        SUCH THAT x <= 4
          AND (SUM(x) WHEN (l_quantity > 25))
              - (SUM(x * l_quantity) WHEN (l_quantity > 25)) + 2 <= 8
        MAXIMIZE SUM(x)
    """
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_quantity AS DOUBLE)
        FROM lineitem WHERE l_orderkey <= 5
    """).fetchall()
    n = len(data)
    assert n > 0

    t_build = time.perf_counter()
    oracle_solver.create_model("cast_lid_negated_reducer")
    agg = {}
    obj = {}
    for i in range(n):
        oracle_solver.add_variable(f"x_{i}", VarType.INTEGER, lb=0.0, ub=4.0)
        obj[f"x_{i}"] = 1.0
        if data[i][0] > 25.0:
            agg[f"x_{i}"] = 1.0 - data[i][0]
    oracle_solver.add_constraint(agg, "<=", 6.0, name="agg")
    oracle_solver.set_objective(obj, ObjSense.MAXIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL

    ci = {c: i for i, c in enumerate(decidb_cols)}
    decidb_obj = sum(float(r[ci["x"]]) for r in decidb_rows)
    assert abs(decidb_obj - result.objective_value) <= 1e-4, (
        f"Objective mismatch: DecidB={decidb_obj:.6f}, Oracle={result.objective_value:.6f}"
    )

    perf_tracker.record(
        "cast_lid_negated_reducer", decidb_time, build_time,
        result.solve_time_seconds, n, 1, n,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )


@pytest.mark.parametrize(
    ("value_sql", "limit_sql"),
    [
        pytest.param(
            "9007199254740990::BIGINT",
            "9007199254740992::DOUBLE",
            id="immediately_below_2_pow_53",
        ),
        pytest.param(
            "9007199254740993::BIGINT",
            "9007199254740994::DOUBLE",
            id="above_2_pow_53",
        ),
    ],
)
@pytest.mark.var_integer
@pytest.mark.cons_perrow
@pytest.mark.correctness
def test_bigint_to_double_cast_lid_preserves_sql_semantics(
    decidb_cli, duckdb_conn, value_sql, limit_sql
):
    """Implicit ``BIGINT -> DOUBLE`` must remain an atomic cast lid.

    Above ``2^53``, distributing the cast and then migrating the data atom turns
    the written threshold ``x = 1`` into ``x = 2``.  The adjacent case starts
    below ``2^53`` but lets the addition cross the exactness cliff, where the
    written expression admits ``x = 3`` and the moved form stops at ``x = 2``.
    """
    actual, expected = _solve_cast_lid_case(
        decidb_cli, duckdb_conn, value_sql, limit_sql
    )
    assert actual == expected, (
        f"canonicalization changed SQL cast semantics: DeciDB={actual}, "
        f"direct DuckDB={expected}"
    )


@pytest.mark.parametrize(
    ("value_sql", "limit_sql"),
    [
        pytest.param(
            "2.25::DECIMAL(6,2)",
            "3.25::DECIMAL(8,2)",
            id="exact_decimal_widening",
        ),
        pytest.param(
            "9007199254740993::DECIMAL(18,0)",
            "9007199254740994::DOUBLE",
            id="inexact_decimal_to_double",
        ),
    ],
)
@pytest.mark.var_integer
@pytest.mark.cons_perrow
@pytest.mark.correctness
def test_decimal_cast_lid_exactness(
    decidb_cli, duckdb_conn, value_sql, limit_sql
):
    """Distribute exact DECIMAL widening, but not an inexact numeric cast.

    The two cases share the same structural shape.  Only the representability
    proof differs, which keeps the test focused on the cast allow-list rather
    than on additive placement generally.
    """
    actual, expected = _solve_cast_lid_case(
        decidb_cli, duckdb_conn, value_sql, limit_sql
    )
    assert actual == expected, (
        f"canonicalization changed SQL cast semantics: DeciDB={actual}, "
        f"direct DuckDB={expected}"
    )
