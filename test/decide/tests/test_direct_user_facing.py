"""What a SQL user sees of direct solve besides the answer: the default mode, error wording, memory use, and policy.

The answers themselves are checked in `test_direct_three_way.py` and the per-rule contract in
`test_direct_rule_contract.py`.
"""

import re
import math
import pytest


@pytest.fixture(autouse=True)
def _allow_direct_path(monkeypatch):
    # The suite also runs with a process-wide forced HiGHS backend. This file
    # opts into the direct path; forced-backend cases use their own CLI wrapper.
    monkeypatch.delenv("DECIDB_FORCE_SOLVER", raising=False)


def _source_query(scores, capacity, sense="MAXIMIZE"):
    values = ", ".join(f"({i}, CAST('{score}' AS DOUBLE))" for i, score in enumerate(scores))
    return f"""
        SELECT id, score, x FROM (
            FROM (VALUES {values}) t(id, score)
            DECIDE x(BOOL) SUCH THAT SUM(x) <= {capacity}
            {sense} SUM(score * x)
        ) q ORDER BY id
    """


def _run(cli, sql, mode="require"):
    return cli.execute(f"SET decide_direct_solve='{mode}'; {sql}")


def _raw(cli, sql, mode="require"):
    return cli.execute_raw(f"SET decide_direct_solve='{mode}'; {sql}")


def _has_decide_operator(plan):
    # The operator's box title. A plain substring test would also match the DECIDE: prefix of the error text that
    # a direct plan carries.
    return re.search(r"│\s+DECIDE(_DIAGNOSE)?\s+│", plan) is not None


@pytest.mark.correctness
def test_s1_direct_reads_bounds_exactly_near_an_integer(decidb_cli):
    # Decided 2026-10-05 (S1-09): direct solve reads a bound exactly and does not copy the solver's feasibility
    # tolerance, which differs between backends. This pins that choice so a change is deliberate. The solver would
    # select 2 rows here (it accepts 1.9999999 as 2); direct selects 1.
    sql = """
        SELECT id, x FROM (
            FROM (VALUES (1, 9.0), (2, 5.0), (3, 8.0)) t(id, score)
            DECIDE x(BOOL) SUCH THAT SUM(x) <= 1.9999999 MAXIMIZE SUM(score * x)
        ) q ORDER BY id
    """
    assert [row[1] for row in _run(decidb_cli, sql)[0]] == [1, 0, 0]


@pytest.mark.correctness
def test_direct_solve_is_on_by_default(decidb_cli):
    # No SET anywhere in this test: it exercises what a user gets out of the box, so it opts out of the
    # suite-wide DECIDB_TEST_DIRECT_SOLVE override.
    decidb_cli = decidb_cli.with_env({"DECIDB_TEST_DIRECT_SOLVE": ""})
    default = decidb_cli.execute_raw("SELECT current_setting('decide_direct_solve')")
    assert "auto" in default.stdout, default.stderr

    s1 = _source_query((2.0, 9.0, -1.0), 1)
    plan = decidb_cli.execute_raw(f"EXPLAIN {s1}").stdout
    assert "Direct solve rule" in plan and "S1_CARDINALITY_INTERVAL" in plan and "WINDOW" in plan and not _has_decide_operator(plan)
    rows, _ = decidb_cli.execute(s1)
    assert sum(row[2] for row in rows) == 1 and sum(row[1] * row[2] for row in rows) == 9.0

    # A shape direct solve does not prove keeps the solver plan and prints nothing about direct solve.
    weighted = """
        SELECT id, x FROM (
            FROM (VALUES (1, 2.0, 3), (2, 9.0, 2)) t(id, score, weight)
            DECIDE x(BOOL) SUCH THAT SUM(weight * x) <= 3
            MAXIMIZE SUM(score * x)
        ) q
    """
    plan = decidb_cli.execute_raw(f"EXPLAIN {weighted}").stdout
    assert _has_decide_operator(plan) and "Direct solve" not in plan
    rows, _ = decidb_cli.execute(weighted)
    assert sorted(rows) == [(1, 0), (2, 1)]

    # `off` still reaches the solver for a shape direct solve would take.
    off = _raw(decidb_cli, f"EXPLAIN {s1}", mode="off").stdout
    assert _has_decide_operator(off) and "Direct solve" not in off


