"""The removed spellings (syntax_reference.md §10.3).

Every old spelling is refused with a message that carries the fixed phrase "is removed"
and names its new spelling, built from the user's own expressions (D37), so the
suggestion pastes back. The PER messages add that a NULL key is now its own group (D62);
the `T.x` message warns that `per T` keys on every column of T (D56). `SUM(D: e)` stays
(D1), `scalar` stays usable as a name, and a qualified name that is not a decision
reference still binds (D43).
"""

import pytest

from ._scope_cases import table

COLUMNS = ("id", "cap", "g")
ROWS = ((1, 3, "A"), (2, 5, "A"), (3, 7, "B"))
T = table("t", COLUMNS, ROWS)

def _refused(cli, sql, *fragments):
    stderr = cli.execute_raw(sql, removed_spelling=True).stderr
    assert "is removed" in stderr, stderr
    assert "  " not in stderr.splitlines()[0], stderr
    for fragment in fragments:
        assert fragment in stderr, f"{fragment!r} missing from:\n{stderr}"


def _query(declarations="x(INT)", clauses="x <= cap", objective="MAXIMIZE SUM(x)", select="id, x"):
    return f"SELECT {select} {T} DECIDE {declarations} SUCH THAT {clauses} {objective} ORDER BY id"


# ---------------------------------------------------------------------------
# Declarations
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("sql, fragments", [
    pytest.param(_query("t.x(INT)"), ("per t: x(INT)", "per t keys on every column of t"), id="T.x"),
    pytest.param(_query("t.x"), ("per t: x(",), id="T.x-without-a-type"),
    pytest.param(f"SELECT id, x DECIDE t.x(INT) {T} SUCH THAT x <= cap MAXIMIZE SUM(x) ORDER BY id",
                 ("per t: x(INT)",), id="T.x-before-FROM"),
    pytest.param(_query("x(INT), scalar s(INT)", "x <= cap AND s <= 4", "MAXIMIZE SUM(x) + s"), ("per (): s(INT)",),
                 id="scalar"),
    pytest.param(_query("x(INT), scalar s", "x <= cap AND s <= 4", "MAXIMIZE SUM(x) + s"), ("per (): s(",),
                 id="scalar-without-a-type"),
    pytest.param(_query("x(INT), scalar t.s(INT)", "x <= cap AND s <= 4", "MAXIMIZE SUM(x) + s"), ("per (): s(INT)",),
                 id="scalar-T.x"),
    pytest.param(_query("x(INT), scalar scalar(INT)", "x <= cap AND scalar <= 4", "MAXIMIZE SUM(x) + scalar"),
                 ('per (): "scalar"(INT)',), id="scalar-named-scalar"),
    pytest.param(_query('t."select"(INT)', '"select" <= cap', 'MAXIMIZE SUM("select")', "id"),
                 ('per t: "select"(INT)',), id="T.x-keyword-name-is-quoted"),
    pytest.param(_query('x(INT), scalar "per"(INT)', 'x <= cap AND "per" <= 4', 'MAXIMIZE SUM(x) + "per"'),
                 ('per (): "per"(INT)',), id="scalar-keyword-name-is-quoted"),
])
def test_old_declaration_refused(decidb_cli, sql, fragments):
    _refused(decidb_cli, sql, *fragments)


def test_scalar_stays_a_name(decidb_cli):
    """`scalar` is a decision name like any other."""
    assert decidb_cli.execute(_query("scalar(INT)", "scalar <= cap", "MAXIMIZE SUM(scalar)", "id, scalar"))[0]


# ---------------------------------------------------------------------------
# Constraints
# ---------------------------------------------------------------------------

NULL_NOTE = "a NULL g is now its own group"


