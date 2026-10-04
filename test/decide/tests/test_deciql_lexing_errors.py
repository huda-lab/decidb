"""Lexing of the DeciQL words, and the catalogue of named errors.

Two halves. The first pins that the DECIDE-only words (`BY`, `AT`, `OVER`,
`FIRST`, `LAST`, `PREVIOUS`, `NEXT`, `EVERY`, `CYCLIC`, `SATISFY`, `ROW`) stay
ordinary identifiers inside a DECIDE clause, that a column named `per` or
`within` is reachable quoted or qualified, that `CASE WHEN` outside the body
still parses, and that a DECIDE query nests in another (syntax_reference §1,
§9; spec §3.1). The second walks the named parser and binder errors of the
2026-09-29 review: every retired or mis-ordered spelling must be refused with a
message that names the fix. Refusal tests assert a short topic phrase only.
"""

import json
import re

import pytest

from solver.types import ObjSense, SolverStatus, VarType

_T = "(VALUES (1, 'a', 3), (2, 'a', 7), (3, 'b', 2)) t(id, grp, cap)"


def _rows(decidb_cli, sql, *cols):
    rows, names = decidb_cli.execute(sql)
    idx = [names.index(c) for c in cols]
    return sorted(tuple(r[i] for i in idx) for r in rows)


def _oracle_caps(oracle_solver, name, caps):
    """MAXIMIZE Σ x with 0 <= x_i <= cap_i: the optimum is every x at its cap."""
    oracle_solver.create_model(name)
    obj = {}
    for i, cap in enumerate(caps):
        oracle_solver.add_variable(f"x_{i}", VarType.INTEGER, lb=0.0, ub=float(cap))
        obj[f"x_{i}"] = 1.0
    oracle_solver.set_objective(obj, ObjSense.MAXIMIZE)
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL
    return result


# ---------------------------------------------------------------------------
# The DECIDE words stay identifiers
# ---------------------------------------------------------------------------

@pytest.mark.var_integer
@pytest.mark.correctness
def test_decide_words_are_ordinary_column_names_in_a_body(decidb_cli, oracle_solver):
    """Columns named by, at, over, first, last, next, previous, every, cyclic,
    satisfy and row are read as data inside a body (spec §3.1: the words are
    contextual). A lexer that rewrote `at` or `by` outside their `AT(` / `BY (`
    positions would fail to parse; the sum of the twelve values is the cap."""
    got = _rows(decidb_cli, """
        SELECT id, x FROM (VALUES (1, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11),
                                  (2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2))
            t(id, by, at, over, first, last, next, previous, every, cyclic, satisfy, row)
        DECIDE x(INT)
        SUCH THAT x <= by + at + over + first + last + next + previous + every + cyclic + satisfy + row
        MAXIMIZE SUM(x)
    """, "id", "x")
    result = _oracle_caps(oracle_solver, "words_as_columns", [66, 22])
    assert got == [(1, 66), (2, 22)]
    assert sum(x for _, x in got) == pytest.approx(result.objective_value)


@pytest.mark.var_integer
@pytest.mark.per_clause
@pytest.mark.correctness
def test_decide_words_as_aliases_keys_and_order_keys(decidb_cli, oracle_solver):
    """A relation aliased `at`, a key column named `at`, an order key named
    `first`: `PER at: SUM(x) BY (at) <= 8` beside `AT(PREVIOUS ELSE 9: cap) OVER
    (first WITHIN at)`. Group g: x1 <= 9 (no previous), x2 <= 3 (previous cap),
    x1 + x2 <= 8; group h: x3 <= 9, x3 <= 8. Optimum 16."""
    rows, cols = decidb_cli.execute("""
        SELECT first, at, x FROM (VALUES (1, 'g', 3), (2, 'g', 7), (3, 'h', 2)) t(first, at, cap)
        DECIDE x(INT)
        SUCH THAT PER at: SUM(x) BY (at) <= 8
              AND x <= AT(PREVIOUS ELSE 9: cap) OVER (first WITHIN at)
        MAXIMIZE SUM(x)
    """)
    xi = cols.index("x")

    oracle_solver.create_model("words_as_keys")
    caps = {1: 9.0, 2: 3.0, 3: 9.0}
    for i, cap in caps.items():
        oracle_solver.add_variable(f"x_{i}", VarType.INTEGER, lb=0.0, ub=cap)
    oracle_solver.add_constraint({"x_1": 1.0, "x_2": 1.0}, "<=", 8.0)
    oracle_solver.add_constraint({"x_3": 1.0}, "<=", 8.0)
    oracle_solver.set_objective({f"x_{i}": 1.0 for i in caps}, ObjSense.MAXIMIZE)
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL
    assert sum(int(r[xi]) for r in rows) == pytest.approx(result.objective_value) == 16


