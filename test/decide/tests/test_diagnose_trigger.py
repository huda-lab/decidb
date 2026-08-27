"""`DIAGNOSE <query>` is the only thing that starts the diagnostics engine.

There is no automatic path and no session setting. A query that is never prefixed
never pays for a diagnostic solve: it reports its state ("DECIDE optimization is
infeasible.") and stops, with no clause named and no repair suggested — naming the
clause *is* the elastic solve, and that only happens when the user asks for it. The
prefix runs the same query and returns the diagnosis as a relation instead of raising.

These tests run under both backends and use `execute_raw` (one `-c` statement),
because every assertion here is observable from the statement itself. The relation's
contents are asserted in test_query_diagnostics_relation.
"""

import pytest


_BACKENDS = ["decidb_cli_highs", "decidb_cli_gurobi"]

# Cleanly UNBOUNDED LP on both backends: a free REAL var maximized with no upper cap.
_UNBOUNDED_SQL = (
    "SELECT id, x FROM (VALUES (1), (2)) t(id) "
    "DECIDE x(REAL) SUCH THAT x >= 0 MAXIMIZE SUM(x)"
)

# Infeasible: SUM of two booleans cannot reach 3.
_INFEASIBLE_SQL = (
    "SELECT id, x FROM (VALUES (1), (2)) t(id) "
    "DECIDE x(BOOL) SUCH THAT SUM(x) >= 3 MAXIMIZE SUM(x)"
)


def _combined(result) -> str:
    return (result.stderr + result.stdout).lower()


@pytest.mark.query_diagnostics
class TestDiagnoseIsTheOnlyTrigger:
    @pytest.mark.error
    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    @pytest.mark.parametrize(
        "sql,state",
        [(_UNBOUNDED_SQL, "unbounded"), (_INFEASIBLE_SQL, "infeasible")],
    )
    def test_a_bare_failure_reports_its_state_and_stops(
        self, request, cli_fixture, sql, state
    ):
        cli = request.getfixturevalue(cli_fixture)
        out = _combined(cli.execute_raw(sql))
        assert f"decide optimization is {state}" in out
        # No clause, no repair, no second statement to run.
        assert "suggested_change" not in out
        assert "decide_diagnostics" not in out
        # It does point at the prefix that would answer the question.
        assert "diagnose" in out

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    @pytest.mark.parametrize(
        "sql,state",
        [(_UNBOUNDED_SQL, "unbounded"), (_INFEASIBLE_SQL, "infeasible")],
    )
    def test_the_prefix_returns_the_diagnosis_instead_of_raising(
        self, request, cli_fixture, sql, state
    ):
        cli = request.getfixturevalue(cli_fixture)
        result = cli.execute_raw(f"DIAGNOSE {sql}")
        assert not result.stderr.strip(), result.stderr
        assert state in result.stdout.lower()

    @pytest.mark.error
    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_the_trigger_pragmas_are_gone(self, request, cli_fixture):
        """`diagnose_decide` and `decide_on_timeout` decided *whether* the engine ran.
        The prefix does that now, so both are unknown settings."""
        cli = request.getfixturevalue(cli_fixture)
        for pragma in ("diagnose_decide='auto'", "decide_on_timeout='error'"):
            out = _combined(cli.execute_raw(f"PRAGMA {pragma};"))
            assert "unrecognized configuration parameter" in out, pragma

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_the_tuning_pragmas_stay(self, request, cli_fixture):
        """The knobs that configure *how* the engine works are untouched."""
        cli = request.getfixturevalue(cli_fixture)
        for pragma in (
            "diagnose_decide_infeasible_slack_scope='expanded'",
            "diagnose_decide_infeasible_slack_scope='query'",
            "diagnose_decide_escape_rate=0.5",
            "diagnose_decide_categorical_ratio=0.25",
            "diagnose_decide_min_categories=10",
            "diagnose_decide_removal_bigm=1e7",
            "decide_l0_tolerance=1e-3",
        ):
            out = _combined(cli.execute_raw(f"PRAGMA {pragma}; SELECT 1 AS ok;"))
            assert "unrecognized configuration parameter" not in out, pragma
            assert "ok" in out, pragma

    @pytest.mark.error
    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_the_prefix_needs_a_decide_clause(self, request, cli_fixture):
        """DIAGNOSE reports on an optimization run. A query with none has nothing to
        report on, and says so rather than returning an empty relation."""
        cli = request.getfixturevalue(cli_fixture)
        out = _combined(cli.execute_raw("DIAGNOSE SELECT 1 AS a;"))
        assert "decide clause" in out
