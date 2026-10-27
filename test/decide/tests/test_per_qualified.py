"""Qualified column references in a `per` key.

A `per` key may name a column with its relation (`per t.col:`), as every other
SQL clause (`SELECT`, `GROUP BY`, `ORDER BY`, `WHERE`) allows. The natural place
to want a qualifier is a JOIN-based DECIDE query, where the user disambiguates
the key column.

The grammar reads a key as an expression list and `makeDecideScope` accepts each
element that is a column or relation name, qualified or not; the binder resolves
it. The same scope rule serves a constraint, a reducer and the objective.

These tests pin down both:
  - Parse acceptance: each qualified form is recognized by the parser.
  - Semantic correctness: the qualifier resolves to the right column,
    so qualified and unqualified keys produce identical solutions for
    equivalent queries.
"""

import time

import pytest

from solver.types import VarType, ObjSense
from comparison.compare import compare_solutions
from ._oracle_helpers import group_indices


@pytest.mark.per_clause
@pytest.mark.correctness
def test_per_single_qualified_column_in_constraint(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """`per s.s_nationkey:` in a JOIN-based SUCH THAT constraint.

    Joins supplier × nation and groups by the supplier-side nationkey via
    the table alias. Confirms both parse acceptance and that the qualifier
    resolves to the supplier column (not nation's — they happen to share
    a name across the join key).
    """
    sql = """
        SELECT s.s_suppkey, s.s_acctbal, s.s_nationkey, n.n_name, x
        FROM supplier s JOIN nation n ON s.s_nationkey = n.n_nationkey
        DECIDE x(BOOL)
        SUCH THAT per s.s_nationkey: sum(x) by (s.s_nationkey) <= 3
        MAXIMIZE SUM(x * s_acctbal)
    """
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(s.s_suppkey AS BIGINT),
               CAST(s.s_acctbal AS DOUBLE),
               CAST(s.s_nationkey AS BIGINT)
        FROM supplier s JOIN nation n ON s.s_nationkey = n.n_nationkey
    """).fetchall()
    n = len(data)

    t_build = time.perf_counter()
    oracle_solver.create_model("per_qualified_constraint")
    for i in range(n):
        oracle_solver.add_variable(f"x_{i}", VarType.BINARY)
    for key, idxs in group_indices(data, lambda r: r[2]).items():
        oracle_solver.add_constraint(
            {f"x_{i}": 1.0 for i in idxs}, "<=", 3.0, name=f"per_{key}",
        )
    oracle_solver.set_objective(
        {f"x_{i}": data[i][1] for i in range(n)}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_rows, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[decidb_cols.index("s_acctbal")])},
    )
    perf_tracker.record(
        "per_qualified_constraint", decidb_time, build_time, result.solve_time_seconds,
        n, n, len(set(r[2] for r in data)),
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.per_clause
@pytest.mark.correctness
def test_per_multi_column_all_qualified(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """`per s.s_nationkey, n.n_regionkey:` — qualified composite key.

    A key of several qualified columns must parse and bind like a key of
    one: a regression that handles only the single-column key fails here.
    """
    sql = """
        SELECT s.s_suppkey, s.s_acctbal, s.s_nationkey, n.n_regionkey, x
        FROM supplier s JOIN nation n ON s.s_nationkey = n.n_nationkey
        DECIDE x(BOOL)
        SUCH THAT per s.s_nationkey, n.n_regionkey: sum(x) by (s.s_nationkey, n.n_regionkey) <= 2
        MAXIMIZE SUM(x * s_acctbal)
    """
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(s.s_suppkey AS BIGINT),
               CAST(s.s_acctbal AS DOUBLE),
               CAST(s.s_nationkey AS BIGINT),
               CAST(n.n_regionkey AS BIGINT)
        FROM supplier s JOIN nation n ON s.s_nationkey = n.n_nationkey
    """).fetchall()
    n = len(data)

    t_build = time.perf_counter()
    oracle_solver.create_model("per_qualified_multi")
    for i in range(n):
        oracle_solver.add_variable(f"x_{i}", VarType.BINARY)
    for key, idxs in group_indices(data, lambda r: (r[2], r[3])).items():
        oracle_solver.add_constraint(
            {f"x_{i}": 1.0 for i in idxs}, "<=", 2.0,
            name=f"per_{key[0]}_{key[1]}",
        )
    oracle_solver.set_objective(
        {f"x_{i}": data[i][1] for i in range(n)}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_rows, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[decidb_cols.index("s_acctbal")])},
    )
    perf_tracker.record(
        "per_qualified_multi", decidb_time, build_time, result.solve_time_seconds,
        n, n, len(set((r[2], r[3]) for r in data)),
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.per_clause
@pytest.mark.correctness
def test_per_multi_column_mixed_qualified_and_bare(decidb_cli, duckdb_conn):
    """`per col, t.col:` and `per t.col, col:` both parse and run.

    Confirms the rule doesn't accidentally require all-or-nothing on the
    qualifier. Uses a simpler same-result-as-bare check instead of a full
    oracle comparison — the semantic correctness is already covered by
    the all-qualified case above; this test guards parse acceptance and
    consistent results across mixed forms.
    """
    bare_sql = """
        SELECT s.s_suppkey, s.s_acctbal, s_nationkey, n_regionkey, x
        FROM supplier s JOIN nation n ON s.s_nationkey = n.n_nationkey
        DECIDE x(BOOL)
        SUCH THAT per s_nationkey, n_regionkey: sum(x) by (s_nationkey, n_regionkey) <= 2
        MAXIMIZE SUM(x * s_acctbal)
    """
    mixed1_sql = bare_sql.replace(
        "per s_nationkey, n_regionkey: sum(x) by (s_nationkey, n_regionkey)",
        "per s.s_nationkey, n_regionkey: sum(x) by (s.s_nationkey, n_regionkey)",
    )
    mixed2_sql = bare_sql.replace(
        "per s_nationkey, n_regionkey: sum(x) by (s_nationkey, n_regionkey)",
        "per s_nationkey, n.n_regionkey: sum(x) by (s_nationkey, n.n_regionkey)",
    )

    bare_rows, _ = decidb_cli.execute(bare_sql)
    mixed1_rows, _ = decidb_cli.execute(mixed1_sql)
    mixed2_rows, _ = decidb_cli.execute(mixed2_sql)

    # Adding a qualifier to one or both columns is a purely syntactic change;
    # the same selection should fall out of the solver (within the equivalent
    # optimal set — compare objective values, not row-by-row identity, since
    # multiple optima may exist).
    def obj_value(rows):
        # Reconstruct objective from the decision vector and the source data.
        return sum(
            float(r[1]) for r in rows if r[-1] == 1
        )

    assert abs(obj_value(bare_rows) - obj_value(mixed1_rows)) < 1e-6, (
        "Mixed-qualifier PER produced a different objective than bare PER"
    )
    assert abs(obj_value(bare_rows) - obj_value(mixed2_rows)) < 1e-6, (
        "Mixed-qualifier PER (other order) produced a different objective "
        "than bare PER"
    )


@pytest.mark.per_clause
@pytest.mark.correctness
def test_per_qualified_equivalent_to_unqualified(decidb_cli):
    """Adding a qualifier to a `per` key must not change semantics — pure syntax.

    Runs the same DECIDE problem twice, differing only in whether the key
    column carries a table qualifier, and compares the resulting objective
    values. Direct regression check that the qualifier propagates through
    the parse → bind → symbolic pipeline as a pure rename, not a different
    column resolution.
    """
    base_sql = """
        SELECT s.s_suppkey, s.s_acctbal, s_nationkey, x
        FROM supplier s JOIN nation n ON s.s_nationkey = n.n_nationkey
        DECIDE x(BOOL)
        SUCH THAT per {per_col}: sum(x) by ({per_col}) <= 3
        MAXIMIZE SUM(x * s_acctbal)
    """
    bare_rows, _ = decidb_cli.execute(base_sql.format(per_col="s_nationkey"))
    qual_rows, _ = decidb_cli.execute(base_sql.format(per_col="s.s_nationkey"))

    obj_bare = sum(float(r[1]) for r in bare_rows if r[-1] == 1)
    obj_qual = sum(float(r[1]) for r in qual_rows if r[-1] == 1)
    assert abs(obj_bare - obj_qual) < 1e-6, (
        f"Qualified PER changed the objective: bare={obj_bare}, "
        f"qualified={obj_qual}"
    )


@pytest.mark.per_clause
@pytest.mark.correctness
def test_per_qualified_with_table_alias(decidb_cli):
    """`FROM supplier AS s` + `per s.s_nationkey:` (alias form).

    The bug-report repro used aliased tables and a qualified key, so this
    test reproduces that exact shape and asserts it both parses and
    returns a result with the right structure.
    """
    sql = """
        SELECT s.s_suppkey, s.s_acctbal, s.s_nationkey, x
        FROM supplier AS s
        DECIDE x(BOOL)
        SUCH THAT per s.s_nationkey: sum(x) by (s.s_nationkey) <= 2
        MAXIMIZE SUM(x * s.s_acctbal)
    """
    rows, cols = decidb_cli.execute(sql)
    assert rows, "Expected at least one result row from qualified-PER alias query"
    assert "x" in cols, f"Expected 'x' in result columns, got {cols}"


@pytest.mark.per_clause
@pytest.mark.correctness
def test_per_with_when_qualified_column(decidb_cli):
    """Combined `when cond per t.col: expr` — a qualified key after a `when`.

    The scope in front reads `when` and `per` together; this pins that a
    qualified key still parses and binds when a condition precedes it.
    """
    sql = """
        SELECT s.s_suppkey, s.s_acctbal, s.s_nationkey, x
        FROM supplier s JOIN nation n ON s.s_nationkey = n.n_nationkey
        DECIDE x(BOOL)
        SUCH THAT when s.s_acctbal > 0 per s.s_nationkey: sum(x) by (s.s_nationkey) <= 3
        MAXIMIZE SUM(x * s.s_acctbal)
    """
    rows, cols = decidb_cli.execute(sql)
    assert rows, "Expected result rows from WHEN+PER+qualifier query"


@pytest.mark.per_clause
@pytest.mark.correctness
def test_per_qualified_in_objective(decidb_cli):
    """`per s.s_nationkey:` on the objective side (the two-level form).

    The objective's key sits inside its outer reducer, so it goes through
    the reducer's scope rather than a constraint's. This test confirms that
    path also accepts a qualified column.
    """
    sql = """
        SELECT s.s_suppkey, s.s_acctbal, s.s_nationkey, x
        FROM supplier s JOIN nation n ON s.s_nationkey = n.n_nationkey
        DECIDE x(BOOL)
        SUCH THAT SUM(x) <= 5
        MAXIMIZE sum(per s.s_nationkey: max(x * s.s_acctbal) by (s.s_nationkey))
    """
    rows, cols = decidb_cli.execute(sql)
    assert rows, "Expected result rows from objective-side qualified PER"


@pytest.mark.per_clause
@pytest.mark.error
def test_per_unknown_qualifier_gives_clean_error(decidb_cli):
    """`per unknown_table.col:` must produce a clean error, not crash.

    A non-existent table qualifier should be rejected at bind time with a
    diagnostic naming the offending reference. The test asserts the
    rejection path stays well-formed even though the parser now accepts
    the qualified form — without this, a future name-resolution change
    that mishandles the key's column reference could produce an uninformative
    error or even a crash.
    """
    sql = """
        SELECT s.s_suppkey, s.s_acctbal, x
        FROM supplier s
        DECIDE x(BOOL)
        SUCH THAT per unknown_table.s_nationkey: sum(x) by (unknown_table.s_nationkey) <= 3
        MAXIMIZE SUM(x * s.s_acctbal)
    """
    # Must produce some error (not parse error since the grammar now accepts
    # the form), but not crash. Accept either a binder error or a clean
    # "not found" / "does not exist" / "no column" wording.
    decidb_cli.assert_error(
        sql, match=r"(?i)(unknown_table|not found|does not exist|no column)"
    )
