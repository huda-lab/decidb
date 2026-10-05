"""The contract every direct-solve rule owes the user, checked for every registered rule.

Each rule supplies a `RuleFixture` (`_direct_rule_fixtures.py`). Its own mathematics is
tested in its own file; this suite checks what must hold whichever rule proved the
query: the DECIDE schema and rows, the all-rows read, prepared plans, serialization,
EXPLAIN, `require` reasons, and the solver path under `off`, a forced backend and
`DIAGNOSE`.
"""

import re

import pytest

from ._direct_rule_fixtures import RULE_FIXTURES, late_error_cases, near_miss_cases


@pytest.fixture(autouse=True)
def _allow_direct_path(monkeypatch):
    # The suite also runs with a process-wide forced HiGHS backend. These tests opt
    # into the direct path; forced-backend cases use their own CLI wrapper.
    monkeypatch.delenv("DECIDB_FORCE_SOLVER", raising=False)


def _run(cli, sql, mode="require"):
    return cli.execute(f"SET decide_direct_solve='{mode}'; {sql}")


def _raw(cli, sql, mode="require"):
    return cli.execute_raw(f"SET decide_direct_solve='{mode}'; {sql}")


def _has_decide_operator(plan):
    # The operator's box title. A plain substring test would also match the DECIDE: prefix of the error text that
    # a direct plan carries.
    return re.search(r"│\s+DECIDE(_DIAGNOSE)?\s+│", plan) is not None


_RULES = pytest.mark.parametrize("fixture", RULE_FIXTURES, ids=lambda fixture: fixture.rule)


def _case_ids(cases):
    # Numbered within each rule, so a failure names the rule and the case.
    counts = {}
    ids = []
    for fixture, _ in cases:
        counts[fixture.rule] = counts.get(fixture.rule, -1) + 1
        ids.append(f"{fixture.rule}-{counts[fixture.rule]:02d}")
    return ids


_NEAR_MISSES = near_miss_cases()
_LATE_ERRORS = late_error_cases()


@pytest.mark.correctness
@_RULES
def test_hit_keeps_the_decide_schema_and_rows(decidb_cli, fixture):
    direct_rows, direct_columns = _run(decidb_cli, fixture.hit)
    solver_rows, solver_columns = _run(decidb_cli, fixture.hit, mode="off")
    assert direct_columns == solver_columns
    assert direct_rows == solver_rows
    plan = _raw(decidb_cli, f"EXPLAIN {fixture.hit}").stdout
    assert f"{fixture.rule}" in plan and not _has_decide_operator(plan)


@pytest.mark.correctness
@_RULES
def test_unused_outputs_keep_the_row_count(decidb_cli, fixture):
    count = f"SELECT COUNT(*) FROM ({fixture.hit}) q"
    assert _run(decidb_cli, count)[0] == _run(decidb_cli, count, mode="off")[0]


@pytest.mark.correctness
@pytest.mark.parametrize("fixture,case", _LATE_ERRORS, ids=_case_ids(_LATE_ERRORS))
def test_every_input_row_is_read_before_any_row_is_released(decidb_cli, fixture, case):
    # The solver reads every row before it returns, so a bad value on the last row raises however little of the
    # result a parent reads. Only LIMIT 0, which reads nothing, may skip it.
    decide, message = case
    for outer in (
        f"SELECT {fixture.late_column} FROM ({decide}) q LIMIT 1",
        f"SELECT COUNT(*) FROM ({decide}) q",
        f"SELECT {fixture.late_column} FROM ({decide}) q WHERE {fixture.late_column}=0",
    ):
        assert message in _raw(decidb_cli, outer).stderr, outer
    rows, _ = _run(decidb_cli, f"SELECT {fixture.late_column} FROM ({decide}) q LIMIT 0")
    assert rows == []


