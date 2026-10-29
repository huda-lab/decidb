"""Variant signatures in the engine (phase 1 of
``context/descriptions/09_anr_language/anr_algorithm_and_frames_plan.md``, §6.4; D64-D66, D77).

A scoped expression is one instance per class of its key and distinct value of its
signature: its data terms by value (those of one additive context summed into one, D77),
its decisions by instance, its reducers' ``by`` keys. Every phase-1 fixture of
``_anr_cases`` runs through DeciDB and is judged by ``_anr_oracle``, which reads no SQL;
the shape of the model is pinned where the rows shape makes one row per instance; and
EXPLAIN ANALYZE and DIAGNOSE name the variants.
"""

import pytest

from ._anr_cases import CASES, F10_SPLIT, F10_SUM, F11_VARYING, F20_JOINED_TERMS, F20_SPLIT_TERMS
from ._anr_oracle import PER_ROW, AnrModel, Col, Decision, Objective, ScopeError, Var, clause, eq, le, ne, reduce
from ._diagnose_relation import rows as diagnose_rows
from ._diagnose_relation import run as run_diagnose
from ._output_helpers import assert_no_internal_leak
from ._scope_cases import answer, dump_counts, scope_solver, table, table_rows  # noqa: F401 -- fixture
from solver.types import ObjSense, SolverStatus

X = Var("x")
MAXIMIZE_X = Objective(ObjSense.MAXIMIZE, reduce("sum", X))


def _named(cases):
    return [pytest.param(case, id=case.name) for case in cases]


def _judged(cli, solver, sql, rows, decisions, clauses, objective=MAXIMIZE_X):
    """DeciDB's answer, judged by the oracle; returns the optimum both reach."""
    model = AnrModel(rows, decisions)
    expected = model.solve(solver, clauses, objective)
    assert expected.status == SolverStatus.OPTIMAL, expected.status
    reached = model.judge(answer(cli, sql), clauses, objective)
    assert reached == pytest.approx(expected.objective, abs=1e-6)
    return reached


# ---------------------------------------------------------------------------
# The phase-1 fixtures (§6.3), judged by the oracle
# ---------------------------------------------------------------------------


@pytest.mark.correctness
@pytest.mark.parametrize("case", _named(c for c in CASES if c.phase == 1))
def test_the_engine_answers_each_fixture(decidb_cli, scope_solver, case):
    """F3, F10, F11, F12, F13, F20: the hand-computed optimum, or the error the data raises."""
    result = decidb_cli.execute_raw(case.sql)
    assert_no_internal_leak(result, case.name)
    if case.error:
        assert ScopeError.PHRASES[case.error] in result.stderr, result.stderr
        return
    assert result.returncode == 0 and not result.stderr.strip(), result.stderr
    model = case.model()
    reached = model.judge(answer(decidb_cli, case.sql), case.clauses, case.objective)
    expected = case.optimum
    if expected is None:
        expected = model.solve(scope_solver, case.clauses, case.objective).objective
    assert reached == pytest.approx(expected, abs=1e-6)


# ---------------------------------------------------------------------------
# The rows shape: one model row per instance
# ---------------------------------------------------------------------------


@pytest.mark.correctness
@pytest.mark.parametrize(
    "case, rows", [pytest.param(F10_SUM, 1, id="F10_sum"), pytest.param(F10_SPLIT, 2, id="F10_split")]
)
def test_a_class_is_one_row_per_variant(decidb_cli, tmp_path, case, rows):
    """D77: `a + b` is one field, so F10's class is one row when the sum agrees (3, 3) and
    two when `a` alone differs (1, 2) -- as many as the oracle generates."""
    assert len(case.model().instances(case.clauses[0])) == rows
    assert dump_counts(decidb_cli.dump_model(case.sql, tmp_path / "model.dump"))["num_rows"] == rows


@pytest.mark.correctness
def test_associativity_does_not_change_the_rows(decidb_cli, tmp_path):
    """D77: `t.a + sum(y) + t.b` and `t.a + t.b + sum(y)` read the one field `t.a + t.b`."""
    split = dump_counts(decidb_cli.dump_model(F20_SPLIT_TERMS.sql, tmp_path / "split.dump"))
    joined = dump_counts(decidb_cli.dump_model(F20_JOINED_TERMS.sql, tmp_path / "joined.dump"))
    assert split == joined


