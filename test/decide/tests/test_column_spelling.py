"""A column prints as the user wrote it, over one relation or many (D45).

The parser keeps where every column reference of a DECIDE clause sits; the binder cuts its spelling and tags the
bound reference with it, so EXPLAIN, DIAGNOSE and the messages of any stage print `S.cap` where the user wrote
`S.cap` and `cap` where they wrote `cap`. No relation name is inserted.
"""

import csv
import io
import re

import pytest

_OFF = "SET decide_direct_solve='off'; "
_JOIN = (
    "(VALUES (1, 'a', 20, true), (2, 'b', 25, true)) S(id, k, cap, priority) "
    "JOIN (VALUES ('a', 7), ('b', 9)) D(k, cap) USING (k)"
)


def _packed(text):
    return re.sub(r"[│\s]", "", text)


def _diagnosis(cli, sql):
    result = cli.execute_script(".mode csv\n" + f"DIAGNOSE {sql};\n")
    return list(csv.DictReader(io.StringIO(result.stdout))), result.stdout + result.stderr


def _answer(cli, sql):
    result = cli.execute_script(".mode csv\n" + sql + ";\n")
    assert not result.stderr.strip(), result.stderr
    return list(csv.DictReader(io.StringIO(result.stdout)))


@pytest.mark.explain
def test_two_relations_sharing_a_column_name_print_as_written(decidb_cli):
    """`cap` is in both relations; the clause echoes with the relation the user wrote on each, not `cap` twice."""
    sql = f"SELECT S.id, x FROM {_JOIN} DECIDE x(BOOL) SUCH THAT when S.cap > 12: x * D.cap >= S.cap MAXIMIZE sum(x)"
    plan = _packed(decidb_cli.execute_raw(f"{_OFF}EXPLAIN {sql}").stdout)
    assert "whenS.cap>12:x*D.cap>=S.cap" in plan, plan

    rows, text = _diagnosis(decidb_cli, sql)
    edit = rows[0]
    assert edit["clause"] == "when S.cap > 12: x * D.cap >= S.cap", text
    assert edit["suggested_change"] == "when S.cap > 12: x * D.cap >= S.cap - 16", text
    # The suggestion pasted back runs and gives the repaired answer: both rows can ship.
    repaired = sql.replace(edit["clause"].split(": ", 1)[1], edit["suggested_change"].split(": ", 1)[1])
    assert [row["x"] for row in _answer(decidb_cli, repaired)] == ["1", "1"]


@pytest.mark.explain
def test_a_bare_column_stays_bare_and_a_quoted_one_stays_quoted(decidb_cli):
    sql = (
        "SELECT id, x FROM (VALUES (1, 5, 'a'), (2, 6, 'b')) t(id, \"my cap\", g) "
        'DECIDE x(INT) SUCH THAT x >= 0 AND x <= t."my cap" AND per g: sum(x) by (g) <= "my cap" MAXIMIZE sum(x)'
    )
    plan = _packed(decidb_cli.execute_raw(f"{_OFF}EXPLAIN {sql}").stdout)
    assert 'x<=t."mycap"' in plan and 'perg:sum(x)by(g)<="mycap"' in plan, plan


@pytest.mark.explain
def test_each_clause_keeps_its_own_spelling_of_a_key(decidb_cli):
    """Two keys over the same columns share one key table entry, which holds one text; each clause echoes its own."""
    sql = (
        "SELECT x FROM (VALUES (1, 'E', 5), (2, 'W', 6)) D(d, region, cost) "
        "DECIDE x(INT) SUCH THAT x >= 0 AND sum(x) by (D.d, D.region, D.cost) <= 7 AND sum(x) by (D) <= 9 "
        "AND SUM(D: D.cost * x) <= 40 AND sum(per D.d, D.region, D.cost: x) <= 30 MAXIMIZE sum(x)"
    )
    plan = _packed(decidb_cli.execute_raw(f"{_OFF}EXPLAIN {sql}").stdout)
    for text in ("sum(x)by(D.d,D.region,D.cost)<=7", "sum(x)by(D)<=9", "SUM(D:D.cost*x)<=40"):
        assert text in plan, plan


@pytest.mark.error
def test_a_product_refusal_over_a_join_quotes_the_column_as_written(decidb_cli):
    """`decide_linear_form.cpp` refuses a product of decisions in the objective; over a join the data column reads as
    the user wrote it (it was `capacity * ship * q` with its relation lost)."""
    sql = (
        f"SELECT S.id, ship FROM (VALUES (1, 'a', 20), (2, 'b', 25)) S(id, k, demand) "
        "JOIN (VALUES ('a', 7), ('b', 9)) D(k, capacity) USING (k) DECIDE ship(INT), q(INT) "
        "SUCH THAT ship <= S.demand AND q <= 5 MAXIMIZE sum(ship) + D.capacity * ship * q"
    )
    stderr = decidb_cli.execute_raw(sql).stderr
    assert "'D.capacity * ship * q' multiplies decisions outside a reducer" in stderr, stderr
