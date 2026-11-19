"""The one message for a NaN or infinite number: every site that refuses one points at the value where it stands in the
query (the caret), and the solver path and the direct-solve plans print the same text and caret.

A coefficient must be finite; a bound may be infinite and must not be NaN; a constant SQL computes as NaN or infinite
(`1 / 0`) is refused whole. A number DeciDB made from the user's text (a quotient's reciprocal, like terms added into
one coefficient, a coefficient under a factor) stands where the text it replaces stands (`DecideReplaces`), so the
error points there. A test of one checks the kind and where the caret lands (`assert_error_at`), not the wording, which
`DecideInvalidValueMessage` (`decide_value_messages.hpp`) owns. A NULL value is skipped, not refused. A number the
model computes across rows (a sum over rows, a square) that is too large for a DOUBLE points at its clause or the
objective.
"""

import pytest
from ._direct_oracle_common import _allow_direct_path  # noqa: F401  (autouse fixture)

NAN = "'NaN'::DOUBLE"
INFINITY = "'Infinity'::DOUBLE"

# A subquery is named as it is written in the query below.
SUBQUERY = "(SELECT max(v) FROM range(2) r(v))"


def _query(decide, bad):
    # The middle row holds the bad value in `b`.
    rows = f"(1, 1.0, 2.0, 3.0, 'g'), (2, 2.0, {bad}, 1.0, 'g'), (3, 3.0, 1.0, 2.0, 'h')"
    return f"SELECT id FROM (FROM (VALUES {rows}) t(id, a, b, c, g) DECIDE {decide}) q"


INVALID_INPUT = "Invalid Input Error"


def _stderr(cli, sql, mode):
    return cli.execute_raw(f"SET decide_direct_solve='{mode}'; {sql}").stderr


def _points_at(cli, sql, mode, at, word=None):
    """`sql` under `mode` fails with an Invalid Input Error whose caret stands inside `at`, the value as written."""
    cli.assert_error_at(f"SET decide_direct_solve='{mode}'; {sql}", kind=INVALID_INPUT, at=at, word=word)


# A forced backend (`decidb_cli_gurobi`, `decidb_cli_highs`) keeps the solver path whatever the mode says
# (`ChooseDecideAnswer`), so those tests run `off` only. `auto` and `require` end differently only when no rule takes
# the query (`ApplyDirectSolveRules`), so a query a rule takes runs the same under both: `require` stands for them.


