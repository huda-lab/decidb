"""The one message for a NaN or infinite number: every site that refuses one names the value as the user wrote it and
the smallest edit, and the solver path and the direct-solve plans print the same text.

A coefficient must be finite (`v is NaN or infinite on some rows. Skip those rows with 'when isfinite(v):'`); a bound
may be infinite and must not be NaN (`the bound v is NaN on some rows. ...'when NOT isnan(v):'`). A NULL value is
skipped, not refused. The wording lives in `DecideInvalidValueMessage` (`decide_value_messages.hpp`).
"""

import pytest

from ._direct_oracle_common import _allow_direct_path  # noqa: F401  (autouse fixture)

NAN = "'NaN'::DOUBLE"
INFINITY = "'Infinity'::DOUBLE"

# DuckDB prints a subquery's table function quoted.
SUBQUERY = '(SELECT max(v) FROM "range"(2) AS r(v))'


def _query(decide, bad):
    # The middle row holds the bad value in `b`.
    rows = f"(1, 1.0, 2.0, 3.0, 'g'), (2, 2.0, {bad}, 1.0, 'g'), (3, 3.0, 1.0, 2.0, 'h')"
    return f"SELECT id FROM (FROM (VALUES {rows}) t(id, a, b, c, g) DECIDE {decide}) q"


def _coefficient(value):
    return f"DECIDE: {value} is NaN or infinite on some rows. Skip those rows with 'when isfinite({value}):'."


def _bound(value):
    return f"DECIDE: the bound {value} is NaN on some rows. Skip those rows with 'when NOT isnan({value}):'."


def _finite_bound(value):
    return (
        f"DECIDE: the bound {value} is infinite on some rows, and this clause needs a finite one. "
        f"Skip those rows with 'when isfinite({value}):'."
    )


def _stderr(cli, sql, mode):
    return cli.execute_raw(f"SET decide_direct_solve='{mode}'; {sql}").stderr


# (name, DECIDE clauses, the value the message names). Both paths take each one.
BOTH_PATHS_COEFFICIENT = [
    ("a1_constraint", "x(REAL) SUCH THAT b * x <= 5 MAXIMIZE SUM(a * x)", "b"),
    ("a1_objective", "x(REAL) SUCH THAT x <= 5 MAXIMIZE SUM(b * x)", "b"),
    ("a1_like_terms_merge", "x(REAL) SUCH THAT x <= 5 MAXIMIZE SUM(b * x + a * x)", "b + a"),
    ("a1_like_terms_subtract", "x(REAL) SUCH THAT x <= 5 MAXIMIZE SUM(a * x - b * x)", "a - b"),
    ("a1_factor_on_the_sum", "x(REAL) SUCH THAT x <= 5 MAXIMIZE 2 * SUM(b * x)", "2 * b"),
    ("a1_divisor_of_the_sum", "x(REAL) SUCH THAT x <= 5 MAXIMIZE SUM(b * x) / 2", "b / 2"),
    ("a1_constant_term", "x(REAL) SUCH THAT x <= 5 MAXIMIZE SUM(a * x + b)", "b"),
    ("a1_inside_a_square", "x(REAL) SUCH THAT x <= 5 MINIMIZE SUM(POWER(x - b, 2))", "b"),
    ("a1_under_a_when", "x(REAL) SUCH THAT x <= 5 MAXIMIZE SUM(when a > 0: b * x)", "b"),
    (
        "a1_query_wide_factor",
        "x(REAL) SUCH THAT x <= 5 MAXIMIZE SUM((SELECT max(v) FROM range(2) r(v)) * b * x)",
        f"{SUBQUERY} * b",
    ),
    ("r1_budget_weight", "x(REAL) SUCH THAT x <= 4 AND SUM(b * x) <= 9 MAXIMIZE SUM(a * x)", "b"),
    ("r1_objective_value", "x(REAL) SUCH THAT x <= 4 AND SUM(a * x) <= 9 MAXIMIZE SUM(b * x)", "b"),
    ("r1_factor_on_the_value", "x(REAL) SUCH THAT x <= 4 AND SUM(a * x) <= 9 MAXIMIZE 2 * SUM(b * x)", "2 * b"),
    ("r1_like_terms_merge", "x(REAL) SUCH THAT x <= 4 AND SUM(a * x + b * x) <= 9 MAXIMIZE SUM(c * x)", "a + b"),
    ("r1_one_of_two_reducers", "x(REAL) SUCH THAT x <= 4 AND SUM(a * x) <= 9 MAXIMIZE SUM(a * x) + SUM(b * x)", "b"),
    ("r1_per_row_clause", "x(REAL) SUCH THAT b * x <= 4 AND SUM(a * x) <= 9 MAXIMIZE SUM(c * x)", "b"),
    ("s1_score", "x(BOOL) SUCH THAT SUM(x) <= 2 MAXIMIZE SUM(b * x)", "b"),
    ("s1_factor_on_the_score", "x(BOOL) SUCH THAT SUM(x) <= 2 MAXIMIZE 2 * SUM(b * x)", "2 * b"),
    ("s1_one_of_two_reducers", "x(BOOL) SUCH THAT SUM(x) <= 2 MAXIMIZE SUM(a * x) + SUM(b * x)", "b"),
    ("s1_fixing_coefficient", "x(BOOL) SUCH THAT SUM(x) <= 2 AND b * x >= 1 MAXIMIZE SUM(a * x)", "b"),
]

