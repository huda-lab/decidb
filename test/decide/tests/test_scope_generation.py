"""Constraint scope: `per K:` and `when c:` written in front of a constraint
(syntax_reference.md §6).

`per K:` makes one constraint per class of K among the kept rows (`per ()`: one; no
`per`, or `per row`: one per kept row) and does not group reducers. `when c:` keeps the
rows where c is true -- NULL is false -- before anything else: classes form from kept
rows only, every reducer of the clause reads kept rows only, and nothing is computed on
a row the `when` drops, so a NULL there, or an expression that would fail there, is no
error. A `when` that keeps no row of a non-empty input is an error (D4); NULL is a class
of `per` like any other value (D5).

Model size is read from DECIDB_DUMP_MODEL; optima are judged against
`_scope_oracle.ScopedModel`, which follows the ANR definition literally.
"""

import math

import pytest

from ._diagnose_relation import rows as diagnose_rows
from ._diagnose_relation import run as run_diagnose
from ._output_helpers import assert_no_internal_leak
from ._scope_cases import (DEPOTS, SHIPMENTS, aligned, answer, dump_counts, from_clause, joined,  # noqa: F401
                           scope_solver, table, table_rows)
from ._scope_oracle import CONSTANT, Decision, Reduce, Scoped, ScopedModel, ScopedObjective
from solver.types import ObjSense

SHIP = [Decision("ship")]
RESERVE = [Decision("reserve", ("depotID",))]

SHIP_BOUND = Scoped((lambda r: {"ship": 1},), "<=", (lambda r: {CONSTANT: r["demand"]},))
MAXIMIZE_SHIP = ScopedObjective(ObjSense.MAXIMIZE, (Reduce("sum", lambda r: {"ship": 1}),))
MAXIMIZE_RESERVE = ScopedObjective(ObjSense.MAXIMIZE, (Reduce("sum", lambda r: {"reserve": 1}),))


def _ship_query(clause: str, shipments=SHIPMENTS, depots=DEPOTS) -> str:
    return f"""
        SELECT shipmentID, ship {from_clause(shipments, depots)}
        DECIDE ship(INT)
        SUCH THAT ship <= S.demand AND {clause}
        MAXIMIZE SUM(ship)
        ORDER BY shipmentID"""


def _check_optimum(cli, solver, sql, model, clauses, objective):
    result = aligned(answer(cli, sql), model)
    assert model.judge_scoped(result, clauses, objective) == pytest.approx(
        model.solve_scoped(solver, clauses, objective))
    return result


# ---------------------------------------------------------------------------
# per K: how many constraints
# ---------------------------------------------------------------------------

@pytest.mark.correctness
def test_per_key_makes_one_constraint_per_class(decidb_cli, scope_solver, tmp_path):
    """`per D.depotID:` over a decision keyed the same way: one row per depot. Without the
    `per`, the same clause is one row per shipment and the same optimum."""
    def query(prefix):
        return f"""
            SELECT shipmentID, reserve {from_clause()}
            DECIDE per D.depotID: reserve(INT)
            SUCH THAT {prefix}reserve <= D.capacity - 5
            MAXIMIZE SUM(reserve)
            ORDER BY shipmentID"""
    assert dump_counts(decidb_cli.dump_model(query("per D.depotID: "), tmp_path / "per.dump"))["num_rows"] == 3
    assert dump_counts(decidb_cli.dump_model(query(""), tmp_path / "row.dump"))["num_rows"] == 5

    model = ScopedModel(joined(), RESERVE)
    clauses = [Scoped((lambda r: {"reserve": 1},), "<=", (lambda r: {CONSTANT: r["capacity"] - 5},),
                      per=("depotID",))]
    _check_optimum(decidb_cli, scope_solver, query("per D.depotID: "), model, clauses, MAXIMIZE_RESERVE)


@pytest.mark.correctness
def test_per_empty_is_one_constraint(decidb_cli, tmp_path):
    """`per ():` is one constraint: the same model as the reducer clause without it."""
    scoped = decidb_cli.dump_model(_ship_query("per (): sum(ship) <= 50"), tmp_path / "scoped.dump")
    assert scoped == decidb_cli.dump_model(_ship_query("sum(ship) <= 50"), tmp_path / "plain.dump")
    assert dump_counts(scoped)["num_rows"] == len(SHIPMENTS) + 1


