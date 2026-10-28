"""Self-checks of the ANR reference evaluator, ``_anr_oracle.py``, before any engine change
depends on it (phase 0 of ``context/descriptions/09_anr_language/
anr_algorithm_and_frames_plan.md``, §6.2).

Five independent readings must agree with it:

1. **The deck and the plan, by hand.** Every ``_anr_cases.Case`` pins the instances the
   deck or the plan computes by hand, its optimum, or the error its data raises.
2. **Exhaustive enumeration** over small domains: the solved model (pick and sign
   indicators, strict stepping) and the numeric reading of the same instances find the
   same optimum.
3. **``_scope_oracle.ScopedModel``** on the 784-cell scope grid and the two-level
   objective: wherever today's determination rule holds, variant signatures generate
   today's model (D64; deck p. 39), and the two-level objective is an ordinary nested
   reducer (D75).
4. **DeciDB**, on the cases today's language already runs (``Case.phase == 0``).
5. **DeciDB's feasibility with every decision fixed**, against the oracle's, assignment
   by assignment (the status grid): a check no de-duplication after solving can pass.
"""

import pytest

from . import _scope_oracle as scope
from ._anr_cases import (
    CASES,
    F1_MAX,
    F5_OFFSET,
    F5_OFFSET_FIRST_ROW,
    F5_WEEKLY,
    F5_WEEKLY_ROW,
    decidb_feasible,
    fixing_clauses,
)
from ._anr_oracle import (
    FIRST,
    Add,
    Agg,
    AnrModel,
    At,
    Cmp,
    Col,
    Column,
    Const,
    Decision,
    Extremum,
    Mul,
    Objective,
    Over,
    Position,
    Range,
    Scope,
    ScopeError,
    Terms,
    Unsupported,
    Var,
    clause,
    le,
    previous,
    reduce,
)
from ._output_helpers import assert_no_internal_leak
from ._scope_cases import answer, scope_solver, table, table_rows  # noqa: F401 -- fixture
from .test_scope_combinations import BODIES, COLUMNS, DECISIONS, FRONTS, MAXIMIZE_X, ROWS, X_BOUND, _cells
from solver.types import ObjSense, SolverStatus

pytestmark = pytest.mark.correctness

X = Var("x")


def _named(cases):
    return [pytest.param(case, id=case.name) for case in cases]


# ---------------------------------------------------------------------------
# 1. The deck and the plan, by hand
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("case", _named(c for c in CASES if c.instances))
def test_generated_instances(case):
    model = case.model()
    for index, expected in case.instances.items():
        generated = tuple(model.render(instance) for instance in model.instances(case.clauses[index]))
        assert generated == expected, f"clause {index} of {case.name} ({case.source})"


@pytest.mark.parametrize("case", _named(c for c in CASES if c.optimum is not None and not c.error))
def test_optimum(case, scope_solver):
    solution = case.model().solve(scope_solver, case.clauses, case.objective)
    assert solution.status == SolverStatus.OPTIMAL, f"{case.name}: {solution.status}"
    assert solution.objective == pytest.approx(case.optimum, abs=1e-6)


@pytest.mark.parametrize("case", _named(c for c in CASES if c.status != SolverStatus.OPTIMAL))
def test_status(case, scope_solver):
    assert case.model().solve(scope_solver, case.clauses, case.objective).status == case.status


@pytest.mark.parametrize("case", _named(c for c in CASES if c.error))
def test_error(case, scope_solver):
    model = case.model()
    if case.error == "conversion":
        with pytest.raises(ValueError):
            model.solve(scope_solver, case.clauses, case.objective)
        return
    with pytest.raises(ScopeError) as raised:
        model.solve(scope_solver, case.clauses, case.objective)
    assert raised.value.kind == case.error


def test_a_nested_extremum_is_one_helper_shared_by_its_consumers():
    """Deck p. 115: x_{1,2} and x_{5,6} are two columns; the two West instances read the
    same x_{5,6}. Deck p. 112: the singleton group c2 is x3 itself, no helper."""
    model = F1_MAX.model()
    generated = model.instances(F1_MAX.clauses[1])
    extrema = [{atom for atom in instance.form if isinstance(atom, Extremum)} for instance in generated]
    assert len(set().union(*extrema)) == 2
    assert extrema[1] == extrema[2]
    assert Column("ship", 2) in generated[1].form


def test_an_anchor_free_range_collapses_to_one_row():
    model = F5_WEEKLY.model()
    generated = model.instances(F5_WEEKLY.clauses[1])
    assert len(generated) == 28
    assert {model.render(instance) for instance in generated} == {F5_WEEKLY_ROW}
    assert len({instance.frozen() for instance in generated}) == 1