# (name, DECIDE clauses, the value the message names). Both paths take each one. A coefficient DeciDB computes (like
# terms merged, a reducer's factor applied) stands where the text it replaces stands.
BOTH_PATHS_COEFFICIENT = [
    ("a1_constraint", "x(REAL) SUCH THAT b * x <= 5 MAXIMIZE SUM(a * x)", "b"),
    ("a1_objective", "x(REAL) SUCH THAT x <= 5 MAXIMIZE SUM(b * x)", "b"),
    ("a1_like_terms_merge", "x(REAL) SUCH THAT x <= 5 MAXIMIZE SUM(b * x + a * x)", "b * x + a * x"),
    ("a1_like_terms_subtract", "x(REAL) SUCH THAT x <= 5 MAXIMIZE SUM(a * x - b * x)", "a * x - b * x"),
    ("a1_factor_on_the_sum", "x(REAL) SUCH THAT x <= 5 MAXIMIZE 2 * SUM(b * x)", "2 * SUM(b * x)"),
    ("a1_divisor_of_the_sum", "x(REAL) SUCH THAT x <= 5 MAXIMIZE SUM(b * x) / 2", "SUM(b * x) / 2"),
    ("a1_constant_term", "x(REAL) SUCH THAT x <= 5 MAXIMIZE SUM(a * x + b)", "b"),
    # Each constant term is checked on its own: the first that is not finite is named.
    ("a1_two_constant_terms", "x(REAL) SUCH THAT x <= 5 MAXIMIZE SUM(a * x + b + b)", "b"),
    ("a1_inside_a_square", "x(REAL) SUCH THAT x <= 5 MINIMIZE SUM(POWER(x - b, 2))", "b"),
    ("a1_under_a_when", "x(REAL) SUCH THAT x <= 5 MAXIMIZE SUM(when a > 0: b * x)", "b"),
    (
        "a1_query_wide_factor",
        "x(REAL) SUCH THAT x <= 5 MAXIMIZE SUM((SELECT max(v) FROM range(2) r(v)) * b * x)",
        f"{SUBQUERY} * b",
    ),
    ("r1_budget_weight", "x(REAL) SUCH THAT x <= 4 AND SUM(b * x) <= 9 MAXIMIZE SUM(a * x)", "b"),
    ("r1_objective_value", "x(REAL) SUCH THAT x <= 4 AND SUM(a * x) <= 9 MAXIMIZE SUM(b * x)", "b"),
    (
        "r1_factor_on_the_value",
        "x(REAL) SUCH THAT x <= 4 AND SUM(a * x) <= 9 MAXIMIZE 2 * SUM(b * x)",
        "2 * SUM(b * x)",
    ),
    (
        "r1_like_terms_merge",
        "x(REAL) SUCH THAT x <= 4 AND SUM(a * x + b * x) <= 9 MAXIMIZE SUM(c * x)",
        "a * x + b * x",
    ),
    ("r1_one_of_two_reducers", "x(REAL) SUCH THAT x <= 4 AND SUM(a * x) <= 9 MAXIMIZE SUM(a * x) + SUM(b * x)", "b"),
    ("r1_per_row_clause", "x(REAL) SUCH THAT b * x <= 4 AND SUM(a * x) <= 9 MAXIMIZE SUM(c * x)", "b"),
    ("s1_score", "x(BOOL) SUCH THAT SUM(x) <= 2 MAXIMIZE SUM(b * x)", "b"),
    ("s1_factor_on_the_score", "x(BOOL) SUCH THAT SUM(x) <= 2 MAXIMIZE 2 * SUM(b * x)", "2 * SUM(b * x)"),
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

# (name, DECIDE clauses, the bound the message names). A bound is refused only when NaN: each case is another place the
# plans and the solver path read a bound (a row's own bound, a reducer's limit, a group's, a count's, a fixing value).
BOTH_PATHS_BOUND = [
    ("a1_bound", "x(REAL) SUCH THAT x <= b MAXIMIZE SUM(a * x)", "b"),
    ("a1_lower_bound", "x(REAL) SUCH THAT x <= 5 AND x >= b MAXIMIZE SUM(a * x)", "b"),
    ("a1_bound_expression", "x(REAL) SUCH THAT x <= b + 1 MAXIMIZE SUM(a * x)", "b + 1"),
    ("a1_bound_under_a_when", "x(REAL) SUCH THAT when a > 0: x <= b MAXIMIZE SUM(a * x)", "b"),
    ("r1_limit", "x(REAL) SUCH THAT x <= 4 AND SUM(a * x) <= b MAXIMIZE SUM(c * x)", "b"),
    ("r1_limit_expression", "x(REAL) SUCH THAT x <= 4 AND SUM(a * x) <= b + 2 MAXIMIZE SUM(c * x)", "b + 2"),
    ("r1_per_row_bound", "x(REAL) SUCH THAT x <= b AND SUM(a * x) <= 9 MAXIMIZE SUM(c * x)", "b"),
    ("r1_limit_by_group", "x(REAL) SUCH THAT x <= 4 AND SUM(a * x) by (g) <= SUM(b) by (g) MAXIMIZE SUM(c * x)", "b"),
    ("s1_count_bound", "x(BOOL) SUCH THAT SUM(x) <= b MAXIMIZE SUM(a * x)", "b"),
    ("s1_count_bound_expression", "x(BOOL) SUCH THAT SUM(x) <= b + 1 MAXIMIZE SUM(a * x)", "b + 1"),
    ("s1_fixing_value", "x(BOOL) SUCH THAT SUM(x) <= 2 AND x >= b MAXIMIZE SUM(a * x)", "b"),
    ("data_reducer_argument", "x(BOOL) SUCH THAT SUM(x) <= MIN(b) MAXIMIZE SUM(a * x)", "b"),
    ("data_reducer_sum_argument", "x(BOOL) SUCH THAT SUM(x) <= SUM(b) MAXIMIZE SUM(a * x)", "b"),
]


# Every site that refuses a coefficient tests `isfinite` (`ExtractDoubleColumn`, the direct plans' `isfinite` checks),
# so NaN and an infinity reach one branch there. NaN runs on every case; the infinity runs once per plan family
# (A1, R1, S1).
_INFINITE_COEFFICIENT = {"a1_constraint", "r1_budget_weight", "s1_score"}
COEFFICIENT_CASES = [
    pytest.param(name, decide, value, bad, id=f"{name}-{label}")
    for name, decide, value in BOTH_PATHS_COEFFICIENT
    for label, bad in (("nan", NAN), ("infinity", INFINITY))
    if label == "nan" or name in _INFINITE_COEFFICIENT
]


@pytest.mark.error
@pytest.mark.parametrize("name,decide,value,bad", COEFFICIENT_CASES)
def test_a_coefficient_that_is_not_finite_is_named_as_written_on_both_paths(decidb_cli, name, decide, value, bad):
    direct = _stderr(decidb_cli, _query(decide, bad), "require")
    solver = _stderr(decidb_cli, _query(decide, bad), "off")
    _points_at(decidb_cli, _query(decide, bad), "off", value)
    assert direct == solver


# A raise call carries its own place (its `query_location`), so a plan that serializes keeps the caret. The harness runs
# the query again on the plan's deserialized copy (`PRAGMA verify_serializer`). The cases reach three raises: a direct
# plan's coefficient check and bound check (`RaiseInvalidValue`), and the solver path's check of a coefficient, which the
# DECIDE node keeps as a scope check.
SERIALIZED_CARET_CASES = [
    ("direct_coefficient", "require", "x(REAL) SUCH THAT b * x <= 5 MAXIMIZE SUM(a * x)", "b"),
    ("solver_coefficient", "off", "x(REAL) SUCH THAT b * x <= 5 MAXIMIZE SUM(a * x)", "b"),
    ("direct_bound", "require", "x(REAL) SUCH THAT x <= b MAXIMIZE SUM(a * x)", "b"),
]


@pytest.mark.error
@pytest.mark.parametrize(
    "mode,decide,at", [c[1:] for c in SERIALIZED_CARET_CASES], ids=[c[0] for c in SERIALIZED_CARET_CASES]
)
def test_a_serialized_plan_keeps_the_caret_of_an_invalid_value(decidb_cli, mode, decide, at):
    sql = f"SET decide_direct_solve='{mode}'; {_query(decide, NAN)}"
    decidb_cli.with_verify_serializer().assert_error_at(sql, kind=INVALID_INPUT, at=at)


class At(str):
    """The expression of the query an error points at (`_points_at`), where a case expects the error of a value that is
    not finite rather than a clause named as too large."""


class TooLarge(str):
    """The clause, or the objective, a sum or square past a DOUBLE belongs to: the error points at it (`_refused`)."""


def _refused(cli, sql, mode, expected):
    """`sql` fails as `expected` says: pointing at an `At` expression, or at the clause (the objective) a sum past a
    DOUBLE belongs to, a `TooLarge` or a plain string."""
    if isinstance(expected, At):
        _points_at(cli, sql, mode, str(expected))
    else:
        _points_at(cli, sql, mode, str(expected), "computes a number too large for a DOUBLE")


# Values each finite whose sum or square is not, on the middle row. Both paths print one text.
DIRECT_OVERFLOW = [
    # The squared coefficient of a decision, `b * b`, is beyond a double. No value of a row is, so the error points at
    # the objective.
    (
        "a1_square_overflows",
        "x(REAL) SUCH THAT x <= 5 MINIMIZE SUM(POWER(b * x, 2))",
        TooLarge("SUM(POWER(b * x, 2))"),
        "1e200",
    ),
    (
        "a1_square_and_linear_overflow",
        "x(REAL) SUCH THAT x <= 5 MINIMIZE SUM(POWER(b * x, 2) + a * x)",
        TooLarge("SUM(POWER(b * x, 2) + a * x)"),
        "1e200",
    ),
    # Like terms on one row merge into that row's coefficient, `b + b`, which stands where their sum is written. Each
    # value is finite, and the merged coefficient is not.
    (
        "s1_score_overflows",
        "x(BOOL) SUCH THAT SUM(x) <= 2 MAXIMIZE SUM(b * x) + SUM(b * x)",
        At("SUM(b * x) + SUM(b * x)"),
        "1e308",
    ),
    (
        "a1_coefficient_overflows",
        "x(REAL) SUCH THAT x <= 5 MAXIMIZE SUM(b * x) + SUM(b * x)",
        At("SUM(b * x) + SUM(b * x)"),
        "1e308",
    ),
]


@pytest.mark.error
@pytest.mark.parametrize("name,decide,message,big", DIRECT_OVERFLOW, ids=[case[0] for case in DIRECT_OVERFLOW])
def test_a_sum_the_plan_computes_that_overflows_is_named_as_written(decidb_cli, name, decide, message, big):
    direct = _stderr(decidb_cli, _query(decide, big), "require")
    solver = _stderr(decidb_cli, _query(decide, big), "off")
    _refused(decidb_cli, _query(decide, big), "require", message)
    assert direct == solver


# A coefficient that is a constant, NaN or infinite itself: SQL computes `1 / 0` as infinity, so no row holds it and no
# `when` skips it. Both paths point at the division the user wrote.
BOTH_PATHS_CONSTANT = [
    ("a1_objective", "x(REAL) SUCH THAT x <= 5 MAXIMIZE SUM(x / 0)", "x / 0"),
    ("a1_infinite_literal", f"x(REAL) SUCH THAT x <= 5 MAXIMIZE SUM(x * {INFINITY})", INFINITY),
    ("r1_objective", "x(REAL) SUCH THAT x <= 4 AND SUM(a * x) <= 9 MAXIMIZE SUM(x / 0)", "x / 0"),
    ("r1_budget_weight", "x(REAL) SUCH THAT x <= 4 AND SUM(x / 0) <= 9 MAXIMIZE SUM(c * x)", "x / 0"),
    ("s1_score", "x(BOOL) SUCH THAT SUM(x) <= 2 MAXIMIZE SUM(x / 0)", "x / 0"),
]


@pytest.mark.error
@pytest.mark.parametrize("name,decide,value", BOTH_PATHS_CONSTANT, ids=[case[0] for case in BOTH_PATHS_CONSTANT])
def test_a_constant_coefficient_that_is_not_finite_is_named_on_both_paths(decidb_cli, name, decide, value):
    direct = _stderr(decidb_cli, _query(decide, "1.0"), "require")
    solver = _stderr(decidb_cli, _query(decide, "1.0"), "off")
    _points_at(decidb_cli, _query(decide, "1.0"), "off", value)
    assert direct == solver


# A division by zero makes the coefficient infinite: the error points at the user's division, whether the zero is a
# literal or a value of a column, in a constraint, the objective or a reducer under MIN.
DIVISIONS_BY_ZERO = [
    ("sum_in_the_objective", "x(REAL) SUCH THAT x <= 5 MAXIMIZE SUM(x / 0)", "x / 0", "1.0"),
    ("comparison_in_a_constraint", "x(REAL) SUCH THAT x / 0 <= 4 MAXIMIZE SUM(a * x)", "x / 0", "1.0"),
    (
        "min_in_the_objective",
        "amount(REAL) SUCH THAT amount <= c AND SUM(amount) <= 9 MAXIMIZE MIN(amount / d)",
        "amount / d",
        "0.0",
    ),
    ("sum_over_a_column", "y(INT) SUCH THAT y <= 6 AND SUM(y / d) <= 30 MAXIMIZE SUM(y)", "y / d", "0.0"),
]


@pytest.mark.error
@pytest.mark.parametrize("mode", ["off", "auto"])
@pytest.mark.parametrize("name,decide,at,d", DIVISIONS_BY_ZERO, ids=[case[0] for case in DIVISIONS_BY_ZERO])
def test_a_division_by_zero_points_at_the_division_the_user_wrote(decidb_cli, name, decide, at, d, mode):
    rows = f"(1, 1.0, 2.0, 3.0), (2, 2.0, {d}, 1.0), (3, 3.0, 1.0, 2.0)"
    sql = f"SELECT id FROM (FROM (VALUES {rows}) t(id, a, d, c) DECIDE {decide}) q"
    _points_at(decidb_cli, sql, mode, at)


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
        "SUM(a * x + b * y)",
        "1e308",
    ),
    (
        "square_in_the_objective",
        "x(REAL) SUCH THAT x <= 5 MINIMIZE SUM(POWER(b * x, 2))",
        "SUM(POWER(b * x, 2))",
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
        "SUM(x) + MAX(x + b + b)",
        "1e308",
    ),
    # The data of an ABS' argument, `-b - b`, is not finite though each `b` is: raised where the ABS is read, naming the
    # clause as written, on each way the ABS is stated (pressed by the objective, Big-M, or a constraint).
    (
        "abs_the_objective_presses_down",
        "x(REAL) SUCH THAT x BETWEEN 0 AND 10 MINIMIZE SUM(ABS(x - b - b))",
        At("x - b - b"),
        "1e308",
    ),
    (
        "abs_the_objective_presses_up",
        "x(REAL) SUCH THAT x BETWEEN 0 AND 10 MAXIMIZE SUM(ABS(x - b - b))",
        At("x - b - b"),
        "1e308",
    ),
    (
        "abs_in_a_constraint",
        "x(REAL) SUCH THAT x BETWEEN 0 AND 10 AND ABS(x - b - b) >= 1 MINIMIZE SUM(x)",
        At("x - b - b"),
        "1e308",
    ),
    # A nested objective's group sums are a helper decision (D81), each read by one member of the maximum, so written into
    # it (D89): the maximum's member computes the sum, and it names the clause that first read the helper, the objective,
    # as written.
    (
        "group_sums_of_a_nested_objective",
        "x(REAL), per (): y(REAL) SUCH THAT x <= 5 AND y <= 1 MINIMIZE max(per g: sum(b * y) by (g))",
        "max(per g: sum(b * y) by (g))",
        "1e308",
    ),
]


