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

import csv
import io
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


def _solve_cast_relation_case(
    decidb_cli, duckdb_conn, value_sql, relation, limit_sql, lo=-6, hi=6
):
    sql = f"""
        SELECT id, value, lim, x
        FROM (VALUES (1, {value_sql}, {limit_sql})) t(id, value, lim)
        DECIDE x(INT)
        SUCH THAT x >= {lo} AND x <= {hi} AND x + value {relation} lim
        MAXIMIZE SUM(x)
    """
    rows, cols = decidb_cli.execute(sql)
    actual = int(rows[0][cols.index("x")])
    expected = duckdb_conn.execute(f"""
        SELECT MAX(candidate)
        FROM range({lo}, {hi + 1}) candidates(candidate),
             (VALUES ({value_sql}, {limit_sql})) t(value, lim)
        WHERE candidate + value {relation} lim
    """).fetchone()[0]
    assert expected is not None
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
        pytest.param("1000000::BIGINT", "1000002::DOUBLE", id="small_magnitude"),
        pytest.param(
            "4503599627370495::BIGINT",
            "4503599627370497::DOUBLE",
            id="just_inside_double_domain",
        ),
    ],
)
@pytest.mark.var_integer
@pytest.mark.cons_perrow
@pytest.mark.correctness
def test_bigint_to_double_cast_lid_preserves_sql_semantics(
    decidb_cli, duckdb_conn, value_sql, limit_sql
):
    """Implicit ``BIGINT -> DOUBLE`` must agree with SQL inside the model domain.

    DECIDE carries one numeric domain, DOUBLE (see ``syntax_reference.md`` ->
    Numeric precision), so a cast into it is erased.  Wherever the values involved
    are representable there -- which is everything below ``2^53`` -- the erased
    form and direct SQL evaluation must give the same answer.  The behaviour past
    that boundary is pinned separately by
    ``test_magnitudes_beyond_double_domain_are_a_documented_limit``.
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
            "1000003::DECIMAL(18,0)",
            "1000005::DOUBLE",
            id="decimal_to_double_in_domain",
        ),
    ],
)
@pytest.mark.var_integer
@pytest.mark.cons_perrow
@pytest.mark.correctness
def test_decimal_cast_lid_exactness(
    decidb_cli, duckdb_conn, value_sql, limit_sql
):
    """DECIMAL widening and DECIMAL -> DOUBLE both agree with SQL in-domain.

    The two cases share the same structural shape, so a divergence points at the
    cast policy rather than at additive placement.
    """
    actual, expected = _solve_cast_lid_case(
        decidb_cli, duckdb_conn, value_sql, limit_sql
    )
    assert actual == expected, (
        f"canonicalization changed SQL cast semantics: DeciDB={actual}, "
        f"direct DuckDB={expected}"
    )


@pytest.mark.parametrize("relation", ["<", "<=", ">", ">=", "=", "<>"])
@pytest.mark.parametrize(
    ("value_sql", "limit_sql"),
    [
        pytest.param("1000000::BIGINT", "1000002::DOUBLE", id="positive"),
        pytest.param("-1000003::BIGINT", "-1000002::DOUBLE", id="negative"),
        pytest.param(
            "1000003.50::DECIMAL(20,2)", "1000005.50::DOUBLE", id="decimal_scale"
        ),
    ],
)
@pytest.mark.var_integer
@pytest.mark.cons_perrow
@pytest.mark.correctness
def test_cast_agrees_with_sql_for_every_relation(
    decidb_cli, duckdb_conn, value_sql, limit_sql, relation
):
    """Every SQL comparison survives cast erasure, on both signs and with scale."""
    actual, expected = _solve_cast_relation_case(
        decidb_cli, duckdb_conn, value_sql, relation, limit_sql
    )
    assert actual == expected, (
        f"wrong {relation} preimage for {value_sql}: "
        f"DeciDB={actual}, direct DuckDB={expected}"
    )


@pytest.mark.var_integer
@pytest.mark.cons_perrow
@pytest.mark.correctness
def test_cast_row_varying_bounds(decidb_cli, duckdb_conn):
    rows, cols = decidb_cli.execute("""
        SELECT id, value, lim, x
        FROM (VALUES
            (1, 1000000::BIGINT, 1000002::DOUBLE),
            (2, 1000003::BIGINT, 1000005::DOUBLE)
        ) t(id, value, lim)
        DECIDE x(INT)
        SUCH THAT x >= -2 AND x <= 4 AND x + value <= lim
        MAXIMIZE SUM(x)
    """)
    ci = {name: i for i, name in enumerate(cols)}
    actual = {int(row[ci["id"]]): int(row[ci["x"]]) for row in rows}
    expected_rows = duckdb_conn.execute("""
        SELECT id, MAX(candidate)
        FROM range(-2, 5) c(candidate), (VALUES
            (1, 1000000::BIGINT, 1000002::DOUBLE),
            (2, 1000003::BIGINT, 1000005::DOUBLE)
        ) t(id, value, lim)
        WHERE candidate + value <= lim
        GROUP BY id
    """).fetchall()
    assert actual == dict(expected_rows)


@pytest.mark.var_integer
@pytest.mark.cons_aggregate
@pytest.mark.per_clause
@pytest.mark.correctness
def test_cast_aggregate_per_bounds(decidb_cli, duckdb_conn):
    rows, cols = decidb_cli.execute("""
        SELECT id, grp, value, lim, x
        FROM (VALUES
            (1, 'a', 1000000::BIGINT, 2000004::DOUBLE),
            (2, 'a', 1000000::BIGINT, 2000004::DOUBLE),
            (3, 'b', 1000001::BIGINT, 2000007::DOUBLE),
            (4, 'b', 1000001::BIGINT, 2000007::DOUBLE)
        ) t(id, grp, value, lim)
        DECIDE x(INT)
        SUCH THAT x >= 0 AND x <= 3 AND SUM(x + value) <= MIN(lim) PER grp
        MAXIMIZE SUM(x)
    """)
    ci = {name: i for i, name in enumerate(cols)}
    actual = {}
    for row in rows:
        actual.setdefault(row[ci["grp"]], 0)
        actual[row[ci["grp"]]] += int(row[ci["x"]])

    expected = {}
    for grp, value, lim in [
        ("a", 1000000, 2000004.0),
        ("b", 1000001, 2000007.0),
    ]:
        feasible = [
            x1 + x2
            for x1 in range(4)
            for x2 in range(4)
            if duckdb_conn.execute(
                "SELECT CAST(?::BIGINT + ?::BIGINT + ?::BIGINT + ?::BIGINT AS DOUBLE) <= ?",
                [x1, value, x2, value, lim],
            ).fetchone()[0]
        ]
        expected[grp] = max(feasible)
    assert actual == expected


@pytest.mark.var_integer
@pytest.mark.cons_perrow
@pytest.mark.correctness
def test_explicit_lossy_cast_excludes_duckdb_overflow_domain(decidb_cli, duckdb_conn):
    """Conversion-error source values lie outside the cast's defined domain."""
    rows, cols = decidb_cli.execute("""
        SELECT id, value, lim, x
        FROM (VALUES (1, 2147483646::BIGINT, 2147483647::INTEGER))
             t(id, value, lim)
        DECIDE x(INT)
        SUCH THAT x >= 0 AND x <= 3
          AND CAST(x + value AS INTEGER) <= lim
        MAXIMIZE SUM(x)
    """)
    actual = int(rows[0][cols.index("x")])

    feasible = []
    for candidate in range(4):
        try:
            matches = duckdb_conn.execute(
                "SELECT CAST(?::BIGINT + 2147483646::BIGINT AS INTEGER) "
                "<= 2147483647::INTEGER",
                [candidate],
            ).fetchone()[0]
        except Exception as exc:
            # The source point is outside DuckDB CAST's defined domain only
            # when the engine reports an integer conversion overflow.
            assert "out of range" in str(exc).lower() or "overflow" in str(exc).lower()
            continue
        if matches:
            feasible.append(candidate)
    assert actual == max(feasible) == 1


