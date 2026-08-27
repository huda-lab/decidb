"""Unbounded variable diagnostics: name each escaping variable.

Every solver column maps back to the user-facing thing it represents: user
variable name, or (for an auxiliary linearization column) the user's original
source expression. Folded into the unbounded diagnosis, the DIAGNOSE
relation reports each escaping variable by NAME with the DIRECTION it escapes
(always +inf today, since user variables are bounded below at 0).

These tests run `DIAGNOSE <query>`, which is the only thing that starts the engine,
reading the relation as CSV so single-letter variable names assert unambiguously. Run
under both backends.

Empirical scope note: in current DECIDE formulations only *user* INTEGER/REAL
variables can actually escape to infinity. Auxiliary variables are structurally
bounded — ABS Big-M and bilinear McCormick require finite bounds (they error
before the solver) and MIN/MAX/`<>` indicators are BOOLEAN [0,1]. So the e2e
escaping-variable naming exercises user vars; the aux→expression resolution is
covered by the C++ unit test (test_decidb_variable_provenance.cpp).
"""

import pytest

from . import _diagnose_relation


_BACKENDS = ["decidb_cli_highs", "decidb_cli_gurobi"]


def _diagnose(cli, decide_sql):
    return _diagnose_relation.run(cli, decide_sql)


def _rows(result):
    return _diagnose_relation.eav_rows(result)


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

        # DIAGNOSE returns the finding; it does not raise.
        assert not result.stderr.strip(), result.stderr
        rows = _rows(result)
        assert _attrs(rows, "x")["grows_toward"] == "+inf"
        assert {r["state"] for r in rows} == {"unbounded"}
        # A4: it prescribes the forced remedy (add a bound) without inventing the cap.
        flat = _diagnose_relation.rows(result)
        assert [f["suggested_change"] for f in flat] == ["x <= <cap>"]

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
        assert {r["state"] for r in rows} == {"unbounded"}

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_multiple_escaping_vars_each_named_once(self, request, cli_fixture):
        """Two escaping vars are each named once, in one diagnosis."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x, y FROM (VALUES (1), (2)) t(id) "
            "DECIDE x(REAL), y(REAL) SUCH THAT x >= 0 AND y >= 0 "
            "MAXIMIZE SUM(x + y)"
        )
        rows = _rows(_diagnose(cli, sql))
        assert _attrs(rows, "x")["grows_toward"] == "+inf"
        assert _attrs(rows, "y")["grows_toward"] == "+inf"
        # Every instance of each escapes, so the count covers the whole variable and no
        # categorical slice is named.
        assert _attrs(rows, "x")["escaping_instances"] == "2"
        assert _attrs(rows, "y")["escaping_instances"] == "2"
        assert {
            r["subject"]
            for r in rows
            if r["subject_kind"] == "variable" and r["attribute"] == "grows_toward"
        } == {"x", "y"}
        assert {r["state"] for r in rows} == {"unbounded"}

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

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_without_the_prefix_no_variable_is_named(self, request, cli_fixture):
        """The unprefixed query reports its state and stops: no variable, no remedy,
        and no second solve to work either out."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x FROM (VALUES (1), (2)) t(id) "
            "DECIDE x(REAL) SUCH THAT x >= 0 MAXIMIZE SUM(x)"
        )
        result = cli.execute_script(f".mode csv\n{sql};\n")
        err = result.stderr.lower()
        assert "decide optimization is unbounded" in err
        assert "variable x" not in err
        # A1: it points the user at the prefix that WOULD name it.
        assert "diagnose" in err