@pytest.mark.error
@pytest.mark.parametrize("mode", ["off"])
@pytest.mark.parametrize("cli_fixture", ["decidb_cli_gurobi", "decidb_cli_highs"])
@pytest.mark.parametrize("name,decide,written,big", TOO_LARGE, ids=[case[0] for case in TOO_LARGE])
def test_a_sum_past_a_double_names_its_clause(request, cli_fixture, mode, name, decide, written, big):
    cli = request.getfixturevalue(cli_fixture)
    _refused(cli, _big_query(decide, big), mode, written)


# The tree evaluator names a root as flat names its row (DecideWrittenClause): a composed clause by its qualifier with
# it, and a row a rewrite added for the objective, which no clause claims, by the objective. A clause is named as
# written in test_a_sum_past_a_double_names_its_clause.
TREE_WRITTEN = [
    (
        "composed_extremum_under_a_when",
        "x(REAL), per (): y(REAL) SUCH THAT x <= 5 AND y <= 1 AND when a > 0: SUM(b * y) + MAX(x) <= 10 "
        "MAXIMIZE SUM(a * x)",
        "SUM(b * y) + MAX(x) <= 10",
        "1e308",
    ),
    (
        "row_of_an_abs_in_the_objective",
        "per (): y(REAL) SUCH THAT y <= 5 AND y >= -5 MAXIMIZE SUM(ABS(b * y + b * y))",
        At("b * y + b * y"),
        "1e308",
    ),
]


@pytest.mark.error
@pytest.mark.parametrize("name,decide,written,big", TREE_WRITTEN, ids=[case[0] for case in TREE_WRITTEN])
def test_the_tree_names_a_root_as_flat_names_its_row(decidb_cli, name, decide, written, big):
    _refused(decidb_cli, _big_query(decide, big), "off", written)


