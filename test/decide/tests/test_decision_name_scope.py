"""A decision's name must not resolve to a column of the input."""

import pytest

DATA = "(VALUES (1, 10), (2, 20)) t(a, v)"


@pytest.mark.error_binder
@pytest.mark.error
def test_decision_named_like_a_using_column(decidb_cli):
    decidb_cli.assert_error_at(
        """SELECT * FROM (VALUES (1, 10), (2, 20)) A(k, v) JOIN (VALUES (1, 5), (2, 6)) B(k, w) USING (k)
        DECIDE k(INT) SUCH THAT k <= 3 MAXIMIZE SUM(k)""",
        kind="Binder Error",
        at="k(INT)",
    )


@pytest.mark.error_binder
@pytest.mark.error
@pytest.mark.parametrize(
    "decisions, where, at",
    [
        # The caret is at the decision in the WHERE clause, which a quoted `'x'` in the message would not show.
        ("x(INT)", "x > 1", "x > 1"),
        ("x(INT), y(INT)", "y > 1", "y > 1"),
        # A decision read by a subquery of the WHERE clause is a correlated column, which holds the location it was
        # written at.
        ("x(INT)", "(SELECT x) > 1", "x"),
    ],
    ids=["decision", "second-decision", "decision-in-correlated-subquery"],
)
def test_decision_in_where(decidb_cli, decisions, where, at):
    decidb_cli.assert_error_at(
        f"""SELECT * FROM {DATA} WHERE {where}
        DECIDE {decisions} SUCH THAT x <= 3 MAXIMIZE SUM(x)""",
        kind="Binder Error",
        at=at,
    )


def test_data_column_in_where_still_filters(decidb_cli):
    rows, _ = decidb_cli.execute(
        f"""SELECT a, x FROM {DATA} WHERE v > 10
        DECIDE x(INT) SUCH THAT x <= 3 MAXIMIZE SUM(x)"""
    )
    assert [row[0] for row in rows] == [2]


def _helper_named_query(decision, coefficient, key):
    """A query whose ABS, reducer body and `per` key each give the optimizer a helper to name."""
    return (
        f'SELECT id, "{decision}" FROM (VALUES (1, 5.0, 1), (2, -3.0, 1), (3, 4.0, 2)) t(id, "{coefficient}", "{key}") '
        f'DECIDE "{decision}"(INT) SUCH THAT "{decision}" <= 3 AND "{decision}" >= -3 '
        f'AND per "{key}": SUM(ABS("{decision}")) <= 4 MAXIMIZE SUM("{decision}" * "{coefficient}")'
    )


@pytest.mark.parametrize("decision", ["__abs_aux_0__", "__null_guard__", "x__value_0__"])
def test_decision_name_like_a_helper_solves(decidb_cli, decision):
    """A helper is found by its index, never by its name: a decision and the columns a clause reads may carry a
    helper's name, and the query solves as it does under any other names."""
    plain, _ = decidb_cli.execute(_helper_named_query("x", "c", "k"))
    named, _ = decidb_cli.execute(_helper_named_query(decision, "__body_0__", "__value_0__"))
    assert named == plain
    assert len(named) == 3
