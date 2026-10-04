"""`IF b:` -- a constraint instance imposed only where a decision-bearing guard holds.

DeciQL spec §7.4: for `[WHEN θ] [PER K] [IF b]: B`, each generated instance is the
implication `b ⟹ B`. `θ` filters known rows before the solve; `b` reads decisions
and switches the instance's row inside the model.

The guard's spellings: a BOOL decision (`IF open:`), its negation (`IF NOT open:`),
or a linear comparison over integer-valued decisions (`IF ship > 0:`). Every oracle
below states the same implication with an explicit Big-M, so the check is against
an independently built model, never against a hand-computed answer.
"""

import pytest

from solver.types import ObjSense, SolverStatus, VarType

# (id, cap, profit, open_cost) -- a shipment is only possible from an opened row.
_ROWS = "(VALUES (1, 10, 5, 20), (2, 8, 3, 50), (3, 6, 9, 10)) t(id, cap, profit, open_cost)"


def _solve_rows(decidb_cli, sql, *cols):
    rows, names = decidb_cli.execute(sql)
    idx = [names.index(c) for c in cols]
    return sorted(tuple(r[i] for i in idx) for r in rows)


@pytest.mark.var_boolean
@pytest.mark.var_integer
@pytest.mark.cons_perrow
@pytest.mark.correctness
def test_bool_decision_guard_switches_a_per_row_bound(decidb_cli, oracle_solver):
    """`IF NOT open: ship <= 0` -- shipping requires opening, opening costs."""
    got = _solve_rows(decidb_cli, f"""
        SELECT id, open, ship FROM {_ROWS}
        DECIDE open(BOOL), ship(INT) BETWEEN 0 AND cap
        SUCH THAT IF NOT open: ship <= 0
        MAXIMIZE SUM(ship * profit) - SUM(open * open_cost)
    """, "id", "open", "ship")

    oracle_solver.create_model("bool_guard")
    data = {1: (10, 5, 20), 2: (8, 3, 50), 3: (6, 9, 10)}
    obj = {}
    for i, (cap, profit, cost) in data.items():
        oracle_solver.add_variable(f"open_{i}", VarType.BINARY)
        oracle_solver.add_variable(f"ship_{i}", VarType.INTEGER, lb=0.0, ub=float(cap))
        # open = 0 ⟹ ship <= 0:  ship - cap * open <= 0
        oracle_solver.add_constraint({f"ship_{i}": 1.0, f"open_{i}": -float(cap)}, "<=", 0.0)
        obj[f"ship_{i}"] = float(profit)
        obj[f"open_{i}"] = -float(cost)
    oracle_solver.set_objective(obj, ObjSense.MAXIMIZE)
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL

    # Row 2 never pays for itself (8 * 3 < 50); rows 1 and 3 do.
    assert got == [(1, True, 10), (2, False, 0), (3, True, 6)]
    total = sum(int(s) * data[i][1] - int(o) * data[i][2] for i, o, s in got)
    assert total == pytest.approx(result.objective_value)


@pytest.mark.var_boolean
@pytest.mark.var_integer
@pytest.mark.cons_perrow
@pytest.mark.correctness
def test_bool_decision_guard_positive_spelling(decidb_cli):
    """`IF open: ship >= 3` -- an opened row must ship a minimum; the unguarded rows
    are free. The answer is pinned against the same query without the guard: the
    guard binds only where opening is worth it."""
    with_guard = _solve_rows(decidb_cli, f"""
        SELECT id, open, ship FROM {_ROWS}
        DECIDE open(BOOL), ship(INT) BETWEEN 0 AND cap
        SUCH THAT IF open: ship >= 3 AND IF NOT open: ship <= 0
        MAXIMIZE SUM(ship * profit) - SUM(open * open_cost)
    """, "id", "open", "ship")
    # Every opened row ships at least 3; every closed row ships nothing.
    for _, is_open, ship in with_guard:
        assert (int(ship) >= 3) if is_open else (int(ship) == 0)
    assert with_guard == [(1, True, 10), (2, False, 0), (3, True, 6)]


