"""A descent per thread, merged in Combine (``pipeline/08_execution/done.md``): each thread's descent reads its own
rows, numbered from its first, and DecideDescent::Merge takes it into the operator's at the rows placed before. The
merged containers are those one descent over every row would fill, so the model and every message are the same.

``DECIDB_DESCENT_SPLIT=n`` merges every ``n`` rows as a thread of its own would; each case compares a query under it
with the query read by one descent, and names the part of Merge it reaches.
"""

import pytest

from . import _diagnose_relation
from ._scope_cases import values

SETUP = "SET threads = 1; SET decide_direct_solve = 'off';\n"

# Row 6's g is NULL: it lies in no class of t.g, and its group of `by (t.g)` in none.
ROWS = (
    (1, "a", "k1", 1, 4),
    (2, "b", "k2", 1, 6),
    (3, "a", "k1", 2, 1),
    (4, "c", "k3", 1, 7),
    (5, "b", "k2", 2, 3),
    (6, None, "k1", 3, 2),
)
SOURCE = f"FROM {values(ROWS, 't', ('id', 'g', 'k', 'period', 'v'))}"
COLLATED_ROWS = ((1, "b"), (2, "B"), (3, "b"), (4, "c"))
COLLATED = f"FROM (SELECT id, g COLLATE NOCASE AS g FROM {values(COLLATED_ROWS, 'v', ('id', 'g'))}) t"

#: Each query reaches a part of Merge no other case reaches.
MODEL_CASES = {
    # A keyed decision's classes: a later piece finds 'a' and 'b' again, makes 'c' and reads none on row 6
    # (class_map, known, new or none), and its G a slot in no class there (in_a_class).
    "keyed-classes": f"""SELECT id, x, y {SOURCE} DECIDE x(INT), per t.g: y(INT)
        SUCH THAT x <= 9 AND x <= y AND y <= 5 AND per t.g: sum(x) by (t.g) <= 7 MAXIMIZE sum(x) + sum(y) ORDER BY id""",
    # A set E's instances of a decision per row, rebased by the row, and of a keyed one, by its scope's classes.
    "a-set-e-of-decisions": f"""SELECT id, x {SOURCE} DECIDE x(INT), per t.g: y(INT)
        SUCH THAT y <= 3 AND x <= 9 AND per t.id: x + y <= 9 MAXIMIZE sum(x) + sum(y) ORDER BY id""",
    # An E within a G's slot, found by its enclosing slot and key (group_maps).
    "a-class-lies-in-the-group-it-feeds": f"""SELECT id, x {SOURCE} DECIDE x(INT)
        SUCH THAT x <= 9 AND x <= sum(per t.k: t.v) by (t.g) MAXIMIZE sum(t.id * x) ORDER BY id""",
    # An E of an empty key within a G's slot, one per slot (empty_key_slots, by its enclosing slot).
    "an-empty-key-in-a-group": f"""SELECT id, x {SOURCE} DECIDE x(INT)
        SUCH THAT x <= 9 AND x <= sum(per (): t.v) by (t.g) MAXIMIZE sum(x) ORDER BY id""",
    # A set E reads an O's `else` at the anchor's own row, which is rebased (rebase_read, fallback).
    "an-else-read-at-its-row": f"""SELECT id, x {SOURCE} DECIDE x(REAL)
        SUCH THAT x <= 9 AND per t.id: x <= at(previous else 0.5: x) over (t.period within t.k) + 2
        MAXIMIZE sum(x) ORDER BY id""",
    # An O two reducers' operands read, whose rows arrive in the slots of both (MIXED_ENCLOSING).
    "an-o-two-reducers-read": f"""SELECT id, x {SOURCE} DECIDE x(INT)
        SUCH THAT x <= 9 AND sum(x + max(at(previous: t.v) over (t.period within t.k)) by (t.k)) <= 40
        MAXIMIZE sum(x) ORDER BY id""",
    # A collated group's spelling, 'b' in the first piece and 'B' in a later one: the least, folded; 'c' is first
    # spelled in a later piece (DecideSpelling::Fold).
    "a-collated-group": f"""SELECT id, x {COLLATED} DECIDE x(BOOL)
        SUCH THAT per t.g: sum(x) by (t.g) <= 1 MAXIMIZE sum(x) ORDER BY id""",
    # A query-wide factor, a scalar subquery's value, read on the input's first row: the first piece's
    # (MergeDecideDescent).
    "a-query-wide-factor": f"""SELECT id, x {SOURCE} DECIDE x(INT), per (): c(INT)
        SUCH THAT x <= 9 AND c <= 3 AND c * (SELECT max(w) FROM (VALUES (4), (7)) s(w)) + sum(x) <= 60
        MAXIMIZE sum(x) + c ORDER BY id""",
}


