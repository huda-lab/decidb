"""Optimizer-pass branches the rest of the suite does not reach (BLOAT-29).

Each case names the branch it reaches. Answers are judged against every assignment of three rows
(`_brute_force.judge_rows`); an error is pinned by its text. The source is `t(id, c)` with three rows.
"""

import re

import pytest

from ._brute_force import judge_rows

_ROWS = "(VALUES (1, -2), (2, 3), (3, -1)) t(id, c)"
_DATA = (-2, 3, -1)


def _judge(cli, sql, domains, feasible, value, maximize=True):
    rows, columns = cli.execute(sql)
    judge_rows([dict(zip(columns, row)) for row in rows], domains, feasible, value, maximize)


def _per_row(name, upper):
    return {f"{name}{i}": range(upper + 1) for i in range(3)}


@pytest.mark.correctness
@pytest.mark.parametrize("domain", ["b IN (0, 1)", "b IN (1, 0)"])
def test_a_boolean_domain_listed_either_way_round_is_no_restriction(decidb_cli_gurobi, decidb_cli_highs, domain):
    """RewriteInDomain: a Boolean `IN` of its own two values lowers to `b >= 0`, in either order (the `a == 1.0` and
    `b == 0.0` operands)."""
    sql = f"""
        SELECT id, b FROM {_ROWS}
        DECIDE b(BOOL)
        SUCH THAT {domain}
        MAXIMIZE SUM(b * c)
        ORDER BY id"""
    for cli in (decidb_cli_gurobi, decidb_cli_highs):
        _judge(cli, sql, _per_row("b", 1), lambda a: True, lambda a: sum(a[f"b{i}"] * _DATA[i] for i in range(3)))


@pytest.mark.correctness
def test_abs_of_data_beside_a_decision_is_not_an_abs_over_a_decision(decidb_cli_gurobi, decidb_cli_highs):
    """IsAbsOverDecideVar: `ABS(c)` holds no decision, so it is a coefficient and no auxiliary is made."""
    sql = f"""
        SELECT id, y FROM {_ROWS}
        DECIDE y(INT)
        SUCH THAT y <= 6 AND SUM(ABS(c) * y) <= 12
        MAXIMIZE SUM(y)
        ORDER BY id"""
    for cli in (decidb_cli_gurobi, decidb_cli_highs):
        _judge(
            cli,
            sql,
            _per_row("y", 6),
            lambda a: sum(abs(_DATA[i]) * a[f"y{i}"] for i in range(3)) <= 12,
            lambda a: sum(a[f"y{i}"] for i in range(3)),
        )


@pytest.mark.correctness
@pytest.mark.parametrize(
    "clause, holds",
    [
        # CollectAbsWithSign: an equality pushes nothing (`pinning_sign == 0`), so the MIN over the ABS takes the
        # exact envelope.
        pytest.param(
            "MIN(ABS(y - 2)) = 1",
            lambda ys: min(abs(y - 2) for y in ys) == 1,
            id="equality-pins-nothing",
        ),
        # A factor whose sign only the rows decide (`node_sign == 0`) leaves the extremum's direction open.
        pytest.param(
            "c * MAX(ABS(y - 2)) <= 3",
            lambda ys: all(c * max(abs(y - 2) for y in ys) <= 3 for c in _DATA),
            id="factor-of-unknown-sign",
        ),
    ],
)
def test_an_extremum_over_abs_with_no_direction_takes_the_exact_envelope(
    decidb_cli_gurobi, decidb_cli_highs, clause, holds
):
    sql = f"""
        SELECT id, y FROM {_ROWS}
        DECIDE y(INT)
        SUCH THAT y <= 3 AND {clause}
        MAXIMIZE SUM(y)
        ORDER BY id"""
    for cli in (decidb_cli_gurobi, decidb_cli_highs):
        _judge(
            cli,
            sql,
            _per_row("y", 3),
            lambda a: holds([a[f"y{i}"] for i in range(3)]),
            lambda a: sum(a[f"y{i}"] for i in range(3)),
        )


