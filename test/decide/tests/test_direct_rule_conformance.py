"""The conformance suite: what every direct-solve rule owes the user, checked for every registered rule.

Each rule supplies a `RuleFixture` (`_direct_rule_fixtures.py`). Its own mathematics is
tested in its own file; this suite checks what must hold whichever rule applied to
the query: the DECIDE schema and rows, the all-rows read, prepared plans, serialization,
EXPLAIN, `require` reasons, and the solver path under `off`, a forced backend and
`DIAGNOSE`.
"""

import math
import re

import pytest

from decidb_cli import DecidBCliError
from solver.types import ObjSense, SolverStatus, VarType

from ._direct_differential import error_class
from ._direct_oracle_common import _allow_direct_path  # noqa: F401  (autouse fixture)
from ._direct_rule_fixtures import (
    APPLIES_UNDER_REQUIRE,
    RULE_FIXTURES,
    S1_AGGREGATE_BOUNDS_NOW_APPLIED,
    late_error_cases,
    near_miss_cases,
)


def _run(cli, sql, mode="require"):
    return cli.execute(f"SET decide_direct_solve='{mode}'; {sql}")


def _raw(cli, sql, mode="require"):
    return cli.execute_raw(f"SET decide_direct_solve='{mode}'; {sql}")


def _has_decide_operator(plan):
    # The operator's box title. A plain substring test would also match the DECIDE: prefix of the error text that
    # a direct plan carries.
    return re.search(r"│\s+DECIDE(_DIAGNOSE)?\s+│", plan) is not None


def _outcome(cli, sql, mode):
    """("ok", rows) or ("error", class of the message). The rows may be empty."""
    try:
        return "ok", _run(cli, sql, mode)[0]
    except DecidBCliError as error:
        return "error", error_class(str(error))


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
def test_applying_rule_keeps_the_decide_schema_and_rows(decidb_cli, fixture):
    direct_rows, direct_columns = _run(decidb_cli, fixture.applies)
    solver_rows, solver_columns = _run(decidb_cli, fixture.applies, mode="off")
    assert direct_columns == solver_columns
    assert direct_rows == solver_rows
    plan = _raw(decidb_cli, f"EXPLAIN {fixture.applies}").stdout
    assert f"{fixture.rule}" in plan and not _has_decide_operator(plan)


@pytest.mark.correctness
@_RULES
def test_empty_source_gives_the_solvers_outcome(decidb_cli, fixture):
    # The solver answers an empty source, and it must also give the same outcome under every mode. The rule applies to
    # this query, so `require` is not allowed to decline it.
    solver = _outcome(decidb_cli, fixture.empty_applies, "off")
    assert solver == ("ok", []), solver
    assert _outcome(decidb_cli, fixture.empty_applies, "auto") == solver
    assert _outcome(decidb_cli, fixture.empty_applies, "require") == solver
    plan = _raw(decidb_cli, f"EXPLAIN {fixture.empty_applies}").stdout
    assert f"{fixture.rule}" in plan and not _has_decide_operator(plan)


def _impossible_bound_cases():
    return [(fixture, sql) for fixture in RULE_FIXTURES for sql in fixture.impossible_bound_cases]


_IMPOSSIBLE_BOUNDS = _impossible_bound_cases()


@pytest.mark.correctness
@pytest.mark.parametrize("fixture,sql", _IMPOSSIBLE_BOUNDS, ids=_case_ids(_IMPOSSIBLE_BOUNDS))
def test_impossible_constant_bound_gives_the_solvers_outcome(decidb_cli, fixture, sql):
    # The solver rejects a constant bound that contradicts the decision's domain before it reads any row, so it
    # raises on an empty source too. A rule may decline such a query (the solver path then answers) but must never
    # answer it with rows or with an empty result of its own.
    solver = _outcome(decidb_cli, sql, "off")
    assert solver[0] == "error", (sql, solver)
    assert _outcome(decidb_cli, sql, "auto") == solver, sql
    required = _outcome(decidb_cli, sql, "require")
    assert required in (solver, ("error", "direct_not_applied")), (sql, required, solver)


@pytest.mark.correctness
@_RULES
def test_unused_outputs_keep_the_row_count(decidb_cli, fixture):
    count = f"SELECT COUNT(*) FROM ({fixture.applies}) q"
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
    rows, _ = _run(decidb_cli.with_verify_serializer(), fixture.applies)
    assert rows == _run(decidb_cli, fixture.applies, mode="off")[0]
    logical = _raw(decidb_cli, f"PRAGMA explain_output='optimized_only'; EXPLAIN {fixture.applies}").stdout
    physical = _raw(decidb_cli, f"EXPLAIN {fixture.applies}").stdout
    profile = _raw(decidb_cli, f"EXPLAIN ANALYZE {fixture.applies}").stdout
    for plan in (logical, physical, profile):
        assert "Direct solve rule" in plan and "Direct solve condition" in plan
        # The box wraps long lines, even inside a word, so compare with the spaces removed.
        text = "".join(plan.replace("│", " ").split())
        assert fixture.rule in text and "".join(fixture.condition.split()) in text, plan
        assert "Direct solve guards" not in plan

    # A direct plan builds no solver model, so there is nothing to dump.
    dump_path = tmp_path / "direct_model.txt"
    _run(decidb_cli.with_env({"DECIDB_DUMP_MODEL": str(dump_path)}), fixture.applies)
    assert not dump_path.exists()