@pytest.mark.correctness
@pytest.mark.parametrize("score", ["'NaN'::DOUBLE", "'Infinity'::DOUBLE"])
def test_invalid_score_error_matches_solver_wording(decidb_cli, score):
    # This message is a fixed string, so direct solve keeps the solver's. The solver also reports the row number; the
    # direct plan has no row to report.
    sql = f"""
        SELECT id, x FROM (
            FROM (VALUES (1, 2.0), (2, {score})) t(id, score)
            DECIDE x(BOOL) SUCH THAT SUM(x) <= 1 MAXIMIZE SUM(score * x)
        ) q
    """
    solver = _raw(decidb_cli, sql, mode="off").stderr
    direct = _raw(decidb_cli, sql, mode="require").stderr
    assert "Error" in direct, direct
    assert re.sub(r" at row \d+", "", solver) == direct


@pytest.mark.correctness
def test_null_score_error_quotes_the_score(decidb_cli):
    # A NULL score is reported with the score as the user wrote it. The solver path says the same for this score.
    sql = """
        SELECT id, x FROM (
            FROM (VALUES (1, '2.0'), (2, 'bad')) t(id, s)
            DECIDE x(BOOL) SUCH THAT SUM(x) <= 1 MAXIMIZE SUM(TRY_CAST(s AS DOUBLE) * x)
        ) q
    """
    solver = _raw(decidb_cli, sql, mode="off").stderr
    direct = _raw(decidb_cli, sql, mode="require").stderr
    assert "TRY_CAST(s AS DOUBLE) is NULL. Impute it with COALESCE()" in direct, direct
    assert solver == direct


@pytest.mark.correctness
def test_s1_null_source_bound_error_names_the_column(decidb_cli):
    """A NULL or NaN bound is reported with the column, then COALESCE or WHERE. The wording is direct solve's own."""

    def error(cap_type, cap, bound="cap"):
        sql = f"""
            SELECT x FROM (
                FROM (VALUES (1,{cap}::{cap_type},9.0)) t(id,cap,score)
                DECIDE x(BOOL) SUCH THAT SUM(x)<={bound} MAXIMIZE SUM(score*x)
            ) q
        """
        return _raw(decidb_cli, sql, mode="require").stderr

    for message in (error("INTEGER", "NULL"), error("DOUBLE", "NULL"), error("DOUBLE", "'NaN'")):
        assert 'column "cap"' in message and "COALESCE(cap, 0)" in message and "WHERE" in message, message
    computed = error("VARCHAR", "'bad'", bound="TRY_CAST(cap AS INTEGER)")
    assert "the bound expression" in computed and "COALESCE()" in computed, computed


@pytest.mark.correctness
def test_unused_wide_output_is_pruned_without_losing_rank_or_bindings(decidb_cli):
    setup = """
        CREATE TEMP TABLE pruning_source AS
        SELECT i, (i % 101)::DOUBLE AS score, repeat('p', 200) AS payload
        FROM range(5000) t(i);
    """
    decide = """
        FROM pruning_source DECIDE x(BOOL)
        SUCH THAT SUM(x)<=500 MAXIMIZE SUM(score*x)
    """
    aggregate = f"SELECT COUNT(*), SUM(x), SUM(score*x) FROM ({decide}) q"
    plan = _raw(decidb_cli, setup + f"EXPLAIN {aggregate}").stdout
    scan = plan.rsplit("SEQ_SCAN", 1)[-1]
    assert "Direct solve rule" in plan and "WINDOW" in plan
    assert "score" in scan and "payload" not in scan

    aliased = """
        SELECT COUNT(*), SUM(x) FROM (
            FROM (SELECT i, score AS profit, payload FROM pruning_source WHERE i>=0) s
            DECIDE x(BOOL) SUCH THAT SUM(x)<=500 MAXIMIZE SUM(profit*x)
        ) q
    """
    alias_plan = _raw(decidb_cli, setup + f"EXPLAIN {aliased}").stdout
    assert "payload" not in alias_plan.rsplit("SEQ_SCAN", 1)[-1]

    rows, _ = _run(decidb_cli.with_verify_serializer(), setup + aggregate)
    expected = sum(sorted((i % 101 for i in range(5000)), reverse=True)[:500])
    assert rows == [(5000, 500, float(expected))]

    full, _ = _run(decidb_cli, setup + f"SELECT i, score, payload, x FROM ({decide}) q WHERE i=0")
    assert full == [(0, 0.0, "p" * 200, 0)]

    invalid = setup.replace("(i % 101)::DOUBLE AS score", "CASE WHEN i=4999 THEN NULL ELSE 1.0 END AS score")
    count_only = f"SELECT COUNT(*) FROM ({decide.replace('<=500', '<=0')}) q"
    assert "is NULL" in _raw(decidb_cli, invalid + count_only).stderr

    throwing_source = """
        SELECT i, i::DOUBLE AS score,
               CASE WHEN i=4999 THEN error('payload boom') ELSE 'ok' END AS payload
        FROM range(5000) t(i)
    """
    throwing = f"""
        SELECT COUNT(*) FROM (
            FROM ({throwing_source}) s DECIDE x(BOOL)
            SUCH THAT SUM(x)<=1 MAXIMIZE SUM(score*x)
        ) q
    """
    for mode in ("require", "off"):
        assert "payload boom" in _raw(decidb_cli, throwing, mode=mode).stderr


