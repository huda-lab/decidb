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


# A failing solve nested inside another DECIDE clause, and two failing solves side
# by side. Both put more than one DECIDE operator in the plan.
_NESTED_SQL = (
    "SELECT id, x FROM (VALUES (1), (2)) t(id) DECIDE x(INT) SUCH THAT x >= 0 "
    "AND SUM(x) <= (SELECT SUM(y) FROM (VALUES (1), (2)) u(uid) "
    "DECIDE y(INT) SUCH THAT y >= 5 AND y <= 1 MAXIMIZE SUM(y)) MAXIMIZE SUM(x)"
)
_SIBLING_FIRST_FAILS = (
    "SELECT a.id, a.x, b.y FROM "
    "(SELECT id, x FROM (VALUES (1), (2)) t(id) DECIDE x(INT) "
    " SUCH THAT x >= 5 AND x <= 1 MAXIMIZE SUM(x)) a JOIN "
    "(SELECT id, y FROM (VALUES (1), (2)) t(id) DECIDE y(INT) "
    " SUCH THAT y >= 0 AND y <= 3 MAXIMIZE SUM(y)) b USING (id)"
)
_SIBLING_SECOND_FAILS = (
    "SELECT a.id, a.x, b.y FROM "
    "(SELECT id, x FROM (VALUES (1), (2)) t(id) DECIDE x(INT) "
    " SUCH THAT x >= 0 AND x <= 3 MAXIMIZE SUM(x)) a JOIN "
    "(SELECT id, y FROM (VALUES (1), (2)) t(id) DECIDE y(INT) "
    " SUCH THAT y >= 5 AND y <= 1 MAXIMIZE SUM(y)) b USING (id)"
)


@pytest.mark.query_diagnostics
class TestDiagnoseReportsOnOneOptimization:
    """A query may run several optimizations; the diagnosis relation describes one.

    `DIAGNOSE` used to arm whichever DECIDE operator the plan walk reached first,
    so whether you got a diagnosis at all depended on which solve happened to fail
    — and when it was the wrong one, the query raised the unprefixed error telling
    the user to add the prefix they had already added. It now refuses up front and
    says how to get an answer. Solving such queries is unaffected; only the prefix
    is refused.
    """

    @pytest.mark.error
    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    @pytest.mark.parametrize(
        "sql,shape",
        [
            (_NESTED_SQL, "nested"),
            (_SIBLING_FIRST_FAILS, "sibling, first fails"),
            (_SIBLING_SECOND_FAILS, "sibling, second fails"),
        ],
    )
    def test_the_prefix_is_refused_on_more_than_one_decide(
        self, request, cli_fixture, sql, shape
    ):
        cli = request.getfixturevalue(cli_fixture)
        out = _combined(cli.execute_raw(f"DIAGNOSE {sql};"))
        assert "one optimization at a time" in out, shape
        assert "2 decide clauses" in out, shape
        assert "separately" in out, shape
        # The refusal replaces the old self-contradicting advice.
        assert "prefix the query with diagnose" not in out, shape

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    @pytest.mark.parametrize(
        "sql,shape",
        [(_NESTED_SQL, "nested"), (_SIBLING_SECOND_FAILS, "sibling")],
    )
    def test_isolating_the_failing_decide_diagnoses_it(
        self, request, cli_fixture, sql, shape
    ):
        """The refusal is actionable: the inner query on its own does diagnose."""
        cli = request.getfixturevalue(cli_fixture)
        isolated = (
            "SELECT SUM(y) FROM (VALUES (1), (2)) u(uid) "
            "DECIDE y(INT) SUCH THAT y >= 5 AND y <= 1 MAXIMIZE SUM(y)"
        )
        result = cli.execute_raw(f"DIAGNOSE {isolated};")
        assert not result.stderr.strip(), result.stderr
        assert "infeasible" in result.stdout.lower(), shape

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_several_decide_clauses_still_solve_without_the_prefix(
        self, request, cli_fixture
    ):
        """Only the diagnosis is refused. A query composing several optimizations
        runs exactly as before."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT a.id, a.x, b.y FROM "
            "(SELECT id, x FROM (VALUES (1), (2)) t(id) DECIDE x(INT) "
            " SUCH THAT x >= 0 AND x <= 3 MAXIMIZE SUM(x)) a JOIN "
            "(SELECT id, y FROM (VALUES (1), (2)) t(id) DECIDE y(INT) "
            " SUCH THAT y >= 0 AND y <= 2 MAXIMIZE SUM(y)) b USING (id)"
        )
        result = cli.execute_raw(f"{sql};")
        assert not result.stderr.strip(), result.stderr
        assert "one optimization at a time" not in result.stdout.lower()

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_a_plain_subquery_is_not_a_second_optimization(self, request, cli_fixture):
        """The count is of DECIDE operators, not of subqueries: an ordinary scalar
        subquery on a constraint bound still leaves exactly one optimization."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x FROM (VALUES (1), (2)) t(id) DECIDE x(INT) "
            "SUCH THAT x >= 9 AND x <= (SELECT MIN(id) FROM (VALUES (1), (2)) u(id)) "
            "MAXIMIZE SUM(x)"
        )
        result = cli.execute_raw(f"DIAGNOSE {sql};")
        assert not result.stderr.strip(), result.stderr
        assert "infeasible" in result.stdout.lower()