def test_a_relative_range_samples_from_its_anchor():
    """From day 1, ``from 2 next to last every 7`` reads days 3, 10, 17, 24; days 27 and
    28 request nothing (their start lies past the last day) and are skipped."""
    model = F5_OFFSET.model()
    generated = model.instances(F5_OFFSET.clauses[1])
    assert model.render(generated[0]) == F5_OFFSET_FIRST_ROW
    assert len(generated) == 26


def test_positions_and_steps_are_positive_whole_numbers():
    """D71."""
    for bad in (0, -1):
        with pytest.raises(Unsupported, match="positive whole number"):
            Position("previous", bad)
        with pytest.raises(Unsupported, match="positive whole number"):
            Range("sum", FIRST, FIRST, X, Over("t"), every=bad)


def test_the_refused_constructs_have_no_answer():
    """D74: a reducer inside a frame payload, a product of decisions, COUNT over a decision."""
    rows = table_rows(("id", "period"), ((1, 1), (2, 2)))
    model = AnrModel(rows, [Decision("x"), Decision("y")])
    over = Over("period")
    refused = (
        le(X, At(previous(), reduce("sum", X), over)),
        le(X * Var("y"), 3),
        le(X, reduce("count", X)),
    )
    for body in refused:
        with pytest.raises(Unsupported):
            model.instances(clause(body))


# ---------------------------------------------------------------------------
# 2. Exhaustive enumeration
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("case", _named(c for c in CASES if c.domains))
def test_enumeration_agrees_with_the_solved_model(case, scope_solver):
    model = case.model()
    solved = model.solve(scope_solver, case.clauses, case.objective)
    exhaustive = model.brute_force(case.clauses, case.objective, case.domains)
    assert exhaustive.status == solved.status
    if solved.status == SolverStatus.OPTIMAL:
        assert exhaustive.objective == pytest.approx(solved.objective, abs=1e-6)
        assert model.feasible(model.generate(case.clauses), solved.values)


# ---------------------------------------------------------------------------
# 3. Today's scope oracle: the conservative extension
# ---------------------------------------------------------------------------


def _translated(part):
    if isinstance(part, scope.Reduce):
        operand = Scope(Terms(part.body), part.when, part.per)
        return Mul(Col(part.factor), Agg(part.agg, operand, part.by))
    if isinstance(part, scope.Nested):
        inner = Agg(part.inner, Scope(Terms(part.body)), part.key)
        return Agg(part.outer, Scope(inner, per=part.key))
    return Terms(part)


def _side(parts):
    return Add(tuple(_translated(part) for part in parts)) if parts else Const(0.0)


def translate_clause(scoped: "scope.Scoped") -> Scope:
    return Scope(Cmp(_side(scoped.lhs), scoped.sense, _side(scoped.rhs)), scoped.when, scoped.per)


def translate_objective(objective: "scope.ScopedObjective") -> Objective:
    return Objective(objective.sense, _side(objective.parts), objective.when)


def _both(solver, rows, decisions, clauses, objective):
    """Each oracle's optimum, or the ``ScopeError`` it raises."""
    try:
        old = scope.ScopedModel(rows, decisions).solve_scoped(solver, clauses, objective)
    except scope.ScopeError as error:
        old = error
    try:
        new = AnrModel(rows, decisions).solve(
            solver, [translate_clause(c) for c in clauses], translate_objective(objective)
        )
    except ScopeError as error:
        new = error
    return old, new


@pytest.mark.parametrize("declaration, front, body, agg, inner", list(_cells()))
def test_agrees_with_the_scope_oracle_on_the_grid(scope_solver, declaration, front, body, agg, inner):
    when, per = FRONTS[front]
    lhs, sense, rhs = BODIES[body][1](agg, inner)
    clauses = [X_BOUND, scope.Scoped(lhs, sense, rhs, when=when, per=per)]
    old, new = _both(scope_solver, table_rows(COLUMNS, ROWS), [DECISIONS[declaration]], clauses, MAXIMIZE_X)
    if not isinstance(old, Exception):
        assert not isinstance(new, Exception), f"today's rule holds, so the answer is today's: {new}"
        assert new.status == SolverStatus.OPTIMAL and new.objective == pytest.approx(old, abs=1e-6)
    elif old.kind == "not_determined":
        # D64: variants answer where today's rule refused; a variant's reducer group may
        # now be one its inner when empties (D8)
        assert not isinstance(new, Exception) or new.kind == "reducer_empty", new
        assert isinstance(new, Exception) or new.status == SolverStatus.OPTIMAL
    else:
        assert isinstance(new, ScopeError) and new.kind == old.kind, f"{old.kind} expected, got {new}"


