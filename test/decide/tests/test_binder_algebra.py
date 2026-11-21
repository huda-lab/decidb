"""What the binder leaves to later stages, and the one answer each gets.

The binder reads what only the parsed tree can show (names, casts, scopes, keys, a reducer's argument count and its
body); whether a clause reads a decision, a frame's degree and a function's vocabulary are judged once, on the
canonical bound tree (`DecideShapeCheck`). Answers are judged against every assignment (`_brute_force`) or by the
scope oracle (`_scope_oracle`); a refusal names its kind and where it points (`assert_error_at`).
"""

import pytest

from ._brute_force import judge_rows
from ._diagnose_relation import rows as diagnose_rows
from ._diagnose_relation import run as run_diagnose
from ._scope_cases import (  # noqa: F401 -- scope_solver is a fixture
    answer,
    const,
    scope_solver,
    table,
    table_rows,
)
from ._scope_oracle import CONSTANT, Decision, Reduce, Scoped, ScopedModel, ScopedObjective
from solver.types import ObjSense

COLUMNS = ("id", "cap", "g", "v")
ROWS = ((1, 3, "A", 2), (2, 5, "A", 3), (3, 7, "B", 5), (4, 4, "B", 4))
CAPS = [cap for _, cap, _, _ in ROWS]
VS = [v for _, _, _, v in ROWS]
SUM_V = sum(VS)
AVG_V = SUM_V / len(VS)
#: The largest `v` of each row's group (`max(v) by (g)`).
GROUP_MAX_V = [max(v for _, _, group, v in ROWS if group == row[2]) for row in ROWS]

X = lambda r: {"x": 1}  # noqa: E731
CAP_BOUND = Scoped((X,), "<=", (lambda r: {CONSTANT: r["cap"]},))


def _query(clauses, objective, decide="x(INT)"):
    names = ", ".join(declaration.split(":")[-1].split("(")[0].strip() for declaration in decide.split("),"))
    return f"""SELECT id, {names} {table('t', COLUMNS, ROWS)} DECIDE {decide}
        SUCH THAT x <= cap AND {clauses} {objective} ORDER BY id"""


def _domains(**more):
    return {**{f"x{i}": range(cap + 1) for i, cap in enumerate(CAPS)}, **more}


def _xs(a):
    return [a[f"x{i}"] for i in range(len(CAPS))]


def _judged(cli, clauses, objective, holds, value, maximize=True, decide="x(INT)", domains=None):
    judge_rows(answer(cli, _query(clauses, objective, decide)), domains or _domains(), holds, value, maximize)


# ---------------------------------------------------------------------------
# A clause reads a decision (DecideShapeCheck::CheckClause)
# ---------------------------------------------------------------------------


@pytest.mark.error_binder
@pytest.mark.error
@pytest.mark.parametrize(
    "clause, objective, at",
    [
        # A constraint no side of which reads a decision: a function of data, a reducer under arithmetic, a frame.
        pytest.param("abs(v) <= 5", "MAXIMIZE sum(x)", "abs(v) <= 5", id="a-function-of-data"),
        pytest.param("sum(v) + 1 <= 50", "MAXIMIZE sum(x)", "sum(v) + 1 <= 50", id="a-reducer-under-arithmetic"),
        pytest.param("at(first: v) over (id) <= 3", "MAXIMIZE sum(x)", "at(first: v) over (id) <= 3", id="a-frame"),
        # The same for the objective: the whole body must read a decision, whatever stands in it.
        pytest.param("x >= 0", "MAXIMIZE sum(v) + 1", "sum(v) + 1", id="objective-a-reducer-plus-a-constant"),
        pytest.param("x >= 0", "MAXIMIZE sum(v) * 2", "sum(v) * 2", id="objective-a-reducer-times-a-constant"),
        pytest.param("x >= 0", "MAXIMIZE abs(3)", "abs(3)", id="objective-a-function-of-a-constant"),
        pytest.param("x >= 0", "MAXIMIZE 1 + 2", "1 + 2", id="objective-constants"),
        pytest.param("x >= 0", "MAXIMIZE at(first: v) over (id)", "at(first: v) over (id)", id="objective-a-frame"),
        # Under a `when`, which only chooses the rows.
        pytest.param("x >= 0", "MAXIMIZE when id > 1: sum(v)", "sum(v)", id="objective-under-when"),
    ],
)
def test_a_clause_that_reads_no_decision_is_refused_at_the_clause(decidb_cli, clause, objective, at):
    decidb_cli.assert_error_at(_query(clause, objective), kind="Binder Error", at=at, word=f"'{at}' reads no decision")


