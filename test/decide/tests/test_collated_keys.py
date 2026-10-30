"""A key groups rows as GROUP BY does: under its column's collation (B1).

`COLLATE NOCASE` puts 'a' and 'A' in one group, for a `by` group, a `per K:` class, a bound-side reducer and a keyed
decision; a message quotes the group's least spelling ('A'). The oracle states NOCASE on its own: it groups on the
lowercased key `gk`.
"""

import re

import pytest

from . import _diagnose_relation
from ._scope_cases import aligned, answer, scope_solver, values  # noqa: F401
from ._scope_oracle import CONSTANT, Decision, Reduce, Scoped, ScopedModel, ScopedObjective
from solver.types import ObjSense, VarType

COLUMNS = ("id", "g", "cap", "p")
ROWS = ((1, "a", 1, 5.0), (2, "A", 2, 4.0), (3, "b", 2, 3.0), (4, "B", 3, 2.0), (5, "b", 2, 1.0))
X = Decision("x", var_type=VarType.BINARY)
MAXIMIZE_PX = ScopedObjective(ObjSense.MAXIMIZE, (Reduce("sum", lambda r: {"x": r["p"]}),))


def _source(rows=ROWS, collation="COLLATE NOCASE"):
    return f"FROM (SELECT id, g {collation} AS g, cap, p FROM {values(rows, 'v', COLUMNS)}) t"


def _oracle_rows(rows=ROWS):
    return [dict(zip(COLUMNS, row), gk=row[1].lower()) for row in rows]


def _judged(cli, solver, sql, decisions, clauses, objective):
    model = ScopedModel(_oracle_rows(), decisions)
    result = aligned(answer(cli, sql), model, "id")
    assert model.judge_scoped(result, clauses, objective) == pytest.approx(
        model.solve_scoped(solver, clauses, objective)
    )
    return result


@pytest.mark.correctness
def test_a_by_group_groups_its_key_under_the_collation(decidb_cli, scope_solver):
    """`sum(x) by (g) <= 1`: 'a' and 'A' are one group, so one of rows 1 and 2 is picked, and one of rows 3-5."""
    sql = f"SELECT id, x {_source()} DECIDE x(BOOL) SUCH THAT sum(x) by (t.g) <= 1 MAXIMIZE SUM(t.p * x) ORDER BY id"
    clauses = [Scoped((Reduce("sum", lambda r: {"x": 1}, by=("gk",)),), "<=", (lambda r: {CONSTANT: 1},))]
    result = _judged(decidb_cli, scope_solver, sql, [X], clauses, MAXIMIZE_PX)
    assert sum(row["x"] for row in result) == 2


@pytest.mark.correctness
def test_a_collated_per_key_determines_its_own_by_group(decidb_cli, scope_solver):
    """`per g: sum(x) by (g) <= 1`: the class and the `by` group both group under NOCASE, so the key determines its
    own `by` element by construction and no check is planned (KeyHolds); 'a' and 'A' are still one class."""
    sql = f"""
        SELECT id, x {_source()}
        DECIDE x(BOOL) SUCH THAT per t.g: sum(x) by (t.g) <= 1 MAXIMIZE SUM(t.p * x) ORDER BY id"""
    plan = decidb_cli.with_env({"DECIDB_TEST_DIRECT_SOLVE": "off"}).execute_raw(f"EXPLAIN {sql}")
    assert plan.returncode == 0, plan.stderr
    assert "notdetermine" not in re.sub(r"[│┌┐└┘├┤┬┴┼─\s]", "", plan.stdout)  # the box wraps text mid-token
    clauses = [Scoped((Reduce("sum", lambda r: {"x": 1}, by=("gk",)),), "<=", (lambda r: {CONSTANT: 1},), per=("gk",))]
    result = _judged(decidb_cli, scope_solver, sql, [X], clauses, MAXIMIZE_PX)
    assert sum(row["x"] for row in result) == 2


def test_a_by_key_under_another_collation_is_another_key(decidb_cli):
    """`per g: sum(x) by (g COLLATE noaccent) <= 1` over a NOCASE g: the two collations group differently, so they are
    two keys (DecideGroupsAlike: the collations differ) and the class {a, A} holds two `by` groups."""
    sql = f"""
        SELECT id, x {_source()}
        DECIDE x(BOOL) SUCH THAT per t.g: sum(x) by (t.g COLLATE noaccent) <= 1 MAXIMIZE SUM(t.p * x)"""
    decidb_cli.assert_error(sql, match=r"per t\.g does not determine t\.g COLLATE noaccent")