@pytest.mark.parametrize("clause, fragments", [
    pytest.param("SUM(x) <= 4 PER g",
                 ("the postfix PER is removed", "per g:", "by (g)", NULL_NOTE, "when g is not null per g:"),
                 id="PER"),
    pytest.param("x <= 2 PER g", ("the postfix PER is removed", "drop the PER"), id="PER-on-a-row-clause"),
    pytest.param("SUM(x) <= 4 PER (g, id)", ("per g, id:", "by (g, id)"), id="PER-tuple"),
    pytest.param("SUM(x) <= 4 PER g, id", ("per g, id:", "by (g, id)"), id="PER-comma"),
    pytest.param("SUM(x) <= COUNT(*) PER g", ("per g:", "by (g)"), id="PER-over-count"),
    pytest.param("norm(x, 1) <= 4 PER g", ("per g:", "by (g)"), id="PER-over-norm"),
    pytest.param("SUM(x) <= 5 WHEN g = 'A'", ("the postfix WHEN is removed", "when g = 'A':"), id="WHEN"),
    pytest.param("SUM(x) <= 5 WHEN g = 'A' PER g", ("when g = 'A' per g:", "by (g)", NULL_NOTE),
                 id="WHEN-PER"),
    pytest.param("SUM(x) WHEN (g = 'A') <= 5", ("sum(when g = 'A': x)",), id="reducer-WHEN"),
    pytest.param("SUM(x) <= SUM(cap) WHEN (g = 'A')", ("sum(when g = 'A': cap)",), id="bound-side-reducer-WHEN"),
    pytest.param("SUM(t: x) WHEN (g = 'A') <= 5", ("sum(when g = 'A' per t: x)",), id="colon-form-WHEN"),
    pytest.param("SUM(x) WHEN (g = 'A') <= 5 PER g", ("per g:",), id="two-old-forms"),
    # The old grammar read a reducer's WHEN condition up to one column; the rest of a longer
    # condition parsed around the marker. The message names the whole condition.
    pytest.param("SUM(x) <= SUM(cap) WHEN cap BETWEEN 2 AND 5", ("sum(when cap BETWEEN 2 AND 5: cap)",),
                 id="reducer-WHEN-BETWEEN"),
    pytest.param("SUM(x) WHEN g IS NULL <= 4", ("sum(when g IS NULL: x)",), id="reducer-WHEN-IS-NULL"),
    pytest.param("SUM(x) <= SUM(cap) WHEN g IN ('A')", ("sum(when g IN ('A'): cap)",), id="reducer-WHEN-IN"),
    pytest.param("SUM(x) WHEN (g = 'A') + SUM(x) WHEN (g = 'B') <= 5",
                 ("sum(when g = 'A': x)", "sum(when g = 'B': x)"), id="two-reducer-WHENs"),
    # A WHEN after a reducer that already has a scope or a by joins it.
    pytest.param("sum(when id > 1: x) WHEN (g = 'A') <= 4", ("sum(when id > 1 and g = 'A': x)",),
                 id="WHEN-after-a-scoped-reducer"),
    pytest.param("sum(per t: x) WHEN (g = 'A') <= 4", ("sum(when g = 'A' per t: x)",), id="WHEN-after-a-keyed-reducer"),
    pytest.param("sum(x) by (g) WHEN (g = 'A') <= 4", ("sum(when g = 'A': x) by (g)",), id="WHEN-after-a-by"),
    pytest.param("SUM(x) WHEN (g = 'A') WHEN (id = 1) <= 4", ("sum(when g = 'A' and id = 1: x)",),
                 id="two-WHENs-after-one-reducer"),
    pytest.param("SUM(x) <= COUNT(*) WHEN (g = 'A')", ("sum(when g = 'A': 1)",), id="count-star-WHEN"),
    pytest.param("norm(x, 1) WHEN (g = 'A') <= 4", ("norm takes no when inside it", "when g = 'A':"),
                 id="norm-WHEN"),
    pytest.param("SUM(x) <= 5 WHEN cap > 3 PER g", ("when cap > 3 per g:", "when cap > 3 and g is not null per g:"),
                 id="WHEN-PER-keeps-the-WHEN-in-the-NULL-note"),
])
def test_old_constraint_spelling_refused(decidb_cli, clause, fragments):
    _refused(decidb_cli, _query(clauses=f"x <= cap AND {clause}"), *fragments)


