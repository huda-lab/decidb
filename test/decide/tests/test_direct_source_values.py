"""Both paths read a bad source value the same way: NaN, +inf, -inf and NULL in every slot a rule reads.

The direct path rejects bad values in SQL (`DirectValidNumberPredicate` for coefficients, `DirectValidBoundPredicate` for
bounds, `direct_builder.cpp`); the solver path rejects them in C++ (`ExtractDoubleColumn`, `physical_decide.cpp`). The
two cannot share code, and they do not apply the same test: a coefficient must be finite, while a bound only has to be
a number, because an infinite bound means "no bound" or "nothing satisfies it". This file puts one bad value on one row
of a four-row source, in each place a rule reads a source value, and asserts that `require` (direct), `off` (solver, the
default backend) and `off` on a forced HiGHS backend give the same outcome: the same error class, or the same rows.

A slot's `strict` field also pins what the solver contract owes, so a change that made both paths accept a value would
not pass unnoticed. "coefficient" slots raise for all four values; "bound" slots raise for NaN and NULL.
"""

import math

import pytest

from ._direct_oracle_common import _allow_direct_path, outcome  # noqa: F401  (autouse fixture)

_LITERALS = {
    "nan": "'NaN'::{type}",
    "inf": "'Infinity'::{type}",
    "-inf": "'-Infinity'::{type}",
    "null": "NULL::{type}",
}

# Column -> its value on an ordinary row i is `base + i * 0.5`.
_BASES = {"score": 1.0, "cap": 2.0, "lo": 0.0, "t": 1.0, "w": 1.0, "pin": 1.0}
_COLUMNS = ["id", "g", "f"] + list(_BASES)
_BAD_ROW = 1


def _source(column, value, type="DOUBLE", when_holds=True, key_is_null=False):
    """A four-row source whose row 1 holds `value` in `column`. `when_holds` is the row's `f`; `key_is_null` nulls its `g`."""
    rows = []
    for i in range(4):
        bad = i == _BAD_ROW
        cells = {
            "id": str(i),
            "g": "NULL" if bad and key_is_null else str(i % 2),
            "f": str(when_holds or not bad).upper(),
        }
        for name, base in _BASES.items():
            if bad and name == column:
                cells[name] = _LITERALS[value].format(type=type)
            else:
                cells[name] = f"CAST({base + i * 0.5} AS {type})"
        rows.append("(" + ", ".join(cells[name] for name in _COLUMNS) + ")")
    return f"(SELECT * FROM (VALUES {', '.join(rows)}) v({', '.join(_COLUMNS)}))"


def _slot(column, decl, constraints, objective, strict, **source):
    return column, decl, constraints, objective, strict, source


# Weights are `id + 1`, never 0, so a tied row cannot make two backends choose differently.
_PICK = "MAXIMIZE SUM((id + 1) * x)"
_PULL = "MINIMIZE SUM(POWER(x - t, 2))"
_BOX = "x BETWEEN 0 AND cap"