@pytest.mark.correctness
def test_per_row_is_the_default_written_out(decidb_cli, tmp_path):
    scoped = decidb_cli.dump_model(_ship_query("per row: ship <= S.demand - 1"), tmp_path / "scoped.dump")
    assert scoped == decidb_cli.dump_model(_ship_query("ship <= S.demand - 1"), tmp_path / "plain.dump")


@pytest.mark.correctness
def test_per_row_adds_nothing_to_a_scoped_clause(decidb_cli, tmp_path):
    """Beside another spelling, `per row` -- in front or inside the reducer -- changes
    nothing: the same model."""
    plain = decidb_cli.dump_model(_ship_query("sum(ship) by (D.depotID) <= D.capacity"), tmp_path / "plain.dump")
    assert plain == decidb_cli.dump_model(_ship_query("per row: sum(per row: ship) by (D.depotID) <= D.capacity"),
                                          tmp_path / "row.dump")


@pytest.mark.correctness
@pytest.mark.parametrize("clause", ["per row: max(ship) > 11", "max(per row: ship) > 11"], ids=["in-front", "inside"])
def test_per_row_written_follows_the_new_rules(decidb_cli, scope_solver, clause):
    """`per row` spells the default, so its clause follows the rules of a bare one (D19):
    the strict bound on a hard MAX steps to the next whole number, and MINIMIZE puts one
    shipment at 12. In front of the clause and inside the reducer alike."""
    sql = f"""
        SELECT shipmentID, ship {from_clause()}
        DECIDE ship(INT)
        SUCH THAT ship <= S.demand AND {clause}
        MINIMIZE SUM(ship)
        ORDER BY shipmentID"""
    model = ScopedModel(joined(), SHIP)
    clauses = [SHIP_BOUND, Scoped((Reduce("max", lambda r: {"ship": 1}),), ">", (lambda r: {CONSTANT: 11},))]
    objective = ScopedObjective(ObjSense.MINIMIZE, (Reduce("sum", lambda r: {"ship": 1}),))
    result = _check_optimum(decidb_cli, scope_solver, sql, model, clauses, objective)
    assert max(row["ship"] for row in result) == 12


@pytest.mark.correctness
@pytest.mark.parametrize("clause, objective, value", [
    pytest.param("per row: x > 2.5", "MINIMIZE", 3, id="greater"),
    pytest.param("per row: x < 2.5", "MAXIMIZE", 2, id="less"),
    pytest.param("per row: x > -2.5 AND per row: x >= -10", "MINIMIZE", -2, id="negative"),
])
def test_a_fractional_strict_bound_steps_onto_the_lattice(decidb_cli, clause, objective, value):
    """`x > 2.5` on a whole-number decision is `x >= 3`, as every row states it. Folded into
    the column box, the bound steps the same way."""
    rows = answer(decidb_cli, f"""
        SELECT id, x {table('t', ('id',), ((1,), (2,)))}
        DECIDE x(INT)
        SUCH THAT x <= 10 AND {clause}
        {objective} SUM(x)""")
    assert [row["x"] for row in rows] == [value, value], rows


@pytest.mark.correctness
def test_unused_per_is_accepted(decidb_cli, tmp_path):
    """`per D.region: sum(ship) <= 50` bounds the total over all rows, once per region;
    the identical constraints are one constraint (D6)."""
    scoped = decidb_cli.dump_model(_ship_query("per D.region: sum(ship) <= 50"), tmp_path / "scoped.dump")
    assert scoped == decidb_cli.dump_model(_ship_query("sum(ship) <= 50"), tmp_path / "plain.dump")


