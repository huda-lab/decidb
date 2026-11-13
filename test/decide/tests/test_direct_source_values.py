"""Both paths read an infinite source value the same way, in every bound slot a rule reads, and neither reads a value on
a row a `when` drops.

The direct path rejects bad values in SQL (`DirectNumberMessage` for coefficients, `DirectValidBoundPredicate`
for bounds, `direct_runtime_checks.cpp`); the solver path rejects them in C++ (`ExtractDoubleColumn`,
`physical_decide.cpp`). The two cannot share code, and they do not apply the same test: a coefficient must be finite,
while a bound only has to be a number, because an infinite bound means "no bound" or "nothing satisfies it". This file
puts one infinite value on one row of a four-row source, in each place a rule reads a bound, and asserts that
`require` (direct), `off` (solver, the default backend) and `off` on a forced HiGHS backend give the same outcome: the
same error class, or the same rows.

A NULL is an error only on a row that reads it: nothing is computed on a row a `when` drops (syntax reference §6.3).
The `*_when_excludes_row` slots put a NULL there, in each place a rule masks its checks. A `when` inside a sum scopes
the sum and not its limit, so a NULL limit there is still read, and still raises.
"""

import math

import pytest

from ._direct_oracle_common import _allow_direct_path, outcome  # noqa: F401  (autouse fixture)

_LITERALS = {
    "inf": "'Infinity'::{type}",
    "-inf": "'-Infinity'::{type}",
    "null": "NULL::{type}",
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
    "s1_limit_per_group": _slot("cap", "x(BOOL)", "SUM(x) by (g) <= cap", _PICK, ("inf",)),
    "s1_limit_float_column": _slot("cap", "x(BOOL)", "SUM(x) <= cap", _PICK, ("inf",), type="FLOAT"),
    "s1_limit_local_when_excludes_row": _slot(
        "cap", "x(BOOL)", "SUM(when f: x) <= cap", _PICK, ("inf", "null"), when_holds=False
    ),
    "s1_limit_clause_when_excludes_row": _slot(
        "cap", "x(BOOL)", "when f: SUM(x) <= cap", _PICK, ("-inf", "inf", "null"), when_holds=False
    ),
    "s1_limit_per_key_is_null": _slot(
        "cap", "x(BOOL)", "SUM(x) by (g) <= cap", _PICK, ("-inf", "inf"), key_is_null=True
    ),
    "s1_fixed_upper": _slot("fixed_value", "x(BOOL)", "SUM(x) <= 2 AND x <= fixed_value", _PICK, ("inf",)),
    "s1_fixed_when_excludes_row": _slot(
        "fixed_value", "x(BOOL)", "SUM(x) <= 2 AND when f: x <= fixed_value", _PICK, ("null",), when_holds=False
    ),
    "a1_upper": _slot("cap", "x(REAL)", _BOX, _PULL, ("inf",)),
    "a1_upper_float_column": _slot("cap", "x(REAL)", _BOX, _PULL, ("inf",), type="FLOAT"),
    "a1_upper_int_variable": _slot("cap", "x(INT)", _BOX, "MAXIMIZE SUM(x)", ("inf",)),
    "a1_lower": _slot("lo", "x(REAL)", "x >= lo AND x <= cap", _PULL, ("-inf",)),
    "a1_upper_when_excludes_row": _slot(
        "cap", "x(REAL)", "when f: x <= cap AND x >= 0", _PULL, ("-inf", "inf", "null"), when_holds=False
    ),
    "a1_objective_own_when_excludes_row": _slot(
        "t",
        "x(REAL)",
        "x BETWEEN 0 AND 3",
        "MINIMIZE SUM(POWER(x - 1, 2)) + SUM(when f: t * x)",
        ("null",),
        when_holds=False,
    ),
    "r1_bound_when_excludes_row": _slot(
        "cap", "x(REAL)", "x BETWEEN 0 AND 3 AND when f: x <= cap AND SUM(x) <= 4", _PICK, ("null",), when_holds=False
    ),
    "r1_weight_when_excludes_row": _slot(
        "fixed_value",
        "x(REAL)",
        "x BETWEEN 0 AND 3 AND when f: SUM(fixed_value * x) <= 4",
        _PICK,
        ("null",),
        when_holds=False,
    ),
    "r1_limit_when_excludes_row": _slot(
        "cap", "x(REAL)", "x BETWEEN 0 AND 3 AND when f: SUM(x) <= cap", _PICK, ("null",), when_holds=False
    ),
    "r1_limit_local_when_excludes_row": _slot(
        "cap", "x(REAL)", "x BETWEEN 0 AND 3 AND SUM(when f: x) <= cap", _PICK, ("null",), when_holds=False
    ),
}
CASES = [pytest.param(slot, value, id=f"{slot}-{value}") for slot, spec in SLOTS.items() for value in spec[4]]

