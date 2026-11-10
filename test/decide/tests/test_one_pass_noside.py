"""The executable graph owns data reducers and generated presence dependencies."""

import re

import pytest

from ._anr_cases import F3_AVG_LIMIT, F15, FRAME_EXAMPLE_3, FRAME_NULL_REDUCED
from ._scope_cases import answer


def native(cli):
    return cli.without_verify_serializer()


def test_data_reducers_have_no_aggregate_or_check_source_plan(decidb_cli):
    """The bound and its validity check used to require independent CTE reads."""
    query = """SELECT x FROM (VALUES (3),(6)) t(cap)
        DECIDE x(BOOL) SUCH THAT sum(x)<=sum(cap)/3 MAXIMIZE sum(x)"""
    cli = native(decidb_cli)
    plan = cli.execute_raw("SET decide_direct_solve='off'; EXPLAIN " + query)
    assert plan.returncode == 0, plan.stderr
    assert "DECIDE" in plan.stdout, plan.stdout
    assert "CTE_SCAN" not in plan.stdout, plan.stdout
    assert "AGGREGATE" not in plan.stdout, plan.stdout
    assert "CROSS_PRODUCT" not in plan.stdout, plan.stdout
    assert plan.stdout.count("COLUMN_DATA_SCAN") == 1, plan.stdout
    assert answer(cli, "SET decide_direct_solve='off'; " + query) == [
        {"x": 1},
        {"x": 1},
    ]


def test_an_outer_reducers_spelling_names_its_inner_reducers(decidb_cli):
    """The plan is the tree's, so a read of a data reducer shows the reducer as written. The outer reducer here, the
    group's limit, reads avg(P.lim) by (P.lim): its spelling is rendered once that read is named, else it shows the
    inner result as an internal column."""
    plan = native(decidb_cli).execute_raw("SET decide_direct_solve='off'; EXPLAIN " + F3_AVG_LIMIT.sql)
    assert plan.returncode == 0, plan.stderr
    assert "#[" not in plan.stdout and "avg(P.lim) by (P.lim) END" in plan.stdout, plan.stdout


@pytest.mark.parametrize("case", [F15, FRAME_EXAMPLE_3, FRAME_NULL_REDUCED], ids=lambda case: case.name)
def test_generated_presence_tests_read_computed_children(decidb_cli, case):
    """A generated NULL test over a frame must read O, not execute an aggregate marker."""
    result = answer(native(decidb_cli), "SET decide_direct_solve='off'; " + case.sql)
    model = case.model()
    assert model.judge(result, case.clauses, case.objective) == pytest.approx(case.optimum)


def test_composed_objective_has_one_scope_and_one_inherited_gate(decidb_cli):
    """The objective's MIN/MAX terms share its E and inherit its `when` reach."""
    query = """SELECT id,x FROM (VALUES (1,true),(2,false)) t(id,p)
        DECIDE x(REAL) SUCH THAT x<=9
        MAXIMIZE when t.p: max(x)+min(x) ORDER BY id"""
    cli = native(decidb_cli).with_env({"DECIDB_DUMP_TREE": "1"})
    result = cli.execute_raw("SET decide_direct_solve='off'; " + query)
    assert result.returncode == 0, result.stderr
    assert "Error:" not in result.stderr, result.stderr
    tree = result.stderr.split("DECIDB_TREE_BEGIN\n", 1)[1].split("DECIDB_TREE_END", 1)[0]
    assert len(re.findall(r"(?m)^\d+ E objective ", tree)) == 1, tree
    assert sum(bool(gate) for gate in re.findall(r"when=\[([^]]*)\]", tree)) == 1, tree