@pytest.mark.var_integer
@pytest.mark.cons_perrow
@pytest.mark.correctness
def test_comparison_guard_minimum_lot(decidb_cli, oracle_solver):
    """`IF ship > 0: ship >= lot` -- a minimum lot size, the classic semicontinuous
    shape, written as an implication over an INT decision."""
    sql = """
        SELECT id, ship FROM (VALUES (1, 4, 5), (2, 6, 3), (3, 5, 9)) t(id, lot, profit)
        DECIDE ship(INT) BETWEEN 0 AND 7
        SUCH THAT IF ship > 0: ship >= lot AND SUM(ship) <= 12
        MAXIMIZE SUM(ship * profit)
    """
    got = _solve_rows(decidb_cli, sql, "id", "ship")

    oracle_solver.create_model("lot_guard")
    data = {1: (4, 5), 2: (6, 3), 3: (5, 9)}
    obj, total = {}, {}
    for i, (lot, profit) in data.items():
        oracle_solver.add_variable(f"ship_{i}", VarType.INTEGER, lb=0.0, ub=7.0)
        oracle_solver.add_variable(f"y_{i}", VarType.BINARY)
        # y = 0 ⟹ ship <= 0 ;  y = 1 ⟹ ship >= lot
        oracle_solver.add_constraint({f"ship_{i}": 1.0, f"y_{i}": -7.0}, "<=", 0.0)
        oracle_solver.add_constraint({f"ship_{i}": 1.0, f"y_{i}": -float(lot)}, ">=", 0.0)
        obj[f"ship_{i}"] = float(profit)
        total[f"ship_{i}"] = 1.0
    oracle_solver.add_constraint(total, "<=", 12.0)
    oracle_solver.set_objective(obj, ObjSense.MAXIMIZE)
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL

    for i, ship in got:
        assert int(ship) == 0 or int(ship) >= data[i][0], got
    assert sum(int(s) * data[i][1] for i, s in got) == pytest.approx(result.objective_value)


@pytest.mark.var_boolean
@pytest.mark.var_integer
@pytest.mark.cons_aggregate
@pytest.mark.per_clause
@pytest.mark.correctness
def test_keyed_guard_on_a_reduced_body(decidb_cli, oracle_solver):
    """`PER d: IF NOT open: SUM(ship) BY (d) <= 0` -- a depot ships only when open,
    the guard read once per depot from the depot's own decision."""
    sql = """
        SELECT routeID, d, open, ship
        FROM (VALUES ('T1', 'D1', 6), ('T2', 'D1', 4), ('T3', 'D2', 9)) t(routeID, d, profit)
        DECIDE PER d: open(BOOL), ship(INT) BETWEEN 0 AND 5
        SUCH THAT PER d IF NOT open: SUM(ship) BY (d) <= 0 AND SUM(ship) <= 8
        MAXIMIZE SUM(ship * profit) - SUM(PER d: open * 30)
    """
    got = _solve_rows(decidb_cli, sql, "routeID", "open", "ship")

    oracle_solver.create_model("keyed_guard")
    for d in ("D1", "D2"):
        oracle_solver.add_variable(f"open_{d}", VarType.BINARY)
    routes = {"T1": ("D1", 6), "T2": ("D1", 4), "T3": ("D2", 9)}
    for r in routes:
        oracle_solver.add_variable(f"ship_{r}", VarType.INTEGER, lb=0.0, ub=5.0)
    # open_D = 0 ⟹ SUM(ship over D) <= 0, Big-M = the depot's own capacity.
    oracle_solver.add_constraint({"ship_T1": 1.0, "ship_T2": 1.0, "open_D1": -10.0}, "<=", 0.0)
    oracle_solver.add_constraint({"ship_T3": 1.0, "open_D2": -5.0}, "<=", 0.0)
    oracle_solver.add_constraint({"ship_T1": 1.0, "ship_T2": 1.0, "ship_T3": 1.0}, "<=", 8.0)
    oracle_solver.set_objective(
        {"ship_T1": 6.0, "ship_T2": 4.0, "ship_T3": 9.0, "open_D1": -30.0, "open_D2": -30.0},
        ObjSense.MAXIMIZE)
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL

    by_route = {r: (bool(o), int(s)) for r, o, s in got}
    for r, (d, _) in routes.items():
        if not by_route[r][0]:
            assert by_route[r][1] == 0
    value = sum(by_route[r][1] * p for r, (_, p) in routes.items()) - 30 * len(
        {routes[r][0] for r in routes if by_route[r][0]})
    assert value == pytest.approx(result.objective_value)


