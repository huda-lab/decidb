"""A message that quotes a clause names it with its `when c per K:` qualifier.

Every site that quotes a clause goes through `QuoteWrittenClause` / `QuoteEditedClause`
(`decide_source_info.hpp`), which attach the qualifier the clause was written under. One test
per site, each failing if that site quotes the clause bare. A bare `per row:` is never printed
(D32), so it is not tested here.
"""

import csv
import io

import pytest

_DATA = "(VALUES (1, true, 'a', 5), (2, true, 'a', 5)) t(id, p, g, cap)"
_SCOPES = ["when p:", "per g:", "when p per g:", "per ():"]


def _select(clauses, objective="MAXIMIZE SUM(x)", var="x(INT)"):
    return f"SELECT id, x FROM {_DATA} DECIDE {var} SUCH THAT {clauses} {objective}"


def _diagnosis(cli, sql):
    """The findings of `DIAGNOSE <sql>`, keyed by the clause each names."""
    result = cli.execute_script(".mode csv\n" + f"DIAGNOSE {sql};\n")
    return {row["clause"]: row for row in csv.DictReader(io.StringIO(result.stdout))}, result.stdout + result.stderr


@pytest.mark.error
@pytest.mark.min_max
@pytest.mark.parametrize("scope", _SCOPES)
def test_minmax_not_equal_refusal_quotes_the_scope(decidb_cli, scope):
    """`decide_rewrite_minmax.cpp`: the `<>` refusal over a MIN/MAX names the clause with its scope."""
    stderr = decidb_cli.execute_raw(_select(f"x <= 3 AND {scope} max(x) <> 2")).stderr
    assert f"MAX of absolute values): {scope} max(x) <> 2" in stderr, stderr


@pytest.mark.error
@pytest.mark.parametrize("scope", _SCOPES)
def test_product_not_equal_refusal_quotes_the_scope(decidb_cli, scope):
    """`decide_rewrite_notequal_avg.cpp`: the `<>` refusal over a product of decisions names its scope."""
    stderr = decidb_cli.execute_raw(_select(f"x <= 3 AND {scope} x * x <> 2")).stderr
    assert f"is not supported yet: {scope} x * x <> 2" in stderr, stderr


@pytest.mark.error
def test_drop_label_quotes_the_scope(decidb_cli):
    """`MakeDropEdit`: the clause offered for removal keeps its `when` and `per`."""
    sql = f"""SELECT id, x FROM (VALUES (1, true, 'a'), (2, true, 'a')) t(id, p, g)
        DECIDE x(BOOL) SUCH THAT when p per g: sum(x) <> 1 AND when p per g: sum(x) <> 0 AND
        when p per g: sum(x) <> 2 MINIMIZE SUM(x)"""
    findings, text = _diagnosis(decidb_cli, sql)
    assert findings["when p per g: sum(x) <> 0"]["edit_source"] == "remove_only", text


@pytest.mark.error
def test_loosen_label_and_suggestion_quote_the_scope(decidb_cli):
    """`MakeLoosenEdit`: both the clause and the suggested change carry the scope."""
    findings, text = _diagnosis(decidb_cli, _select("when p per g: sum(x) >= 5", var="x(BOOL)"))
    assert findings["when p per g: sum(x) >= 5"]["suggested_change"] == "when p per g: sum(x) >= 2", text


@pytest.mark.error
def test_virtual_offset_label_and_suggestion_quote_the_scope(decidb_cli):
    """`MakeVirtualOffsetEdit`: a data-backed bound's offset keeps the scope, in the label and the change."""
    sql = f"SELECT id, x FROM {_DATA} DECIDE x(BOOL) SUCH THAT when p: x >= cap MAXIMIZE SUM(x)"
    findings, text = _diagnosis(decidb_cli, sql)
    assert findings["when p: x >= cap"]["suggested_change"] == "when p: x >= cap - 4", text


@pytest.mark.error
def test_unreachable_bound_label_quotes_the_scope(decidb_cli):
    """`CollectUnreachableClauses`: the clause named as out of reach keeps its scope."""
    sql = """WITH data AS (SELECT 0 AS g, -1e1000::DOUBLE AS cap, true AS p UNION ALL
        SELECT 0, -1e1000::DOUBLE, true UNION ALL SELECT 1, 3.0, true UNION ALL SELECT 1, 1.0, true)
        SELECT g, x FROM data DECIDE x(INT)
        SUCH THAT x >= 0 AND x <= 6 AND when p per g: min(x) by (g) <= max(cap) by (g) MAXIMIZE SUM(x)"""
    findings, text = _diagnosis(decidb_cli, sql)
    assert findings["when p per g: min(x) by (g) <= -inf"]["edit_source"] == "unreachable_bound", text


@pytest.mark.error
def test_bool_bound_refusal_quotes_the_scope(decidb_cli):
    """`PhysicalDecide`: the bound a BOOL cannot hold is quoted with the `per` it was written under."""
    stderr = decidb_cli.execute_raw(_select("per g: x <= -1", var="x(BOOL)")).stderr
    assert "infeasible: per g: x <= -1 cannot hold for a BOOL variable" in stderr, stderr


@pytest.mark.error
def test_highs_coefficient_refusal_quotes_the_scope(decidb_cli_highs):
    """`DescribeRowClause`: HiGHS's refusal of a limit it cannot hold names the clause with its scope."""
    sql = _select("x >= 0 AND x <= 5 AND when p per g: SUM(x) >= 1e40", var="x(REAL)")
    stderr = decidb_cli_highs.execute_raw(sql, expect_large_numbers=True).stderr
    assert "Constraint `when p per g: sum(x) >= 1e+40` sets a limit" in stderr, stderr
