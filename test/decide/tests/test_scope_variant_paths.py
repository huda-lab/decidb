"""A `per K:` clause whose classes hold several values of its bound is one instance per variant (language/guide.md
§7.1, D70); in the aggregate shape every instance of a group differs only in its bound, so the group takes its
tightest bound. The solver path (`off`) and the direct path (`require`, S1) read the same classes -- a relation's NULL
column is a class of its own, a column key's NULL row is in none, a collated key groups under its collation -- over a
single table and over a join, and answer alike.
"""

import pytest

from ._direct_oracle_common import _allow_direct_path  # noqa: F401
from ._direct_oracle_common import run

MODES = ("off", "require")

#: Stored tables, so the join below is not a VALUES list the binder could fold.
STORED = (
    "CREATE OR REPLACE TEMP TABLE cp_d AS FROM (VALUES ('D1', 'East'), ('D1', 'West')) d(dk, region); "
    "CREATE OR REPLACE TEMP TABLE cp_s AS FROM (VALUES (1, 'D1', 'a', 9.0, {cap}), (2, 'D1', 'b', 8.0, 5), "
    "(3, 'D1', 'b', 7.0, 5)) s(id, dk, grp, v, cap); "
)
STORED_JOIN = (
    "SELECT id, x FROM cp_d d JOIN cp_s s ON d.dk = s.dk "
    "DECIDE x(BOOL) SUCH THAT per d.dk: SUM(x) <= s.cap MAXIMIZE SUM(s.v * x) ORDER BY id, x"
)
#: A `by` key and no bound: a bound reads its class's value through a window over the source, which is then no scan.
STORED_TABLE = (
    "SELECT id, x FROM cp_s s DECIDE x(BOOL) SUCH THAT per s.dk: SUM(x) by (s.grp) <= 2 MAXIMIZE SUM(s.v * x)"
)

CASES = [
    pytest.param(
        "SELECT id, x FROM (VALUES ('D1', 'East'), ('D1', NULL)) D(dk, region) "
        "JOIN (VALUES (1, 'D1', 9.0, 3), (2, 'D1', 8.0, 5)) S(id, dk, v, cap) ON D.dk = S.dk "
        "DECIDE x(BOOL) SUCH THAT per D: SUM(x) <= S.cap MAXIMIZE SUM(S.v * x) ORDER BY id, x",
        # Two classes (East and the NULL region), each with caps 3 and 5: the total of all four rows is at most 3.
        [(1, True), (1, True), (2, False), (2, True)],
        id="null-relation-column-is-a-class",
    ),
    pytest.param(
        "SELECT id, x FROM (VALUES (1, 'a', 'x', 9.0, 3), (2, 'a', NULL, 8.0, 1), (3, 'a', NULL, 7.0, 2), "
        "(4, 'b', 'y', 6.0, 2), (5, 'b', 'y', 5.0, 4)) t(id, g, h, v, cap) "
        "DECIDE x(BOOL) SUCH THAT per t.g, t.h: SUM(x) <= cap MAXIMIZE SUM(v * x) ORDER BY id",
        # The rows whose h is NULL are in no class, so their caps bound nothing: the tightest is 2.
        [(1, True), (2, True), (3, False), (4, False), (5, False)],
        id="null-column-key-in-no-class",
    ),
    pytest.param(
        "SELECT id, x FROM (SELECT id, g COLLATE NOCASE AS g, v, cap FROM (VALUES (1, 'b', 9.0, 2), (2, 'B', 8.0, 5), "
        "(3, 'a', 7.0, 1), (4, 'A', 6.0, 3), (5, 'c', 5.0, 1)) v(id, g, v, cap)) t "
        "DECIDE x(BOOL) SUCH THAT per t.g: SUM(x) <= cap MAXIMIZE SUM(v * x) ORDER BY id",
        # The tightest cap of every class is 1.
        [(1, True), (2, False), (3, False), (4, False), (5, False)],
        id="collated-key",
    ),
    pytest.param(
        STORED.format(cap=3) + STORED_JOIN,
        [(1, True), (1, True), (2, False), (2, True), (3, False), (3, False)],
        id="join-source",
    ),
    pytest.param(
        STORED.format(cap=3) + STORED_TABLE + " ORDER BY id",
        [(1, True), (2, True), (3, True)],
        id="table-source",
    ),
]


@pytest.mark.correctness
@pytest.mark.parametrize("sql, expected", CASES)
def test_both_paths_answer_the_variants_alike(decidb_cli, sql, expected):
    answers = [[tuple(row) for row in run(decidb_cli, sql, mode)[0]] for mode in MODES]
    assert answers[0] == answers[1] == expected, answers


def test_a_join_source_keeps_its_rows(decidb_cli):
    """The rule reads the join through its reader; it solves as the solver path does."""
    sql = STORED.format(cap=5) + STORED_JOIN
    assert run(decidb_cli, sql, "require") == run(decidb_cli, sql, "off")


def test_diagnose_names_a_join_source_column_by_the_users_name(decidb_cli):
    """The user's names of the source's columns follow it into the materialized read."""
    result = decidb_cli.execute_raw(
        "CREATE OR REPLACE TEMP TABLE cp_items(id INTEGER, w INTEGER, zone VARCHAR, region VARCHAR); "
        "INSERT INTO cp_items VALUES (1, 1, 'P', 'A'), (2, 2, 'P', 'A'), (3, 3, 'P', 'A'), "
        "(4, 4, 'Q', 'A'), (5, 5, 'Q', 'A'), (6, 6, 'Q', 'A'); "
        "CREATE OR REPLACE TEMP TABLE cp_caps(region VARCHAR, cap INTEGER); INSERT INTO cp_caps VALUES ('A', 1); "
        "SET decide_direct_solve='off'; "
        "DIAGNOSE SELECT i.zone, buy FROM cp_items i JOIN cp_caps c ON i.region = c.region DECIDE buy(REAL) "
        "SUCH THAT when i.id <= 3: buy <= 100 AND per i.region: sum(buy) by (i.region) >= c.cap "
        "MAXIMIZE SUM(buy * i.w);"
    )
    assert "when zone = 'Q': buy <= <cap>" in result.stdout, result.stdout
