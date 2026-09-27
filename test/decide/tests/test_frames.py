"""Frame expressions (DeciQL spec §4.2, §7.3): navigating a peer-ordered timeline.

`AT(sel [ELSE v]: e) OVER (key [DESC] [CYCLIC] [WITHIN P])` reads `e` at a position
relative to the instance's own; `SUM(FROM a TO b [EVERY d] [ELSE v | ALL]: e) OVER (...)`
reduces `e` over a range of positions. A missing position is skipped inside a range,
filled under `ELSE v`, or drops the instance (`AT` with no ELSE, `ALL`, or a range
with no position at all).

Every oracle below states the navigated rows explicitly.
"""

import pytest

from solver.types import ObjSense, SolverStatus, VarType

# (product, period, demand, cap, cost, hold)
_PLAN = ("(VALUES ('A', 1, 3, 6, 2, 1), ('A', 2, 5, 6, 2, 1), ('A', 3, 4, 6, 3, 1), "
         "('B', 1, 2, 4, 1, 2), ('B', 2, 6, 4, 1, 2), ('B', 3, 1, 4, 1, 2)) "
         "p(product, period, demand, cap, cost, hold)")
_DATA = {("A", 1): (3, 6, 2, 1), ("A", 2): (5, 6, 2, 1), ("A", 3): (4, 6, 3, 1),
         ("B", 1): (2, 4, 1, 2), ("B", 2): (6, 4, 1, 2), ("B", 3): (1, 4, 1, 2)}


def _rows(decidb_cli, sql, *cols):
    rows, names = decidb_cli.execute(sql)
    idx = [names.index(c) for c in cols]
    return sorted(tuple(r[i] for i in idx) for r in rows)


@pytest.mark.var_integer
@pytest.mark.correctness
def test_inventory_balance_with_previous_stock(decidb_cli, oracle_solver):
    """The production plan: stock carries over from the previous period within a
    product, starting from 0. Timeline navigation, not constraint generation."""
    got = _rows(decidb_cli, f"""
        SELECT product, period, produce, stock FROM {_PLAN}
        DECIDE produce(INT) BETWEEN 0 AND cap, stock(INT) BETWEEN 0 AND 20
        SUCH THAT stock = AT(PREVIOUS ELSE 0: stock) OVER (period WITHIN product) + produce - demand
        MINIMIZE SUM(produce * cost) + SUM(stock * hold)
    """, "product", "period", "produce", "stock")

    oracle_solver.create_model("balance")
    obj = {}
    for (prod, t), (demand, cap, cost, hold) in _DATA.items():
        oracle_solver.add_variable(f"produce_{prod}{t}", VarType.INTEGER, lb=0.0, ub=float(cap))
        oracle_solver.add_variable(f"stock_{prod}{t}", VarType.INTEGER, lb=0.0, ub=20.0)
        obj[f"produce_{prod}{t}"] = float(cost)
        obj[f"stock_{prod}{t}"] = float(hold)
    for (prod, t), (demand, *_) in _DATA.items():
        row = {f"stock_{prod}{t}": 1.0, f"produce_{prod}{t}": -1.0}
        if t > 1:
            row[f"stock_{prod}{t - 1}"] = -1.0
        oracle_solver.add_constraint(row, "=", -float(demand))
    oracle_solver.set_objective(obj, ObjSense.MINIMIZE)
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL

    by_key = {(p, t): (int(pr), int(st)) for p, t, pr, st in got}
    # The balance holds row by row against the previous period's stock.
    for (prod, t), (demand, *_) in _DATA.items():
        prev = by_key[(prod, t - 1)][1] if t > 1 else 0
        assert by_key[(prod, t)][1] == prev + by_key[(prod, t)][0] - demand
    value = sum(pr * _DATA[k][2] + st * _DATA[k][3] for k, (pr, st) in by_key.items())
    assert value == pytest.approx(result.objective_value)
    # B needs 6 in period 2 with a cap of 4, so it must build stock in period 1.
    assert by_key[("B", 1)] == (4, 2)


