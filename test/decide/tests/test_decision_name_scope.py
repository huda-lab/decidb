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