# A composed query where one DECIDE is still live when its sibling throws. The
# healthy side is deliberately large enough that its solve has not finished by the
# time the tiny infeasible side fails: with two rows it finishes first and nothing
# is interrupted. Small enough to solve in well under a second on its own.
_LIVE_SIBLING_WHEN_OTHER_FAILS = (
    "SELECT a.id, a.x, b.y FROM "
    "(SELECT i AS id, x FROM range(1, 600) t(i) DECIDE x(BOOL) "
    " SUCH THAT SUM(x * ((i * 7919) % 1000 + 1)) <= 150000 "
    "       AND SUM(x * ((i * 104729) % 997 + 1)) >= 90000 "
    " MAXIMIZE SUM(x * ((i * 104729) % 997 + 1))) a JOIN "
    "(SELECT i AS id, y FROM range(1, 600) t(i) DECIDE y(INT) "
    " SUCH THAT y >= 5 AND y <= 1 MAXIMIZE SUM(y)) b USING (id)"
)


@pytest.mark.query_diagnostics
class TestASiblingFailureIsNotACancellation:
    """One DECIDE failing must not make its sibling report a cancellation.

    DuckDB stops the remaining pipelines of a failed query by setting the same
    query interrupt Ctrl-C sets (`Executor::PushError`). The slow-solve checkpoint
    report reads that interrupt to tell "you stopped it" from "the clock ran out",
    so a live sibling used to print

        DECIDE stopped at your request before finding a solution yet.
          elapsed 0.00065s · peak memory 29.3 MB

    above the real error. Nobody asked it to stop, the two lines contradict each
    other, and being first it was read first — sending the user looking for a
    cancellation instead of the infeasible clause named underneath. The checkpoint
    now stays silent when the executor already holds an error.

    Only Gurobi reproduced it: HiGHS has no thread-safe terminate, so its solve is
    never cut short mid-search. Both backends run here anyway — the contract is
    that neither reports a cancellation.
    """

    @pytest.mark.error
    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_no_cancellation_wording_when_a_sibling_fails(self, request, cli_fixture):
        cli = request.getfixturevalue(cli_fixture)
        out = _combined(cli.execute_raw(f"{_LIVE_SIBLING_WHEN_OTHER_FAILS};"))
        assert "infeasible" in out, out[:600]
        assert "at your request" not in out, (
            f"A sibling's failure was reported as a cancellation:\n{out[:600]}"
        )
        # The elapsed/memory tail belongs to that report and describes a solve that
        # was never the problem, so it must be gone too.
        assert "peak memory" not in out, out[:600]
