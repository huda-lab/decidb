"""The suite log's compare (``suite_log.py``): one minimal pair of real records per verdict branch.

Every record below was captured from the suite at ed249504d4, and every pair named "real" is two captures of the same
binary, so the compare must call it the same or unproven. Other pairs change one thing in a real record (one printed
with its columns swapped first), and each test fails when the rule it names is removed. A long box is cut to the lines
around the one that moved.
"""

from __future__ import annotations

import json
import re
from pathlib import Path
from xml.sax.saxutils import escape

import pytest

from suite_log import main

#: The caret under ``LINE 4:`` is placed by spaces.
CARET = {
    "mode": "auto",
    "test": "tests/test_error_binder.py::TestBinderErrors::test_power_wrapping_bilinear_rejected (call)",
    "env": {},
    "sql": "\n                SELECT l_quantity FROM lineitem\n                DECIDE x(REAL), y(REAL)\n"
    "                SUCH THAT x <= 5 AND y <= 5\n                MINIMIZE SUM(POWER(x * y, 2)) LIMIT 1\n            ",
    "returncode": 1,
    "stdout": "",
    "stderr": "Binder Error: Triple or higher-order products of DECIDE variables are not supported (total degree > 2). "
    "This term reaches degree 4. A term may use one decision, one decision squared (POWER(x, 2)), or two different "
    "decisions multiplied (x * y).\n\nLINE 4:                 MINIMIZE SUM(POWER(x * y, 2)) LIMIT 1\n"
    "                                 ^\n",
}

#: x = (2, 2) is the optimum, 6: x <> 3 leaves 0..2 to each, and their sum, 4, is not 6. The test checks only that
#: there are two rows, no 3 and a sum other than 6, so x = (2, 1), objective 4, passes it too.
NOT_EQUAL = {
    "mode": "auto",
    "test": "tests/test_error_binder.py::TestBinderErrors::test_not_equal_still_accepted (call)",
    "env": {"DECIDB_STATUS_MARKERS": "1"},
    "sql": "\n                FROM (VALUES (1, 1.0), (2, 2.0)) t(id, s)\n                DECIDE x(INT)\n"
    "                SUCH THAT x BETWEEN 0 AND 3 AND SUM(x) <> 6 AND x <> 3\n"
    "                MAXIMIZE SUM(s * x)\n            ",
    "returncode": 0,
    "stdout": '[{"id":1,"s":1.0,"x":2},\n{"id":2,"s":2.0,"x":2}]\n',
    "stderr": "",
}

#: Ends in ORDER BY id.
ORDERED = {
    "mode": "auto",
    "test": "tests/test_anr_composition.py::TestMembersDropped::test_a_member_is_dropped_where_its_frame_selects_"
    "nothing[deck-example-3-decidb_cli_gurobi] (call)",
    "env": {"DECIDB_FORCE_SOLVER": "gurobi", "DECIDB_STATUS_MARKERS": "1"},
    "sql": "SELECT id, x FROM (VALUES (1, 'A', 1, 4, 2), (2, 'A', 2, 6, 1), (3, 'A', 3, 1, 3), (4, 'B', 1, 7, 2), "
    "(5, 'B', 2, 3, 1), (6, 'C', 1, 5, 2)) t(id, product, period, v, w) DECIDE x(INT)\n        SUCH THAT x <= 9 AND "
    "per t.product: sum(at(previous: x) over (t.period within t.product)) by (t.product) <= 4 MAXIMIZE sum(x) "
    "ORDER BY id",
    "returncode": 0,
    "stdout": '[{"id":1,"x":4},\n{"id":2,"x":0},\n{"id":3,"x":9},\n{"id":4,"x":4},\n{"id":5,"x":9},\n{"id":6,"x":9}]\n',
    "stderr": "",
}