@pytest.mark.var_boolean
@pytest.mark.var_integer
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_row_guard_on_a_global_reducer_generates_per_row(decidb_cli, oracle_solver):
    """`IF flag: SUM(x) <= 4` with a per-row `flag` -- no PER means one instance per
    row, each conditioning the same global sum on its own decision. Raising any flag
    caps the total, so the optimum raises none unless a flag pays."""
    sql = """
        SELECT id, flag, x FROM (VALUES (1, 3), (2, 5), (3, 2)) t(id, bonus)
        DECIDE flag(BOOL), x(INT) BETWEEN 0 AND 3
        SUCH THAT IF flag: SUM(x) <= 4
        MAXIMIZE SUM(x) + SUM(flag * bonus)
    """
    got = _solve_rows(decidb_cli, sql, "id", "flag", "x")

    oracle_solver.create_model("row_guard_global_sum")
    bonus = {1: 3, 2: 5, 3: 2}
    obj = {}
    for i in bonus:
        oracle_solver.add_variable(f"flag_{i}", VarType.BINARY)
        oracle_solver.add_variable(f"x_{i}", VarType.INTEGER, lb=0.0, ub=3.0)
        obj[f"x_{i}"] = 1.0
        obj[f"flag_{i}"] = float(bonus[i])
    for i in bonus:
        # flag_i = 1 ⟹ x_1 + x_2 + x_3 <= 4  (Big-M = 9 - 4)
        oracle_solver.add_constraint({"x_1": 1.0, "x_2": 1.0, "x_3": 1.0, f"flag_{i}": 5.0}, "<=", 9.0)
    oracle_solver.set_objective(obj, ObjSense.MAXIMIZE)
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL

    total_x = sum(int(x) for _, _, x in got)
    if any(f for _, f, _ in got):
        assert total_x <= 4
    value = total_x + sum(bonus[i] for i, f, _ in got if f)
    assert value == pytest.approx(result.objective_value)


@pytest.mark.var_boolean
@pytest.mark.var_integer
@pytest.mark.when
@pytest.mark.correctness
def test_when_filters_rows_before_the_guard_reads_them(decidb_cli):
    """`WHEN id <= 2 IF NOT open: ship <= 0` -- row 3 is not generated at all, so it
    ships without opening; rows 1 and 2 must open first."""
    got = _solve_rows(decidb_cli, f"""
        SELECT id, open, ship FROM {_ROWS}
        DECIDE open(BOOL), ship(INT) BETWEEN 0 AND cap
        SUCH THAT WHEN id <= 2 IF NOT open: ship <= 0
        MAXIMIZE SUM(ship * profit) - SUM(open * open_cost)
    """, "id", "open", "ship")
    assert got == [(1, True, 10), (2, False, 0), (3, False, 6)]


@pytest.mark.edge_case
@pytest.mark.correctness
def test_guard_that_always_or_never_holds(decidb_cli):
    """`IF open >= 0:` holds for every assignment and `IF open > 1:` for none: the
    first imposes its row unconditionally, the second drops it."""
    always = _solve_rows(decidb_cli, f"""
        SELECT id, ship FROM {_ROWS}
        DECIDE open(BOOL), ship(INT) BETWEEN 0 AND cap
        SUCH THAT IF open >= 0: ship <= 2
        MAXIMIZE SUM(ship)
    """, "id", "ship")
    assert always == [(1, 2), (2, 2), (3, 2)]
    never = _solve_rows(decidb_cli, f"""
        SELECT id, ship FROM {_ROWS}
        DECIDE open(BOOL), ship(INT) BETWEEN 0 AND cap
        SUCH THAT IF open > 1: ship <= 2
        MAXIMIZE SUM(ship)
    """, "id", "ship")
    assert never == [(1, 10), (2, 8), (3, 6)]


@pytest.mark.error
@pytest.mark.error_binder
def test_guard_over_known_data_is_a_when(decidb_cli):
    decidb_cli.assert_error(f"""
        SELECT id, ship FROM {_ROWS}
        DECIDE ship(INT) BETWEEN 0 AND cap
        SUCH THAT IF cap > 6: ship <= 2
        MAXIMIZE SUM(ship)
    """, match=r"An IF guard must reference a decision.*WHEN <condition>:")