COLUMNS = ("id", "g", "h", "b", "v")


def _query(decide, clause_sql, rows, objective="MAXIMIZE sum(x)"):
    return f"""
        SELECT id, x {table('t', COLUMNS, rows)}
        DECIDE {decide}
        SUCH THAT {clause_sql}
        {objective}
        ORDER BY id"""


@pytest.mark.correctness
def test_a_null_value_is_a_variant_of_its_own(decidb_cli, scope_solver):
    """NULL is a value of a field like any other: under `per t.g`, the NULL `h` group of
    the bound's reducer is a variant of class 'a' beside the 'p' group, 5 and 6."""
    rows = ((1, "a", "p", 0, 4), (2, "a", None, 0, 6), (3, "a", "p", 0, 1))
    sql = _query("per t.g: x(INT)", "per t.g: x <= sum(t.v) by (t.h)", rows)
    decisions = [Decision("x", ("g",))]
    clauses = [clause(le(X, reduce("sum", Col("v"), by=("h",))), per=("g",))]
    model = AnrModel(table_rows(COLUMNS, rows), decisions)
    assert len(model.instances(clauses[0])) == 2
    assert _judged(decidb_cli, scope_solver, sql, table_rows(COLUMNS, rows), decisions, clauses) == 3 * 5


@pytest.mark.correctness
def test_equal_values_in_two_classes_stay_two_instances(decidb_cli, tmp_path):
    """Two classes whose bodies agree are two instances, and the model keeps both rows: a
    row the formulation does not merge, harmless since they are one constraint."""
    rows = ((1, "a", "p", 5, 0), (2, "b", "p", 5, 0))
    sql = _query("x(INT), per (): s(INT)", "x <= 1 AND per t.g: s <= t.b", rows, "MAXIMIZE s")
    model = AnrModel(table_rows(COLUMNS, rows), [Decision("x"), Decision("s", ())])
    assert len(model.instances(clause(le(Var("s"), Col("b")), per=("g",)))) == 2
    assert dump_counts(decidb_cli.dump_model(sql, tmp_path / "model.dump"))["num_rows"] == 2


@pytest.mark.correctness
def test_equality_with_two_values_in_a_class_is_infeasible(decidb_cli, scope_solver):
    """`per t.g: x = t.b` over a decision keyed by `g`, with two values of `b` in class 'a':
    two instances that cannot both hold. (The aggregate shape names the contradiction
    before solving; test_error_infeasible.py.)"""
    rows = ((1, "a", "p", 1, 0), (2, "a", "q", 2, 0))
    model = AnrModel(table_rows(COLUMNS, rows), [Decision("x", ("g",))])
    solved = model.solve(scope_solver, [clause(eq(X, Col("b")), per=("g",))], MAXIMIZE_X)
    assert solved.status == SolverStatus.INFEASIBLE
    result = decidb_cli.execute_raw(_query("per t.g: x(INT)", "per t.g: x = t.b", rows))
    assert "is infeasible" in result.stderr, result.stderr


@pytest.mark.correctness
def test_not_equal_keeps_every_variant(decidb_cli, scope_solver):
    """`per t.g: x <> t.b` with `b` 1 and 2 in class 'a': both values are excluded."""
    rows = ((1, "a", "p", 1, 0), (2, "a", "q", 2, 0))
    decisions = [Decision("x", ("g",), ub=2)]
    clauses = [clause(le(X, 2)), clause(ne(X, Col("b")), per=("g",))]
    sql = _query("per t.g: x(INT)", "x <= 2 AND per t.g: x <> t.b", rows)
    assert _judged(decidb_cli, scope_solver, sql, table_rows(COLUMNS, rows), decisions, clauses) == 0


