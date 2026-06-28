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
    def test_contradictory_absorbed_bounds_are_diagnosed(self, request, cli_fixture):
        """Bug 1: two contradictory USER bounds both absorbed into the column box
        (`x >= 5 AND x <= 1`) invert it (col_lower > col_upper). The build no longer
        throws under diagnosis; the elastic engine resets the box to the intrinsic
        domain, re-emits both bounds as slackable rows, and reports a least-change
        loosen. Neither bound conflicts with the type domain (both >= 0), so this is a
        user-vs-user conflict, not the Part C static error. The L1 minimizer ties
        between the two bounds (each loosens by 4), so assert on whichever the backend
        picks rather than a fixed direction."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x FROM (VALUES (1), (2)) t(id) "
            "DECIDE x IS REAL SUCH THAT x >= 5 AND x <= 1 MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql, mode="auto")
        assert "select * from decide_diagnostics()" in result.stderr.lower()
        rows = _rows(result)
        assert {r["state"] for r in rows} == {"infeasible"}
        edits = {
            r["subject"]: r["value"]
            for r in rows
            if r["attribute"] == "suggested_change"
        }
        # Exactly one of the two contradictory bounds is loosened to meet the other.
        assert edits in ({"x >= 5": "x >= 1"}, {"x <= 1": "x <= 5"}), edits

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
    def test_infeasible_easy_max_shares_one_slack_max_overshoot(self, request, cli_fixture):
        """I2.a: `MAX(x) <= 5` is absorbed as a per-instance 0/1-style cap that shares
        ONE knob across rows. Floors force x@row1 >= 8 (overshoot 3) and x@row2 >= 12
        (overshoot 7). The cap is one shared slack, so the minimal total loosening is
        the MAX overshoot (7) — not the per-row sum (10) independent slacks would force.
        (The split across clauses is degenerate — a competing relaxable floor also
        resolves it — so I2 pins the total; I3 picks the objective-best edit.)"""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x FROM (VALUES (1, 8), (2, 12)) t(id, lo) "
            "DECIDE x IS REAL SUCH THAT x >= lo AND MAX(x) <= 5 MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql, mode="auto")

        rows = _rows(result)
        assert {r["state"] for r in rows} == {"infeasible"}
        amounts = [float(r["value"]) for r in rows if r["attribute"] == "amount"]
        assert sum(amounts) == pytest.approx(7.0)
        assert _attrs(rows, "clause", "x <= 5")

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_infeasible_multi_instance_bound_shares_one_slack(self, request, cli_fixture):
        """I2.a: the absorbed bound `x <= 5` on a multi-instance variable (2 rows) is
        re-emitted as one row per instance under a single knob → ONE shared slack.
        Floors force overshoots 3 and 7, so the minimal total loosening is the MAX
        (7), the shared saving vs the sum (10). The default non-negativity and any
        BOOLEAN 0/1 domain are NOT re-emitted (they stay rigid)."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x FROM (VALUES (1, 8), (2, 12)) t(id, lo) "
            "DECIDE x IS REAL SUCH THAT x <= 5 AND x >= lo MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql, mode="auto")

        rows = _rows(result)
        assert {r["state"] for r in rows} == {"infeasible"}
        amounts = [float(r["value"]) for r in rows if r["attribute"] == "amount"]
        assert sum(amounts) == pytest.approx(7.0)
        assert _attrs(rows, "clause", "x <= 5")

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_infeasible_per_group_reports_one_edit_per_group(self, request, cli_fixture):
        """I2.b: an aggregate `SUM(x) >= 5 PER grp` emits one row per group, so each
        group gets its own slack — a per-group edit, not one global edit. Group 'a'
        (2 BOOLEAN x, max 2) loosens to >= 2 (amount 3); group 'b' (3 x, max 3) to
        >= 3 (amount 2). BOOLEAN keeps the domain rigid, so there is no floor
        competition: the per-group amounts are exact."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x FROM (VALUES (1,'a'),(2,'a'),(3,'b'),(4,'b'),(5,'b')) t(id, grp) "
            "DECIDE x IS BOOLEAN SUCH THAT SUM(x) >= 5 PER grp MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql, mode="auto")

        rows = _rows(result)
        assert {r["state"] for r in rows} == {"infeasible"}
        # One edit per group, each loosening its own SUM row to that group's max.
        group_a = _attrs(rows, "clause", "x + x >= 5")
        group_b = _attrs(rows, "clause", "x + x + x >= 5")
        assert group_a["suggested_change"] == "x + x >= 2"
        assert group_a["amount"] == "3"
        assert group_b["suggested_change"] == "x + x + x >= 3"
        assert group_b["amount"] == "2"

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_infeasible_boolean_loosens_constraint_not_domain(self, request, cli_fixture):
        """Regression guard: a BOOLEAN variable's 0/1 domain must never be loosened.
        `SUM(x) >= 3` over two BOOLEAN x (max achievable 2) is infeasible; the only
        honest fix is loosening the SUM target, NOT widening the 0/1 box to `x <= 2`."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x FROM (VALUES (1), (2)) t(id) "
            "DECIDE x IS BOOLEAN SUCH THAT SUM(x) >= 3 MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql, mode="auto")

        rows = _rows(result)
        assert {r["state"] for r in rows} == {"infeasible"}
        # The edit targets the SUM clause, never the 0/1 domain.
        subjects = {r["subject"] for r in rows if r["attribute"] == "suggested_change"}
        assert subjects, "expected at least one suggested_change edit"
        assert all("<= 1" not in s and "<= 2" not in s for s in subjects), subjects

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_infeasible_data_rhs_reports_conflict_summary(self, request, cli_fixture):
        """I2.c: when the RHS is per-row data (`x >= hi`), there is no single literal to
        loosen, so the clause reports ONE conflict summary (M of N rows) — not a
        `suggested_change`/`amount`. A rigid BOOLEAN 0/1 domain forces the data row to
        be the only thing that could move, so the conflict is deterministic (every row)."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x FROM (VALUES (1,5),(2,5)) t(id, hi) "
            "DECIDE x IS BOOLEAN SUCH THAT x >= hi MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql, mode="auto")

        rows = _rows(result)
        assert {r["state"] for r in rows} == {"infeasible"}
        floor = _attrs(rows, "clause", "x >= 5")
        assert floor["conflict"] == "conflicts in 2 of 2 rows"
        # A data-RHS clause never gets a scalar loosening suggestion.
        assert "suggested_change" not in floor and "amount" not in floor

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_infeasible_data_rhs_penalized_prefers_editable_edit(self, request, cli_fixture):
        """I2.c: when an editable knob (`x <= 5`) AND a data floor (`x >= lo`) both
        conflict, the data slack is penalized, so the solver loosens the editable cap
        and leaves the data row alone — a clean single edit, no conflict row."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x FROM (VALUES (1,12)) t(id, lo) "
            "DECIDE x IS REAL SUCH THAT x <= 5 AND x >= lo MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql, mode="auto")

        rows = _rows(result)
        cap = _attrs(rows, "clause", "x <= 5")
        assert cap["suggested_change"] == "x <= 12"
        assert cap["amount"] == "7"
        assert not [r for r in rows if r["attribute"] == "conflict"]

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_infeasible_foldable_rhs_cap_is_editable(self, request, cli_fixture):
        """Bug 3: a foldable RHS (`x <= 2 + 3`) is a single scalar shared by every row,
        so it is an editable cap — not per-row data. It is also copied into the rigid
        column box by implied-bound propagation; the elastic engine resets the box to
        the intrinsic domain so the (slackable) row governs. Against a data floor
        (`x >= lo` = 10) the cap loosens 5 → 10, exactly like a literal `x <= 5` would —
        no data conflict summary."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x FROM (VALUES (1,10)) t(id, lo) "
            "DECIDE x IS REAL SUCH THAT x <= 2 + 3 AND x >= lo MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql, mode="auto")

        rows = _rows(result)
        assert {r["state"] for r in rows} == {"infeasible"}
        cap = _attrs(rows, "clause", "x <= 5")
        assert cap["suggested_change"] == "x <= 10"
        assert cap["amount"] == "5"
        assert not [r for r in rows if r["attribute"] == "conflict"]

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_infeasible_single_absorbed_bound_vs_row(self, request, cli_fixture):
        """Bug 1: a single absorbed cap (`x <= 4`) that conflicts with a matrix row
        (`2*x >= 30` → x >= 15) — the column box (implied + absorbed) no longer pins the
        variable, so the cap loosens 4 → 15 (amount 11). The `2*x` coefficient breaks the
        L1 tie, so the edit is deterministic."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT x FROM (VALUES (1)) t(id) "
            "DECIDE x IS REAL SUCH THAT x <= 4 AND 2 * x >= 30 MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql, mode="auto")

        rows = _rows(result)
        assert {r["state"] for r in rows} == {"infeasible"}
        cap = _attrs(rows, "clause", "x <= 4")
        assert cap["suggested_change"] == "x <= 15"
        assert cap["amount"] == "11"

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_infeasible_avg_reports_raw_slack_and_avg_label(self, request, cli_fixture):
        """I2.d: an AVG constraint is stored pre-scaled by 1/N, so the engine renders
        the clause as `AVG(x)` (not `0.33*x + ...`) and reports the slack in the user's
        AVG units. A penalized data floor (`x >= lo`) forces AVG(x) up to 10, so the
        edit is `AVG(x) <= 5` → `AVG(x) <= 10` (raw amount 5)."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x FROM (VALUES (1,10),(2,10),(3,10)) t(id, lo) "
            "DECIDE x IS REAL SUCH THAT AVG(x) <= 5 AND x >= lo MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql, mode="auto")

        rows = _rows(result)
        avg = _attrs(rows, "clause", "AVG(x) <= 5")
        assert avg["suggested_change"] == "AVG(x) <= 10"
        assert avg["amount"] == "5"

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_infeasible_strict_less_than_requotes_typed_literal(self, request, cli_fixture):
        """I2.d: `x < 10` (integer) is built as `x <= 9` (δ baked in). The reported edit
        must re-quote against the user's typed `10` and render `<`, not the δ-adjusted
        `<=`. A penalized data floor forces the strict cap to loosen by 6 → `x < 16`."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x FROM (VALUES (1,15)) t(id, lo) "
            "DECIDE x IS INTEGER SUCH THAT SUM(x) < 10 AND x >= lo MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql, mode="auto")

        rows = _rows(result)
        edit = _attrs(rows, "clause", "x < 10")
        assert edit["suggested_change"] == "x < 16"
        assert edit["amount"] == "6"

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_infeasible_strict_greater_than_requotes_typed_literal(self, request, cli_fixture):
        """I2.d: the `>` mirror of the strict re-quote. `x > 10` (built as `x >= 11`)
        loosens downward against a penalized data cap → `x > 2`."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x FROM (VALUES (1,3)) t(id, hi) "
            "DECIDE x IS INTEGER SUCH THAT SUM(x) > 10 AND x <= hi MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql, mode="auto")

        rows = _rows(result)
        edit = _attrs(rows, "clause", "x > 10")
        assert edit["suggested_change"] == "x > 2"
        assert edit["amount"] == "8"

    @pytest.mark.parametrize("cli_fixture", ["decidb_cli_gurobi"])
    def test_infeasible_quadratic_loosens_linear_rhs(self, request, cli_fixture):
        """I2.d: a quadratic constraint (`POWER(x,2) <= 4`) gets a slack on its linear
        RHS only; loosening it (against a penalized data floor) re-quotes the clause as
        `POWER(x, 2) <= 100`. QCQP is Gurobi-only (HiGHS cannot solve it)."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x FROM (VALUES (1,10)) t(id, lo) "
            "DECIDE x IS REAL SUCH THAT POWER(x,2) <= 4 AND x >= lo MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql, mode="auto")

        rows = _rows(result)
        assert {r["state"] for r in rows} == {"infeasible"}
        edit = _attrs(rows, "clause", "POWER(x, 2) <= 4")
        assert edit["suggested_change"].startswith("POWER(x, 2) <= 100")
        # amount ≈ 96 (10² − 4), allowing for solver tolerance.
        assert abs(float(edit["amount"]) - 96.0) < 1e-3

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_infeasible_ne_rows_stay_rigid(self, request, cli_fixture):
        """I2.e: `<>` is remove-only — its indicator rows (USER_MECHANISM) are rigid and
        never slackened. The conflict `x <> 5 AND 5 <= x <= 5` is resolved by loosening
        an editable bound, leaving the `<>` untouched (no removal synthesized)."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x FROM (VALUES (1)) t(id) "
            "DECIDE x IS INTEGER SUCH THAT x <> 5 AND x >= 5 AND x <= 5 MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql, mode="auto")

        rows = _rows(result)
        assert {r["state"] for r in rows} == {"infeasible"}
        # The edit loosens a bound; no clause mentions the `<>` mechanism.
        subjects = {r["subject"] for r in rows if r["attribute"] == "suggested_change"}
        assert subjects, "expected an editable-bound edit"
        assert all("<>" not in s for s in subjects), subjects

    @pytest.mark.parametrize("cli_fixture", ["decidb_cli_gurobi"])
    def test_infeasible_mccormick_rows_stay_rigid(self, request, cli_fixture):
        """I2.e: McCormick bilinear-link rows (STRUCTURAL) are rigid. A bilinear conflict
        (`x*y <= 1 AND x >= 5 AND y >= 5`) is resolved by loosening a user bound; no slack
        attaches to the synthesized link rows. Bilinear is Gurobi-only here."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x, y FROM (VALUES (1)) t(id) "
            "DECIDE x IS REAL, y IS REAL SUCH THAT x * y <= 1 AND x >= 5 AND y >= 5 "
            "MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql, mode="auto")

        rows = _rows(result)
        assert {r["state"] for r in rows} == {"infeasible"}
        # The edit loosens a user bound (`x >= 5` / `y >= 5`), not an internal link row.
        subjects = {r["subject"] for r in rows if r["attribute"] == "suggested_change"}
        assert subjects, "expected an editable-bound edit"
        assert any(s in ("x >= 5", "y >= 5") for s in subjects), subjects

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
