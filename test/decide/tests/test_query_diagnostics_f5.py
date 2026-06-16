"""F5 — the shared diagnostic reporting surface (decide_diagnostics()).

F5 adds a structured diagnosis that a state engine populates, stashes
per-connection, and surfaces via the `decide_diagnostics()` table function with a
fixed schema (state, clause, group_key, edit_kind, suggested_change). This session
the unbounded engine fills a scaffold row (status + "add a bound" prescription);
U3/F6 will later enrich it with the named escaping variables.

The end-to-end flow spans two statements on one connection (a failing DECIDE that
stashes, then a SELECT that reads it back), so these tests drive the CLI via
`execute_script` (stdin) — `-c` halts after the DECIDE error. Runs under both
backends.
"""

import pytest


_BACKENDS = ["decidb_cli_highs", "decidb_cli_gurobi"]

_UNBOUNDED_SQL = (
    "SELECT id, x FROM (VALUES (1), (2)) t(id) "
    "DECIDE x IS REAL SUCH THAT x >= 0 MAXIMIZE SUM(x)"
)

_DIAGNOSE_THEN_READ = (
    "PRAGMA diagnose_decide='unbounded';\n"
    f"{_UNBOUNDED_SQL};\n"
    "SELECT * FROM decide_diagnostics();\n"
)


@pytest.mark.query_diagnostics
class TestF5DiagnosticsRelation:
    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_unbounded_diagnosis_surfaces_as_relation(self, request, cli_fixture):
        """Failing DECIDE stashes; the follow-up SELECT returns the scaffold row."""
        cli = request.getfixturevalue(cli_fixture)
        result = cli.execute_script(_DIAGNOSE_THEN_READ)

        # The DECIDE itself still errors, with the pointer on stderr.
        assert (
            "diagnosis ready: select * from decide_diagnostics()"
            in result.stderr.lower()
        )

        # The relation is read from stdout (table format).
        out = result.stdout.lower()
        assert "unbounded" in out          # state column
        assert "add bound" in out          # edit_kind column
        assert "add an upper bound" in out  # suggested_change column

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_relation_has_fixed_schema(self, request, cli_fixture):
        """The table function's bind-time schema is the shared 5-column relation."""
        cli = request.getfixturevalue(cli_fixture)
        rows, cols = cli.execute("SELECT * FROM decide_diagnostics()")
        # Fresh process => empty stash => zero rows, but the schema is fixed.
        # (`execute` returns ([], []) on an empty JSON result, so assert schema via
        #  a DESCRIBE instead, which always yields the column list.)
        desc_rows, _ = cli.execute("DESCRIBE SELECT * FROM decide_diagnostics()")
        names = [str(r[0]).lower() for r in desc_rows]
        assert names == [
            "state",
            "clause",
            "group_key",
            "edit_kind",
            "suggested_change",
        ]
        assert rows == []

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_empty_when_nothing_diagnosed(self, request, cli_fixture):
        """With no prior failed DECIDE on the connection, the relation is empty."""
        cli = request.getfixturevalue(cli_fixture)
        rows, _ = cli.execute("SELECT * FROM decide_diagnostics()")
        assert rows == []

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_no_diagnosis_stashed_without_pragma(self, request, cli_fixture):
        """Manual-first: an unbounded DECIDE with no pragma stashes nothing."""
        cli = request.getfixturevalue(cli_fixture)
        script = f"{_UNBOUNDED_SQL};\nSELECT * FROM decide_diagnostics();\n"
        result = cli.execute_script(script)
        # Static error, no pointer, and the relation stays empty.
        assert "diagnosis ready" not in result.stderr.lower()
        assert "add bound" not in result.stdout.lower()