@pytest.mark.correctness
def test_a_decision_the_key_fixes_is_one_instance_per_class(decidb_cli, tmp_path):
    """`x` keyed by `g` and `b` one value per class: one row per class, as before D64."""
    rows = ((1, "a", "p", 1, 0), (2, "a", "q", 1, 0), (3, "b", "p", 2, 0))
    sql = _query("per t.g: x(INT)", "per t.g: x <= t.b", rows)
    assert dump_counts(decidb_cli.dump_model(sql, tmp_path / "model.dump"))["num_rows"] == 2


@pytest.mark.correctness
def test_per_empty_over_a_per_row_decision_is_one_instance_per_row(decidb_cli, scope_solver, tmp_path):
    """`per (): x <= t.b` with `x` per row: one class, one variant per row."""
    rows = ((1, "a", "p", 1, 0), (2, "a", "q", 2, 0), (3, "b", "p", 3, 0))
    sql = _query("x(INT)", "per (): x <= t.b", rows)
    decisions = [Decision("x", PER_ROW)]
    clauses = [clause(le(X, Col("b")), per=())]
    assert _judged(decidb_cli, scope_solver, sql, table_rows(COLUMNS, rows), decisions, clauses) == 1 + 2 + 3
    assert dump_counts(decidb_cli.dump_model(sql, tmp_path / "model.dump"))["num_rows"] == 3


# ---------------------------------------------------------------------------
# What the plan and the diagnosis say about variants
# ---------------------------------------------------------------------------


def _profile(cli, sql: str) -> str:
    result = cli.execute_raw("EXPLAIN ANALYZE " + sql)
    assert result.returncode == 0, result.stderr
    # The box renderer wraps a line at the box's width; join the box's lines back.
    return "".join(line.strip().strip("│").strip() for line in result.stdout.splitlines())


@pytest.mark.correctness
def test_explain_analyze_counts_the_variants(decidb_cli):
    """D65's trap made visible: the inner `per D` reads three values from two depots, and
    F10's class makes two constraints. A plain EXPLAIN runs nothing, so it says neither."""
    text = _profile(decidb_cli, F11_VARYING.sql).replace(" ", "")
    assert "3valuesfrom2classes,oneperdistinct(T.unit,ship)withinaclass" in text, text
    text = _profile(decidb_cli, F10_SPLIT.sql).replace(" ", "")
    assert "pert.g:2constraintsfrom1class,oneperdistinct(a)withinaclass" in text, text
    plain = decidb_cli.execute_raw("EXPLAIN " + F11_VARYING.sql)
    assert "within a class" not in plain.stdout


@pytest.mark.correctness
def test_explain_analyze_is_silent_without_variants(decidb_cli):
    """F10_sum's class has one variant: nothing to say."""
    assert "withinaclass" not in _profile(decidb_cli, F10_SUM.sql).replace(" ", "")


@pytest.mark.diagnose
def test_diagnose_names_the_variant_it_loosens(decidb_cli):
    """`per t.g: x = t.b` with `b` 2 and 3 in class 'a': the expanded diagnosis loosens one
    variant, labelled by its class and its own value."""
    rows = ((1, "a", "p", 2, 0), (2, "a", "q", 3, 0), (3, "b", "p", 1, 0))
    result = run_diagnose(decidb_cli, _query("per t.g: x(INT)", "per t.g: x = t.b", rows), scope="expanded")
    assert_no_internal_leak(result, "a diagnosed variant")
    findings = [row for row in diagnose_rows(result) if row["edit_source"] == "expanded_group"]
    assert [row["group"] for row in findings] == ["a, 2"], result.stdout


def test_diagnose_tells_variants_of_one_value_apart(decidb_cli):
    """`per t.g: x <= t.v` with `v` -2 on both rows of class 'a': two variants, one per
    instance of `x`, with the same value -- each is named by the row it reads as well."""
    rows = ((1, "a", "p", -2, 0), (2, "a", "q", -2, 0), (3, "b", "p", 9, 0))
    result = run_diagnose(decidb_cli, _query("x(INT)", "per t.g: x <= t.b", rows), scope="expanded")
    assert_no_internal_leak(result, "two variants of one value")
    findings = [row for row in diagnose_rows(result) if row["edit_source"] == "expanded_group"]
    assert sorted(row["group"] for row in findings) == ["a, -2, row 1", "a, -2, row 2"], result.stdout