#: Its only ORDER BY orders a window inside a subquery, not the rows printed.
WINDOW = {
    "mode": "auto",
    "test": "tests/test_anr_frames.py::TestSyntax::test_a_window_in_a_subquery_of_the_clause_still_answers (call)",
    "env": {"DECIDB_STATUS_MARKERS": "1"},
    "sql": "SELECT id, x FROM (VALUES (1, 'A', 1, 5, 2.5), (2, 'A', 2, 6, 1.5), (3, 'B', 1, 7, 0.5)) "
    "t(id, product, period, demand, price) DECIDE x(REAL), per (): s(REAL) SUCH THAT x <= 9 AND x <= (SELECT max(r) "
    "FROM (SELECT row_number() OVER (ORDER BY v.id) AS r FROM (VALUES (1), (2)) v(id))) MAXIMIZE sum(x)",
    "returncode": 0,
    "stdout": '[{"id":1,"x":2.0},\n{"id":2,"x":2.0},\n{"id":3,"x":2.0}]\n',
    "stderr": "",
}

REPAIR = {
    "mode": "auto",
    "test": "tests/test_query_diagnostics_relation.py::TestPrintedRepairIsTrue::test_the_printed_repair_restores_"
    "feasibility[bound_longer_than_amount-decidb_cli_gurobi] (call)",
    "env": {"DECIDB_FORCE_SOLVER": "gurobi", "DECIDB_STATUS_MARKERS": "1"},
    "sql": "SELECT id, x FROM (VALUES (1), (2), (3)) t(id) DECIDE x(REAL) SUCH THAT x <= 100000.333333 AND "
    "SUM(x) >= 300001 MINIMIZE SUM(x)",
    "returncode": 0,
    "stdout": '[{"id":1,"x":100000.33333399998},\n{"id":2,"x":100000.333333},\n{"id":3,"x":100000.333333}]\n',
    "stderr": "",
}

#: Cut to the plan's first two operators.
PLAN = {
    "mode": "auto",
    "test": "tests/test_direct_user_facing.py::test_direct_solve_is_on_by_default (call)",
    "env": {"DECIDB_TEST_DIRECT_SOLVE": ""},
    "sql": "EXPLAIN \n        SELECT id, x FROM (\n"
    "            FROM (VALUES (1, 2.0, 3), (2, 9.0, 2)) t(id, score, weight)\n"
    "            DECIDE x(BOOL) SUCH THAT SUM(weight * x) <= 3\n            MAXIMIZE SUM(score * x)\n        ) q\n    ",
    "returncode": 0,
    "stdout": "┌───────────────────────────┐\n│         PROJECTION        │\n│    ────────────────────   │\n"
    "│             id            │\n│             x             │\n└─────────────┬─────────────┘\n"
    "┌─────────────┴─────────────┐\n│           DECIDE          │\n│    ────────────────────   │\n"
    "│        Variables: x       │\n└─────────────┬─────────────┘\n",
    "stderr": "",
}

#: A real pair, cut to the query and its total time.
ANALYZE_SQL = (
    "SET decide_direct_solve='off'; EXPLAIN ANALYZE SELECT id, x FROM (VALUES (1, 'A', 1, 4, 2), (2, 'A', 2, 6, 1), "
    "(3, 'A', 3, 1, 3), (4, 'B', 1, 7, 2), (5, 'B', 2, 3, 1), (6, 'C', 1, 5, 2)) t(id, product, period, v, w) DECIDE "
    "x(INT) SUCH THAT x <= 9 AND when t.v > 100: x <= 3 MAXIMIZE sum(x)"
)
ANALYZE = {
    "mode": "auto",
    "test": "tests/test_anr_reports.py::test_a_when_that_keeps_no_row_is_no_warning[per-row] (call)",
    "env": {},
    "sql": ANALYZE_SQL,
    "returncode": 0,
    "stdout": f"{ANALYZE_SQL.split('; ', 1)[1]}\n┌────────────────────────────────────────────────┐\n"
    "│┌──────────────────────────────────────────────┐│\n││              Total Time: 0.0063s             ││\n"
    "│└──────────────────────────────────────────────┘│\n└────────────────────────────────────────────────┘\n",
    "stderr": "",
}