@pytest.mark.var_integer
@pytest.mark.cons_perrow
@pytest.mark.correctness
def test_decision_free_lossy_cast_uses_duckdb_evaluation(decidb_cli, duckdb_conn):
    sql_bound = "CAST(1.6 AS INTEGER)"
    rows, cols = decidb_cli.execute(f"""
        SELECT x
        FROM (VALUES (1)) t(id)
        DECIDE x(INT)
        SUCH THAT x <= {sql_bound}
        MAXIMIZE SUM(x)
    """)
    actual = int(rows[0][cols.index("x")])
    expected = int(duckdb_conn.execute(f"SELECT {sql_bound}").fetchone()[0])
    assert actual == expected == 2


@pytest.mark.parametrize("cli_fixture", ["decidb_cli_highs", "decidb_cli_gurobi"])
@pytest.mark.var_boolean
@pytest.mark.cons_perrow
@pytest.mark.query_diagnostics
@pytest.mark.correctness
def test_lossy_cast_generated_rows_keep_one_diagnostic_clause(
    request, cli_fixture
):
    """An equality's two preimage boundaries remain one editable user clause."""
    cli = request.getfixturevalue(cli_fixture)
    result = cli.execute_script(
        ".mode csv\n"
        "PRAGMA diagnose_decide='auto';\n"
        "SELECT id, value, lim, x "
        "FROM (VALUES "
        "(1, 9007199254740992::BIGINT, 9007199254740994::DOUBLE)"
        ") t(id, value, lim) "
        "DECIDE x(BOOL) "
        "SUCH THAT x + value = lim "
        "MAXIMIZE SUM(x);\n"
        "SELECT * FROM decide_diagnostics();\n"
    )
    rows = list(csv.DictReader(io.StringIO(result.stdout)))
    assert rows and {row["state"] for row in rows} == {"infeasible"}
    edits = [
        row for row in rows
        if row["subject_kind"] == "clause" and row["attribute"] == "edit_kind"
    ]
    assert len(edits) == 1, rows
    assert edits[0]["value"] == "loosen"
    # Rigid cast-defined-domain rows must never become diagnostic edit subjects.
    assert all("structural" not in row["subject"].lower() for row in rows)