@pytest.mark.var_integer
@pytest.mark.correctness
def test_range_frame_rolling_capacity(decidb_cli, oracle_solver):
    """`SUM(FROM 2 PREVIOUS TO PREVIOUS: produce)`: the two periods before this one
    together may not exceed a rolling limit; the first periods see fewer positions."""
    got = _rows(decidb_cli, f"""
        SELECT product, period, produce FROM {_PLAN}
        DECIDE produce(INT) BETWEEN 0 AND cap
        SUCH THAT SUM(FROM 2 PREVIOUS TO PREVIOUS: produce) OVER (period WITHIN product) <= 7
              AND produce + 0 <= cap
        MAXIMIZE SUM(produce)
    """, "product", "period", "produce")

    oracle_solver.create_model("rolling")
    obj = {}
    for (prod, t), (_, cap, *_) in _DATA.items():
        oracle_solver.add_variable(f"produce_{prod}{t}", VarType.INTEGER, lb=0.0, ub=float(cap))
        obj[f"produce_{prod}{t}"] = 1.0
    for (prod, t) in _DATA:
        prev = {f"produce_{prod}{u}": 1.0 for u in (t - 2, t - 1) if (prod, u) in _DATA}
        if prev:
            oracle_solver.add_constraint(prev, "<=", 7.0)
    oracle_solver.set_objective(obj, ObjSense.MAXIMIZE)
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL

    by_key = {(p, t): int(pr) for p, t, pr in got}
    for prod in ("A", "B"):
        assert by_key[(prod, 1)] + by_key[(prod, 2)] <= 7
    assert sum(by_key.values()) == pytest.approx(result.objective_value)


@pytest.mark.var_integer
@pytest.mark.edge_case
@pytest.mark.correctness
def test_missing_position_policies(decidb_cli):
    """No ELSE: the first period has no previous position, so its instance is not
    imposed. `ELSE 0`: it reads 0 there. `ALL` on a range needing two earlier
    positions imposes only from period 3."""
    # AT with no ELSE: period 1 is free of the `x <= previous x` chain.
    got = _rows(decidb_cli, f"""
        SELECT product, period, x FROM {_PLAN}
        DECIDE x(INT) BETWEEN 0 AND 9
        SUCH THAT x <= AT(PREVIOUS: x) OVER (period WITHIN product) - 1
        MAXIMIZE SUM(x)
    """, "product", "period", "x")
    assert [x for p, t, x in got if p == "A"] == [9, 8, 7]
    # ELSE 5 on the same shape makes period 1 read 5, so the chain starts at 4.
    got = _rows(decidb_cli, f"""
        SELECT product, period, x FROM {_PLAN}
        DECIDE x(INT) BETWEEN 0 AND 9
        SUCH THAT x <= AT(PREVIOUS ELSE 5: x) OVER (period WITHIN product) - 1
        MAXIMIZE SUM(x)
    """, "product", "period", "x")
    assert [x for p, t, x in got if p == "A"] == [4, 3, 2]
    # ALL: the rolling row is imposed only where both earlier positions exist.
    got = _rows(decidb_cli, f"""
        SELECT product, period, x FROM {_PLAN}
        DECIDE x(INT) BETWEEN 0 AND 9
        SUCH THAT SUM(FROM 2 PREVIOUS TO PREVIOUS ALL: x) OVER (period WITHIN product) <= 5
        MAXIMIZE SUM(x)
    """, "product", "period", "x")
    by_key = {(p, t): x for p, t, x in got}
    assert by_key[("A", 1)] + by_key[("A", 2)] <= 5 and by_key[("A", 3)] == 9


@pytest.mark.var_integer
@pytest.mark.correctness
def test_cyclic_and_descending_order(decidb_cli):
    """CYCLIC wraps the last position round to the first; DESC reverses the walk."""
    # A cyclic chain `x(d) <= x(next d) - 1` around three days is infeasible: every
    # value would have to be below the next, round the circle.
    decidb_cli.assert_error("""
        SELECT day, x FROM (VALUES (1), (2), (3)) t(day)
        DECIDE x(INT) BETWEEN 0 AND 9
        SUCH THAT x <= AT(NEXT: x) OVER (day CYCLIC) - 1
        MAXIMIZE SUM(x)
    """, match="infeasible")
    # Without CYCLIC the last day has no next, so it is the free end of the chain.
    got = _rows(decidb_cli, """
        SELECT day, x FROM (VALUES (1), (2), (3)) t(day)
        DECIDE x(INT) BETWEEN 0 AND 9
        SUCH THAT x <= AT(NEXT: x) OVER (day) - 1
        MAXIMIZE SUM(x)
    """, "day", "x")
    assert [x for _, x in got] == [7, 8, 9]
    # DESC: `previous` now means the later day, so the chain runs the other way.
    got = _rows(decidb_cli, """
        SELECT day, x FROM (VALUES (1), (2), (3)) t(day)
        DECIDE x(INT) BETWEEN 0 AND 9
        SUCH THAT x <= AT(PREVIOUS: x) OVER (day DESC) - 1
        MAXIMIZE SUM(x)
    """, "day", "x")
    assert [x for _, x in got] == [7, 8, 9]