#: A real pair, cut to the first two events.
PROFILE = {
    "mode": "auto",
    "test": "tests/test_scope_reducer_by.py::test_a_key_shared_by_a_decision_and_a_by_is_computed_once (call)",
    "env": {"DECIDB_PROFILE": "1"},
    "sql": "SET decide_direct_solve='off';\n        SELECT id, x FROM (VALUES (1, 'a'), (2, 'a'), (3, 'b')) t(id, g)\n"
    "        DECIDE per g: x(INT)\n        SUCH THAT x <= 9 AND sum(x) by (g) <= 12\n        MAXIMIZE SUM(x)",
    "returncode": 0,
    "stdout": "┌───────┬───────┐\n│  id   │   x   │\n│ int32 │ int64 │\n├───────┼───────┤\n│     1 │     6 │\n"
    "│     2 │     6 │\n│     3 │     9 │\n└───────┴───────┘\n",
    "stderr": 'DECIDB_PROFILE: {"event":"begin","id":1,"parent":0,"name":"frontend.parse","start_ns":1751663126142291,'
    '"rss_bytes":10534912}\nDECIDB_PROFILE: {"event":"end","id":1,"parent":0,"name":"frontend.parse",'
    '"end_ns":1751663126198166,"duration_ms":0.055875,"rss_bytes":10878976,"peak_rss_bytes":10878976,'
    '"unwinding":false}\n',
}
PROFILE_AGAIN = {
    **PROFILE,
    "stderr": 'DECIDB_PROFILE: {"event":"begin","id":1,"parent":0,"name":"frontend.parse","start_ns":1751666051540875,'
    '"rss_bytes":10321920}\nDECIDB_PROFILE: {"event":"end","id":1,"parent":0,"name":"frontend.parse",'
    '"end_ns":1751666051585416,"duration_ms":0.044541,"rss_bytes":10665984,"peak_rss_bytes":10665984,'
    '"unwinding":false}\n',
}

#: A real pair: the model dump goes to a folder pytest numbers by session.
DUMP = {
    "mode": "auto",
    "test": "tests/test_anr_composition.py::TestMembersDropped::test_a_dropped_member_costs_nothing (call)",
    "env": {
        "DECIDB_TEST_DIRECT_SOLVE": "off",
        "DECIDB_DUMP_MODEL": "/private/var/folders/s8/8n0cr0654s53nm_hr_8m1m500000gn/T/pytest-of-samiakouk/pytest-1115/"
        "popen-gw0/test_a_dropped_member_costs_no0/model.dump",
    },
    "sql": "SET threads=1; SELECT id, x FROM (VALUES (1, 'A', 1, 4, 2), (2, 'A', 2, 6, 1), (3, 'A', 3, 1, 3), "
    "(4, 'B', 1, 7, 2), (5, 'B', 2, 3, 1)) t(id, product, period, v, w) DECIDE x(INT)\n        SUCH THAT x <= 9 AND "
    "per t.product: sum(at(previous: x) over (t.period within t.product)) by (t.product) <= 4 MAXIMIZE sum(x) "
    "ORDER BY id",
    "returncode": 0,
    "stdout": "┌───────┬───────┐\n│  id   │   x   │\n│ int32 │ int64 │\n├───────┼───────┤\n│     1 │     4 │\n"
    "│     2 │     0 │\n│     3 │     9 │\n│     4 │     4 │\n│     5 │     9 │\n└───────┴───────┘\n",
    "stderr": "",
}

#: A real pair: the profile goes to a folder pytest numbers by session and by worker. Its box is cut to two rows.
PROFILED_SQL = (
    "SET decide_direct_solve='require'; PRAGMA enable_profiling='json'; PRAGMA profiling_output='/private/var/folders/"
    "s8/8n0cr0654s53nm_hr_8m1m500000gn/T/pytest-of-samiakouk/pytest-{}/test_a1_each_verdict_gates_one0/profile.json'; "
    "SELECT i, x FROM (SELECT i, (i % 3 + 1)::DOUBLE AS q, 0.0 AS lo, i % 2 AS g FROM range(5000) r(i)) DECIDE x(REAL) "
    "SUCH THAT x >= lo AND x <= 9 MAXIMIZE SUM(x) "
)
PROFILED = {
    "mode": "auto",
    "test": "tests/test_direct_a1_separable_box.py::test_a1_each_verdict_gates_one_read_and_the_rows_wait_for_the_"
    "newest (call)",
    "env": {},
    "sql": PROFILED_SQL.format("1115/popen-gw5"),
    "returncode": 0,
    "stdout": "┌───────┬────────┐\n│   i   │   x    │\n│ int64 │ double │\n├───────┼────────┤\n│     0 │    9.0 │\n"
    "│  4999 │    9.0 │\n├───────┴────────┤\n│   5000 rows    │\n│   (40 shown)   │\n└────────────────┘\n",
    "stderr": "",
}

