"""A bound on one decision admits exactly the values DuckDB says it does.

A whole-numbered decision admits `n` when DuckDB, evaluating the written clause with `n` in place of `x`, returns true.
DuckDB types `0.1` as DECIMAL, so `0.1*x <= 0.3` admits 3; a DOUBLE spelling of the same equality admits nothing. The
classifier asks DuckDB the question instead of computing `r / a` in doubles, and every path (the plan-time fold, the
per-row fold of a data bound, direct solve) reads its answer, so the paths and both backends agree.

DuckDB is the judge here: a single-row query's optimum is the extreme whole `n` in a window for which DuckDB returns
true on the whole `SUCH THAT` clause, or infeasible when no `n` in the window passes. Every case sets an unconditional
literal floor, so the default floor of 0 (which is not SQL) never decides the answer.

Covers:
  - test_answer_is_the_value_duckdb_admits: literal and data bounds, DECIMAL and DOUBLE coefficients, `=`, a negative
    data coefficient, a quotient just below a whole number, a right side past the INTEGER range, an `ABS` helper,
    an easy MAX whose member is scaled by data or shifted, a data coefficient of 0 against a right side within the
    model builder's tolerance of 0 (DuckDB decides, not the tolerance)
"""

import re

import pytest

from decidb_cli import DecidBCliError

_WINDOW = range(-50, 51)
_INFEASIBLE = r"DECIDE optimization is infeasible\. Prefix the query with DIAGNOSE"

# (id, source row, SUCH THAT clause, objective sense). The source has one row, `t(id, c)`.
_CASES = [
    ("data_quotient_decimal", "(1, 4.3)", "x <= c / 0.1 AND x >= -10", "MAXIMIZE"),
    ("data_quotient_below_whole", "(1, 0.3)", "x <= c / 0.1 AND x >= -10", "MAXIMIZE"),
    ("literal_just_below_whole", "(1, 0)", "x <= 42.9999999 AND x >= -10", "MAXIMIZE"),
    ("decimal_coefficient_decimal_literal", "(1, 0)", "0.1 * x <= 0.3 AND x >= -10", "MAXIMIZE"),
    ("decimal_coefficient_double_data", "(1, 0.3::DOUBLE)", "0.1 * x <= c AND x >= -10", "MAXIMIZE"),
    ("decimal_equality_on_data", "(1, 0.3)", "0.1 * x = c", "MAXIMIZE"),
    ("double_equality_admits_nothing", "(1, 0)", "0.1::DOUBLE * x = 0.3::DOUBLE", "MAXIMIZE"),
    ("non_whole_equality", "(1, 0)", "x = -0.5", "MAXIMIZE"),
    ("box_between_whole_numbers", "(1, 0)", "x >= -0.7 AND x <= -0.2", "MAXIMIZE"),
    ("negative_data_coefficient", "(1, -2.5)", "c * x <= 10 AND x >= -10 AND x <= 6", "MINIMIZE"),
    ("tiny_coefficient", "(1, 1.0)", "1e-300 * x >= c AND x >= -10 AND x <= 5", "MAXIMIZE"),
    ("zero_data_coefficient_fails", "(1, 0.0)", "c * x <= -1e-10 AND x >= -10 AND x <= 5", "MAXIMIZE"),
    ("zero_data_coefficient_holds", "(1, 0.0)", "c * x <= 1e-10 AND x >= -10 AND x <= 5", "MAXIMIZE"),
    ("data_past_the_integer_range", "(1, 3e9)", "x <= c AND x >= -10 AND x <= 5", "MAXIMIZE"),
    ("negative_infinite_floor_frees", "(1, 0)", "x >= -'inf'::DOUBLE AND x >= -5", "MINIMIZE"),
    ("abs_helper_keeps_its_type", "(1, 0)", "abs(x - 0.5) <= 2 AND x >= -10", "MAXIMIZE"),
    ("easy_max_over_a_data_scaled_member", "(1, 0.1)", "MAX(c * x) <= 0.3 AND x >= -10", "MAXIMIZE"),
    ("easy_max_over_a_shifted_member", "(1, 0)", "MAX(0.1 * x + 0.1) <= 0.4 AND x >= -10", "MAXIMIZE"),
]


def _duckdb_optimum(duckdb_conn, source, clause, sense):
    """The extreme whole `n` in the window for which DuckDB holds the clause on the row, or None."""

    def holds(n):
        written = re.sub(r"\bx\b", f"CAST({n} AS INTEGER)", clause)
        return duckdb_conn.execute(f"SELECT {written} FROM (VALUES {source}) t(id, c)").fetchone()[0]

    admitted = [n for n in _WINDOW if holds(n)]
    if not admitted:
        return None
    best = max(admitted) if sense == "MAXIMIZE" else min(admitted)
    assert best not in (_WINDOW[0], _WINDOW[-1]), "the window must contain the optimum"
    return best


@pytest.mark.cons_perrow
@pytest.mark.cons_comparison
@pytest.mark.edge_case
@pytest.mark.correctness
@pytest.mark.parametrize("name,source,clause,sense", _CASES, ids=[c[0] for c in _CASES])
def test_answer_is_the_value_duckdb_admits(
    decidb_cli_gurobi, decidb_cli_highs, duckdb_conn, name, source, clause, sense
):
    expected = _duckdb_optimum(duckdb_conn, source, clause, sense)
    sql = f"SELECT x FROM (VALUES {source}) t(id, c) DECIDE x(INT) SUCH THAT {clause} {sense} SUM(x)"
    # `1e-300 * x >= 1` is the bound `x >= 1e300`, a number of the model the solver is handed (it is then infeasible).
    large = name == "tiny_coefficient"
    for cli in (decidb_cli_gurobi, decidb_cli_highs):
        for mode in ("off", "auto"):
            full = f"SET decide_direct_solve='{mode}'; {sql}"
            if expected is None:
                with pytest.raises(DecidBCliError, match=_INFEASIBLE):
                    cli.execute(full, expect_large_numbers=large)
            else:
                rows, _ = cli.execute(full, expect_large_numbers=large)
                assert rows == [(expected,)], f"[{mode}] {rows} != {expected}"