@pytest.mark.error
@pytest.mark.parametrize("mode", ["off"])
def test_a_square_past_a_double_in_a_constraint_names_its_clause(decidb_cli_gurobi, mode):
    # Gurobi only: HiGHS refuses a quadratic constraint when the query is planned, whatever its numbers.
    decide = "x(REAL) SUCH THAT x <= 5 AND SUM(POWER(b * x, 2)) <= 10 MAXIMIZE SUM(a * x)"
    _refused(decidb_cli_gurobi, _big_query(decide, "1e200"), mode, TooLarge("SUM(POWER(b * x, 2)) <= 10"))


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
    # The same reach on the objective's MIN/MAX: the one signed walk sizes it as it sizes a composed term.
    (
        "reach_of_an_extremum_in_the_objective",
        "y(REAL) SUCH THAT y <= 5 MINIMIZE MAX(b * y)",
        "MAX(b * y)",
    ),
    # A helper decision's reach (the two-level form's group sums) overflows at one end of its box, which stays open
    # there: refused where the helper is defined (RefuseOverflowedReach), each end on its own.
    (
        "reach_of_a_group_sum_extremum",
        "y(REAL) SUCH THAT y <= 5 MINIMIZE MAX(per id: SUM(b * y) BY (id))",
        "MAX(per id: SUM(b * y) BY (id))",
    ),
    (
        "floor_of_a_group_sum_extremum",
        "y(REAL) SUCH THAT y >= -5 AND y <= 0 MINIMIZE MAX(per id: SUM(b * y) BY (id))",
        "MAX(per id: SUM(b * y) BY (id))",
    ),
    # Members of either sign near the limit: the span of the MIN/MAX link is no Big-M (EmitExtremumLink).
    ("span_of_an_extremum", "y(REAL) SUCH THAT y <= 5 AND MAX(y + c) >= 0 MAXIMIZE SUM(y)", "MAX(y + c) >= 0"),
    # A MIN/MAX bounded from its far side: the Big-M on HiGHS, the native auxiliary's box on Gurobi.
    ("extremum_bounded_below", "y(REAL) SUCH THAT y <= 5 AND MAX(b * y) >= 1 MINIMIZE SUM(y)", "MAX(b * y) >= 1"),
    # ABS the objective pushes up: the Big-M on HiGHS, the native auxiliary's box on Gurobi.
    ("abs_in_the_objective", "y(REAL) SUCH THAT y <= 5 MAXIMIZE SUM(ABS(b * y))", "MAXIMIZE SUM(ABS(b * y))"),
    # Lowered through ABS on HiGHS (DeriveAbsAuxiliaryBounds), stated natively on Gurobi.
    ("reach_of_a_norm", "y(REAL) SUCH THAT y <= 5 AND norm(b * y, 0) <= 1 MAXIMIZE SUM(y)", "norm(b * y, 0) <= 1"),
    # A literal past a DOUBLE on one decision folds into no box (FoldKnownBound): the row's coefficient check
    # refuses it, as it refuses one beside another decision.
    (
        "literal_on_one_decision",
        "y(REAL) SUCH THAT y <= 5 AND 2e308 * y <= 1e308 MAXIMIZE SUM(y)",
        At("2e308"),
    ),
    (
        "like_literals_on_one_decision",
        "y(REAL) SUCH THAT y <= 5 AND 1e308 * y + 1e308 * y <= 1e308 MAXIMIZE SUM(y)",
        At("1e308 * y + 1e308 * y"),
    ),
    (
        "literal_beside_another_decision",
        "y(REAL), z(REAL) SUCH THAT y <= 5 AND z <= 5 AND 2e308 * y + 0.5 * z <= 1e308 MAXIMIZE SUM(y)",
        At("2e308"),
    ),
    (
        "reciprocal_of_a_literal",
        "y(REAL) SUCH THAT y <= 5 AND SUM(y) / 1e-310 <= 10 MAXIMIZE SUM(y)",
        At("SUM(y) / 1e-310"),
    ),
    # A composed MIN/MAX reads its member's coefficients and its factor apart from the rows of a sum.
    (
        "literal_inside_a_composed_extremum",
        "y(REAL) SUCH THAT y <= 5 AND SUM(y) + MAX(2e308 * y) <= 10 MAXIMIZE SUM(y)",
        At("2e308"),
    ),
    (
        "literal_factor_on_a_composed_extremum",
        "y(REAL) SUCH THAT y <= 5 AND 2e308 * MAX(y) + SUM(y) <= 10 MAXIMIZE SUM(y)",
        At("2e308"),
    ),
    # The reciprocal of a finite factor on the objective's MIN/MAX: the objective check at the hand-off.
    (
        "reciprocal_on_an_extremum_in_the_objective",
        "y(REAL) SUCH THAT y <= 5 MINIMIZE MAX(y) / (SELECT 1e-310)",
        "MAX(y) / (SELECT 1e-310)",
    ),
]


@pytest.mark.error
@pytest.mark.parametrize("mode", ["off"])
@pytest.mark.parametrize("cli_fixture", ["decidb_cli_gurobi", "decidb_cli_highs"])
@pytest.mark.parametrize("name,decide,written", DERIVED_TOO_LARGE, ids=[case[0] for case in DERIVED_TOO_LARGE])
def test_a_number_derived_past_a_double_names_its_clause(request, cli_fixture, mode, name, decide, written):
    cli = request.getfixturevalue(cli_fixture)
    _refused(cli, _huge_query(decide), mode, written)


@pytest.mark.error
@pytest.mark.parametrize("name", ["literal_on_one_decision", "like_literals_on_one_decision"])
def test_a_literal_past_a_double_on_one_decision_names_its_clause_on_both_paths(decidb_cli, name):
    # Direct solve reads the clause as a bound on the decision; its coefficient has no column in it.
    decide, written = next((case[1], case[2]) for case in DERIVED_TOO_LARGE if case[0] == name)
    direct = _stderr(decidb_cli, _huge_query(decide), "require")
    _refused(decidb_cli, _huge_query(decide), "require", written)
    assert direct == _stderr(decidb_cli, _huge_query(decide), "off")


@pytest.mark.error
@pytest.mark.parametrize("mode", ["off"])
def test_a_literal_past_a_double_in_the_objective_names_it_on_the_solver_path(decidb_cli_highs, mode):
    # HiGHS: the solver path either way.
    _refused(decidb_cli_highs, _huge_query("y(REAL) SUCH THAT y <= 5 MAXIMIZE SUM(2e308 * y)"), mode, At("2e308"))


