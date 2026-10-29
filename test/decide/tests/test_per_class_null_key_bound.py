"""A row whose `per K:` key is NULL is in no class (D5), so its bound limits no instance (BUG-12).

`per g: SUM(x) <= cap` bounds the whole total once per class of `g` (D6), with the bound the class determines. A row
with a NULL `g` still counts in the total, but it names no class, so its own `cap` is read by no instance. A relation
key (`per D`) compares NULL as a value: a NULL in a column of `D` is a class, and its rows' bound is read.

Each test builds the oracle model from the raw rows, as the language reads the clause: one constraint over every kept
row for each class, with that class's bound; a row in no class gives none. The solver path (`off`) and the direct rules
(`require`: S1 for BOOL, R1 for REAL) must both answer as the oracle does.
"""

import math

import pytest

from decidb_cli import DeciDBCliError
from solver.types import ObjSense, VarType

from ._direct_oracle_common import _allow_direct_path  # noqa: F401  (autouse fixture)
from ._direct_oracle_common import agree_with_oracle, run, sql_literal

NAN = float("nan")
TYPES = ["INTEGER", "VARCHAR", "DOUBLE", "DOUBLE", "DOUBLE"]

# id, g, w, v, cap. Rows 2 and 6 are in no class; their bounds (1 and 0.5) are tighter than any class's (a: 4, b: 6).
ROWS = [
    (1, "a", 1.0, 9.0, 4.0),
    (2, None, 1.0, 8.0, 1.0),
    (3, "a", 2.0, 7.0, 4.0),
    (4, "b", 1.0, 6.0, 6.0),
    (5, "a", 1.0, 5.0, 4.0),
    (6, None, 2.0, 4.0, 0.5),
    (7, "b", 2.0, 3.0, 6.0),
    (8, "a", 1.0, 2.0, 4.0),
]

# shape: the declaration and its box, the clause body, and the weight the clause gives a row.
SHAPES = {
    "bool": ("x(BOOL)", "", "SUM(x)", lambda row: 1.0, VarType.BINARY),
    "real": ("x(REAL)", "x BETWEEN 0 AND 2 AND ", "SUM(w*x)", lambda row: row[2], VarType.CONTINUOUS),
}


def _sql(rows, shape):
    declaration, box, clause, _, _ = SHAPES[shape]
    values = ", ".join(
        "(" + ", ".join(sql_literal(value, sql_type) for value, sql_type in zip(row, TYPES)) + ")" for row in rows
    )
    return (
        f"SELECT id, x FROM (FROM (VALUES {values}) t(id, g, w, v, cap) DECIDE {declaration} "
        f"SUCH THAT {box}per g: {clause} <= cap MAXIMIZE SUM(v*x)) q ORDER BY id"
    )


def _agree(cli, oracle_solver, rows, shape):
    _, _, _, weight, var_type = SHAPES[shape]
    oracle_solver.create_model("per_class_null_key_bound")
    for row in rows:
        if var_type == VarType.BINARY:
            oracle_solver.add_variable(f"x_{row[0]}", var_type)
        else:
            oracle_solver.add_variable(f"x_{row[0]}", var_type, 0.0, 2.0)
    caps = {}
    for row in rows:
        if row[1] is not None:
            caps.setdefault(row[1], set()).add(row[4])
    limits = {}
    for name, values in caps.items():
        assert len(values) == 1, name
        limits[name] = next(iter(values))
        oracle_solver.add_constraint({f"x_{row[0]}": weight(row) for row in rows}, "<=", limits[name])
    oracle_solver.set_objective({f"x_{row[0]}": row[3] for row in rows}, ObjSense.MAXIMIZE)
    oracle = oracle_solver.solve()

    def check_assignment(result, columns, mode):
        assert columns == ["id", "x"], mode
        chosen = {row[0]: row[1] for row in result}
        assert set(chosen) == {row[0] for row in rows}, mode
        used = sum(weight(row) * chosen[row[0]] for row in rows)
        for name, limit in limits.items():
            assert used <= limit + 1e-6, (mode, name, used, limit)
        value = sum(row[3] * chosen[row[0]] for row in rows)
        assert math.isclose(value, oracle.objective_value, abs_tol=1e-6), (mode, value, oracle.objective_value)

    agree_with_oracle(cli, _sql(rows, shape), oracle, check_assignment)


