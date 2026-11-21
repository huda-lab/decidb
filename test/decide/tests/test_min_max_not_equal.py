"""`<>` on a MIN or MAX: the helper holds the extremum, and the row on it is the `<>` disjunction.

Each answer is judged against every assignment of three whole-numbered decisions (`_brute_force`), which states the
clause as Python over the rows, from no solver.
"""

import pytest

from ._brute_force import judge_rows
from ._scope_cases import answer

_DOMAINS = {f"x{i}": range(4) for i in range(3)}
_ROWS = "(VALUES (1, 'a'), (2, 'a'), (3, 'b')) t(id, g)"


def _x(a):
    return [a["x0"], a["x1"], a["x2"]]


def _total(a):
    return sum(_x(a))


@pytest.mark.min_max
@pytest.mark.correctness
@pytest.mark.parametrize(
    "clause, holds",
    [
        pytest.param("max(x) <> 3", lambda a: max(_x(a)) != 3, id="max"),
        pytest.param("min(x) <> 3", lambda a: min(_x(a)) != 3, id="min"),
        pytest.param("sum(x) + max(x) <> 12", lambda a: _total(a) + max(_x(a)) != 12, id="beside-a-sum"),
        pytest.param("2 * max(x) <> 6", lambda a: 2 * max(_x(a)) != 6, id="under-a-factor"),
        pytest.param("-max(x) <> -3", lambda a: -max(_x(a)) != -3, id="negated"),
        pytest.param("max(x) <> (SELECT 3)", lambda a: max(_x(a)) != 3, id="subquery-bound"),
        pytest.param("max(x) by (g) <> 3", lambda a: max(a["x0"], a["x1"]) != 3 and a["x2"] != 3, id="per-group"),
    ],
)
def test_not_equal_on_an_extremum_agrees_with_every_assignment(decidb_cli, clause, holds):
    rows = answer(
        decidb_cli,
        f"SELECT id, x FROM {_ROWS} DECIDE x(INT) SUCH THAT x <= 3 AND {clause} MAXIMIZE sum(x) ORDER BY id",
    )
    judge_rows(rows, _DOMAINS, holds, _total)