@pytest.mark.error
@pytest.mark.parametrize("mode", ["off"])
def test_a_helper_reach_past_a_double_is_not_refused_where_a_clause_bounds_it(decidb_cli_gurobi, mode):
    # Gurobi only: HiGHS refuses to load the range. The clause bounds the group sums at 5, so their reach past a
    # DOUBLE leaves no open end to refuse (RefuseOverflowedReach).
    decide = "y(REAL) SUCH THAT y <= 5 AND MAX(per id: SUM(b * y) BY (id)) <= 5 MAXIMIZE SUM(y)"
    sql = f"SET decide_direct_solve='{mode}'; {_huge_query(decide)}"
    stderr = decidb_cli_gurobi.execute_raw(sql, expect_number_range=True).stderr
    assert "Error" not in stderr, stderr


# HiGHS only: Gurobi states the closing side of a MIN/MAX natively, so it has no Big-M to overflow. The helper's members
# span `-1e308` to `1e308 + 5`, so its Big-M (EmitExtremumLink) is past a DOUBLE while its own reach is not: the
# refusal names the clause as the user wrote it, not the helper's plan label.
@pytest.mark.error
@pytest.mark.parametrize("mode", ["off"])
def test_a_helper_big_m_past_a_double_names_the_clause_as_written(decidb_cli_highs, mode):
    decide = "y(REAL) SUCH THAT y <= 5 AND MAX(per id: SUM(y + c) BY (id)) >= 0 MAXIMIZE SUM(y)"
    _refused(decidb_cli_highs, _huge_query(decide), mode, TooLarge("MAX(per id: SUM(y + c) BY (id)) >= 0"))


# A helper decision (the two-level form's group sum or extremum) is defined where its first reader reads it, and a sum
# of its members past a DOUBLE names that clause as written, not the reducer as the plan prints it (lowercased, with
# its `by`). `by (b)` puts both rows in one group, and each of `b`'s 1e308 is finite. A sum read twice costs a column
# and a definition (InliningAddsNoNonzero), so those cases read it in two clauses. (name, DECIDE clauses, the clause
# the message names, the backends that reach the site.)
HELPER_SUMS = [
    # SparseCoeffAccumulator::BeginForDecisions of a member: one row's two terms read `y` at the same row.
    (
        "like_terms_in_an_extremum_member",
        "y(REAL) SUCH THAT y <= 5 AND MAX(PER (): MAX(b * y + b * AT(FIRST: y) OVER (a)) BY (b)) >= 0 "
        "MAXIMIZE SUM(y)",
        "MAX(PER (): MAX(b * y + b * AT(FIRST: y) OVER (a)) BY (b)) >= 0",
        ["decidb_cli_gurobi"],
    ),
    # BeginForDecisions of a group's sum: the group's rows read the query-wide `s` with `b` each.
    (
        "like_terms_in_a_group_sum",
        "y(REAL), z(REAL), PER (): s(REAL) SUCH THAT y <= 5 AND z <= 5 AND s <= 5 AND "
        "MAX(PER (): SUM(y + z + b * s) BY (b)) >= 0 AND MAX(PER (): SUM(y + z + b * s) BY (b)) <= 10 "
        "MAXIMIZE SUM(y)",
        "MAX(PER (): SUM(y + z + b * s) BY (b)) >= 0",
        ["decidb_cli_gurobi", "decidb_cli_highs"],
    ),
    # AddCoefficient on the constants of a group's sum: `b` of each row.
    (
        "constants_of_a_group_sum",
        "y(REAL), z(REAL) SUCH THAT y <= 5 AND z <= 5 AND MAX(PER (): SUM(y + z + b) BY (b)) >= 0 AND "
        "MAX(PER (): SUM(y + z + b) BY (b)) <= 10 MAXIMIZE SUM(y)",
        "MAX(PER (): SUM(y + z + b) BY (b)) >= 0",
        ["decidb_cli_gurobi", "decidb_cli_highs"],
    ),
    # DecideTreeColumns::Numbers: one member's coefficient of `s`, `b + b`, is past a DOUBLE where it is read.
    (
        "coefficient_of_a_member",
        "y(REAL), PER (): s(REAL) SUCH THAT y <= 5 AND s <= 5 AND MAX(PER (): MAX(b * s + b * s + y) BY (b)) >= 0 "
        "MAXIMIZE SUM(y)",
        At("b * s + b * s"),
        ["decidb_cli_gurobi", "decidb_cli_highs"],
    ),
    # DecideModelEmitter::ValueLinks: one member's constant, `b + b`, is past a DOUBLE.
    (
        "constants_of_a_member",
        "y(REAL) SUCH THAT y <= 5 AND MAX(PER (): SUM(y + b + b) BY (b)) >= 0 MAXIMIZE SUM(y)",
        "MAX(PER (): SUM(y + b + b) BY (b)) >= 0",
        ["decidb_cli_gurobi", "decidb_cli_highs"],
    ),
]


@pytest.mark.error
@pytest.mark.parametrize("mode", ["off"])
@pytest.mark.parametrize("name,decide,written,backends", HELPER_SUMS, ids=[case[0] for case in HELPER_SUMS])
def test_a_helper_sum_past_a_double_names_the_clause_as_written(request, mode, name, decide, written, backends):
    for backend in backends:
        _refused(request.getfixturevalue(backend), _huge_query(decide), mode, written)


# HiGHS only: Gurobi states MIN/MAX natively and needs no bound on `y`. An open end asks for its bound, not the clause.
@pytest.mark.error
@pytest.mark.parametrize("mode", ["off"])
def test_a_big_m_over_an_open_decision_still_asks_for_its_bound(decidb_cli_highs, mode):
    _points_at(
        decidb_cli_highs,
        _huge_query("y(REAL) SUCH THAT MAX(b * y) >= 1 MINIMIZE SUM(y)"),
        mode,
        "MAX(b * y) >= 1",
        "'y' has no finite bound, which MIN or MAX needs",
    )


@pytest.mark.error
@pytest.mark.parametrize("mode", ["off"])
def test_a_reciprocal_past_a_double_on_a_squared_sum_names_its_clause(decidb_cli_gurobi, mode):
    # Gurobi only: HiGHS has no quadratic constraints. A query-wide factor is read when the query runs; its
    # reciprocal is computed then.
    decide = "y(REAL) SUCH THAT y <= 5 AND SUM(POWER(y, 2)) / (SELECT 1e-310) <= 10 MAXIMIZE SUM(y)"
    _refused(decidb_cli_gurobi, _huge_query(decide), mode, At("SUM(POWER(y, 2)) / (SELECT 1e-310)"))