# ---------------------------------------------------------------------------
# The objective's data terms and the one function it reads over a reducer
# ---------------------------------------------------------------------------

DATA_TERMS = [
    pytest.param(
        "MAXIMIZE sum(x) + sum(v)",
        ScopedObjective(ObjSense.MAXIMIZE, (Reduce("sum", X), Reduce("sum", lambda r: {CONSTANT: r["v"]}))),
        id="a-data-reducer",
    ),
    pytest.param(
        "MINIMIZE sum(x) - (SELECT 100)",
        ScopedObjective(ObjSense.MINIMIZE, (Reduce("sum", X), const(-100.0))),
        id="a-subquery",
    ),
    pytest.param(
        "MAXIMIZE 2 * (sum(x) - sum(v))",
        ScopedObjective(ObjSense.MAXIMIZE, (Reduce("sum", X, factor=lambda r: 2.0), const(-2.0 * SUM_V))),
        id="a-data-reducer-under-a-factor",
    ),
]


@pytest.mark.diagnose
@pytest.mark.correctness
@pytest.mark.parametrize("objective_sql, objective", DATA_TERMS)
def test_a_data_term_beside_the_decisions_is_in_the_objectives_value(
    decidb_cli, scope_solver, objective_sql, objective
):
    """A data term stays a term the objective reads, and its one value is in the objective's constant: the
    achievable objective of the repair of `sum(x) >= 100` (loosened until the rows fit) is the oracle's, the data term
    included."""
    result = run_diagnose(decidb_cli, _query("sum(x) >= 100", objective_sql))
    assert result.returncode == 0, result.stderr
    findings = diagnose_rows(result)
    repair = next(row for row in findings if row["clause"] == "sum(x) >= 100")
    achieved = float(next(row["amount"] for row in findings if row["edit_source"] == "achievable_objective"))
    clauses = [CAP_BOUND, Scoped((Reduce("sum", X),), ">=", (const(100 - float(repair["amount"])),))]
    model = ScopedModel(table_rows(COLUMNS, ROWS), [Decision("x")])
    assert achieved == pytest.approx(model.solve_scoped(scope_solver, clauses, objective), abs=1e-4)


@pytest.mark.error
def test_a_data_term_that_takes_two_values_is_a_second_objective(decidb_cli):
    """The data term is a field the objective reads: `v` differs between the kept rows, so the objective would be
    produced twice, and is not read off the first row."""
    decidb_cli.assert_error_at(
        _query("x >= 0", "MAXIMIZE sum(x) + v"),
        kind="Invalid Input Error",
        at="v",
        word="'v' takes more than one value over the kept rows (2, 5)",
    )


@pytest.mark.correctness
@pytest.mark.parametrize(
    "objective, value",
    [
        pytest.param("MINIMIZE abs(sum(x) - 9)", lambda a: abs(sum(_xs(a)) - 9), id="alone"),
        pytest.param(
            "MINIMIZE 2 * abs(sum(x) - 9) + sum(x)", lambda a: 2 * abs(sum(_xs(a)) - 9) + sum(_xs(a)), id="in-a-sum"
        ),
    ],
)
def test_an_abs_over_a_reducer_in_the_objective_solves(decidb_cli, objective, value):
    domains = {f"x{i}": range(1, cap + 1) for i, cap in enumerate(CAPS)}
    _judged(decidb_cli, "x >= 1", objective, lambda a: True, value, maximize=False, domains=domains)


@pytest.mark.error
@pytest.mark.parametrize(
    "decide, objective, at",
    [
        pytest.param("x(INT)", "MINIMIZE power(sum(x) - 3, 2)", "power(sum(x) - 3, 2)", id="of-a-reducer"),
        pytest.param(
            "x(INT), per (): y(INT)", "MINIMIZE power(y - 3, 2) + sum(x)", "power(y - 3, 2)", id="of-a-decision"
        ),
    ],
)
def test_a_power_outside_a_reducer_in_the_objective_is_refused_by_the_one_rule(decidb_cli, decide, objective, at):
    """A square outside a reducer in the objective has one refusal, whatever it squares."""
    decidb_cli.assert_error_at(
        _query("x >= 1 AND x <= 3", objective, decide),
        kind="Not implemented Error",
        at=at,
        word="multiplies decisions outside a reducer",
    )