@pytest.mark.correctness
def test_a_scope_covers_one_constraint(decidb_cli, scope_solver, tmp_path):
    """In `per K: A AND B` only A is scoped: B starts a new constraint, one per row."""
    sql = f"""
        SELECT shipmentID, reserve {from_clause()}
        DECIDE per D.depotID: reserve(INT)
        SUCH THAT per D.depotID: reserve <= D.capacity AND reserve <= S.demand
        MAXIMIZE SUM(reserve)
        ORDER BY shipmentID"""
    assert dump_counts(decidb_cli.dump_model(sql, tmp_path / "model.dump"))["num_rows"] == 3 + 5
    model = ScopedModel(joined(), RESERVE)
    clauses = [Scoped((lambda r: {"reserve": 1},), "<=", (lambda r: {CONSTANT: r["capacity"]},), per=("depotID",)),
               Scoped((lambda r: {"reserve": 1},), "<=", (lambda r: {CONSTANT: r["demand"]},))]
    _check_optimum(decidb_cli, scope_solver, sql, model, clauses, MAXIMIZE_RESERVE)


@pytest.mark.correctness
def test_a_scope_covers_both_halves_of_a_between(decidb_cli, scope_solver, tmp_path):
    sql = f"""
        SELECT shipmentID, reserve {from_clause()}
        DECIDE per D.depotID: reserve(INT)
        SUCH THAT per D.depotID: reserve BETWEEN D.capacity - 10 AND D.capacity
        MINIMIZE SUM(reserve)
        ORDER BY shipmentID"""
    assert dump_counts(decidb_cli.dump_model(sql, tmp_path / "model.dump"))["num_rows"] == 2 * 3
    model = ScopedModel(joined(), RESERVE)
    clauses = [Scoped((lambda r: {"reserve": 1},), ">=", (lambda r: {CONSTANT: r["capacity"] - 10},),
                      per=("depotID",)),
               Scoped((lambda r: {"reserve": 1},), "<=", (lambda r: {CONSTANT: r["capacity"]},), per=("depotID",))]
    _check_optimum(decidb_cli, scope_solver, sql, model, clauses,
                   ScopedObjective(ObjSense.MINIMIZE, (Reduce("sum", lambda r: {"reserve": 1}),)))


@pytest.mark.correctness
def test_null_is_a_class_of_per(decidb_cli, scope_solver, tmp_path):
    """D3's region is NULL: its rows form a class of `per D.region` and get a constraint."""
    depots = DEPOTS[:2] + (("D3", None, "US", 20, 30, 0.3, 0.7, 60),)
    sql = _ship_query("per D.region: sum(ship) by (D.region) <= 12", depots=depots)
    assert dump_counts(decidb_cli.dump_model(sql, tmp_path / "model.dump"))["num_rows"] == len(SHIPMENTS) + 2
    model = ScopedModel(joined(depots=depots), SHIP)
    clauses = [SHIP_BOUND, Scoped((Reduce("sum", lambda r: {"ship": 1}, by=("region",)),), "<=",
                                  (lambda r: {CONSTANT: 12},), per=("region",))]
    result = _check_optimum(decidb_cli, scope_solver, sql, model, clauses, MAXIMIZE_SHIP)
    assert result[4]["ship"] == 12


# ---------------------------------------------------------------------------
# when c: which rows
# ---------------------------------------------------------------------------

@pytest.mark.correctness
def test_classes_form_from_kept_rows_only(decidb_cli, scope_solver, tmp_path):
    """Only D2 has a shipment on hold: one class, one constraint, no error for the others."""
    sql = _ship_query("when S.status = 'hold' per D.depotID: sum(ship) by (D.depotID) <= 3")
    assert dump_counts(decidb_cli.dump_model(sql, tmp_path / "model.dump"))["num_rows"] == len(SHIPMENTS) + 1
    model = ScopedModel(joined(), SHIP)
    clauses = [SHIP_BOUND, Scoped((Reduce("sum", lambda r: {"ship": 1}, by=("depotID",)),), "<=",
                                  (lambda r: {CONSTANT: 3},), when=lambda r: r["status"] == "hold",
                                  per=("depotID",))]
    result = _check_optimum(decidb_cli, scope_solver, sql, model, clauses, MAXIMIZE_SHIP)
    assert result[3]["ship"] == 3


