"""Failures in computed frame values belong to the anchors that actually read them.

A frame is computed before its clause's instances are settled. A NULL ``per`` key can leave an anchor without an
instance, so its payload, fallback or range error must travel with the value until a surviving consumer reads it.
"""

import pytest

from ._output_helpers import assert_no_internal_leak
from ._scope_cases import answer


FRAME_ERRORS = [
    pytest.param(
        "at(previous: sqrt(t.c))",
        (-4.0, 9.0, 7.0),
        2,
        3,
        "square root",
        id="point-payload",
    ),
    pytest.param(
        "at(next else sqrt(t.c): 4)",
        (4.0, 9.0, -1.0),
        3,
        4,
        "square root",
        id="point-fallback",
    ),
    pytest.param(
        "sum(from previous to previous: sqrt(t.c))",
        (-4.0, 9.0, 7.0),
        2,
        3,
        "square root",
        id="range-payload",
    ),
    pytest.param(
        "sum(from next to next else sqrt(t.c): 4)",
        (4.0, 9.0, -1.0),
        3,
        4,
        "square root",
        id="range-fallback",
    ),
    pytest.param(
        "min(from previous to previous: t.c)",
        ("NaN", 9.0, 7.0),
        2,
        9,
        "reads a NaN",
        id="range-fold",
    ),
]


def _query(frame, cells, failed_anchor, consumed):
    rows = ", ".join(
        f"({i}, 'a', {'1' if consumed or i != failed_anchor else 'NULL'}, {i}, '{value}'::DOUBLE)"
        for i, value in enumerate(cells, 1)
    )
    return f"""SET decide_direct_solve = 'off';
        SELECT id, x FROM (VALUES {rows}) t(id, g, k, p, c) DECIDE x(REAL)
        SUCH THAT x <= 100 AND per t.k: sum(x) by (t.g) <= {frame} over (t.p)
        MAXIMIZE sum(x) ORDER BY id"""


@pytest.mark.correctness
@pytest.mark.parametrize("frame,cells,failed_anchor,total,error", FRAME_ERRORS)
def test_a_frame_failure_without_a_consuming_instance_is_deferred(
    decidb_cli, frame, cells, failed_anchor, total, error
):
    """Only a NULL-key anchor reads the bad value; the remaining instance still enforces its good bound."""
    rows = answer(decidb_cli, _query(frame, cells, failed_anchor, consumed=False))
    assert sum(row["x"] for row in rows) == pytest.approx(total)


@pytest.mark.error
@pytest.mark.parametrize("frame,cells,failed_anchor,total,error", FRAME_ERRORS)
def test_a_frame_failure_is_raised_when_an_instance_consumes_it(
    decidb_cli, frame, cells, failed_anchor, total, error
):
    """The same anchor now has a class: deferral must preserve and report its original failure."""
    result = decidb_cli.execute_raw(_query(frame, cells, failed_anchor, consumed=True))
    assert error in result.stderr, result.stderr
    assert_no_internal_leak(result, "a consumed frame failure")