@pytest.mark.correctness
def test_a_by_key_written_under_the_columns_collation_is_the_same_key(decidb_cli, scope_solver):
    """`per g: sum(x) by (g COLLATE nocase) <= 1` over a NOCASE g: one collation, so one key (DecideGroupsAlike: the
    collations are equal) and no check is planned."""
    sql = f"""
        SELECT id, x {_source()}
        DECIDE x(BOOL) SUCH THAT per t.g: sum(x) by (t.g COLLATE nocase) <= 1 MAXIMIZE SUM(t.p * x) ORDER BY id"""
    plan = decidb_cli.with_env({"DECIDB_TEST_DIRECT_SOLVE": "off"}).execute_raw(f"EXPLAIN {sql}")
    assert plan.returncode == 0, plan.stderr
    assert "notdetermine" not in re.sub(r"[│┌┐└┘├┤┬┴┼─\s]", "", plan.stdout)  # the box wraps text mid-token
    clauses = [Scoped((Reduce("sum", lambda r: {"x": 1}, by=("gk",)),), "<=", (lambda r: {CONSTANT: 1},), per=("gk",))]
    _judged(decidb_cli, scope_solver, sql, [X], clauses, MAXIMIZE_PX)


@pytest.mark.correctness
def test_the_default_collation_and_the_same_written_collation_are_one_key(decidb_cli, scope_solver):
    """Under `default_collation='nocase'`, a plain g and `g COLLATE NOCASE` group under one collation function, though
    their types name different collations, so they are one key (Grouping: equal) and no check is planned."""
    sql = f"""
        SET default_collation='nocase';
        SELECT id, x {_source(collation="")}
        DECIDE x(BOOL) SUCH THAT per t.g: sum(x) by (t.g COLLATE NOCASE) <= 1 MAXIMIZE SUM(t.p * x) ORDER BY id"""
    plan = decidb_cli.with_env({"DECIDB_TEST_DIRECT_SOLVE": "off"}).execute_raw(
        sql.replace("SELECT", "EXPLAIN SELECT", 1)
    )
    assert plan.returncode == 0, plan.stderr
    assert "notdetermine" not in re.sub(r"[│┌┐└┘├┤┬┴┼─\s]", "", plan.stdout)  # the box wraps text mid-token
    clauses = [Scoped((Reduce("sum", lambda r: {"x": 1}, by=("gk",)),), "<=", (lambda r: {CONSTANT: 1},), per=("gk",))]
    _judged(decidb_cli, scope_solver, sql, [X], clauses, MAXIMIZE_PX)


@pytest.mark.correctness
def test_a_bound_side_reducer_reads_its_group_under_the_collation(decidb_cli, scope_solver):
    """`sum(x) by (g) <= min(cap) by (g)`: the bound is read over 'a' and 'A' together, as the sum is."""
    sql = f"""
        SELECT id, x {_source()}
        DECIDE x(BOOL) SUCH THAT sum(x) by (t.g) <= min(t.cap) by (t.g) MAXIMIZE SUM(t.p * x) ORDER BY id"""
    clauses = [
        Scoped(
            (Reduce("sum", lambda r: {"x": 1}, by=("gk",)),),
            "<=",
            (Reduce("min", lambda r: {CONSTANT: r["cap"]}, by=("gk",)),),
        )
    ]
    _judged(decidb_cli, scope_solver, sql, [X], clauses, MAXIMIZE_PX)


@pytest.mark.correctness
def test_a_keyed_decision_has_one_value_per_collated_key(decidb_cli, scope_solver):
    """`per g: y` with `y <= cap`: rows 1 ('a', cap 1) and 2 ('A', cap 2) share one decision, so both hold 1."""
    sql = f"""
        SELECT id, y {_source()}
        DECIDE per t.g: y(INT) SUCH THAT y <= t.cap MAXIMIZE SUM(t.p * y) ORDER BY id"""
    clauses = [Scoped((lambda r: {"y": 1},), "<=", (lambda r: {CONSTANT: r["cap"]},))]
    objective = ScopedObjective(ObjSense.MAXIMIZE, (Reduce("sum", lambda r: {"y": r["p"]}),))
    result = _judged(decidb_cli, scope_solver, sql, [Decision("y", key=("gk",))], clauses, objective)
    assert [row["y"] for row in result[:2]] == [1, 1]


def test_a_message_quotes_the_least_spelling_of_the_group(decidb_cli):
    """`per g` does not determine `cap` once 'a' (cap 1) and 'A' (cap 3) are one class; the message names 'A'."""
    rows = ((1, "a", 1, 5.0), (2, "A", 3, 4.0))
    sql = f"SELECT id, x {_source(rows)} DECIDE x(BOOL) SUCH THAT per t.g: sum(x) <= t.cap MAXIMIZE SUM(t.p * x)"
    decidb_cli.assert_error(sql, match=r"t\.g = 'A' has more than one value")


KEYED_COLUMNS = ("id", "k", "g", "cap", "p")
# `cap` is one value per NOCASE group, so an inner `per g` reads one per class.
KEYED_ROWS = ((1, 1, "a", 1, 5.0), (2, 1, "A", 1, 4.0), (3, 2, "b", 2, 3.0), (4, 2, "B", 2, 2.0))


