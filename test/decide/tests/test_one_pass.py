"""One-pass evaluation (deck §3, pp. 77-119): the operator reads its collected rows once, filling every node's
container top-down, and evaluates the containers bottom-up (``pipeline/08_execution/done.md``).

``execution.input_scans`` (DECIDB_PROFILE) counts the scans of the input a query's evaluation makes.
"""

import json

import pytest

from ._anr_cases import EXAMPLE_9, F1_MAX, F1_SUM, F2_PREVIOUS


def input_scans(cli, sql: str) -> list[float]:
    """The `execution.input_scans` counters a query reports, one per evaluation."""
    profiled = cli.with_env({"DECIDB_PROFILE": "1"}).execute_raw(
        "SET threads = 1; SET decide_direct_solve = 'off';\n" + sql
    )
    counters = []
    for line in profiled.stderr.splitlines():
        if not line.startswith("DECIDB_PROFILE: "):
            continue
        event = json.loads(line[len("DECIDB_PROFILE: ") :])
        if event.get("event") == "counter" and event.get("name") == "execution.input_scans":
            counters.append(event["value"])
    return counters


@pytest.mark.parametrize("case", [F1_MAX, F1_SUM, F2_PREVIOUS, EXAMPLE_9], ids=lambda case: case.name)
def test_the_evaluation_reads_the_input_once(decidb_cli, case):
    """Nested reducers, a frame and the deck's example 9: every clause, key and frame reads the pass."""
    assert input_scans(decidb_cli.without_verify_serializer(), case.sql) == [1]
