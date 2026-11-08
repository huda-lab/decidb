"""Requesting the tree under flat (DECIDB_DUMP_TREE) builds the tree beside flat's plan: every rewrite that runs only
for the tree keeps its own copy, and flat's EXPLAIN and answer stay as written. One query per such rewrite, each the one
that reaches it."""

import re

import pytest

ROWS = "(VALUES (1, 'a', 3.0, 2.0, true), (2, 'a', 5.0, 1.0, false), (3, 'b', 1.0, 4.0, true)) t(id, g, c, w, p)"

#: Each query reaches one tree-only rewrite (named by its id).
TREE_ONLY_REWRITES = {
    # DirectInstallRowChecks: the record's spelling, which the tree gives its reads.
    "record-spelling": f"""SELECT id, x FROM {ROWS} DECIDE x(INT)
        SUCH THAT x <= 2 * t.c AND sum(x) <= avg(t.c) * 3 MAXIMIZE sum(x)""",
    # InlineSumOfGroupSums: the nest before it is inlined (retained_expressions).
    "retained-nest": f"""SELECT id, x FROM {ROWS} DECIDE x(INT)
        SUCH THAT x <= 3 MAXIMIZE sum(per t.g: sum(t.w * x) by (t.g))""",
    # KeepTreeReducerScope: a flat MIN/MAX objective keeps its reducer's scope for the tree.
    "extremum-scope": f"SELECT id, x FROM {ROWS} DECIDE x(REAL) SUCH THAT x >= t.c MINIMIZE max(x)",
    # MakeFrameAggregate: a qualified reducer over a frame keeps its qualifier for the tree.
    "frame-aggregate-scope": f"""SELECT id, x FROM {ROWS} DECIDE x(REAL)
        SUCH THAT x <= sum(per t.g: at(first: t.c) over (t.id within t.g)) MAXIMIZE sum(x)""",
    # RewriteComposedMinMaxObjective: an objective of MIN/MAX terms alone keeps its `when` for the tree.
    "composed-objective-when": f"""SELECT id, x FROM {ROWS} DECIDE x(REAL)
        SUCH THAT x <= 9 MAXIMIZE when t.p: max(x) + min(x)""",
}

DUMP = re.compile(r"DECIDB_TREE_BEGIN\n.*?DECIDB_TREE_END\n", re.DOTALL)


def _flat(cli, sql, **env):
    wrapper = cli.with_env({"DECIDB_EVALUATOR": "flat", **env})
    result = wrapper.execute_raw(f"SET threads=1; SET decide_direct_solve='off'; {sql}")
    return result.returncode, result.stdout, DUMP.sub("", result.stderr)


@pytest.mark.parametrize("rewrite", list(TREE_ONLY_REWRITES))
@pytest.mark.parametrize("explain", [False, True], ids=["answer", "explain"])
def test_requesting_the_tree_leaves_flat_as_written(decidb_cli, rewrite, explain):
    sql = ("EXPLAIN " if explain else "") + TREE_ONLY_REWRITES[rewrite] + (" ORDER BY id" if not explain else "")
    written = _flat(decidb_cli, sql)
    assert written[0] == 0, written[2]
    assert _flat(decidb_cli, sql, DECIDB_DUMP_TREE="1") == written