@pytest.mark.var_integer
@pytest.mark.correctness
def test_per_and_within_columns_are_reachable_quoted_or_qualified(decidb_cli, oracle_solver):
    """`per` and `within` are keywords inside the clause, so a column so named is
    written quoted or qualified (syntax_reference §9). Each spelling reads the
    column: x = per = (1, 2). A lexer that rewrote `per` after a dot would fail
    to parse `t.per`."""
    for bound in ('t.per', '"per"', 't.within', '"within"'):
        got = _rows(decidb_cli, f"""
            SELECT id, x FROM (VALUES (1, 1, 3), (2, 2, 4)) t(id, per, within)
            DECIDE x(INT) SUCH THAT x <= {bound} MAXIMIZE SUM(x)
        """, "id", "x")
        caps = [1, 2] if "per" in bound else [3, 4]
        result = _oracle_caps(oracle_solver, "quoted_" + re.sub(r"\W", "", bound), caps)
        assert [x for _, x in got] == caps, bound
        assert sum(x for _, x in got) == pytest.approx(result.objective_value)


@pytest.mark.var_integer
@pytest.mark.per_clause
@pytest.mark.correctness
def test_row_is_the_default_only_in_a_per_key(decidb_cli, oracle_solver):
    """`PER ROW:` is the explicit default (one instance per row, spec §6.1); in a
    frame's `WITHIN row` the word names the column, like `BY (row)`. With rows
    (row='g', id2=1, cap=3), ('g', 2, 7), ('h', 3, 2): WITHIN row partitions by
    the column, so x = (5, 3, 5) under ELSE 5; read as "no partition" the third
    row would see cap 7 instead."""
    got = _rows(decidb_cli, """
        SELECT id, x FROM (VALUES (1, 'g', 1, 3), (2, 'g', 2, 7), (3, 'h', 3, 2)) t(id, row, id2, cap)
        DECIDE x(INT)
        SUCH THAT PER ROW: x <= AT(PREVIOUS ELSE 5: cap) OVER (id2 WITHIN row)
        MAXIMIZE SUM(x)
    """, "id", "x")
    result = _oracle_caps(oracle_solver, "within_row", [5, 3, 5])
    assert [x for _, x in got] == [5, 3, 5]
    assert sum(x for _, x in got) == pytest.approx(result.objective_value)
    # PER ROW is the same clause as no prefix.
    bare = _rows(decidb_cli, """
        SELECT id, x FROM (VALUES (1, 'g', 1, 3), (2, 'g', 2, 7), (3, 'h', 3, 2)) t(id, row, id2, cap)
        DECIDE x(INT)
        SUCH THAT x <= AT(PREVIOUS ELSE 5: cap) OVER (id2 WITHIN row)
        MAXIMIZE SUM(x)
    """, "id", "x")
    assert bare == got


