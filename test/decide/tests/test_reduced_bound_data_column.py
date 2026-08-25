"""C1: a bare data column as the bound of a reduced constraint, under `PER`.

`IsAllowedDecisionFreeBoundExpression` used to switch on `ExpressionClass` with no
`COLUMN_REF` case, so a bare column fell to `default: return false` and every reduced
constraint whose bound was a plain column was rejected — including both of the paper's
own running-example constraints (Figure 1, lines 8-9). Adding the missing case did not
require any change downstream: `ReduceAggregateRhsPerGroup` (physical_decide.cpp) already
took the tightest bound per `PER` group, cited by name against paper §3.2.1 before this
file existed. This was unblocking, not building.

See `test_canonicalize_side_agnostic.py::test_row_varying_bound_collapses_to_tightest_on_either_side`
for the no-PER case.
"""

import pytest

from solver.types import ObjSense, SolverStatus, VarType


@pytest.mark.per_clause
@pytest.mark.correctness
def test_column_bound_reduces_to_tightest_per_group(decidb_cli, oracle_solver):
    """`SUM(x) <= cap PER grp`: each group takes the tightest of its own rows' caps.

    Group 'a' has two rows (caps 10 and 15) so its bound is MIN(10, 15) = 10; group
    'b' has one row (cap 7) so its bound is just 7. Verified against an independently
    built model carrying the same per-row conjunction, not a hand-computed MIN.
    """
    rows, cols = decidb_cli.execute("""
        SELECT id, grp, x FROM (
            VALUES (1, 'a', 10), (2, 'a', 15), (3, 'b', 7)
        ) t(id, grp, cap)
        DECIDE x(INT)
        SUCH THAT SUM(x) <= cap PER grp
        MAXIMIZE SUM(x)
    """)
    gi, xi = cols.index("grp"), cols.index("x")
    by_group: dict[str, int] = {}
    for r in rows:
        by_group[r[gi]] = by_group.get(r[gi], 0) + int(r[xi])

    oracle_solver.create_model("column_bound_per_group")
    for i in (1, 2, 3):
        oracle_solver.add_variable(f"x_{i}", VarType.INTEGER, lb=0.0)
    # Group 'a' (rows 1, 2): the conjunction of both rows' own caps.
    oracle_solver.add_constraint({"x_1": 1.0, "x_2": 1.0}, "<=", 10.0, name="a_row1")
    oracle_solver.add_constraint({"x_1": 1.0, "x_2": 1.0}, "<=", 15.0, name="a_row2")
    # Group 'b' (row 3 alone).
    oracle_solver.add_constraint({"x_3": 1.0}, "<=", 7.0, name="b_row3")
    oracle_solver.set_objective({"x_1": 1.0, "x_2": 1.0, "x_3": 1.0}, ObjSense.MAXIMIZE)
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL

    assert by_group == {"a": 10, "b": 7}
    assert sum(by_group.values()) == pytest.approx(result.objective_value)


@pytest.mark.per_clause
@pytest.mark.cons_subquery
@pytest.mark.correctness
def test_figure1_paper_query_matches_published_output(decidb_cli, oracle_solver):
    """The paper's own running example (Figure 1), verbatim data and both constraints.

    Both `SUCH THAT` lines are a bare column bound under `PER` — line 8
    (`SUM(ship) <= stock PER depotID`) and line 9
    (`SUM(ship) >= demand WHEN priority = 'critical' PER regionID`) — and both were
    a `Binder Error` before C1. This is the query that motivated the whole batch: one
    missing `case` blocked the paper's central worked example.

    D1 (stock 800) serves routes T1 (cap 500, region R1, demand 450, critical) and T2
    (cap 350, region R2, demand n/a — not critical, so line 9 doesn't apply to it). D2
    (stock 500, opening cost 8000) serves only T3 (cap 300, region R2). Opening D2 to
    ship along T3 costs 8000 up front for at most a 900 (300 * 3) shipping saving, so
    the minimizer leaves it closed — matching the paper's published output exactly.
    """
    query = """
        WITH Depots(depotID, stock, opening_cost) AS (
            VALUES ('D1', 800, 12000), ('D2', 500, 8000)
        ),
        Routes(routeID, depotID, regionID, capacity, unit_cost) AS (
            VALUES ('T1', 'D1', 'R1', 500, 6),
                   ('T2', 'D1', 'R2', 350, 6),
                   ('T3', 'D2', 'R2', 300, 3)
        ),
        Regions(regionID, demand, priority) AS (
            VALUES ('R1', 450, 'critical'), ('R2', 600, 'standard')
        )
        SELECT routeID, depotID, regionID, open, ship
        DECIDE D.open(BOOL), T.ship(INT)
        FROM Depots D JOIN Routes T USING (depotID) JOIN Regions R USING (regionID)
        SUCH THAT
            ship BETWEEN 0 AND capacity * open AND
            SUM(ship) <= stock PER depotID AND
            SUM(ship) >= demand WHEN priority = 'critical' PER regionID
        MINIMIZE SUM(unit_cost * ship) + SUM(D: opening_cost * open)
    """
    rows, cols = decidb_cli.execute(query)
    ri, oi, si = cols.index("routeID"), cols.index("open"), cols.index("ship")
    by_route = {r[ri]: (bool(r[oi]), int(r[si])) for r in rows}

    assert by_route == {
        "T1": (True, 450),
        "T2": (True, 0),
        "T3": (False, 0),
    }

    # Independent cross-check: the same MP, built directly.
    oracle_solver.create_model("figure1_paper_query")
    oracle_solver.add_variable("open_D1", VarType.BINARY)
    oracle_solver.add_variable("open_D2", VarType.BINARY)
    oracle_solver.add_variable("ship_T1", VarType.INTEGER, lb=0.0)
    oracle_solver.add_variable("ship_T2", VarType.INTEGER, lb=0.0)
    oracle_solver.add_variable("ship_T3", VarType.INTEGER, lb=0.0)

    # ship <= capacity * open, per route.
    oracle_solver.add_constraint({"ship_T1": 1.0, "open_D1": -500.0}, "<=", 0.0, name="cap_t1")
    oracle_solver.add_constraint({"ship_T2": 1.0, "open_D1": -350.0}, "<=", 0.0, name="cap_t2")
    oracle_solver.add_constraint({"ship_T3": 1.0, "open_D2": -300.0}, "<=", 0.0, name="cap_t3")

    # SUM(ship) <= stock, per depot.
    oracle_solver.add_constraint(
        {"ship_T1": 1.0, "ship_T2": 1.0}, "<=", 800.0, name="stock_d1"
    )
    oracle_solver.add_constraint({"ship_T3": 1.0}, "<=", 500.0, name="stock_d2")

    # SUM(ship) >= demand, per region, only where priority = 'critical' (R1 only).
    oracle_solver.add_constraint({"ship_T1": 1.0}, ">=", 450.0, name="demand_r1")

    oracle_solver.set_objective(
        {
            "ship_T1": 6.0, "ship_T2": 6.0, "ship_T3": 3.0,
            "open_D1": 12000.0, "open_D2": 8000.0,
        },
        ObjSense.MINIMIZE,
    )
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL
    assert result.variable_values["ship_T1"] == pytest.approx(450.0)
    assert result.variable_values["ship_T2"] == pytest.approx(0.0)
    assert result.variable_values["ship_T3"] == pytest.approx(0.0)
    assert result.variable_values["open_D1"] == pytest.approx(1.0)
    assert result.variable_values["open_D2"] == pytest.approx(0.0)
