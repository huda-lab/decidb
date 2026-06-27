"""The shared diagnostic reporting surface: decide_diagnostics().

A state engine populates a structured diagnosis, stashes it per-connection, and
surfaces it via the `decide_diagnostics()` table function with a fixed long-form
schema. For the unbounded state each variable owns attributes:

    diagnosis_id | state | subject_kind | subject | attribute | value

`affected_rows` characterizes which rows of the variable escape (here all of
them); `diagnosis_id` ties together every row of one failed solve. The
forced remedy (add a bound) is prescribed in the stderr summary, not a per-row
column.

The end-to-end flow spans two statements on one connection (a failing DECIDE that
stashes, then a SELECT that reads it back), so these tests drive the CLI via
`execute_script` (stdin) — `-c` halts after the DECIDE error. The relation is read
as CSV so its rows parse unambiguously. Runs under both backends.
"""

import csv
import io
import re

import pytest


_BACKENDS = ["decidb_cli_highs", "decidb_cli_gurobi"]

_UNBOUNDED_SQL = (
    "SELECT id, x FROM (VALUES (1), (2)) t(id) "
    "DECIDE x IS REAL SUCH THAT x >= 0 MAXIMIZE SUM(x)"
)

_EXPECTED_SCHEMA = [
    "diagnosis_id",
    "state",
    "subject_kind",
    "subject",
    "attribute",
    "value",
]


def _diagnose(cli, decide_sql, mode="auto"):
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


def _attrs(rows, subject_kind, subject):
    return {
        r["attribute"]: r["value"]
        for r in rows
        if r["subject_kind"] == subject_kind and r["subject"] == subject
    }


def _diagnosis_marker(stdout, label):
    match = re.search(rf"{label}=(\d+):(\d+):(\d+)", stdout)
    assert match, f"Missing {label!r} diagnosis marker in stdout:\n{stdout}"
    return tuple(int(group) for group in match.groups())