@pytest.mark.correctness
def test_a_reducer_reads_the_kept_rows_only(decidb_cli, scope_solver):
    """`sum(S.demand)` under `when S.priority:` totals the priority shipments (65)."""
    sql = _ship_query("when S.priority: ship <= 0.5 * sum(S.demand)")
    model = ScopedModel(joined(), SHIP)
    clauses = [SHIP_BOUND, Scoped((lambda r: {"ship": 1},), "<=",
                                  (Reduce("sum", lambda r: {CONSTANT: r["demand"]}, factor=lambda r: 0.5),),
                                  when=lambda r: r["priority"])]
    result = _check_optimum(decidb_cli, scope_solver, sql, model, clauses, MAXIMIZE_SHIP)
    assert [row["ship"] for row in result] == [10, 20, 30, 15, 25]
    sql = _ship_query("when S.priority: ship <= 0.25 * sum(S.demand)")
    clauses[1] = Scoped((lambda r: {"ship": 1},), "<=",
                        (Reduce("sum", lambda r: {CONSTANT: r["demand"]}, factor=lambda r: 0.25),),
                        when=lambda r: r["priority"])
    result = _check_optimum(decidb_cli, scope_solver, sql, model, clauses, MAXIMIZE_SHIP)
    assert [row["ship"] for row in result] == [10, 20, 16, 15, 16]


@pytest.mark.correctness
def test_null_condition_drops_the_row(decidb_cli, scope_solver):
    shipments = (SHIPMENTS[0], (2, "D1", 20, None, 1, 3, "active")) + SHIPMENTS[2:]
    sql = _ship_query("when S.priority: ship <= 1", shipments)
    model = ScopedModel(joined(shipments), SHIP)
    clauses = [SHIP_BOUND, Scoped((lambda r: {"ship": 1},), "<=", (lambda r: {CONSTANT: 1},),
                                  when=lambda r: r["priority"])]
    result = _check_optimum(decidb_cli, scope_solver, sql, model, clauses, MAXIMIZE_SHIP)
    assert [row["ship"] for row in result] == [1, 20, 1, 15, 1]


@pytest.mark.correctness
def test_compound_condition_without_parentheses(decidb_cli, scope_solver):
    """`and` before `per` belongs to the condition."""
    sql = _ship_query("when S.priority and S.demand > 12 per D.depotID: sum(ship) by (D.depotID) <= 9")
    model = ScopedModel(joined(), SHIP)
    clauses = [SHIP_BOUND, Scoped((Reduce("sum", lambda r: {"ship": 1}, by=("depotID",)),), "<=",
                                  (lambda r: {CONSTANT: 9},),
                                  when=lambda r: r["priority"] and r["demand"] > 12, per=("depotID",))]
    result = _check_optimum(decidb_cli, scope_solver, sql, model, clauses, MAXIMIZE_SHIP)
    assert [row["ship"] for row in result] == [10, 20, 9, 15, 9]


@pytest.mark.correctness
def test_a_subquery_in_the_condition(decidb_cli):
    """The condition may hold a subquery; a BETWEEN binds it once per half."""
    rows, _ = decidb_cli.execute(_ship_query("when S.demand > (SELECT 12): ship BETWEEN 2 AND 4"))
    assert [ship for _, ship in rows] == [10, 4, 4, 4, 4]


TARGET_COLUMNS = ("id", "target")
TARGETS = ((1, 3), (2, None), (3, 5), (4, None))


@pytest.mark.correctness
@pytest.mark.parametrize("clause, expected", [
    pytest.param("ship <= t.target", [3, 9, 5, 9], id="bound"),
    pytest.param("ship BETWEEN t.target AND t.target + 1", [4, 9, 6, 9], id="between"),
    pytest.param("abs(ship - t.target) <= 1", [4, 9, 6, 9], id="abs"),
    pytest.param("ship IN (t.target, 0)", [3, 9, 5, 9], id="in"),
    pytest.param("norm(ship - t.target, 'inf') <= 1", [4, 9, 6, 9], id="norm-inf"),
])
def test_rows_a_when_drops_are_never_read(decidb_cli, clause, expected):
    """`when t.target is not null:` drops the NULL rows, so their NULL is no error -- in
    the clause itself and in every row a rewrite adds for it."""
    rows, _ = decidb_cli.execute(f"""
        SELECT id, ship {table('t', TARGET_COLUMNS, TARGETS)}
        DECIDE ship(INT)
        SUCH THAT ship <= 9 AND when t.target is not null: {clause}
        MAXIMIZE SUM(ship)
        ORDER BY id""")
    assert [ship for _, ship in rows] == expected


