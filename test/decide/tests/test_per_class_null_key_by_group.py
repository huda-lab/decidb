"""A `per K:` clause with a `by` reducer has a row only for the groups its classes reach (D5, deck p. 30).

`per k: SUM(x) by (g) <= b` is generated once per class of `k`, and each class's sum reads the kept rows of its `g`
group. A row whose `k` is NULL is in no class, so a group whose rows all have a NULL `k` has no constraint. A row in no
class still counts in a group another row's class reaches. A clause with no `by` key is one group, all the kept
rows (D6): a composed MIN/MAX (`per k: max(x) + min(x) <= b`) has its one row only when some kept row has a class.

Each test builds the oracle model from the raw rows, as the language reads the clause: one constraint per class, over
every row of that class's group. The solver path (`off`) and the direct rules (`require`: S1 for BOOL, R1 for REAL)
must both answer as the oracle does.
"""

import math

import pytest

from solver.types import ObjSense, VarType

from ._direct_oracle_common import _allow_direct_path  # noqa: F401  (autouse fixture)
from ._direct_oracle_common import agree_with_oracle, sql_literal

TYPES = ["INTEGER", "VARCHAR", "INTEGER", "DOUBLE", "DOUBLE"]

# id, g, k, w, v. Group a: every k NULL, so no class reaches it. Group b: class 1. Group c: class 2 and row 5 in no
# class, which still counts in c's sum.
ROWS = [
    (1, "a", None, 1.0, 9.0),
    (2, "a", None, 2.0, 8.0),
    (3, "b", 1, 1.0, 7.0),
    (4, "b", 1, 1.0, 6.0),
    (5, "c", None, 1.0, 5.0),
    (6, "c", 2, 2.0, 4.0),
    (7, "c", 2, 1.0, 3.0),
]

# shape: the declaration and its box, the clause body, its bound, and the weight the clause gives a row.
SHAPES = {
    "bool": ("x(BOOL)", "", "SUM(x)", 1.0, lambda row: 1.0, VarType.BINARY),
    "real": ("x(REAL)", "x BETWEEN 0 AND 2 AND ", "SUM(w*x)", 1.5, lambda row: row[3], VarType.CONTINUOUS),
}


def _sql(rows, shape):
    declaration, box, clause, bound, _, _ = SHAPES[shape]
    values = ", ".join(
        "(" + ", ".join(sql_literal(value, sql_type) for value, sql_type in zip(row, TYPES)) + ")" for row in rows
    )
    return (
        f"SELECT id, x FROM (FROM (VALUES {values}) t(id, g, k, w, v) DECIDE {declaration} "
        f"SUCH THAT {box}per k: {clause} by (g) <= {bound} MAXIMIZE SUM(v*x)) q ORDER BY id"
    )


def _agree(cli, oracle_solver, rows, shape):
    _, _, _, bound, weight, var_type = SHAPES[shape]
    oracle_solver.create_model("per_class_null_key_by_group")
    for row in rows:
        if var_type == VarType.BINARY:
            oracle_solver.add_variable(f"x_{row[0]}", var_type)
        else:
            oracle_solver.add_variable(f"x_{row[0]}", var_type, 0.0, 2.0)
    # Each class's group: `k` determines `g`. Classes that share a group give one constraint.
    reached = {row[1] for row in rows if row[2] is not None}
    for group in sorted(reached):
        oracle_solver.add_constraint({f"x_{row[0]}": weight(row) for row in rows if row[1] == group}, "<=", bound)
    oracle_solver.set_objective({f"x_{row[0]}": row[4] for row in rows}, ObjSense.MAXIMIZE)
    oracle = oracle_solver.solve()

    def check_assignment(result, columns, mode):
        assert columns == ["id", "x"], mode
        chosen = {row[0]: row[1] for row in result}
        assert set(chosen) == {row[0] for row in rows}, mode
        for group in reached:
            used = sum(weight(row) * chosen[row[0]] for row in rows if row[1] == group)
            assert used <= bound + 1e-6, (mode, group, used)
        value = sum(row[4] * chosen[row[0]] for row in rows)
        assert math.isclose(value, oracle.objective_value, abs_tol=1e-6), (mode, value, oracle.objective_value)

    agree_with_oracle(cli, _sql(rows, shape), oracle, check_assignment)


@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize("shape", ["bool", "real"])
def test_a_group_no_class_reaches_is_free(decidb_cli, oracle_solver, shape):
    """Group a's rows all have a NULL k, so it has no row; group c's NULL-k row still counts in c's sum."""
    _agree(decidb_cli, oracle_solver, ROWS, shape)


# id, k, v. The composed clause reads every row; only the classes decide whether it holds.
COMPOSED_ROWS = [(1, 9.0), (2, 8.0), (3, 7.0), (4, 6.0)]


@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize("class_row", [None, 3], ids=["every_k_null", "one_class"])
def test_a_composed_min_max_holds_only_with_a_class(decidb_cli, oracle_solver, class_row):
    """`per k: max(x) + min(x) <= 1` over x in 0..3: with every k NULL there is no class and no constraint, so every x
    takes 3; with row 3 in a class the clause holds once over all four rows. No direct rule takes it."""
    rows = [(i, 1 if i == class_row else None, v) for i, v in COMPOSED_ROWS]
    values = ", ".join(f"({i}, {sql_literal(k, 'INTEGER')}, {v})" for i, k, v in rows)
    sql = (
        f"SELECT id, x FROM (FROM (VALUES {values}) t(id, k, v) DECIDE x(INT) "
        "SUCH THAT x BETWEEN 0 AND 3 AND per k: max(x) + min(x) <= 1 MAXIMIZE SUM(v*x)) q ORDER BY id"
    )
    oracle_solver.create_model("per_class_null_key_composed")
    for i, _, _ in rows:
        oracle_solver.add_variable(f"x_{i}", VarType.INTEGER, 0.0, 3.0)
    if class_row is not None:
        # max + min <= 1 holds when some row j has x_i + x_j <= 1 for every i: z_j picks that row, and a z_j of 0
        # relaxes its rows by 6, the most two x in 0..3 can add.
        for j, _, _ in rows:
            oracle_solver.add_variable(f"z_{j}", VarType.BINARY)
        oracle_solver.add_constraint({f"z_{j}": 1.0 for j, _, _ in rows}, "=", 1.0)
        for j, _, _ in rows:
            for i, _, _ in rows:
                coefficients = {f"x_{i}": 1.0, f"z_{j}": 6.0}
                coefficients[f"x_{j}"] = coefficients.get(f"x_{j}", 0.0) + 1.0
                oracle_solver.add_constraint(coefficients, "<=", 7.0)
    oracle_solver.set_objective({f"x_{i}": v for i, _, v in rows}, ObjSense.MAXIMIZE)
    oracle = oracle_solver.solve()

    def check_assignment(result, columns, mode):
        chosen = {row[0]: row[1] for row in result}
        if class_row is not None:
            assert max(chosen.values()) + min(chosen.values()) <= 1, (mode, chosen)
        value = sum(v * chosen[i] for i, _, v in rows)
        assert math.isclose(value, oracle.objective_value, abs_tol=1e-6), (mode, value, oracle.objective_value)

    agree_with_oracle(decidb_cli, sql, oracle, check_assignment, modes=("off",))
