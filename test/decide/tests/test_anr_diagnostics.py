"""DIAGNOSE and EXPLAIN over variants, nests and frames (``syntax_reference.md`` §5.5,
§5.6, §6.4).

A diagnosis loosens or drops what the user wrote: never a row a rewrite added to define a
helper decision (a structural row), and it names each clause, group and decision as the
query does -- no internal name in any output. Each repair is checked by solving the query
it describes, through the oracle or DeciDB itself.
"""

import csv
import io

import pytest

from ._anr_cases import F1_COLUMNS, F1_DATA, F1_MAX, F17
from ._anr_oracle import AnrModel, Col, Var, clause, ge, le
from ._diagnose_relation import rows as diagnose_rows
from ._diagnose_relation import run as run_diagnose
from ._output_helpers import assert_no_internal_leak
from ._scope_cases import scope_solver, table  # noqa: F401 -- fixture
from solver.types import SolverStatus

SHIP = Var("ship")
BOTH_SOLVERS = pytest.mark.parametrize("cli_fixture", ["decidb_cli_gurobi", "decidb_cli_highs"])

EDGE = ("id", "product", "period", "v", "w")
EDGE_ROWS = ((1, "A", 1, 4, 2), (2, "A", 2, 6, 1), (3, "A", 3, 1, 3), (4, "B", 1, 7, 2), (5, "B", 2, 3, 1),
             (6, "C", 1, 5, 2))
TL = "over (t.period within t.product)"


def _edge(clause_sql, objective="MAXIMIZE sum(x)", decide="x(INT)"):
    return f"""SELECT id, x {table('t', EDGE, EDGE_ROWS)} DECIDE {decide}
        SUCH THAT x <= 9 AND {clause_sql} {objective} ORDER BY id"""


def _findings(result, written):
    assert result.returncode == 0, result.stderr
    assert_no_internal_leak(result, "a diagnosis", written)
    return diagnose_rows(result)


def _achievable(findings) -> float:
    return float(next(row["amount"] for row in findings if row["edit_source"] == "achievable_objective"))


def _solved(cli, sql, column):
    out = cli.execute_script(".mode csv\n" + sql + ";\n")
    assert out.returncode == 0 and not out.stderr.strip(), out.stderr
    return sum(float(row[column]) for row in csv.DictReader(io.StringIO(out.stdout)))


@pytest.mark.diagnose
@BOTH_SOLVERS
def test_f17_diagnoses_a_variant_that_cannot_hold(request, scope_solver, cli_fixture):
    """F17 (§6.3): per depot, one instance per region its rows read (D64); D2's limit 0
    makes the variant `ship1 + ship2 + ship5 + ship6 <= 0` impossible beside `ship >= 1`.
    The cheapest repair loosens `ship >= 1`, and the objective it reaches is the oracle's
    over the repaired clauses."""
    model = F17.model()
    assert [model.render(i) for i in model.instances(F17.clauses[1])] == list(F17.instances[1])
    assert model.solve(scope_solver, F17.clauses, F17.objective).status == SolverStatus.INFEASIBLE
    findings = _findings(run_diagnose(request.getfixturevalue(cli_fixture), F17.sql[len("DIAGNOSE "):]), F17.sql)
    repairs = [row for row in findings if row["clause"] not in ("", "NULL")]
    assert [(row["clause"], row["suggested_change"]) for row in repairs] == [("ship >= 1", "ship >= 0")], findings
    repaired = [clause(ge(SHIP, 0)), F17.clauses[1]]
    assert _achievable(findings) == pytest.approx(model.solve(scope_solver, repaired, F17.objective).objective)