@pytest.mark.correctness
def test_a_composed_clause_reads_an_avg_beside_a_sum_and_a_max(decidb_cli_gurobi, decidb_cli_highs):
    """WalkComposedLhs: the `avg` term of a composed MIN/MAX clause (`name != "avg"` false)."""
    sql = f"""
        SELECT id, y FROM {_ROWS}
        DECIDE y(INT)
        SUCH THAT y <= 6 AND SUM(y) + MAX(y) + AVG(y) <= 14
        MAXIMIZE SUM(y)
        ORDER BY id"""
    for cli in (decidb_cli_gurobi, decidb_cli_highs):
        _judge(
            cli,
            sql,
            _per_row("y", 6),
            lambda a: sum(a[f"y{i}"] for i in range(3))
            + max(a[f"y{i}"] for i in range(3))
            + sum(a[f"y{i}"] for i in range(3)) / 3
            <= 14 + 1e-9,
            lambda a: sum(a[f"y{i}"] for i in range(3)),
        )


@pytest.mark.correctness
def test_an_equality_on_a_max_divided_by_data_splits_into_its_two_halves(decidb_cli_gurobi, decidb_cli_highs):
    """RewriteMinMaxInConstraint: the factor of `MAX(y) / 2 = 3` is a divisor (`scale_divides`), re-attached as `/`."""
    sql = f"""
        SELECT id, y FROM {_ROWS}
        DECIDE y(INT)
        SUCH THAT y <= 6 AND MAX(y) / 2 = 3
        MINIMIZE SUM(y)
        ORDER BY id"""
    for cli in (decidb_cli_gurobi, decidb_cli_highs):
        _judge(
            cli,
            sql,
            _per_row("y", 6),
            lambda a: max(a[f"y{i}"] for i in range(3)) / 2 == 3,
            lambda a: sum(a[f"y{i}"] for i in range(3)),
            False,
        )


@pytest.mark.correctness
def test_a_query_wide_decision_beside_its_own_sum(decidb_cli_gurobi, decidb_cli_highs):
    """CollectLikeTerms: `s` and `SUM(s)` name one column but are different contributions (`reduction` differs), so
    they stay apart."""
    sql = f"""
        SELECT id, s FROM {_ROWS}
        DECIDE per (): s(INT)
        SUCH THAT s <= 9 AND s + SUM(s) <= 10
        MAXIMIZE s
        ORDER BY id"""
    for cli in (decidb_cli_gurobi, decidb_cli_highs):
        rows, columns = cli.execute(sql)
        values = {row[columns.index("s")] for row in rows}
        # `s + 3 * s <= 10`: the largest whole s is 2.
        assert values == {2}


# ---------------------------------------------------------------------------
# Refusals, pinned by their text
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("backend", ["gurobi", "highs"])
def test_a_strict_bound_on_a_fractional_reducer_is_refused_for_less_than(request, backend):
    """RefuseFractionalStep: the `<` message, beside the `>` and `<>` ones the suite pins."""
    cli = request.getfixturevalue(f"decidb_cli_{backend}")
    sql = f"""
        SELECT id, y FROM {_ROWS}
        DECIDE y(INT)
        SUCH THAT y <= 6 AND y < sum(y + 0.5) by (id)
        MAXIMIZE SUM(y)"""
    cli.assert_error(sql, match=re.escape("Comparison '<' is not supported here") + r".*Compare with '<='")


@pytest.mark.parametrize("backend", ["gurobi", "highs"])
def test_dividing_an_abs_by_a_zero_literal_is_refused_where_the_number_is_read(request, backend):
    """WalkPolarity reads a zero divisor as a factor that is exactly zero (`ZERO` and `is_divisor`)."""
    cli = request.getfixturevalue(f"decidb_cli_{backend}")
    sql = f"""
        SELECT id, y FROM {_ROWS}
        DECIDE y(INT)
        SUCH THAT y <= 6 AND SUM(ABS(y) / 0) <= 30
        MAXIMIZE SUM(y)"""
    cli.assert_error(sql, match=re.escape("SUM(ABS(y) / 0) <= 30 computes a number too large for a DOUBLE"))


