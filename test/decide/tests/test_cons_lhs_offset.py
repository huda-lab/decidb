"""Regression: per-row constraint with a constant/data-led LHS and a decision
variable on the RHS must be enforced.

Previously the LHS constant/data part was silently dropped in the multi-variable
per-row path, so `(10 - x) <= y` (meaning `x + y >= 10`) was treated as
`x + y >= 0`. See 07_issues/bugs/done.md.
"""

import pytest


def _xy_sums(rows, cols):
    xi, yi = cols.index("x"), cols.index("y")
    return [float(r[xi]) + float(r[yi]) for r in rows]


_Q = """
    SELECT l_orderkey, l_linenumber, x, y
    FROM lineitem WHERE l_orderkey <= 2
    DECIDE x IS REAL, y IS REAL
    SUCH THAT {cons}
    MINIMIZE SUM(x + y)
"""


@pytest.mark.correctness
def test_const_led_lhs_with_rhs_var_enforced(decidb_cli):
    """(10 - x) <= y  ⇒  x + y >= 10 on every row (constant not dropped)."""
    rows, cols = decidb_cli.execute(_Q.format(cons="(10 - x) <= y"))
    assert all(s >= 10 - 1e-6 for s in _xy_sums(rows, cols))


@pytest.mark.correctness
def test_const_led_matches_decide_led_form(decidb_cli):
    """The data-led form matches its algebraic equivalent `x + y >= 10`."""
    a, ca = decidb_cli.execute(_Q.format(cons="(10 - x) <= y"))
    b, cb = decidb_cli.execute(_Q.format(cons="x + y >= 10"))
    assert sum(_xy_sums(a, ca)) == pytest.approx(sum(_xy_sums(b, cb)), abs=1e-4)


@pytest.mark.correctness
def test_data_column_led_lhs_with_rhs_var_enforced(decidb_cli):
    """LHS leads with a DATA column: (l_quantity - x) <= y ⇒ x + y >= l_quantity."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_quantity, x, y
        FROM lineitem WHERE l_orderkey <= 2
        DECIDE x IS REAL, y IS REAL
        SUCH THAT (l_quantity - x) <= y
        MINIMIZE SUM(x + y)
    """
    rows, cols = decidb_cli.execute(sql)
    qi, xi, yi = cols.index("l_quantity"), cols.index("x"), cols.index("y")
    for r in rows:
        assert float(r[xi]) + float(r[yi]) >= float(r[qi]) - 1e-6
