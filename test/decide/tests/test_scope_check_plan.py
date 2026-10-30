"""The plan of the scope row checks (language/guide.md §7.1): each check is an aggregate over the source, in the
form of DuckDB's uncorrelated scalar subquery, cross-joined on the build side of the source's rows.

A determination check groups the kept rows by its key, keeps the classes that hold two values, and reports the least
of them: `arg_min` over the class's key as a struct, whose NULL fields sort last. The check reads the source a second
time: a scan of a single table is repeated, and any other source (here a join) is materialized once and read twice, with
everything above it re-pointed at the read. Each case reaches one of those branches, on the solver path (`off`) and
on the direct path (`require`), which install the checks at different sites. The messages were checked against the
plan the checks replaced, which ranked the classes with a window.
"""

import json

import pytest

from ._direct_oracle_common import _allow_direct_path  # noqa: F401
from ._direct_oracle_common import run
from ._output_helpers import assert_no_internal_leak

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

FAILING = [
    pytest.param(
        "SELECT id, x FROM (VALUES ('D1', 'East'), ('D1', NULL)) D(dk, region) "
        "JOIN (VALUES (1, 'D1', 9.0, 3), (2, 'D1', 8.0, 5)) S(id, dk, v, cap) ON D.dk = S.dk "
        "DECIDE x(BOOL) SUCH THAT per D: SUM(x) <= S.cap MAXIMIZE SUM(S.v * x)",
        "DECIDE: per D does not determine S.cap. D.dk = 'D1', D.region = 'East' has more than one value: 3, 5.",
        # Both classes fail; the one whose relation column is NULL sorts after the other.
        id="null-relation-column-sorts-last",
    ),
    pytest.param(
        "SELECT id, x FROM (VALUES ('East'), (NULL)) D(region) "
        "JOIN (VALUES (1, 'East', 9.0, 3), (2, 'East', 8.0, 3), (3, NULL, 7.0, 1), (4, NULL, 6.0, 2)) "
        "S(id, region, v, cap) ON D.region IS NOT DISTINCT FROM S.region "
        "DECIDE x(BOOL) SUCH THAT per D: SUM(x) <= S.cap MAXIMIZE SUM(S.v * x)",
        "DECIDE: per D does not determine S.cap. D.region = NULL has more than one value: 1, 2.",
        # A relation's NULL column is a class of its own, and is reported when it alone fails.
        id="null-relation-column-is-a-class",
    ),
    pytest.param(
        "SELECT id, x FROM (VALUES (1, 'a', 'x', 9.0, 3), (2, 'a', NULL, 8.0, 1), (3, 'a', NULL, 7.0, 2), "
        "(4, 'b', 'y', 6.0, 2), (5, 'b', 'y', 5.0, 4)) t(id, g, h, v, cap) "
        "DECIDE x(BOOL) SUCH THAT per t.g, t.h: SUM(x) <= cap MAXIMIZE SUM(v * x)",
        "DECIDE: per t.g, t.h does not determine cap. t.g = 'b', t.h = 'y' has more than one value: 2, 4.",
        # A column key's NULL rows are in no class, so ('a', NULL), which would sort first, is not checked.
        id="null-column-key-in-no-class",
    ),
    pytest.param(
        "SELECT id, x FROM (SELECT id, g COLLATE NOCASE AS g, v, cap FROM (VALUES (1, 'b', 9.0, 2), (2, 'B', 8.0, 5), "
        "(3, 'a', 7.0, 1), (4, 'A', 6.0, 3), (5, 'c', 5.0, 1)) v(id, g, v, cap)) t "
        "DECIDE x(BOOL) SUCH THAT per t.g: SUM(x) <= cap MAXIMIZE SUM(v * x)",
        "DECIDE: per t.g does not determine cap. t.g = 'A' has more than one value: 1, 3.",
        # A collated key groups and orders under its collation, and the class is spelled by its least label.
        id="collated-key",
    ),
    pytest.param(
        STORED.format(cap=3) + STORED_JOIN,
        "DECIDE: per d.dk does not determine s.cap. d.dk = 'D1' has more than one value: 3, 5.",
        # A join is materialized and read twice; the rows above are re-pointed at the read.
        id="join-source",
    ),
    pytest.param(
        STORED.format(cap=3) + STORED_TABLE,
        "DECIDE: per s.dk does not determine s.grp (in by (s.grp)). s.dk = 'D1' has more than one value: 'a', 'b'.",
        # The table's scan is repeated; nothing is re-pointed.
        id="table-source",
    ),
]


@pytest.mark.parametrize("mode", MODES)
@pytest.mark.parametrize("sql, message", FAILING)
def test_a_failing_check_reports_its_least_class(decidb_cli, sql, message, mode):
    result = decidb_cli.execute_raw(f"SET decide_direct_solve='{mode}'; {sql};")
    assert message in result.stderr, result.stderr
    assert_no_internal_leak(result, "a determination error")


def _operators(cli, setup: str, sql: str) -> list[str]:
    result = cli.execute_raw(f"{setup} SET decide_direct_solve='off'; EXPLAIN (FORMAT JSON) {sql};")
    plan = json.loads(result.stdout[result.stdout.index("[") :])
    names, stack = [], list(plan)
    while stack:
        node = stack.pop()
        names.append(node["name"])
        stack.extend(node.get("children", []))
    return names


@pytest.mark.explain
@pytest.mark.parametrize(
    "sql, materialized",
    [
        pytest.param(STORED_JOIN, True, id="join-source-is-materialized"),
        pytest.param(STORED_TABLE, False, id="table-source-is-scanned-again"),
    ],
)
def test_the_check_reads_the_source_a_second_time(decidb_cli, sql, materialized):
    assert ("CTE_SCAN" in _operators(decidb_cli, STORED.format(cap=5), sql)) == materialized


def test_a_passing_check_on_a_join_source_keeps_its_rows(decidb_cli):
    """The direct path re-reads its IR once the join is materialized; it solves as the solver path does."""
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