# The continuous knapsack plan (R1) computes each item's price, value over weight, and the group's sums of the rooms
# and of the anchors' spend. Either past a DOUBLE names the budget.
R1_TOO_LARGE = [
    ("price", "x(REAL) SUCH THAT x <= 5 AND SUM(1e-300 * x) <= 1 MAXIMIZE SUM(b * x)", "SUM(1e-300 * x) <= 1"),
    ("sum_of_the_rooms", "x(REAL) SUCH THAT x <= 5 AND SUM(1e308 * x) <= 1 MAXIMIZE SUM(a * x)", "SUM(1e308 * x) <= 1"),
]


@pytest.mark.error
@pytest.mark.parametrize("mode", ["require"])
@pytest.mark.parametrize("name,decide,written", R1_TOO_LARGE, ids=[case[0] for case in R1_TOO_LARGE])
def test_a_number_the_knapsack_plan_computes_past_a_double_names_the_budget(decidb_cli, mode, name, decide, written):
    _refused(decidb_cli, _huge_query(decide), mode, TooLarge(written))


def _derived_query(decide, outcome):
    # `b` is near the largest DOUBLE on both rows, so DeciDB's own arithmetic on it leaves the DOUBLE range, or it is
    # infinite on the second row, which the message then names.
    second = "1e308" if outcome == "too_large" else "'Infinity'"
    first = "1e308" if outcome == "too_large" else "1.0"
    rows = f"(1, 1.0, {first}::DOUBLE, 1.0), (2, 2.0, {second}::DOUBLE, 2.0)"
    return f"SELECT id FROM (FROM (VALUES {rows}) t(id, a, b, c) DECIDE {decide}) q"


# (name, DECIDE clauses, the expression the error points at). Each derives a coefficient from the values it reads at a
# different place: like terms merged or a reducer's factor applied on the solver path, and the sum the direct plan
# checks (DirectNumberCheck). The coefficient DeciDB makes stands where the user's text it replaces stands
# (DecideReplaces): the sum of the like terms, or the scaled reducer. The same query raises the same error when every
# value is finite and the sum is not, and when a value is infinite, on every path that takes it.
DERIVED_BOTH_PATHS = [
    # Solver: like terms merged on a row of a constraint. A1: the clause's coefficient.
    ("a1_like_terms_on_one_row", "y(REAL) SUCH THAT y <= 5 AND b * y + b * y <= 1 MAXIMIZE SUM(y)", "b * y + b * y"),
    # Solver: like terms merged in the objective. A1: one reducer's sum of a decision's terms.
    ("a1_like_terms_in_a_reducer", "y(REAL) SUCH THAT y <= 5 MAXIMIZE SUM(b * y + b * y)", "b * y + b * y"),
    # Solver: the objective's factor applied. A1: the parts' sum, scaled, after each part's values were checked.
    ("a1_factor_on_a_reducer", "y(REAL) SUCH THAT y <= 5 MAXIMIZE 2 * SUM(b * y)", "2 * SUM(b * y)"),
    # Solver: a scaled coefficient merged with a like term, whose values it already holds.
    (
        "a1_factor_then_like_terms",
        "y(REAL) SUCH THAT y <= 5 MAXIMIZE 2 * SUM(b * y) + SUM(b * y)",
        "2 * SUM(b * y) + SUM(b * y)",
    ),
    # Solver: the objective's factor or divisor applied to a constant term. A1: each constant term, scaled.
    ("a1_factor_on_a_constant", "y(REAL) SUCH THAT y <= 5 MAXIMIZE 2 * SUM(y + b)", "2 * SUM(y + b)"),
    ("a1_divisor_of_a_constant", "y(REAL) SUCH THAT y <= 5 MAXIMIZE SUM(y + b) / 0.5", "SUM(y + b) / 0.5"),
    # S1: one reducer's share of the score.
    ("s1_like_terms_in_a_reducer", "y(BOOL) SUCH THAT SUM(y) <= 1 MAXIMIZE SUM(b * y + b * y)", "b * y + b * y"),
    # R1: a per-row clause's coefficient, the budget's weight, an objective part's value, and the item's value.
    (
        "r1_like_terms_on_one_row",
        "y(REAL) SUCH THAT y <= 5 AND b * y + b * y <= 1 AND SUM(a * y) <= 9 MAXIMIZE SUM(a * y)",
        "b * y + b * y",
    ),
    (
        "r1_like_terms_in_the_budget",
        "y(REAL) SUCH THAT y <= 5 AND SUM(b * y + b * y) <= 9 MAXIMIZE SUM(a * y)",
        "b * y + b * y",
    ),
    (
        "r1_like_terms_in_the_objective",
        "y(REAL) SUCH THAT y <= 5 AND SUM(a * y) <= 9 MAXIMIZE SUM(b * y + b * y)",
        "b * y + b * y",
    ),
    (
        "r1_factor_on_the_objective",
        "y(REAL) SUCH THAT y <= 5 AND SUM(a * y) <= 9 MAXIMIZE 2 * SUM(b * y)",
        "2 * SUM(b * y)",
    ),
]

# Sites only the solver path reads.
DERIVED_SOLVER = [
    # A divisor known when the query runs, applied to a constraint's coefficient.
    (
        "divisor_known_at_run_time",
        "y(REAL) SUCH THAT y <= 5 AND SUM(b * y) / (SELECT 0.5) <= 10 MAXIMIZE SUM(y)",
        "SUM(b * y) / (SELECT 0.5)",
    ),
    # Like terms merged inside a composed MIN or MAX, in a constraint and in the objective.
    (
        "composed_extremum_in_a_constraint",
        "y(REAL) SUCH THAT y <= 5 AND SUM(y) + MAX(b * y + b * y) <= 10 MAXIMIZE SUM(y)",
        "b * y + b * y",
    ),
    (
        "composed_extremum_in_the_objective",
        "y(REAL) SUCH THAT y <= 5 MINIMIZE SUM(y) + MAX(b * y + b * y)",
        "b * y + b * y",
    ),
    # Like terms merged in a reducer a row-varying factor scales, which a helper decision holds.
    (
        "reducer_under_a_row_factor",
        "y(REAL) SUCH THAT y <= 5 AND c * SUM(b * y + b * y) <= 10 MAXIMIZE SUM(y)",
        "b * y + b * y",
    ),
]

# Gurobi only: HiGHS refuses a product of decisions when the query is planned. The factor is applied to the product's
# coefficient in the objective and in a constraint.
DERIVED_PRODUCT = [
    (
        "factor_on_a_product_in_the_objective",
        "x(REAL), y(REAL) SUCH THAT x <= 4 AND y <= 4 MAXIMIZE 2 * SUM(b * x * y)",
        "2 * SUM(b * x * y)",
    ),
    (
        "factor_on_a_product_in_a_constraint",
        "x(REAL), y(REAL) SUCH THAT x <= 4 AND y <= 4 AND 2 * SUM(b * x * y) <= 20 MAXIMIZE SUM(x)",
        "2 * SUM(b * x * y)",
    ),
]

