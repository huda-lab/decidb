"""A message that names a value quotes the span of the query the user wrote for it (MSG-1).

The parser keeps where every expression of a DECIDE tail sits; a node's query location names its span, and a message
cuts the text instead of reprinting the bound tree. A node the engine built has no location and prints its operator
over the spans of its children. The cases below are the messages that used to print what the engine built: a cast
chain, a CASE it expanded, `CAST(w AS DECIMAL(12,1)) + CAST(1 AS DECIMAL(12,1))`.
"""

import pytest

from ._output_helpers import assert_no_internal_leak

#: `w` is a DECIMAL, so a reprint of the bound tree shows the casts the binder added around it.
_DATA = "(VALUES (1, 'F', 2.5, 1, 'a'), (2, 'G', 3.5, 2, 'a'), (3, 'F', 4.5, 3, 'b')) t(id, s, w, b, g)"


def _query(clauses, objective="MAXIMIZE SUM(x)", var="x(REAL)"):
    return f"SELECT id, x FROM {_DATA} DECIDE {var} SUCH THAT {clauses} {objective}"


def _stderr(cli, sql):
    result = cli.execute_raw(sql)
    assert_no_internal_leak(result, sql)
    return result.stderr


# ---------------------------------------------------------------------------
# A conditional the user wrote as `if`
# ---------------------------------------------------------------------------


@pytest.mark.error
def test_an_if_is_refused_as_an_if_not_as_a_case(decidb_cli):
    """`if(c, a, b)` reaches the binder as the parser's CASE; the refusal names what was written (the shape check's
    WrittenConditional) and points at it."""
    sql = _query("SUM(if(s = 'F', x, 2 * x)) <= 3")
    decidb_cli.assert_error_at(
        sql, kind="Not implemented Error", at="if(s = 'F', x, 2 * x)", word="IF over a decision is not supported"
    )
    assert "CASE over" not in _stderr(decidb_cli, sql)


@pytest.mark.error
def test_a_case_is_still_refused_as_a_case(decidb_cli):
    """The branch of WrittenConditional that keeps the word `CASE` when the text starts with it."""
    decidb_cli.assert_error_at(
        _query("SUM(CASE WHEN s = 'F' THEN x ELSE 2 * x END) <= 3"),
        kind="Not implemented Error",
        at="CASE WHEN s = 'F' THEN x ELSE 2 * x END",
        word="CASE over a decision is not supported",
    )


# ---------------------------------------------------------------------------
# A key or a term the engine rebuilt
# ---------------------------------------------------------------------------


@pytest.mark.error
def test_a_product_term_is_named_by_its_operator_over_the_written_factors(decidb_cli):
    """`SUM(x) * (w * x)` is refused at its decision factor: canonicalization built the product, so a message prints
    nothing of it, and the casts around `w` (a DECIMAL) do not appear."""
    sql = _query("SUM(x) * (w * x) <= 7")
    decidb_cli.assert_error_at(
        sql,
        kind="Binder Error",
        at="SUM(x) * (w * x)",
        word="'x' is a REAL decision, which cannot scale a reducer",
    )
    stderr = _stderr(decidb_cli, sql)
    assert "CAST" not in stderr and '"*"' not in stderr, stderr


@pytest.mark.error
def test_a_case_factor_is_quoted_as_written(decidb_cli):
    decidb_cli.assert_error_at(
        _query("SUM(x) <= 7", "MAXIMIZE SUM(x) * CASE WHEN s = 'F' THEN 1 ELSE 2 END"),
        kind="Binder Error",
        at="CASE WHEN s = 'F' THEN 1 ELSE 2 END",
        word="'CASE WHEN s = 'F' THEN 1 ELSE 2 END' varies",
    )