@pytest.mark.query_diagnostics
class TestDiagnosticsRelation:
    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_unbounded_diagnosis_surfaces_as_relation(self, request, cli_fixture):
        """Failing DECIDE stashes; the follow-up SELECT returns one named row."""
        cli = request.getfixturevalue(cli_fixture)
        result = _diagnose(cli, _UNBOUNDED_SQL)

        # The DECIDE itself still errors, with the pointer on stderr.
        assert (
            "details: select * from decide_diagnostics()"
            in result.stderr.lower()
        )

        rows = _rows(result)
        assert len(rows) == 2
        assert {r["state"] for r in rows} == {"unbounded"}
        attrs = _attrs(rows, "variable", "x")
        assert attrs["grows_toward"] == "+inf"
        # Both instances of x escape, so affected_rows reports the total-escape summary.
        assert attrs["affected_rows"] == "all 2 rows"

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
    def test_no_diagnosis_stashed_when_off(self, request, cli_fixture):
        """With diagnosis turned `off`, an unbounded DECIDE stashes nothing."""
        cli = request.getfixturevalue(cli_fixture)
        script = (
            ".mode csv\n"
            "PRAGMA diagnose_decide='off';\n"
            f"{_UNBOUNDED_SQL};\nSELECT * FROM decide_diagnostics();\n"
        )
        result = cli.execute_script(script)
        # Static error, no pointer, and the relation stays empty.
        assert "select * from decide_diagnostics()" not in result.stderr.lower()
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
            "PRAGMA diagnose_decide='auto';\n"
            f"{_UNBOUNDED_SQL};\n"  # stashes an unbounded diagnosis (and errors)
            f"{bounded_sql};\n"  # succeeds -> clears the stash
            "SELECT 'rows=' || count(*) AS diag FROM decide_diagnostics();\n"
        )
        result = cli.execute_script(script)
        assert "rows=0" in result.stdout
        assert "rows=1" not in result.stdout

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_repeated_failures_replace_rows_and_advance_diagnosis_id(
        self, request, cli_fixture
    ):
        """The stash holds only the latest failure, while ids advance per diagnosis."""
        cli = request.getfixturevalue(cli_fixture)
        y_sql = (
            "SELECT id, y FROM (VALUES (1), (2)) t(id) "
            "DECIDE y IS REAL SUCH THAT y >= 0 MAXIMIZE SUM(y)"
        )
        marker_sql = (
            "SELECT '{label}=' || min(diagnosis_id) || ':' || "
            "count(DISTINCT diagnosis_id) || ':' || count(*) AS marker "
            "FROM decide_diagnostics();\n"
        )
        script = (
            ".mode csv\n"
            "PRAGMA diagnose_decide='auto';\n"
            f"{_UNBOUNDED_SQL};\n"
            f"{marker_sql.format(label='first')}"
            f"{y_sql};\n"
            f"{marker_sql.format(label='second')}"
        )
        result = cli.execute_script(script)

        first_id, first_distinct, first_rows = _diagnosis_marker(
            result.stdout, "first"
        )
        second_id, second_distinct, second_rows = _diagnosis_marker(
            result.stdout, "second"
        )
        assert (first_distinct, first_rows) == (1, 2)
        assert (second_distinct, second_rows) == (1, 2)
        assert second_id == first_id + 1

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_qp_unbounded_no_content_free_diagnosis(self, request, cli_fixture):
        """A5: an unbounded solve that names no variable must not stash a content-free
        all-NULL row. A quadratic objective attaches no ray, so under `auto` the
        diagnosis has no per-variable content and falls through to the rich static
        error (Gurobi reaches UNBOUNDED here); HiGHS rejects the non-convex QP
        pre-solve. Either path: no diagnosis pointer and an empty relation.

        C8: when diagnosis was requested but cannot produce content, the error states
        it is unavailable and why (quadratic) instead of re-advertising the opt-in the
        user already enabled."""
        cli = request.getfixturevalue(cli_fixture)
        qp_sql = (
            "SELECT id, x FROM (VALUES (1), (2)) t(id) "
            "DECIDE x IS REAL SUCH THAT x >= 0 MAXIMIZE SUM(POWER(x, 2))"
        )
        result = _diagnose(cli, qp_sql, mode="auto")
        assert "select * from decide_diagnostics()" not in result.stderr.lower()
        assert _rows(result) == []
        if "gurobi" in cli_fixture:
            # Gurobi reports UNBOUNDED, exercising the C8 "diagnosis unavailable"
            # message (the old code replaced this with an all-NULL diagnosis row,
            # then with the generic re-run advert).
            err = result.stderr.lower()
            assert "add an upper bound" in err
            assert "non-linear" in err
            # The misleading opt-in advert must NOT appear: the mode is already on.
            assert "set pragma diagnose_decide='auto' and re-run" not in err

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_contradictory_bounds_reach_infeasible_gate(self, request, cli_fixture):
        """B5: pre-solve contradictory bounds surface as INFEASIBLE status instead
        of bypassing the diagnose gate with a build-time throw. Here both bounds are
        absorbed (and contradict) on a multi-instance variable, so the build-time
        contradiction fast-path leaves no relaxable rows for the elastic engine — it
        returns no diagnosis and the static infeasible error stands."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x FROM (VALUES (1), (2)) t(id) "
            "DECIDE x IS REAL SUCH THAT x >= 5 AND x <= 1 MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql, mode="auto")
        assert "decide optimization is infeasible" in result.stderr.lower()
        assert "select * from decide_diagnostics()" not in result.stderr.lower()
        assert _rows(result) == []

    # I1: the elastic engine turns an infeasible solve into the least-change fix.
    # These cases have a UNIQUE minimizer (the `2*x` coefficient breaks the L1 tie),
    # so the reported edit is deterministic across both backends.

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_infeasible_absorbed_bound_is_loosened(self, request, cli_fixture):
        """Decision 1a end-to-end: a user `x <= 5` is absorbed into the column bound
        (no row, no provenance), yet the engine must still loosen it. The conflict is
        with the row `2*x >= 30` (x >= 15); loosening the cap to 15 is the unique fix.
        Proves the operator re-emits the absorbed bound as a slackable row."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT x FROM (VALUES (1)) t(id) "
            "DECIDE x IS REAL SUCH THAT x <= 5 AND 2 * x >= 30 MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql, mode="auto")

        assert "select * from decide_diagnostics()" in result.stderr.lower()
        rows = _rows(result)
        assert {r["state"] for r in rows} == {"infeasible"}
        attrs = _attrs(rows, "clause", "x <= 5")
        assert attrs["suggested_change"] == "x <= 15"
        assert attrs["amount"] == "10"

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_infeasible_between_bound_is_loosened(self, request, cli_fixture):
        """BETWEEN is now tracked for re-emission too (it was absorbed but untracked).
        `x BETWEEN 0 AND 5` against `2*x >= 30` loosens the upper side to 15; the
        lower side (0) takes no slack, so only the cap edit is reported."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT x FROM (VALUES (1)) t(id) "
            "DECIDE x IS REAL SUCH THAT x BETWEEN 0 AND 5 AND 2 * x >= 30 MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql, mode="auto")

        rows = _rows(result)
        assert {r["state"] for r in rows} == {"infeasible"}
        attrs = _attrs(rows, "clause", "x <= 5")
        assert attrs["suggested_change"] == "x <= 15"
        assert attrs["amount"] == "10"

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_infeasible_diagnosis_suppressed_when_off(self, request, cli_fixture):
        """Under `off`, the same infeasible query reproduces the plain static error:
        no diagnosis pointer and an empty relation."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT x FROM (VALUES (1)) t(id) "
            "DECIDE x IS REAL SUCH THAT x <= 5 AND 2 * x >= 30 MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql, mode="off")
        assert "select * from decide_diagnostics()" not in result.stderr.lower()
        assert _rows(result) == []