# (CLI, direct-solve mode): the solver path on each backend, and direct solve, which a forced backend turns off. The
# unforced `auto` of SOLVER_PATHS is a query no rule takes, so it reaches the solver. EVERY_PATH holds queries a rule
# takes, where `require` is also what `auto` runs.
SOLVER_PATHS = [("decidb_cli_gurobi", "off"), ("decidb_cli_highs", "off"), ("decidb_cli", "auto")]
EVERY_PATH = SOLVER_PATHS[:2] + [("decidb_cli", "require")]


@pytest.mark.error
@pytest.mark.parametrize("outcome", ["too_large", "infinite_value"])
@pytest.mark.parametrize("cli_fixture,mode", EVERY_PATH)
@pytest.mark.parametrize("name,decide,at", DERIVED_BOTH_PATHS, ids=[case[0] for case in DERIVED_BOTH_PATHS])
def test_a_derived_coefficient_names_its_value_or_its_clause_on_every_path(
    request, cli_fixture, mode, outcome, name, decide, at
):
    cli = request.getfixturevalue(cli_fixture)
    _points_at(cli, _derived_query(decide, outcome), mode, at)


@pytest.mark.error
@pytest.mark.parametrize("outcome", ["too_large", "infinite_value"])
@pytest.mark.parametrize("cli_fixture,mode", SOLVER_PATHS)
@pytest.mark.parametrize("name,decide,at", DERIVED_SOLVER, ids=[case[0] for case in DERIVED_SOLVER])
def test_a_coefficient_the_solver_derives_names_its_value_or_its_clause(
    request, cli_fixture, mode, outcome, name, decide, at
):
    cli = request.getfixturevalue(cli_fixture)
    _points_at(cli, _derived_query(decide, outcome), mode, at)


@pytest.mark.error
@pytest.mark.parametrize("outcome", ["too_large", "infinite_value"])
@pytest.mark.parametrize("mode", ["off"])
@pytest.mark.parametrize("name,decide,at", DERIVED_PRODUCT, ids=[case[0] for case in DERIVED_PRODUCT])
def test_a_factor_on_a_product_names_its_value_or_its_clause(decidb_cli_gurobi, mode, outcome, name, decide, at):
    _points_at(decidb_cli_gurobi, _derived_query(decide, outcome), mode, at)


# The terms inside a square are not merged into one coefficient on the solver path: each is read where it is written,
# and the sum of finite ones, past a DOUBLE, is the model builder's own arithmetic, which names the objective. The
# direct plan reads the square's form as one number and points at where the form stands.
SQUARES = [
    (
        "a1_square_of_like_terms",
        "y(REAL) SUCH THAT y <= 5 MINIMIZE SUM(POWER(b * y + b * y, 2))",
        "b * y + b * y",
        "SUM(POWER(b * y + b * y, 2))",
    ),
    (
        "a1_square_of_constants",
        "y(REAL) SUCH THAT y <= 5 MINIMIZE SUM(POWER(y - b - b, 2))",
        "y - b - b",
        "SUM(POWER(y - b - b, 2))",
    ),
]


@pytest.mark.error
@pytest.mark.parametrize("outcome", ["too_large", "infinite_value"])
@pytest.mark.parametrize("name,decide,direct_at,clause", SQUARES, ids=[case[0] for case in SQUARES])
def test_a_square_of_a_sum_names_where_each_path_reads_it(
    decidb_cli, decidb_cli_gurobi, decidb_cli_highs, outcome, name, decide, direct_at, clause
):
    sql = _derived_query(decide, outcome)
    _points_at(decidb_cli, sql, "require", direct_at)
    for cli in (decidb_cli_gurobi, decidb_cli_highs):
        if outcome == "too_large":
            _refused(cli, sql, "off", TooLarge(clause))
        else:
            _points_at(cli, sql, "off", "b")


@pytest.mark.error
@pytest.mark.parametrize("cli_fixture,mode", EVERY_PATH)
def test_the_constant_terms_of_the_objective_are_never_summed(request, cli_fixture, mode):
    # Each `b` is finite and `b + b` is not. A constant adds nothing to the answer, so every path reads each term on
    # its own and none adds them up: the query answers as the solver path does (A1's per-term check, BUG-24).
    decide = "x(REAL) SUCH THAT x <= 5 AND x >= 0 MAXIMIZE SUM(a * x + b + b)"
    sql = _huge_query(decide).replace("SELECT id FROM", "SELECT id, x FROM", 1) + " ORDER BY id"
    expected = request.getfixturevalue("decidb_cli_gurobi").execute(f"SET decide_direct_solve='off'; {sql}")[0]
    cli = request.getfixturevalue(cli_fixture)
    assert cli.execute(f"SET decide_direct_solve='{mode}'; {sql}")[0] == expected


# A model whose numbers are each finite, which the solver then reads its own way: the warning stays (the CLI wrapper
# checks it is printed), and so does the solver's verdict. Gurobi answers; HiGHS refuses to load the range.
@pytest.mark.error
@pytest.mark.parametrize("mode", ["off"])
def test_a_model_of_finite_huge_numbers_keeps_the_solvers_verdict(decidb_cli_gurobi, decidb_cli_highs, mode):
    sql = f"SET decide_direct_solve='{mode}'; {_huge_query('y(REAL) SUCH THAT y <= 5 MINIMIZE SUM(ABS(b * y))')}"
    answered = decidb_cli_gurobi.execute_raw(sql, expect_number_range=True).stderr
    assert "Error" not in answered, answered
    decidb_cli_highs.assert_error_at(
        sql,
        kind="Not implemented Error",
        at="SUM(ABS(b * y))",
        word="the objective holds numbers too far apart in size",
        expect_number_range=True,
    )


@pytest.mark.error
@pytest.mark.parametrize("name,decide,value", SOLVER_COEFFICIENT, ids=[case[0] for case in SOLVER_COEFFICIENT])
def test_a_coefficient_only_the_solver_reads_is_named_as_written(decidb_cli, name, decide, value):
    _points_at(decidb_cli, _query(decide, NAN), "off", value)


