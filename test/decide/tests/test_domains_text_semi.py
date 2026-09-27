"""The DeciQL domains beyond INT/REAL/BOOL (spec §4.1, §8.6).

- `x(TEXT IN ['a', 'b'])` -- a decision that takes one of a list of strings, read
  back as VARCHAR. Encoded as one hidden BOOL indicator per value with a sum-to-one
  row; `x = 'a'`, `x <> 'a'` and `x IN ('a', 'b')` are the only ways to constrain it.
- `x(SEMIREAL) BETWEEN lo AND hi` / `SEMIINT` -- `x = 0 OR lo <= x <= hi`, encoded as
  a hidden BOOL switch with `x <= hi * on` and `x >= lo * on`.

Every oracle states the same encoding explicitly.
"""

import pytest

from solver.types import ObjSense, SolverStatus, VarType

# depot (id, capacity, open_cost, repair_cost), served demand; a depot is closed,
# open, or in repair (half capacity, cheaper to run).
_DEPOTS = "(VALUES ('D1', 10, 8, 3), ('D2', 6, 4, 2), ('D3', 8, 9, 1)) d(depot, cap, open_cost, repair_cost)"


def _rows(decidb_cli, sql, *cols):
    rows, names = decidb_cli.execute(sql)
    idx = [names.index(c) for c in cols]
    return sorted(tuple(r[i] for i in idx) for r in rows)


