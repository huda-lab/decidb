"""Structured solver-result regressions for query diagnostics.

The solver boundary returns SolverResult for non-optimal statuses; PhysicalDecide
decides how to surface them. These tests pin the externally visible contract:

  * optimal statuses still return normal DECIDE rows;
  * non-optimal statuses still surface friendly DECIDE errors;
  * HiGHS's ambiguous MILP status 9 is disambiguated to a definitive error.
"""

import pytest


_OPTIMAL_SQL = """
    WITH data AS (
        SELECT 1 AS id, 10.0 AS val UNION ALL
        SELECT 2, 20.0 UNION ALL
        SELECT 3, 30.0
    )
    SELECT id, val, x FROM data
    DECIDE x(BOOL)
    SUCH THAT SUM(x) = 1
    MAXIMIZE SUM(x * val)
"""


_INFEASIBLE_SQL = """
    SELECT id, x FROM (VALUES (1), (2)) t(id)
    DECIDE x(BOOL)
    SUCH THAT SUM(x) >= 3
    MAXIMIZE SUM(x)
"""


_UNBOUNDED_LP_SQL = """
    SELECT id, x FROM (VALUES (1), (2)) t(id)
    DECIDE x(REAL)
    SUCH THAT x >= 0
    MAXIMIZE SUM(x)
"""


_UNBOUNDED_MILP_SQL = """
    SELECT id, x FROM (VALUES (1), (2)) t(id)
    DECIDE x(INT)
    SUCH THAT x >= 1
    MAXIMIZE SUM(x)
"""


# Same MILP-unbounded shape but escaping toward -inf via MINIMIZE. The
# zero-objective probe zeros the objective entirely, so disambiguation must be
# sense-agnostic: a MINIMIZE-unbounded MILP has to resolve to the same definitive
# "unbounded" error, not depend on the MAXIMIZE sense.
_UNBOUNDED_MILP_MIN_SQL = """
    SELECT id, x FROM (VALUES (1), (2)) t(id)
    DECIDE x(INT)
    SUCH THAT x >= 1
    MINIMIZE SUM(x * -1)
"""


def _combined_error(cli, sql: str) -> str:
    result = cli.execute_raw(sql)
    combined = result.stderr + result.stdout
    assert result.stderr.strip(), (
        "Expected DECIDE query to fail, but stderr was empty.\n"
        f"stdout: {result.stdout[:500]}"
    )
    return combined


def _assert_friendly_error(cli, sql: str, required: str) -> None:
    combined = _combined_error(cli, sql)
    lower = combined.lower()
    assert required.lower() in lower, (
        f"Expected friendly error containing {required!r}, got:\n"
        f"{combined[:700]}"
    )
    assert "solver status" not in lower, (
        "Structured-result regression: raw backend status leaked through the user-facing "
        f"error:\n{combined[:700]}"
    )


@pytest.mark.query_diagnostics
class TestStructuredSolverResult:
    @pytest.mark.parametrize(
        "cli_fixture", ["decidb_cli_highs", "decidb_cli_gurobi"]
    )
    def test_optimal_status_still_returns_solution(self, request, cli_fixture):
        """Backends still populate SolverResult.solution on OPTIMAL."""
        cli = request.getfixturevalue(cli_fixture)
        rows, cols = cli.execute(_OPTIMAL_SQL)

        assert rows
        id_idx = cols.index("id")
        x_idx = cols.index("x")
        selected_ids = [int(row[id_idx]) for row in rows if int(row[x_idx]) == 1]
        assert selected_ids == [3]

    @pytest.mark.error
    @pytest.mark.parametrize(
        ("sql", "required"),
        [
            (_INFEASIBLE_SQL, "DECIDE optimization is infeasible"),
            (_UNBOUNDED_LP_SQL, "DECIDE optimization is unbounded"),
        ],
    )
    def test_highs_non_optimal_statuses_use_friendly_errors(
        self, decidb_cli_highs, sql, required
    ):
        """HiGHS INFEASIBLE / UNBOUNDED return SolverResult, then operator throws."""
        _assert_friendly_error(decidb_cli_highs, sql, required)

    @pytest.mark.error
    @pytest.mark.parametrize(
        "sql", [_UNBOUNDED_MILP_SQL, _UNBOUNDED_MILP_MIN_SQL]
    )
    def test_highs_inf_or_unbd_status_9_is_disambiguated(
        self, decidb_cli_highs, sql
    ):
        """HiGHS MILP-unbounded can report kUnboundedOrInfeasible (raw 9).

        The backend maps that to INF_OR_UNBD instead of falling through to the
        generic raw-status message. The zero-objective feasibility probe then
        converts this case to a definitive unbounded error. The probe zeros the
        objective, so both MAXIMIZE and MINIMIZE unbounded MILPs must disambiguate
        the same way.
        """
        combined = _combined_error(decidb_cli_highs, sql)
        lower = combined.lower()
        assert "decide optimization is unbounded" in lower, (
            "HiGHS status 9 was not disambiguated to a "
            f"definitive unbounded error:\n{combined[:700]}"
        )
        assert "infeasible or unbounded" not in lower, (
            "Ambiguous INF_OR_UNBD message leaked through:\n"
            f"{combined[:700]}"
        )
        assert "solver status" not in lower, (
            "Structured-result regression: raw backend status leaked through the user-facing "
            f"error:\n{combined[:700]}"
        )

    @pytest.mark.error
    @pytest.mark.parametrize(
        ("sql", "required"),
        [
            (_INFEASIBLE_SQL, "DECIDE optimization is infeasible"),
            (_UNBOUNDED_LP_SQL, "DECIDE optimization is unbounded"),
        ],
    )
    def test_gurobi_non_optimal_statuses_use_friendly_errors(
        self, decidb_cli_gurobi, sql, required
    ):
        """Gurobi non-optimal statuses follow the same operator throw path."""
        _assert_friendly_error(decidb_cli_gurobi, sql, required)
