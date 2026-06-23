"""F5 — the shared diagnostic reporting surface (decide_diagnostics()).

F5 adds a structured diagnosis that a state engine populates, stashes
per-connection, and surfaces via the `decide_diagnostics()` table function with a
fixed schema. For the unbounded state each row names one escaping variable:

    query_id | state | variable | direction | escaping_instances

`escaping_instances` characterizes which instances of the variable escape (here
all of them); `query_id` ties together every row of one failed solve. The forced
remedy (add a bound) is prescribed in the stderr summary, not a per-row column.

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
    "escaping_instances",
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
            "diagnosis ready (this session): select * from decide_diagnostics()"
            in result.stderr.lower()
        )

        rows = _rows(result)
        assert len(rows) == 1
        row = rows[0]
        assert row["state"] == "unbounded"
        assert row["variable"] == "x"
        assert row["direction"] == "+inf"
        # Both instances of x escape, so escaping_instances reports the total-escape
        # summary.
        assert row["escaping_instances"] == "all 2 instances escape"

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

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_successful_solve_clears_stale_diagnosis(self, request, cli_fixture):
        """A2: fail -> fix -> succeed invalidates the stash, so decide_diagnostics()
        does not keep reporting a now-resolved failure. The success DECIDE also emits
        CSV, so the relation read emits a `rows=N` sentinel to parse past it."""
        cli = request.getfixturevalue(cli_fixture)
        bounded_sql = (
            "SELECT id, x FROM (VALUES (1), (2)) t(id) "
            "DECIDE x IS REAL SUCH THAT x >= 0 AND x <= 5 MAXIMIZE SUM(x)"
        )
        script = (
            ".mode csv\n"
            "PRAGMA diagnose_decide='unbounded';\n"
            f"{_UNBOUNDED_SQL};\n"  # stashes an unbounded diagnosis (and errors)
            f"{bounded_sql};\n"  # succeeds -> clears the stash
            "SELECT 'rows=' || count(*) AS diag FROM decide_diagnostics();\n"
        )
        result = cli.execute_script(script)
        assert "rows=0" in result.stdout
        assert "rows=1" not in result.stdout

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_qp_unbounded_no_content_free_diagnosis(self, request, cli_fixture):
        """A5: an unbounded solve that names no variable must not stash a content-free
        all-NULL row. A quadratic objective attaches no ray, so under `auto` the
        diagnosis has no per-variable content and falls through to the rich static
        error (Gurobi reaches UNBOUNDED here); HiGHS rejects the non-convex QP
        pre-solve. Either path: no 'diagnosis ready' pointer and an empty relation."""
        cli = request.getfixturevalue(cli_fixture)
        qp_sql = (
            "SELECT id, x FROM (VALUES (1), (2)) t(id) "
            "DECIDE x IS REAL SUCH THAT x >= 0 MAXIMIZE SUM(POWER(x, 2))"
        )
        result = _diagnose(cli, qp_sql, mode="auto")
        assert "diagnosis ready" not in result.stderr.lower()
        assert _rows(result) == []
        if "gurobi" in cli_fixture:
            # Gurobi reports UNBOUNDED, exercising the A5 fall-through to the static
            # error (the old code replaced this with an all-NULL diagnosis row).
            assert "you must add constraints to bound" in result.stderr.lower()
