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
@pytest.mark.per_clause
@pytest.mark.error_binder
@pytest.mark.error
def test_data_only_reducer_cannot_make_per_row_constraint_eligible_for_per(decidb_cli):
    """Moving ``SUM(p)`` to the bound reveals ``x`` as a per-row constraint.

    The existing PER error is already the user-facing contract; the regression
    is that the pre-canonicalization gate currently sees the data-only reducer
    and lets the meaningless wrapper through.
    """
    decidb_cli.assert_error("""
        SELECT id, grp, p, x
        FROM (VALUES (1, 'a', 1), (2, 'a', 2), (3, 'b', 3)) t(id, grp, p)
        DECIDE x(INT)
        SUCH THAT SUM(p) + x <= 10 PER grp
        MAXIMIZE SUM(x)
    """, match=r"Binder Error: PER can only be applied to aggregate .* constraints")
