"""Tests for like-term collection (BuildDecidePreparedModel, stage 05).

`2*ship + 3*ship` used to reach the solver as two terms naming one column.
Collection sums them into one, so no consumer has to remember that a variable
index can repeat.

The merge itself is deliberately invisible from outside: the model builder
already folded duplicate column entries when writing the matrix row, so a merged
and an unmerged term list emit the identical row. The combined-coefficient
behaviour that *is* observable has its own test in `test_implied_bounds.py`.

What these tests guard is the other direction — the cases where two terms name
one column but are **not** the same contribution, and merging them would be
wrong. Naming a variable is not enough; a term also says which rows it applies
to and which reducer produced it.
"""

import re

import pytest


def _row_terms(dump: str, row: int = 0) -> dict[int, float]:
    """`row N: ... | col:coef col:coef` → {col: coef}."""
    match = re.search(rf"^row {row}: .*?\|(.*)$", dump, re.M)
    assert match, f"no row {row} in dump:\n{dump}"
    return {
        int(col): float(coef)
        for col, coef in (pair.split(":") for pair in match.group(1).split())
    }


_ROWS = "(VALUES (1, 'A'), (2, 'B'), (3, 'A')) t(id, grp)"


@pytest.mark.correctness
def test_differing_aggregate_local_when_filters_do_not_merge(decidb_cli, tmp_path):
    """`SUM(x) WHEN a + SUM(x) WHEN b` is two row sets, not one column twice.

    Merging these would keep one term's mask and apply it to both, silently
    dropping every row the surviving filter excludes. Here that would drop the
    'B' row from the constraint entirely, so the check is that all three columns
    are still present.
    """
    dump = decidb_cli.dump_model(
        f"""
            SELECT id, x FROM {_ROWS}
            DECIDE x(INT)
            SUCH THAT x <= 9
                  AND SUM(x) WHEN (grp = 'A') + SUM(x) WHEN (grp = 'B') <= 6
            MAXIMIZE SUM(x)
        """,
        tmp_path / "when.dump")
    assert _row_terms(dump) == pytest.approx({0: 1.0, 1: 1.0, 2: 1.0}), \
        f"a disjoint-WHEN pair collapsed into one masked term:\n{dump}"


@pytest.mark.correctness
def test_avg_and_sum_on_one_column_do_not_merge(decidb_cli, tmp_path):
    """`SUM(x) + AVG(x)` names one column under two scalings.

    AVG terms are divided by the group's row count downstream, so summing them
    with a SUM term before that scaling applies the division to both. Over three
    rows the correct combined coefficient is 1 + 1/3.
    """
    dump = decidb_cli.dump_model(
        f"""
            SELECT id, x FROM {_ROWS}
            DECIDE x(INT) SUCH THAT x <= 9 AND SUM(x) + AVG(x) <= 8
            MAXIMIZE SUM(x)
        """,
        tmp_path / "avg.dump")
    assert _row_terms(dump) == pytest.approx({c: 4.0 / 3.0 for c in range(3)}), \
        f"an AVG term merged with a SUM term before scaling:\n{dump}"


@pytest.mark.correctness
def test_opposite_signs_subtract_rather_than_add(decidb_cli, tmp_path):
    """`SUM(3*x) - SUM(x)` combines to 2, not 4.

    A term contributes `sign * coefficient`, so a group merges as
    `sign_first * (coef_first ± coef_next)`. Getting the sign wrong here would
    not fail loudly — it would emit a plausible row with the wrong slope.
    """
    dump = decidb_cli.dump_model(
        f"""
            SELECT id, x FROM {_ROWS}
            DECIDE x(INT) SUCH THAT x <= 9 AND SUM(3 * x) - SUM(x) <= 12
            MAXIMIZE SUM(x)
        """,
        tmp_path / "signs.dump")
    assert _row_terms(dump) == pytest.approx({c: 2.0 for c in range(3)}), \
        f"opposite-signed like terms did not subtract:\n{dump}"