@pytest.mark.var_boolean
@pytest.mark.var_integer
@pytest.mark.correctness
def test_text_decision_is_read_back_as_its_value(decidb_cli, oracle_solver):
    """`status(TEXT IN ['closed', 'open', 'repair'])` with guards per value."""
    # The status costs enter the objective through BOOL decisions the guards force,
    # since a TEXT decision itself has no numeric value to weigh.
    sql = f"""
        SELECT depot, status, ship FROM {_DEPOTS}
        DECIDE status(TEXT IN ['closed', 'open', 'repair']), ship(INT) BETWEEN 0 AND cap,
               opened(BOOL), repairing(BOOL)
        SUCH THAT IF status = 'closed': ship <= 0
              AND IF status = 'repair': ship <= cap / 2
              AND IF status = 'open': opened >= 1
              AND IF status = 'repair': repairing >= 1
              AND SUM(ship) >= 15
        MINIMIZE SUM(opened * open_cost) + SUM(repairing * repair_cost)
    """
    got = _rows(decidb_cli, sql, "depot", "status", "ship")

    oracle_solver.create_model("text_domain")
    data = {"D1": (10, 8, 3), "D2": (6, 4, 2), "D3": (8, 9, 1)}
    obj, total = {}, {}
    for d, (cap, open_cost, repair_cost) in data.items():
        for v in ("closed", "open", "repair"):
            oracle_solver.add_variable(f"{d}_{v}", VarType.BINARY)
        oracle_solver.add_constraint({f"{d}_closed": 1.0, f"{d}_open": 1.0, f"{d}_repair": 1.0}, "=", 1.0)
        oracle_solver.add_variable(f"ship_{d}", VarType.INTEGER, lb=0.0, ub=float(cap))
        # closed ⟹ ship <= 0 ; repair ⟹ ship <= cap/2   (Big-M = cap)
        oracle_solver.add_constraint({f"ship_{d}": 1.0, f"{d}_closed": float(cap)}, "<=", float(cap))
        oracle_solver.add_constraint({f"ship_{d}": 1.0, f"{d}_repair": float(cap) / 2}, "<=", float(cap))
        obj[f"{d}_open"] = float(open_cost)
        obj[f"{d}_repair"] = float(repair_cost)
        total[f"ship_{d}"] = 1.0
    oracle_solver.add_constraint(total, ">=", 15.0)
    oracle_solver.set_objective(obj, ObjSense.MINIMIZE)
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL

    assert {s for _, s, _ in got} <= {"closed", "open", "repair"}
    for d, status, ship in got:
        cap = data[d][0]
        assert (status != "closed" or ship == 0) and (status != "repair" or ship <= cap // 2)
    cost = sum(data[d][1] if s == "open" else data[d][2] if s == "repair" else 0 for d, s, _ in got)
    assert cost == pytest.approx(result.objective_value)
    assert sum(int(ship) for _, _, ship in got) >= 15


@pytest.mark.var_boolean
@pytest.mark.per_clause
@pytest.mark.correctness
def test_text_decision_per_key_with_in_and_not_equal(decidb_cli):
    """A keyed TEXT decision, constrained with `<>` and `IN`, read back per key."""
    got = _rows(decidb_cli, """
        SELECT route, depot, mode FROM (VALUES ('T1', 'D1'), ('T2', 'D1'), ('T3', 'D2')) t(route, depot)
        DECIDE PER depot: mode(TEXT IN ['road', 'rail', 'air']), pick(BOOL)
        SUCH THAT PER depot: mode <> 'air'
              AND mode IN ('rail', 'air')
              AND SUM(pick) >= 1
        MAXIMIZE SUM(pick)
    """, "route", "depot", "mode")
    assert got == [("T1", "D1", "rail"), ("T2", "D1", "rail"), ("T3", "D2", "rail")]


@pytest.mark.error
@pytest.mark.error_binder
def test_text_value_outside_the_domain_is_rejected(decidb_cli):
    decidb_cli.assert_error(f"""
        SELECT depot, status FROM {_DEPOTS}
        DECIDE status(TEXT IN ['closed', 'open'])
        SUCH THAT status = 'repair'
        SATISFY
    """, match=r"TEXT decision 'status' has no value 'repair'; its values are \['closed', 'open'\]")


@pytest.mark.error
@pytest.mark.error_binder
def test_text_decision_in_arithmetic_is_rejected(decidb_cli):
    decidb_cli.assert_error(f"""
        SELECT depot, status FROM {_DEPOTS}
        DECIDE status(TEXT IN ['closed', 'open']), ship(INT) BETWEEN 0 AND cap
        SUCH THAT ship + status <= 5
        MAXIMIZE SUM(ship)
    """, match=r"TEXT decision 'status' can only be compared with = or <>")


@pytest.mark.error
@pytest.mark.error_binder
def test_text_decision_ordered_comparison_is_rejected(decidb_cli):
    decidb_cli.assert_error(f"""
        SELECT depot, status FROM {_DEPOTS}
        DECIDE status(TEXT IN ['closed', 'open'])
        SUCH THAT status >= 'open'
        SATISFY
    """, match=r"TEXT decision 'status' supports = and <>")


@pytest.mark.var_integer
@pytest.mark.correctness
def test_semiint_is_zero_or_in_range(decidb_cli, oracle_solver):
    """`ship(SEMIINT) BETWEEN lot AND cap`: each row ships nothing or at least a lot."""
    sql = """
        SELECT id, ship FROM (VALUES (1, 4, 9, 5), (2, 6, 9, 3), (3, 5, 9, 9)) t(id, lot, cap, profit)
        DECIDE ship(SEMIINT) BETWEEN lot AND cap
        SUCH THAT SUM(ship) <= 12
        MAXIMIZE SUM(ship * profit)
    """
    got = _rows(decidb_cli, sql, "id", "ship")

    oracle_solver.create_model("semiint")
    data = {1: (4, 9, 5), 2: (6, 9, 3), 3: (5, 9, 9)}
    obj, total = {}, {}
    for i, (lot, cap, profit) in data.items():
        oracle_solver.add_variable(f"ship_{i}", VarType.INTEGER, lb=0.0, ub=float(cap))
        oracle_solver.add_variable(f"on_{i}", VarType.BINARY)
        oracle_solver.add_constraint({f"ship_{i}": 1.0, f"on_{i}": -float(cap)}, "<=", 0.0)
        oracle_solver.add_constraint({f"ship_{i}": 1.0, f"on_{i}": -float(lot)}, ">=", 0.0)
        obj[f"ship_{i}"] = float(profit)
        total[f"ship_{i}"] = 1.0
    oracle_solver.add_constraint(total, "<=", 12.0)
    oracle_solver.set_objective(obj, ObjSense.MAXIMIZE)
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL

    for i, ship in got:
        assert int(ship) == 0 or data[i][0] <= int(ship) <= data[i][1], got
    assert sum(int(s) * data[i][2] for i, s in got) == pytest.approx(result.objective_value)


@pytest.mark.var_real
@pytest.mark.correctness
def test_semireal_with_a_negative_floor(decidb_cli):
    """`x(SEMIREAL) BETWEEN -4 AND -1`: zero or a value in a negative range."""
    got = _rows(decidb_cli, """
        SELECT id, x FROM (VALUES (1, 1.0), (2, -1.0)) t(id, w)
        DECIDE x(SEMIREAL) BETWEEN -4 AND -1
        SUCH THAT x <= 0
        MAXIMIZE SUM(x * w)
    """, "id", "x")
    by_id = {i: float(x) for i, x in got}
    # w = 1 wants x as large as possible: 0 beats the whole negative range;
    # w = -1 wants x as small as possible: the floor of the range.
    assert by_id == {1: 0.0, 2: -4.0}


@pytest.mark.error
@pytest.mark.error_binder
def test_semi_domain_needs_both_bounds(decidb_cli):
    decidb_cli.assert_error("""
        SELECT id, x FROM (VALUES (1), (2)) t(id)
        DECIDE x(SEMIINT) <= 9
        SUCH THAT x <= 9
        MAXIMIZE SUM(x)
    """, match=r"is SEMIINT and needs both bounds")


@pytest.mark.correctness
def test_hidden_domain_decisions_stay_hidden(decidb_cli):
    """Neither `SELECT *` nor EXPLAIN shows the indicators behind a domain."""
    rows, names = decidb_cli.execute("""
        SELECT * FROM (VALUES (1), (2)) t(id)
        DECIDE mode(TEXT IN ['a', 'b']), y(SEMIINT) BETWEEN 2 AND 5
        SUCH THAT mode = 'b'
        MAXIMIZE SUM(y)
    """)
    assert names == ["id", "mode", "y"], names
    assert sorted(r[1] for r in rows) == ["b", "b"]
    result = decidb_cli.execute_raw("""
        EXPLAIN SELECT id, mode FROM (VALUES (1), (2)) t(id)
        DECIDE mode(TEXT IN ['a', 'b']), y(SEMIINT) BETWEEN 2 AND 5
        SUCH THAT mode = 'b'
        MAXIMIZE SUM(y)
    """)
    text = result.stdout + result.stderr
    compact = "".join(ch for ch in text if ch not in "│┌┐└┘─┬┴ \n")
    # The declaration list names only what was declared; the rows the domains became
    # lead with the spelling the user wrote (the rewritten row follows, as for IN).
    declared = compact.split("Variables:")[1].split("Objective:")[0]
    assert "__" not in declared, text
    assert "mode='b'" in compact and "modeIN['a','b']" in compact, text
