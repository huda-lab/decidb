"""An empty group skips its constraint (B1, deck p. 56).

A reducer over no rows is NULL, and a NULL that reaches a constraint unreduced skips that instance; a NULL objective
term is dropped. So a `when` that keeps no row yields no constraint, and a group in which some reducer reads no row
yields none. COUNT over no rows is 0, as in SQL. Every case is judged by `_scope_oracle`, which states the rule
independently; a case direct solve takes also runs under `require`.
"""

import pytest

from ._direct_oracle_common import _allow_direct_path, run  # noqa: F401
from ._scope_cases import aligned, scope_solver, table, table_rows  # noqa: F401
from ._scope_oracle import CONSTANT, GLOBAL, Decision, Reduce, Scoped, ScopedModel, ScopedObjective
from solver.types import ObjSense, VarType

COLUMNS = ("id", "g", "val", "c", "w")
ROWS = ((1, "a", 10.0, 1, 3.0), (2, "a", 7.0, 0, 2.0), (3, "b", 4.0, 0, 1.0), (4, "b", 2.0, 0, 2.0))
X = Decision("x", var_type=VarType.BINARY)
Y = Decision("y", var_type=VarType.BINARY)
X_REAL = Decision("x", var_type=VarType.CONTINUOUS)


def _x(row):
    return {"x": 1}


def _x_val(row):
    return {"x": row["val"]}


def _const(value):
    return lambda row: {CONSTANT: value}


def _empty(row):
    return row["val"] > 100


AT_MOST_ONE = Scoped((lambda row: {"x": 1},), "<=", (_const(1),))
MAXIMIZE_VAL = ScopedObjective(ObjSense.MAXIMIZE, (Reduce("sum", _x_val),))
MINIMIZE_VAL = ScopedObjective(ObjSense.MINIMIZE, (Reduce("sum", _x_val),))

