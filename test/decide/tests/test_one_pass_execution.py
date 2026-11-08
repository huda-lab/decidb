"""The solver consumes the bottom-up E/G/O values, including an altered child value."""

import json
import os
import re
import subprocess

import pytest

from ._anr_cases import F1_MAX, F1_SUM, F2_PREVIOUS
from ._scope_cases import answer, dump_counts


@pytest.mark.parametrize(
    "case,counts",
    [
        (F1_MAX, {"num_vars": 8, "num_rows": 7}),
        (F1_SUM, {"num_vars": 6, "num_rows": 3}),
        (F2_PREVIOUS, {"num_vars": 6, "num_rows": 4}),
    ],
    ids=lambda value: value.name if hasattr(value, "name") else None,
)
def test_tree_values_reach_the_decks_model(decidb_cli, tmp_path, case, counts):
    cli = decidb_cli.with_env({"DECIDB_EVALUATOR": "tree", "DECIDB_NATIVE_CONSTRUCTS": "off"})
    sql = "SET decide_direct_solve='off'; " + case.sql
    assert case.model().judge(answer(cli, sql), case.clauses, case.objective) == pytest.approx(case.optimum)
    assert dump_counts(cli.dump_model(sql, tmp_path / "tree.dump")) == counts


def test_changing_a_data_child_changes_the_answer(decidb_cli):
    """L7: changing the computed SUM to 8 must flow through the parent and into the solver's bound."""
    sql = """SET decide_direct_solve='off';
        SELECT id,x FROM (VALUES (1,2),(2,3)) t(id,d)
        DECIDE per (): x(REAL) SUCH THAT x <= sum(t.d) MAXIMIZE x ORDER BY id"""
    env = {**os.environ, **(decidb_cli.env or {}), "DECIDB_EVALUATOR": "tree", "DECIDB_DUMP_TREE": "1"}
    original = subprocess.run(
        [decidb_cli.exe, "-json", "-c", sql], env=env, capture_output=True, text=True, timeout=120
    )
    assert original.returncode == 0, original.stderr
    # Find the data G through the actual graph, instead of relying on a fixed planner node number.
    node = re.search(r"(?m)^\s*(\d+) G .*", original.stderr)
    assert node, original.stderr
    changed = subprocess.run(
        [decidb_cli.exe, "-json", "-c", sql],
        env={**env, "DECIDB_TREE_INJECT": node[1] + ":data:8"},
        capture_output=True,
        text=True,
        timeout=120,
    )
    assert changed.returncode == 0, changed.stderr
    assert [row["x"] for row in json.loads(original.stdout)] == [5, 5]
    assert [row["x"] for row in json.loads(changed.stdout)] == [8, 8]


#: Each query reaches a branch of the tree formulator's column box (decide_tree_formulation.cpp) that no other case
#: reaches, with its answer worked by hand. The default suite runs flat, which never reaches that file.
BOX_CASES = {
    # FoldBound refuses `<>`, which is no interval, and the `<>` is lowered after the auxiliaries, as its Big-M reads
    # the product's range: b = 1 and x = 3.
    "a-not-equal-reads-the-settled-box": (
        """SELECT id, x, b FROM (VALUES (1)) t(id) DECIDE x(INT), per (): b(BOOL)
        SUCH THAT x <= 4 AND b * SUM(x) <> 4 MAXIMIZE SUM(x) + 4 * b""",
        [{"id": 1, "x": 3, "b": True}],
    ),
    # AuxiliaryRange intersects: the folded b * x <= t.k survives the product's derived range [0, 5].
    "a-derived-range-keeps-a-folded-bound": (
        """SELECT id, x, b FROM (VALUES (1, 2), (2, 1)) t(id, k) DECIDE x(INT), b(BOOL)
        SUCH THAT x <= 5 AND b * x <= t.k MAXIMIZE SUM(x) + 4.5 * SUM(b) ORDER BY id""",
        [{"id": 1, "x": 2, "b": True}, {"id": 2, "x": 1, "b": True}],
    ),
    # A floor the user wrote moves into the instance before the auxiliary's floor lowers: |x - q| >= 30 holds.
    "a-lowered-floor-keeps-the-written-floor": (
        """SELECT id, x FROM (VALUES (1, 10.0), (2, 20.0)) t(id, q) DECIDE x(REAL)
        SUCH THAT x <= 100 AND ABS(x - q) >= 30 MINIMIZE SUM(x) ORDER BY id""",
        [{"id": 1, "x": 40.0}, {"id": 2, "x": 50.0}],
    ),
}


@pytest.mark.parametrize("case", list(BOX_CASES))
def test_the_tree_column_box(decidb_cli, case):
    sql, expected = BOX_CASES[case]
    cli = decidb_cli.with_env({"DECIDB_EVALUATOR": "tree"})
    assert answer(cli, "SET decide_direct_solve='off'; " + sql) == expected