@pytest.mark.var_integer
@pytest.mark.correctness
def test_decisions_may_be_named_after_frame_words(decidb_cli, oracle_solver):
    """`next(INT)` and `previous(INT)` are ordinary decision names; `at`, `by`
    and `over` are too once quoted (their bare spelling followed by `(` is the
    frame / group token)."""
    got = _rows(decidb_cli, """
        SELECT id, next, "at" FROM (VALUES (1, 3), (2, 7)) t(id, cap)
        DECIDE next(INT), "at"(INT)
        SUCH THAT next <= cap AND "at" <= cap - 1
        MAXIMIZE SUM(next) + SUM("at")
    """, "id", "next", "at")
    result = _oracle_caps(oracle_solver, "frame_word_names", [3, 7, 2, 6])
    assert got == [(1, 3, 2), (2, 7, 6)]
    assert sum(n + a for _, n, a in got) == pytest.approx(result.objective_value)


# ---------------------------------------------------------------------------
# CASE WHEN outside the body, subqueries, nesting, scripts
# ---------------------------------------------------------------------------

@pytest.mark.var_integer
@pytest.mark.correctness
def test_case_when_outside_the_body_still_lexes_as_sql(decidb_cli, oracle_solver):
    """`CASE WHEN` in the SELECT list before a split-order DECIDE, in a JOIN ON
    between the two slots, and in a WHERE before a single-block DECIDE all lex
    as ordinary SQL (the DECIDE `WHEN` token is armed only inside the clause).
    Each query still solves x = cap."""
    result = _oracle_caps(oracle_solver, "case_outside", [3, 7])
    split = _rows(decidb_cli, """
        SELECT id, CASE WHEN cap > 5 THEN 'big' ELSE 'small' END AS size, x
        DECIDE x(INT)
        FROM (VALUES (1, 3), (2, 7)) t(id, cap)
        SUCH THAT x <= cap MAXIMIZE SUM(x)
    """, "id", "size", "x")
    assert split == [(1, "small", 3), (2, "big", 7)]
    joined = _rows(decidb_cli, """
        SELECT t.id, x
        DECIDE x(INT)
        FROM (VALUES (1, 3), (2, 7)) t(id, cap)
        JOIN (VALUES (1), (2)) u(id) ON t.id = CASE WHEN u.id > 5 THEN 0 ELSE u.id END
        SUCH THAT x <= cap MAXIMIZE SUM(x)
    """, "id", "x")
    assert joined == [(1, 3), (2, 7)]
    assert sum(x for _, x in joined) == pytest.approx(result.objective_value)
    filtered = _rows(decidb_cli, """
        SELECT id, x FROM (VALUES (1, 3), (2, 7)) t(id, cap)
        WHERE CASE WHEN cap > 5 THEN true ELSE false END
        DECIDE x(INT) SUCH THAT x <= cap MAXIMIZE SUM(x)
    """, "id", "x")
    assert filtered == [(2, 7)]


@pytest.mark.var_integer
@pytest.mark.cons_subquery
@pytest.mark.correctness
def test_a_bound_subquery_keeps_its_own_window_and_order(decidb_cli, oracle_solver):
    """A scalar subquery in a bound is ordinary SQL: its `OVER ()` window and
    `ORDER BY` are not DECIDE frames. The bound is the whole table's cap total."""
    got = _rows(decidb_cli, """
        SELECT id, x FROM (VALUES (1, 3), (2, 7)) t(id, cap)
        DECIDE x(INT)
        SUCH THAT x <= (SELECT MAX(c) FROM (SELECT SUM(cap) OVER () AS c
                                             FROM (VALUES (1, 3), (2, 7)) u(id, cap) ORDER BY id))
        MAXIMIZE SUM(x)
    """, "id", "x")
    result = _oracle_caps(oracle_solver, "window_bound", [10, 10])
    assert got == [(1, 10), (2, 10)]
    assert sum(x for _, x in got) == pytest.approx(result.objective_value)


