"""DECIDB_EVALUATOR=verify, the harness the deck's evaluator is measured against (literal plan, §7): the flat
evaluator runs and alone prints, a second run follows in silence, and the two are compared on what each would hand the
model. Until the tree exists the second run is flat too, so a difference is a fault in the harness. Each compared group
holds an injected difference here, which is how the comparison shows it can fail."""

import os
import subprocess

import pytest

from ._output_helpers import assert_no_internal_leak
from ._verify_known import allow_setting, stale_markers

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
    "facts": PLAIN,
    "class_sets": f"""DIAGNOSE SELECT id, x FROM {ROWS} DECIDE x(REAL)
        SUCH THAT x >= 9 AND per t.g: sum(x) by (t.g) <= 5 MAXIMIZE sum(x)""",
    "absent": """SELECT id, x FROM (VALUES (1, 3.0), (2, NULL), (3, 5.0)) t(id, c) DECIDE x(REAL)
        SUCH THAT x <= 9 AND x + abs(x - t.c) <= 20 MAXIMIZE sum(x)""",
}

#: The groups whose query multiplies or squares decisions in a clause, which needs Gurobi: the solver check refuses it
#: before either run, so these run through decidb_cli_gurobi, which skips without it.
GUROBI_GROUPS = {
    "products": f"""SELECT id, x, y FROM {ROWS} DECIDE x(REAL), y(REAL)
        SUCH THAT x <= 9 AND y <= 9 AND t.c * x * y <= 10 MAXIMIZE sum(x + y)""",
    "squares": f"SELECT id, x FROM {ROWS} DECIDE x(REAL) SUCH THAT x <= 9 AND sum(power(x - t.c, 2)) <= 50 MAXIMIZE sum(x)",
}


def _verify(cli, sql, **env):
    """On the solver path, in a process of its own: a difference is an INTERNAL error, which ends a shared session. The
    serializer check runs a query twice, from the round-tripped plan, so it is off: these count one execution."""
    wrapper = cli.without_verify_serializer().with_env({"DECIDB_EVALUATOR": "verify", **env})
    return wrapper.execute_raw(f"SET threads=1; SET decide_direct_solve='off'; {sql}")


def _assert_injected_difference_caught(cli, group, sql):
    """A difference is an INTERNAL error, so a test run under `verify` fails with none written: every query the wrapper
    runs is checked for one. This expects one, so it runs the binary itself."""
    env = {**os.environ, **(cli.env or {}), "DECIDB_EVALUATOR": "verify", "DECIDB_EVALUATOR_INJECT": group}
    sql = f"SET threads=1; SET decide_direct_solve='off'; {sql}"
    result = subprocess.run([cli.exe, "-c", sql], env=env, capture_output=True, text=True, timeout=120)
    assert f"DECIDE evaluators differ in {group} (" in result.stderr, result.stderr


@pytest.mark.parametrize("group", list(GROUPS))
def test_an_injected_difference_is_caught(decidb_cli, group):
    _assert_injected_difference_caught(decidb_cli, group, GROUPS[group])


@pytest.mark.parametrize("group", list(GUROBI_GROUPS))
def test_an_injected_quadratic_difference_is_caught(decidb_cli_gurobi, group):
    _assert_injected_difference_caught(decidb_cli_gurobi, group, GUROBI_GROUPS[group])


def _assert_two_runs_agree(cli, sql):
    result = _verify(cli, sql)
    assert "differ" not in result.stderr, result.stderr
    assert_no_internal_leak(result, "two runs of the flat evaluator")


@pytest.mark.parametrize("sql", list(dict.fromkeys(GROUPS.values())))
def test_two_runs_of_one_evaluator_agree(decidb_cli, sql):
    _assert_two_runs_agree(decidb_cli, sql)


@pytest.mark.parametrize("sql", list(GUROBI_GROUPS.values()))
def test_two_runs_of_one_evaluator_agree_on_quadratic_terms(decidb_cli_gurobi, sql):
    _assert_two_runs_agree(decidb_cli_gurobi, sql)


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


def test_verify_builds_no_tree(decidb_cli):
    """Until its second run is the tree, `verify` reads the plan flat reads. A descent would refuse this chunk size."""
    result = _verify(decidb_cli, PLAIN, DECIDB_DESCENT_CHUNK="0")
    assert result.returncode == 0 and "DECIDB_DESCENT_CHUNK" not in result.stderr, result.stderr


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


def _verify_raising(cli, sql, **env):
    """As _verify, for a query expected to raise a difference, which is an INTERNAL error: the binary runs here rather
    than through the wrapper, which refuses one."""
    environment = {**os.environ, "DECIDB_EVALUATOR": "verify", **env}
    sql = f"SET threads=1; SET decide_direct_solve='off'; {sql}"
    return subprocess.run([cli.exe, "-c", sql], env=environment, capture_output=True, text=True, timeout=120)


#: Two clauses, each with a bound: `bounds (clause 0)` and `bounds (clause 1)`.
TWO_BOUNDS = f"SELECT id, x FROM {ROWS} DECIDE x(REAL) SUCH THAT x <= t.c AND x + 1 <= t.c + 2 MAXIMIZE sum(x)"


def test_an_allowed_difference_is_logged_not_raised(decidb_cli, tmp_path):
    """A difference DECIDB_VERIFY_ALLOW names is its bug in DECIDB_VERIFY_ALLOW_LOG, not an error."""
    log = tmp_path / "allowed.log"
    result = _verify(
        decidb_cli,
        PLAIN,
        DECIDB_EVALUATOR_INJECT="bounds",
        DECIDB_VERIFY_ALLOW="T1:bounds",
        DECIDB_VERIFY_ALLOW_LOG=str(log),
    )
    assert result.returncode == 0 and "differ" not in result.stderr, result.stderr
    assert log.read_text().split() == ["T1"]