TIME_LIMIT_ERROR = (
    "Invalid Input Error: DECIDE optimization hit the time limit. Simplify the constraints or reduce the input size.\n"
)

#: A real pair: on a 1 ms limit the solver has found a solution on some runs and none on others.
STOP_FOUND = {
    "mode": "auto",
    "test": "tests/test_query_diagnostics_slow.py::TestNonMipTimeoutHonesty::test_lp_timeout_no_fabricated_bound"
    "[decidb_cli_highs] (call)",
    "env": {"DECIDB_FORCE_SOLVER": "highs", "DECIDB_TIME_LIMIT": "0.001"},
    "sql": "SELECT id, x FROM (SELECT id, ((id%97)+1) AS w FROM range(1,200001) t(id)) DECIDE x(REAL) SUCH THAT x >= 0 "
    "AND x <= 1000 AND SUM(w*x) <= 100000000 MAXIMIZE SUM(x)",
    "returncode": 1,
    "stdout": "",
    "stderr": "DECIDE hit the 0.001s time limit with a usable solution (not proven best).\n  best objective so far: 0\n"
    f"  elapsed 0.16s · peak memory 157 MB\n{TIME_LIMIT_ERROR}",
}
STOP_NOT_FOUND = {
    **STOP_FOUND,
    "stderr": "DECIDE hit the 0.001s time limit without finding a solution yet.\n"
    f"  elapsed 0.027s · peak memory 63.2 MB\n{TIME_LIMIT_ERROR}",
}

#: A real pair, the box cut to the rows around row 17, whose x the stop caught at 0 in one run and at 1 in the other.
INCUMBENT_BOX = (
    "┌───────┬───────┐\n│  id   │   x   │\n│ int64 │ int32 │\n├───────┼───────┤\n│    16 │     0 │\n"
    "│    17 │     {} │\n│    18 │     1 │\n├───────┴───────┤\n│   400 rows    │\n│  (40 shown)   │\n"
    "└───────────────┘\n"
)
INCUMBENT = {
    "mode": "auto",
    "test": "tests/test_query_diagnostics_slow.py::TestSlowContinuation::test_interrupt_delivers_the_incumbent_"
    "without_a_terminal[decidb_cli_gurobi] (call)",
    "env": {"DECIDB_FORCE_SOLVER": "gurobi", "DECIDB_TIME_LIMIT": "3"},
    "sql": (
        "SELECT id, x FROM (SELECT id, ((id*7)%97)+1 AS c0, ((id*9)%97)+1 AS c1, ((id*11)%97)+1 AS c2, "
        "((id*13)%97)+1 AS c3, ((id*15)%97)+1 AS c4, ((id*17)%97)+1 AS c5, ((id*19)%97)+1 AS c6, ((id*21)%97)+1 AS "
        "c7, ((id*23)%97)+1 AS c8, ((id*25)%97)+1 AS c9, ((id*27)%97)+1 AS c10, ((id*29)%97)+1 AS c11, "
        "((id*31)%97)+1 AS c12, ((id*33)%97)+1 AS c13, ((id*35)%97)+1 AS c14, ((id*13)%89)+1 AS p FROM range(1,401) "
        "t(id)) DECIDE x(BOOL) SUCH THAT SUM(c0*x) <= 9785 AND SUM(c1*x) <= 9766 AND SUM(c2*x) <= 9747 AND SUM(c3*x) "
        "<= 9776 AND SUM(c4*x) <= 9806 AND SUM(c5*x) <= 9787 AND SUM(c6*x) <= 9816 AND SUM(c7*x) <= 9797 AND "
        "SUM(c8*x) <= 9827 AND SUM(c9*x) <= 9759 AND SUM(c10*x) <= 9789 AND SUM(c11*x) <= 9818 AND SUM(c12*x) <= "
        "9848 AND SUM(c13*x) <= 9732 AND SUM(c14*x) <= 9810 MAXIMIZE SUM(p*x)"
    ),
    "returncode": 0,
    "stdout": INCUMBENT_BOX.format(0),
    "stderr": "DECIDE stopped at your request with a usable solution (not proven best).\n"
    "  best objective so far: 13484  (within 0.15% of the best possible)\n  elapsed 0.99s · peak memory 65.3 MB\n"
    "DECIDE is returning the best solution found so far — it is NOT proven the best possible.\n",
}
INCUMBENT_AGAIN = {
    **INCUMBENT,
    "stdout": INCUMBENT_BOX.format(1),
    "stderr": INCUMBENT["stderr"]
    .replace("13484  (within 0.15%", "13479  (within 0.24%")
    .replace("0.99s · peak memory 65.3", "0.93s · peak memory 58.8"),
}