# name -> (decisions, SUCH THAT ..., objective SQL, clauses, objective, direct solve takes it)
CASES = {
    # The clause's `when` keeps no row, in the aggregate shape and in the per-row shape.
    "when_keeps_no_row": (
        [X],
        "when val > 100: sum(x) >= 1",
        "MINIMIZE SUM(val * x)",
        [Scoped((Reduce("sum", _x),), ">=", (_const(1),), when=_empty, per=GLOBAL)],
        MINIMIZE_VAL,
        True,
    ),
    "when_keeps_no_row_on_a_row_bound": (
        [X],
        "sum(x) <= 1 AND when id = 99: x >= 1",
        "MAXIMIZE SUM(val * x)",
        [
            Scoped((Reduce("sum", _x),), "<=", (_const(1),), per=GLOBAL),
            Scoped((_x,), ">=", (_const(1),), when=lambda row: row["id"] == 99),
        ],
        MAXIMIZE_VAL,
        True,
    ),
    "when_keeps_no_row_under_a_bound_reducer": (
        [X],
        "when val > 100: sum(x) <= min(w)",
        "MAXIMIZE SUM(val * x)",
        [Scoped((Reduce("sum", _x),), "<=", (Reduce("min", lambda row: {CONSTANT: row["w"]}),), when=_empty)],
        MAXIMIZE_VAL,
        True,
    ),
    # A reducer's own `when` keeps no row: alone, and as one of two reducers in a `by` group (group b).
    "reducer_reads_no_row": (
        [X],
        "sum(when val > 100: x) >= 1",
        "MINIMIZE SUM(val * x)",
        [Scoped((Reduce("sum", _x, when=_empty),), ">=", (_const(1),), per=GLOBAL)],
        MINIMIZE_VAL,
        True,
    ),
    "one_reducer_of_two_reads_no_row_in_a_group": (
        [X],
        "sum(when c = 1: x) by (g) + sum(x) by (g) >= 2",
        "MINIMIZE SUM(val * x)",
        [
            Scoped(
                (Reduce("sum", _x, when=lambda row: row["c"] == 1, by=("g",)), Reduce("sum", _x, by=("g",))),
                ">=",
                (_const(2),),
            )
        ],
        MINIMIZE_VAL,
        False,
    ),
    # MIN and MAX: the hard direction, the easy one under a `when` in front, and a composed clause.
    "hard_max_reads_no_row": (
        [X],
        "max(when val > 100: x * val) >= 5",
        "MINIMIZE SUM(val * x)",
        [Scoped((Reduce("max", _x_val, when=_empty),), ">=", (_const(5),), per=GLOBAL)],
        MINIMIZE_VAL,
        False,
    ),
    "easy_min_under_a_when_that_keeps_no_row": (
        [X],
        "when val > 100: min(x * val) >= 5",
        "MINIMIZE SUM(val * x)",
        [Scoped((Reduce("min", _x_val),), ">=", (_const(5),), when=_empty, per=GLOBAL)],
        MINIMIZE_VAL,
        False,
    ),
    "composed_max_reads_no_row": (
        [X],
        "sum(x * val) + max(when val > 100: x * val) <= 5",
        "MAXIMIZE SUM(val * x)",
        [Scoped((Reduce("sum", _x_val), Reduce("max", _x_val, when=_empty)), "<=", (_const(5),), per=GLOBAL)],
        MAXIMIZE_VAL,
        False,
    ),
    # The objective: a flat MAX under a `when` that keeps no row, and a composed MIN term that reads none.
    "flat_max_objective_reads_no_row": (
        [X],
        "sum(x) >= 1",
        "MAXIMIZE when val > 100: max(x * val)",
        [Scoped((Reduce("sum", _x),), ">=", (_const(1),), per=GLOBAL)],
        ScopedObjective(ObjSense.MAXIMIZE, (Reduce("max", _x_val),), when=_empty),
        False,
    ),
    "composed_min_objective_term_reads_no_row": (
        [X],
        "sum(x) <= 2",
        "MAXIMIZE sum(x * val) + min(when val > 100: x * val)",
        [Scoped((Reduce("sum", _x),), "<=", (_const(2),), per=GLOBAL)],
        ScopedObjective(ObjSense.MAXIMIZE, (Reduce("sum", _x_val), Reduce("min", _x_val, when=_empty))),
        False,
    ),
    # A reducer beside a decision is a helper decision per `by` group; group b has no member, so no helper.
    "helper_group_with_no_member": (
        [X, Y],
        "y <= sum(when c = 1: x) by (g)",
        "MAXIMIZE SUM(val * y) - SUM(w * x)",
        [Scoped((lambda row: {"y": 1},), "<=", (Reduce("sum", _x, when=lambda row: row["c"] == 1, by=("g",)),))],
        ScopedObjective(ObjSense.MAXIMIZE, (Reduce("sum", lambda row: {"y": row["val"], "x": -row["w"]}),)),
        False,
    ),
    # Continuous rows under a budget whose `when` keeps no row, in front and inside the sum; and an objective whose
    # `when` keeps no row, under a box.
    "budget_when_keeps_no_row": (
        [X_REAL],
        "x <= 1 AND when val > 100: sum(w * x) <= 2",
        "MAXIMIZE SUM(val * x)",
        [
            Scoped((_x,), "<=", (_const(1),)),
            Scoped((Reduce("sum", lambda row: {"x": row["w"]}),), "<=", (_const(2),), when=_empty, per=GLOBAL),
        ],
        MAXIMIZE_VAL,
        True,
    ),
    "budget_reads_no_row": (
        [X_REAL],
        "x <= 1 AND sum(when val > 100: w * x) <= 2",
        "MAXIMIZE SUM(val * x)",
        [
            Scoped((_x,), "<=", (_const(1),)),
            Scoped((Reduce("sum", lambda row: {"x": row["w"]}, when=_empty),), "<=", (_const(2),), per=GLOBAL),
        ],
        MAXIMIZE_VAL,
        True,
    ),
    "objective_when_keeps_no_row": (
        [X_REAL],
        "x <= 1",
        "MAXIMIZE when val > 100: SUM(val * x)",
        [Scoped((_x,), "<=", (_const(1),))],
        ScopedObjective(ObjSense.MAXIMIZE, (Reduce("sum", _x_val),), when=_empty),
        True,
    ),
}


@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(CASES))
def test_an_empty_group_skips_its_constraint(decidb_cli, scope_solver, name):
    decisions, constraints, objective_sql, clauses, objective, direct = CASES[name]
    names = ", ".join(decision.name for decision in decisions)
    declared = ", ".join(
        f"{decision.name}({'REAL' if decision.var_type == VarType.CONTINUOUS else 'BOOL'})" for decision in decisions
    )
    sql = (
        f"SELECT id, {names} {table('t', COLUMNS, ROWS)} "
        f"DECIDE {declared} SUCH THAT {constraints} {objective_sql} ORDER BY id"
    )
    model = ScopedModel(table_rows(COLUMNS, ROWS), decisions)
    optimum = model.solve_scoped(scope_solver, clauses, objective)
    for mode in ("off", "require") if direct else ("off",):
        rows, columns = run(decidb_cli, sql, mode)
        result = aligned([dict(zip(columns, row)) for row in rows], model, "id")
        assert model.judge_scoped(result, clauses, objective) == pytest.approx(optimum), mode