def test_highs_names_the_negated_square_when_the_objective_minimizes_it(decidb_cli_highs):
    """DescribeGap: a `MINIMIZE` of negated squares is told apart from a `MAXIMIZE` of squares."""
    sql = f"""
        SELECT id, y FROM {_ROWS}
        DECIDE y(REAL)
        SUCH THAT y <= 3
        MINIMIZE SUM(-POWER(y, 2))"""
    decidb_cli_highs.assert_error(
        sql,
        match=r"the objective MINIMIZEs negated squared decision terms.*MINIMIZE the squared terms without negating",
    )


@pytest.mark.correctness
def test_a_boolean_product_expands_to_a_product_of_two_integers_too(decidb_cli_gurobi):
    """FindAndReplaceBilinear: `(b - x) * (b + x)` is `b * b - x * x`: the square of the Boolean is itself and the
    square of the integer is the Q matrix's (`first == second` on a decision that is not Boolean)."""
    sql = """
        SELECT id, x, b FROM (VALUES (1), (2)) t(id)
        DECIDE x(INT), b(BOOL)
        SUCH THAT x <= 3 AND (b - x) * (b + x) >= -4
        MINIMIZE SUM(x) + SUM(b)
        ORDER BY id"""
    domains = {**{f"x{i}": range(4) for i in range(2)}, **{f"b{i}": range(2) for i in range(2)}}
    _judge(
        decidb_cli_gurobi,
        sql,
        domains,
        lambda a: all((a[f"b{i}"] - a[f"x{i}"]) * (a[f"b{i}"] + a[f"x{i}"]) >= -4 for i in range(2)),
        lambda a: sum(a[f"x{i}"] + a[f"b{i}"] for i in range(2)),
        False,
    )


#: A MIN or MAX over a squared body is defined by a helper per row (DefineNonlinearReducerBodies), stated natively.
_SQUARE_BODY_CASES = [
    # Two clauses read the same reducer under different `when`s: one helper each (SameReducer, `when` against none).
    pytest.param(
        "when id < 3: MAX(POWER(y - 1, 2)) >= 3 AND when id < 2: MAX(POWER(y - 1, 2)) >= 3",
        lambda ys: max((y - 1) ** 2 for y in ys[:2]) >= 3 and max((y - 1) ** 2 for y in ys[:1]) >= 3,
        "MAXIMIZE SUM(y)",
        lambda ys: sum(ys),
        id="the-same-reducer-under-two-whens",
    ),
    # A reducer with its own filter and the unfiltered one are different reducers (SameReducer, `Equals`).
    pytest.param(
        "MAX(when id < 3: POWER(y - 1, 2)) >= 3 AND MAX(POWER(y - 1, 2)) >= 3",
        lambda ys: max((y - 1) ** 2 for y in ys[:2]) >= 3 and max((y - 1) ** 2 for y in ys) >= 3,
        "MAXIMIZE SUM(y)",
        lambda ys: sum(ys),
        id="a-filtered-reducer-beside-an-unfiltered-one",
    ),
    # A SUM of squares in the objective beside a MIN or MAX is not defined by a helper (CollectBodySites).
    pytest.param(
        "MAX(POWER(y - 1, 2)) >= 3",
        lambda ys: max((y - 1) ** 2 for y in ys) >= 3,
        "MINIMIZE SUM(POWER(y - 3, 2)) + MAX(y)",
        lambda ys: sum((y - 3) ** 2 for y in ys) + max(ys),
        id="a-sum-of-squares-beside-a-max-in-the-objective",
    ),
]


@pytest.mark.correctness
@pytest.mark.parametrize("clause, holds, objective, value", _SQUARE_BODY_CASES)
def test_reducers_over_squared_bodies_share_a_helper_only_when_they_are_the_same_reducer(
    decidb_cli_gurobi, clause, holds, objective, value
):
    sql = f"""
        SELECT id, y FROM {_ROWS}
        DECIDE y(INT)
        SUCH THAT y <= 3 AND {clause}
        {objective}
        ORDER BY id"""
    _judge(
        decidb_cli_gurobi,
        sql,
        _per_row("y", 3),
        lambda a: holds([a[f"y{i}"] for i in range(3)]),
        lambda a: value([a[f"y{i}"] for i in range(3)]),
        objective.startswith("MAXIMIZE"),
    )