@pytest.mark.var_integer
@pytest.mark.correctness
def test_a_decide_query_nests_in_another_in_either_clause_order(decidb_cli, oracle_solver):
    """A DECIDE subquery in the FROM of a DECIDE query, inner in split order and
    in single-block order, and through a CTE: the inner solves x = cap and the
    outer reads it (y = x). The split-order inner used to be refused as
    'DECIDE appears twice'."""
    result = _oracle_caps(oracle_solver, "nested_decide", [3, 7])
    for inner in (
        "SELECT id, x DECIDE x(INT) FROM (VALUES (1, 3), (2, 7)) t(id, cap) SUCH THAT x <= cap MAXIMIZE SUM(x)",
        "SELECT id, x FROM (VALUES (1, 3), (2, 7)) t(id, cap) DECIDE x(INT) SUCH THAT x <= cap MAXIMIZE SUM(x)",
    ):
        got = _rows(decidb_cli, f"""
            SELECT s.id, s.x, y FROM ({inner}) s
            DECIDE y(INT) SUCH THAT y <= s.x MAXIMIZE SUM(y)
        """, "id", "x", "y")
        assert got == [(1, 3, 3), (2, 7, 7)]
        assert sum(y for _, _, y in got) == pytest.approx(result.objective_value)
    via_cte = _rows(decidb_cli, """
        WITH solved AS (SELECT id, x FROM (VALUES (1, 3), (2, 7)) t(id, cap)
                        DECIDE x(INT) SUCH THAT x <= cap MAXIMIZE SUM(x))
        SELECT id, y FROM solved DECIDE y(INT) SUCH THAT y <= x MAXIMIZE SUM(y)
    """, "id", "y")
    assert via_cte == [(1, 3), (2, 7)]


@pytest.mark.correctness
def test_lexer_state_does_not_leak_across_statements(decidb_cli):
    """Two DECIDE statements and a plain `CASE WHEN` statement in one script on
    one connection: the DECIDE tokens are disarmed at the end of each clause, so
    the CASE still lexes as SQL and the second DECIDE re-arms cleanly."""
    proc = decidb_cli.execute_script("""
        SELECT id, x FROM (VALUES (1, 3)) t(id, cap) DECIDE x(INT) SUCH THAT x <= cap MAXIMIZE SUM(x);
        SELECT CASE WHEN 1 = 1 THEN 'lexed-as-sql' ELSE 'n' END AS c;
        SELECT id, x FROM (VALUES (2, 7)) t(id, cap) DECIDE x(INT) SUCH THAT x <= cap MAXIMIZE SUM(x);
    """)
    assert proc.stderr.strip() == "", proc.stderr
    assert "lexed-as-sql" in proc.stdout
    assert re.search(r"\b3\b", proc.stdout) and re.search(r"\b7\b", proc.stdout)


@pytest.mark.correctness
def test_case_whitespace_and_comments_are_free(decidb_cli):
    """Keywords in any case, line and block comments inside the clause, spaces
    before the colon and inside `sum ( x )`."""
    got = _rows(decidb_cli, """
        select id, x from (values (1, 3), (2, 7)) t(id, cap)
        decide x(int) /* a block comment */
        such that -- a line comment
            per row : x <= cap
        maximize sum ( x )
    """, "id", "x")
    assert got == [(1, 3), (2, 7)]


# ---------------------------------------------------------------------------
# The named-error catalogue
# ---------------------------------------------------------------------------

