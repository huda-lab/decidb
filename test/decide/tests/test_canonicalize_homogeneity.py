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
        DECIDE scalar s(INT)
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
        DECIDE scalar s(INT)
        SUCH THAT SUM(p) + s <= 10 PER grp AND s <= 10
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
@pytest.mark.error_binder
@pytest.mark.error
def test_row_scoped_decision_beside_data_reducer_rejected(decidb_cli):
    """``SUM(p) + x`` is neither a per-row nor a homogeneous aggregate row."""
    decidb_cli.assert_error("""
        SELECT id, p, x
        FROM (VALUES (1, 1), (2, 2), (3, 3)) t(id, p)
        DECIDE x(INT)
        SUCH THAT SUM(p) + x <= 10
        MAXIMIZE SUM(x)
    """, match=r"Binder Error: DECIDE constraint.*row-scoped.*'x'.*outside.*(?:reducer|SUM)")


@pytest.mark.var_integer
@pytest.mark.cons_aggregate
@pytest.mark.sql_joins
@pytest.mark.error_binder
@pytest.mark.error
def test_entity_scoped_decision_beside_global_reducer_rejected(decidb_cli):
    """An entity decision is not one value for a reducer over many entities."""
    decidb_cli.assert_error("""
        SELECT c.c_custkey, n.n_nationkey, y
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE c.c_custkey <= 20
        DECIDE n.y(INT)
        SUCH THAT SUM(c.c_acctbal) + y <= 1000000000 AND y <= 10
        MAXIMIZE SUM(y)
    """, match=r"Binder Error: DECIDE constraint.*entity-scoped.*'y'.*outside.*(?:reducer|SUM)")


@pytest.mark.var_integer
@pytest.mark.cons_aggregate
@pytest.mark.error_binder
@pytest.mark.error
def test_row_varying_coefficient_on_scalar_beside_reducer_rejected(decidb_cli):
    """A scalar decision is not query-wide once multiplied by a row column."""
    decidb_cli.assert_error("""
        SELECT id, p, s
        FROM (VALUES (1, 1), (2, 2), (3, 3)) t(id, p)
        DECIDE scalar s(INT)
        SUCH THAT SUM(p) + p * s <= 100
        MAXIMIZE s
    """, match=r"Binder Error: DECIDE constraint.*varies per row.*outside.*(?:reducer|SUM)")


@pytest.mark.var_integer
@pytest.mark.per_clause
@pytest.mark.error_binder
@pytest.mark.error
def test_data_only_reducer_cannot_make_per_row_constraint_eligible_for_per(decidb_cli):
    """Moving ``SUM(p)`` to the bound reveals ``x`` as a per-row constraint.

    The existing PER error is the user-facing contract.  This pins the final
    canonical classification rather than the removed parsed-shape gate.
    """
    decidb_cli.assert_error("""
        SELECT id, grp, p, x
        FROM (VALUES (1, 'a', 1), (2, 'a', 2), (3, 'b', 3)) t(id, grp, p)
        DECIDE x(INT)
        SUCH THAT SUM(p) + x <= 10 PER grp
        MAXIMIZE SUM(x)
    """, match=r"Binder Error: PER can only be applied to aggregate .* constraints")
