"""Aggregate/per-row homogeneity at the canonicalization boundary.

A reducer anywhere in a comparison makes the comparison aggregate-shaped.  A
bare decision term beside it is legal only when that decision is one value for
the whole query.  Row- and entity-scoped decisions must occur inside a reducer;
otherwise no single solver value exists to compare with the reduced number.

These tests also pin error ownership.  Unsupported mixtures are binder/planning
errors with SQL-level guidance, never late physical-extractor errors.  ``PER``
uses the same validated classification, so a data-only reducer cannot disguise
a per-row decision constraint as aggregate.
"""

import pytest


@pytest.mark.var_integer
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_query_wide_decision_beside_data_reducer(decidb_cli):
    """A scalar decision is row-invariant and may sit beside a data reducer."""
    rows, cols = decidb_cli.execute("""
        SELECT id, p, s
        FROM (VALUES (1, 1), (2, 2), (3, 3)) t(id, p)
        DECIDE PER (): s(INT)
        SUCH THAT SUM(p) + s <= 10 AND s <= 10
        MAXIMIZE s
    """)
    si = cols.index("s")
    assert {int(r[si]) for r in rows} == {4}, rows


@pytest.mark.var_integer
@pytest.mark.cons_aggregate
@pytest.mark.per_clause
@pytest.mark.correctness
def test_query_wide_decision_beside_per_data_reducer(decidb_cli):
    """The same scalar participates in every PER group's reduced bound."""
    rows, cols = decidb_cli.execute("""
        SELECT id, grp, p, s
        FROM (VALUES (1, 'a', 1), (2, 'a', 2), (3, 'b', 3)) t(id, grp, p)
        DECIDE PER (): s(INT)
        SUCH THAT PER grp: SUM(p) BY (grp) + s <= 10 AND s <= 10
        MAXIMIZE s
    """)
    si = cols.index("s")
    assert {int(r[si]) for r in rows} == {7}, rows


@pytest.mark.var_integer
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_row_scoped_decision_inside_reducer_remains_legal(decidb_cli):
    """Row scope is legal when the reducer owns the row-to-one collapse."""
    rows, cols = decidb_cli.execute("""
        SELECT id, p, x
        FROM (VALUES (1, 1), (2, 2), (3, 3)) t(id, p)
        DECIDE x(INT)
        SUCH THAT SUM(p * x) <= 6 AND x <= 3
        MAXIMIZE SUM(x)
    """)
    xi = cols.index("x")
    assert sum(int(r[xi]) * int(r[cols.index("p")]) for r in rows) <= 6


@pytest.mark.var_integer
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_row_scoped_decision_beside_data_reducer_is_per_row(decidb_cli):
    """``SUM(p) + x <= 10`` is one row per tuple: the reducer is the global sum (6)
    and ``x`` is the tuple's own, so every row reads ``x <= 4``.

    DeciQL separates generation (omitted: per row) from aggregation (omitted BY:
    the whole input), so a reducer beside a direct term is a well-defined mixed
    shape rather than an ambiguity to reject.
    """
    rows, cols = decidb_cli.execute("""
        SELECT id, p, x
        FROM (VALUES (1, 1), (2, 2), (3, 3)) t(id, p)
        DECIDE x(INT)
        SUCH THAT SUM(p) + x <= 10
        MAXIMIZE SUM(x)
    """)
    xi = cols.index("x")
    assert sorted(int(r[xi]) for r in rows) == [4, 4, 4]


@pytest.mark.var_integer
@pytest.mark.cons_aggregate
@pytest.mark.sql_joins
@pytest.mark.correctness
def test_entity_scoped_decision_beside_global_reducer_is_per_row(decidb_cli, duckdb_conn):
    """A per-nation decision beside the global balance sum: one row per joined
    tuple, each reading the same global sum and the tuple's nation decision."""
    total = duckdb_conn.execute(
        "SELECT SUM(c_acctbal) FROM customer WHERE c_custkey <= 20"
    ).fetchone()[0]
    nations = duckdb_conn.execute(
        "SELECT COUNT(DISTINCT c_nationkey) FROM customer WHERE c_custkey <= 20"
    ).fetchone()[0]
    bound = float(total) + 5
    rows, cols = decidb_cli.execute(f"""
        SELECT c.c_custkey, n.n_nationkey, y
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE c.c_custkey <= 20
        DECIDE PER n: y(INT)
        SUCH THAT SUM(c.c_acctbal) + y <= {bound} AND y >= 0 AND y <= 10
        MAXIMIZE SUM(PER n: y)
    """)
    ni, yi = cols.index("n_nationkey"), cols.index("y")
    per_nation = {r[ni]: int(r[yi]) for r in rows}
    assert len(per_nation) == nations
    assert all(v == 5 for v in per_nation.values())


@pytest.mark.var_integer
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_row_varying_coefficient_on_scalar_beside_reducer_is_per_row(decidb_cli):
    """``SUM(p) + p * s <= 100`` is one row per tuple: ``6 + p * s <= 100`` for
    p = 1, 2, 3, so the binding row is p = 3 and ``s = 31``."""
    rows, cols = decidb_cli.execute("""
        SELECT id, p, s
        FROM (VALUES (1, 1), (2, 2), (3, 3)) t(id, p)
        DECIDE PER (): s(INT)
        SUCH THAT SUM(p) + p * s <= 100
        MAXIMIZE s
    """)
    si = cols.index("s")
    assert {int(r[si]) for r in rows} == {31}


@pytest.mark.var_integer
@pytest.mark.per_clause
@pytest.mark.error_binder
@pytest.mark.error
def test_data_only_reducer_cannot_make_per_row_constraint_eligible_for_per(decidb_cli):
    """``PER grp`` generates one row per group, so a bare ``x`` beside the
    per-group ``SUM(p)`` is a decision the key does not determine: the
    functional-dependency check names it and the fix (reduce it with ``BY``).
    """
    decidb_cli.assert_error("""
        SELECT id, grp, p, x
        FROM (VALUES (1, 'a', 1), (2, 'a', 2), (3, 'b', 3)) t(id, grp, p)
        DECIDE x(INT)
        SUCH THAT PER grp: SUM(p) BY (grp) + x <= 10
        MAXIMIZE SUM(x)
    """, match=r"Binder Error: decision 'x' is generated once per row, which PER grp does not identify")
