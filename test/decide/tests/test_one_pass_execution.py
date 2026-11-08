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