@pytest.mark.correctness
def test_a_norm_reads_only_the_rows_its_when_keeps(decidb_cli):
    """`norm(e, 1)` sums |e| over the kept rows: 1 to share between targets 3 and 5."""
    rows, _ = decidb_cli.execute(f"""
        SELECT id, ship {table('t', TARGET_COLUMNS, TARGETS)}
        DECIDE ship(INT)
        SUCH THAT ship <= 9 AND when t.target is not null: norm(ship - t.target, 1) <= 1
        MAXIMIZE SUM(ship)
        ORDER BY id""")
    ships = [ship for _, ship in rows]
    assert ships[0] + ships[2] == 9 and abs(ships[0] - 3) + abs(ships[2] - 5) == 1
    assert ships[1] == ships[3] == 9


GUARD_COLUMNS = ("id", "s", "v", "grp")
#: Row 2's text is no number: CAST(s AS INTEGER) fails on it, and only on it.
GUARD_ROWS = ((1, "5", 10, 1), (2, "abc", 20, 1), (3, "5", 30, 1), (4, "7", 15, 2))
NUMERIC = lambda r: r["s"] != "abc"  # noqa: E731
X_BOUND = Scoped((lambda r: {"x": 1},), "<=", (lambda r: {CONSTANT: r["v"]},))


def _x_sum(when=None, by=()):
    return Reduce("sum", lambda r: {"x": 1}, when=when, by=by)


