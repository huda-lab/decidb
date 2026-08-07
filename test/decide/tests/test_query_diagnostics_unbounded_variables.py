"""Unbounded variable diagnostics: name each escaping variable.

Every solver column maps back to the user-facing thing it represents: user
variable name, or (for an auxiliary linearization column) the user's original
source expression. Folded into the unbounded diagnosis, the decide_diagnostics()
relation reports each escaping variable by NAME with the DIRECTION it escapes
(always +inf today, since user variables are bounded below at 0).

These tests drive the two-statement flow (a failing DECIDE that stashes, then a
SELECT that reads `decide_diagnostics()` back) via `execute_script`, reading the
relation as CSV so single-letter variable names assert unambiguously. Run under
both backends.

Empirical scope note: in current DECIDE formulations only *user* INTEGER/REAL
variables can actually escape to infinity. Auxiliary variables are structurally
bounded — ABS Big-M and bilinear McCormick require finite bounds (they error
before the solver) and MIN/MAX/`<>` indicators are BOOLEAN [0,1]. So the e2e
escaping-variable naming exercises user vars; the aux→expression resolution is
covered by the C++ unit test (test_decidb_variable_provenance.cpp).
"""

import csv
import io

import pytest


_BACKENDS = ["decidb_cli_highs", "decidb_cli_gurobi"]


def _diagnose(cli, decide_sql, mode="auto"):
    script = (
        ".mode csv\n"
        f"PRAGMA diagnose_decide='{mode}';\n"
        f"{decide_sql};\n"
        "SELECT * FROM decide_diagnostics();\n"
    )
    return cli.execute_script(script)


def _rows(result):
    return list(csv.DictReader(io.StringIO(result.stdout)))


def _attrs(rows, subject):
    return {
        r["attribute"]: r["value"]
        for r in rows
        if r["subject_kind"] == "variable" and r["subject"] == subject
    }


@pytest.mark.query_diagnostics
class TestUnboundedVariableDiagnostics:
    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_unbounded_real_var_named(self, request, cli_fixture):
        """A single escaping REAL user variable is named in the relation."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x FROM (VALUES (1), (2)) t(id) "
            "DECIDE x(REAL) SUCH THAT x >= 0 MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql)

        assert (
            "details: select * from decide_diagnostics()"
            in result.stderr.lower()
        )
        rows = _rows(result)
        assert _attrs(rows, "x")["grows_toward"] == "+inf"
        assert {r["state"] for r in rows} == {"unbounded"}
        # The summary still names the variable on stderr.
        assert "variable x can grow without bound" in result.stderr.lower()
        # A4: and prescribes the forced remedy (add a bound) without a number.
        assert "add an upper bound, e.g. such that x <= <cap>" in result.stderr.lower()
        assert "it may instead be infeasible." not in result.stderr.lower()

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_unbounded_integer_var_named(self, request, cli_fixture):
        """An INTEGER var is a MILP — on HiGHS this is the INF_OR_UNBD (status 9)
        path that the zero-objective probe disambiguates — and is still named
        correctly."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, n FROM (VALUES (1), (2)) t(id) "
            "DECIDE n(INT) SUCH THAT n >= 0 MAXIMIZE SUM(n)"
        )
        result = _diagnose(cli, sql)
        rows = _rows(result)
        assert _attrs(rows, "n")["grows_toward"] == "+inf"
        assert "it may instead be infeasible." not in result.stderr.lower()

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_multiple_escaping_vars_each_named_once(self, request, cli_fixture):
        """Two escaping vars are deduped by variable and share one diagnosis_id."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x, y FROM (VALUES (1), (2)) t(id) "
            "DECIDE x(REAL), y(REAL) SUCH THAT x >= 0 AND y >= 0 "
            "MAXIMIZE SUM(x + y)"
        )
        rows = _rows(_diagnose(cli, sql))
        assert _attrs(rows, "x")["grows_toward"] == "+inf"
        assert _attrs(rows, "y")["grows_toward"] == "+inf"
        assert _attrs(rows, "x")["affected_rows"] == "all 2 rows"
        assert _attrs(rows, "y")["affected_rows"] == "all 2 rows"
        assert {
            r["subject"]
            for r in rows
            if r["subject_kind"] == "variable" and r["attribute"] == "grows_toward"
        } == {"x", "y"}
        assert len({r["diagnosis_id"] for r in rows}) == 1

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_minimize_unbounded_var_named_from_improving_direction(
        self, request, cli_fixture
    ):
        """A MINIMIZE objective that improves toward -inf still names x escaping +inf."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x FROM (VALUES (1), (2)) t(id) "
            "DECIDE x(REAL) SUCH THAT x >= 0 MINIMIZE SUM(x * -1)"
        )
        result = _diagnose(cli, sql)
        rows = _rows(result)
        assert _attrs(rows, "x")["grows_toward"] == "+inf"
        assert "variable x can grow without bound" in result.stderr.lower()

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_auto_mode_names_unbounded_var(self, request, cli_fixture):
        """`auto` routes an unbounded solve to the named diagnosis too."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x FROM (VALUES (1), (2)) t(id) "
            "DECIDE x(REAL) SUCH THAT x >= 0 MAXIMIZE SUM(x)"
        )
        rows = _rows(_diagnose(cli, sql, mode="auto"))
        assert _attrs(rows, "x")["grows_toward"] == "+inf"

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_off_no_naming(self, request, cli_fixture):
        """With diagnosis turned `off`, nothing is stashed or named."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x FROM (VALUES (1), (2)) t(id) "
            "DECIDE x(REAL) SUCH THAT x >= 0 MAXIMIZE SUM(x)"
        )
        script = (
            ".mode csv\n"
            "PRAGMA diagnose_decide='off';\n"
            f"{sql};\nSELECT * FROM decide_diagnostics();\n"
        )
        result = cli.execute_script(script)
        assert "select * from decide_diagnostics()" not in result.stderr.lower()
        assert _rows(result) == []
        # A1: the static error points a user who turned diagnosis off back to it.
        assert (
            "set pragma diagnose_decide='auto' and re-run"
            in result.stderr.lower()
        )
