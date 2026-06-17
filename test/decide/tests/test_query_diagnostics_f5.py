"""F5 — the shared diagnostic reporting surface (decide_diagnostics()).

F5 adds a structured diagnosis that a state engine populates, stashes
per-connection, and surfaces via the `decide_diagnostics()` table function with a
fixed schema. For the unbounded state each row names one escaping variable:

    query_id | state | variable | direction | group_label | suggested_bound

`group_label` and `suggested_bound` are reserved for later enrichment and read
NULL for now; `query_id` ties together every row of one failed solve.

The end-to-end flow spans two statements on one connection (a failing DECIDE that
stashes, then a SELECT that reads it back), so these tests drive the CLI via
`execute_script` (stdin) — `-c` halts after the DECIDE error. The relation is read
as CSV so its rows parse unambiguously. Runs under both backends.
"""

import csv
import io

import pytest


_BACKENDS = ["decidb_cli_highs", "decidb_cli_gurobi"]

_UNBOUNDED_SQL = (
    "SELECT id, x FROM (VALUES (1), (2)) t(id) "
    "DECIDE x IS REAL SUCH THAT x >= 0 MAXIMIZE SUM(x)"
)

_EXPECTED_SCHEMA = [
    "query_id",
    "state",
    "variable",
    "direction",
    "group_label",
    "suggested_bound",
]


def _diagnose(cli, decide_sql, mode="unbounded"):
    """Run PRAGMA + a failing DECIDE + the relation read on one stdin session.
    The relation is emitted as CSV so its rows parse unambiguously."""
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
class TestF5DiagnosticsRelation:
    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_unbounded_diagnosis_surfaces_as_relation(self, request, cli_fixture):
        """Failing DECIDE stashes; the follow-up SELECT returns one named row."""
        cli = request.getfixturevalue(cli_fixture)
        result = _diagnose(cli, _UNBOUNDED_SQL)

        # The DECIDE itself still errors, with the pointer on stderr.
        assert (
            "diagnosis ready: select * from decide_diagnostics()"
            in result.stderr.lower()
        )

        rows = _rows(result)
        assert len(rows) == 1
        row = rows[0]
        assert row["state"] == "unbounded"
        assert row["variable"] == "x"
        assert row["direction"] == "+∞"
        # group_label + suggested_bound are SQL NULL (reserved for later); DuckDB's
        # CSV writer renders a NULL cell as the literal "NULL".
        assert row["group_label"] == "NULL"
        assert row["suggested_bound"] == "NULL"

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_relation_has_fixed_schema(self, request, cli_fixture):
        """The table function's bind-time schema is the fixed unbounded relation."""
        cli = request.getfixturevalue(cli_fixture)
        # Fresh process => empty stash => zero rows, but the schema is fixed.
        # DESCRIBE always yields the column list regardless of stash contents.
        desc_rows, _ = cli.execute("DESCRIBE SELECT * FROM decide_diagnostics()")
        names = [str(r[0]).lower() for r in desc_rows]
        assert names == _EXPECTED_SCHEMA

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
        script = f".mode csv\n{_UNBOUNDED_SQL};\nSELECT * FROM decide_diagnostics();\n"
        result = cli.execute_script(script)
        # Static error, no pointer, and the relation stays empty.
        assert "diagnosis ready" not in result.stderr.lower()
        assert _rows(result) == []
