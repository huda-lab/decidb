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
    WrittenConditional)."""
    stderr = _stderr(decidb_cli, _query("SUM(if(s = 'F', x, 2 * x)) <= 3"))
    assert "IF over a DECIDE variable is not supported." in stderr, stderr
    assert "CASE over" not in stderr, stderr
    assert "`SUM(if(s = 'F', x, 2 * x)) <= 3`" in stderr, stderr


@pytest.mark.error
def test_a_case_is_still_refused_as_a_case(decidb_cli):
    """The branch of WrittenConditional that keeps the word `CASE` when the text starts with it."""
    stderr = _stderr(decidb_cli, _query("SUM(CASE WHEN s = 'F' THEN x ELSE 2 * x END) <= 3"))
    assert "CASE over a DECIDE variable is not supported." in stderr, stderr


# ---------------------------------------------------------------------------
# A key or a term the engine rebuilt
# ---------------------------------------------------------------------------


@pytest.mark.error
def test_a_by_key_that_is_a_case_is_quoted_as_written(decidb_cli):
    """`by (CASE WHEN ...)` is the user's CASE, not the parser's expansion of it, in the determination message."""
    stderr = _stderr(decidb_cli, _query("per g: SUM(x) by (CASE WHEN b > 1 THEN 1 ELSE 2 END) <= 3"))
    assert "(in by (CASE WHEN b > 1 THEN 1 ELSE 2 END))" in stderr, stderr
    assert "does not determine CASE WHEN b > 1 THEN 1 ELSE 2 END" in stderr, stderr


@pytest.mark.error
def test_a_product_term_is_named_by_its_operator_over_the_written_factors(decidb_cli):
    """`SUM(x) * (w * x)` is refused naming the term `w * SUM(x)`: canonicalization built the product, so it prints
    its operator over its factors, each as written; the casts around `w` (a DECIMAL) do not appear."""
    stderr = _stderr(decidb_cli, _query("SUM(x) * (w * x) <= 7"))
    assert "'x' is a decision, so it cannot multiply w * SUM(x)." in stderr, stderr
    assert "CAST" not in stderr and '"*"' not in stderr, stderr


@pytest.mark.error
def test_a_case_factor_is_quoted_as_written(decidb_cli):
    stderr = _stderr(decidb_cli, _query("SUM(x) <= 7", "MAXIMIZE SUM(x) * CASE WHEN s = 'F' THEN 1 ELSE 2 END"))
    assert "'CASE WHEN s = 'F' THEN 1 ELSE 2 END' varies" in stderr, stderr
    assert "e.g. SUM(x * (CASE WHEN s = 'F' THEN 1 ELSE 2 END))." in stderr, stderr


@pytest.mark.error
def test_a_sum_in_a_factor_is_quoted_as_written_and_parenthesized_in_the_suggestion(decidb_cli):
    """`(w + 1) * SUM(x)`: the factor is `w + 1`, as written, and the suggestion keeps it whole under the `*`."""
    stderr = _stderr(decidb_cli, _query("SUM(x) <= 7", "MAXIMIZE (w + 1) * SUM(x)"))
    assert "'w + 1' varies per row, so it cannot multiply SUM(x)." in stderr, stderr
    assert "e.g. SUM(x * (w + 1))." in stderr, stderr
    assert "DECIMAL" not in stderr, stderr


# ---------------------------------------------------------------------------
# What a cut keeps
# ---------------------------------------------------------------------------


@pytest.mark.error
def test_a_call_that_binds_as_a_case_takes_no_parentheses_under_a_product(decidb_cli):
    """`if(...)` binds as a CASE, which a bound node would parenthesize; the text is a call, so it stays whole."""
    stderr = _stderr(decidb_cli, _query("SUM(x) <= 7", "MAXIMIZE SUM(x) * if(s = 'F', 1, 2)"))
    assert "e.g. SUM(x * if(s = 'F', 1, 2))." in stderr, stderr


@pytest.mark.error
def test_a_parameter_is_quoted_as_written(decidb_cli):
    """`$1` is what the user wrote, not its value."""
    result = decidb_cli.execute_script("PREPARE p AS " + _query("SUM(x) <= 7", "MAXIMIZE SUM(x) * $1") + ";\n")
    assert "'$1' varies per row, so it cannot multiply SUM(x)." in result.stderr, result.stderr


@pytest.mark.error
def test_a_macro_call_is_quoted_as_the_call(decidb_cli):
    """A macro's body was parsed from its definition, so its locations are cleared and the expansion takes the call's:
    the message quotes `twice(b + 1)`, never a span of this query cut at an offset of the macro's text."""
    result = decidb_cli.without_verify_serializer().execute_script(
        "CREATE TEMP MACRO twice(v) AS v * 2 + w;\n" + _query("SUM(x) <= 7", "MAXIMIZE SUM(x) * twice(b + 1)") + ";\n"
    )
    assert "'twice(b + 1)' varies per row, so it cannot multiply SUM(x)." in result.stderr, result.stderr
    # The call is whole in the text, though its expansion is a `+`: the suggestion takes no parentheses.
    assert "e.g. SUM(x * twice(b + 1))." in result.stderr, result.stderr


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
    stderr = _stderr(decidb_cli, _query("SUM(x) <= 7", "MAXIMIZE SUM(x) * (w /* kg */  + 1)"))
    assert "'w /* kg */  + 1' varies per row" in stderr, stderr


@pytest.mark.error
def test_a_column_is_quoted_with_the_relation_the_user_wrote(decidb_cli):
    stderr = _stderr(decidb_cli, _query("SUM(x) <= 7", "MAXIMIZE SUM(x) * t.w"))
    assert "'t.w' varies per row" in stderr, stderr
