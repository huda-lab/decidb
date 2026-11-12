"""The fuzzers' defects once listed in fuzz/known.py, each fixed at its cause, with its answer worked by hand."""

import pytest

from ._scope_cases import answer

SETUP = "SET decide_direct_solve = 'off';\n"

#: Each query reaches one fixed branch, named, and its answer, worked by hand, differs from the defect's.
CASES = {
    # Q7-wrong-2. Row 3's g is NULL, so it is in no class of the MIN's own `per`, and no member of it
    # (DecideModelEmitter::Filter): the MIN over classes b and a must reach 0, which the cheaper, a (weight 2), does.
    # Read as a member, row 3 (weight 1) reached it: 2, 2, 0.
    "a-row-in-no-class-is-no-member-of-a-per-reducer": (
        """SELECT id, x FROM (VALUES (1, 'b'), (2, 'a'), (3, NULL)) t(id, g) DECIDE x(INT)
        SUCH THAT x <= 2 AND min(per t.g: x) <= 0 MAXIMIZE sum((4 - t.id) * x) ORDER BY id""",
        [2, 0, 2],
    ),
    # Q7-wrong-3. Group b's rows have a NULL k, so no class of the clause's `per` holds them and the clause bounds no
    # group of them (GroupRowsByShape): only group a is bounded. Bounded too, b took 1 of its 10.
    "a-group-in-no-class-holds-no-instance": (
        """SELECT id, x FROM (VALUES (1, 'a', 'p'), (2, 'b', NULL), (3, 'b', NULL)) t(id, g, k) DECIDE x(INT)
        SUCH THAT x <= 5 AND per t.k: sum(x) by (t.g) <= 1 MAXIMIZE sum(x) ORDER BY id""",
        [1, 5, 5],
    ),
    # Every k is NULL and the clause has no `by` key: its kept rows are one group, in no class, so it holds nothing
    # (GroupRowsByShape, the group of every kept row).
    "a-clause-with-no-class-and-no-key-holds-nothing": (
        """SELECT id, x FROM (VALUES (1, NULL::VARCHAR), (2, NULL)) t(id, k) DECIDE x(INT)
        SUCH THAT x <= 5 AND per t.k: sum(x) <= 1 MAXIMIZE sum(x) ORDER BY id""",
        [5, 5],
    ),
    # Q7-internal-abs. The inner sum is lifted into a helper before the ABS rewrite (LiftNestedReducers); its body's
    # ABS is lowered too (RewriteAbs over the helpers' bodies): 2 * (|x1| + |x2|) <= 4, so x1 + x2 <= 2, all on x2.
    "an-abs-in-a-lifted-reducer": (
        """SELECT id, x FROM (VALUES (1), (2)) t(id) DECIDE x(INT)
        SUCH THAT x <= 3 AND sum(abs(sum(abs(x)))) <= 4 MAXIMIZE sum(t.id * x) ORDER BY id""",
        [0, 2],
    ),
    # The same body under `>=`: its ABS is pinned exactly (TagAbsForBigM over the helpers' bodies), so |x1| + |x2| >=
    # 2 costs the cheaper x1 two units. Pinned from below alone, the helper's value grew past 2 with x at 0.
    "an-abs-in-a-lifted-reducer-pushed-up": (
        """SELECT id, x FROM (VALUES (1), (2)) t(id) DECIDE x(INT)
        SUCH THAT x <= 3 AND sum(abs(sum(abs(x)))) >= 4 MINIMIZE sum(t.id * x) ORDER BY id""",
        [2, 0],
    ),
}


@pytest.mark.parametrize("case", list(CASES))
def test_a_fuzz_defect_answers_as_worked_by_hand(decidb_cli, case):
    sql, xs = CASES[case]
    assert [row["x"] for row in answer(decidb_cli, SETUP + sql)] == xs


def test_a_pinned_keyed_decision_bounds_a_max_on_highs(decidb_cli):
    """D5. Row 3's p is NULL, so it has no instance of x and no bound from `x = t.v`; every instance x has is pinned,
    so x is bounded and HiGHS lowers the MAX with a Big-M (DecidePropagateImpliedBounds bounds a decision by the rows
    that read its instances). The row with no instance stopped the bound, and HiGHS refused for want of one."""
    sql = """SELECT id, x FROM (VALUES (1, 'a', 10), (2, 'a', 10), (3, NULL, 4)) t(id, p, v)
        DECIDE per t.p: x(REAL) SUCH THAT x = t.v AND max(x) >= 5 MAXIMIZE sum(x) ORDER BY id"""
    cli = decidb_cli.with_env({"DECIDB_FORCE_SOLVER": "highs"})
    assert [row["x"] for row in answer(cli, SETUP + sql)] == [10, 10, None]
