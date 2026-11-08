"""Row order: a query answers alike however its rows arrive.

A relation has no row order, so nothing a clause generates may follow one. Every ``_anr_cases`` case that runs through
the engine runs here with the rows of each of its relations reordered in its own VALUES, the query otherwise unchanged,
and is judged as its phase suite judges it: by the error its data raises, or by ``Case.model()`` and the optimum, so an
alternate optimum passes.

The orders are seeded by the case's name. Each row of each relation comes first in one order and last in another, which
makes it the first and the last row of every class it is in, and the rows between are shuffled; the reverse order flips
every pair. An answer can still hide a clause that was lost and does not bind, so the model's size is held too.

This is the baseline a rewrite of the evaluator is measured against, so everything runs on the solver path: a
direct-solve rule answers some cases without the evaluator.
"""

import random
import re

import pytest

from ._anr_cases import CASES, F17, MAXIMIZE, SHIP, X, Case
from ._anr_oracle import Abs, Col, Decision, Objective, ScopeError, clause, ge, le, reduce
from ._diagnose_relation import run as run_diagnose
from ._scope_cases import (  # noqa: F401 -- fixture
    aligned,
    answer,
    dump_bounds,
    dump_counts,
    scope_solver,
    table,
    values,
)
from .test_anr_diagnostics import _achievable, _findings
from solver.types import SolverStatus, VarType

#: One row of a VALUES list, and the list itself, which runs to the list's closing parenthesis. A cell is a literal, so
#: a row holds no parenthesis but in a string, and the rows are written `(VALUES (..), (..))` on one line. A list
#: written otherwise matches no part, and `_relations` refuses the query.
_ROW = r"\((?:[^()']|'[^']*')*\)"
_RELATION = re.compile(rf"(?<=\(VALUES ){_ROW}(?:, {_ROW})*(?=\))")
_VALUES = re.compile(r"\bVALUES\b", re.I)

#: The text each kind of refusal carries.
_REFUSALS = {**ScopeError.PHRASES, "conversion": "Conversion Error: Could not convert string 'none'"}


def _relations(sql: str) -> list[list[str]]:
    """The rows of each VALUES list of ``sql``, as written. A query with a list the pattern does not match is refused,
    so every caller fails, a case at collection, rather than run the query in no order."""
    relations = _RELATION.findall(sql)
    if len(relations) != len(_VALUES.findall(sql)):
        raise ValueError(f"a VALUES list is not matched whole:\n{sql}")
    return [re.findall(_ROW, rows) for rows in relations]


def _reordered(sql: str, order: tuple) -> str:
    """``sql`` with the rows of its i-th VALUES list in the order ``order[i]``, and no other change."""
    orders = iter(order)

    def rewrite(match) -> str:
        rows = re.findall(_ROW, match.group())
        return ", ".join(rows[i] for i in next(orders))

    return _RELATION.sub(rewrite, sql)


def _leading(size: int, first: int, rng: random.Random) -> tuple[int, ...]:
    """Row ``first`` first, the row before it last, the rest shuffled: over every ``first``, each leads and trails."""
    last = (first - 1) % size
    between = [i for i in range(size) if i not in (first, last)]
    rng.shuffle(between)
    return tuple(dict.fromkeys((first, *between, last)))


def _orders(sql: str, seed: str) -> dict[str, tuple]:
    """The orders ``sql`` runs in, by name: one per row of its longest relation and the reverse. The order as written is
    left out, since the case's phase suite runs that one, and so is an order that repeats another."""
    sizes = [len(rows) for rows in _relations(sql)]
    rng = random.Random(seed)
    orders = {"reversed": tuple(tuple(reversed(range(size))) for size in sizes)}
    for first in range(max(sizes)):
        orders[f"row-{first}-first"] = tuple(_leading(size, first % size, rng) for size in sizes)
    seen = {tuple(tuple(range(size)) for size in sizes)}
    return {name: order for name, order in orders.items() if order not in seen and not seen.add(order)}


def _judge(cli, solver, case: Case, sql: str) -> None:
    """Judge ``sql``, which is ``case.sql`` in some row order, as the case's phase suite judges ``case.sql``."""
    if case.error or case.status != SolverStatus.OPTIMAL:
        refusal = _REFUSALS[case.error] if case.error else "is infeasible"
        result = cli.execute_raw(sql)
        assert refusal in result.stderr, result.stderr
        return
    model = case.model()
    rows = answer(cli, sql, model.warnings(case.clauses, case.objective))
    # The oracle reads the answer by position, which only the query's ORDER BY now gives it.
    reached = model.judge(aligned(rows, model, next(iter(rows[0]))), case.clauses, case.objective)
    expected = case.optimum
    if expected is None:
        expected = model.solve(solver, case.clauses, case.objective).objective
    assert reached == pytest.approx(expected, abs=1e-6)