_SUMMARY = re.compile(
    r"^auto: \d+ queries: (\d+) same, (\d+) unproven, (\d+) differ, (\d+) only in old, (\d+) only in new$", re.MULTILINE
)


def _testcase(nodeid: str, failed: bool) -> str:
    """A ``<testcase>`` as pytest's junit file spells one: ``tests/a.py::C::t[p]`` is class ``tests.a.C``, name
    ``t[p]``."""
    address, bracket, parameters = nodeid.rsplit(" (", 1)[0].partition("[")
    path, *scopes, name = address.split("::")
    classname = ".".join([path.removesuffix(".py").replace("/", "."), *scopes])
    failure = '<failure message="AssertionError">AssertionError</failure>' if failed else ""
    name = escape(name + bracket + parameters)
    return f'<testcase classname="{classname}" name="{name}" time="0.1">{failure}</testcase>'


def _log(folder: Path, records: list[dict], failed: set[str], modes: tuple[str, ...]) -> Path:
    """A log as ``capture_suite_log.sh`` writes one: per mode, a worker's records and the run's junit file, in which
    every test that ran a record passed but those ``failed`` names."""
    for mode in modes:
        (folder / mode).mkdir(parents=True)
        (folder / mode / "gw0.jsonl").write_text("".join(json.dumps(record) + "\n" for record in records))
        cases = "".join(_testcase(test, test in failed) for test in sorted({record["test"] for record in records}))
        (folder / mode / "junit.xml").write_text(
            f'<?xml version="1.0" encoding="utf-8"?><testsuites><testsuite>{cases}</testsuite></testsuites>'
        )
    return folder


@pytest.fixture
def compare(tmp_path, capsys):
    """Compare a log of ``old`` records with one of ``new``: the exit code, the verdicts the summary counts, and the
    printed text."""

    def run(old, new, *options, failed=(), old_modes=("auto",)):
        old_log = _log(tmp_path / "old", old, set(), old_modes)
        new_log = _log(tmp_path / "new", new, set(failed), ("auto",))
        code = main([str(old_log), str(new_log), *options])
        out = capsys.readouterr().out
        counts = _SUMMARY.search(out).groups()
        names = ("same", "unproven", "differ", "only in old", "only in new")
        return code, [name for name, count in zip(names, counts) if count != "0"], out

    return run


def test_a_number_past_the_sixth_decimal_is_the_same(compare):
    """stdout goes through golden/normalize_results.py: both values round to 100000.333334."""
    moved = {**REPAIR, "stdout": REPAIR["stdout"].replace("100000.33333399998", "100000.33333400001")}
    assert compare([REPAIR], [moved])[:2] == (0, ["same"])


def test_a_measured_time_is_the_same(compare):
    """A real pair whose total times differ."""
    again = {**ANALYZE, "stdout": ANALYZE["stdout"].replace("0.0063s", "0.0025s")}
    assert compare([ANALYZE], [again])[:2] == (0, ["same"])