@pytest.mark.correctness
@pytest.mark.parametrize("clause, objective, oracle_clause, oracle_objective", [
    pytest.param("when t.s <> 'abc': x <= CAST(t.s AS INTEGER)", "MAXIMIZE SUM(x)",
                 Scoped((lambda r: {"x": 1},), "<=", (lambda r: {CONSTANT: int(r["s"])},), when=NUMERIC),
                 ScopedObjective(ObjSense.MAXIMIZE, (_x_sum(),)), id="bound"),
    pytest.param("sum(when t.s <> 'abc': x * CAST(t.s AS INTEGER)) <= 30", "MAXIMIZE SUM(x)",
                 Scoped((Reduce("sum", lambda r: {"x": int(r["s"])}, when=NUMERIC),), "<=",
                        (lambda r: {CONSTANT: 30},)),
                 ScopedObjective(ObjSense.MAXIMIZE, (_x_sum(),)), id="coefficient-under-an-inner-when"),
    pytest.param("when t.s <> 'abc': sum(x) by (CAST(t.s AS INTEGER)) <= 3", "MAXIMIZE SUM(x)",
                 Scoped((_x_sum(by=lambda r: (int(r["s"]),)),), "<=", (lambda r: {CONSTANT: 3},), when=NUMERIC),
                 ScopedObjective(ObjSense.MAXIMIZE, (_x_sum(),)), id="by-key"),
    pytest.param("when t.s <> 'abc': sum(when CAST(t.s AS INTEGER) > 5: x) <= 4", "MAXIMIZE SUM(x)",
                 Scoped((_x_sum(when=lambda r: int(r["s"]) > 5),), "<=", (lambda r: {CONSTANT: 4},), when=NUMERIC),
                 ScopedObjective(ObjSense.MAXIMIZE, (_x_sum(),)), id="inner-when"),
    pytest.param("when t.s <> 'abc': x <= max(CAST(t.s AS INTEGER)) by (t.grp)", "MAXIMIZE SUM(x)",
                 Scoped((lambda r: {"x": 1},), "<=",
                        (Reduce("max", lambda r: {CONSTANT: int(r["s"])}, by=("grp",)),), when=NUMERIC),
                 ScopedObjective(ObjSense.MAXIMIZE, (_x_sum(),)), id="bound-side-reducer"),
    pytest.param("when t.s <> 'abc' per t.grp: sum(x) by (t.grp) <= CAST(t.s AS INTEGER)", "MAXIMIZE SUM(x)",
                 Scoped((_x_sum(by=("grp",)),), "<=", (lambda r: {CONSTANT: int(r["s"])},), when=NUMERIC,
                        per=("grp",)),
                 ScopedObjective(ObjSense.MAXIMIZE, (_x_sum(),)), id="value-a-scope-check-compares"),
    pytest.param("when t.s <> 'abc' and CAST(t.s AS INTEGER) > 5: x <= 1", "MAXIMIZE SUM(x)",
                 Scoped((lambda r: {"x": 1},), "<=", (lambda r: {CONSTANT: 1},),
                        when=lambda r: r["s"] != "abc" and int(r["s"]) > 5),
                 ScopedObjective(ObjSense.MAXIMIZE, (_x_sum(),)), id="and-read-left-to-right"),
    pytest.param("when t.id = 2 or (t.s <> 'abc' and CAST(t.s AS INTEGER) > 5): x <= 1", "MAXIMIZE SUM(x)",
                 Scoped((lambda r: {"x": 1},), "<=", (lambda r: {CONSTANT: 1},),
                        when=lambda r: r["id"] == 2 or (r["s"] != "abc" and int(r["s"]) > 5)),
                 ScopedObjective(ObjSense.MAXIMIZE, (_x_sum(),)), id="or-read-left-to-right"),
    pytest.param("when t.s <> 'abc': sum(x * CAST(t.s AS INTEGER)) + max(x) <= 60", "MAXIMIZE SUM(x)",
                 Scoped((Reduce("sum", lambda r: {"x": int(r["s"])}), Reduce("max", lambda r: {"x": 1})), "<=",
                        (lambda r: {CONSTANT: 60},), when=NUMERIC),
                 ScopedObjective(ObjSense.MAXIMIZE, (_x_sum(),)), id="composed"),
    pytest.param("when t.s <> 'abc': max(x * CAST(t.s AS INTEGER)) by (t.grp) >= 100", "MINIMIZE SUM(x)",
                 Scoped((Reduce("max", lambda r: {"x": int(r["s"])}, by=("grp",)),), ">=",
                        (lambda r: {CONSTANT: 100},), when=NUMERIC),
                 ScopedObjective(ObjSense.MINIMIZE, (_x_sum(),)), id="hard-max"),
    pytest.param("per (): sum(x) <= 12", "MAXIMIZE when t.s <> 'abc': sum(x * CAST(t.s AS INTEGER))",
                 Scoped((_x_sum(),), "<=", (lambda r: {CONSTANT: 12},), per=()),
                 ScopedObjective(ObjSense.MAXIMIZE, (Reduce("sum", lambda r: {"x": int(r["s"])}),), when=NUMERIC),
                 id="objective"),
])
def test_nothing_is_computed_on_a_row_the_when_drops(decidb_cli, scope_solver, clause, objective, oracle_clause,
                                                       oracle_objective):
    """`when` filters before anything else (§6.1): an expression that would fail on a row
    the `when` drops -- a cast of 'abc' -- is never computed there, as in SQL's WHERE."""
    sql = f"""
        SELECT id, x {table('t', GUARD_COLUMNS, GUARD_ROWS)}
        DECIDE x(INT)
        SUCH THAT x <= t.v AND {clause}
        {objective}
        ORDER BY id"""
    model = ScopedModel(table_rows(GUARD_COLUMNS, GUARD_ROWS), [Decision("x")])
    clauses = [X_BOUND, oracle_clause]
    result = aligned(answer(decidb_cli, sql), model, "id")
    assert model.judge_scoped(result, clauses, oracle_objective) == pytest.approx(
        model.solve_scoped(scope_solver, clauses, oracle_objective))


@pytest.mark.correctness
def test_a_when_guards_a_function_that_fails_on_its_dropped_rows(decidb_cli):
    """`ln(0)` is an error: the `when` drops the row where w is 0, so it is never taken."""
    columns = ("id", "w", "v")
    rows = ((1, 0.0, 10), (2, 4.0, 20), (3, 9.0, 30))
    rows_out, _ = decidb_cli.execute(f"""
        SELECT id, x {table('t', columns, rows)}
        DECIDE x(INT)
        SUCH THAT x <= t.v AND when t.w > 0: x <= ln(t.w) * 10
        MAXIMIZE SUM(x)
        ORDER BY id""")
    assert [x for _, x in rows_out] == [10, int(math.log(4.0) * 10), int(math.log(9.0) * 10)]


# ---------------------------------------------------------------------------
# Errors
# ---------------------------------------------------------------------------

