#!/usr/bin/env python3
"""Compare two suite logs of ``capture_suite_log.sh``, query by query.

A log holds what the CLI printed for every query of the suite (``_log_query`` in ``decidb_cli.py`` writes it). A later
change to the evaluator is measured against a log taken before it: each query must print what it printed then, except
for what the suite already lets a run vary:

  numbers    stdout goes through the golden check's own filter, ``golden/normalize_results.py``, run as it is
             (fractions to 6 places, box padding collapsed). Stderr does not: its spaces place an error's caret
  row order  rows are sorted, as ``compare_solutions`` sorts, unless the query is an EXPLAIN or holds an ORDER BY
             outside every parenthesis
  timings    a time or size with its unit (Total Time, elapsed, peak memory); DECIDB_BENCH and DECIDB_PROFILE lines go
             whole, being instrumentation a test switched on rather than output of the query
  paths      a path under a temporary directory keeps its file name alone, in the SQL and the environment
  unproven   every run in both logs stopped its solve before proving an answer. What the solver held when the stop came
             is the clock's: the stop's report, but for what stopped it, and the values of the decisions in its rows
             are waived, provided every test that ran the query passed in both logs and the rest of the output matches.
             The row count, the keys, the types and every other column stay: none of them follows the clock

Any other change is a difference, a changed value included. No output states the objective, so a log cannot tell an
alternate optimum from a wrong answer, and a test that passes proves neither: ``test_not_equal_still_accepted`` passes
on an assignment worse than the optimum. A person reads each one.

Some queries vary between two runs of one binary: a query with several optimal answers can get another one when its
rows arrive in another order across threads. ``--again`` names a second log of OLD's commit, and a query NEW prints as
one of the two baseline runs printed it counts as seen, not as a difference. An output neither printed is still one,
so a tie stays allowed while a wrong answer on the same query does not.

    test/decide/suite_log.py OLD NEW [--again OLD_AGAIN] [--limit N]

Exit code 0 with no difference, 1 with some (a query or a mode only one log holds is one), 2 when OLD or NEW holds no
mode.
"""

from __future__ import annotations

import argparse
import difflib
import json
import re
import subprocess
import sys
import xml.etree.ElementTree as ElementTree
from pathlib import Path
from typing import NamedTuple

#: Run as the filter it is, so the golden check and this one cannot drift apart.
_GOLDEN_NORMALIZER = Path(__file__).resolve().parent / "golden" / "normalize_results.py"

#: Ends each stdout sent through the golden filter: a line of its own, which the filter leaves as it is.
_PART_END = "\n\x1e\n"

#: A file under the system's temporary directories: only its own name says anything, since pytest numbers the folder by
#: session and by what the worker ran before. A test names one in the SQL (``PRAGMA profiling_output``) or the
#: environment (``DECIDB_DUMP_MODEL``); no query has printed one.
_TEMP_PATH = re.compile(r"(?:/private)?/(?:var/folders|tmp)/[^\s'\"]*/")

#: A measurement of this run, as ``FormatDuration``, ``FormatMemory`` and DuckDB's ``Total Time`` print one.
_MEASURE = re.compile(r"(?<![\w.])\d+(?:\.\d+)?(?:s\b| [KMG]B\b)")

#: A line of the instrumentation a test switched on: spans, timings and the solver's own counters.
_INSTRUMENTATION = re.compile(r"^DECIDB_(?:BENCH|PROFILE): .*\n?", re.MULTILINE)

#: The report of a solve stopped before it proved an answer: what stopped it, the clock or the user, then the rest of
#: its opening line and the indented lines under it. That rest follows when the stop came: on a 1 ms limit
#: ``test_lp_timeout_no_fabricated_bound`` has found a solution on some runs and none on others.
_STOP_REPORT = re.compile(r"^(DECIDE (?:hit the|stopped at your request))\b.*(?:\n  .*)*", re.MULTILINE)

#: A plan: its line order is its structure.
_PLAN = re.compile(r"\bEXPLAIN\b", re.IGNORECASE)

_ORDER_BY = re.compile(r"\bORDER\s+BY\b", re.IGNORECASE)

_INNERMOST_PARENTHESES = re.compile(r"\([^()]*\)")

#: A declarator, ``x(INT)``: a name before one word in parentheses, the column its values print in. A key before it,
#: ``per K:``, is none. The shape is also a call or a table's column list (``t(id)``), which in no logged query names
#: a column it prints.
_DECLARATOR = re.compile(r"(\w+)\s*\(\s*\w+\s*\)")


class _Printed(NamedTuple):
    """What one run of a query printed, with what a run may vary settled."""

    returncode: int
    stderr: str
    stdout: tuple[str, ...]