@pytest.mark.correctness
@_RULES
def test_prepared_plan_keeps_its_selection_until_rebound(decidb_cli, fixture):
    prepared = _raw(decidb_cli, f"""
        PREPARE direct_conformance AS {fixture.applies};
        SET decide_direct_solve='off';
        EXPLAIN EXECUTE direct_conformance;
    """).stdout
    assert "Direct solve rule" in prepared and "require" in prepared

    rebound = decidb_cli.execute_raw(f"""
        {fixture.table_setup}
        SET decide_direct_solve='require';
        PREPARE direct_conformance_table AS {fixture.table_applies};
        SET decide_direct_solve='off';
        {fixture.table_change}
        EXPLAIN EXECUTE direct_conformance_table;
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
    if reason == APPLIES_UNDER_REQUIRE:
        # A right-hand aggregate is a window column of the source (H-19), so the rule reads it and applies. The three
        # modes answer alike; `test_aggregate_bounds_that_apply_agree_with_the_oracle` checks the answer itself.
        required = _raw(decidb_cli, sql)
        assert "decide_direct_solve=require:" not in required.stderr, (sql, required.stderr)
        assert "Direct solve" not in required.stderr, (sql, required.stderr)
        plan = _raw(decidb_cli, f"EXPLAIN {sql}", mode="require")
        assert f"{fixture.rule}" in plan.stdout and not _has_decide_operator(plan.stdout), (sql, plan.stderr)
        # A direct plan may return the rows in another order than the solver path (the query has no ORDER BY).
        require, off, auto = (_outcome(decidb_cli, sql, mode) for mode in ("require", "off", "auto"))
        assert require[0] == off[0] == auto[0], (sql, require, off, auto)
        assert sorted(require[1], key=str) == sorted(off[1], key=str) == sorted(auto[1], key=str), (sql, require, off)
        return
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
    off = _raw(decidb_cli, f"EXPLAIN {fixture.applies}", mode="off").stdout
    assert _has_decide_operator(off) and "Direct solve" not in off

    forced = _raw(decidb_cli_highs, f"EXPLAIN {fixture.applies}", mode="auto").stdout
    assert _has_decide_operator(forced) and "Direct solve" not in forced
    rows, _ = _run(decidb_cli_highs, fixture.applies, mode="auto")
    assert rows == _run(decidb_cli, fixture.applies, mode="off")[0]
    assert "conflicts with DECIDB_FORCE_SOLVER" in _raw(decidb_cli_highs, fixture.applies).stderr


@pytest.mark.correctness
@_RULES
def test_diagnose_keeps_the_solver_path(decidb_cli, fixture):
    assert "conflicts with DIAGNOSE" in _raw(decidb_cli, f"DIAGNOSE {fixture.applies}").stderr
    diagnose_plan = _raw(decidb_cli, f"EXPLAIN DIAGNOSE {fixture.applies}", mode="auto").stdout
    assert "DECIDE_DIAGNOSE" in diagnose_plan and "Direct solve" not in diagnose_plan


@pytest.mark.correctness
@pytest.mark.parametrize(
    "constraint,bound", S1_AGGREGATE_BOUNDS_NOW_APPLIED, ids=[c for c, _ in S1_AGGREGATE_BOUNDS_NOW_APPLIED]
)
def test_aggregate_bounds_that_apply_agree_with_the_oracle(decidb_cli, oracle_solver, constraint, bound):
    """The bound is a window column of the source: `require` applies and `off` and `require` reach the oracle's best."""
    fixture = next(f for f in RULE_FIXTURES if f.rule == "S1_UNIFORM_MATROID")
    sql = (
        f"SELECT id, x FROM ({fixture.near_miss_source} DECIDE x(BOOL) SUCH THAT {constraint} "
        f"MAXIMIZE SUM(p*x)) q ORDER BY id"
    )
    scores = {1: 9.0, 2: 10.0}
    limit = 2 if bound == math.inf else min(math.floor(bound), 2)
    oracle_solver.create_model("direct_s1_aggregate_bound")
    for i in scores:
        oracle_solver.add_variable(f"x_{i}", VarType.BINARY)
    oracle_solver.add_constraint({f"x_{i}": 1.0 for i in scores}, "<=", float(limit))
    oracle_solver.set_objective({f"x_{i}": score for i, score in scores.items()}, ObjSense.MAXIMIZE)
    oracle = oracle_solver.solve()
    assert oracle.status == SolverStatus.OPTIMAL
    for mode in ("off", "require"):
        rows, _ = _run(decidb_cli, sql, mode)
        chosen = {row[0]: row[1] for row in rows}
        assert sum(chosen.values()) <= limit, (mode, rows)
        assert sum(scores[i] * chosen[i] for i in scores) == oracle.objective_value, (
            mode,
            rows,
            oracle.objective_value,
        )