@pytest.mark.error
@pytest.mark.parametrize("clause, objective", [
    pytest.param("when S.status = 'lost': ship <= 1", "MAXIMIZE SUM(ship)", id="rows-clause"),
    pytest.param("when S.status = 'lost' per D.depotID: sum(ship) by (D.depotID) <= 1", "MAXIMIZE SUM(ship)",
                 id="aggregate-clause"),
    pytest.param("ship <= 1", "MAXIMIZE when S.status = 'lost': sum(ship)", id="objective"),
])
def test_a_when_that_keeps_no_row_is_an_error(decidb_cli, clause, objective):
    """D4, with or without a reducer in the clause."""
    result = decidb_cli.execute_raw(f"""
        SELECT shipmentID, ship {from_clause()}
        DECIDE ship(INT)
        SUCH THAT ship <= S.demand AND {clause}
        {objective}""")
    assert "DECIDE: when S.status = 'lost' matches no rows." in result.stderr, result.stderr
    assert_no_internal_leak(result, "a when that keeps no row")


@pytest.mark.correctness
def test_an_input_with_no_rows_is_not_an_error(decidb_cli):
    """S9: the result is empty, as today."""
    rows, _ = decidb_cli.execute(f"""
        SELECT shipmentID, ship {from_clause()} WHERE S.demand > 1000
        DECIDE ship(INT)
        SUCH THAT when S.status = 'lost': ship <= 1 AND per D.depotID: sum(ship) by (D.depotID) <= 1
        MAXIMIZE when S.status = 'lost': sum(ship)""")
    assert rows == []


@pytest.mark.error
@pytest.mark.error_binder
@pytest.mark.parametrize("clause, message", [
    pytest.param("when ship > 1: ship <= 5", "DECIDE: ship is a decision; when, per and by may only use data",
                 id="decision-in-when"),
    pytest.param("per (): sum(ship) by (ship) <= 5", "DECIDE: ship is a decision; when, per and by may only use data",
                 id="decision-in-by"),
    pytest.param("when S.priority: o", "'o' is not a valid SUCH THAT constraint on its own; compare it with a value",
                 id="bare-decision"),
    pytest.param("when row_number() over () > 1: ship <= 1",
                 "DECIDE: a window function such as 'row_number() OVER ()' is not supported in when, per or by",
                 id="window-function-in-when"),
    pytest.param("sum(ship) by (row_number() over ()) <= 1",
                 "DECIDE: a window function such as 'row_number() OVER ()' is not supported in when, per or by",
                 id="window-function-in-by"),
    pytest.param("when S.priority: sum() <= 3", "SUM needs one argument, found 'sum()'", id="reducer-without-argument"),
    pytest.param("when S.priority: norm() <= 3",
                 "NORM needs an expression and an order, as in norm(e, 1), found 'norm()'", id="norm-without-argument"),
])
def test_scope_errors(decidb_cli, clause, message):
    result = decidb_cli.execute_raw(f"""
        SELECT shipmentID, ship {from_clause()}
        DECIDE ship(INT), per D: o(BOOL)
        SUCH THAT ship <= S.demand AND {clause}
        MAXIMIZE SUM(ship)""")
    assert message in result.stderr, result.stderr
    assert_no_internal_leak(result, "a scope error")


@pytest.mark.error
def test_a_null_subquery_is_named_by_its_text(decidb_cli):
    """West has no row in the subquery's table, so its bound is NULL: the message quotes
    the subquery as DuckDB prints it, never the column it was flattened into."""
    result = decidb_cli.execute_raw(_ship_query(
        "per D.region: sum(ship) by (D.region) <= (SELECT max(c.cap) FROM (VALUES ('East', 30)) c(region, cap) "
        "WHERE c.region = D.region)"))
    assert "DECIDE: the subquery (SELECT max(c.cap) FROM " in result.stderr, result.stderr
    assert "WHERE (c.region = D.region)) is NULL. Impute it with COALESCE((SELECT max(c.cap) FROM " in result.stderr, (
        result.stderr)
    assert_no_internal_leak(result, "a NULL subquery")


# ---------------------------------------------------------------------------
# EXPLAIN and the serialized plan
# ---------------------------------------------------------------------------