class _LoggedQuery(NamedTuple):
    """Every run of one query in one log. A query is its SQL and the environment the wrapper gave it, never the test
    that ran it: a session fixture's query runs under whichever test a worker reached first, and how many times it ran
    follows the schedule too. So its runs are compared as a set, and so are what they promise (``_promised``)."""

    sql: str
    tests: set[str]
    printed: set[_Printed]
    promised: set[_Printed]


def _settled(text: str) -> str:
    """``text`` without what belongs to this run and not to the query."""
    return _MEASURE.sub("<measure>", _INSTRUMENTATION.sub("", text))


def _golden_normalized(stdouts: list[str]) -> list[str]:
    """``stdouts`` after the golden check's filter, in one run of it."""
    filtered = subprocess.run(
        [sys.executable, "-I", "-X", "utf8", str(_GOLDEN_NORMALIZER)],
        input=_PART_END.join(stdouts),
        capture_output=True,
        encoding="utf-8",
        check=True,
    )
    return filtered.stdout.split(_PART_END)


def _ordered(sql: str) -> bool:
    """Does ``sql`` order its rows: an ORDER BY outside every parenthesis? One inside belongs to a subquery, a window or
    an aggregate, and orders nothing that prints."""
    text, removed = sql, 1
    while removed:
        text, removed = _INNERMOST_PARENTHESES.subn(" ", text)
    return bool(_ORDER_BY.search(text))


def _rows(stdout: str, sql: str) -> tuple[str, ...]:
    """The lines of ``stdout``, in the order printed where the query states one, sorted elsewhere: without it the order
    follows the threads. The strip takes off what JSON mode writes around a row, which moves with the row."""
    lines = stdout.splitlines()
    if _PLAN.search(sql) or _ordered(sql):
        return tuple(lines)
    return tuple(sorted(line.strip("[],") for line in lines))


def _decisions_masked(stdout: str, sql: str) -> str:
    """``stdout`` with each value of a decision in a box replaced by ``#``. A box names its columns in the line under
    its top border and holds its rows between the next two rules, so its types and its row count stay.

    A stop hands its rows back only through ``execute_interrupted`` and ``execute_interactive``, which print the CLI's
    box: rows printed in another form, or a column computed from a decision, keep their values, and so differ."""
    decisions = set(_DECLARATOR.findall(sql))
    lines, columns, part = [], set(), 0
    for line in stdout.split("\n"):
        cells = line.split("│")
        if line.startswith("┌"):
            columns, part = None, 0
        elif line.startswith("├"):
            part += 1
        elif columns is None:
            columns = {index for index, cell in enumerate(cells) if cell.strip() in decisions}
        elif part == 1:
            cells = ["#" if index in columns else cell for index, cell in enumerate(cells)]
        lines.append("│".join(cells))
    return "\n".join(lines)


def _promised(returncode: int, stderr: str, stdout: str, sql: str) -> _Printed:
    """What a run still promises should its solve have been stopped: its exit code, its stderr with each report of a
    stop cut to what stopped it, and its rows but for the values of its decisions, whatever the solver held when the
    stop came."""
    return _Printed(returncode, _STOP_REPORT.sub(r"\1", stderr), _rows(_decisions_masked(stdout, sql), sql))


def _read_mode(directory: Path) -> dict[tuple, _LoggedQuery]:
    """One mode of a log, a folder of ``_log_query`` files, by query."""
    records = [json.loads(line) for path in sorted(directory.glob("*.jsonl")) for line in path.read_text().splitlines()]
    stdouts = _golden_normalized([_settled(record["stdout"]) for record in records])
    queries: dict[tuple, _LoggedQuery] = {}
    for record, stdout in zip(records, stdouts):
        sql = _TEMP_PATH.sub("<tmp>/", record["sql"])
        env = tuple(sorted((name, _TEMP_PATH.sub("<tmp>/", value)) for name, value in record["env"].items()))
        query = queries.setdefault((record["mode"], sql, env), _LoggedQuery(sql, set(), set(), set()))
        query.tests.add(record["test"].rsplit(" (", 1)[0])
        returncode, stderr = record["returncode"], _settled(record["stderr"])
        query.printed.add(_Printed(returncode, stderr, _rows(stdout, sql)))
        query.promised.add(_promised(returncode, stderr, stdout, sql))
    return queries


def _passed_tests(directory: Path) -> set[str]:
    """The tests that passed in one mode of a log, as ``_junit_name`` spells them."""
    return {
        f"{case.get('classname')}.{case.get('name')}"
        for case in ElementTree.parse(directory / "junit.xml").iter("testcase")
        if not any(child.tag in ("failure", "error", "skipped") for child in case)
    }


def _junit_name(nodeid: str) -> str:
    """A pytest node id as junit writes it: ``tests/a.py::C::t[p]`` is ``tests.a.C.t[p]``."""
    address, bracket, parameters = nodeid.partition("[")
    return address.replace("/", ".").replace(".py::", ".").replace("::", ".") + bracket + parameters


