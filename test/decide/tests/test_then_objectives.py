"""`MAXIMIZE a THEN MINIMIZE b` -- lexicographic objectives (DeciQL spec §4.3, §7.5).

The first objective is optimized; among its optima the second is optimized; and so
on. Every oracle below performs the same staged solve explicitly: solve stage one,
freeze its value as a constraint, solve stage two.
"""

import pytest

from solver.types import ObjSense, SolverStatus, VarType

# (shift, need, overtime_cost, pref): staff each shift; understaffing is what matters
# most, overtime next, preference last.
_SHIFTS = "(VALUES ('mon', 3, 2, 5), ('tue', 2, 5, 1), ('wed', 4, 1, 3)) s(shift, need, ot_cost, pref)"


def _rows(decidb_cli, sql, *cols):
    rows, names = decidb_cli.execute(sql)
    idx = [names.index(c) for c in cols]
    return sorted(tuple(r[i] for i in idx) for r in rows)


@pytest.mark.var_integer
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_second_stage_breaks_ties_among_first_stage_optima(decidb_cli, oracle_solver):
    """Total staffing is capped, so understaffing cannot be zero; among the
    assignments that minimize it, the one with the cheapest overtime wins."""
    sql = f"""
        SELECT shift, work, short FROM {_SHIFTS}
        DECIDE work(INT) BETWEEN 0 AND 6, short(INT) BETWEEN 0 AND 10
        SUCH THAT work + short >= need AND SUM(work) <= 7
        MINIMIZE SUM(short) THEN MINIMIZE SUM(work * ot_cost)
    """
    got = _rows(decidb_cli, sql, "shift", "work", "short")

    data = {"mon": (3, 2), "tue": (2, 5), "wed": (4, 1)}

    def build(freeze_short=None):
        oracle_solver.create_model("lexicographic")
        for s in data:
            oracle_solver.add_variable(f"work_{s}", VarType.INTEGER, lb=0.0, ub=6.0)
            oracle_solver.add_variable(f"short_{s}", VarType.INTEGER, lb=0.0, ub=10.0)
            oracle_solver.add_constraint({f"work_{s}": 1.0, f"short_{s}": 1.0}, ">=", float(data[s][0]))
        oracle_solver.add_constraint({f"work_{s}": 1.0 for s in data}, "<=", 7.0)
        if freeze_short is not None:
            oracle_solver.add_constraint({f"short_{s}": 1.0 for s in data}, "<=", freeze_short)

    build()
    oracle_solver.set_objective({f"short_{s}": 1.0 for s in data}, ObjSense.MINIMIZE)
    stage1 = oracle_solver.solve()
    assert stage1.status == SolverStatus.OPTIMAL
    build(freeze_short=stage1.objective_value)
    oracle_solver.set_objective({f"work_{s}": float(data[s][1]) for s in data}, ObjSense.MINIMIZE)
    stage2 = oracle_solver.solve()
    assert stage2.status == SolverStatus.OPTIMAL

    total_short = sum(int(sh) for _, _, sh in got)
    total_ot = sum(int(w) * data[s][1] for s, w, _ in got)
    assert total_short == pytest.approx(stage1.objective_value)
    assert total_ot == pytest.approx(stage2.objective_value)
    # The tie-break is real: without the second stage a costlier assignment (working
    # 'tue' at cost 5 instead of 'mon' or 'wed') is also short by exactly 2.
    assert total_short == 2 and total_ot == 10


@pytest.mark.var_integer
@pytest.mark.correctness
def test_three_stages_in_order(decidb_cli):
    """`MINIMIZE short THEN MINIMIZE overtime THEN MAXIMIZE preference`: every stage
    ranks only within the previous stages' optima."""
    got = _rows(decidb_cli, f"""
        SELECT shift, work, short FROM {_SHIFTS}
        DECIDE work(INT) BETWEEN 0 AND 6, short(INT) BETWEEN 0 AND 10
        SUCH THAT work + short >= need AND SUM(work) <= 7
        MINIMIZE SUM(short) THEN MINIMIZE SUM(work * ot_cost) THEN MAXIMIZE SUM(work * pref)
    """, "shift", "work", "short")
    by_shift = {s: (int(w), int(sh)) for s, w, sh in got}
    assert sum(sh for _, sh in by_shift.values()) == 2
    assert sum(w * c for (w, _), c in zip(
        (by_shift[s] for s in ("mon", "tue", "wed")), (2, 5, 1))) == 10
    # Among the (short = 2, overtime = 10) assignments, the third stage cannot move:
    # the overtime optimum already fixes the assignment, so it is confirmed rather than changed.
    assert by_shift["mon"] == (3, 0) and by_shift["wed"] == (4, 0) and by_shift["tue"] == (0, 2)


@pytest.mark.var_boolean
@pytest.mark.correctness
def test_stage_over_a_keyed_reducer(decidb_cli):
    """A later stage may use the same reducer spellings as the first."""
    got = _rows(decidb_cli, """
        SELECT id, grp, x FROM (VALUES (1, 'a', 4), (2, 'a', 4), (3, 'b', 1)) t(id, grp, w)
        DECIDE x(BOOL), PER grp: open(BOOL)
        SUCH THAT PER grp IF NOT open: SUM(x) BY (grp) <= 0 AND SUM(x) <= 2
        MAXIMIZE SUM(x * w) THEN MINIMIZE SUM(PER grp: open)
    """, "id", "x")
    # Two picks of weight 4 are best and both sit in group 'a'; opening only 'a' is
    # the second stage's answer, so row 3 is not picked even though it would be free.
    assert got == [(1, True), (2, True), (3, False)]


@pytest.mark.error
@pytest.mark.error_binder
def test_nonlinear_stage_is_rejected(decidb_cli):
    decidb_cli.assert_error(f"""
        SELECT shift, work FROM {_SHIFTS}
        DECIDE work(INT) BETWEEN 0 AND 6
        SUCH THAT SUM(work) <= 7
        MINIMIZE SUM(work) THEN MINIMIZE MAX(work)
    """, match=r"A THEN objective stage must be linear.*MAX")


@pytest.mark.correctness
def test_explain_renders_every_stage(decidb_cli):
    result = decidb_cli.execute_raw(f"""
        EXPLAIN SELECT shift, work FROM {_SHIFTS}
        DECIDE work(INT) BETWEEN 0 AND 6
        SUCH THAT SUM(work) <= 7
        MINIMIZE SUM(work) THEN MAXIMIZE SUM(work * pref)
    """)
    text = result.stdout + result.stderr
    compact = "".join(ch for ch in text if ch not in "│┌┐└┘─┬┴ \n")
    assert "THENMAXIMIZESUM(work*pref)" in compact, text
