"""F1 structured solver-result regressions.

F1 changed the solver boundary from "backend throws on non-optimal status" to
"backend returns SolverResult; PhysicalDecide decides how to surface it." These
tests pin the externally visible contract until F4 adds the diagnose pragma:

  * optimal statuses still return normal DECIDE rows;
  * non-optimal statuses still surface the friendly DECIDE errors;
  * HiGHS's ambiguous MILP status 9 no longer leaks as "solver status 9".
"""

import pytest


_OPTIMAL_SQL = """
    WITH data AS (
        SELECT 1 AS id, 10.0 AS val UNION ALL
        SELECT 2, 20.0 UNION ALL
        SELECT 3, 30.0
    )
    SELECT id, val, x FROM data
    DECIDE x IS BOOLEAN
    SUCH THAT SUM(x) = 1
    MAXIMIZE SUM(x * val)
"""


_INFEASIBLE_SQL = """
    SELECT id, x FROM (VALUES (1), (2)) t(id)
    DECIDE x IS BOOLEAN
    SUCH THAT SUM(x) >= 3
    MAXIMIZE SUM(x)
"""


_UNBOUNDED_LP_SQL = """
    SELECT id, x FROM (VALUES (1), (2)) t(id)
    DECIDE x IS REAL
    SUCH THAT x >= 0
    MAXIMIZE SUM(x)
"""


_UNBOUNDED_MILP_SQL = """
    SELECT id, x FROM (VALUES (1), (2)) t(id)
    DECIDE x IS INTEGER
    SUCH THAT x >= 1
    MAXIMIZE SUM(x)
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
        "F1 regression: raw backend status leaked through the user-facing "
        f"error:\n{combined[:700]}"
    )


@pytest.mark.query_diagnostics
class TestF1StructuredSolverResult:
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
    def test_highs_non_optimal_statuses_use_default_errors(
        self, decidb_cli_highs, sql, required
    ):
        """HiGHS INFEASIBLE / UNBOUNDED return SolverResult, then operator throws."""
        _assert_friendly_error(decidb_cli_highs, sql, required)

    @pytest.mark.error
    def test_highs_inf_or_unbd_status_9_is_not_generic(
        self, decidb_cli_highs
    ):
        """HiGHS MILP-unbounded can report kUnboundedOrInfeasible (raw 9).

        F1 must map that to INF_OR_UNBD, whose default message mentions
        unboundedness, instead of falling through to the generic raw-status
        message. U1 may later disambiguate this to a definitive unbounded error;
        either way, raw "solver status 9" must not leak.
        """
        _assert_friendly_error(decidb_cli_highs, _UNBOUNDED_MILP_SQL, "unbounded")

    @pytest.mark.error
    @pytest.mark.parametrize(
        ("sql", "required"),
        [
            (_INFEASIBLE_SQL, "DECIDE optimization is infeasible"),
            (_UNBOUNDED_LP_SQL, "DECIDE optimization is unbounded"),
        ],
    )
    def test_gurobi_non_optimal_statuses_use_default_errors(
        self, decidb_cli_gurobi, sql, required
    ):
        """Gurobi non-optimal statuses follow the same F1 operator throw path."""
        _assert_friendly_error(decidb_cli_gurobi, sql, required)