# `ABS` is lowered to bounds on a helper, which would word its argument's value as a bound the user never wrote ("the
# bound -v is NaN", or "infeasible" for an infinity). The value is refused where the term is read, as written, on
# each backend (the direct-solve plans word it the same, in test_direct_a2_single_facility_location.py).
@pytest.mark.error
@pytest.mark.parametrize("cli_fixture", ["decidb_cli_gurobi", "decidb_cli_highs"])
@pytest.mark.parametrize("bad", [NAN, INFINITY], ids=["nan", "infinity"])
def test_a_value_under_abs_is_named_as_written_not_as_a_bound(request, cli_fixture, bad):
    cli = request.getfixturevalue(cli_fixture)
    decide = "per (): x(REAL) SUCH THAT x BETWEEN 0 AND 10 MINIMIZE SUM(ABS(x - b))"
    _points_at(cli, _query(decide, bad), "off", "b")


# The term under `ABS` may be a frame (`at(...)`, `sum(from ... to ...: ...)`). The value check reads it at the row the
# frame selects, as a coefficient does: row 4 reads row 3's infinite value, though row 4's own is finite.
FRAME_UNDER_ABS = [
    ("point-else", "at(previous else 0: t.v) over (t.id)", "MINIMIZE SUM(ABS(x - {frame}))", "x BETWEEN 0 AND 10"),
    ("point", "at(previous: t.v) over (t.id)", "MINIMIZE SUM(ABS(x - {frame}))", "x BETWEEN 0 AND 10"),
    (
        "point-in-a-constraint",
        "at(previous else 0: t.v) over (t.id)",
        "MINIMIZE SUM(x)",
        "x BETWEEN 0 AND 10 AND abs(x - {frame}) <= 100",
    ),
    ("range", "sum(from previous to next: t.v) over (t.id)", "MINIMIZE SUM(ABS(x - {frame}))", "x BETWEEN 0 AND 10"),
]


@pytest.mark.error
@pytest.mark.parametrize("name,frame,objective,clauses", FRAME_UNDER_ABS, ids=[case[0] for case in FRAME_UNDER_ABS])
def test_a_frame_under_abs_is_named_as_written(decidb_cli, name, frame, objective, clauses):
    rows = "(1, 1.0), (2, 2.0), (3, 'Infinity'::DOUBLE), (4, 1.0)"
    sql = (
        f"SELECT id, x FROM (VALUES {rows}) t(id, v) DECIDE x(REAL) SUCH THAT "
        f"{clauses.format(frame=frame)} {objective.format(frame=frame)}"
    )
    _points_at(decidb_cli, sql, "off", frame)


@pytest.mark.error
@pytest.mark.parametrize("name,decide,value", BOTH_PATHS_BOUND, ids=[case[0] for case in BOTH_PATHS_BOUND])
def test_a_bound_that_is_nan_is_named_as_written_on_both_paths(decidb_cli, name, decide, value):
    direct = _stderr(decidb_cli, _query(decide, NAN), "require")
    solver = _stderr(decidb_cli, _query(decide, NAN), "off")
    _points_at(decidb_cli, _query(decide, NAN), "off", value)
    assert direct == solver


# A factor on a squared sum in a constraint, and on a composed MIN or MAX. It is one value for the whole query, so no
# row can be skipped. Every `b` is finite: the factor is the bad value. The squared sum's coefficient is the factor
# applied, which stands where the product is written; the composed MIN or MAX reads the factor itself.
FACTORS = [
    (
        "factor_on_a_squared_sum",
        "x(REAL) SUCH THAT x <= 4 AND (SELECT {bad}) * SUM(POWER(b * x, 2)) <= 20 MAXIMIZE SUM(a * x)",
        "(SELECT '{written}'::DOUBLE) * SUM(POWER(b * x, 2))",
    ),
    (
        "factor_on_a_composed_extremum",
        "x(REAL) SUCH THAT x <= 4 AND (SELECT {bad}) * MAX(b * x) + SUM(x) <= 20 MAXIMIZE SUM(a * x)",
        "(SELECT '{written}'::DOUBLE)",
    ),
]


@pytest.mark.error
@pytest.mark.parametrize("bad,written", [(NAN, "NaN"), (INFINITY, "Infinity")], ids=["nan", "infinity"])
@pytest.mark.parametrize("name,decide,at", FACTORS, ids=[case[0] for case in FACTORS])
def test_a_factor_that_is_not_finite_is_named_as_written(decidb_cli, name, decide, at, bad, written):
    sql = _query(decide.format(bad=bad), "1.0")
    _points_at(decidb_cli, sql, "off", at.format(written=written))


# A NULL at the places the refusals above read a value (a coefficient and a bound, in A1, R1 and S1): skipped in each.
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
    # `norm(e, 0)` states its links with a Big-M, which no infinite bound can dominate: the value is refused where the
    # norm's `ABS` is read, as written, whether it is infinite or NaN.
    decide = "x(REAL) SUCH THAT x <= 10 AND norm(x + b, 0) <= 2 MAXIMIZE SUM(a * x)"
    _points_at(decidb_cli, _query(decide, INFINITY), "off", "b")
    _points_at(decidb_cli, _query(decide, NAN), "off", "b")


# A constant bound as a literal, as an expression that is NaN, and with a factor on the decision.
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
    _points_at(
        decidb_cli,
        _query(decide, "1.0"),
        "off",
        decide.split("<= ")[-1].split(" MAXIMIZE")[0] if "AND x >=" not in decide else "0.0 / 0.0",
    )


# The caret stands where the user wrote what DeciDB scaled or merged into the coefficient: the scaled reducer, or the
# sum of the like terms (DecideReplaces), not the column inside it.
@pytest.mark.error
@pytest.mark.parametrize("mode", ["off", "require"])
@pytest.mark.parametrize(
    "decide,at",
    [
        ("x(REAL) SUCH THAT x <= 5 MAXIMIZE 2 * SUM(b * x)", "2 * SUM(b * x)"),
        ("x(REAL) SUCH THAT x <= 5 MAXIMIZE SUM(a * x + b * x)", "a * x + b * x"),
    ],
    ids=["factor-on-the-sum", "like-terms-merged"],
)
def test_the_caret_stands_where_a_scaled_or_merged_coefficient_is_written(decidb_cli, decide, at, mode):
    decidb_cli.assert_error_at(f"SET decide_direct_solve='{mode}'; {_query(decide, NAN)}", kind=INVALID_INPUT, at=at)


# Like terms of opposite signs merge to a difference of the user's literals, each finite and the difference not: the
# merged coefficient stands where the difference is written, and the error points there.
@pytest.mark.error
def test_a_difference_of_finite_literals_past_a_double_names_the_objective(decidb_cli):
    objective = "SUM(1e308 * x) - SUM(-1e308 * x)"
    sql = f"SELECT id, x FROM (VALUES (1), (2), (3)) t(id) DECIDE x(BOOL) SUCH THAT SUM(x) <= 1 MAXIMIZE {objective}"
    _points_at(decidb_cli, sql, "off", objective)
    assert _stderr(decidb_cli, sql, "require") == _stderr(decidb_cli, sql, "off")