def _verdict(old: _LoggedQuery, new: _LoggedQuery, passed: set[str]) -> str:
    """``same``, ``unproven`` or ``differ``: see the module's docstring."""
    if old.printed == new.printed:
        return "same"
    if (
        all(_STOP_REPORT.search(printed.stderr) for printed in old.printed | new.printed)
        and {_junit_name(test) for test in old.tests | new.tests} <= passed
        and old.promised == new.promised
    ):
        return "unproven"
    return "differ"


def _printed_text(printed: _Printed | None) -> list[str]:
    return [] if printed is None else [f"exit {printed.returncode}", *printed.stderr.splitlines(), *printed.stdout]


def _heading(mode: str, what: str, tests: set[str], sql: str) -> str:
    return f"\n[{mode}] {what}: {min(tests)}\n  {' '.join(sql.split())[:300]}"


def _compare_mode(old_dir: Path, new_dir: Path, limit: int, again_dir: Path | None = None) -> int:
    """Print how one mode of two logs compares; return how many of its queries differ or are in one log alone. With
    ``again_dir``, a second run of OLD's mode, a query printed as either run printed it is seen."""
    mode = old_dir.name
    old, new = _read_mode(old_dir), _read_mode(new_dir)
    again = _read_mode(again_dir) if again_dir is not None and again_dir.is_dir() else {}
    passed = _passed_tests(old_dir) & _passed_tests(new_dir)
    counts = {"same": 0, "unproven": 0, "differ": 0, "seen": 0}
    reports, unproven = [], []
    for key in sorted(old.keys() & new.keys()):
        found = _verdict(old[key], new[key], passed)
        if found == "differ" and key in again and new[key].printed <= old[key].printed | again[key].printed:
            found = "seen"
        counts[found] += 1
        if found in ("unproven", "seen"):
            unproven.append(f"[{mode}] {found}: {min(old[key].tests | new[key].tests)}")
        elif found == "differ":
            removed = min(old[key].printed - new[key].printed, default=None)
            added = min(new[key].printed - old[key].printed, default=None)
            diff = difflib.unified_diff(_printed_text(removed), _printed_text(added), lineterm="", n=1)
            heading = _heading(mode, "differ", old[key].tests | new[key].tests, old[key].sql)
            reports.append("\n".join([heading, *(f"  {line[:200]}" for line in list(diff)[2:40])]))
    for side, here, there in (("old", old, new), ("new", new, old)):
        for key in sorted(here.keys() - there.keys()):
            reports.append(_heading(mode, f"only in {side}", here[key].tests, here[key].sql))
    for report in reports[:limit]:
        print(report)
    if len(reports) > limit:
        print(f"\n[{mode}] {len(reports) - limit} more not shown (--limit)")
    for line in unproven:
        print(line)
    only_old, only_new = len(old.keys() - new.keys()), len(new.keys() - old.keys())
    seen = f", {counts['seen']} as a second baseline run printed" if again_dir is not None else ""
    print(
        f"{mode}: {len(old.keys() | new.keys())} queries: {counts['same']} same, {counts['unproven']} unproven, "
        f"{counts['differ']} differ, {only_old} only in old, {only_new} only in new{seen}"
    )
    return len(reports)


def _modes(log: Path) -> set[str]:
    return {path.name for path in log.glob("*") if path.is_dir()}


def _compare_logs(old: Path, new: Path, limit: int = 20, again: Path | None = None) -> int:
    """Print how two logs compare, mode by mode; return how many differences they hold, a mode only one holds being
    one: a run that captured fewer modes has not shown the others still print what they did."""
    old_modes, new_modes = _modes(old), _modes(new)
    differences = 0
    for mode in sorted(old_modes | new_modes):
        if mode in old_modes and mode in new_modes:
            differences += _compare_mode(old / mode, new / mode, limit, again / mode if again is not None else None)
        else:
            print(f"\n[{mode}] only in {'old' if mode in old_modes else 'new'}: the whole mode")
            differences += 1
    return differences


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Compare two logs of capture_suite_log.sh, query by query.")
    parser.add_argument("old", type=Path)
    parser.add_argument("new", type=Path)
    parser.add_argument("--limit", type=int, default=20, help="the most differing queries to print in full, per mode")
    parser.add_argument("--again", type=Path, help="a second log of OLD's commit: what either run printed is seen")
    args = parser.parse_args(argv)
    for log in (args.old, args.new, *([args.again] if args.again else [])):
        if not _modes(log):
            print(f"{log} holds no mode, so it is no suite log", file=sys.stderr)
            return 2
    differences = _compare_logs(args.old, args.new, args.limit, args.again)
    print(f"{differences} differences" if differences else "no differences")
    return 1 if differences else 0


if __name__ == "__main__":
    sys.exit(main())