def test_keyword_named_columns_are_quoted_in_the_suggestion(decidb_cli):
    sql = f"""SELECT id, x {table('t', ('id', 'cap', 'per'), ((1, 3, 'A'), (2, 5, 'B')))}
        DECIDE x(INT) SUCH THAT x <= cap AND SUM(x) <= 4 PER "per" MAXIMIZE SUM(x) ORDER BY id"""
    _refused(decidb_cli, sql, 'per "per":', 'by ("per")')


# ---------------------------------------------------------------------------
# The objective
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("objective, fragments", [
    pytest.param("MAXIMIZE SUM(x) WHEN g = 'A'", ("MAXIMIZE when g = 'A': sum(x)",), id="WHEN"),
    pytest.param("MAXIMIZE SUM(x) WHEN g IN ('A')", ("when g IN ('A'):",), id="WHEN-IN"),
    pytest.param("MINIMIZE SUM(MAX(x)) PER g", ("MINIMIZE sum(per g: max(x) by (g))",), id="nested"),
    pytest.param("MAXIMIZE AVG(SUM(x)) PER g", ("sum(per g: sum(x) by (g))",), id="nested-outer-avg"),
    pytest.param("MAXIMIZE SUM(x) PER g", ("MAXIMIZE sum(x)",), id="flat"),
    pytest.param("MAXIMIZE MAX(x) PER g", ("max(x)", "sum(per g: max(x) by (g))"), id="flat-max"),
    pytest.param("MAXIMIZE MIN(x) PER g", ("min(x)", "sum(per g: min(x) by (g))"), id="flat-min"),
    pytest.param("MAXIMIZE when id > 0: SUM(x) WHEN g = 'A'", (), id="old-and-new"),
    pytest.param("MAXIMIZE SUM(x) WHEN cap + 1 > 3", ("MAXIMIZE when cap + 1 > 3: sum(x)",), id="WHEN-arithmetic"),
    pytest.param("MAXIMIZE SUM(x) WHEN g IN (SELECT 'A')", ("ANY(SELECT 'A')", ": sum(x)"), id="WHEN-IN-subquery"),
    pytest.param("MAXIMIZE 2 * SUM(x) WHEN g IN ('A')", ("sum(when g IN ('A'): x)",), id="WHEN-under-a-factor"),
    pytest.param("MINIMIZE SUM(MAX(x)) PER g",
                 ("MINIMIZE sum(per g: max(x) by (g))", "MINIMIZE when g is not null: sum(per g: max(x) by (g))"),
                 id="nested-NULL-note"),
    pytest.param("MINIMIZE SUM(MAX(x) WHEN (cap > 2)) PER g", ("MINIMIZE when cap > 2: sum(per g: max(x) by (g))",),
                 id="nested-inner-WHEN"),
    pytest.param("MINIMIZE SUM(MAX(t: x)) PER g", ("takes no per inside its inner reducer yet",),
                 id="nested-inner-colon-form"),
    pytest.param("MINIMIZE 2 * SUM(MAX(x)) PER g", ("is not supported yet",), id="nested-under-a-factor"),
    pytest.param("MAXIMIZE AVG(x * cap) PER g",
                 ("MAXIMIZE when g is not null: sum(per g: avg(", "for one average over all rows"), id="flat-avg"),
])
def test_old_objective_spelling_refused(decidb_cli, objective, fragments):
    _refused(decidb_cli, _query(objective=objective), *fragments)


def test_count_star_is_quoted_as_written(decidb_cli):
    """`count(*)` is `count_star()` to the parser; a message quotes what the user wrote."""
    for objective in ("MAXIMIZE COUNT(*) WHEN g = 'A'", "MAXIMIZE COUNT(*) PER g", "MAXIMIZE SUM(x) + COUNT(*) PER g"):
        stderr = decidb_cli.execute_raw(_query(objective=objective), removed_spelling=True).stderr
        assert "is removed" in stderr and "count(*)" in stderr and "count_star" not in stderr, stderr