def test_an_allowed_difference_needs_no_log(decidb_cli):
    result = _verify(decidb_cli, PLAIN, DECIDB_EVALUATOR_INJECT="bounds", DECIDB_VERIFY_ALLOW="T1:bounds")
    assert result.returncode == 0 and "differ" not in result.stderr, result.stderr


def test_an_allowance_covers_its_group_alone(decidb_cli):
    sql = GROUPS["mappings"]
    result = _verify_raising(decidb_cli, sql, DECIDB_EVALUATOR_INJECT="mappings", DECIDB_VERIFY_ALLOW="T1:terms")
    assert "DECIDE evaluators differ in mappings (" in result.stderr, result.stderr


@pytest.mark.parametrize("allowed,raised", [("clause 0", False), ("clause 1", True)])
def test_an_allowance_covers_the_fields_its_prefix_names(decidb_cli, allowed, raised):
    """The injected difference is clause 0's bound: allowing clause 1's leaves it raised."""
    result = _verify_raising(
        decidb_cli, TWO_BOUNDS, DECIDB_EVALUATOR_INJECT="bounds", DECIDB_VERIFY_ALLOW=f"T1:bounds:{allowed}"
    )
    assert ("DECIDE evaluators differ in bounds (clause 0)" in result.stderr) == raised, result.stderr


#: Eleven clauses, of which the last, clause 10, alone has group names.
ELEVEN_CLAUSES = f"""SELECT id, x, y FROM {ROWS} DECIDE x(REAL), y(REAL)
    SUCH THAT {" AND ".join(f"x + y <= t.c + {k}" for k in range(10))} AND sum(x) by (t.g) <= 50 MAXIMIZE sum(x + y)"""


@pytest.mark.parametrize(
    "group,allowed,raised",
    [("names", "clause 1", True), ("terms", "clause 0", False)],
    ids=["clause-1-is-not-clause-10", "clause-0-covers-clause-0-term-0"],
)
def test_an_allowance_prefix_covers_whole_words(decidb_cli, group, allowed, raised):
    """A field prefix ends at a word: `clause 1` leaves clause 10's names raised, and `clause 0` covers the field
    `clause 0 term 0`, each the group's first field, which the injection changes."""
    result = _verify_raising(
        decidb_cli, ELEVEN_CLAUSES, DECIDB_EVALUATOR_INJECT=group, DECIDB_VERIFY_ALLOW=f"T1:{group}:{allowed}"
    )
    assert ("DECIDE evaluators differ in " in result.stderr) == raised, result.stderr


def test_a_group_that_records_more_shifts_no_other(decidb_cli, tmp_path):
    """Groups compare apart: a field more in `terms`, which is allowed, leaves `mappings` compared field by field."""
    log = tmp_path / "allowed.log"
    result = _verify_raising(
        decidb_cli,
        GROUPS["mappings"],
        DECIDB_EVALUATOR_INJECT="terms:extra,mappings",
        DECIDB_VERIFY_ALLOW="T1:terms",
        DECIDB_VERIFY_ALLOW_LOG=str(log),
    )
    assert "DECIDE evaluators differ in mappings (" in result.stderr, result.stderr
    assert log.read_text().split() == ["T1"]


def test_a_group_that_records_more_is_named(decidb_cli):
    result = _verify_raising(decidb_cli, PLAIN, DECIDB_EVALUATOR_INJECT="terms:extra")
    assert "DECIDE evaluators differ in how much they record in terms" in result.stderr, result.stderr


def test_a_malformed_allowance_is_named(decidb_cli):
    result = _verify(decidb_cli, PLAIN, DECIDB_VERIFY_ALLOW="bounds")
    assert "DECIDB_VERIFY_ALLOW=bounds: expected bug:group[:field prefix] entries" in result.stderr, result.stderr


def test_the_marker_allows_its_groups_and_finds_a_stale_one():
    """The verify_known_difference marker (conftest.py): its setting, and the bugs that no longer differ."""
    marks = [("T1", ("bounds", "terms:clause 0")), ("T2", ("mappings",))]
    assert allow_setting(marks) == "T1:bounds,T1:terms:clause 0,T2:mappings"
    assert stale_markers(marks, ["T1"]) == {"T2"}
    assert stale_markers(marks, ["T2", "T1"]) == set()


@pytest.mark.verify_known_difference("T0", ["bounds"])
def test_a_marked_difference_passes_under_verify(decidb_cli):
    """Under DECIDB_EVALUATOR=verify the marker allows the bounds this query's injection changes, and expects them to
    differ; under any other evaluator nothing is compared."""
    result = decidb_cli.with_env({"DECIDB_EVALUATOR_INJECT": "bounds"}).execute_raw(
        f"SET threads=1; SET decide_direct_solve='off'; {PLAIN}"
    )
    assert result.returncode == 0, result.stderr


def test_a_group_only_the_second_run_records_is_a_difference(decidb_cli):
    result = _verify_raising(decidb_cli, PLAIN, DECIDB_EVALUATOR_INJECT="unheard:extra")
    assert "DECIDE evaluators differ in how much they record in unheard" in result.stderr, result.stderr


def test_a_frame_read_records_the_row_it_selects(decidb_cli):
    """Stage 06 reads a frame's decision at the row the frame selects: row 0 has no previous row."""
    result = _verify_raising(decidb_cli, GROUPS["frames"], DECIDB_EVALUATOR_INJECT="frames")
    assert "differ in frames (0 through frame 0) at 0: flat has -1.000000, the second run 0.000000" in result.stderr
