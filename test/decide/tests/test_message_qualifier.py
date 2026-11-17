"""A message that quotes a clause names it with its `when c per K:` qualifier.

Every site that quotes a clause goes through `QuoteWrittenClause` / `QuoteEditedClause`
(`decide_source_info.hpp`), which attach the qualifier the clause was written under. One test
per site, each failing if that site quotes the clause bare. A bare `per row:` is never printed
(D32), so it is not tested here.
"""

import csv
import io

import pytest
from solver.types import ObjSense, VarType

_DATA = "(VALUES (1, true, 'a', 5), (2, true, 'a', 5)) t(id, p, g, cap)"
#: The four shapes of `ScopeQualifier` (`decide_source_provenance.cpp`): a `when` alone, a `per` alone, both and
#: `per ()`. The qualifier is a string by the time a site quotes it (`AttachDecideQualifier`), so one site carries all
#: four shapes and each other site one.
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
@pytest.mark.parametrize("scope", ["when p per g:"])
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
    """`PhysicalDecide`: the bound a BOOL cannot hold is quoted with the `per` it was written under. The `per` is a
    relation element (`per t`) because the refusal reads the box a constant bound folds into, and a `per` over a
    written column (`per g:`) does not fold: a row whose `g` is NULL takes no instance of the clause."""
    stderr = decidb_cli.execute_raw(_select("per t: x <= -1", var="x(BOOL)")).stderr
    assert "infeasible: per t: x <= -1 cannot hold for a BOOL variable" in stderr, stderr


#: The two refusals of a bound the domain cannot hold, for a bound under each scope the clause can carry.
_NON_NEGATIVE = "cannot hold because x is non-negative (>= 0). Add an explicit lower bound if it may be negative."


@pytest.mark.error
@pytest.mark.parametrize(
    "scope, mode",
    [
        (scope, mode)
        for scope in _SCOPES
        for mode in ("off", "auto")
        # A bound under a `when` alone is answered by direct solve (A1) before the solver's check: `auto` says the
        # generic infeasible there, where the solver names the clause.
        if (scope, mode) != ("when p:", "auto")
    ],
)
def test_a_scoped_bool_bound_refusal_quotes_the_scope(decidb_cli, scope, mode):
    """`CheckDomain`: a scoped bound a BOOL cannot hold (the upper side) names its clause, whether or not the bound
    folds into the decision's box, and a direct solve and the solver print one message."""
    sql = f"SET decide_direct_solve = '{mode}'; " + _select(f"{scope} x <= -1", var="x(BOOL)")
    stderr = decidb_cli.execute_raw(sql).stderr
    assert f"infeasible: {scope} x <= -1 cannot hold for a BOOL variable." in stderr, stderr


@pytest.mark.error
def test_a_per_bound_a_bool_cannot_reach_names_its_clause(decidb_cli):
    """`CheckDomain` reads the lower end too: a floor above 1 contradicts a BOOL, as it does a folded bound."""
    stderr = decidb_cli.execute_raw(_select("per g: x >= 2", var="x(BOOL)")).stderr
    assert "infeasible: per g: x >= 2 cannot hold for a BOOL variable." in stderr, stderr


@pytest.mark.error
@pytest.mark.parametrize("mode", ["off", "auto"])
@pytest.mark.parametrize("scope", ["per g:", "when p per g:"])
def test_a_per_bound_a_non_negative_decision_cannot_hold_names_its_clause(decidb_cli, scope, mode):
    """`CheckDomain`: the floor of 0 and the upper bound below it, under a `per` over a written column, which does
    not fold into a box. A direct solve and the solver print one message."""
    sql = f"SET decide_direct_solve = '{mode}'; " + _select(f"{scope} x <= -1")
    stderr = decidb_cli.execute_raw(sql).stderr
    assert f"infeasible: {scope} x <= -1 {_NON_NEGATIVE}" in stderr, stderr


@pytest.mark.error
def test_a_bound_that_reads_the_data_is_not_a_domain_refusal(decidb_cli):
    """`CheckDomain` declines a bound whose ends are not constants (`per g: x <= cap`): the solve and DIAGNOSE report
    it, since the data says whether a row is left that it contradicts."""
    sql = "SELECT id, x FROM (VALUES (1, 'a', -1), (2, 'a', -1)) t(id, g, cap) DECIDE x(INT) SUCH THAT per g: x <= cap MAXIMIZE SUM(x)"
    stderr = decidb_cli.execute_raw(sql).stderr
    assert "DECIDE optimization is infeasible. Prefix the query with DIAGNOSE" in stderr, stderr


def _solves_as_the_oracle(cli, oracle, name, rows, clauses, bounds):
    """DeciDB's objective on `rows` ((id, g) pairs; `g` None is NULL) under `clauses`, and the oracle's over the
    interval `bounds[id]` each `x` takes, which the oracle states itself."""
    values = ", ".join(f"({i}, {'NULL::VARCHAR' if g is None else repr(g)})" for i, g in rows)
    sql = f"SELECT id, x FROM (VALUES {values}) t(id, g) DECIDE x(INT) SUCH THAT {clauses} MAXIMIZE SUM(x)"
    found, columns = cli.execute(sql)
    oracle.create_model(name)
    for i, _ in rows:
        low, high = bounds[i]
        oracle.add_variable(f"x_{i}", VarType.INTEGER, lb=low, ub=high)
    oracle.set_objective({f"x_{i}": 1.0 for i, _ in rows}, ObjSense.MAXIMIZE)
    expected = oracle.solve().objective_value
    assert sum(row[columns.index("x")] for row in found) == pytest.approx(expected)


@pytest.mark.correctness
def test_a_per_bound_no_row_reaches_is_not_checked(decidb_cli, oracle_solver):
    """`CheckDomain` checks an instance's row only: every `g` is NULL, so no row is in a class of `per g:` and
    `per g: x <= -1` writes none. The query solves, `x` at its other bound."""
    _solves_as_the_oracle(
        decidb_cli,
        oracle_solver,
        "per_bound_no_row_reaches",
        [(1, None), (2, None)],
        "x <= 4 AND per g: x <= -1",
        {1: (0, 4), 2: (0, 4)},
    )


@pytest.mark.correctness
def test_a_per_floor_that_lowers_the_domain_lets_the_upper_bound_below_zero_hold(decidb_cli, oracle_solver):
    """`per g: x >= -5` lowers the floor of the decisions of its classes, so `per g: x <= -3` on them holds. The row
    in no class keeps the floor of 0, and `CheckDomain` never sees it."""
    _solves_as_the_oracle(
        decidb_cli,
        oracle_solver,
        "per_floor_lowers_the_domain",
        [(1, "a"), (2, "a"), (3, None)],
        "x <= 4 AND per g: x >= -5 AND per g: x <= -3",
        {1: (-5, -3), 2: (-5, -3), 3: (0, 4)},
    )


@pytest.mark.error
def test_highs_coefficient_refusal_quotes_the_scope(decidb_cli_highs):
    """`DescribeRowClause`: HiGHS's refusal of a limit it cannot hold names the clause with its scope."""
    sql = _select("x >= 0 AND x <= 5 AND when p per g: SUM(x) >= 1e40", var="x(REAL)")
    stderr = decidb_cli_highs.execute_raw(sql, expect_number_range=True).stderr
    assert "Constraint `when p per g: SUM(x) >= 1e+40` sets a limit" in stderr, stderr