# A coefficient only the solver path reads: a product of decisions, a composed MIN or MAX, a square in a constraint, a
# factor known only when the query runs.
SOLVER_COEFFICIENT = [
    ("bilinear_objective", "x(REAL), y(REAL) SUCH THAT x <= 4 AND y <= 4 MAXIMIZE SUM(b * x * y)", "b"),
    (
        "bilinear_constraint",
        "x(REAL), y(REAL) SUCH THAT x <= 4 AND y <= 4 AND SUM(b * x * y) <= 20 MAXIMIZE SUM(a * x)",
        "b",
    ),
    (
        "composed_extremum_in_the_objective",
        "x(REAL) SUCH THAT x <= 4 AND SUM(x) >= 1 MINIMIZE SUM(x) + MAX(b * x)",
        "b",
    ),
    (
        "composed_extremum_in_a_constraint",
        "x(REAL) SUCH THAT x <= 4 AND SUM(x) + MAX(b * x) <= 20 MAXIMIZE SUM(a * x)",
        "b",
    ),
    ("square_in_a_constraint", "x(REAL) SUCH THAT x <= 4 AND SUM(POWER(b * x, 2)) <= 20 MAXIMIZE SUM(a * x)", "b"),
    ("product_of_reducers", "x(INT) SUCH THAT x <= 4 AND SUM(b * x) * SUM(a * x) <= 20 MAXIMIZE SUM(a * x)", "b"),
]

BOTH_PATHS_BOUND = [
    ("a1_bound", "x(REAL) SUCH THAT x <= b MAXIMIZE SUM(a * x)", "b"),
    ("a1_lower_bound", "x(REAL) SUCH THAT x <= 5 AND x >= b MAXIMIZE SUM(a * x)", "b"),
    ("a1_bound_expression", "x(REAL) SUCH THAT x <= b + 1 MAXIMIZE SUM(a * x)", "b + 1"),
    ("a1_bound_under_a_when", "x(REAL) SUCH THAT when a > 0: x <= b MAXIMIZE SUM(a * x)", "b"),
    ("r1_limit", "x(REAL) SUCH THAT x <= 4 AND SUM(a * x) <= b MAXIMIZE SUM(c * x)", "b"),
    ("r1_limit_expression", "x(REAL) SUCH THAT x <= 4 AND SUM(a * x) <= b + 2 MAXIMIZE SUM(c * x)", "b + 2"),
    ("r1_equality_limit", "x(REAL) SUCH THAT x <= 4 AND SUM(a * x) = b MINIMIZE SUM(c * x)", "b"),
    ("r1_per_row_bound", "x(REAL) SUCH THAT x <= b AND SUM(a * x) <= 9 MAXIMIZE SUM(c * x)", "b"),
    ("r1_limit_by_group", "x(REAL) SUCH THAT x <= 4 AND SUM(a * x) by (g) <= SUM(b) by (g) MAXIMIZE SUM(c * x)", "b"),
    ("s1_count_bound", "x(BOOL) SUCH THAT SUM(x) <= b MAXIMIZE SUM(a * x)", "b"),
    ("s1_count_bound_expression", "x(BOOL) SUCH THAT SUM(x) <= b + 1 MAXIMIZE SUM(a * x)", "b + 1"),
    ("s1_fixing_value", "x(BOOL) SUCH THAT SUM(x) <= 2 AND x >= b MAXIMIZE SUM(a * x)", "b"),
    ("data_reducer_argument", "x(BOOL) SUCH THAT SUM(x) <= MIN(b) MAXIMIZE SUM(a * x)", "b"),
    ("data_reducer_sum_argument", "x(BOOL) SUCH THAT SUM(x) <= SUM(b) MAXIMIZE SUM(a * x)", "b"),
]