def _keyed_source(rows=KEYED_ROWS):
    return f"FROM (SELECT id, k, g COLLATE NOCASE AS g, cap, p FROM {values(rows, 'v', KEYED_COLUMNS)}) t"


def _keyed_judged(cli, solver, sql, clauses, objective, rows=KEYED_ROWS):
    model = ScopedModel([dict(zip(KEYED_COLUMNS, row), gk=row[2].lower()) for row in rows], [X])
    result = aligned(answer(cli, sql), model, "id")
    assert model.judge_scoped(result, clauses, objective) == pytest.approx(
        model.solve_scoped(solver, clauses, objective)
    )


@pytest.mark.correctness
def test_an_inner_per_counts_each_collated_class_once(decidb_cli, scope_solver):
    """`sum(x) <= sum(per g: cap)`: the classes are {a, A} and {b, B}, so the bound is 1 + 2 = 3, not 6."""
    sql = f"""
        SELECT id, x {_keyed_source()}
        DECIDE x(BOOL) SUCH THAT sum(x) <= sum(per t.g: t.cap) MAXIMIZE SUM(t.p * x) ORDER BY id"""
    clauses = [
        Scoped(
            (Reduce("sum", lambda r: {"x": 1}),),
            "<=",
            (Reduce("sum", lambda r: {CONSTANT: r["cap"]}, per=("gk",)),),
            per=(),
        )
    ]
    _keyed_judged(decidb_cli, scope_solver, sql, clauses, MAXIMIZE_PX)


@pytest.mark.correctness
def test_a_per_key_determines_a_collated_by_group(decidb_cli, scope_solver):
    """`per k: sum(x) by (g) <= 1`: class k = 1 holds 'a' and 'A', which are one `by` group, so `k` determines it."""
    sql = f"""
        SELECT id, x {_keyed_source()}
        DECIDE x(BOOL) SUCH THAT per t.k: sum(x) by (t.g) <= 1 MAXIMIZE SUM(t.p * x) ORDER BY id"""
    clauses = [Scoped((Reduce("sum", lambda r: {"x": 1}, by=("gk",)),), "<=", (lambda r: {CONSTANT: 1},), per=("k",))]
    _keyed_judged(decidb_cli, scope_solver, sql, clauses, MAXIMIZE_PX)


@pytest.mark.correctness
def test_an_objective_reads_one_collated_by_group(decidb_cli, scope_solver):
    """`MAXIMIZE sum(p * x) by (g)` over rows 'a' and 'A' alone: one `by` group, so the objective is produced once."""
    rows = KEYED_ROWS[:2]
    sql = f"""
        SELECT id, x {_keyed_source(rows)}
        DECIDE x(BOOL) SUCH THAT sum(x) <= 1 MAXIMIZE sum(t.p * x) by (t.g) ORDER BY id"""
    clauses = [Scoped((Reduce("sum", lambda r: {"x": 1}),), "<=", (lambda r: {CONSTANT: 1},), per=())]
    objective = ScopedObjective(ObjSense.MAXIMIZE, (Reduce("sum", lambda r: {"x": r["p"]}, by=("gk",)),))
    _keyed_judged(decidb_cli, scope_solver, sql, clauses, objective, rows)


# 'b' is read on rows 4-5 by the `when` clause and on rows 1-5 by the other: one group, spelled 'B' over every row read.
SPELLING_ROWS = ((1, "a"), (2, "A"), (3, "B"), (4, "b"), (5, "b"))
NARROW = "when t.id >= 4 per t.g: sum(x) by (t.g) >= 3"
WIDE = "per t.g: sum(x) by (t.g) <= 1"


@pytest.mark.diagnose
@pytest.mark.parametrize("clauses", [(NARROW, WIDE), (WIDE, NARROW)], ids=["narrow-first", "wide-first"])
def test_every_finding_spells_a_group_by_its_least_spelling_over_the_rows_read(decidb_cli, clauses):
    """The `when` clause alone reads only 'b'; the other clause reads 'B' too. Each finding on the group names it 'B',
    the least spelling over every row the query read the key on, whichever clause reads it first (ResolveGroupNames)."""
    sql = f"""
        SELECT id, x FROM (SELECT id, g COLLATE NOCASE AS g FROM {values(SPELLING_ROWS, 'v', ('id', 'g'))}) t
        DECIDE x(BOOL) SUCH THAT {clauses[0]} AND {clauses[1]} MAXIMIZE SUM(x)"""
    rows = _diagnose_relation.rows(_diagnose_relation.run(decidb_cli, sql, scope="expanded"))
    groups = sorted((row["clause"], row["group"]) for row in rows if row["edit_source"] == "expanded_group")
    assert groups == sorted([(NARROW, "B"), (WIDE, "B")]), rows