@pytest.mark.correctness
@pytest.mark.parametrize(
    "objective, value",
    [
        pytest.param("MAXIMIZE sum(x) * avg(v)", lambda a: sum(_xs(a)) * AVG_V, id="a-factor"),
        pytest.param("MAXIMIZE sum(x) * (avg(v) - 5)", lambda a: sum(_xs(a)) * (AVG_V - 5), id="a-negative-factor"),
        pytest.param("MINIMIZE sum(x) / avg(v)", lambda a: sum(_xs(a)) / AVG_V, id="a-divisor"),
        pytest.param("MINIMIZE sum(x) * (sum(v) - 20)", lambda a: sum(_xs(a)) * (SUM_V - 20), id="a-sum"),
    ],
)
def test_a_data_reducer_without_by_scales_the_objective_as_one_value(decidb_cli, objective, value):
    _judged(
        decidb_cli,
        "sum(x) >= 6 AND sum(x) <= 12",
        objective,
        lambda a: 6 <= sum(_xs(a)) <= 12,
        value,
        maximize=objective.startswith("MAXIMIZE"),
    )


@pytest.mark.error_binder
@pytest.mark.error
def test_a_data_reducer_with_by_does_not_scale_the_objective(decidb_cli):
    """It has one value per group, so it varies per row; the refusal quotes the reducer as written."""
    decidb_cli.assert_error_at(
        _query("x >= 0", "MAXIMIZE sum(x) * avg(v) by (g)"),
        kind="Binder Error",
        at="avg(v) by (g)",
        word="'avg(v) by (g)' varies per row, which cannot scale a reducer in the objective",
    )


# ---------------------------------------------------------------------------
# A data reducer scaling a decision in a constraint
# ---------------------------------------------------------------------------


@pytest.mark.correctness
@pytest.mark.parametrize(
    "clause, holds",
    [
        # A multiplier of a per-row decision, of a reducer, and a divisor of each.
        pytest.param("x * avg(v) <= 8", lambda a: all(x * AVG_V <= 8 for x in _xs(a)), id="multiplier-of-a-decision"),
        pytest.param("avg(v) * sum(x) <= 25", lambda a: sum(_xs(a)) * AVG_V <= 25, id="multiplier-of-a-reducer"),
        pytest.param("sum(x) * max(v) <= 25", lambda a: sum(_xs(a)) * max(VS) <= 25, id="max-as-a-multiplier"),
        pytest.param(
            "x / sum(v) <= 0.25", lambda a: all(x / SUM_V <= 0.25 for x in _xs(a)), id="divisor-of-a-decision"
        ),
        pytest.param("sum(x) / sum(v) <= 1", lambda a: sum(_xs(a)) / SUM_V <= 1, id="divisor-of-a-reducer"),
        pytest.param("sum(x) / count(*) <= 2", lambda a: sum(_xs(a)) / len(CAPS) <= 2, id="count-as-a-divisor"),
        pytest.param(
            "sum(x * cap) / sum(cap) <= 5",
            lambda a: sum(x * cap for x, cap in zip(_xs(a), CAPS)) / sum(CAPS) <= 5,
            id="divisor-of-a-reducer-over-data-in-its-body",
        ),
        pytest.param(
            "x <= sum(x) / sum(v)",
            lambda a: all(x <= sum(_xs(a)) / SUM_V for x in _xs(a)),
            id="reducer-over-a-divisor-on-the-bound-side",
        ),
        # A data reducer divided together with a decision term, in each scope spelling and on either side.
        pytest.param(
            "(sum(x) + sum(v)) / 2 <= 8", lambda a: (sum(_xs(a)) + SUM_V) / 2 <= 8, id="divided-together-with-a-term"
        ),
        pytest.param(
            "per g: (sum(x) + sum(v)) / 2 <= 8", lambda a: (sum(_xs(a)) + SUM_V) / 2 <= 8, id="divided-under-per"
        ),
        pytest.param("8 >= (sum(x) + sum(v)) / 2", lambda a: (sum(_xs(a)) + SUM_V) / 2 <= 8, id="divided-on-the-right"),
        pytest.param(
            "(sum(x) + sum(v)) / (SELECT 2) <= 8",
            lambda a: (sum(_xs(a)) + SUM_V) / 2 <= 8,
            id="divided-by-a-subquery",
        ),
    ],
)
def test_a_data_reducer_scaling_a_decision_answers(decidb_cli, clause, holds):
    _judged(decidb_cli, clause, "MAXIMIZE sum(x)", holds, lambda a: sum(_xs(a)))