@pytest.mark.error
@pytest.mark.error_binder
def test_when_over_a_decision_is_an_if(decidb_cli):
    decidb_cli.assert_error(f"""
        SELECT id, ship FROM {_ROWS}
        DECIDE open(BOOL), ship(INT) BETWEEN 0 AND cap
        SUCH THAT WHEN open: ship <= 2
        MAXIMIZE SUM(ship)
    """, match=r"A WHEN condition filters rows before the solve, so it cannot reference a decision.*IF <condition>:")


@pytest.mark.error
@pytest.mark.error_binder
def test_equality_guard_is_rejected(decidb_cli):
    """`IF ship = 3:` has no single-row complement; the message points at the
    comparisons that do, and at a BOOL decision."""
    decidb_cli.assert_error(f"""
        SELECT id, ship FROM {_ROWS}
        DECIDE ship(INT) BETWEEN 0 AND cap, y(INT) BETWEEN 0 AND 5
        SUCH THAT IF ship = 3: y <= 1
        MAXIMIZE SUM(ship) + SUM(y)
    """, match=r"An IF guard compares with <=, <, >= or >")


@pytest.mark.error
def test_real_comparison_guard_is_rejected(decidb_cli):
    """A REAL decision has no integer step, so `NOT (x >= 1.5)` is not a row."""
    decidb_cli.assert_error(f"""
        SELECT id, x FROM {_ROWS}
        DECIDE x(REAL) BETWEEN 0 AND 10, y(INT) BETWEEN 0 AND 5
        SUCH THAT IF x >= 1.5: y <= 1
        MAXIMIZE SUM(x) + SUM(y)
    """, match=r"IF x >= 1.5 compares an expression that is not integer-valued")


@pytest.mark.error
@pytest.mark.error_binder
def test_reducer_guard_is_rejected(decidb_cli):
    decidb_cli.assert_error(f"""
        SELECT id, ship FROM {_ROWS}
        DECIDE ship(INT) BETWEEN 0 AND cap
        SUCH THAT IF SUM(ship) >= 5: ship <= 2
        MAXIMIZE SUM(ship)
    """, match=r"An IF guard reads the instance's own decisions; a reducer")


@pytest.mark.error
@pytest.mark.error_binder
@pytest.mark.per_clause
def test_guard_must_be_determined_by_the_generation_key(decidb_cli):
    """Under `PER d`, a per-row `open` is not one value per depot."""
    decidb_cli.assert_error("""
        SELECT routeID, ship
        FROM (VALUES ('T1', 'D1'), ('T2', 'D1'), ('T3', 'D2')) t(routeID, d)
        DECIDE open(BOOL), ship(INT) BETWEEN 0 AND 5
        SUCH THAT PER d IF NOT open: SUM(ship) BY (d) <= 0
        MAXIMIZE SUM(ship)
    """, match=r"decision 'open'.*PER d")


@pytest.mark.error
def test_guard_on_a_minmax_body_is_not_implemented_yet(decidb_cli):
    decidb_cli.assert_error(f"""
        SELECT id, ship FROM {_ROWS}
        DECIDE open(BOOL), ship(INT) BETWEEN 0 AND cap
        SUCH THAT IF open: MAX(ship) >= 5
        MAXIMIZE SUM(ship) - SUM(open)
    """, match=r"an IF guard is supported on linear constraints only")


@pytest.mark.correctness
def test_explain_renders_the_guard_prefix(decidb_cli):
    result = decidb_cli.execute_raw(f"""
        EXPLAIN SELECT id, ship FROM {_ROWS}
        DECIDE open(BOOL), ship(INT) BETWEEN 0 AND cap
        SUCH THAT WHEN id <= 2 IF NOT open: ship <= 0
        MAXIMIZE SUM(ship * profit) - SUM(open * open_cost)
    """)
    # The plan is boxed and wrapped, so compare with every box character and space
    # removed: the prefix renders as the user wrote it, casts and all noise stripped.
    text = result.stdout + result.stderr
    compact = "".join(ch for ch in text if ch not in "│┌┐└┘─┬┴ \n")
    assert "WHENid<=2IFNOTopen:ship<=0" in compact, text