@pytest.mark.diagnose
@pytest.mark.parametrize("clause_sql, written", [
    pytest.param(f"x >= 5 AND sum(from previous to next: x) {TL} <= 7",
                 {"x >= 5", f"sum(from previous to next: x) {TL} <= 7"}, id="a-range-helper"),
    pytest.param(f"x >= 5 AND per t.product: sum(at(previous: x) {TL}) by (t.product) <= 4",
                 {"x >= 5", f"per t.product: sum(at(previous: x) {TL}) by (t.product) <= 4"}, id="a-member-helper"),
    pytest.param(f"x = t.v AND sum(abs(x - at(previous: x) {TL})) <= 1",
                 {"x = t.v", f"sum(abs(x - at(previous: x) {TL})) <= 1"}, id="an-abs-member"),
])
@pytest.mark.parametrize("scope", ["query", "expanded"])
def test_a_definition_is_never_offered_for_repair(decidb_cli, clause_sql, written, scope):
    """The rows defining a helper -- a range's window, a reducer whose members a frame
    drops, an ABS auxiliary standing for a member -- are structural: the diagnosis loosens
    what the user wrote, by its own text."""
    sql = _edge(clause_sql)
    findings = _findings(run_diagnose(decidb_cli, sql, scope=scope), sql)
    clauses = {row["clause"] for row in findings if row["clause"] not in ("", "NULL")}
    assert clauses and clauses <= written, findings


@pytest.mark.diagnose
def test_an_expanded_diagnosis_names_the_groups_a_frame_leaves(decidb_cli):
    """Per product, the helper's instance a frame leaves with no member (C, one period)
    has no row, so no group to loosen: A and B are named, each by the amount it needs."""
    sql = _edge(f"x >= 5 AND per t.product: sum(at(previous: x) {TL}) by (t.product) <= 4")
    findings = _findings(run_diagnose(decidb_cli, sql, scope="expanded"), sql)
    groups = {row["group"]: float(row["amount"]) for row in findings if row["edit_source"] == "expanded_group"}
    assert groups == {"A": 6.0, "B": 1.0}, findings


@pytest.mark.diagnose
@pytest.mark.parametrize("clause_sql, objective_sql, named", [
    pytest.param("", f"MAXIMIZE sum(at(previous: x) {TL})", set(), id="an-inlined-member-helper"),
    pytest.param("", f"MAXIMIZE sum(sum(from previous to next: x) {TL})", set(), id="an-inlined-range-helper"),
    pytest.param(f"AND x <= sum(at(previous: x) {TL}) + 1", "MAXIMIZE sum(x)", {f"sum(at(previous: x) {TL})"},
                 id="a-member-helper"),
    pytest.param(f"AND x <= sum(sum(from previous to next: x) {TL}) + 1", "MAXIMIZE sum(x)",
                 {f"sum(sum(from previous to next: x) {TL})"}, id="a-range-helper"),
])
def test_the_unbounded_remedy_names_decisions_and_reducers(decidb_cli, clause_sql, objective_sql, named):
    """A model that rises without end through a helper over a frame: the remedy bounds `x`
    where a member reads it, or the reducer as written -- never the helper by an internal
    name. A helper the objective alone reads is written into it (phase 7) and has no
    column to name; one every row reads keeps its column, the range inside it written
    into its members."""
    sql = f"""SELECT id, x {table('t', EDGE, EDGE_ROWS)} DECIDE x(INT) SUCH THAT x >= 0 {clause_sql} {objective_sql}"""
    findings = _findings(run_diagnose(decidb_cli, sql), sql)
    assert {row["state"] for row in findings} == {"unbounded"}, findings
    assert {row["clause"] for row in findings} == {"x"} | named, findings