def test_instrumentation_is_dropped(compare):
    """A real pair whose DECIDB_PROFILE spans differ in clock and memory."""
    assert compare([PROFILE], [PROFILE_AGAIN])[:2] == (0, ["same"])


def test_a_temporary_folder_in_the_environment_keeps_only_its_file_name(compare):
    """A real pair: without the rule, the query's environment differs, and it is in one log alone in each."""
    again = {
        **DUMP,
        "env": {**DUMP["env"], "DECIDB_DUMP_MODEL": DUMP["env"]["DECIDB_DUMP_MODEL"].replace("1115", "1117")},
    }
    assert compare([DUMP], [again])[:2] == (0, ["same"])


def test_a_temporary_folder_in_the_sql_keeps_only_its_file_name(compare):
    """A real pair: without the rule, the query's SQL differs, and it is in one log alone in each."""
    again = {**PROFILED, "sql": PROFILED_SQL.format("1117/popen-gw0")}
    assert compare([PROFILED], [again])[:2] == (0, ["same"])


def test_rows_are_sorted_when_only_a_subquery_orders(compare):
    """The ORDER BY in parentheses orders a window; the rows printed come in the threads' order."""
    shuffled = {**WINDOW, "stdout": '[{"id":3,"x":2.0},\n{"id":1,"x":2.0},\n{"id":2,"x":2.0}]\n'}
    assert compare([WINDOW], [shuffled])[:2] == (0, ["same"])


def test_rows_under_a_top_level_order_by_keep_their_order(compare):
    rows = ORDERED["stdout"].strip("[]\n").split(",\n")
    reversed_rows = {**ORDERED, "stdout": "[" + ",\n".join(reversed(rows)) + "]\n"}
    assert compare([ORDERED], [reversed_rows])[:2] == (1, ["differ"])


def test_a_plan_keeps_its_line_order(compare):
    """DECIDE above the projection is another plan."""
    lines = PLAN["stdout"].splitlines(keepends=True)
    lines[1], lines[7] = lines[7], lines[1]
    assert compare([PLAN], [{**PLAN, "stdout": "".join(lines)}])[:2] == (1, ["differ"])


def test_a_moved_caret_differs(compare):
    """stderr's spaces are not collapsed: the caret now points five columns to the right."""
    moved = {**CARET, "stderr": CARET["stderr"].replace(" ^\n", "      ^\n")}
    assert compare([CARET], [moved])[:2] == (1, ["differ"])


def test_a_changed_value_differs_though_its_test_passes(compare):
    """No stop came, so x's values are the answer. Rows 3 and 5 are no row's previous period, so x <= 9 alone bounds
    them: x = 8 on row 5 is worse than the optimum, and the log cannot tell it from an alternate optimum."""
    worse = {**DUMP, "stdout": DUMP["stdout"].replace("│     5 │     9 │", "│     5 │     8 │")}
    assert compare([DUMP], [worse])[:2] == (1, ["differ"])


def test_a_stop_report_that_follows_the_clock_is_unproven(compare):
    """A real pair: the rest of a stop's report is waived, and the query is listed."""
    code, verdicts, out = compare([STOP_FOUND], [STOP_NOT_FOUND])
    assert (code, verdicts) == (0, ["unproven"])
    assert f"[auto] unproven: {STOP_FOUND['test'].rsplit(' (', 1)[0]}\n" in out


def test_the_rows_a_stop_caught_are_unproven(compare):
    """A real pair: the values of x in the rows a stopped solve returns are the solver's when the stop came."""
    assert compare([INCUMBENT], [INCUMBENT_AGAIN])[:2] == (0, ["unproven"])


def test_a_stop_that_changes_a_key_differs(compare):
    """Only x's values are waived, and the header names x's column: key 17 is now a second 16."""
    duplicated = {**INCUMBENT_AGAIN, "stdout": INCUMBENT_AGAIN["stdout"].replace("│    17 │", "│    16 │")}
    assert compare([INCUMBENT], [duplicated])[:2] == (1, ["differ"])


