"""F6 — variable provenance (column-side): name the escaping variables.

F6 maps every solver column back to the user-facing thing it represents — user
variable name, or (for an auxiliary linearization column) the user's original
source expression. Folded into the unbounded diagnosis, the decide_diagnostics()
relation now reports each escaping variable by NAME with the DIRECTION it escapes
(always +∞ today, since user variables are bounded below at 0).

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


def _diagnose(cli, decide_sql, mode="unbounded"):
    script = (
        ".mode csv\n"
        f"PRAGMA diagnose_decide='{mode}';\n"
        f"{decide_sql};\n"
        "SELECT * FROM decide_diagnostics();\n"
    )
    return cli.execute_script(script)


def _rows(result):
    return list(csv.DictReader(io.StringIO(result.stdout)))


@pytest.mark.query_diagnostics
class TestF6VariableNaming:
    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_unbounded_real_var_named(self, request, cli_fixture):
        """A single escaping REAL user variable is named in the relation."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x FROM (VALUES (1), (2)) t(id) "
            "DECIDE x IS REAL SUCH THAT x >= 0 MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql)

        assert (
            "diagnosis ready: select * from decide_diagnostics()"
            in result.stderr.lower()
        )
        rows = _rows(result)
        assert len(rows) == 1
        assert rows[0]["variable"] == "x"
        assert rows[0]["direction"] == "+∞"
        assert rows[0]["state"] == "unbounded"
        # The summary still names the variable on stderr.
        assert "the variable x can grow without bound" in result.stderr.lower()

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_unbounded_integer_var_named(self, request, cli_fixture):
        """An INTEGER var is a MILP — on HiGHS this is the INF_OR_UNBD (status 9)
        path that U1 disambiguates — and is still named correctly."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, n FROM (VALUES (1), (2)) t(id) "
            "DECIDE n IS INTEGER SUCH THAT n >= 0 MAXIMIZE SUM(n)"
        )
        rows = _rows(_diagnose(cli, sql))
        assert [r["variable"] for r in rows] == ["n"]
        assert rows[0]["direction"] == "+∞"

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_multiple_escaping_vars_each_named_once(self, request, cli_fixture):
        """Two escaping vars yield two rows; each row-scoped var is deduped to one
        row even though it instantiates a column per source row, and both rows
        share one query_id (same solve)."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x, y FROM (VALUES (1), (2)) t(id) "
            "DECIDE x IS REAL, y IS REAL SUCH THAT x >= 0 AND y >= 0 "
            "MAXIMIZE SUM(x + y)"
        )
        rows = _rows(_diagnose(cli, sql))
        assert len(rows) == 2
        assert sorted(r["variable"] for r in rows) == ["x", "y"]
        assert len({r["query_id"] for r in rows}) == 1

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_auto_mode_names_unbounded_var(self, request, cli_fixture):
        """`auto` routes an unbounded solve to the named diagnosis too."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x FROM (VALUES (1), (2)) t(id) "
            "DECIDE x IS REAL SUCH THAT x >= 0 MAXIMIZE SUM(x)"
        )
        rows = _rows(_diagnose(cli, sql, mode="auto"))
        assert [r["variable"] for r in rows] == ["x"]

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_no_pragma_no_naming(self, request, cli_fixture):
        """Manual-first: without the pragma, nothing is stashed or named."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x FROM (VALUES (1), (2)) t(id) "
            "DECIDE x IS REAL SUCH THAT x >= 0 MAXIMIZE SUM(x)"
        )
        script = f".mode csv\n{sql};\nSELECT * FROM decide_diagnostics();\n"
        result = cli.execute_script(script)
        assert "diagnosis ready" not in result.stderr.lower()
        assert _rows(result) == []
