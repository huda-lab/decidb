"""A `per K:` clause with a `by` reducer has a row only for the groups its classes reach (D5, deck p. 30).

`per k: SUM(x) by (g) <= b` is generated once per class of `k`, and each class's sum reads the kept rows of its `g`
group. A row whose `k` is NULL is in no class, so a group whose rows all have a NULL `k` has no constraint. A row in no
class still counts in a group another row's class reaches.

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