SLOTS = {
    # Objective coefficients, constraint coefficients and square targets: no infinity reading exists.
    "s1_score_maximize": _slot("score", "x(BOOL)", "SUM(x) <= 2", "MAXIMIZE SUM(score * x)", "coefficient"),
    "s1_score_minimize": _slot("score", "x(BOOL)", "SUM(x) >= 1", "MINIMIZE SUM(score * x)", "coefficient"),
    "s1_score_float_column": _slot(
        "score", "x(BOOL)", "SUM(x) <= 2", "MAXIMIZE SUM(score * x)", "coefficient", type="FLOAT"
    ),
    "a1_score": _slot("score", "x(REAL)", _BOX, "MAXIMIZE SUM(score * x)", "coefficient"),
    "a1_score_objective_when_excludes_row": _slot(
        "score", "x(REAL)", _BOX, "MAXIMIZE SUM(score * x) WHEN f", "coefficient", when_holds=False
    ),
    "a1_target": _slot("t", "x(REAL)", _BOX, _PULL, "coefficient"),
    "a1_target_float_column": _slot("t", "x(REAL)", _BOX, _PULL, "coefficient", type="FLOAT"),
    "a1_target_objective_when_excludes_row": _slot(
        "t", "x(REAL)", _BOX, _PULL + " WHEN f", "coefficient", when_holds=False
    ),
    "a1_square_weight": _slot("w", "x(REAL)", _BOX, "MINIMIZE SUM(POWER(w * x - t, 2))", "coefficient"),
    "a1_constraint_coefficient": _slot("w", "x(REAL)", "w * x <= cap AND x >= 0", "MAXIMIZE SUM(x)", "coefficient"),
    "a1_constraint_coefficient_when_excludes_row": _slot(
        "w", "x(REAL)", "w * x <= cap WHEN f AND x >= 0 AND x <= 5", "MAXIMIZE SUM(x)", "coefficient", when_holds=False
    ),
    # Bounds and limits: an infinite value is a legitimate bound, NaN and NULL are not.
    "s1_limit_upper": _slot("cap", "x(BOOL)", "SUM(x) <= cap", _PICK, "bound"),
    "s1_limit_lower": _slot("cap", "x(BOOL)", "SUM(x) >= cap AND SUM(x) <= 3", _PICK, "bound"),
    "s1_limit_per_group": _slot("cap", "x(BOOL)", "SUM(x) <= cap PER g", _PICK, "bound"),
    "s1_limit_float_column": _slot("cap", "x(BOOL)", "SUM(x) <= cap", _PICK, "bound", type="FLOAT"),
    "s1_limit_local_when_excludes_row": _slot(
        "cap", "x(BOOL)", "SUM(x) WHEN f <= cap", _PICK, "bound", when_holds=False
    ),
    "s1_limit_clause_when_excludes_row": _slot(
        "cap", "x(BOOL)", "SUM(x) <= cap WHEN f", _PICK, "bound", when_holds=False
    ),
    "s1_limit_per_key_is_null": _slot("cap", "x(BOOL)", "SUM(x) <= cap PER g", _PICK, "bound", key_is_null=True),
    "s1_pin_upper": _slot("pin", "x(BOOL)", "SUM(x) <= 2 AND x <= pin", _PICK, "bound"),
    "s1_pin_lower": _slot("pin", "x(BOOL)", "SUM(x) <= 2 AND x >= pin", _PICK, "bound"),
    "s1_pin_equal": _slot("pin", "x(BOOL)", "SUM(x) <= 2 AND x = pin", _PICK, "bound"),
    "s1_pin_when_excludes_row": _slot(
        "pin", "x(BOOL)", "SUM(x) <= 2 AND x >= pin WHEN f", _PICK, "bound", when_holds=False
    ),
    "a1_upper": _slot("cap", "x(REAL)", _BOX, _PULL, "bound"),
    "a1_upper_float_column": _slot("cap", "x(REAL)", _BOX, _PULL, "bound", type="FLOAT"),
    "a1_upper_int_variable": _slot("cap", "x(INT)", _BOX, "MAXIMIZE SUM(x)", "bound"),
    "a1_lower": _slot("lo", "x(REAL)", "x >= lo AND x <= cap", _PULL, "bound"),
    "a1_upper_when_excludes_row": _slot(
        "cap", "x(REAL)", "x <= cap WHEN f AND x >= 0", _PULL, "bound", when_holds=False
    ),
}


def _same(first, second):
    """Equal outcomes: the same error class, or the same rows (to the solver's numerical tolerance)."""
    if first[0] != second[0]:
        return False
    if first[0] == "error":
        return first[1] == second[1]
    if len(first[1]) != len(second[1]):
        return False
    return all(
        len(a) == len(b) and all(x == y or math.isclose(x, y, abs_tol=1e-6) for x, y in zip(a, b))
        for a, b in zip(first[1], second[1])
    )


@pytest.mark.error
@pytest.mark.parametrize("value", list(_LITERALS))
@pytest.mark.parametrize("slot", list(SLOTS))
def test_both_paths_read_a_bad_source_value_the_same_way(decidb_cli, decidb_cli_highs, slot, value):
    column, decl, constraints, objective, strict, source = SLOTS[slot]
    sql = (
        f"SELECT id, x FROM {_source(column, value, **source)} "
        f"DECIDE {decl} SUCH THAT {constraints} {objective} ORDER BY id"
    )
    direct = outcome(decidb_cli, sql, "require")
    solver = outcome(decidb_cli, sql, "off")
    solver_highs = outcome(decidb_cli_highs, sql, "off")
    assert _same(direct, solver), f"direct {direct} but solver {solver}"
    assert _same(solver, solver_highs), f"solver {solver} but forced HiGHS {solver_highs}"
    must_raise = value in ("nan", "null") or strict == "coefficient"
    if must_raise:
        assert direct[0] == "error", f"{value} in {column} was accepted: {direct}"