@pytest.mark.correctness
@pytest.mark.parametrize(
    "clause, holds",
    [
        # With a `by`, the factor is one value per group, so each row's clause reads its group's: a tuple term of a
        # per-row decision, and a factor of a reducer, under `<=`, `<` and `<>`. `max(v)` is whole-numbered, so `<`
        # and `<>` step.
        pytest.param(
            "x * max(v) by (g) <= 8",
            lambda a: all(x * m <= 8 for x, m in zip(_xs(a), GROUP_MAX_V)),
            id="tuple-term-le",
        ),
        pytest.param(
            "x * max(v) by (g) < 9",
            lambda a: all(x * m < 9 for x, m in zip(_xs(a), GROUP_MAX_V)),
            id="tuple-term-lt",
        ),
        pytest.param(
            "x * max(v) by (g) <> 6",
            lambda a: all(x * m != 6 for x, m in zip(_xs(a), GROUP_MAX_V)),
            id="tuple-term-ne",
        ),
        pytest.param(
            "sum(x) * max(v) by (g) <= 25",
            lambda a: all(sum(_xs(a)) * m <= 25 for m in GROUP_MAX_V),
            id="reducer-factor-le",
        ),
        pytest.param(
            "sum(x) * max(v) by (g) < 26",
            lambda a: all(sum(_xs(a)) * m < 26 for m in GROUP_MAX_V),
            id="reducer-factor-lt",
        ),
        pytest.param(
            "sum(x) * max(v) by (g) <> 30",
            lambda a: all(sum(_xs(a)) * m != 30 for m in GROUP_MAX_V),
            id="reducer-factor-ne",
        ),
        # Without one, the factor is one value for the query.
        pytest.param("sum(x) * max(v) < 26", lambda a: sum(_xs(a)) * max(VS) < 26, id="no-by-lt"),
        pytest.param("sum(x) * max(v) <> 30", lambda a: sum(_xs(a)) * max(VS) != 30, id="no-by-ne"),
    ],
)
def test_a_data_reducer_with_by_scaling_a_decision_answers_per_group(decidb_cli, clause, holds):
    _judged(decidb_cli, clause, "MAXIMIZE sum(x)", holds, lambda a: sum(_xs(a)))


# ---------------------------------------------------------------------------
# A reducer's argument count and its body, at every position (BindAggregate)
# ---------------------------------------------------------------------------


@pytest.mark.error_binder
@pytest.mark.error
@pytest.mark.parametrize(
    "clause, objective, at, word",
    [
        pytest.param("sum(x) <= max(v, 2)", "MAXIMIZE sum(x)", "max(v, 2)", "'max(v, 2)' has 2 arguments", id="bound"),
        pytest.param("sum() <= 2", "MAXIMIZE sum(x)", "sum()", "'sum()' has 0 arguments", id="no-argument"),
        pytest.param("x >= 0", "MAXIMIZE sum(x, 2)", "sum(x, 2)", "'sum(x, 2)' has 2 arguments", id="objective"),
    ],
)
def test_a_reducer_takes_one_argument_wherever_it_stands(decidb_cli, clause, objective, at, word):
    decidb_cli.assert_error_at(_query(clause, objective), kind="Binder Error", at=at, word=word)


@pytest.mark.error_binder
@pytest.mark.error
@pytest.mark.parametrize(
    "clause, objective, at",
    [
        pytest.param("sum(y) <= 4", "MAXIMIZE sum(x)", "sum(y)", id="alone"),
        pytest.param("sum(y) + sum(x) <= 9", "MAXIMIZE sum(x)", "sum(y)", id="beside-another-term"),
        pytest.param(
            "sum(y) by (g) + sum(x) <= 9", "MAXIMIZE sum(x)", "sum(y) by (g)", id="with-by-beside-another-term"
        ),
        pytest.param("y <= 3", "MAXIMIZE sum(y)", "sum(y)", id="objective"),
        pytest.param("y <= 3", "MAXIMIZE sum(x) + sum(y)", "sum(y)", id="objective-beside-another-term"),
    ],
)
def test_a_reducer_over_a_query_wide_decision_has_no_rows_to_reduce(decidb_cli, clause, objective, at):
    decidb_cli.assert_error_at(
        _query(clause, objective, "x(INT), per (): y(INT)"),
        kind="Binder Error",
        at=at,
        word="'y' is a query-wide decision, so 'sum(y)' has no rows to reduce",
    )


@pytest.mark.correctness
@pytest.mark.parametrize(
    "clause, holds",
    [
        pytest.param(
            "sum(x + y) <= 30", lambda a: sum(_xs(a)) + len(CAPS) * a["y"] <= 30, id="decision-and-query-wide"
        ),
        pytest.param(
            "sum(v * y) + sum(x) <= 30", lambda a: SUM_V * a["y"] + sum(_xs(a)) <= 30, id="data-times-query-wide"
        ),
    ],
)
def test_a_reducer_over_a_query_wide_decision_and_more_reduces(decidb_cli, clause, holds):
    _judged(
        decidb_cli,
        f"y <= 3 AND {clause}",
        "MAXIMIZE sum(x) + y",
        holds,
        lambda a: sum(_xs(a)) + a["y"],
        decide="x(INT), per (): y(INT)",
        domains=_domains(y=range(4)),
    )