_ERRORS = [
    # retired declarator spellings
    ("trailing key", f"SELECT id, x FROM {_T} DECIDE x(INT) BETWEEN 0 AND cap PER grp SUCH THAT x >= 0 MAXIMIZE SUM(x)",
     r"before its name"),
    ("old relation-scoped decl", f"SELECT id, x FROM {_T} DECIDE t.x(INT) SUCH THAT x >= 0 MAXIMIZE SUM(x)",
     r"PER t: x"),
    ("old scalar decl", f"SELECT id, x FROM {_T} DECIDE scalar x(INT) SUCH THAT x >= 0 MAXIMIZE SUM(x)",
     r"PER \(\): x"),
    ("domain missing", f"SELECT id, x FROM {_T} DECIDE x SUCH THAT x >= 0 MAXIMIZE SUM(x)", r"needs a domain"),
    ("INTEGER domain", f"SELECT id, x FROM {_T} DECIDE x(INTEGER) SUCH THAT x >= 0 MAXIMIZE SUM(x)",
     r"not a DECIDE domain"),
    ("DOUBLE domain", f"SELECT id, x FROM {_T} DECIDE x(DOUBLE) SUCH THAT x >= 0 MAXIMIZE SUM(x)",
     r"not a DECIDE domain"),
    ("unknown domain word", f"SELECT id, x FROM {_T} DECIDE x(foo) SUCH THAT x >= 0 MAXIMIZE SUM(x)",
     r"not a DECIDE domain"),
    ("TEXT without values", f"SELECT id, s FROM {_T} DECIDE s(TEXT) SUCH THAT s = 'a' SATISFY", r"lists its values"),
    ("TEXT with parentheses", f"SELECT id, s FROM {_T} DECIDE s(TEXT IN ('a', 'b')) SUCH THAT s = 'a' SATISFY",
     r"brackets"),
    ("TEXT with a bound", f"SELECT id, s FROM {_T} DECIDE s(TEXT IN ['a', 'b']) <= 5 SUCH THAT s = 'a' SATISFY",
     r"takes no BETWEEN"),
    ("SEMI without bounds", f"SELECT id, x FROM {_T} DECIDE x(SEMIINT) SUCH THAT x <= 9 MAXIMIZE SUM(x)",
     r"needs both bounds"),
    ("declarator key missing colon", f"SELECT id, x FROM {_T} DECIDE PER grp x(INT) SUCH THAT x <= cap MAXIMIZE SUM(x)",
     r"ends in a colon"),
    ("declarator key names a decision",
     f"SELECT id, x FROM {_T} DECIDE y(INT) BETWEEN 0 AND 3, PER y: x(INT) SUCH THAT x <= cap MAXIMIZE SUM(x)",
     r"is a decision"),
    # prefix order, colon, retired constraint spellings
    ("PER before WHEN", f"SELECT id, x FROM {_T} DECIDE x(INT) SUCH THAT PER grp WHEN cap > 2: SUM(x) BY (grp) <= 5 MAXIMIZE SUM(x)",
     r"WHEN <filter> PER <key> IF <guard>"),
    ("IF before PER", f"SELECT id, x FROM {_T} DECIDE PER grp: o(BOOL), x(INT) SUCH THAT IF o PER grp: SUM(x) BY (grp) <= 5 MAXIMIZE SUM(x)",
     r"WHEN <filter> PER <key> IF <guard>"),
    ("IF before WHEN", f"SELECT id, x FROM {_T} DECIDE o(BOOL), x(INT) SUCH THAT IF o WHEN cap > 2: x <= 5 MAXIMIZE SUM(x)",
     r"WHEN <filter> PER <key> IF <guard>"),
    ("WHEN IF PER", f"SELECT id, x FROM {_T} DECIDE PER grp: o(BOOL), x(INT) SUCH THAT WHEN cap > 2 IF o PER grp: SUM(x) BY (grp) <= 5 MAXIMIZE SUM(x)",
     r"WHEN <filter> PER <key> IF <guard>"),
    ("constraint key missing colon", f"SELECT id, x FROM {_T} DECIDE x(INT) SUCH THAT PER grp SUM(x) BY (grp) <= 5 MAXIMIZE SUM(x)",
     r"ends in a colon"),
    ("postfix PER", f"SELECT id, x FROM {_T} DECIDE x(INT) SUCH THAT SUM(x) BY (grp) <= 5 PER grp MAXIMIZE SUM(x)",
     r"PER is a prefix"),
    ("postfix WHEN", f"SELECT id, x FROM {_T} DECIDE x(INT) SUCH THAT x <= cap WHEN cap > 2 MAXIMIZE SUM(x)",
     r"WHEN is a prefix"),
    ("SUM(K: e)", f"SELECT id, x FROM {_T} DECIDE x(INT) SUCH THAT PER grp: SUM(grp: x) <= 5 MAXIMIZE SUM(x)",
     r"written with PER"),
    ("SUM(x WHEN c)", f"SELECT id, x FROM {_T} DECIDE x(INT) SUCH THAT SUM(x WHEN grp = 'a') <= 5 AND x <= cap MAXIMIZE SUM(x)",
     r"filter is a prefix"),
    ("comma between constraints", f"SELECT id, x FROM {_T} DECIDE x(INT) SUCH THAT x <= cap, SUM(x) <= 5 MAXIMIZE SUM(x)",
     r"comma-separated"),
    ("OR between constraints", f"SELECT id, x FROM {_T} DECIDE x(INT) SUCH THAT (x <= 1 OR x >= 5) AND x <= 8 MAXIMIZE SUM(x)",
     r"OR does not connect"),
    ("PER key names a decision", f"SELECT id, x FROM {_T} DECIDE o(BOOL), x(INT) SUCH THAT PER o: SUM(x) BY () <= 5 MAXIMIZE SUM(x)",
     r"is a decision"),
    ("BY key names a decision", f"SELECT id, x FROM {_T} DECIDE o(BOOL), x(INT) SUCH THAT SUM(x) BY (o) <= 3 MAXIMIZE SUM(x)",
     r"is a decision"),
    ("NULL in an IN list", f"SELECT id, x FROM {_T} DECIDE x(INT) <= 9 SUCH THAT x IN (1, NULL) MAXIMIZE SUM(x)",
     r"holds a NULL"),
    ("constant body", f"SELECT id, x FROM {_T} DECIDE x(INT) SUCH THAT 5 <= 10 AND x <= cap MAXIMIZE SUM(x)",
     r"reads a decision"),
    ("decision in a WHEN", f"SELECT id, x FROM {_T} DECIDE x(INT) SUCH THAT WHEN x > 1: x <= cap MAXIMIZE SUM(x)",
     r"IF <condition>"),
    ("decision in a reducer's WHEN", f"SELECT id, x FROM {_T} DECIDE x(INT) SUCH THAT x <= cap AND SUM(WHEN x > 0: cap) <= 5 MAXIMIZE SUM(x)",
     r"cannot reference a decision"),
    ("known data in an IF", f"SELECT id, x FROM {_T} DECIDE x(INT) SUCH THAT IF cap > 2: x <= 1 MAXIMIZE SUM(x)",
     r"WHEN <condition>"),
    ("known data inside an AND guard",
     f"SELECT id, x FROM {_T} DECIDE o(BOOL), x(INT) SUCH THAT IF o AND cap > 5: x <= 1 AND SUM(o) >= 1 MAXIMIZE SUM(x)",
     r"reads known data"),
    # frames
    ("AT without OVER", f"SELECT id, x FROM {_T} DECIDE x(INT) SUCH THAT x <= AT(PREVIOUS ELSE 9: cap) MAXIMIZE SUM(x)",
     r"OVER"),
    ("FROM without TO", f"SELECT id, x FROM {_T} DECIDE x(INT) SUCH THAT x <= SUM(FROM PREVIOUS: cap) OVER (id) MAXIMIZE SUM(x)",
     r"both endpoints"),
    ("frame in WHEN", f"SELECT id, x FROM {_T} DECIDE x(INT) SUCH THAT WHEN AT(PREVIOUS ELSE 0: cap) OVER (id) > 3: x <= 1 MAXIMIZE SUM(x)",
     r"cannot filter"),
    ("frame in IF", f"SELECT id, x FROM {_T} DECIDE x(INT) BETWEEN 0 AND 9 SUCH THAT IF AT(PREVIOUS ELSE 0: x) OVER (id) > 3: x <= 1 MAXIMIZE SUM(x)",
     r"cannot guard"),
    ("decision-free frame comparison", f"SELECT id, x FROM {_T} DECIDE x(INT) SUCH THAT AT(PREVIOUS ELSE 9: cap) OVER (id) <= 6 AND x <= 5 MAXIMIZE SUM(x)",
     r"decides nothing"),
    ("ABS over a frame", f"SELECT id, x FROM {_T} DECIDE x(INT) BETWEEN 0 AND 9 SUCH THAT ABS(AT(PREVIOUS ELSE 0: x) OVER (id) - x) <= 2 MAXIMIZE SUM(x)",
     r"ABS over a frame"),
    ("frame in an objective", f"SELECT id, x FROM {_T} DECIDE x(INT) SUCH THAT x <= cap MAXIMIZE SUM(AT(PREVIOUS ELSE 0: x) OVER (id))",
     r"objective has none"),
    ("selector distance 0", f"SELECT id, x FROM {_T} DECIDE x(INT) SUCH THAT x <= AT(0 PREVIOUS ELSE 9: cap) OVER (id) MAXIMIZE SUM(x)",
     r"between 1 and 999"),
    # objectives
    ("keyed objective", f"SELECT id, x FROM {_T} DECIDE x(INT) SUCH THAT x <= cap MAXIMIZE PER grp: SUM(x)", r"takes no key"),
    ("parenthesized keyed objective", f"SELECT id, x FROM {_T} DECIDE x(INT) SUCH THAT x <= cap MAXIMIZE PER (grp): SUM(x)",
     r"takes no key"),
    ("postfix objective PER", f"SELECT id, x FROM {_T} DECIDE x(INT) SUCH THAT x <= cap MAXIMIZE SUM(x) PER grp", r"takes no key"),
    ("PER () without colon", f"SELECT id, x FROM {_T} DECIDE x(INT) SUCH THAT x <= cap MAXIMIZE PER () SUM(x)", r"colon"),
    ("two stages without THEN", f"SELECT id, x FROM {_T} DECIDE x(INT) SUCH THAT x <= cap MAXIMIZE SUM(x) MINIMIZE SUM(x)",
     r"chained with THEN"),
    ("stage then SATISFY", f"SELECT id, x FROM {_T} DECIDE x(INT) SUCH THAT x <= cap MAXIMIZE SUM(x) SATISFY", r"SATISFY"),
    ("SATISFY then stage", f"SELECT id, x FROM {_T} DECIDE x(INT) SUCH THAT x <= cap SATISFY THEN MAXIMIZE SUM(x)", r"SATISFY"),
    ("THEN SATISFY", f"SELECT id, x FROM {_T} DECIDE x(INT) SUCH THAT x <= cap MAXIMIZE SUM(x) THEN SATISFY", r"SATISFY"),
    ("SUCH THAT without a constraint", f"SELECT id, x FROM {_T} DECIDE x(INT) SUCH THAT MAXIMIZE SUM(x)", r"at least one constraint"),
    ("objective before SUCH THAT", f"SELECT id, x FROM {_T} DECIDE x(INT) MAXIMIZE SUM(x) SUCH THAT x <= cap", r"after SUCH THAT"),
    ("no SUCH THAT at all", f"SELECT id, x FROM {_T} DECIDE x(INT) MAXIMIZE SUM(x)", r"SUCH THAT"),
    ("DECIDE in both slots", f"SELECT id, x DECIDE x(INT) FROM {_T} DECIDE x(INT) SUCH THAT x <= cap MAXIMIZE SUM(x)",
     r"appears twice"),
    ("data column objective", f"SELECT id, x FROM {_T} DECIDE x(INT) SUCH THAT x <= cap MAXIMIZE cap", r"data column"),
    ("constant objective", f"SELECT id, x FROM {_T} DECIDE x(INT) SUCH THAT x <= cap MAXIMIZE 1", r"reads no decision"),
    ("row decision alone in an objective", f"SELECT id, x FROM {_T} DECIDE x(INT) SUCH THAT x <= cap MAXIMIZE x", r"varies across rows"),
    # a DECIDE word where a name was meant: the syntax error says how to write the name
    ("unquoted per column", "SELECT id, x FROM (VALUES (1, 3)) t(id, per) DECIDE x(INT) SUCH THAT x <= per MAXIMIZE SUM(x)",
     r"t\.per"),
    ("unquoted within column",
     "SELECT id, x FROM (VALUES (1, 3)) t(id, within) DECIDE x(INT) SUCH THAT x <= within MAXIMIZE SUM(x)",
     r"t\.within"),
    ("if() function inside the clause", f"SELECT id, x FROM {_T} DECIDE x(INT) SUCH THAT x <= if(cap > 5, 6, 2) MAXIMIZE SUM(x)",
     r"if\(\.\.\.\) function"),
    ("decision declared as at(INT)", f"SELECT id, at FROM {_T} DECIDE at(INT) SUCH THAT at <= cap MAXIMIZE SUM(at)",
     r"named at"),
    # bounds that are not one value per instance
    ("CASE beside a reducer",
     f"SELECT id, x FROM {_T} DECIDE x(INT) SUCH THAT x <= cap AND PER (): SUM(WHEN grp = 'a': x) <= CASE WHEN 1 = 1 THEN 2 ELSE 0 END MAXIMIZE SUM(x)",
     r"CASE expressions"),
    ("comparison as a bound", f"SELECT id, x FROM {_T} DECIDE x(INT) SUCH THAT x <= cap AND SUM(x) <= (cap > 5) MAXIMIZE SUM(x)",
     r"one value per instance"),
    # shapes that used to be read wrongly without a word
    ("IF over an IN list",
     "SELECT id, x FROM (VALUES (1), (2)) t(id) DECIDE x(INT) BETWEEN 0 AND 10, o(BOOL) "
     "SUCH THAT IF o: x IN (2, 3) AND SUM(o) = 1 MAXIMIZE SUM(x)", r"cannot be guarded"),
    ("MAX against a frame", f"SELECT id, x FROM {_T} DECIDE x(INT) BETWEEN 0 AND 9 "
     "SUCH THAT PER grp: MAX(x) BY (grp) <= AT(FIRST: cap) OVER (id) MAXIMIZE SUM(x)", r"compared with a frame"),
    ("keyed difference of MIN/MAX", f"SELECT id, x FROM {_T} DECIDE x(INT) BETWEEN 0 AND 9 "
     "SUCH THAT PER grp: MAX(x) BY (grp) - MIN(x) BY (grp) <= 2 MAXIMIZE SUM(x)", r"outer WHEN/PER"),
    ("frame beside a reducer in an objective", f"SELECT id, x FROM {_T} DECIDE x(INT) BETWEEN 0 AND 9 "
     "SUCH THAT x <= cap MAXIMIZE SUM(x) - 3 * AT(FIRST: x) OVER (id)", r"objective has none"),
    ("decision inside a bound subquery",
     f"SELECT id, x FROM {_T} DECIDE x(INT) SUCH THAT x <= cap AND SUM(x) <= (SELECT MAX(cap) + x FROM {_T}) MAXIMIZE SUM(x)",
     r"cannot read a decision"),
]


@pytest.mark.error
@pytest.mark.parametrize("sql,topic", [(sql, topic) for _, sql, topic in _ERRORS], ids=[name for name, _, _ in _ERRORS])
def test_named_errors(decidb_cli, sql, topic):
    """Each retired, mis-ordered or ill-formed spelling is refused with a message
    that names the fix (syntax_reference §1–§6). The assertion is a short topic
    phrase, never the sentence, so the wording may still be improved."""
    decidb_cli.assert_error(sql, match=topic)
