"""DECIDB_EVALUATOR=verify, the harness the deck's evaluator is measured against (literal plan, §7): the flat
evaluator runs and alone prints, a second run follows in silence, and the two are compared on what each would hand the
model. Until the tree exists the second run is flat too, so a difference is a fault in the harness. Each compared group
holds an injected difference here, which is how the comparison shows it can fail."""

import os
import subprocess

import pytest

from ._output_helpers import assert_no_internal_leak

ROWS = "(VALUES (1, 'a', 3.0::DOUBLE), (2, 'a', 4.0::DOUBLE), (3, 'b', 5.0::DOUBLE)) t(id, g, c)"
PLAIN = f"SELECT id, x FROM {ROWS} DECIDE x(REAL) SUCH THAT x <= t.c MAXIMIZE sum(x)"

#: One query per compared group, each holding a field of it.
GROUPS = {
    "rows": PLAIN,
    "terms": PLAIN,
    "bounds": PLAIN,
    "clauses": PLAIN,
    "names": f"SELECT id, x FROM {ROWS} DECIDE x(REAL) SUCH THAT x <= 9 AND sum(x) by (t.g) <= 5 MAXIMIZE sum(x)",
    "mappings": f"SELECT id, y FROM {ROWS} DECIDE per t.g: y(REAL) SUCH THAT y <= t.c MAXIMIZE sum(y)",
    "links": f"SELECT id, x FROM {ROWS} DECIDE x(REAL) SUCH THAT x <= 9 AND sum(max(x) by (t.g)) <= 5 MAXIMIZE sum(x)",
    "frames": f"""SELECT id, x FROM {ROWS} DECIDE x(REAL)
        SUCH THAT x <= 9 AND x <= at(previous else 1: x) over (t.id) MAXIMIZE sum(x)""",
    "instances": f"SELECT id, x FROM {ROWS} DECIDE x(REAL) SUCH THAT x <= 9 AND sum(x) by (t.g) <= 5 MAXIMIZE sum(x)",
    "accounts": f"""SELECT id, x FROM {ROWS} DECIDE x(REAL)
        SUCH THAT x <= 9 AND sum(when t.c > 100: x) by (t.g) <= 5 AND sum(x) by (t.g) <= 7 MAXIMIZE sum(x)""",
}


def _verify(cli, sql, **env):
    """On the solver path, in a process of its own: a difference is an INTERNAL error, which ends a shared session. The
    serializer check runs a query twice, from the round-tripped plan, so it is off: these count one execution."""
    wrapper = cli.without_verify_serializer().with_env({"DECIDB_EVALUATOR": "verify", **env})
    return wrapper.execute_raw(f"SET threads=1; SET decide_direct_solve='off'; {sql}")


@pytest.mark.parametrize("group", list(GROUPS))
def test_an_injected_difference_is_caught(decidb_cli, group):
    """A difference is an INTERNAL error, so a test run under `verify` fails with none written: every query the wrapper
    runs is checked for one. This expects one, so it runs the binary itself."""
    env = {**os.environ, "DECIDB_EVALUATOR": "verify", "DECIDB_EVALUATOR_INJECT": group}
    sql = f"SET threads=1; SET decide_direct_solve='off'; {GROUPS[group]}"
    result = subprocess.run([decidb_cli.exe, "-c", sql], env=env, capture_output=True, text=True, timeout=120)
    assert f"DECIDE evaluators differ in {group} (" in result.stderr, result.stderr


@pytest.mark.parametrize("sql", list(dict.fromkeys(GROUPS.values())))
def test_two_runs_of_one_evaluator_agree(decidb_cli, sql):
    result = _verify(decidb_cli, sql)
    assert "differ" not in result.stderr, result.stderr
    assert_no_internal_leak(result, "two runs of the flat evaluator")


def test_a_difference_of_one_ulp_is_no_difference(decidb_cli):
    """A second evaluator may sum in another order, so doubles compare within a few ulps."""
    result = _verify(decidb_cli, PLAIN, DECIDB_EVALUATOR_INJECT="bounds:ulp")
    assert "differ" not in result.stderr, result.stderr


def test_only_the_first_run_prints(decidb_cli):
    """The clause's one instance is skipped, which warns once, not once per run."""
    sql = f"""SELECT id, x FROM {ROWS} DECIDE x(REAL)
        SUCH THAT x <= 9 AND sum(when t.c > 100: x) <= 5 MAXIMIZE sum(x)"""
    result = _verify(decidb_cli, sql)
    assert result.stderr.count("constrains nothing") == 1, result.stderr


def test_an_error_both_runs_raise_is_raised_once(decidb_cli):
    """A NaN bound stops both runs the same way, so the user sees the flat evaluator's error."""
    sql = """SELECT id, x FROM (VALUES (1, 'NaN'::DOUBLE)) t(id, c) DECIDE x(REAL)
        SUCH THAT x <= at(first: t.c) over (t.id) MAXIMIZE sum(x)"""
    result = _verify(decidb_cli, sql)
    assert "is NaN on some rows" in result.stderr and "differ" not in result.stderr, result.stderr


def test_an_answer_does_not_change(decidb_cli):
    flat, _ = decidb_cli.execute(f"{PLAIN} ORDER BY id")
    verified, _ = decidb_cli.with_env({"DECIDB_EVALUATOR": "verify"}).execute(f"{PLAIN} ORDER BY id")
    assert verified == flat


@pytest.mark.parametrize("value", ["tree", "tree-noside"])
def test_the_tree_executes_the_simple_model(decidb_cli, value):
    sql = f"SET decide_direct_solve='off'; {PLAIN} ORDER BY id"
    flat, _ = decidb_cli.with_env({"DECIDB_EVALUATOR": "flat"}).execute(sql)
    tree, _ = decidb_cli.with_env({"DECIDB_EVALUATOR": value}).execute(sql)
    assert tree == flat


def test_an_unknown_evaluator_is_named(decidb_cli):
    result = decidb_cli.with_env({"DECIDB_EVALUATOR": "bogus"}).execute_raw(f"SET decide_direct_solve='off'; {PLAIN}")
    assert "DECIDB_EVALUATOR=bogus" in result.stderr, result.stderr