@pytest.mark.error_binder
@pytest.mark.error
@pytest.mark.parametrize(
    "clause, objective, at, body",
    [
        pytest.param("norm(y - 1, 0) <= 0", "MAXIMIZE sum(x)", "norm(y - 1, 0)", "y - 1", id="norm-l0"),
        pytest.param(
            "x <= sum(from first to last: y) over (t.id)",
            "MAXIMIZE sum(x)",
            "sum(from first to last: y) over (t.id)",
            "y",
            id="range-frame",
        ),
    ],
)
def test_a_norm_and_a_range_frame_over_a_query_wide_decision_are_refused_as_any_reducer_is(
    decidb_cli, clause, objective, at, body
):
    """Reducing once and reducing once per row are different problems for a norm and for a frame's range as they are
    for `sum(y)`, so the check `sum(y)` meets (BindAggregate) is the one they meet: the caret is on the call."""
    decidb_cli.assert_error_at(
        _query(clause, objective, "x(INT), per (): y(INT)"),
        kind="Binder Error",
        at=at,
        word=f"'{body}' is a query-wide decision, so '{at}' has no rows to reduce",
    )


@pytest.mark.correctness
def test_a_range_frame_over_a_keyed_decision_reduces_the_rows_of_its_frame(decidb_cli):
    """A keyed body is not query-wide, so a range frame over it answers: `sum(from first to last: k) over (t.id)` is
    `2 * k_A + 2 * k_B` for every row."""
    _judged(
        decidb_cli,
        "k <= 2 AND x <= sum(from first to last: k) over (t.id)",
        "MAXIMIZE sum(x) - 3 * sum(k)",
        lambda a: a["k0"] == a["k1"]
        and a["k2"] == a["k3"]
        and all(k <= 2 for k in (a["k0"], a["k2"]))
        and all(x <= sum(a[f"k{i}"] for i in range(len(CAPS))) for x in _xs(a)),
        lambda a: sum(_xs(a)) - 3 * sum(a[f"k{i}"] for i in range(len(CAPS))),
        decide="x(INT), per g: k(INT)",
        domains=_domains(k0=range(3), k1=range(3), k2=range(3), k3=range(3)),
    )


# ---------------------------------------------------------------------------
# A window function, in each place a clause reads data
# ---------------------------------------------------------------------------


@pytest.mark.error_binder
@pytest.mark.error
@pytest.mark.parametrize(
    "clause, objective",
    [
        pytest.param("sum(x) <= sum(v) OVER ()", "MAXIMIZE sum(x)", id="a-bound"),
        pytest.param("sum(v) OVER () >= sum(x)", "MAXIMIZE sum(x)", id="the-left-side"),
        pytest.param("sum(x * (sum(v) OVER ())) <= 5", "MAXIMIZE sum(x)", id="in-a-reducer-body"),
        pytest.param("sum(x) <= 1 + abs(sum(v) OVER ())", "MAXIMIZE sum(x)", id="under-a-function"),
        pytest.param("x >= 0", "MAXIMIZE sum(x) + sum(v) OVER ()", id="in-the-objective"),
    ],
)
def test_a_window_function_is_refused_where_it_stands(decidb_cli, clause, objective):
    decidb_cli.assert_error_at(
        _query(clause, objective), kind="Binder Error", at="sum(v) OVER ()", word="a window function is not allowed"
    )


# ---------------------------------------------------------------------------
# A frame holds a linear payload, however the power is written
# ---------------------------------------------------------------------------


@pytest.mark.error_binder
@pytest.mark.error
@pytest.mark.parametrize(
    "payload",
    [
        pytest.param("power(x, 2)", id="power"),
        pytest.param("x ** 2", id="operator"),
        pytest.param("x * x", id="product"),
    ],
)
def test_a_square_in_a_frames_payload_is_one_refusal(decidb_cli, payload):
    """`power(x, 2)` and `x ** 2` were two answers, a Not implemented and a Binder error."""
    decidb_cli.assert_error_at(
        _query(f"x <= at(previous: {payload}) over (t.id)", "MAXIMIZE sum(x)", "x(REAL)"),
        kind="Binder Error",
        at=payload,
        word=f"'{payload}' squares or multiplies decisions inside a frame, which is not allowed",
    )