@pytest.mark.var_integer
@pytest.mark.correctness
def test_data_only_frame_reads_another_rows_data(decidb_cli):
    """`AT(PREVIOUS ELSE 100: cap)`: a bound read from the previous row."""
    got = _rows(decidb_cli, """
        SELECT t, x FROM (VALUES (1, 5), (2, 2), (3, 8)) r(t, cap)
        DECIDE x(INT) BETWEEN 0 AND 50
        SUCH THAT x <= AT(PREVIOUS ELSE 100: cap) OVER (t)
        MAXIMIZE SUM(x)
    """, "t", "x")
    assert got == [(1, 50), (2, 5), (3, 2)]


@pytest.mark.var_integer
@pytest.mark.correctness
def test_every_steps_through_a_range(decidb_cli):
    """`FROM 4 PREVIOUS TO PREVIOUS EVERY 3`: positions 4 back and 1 back only."""
    got = _rows(decidb_cli, """
        SELECT t, x FROM (VALUES (1), (2), (3), (4), (5)) r(t)
        DECIDE x(INT) BETWEEN 0 AND 9
        SUCH THAT SUM(FROM 4 PREVIOUS TO PREVIOUS EVERY 3: x) OVER (t) <= 10
        MAXIMIZE SUM(x)
    """, "t", "x")
    by_t = dict(got)
    # Row 5 reads rows 1 and 4; rows 2..4 read only their previous row, which the
    # cap of 9 already satisfies. So only `x1 + x4 <= 10` binds: 37 in total.
    assert by_t[1] + by_t[4] <= 10
    assert sum(by_t.values()) == 37


@pytest.mark.error
@pytest.mark.error_binder
def test_frame_in_an_objective_is_rejected(decidb_cli):
    decidb_cli.assert_error("""
        SELECT t, x FROM (VALUES (1), (2)) r(t)
        DECIDE x(INT) BETWEEN 0 AND 9
        SUCH THAT x <= 9
        MAXIMIZE AT(PREVIOUS: x) OVER (t)
    """, match=r"the position of a frame")


@pytest.mark.error
@pytest.mark.error_binder
def test_frame_order_key_cannot_be_a_decision(decidb_cli):
    decidb_cli.assert_error("""
        SELECT t, x FROM (VALUES (1), (2)) r(t)
        DECIDE x(INT) BETWEEN 0 AND 9, y(INT) BETWEEN 0 AND 9
        SUCH THAT x <= AT(PREVIOUS: x) OVER (y)
        MAXIMIZE SUM(x)
    """, match=r"OVER key orders known rows, so it cannot reference a decision")


@pytest.mark.error
@pytest.mark.error_binder
def test_min_range_frame_is_not_available(decidb_cli):
    decidb_cli.assert_error("""
        SELECT t, x FROM (VALUES (1), (2)) r(t)
        DECIDE x(INT) BETWEEN 0 AND 9
        SUCH THAT MIN(FROM 2 PREVIOUS TO PREVIOUS: x) OVER (t) <= 5
        MAXIMIZE SUM(x)
    """, match=r"A range frame reduces with SUM; MIN")


@pytest.mark.correctness
def test_explain_renders_the_frame_as_written(decidb_cli):
    result = decidb_cli.execute_raw(f"""
        EXPLAIN SELECT product, period, stock FROM {_PLAN}
        DECIDE produce(INT) BETWEEN 0 AND cap, stock(INT) BETWEEN 0 AND 20
        SUCH THAT stock = AT(PREVIOUS ELSE 0: stock) OVER (period WITHIN product) + produce - demand
        MINIMIZE SUM(produce * cost)
    """)
    text = result.stdout + result.stderr
    compact = "".join(ch for ch in text if ch not in "│┌┐└┘─┬┴ \n")
    assert "AT(PREVIOUSELSE0:stock)OVER(periodWITHINproduct)" in compact, text