TWO_LEVEL_COLUMNS = ("id", "g", "v")
TWO_LEVEL_ROWS = ((1, "a", 3), (2, "a", 5), (3, "b", 4), (4, "b", 2), (5, "b", 6))


@pytest.mark.parametrize("sense", (ObjSense.MAXIMIZE, ObjSense.MINIMIZE), ids=("maximize", "minimize"))
@pytest.mark.parametrize("inner", ("sum", "avg", "min", "max"))
@pytest.mark.parametrize("outer", ("sum", "avg", "min", "max"))
def test_the_two_level_objective_is_a_nested_reducer(scope_solver, outer, inner, sense):
    """D75: ``outer(per g: inner(x) by (g))`` read as nesting answers as the special form."""
    rows = table_rows(TWO_LEVEL_COLUMNS, TWO_LEVEL_ROWS)
    bounds = [
        scope.Scoped((lambda r: {"x": 1},), "<=", (lambda r: {scope.CONSTANT: r["v"]},)),
        scope.Scoped((lambda r: {"x": 1},), ">=", (lambda r: {scope.CONSTANT: 1},)),
    ]
    objective = scope.ScopedObjective(sense, (scope.Nested(outer, ("g",), inner, lambda r: {"x": 1}),))
    old, new = _both(scope_solver, rows, [scope.Decision("x")], bounds, objective)
    assert not isinstance(old, Exception) and not isinstance(new, Exception)
    assert new.objective == pytest.approx(old, abs=1e-6)


# ---------------------------------------------------------------------------
# 4. DeciDB, on what today's language runs
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("case", _named(c for c in CASES if c.phase == 0))
def test_todays_engine_agrees(decidb_cli, case):
    result = decidb_cli.execute_raw(case.sql)
    assert_no_internal_leak(result, case.name)
    if case.error:
        assert ScopeError.PHRASES[case.error] in result.stderr, result.stderr
        return
    assert result.returncode == 0 and not result.stderr.strip(), result.stderr
    reached = case.model().judge(answer(decidb_cli, case.sql), case.clauses, case.objective)
    assert reached == pytest.approx(case.optimum, abs=1e-6)


# ---------------------------------------------------------------------------
# 5. The status grid: DeciDB's feasibility with every decision fixed
# ---------------------------------------------------------------------------


def _status_grid(cli, model, clauses, template, alias, id_column, domains):
    instances = model.generate(clauses)
    outcomes, mismatches = set(), []
    for values in model.assignments(domains):
        expected = model.feasible(instances, values)
        outcomes.add(expected)
        sql = template.format(fix=fixing_clauses(model, values, alias, id_column))
        if decidb_feasible(cli, sql) != expected:
            mismatches.append({model.label(column): value for column, value in values.items()})
    assert outcomes == {True, False}, "the grid must hold feasible and infeasible assignments"
    assert not mismatches, f"DeciDB and the oracle disagree on {len(mismatches)} assignments: {mismatches[:5]}"


GRID_COLUMNS = ("id", "g", "cap")
GRID_DATA = ((1, "a", 3), (2, "a", 2), (3, "b", 3))


def test_status_grid_over_a_helper_decision(decidb_cli):
    """A per-row decision beside a reducer: today's value rewrite."""
    model = AnrModel(table_rows(GRID_COLUMNS, GRID_DATA), [Decision("x")])
    clauses = [clause(le(X, Col("cap"))), clause(le(X, 0.5 * reduce("sum", X, by=("g",)) + 1))]
    template = (
        f"SELECT id, x {table('t', GRID_COLUMNS, GRID_DATA)} DECIDE x(INT) "
        "SUCH THAT x <= t.cap AND x <= 0.5 * sum(x) by (t.g) + 1 AND {fix} MAXIMIZE sum(x) ORDER BY id"
    )
    _status_grid(decidb_cli, model, clauses, template, "t", "id", {"x": range(0, 4)})


KEYED_COLUMNS = ("id", "g", "h", "cap")
KEYED_DATA = ((1, "a", "p", 4), (2, "a", "q", 4), (3, "b", "p", 5))


def test_status_grid_over_a_keyed_decision(decidb_cli):
    """A keyed decision read on two rows of a ``per`` class."""
    model = AnrModel(table_rows(KEYED_COLUMNS, KEYED_DATA), [Decision("x", ("h",))])
    clauses = [clause(le(reduce("sum", X, by=("g",)), Col("cap")), per=("g",))]
    template = (
        f"SELECT id, x {table('t', KEYED_COLUMNS, KEYED_DATA)} DECIDE per t.h: x(INT) "
        "SUCH THAT per t.g: sum(x) by (t.g) <= t.cap AND {fix} MAXIMIZE sum(x) ORDER BY id"
    )
    _status_grid(decidb_cli, model, clauses, template, "t", "id", {"x": range(0, 6)})
