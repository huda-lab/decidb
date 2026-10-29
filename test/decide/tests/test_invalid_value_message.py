"""The one message for a NaN or infinite number: every site that refuses one names the value as the user wrote it and
the smallest edit, and the solver path and the direct-solve plans print the same text.

A coefficient must be finite (`v is NaN or infinite on some rows. Skip those rows with 'when isfinite(v):'`); a bound
may be infinite and must not be NaN (`the bound v is NaN on some rows. ...'when NOT isnan(v):'`). A NULL value is
skipped, not refused. A number the model computes from finite ones (a sum over rows, a square) that is too large for a
DOUBLE names its clause or the objective and asks to scale its data. The wording lives in `DecideInvalidValueMessage`
(`decide_value_messages.hpp`).
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


def _too_large(written):
    return f"DECIDE: {written} computes a number too large for a DOUBLE. Scale its data down."


# Values each finite whose sum or square is not, on the middle row. Both paths print one text.
DIRECT_OVERFLOW = [
    # The squared coefficient of a decision, `b * b`, is beyond a double. No value of a row is, so the message names
    # the objective and asks to scale its data.
    (
        "a1_square_overflows",
        "x(REAL) SUCH THAT x <= 5 MINIMIZE SUM(POWER(b * x, 2))",
        _too_large("MINIMIZE sum(power(b * x, 2))"),
        "1e200",
    ),
    (
        "a1_square_and_linear_overflow",
        "x(REAL) SUCH THAT x <= 5 MINIMIZE SUM(POWER(b * x, 2) + a * x)",
        _too_large("MINIMIZE sum(power(b * x, 2) + a * x)"),
        "1e200",
    ),
    # Like terms on one row merge into that row's value, `b + b`, which a `when` can skip: the message names it.
    (
        "s1_score_overflows",
        "x(BOOL) SUCH THAT SUM(x) <= 2 MAXIMIZE SUM(b * x) + SUM(b * x)",
        _coefficient("b + b"),
        "1e308",
    ),
    (
        "a1_coefficient_overflows",
        "x(REAL) SUCH THAT x <= 5 MAXIMIZE SUM(b * x) + SUM(b * x)",
        _coefficient("b + b"),
        "1e308",
    ),
]


@pytest.mark.error
@pytest.mark.parametrize("name,decide,message,big", DIRECT_OVERFLOW, ids=[case[0] for case in DIRECT_OVERFLOW])
def test_a_sum_the_plan_computes_that_overflows_is_named_as_written(decidb_cli, name, decide, message, big):
    direct = _stderr(decidb_cli, _query(decide, big), "require")
    solver = _stderr(decidb_cli, _query(decide, big), "off")
    assert message in direct, direct
    assert direct == solver


def _big_query(decide, big):
    # Every row holds the big value in `b`: a sum over rows, or a square of one, is past a DOUBLE.
    rows = f"(1, 1.0, {big}, 'g'), (2, 2.0, {big}, 'g'), (3, 3.0, {big}, 'h')"
    return f"SELECT id FROM (FROM (VALUES {rows}) t(id, a, b, g) DECIDE {decide}) q"


# (name, DECIDE clauses, what the message names, the big value). Each merges finite numbers past a DOUBLE at a
# different place the model builds: like coefficients on one column (a query-wide or keyed decision over the rows),
# the square of a coefficient, the fixed terms moved to the bound side (summed over the rows, or within one row), a
# composed MIN/MAX's outer row, and a two-level objective's group sums.
TOO_LARGE = [
    (
        "constraint_on_a_query_wide_decision",
        "x(REAL), per (): y(REAL) SUCH THAT x <= 5 AND y <= 5 AND SUM(b * y) <= 10 MAXIMIZE SUM(a * x)",
        "SUM(b * y) <= 10",
        "1e308",
    ),
    (
        "constraint_on_a_keyed_decision_by_group",
        "x(REAL), per g: y(REAL) SUCH THAT x <= 5 AND y <= 5 AND SUM(b * y) by (g) <= 10 MAXIMIZE SUM(a * x)",
        "SUM(b * y) by (g) <= 10",
        "1e308",
    ),
    (
        "objective_on_a_query_wide_decision",
        "x(REAL), per (): y(REAL) SUCH THAT x <= 5 AND y <= 5 MAXIMIZE SUM(a * x + b * y)",
        "MAXIMIZE sum(a * x + b * y)",
        "1e308",
    ),
    (
        "square_in_the_objective",
        "x(REAL) SUCH THAT x <= 5 MINIMIZE SUM(POWER(b * x, 2))",
        "MINIMIZE sum(power(b * x, 2))",
        "1e200",
    ),
    (
        "fixed_terms_of_a_sum",
        "x(REAL) SUCH THAT x <= 5 AND SUM(x + b) <= 10 MAXIMIZE SUM(a * x)",
        "SUM(x + b) <= 10",
        "1e308",
    ),
    (
        "fixed_terms_of_one_row",
        "x(INT) SUCH THAT x <= 5 AND MAX(x + b + b) >= 3 MAXIMIZE SUM(a * x)",
        "MAX(x + b + b) >= 3",
        "1e308",
    ),
    (
        "outer_row_of_a_composed_extremum",
        "x(REAL), per (): y(REAL) SUCH THAT x <= 5 AND y <= 1 AND SUM(b * y) + MAX(x) <= 10 MAXIMIZE SUM(a * x)",
        "SUM(b * y) + MAX(x) <= 10",
        "1e308",
    ),
    # A row a rewrite adds for the objective belongs to no clause: it names the objective.
    (
        "member_of_an_extremum_in_the_objective",
        "x(REAL) SUCH THAT x <= 5 MINIMIZE SUM(x) + MAX(x + b + b)",
        "MINIMIZE sum(x) + max(x + b + b)",
        "1e308",
    ),
    (
        "group_sums_of_a_two_level_objective",
        "x(REAL), per (): y(REAL) SUCH THAT x <= 5 AND y <= 1 MINIMIZE max(per g: sum(b * y) by (g))",
        "MINIMIZE max(per g: sum(b * y) by (g))",
        "1e308",
    ),
]


@pytest.mark.error
@pytest.mark.parametrize("mode", ["off", "auto"])
@pytest.mark.parametrize("cli_fixture", ["decidb_cli_gurobi", "decidb_cli_highs"])
@pytest.mark.parametrize("name,decide,written,big", TOO_LARGE, ids=[case[0] for case in TOO_LARGE])
def test_a_sum_past_a_double_names_its_clause(request, cli_fixture, mode, name, decide, written, big):
    cli = request.getfixturevalue(cli_fixture)
    assert _too_large(written) in _stderr(cli, _big_query(decide, big), mode)


@pytest.mark.error
@pytest.mark.parametrize("mode", ["off", "auto"])
def test_a_square_past_a_double_in_a_constraint_names_its_clause(decidb_cli_gurobi, mode):
    # Gurobi only: HiGHS refuses a quadratic constraint when the query is planned, whatever its numbers.
    decide = "x(REAL) SUCH THAT x <= 5 AND SUM(POWER(b * x, 2)) <= 10 MAXIMIZE SUM(a * x)"
    stderr = _stderr(decidb_cli_gurobi, _big_query(decide, "1e200"), mode)
    assert _too_large("SUM(POWER(b * x, 2)) <= 10") in stderr


def _huge_query(decide):
    # Every value is finite: `b` is near the largest DOUBLE on both rows, and `c` is near it with either sign.
    rows = "(1, 1.0, 1e308::DOUBLE, -1e308::DOUBLE), (2, 2.0, 1e308::DOUBLE, 1e308::DOUBLE)"
    return f"SELECT id FROM (FROM (VALUES {rows}) t(id, a, b, c) DECIDE {decide}) q"


# (name, DECIDE clauses, the clause the message names). Each derives a number past a DOUBLE from finite ones, at a
# different place: a reach or a Big-M a rewrite derives, or a literal coefficient, factor or reciprocal, which has no
# column in it. Each names its clause on both backends, with direct solve off and on.
DERIVED_TOO_LARGE = [
    # A MIN/MAX auxiliary's box reaches past a DOUBLE: the column check at the hand-off names the clause that reads it.
    (
        "reach_of_an_extremum_member",
        "x(REAL), y(REAL) SUCH THAT x <= 5 AND y <= 5 AND SUM(x) + MAX(b * y) <= 10 MAXIMIZE SUM(x)",
        "SUM(x) + MAX(b * y) <= 10",
    ),
    # Members of either sign near the limit: the span of the MIN/MAX link is no Big-M (EmitExtremumLink).
    ("span_of_an_extremum", "y(REAL) SUCH THAT y <= 5 AND MAX(y + c) >= 0 MAXIMIZE SUM(y)", "MAX(y + c) >= 0"),
    # Lowered through ABS on HiGHS (DeriveAbsAuxiliaryBounds), stated natively on Gurobi.
    ("reach_of_a_norm", "y(REAL) SUCH THAT y <= 5 AND norm(b * y, 0) <= 1 MAXIMIZE SUM(y)", "norm(b * y, 0) <= 1"),
    # A literal past a DOUBLE on one decision folds into no box (FoldKnownBound): the row's coefficient check
    # refuses it, as it refuses one beside another decision.
    (
        "literal_on_one_decision",
        "y(REAL) SUCH THAT y <= 5 AND 2e308 * y <= 1e308 MAXIMIZE SUM(y)",
        "2e308 * y <= 1e308",
    ),
    (
        "like_literals_on_one_decision",
        "y(REAL) SUCH THAT y <= 5 AND 1e308 * y + 1e308 * y <= 1e308 MAXIMIZE SUM(y)",
        "1e308 * y + 1e308 * y <= 1e308",
    ),
    (
        "literal_beside_another_decision",
        "y(REAL), z(REAL) SUCH THAT y <= 5 AND z <= 5 AND 2e308 * y + 0.5 * z <= 1e308 MAXIMIZE SUM(y)",
        "2e308 * y + 0.5 * z <= 1e308",
    ),
    (
        "reciprocal_of_a_literal",
        "y(REAL) SUCH THAT y <= 5 AND SUM(y) / 1e-310 <= 10 MAXIMIZE SUM(y)",
        "SUM(y) / 1e-310 <= 10",
    ),
    # A composed MIN/MAX reads its member's coefficients and its factor apart from the rows of a sum.
    (
        "literal_inside_a_composed_extremum",
        "y(REAL) SUCH THAT y <= 5 AND SUM(y) + MAX(2e308 * y) <= 10 MAXIMIZE SUM(y)",
        "SUM(y) + MAX(2e308 * y) <= 10",
    ),
    (
        "literal_factor_on_a_composed_extremum",
        "y(REAL) SUCH THAT y <= 5 AND 2e308 * MAX(y) + SUM(y) <= 10 MAXIMIZE SUM(y)",
        "2e308 * MAX(y) + SUM(y) <= 10",
    ),
    # The reciprocal of a finite factor on the objective's MIN/MAX: the objective check at the hand-off.
    (
        "reciprocal_on_an_extremum_in_the_objective",
        "y(REAL) SUCH THAT y <= 5 MINIMIZE MAX(y) / (SELECT 1e-310)",
        "MINIMIZE max(y) / (SELECT 1e-310)",
    ),
]


@pytest.mark.error
@pytest.mark.parametrize("mode", ["off", "auto"])
@pytest.mark.parametrize("cli_fixture", ["decidb_cli_gurobi", "decidb_cli_highs"])
@pytest.mark.parametrize("name,decide,written", DERIVED_TOO_LARGE, ids=[case[0] for case in DERIVED_TOO_LARGE])
def test_a_number_derived_past_a_double_names_its_clause(request, cli_fixture, mode, name, decide, written):
    cli = request.getfixturevalue(cli_fixture)
    assert _too_large(written) in _stderr(cli, _huge_query(decide), mode)


@pytest.mark.error
@pytest.mark.parametrize("name", ["literal_on_one_decision", "like_literals_on_one_decision"])
def test_a_literal_past_a_double_on_one_decision_names_its_clause_on_both_paths(decidb_cli, name):
    # Direct solve reads the clause as a bound on the decision; its coefficient has no column in it.
    decide, written = next((case[1], case[2]) for case in DERIVED_TOO_LARGE if case[0] == name)
    direct = _stderr(decidb_cli, _huge_query(decide), "require")
    assert _too_large(written) in direct
    assert direct == _stderr(decidb_cli, _huge_query(decide), "off")


@pytest.mark.error
@pytest.mark.parametrize("mode", ["off", "auto"])
def test_a_literal_past_a_double_in_the_objective_names_it_on_the_solver_path(decidb_cli_highs, mode):
    # HiGHS: the solver path either way.
    stderr = _stderr(decidb_cli_highs, _huge_query("y(REAL) SUCH THAT y <= 5 MAXIMIZE SUM(2e308 * y)"), mode)
    assert _too_large("MAXIMIZE sum(1e1000 * y)") in stderr


# HiGHS only: Gurobi states MIN/MAX and ABS natively, so their Big-M rewrites never run there. Every member's box is
# finite, so the refusal names the clause, not a decision to bound.
HIGHS_BIG_M_TOO_LARGE = [
    ("abs_in_the_objective", "y(REAL) SUCH THAT y <= 5 MAXIMIZE SUM(ABS(b * y))", "MAXIMIZE sum(abs(b * y))"),
    ("extremum_bounded_below", "y(REAL) SUCH THAT y <= 5 AND MAX(b * y) >= 1 MINIMIZE SUM(y)", "MAX(b * y) >= 1"),
]


@pytest.mark.error
@pytest.mark.parametrize("mode", ["off", "auto"])
@pytest.mark.parametrize("name,decide,written", HIGHS_BIG_M_TOO_LARGE, ids=[c[0] for c in HIGHS_BIG_M_TOO_LARGE])
def test_a_big_m_past_a_double_names_its_clause(decidb_cli_highs, mode, name, decide, written):
    assert _too_large(written) in _stderr(decidb_cli_highs, _huge_query(decide), mode)


@pytest.mark.error
@pytest.mark.parametrize("mode", ["off", "auto"])
def test_a_big_m_over_an_open_decision_still_asks_for_its_bound(decidb_cli_highs, mode):
    stderr = _stderr(decidb_cli_highs, _huge_query("y(REAL) SUCH THAT MAX(b * y) >= 1 MINIMIZE SUM(y)"), mode)
    assert "Rewriting MIN/MAX requires a finite bound on 'y'." in stderr, stderr


@pytest.mark.error
@pytest.mark.parametrize("mode", ["off", "auto"])
def test_a_reciprocal_past_a_double_on_a_squared_sum_names_its_clause(decidb_cli_gurobi, mode):
    # Gurobi only, as above. A query-wide factor is read when the query runs; its reciprocal is computed then.
    decide = "y(REAL) SUCH THAT y <= 5 AND SUM(POWER(y, 2)) / (SELECT 1e-310) <= 10 MAXIMIZE SUM(y)"
    stderr = _stderr(decidb_cli_gurobi, _huge_query(decide), mode)
    assert _too_large("SUM(POWER(y, 2)) / (SELECT 1e-310) <= 10") in stderr


# The continuous knapsack plan (R1) computes each item's price, value over weight, and the group's sums of the rooms
# and of the anchors' spend. Either past a DOUBLE names the budget.
R1_TOO_LARGE = [
    ("price", "x(REAL) SUCH THAT x <= 5 AND SUM(1e-300 * x) <= 1 MAXIMIZE SUM(b * x)", "SUM(1e-300 * x) <= 1"),
    ("sum_of_the_rooms", "x(REAL) SUCH THAT x <= 5 AND SUM(1e308 * x) <= 1 MAXIMIZE SUM(a * x)", "SUM(1e308 * x) <= 1"),
]


@pytest.mark.error
@pytest.mark.parametrize("mode", ["auto", "require"])
@pytest.mark.parametrize("name,decide,written", R1_TOO_LARGE, ids=[case[0] for case in R1_TOO_LARGE])
def test_a_number_the_knapsack_plan_computes_past_a_double_names_the_budget(decidb_cli, mode, name, decide, written):
    assert _too_large(written) in _stderr(decidb_cli, _huge_query(decide), mode)


# A model whose numbers are each finite, which the solver then reads its own way: the warning stays (the CLI wrapper
# checks it is printed), and so does the solver's verdict.
@pytest.mark.error
@pytest.mark.parametrize("mode", ["off", "auto"])
def test_a_model_of_finite_huge_numbers_keeps_the_solvers_verdict(decidb_cli_gurobi, decidb_cli_highs, mode):
    def stderr(cli, decide):
        sql = f"SET decide_direct_solve='{mode}'; {_huge_query(decide)}"
        return cli.execute_raw(sql, expect_large_numbers=True).stderr

    unbounded = stderr(decidb_cli_gurobi, "y(REAL) SUCH THAT y <= 5 MAXIMIZE SUM(ABS(b * y))")
    assert "DECIDE optimization is unbounded." in unbounded, unbounded
    refused = stderr(decidb_cli_highs, "y(REAL) SUCH THAT y <= 5 MINIMIZE MAX(b * y)")
    assert "mixes numbers too far apart in size for HiGHS to load" in refused, refused


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