@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize("shape", ["bool", "real"])
def test_a_null_key_rows_bound_limits_no_class(decidb_cli, oracle_solver, shape):
    """The BUG-12 shape: rows 2 and 6 are in no class, so only class a's and b's bounds limit the total."""
    _agree(decidb_cli, oracle_solver, ROWS, shape)


@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize("shape", ["bool", "real"])
def test_a_nan_bound_on_a_null_key_row_is_never_read(decidb_cli, oracle_solver, shape):
    """A row in no class has no instance, so its NaN bound raises nothing, as nothing is computed on a row a `when`
    drops."""
    rows = [row[:4] + (NAN if row[1] is None else row[4],) for row in ROWS]
    _agree(decidb_cli, oracle_solver, rows, shape)


@pytest.mark.cons_aggregate
@pytest.mark.error
@pytest.mark.parametrize("shape", ["bool", "real"])
@pytest.mark.parametrize("mode", ["off", "require"])
def test_a_nan_bound_on_a_class_row_still_raises(decidb_cli, shape, mode):
    """Class b's one row has a NaN bound: the class determines it, and the message is the same on both paths."""
    rows = [row[:4] + (NAN if row[0] == 4 else row[4],) for row in ROWS if row[1] != "b" or row[0] == 4]
    with pytest.raises(
        DeciDBCliError,
        match=r"DECIDE: the bound cap is NaN on some rows\. Skip those rows with 'when NOT isnan\(cap\):'\.",
    ):
        run(decidb_cli, _sql(rows, shape), mode)


# A relation key: D2's NULL region is a value of `D`, so D2 is a class of its own and its rows' bound (1) is read.
DEPOTS = "(VALUES ('D1', 'East'), ('D2', CAST(NULL AS VARCHAR))) D(dk, region)"
SHIPS = (
    "(VALUES (1, 'D1', 9.0, 3.0), (2, 'D1', 8.0, 3.0), (3, 'D2', 7.0, 1.0), (4, 'D2', 6.0, 1.0), (5, 'D1', 5.0, 3.0)) "
    "S(id, dk, v, cap)"
)


@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_a_relation_key_with_a_null_column_still_forms_its_class(decidb_cli, oracle_solver):
    sql = (
        f"SELECT id, x FROM (FROM {DEPOTS} JOIN {SHIPS} ON D.dk = S.dk "
        "DECIDE x(BOOL) SUCH THAT per D: SUM(x) <= S.cap MAXIMIZE SUM(S.v * x)) q ORDER BY id"
    )
    values = {1: 9.0, 2: 8.0, 3: 7.0, 4: 6.0, 5: 5.0}
    oracle_solver.create_model("per_class_relation_null")
    for i in values:
        oracle_solver.add_variable(f"x_{i}", VarType.BINARY)
    # Both depots are classes: D1 allows 3 and D2 allows 1, and the total sits under both.
    for limit in (3.0, 1.0):
        oracle_solver.add_constraint({f"x_{i}": 1.0 for i in values}, "<=", limit)
    oracle_solver.set_objective({f"x_{i}": v for i, v in values.items()}, ObjSense.MAXIMIZE)
    oracle = oracle_solver.solve()

    def check_assignment(result, columns, mode):
        chosen = {row[0]: row[1] for row in result}
        assert sum(chosen.values()) <= 1, (mode, chosen)
        value = sum(values[i] * chosen[i] for i in values)
        assert math.isclose(value, oracle.objective_value, abs_tol=1e-6), (mode, value, oracle.objective_value)

    agree_with_oracle(decidb_cli, sql, oracle, check_assignment)
