"""Where a decision's name may stand outside SUCH THAT and the objective.

Two binder rules, each closing a silent wrong answer:

- A decision's name is free of every column of the FROM clause. A column a USING or
  NATURAL join merges counts: a bare name binds it ahead of every relation, so a decision
  named like it was never read -- the clauses read the join column and every decision came
  back 0.
- WHERE may only use data. It picks the rows before the solve (its filter is planned below
  the DECIDE operator), so a decision in it read whichever input column sat at the
  decision's position. It is refused however it is reached: by name, through a SELECT
  alias, `COLUMNS(*)`, a lambda body or a correlated subquery.

See `01_pipeline/02_binder/done.md` §1 and `syntax_reference.md` §10.1.
"""

import re

import pytest

from ._output_helpers import assert_no_internal_leak

ROWS = "(VALUES (1, 3, [1]), (2, 5, [3]), (3, 7, [0, 5])) t(id, cap, l)"
JOIN = "(VALUES (1, 5), (2, 6)) A(k, v) {join} (VALUES (1, 5), (2, 6)) B(k, w) {condition}"


def _conflict(name):
    return re.escape(f"DECIDE variable '{name}' conflicts with an existing column name.")


def _in_where(name):
    return re.escape(f"DECIDE: {name} is a decision; WHERE may only use data, since it picks the rows before "
                     f"the solve. Filter on {name} in an outer query")


@pytest.mark.error_binder
@pytest.mark.error
class TestNameConflicts:
    """A decision named like any column of the FROM clause is the conflict error."""

    @pytest.mark.parametrize("join, condition, name", [
        pytest.param("JOIN", "USING (k)", "k", id="using"),
        pytest.param("JOIN", "USING (k)", "K", id="using-other-case"),
        pytest.param("FULL OUTER JOIN", "USING (k)", "k", id="full-outer-using"),
        pytest.param("NATURAL JOIN", "", "k", id="natural"),
        # Not a USING column: two relations share it, where the check answered DuckDB's
        # "Ambiguous reference to column name".
        pytest.param("JOIN", "ON A.k = B.k", "k", id="shared-by-two-relations"),
    ])
    def test_join_columns(self, decidb_cli, join, condition, name):
        decidb_cli.assert_error(f"""
            SELECT w FROM {JOIN.format(join=join, condition=condition)}
            DECIDE {name}(INT) SUCH THAT {name} <= 3 MAXIMIZE SUM({name})
        """, match=_conflict(name))

    def test_split_order(self, decidb_cli):
        decidb_cli.assert_error(f"""
            SELECT w DECIDE k(INT) FROM {JOIN.format(join="JOIN", condition="USING (k)")}
            SUCH THAT k <= 3 MAXIMIZE SUM(k)
        """, match=_conflict("k"))


@pytest.mark.sql_joins
def test_a_decision_beside_a_using_join_reads_the_join_column(decidb_cli):
    """A decision with a name of its own answers over a USING join, which reads `k` as the
    merged column: x <= k + w is 1 + 5 and 2 + 6."""
    rows, _ = decidb_cli.execute(f"""
        SELECT k, x FROM {JOIN.format(join="JOIN", condition="USING (k)")}
        DECIDE x(INT) SUCH THAT x <= k + w MAXIMIZE SUM(x) ORDER BY k
    """)
    assert rows == [(1, 6), (2, 8)]


@pytest.mark.error_binder
@pytest.mark.error
class TestWhere:
    """A decision in WHERE is refused by name, however the condition reaches it."""

    @pytest.mark.parametrize("select, where, name", [
        pytest.param("x", "x > 1", "x", id="by-name"),
        pytest.param("x, y", "cap > 3 AND y = 0", "y", id="the-second-decision"),
        pytest.param("x AS z", "z > 1", "x", id="through-a-select-alias"),
        pytest.param("x + 1 AS z", "z > 1", "x", id="through-an-aliased-expression"),
        pytest.param("x", "COLUMNS(*) IS NOT NULL", "x", id="through-columns-star"),
        pytest.param("x", "len(list_filter(l, e -> e > x)) > 0", "x", id="in-a-lambda-body"),
        pytest.param("x", "x IN (SELECT 1)", "x", id="left-of-an-in-subquery"),
        # These two failed internally: the correlated column had no binding below DECIDE.
        pytest.param("x", "EXISTS (SELECT 1 WHERE x > 1)", "x", id="in-a-correlated-subquery"),
        pytest.param("x", "id IN (SELECT a FROM (VALUES (1), (2)) u(a) WHERE EXISTS (SELECT 1 WHERE a < x))",
                     "x", id="two-subqueries-down"),
    ])
    def test_refused(self, decidb_cli, select, where, name):
        sql = f"""
            SELECT id, {select} FROM {ROWS} WHERE {where}
            DECIDE x(INT), y(INT) SUCH THAT x <= cap AND y <= 1 MAXIMIZE SUM(x)
        """
        result = decidb_cli.execute_raw(sql)
        assert re.search(_in_where(name), result.stderr), result.stderr
        assert_no_internal_leak(result, "a decision in WHERE", sql)

    def test_split_order(self, decidb_cli):
        decidb_cli.assert_error(f"""
            SELECT id, x DECIDE x(INT) FROM {ROWS} WHERE x > 1
            SUCH THAT x <= cap MAXIMIZE SUM(x)
        """, match=_in_where("x"))


@pytest.mark.parametrize("where, ids", [
    pytest.param("cap > 3", [2, 3], id="a-column"),
    pytest.param("EXISTS (SELECT 1 FROM (VALUES (3), (7)) u(c) WHERE u.c = t.cap)", [1, 3],
                 id="a-correlated-data-subquery"),
    # The lambda's own `x` is its parameter, not the decision.
    pytest.param("len(list_filter(l, x -> x > 1)) > 0", [2, 3], id="a-lambda-parameter-named-like-a-decision"),
])
def test_where_on_data_keeps_its_rows(decidb_cli, where, ids):
    """WHERE over data picks the rows the decisions are made for; each kept row's x reaches
    its cap."""
    rows, _ = decidb_cli.execute(f"""
        SELECT id, x FROM {ROWS} WHERE {where}
        DECIDE x(INT) SUCH THAT x <= cap MAXIMIZE SUM(x) ORDER BY id
    """)
    caps = {1: 3, 2: 5, 3: 7}
    assert rows == [(i, caps[i]) for i in ids]


def test_an_outer_query_filters_on_the_solved_value(decidb_cli):
    """What the WHERE refusal points to. With sum(x) <= 9 and weight id, the one optimum is
    x = (0, 2, 7), objective 25; the outer filter keeps the two rows whose x exceeds 1, and
    the inner query is solved over all three."""
    rows, _ = decidb_cli.execute(f"""
        SELECT * FROM (
            SELECT id, x FROM {ROWS}
            DECIDE x(INT) SUCH THAT x <= cap AND sum(x) <= 9 MAXIMIZE SUM(x * id))
        WHERE x > 1 ORDER BY id
    """)
    assert rows == [(2, 2), (3, 7)]