@pytest.mark.correctness
@_RULES
def test_serializer_explain_and_model_dump(decidb_cli, fixture, tmp_path):
    rows, _ = _run(decidb_cli.with_verify_serializer(), fixture.hit)
    assert rows == _run(decidb_cli, fixture.hit, mode="off")[0]
    logical = _raw(decidb_cli, f"PRAGMA explain_output='optimized_only'; EXPLAIN {fixture.hit}").stdout
    physical = _raw(decidb_cli, f"EXPLAIN {fixture.hit}").stdout
    profile = _raw(decidb_cli, f"EXPLAIN ANALYZE {fixture.hit}").stdout
    for plan in (logical, physical, profile):
        assert "Direct solve rule" in plan and "Direct solve proof" in plan

    # A direct plan builds no solver model, so there is nothing to dump.
    dump_path = tmp_path / "direct_model.txt"
    _run(decidb_cli.with_env({"DECIDB_DUMP_MODEL": str(dump_path)}), fixture.hit)
    assert not dump_path.exists()


@pytest.mark.correctness
@_RULES
def test_prepared_plan_keeps_its_selection_until_rebound(decidb_cli, fixture):
    prepared = _raw(decidb_cli, f"""
        PREPARE direct_contract AS {fixture.hit};
        SET decide_direct_solve='off';
        EXPLAIN EXECUTE direct_contract;
    """).stdout
    assert "Direct solve rule" in prepared and "require" in prepared

    rebound = decidb_cli.execute_raw(f"""
        {fixture.table_setup}
        SET decide_direct_solve='require';
        PREPARE direct_contract_table AS {fixture.table_hit};
        SET decide_direct_solve='off';
        {fixture.table_change}
        EXPLAIN EXECUTE direct_contract_table;
    """).stdout
    assert _has_decide_operator(rebound) and "Direct solve" not in rebound


@pytest.mark.correctness
@pytest.mark.parametrize("fixture,case", _NEAR_MISSES, ids=_case_ids(_NEAR_MISSES))
def test_near_misses_name_their_reason_and_keep_the_solver_plan(decidb_cli, fixture, case):
    declaration, constraint, objective, reason = case
    sql = (
        f"SELECT {fixture.near_miss_column} FROM ({fixture.near_miss_source} "
        f"DECIDE {declaration} SUCH THAT {constraint} {objective}) q"
    )
    required = _raw(decidb_cli, sql).stderr
    assert "decide_direct_solve=require:" in required, (sql, required)
    assert f"{fixture.rule}: " in required and reason in required, (sql, required)
    assert "solver skipped=true" in required, (sql, required)
    plan = _raw(decidb_cli, f"EXPLAIN {sql}", mode="auto")
    assert _has_decide_operator(plan.stdout) and "Direct solve" not in plan.stdout, (sql, plan.stderr)

    # A decline is invisible to the user: under `auto` the query still answers, exactly as it does under `off`. (A
    # query that calls random() has no fixed answer, so only the way it ends is compared.)
    auto, off = _raw(decidb_cli, sql, mode="auto"), _raw(decidb_cli, sql, mode="off")
    assert bool(auto.stderr.strip()) == bool(off.stderr.strip()), (sql, auto.stderr, off.stderr)
    if "random()" not in sql:
        assert (auto.stdout, auto.stderr) == (off.stdout, off.stderr), sql


@pytest.mark.correctness
@_RULES
def test_off_and_a_forced_backend_keep_the_solver_path(decidb_cli, decidb_cli_highs, fixture):
    off = _raw(decidb_cli, f"EXPLAIN {fixture.hit}", mode="off").stdout
    assert _has_decide_operator(off) and "Direct solve" not in off

    forced = _raw(decidb_cli_highs, f"EXPLAIN {fixture.hit}", mode="auto").stdout
    assert _has_decide_operator(forced) and "Direct solve" not in forced
    rows, _ = _run(decidb_cli_highs, fixture.hit, mode="auto")
    assert rows == _run(decidb_cli, fixture.hit, mode="off")[0]
    assert "conflicts with DECIDB_FORCE_SOLVER" in _raw(decidb_cli_highs, fixture.hit).stderr


@pytest.mark.correctness
@_RULES
def test_diagnose_keeps_the_solver_path(decidb_cli, fixture):
    assert "conflicts with DIAGNOSE" in _raw(decidb_cli, f"DIAGNOSE {fixture.hit}").stderr
    diagnose_plan = _raw(decidb_cli, f"EXPLAIN DIAGNOSE {fixture.hit}", mode="auto").stdout
    assert "DECIDE_DIAGNOSE" in diagnose_plan and "Direct solve" not in diagnose_plan