def test_a_stop_that_changes_a_decisions_type_differs(compare):
    """The line between the header and the rows holds the types: x is now int64."""
    retyped = {**INCUMBENT_AGAIN, "stdout": INCUMBENT_AGAIN["stdout"].replace("│ int32 │", "│ int64 │")}
    assert compare([INCUMBENT], [retyped])[:2] == (1, ["differ"])


def test_a_stop_that_drops_a_row_differs_though_its_decision_prints_first(compare):
    """The real record printed with x first (SELECT x, id): below the rows, the row count stands in x's column."""
    swapped = {
        **INCUMBENT,
        "sql": INCUMBENT["sql"].replace("SELECT id, x", "SELECT x, id", 1),
        "stdout": re.sub(r"^│(.*?│)(.*?│)$", r"│\2\1", INCUMBENT["stdout"], flags=re.MULTILINE),
    }
    dropped = {**swapped, "stdout": swapped["stdout"].replace("400 rows", "399 rows")}
    assert compare([swapped], [dropped])[:2] == (1, ["differ"])


def test_a_stop_whose_test_failed_differs(compare):
    """Only a test that passed in both logs has checked what the stop printed."""
    assert compare([STOP_FOUND], [STOP_NOT_FOUND], failed={STOP_FOUND["test"]})[:2] == (1, ["differ"])


def test_a_stop_by_another_cause_differs(compare):
    """What stopped the solve is kept of its report: the time limit's report now says the user stopped it."""
    stopped = {
        **STOP_FOUND,
        "stderr": STOP_FOUND["stderr"].replace("hit the 0.001s time limit", "stopped at your request"),
    }
    assert compare([STOP_FOUND], [stopped])[:2] == (1, ["differ"])


def test_a_stop_followed_by_another_error_differs(compare):
    stopped = {
        **STOP_FOUND,
        "stderr": STOP_FOUND["stderr"].replace(TIME_LIMIT_ERROR, 'Binder Error: column "c0" is ambiguous\n'),
    }
    assert compare([STOP_FOUND], [stopped])[:2] == (1, ["differ"])


def test_a_stop_that_drops_its_rows_differs(compare):
    assert compare([INCUMBENT], [{**INCUMBENT, "stdout": ""}])[:2] == (1, ["differ"])


def test_a_stop_whose_exit_code_changes_differs(compare):
    assert compare([INCUMBENT], [{**INCUMBENT, "returncode": 1}])[:2] == (1, ["differ"])


def test_a_query_in_one_log_alone_differs(compare):
    code, verdicts, out = compare([NOT_EQUAL], [CARET])
    assert (code, verdicts) == (1, ["only in old", "only in new"])
    assert "[auto] only in old: tests/test_error_binder.py::TestBinderErrors::test_not_equal_still_accepted" in out


def test_the_limit_counts_what_it_does_not_print(compare):
    moved = {**CARET, "stderr": CARET["stderr"].replace(" ^\n", "      ^\n")}
    worse = {**NOT_EQUAL, "stdout": NOT_EQUAL["stdout"].replace('"s":2.0,"x":2', '"s":2.0,"x":1')}
    code, verdicts, out = compare([CARET, NOT_EQUAL], [moved, worse], "--limit", "1")
    assert (code, verdicts) == (1, ["differ"])
    assert out.count("[auto] differ: ") == 1 and "[auto] 1 more not shown (--limit)" in out


def test_a_mode_in_one_log_alone_differs(compare):
    """A log that captured fewer modes has not shown the others still print what they did."""
    code, verdicts, out = compare([NOT_EQUAL], [NOT_EQUAL], old_modes=("auto", "highs"))
    assert (code, verdicts) == (1, ["same"])
    assert "[highs] only in old: the whole mode" in out and out.endswith("1 differences\n")


def test_a_folder_without_a_mode_is_no_log(tmp_path):
    """Two such folders would otherwise compare as no differences."""
    old = _log(tmp_path / "old", [NOT_EQUAL], set(), ("auto",))
    (tmp_path / "new").mkdir()
    assert main([str(old), str(tmp_path / "new")]) == 2