@pytest.mark.var_integer
@pytest.mark.cons_perrow
@pytest.mark.correctness
def test_lossy_cast_highs_and_gurobi_match(
    decidb_cli_highs, decidb_cli_gurobi, duckdb_conn
):
    sql = """
        SELECT id, value, lim, x
        FROM (VALUES
            (1, 1000000::BIGINT, 1000002::DOUBLE),
            (2, 1000003::BIGINT, 1000005::DOUBLE)
        ) t(id, value, lim)
        DECIDE x(INT)
        SUCH THAT x >= -2 AND x <= 4 AND x + value <> lim
        MAXIMIZE SUM(x)
    """
    highs_rows, highs_cols = decidb_cli_highs.execute(sql)
    gurobi_rows, gurobi_cols = decidb_cli_gurobi.execute(sql)

    def decisions(rows, cols):
        ci = {name: i for i, name in enumerate(cols)}
        return sorted((int(row[ci["id"]]), int(row[ci["x"]])) for row in rows)

    expected = duckdb_conn.execute("""
        SELECT id, MAX(candidate)
        FROM range(-2, 5) c(candidate), (VALUES
            (1, 1000000::BIGINT, 1000002::DOUBLE),
            (2, 1000003::BIGINT, 1000005::DOUBLE)
        ) t(id, value, lim)
        WHERE candidate + value <> lim
        GROUP BY id ORDER BY id
    """).fetchall()
    assert decisions(highs_rows, highs_cols) == expected
    assert decisions(gurobi_rows, gurobi_cols) == expected


@pytest.mark.var_integer
@pytest.mark.cons_perrow
@pytest.mark.correctness
def test_magnitudes_beyond_double_domain_are_a_documented_limit(
    decidb_cli, duckdb_conn
):
    """Past 2^53 a DECIDE model and row-wise SQL may disagree, by design.

    A solver model is built in DOUBLE -- `solver_input.hpp` hands the backend
    `vector<double>` for every bound and coefficient -- so it carries about 15
    significant digits and nothing finer survives to the solver either way.  Inside
    that domain DECIDE and SQL agree exactly, which every other test in this file
    pins.  Outside it they can differ, because SQL rounds each side of the written
    comparison independently while DECIDE folds the whole bound once.

    This test exists so that limit is a *recorded* property rather than an accident
    nobody notices: it asserts the divergence, not agreement.  If a future change
    makes these match, that is a real improvement -- update this test deliberately,
    do not delete it.  See `syntax_reference.md` -> Numeric precision.
    """
    sql = """
        SELECT id, x
        FROM (VALUES (1, 9007199254740990::BIGINT)) t(id, value)
        DECIDE x(INT)
        SUCH THAT x >= 0 AND x <= 6 AND x + value <= 9007199254740992::DOUBLE
        MAXIMIZE SUM(x)
    """
    rows, cols = decidb_cli.execute(sql)
    decidb = int(rows[0][cols.index("x")])

    row_wise_sql = duckdb_conn.execute("""
        SELECT MAX(candidate)
        FROM range(0, 7) c(candidate)
        WHERE candidate + 9007199254740990::BIGINT <= 9007199254740992::DOUBLE
    """).fetchone()[0]

    # Exact arithmetic on the folded bound: 9007199254740992 - 9007199254740990.
    assert decidb == 2, f"folded-bound answer changed: {decidb}"
    # Row-wise SQL admits more, because 9007199254740993 rounds down to the limit.
    assert int(row_wise_sql) == 3
    assert decidb != row_wise_sql, (
        "DECIDE and row-wise SQL now agree past 2^53 -- if that is intentional, "
        "update this test and syntax_reference.md together"
    )


@pytest.mark.var_integer
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_decision_bearing_cast_in_objective(decidb_cli, oracle_solver):
    """The objective path erases casts on the same terms as the constraint path.

    `ExtractLinearAndBilinearTerms` and `ExtractAggregateObjectiveTerms` peel casts
    without any guard of their own; they are correct only because the canonicalizer
    rejects value-changing decision casts first.  Nothing covered the objective side
    of that before, so a divergence here would have been silent.
    """
    rows, cols = decidb_cli.execute("""
        SELECT id, w, x
        FROM (VALUES (1, 2.5), (2, 3.5), (3, 1.5)) t(id, w)
        DECIDE x(INT)
        SUCH THAT x >= 0 AND x <= 4 AND SUM(x) <= 6
        MAXIMIZE SUM(CAST(x AS DOUBLE) * w)
    """)
    ci = {name: i for i, name in enumerate(cols)}
    chosen = {int(r[ci["id"]]): int(r[ci["x"]]) for r in rows}
    weights = {1: 2.5, 2: 3.5, 3: 1.5}

    assert sum(chosen.values()) <= 6
    # Greedy is optimal here: spend the budget on the heaviest rows first.
    assert chosen == {1: 2, 2: 4, 3: 0}, chosen
    achieved = sum(weights[i] * v for i, v in chosen.items())
    assert abs(achieved - 19.0) < 1e-9, achieved