@pytest.mark.correctness
def test_inner_join_passthrough_prunes_stored_payload_but_keeps_computed_errors(decidb_cli):
    setup = """
        CREATE TEMP TABLE join_source AS
        SELECT i, (i % 101)::DOUBLE AS score, repeat('p', 200) AS payload
        FROM range(5000) t(i);
        CREATE TEMP TABLE join_weights AS
        SELECT k, 1.0 + (k % 3)::DOUBLE / 10.0 AS weight FROM range(7) t(k);
    """
    relation = """
        (SELECT s.i, s.score * w.weight AS score, s.payload
         FROM join_source s JOIN join_weights w ON s.i % 7 = w.k) j
    """
    decide = f"""
        FROM {relation} DECIDE x(BOOL)
        SUCH THAT SUM(x)<=500 MAXIMIZE SUM(score*x)
    """
    aggregate = f"SELECT COUNT(*), SUM(x), SUM(score*x) FROM ({decide}) q"
    plan = _raw(decidb_cli, setup + f"EXPLAIN {aggregate}").stdout
    assert "Direct solve rule" in plan and "HASH_JOIN" in plan and "WINDOW" in plan
    assert "payload" not in plan

    rows, _ = _run(decidb_cli.with_verify_serializer(), setup + aggregate)
    scores = [(i % 101) * (1.0 + (i % 7 % 3) / 10.0) for i in range(5000)]
    assert rows[0][:2] == (5000, 500)
    assert math.isclose(rows[0][2], sum(sorted(scores, reverse=True)[:500]), rel_tol=0, abs_tol=1e-7)

    full, _ = _run(decidb_cli, setup + f"SELECT i, score, payload, x FROM ({decide}) q WHERE i=0")
    assert full == [(0, 0.0, "p" * 200, 0)]

    throwing_relation = relation.replace("s.payload", "CASE WHEN s.i=4999 THEN error('joined payload boom') "
                                                  "ELSE s.payload END AS payload")
    throwing = f"""
        SELECT COUNT(*) FROM (
            FROM {throwing_relation} DECIDE x(BOOL)
            SUCH THAT SUM(x)<=1 MAXIMIZE SUM(score*x)
        ) q
    """
    for mode in ("require", "off"):
        assert "joined payload boom" in _raw(decidb_cli, setup + throwing, mode=mode).stderr


@pytest.mark.correctness
def test_invalid_forced_solver_and_disabled_optimizer_policy(decidb_cli):
    # DIAGNOSE and a valid forced backend are part of every rule's contract (test_direct_rule_contract.py).
    sql = _source_query((2.0, 9.0), 1)
    invalid = decidb_cli.with_env({"DECIDB_FORCE_SOLVER": "unknown_backend"})
    error = _raw(invalid, sql, mode="auto").stderr
    assert "DECIDB_FORCE_SOLVER=unknown_backend" in error

    disabled = _raw(decidb_cli, f"SET disabled_optimizers='decide_optimizer'; {sql}").stderr
    assert "DECIDE optimizer did not run" in disabled


@pytest.mark.correctness
def test_extension_registration_preserves_binder_errors(decidb_cli):
    sql = """
        SELECT x FROM (
            FROM (VALUES (1,2),(2,9)) n(id,score)
            DECIDE x(BOOL) SUCH THAT SUM(n: x)<=1 MAXIMIZE SUM(score*x)
        ) q
    """
    for mode in ("off", "require"):
        result = _raw(decidb_cli, sql, mode=mode)
        assert "Binder Error" in result.stderr
        assert "not a decision of n" in result.stderr