@pytest.mark.error
@pytest.mark.parametrize("bad", [NAN, INFINITY], ids=["nan", "infinity"])
@pytest.mark.parametrize("name,decide,value", BOTH_PATHS_COEFFICIENT, ids=[case[0] for case in BOTH_PATHS_COEFFICIENT])
def test_a_coefficient_that_is_not_finite_is_named_as_written_on_both_paths(decidb_cli, name, decide, value, bad):
    direct = _stderr(decidb_cli, _query(decide, bad), "require")
    solver = _stderr(decidb_cli, _query(decide, bad), "off")
    assert _coefficient(value) in solver, solver
    assert direct == solver


# Values the solver finds finite and the direct plan finds too large: each is a sum or a square it computes before it
# answers. The solver path reaches the same numbers as an internal error or a model it warns about.
DIRECT_OVERFLOW = [
    # The squared coefficient of a decision, `b * b`, is beyond a double: the message names the objective's terms of it.
    ("a1_square_overflows", "x(REAL) SUCH THAT x <= 5 MINIMIZE SUM(POWER(b * x, 2))", "power(b, 2)", "1e200"),
    (
        "a1_square_and_linear_overflow",
        "x(REAL) SUCH THAT x <= 5 MINIMIZE SUM(POWER(b * x, 2) + a * x)",
        "a + power(b, 2)",
        "1e200",
    ),
    # Two reducers whose coefficients are each finite and whose score is not: the message names the score.
    ("s1_score_overflows", "x(BOOL) SUCH THAT SUM(x) <= 2 MAXIMIZE SUM(b * x) + SUM(b * x)", "b + b", "1e308"),
    ("a1_coefficient_overflows", "x(REAL) SUCH THAT x <= 5 MAXIMIZE SUM(b * x) + SUM(b * x)", "b + b", "1e308"),
]


@pytest.mark.error
@pytest.mark.parametrize("name,decide,value,big", DIRECT_OVERFLOW, ids=[case[0] for case in DIRECT_OVERFLOW])
def test_a_sum_the_plan_computes_that_overflows_is_named_as_written(decidb_cli, name, decide, value, big):
    assert _coefficient(value) in _stderr(decidb_cli, _query(decide, big), "require")


@pytest.mark.error
@pytest.mark.parametrize("name,decide,value", SOLVER_COEFFICIENT, ids=[case[0] for case in SOLVER_COEFFICIENT])
def test_a_coefficient_only_the_solver_reads_is_named_as_written(decidb_cli, name, decide, value):
    assert _coefficient(value) in _stderr(decidb_cli, _query(decide, NAN), "off")


@pytest.mark.error
@pytest.mark.parametrize("name,decide,value", BOTH_PATHS_BOUND, ids=[case[0] for case in BOTH_PATHS_BOUND])
def test_a_bound_that_is_nan_is_named_as_written_on_both_paths(decidb_cli, name, decide, value):
    direct = _stderr(decidb_cli, _query(decide, NAN), "require")
    solver = _stderr(decidb_cli, _query(decide, NAN), "off")
    assert _bound(value) in solver, solver
    assert direct == solver