_BALL = "SUM(POWER(x, 2)) <= 4"

# N1: the places it reads a value of a row a `when` drops, and the limit of a group. HiGHS refuses a quadratic limit, so
# these compare the direct path with Gurobi alone.
N1_SLOTS = {
    "n1_limit_tightest_of_group": _slot("cap", "x(REAL)", "SUM(POWER(x, 2)) by (g) <= cap", _PICK, ("inf", "null")),
    "n1_limit_per_key_is_null": _slot(
        "cap", "x(REAL)", "SUM(POWER(x, 2)) by (g) <= cap", _PULL, ("-inf", "inf", "null"), key_is_null=True
    ),
    "n1_limit_clause_when_excludes_row": _slot(
        "cap", "x(REAL)", "when f: SUM(POWER(x, 2)) <= cap", _PULL, ("-inf", "inf", "null"), when_holds=False
    ),
    "n1_limit_local_when_excludes_row": _slot(
        "cap", "x(REAL)", "SUM(when f: POWER(x, 2)) <= cap", _PULL, ("-inf", "inf", "null"), when_holds=False
    ),
    "n1_score_part_when_excludes_row": _slot(
        "t", "x(REAL)", _BALL, "MAXIMIZE SUM(when f: t * x)", ("-inf", "inf", "null"), when_holds=False
    ),
    "n1_score_objective_when_excludes_row": _slot(
        "t", "x(REAL)", _BALL, "MAXIMIZE when f: SUM(t * x)", ("-inf", "inf", "null"), when_holds=False
    ),
    "n1_target_part_when_excludes_row": _slot(
        "t", "x(REAL)", _BALL, "MINIMIZE SUM(when f: POWER(x - t, 2))", ("-inf", "inf", "null"), when_holds=False
    ),
    "n1_constant_part_when_excludes_row": _slot(
        "t", "x(REAL)", _BALL, "MAXIMIZE SUM(when f: (id + 1) * x + t)", ("-inf", "inf", "null"), when_holds=False
    ),
    # A weight inside the square, on the row a `when` drops: the objective drops that row too, so it has no direction.
    "n1_weight_clause_when_excludes_row": _slot(
        "fixed_value",
        "x(REAL)",
        "when f: SUM(POWER(SQRT(fixed_value) * x, 2)) <= 4",
        "MAXIMIZE SUM(when f: (id + 1) * x)",
        ("-inf", "inf", "null"),
        when_holds=False,
    ),
    "n1_weight_local_when_excludes_row": _slot(
        "fixed_value",
        "x(REAL)",
        "SUM(when f: POWER(fixed_value * x, 2)) <= 4",
        "MAXIMIZE SUM(when f: (id + 1) * x)",
        ("-inf", "inf", "null"),
        when_holds=False,
    ),
}
N1_CASES = [pytest.param(slot, value, id=f"{slot}-{value}") for slot, spec in N1_SLOTS.items() for value in spec[4]]

# Gurobi stops a quadratic limit at its barrier tolerance, where an exact 0 comes back as 0.001.
_N1_SOLVER_TOLERANCE = 2e-3


def _same(first, second, abs_tol=1e-6):
    """Equal outcomes: the same error class, or the same rows (to the solver's numerical tolerance)."""
    if first[0] != second[0]:
        return False
    if first[0] == "error":
        return first[1] == second[1]
    if len(first[1]) != len(second[1]):
        return False
    return all(
        len(a) == len(b) and all(x == y or math.isclose(x, y, abs_tol=abs_tol) for x, y in zip(a, b))
        for a, b in zip(first[1], second[1])
    )