WHEN_COLUMNS = ("id", "p")
WHEN_ROWS = ((1, False), (2, True), (3, False), (4, True))
#: An auxiliary's definition stood on its class's first row, which a `when` may reject (5f88e23d18): with a rejected row
#: first the clause was lost and this answered 20. `when t.p` keeps rows 2 and 4, and the outer sum reads |x2 + x4| once
#: for each, so x2 + x4 <= 2 and the rejected rows hold 5 each: 12. The query writes a kept row first, the order that
#: defect answered right, so only a reordering reaches it.
ABS_UNDER_A_WHEN = Case(
    "abs_under_a_when",
    "5f88e23d18",
    2,
    WHEN_COLUMNS,
    WHEN_ROWS,
    (Decision("x", var_type=VarType.CONTINUOUS),),
    (clause(le(X, 5)), clause(le(reduce("sum", Abs(reduce("sum", X)), when=lambda r: r["p"]), 4))),
    Objective(MAXIMIZE, reduce("sum", X)),
    f"""SELECT id, x {table('t', WHEN_COLUMNS, WHEN_ROWS[1:] + WHEN_ROWS[:1])} DECIDE x(REAL)
        SUCH THAT x <= 5 AND sum(when t.p: abs(sum(x))) <= 4 MAXIMIZE sum(x) ORDER BY id""",
    12,
)


def _control(name, bound_sql, bounds, optimum) -> Case:
    """A query that does follow row order, on purpose: ``bound_sql`` reads a row's place among the rows as they arrive,
    and the model reads the ``bounds`` of the order written."""
    return Case(
        name,
        "a control",
        0,
        ("id", "bound"),
        tuple(enumerate(bounds, 1)),
        (Decision("x"),),
        (clause(le(X, Col("bound"))),),
        Objective(MAXIMIZE, reduce("sum", X)),
        f"""SELECT id, x FROM (SELECT id, {bound_sql} AS bound FROM (VALUES (1), (2), (3)) t(id)) t
            DECIDE x(INT) SUCH THAT x <= t.bound MAXIMIZE sum(x) ORDER BY id""",
        optimum,
    )


#: A row's bound moves with it, so in any other order some row exceeds the bound the model reads.
BREAKS_A_CLAUSE = _control("breaks_a_clause", "row_number() OVER ()", (1, 2, 3), 6)
#: A row out of place holds 0, so any other order satisfies the model's bounds and falls short of its optimum.
MISSES_THE_OPTIMUM = _control(
    "misses_the_optimum", "CASE WHEN row_number() OVER () = id THEN 3 ELSE 0 END", (3, 3, 3), 9
)

#: F17 is a DIAGNOSE, judged on its own below.
RUN = [case for case in CASES if case is not F17] + [ABS_UNDER_A_WHEN]
ANSWERED = [case for case in RUN if not case.error and case.status == SolverStatus.OPTIMAL]


def _named(cases):
    return [pytest.param(case, id=case.name) for case in cases]


def _each_order(case: Case):
    return [pytest.param(order, id=name) for name, order in _orders(case.sql, case.name).items()]


@pytest.fixture(scope="module")
def solver_path(decidb_cli):
    return decidb_cli.with_env({"DECIDB_TEST_DIRECT_SOLVE": "off"})


# ---------------------------------------------------------------------------
# The orders themselves
# ---------------------------------------------------------------------------


def test_a_list_the_pattern_cannot_read_whole_is_refused():
    """Read up to a line break, a list would give its query fewer orders than its rows, or none, and the query would
    pass with no evidence. `_relations` matches a list to its closing parenthesis or not at all, and refuses it."""
    with pytest.raises(ValueError, match="a VALUES list is not matched whole"):
        _relations("SELECT id FROM (VALUES (1),\n (2), (3)) t(id)")


def test_an_order_moves_each_row_and_nothing_else():
    """In every case, the query as written renders back byte for byte, an order puts row ``order[i][k]`` of list i at
    place k and keeps the list's rows and every other character, each row of each list leads and trails in some order,
    and the reverse order is there."""
    for case in [*RUN, F17]:
        relations = _relations(case.sql)
        written = tuple(tuple(range(len(rows))) for rows in relations)
        assert _reordered(case.sql, written) == case.sql, case.name
        orders = {"written": written, **_orders(case.sql, case.name)}
        assert tuple(order[::-1] for order in written) in orders.values(), case.name
        for name, order in orders.items():
            moved = _reordered(case.sql, order)
            where = f"{case.name}-{name}"
            assert _RELATION.sub("", moved) == _RELATION.sub("", case.sql), where
            assert [sorted(rows) for rows in _relations(moved)] == [sorted(rows) for rows in relations], where
            assert _relations(moved) == [[rows[k] for k in o] for rows, o in zip(relations, order)], where
        for i, rows in enumerate(relations):
            assert {order[i][0] for order in orders.values()} == set(range(len(rows))), case.name
            assert {order[i][-1] for order in orders.values()} == set(range(len(rows))), case.name