# A factor on a squared sum in a constraint, and on a composed MIN or MAX. It is one value for the whole query, so no
# row can be skipped, and the message names it as written all the same. Every `b` is finite: the factor is the bad
# value.
FACTORS = [
    (
        "factor_on_a_squared_sum",
        "x(REAL) SUCH THAT x <= 4 AND (SELECT {bad}) * SUM(POWER(b * x, 2)) <= 20 MAXIMIZE SUM(a * x)",
    ),
    (
        "factor_on_a_composed_extremum",
        "x(REAL) SUCH THAT x <= 4 AND (SELECT {bad}) * MAX(b * x) + SUM(x) <= 20 MAXIMIZE SUM(a * x)",
    ),
]


@pytest.mark.error
@pytest.mark.parametrize("bad,written", [(NAN, "NaN"), (INFINITY, "Infinity")], ids=["nan", "infinity"])
@pytest.mark.parametrize("name,decide", FACTORS, ids=[case[0] for case in FACTORS])
def test_a_factor_that_is_not_finite_is_named_as_written(decidb_cli, name, decide, bad, written):
    sql = _query(decide.format(bad=bad), "1.0")
    assert _coefficient(f"(SELECT CAST('{written}' AS DOUBLE))") in _stderr(decidb_cli, sql, "off")


@pytest.mark.error
@pytest.mark.parametrize(
    "decide",
    [
        "x(REAL) SUCH THAT x <= 9 AND b * x <= 5 MAXIMIZE SUM(a * x)",
        "x(REAL) SUCH THAT x <= 5 MAXIMIZE SUM(b * x)",
        "x(REAL) SUCH THAT x <= 9 AND x <= b MAXIMIZE SUM(a * x)",
        "x(REAL) SUCH THAT x <= 4 AND SUM(b * x) <= 9 MAXIMIZE SUM(a * x)",
        "x(REAL) SUCH THAT x <= 4 AND SUM(a * x) <= b MAXIMIZE SUM(c * x)",
        "x(BOOL) SUCH THAT SUM(x) <= 2 MAXIMIZE SUM(b * x)",
        "x(BOOL) SUCH THAT SUM(x) <= b MAXIMIZE SUM(a * x)",
    ],
)
def test_a_null_value_is_still_skipped_not_refused(decidb_cli, decide):
    sql = _query(decide, "NULL::DOUBLE")
    for mode in ("require", "off"):
        assert _stderr(decidb_cli, sql, mode) == "", mode


@pytest.mark.error
def test_a_rewrite_that_needs_a_finite_bound_names_the_infinite_one_as_written(decidb_cli):
    # `norm(e, 0)` states its links with a Big-M, which no infinite bound can dominate; a NaN there keeps the bound text.
    decide = "x(REAL) SUCH THAT x <= 10 AND norm(x + b, 0) <= 2 MAXIMIZE SUM(a * x)"
    assert _finite_bound("b") in _stderr(decidb_cli, _query(decide, INFINITY), "off")
    assert _bound("b") in _stderr(decidb_cli, _query(decide, NAN), "off")


@pytest.mark.error
@pytest.mark.parametrize(
    "decide",
    [
        "x(REAL) SUCH THAT x <= 'NaN'::DOUBLE MAXIMIZE SUM(a * x)",
        "x(REAL) SUCH THAT x <= 5 AND x >= 0.0 / 0.0 MAXIMIZE SUM(a * x)",
        "x(INT) SUCH THAT x <= 5 AND 2 * x <= 'NaN'::DOUBLE MAXIMIZE SUM(a * x)",
    ],
)
def test_a_constant_bound_that_is_nan_is_named(decidb_cli, decide):
    # Nothing reads a constant bound per row, so the check on the values it admits is the only one that sees it.
    stderr = _stderr(decidb_cli, _query(decide, "1.0"), "off")
    assert "is NaN on some rows. Skip those rows with 'when NOT isnan(" in stderr, stderr