@pytest.mark.error
@pytest.mark.parametrize("slot,value", CASES)
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


@pytest.mark.error
@pytest.mark.parametrize("slot,value", N1_CASES)
def test_n1_reads_a_bad_source_value_as_the_solver_does(decidb_cli, slot, value):
    column, decl, constraints, objective, _, source = N1_SLOTS[slot]
    sql = (
        f"SELECT id, x FROM {_source(column, value, **source)} "
        f"DECIDE {decl} SUCH THAT {constraints} {objective} ORDER BY id"
    )
    direct = outcome(decidb_cli, sql, "require")
    solver = outcome(decidb_cli, sql, "off")
    assert _same(direct, solver, _N1_SOLVER_TOLERANCE), f"direct {direct} but solver {solver}"


# Nothing is computed on a row a `when` drops: an expression that raises (a logarithm of 0, an overflow) raises on a
# kept row only. Row 2 holds the bad input; `f` decides whether a `when` keeps it. Each case reaches one place a rule
# computes a value under a `when`.
def _raising_source(kept):
    return (
        "(VALUES (1, true, 2.0::DOUBLE, 3::BIGINT, 5.0::DOUBLE), "
        f"(2, {str(kept).lower()}, 0.0, 4000000000000000000, 4.0)) t(id, f, w, big, v)"
    )


RAISING_CASES = {
    "a1_clause_bound": "x(REAL) SUCH THAT x BETWEEN 0 AND 5 AND when f: x <= ln(w) MAXIMIZE SUM(x)",
    "a1_clause_coefficient": "x(REAL) SUCH THAT x BETWEEN 0 AND 5 AND when f: ln(w) * x <= 3 MAXIMIZE SUM(x)",
    "a1_objective_part": "x(REAL) SUCH THAT x BETWEEN 0 AND 5 MINIMIZE SUM(POWER(x - 1, 2)) + SUM(when f: ln(w) * x)",
    "s1_limit": "x(BOOL) SUCH THAT when f: SUM(x) <= big * 3 MAXIMIZE SUM(v*x)",
    "r1_clause_bound": "x(REAL) SUCH THAT x BETWEEN 0 AND 5 AND when f: x <= ln(w) AND SUM(x) <= 4 MAXIMIZE SUM(v*x)",
    "r1_weight": "x(REAL) SUCH THAT x BETWEEN 0 AND 5 AND when f: SUM(ln(w) * x) <= 3 MAXIMIZE SUM(v*x)",
    "r1_limit": "x(REAL) SUCH THAT x BETWEEN 0 AND 5 AND when f: SUM(x) <= big * 3 MAXIMIZE SUM(v*x)",
    "n1_limit": "x(REAL) SUCH THAT when f: SUM(POWER(x, 2)) <= big * 3 MINIMIZE SUM(POWER(x - v, 2))",
    "n1_score": "x(REAL) SUCH THAT SUM(POWER(x, 2)) <= 4 MAXIMIZE SUM(when f: ln(w) * x)",
    "n1_target": "x(REAL) SUCH THAT SUM(POWER(x, 2)) <= 4 MINIMIZE SUM(when f: POWER(x - ln(w), 2))",
    "n1_constant": "x(REAL) SUCH THAT SUM(POWER(x, 2)) <= 4 MAXIMIZE SUM(when f: v * x + ln(w))",
    "n1_weight": "x(REAL) SUCH THAT when f: SUM(POWER(SQRT(ln(w)) * x, 2)) <= 4 MAXIMIZE SUM(when f: v * x)",
}


@pytest.mark.error
@pytest.mark.parametrize("kept", [False, True], ids=["dropped", "kept"])
@pytest.mark.parametrize("name", sorted(RAISING_CASES))
def test_both_paths_compute_nothing_on_a_row_a_when_drops(decidb_cli, name, kept):
    sql = f"SELECT id, x FROM (FROM {_raising_source(kept)} DECIDE {RAISING_CASES[name]}) q ORDER BY id"
    direct = outcome(decidb_cli, sql, "require")
    solver = outcome(decidb_cli, sql, "off")
    assert direct[0] == ("error" if kept else "ok"), direct
    assert _same(direct, solver), f"direct {direct} but solver {solver}"
