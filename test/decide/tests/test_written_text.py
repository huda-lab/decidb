"""A clause's written text is cut from the query, never re-rendered.

The parser keeps the span of a `when` condition, a `per` or `by` key and both sides of a comparison, and the
transformer cuts that text out of the query string. EXPLAIN and the messages echo it exactly as typed: case, spacing,
casts, subqueries, column spellings. Text DeciDB writes itself (the canonical layer, edited clauses) keeps its own
rendering.
"""

import re

import pytest

_DATA = "(VALUES (1, 'a', 5, 7), (2, 'b', 6, 7)) t(id, g, cap, v)"
_OFF = "SET decide_direct_solve='off'; "


def _plan(cli, clause, objective="MAXIMIZE sum(x)", extra=""):
    """The plan of `SELECT ... DECIDE x(INT) SUCH THAT x >= 0 AND <clause>`, whitespace removed (the box wraps tokens)."""
    sql = f"{_OFF}EXPLAIN SELECT id, x FROM {_DATA} DECIDE x(INT) SUCH THAT x >= 0 AND {clause} {objective}{extra}"
    result = cli.execute_raw(sql)
    assert not result.stderr.strip(), result.stderr
    return re.sub(r"[│\s]", "", result.stdout)


def _packed(text):
    return re.sub(r"\s", "", text)


@pytest.mark.explain
@pytest.mark.parametrize(
    "clause",
    [
        pytest.param("sum(x) by (id % 2) <= 10", id="by-expression"),
        pytest.param("when v in (SELECT 1000): sum(x) <= 10", id="when-in-subquery"),
        pytest.param("when exists (SELECT 1 WHERE false): sum(x) <= 10", id="when-exists"),
        pytest.param("when id > 100 and g is not null: sum(x) <= 10", id="when-and-is-not-null"),
        pytest.param(
            "when g in (SELECT region FROM (VALUES ('East', 30)) c(region, cap)): sum(x) <= 10",
            id="when-values-subquery",
        ),
        pytest.param("sum(x * cap::INT) <= 10", id="cast-in-a-side"),
        pytest.param("per g: sum(x) by (g) <= 10", id="per-and-by"),
    ],
)
def test_a_clause_echoes_exactly_as_typed(decidb_cli, clause):
    """Each example of the old re-rendering (`by ((id % 2))`, `(v = ANY(SELECT 1000))`, `CAST('f' AS BOOLEAN)`,
    `(g IS NOT NULL)`, `valueslist`) now reads as written."""
    assert _packed(clause) in _plan(decidb_cli, clause)


@pytest.mark.explain
@pytest.mark.parametrize("clause", ["SUM(x) <= 10", "Sum(X) <= 10", "when ID > 1 AND G IS NOT NULL: SUM(x) <= 10"])
def test_mixed_case_echoes_as_typed(decidb_cli, clause):
    """D32's lower-casing no longer applies to quoted text: the case is the user's."""
    plan = _plan(decidb_cli, clause)
    assert _packed(clause) in plan
    # nothing is shown under it as a canonical reading when canonicalization moved nothing
    assert "≡" not in plan


@pytest.mark.explain
def test_a_comparison_that_canonicalization_moved_keeps_its_canonical_layer(decidb_cli):
    plan = _plan(decidb_cli, "10 >= SUM(x)")
    assert "10>=SUM(x)" in plan and "≡sum(x)<=10" in plan, plan


@pytest.mark.explain
def test_a_view_echoes_as_typed(decidb_cli):
    """A stored view re-parses its SQL, so its clauses have spans too."""
    result = decidb_cli.execute_raw(
        f"{_OFF}CREATE TEMP VIEW written_text_view AS SELECT id, x FROM {_DATA} "
        "DECIDE x(INT) SUCH THAT x >= 0 AND when ID > 1: SUM(x) <= 10 MAXIMIZE sum(x); "
        "EXPLAIN SELECT * FROM written_text_view"
    )
    assert "whenID>1:SUM(x)<=10" in re.sub(r"[│\s]", "", result.stdout), result.stdout + result.stderr


@pytest.mark.explain
def test_a_prepared_statement_with_a_placeholder_echoes_as_typed(decidb_cli):
    """A `?` in the select list sits next to the DECIDE tail; under `PRAGMA verify_serializer` the statement is
    re-parsed from its ToString, which prints the tail as written, so the echo is the same in both modes."""
    result = decidb_cli.execute_raw(
        f"{_OFF}PREPARE q AS SELECT ?::INT AS p, id, x FROM {_DATA} DECIDE x(INT) "
        "SUCH THAT SUM(x) <= 10 AND when ID > 1: Sum(x) >= 0 MAXIMIZE sum(x); EXPLAIN EXECUTE q(1)"
    )
    plan = re.sub(r"[│\s]", "", result.stdout)
    assert "SUM(x)<=10" in plan and "whenID>1:Sum(x)>=0" in plan, result.stdout + result.stderr


@pytest.mark.explain
def test_a_macro_argument_in_a_decide_tail_does_not_borrow_the_bodys_text(decidb_cli):
    """A macro's body was cut from the macro's definition and an argument from the call, so their locations mean
    different text. The call is padded so the argument's operator sits at the offset of the body's `>=`: without the
    rule that a tail an argument was substituted into loses its source text, the echo is the body's `x <= 0`."""
    create = (
        "CREATE TEMP MACRO m(c) AS TABLE SELECT id, x FROM (VALUES (1, 5), (2, 6)) t(id, cap) "
        "DECIDE x(INT) SUCH THAT x >= 0 AND c MAXIMIZE sum(x)"
    )
    head = "EXPLAIN SELECT * FROM m(x"
    call = head + " " * (create.index(">=") - len(head)) + "<= 3)"
    assert call.index("<=") == create.index(">=")
    plan = re.sub(r"[│\s]", "", decidb_cli.without_verify_serializer().execute_raw(f"{_OFF}{create}; {call}").stdout)
    assert "x>=0" in plan and "x<=3" in plan and "x<=0" not in plan, plan


@pytest.mark.explain
def test_a_macro_without_arguments_in_its_tail_keeps_its_text(decidb_cli):
    create = (
        "CREATE TEMP MACRO n() AS TABLE SELECT id, x FROM (VALUES (1, 5)) t(id, cap) "
        "DECIDE x(INT) SUCH THAT SUM(x) <= 3 MAXIMIZE sum(x)"
    )
    cli = decidb_cli.without_verify_serializer()  # a TEMP MACRO cannot be replayed by the serializer round trip
    plan = re.sub(r"[│\s]", "", cli.execute_raw(f"{_OFF}{create}; EXPLAIN SELECT * FROM n()").stdout)
    assert "SUM(x)<=3" in plan, plan
