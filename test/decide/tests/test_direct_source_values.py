"""Both paths read an infinite source value the same way, in every bound slot a rule reads.

The direct path rejects bad values in SQL (`DirectValidNumberPredicate` for coefficients, `DirectValidBoundPredicate`
for bounds, `direct_runtime_checks.cpp`); the solver path rejects them in C++ (`ExtractDoubleColumn`,
`physical_decide.cpp`). The two cannot share code, and they do not apply the same test: a coefficient must be finite,
while a bound only has to be a number, because an infinite bound means "no bound" or "nothing satisfies it". This file
puts one infinite value on one row of a four-row source, in each place a rule reads a bound, and asserts that
`require` (direct), `off` (solver, the default backend) and `off` on a forced HiGHS backend give the same outcome: the
same error class, or the same rows.
"""

import math

import pytest

from ._direct_oracle_common import _allow_direct_path, outcome  # noqa: F401  (autouse fixture)

_LITERALS = {
    "inf": "'Infinity'::{type}",
    "-inf": "'-Infinity'::{type}",
}

# Column -> its value on an ordinary row i is `base + i * 0.5`.
_BASES = {"cap": 2.0, "lo": 0.0, "t": 1.0, "fixed_value": 1.0}
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


def _slot(column, decl, constraints, objective, values, **source):
    return column, decl, constraints, objective, values, source


# Weights are `id + 1`, never 0, so a tied row cannot make two backends choose differently.
_PICK = "MAXIMIZE SUM((id + 1) * x)"
_PULL = "MINIMIZE SUM(POWER(x - t, 2))"
_BOX = "x BETWEEN 0 AND cap"

# Bounds and limits: an infinite value is a legitimate bound. Each slot lists the infinite values the file puts in it.
SLOTS = {
    "s1_limit_upper": _slot("cap", "x(BOOL)", "SUM(x) <= cap", _PICK, ("inf",)),
    "s1_limit_lower": _slot("cap", "x(BOOL)", "SUM(x) >= cap AND SUM(x) <= 3", _PICK, ("-inf",)),
    "s1_limit_per_group": _slot("cap", "x(BOOL)", "SUM(x) <= cap PER g", _PICK, ("inf",)),
    "s1_limit_float_column": _slot("cap", "x(BOOL)", "SUM(x) <= cap", _PICK, ("inf",), type="FLOAT"),
    "s1_limit_local_when_excludes_row": _slot(
        "cap", "x(BOOL)", "SUM(x) WHEN f <= cap", _PICK, ("inf",), when_holds=False
    ),
    "s1_limit_clause_when_excludes_row": _slot(
        "cap", "x(BOOL)", "SUM(x) <= cap WHEN f", _PICK, ("-inf", "inf"), when_holds=False
    ),
    "s1_limit_per_key_is_null": _slot(
        "cap", "x(BOOL)", "SUM(x) <= cap PER g", _PICK, ("-inf", "inf"), key_is_null=True
    ),
    "s1_fixed_upper": _slot("fixed_value", "x(BOOL)", "SUM(x) <= 2 AND x <= fixed_value", _PICK, ("inf",)),
    "a1_upper": _slot("cap", "x(REAL)", _BOX, _PULL, ("inf",)),
    "a1_upper_float_column": _slot("cap", "x(REAL)", _BOX, _PULL, ("inf",), type="FLOAT"),
    "a1_upper_int_variable": _slot("cap", "x(INT)", _BOX, "MAXIMIZE SUM(x)", ("inf",)),
    "a1_lower": _slot("lo", "x(REAL)", "x >= lo AND x <= cap", _PULL, ("-inf",)),
    "a1_upper_when_excludes_row": _slot(
        "cap", "x(REAL)", "x <= cap WHEN f AND x >= 0", _PULL, ("-inf", "inf"), when_holds=False
    ),
}
CASES = [(slot, value) for slot, spec in SLOTS.items() for value in spec[4]]


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
@pytest.mark.parametrize("slot,value", CASES, ids=[f"{slot}-{value}" for slot, value in CASES])
def test_both_paths_read_a_bad_source_value_the_same_way(decidb_cli, decidb_cli_highs, slot, value):
    column, decl, constraints, objective, _, source = SLOTS[slot]
    sql = (
        f"SELECT id, x FROM {_source(column, value, **source)} "
        f"DECIDE {decl} SUCH THAT {constraints} {objective} ORDER BY id"
    )
    direct = outcome(decidb_cli, sql, "require")
    solver = outcome(decidb_cli, sql, "off")
    solver_highs = outcome(decidb_cli_highs, sql, "off")
    assert _same(direct, solver), f"direct {direct} but solver {solver}"
    assert _same(solver, solver_highs), f"solver {solver} but forced HiGHS {solver_highs}"