@pytest.mark.diagnose
def test_f1_with_its_limits_lowered(decidb_cli, scope_solver):
    """F1 (deck pp. 97, 115) with every `c` below its allowance: the nested clause cannot
    hold at any `ship`, and the diagnosis loosens it to what the allowance needs (one
    relation in FROM: its columns print bare)."""
    data = tuple(row[:-1] + (5,) for row in F1_DATA)
    sql = F1_MAX.sql.replace(table("t", F1_COLUMNS, F1_DATA), table("t", F1_COLUMNS, data))
    model = AnrModel([dict(zip(F1_COLUMNS, row)) for row in data], F1_MAX.decisions)
    assert model.solve(scope_solver, F1_MAX.clauses, F1_MAX.objective).status == SolverStatus.INFEASIBLE
    findings = _findings(run_diagnose(decidb_cli, sql), sql)
    repairs = [row for row in findings if row["clause"] not in ("", "NULL")]
    assert repairs and all(row["clause"].startswith("per t.id: sum(when p per t.cid: max(ship) by (t.cid)) by (t.rid)")
                           for row in repairs), findings
    # Loosened by the amount it names, the clause is the oracle's with `c` raised by it.
    amount = float(repairs[0]["amount"])
    nested = F1_MAX.clauses[1]
    loosened = clause(le(nested.body.lhs, Col("c") + amount), per=nested.per)
    expected = model.solve(scope_solver, [F1_MAX.clauses[0], loosened], F1_MAX.objective)
    assert expected.status == SolverStatus.OPTIMAL
    assert _achievable(findings) == pytest.approx(expected.objective)


@pytest.mark.diagnose
@BOTH_SOLVERS
def test_dropping_a_clause_drops_its_helpers_alone(request, cli_fixture):
    """Four `<>` over one range's sum of three BOOLs leave it no value, so one must go. Each
    clause has its own helper; the one dropped goes with its definition, and `y`'s bound
    keeps its own: `y` cannot exceed the sum, so the objective is 0 whichever is dropped --
    where a free helper would let it reach 3. The repair is re-solved by DeciDB."""
    frame = "sum(from first to last: x) over (t.period)"
    clauses = [f"{frame} <> {k}" for k in range(4)]
    sql = f"""SELECT id, x, y FROM (VALUES (1, 1), (2, 2), (3, 3)) t(id, period)
        DECIDE x(BOOL), per (): y(INT)
        SUCH THAT y <= 10 AND {' AND '.join(clauses)} AND y <= {frame}
        MAXIMIZE y - sum(x)"""
    cli = request.getfixturevalue(cli_fixture)
    findings = _findings(run_diagnose(cli, sql), sql)
    drops = [row for row in findings if row["edit_source"] == "remove_only"]
    assert len(drops) == 1 and drops[0]["clause"] in clauses, findings
    assert _achievable(findings) == pytest.approx(0.0)
    fixed = sql.replace(f"{drops[0]['clause']} AND ", "")
    rows = list(csv.DictReader(io.StringIO(cli.execute_script(".mode csv\n" + fixed + ";\n").stdout)))
    assert sum(float(row["y"]) for row in rows[:1]) - sum(float(row["x"]) for row in rows) == pytest.approx(0.0)


@pytest.mark.explain
@pytest.mark.parametrize("clause_sql, account", [
    pytest.param(f"sum(at(previous: x) {TL}) by (t.product) <= 4", "becomesahelperdecisionpergroup",
                 id="a-reducer-whose-members-a-frame-drops"),
    pytest.param(f"sum(abs(x - at(previous: x) {TL})) <= 4", "becomesahelperdecision",
                 id="a-sum-of-abs-over-a-frame"),
])
def test_explain_names_what_each_construct_became(decidb_cli, clause_sql, account):
    result = decidb_cli.execute_raw("EXPLAIN " + _edge(clause_sql))
    assert result.returncode == 0, result.stderr
    assert account in "".join(result.stdout.split()).replace("│", ""), result.stdout
    assert_no_internal_leak(result, "EXPLAIN of " + clause_sql)


@pytest.mark.diagnose
def test_an_objective_with_no_member_is_no_diagnosis(decidb_cli):
    """An objective reducer every frame leaves with no member is an error before any
    solve: there is nothing to diagnose (syntax_reference.md §8.3)."""
    sql = f"""SELECT id, x FROM (VALUES (1, 'A', 1), (2, 'B', 1)) t(id, product, period)
        DECIDE x(INT) SUCH THAT x <= 3 MAXIMIZE sum(at(previous: x) {TL})"""
    result = run_diagnose(decidb_cli, sql)
    assert "selects no position on any member of" in result.stderr, result.stderr
    assert_no_internal_leak(result, "an objective with no member", sql)
