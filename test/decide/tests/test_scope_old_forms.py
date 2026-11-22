"""Old DECIDE spellings fail as any other syntax error does (decisions.md R13).

The grammar has only the current syntax: an old spelling is a syntax error at the token the grammar cannot place,
or, for a declaration, the error the current grammar gives a name with no type. The neighbours of the old forms
that stay are checked here too: `scalar` as a name and a relation-qualified name.
"""

import pytest

from ._scope_cases import table

COLUMNS = ("id", "cap", "g")
ROWS = ((1, 3, "A"), (2, 5, "A"), (3, 7, "B"))
T = table("t", COLUMNS, ROWS)


def _query(declarations="x(INT)", clauses="x <= cap", objective="MAXIMIZE SUM(x)", select="id, x"):
    return f"SELECT {select} {T} DECIDE {declarations} SUCH THAT {clauses} {objective} ORDER BY id"


# ---------------------------------------------------------------------------
# What was removed
# ---------------------------------------------------------------------------


@pytest.mark.parametrize(
    "sql, at",
    [
        pytest.param(_query(clauses="x <= cap AND SUM(x) <= 4 PER g"), "PER g", id="PER-after-a-constraint"),
        pytest.param(_query(clauses="x <= cap AND SUM(x) <= 5 WHEN g = 'A'"), "WHEN g", id="WHEN-after-a-constraint"),
        pytest.param(_query(clauses="x <= cap AND SUM(x) WHEN (g = 'A') <= 5"), "WHEN (g", id="WHEN-after-a-reducer"),
        pytest.param(_query(objective="MAXIMIZE SUM(x) WHEN g = 'A'"), "WHEN g", id="WHEN-after-the-objective"),
        pytest.param(_query(objective="MAXIMIZE SUM(x) PER g"), "PER g", id="PER-after-the-objective"),
    ],
)
def test_old_postfix_is_a_syntax_error(decidb_cli, sql, at):
    decidb_cli.assert_error_at(sql, kind="Parser Error", at=at, word="syntax error")


@pytest.mark.parametrize(
    "sql, at",
    [
        pytest.param(_query("t.x(INT)"), ".x(INT)", id="T.x"),
        pytest.param(_query("x(INT), scalar s(INT)", "x <= cap AND s <= 4"), "s(INT)", id="scalar"),
        pytest.param(_query("x IS INTEGER"), "IS INTEGER", id="IS-type"),
    ],
)
def test_old_declaration_is_a_syntax_error_after_the_name(decidb_cli, sql, at):
    """The name before the old spelling is not refused as a declaration with no type: the syntax error is at the
    token the grammar cannot place."""
    decidb_cli.assert_error_at(sql, kind="Parser Error", at=at, word="syntax error")


@pytest.mark.parametrize(
    "declarations, at",
    [
        pytest.param("x", "DECIDE x", id="bare"),
        pytest.param("per g: x", ": x", id="under-a-per"),
    ],
)
def test_a_declaration_with_no_type_is_refused_at_its_name(decidb_cli, declarations, at):
    decidb_cli.assert_error_at(_query(declarations), kind="Parser Error", at=at)


@pytest.mark.parametrize(
    "clause, at",
    [
        pytest.param("per g: sum(x) by (g) <= 5 PER g", "PER g", id="PER"),
        pytest.param("when g = 'A': x <= 5 WHEN g = 'A'", "WHEN g", id="WHEN"),
    ],
)
def test_a_postfix_after_a_scope_is_a_syntax_error(decidb_cli, clause, at):
    """A postfix written after a clause that already has a scope in front is a syntax error too."""
    decidb_cli.assert_error_at(
        _query(clauses=f"x <= cap AND {clause}"), kind="Parser Error", at=at, word="syntax error"
    )


@pytest.mark.parametrize(
    "sql, at",
    [
        pytest.param(
            _query("per t: open(BOOL)", "t.open <= 1", "MAXIMIZE SUM(open)", "id, open"), "t.open <= 1", id="constraint"
        ),
        pytest.param(
            _query("per t: open(BOOL)", "open <= 1", "MAXIMIZE SUM(t.open)", "id, open"), "SUM(t.open)", id="objective"
        ),
        pytest.param(
            _query("per t: open(BOOL)", "open <= 1", "MAXIMIZE SUM(open)", "id, t.open"), "t.open", id="select-list"
        ),
    ],
)
def test_a_relation_qualified_decision_is_an_unknown_column(decidb_cli, sql, at):
    """`t.open` names no column of t, so each clause refuses it as it refuses any name that is no decision."""
    decidb_cli.assert_error_at(sql, kind="Binder Error", at=at)


# ---------------------------------------------------------------------------
# What stays
# ---------------------------------------------------------------------------


@pytest.mark.parametrize(
    "clauses",
    [
        pytest.param("when t.open = 1: open <= 1", id="when"),
        pytest.param("per t.open: open <= 1", id="per"),
        pytest.param("sum(open) by (t.open) <= 1", id="by"),
    ],
)
def test_a_relation_qualified_decision_in_a_when_per_or_by_is_refused(decidb_cli, clauses):
    """In a `when`, `per` or `by` a decision stands where only data may."""
    decidb_cli.assert_error_at(
        _query("per t: open(BOOL)", clauses, "MAXIMIZE SUM(open)", "id, open"), kind="Binder Error", at="t.open"
    )


def test_scalar_stays_a_name(decidb_cli):
    """`scalar` is a decision name like any other."""
    assert decidb_cli.execute(_query("scalar(INT)", "scalar <= cap", "MAXIMIZE SUM(scalar)", "id, scalar"))[0]


def test_qualified_names_that_are_not_decisions_still_bind(decidb_cli):
    """A struct field or a relation's column named like a decision is data."""
    sql = f"""SELECT id, open FROM (SELECT *, {{'open': cap}} AS s FROM (VALUES (1, 3), (2, 5)) v(id, cap)) t
        DECIDE open(INT) SUCH THAT open <= t.s.open AND open <= s.open MAXIMIZE SUM(open) ORDER BY id"""
    assert decidb_cli.execute(sql)[0] == decidb_cli.execute(sql.replace("t.s.open", "cap").replace("s.open", "cap"))[0]


def test_a_relation_before_a_colon_is_a_syntax_error(decidb_cli):
    """A reducer over a relation's classes is written `sum(per t: x)`; `SUM(t: x)` is no syntax."""
    sql = _query(clauses="x <= cap AND SUM(t: x) <= 8")
    decidb_cli.assert_error_at(sql, kind="Parser Error", at=": x", word="syntax error")
