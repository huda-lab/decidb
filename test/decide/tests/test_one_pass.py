"""One-pass evaluation (deck §3, pp. 77-119): the operator reads each row once, as Sink receives it, filling every
node's container top-down, and evaluates the containers bottom-up (``pipeline/08_execution/done.md``).

``execution.descent_rows`` (DECIDB_PROFILE) counts the rows the descent read: the evaluation has no scan of the
buffered input, whose first chunk alone it fetches, for a value the whole query shares.
"""

import json

import pytest

from ._anr_cases import EXAMPLE_9, F1_MAX, F1_SUM, F2_PREVIOUS


def counters(cli, sql: str, name: str) -> list[float]:
    """The `name` counters a query reports, one per evaluation."""
    profiled = cli.with_env({"DECIDB_PROFILE": "1"}).execute_raw(
        "SET threads = 1; SET decide_direct_solve = 'off';\n" + sql
    )
    counters = []
    for line in profiled.stderr.splitlines():
        if not line.startswith("DECIDB_PROFILE: "):
            continue
        event = json.loads(line[len("DECIDB_PROFILE: ") :])
        if event.get("event") == "counter" and event.get("name") == name:
            counters.append(event["value"])
    return counters


@pytest.mark.parametrize("case", [F1_MAX, F1_SUM, F2_PREVIOUS, EXAMPLE_9], ids=lambda case: case.name)
def test_the_evaluation_reads_the_input_once(decidb_cli, case):
    """Nested reducers, a frame and the deck's example 9: the descent reads each row as it arrives, and the evaluation
    never scans the buffer."""
    cli = decidb_cli.without_verify_serializer()
    rows = counters(cli, case.sql, "execution.descent_rows")
    assert len(rows) == 1 and rows[0] > 0, rows