@pytest.mark.explain
def test_explain_echoes_the_scope_as_written(decidb_cli):
    result = decidb_cli.execute_raw(f"""
        EXPLAIN SELECT shipmentID, ship {from_clause()}
        DECIDE ship(INT), per D.depotID: reserve(INT)
        SUCH THAT per D.depotID: reserve <= D.capacity
          AND when S.priority per D.depotID: sum(ship) by (D.depotID) <= D.priorityCapacity
          AND per (): sum(ship) <= 50
        MAXIMIZE when S.priority: sum(ship)""")
    plan = "".join(line.strip("│ ") for line in result.stdout.splitlines())
    for text in ("per D.depotID: reserve <= capacity",
                 "when priority per D.depotID: sum(ship) by (D.depotID) <= priorityCapacity",
                 "per (): sum(ship) <= 50",
                 "MAXIMIZE when priority: sum(ship)"):
        assert text.replace(" ", "") in plan.replace(" ", ""), f"{text!r} not in:\n{result.stdout}"
    assert_no_internal_leak(result, "EXPLAIN of the scope spellings")


#: Two columns whose names end in a colon, as a `when c:` qualifier does.
COLON_SOURCE = """(VALUES (1, 5, true, 'a'), (2, 4, false, 'b')) t(id, cap, "ok:", "g:")"""


@pytest.mark.explain
def test_a_key_or_condition_ending_in_a_colon_is_quoted_in_the_echo(decidb_cli):
    """A name that ends in a colon is a quoted identifier inside a scope, and EXPLAIN and
    DIAGNOSE keep the quotes, so the echo reads back as the scope it is."""
    result = decidb_cli.execute_raw(f"""
        EXPLAIN SELECT id, x FROM {COLON_SOURCE}
        DECIDE x(INT)
        SUCH THAT x <= cap AND when "ok:": sum(x) <= 7 AND per "g:": sum(x) by ("g:") <= 8
        MAXIMIZE SUM(x)""")
    plan = "".join(line.strip("│ ") for line in result.stdout.splitlines()).replace(" ", "")
    for text in ('when "ok:": sum(x) <= 7', 'per "g:": sum(x) by ("g:") <= 8'):
        assert text.replace(" ", "") in plan, f"{text!r} not in:\n{result.stdout}"

    for clause, suggested in (('when "ok:": x >= 9', 'when "ok:": x >= 5'),
                              ('per "g:": sum(x) by ("g:") >= 30', 'per "g:": sum(x) by ("g:") >= 4')):
        findings = diagnose_rows(run_diagnose(decidb_cli, f"""
            SELECT id, x FROM {COLON_SOURCE}
            DECIDE x(INT)
            SUCH THAT x <= cap AND {clause}
            MAXIMIZE SUM(x)"""))
        assert any(row["clause"] == clause and row["suggested_change"] == suggested for row in findings), findings


@pytest.mark.correctness
def test_scoped_plan_survives_a_serialization_round_trip(decidb_cli, tmp_path):
    """The `by` keys, the scope checks and the objective's mark are on the plan: the copy
    builds the same model, gives the same answer, and still raises the same errors."""
    sql = f"""
        SELECT shipmentID, ship, reserve {from_clause()}
        DECIDE ship(INT), per D.depotID: reserve(INT)
        SUCH THAT ship <= S.demand AND per D.depotID: reserve <= D.capacity
          AND when S.priority per D.depotID: sum(ship) by (D.depotID) <= D.priorityCapacity
          AND sum(when S.status = 'active' per D: D.capacity * reserve) by (D.region) <= 900
        MAXIMIZE when S.demand > 12: sum(ship) + sum(reserve)
        ORDER BY shipmentID"""
    copied = decidb_cli.with_verify_serializer()
    assert copied.dump_model(sql, tmp_path / "copied.dump") == decidb_cli.dump_model(sql, tmp_path / "plain.dump")
    assert copied.execute(sql) == decidb_cli.execute(sql)
    failing = sql.replace("S.status = 'active'", "S.status = 'lost'")
    message = "DECIDE: sum(when S.status = 'lost' per D: D.capacity * reserve) has no rows for D.region = 'East'."
    assert message in copied.execute_raw(failing).stderr