def test_a_postfix_after_a_scope_is_a_syntax_error(decidb_cli):
    """A removed form written after a clause that already has a scope in front is a plain
    syntax error: the grammar does not read it far enough to name it."""
    for clause in ("per g: sum(x) by (g) <= 5 PER g", "when g = 'A': x <= 5 WHEN g = 'A'"):
        stderr = decidb_cli.execute_raw(_query(clauses=f"x <= cap AND {clause}")).stderr
        assert "syntax error at or near" in stderr, stderr


# ---------------------------------------------------------------------------
# Wherever the query sits
# ---------------------------------------------------------------------------

OLD = _query(clauses="x <= cap AND SUM(x) <= 4 PER g")


@pytest.mark.parametrize("sql", [
    pytest.param(f"EXPLAIN {OLD}", id="EXPLAIN"),
    pytest.param(f"DIAGNOSE {OLD}", id="DIAGNOSE"),
    pytest.param(f"WITH q AS ({OLD}) SELECT * FROM q", id="CTE"),
    pytest.param(f"""SELECT id, y {T} DECIDE y(INT)
        SUCH THAT y <= (SELECT max(x) FROM ({OLD})) MAXIMIZE SUM(y) ORDER BY id""", id="nested-DECIDE"),
    pytest.param(f"CREATE TEMP VIEW v AS {OLD}", id="VIEW"),
    pytest.param(f"PREPARE p AS {OLD}", id="PREPARE"),
])
def test_old_spelling_refused_in_every_position(decidb_cli, sql):
    _refused(decidb_cli, sql, "per g:")


@pytest.mark.parametrize("bound, fragment", [
    pytest.param("(SELECT SUM(cap) WHEN (g = 'A') FROM (VALUES (1, 'A')) u(cap, g))", "sum(when g = 'A': cap)",
                 id="select-list"),
    pytest.param("(SELECT 1 FROM (VALUES (1, 'A')) u(cap, g) HAVING SUM(cap) WHEN (g = 'A') > 0)",
                 "sum(when g = 'A': cap)", id="having"),
])
def test_old_spelling_inside_a_plain_subquery_is_refused(decidb_cli, bound, fragment):
    """A subquery inside a clause is read with the clause's spellings, so a removed form there
    is refused by name, never as an internal function."""
    _refused(decidb_cli, _query(clauses=f"x <= cap AND x <= {bound}"), fragment)


def test_colon_form_inside_a_plain_subquery_is_named(decidb_cli):
    """The colon form stays a spelling, but only for a DECIDE clause's own reducers."""
    sql = _query(clauses="x <= cap AND x <= (SELECT SUM(u: cap) FROM (VALUES (1, 'A')) u(cap, g))")
    stderr = decidb_cli.execute_raw(sql).stderr
    assert "the colon form sum(u: cap) is not supported inside a subquery yet" in stderr, stderr


# ---------------------------------------------------------------------------
# A decision referenced with a relation (D43)
# ---------------------------------------------------------------------------

BARE = "DECIDE: write open, not t.open; a decision is referenced by its bare name"
DATA = "DECIDE: t.open is a decision; when, per and by may only use data"


@pytest.mark.parametrize("sql, message", [
    pytest.param(_query("per t: open(BOOL)", "t.open <= 1", "MAXIMIZE SUM(open)", "id, open"), BARE,
                 id="constraint"),
    pytest.param(_query("per t: open(BOOL)", "open <= 1", "MAXIMIZE SUM(t.open)", "id, open"), BARE, id="objective"),
    pytest.param(_query("per t: open(BOOL)", "open <= 1", "MAXIMIZE SUM(open)", "id, t.open"), BARE,
                 id="select-list"),
    pytest.param(_query("per t: open(BOOL)", "open <= 1", "MAXIMIZE SUM(open)", "id, open") + ", t.open", BARE,
                 id="order-by"),
    pytest.param(_query("open(INT)", "open <= cap AND sum(open) <= 2 * t.open", "MAXIMIZE SUM(open)", "id, open"),
                 BARE, id="per-row-decision"),
    pytest.param(_query("per t: open(BOOL)", "when t.open = 1: open <= 1", "MAXIMIZE SUM(open)", "id, open"), DATA,
                 id="when"),
    pytest.param(_query("per t: open(BOOL)", "per t.open: open <= 1", "MAXIMIZE SUM(open)", "id, open"), DATA,
                 id="per"),
    pytest.param(_query("per t: open(BOOL)", "sum(open) by (t.open) <= 1", "MAXIMIZE SUM(open)", "id, open"), DATA,
                 id="by"),
])
def test_relation_qualified_decision_reference_refused(decidb_cli, sql, message):
    """A decision is referenced by its bare name (D43); in a `when`, `per` or `by` it is
    a decision where only data may stand."""
    stderr = decidb_cli.execute_raw(sql).stderr
    assert message in stderr, stderr


