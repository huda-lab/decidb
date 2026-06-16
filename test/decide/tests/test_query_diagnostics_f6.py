"""F6 — variable provenance (column-side): name the escaping variables.

F6 maps every solver column back to the user-facing thing it represents — user
variable name, or (for an auxiliary linearization column) the user's original
source expression. Folded into the unbounded diagnosis, the report now names the
escaping variable(s) instead of the generic scaffold prescription.

These tests drive the two-statement flow (a failing DECIDE that stashes, then a
SELECT that reads `decide_diagnostics()` back) via `execute_script`, under both
backends.

Empirical scope note: in current DECIDE formulations only *user* INTEGER/REAL
variables can actually escape to infinity. Auxiliary variables are structurally
bounded — ABS Big-M and bilinear McCormick require finite bounds (they error
before the solver) and MIN/MAX/`<>` indicators are BOOLEAN [0,1]. So the e2e
escaping-variable naming exercises user vars; the aux→expression resolution is
covered by the C++ unit test (test_decidb_variable_provenance.cpp).
"""

import pytest


_BACKENDS = ["decidb_cli_highs", "decidb_cli_gurobi"]


def _script(decide_sql, mode="unbounded"):
    return (
        f"PRAGMA diagnose_decide='{mode}';\n"
        f"{decide_sql};\n"
        "SELECT * FROM decide_diagnostics();\n"
    )


@pytest.mark.query_diagnostics
class TestF6VariableNaming:
    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_unbounded_real_var_named(self, request, cli_fixture):
        """A single escaping REAL user variable is named in the prescription."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x FROM (VALUES (1), (2)) t(id) "
            "DECIDE x IS REAL SUCH THAT x >= 0 MAXIMIZE SUM(x)"
        )
        result = cli.execute_script(_script(sql))

        assert (
            "diagnosis ready: select * from decide_diagnostics()"
            in result.stderr.lower()
        )
        out = result.stdout.lower()
        assert "unbounded" in out
        assert "add bound" in out
        # The variable name 'x' rides in suggested_change, summary mentions it too.
        assert "'x' can grow without bound" in out
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
        result = cli.execute_script(_script(sql))
        assert "'n' can grow without bound" in result.stdout.lower()

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_multiple_escaping_vars_each_named_once(self, request, cli_fixture):
        """Two escaping vars yield two rows; each row-scoped var is deduped to one
        row even though it instantiates a column per source row."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x, y FROM (VALUES (1), (2)) t(id) "
            "DECIDE x IS REAL, y IS REAL SUCH THAT x >= 0 AND y >= 0 "
            "MAXIMIZE SUM(x + y)"
        )
        rows = cli.execute_script(_script(sql)).stdout.lower()
        assert "'x' can grow without bound" in rows
        assert "'y' can grow without bound" in rows
        # Deduped: exactly one row per variable (not one per (var, source-row)).
        assert rows.count("'x' can grow without bound") == 1
        assert rows.count("'y' can grow without bound") == 1

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_auto_mode_names_unbounded_var(self, request, cli_fixture):
        """`auto` routes an unbounded solve to the named diagnosis too."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x FROM (VALUES (1), (2)) t(id) "
            "DECIDE x IS REAL SUCH THAT x >= 0 MAXIMIZE SUM(x)"
        )
        out = cli.execute_script(_script(sql, mode="auto")).stdout.lower()
        assert "'x' can grow without bound" in out

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_no_pragma_no_naming(self, request, cli_fixture):
        """Manual-first: without the pragma, nothing is stashed or named."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x FROM (VALUES (1), (2)) t(id) "
            "DECIDE x IS REAL SUCH THAT x >= 0 MAXIMIZE SUM(x)"
        )
        script = f"{sql};\nSELECT * FROM decide_diagnostics();\n"
        result = cli.execute_script(script)
        assert "diagnosis ready" not in result.stderr.lower()
        assert "can grow without bound" not in result.stdout.lower()