@pytest.mark.error
def test_a_sum_in_a_factor_is_quoted_as_written_and_parenthesized_in_the_suggestion(decidb_cli):
    """`(w + 1) * SUM(x)`: the factor is `w + 1`, as written, and the message points at it."""
    sql = _query("SUM(x) <= 7", "MAXIMIZE (w + 1) * SUM(x)")
    decidb_cli.assert_error_at(
        sql, kind="Binder Error", at="w + 1", word="'w + 1' varies per row, which cannot scale a reducer"
    )
    assert "DECIMAL" not in _stderr(decidb_cli, sql)


# ---------------------------------------------------------------------------
# What a cut keeps
# ---------------------------------------------------------------------------


@pytest.mark.error
def test_a_call_that_binds_as_a_case_takes_no_parentheses_under_a_product(decidb_cli):
    """`if(...)` binds as a CASE, which a bound node would parenthesize; the text is a call, so it stays whole."""
    decidb_cli.assert_error_at(
        _query("SUM(x) <= 7", "MAXIMIZE SUM(x) * if(s = 'F', 1, 2)"),
        kind="Binder Error",
        at="if(s = 'F', 1, 2)",
        word="'if(s = 'F', 1, 2)' varies per row",
    )


@pytest.mark.error
def test_a_parameter_is_quoted_as_written(decidb_cli):
    """`$1` is what the user wrote, not its value."""
    decidb_cli.assert_error_at(
        "PREPARE p AS " + _query("SUM(x) <= 7", "MAXIMIZE SUM(x) * $1") + ";",
        kind="Binder Error",
        at="$1",
        word="'$1' varies per row",
    )


@pytest.mark.error
def test_a_macro_call_is_quoted_as_the_call(decidb_cli):
    """A macro's body was parsed from its definition, so its locations are cleared and the expansion takes the call's:
    the message quotes `twice(b + 1)` and points at the call, never a span of this query cut at an offset of the
    macro's text."""
    decidb_cli.without_verify_serializer().assert_error_at(
        "CREATE TEMP MACRO twice(v) AS v * 2 + w;\n" + _query("SUM(x) <= 7", "MAXIMIZE SUM(x) * twice(b + 1)") + ";",
        kind="Binder Error",
        at="twice(b + 1)",
        word="'twice(b + 1)' varies per row",
    )


@pytest.mark.explain
def test_a_macro_body_is_reprinted_beside_the_arguments_written_in_the_query(decidb_cli):
    """The body's constants have no location in this query: the canonical layer prints `3 * x`, the `3` from the body
    and the `x` as written. The macro is padded so that its `3` sits at the offset of the query's `7`, where a location
    kept from the macro's text would cut `7` out of the query."""
    query = "EXPLAIN " + _query("SUM(x) <= 7 AND SUM(triple(x)) >= 1", "MAXIMIZE SUM(triple(x * w))")
    prefix = "CREATE TEMP MACRO triple(v) AS v *"
    macro = prefix + " " * (query.index("<= 7") + 3 - len(prefix)) + "3"
    assert macro.index("3") == query.index("<= 7") + 3
    result = decidb_cli.without_verify_serializer().execute_script(macro + ";\n" + query + ";\n")
    plan = "".join(result.stdout.split()).replace("│", "")
    assert "SUM(triple(x))>=1≡sum(3*x)>=1" in plan, result.stdout


@pytest.mark.error
def test_a_cut_keeps_its_whitespace_and_comments(decidb_cli):
    """A cut is the text between its two ends, verbatim (Clang's SourceRange): the comment inside stays."""
    decidb_cli.assert_error_at(
        _query("SUM(x) <= 7", "MAXIMIZE SUM(x) * (w /* kg */  + 1)"),
        kind="Binder Error",
        at="w /* kg */  + 1",
        word="'w /* kg */  + 1' varies per row",
    )


@pytest.mark.error
def test_a_column_is_quoted_with_the_relation_the_user_wrote(decidb_cli):
    decidb_cli.assert_error_at(
        _query("SUM(x) <= 7", "MAXIMIZE SUM(x) * t.w"), kind="Binder Error", at="t.w", word="'t.w' varies per row"
    )