#: Row i of the list has id i + 1.
ARRIVAL_FROM = table("t", ("id", "g", "v"), ((1, "a", 5), (2, "b", 6), (3, "a", 7), (4, "b", 8)))
ARRIVAL_JOIN = f"{ARRIVAL_FROM} JOIN {values((('a', 9), ('b', 9)), 'd', ('g', 'cap'))} USING (g)"


@pytest.mark.correctness
@pytest.mark.parametrize(
    "from_sql, clause_sql",
    [
        pytest.param(ARRIVAL_FROM, "x <= t.id", id="a-scan"),
        pytest.param(ARRIVAL_JOIN, "x <= t.id", id="a-join"),
        pytest.param(ARRIVAL_FROM, "x <= t.id AND sum(x) by (t.g) <= sum(t.v) by (t.g)", id="a-side-plan"),
    ],
)
def test_rows_reach_the_operator_in_the_order_written(solver_path, tmp_path, from_sql, clause_sql):
    """Reordering VALUES proves nothing unless the operator then reads the rows in that order. A model's columns follow
    the rows the operator reads, and `x <= t.id` folds into each column's box, so the boxes spell the order of arrival:
    through a scan, through a join, and past the side plan of a reducer over data."""
    sql = f"SELECT id, x {from_sql} DECIDE x(INT) SUCH THAT {clause_sql} MAXIMIZE sum(x) ORDER BY id"
    for name, order in _orders(sql, "arrival").items():
        dump = solver_path.dump_model(_reordered(sql, order), tmp_path / f"{name}.dump")
        assert [ub for _, ub in dump_bounds(dump)] == [i + 1 for i in order[0]], name


@pytest.mark.correctness
@pytest.mark.parametrize(
    "control, failure",
    [
        pytest.param(BREAKS_A_CLAUSE, "a generated clause is violated", id="a-broken-clause"),
        pytest.param(MISSES_THE_OPTIMUM, "== 9 ", id="a-missed-optimum"),
    ],
)
def test_the_judgment_fails_an_answer_that_follows_row_order(solver_path, scope_solver, control, failure):
    """Were an order to change nothing, or the judgment pass anything, this file would pass with no evidence. Each
    control passes as written and fails in every other order, on the check it is named for."""
    _judge(solver_path, scope_solver, control, control.sql)
    for order in _orders(control.sql, control.name).values():
        with pytest.raises(AssertionError, match=failure):
            _judge(solver_path, scope_solver, control, _reordered(control.sql, order))


# ---------------------------------------------------------------------------
# Every case, in every order
# ---------------------------------------------------------------------------


@pytest.mark.correctness
@pytest.mark.parametrize(
    "case, order",
    [
        pytest.param(case, order, id=f"{case.name}-{name}")
        for case in RUN
        for name, order in _orders(case.sql, case.name).items()
    ],
)
def test_the_answer_holds_in_every_order(solver_path, scope_solver, case, order):
    """The hand-computed optimum or status, or the error the data raises, whatever order the rows arrive in."""
    _judge(solver_path, scope_solver, case, _reordered(case.sql, order))


@pytest.mark.correctness
@pytest.mark.parametrize("case", _named(ANSWERED))
def test_the_model_keeps_its_size_in_every_order(solver_path, tmp_path, case):
    """A clause lost on some order, or one generated twice, may not move the optimum when it does not bind. The model's
    columns and rows are counted in the order written and in each other."""
    warnings = case.model().warnings(case.clauses, case.objective)

    def size(name, sql):
        return dump_counts(solver_path.dump_model(sql, tmp_path / f"{name}.dump", expect_warnings=warnings))

    written = size("written", case.sql)
    orders = _orders(case.sql, case.name)
    moved = {
        name: counts for name, order in orders.items() if (counts := size(name, _reordered(case.sql, order))) != written
    }
    assert not moved, f"written {written}, moved {moved}"


@pytest.mark.diagnose
@pytest.mark.parametrize("order", _each_order(F17))
def test_a_diagnosis_finds_one_repair_in_every_order(solver_path, scope_solver, order):
    """F17: the cheapest repair loosens `ship >= 1`, and the objective it reaches is the oracle's over the repaired
    clauses, whatever order the rows arrive in."""
    sql = _reordered(F17.sql, order)
    findings = _findings(run_diagnose(solver_path, sql[len("DIAGNOSE ") :]), sql)
    repairs = [row for row in findings if row["clause"] not in ("", "NULL")]
    assert [(row["clause"], row["suggested_change"]) for row in repairs] == [("ship >= 1", "ship >= 0")], findings
    repaired = [clause(ge(SHIP, 0)), F17.clauses[1]]
    assert _achievable(findings) == pytest.approx(F17.model().solve(scope_solver, repaired, F17.objective).objective)