def _split(cli, rows):
    return cli.with_env({"DECIDB_DESCENT_SPLIT": str(rows)})


def _containers(cli, sql):
    """The descent's containers once every row is read (DecideDescent::Dump)."""
    result = cli.with_env({"DECIDB_DUMP_CONTAINERS": "1"}).execute_raw(SETUP + sql)
    return result.stderr.split("DECIDB_CONTAINERS_BEGIN\n", 1)[1].split("DECIDB_CONTAINERS_END", 1)[0]


@pytest.mark.parametrize("sql", list(MODEL_CASES.values()), ids=list(MODEL_CASES))
def test_merged_descents_fill_the_containers_one_descent_fills(decidb_cli, tmp_path, sql):
    """Every slot, class, entry, instance, read and routed row, numbered as one descent numbers them, and so the
    model."""
    cli = decidb_cli.without_verify_serializer()
    whole = _containers(cli, sql)
    model = cli.dump_model(SETUP + sql, tmp_path / "whole.dump")
    for rows in (1, 2):
        assert _containers(_split(cli, rows), sql) == whole, rows
        assert _split(cli, rows).dump_model(SETUP + sql, tmp_path / f"split{rows}.dump") == model, rows


def test_a_collated_group_is_spelled_by_its_least_spelling_in_any_piece(decidb_cli):
    """The group's first piece spells it 'b', a later one 'B': the message names 'B' (DecideSpelling::Fold of a
    merged descent's spellings)."""
    rows = ((1, "b"), (2, "B"), (3, "b"))
    sql = f"""SELECT id, x FROM (SELECT id, g COLLATE NOCASE AS g FROM {values(rows, 'v', ('id', 'g'))}) t
        DECIDE x(BOOL) SUCH THAT per t.g: sum(x) >= 4 MAXIMIZE SUM(x)"""
    found = _diagnose_relation.rows(_diagnose_relation.run(_split(decidb_cli, 1), sql, scope="expanded"))
    assert [row["group"] for row in found if row["edit_source"] == "expanded_group"] == ["B"], found


def test_an_error_ranks_at_its_rebased_row(decidb_cli):
    """Rows 2100 and 4100 fail to cast, in pieces of 2048 rows: the first raised ranks first. Unrebased, row 4100's
    piece would place it in the input's first window, before row 2100's (DecideErrorLedger::Rebase)."""
    sql = """SELECT i, x FROM (SELECT i, CASE i WHEN 2100 THEN 'first' WHEN 4100 THEN 'second' ELSE '1' END AS s
        FROM range(4200) r(i)) t
        DECIDE x(INT) SUCH THAT x <= 1 AND sum(x) by (CAST(t.s AS INT)) <= 10 MAXIMIZE sum(x)"""
    cli = decidb_cli.without_verify_serializer()
    whole = cli.execute_raw(SETUP + sql)
    split = _split(cli, 2048).execute_raw(SETUP + sql)
    assert "'first'" in whole.stderr, whole.stderr
    assert split.stderr == whole.stderr