def test_qualified_names_that_are_not_decisions_still_bind(decidb_cli):
    """A struct field or a relation's column named like a decision is data."""
    sql = f"""SELECT id, open FROM (SELECT *, {{'open': cap}} AS s FROM (VALUES (1, 3), (2, 5)) v(id, cap)) t
        DECIDE open(INT) SUCH THAT open <= t.s.open AND open <= s.open MAXIMIZE SUM(open) ORDER BY id"""
    assert decidb_cli.execute(sql)[0] == decidb_cli.execute(sql.replace("t.s.open", "cap").replace("s.open", "cap"))[0]


# ---------------------------------------------------------------------------
# What stays
# ---------------------------------------------------------------------------

def test_colon_form_stays(decidb_cli):
    """`SUM(D: e)` is still a spelling (D1), the same as `sum(per D: e)` (S4)."""
    assert decidb_cli.execute(_query(clauses="x <= cap AND SUM(t: x) <= 8"))[0] == \
        decidb_cli.execute(_query(clauses="x <= cap AND sum(per t: x) <= 8"))[0]


@pytest.mark.parametrize("old, suggested", [
    pytest.param("SUM(x) <= 4 PER g", "when g is not null per g: sum(x) by (g) <= 4", id="PER"),
    pytest.param("SUM(x) <= 5 WHEN g = 'A'", "when g = 'A': sum(x) <= 5", id="WHEN"),
])
def test_suggestion_pastes_back(decidb_cli, old, suggested):
    """The spelling a message suggests is a query that runs."""
    _refused(decidb_cli, _query(clauses=f"x <= cap AND {old}"))
    assert decidb_cli.execute(_query(clauses=f"x <= cap AND {suggested}"))[0]


@pytest.mark.parametrize("old, suggested, equivalent", [
    pytest.param("SUM(x) <= SUM(cap) WHEN cap BETWEEN 2 AND 5", "sum(when cap BETWEEN 2 AND 5: cap)",
                 "(SELECT sum(cap) FROM t WHERE cap BETWEEN 2 AND 5)", id="BETWEEN"),
    pytest.param("SUM(x) <= SUM(cap) WHEN g IN ('A')", "sum(when g IN ('A'): cap)",
                 "(SELECT sum(cap) FROM t WHERE g IN ('A'))", id="IN"),
])
def test_suggested_condition_keeps_the_written_one(decidb_cli, old, suggested, equivalent):
    """The suggestion quotes the whole condition: pasted in place of the removed reducer, it
    gives the answer of the same bound computed by a filtered subquery."""
    stderr = decidb_cli.execute_raw(_query(clauses=f"x <= cap AND {old}"), removed_spelling=True).stderr
    assert "is removed" in stderr and suggested in stderr, stderr
    pasted = old.split(" <= ")[0] + " <= " + suggested
    with_subquery = f"WITH t AS (SELECT * {T}) SELECT id, x FROM t DECIDE x(INT) SUCH THAT x <= cap AND "
    assert decidb_cli.execute(_query(clauses=f"x <= cap AND {pasted}"))[0] == \
        decidb_cli.execute(with_subquery + f"SUM(x) <= {equivalent} MAXIMIZE SUM(x) ORDER BY id")[0]
