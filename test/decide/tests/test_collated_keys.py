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


def _assert_no_check_planned(cli, explain_sql):
    """The solver path's plan of `explain_sql` holds no determination check."""
    plan = cli.with_env({"DECIDB_TEST_DIRECT_SOLVE": "off"}).execute_raw(explain_sql)
    assert plan.returncode == 0, plan.stderr
    assert "notdetermine" not in re.sub(r"[│┌┐└┘├┤┬┴┼─\s]", "", plan.stdout)  # the box wraps text mid-token


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
    _assert_no_check_planned(decidb_cli, f"EXPLAIN {sql}")
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
    _assert_no_check_planned(decidb_cli, f"EXPLAIN {sql}")
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
    _assert_no_check_planned(decidb_cli, sql.replace("SELECT", "EXPLAIN SELECT", 1))
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


def _written_order_source(cap_of_a, lo_of_a):
    """Rows 1 and 2 are one class of (g NOCASE, k): `a` and `A`, and `cap` / `lo` hold the given values on row 2."""
    rows = ((1, 1, "a", 5, 0, 5.0), (2, 1, "A", cap_of_a, lo_of_a, 4.0))
    columns = ("id", "k", "g", "cap", "lo", "p")
    return f"FROM (SELECT id, k, g COLLATE NOCASE AS g, cap, lo, p FROM {values(rows, 'v', columns)}) t"


#: The class of rows 1 and 2, named by the least spelling of g, then the value of k.
_CLASS_GK = "t.g = 'A', t.k = 1"
_CLASS_KG = "t.k = 1, t.g = 'A'"


@pytest.mark.parametrize(
    "clauses, cap, lo, subject, cls, body, values_text",
    [
        # Only the first clause fails; it wrote `t.g, t.k`, which is not the order the key keeps its elements in.
        pytest.param(
            "per t.g, t.k: sum(x) <= t.cap AND per t.k, t.g: sum(x) >= t.lo",
            9,
            0,
            "t.g, t.k",
            _CLASS_GK,
            "t.cap",
            "5, 9",
            id="first-clause-fails",
        ),
        # Only the second fails, and it writes the key in another order than the first clause: its own words.
        pytest.param(
            "per t.k, t.g: sum(x) <= t.cap AND per t.g, t.k: sum(x) >= t.lo",
            5,
            9,
            "t.g, t.k",
            _CLASS_GK,
            "t.lo",
            "0, 9",
            id="second-clause-fails",
        ),
        pytest.param(
            "per t.k, t.g: sum(x) <= t.cap AND per t.g, t.k: sum(x) >= t.lo",
            9,
            0,
            "t.k, t.g",
            _CLASS_KG,
            "t.cap",
            "5, 9",
            id="first-clause-fails-in-the-keys-order",
        ),
    ],
)
def test_a_message_quotes_its_clauses_key_and_lists_the_values_in_that_order(
    decidb_cli, clauses, cap, lo, subject, cls, body, values_text
):
    """Two clauses write one key in two orders. The message that names a clause quotes that clause's key as it wrote it
    and lists the class's values in the same order, on the direct path and on the solver path alike."""
    sql = f"""
        SELECT id, x {_written_order_source(cap, lo)}
        DECIDE x(BOOL) SUCH THAT {clauses} MAXIMIZE SUM(t.p * x)"""
    message = f"DECIDE: per {subject} does not determine {body}. {cls} has more than one value: {values_text}."
    decidb_cli.assert_error(sql, match=re.escape(message))
    decidb_cli.with_env({"DECIDB_TEST_DIRECT_SOLVE": "off"}).assert_error(sql, match=re.escape(message))


@pytest.mark.parametrize("direct", ["auto", "off"])
def test_a_reducers_message_quotes_the_key_that_reducer_wrote(decidb_cli, direct):
    """Two reducers qualify by the same columns written in two orders; they share one key, and each message quotes its own
    reducer's `per` and lists the class's values in that order, not in the first reducer's."""
    sql = f"""
        SET decide_direct_solve='{direct}';
        SELECT id, x {_written_order_source(5, 9)}
        DECIDE per t.g, t.k: x(BOOL)
        SUCH THAT sum(per t.k, t.g: t.cap * x) <= 100 AND sum(per t.g, t.k: t.lo * x) >= 0
        MAXIMIZE SUM(x)"""
    message = (
        "DECIDE: per t.g, t.k does not determine t.lo (in sum(per t.g, t.k: t.lo * x)). "
        f"{_CLASS_GK} has more than one value: 0, 9."
    )
    decidb_cli.assert_error(sql, match=re.escape(message))


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


@pytest.mark.correctness
def test_a_key_of_a_collated_and_a_plain_column_groups_each_as_its_column_does(decidb_cli, scope_solver):
    """`sum(x) by (g, k) <= 1`: 'a' and 'A' are one value of the collated g, and k (not collated) tells groups apart
    that g alone would join: (a, 1) holds rows 1 and 3, (a, 2) rows 2 and 4. Only the collated column is spelled by its
    least spelling, so the key reads a label for g and the grouped value for k (BuildGroupIds)."""
    rows = ((1, 1, "a", 1, 5.0), (2, 2, "A", 1, 4.0), (3, 1, "A", 1, 3.0), (4, 2, "a", 1, 2.0))
    sql = f"""
        SELECT id, x {_keyed_source(rows)}
        DECIDE x(BOOL) SUCH THAT sum(x) by (t.g, t.k) <= 1 MAXIMIZE SUM(t.p * x) ORDER BY id"""
    clauses = [Scoped((Reduce("sum", lambda r: {"x": 1}, by=("gk", "k")),), "<=", (lambda r: {CONSTANT: 1},))]
    _keyed_judged(decidb_cli, scope_solver, sql, clauses, MAXIMIZE_PX, rows)


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


@pytest.mark.diagnose
def test_a_finding_spells_a_group_by_its_least_spelling_when_a_later_row_holds_it(decidb_cli):
    """The group's first row spells it 'b', a later row 'B': the finding names 'B', whatever the first row reads."""
    rows = ((1, "b"), (2, "B"), (3, "b"))
    sql = f"""
        SELECT id, x FROM (SELECT id, g COLLATE NOCASE AS g FROM {values(rows, 'v', ('id', 'g'))}) t
        DECIDE x(BOOL) SUCH THAT per t.g: sum(x) >= 4 MAXIMIZE SUM(x)"""
    found = _diagnose_relation.rows(_diagnose_relation.run(decidb_cli, sql, scope="expanded"))
    assert [row["group"] for row in found if row["edit_source"] == "expanded_group"] == ["B"], found


@pytest.mark.correctness
def test_an_inner_per_within_a_collated_by_group(decidb_cli, scope_solver):
    """`sum(x) by (g) <= sum(per g: cap) by (g)`: the bound reducer's `per` key and `by` key are one collated key, whose
    elements the reducer's scope check copies (WithinGroups). Each collated group is bounded by its own cap."""
    sql = f"""
        SELECT id, x {_keyed_source()}
        DECIDE x(BOOL) SUCH THAT sum(x) by (t.g) <= sum(per t.g: t.cap) by (t.g) MAXIMIZE SUM(t.p * x) ORDER BY id"""
    clauses = [
        Scoped(
            (Reduce("sum", lambda r: {"x": 1}, by=("gk",)),),
            "<=",
            (Reduce("sum", lambda r: {CONSTANT: r["cap"]}, per=("gk",), by=("gk",)),),
        )
    ]
    _keyed_judged(decidb_cli, scope_solver, sql, clauses, MAXIMIZE_PX)
