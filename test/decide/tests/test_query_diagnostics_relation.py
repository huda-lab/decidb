"""The shared diagnostic reporting surface: the relation `DIAGNOSE <query>` returns.

`DIAGNOSE` is the only thing that starts the diagnostics engine. It runs the query and
returns its findings directly, one row per finding:

    state | clause | suggested_change | amount | total | scope | edit_source | group | row

A feasible query returns one row saying so; an infeasible or unbounded one returns the
findings and does NOT raise. The same query without the prefix reports its state and
stops.

These tests read the relation as CSV so its rows parse unambiguously, and most of them
read it through `_rows`, which re-expresses each finding as the (subject, attribute,
value) facts it carries — they are about *what* the engine finds, not about the column
layout. The layout itself is asserted directly in `TestDiagnoseRelationShape`. Runs under
both backends.
"""

import csv
import io
import re

import pytest

from . import _diagnose_relation
from ._diagnostic_invariants import assert_backends_agree, assert_edits_are_users_text


_BACKENDS = ["decidb_cli_highs", "decidb_cli_gurobi"]

_UNBOUNDED_SQL = (
    "SELECT id, x FROM (VALUES (1), (2)) t(id) "
    "DECIDE x(REAL) SUCH THAT x >= 0 MAXIMIZE SUM(x)"
)

_EXPECTED_SCHEMA = [
    "state",
    "clause",
    "suggested_change",
    "amount",
    "total",
    "scope",
    "edit_source",
    "group",
    "row",
]


def _diagnose(cli, decide_sql, scope=None):
    """Run `DIAGNOSE <decide_sql>` on one stdin session and return the result."""
    return _diagnose_relation.run(cli, decide_sql, scope=scope)


def _rows(result):
    """The findings, expressed as long-form (subject, attribute, value) facts."""
    return _diagnose_relation.eav_rows(result)


def _flat(result):
    """The findings as the relation actually returns them, one dict per row."""
    return _diagnose_relation.rows(result)


def _errors(result):
    """stderr lines that are real errors — DIAGNOSE reports findings, it does not raise."""
    return [
        line for line in result.stderr.strip().splitlines()
        if line and not line.startswith("Warning:")
    ]


def _attrs(rows, subject_kind, subject):
    return {
        r["attribute"]: r["value"]
        for r in rows
        if r["subject_kind"] == subject_kind and r["subject"] == subject
    }


def _resolve_objective(cli, sql, drop_sqls, columns):
    """Oracle for the achievable objective a drop-repair reports: rebuild the query with
    each clause in `drop_sqls` removed, re-solve it independently, and return the summed
    value of `columns` over the result rows. Compares diagnosis output to a real re-solve,
    never a hand-computed constant."""
    fixed_sql = sql
    for clause in drop_sqls:
        fixed_sql = (
            fixed_sql.replace(f"{clause} AND ", "")
            if f"{clause} AND " in fixed_sql
            else fixed_sql.replace(f" AND {clause}", "")
        )
    fixed = list(csv.DictReader(io.StringIO(
        cli.execute_script(".mode csv\n" + fixed_sql + ";\n").stdout)))
    return sum(sum(float(r[c]) for c in columns) for r in fixed)


def _clause_edits(rows):
    """Group the EAV clause rows into one dict per edit. Each edit's rows are emitted as
    a contiguous run starting at `edit_kind`, so a new `edit_kind` row opens a new edit.
    Lets two PER-group edits that share the same folded `subject` be told apart by their
    separate `group` attribute."""
    edits = []
    cur = None
    for r in rows:
        if r["subject_kind"] != "clause":
            continue
        if r["attribute"] == "edit_kind":
            cur = {"subject": r["subject"], "edit_kind": r["value"]}
            edits.append(cur)
        elif cur is not None:
            cur[r["attribute"]] = r["value"]
    return edits


def _apply_reported_fix(cli, sql, rows, subject_to_sql=None):
    """E1 apply-the-fix harness: apply every reported edit to the SQL (loosen —
    replace the clause with its suggested_change; drop — remove the clause) and
    assert the edited query actually solves. This checks the core promise that the
    least-change edit restores a usable solution by re-running the query, never by
    trusting the diagnosis. `subject_to_sql` maps a rendered clause subject to the
    literal SQL fragment when the two differ (BETWEEN sides, reversed bounds,
    MAX(x) composition, subquery/foldable RHS, whitespace). Returns the edited SQL
    so callers can oracle-check the achievable objective against it."""
    subject_to_sql = subject_to_sql or {}
    edits = _clause_edits(rows)
    # Every test that applies a fix also asserts the fix was the user's own text.
    # Checked before applying, so a fabricated clause is reported as a fabrication
    # rather than as "cannot locate <clause> in <sql>".
    assert_edits_are_users_text(sql, edits, "reported repair")
    fixed_sql = sql
    edited = False
    for edit in edits:
        clause = subject_to_sql.get(edit["subject"], edit["subject"])
        if edit["edit_kind"] == "drop":
            assert f"{clause} AND " in fixed_sql or f" AND {clause}" in fixed_sql, (
                f"cannot locate dropped clause {clause!r} in:\n{fixed_sql}"
            )
            fixed_sql = (
                fixed_sql.replace(f"{clause} AND ", "", 1)
                if f"{clause} AND " in fixed_sql
                else fixed_sql.replace(f" AND {clause}", "", 1)
            )
            edited = True
        elif "suggested_change" in edit:
            assert clause in fixed_sql, f"cannot locate {clause!r} in:\n{fixed_sql}"
            fixed_sql = fixed_sql.replace(clause, edit["suggested_change"], 1)
            edited = True
    assert edited, f"diagnosis reported no applicable edit:\n{rows}"
    result = cli.execute_script(".mode csv\n" + fixed_sql + ";\n")
    errors = [
        line for line in result.stderr.strip().splitlines()
        if line and not line.startswith("Warning:")
    ]
    assert not errors, (
        f"edited query still fails:\n{fixed_sql}\n" + "\n".join(errors)
    )
    assert list(csv.DictReader(io.StringIO(result.stdout))), (
        f"edited query returned no rows:\n{fixed_sql}"
    )
    return fixed_sql


def _diagnosis_marker(stdout, label):
    match = re.search(rf"{label}=(\d+):(\d+):(\d+)", stdout)
    assert match, f"Missing {label!r} diagnosis marker in stdout:\n{stdout}"
    return tuple(int(group) for group in match.groups())


@pytest.mark.query_diagnostics
class TestDiagnoseRelationShape:
    """The relation itself: its columns, its types, and the fact that it composes."""

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_relation_has_fixed_schema(self, request, cli_fixture):
        cli = request.getfixturevalue(cli_fixture)
        desc_rows, _ = cli.execute(f"DESCRIBE SELECT * FROM (DIAGNOSE {_UNBOUNDED_SQL})")
        names = [str(r[0]).lower() for r in desc_rows]
        assert names == _EXPECTED_SCHEMA
        types = [str(r[1]).upper() for r in desc_rows]
        # Real types, not the all-VARCHAR EAV shape this replaced: `amount` is a
        # number you can compare against, `total` and `row` integer identities/counts.
        assert types[3] == "DOUBLE"
        assert types[4] == "BIGINT"
        assert types[8] == "BIGINT"

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_unbounded_names_the_runaway_and_does_not_raise(self, request, cli_fixture):
        cli = request.getfixturevalue(cli_fixture)
        result = _diagnose(cli, _UNBOUNDED_SQL)

        # DIAGNOSE reports on the run; the failure is the answer, not an error.
        assert not _errors(result), result.stderr
        flat = _flat(result)
        assert len(flat) == 1
        assert flat[0]["state"] == "unbounded"
        assert flat[0]["clause"] == "x"
        assert flat[0]["suggested_change"] == "x <= <cap>"
        assert flat[0]["edit_source"] == "runaway_+inf"
        # Both instances of x escape, so the count covers the whole variable and no
        # categorical slice is named.
        assert float(flat[0]["amount"]) == 2
        assert int(flat[0]["total"]) == 2
        assert flat[0]["scope"] == "row"
        assert flat[0]["group"] in ("", "NULL")

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_a_feasible_query_returns_one_row_saying_so(self, request, cli_fixture):
        """No separate output path for a query that worked: one row, state = feasible."""
        cli = request.getfixturevalue(cli_fixture)
        bounded_sql = (
            "SELECT id, x FROM (VALUES (1), (2)) t(id) "
            "DECIDE x(REAL) SUCH THAT x >= 0 AND x <= 5 MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, bounded_sql)
        assert not _errors(result), result.stderr
        flat = _flat(result)
        assert len(flat) == 1
        assert flat[0]["state"] == "feasible"
        assert all(flat[0][c] in ("", "NULL") for c in _EXPECTED_SCHEMA[1:])

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_the_relation_composes(self, request, cli_fixture):
        """It is a relation, not a printed report: it can be selected from and filtered."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT x FROM (VALUES (1)) t(id) "
            "DECIDE x(REAL) SUCH THAT x <= 5 AND 2 * x >= 30 MAXIMIZE SUM(x)"
        )
        result = cli.execute_script(
            ".mode csv\n"
            f"SELECT clause, amount FROM (DIAGNOSE {sql}) "
            "WHERE clause IS NOT NULL AND amount > 1;\n"
        )
        rows = list(csv.DictReader(io.StringIO(result.stdout)))
        assert rows == [{"clause": "x <= 5", "amount": "10.0"}], result.stdout

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_a_bare_failure_reports_its_state_and_stops(self, request, cli_fixture):
        """Without the prefix: the state, and how to ask for more. No clause name, no
        repair, no second statement to run — naming the clause IS the elastic solve."""
        cli = request.getfixturevalue(cli_fixture)
        result = cli.execute_script(f".mode csv\n{_UNBOUNDED_SQL};\n")
        err = result.stderr.lower()
        assert "decide optimization is unbounded" in err
        assert "diagnose" in err
        # None of the diagnosis's own vocabulary leaks into the bare error.
        assert "x <=" not in err
        assert "decide_diagnostics" not in err

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_diagnosis_does_not_outlive_its_statement(self, request, cli_fixture):
        """The findings cross from the DECIDE operator to the DIAGNOSE operator inside
        one statement and no further: a later DIAGNOSE reports its own run, never the
        previous one's."""
        cli = request.getfixturevalue(cli_fixture)
        bounded_sql = (
            "SELECT id, x FROM (VALUES (1), (2)) t(id) "
            "DECIDE x(REAL) SUCH THAT x >= 0 AND x <= 5 MAXIMIZE SUM(x)"
        )
        result = cli.execute_script(
            ".mode csv\n"
            f"DIAGNOSE {_UNBOUNDED_SQL};\n"
            f"DIAGNOSE {bounded_sql};\n"
        )
        # Two relations back to back: the unbounded one, then a clean `feasible`.
        assert "unbounded" in result.stdout
        assert "feasible" in result.stdout

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_diagnose_needs_a_decide_clause(self, request, cli_fixture):
        cli = request.getfixturevalue(cli_fixture)
        result = cli.execute_script(".mode csv\nDIAGNOSE SELECT 1 AS a;\n")
        assert "decide clause" in result.stderr.lower()

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_qp_unbounded_says_why_it_cannot_name_a_variable(self, request, cli_fixture):
        """A5/C8: an unbounded solve that names no variable must not report a
        content-free row. A quadratic objective attaches no ray, so the diagnosis says
        plainly that it is unavailable and why (Gurobi reaches UNBOUNDED here; HiGHS
        rejects the non-convex QP in pre-solve)."""
        cli = request.getfixturevalue(cli_fixture)
        qp_sql = (
            "SELECT id, x FROM (VALUES (1), (2)) t(id) "
            "DECIDE x(REAL) SUCH THAT x >= 0 MAXIMIZE SUM(POWER(x, 2))"
        )
        result = _diagnose(cli, qp_sql)
        if "gurobi" in cli_fixture:
            flat = _flat(result)
            assert len(flat) == 1
            assert flat[0]["state"] == "unbounded"
            assert flat[0]["edit_source"] == "undiagnosed"
            assert "non-linear" in flat[0]["suggested_change"]
            assert flat[0]["clause"] in ("", "NULL")
        else:
            # HiGHS refuses the model class outright, before any solve.
            assert _errors(result)


@pytest.mark.query_diagnostics
class TestDiagnosticsRelation:
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
            "DECIDE x(REAL) SUCH THAT x >= 5 AND x <= 1 MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql)
        assert not _errors(result), result.stderr
        rows = _rows(result)
        assert {r["state"] for r in rows} == {"infeasible"}
        edits = {
            r["subject"]: r["value"]
            for r in rows
            if r["attribute"] == "suggested_change"
        }
        # Exactly one of the two contradictory bounds is loosened to meet the other.
        assert edits in ({"x >= 5": "x >= 1"}, {"x <= 1": "x <= 5"}), edits
        _apply_reported_fix(cli, sql, rows)

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
            "DECIDE x(REAL) SUCH THAT x <= 5 AND 2 * x >= 30 MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql)

        assert not _errors(result), result.stderr
        rows = _rows(result)
        assert {r["state"] for r in rows} == {"infeasible"}
        attrs = _attrs(rows, "clause", "x <= 5")
        assert attrs["suggested_change"] == "x <= 15"
        assert attrs["amount"] == "10"
        _apply_reported_fix(cli, sql, rows)

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_infeasible_entity_scoped_label_drops_accumulated_coefficient(
        self, request, cli_fixture
    ):
        """An entity-scoped variable maps every joined row of an entity onto ONE solver
        column, so the builder accumulates rather than fanning out. Here each region owns
        exactly 5 nation rows, so `SUM(keepR)` reaches the matrix as `5*keepR` — one index,
        coefficient 5, no fan-out for the reconstruction to fold. The label must still read
        as written (`SUM(keepR)`, never `SUM(5*keepR)`), or the suggested edit is not
        pasteable and leaks the entity/row-scope distinction the user never asked about."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT r_name, keepR "
            "FROM nation n JOIN region r ON n.n_regionkey = r.r_regionkey "
            "DECIDE r.keepR(BOOL) "
            "SUCH THAT SUM(keepR) >= 6 PER r.r_name "
            "MAXIMIZE SUM(keepR)"
        )
        result = _diagnose(cli, sql)

        rows = _rows(result)
        assert {r["state"] for r in rows} == {"infeasible"}
        subject = "SUM(keepR) >= 6 PER r_name"
        assert any(r["subject"] == subject for r in rows), (
            "clause label must quote the written coefficient, not the accumulated one:\n"
            + "\n".join(sorted({r["subject"] for r in rows if r["subject_kind"] == "clause"}))
        )
        attrs = _attrs(rows, "clause", subject)
        assert attrs["suggested_change"] == "SUM(keepR) >= 5 PER r_name"
        _apply_reported_fix(cli, sql, rows, {subject: "SUM(keepR) >= 6 PER r.r_name"})

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_infeasible_entity_scoped_label_survives_mixed_multiplicity(
        self, request, cli_fixture
    ):
        """Same fold, harder shape: one PER group holding several entities with *different*
        join multiplicities (nations per region own 50-72 customers each), so the accumulated
        coefficients differ across columns of the same variable. The reconstruction reads
        that spread as a data-varying weight and renders the coefficient's source text —
        `SUM(keepN * 1)`. Guards the branch a coefficient-of-1 gate would miss, since no
        column here carries the user's written 1."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT n_name, keepN "
            "FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey "
            "JOIN region r ON n.n_regionkey = r.r_regionkey "
            "DECIDE n.keepN(BOOL) "
            "SUCH THAT SUM(keepN) >= 1000 PER r.r_name "
            "MAXIMIZE SUM(keepN)"
        )
        result = _diagnose(cli, sql)

        rows = _rows(result)
        assert {r["state"] for r in rows} == {"infeasible"}
        subject = "SUM(keepN) >= 1000 PER r_name"
        assert any(r["subject"] == subject for r in rows), (
            "mixed-multiplicity group must not render as a data-varying weight:\n"
            + "\n".join(sorted({r["subject"] for r in rows if r["subject_kind"] == "clause"}))
        )
        _apply_reported_fix(cli, sql, rows, {subject: "SUM(keepN) >= 1000 PER r.r_name"})

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_infeasible_row_scoped_label_unaffected_by_fold_rendering(
        self, request, cli_fixture
    ):
        """Guard on the two tests above: a row-scoped variable genuinely fans out over its
        rows, so it never takes the accumulating build path and must keep rendering through
        the existing fan-out fold. Pins that the entity-scoped fix did not reroute the
        common case."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, grp, buy FROM (VALUES (1, 'a'), (2, 'a'), (3, 'b')) t(id, grp) "
            "DECIDE buy(BOOL) "
            "SUCH THAT SUM(buy) >= 5 PER grp "
            "MAXIMIZE SUM(buy)"
        )
        result = _diagnose(cli, sql)

        rows = _rows(result)
        assert {r["state"] for r in rows} == {"infeasible"}
        attrs = _attrs(rows, "clause", "SUM(buy) >= 5 PER grp")
        assert attrs["suggested_change"] == "SUM(buy) >= 1 PER grp"

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_infeasible_stage2_picks_objective_best_edit(self, request, cli_fixture):
        """I3: two editable caps `x <= 0` and `y <= 0` tie with the floor `x + y >= 10`
        on total loosening (S* = 10), so the minimal fix is non-unique. Stage 2 freezes
        the budget and maximizes the user's objective, so it must loosen x's cap (not y's)
        and report the achievable objective. The reported objective is differential-checked
        against an independent re-solve of the fixed query — nothing is hand-computed."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT x, y FROM (VALUES (1)) t(id) "
            "DECIDE x(REAL), y(REAL) "
            "SUCH THAT x <= 0 AND y <= 0 AND x + y >= 10 MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql)

        rows = _rows(result)
        assert {r["state"] for r in rows} == {"infeasible"}
        # Stage 2 loosens x's cap (raises the objective), never y's.
        x_cap = _attrs(rows, "clause", "x <= 0")
        assert x_cap["suggested_change"] == "x <= 10"
        # I5: every edit row carries a uniform edit_kind so the relation self-describes.
        assert x_cap["edit_kind"] == "loosen"
        assert "suggested_change" not in _attrs(rows, "clause", "y <= 0")
        # The achievable objective is surfaced as a model-level fact (NULL subject).
        reported = _attrs(rows, "model", "NULL")["achievable_objective"]

        # Differential: apply the reported edit, re-solve the now-feasible query, and
        # confirm its objective matches what the diagnosis promised.
        fixed_sql = sql.replace("x <= 0", x_cap["suggested_change"])
        fixed = list(csv.DictReader(io.StringIO(
            cli.execute_script(".mode csv\n" + fixed_sql + ";\n").stdout)))
        objective = sum(float(r["x"]) for r in fixed)
        assert float(reported) == pytest.approx(objective)

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_infeasible_no_objective_loosen_tie_is_solver_agnostic(
        self, request, cli_fixture
    ):
        """A5 tie-break on loosen edits — the same cap/floor tie as the stage-2 test but
        WITHOUT an objective, so stage 2 cannot arbitrate: any split of the 10 units of
        loosening across `x <= 0`, `y <= 0`, and `x + y >= 10` is an equally-minimal
        repair, and each backend used to name a different clause (Gurobi loosened
        `x <= 0`, HiGHS the floor; an LP tie can even split one repair fractionally
        across two clauses). The rank-weighted tie-break re-solves under the frozen tier
        budgets and concentrates the repair on the lowest-ranked clause — ranks follow
        slack emission order: matrix rows in declaration order first, then re-emitted
        absorbed bounds — so both backends report the single floor edit. Asserting the
        SAME clause on both backends is the solver-agnosticism guarantee."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT x, y FROM (VALUES (1)) t(id) "
            "DECIDE x(REAL), y(REAL) SUCH THAT x <= 0 AND y <= 0 AND x + y >= 10"
        )
        result = _diagnose(cli, sql)

        rows = _rows(result)
        assert {r["state"] for r in rows} == {"infeasible"}
        edits = _clause_edits(rows)
        # One clause carries the whole repair — never a fractional split across two.
        assert len(edits) == 1, rows
        assert edits[0]["subject"] == "x + y >= 10", rows  # same clause on both backends
        assert edits[0]["suggested_change"] == "x + y >= 0"
        _apply_reported_fix(cli, sql, rows)

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_infeasible_between_bound_is_loosened(self, request, cli_fixture):
        """BETWEEN is now tracked for re-emission too (it was absorbed but untracked).
        `x BETWEEN 0 AND 5` against `2*x >= 30` loosens the upper side to 15; the
        lower side (0) takes no slack, so only the cap edit is reported."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT x FROM (VALUES (1)) t(id) "
            "DECIDE x(REAL) SUCH THAT x BETWEEN 0 AND 5 AND 2 * x >= 30 MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql)

        rows = _rows(result)
        assert {r["state"] for r in rows} == {"infeasible"}
        attrs = _attrs(rows, "clause", "x <= 5")
        assert attrs["suggested_change"] == "x <= 15"
        assert attrs["amount"] == "10"
        _apply_reported_fix(cli, sql, rows, {"x <= 5": "x BETWEEN 0 AND 5"})

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
            "DECIDE x(REAL) SUCH THAT x >= lo AND MAX(x) <= 5 MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql)

        rows = _rows(result)
        assert {r["state"] for r in rows} == {"infeasible"}
        amounts = [float(r["value"]) for r in rows if r["attribute"] == "amount"]
        assert sum(amounts) == pytest.approx(7.0)
        assert _attrs(rows, "clause", "MAX(x) <= 5")
        _apply_reported_fix(cli, sql, rows)

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
            "DECIDE x(REAL) SUCH THAT x <= 5 AND x >= lo MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql)

        rows = _rows(result)
        assert {r["state"] for r in rows} == {"infeasible"}
        amounts = [float(r["value"]) for r in rows if r["attribute"] == "amount"]
        assert sum(amounts) == pytest.approx(7.0)
        assert _attrs(rows, "clause", "x <= 5")
        _apply_reported_fix(cli, sql, rows)

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_infeasible_reversed_bound_is_absorbed_and_reported(self, request, cli_fixture):
        """A reversed simple bound (`5 >= x`) normalizes to `x <= 5` before bound
        absorption, so the elastic engine still re-emits it as an editable user
        bound instead of losing it behind the rigid column box."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x FROM (VALUES (1,12)) t(id, lo) "
            "DECIDE x(REAL) SUCH THAT 5 >= x AND x >= lo MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql)

        rows = _rows(result)
        assert {r["state"] for r in rows} == {"infeasible"}
        cap = _attrs(rows, "clause", "x <= 5")
        assert cap["suggested_change"] == "x <= 12"
        assert cap["amount"] == "7"
        _apply_reported_fix(cli, sql, rows, {"x <= 5": "5 >= x"})

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_infeasible_explicit_data_cast_keeps_source_spelling(self, request, cli_fixture):
        """Diagnostics use source provenance for a data expression, including an
        explicit cast that must not be replaced by a flattened/internal name."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x FROM (VALUES (1,-1)) t(id, cap) "
            "DECIDE x(REAL) SUCH THAT x <= CAST(cap AS DOUBLE) MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql)

        rows = _rows(result)
        clause = "x <= CAST(cap AS DOUBLE)"
        edit = _attrs(rows, "clause", clause)
        assert edit["edit_kind"] == "loosen"
        assert edit["suggested_change"] == f"{clause} + 1"
        assert "__source_" not in result.stdout

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_infeasible_when_per_source_qualifier_order(self, request, cli_fixture):
        """A source label retains both wrappers in DECIDE grammar order."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x FROM (VALUES (1,'a',true),(2,'a',false)) t(id, grp, active) "
            "DECIDE x(BOOL) SUCH THAT SUM(x) >= 5 WHEN active PER grp MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql)

        rows = _rows(result)
        clause = "SUM(x) >= 5 WHEN active PER grp"
        edit = _attrs(rows, "clause", clause)
        assert edit["edit_kind"] == "loosen"
        assert "PER grp WHEN" not in result.stdout

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_infeasible_per_group_query_mode_folds_to_one_edit(self, request, cli_fixture):
        """T3 query mode (default): a PER aggregate `SUM(x) >= 5 PER grp` is ONE SQL literal
        the user edits, so all groups fold into a single clause-level edit — the shared
        slack is the max overshoot across groups. Group 'a' (2 BOOLEAN x, max 2) needs 3;
        group 'b' (3 x, max 3) needs 2; the fold reports the max, `SUM(x) >= 2 PER grp`
        (amount 3), with no per-group breakdown in the relation."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x FROM (VALUES (1,'a'),(2,'a'),(3,'b'),(4,'b'),(5,'b')) t(id, grp) "
            "DECIDE x(BOOL) SUCH THAT SUM(x) >= 5 PER grp MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql)
        err = result.stderr.lower()

        rows = _rows(result)
        assert {r["state"] for r in rows} == {"infeasible"}
        edits = _clause_edits(rows)
        assert len(edits) == 1
        edit = edits[0]
        assert edit["subject"] == "SUM(x) >= 5 PER grp"
        assert edit["suggested_change"] == "SUM(x) >= 2 PER grp"
        assert edit["amount"] == "3"
        assert edit["edit_source"] == "source_literal"
        assert edit["offset_scope"] == "clause"
        # No per-group `group` row in query mode (the clause is one folded edit).
        assert not [r for r in rows if r["attribute"] == "group"]
        _apply_reported_fix(cli, sql, rows)

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_infeasible_per_group_expanded_mode_reports_per_group(self, request, cli_fixture):
        """T3 expanded mode: the same PER aggregate breaks out per group — group 'a' loosens
        to >= 2 (amount 3), group 'b' to >= 3 (amount 2) — each tagged expanded_group/group
        with a `group` key. This is the per-group profile the query fold summarizes."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x FROM (VALUES (1,'a'),(2,'a'),(3,'b'),(4,'b'),(5,'b')) t(id, grp) "
            "DECIDE x(BOOL) SUCH THAT SUM(x) >= 5 PER grp MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql, scope="expanded")

        rows = _rows(result)
        assert {r["state"] for r in rows} == {"infeasible"}
        edits = _clause_edits(rows)
        # The clause reads as written; the group is its own column, not a suffix on the
        # label the user has to parse back out.
        assert [e["subject"] for e in edits] == [
            "SUM(x) >= 5 PER grp",
            "SUM(x) >= 5 PER grp",
        ]
        by_group = {e["group"]: e for e in edits}
        assert set(by_group) == {"a", "b"}
        assert by_group["a"]["suggested_change"] == "SUM(x) >= 2 PER grp"
        assert by_group["a"]["amount"] == "3"
        assert by_group["a"]["edit_source"] == "expanded_group"
        assert by_group["a"]["offset_scope"] == "group"
        assert by_group["b"]["suggested_change"] == "SUM(x) >= 3 PER grp"
        assert by_group["b"]["amount"] == "2"

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_infeasible_per_group_expanded_mode_preserves_empty_string_group(
        self, request, cli_fixture
    ):
        """B4: an empty-string PER key is a real group value, not the ungrouped case."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x FROM (VALUES (1,''),(2,'a')) t(id, grp) "
            "DECIDE x(BOOL) SUCH THAT SUM(x) >= 2 PER grp MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql, scope="expanded")

        rows = _rows(result)
        edits = _clause_edits(rows)
        by_group = {e["group"]: e for e in edits}
        assert set(by_group) == {"''", "a"}
        assert by_group["''"]["subject"] == "SUM(x) >= 2 PER grp"
        assert by_group["''"]["edit_source"] == "expanded_group"
        assert by_group["''"]["offset_scope"] == "group"
        assert by_group["''"]["suggested_change"] == "SUM(x) >= 1 PER grp"

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_infeasible_single_row_when_group_keeps_sum_wrapper(self, request, cli_fixture):
        """Facet B/C: a single-row aggregate group keeps its `SUM(...)` wrapper and the
        WHEN qualifier in the label. `SUM(x) >= 99 WHEN grp='a'` matches one BOOLEAN row
        (max 1), so it is infeasible and must render `SUM(x) >= 99 WHEN grp = 'a'` — not a
        bare `x >= 99` — even though there is no per-row fan-out to fold. The WHEN
        predicate is rendered cleanly (no implicit CAST / extra parens)."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x FROM (VALUES (1,'a'),(2,'b')) t(id, grp) "
            "DECIDE x(BOOL) SUCH THAT SUM(x) >= 99 WHEN grp='a' MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql)

        rows = _rows(result)
        assert {r["state"] for r in rows} == {"infeasible"}
        edit = _attrs(rows, "clause", "SUM(x) >= 99 WHEN grp = 'a'")
        assert edit["edit_kind"] == "loosen"
        assert edit["suggested_change"] == "SUM(x) >= 1 WHEN grp = 'a'"
        _apply_reported_fix(
            cli, sql, rows, {"SUM(x) >= 99 WHEN grp = 'a'": "SUM(x) >= 99 WHEN grp='a'"}
        )

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_infeasible_boolean_loosens_constraint_not_domain(self, request, cli_fixture):
        """Regression guard: a BOOLEAN variable's 0/1 domain must never be loosened.
        `SUM(x) >= 3` over two BOOLEAN x (max achievable 2) is infeasible; the only
        honest fix is loosening the SUM target, NOT widening the 0/1 box to `x <= 2`."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x FROM (VALUES (1), (2)) t(id) "
            "DECIDE x(BOOL) SUCH THAT SUM(x) >= 3 MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql)

        rows = _rows(result)
        assert {r["state"] for r in rows} == {"infeasible"}
        # The edit targets the SUM clause, never the 0/1 domain.
        subjects = {r["subject"] for r in rows if r["attribute"] == "suggested_change"}
        assert subjects, "expected at least one suggested_change edit"
        assert all("<= 1" not in s and "<= 2" not in s for s in subjects), subjects
        _apply_reported_fix(cli, sql, rows)

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_infeasible_boolean_pin_floor_is_diagnosed(self, request, cli_fixture):
        """A1 (silent-failure repro): a genuine user pin on a BOOLEAN (`x >= 1`) used
        to be erased from the elastic model — absorbed into the 0/1 box and never
        re-emitted — so this trivially diagnosable conflict fell through to the bare
        static error with an empty relation. Pinning all 3 x to 1 conflicts with
        `SUM(x) <= 2`; the engine must produce a diagnosis whose edit names one of
        the two user clauses, and applying it must make the query solve."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x FROM (VALUES (1), (2), (3)) t(id) "
            "DECIDE x(BOOL) SUCH THAT x >= 1 AND SUM(x) <= 2 MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql)

        assert not _errors(result), result.stderr
        rows = _rows(result)
        assert {r["state"] for r in rows} == {"infeasible"}
        subjects = {r["subject"] for r in rows if r["attribute"] == "suggested_change"}
        assert subjects and subjects <= {"x >= 1", "SUM(x) <= 2"}, subjects
        _apply_reported_fix(cli, sql, rows)

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_infeasible_boolean_pin_cap_edit_restores_feasibility(self, request, cli_fixture):
        """A1 (actively-wrong-edit repro): with the BOOLEAN pin `x <= 0` erased, the
        elastic model reached `SUM(x) + SUM(y) >= 9` by silently setting the erased x
        to 1 and reported `y <= 1` -> `y <= 2` — an edit that leaves the real query
        INFEASIBLE. With the pin re-emitted, whatever edit set is reported must
        restore feasibility, and the reported achievable objective must match an
        independent re-solve of the edited query."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x, y FROM (VALUES (1), (2), (3)) t(id) "
            "DECIDE x(BOOL), y(INT) SUCH THAT x <= 0 AND y <= 1 "
            "AND SUM(y) >= 5 AND SUM(x) + SUM(y) >= 9 MAXIMIZE SUM(y)"
        )
        result = _diagnose(cli, sql)

        assert not _errors(result), result.stderr
        rows = _rows(result)
        assert {r["state"] for r in rows} == {"infeasible"}
        fixed_sql = _apply_reported_fix(cli, sql, rows)
        # Oracle: the promised objective is achievable on the edited query.
        reported = _attrs(rows, "model", "NULL")["achievable_objective"]
        fixed = list(csv.DictReader(io.StringIO(
            cli.execute_script(".mode csv\n" + fixed_sql + ";\n").stdout)))
        assert float(reported) == pytest.approx(sum(float(r["y"]) for r in fixed))

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_infeasible_boolean_equality_pin_is_diagnosed(self, request, cli_fixture):
        """A1, `=` shape: an equality pin `x = 1` on a BOOLEAN is a genuine user
        constraint (not the domain), so it must reach the elastic model and yield a
        diagnosis whose edit restores feasibility."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x FROM (VALUES (1), (2), (3)) t(id) "
            "DECIDE x(BOOL) SUCH THAT x = 1 AND SUM(x) <= 2 MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql)

        assert not _errors(result), result.stderr
        rows = _rows(result)
        assert {r["state"] for r in rows} == {"infeasible"}
        _apply_reported_fix(cli, sql, rows)

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_infeasible_boolean_domain_restatement_stays_rigid(self, request, cli_fixture):
        """A1 guard: a user-written `x <= 1 AND x >= 0` on a BOOLEAN merely restates
        the intrinsic 0/1 domain and must NOT become an editable knob — widening the
        box (`x <= 2`) is never an honest fix for a BOOLEAN. The only edit loosens
        the SUM floor."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x FROM (VALUES (1), (2)) t(id) "
            "DECIDE x(BOOL) SUCH THAT x <= 1 AND x >= 0 AND SUM(x) >= 3 "
            "MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql)

        rows = _rows(result)
        assert {r["state"] for r in rows} == {"infeasible"}
        subjects = {r["subject"] for r in rows if r["attribute"] == "suggested_change"}
        assert subjects == {"SUM(x) >= 3"}, subjects
        _apply_reported_fix(cli, sql, rows)

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_infeasible_boolean_data_conflict_gets_real_diagnosis(self, request, cli_fixture):
        """DomainSpec regression guard (06_issues/code_quality: "BOOL domains round-trip
        through the constraint tree instead of being variable bounds" — fixed by applying
        a BOOLEAN's `[0,1]` domain directly to the solver column instead of synthesizing
        `x >= 0 AND x <= 1` rows). Two BOOLEAN variables, no bound anywhere near the 0/1
        domain: `SUM(x) >= 2` needs 2 of 3 rows' x=1, `SUM(x) + SUM(y) <= 1` caps their
        combined total at 1 — a genuine aggregate-vs-aggregate conflict. With the domain no
        longer represented as constraint rows, the elastic engine must still see and
        diagnose this real conflict (it must not lose coverage just because the synthetic
        bound rows are gone), and the reported edit must target one of the two written
        clauses."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x, y FROM (VALUES (1), (2), (3)) t(id) "
            "DECIDE x(BOOL), y(BOOL) SUCH THAT SUM(x) >= 2 "
            "AND SUM(x) + SUM(y) <= 1 MAXIMIZE SUM(x) + SUM(y)"
        )
        result = _diagnose(cli, sql)

        rows = _rows(result)
        assert {r["state"] for r in rows} == {"infeasible"}
        edits = _clause_edits(rows)
        subjects = {e["subject"] for e in edits}
        assert subjects, "expected at least one clause edit"
        assert subjects <= {"SUM(x) >= 2", "SUM(x) + SUM(y) <= 1"}, subjects
        _apply_reported_fix(cli, sql, rows)

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_infeasible_boolean_domain_never_offered_even_as_last_resort(self, request, cli_fixture):
        """DomainSpec regression guard, companion to the data-conflict test above and to
        `test_infeasible_boolean_loosens_constraint_not_domain`: construct the case so
        widening the 0/1 box is the ONLY numeric change that would make `SUM(x) >= 5` over
        3 BOOLEAN rows satisfiable in isolation (max achievable is 3) — checking that even
        under that pressure, no reported edit ever names a bare variable subject (`x`) or a
        suggested_change that widens past 1, since the domain was never emitted as a
        slackable row to begin with. The only honest edit loosens the SUM floor itself."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x FROM (VALUES (1), (2), (3)) t(id) "
            "DECIDE x(BOOL) SUCH THAT SUM(x) >= 5 MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql)

        rows = _rows(result)
        assert {r["state"] for r in rows} == {"infeasible"}
        edits = _clause_edits(rows)
        assert edits, "expected at least one clause edit"
        assert all(e["subject"] != "x" for e in edits), edits
        assert all(
            "suggested_change" not in e or ("<= 1" not in e["suggested_change"] and "<= 2" not in e["suggested_change"])
            for e in edits
        ), edits
        _apply_reported_fix(cli, sql, rows)

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_infeasible_data_rhs_query_mode_virtual_offset(self, request, cli_fixture):
        """T3 query mode (default): a per-row data RHS (`x >= hi`) has no literal to loosen,
        so the clause reports ONE virtual query offset `x >= hi - delta` (delta = max
        overshoot) tagged edit_source='virtual_offset', not a dead-end conflict. The RHS
        column name `hi` is carried through so the suggestion names it."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x FROM (VALUES (1,5),(2,5)) t(id, hi) "
            "DECIDE x(BOOL) SUCH THAT x >= hi MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql)

        rows = _rows(result)
        assert {r["state"] for r in rows} == {"infeasible"}
        floor = _attrs(rows, "clause", "x >= hi")
        assert floor["edit_kind"] == "loosen"
        assert floor["edit_source"] == "virtual_offset"
        assert floor["offset_scope"] == "clause"
        # BOOLEAN can only reach 1, so the floor of 5 needs a -4 offset to become feasible.
        assert floor["suggested_change"] == "x >= hi - 4"
        assert floor["amount"] == "4"
        _apply_reported_fix(cli, sql, rows)

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_infeasible_data_rhs_expanded_mode_per_row_profile(self, request, cli_fixture):
        """T3 expanded mode: the same data RHS stays per-row, so each conflicting row is a
        separate expanded_row profile entry (a debug view of which generated rows are tight),
        not a single virtual offset. Here two rows with distinct caps (5, 7) each surface."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x FROM (VALUES (1,5),(2,7)) t(id, hi) "
            "DECIDE x(BOOL) SUCH THAT x >= hi MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql, scope="expanded")

        rows = _rows(result)
        assert {r["state"] for r in rows} == {"infeasible"}
        # Each per-row floor is exposed independently, tagged expanded_row/row.
        sources = {r["value"] for r in rows if r["attribute"] == "edit_source"}
        assert sources == {"expanded_row"}
        scopes = {r["value"] for r in rows if r["attribute"] == "offset_scope"}
        assert scopes == {"row"}
        # Both distinct floors appear as separate subjects.
        loosen_subjects = {
            r["subject"] for r in rows if r["attribute"] == "edit_kind" and r["value"] == "loosen"
        }
        assert loosen_subjects == {"x >= 5", "x >= 7"}

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_infeasible_data_rhs_tier_prefers_editable_edit(self, request, cli_fixture):
        """I2.c: when an editable knob (`x <= 5`) AND a data floor (`x >= lo`) both
        conflict, the data-offset tier is frozen at zero before editable loosening, so the
        solver loosens the editable cap and leaves the data row alone."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x FROM (VALUES (1,12)) t(id, lo) "
            "DECIDE x(REAL) SUCH THAT x <= 5 AND x >= lo MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql)

        rows = _rows(result)
        cap = _attrs(rows, "clause", "x <= 5")
        assert cap["suggested_change"] == "x <= 12"
        assert cap["amount"] == "7"
        assert not [r for r in rows if r["attribute"] == "conflict"]
        _apply_reported_fix(cli, sql, rows)

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_infeasible_lexicographic_data_vs_large_editable_slack(self, request, cli_fixture):
        """T2: a large editable slack still beats a small data virtual offset. The old
        summed ladder could prefer `x >= demand - 1` because 1 * 1e3 < 10000; the
        lexicographic D tier must stay at zero and loosen the source literal instead."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x FROM (VALUES (1,1)) t(id, demand) "
            "DECIDE x(REAL) SUCH THAT 10000*x <= 0 AND x >= demand MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql)

        rows = _rows(result)
        assert {r["state"] for r in rows} == {"infeasible"}
        cap = _attrs(rows, "clause", "10000 * x <= 0")
        assert cap["edit_kind"] == "loosen"
        assert cap["edit_source"] == "source_literal"
        assert cap["suggested_change"] == "10000 * x <= 10000"
        assert cap["amount"] == "10000"
        assert not [r for r in rows if r["attribute"] == "edit_source" and r["value"] == "virtual_offset"]
        _apply_reported_fix(cli, sql, rows, {"10000 * x <= 0": "10000*x <= 0"})

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
            "DECIDE x(REAL) SUCH THAT x <= 2 + 3 AND x >= lo MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql)

        rows = _rows(result)
        assert {r["state"] for r in rows} == {"infeasible"}
        cap = _attrs(rows, "clause", "x <= 5")
        assert cap["suggested_change"] == "x <= 10"
        assert cap["amount"] == "5"
        assert not [r for r in rows if r["attribute"] == "conflict"]
        _apply_reported_fix(cli, sql, rows, {"x <= 5": "x <= 2 + 3"})

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_infeasible_uncorrelated_subquery_cap_is_editable(self, request, cli_fixture):
        """I2 follow-up: an UNCORRELATED scalar subquery RHS (`x <= (SELECT 5)`) flattens
        to a cross-joined column ref, structurally identical to row data — but it is one
        shared editable cap. Planning stamps the flattened value with query-wide
        provenance and canonicalization classifies the complete RHS, so the engine
        loosens it exactly like a literal `x <= 5` would (5 → 10 against the `x >= lo`
        = 10 floor), not as a per-row data conflict."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x FROM (VALUES (1,10)) t(id, lo) "
            "DECIDE x(REAL) SUCH THAT x <= (SELECT 5) AND x >= lo MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql)

        rows = _rows(result)
        assert {r["state"] for r in rows} == {"infeasible"}
        cap = _attrs(rows, "clause", "x <= 5")
        assert cap["suggested_change"] == "x <= 10"
        assert cap["amount"] == "5"
        assert not [r for r in rows if r["attribute"] == "conflict"]
        _apply_reported_fix(cli, sql, rows, {"x <= 5": "x <= (SELECT 5)"})

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_infeasible_reversed_uncorrelated_subquery_cap_is_editable(
        self, request, cli_fixture
    ):
        """The reversed spelling ``(SELECT 5) >= x`` has the same provenance.

        Canonicalization swaps the complete sides, but provenance is semantic,
        not an "original RHS" bit.  Diagnostics must therefore render the same
        editable ``x <= 5`` clause as the forward spelling, never the flattened
        internal name ``SUBQUERY``.
        """
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x FROM (VALUES (1,10)) t(id, lo) "
            "DECIDE x(REAL) SUCH THAT (SELECT 5) >= x AND x >= lo MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql)

        rows = _rows(result)
        assert {r["state"] for r in rows} == {"infeasible"}
        cap = _attrs(rows, "clause", "x <= 5")
        assert cap["suggested_change"] == "x <= 10"
        assert cap["amount"] == "5"
        assert "SUBQUERY" not in result.stdout
        assert not [r for r in rows if r["attribute"] == "conflict"]
        _apply_reported_fix(cli, sql, rows, {"x <= 5": "(SELECT 5) >= x"})

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    @pytest.mark.parametrize(
        "constraint",
        ("x + 2 <= (SELECT 7)", "(SELECT 7) >= x + 2"),
    )
    def test_infeasible_rebuilt_query_wide_subquery_bound_is_editable(
        self, request, cli_fixture, constraint
    ):
        """Classification belongs to the complete canonical bound, not one source node.

        Moving the data-only ``2`` across the relation rebuilds the bound as the
        query-wide expression ``(SELECT 7) - 2``. Both orientations must retain one
        editable scalar cap and render only its evaluated SQL-level value.
        """
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x FROM (VALUES (1,10)) t(id, lo) "
            f"DECIDE x(REAL) SUCH THAT {constraint} AND x >= lo MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql)

        rows = _rows(result)
        cap = _attrs(rows, "clause", "x <= 5")
        assert cap["suggested_change"] == "x <= 10"
        assert cap["amount"] == "5"
        assert "SUBQUERY" not in result.stdout
        assert "__query_wide" not in result.stdout
        _apply_reported_fix(cli, sql, rows, {"x <= 5": constraint})

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_multiple_query_wide_subqueries_form_one_shared_bound(
        self, request, cli_fixture
    ):
        """Arithmetic composed entirely from query-wide values remains one knob."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x FROM (VALUES (1,10)) t(id, lo) "
            "DECIDE x(REAL) SUCH THAT x <= (SELECT 2) + (SELECT 3) "
            "AND x >= lo MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql)

        rows = _rows(result)
        cap = _attrs(rows, "clause", "x <= 5")
        assert cap["suggested_change"] == "x <= 10"
        assert cap["amount"] == "5"
        assert "SUBQUERY" not in result.stdout
        assert "__query_wide" not in result.stdout

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    @pytest.mark.parametrize(
        "constraint",
        ("x <= lo + (SELECT 1)", "lo + (SELECT 1) >= x"),
    )
    def test_mixed_subquery_and_row_data_bound_stays_data_backed(
        self, request, cli_fixture, constraint
    ):
        """One query-wide component must not promote a row-varying rebuilt bound.

        The data-backed cap remains non-editable, so diagnosis loosens the explicit
        floor. Internal flattened-subquery names are suppressed instead of appearing
        in a virtual-offset suggestion.
        """
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x FROM (VALUES (1,4),(2,7)) t(id, lo) "
            f"DECIDE x(REAL) SUCH THAT {constraint} AND x >= 100 MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql)

        rows = _rows(result)
        floor = _attrs(rows, "clause", "x >= 100")
        assert floor["suggested_change"] == "x >= 5"
        assert floor["amount"] == "95"
        assert not [
            r for r in rows
            if r["subject_kind"] == "clause"
            and r["subject"].startswith("x <=")
            and r["attribute"] == "suggested_change"
        ]
        assert "SUBQUERY" not in result.stdout
        assert "__query_wide" not in result.stdout

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_infeasible_correlated_subquery_cap_stays_per_row(self, request, cli_fixture):
        """I2 follow-up guard: a CORRELATED scalar subquery RHS (`x <= (SELECT hi)`, hi from
        the outer row) is genuinely per-row data and must NOT be tagged shared. The two
        distinct caps (5, 8) stay independent per-row bounds, so the conflict against the
        `x >= 100` floor loosens that editable floor down to the binding per-row cap (5),
        and no `x <= ...` clause ever becomes an editable scalar."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x FROM (VALUES (1,5),(2,8)) t(id, hi) "
            "DECIDE x(REAL) SUCH THAT x <= (SELECT hi) AND x >= 100 MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql)

        rows = _rows(result)
        assert {r["state"] for r in rows} == {"infeasible"}
        # The editable floor is the loosened knob; per-row caps bound it at min(5,8)=5.
        floor = _attrs(rows, "clause", "x >= 100")
        assert floor["edit_kind"] == "loosen"
        assert floor["suggested_change"] == "x >= 5"
        # The correlated subquery cap never gets a scalar loosening suggestion.
        assert not [
            r for r in rows
            if r["subject_kind"] == "clause"
            and r["subject"].startswith("x <=")
            and r["attribute"] == "suggested_change"
        ]
        assert "SUBQUERY" not in result.stdout
        assert "__row_varying_subquery" not in result.stdout
        _apply_reported_fix(cli, sql, rows)

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_infeasible_single_absorbed_bound_vs_row(self, request, cli_fixture):
        """Bug 1: a single absorbed cap (`x <= 4`) that conflicts with a matrix row
        (`2*x >= 30` → x >= 15) — the column box (implied + absorbed) no longer pins the
        variable, so the cap loosens 4 → 15 (amount 11). The `2*x` coefficient breaks the
        L1 tie, so the edit is deterministic."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT x FROM (VALUES (1)) t(id) "
            "DECIDE x(REAL) SUCH THAT x <= 4 AND 2 * x >= 30 MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql)

        rows = _rows(result)
        assert {r["state"] for r in rows} == {"infeasible"}
        cap = _attrs(rows, "clause", "x <= 4")
        assert cap["suggested_change"] == "x <= 15"
        assert cap["amount"] == "11"
        _apply_reported_fix(cli, sql, rows)

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_infeasible_avg_reports_raw_slack_and_avg_label(self, request, cli_fixture):
        """I2.d: an AVG constraint is stored pre-scaled by 1/N, so the engine renders
        the clause as `AVG(x)` (not `0.33*x + ...`) and reports the slack in the user's
        AVG units. A data-offset floor (`x >= lo`) forces AVG(x) up to 10, so the
        edit is `AVG(x) <= 5` → `AVG(x) <= 10` (raw amount 5)."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x FROM (VALUES (1,10),(2,10),(3,10)) t(id, lo) "
            "DECIDE x(REAL) SUCH THAT AVG(x) <= 5 AND x >= lo MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql)

        rows = _rows(result)
        avg = _attrs(rows, "clause", "AVG(x) <= 5")
        assert avg["suggested_change"] == "AVG(x) <= 10"
        assert avg["amount"] == "5"
        _apply_reported_fix(cli, sql, rows)

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_infeasible_data_varying_avg_keeps_avg_label(self, request, cli_fixture):
        """I2.d regression: a *data-varying* AVG coefficient (`AVG(x * w)`, w a
        column) can't take the clean FormatAvgLhs path, but it must still render as
        `AVG(...)` — not `SUM(...)`. The value is already in AVG units, so a SUM
        label makes the suggested edit a different, wrong constraint.

        w=[10,20,30], x BOOLEAN: max AVG(x*w) is all selected = 20 < 100, so the
        constraint is infeasible; the loosen is `AVG(x * w) >= 20` (amount 80).
        """
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, w, x FROM (VALUES (1,10),(2,20),(3,30)) t(id, w) "
            "DECIDE x(BOOL) SUCH THAT AVG(x * w) >= 100 MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql)

        rows = _rows(result)
        # The clause and its suggested edit both name AVG, never SUM.
        assert not any(str(r["subject"]).startswith("SUM(") for r in rows), (
            "data-varying AVG was mislabeled as SUM in the diagnosis"
        )
        avg = _attrs(rows, "clause", "AVG(x * w) >= 100")
        assert avg["suggested_change"] == "AVG(x * w) >= 20"
        assert avg["amount"] == "80"
        _apply_reported_fix(cli, sql, rows)

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_infeasible_strict_less_than_requotes_typed_literal(self, request, cli_fixture):
        """I2.d: `SUM(x) < 10` (integer, single row) is built as `<= 9` (δ baked in). The
        reported edit must re-quote against the user's typed `10` and render `<`, not the
        δ-adjusted `<=`. The single-row aggregate keeps its `SUM(...)` wrapper (Facet B). A
        data-offset floor forces the strict cap to loosen by 6 → `SUM(x) < 16`."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x FROM (VALUES (1,15)) t(id, lo) "
            "DECIDE x(INT) SUCH THAT SUM(x) < 10 AND x >= lo MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql)

        rows = _rows(result)
        edit = _attrs(rows, "clause", "SUM(x) < 10")
        assert edit["suggested_change"] == "SUM(x) < 16"
        assert edit["amount"] == "6"
        _apply_reported_fix(cli, sql, rows)

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_infeasible_strict_greater_than_requotes_typed_literal(self, request, cli_fixture):
        """I2.d: the `>` mirror of the strict re-quote. `SUM(x) > 10` (single row, built as
        `>= 11`) loosens downward against a data-offset cap → `SUM(x) > 2`. The
        single-row aggregate keeps its `SUM(...)` wrapper (Facet B)."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x FROM (VALUES (1,3)) t(id, hi) "
            "DECIDE x(INT) SUCH THAT SUM(x) > 10 AND x <= hi MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql)

        rows = _rows(result)
        edit = _attrs(rows, "clause", "SUM(x) > 10")
        assert edit["suggested_change"] == "SUM(x) > 2"
        assert edit["amount"] == "8"
        _apply_reported_fix(cli, sql, rows)

    @pytest.mark.parametrize("cli_fixture", ["decidb_cli_gurobi"])
    def test_infeasible_quadratic_loosens_linear_rhs(self, request, cli_fixture):
        """I2.d: a quadratic constraint (`POWER(x,2) <= 4`) gets a slack on its linear
        RHS only; loosening it (against a data-offset floor) re-quotes the clause as
        `POWER(x, 2) <= 100`. QCQP is Gurobi-only (HiGHS cannot solve it)."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x FROM (VALUES (1,10)) t(id, lo) "
            "DECIDE x(REAL) SUCH THAT POWER(x,2) <= 4 AND x >= lo MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql)

        rows = _rows(result)
        assert {r["state"] for r in rows} == {"infeasible"}
        edit = _attrs(rows, "clause", "POWER(x, 2) <= 4")
        assert edit["suggested_change"].startswith("POWER(x, 2) <= 100")
        # amount ≈ 96 (10² − 4), allowing for solver tolerance.
        assert abs(float(edit["amount"]) - 96.0) < 1e-3
        assert not [r for r in rows if "CAST(lo AS DOUBLE)" in r["subject"]]
        assert not [r for r in rows if r["attribute"] == "amount" and float(r["value"]) == 0.0]
        _apply_reported_fix(cli, sql, rows, {"POWER(x, 2) <= 4": "POWER(x,2) <= 4"})

    @pytest.mark.parametrize("cli_fixture", ["decidb_cli_gurobi"])
    def test_infeasible_strict_quadratic_requotes_typed_literal(self, request, cli_fixture):
        """I2.d regression: strict quadratic constraints must carry the user's typed
        literal through the quadratic builder too. `POWER(x,2) < 10` is enforced as
        `<= 9`, but the diagnosis should re-quote the edit as `< 10` -> `< 17`,
        not expose the normalized `<= 9` row."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x FROM (VALUES (1,4)) t(id, lo) "
            "DECIDE x(INT) SUCH THAT POWER(x,2) < 10 AND x >= lo MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql)

        rows = _rows(result)
        assert {r["state"] for r in rows} == {"infeasible"}
        edit = _attrs(rows, "clause", "POWER(x, 2) < 10")
        assert edit["suggested_change"] == "POWER(x, 2) < 17"
        assert edit["amount"] == "7"
        _apply_reported_fix(cli, sql, rows, {"POWER(x, 2) < 10": "POWER(x,2) < 10"})

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_infeasible_ne_prefers_loosen_over_drop(self, request, cli_fixture):
        """I4 prefer-loosen: `<>` is remove-only, and I4 now offers a removal dial for it,
        but dropping is the last resort (the removal tier is minimized first). The conflict
        `x <> 5 AND 5 <= x <= 5` can be fixed either by dropping the `<>` or by loosening an
        editable bound, so the engine loosens the bound and never drops."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x FROM (VALUES (1)) t(id) "
            "DECIDE x(INT) SUCH THAT x <> 5 AND x >= 5 AND x <= 5 MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql)

        rows = _rows(result)
        assert {r["state"] for r in rows} == {"infeasible"}
        # The edit loosens a bound; the `<>` is offered but not dropped.
        subjects = {r["subject"] for r in rows if r["attribute"] == "suggested_change"}
        assert subjects, "expected an editable-bound edit"
        assert all("<>" not in s for s in subjects), subjects
        assert not [r for r in rows if r["attribute"] == "edit_kind" and r["value"] == "drop"]
        _apply_reported_fix(cli, sql, rows)

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_infeasible_lexicographic_removal_vs_large_loosen(self, request, cli_fixture):
        """T2: a huge editable loosening still beats dropping `<>`. The old weighted
        ladder could prefer DROP because 1e6 < 10000000; lexicographic repair keeps the
        removal count at zero whenever a source-literal loosening can restore feasibility."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x FROM (VALUES (1,5)) t(id, lo) "
            "DECIDE x(INT) SUCH THAT x <> 5 AND x >= lo "
            "AND 10000000*x <= 50000000 MINIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql)

        rows = _rows(result)
        assert {r["state"] for r in rows} == {"infeasible"}
        cap = _attrs(rows, "clause", "10000000 * x <= 50000000")
        assert cap["edit_kind"] == "loosen"
        assert cap["suggested_change"] == "10000000 * x <= 60000000"
        assert cap["amount"] == "10000000"
        assert not [r for r in rows if r["attribute"] == "edit_kind" and r["value"] == "drop"]
        _apply_reported_fix(
            cli, sql, rows,
            {"10000000 * x <= 50000000": "10000000*x <= 50000000"},
        )

    @pytest.mark.parametrize("objective, expected_drop, drop_sql, expected_objective", [
        ("MINIMIZE SUM(x)", "x <> 0", "x <> 0", "0"),
        ("MAXIMIZE SUM(x)", "x <> 1", "x <> 1", "1"),
    ])
    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_infeasible_ne_must_drop_reports_objective_best_drop(
        self, request, cli_fixture, objective, expected_drop, drop_sql, expected_objective
    ):
        """T4: `x <> 0 AND x <> 1` on a BOOLEAN must drop exactly one `<>`, and stage 2
        chooses the objective-best minimum-cardinality DROP set rather than freezing the
        stage-1 arbitrary choice: `x <> 0` under MINIMIZE (pins x=0), `x <> 1` under
        MAXIMIZE (pins x=1). The reported objective is both the expected literal AND
        differential-checked against an independent re-solve of the fixed query."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT x FROM (VALUES (1)) t(id) "
            f"DECIDE x(BOOL) SUCH THAT x <> 0 AND x <> 1 {objective}"
        )
        result = _diagnose(cli, sql)

        rows = _rows(result)
        assert {r["state"] for r in rows} == {"infeasible"}
        drops = [r for r in rows if r["attribute"] == "edit_kind" and r["value"] == "drop"]
        assert len(drops) == 1, rows  # minimum-cardinality: exactly one dropped
        dropped = drops[0]["subject"]
        assert dropped == expected_drop
        reported = _attrs(rows, "model", "NULL")["achievable_objective"]
        assert reported == expected_objective
        # Oracle: re-solve the query with the dropped `<>` removed and confirm the match.
        assert float(reported) == pytest.approx(
            _resolve_objective(cli, sql, [drop_sql], ["x"]))

    @pytest.mark.parametrize("objective, expected_drop, drop_sql, expected_objective", [
        ("MINIMIZE SUM(x)", "SUM(x) <> 0", "SUM(x) <> 0", "0"),
        ("MAXIMIZE SUM(x)", "SUM(x) <> 1", "SUM(x) <> 1", "1"),
    ])
    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_infeasible_aggregate_ne_must_drop_is_named_and_objective_best(
        self, request, cli_fixture, objective, expected_drop, drop_sql, expected_objective
    ):
        """T4 plus I4 aggregate naming: aggregate `<>` indicators live in the global
        block, so the DROP must stay *named* while stage 2 chooses the objective-best
        minimum-cardinality aggregate DROP set. Objective is differential-checked."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT x FROM (VALUES (1)) t(id) "
            f"DECIDE x(BOOL) SUCH THAT SUM(x) <> 0 AND SUM(x) <> 1 {objective}"
        )
        result = _diagnose(cli, sql)

        rows = _rows(result)
        assert {r["state"] for r in rows} == {"infeasible"}
        drops = [r for r in rows if r["attribute"] == "edit_kind" and r["value"] == "drop"]
        assert len(drops) == 1, rows  # minimum-cardinality: exactly one dropped
        dropped = drops[0]["subject"]
        assert dropped.strip(), "dropped aggregate `<>` must be named, not empty"
        assert "<>" in dropped and "sum" in dropped.lower(), dropped
        assert dropped == expected_drop
        reported = _attrs(rows, "model", "NULL")["achievable_objective"]
        assert reported == expected_objective
        # Oracle: re-solve with the dropped aggregate `<>` removed and confirm the match.
        assert float(reported) == pytest.approx(
            _resolve_objective(cli, sql, [drop_sql], ["x"]))

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_infeasible_objective_indifferent_drop_tie_is_solver_agnostic(
        self, request, cli_fixture
    ):
        """T4 tie-break — solver-agnostic determinism when the objective is *indifferent*
        between two equally-minimal DROP sets. The conflict is on `x` (`x <> 0 AND x <> 1`
        on a BOOLEAN), but the objective only involves the free `y`, so stage 2 has no
        objective reason to prefer either `<>` — the achievable objective is 5 whichever is
        dropped. Without a tie-break Gurobi and HiGHS name *different* clauses; the stage-2b
        source-order tie-break makes both drop the earliest-declared `x <> 0`. Asserting the
        SAME clause on both backends is the solver-agnosticism guarantee.

        (Stage 1 has no objective, so its arbitrary pick cannot be forced from SQL; that both
        backends — which seed stage 1 differently — report the same objective-best drop is the
        available evidence that stage 2, not stage 1, selects what is reported.)"""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT x, y FROM (VALUES (1)) t(id) "
            "DECIDE x(BOOL), y(INT) SUCH THAT x <> 0 AND x <> 1 AND y <= 5 "
            "MAXIMIZE SUM(y)"
        )
        result = _diagnose(cli, sql)

        rows = _rows(result)
        assert {r["state"] for r in rows} == {"infeasible"}
        drops = [r for r in rows if r["attribute"] == "edit_kind" and r["value"] == "drop"]
        assert len(drops) == 1, rows  # minimum-cardinality: exactly one dropped
        assert drops[0]["subject"] == "x <> 0", rows  # earliest-declared, both backends
        reported = _attrs(rows, "model", "NULL")["achievable_objective"]
        assert reported == "5"  # objective is unaffected by which `<>` is dropped
        # Oracle: dropping either `<>` yields the same objective; check the reported one.
        assert float(reported) == pytest.approx(
            _resolve_objective(cli, sql, ["x <> 0"], ["y"]))

    @pytest.mark.parametrize("objective, expected_drops, expected_objective", [
        ("MINIMIZE SUM(x) + SUM(y)", ["x <> 0", "y <> 0"], "0"),
        ("MAXIMIZE SUM(x) + SUM(y)", ["x <> 1", "y <> 1"], "2"),
    ])
    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_infeasible_must_drop_multiple_reports_objective_best_set(
        self, request, cli_fixture, objective, expected_drops, expected_objective
    ):
        """T4 at cardinality > 1 — the objective-best *set*, not just a single drop. Two
        independent BOOLEANs, each with a contradictory `<> 0 AND <> 1`, so the only fix is
        to drop exactly one `<>` per variable (minimum cardinality 2). Stage 2 picks the
        objective-best drop per variable: `<> 0` under MINIMIZE (pins each to 0), `<> 1`
        under MAXIMIZE (pins each to 1). Objective is differential-checked."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT x, y FROM (VALUES (1)) t(id) "
            "DECIDE x(BOOL), y(BOOL) SUCH THAT "
            f"x <> 0 AND x <> 1 AND y <> 0 AND y <> 1 {objective}"
        )
        result = _diagnose(cli, sql)

        rows = _rows(result)
        assert {r["state"] for r in rows} == {"infeasible"}
        drops = sorted(
            r["subject"] for r in rows
            if r["attribute"] == "edit_kind" and r["value"] == "drop"
        )
        assert drops == sorted(expected_drops), rows  # exactly two, objective-best per var
        reported = _attrs(rows, "model", "NULL")["achievable_objective"]
        assert reported == expected_objective
        # Oracle: re-solve with both dropped `<>` removed and confirm the match.
        assert float(reported) == pytest.approx(
            _resolve_objective(cli, sql, expected_drops, ["x", "y"]))

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    @pytest.mark.parametrize("domain", ["(2)", "(2, 3)"])
    @pytest.mark.parametrize("when", ["", " WHEN id = 1"])
    def test_infeasible_in_formulation_is_one_atomic_drop(
        self, request, cli_fixture, domain, when
    ):
        """Singleton, multi-value, negative, and WHEN-wrapped IN formulations are
        source-level remove-only groups. Cardinality/linking/absorbed-floor rows never
        surface separately and always disappear together."""
        cli = request.getfixturevalue(cli_fixture)
        clause = f"b IN {domain}{when}"
        sql = (
            "SELECT id, b FROM (VALUES (1)) t(id) DECIDE b(BOOL) "
            f"SUCH THAT SUM(b) >= 0 AND {clause} MAXIMIZE SUM(b)"
        )
        result = _diagnose(cli, sql)
        assert not _errors(result), result.stderr
        rows = _rows(result)
        edits = _clause_edits(rows)
        assert edits == [{"subject": clause, "edit_kind": "drop"}], rows
        fixed_sql = _apply_reported_fix(cli, sql, rows)
        assert float(_attrs(rows, "model", "NULL")["achievable_objective"]) == pytest.approx(
            _resolve_objective(cli, fixed_sql, [], ["b"])
        )

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_negative_in_group_participates_in_atomic_choice(self, request, cli_fixture):
        """A negative IN formulation (including its absorbed floor) is one candidate.
        Against a disjoint positive IN, MAXIMIZE selects dropping the negative clause."""
        cli = request.getfixturevalue(cli_fixture)
        negative = "x IN (-2, -3)"
        sql = (
            "SELECT x FROM (VALUES (1)) t(id) DECIDE x(INT) "
            f"SUCH THAT {negative} AND x IN (1, 2) MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql)
        assert not _errors(result), result.stderr
        rows = _rows(result)
        assert _clause_edits(rows) == [{"subject": negative, "edit_kind": "drop"}], rows
        _apply_reported_fix(cli, sql, rows)

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    @pytest.mark.parametrize(
        "clause",
        [
            "norm(b, 0, 10) >= 2",
            "norm(b, 0) = 2",
            "norm(b, 0, 10) <= -1",
        ],
    )
    def test_infeasible_l0_formulation_is_one_atomic_drop(
        self, request, cli_fixture, clause
    ):
        """Explicit/auto L0 and <=/>=/= outer comparisons drop the source clause,
        never one indicator/link/envelope row and never a LOOSEN amount."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT b FROM (VALUES (1)) t(id) DECIDE b(BOOL) "
            f"SUCH THAT SUM(b) >= 0 AND {clause} MAXIMIZE SUM(b)"
        )
        result = _diagnose(cli, sql)
        assert not _errors(result), result.stderr
        rows = _rows(result)
        edits = _clause_edits(rows)
        assert len(edits) == 1, rows
        assert edits[0]["edit_kind"] == "drop"
        assert "NORM" in edits[0]["subject"].upper()
        assert "amount" not in edits[0]
        _apply_reported_fix(cli, sql, rows, {edits[0]["subject"]: clause})

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_objective_l0_is_not_removable(self, request, cli_fixture):
        """An L0 marker in the objective is not a constraint repair group."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT b FROM (VALUES (1)) t(id) DECIDE b(BOOL) "
            "SUCH THAT b <= 0 AND SUM(b) >= 1 MAXIMIZE norm(b, 0, 10)"
        )
        rows = _rows(_diagnose(cli, sql))
        assert not [e for e in _clause_edits(rows) if e["edit_kind"] == "drop"], rows

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_l0_removal_survives_composed_minmax_extraction(self, request, cli_fixture):
        """A comparison containing L0 keeps one DROP identity when composed MIN/MAX
        extraction replaces the source tree with an execution-time auxiliary block."""
        cli = request.getfixturevalue(cli_fixture)
        clause = "norm(b, 0, 10) + MAX(x) >= 3"
        sql = (
            "SELECT b, x FROM (VALUES (1)) t(id) DECIDE b(BOOL), x(BOOL) "
            f"SUCH THAT {clause} MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql)
        assert not _errors(result), result.stderr
        rows = _rows(result)
        edits = _clause_edits(rows)
        assert len(edits) == 1, rows
        assert edits[0]["edit_kind"] == "drop"
        assert "NORM" in edits[0]["subject"].upper()
        assert "MAX" in edits[0]["subject"].upper()

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_prepared_l0_bilinear_removal_metadata_survives_replay(
        self, request, cli_fixture
    ):
        """The serialized BilinearLink keeps source/removal provenance for the
        execution-time McCormick descendants inside an L0 group."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT b, x FROM (VALUES (1)) t(id) DECIDE b(BOOL), x(INT) "
            "SUCH THAT x <= 1 AND norm(b * x, 0, 10) >= 2 MAXIMIZE SUM(x)"
        )
        out = cli.execute_script(
            ".mode csv\n"
            f"PREPARE atomic_drop AS DIAGNOSE {sql};\n"
            "EXECUTE atomic_drop;\n"
            "EXECUTE atomic_drop;\n"
        )
        assert not _errors(out), out.stderr
        assert out.stdout.count("remove this clause") == 2, out.stdout

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_removal_bigm_pragma_is_gone(self, request, cli_fixture):
        """Exact grouped omission has no diagnostic Big-M to tune."""
        cli = request.getfixturevalue(cli_fixture)
        out = cli.execute_script("PRAGMA diagnose_decide_removal_bigm=1e7;\nSELECT 1;\n")
        assert "unrecognized configuration parameter" in out.stderr.lower()

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_slack_scope_pragma_validation(self, request, cli_fixture):
        """T3: the slack-scope pragma accepts only query|expanded (case-insensitive,
        normalized to lowercase), and rejects anything else at SET time."""
        cli = request.getfixturevalue(cli_fixture)
        # Valid values normalize to lowercase.
        ok = cli.execute_script(
            ".mode csv\n"
            "PRAGMA diagnose_decide_infeasible_slack_scope='EXPANDED';\n"
            "SELECT current_setting('diagnose_decide_infeasible_slack_scope') AS s;\n"
        )
        assert "expanded" in ok.stdout
        # An unknown value is rejected at SET time.
        bad = cli.execute_script(
            "PRAGMA diagnose_decide_infeasible_slack_scope='bogus';\nSELECT 1;\n"
        )
        assert "slack_scope" in bad.stderr.lower()

    @pytest.mark.parametrize("cli_fixture", ["decidb_cli_gurobi"])
    def test_infeasible_mccormick_rows_stay_rigid(self, request, cli_fixture):
        """I2.e: McCormick bilinear-link rows (STRUCTURAL) are rigid. A bilinear conflict
        (`x*y <= 1 AND x >= 5 AND y >= 5`) is resolved by loosening a user bound; no slack
        attaches to the synthesized link rows. Bilinear is Gurobi-only here."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x, y FROM (VALUES (1)) t(id) "
            "DECIDE x(REAL), y(REAL) SUCH THAT x * y <= 1 AND x >= 5 AND y >= 5 "
            "MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql)

        rows = _rows(result)
        assert {r["state"] for r in rows} == {"infeasible"}
        # The edit loosens a user bound (`x >= 5` / `y >= 5`), not an internal link row.
        subjects = {r["subject"] for r in rows if r["attribute"] == "suggested_change"}
        assert subjects, "expected an editable-bound edit"
        assert any(s in ("x >= 5", "y >= 5") for s in subjects), subjects
        _apply_reported_fix(cli, sql, rows)

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_infeasible_without_the_prefix_reports_only_its_state(self, request, cli_fixture):
        """The same infeasible query, unprefixed: the plain state, no clause, no repair,
        and no second solve to find one."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT x FROM (VALUES (1)) t(id) "
            "DECIDE x(REAL) SUCH THAT x <= 5 AND 2 * x >= 30 MAXIMIZE SUM(x)"
        )
        result = cli.execute_script(f".mode csv\n{sql};\n")
        err = result.stderr.lower()
        assert "decide optimization is infeasible" in err
        assert "x <= 15" not in err
        assert not list(csv.DictReader(io.StringIO(result.stdout)))


@pytest.mark.query_diagnostics
class TestInfeasibleHeadlineAndRendering:
    """The diagnosis names the relevant clause, and clause labels read in the user's SQL
    terms — an ungrouped SUM folds back to `SUM(...)` (not `x + x + x`), and a `<>` drops
    its implicit CAST/parens."""

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_one_problem_clause_is_named(self, request, cli_fixture):
        """A unique loosen fix names exactly the clause it applies to."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x FROM (VALUES (1),(2),(3)) t(id) "
            "DECIDE x(INT) SUCH THAT x >= 10 AND x <= 5 MAXIMIZE SUM(x)"
        )
        rows = _rows(_diagnose(cli, sql))
        # One clause is named, and it is the one the user can edit.
        assert [e["subject"] for e in _clause_edits(rows)] == ["x <= 5"], rows

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_ungrouped_sum_folds_in_clause_and_change(self, request, cli_fixture):
        """An ungrouped `SUM(x) >= K` renders as `SUM(x)`, in both the finding's clause
        and its suggested change — never the row-expanded `x + x + x`."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x FROM (VALUES (1),(2),(3)) t(id) "
            "DECIDE x(BOOL) SUCH THAT SUM(x) >= 999999 MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql)
        rows = _rows(result)
        attrs = _attrs(rows, "clause", "SUM(x) >= 999999")
        assert attrs["suggested_change"] == "SUM(x) >= 3"
        assert "x + x" not in result.stdout
        assert [e["subject"] for e in _clause_edits(rows)] == ["SUM(x) >= 999999"], rows
        _apply_reported_fix(cli, sql, rows)

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_ungrouped_weighted_sum_folds_with_uniform_coeff(self, request, cli_fixture):
        """A uniform-valued data coefficient keeps the written `SUM(x * w)` source
        expression (never a raw `x + x` fan-out). The infeasibility is repaired by a
        single objective-preserving edit: loosen the budget to `<= 5` so one `x` can be
        chosen (achievable objective 1). Before the boolean bound-absorption fix, `x` was
        wrongly pinned to 0 (the budget `<= -1` absorbed `x <= -0.2`), which forced a
        degenerate two-clause objective-0 fix; the bound fix now lets stage 2 keep the
        floor and reach objective 1 with one edit."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, w, x FROM (VALUES (1,5),(2,5),(3,5)) t(id,w) "
            "DECIDE x(BOOL) SUCH THAT SUM(x) >= 1 AND SUM(x * w) <= -1 MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql)
        rows = _rows(result)
        assert _attrs(rows, "clause", "SUM(x * w) <= -1")["suggested_change"] == "SUM(x * w) <= 5"
        assert _attrs(rows, "model", "NULL")["achievable_objective"] == "1"
        assert "x + x" not in result.stdout
        # The single objective-preserving fix loosens only the budget clause.
        assert [e["subject"] for e in _clause_edits(rows)] == ["SUM(x * w) <= -1"], rows
        _apply_reported_fix(cli, sql, rows, {"SUM(5*x) <= -1": "SUM(x * w) <= -1"})

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_data_weighted_sum_renders_symbolic_column(self, request, cli_fixture):
        """A data-VARYING weighted SUM `SUM(x * p)` has no single literal coefficient to
        quote, but the binder carries the coefficient column name `p` through to the
        diagnosis, so the clause renders symbolically as `SUM(x * p)` instead of dumping
        the per-row numeric fan-out (`10*x + 20*x + 30*x`)."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, p, x FROM (VALUES (1,10),(2,20),(3,30)) t(id,p) "
            "DECIDE x(BOOL) SUCH THAT SUM(x) >= 3 AND SUM(x * p) <= 5 MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql)
        rows = _rows(result)
        cap = _attrs(rows, "clause", "SUM(x * p) <= 5")
        assert cap["edit_kind"] == "loosen"
        assert cap["suggested_change"] == "SUM(x * p) <= 30"
        # The symbolic column name replaces the raw per-row numeric fan-out.
        assert "10*x" not in result.stdout
        assert "20*x" not in result.stdout
        _apply_reported_fix(cli, sql, rows)

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_ne_drop_label_is_clean(self, request, cli_fixture):
        """A dropped `<>` reads `x <> 1` — the binder's implicit CAST and the wrapping
        parens are stripped — in the finding's clause."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT x FROM (VALUES (1)) t(id) "
            "DECIDE x(BOOL) SUCH THAT x <> 0 AND x <> 1 MINIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql)
        rows = _rows(result)
        dropped = [r["subject"] for r in rows if r["attribute"] == "edit_kind" and r["value"] == "drop"]
        assert len(dropped) == 1
        assert "cast" not in dropped[0].lower() and "(" not in dropped[0]
        assert re.fullmatch(r"x <> [01]", dropped[0]), dropped[0]
        _apply_reported_fix(cli, sql, rows)

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_three_edits_are_all_reported(self, request, cli_fixture):
        """Multiple actionable edits point to all problem clauses without implying that
        any one clause alone is sufficient."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id,x,y,z FROM (VALUES (1)) t(id) "
            "DECIDE x(REAL),y(REAL),z(REAL) "
            "SUCH THAT x<=1 AND x>=5 AND y<=1 AND y>=5 AND z<=1 AND z>=5 MAXIMIZE SUM(x+y+z)"
        )
        result = _diagnose(cli, sql)
        rows = _rows(result)
        # All three problem clauses are named, none implied sufficient on its own.
        assert sorted(e["subject"] for e in _clause_edits(rows)) == [
            "x <= 1", "y <= 1", "z <= 1"
        ], rows
        _apply_reported_fix(
            cli, sql, _rows(result),
            {"x <= 1": "x<=1", "y <= 1": "y<=1", "z <= 1": "z<=1"},
        )

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_large_magnitude_suggestion_is_exact(self, request, cli_fixture):
        """A large integer bound is reported exactly — significant-figure rounding used to
        mangle `x <= 1234567890` into `x <= 1234570001`."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id,x FROM (VALUES (1)) t(id) "
            "DECIDE x(REAL) SUCH THAT x <= 1 AND x >= 1234567890 MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql)
        rows = _rows(result)
        attrs = _attrs(rows, "clause", "x <= 1")
        assert attrs["suggested_change"] == "x <= 1234567890"
        assert attrs["amount"] == "1234567889"
        _apply_reported_fix(cli, sql, rows)

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_composed_minmax_clause_is_named(self, request, cli_fixture):
        """A3 — the composed MIN/MAX outer pin is a constraint over an internal global
        z variable; the z carries its user source text (`MAX(x)`) so the diagnosis
        renders `SUM(x) + MAX(x) <= -1`, never an internal column name like `col3` —
        in both the finding's clause and its suggested change."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT x FROM (VALUES (1),(2),(3)) t(id) "
            "DECIDE x(INT) SUCH THAT MAX(x) + SUM(x) <= -1 MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql)
        rows = _rows(result)
        assert {r["state"] for r in rows} == {"infeasible"}
        edits = _clause_edits(rows)
        assert len(edits) == 1, rows
        subject = edits[0]["subject"]
        assert "MAX(x)" in subject and "SUM(x)" in subject, subject
        # No internal column name anywhere a user reads.
        for text in (result.stdout, result.stderr):
            assert not re.search(r"\bcol\d+\b", text), text
        _apply_reported_fix(cli, sql, rows, {subject: "MAX(x) + SUM(x) <= -1"})

    # `c` differs per row (0 / 100) INSIDE the extremum, so the closing Big-M is the
    # family's Span (110) rather than its BigM (10). That gap is what made the internal
    # row ~11x cheaper to blame than any clause the user wrote; the test above cannot
    # see it, because `MAX(x)` has no data column and there Span == BigM.
    _COMPOSED_DATA_SQL = (
        "SELECT id, x, x * v AS contrib FROM (VALUES (1, 1.0, 0.0), (2, 2.0, 100.0)) t(id, v, c) "
        "DECIDE x(REAL) "
        "SUCH THAT x >= 0 AND x <= 10 AND SUM(x) >= 19 AND MIN(x + c) + SUM(x) <= 22 "
        "MAXIMIZE SUM(x * v)"
    )

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_composed_minmax_never_blames_its_own_big_m(self, request, cli_fixture):
        """A composed MIN/MAX lowers to an envelope, closing rows and an outer row. Only
        the outer row is the user's clause; the closing rows encode `z = MIN(...)`, carry
        the Big-M, and used to be marked loosenable, so the engine blamed one and printed
        `MIN((x + c)) - x - 110*MIN((x + c)) >= -110` — a string appearing nowhere in the
        query, with `edit_source=source_literal` asserting the user had typed it."""
        cli = request.getfixturevalue(cli_fixture)
        sql = self._COMPOSED_DATA_SQL
        result = _diagnose(cli, sql)
        rows = _rows(result)
        assert {r["state"] for r in rows} == {"infeasible"}

        # The blamed clause is one the user can find in their own query. (The shared
        # invariant in _apply_reported_fix asserts this generally; naming it here says
        # which regression this test exists for.)
        edits = _clause_edits(rows)
        assert len(edits) == 1, rows
        assert "110" not in edits[0]["subject"], edits[0]["subject"]

        reported = _attrs(rows, "model", "NULL")["achievable_objective"]
        fixed_sql = _apply_reported_fix(cli, sql, rows)
        # Oracle: the promised payoff is what the repaired query actually returns.
        solved = list(csv.DictReader(io.StringIO(
            cli.execute_script(".mode csv\n" + fixed_sql + ";\n").stdout)))
        assert float(reported) == pytest.approx(
            sum(float(r["contrib"]) for r in solved))

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_composed_minmax_repair_is_in_the_users_units(self, request, cli_fixture):
        """The number, not the name. Slack on a closing row is measured in the auxiliary's
        units; the user's bound moves by that slack times the coefficient `z` carries in
        the outer row. With `3*MIN(...)` the two differ by 3 — the engine reported 8 where
        the real repair is 24, and `<= 30` re-solved to infeasible. Blaming the outer row
        instead puts the slack on the user's bound directly, factor 1 by construction.

        `_apply_reported_fix` re-solves the edited query, so an amount in the wrong units
        fails here even when the clause it names is perfectly correct."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x, x * v AS contrib FROM (VALUES (1, 1.0, 0.0), (2, 2.0, 100.0)) t(id, v, c) "
            "DECIDE x(REAL) "
            "SUCH THAT x >= 0 AND x <= 10 AND SUM(x) >= 19 AND 3*MIN(x + c) + SUM(x) <= 22 "
            "MAXIMIZE SUM(x * v)"
        )
        result = _diagnose(cli, sql)
        rows = _rows(result)
        assert {r["state"] for r in rows} == {"infeasible"}

        reported = _attrs(rows, "model", "NULL")["achievable_objective"]
        fixed_sql = _apply_reported_fix(cli, sql, rows)
        solved = list(csv.DictReader(io.StringIO(
            cli.execute_script(".mode csv\n" + fixed_sql + ";\n").stdout)))
        assert float(reported) == pytest.approx(
            sum(float(r["contrib"]) for r in solved))

    def test_composed_minmax_advice_does_not_depend_on_the_backend(
        self, request, decidb_cli_highs, decidb_cli_gurobi
    ):
        """Invariant 2 in its observable form. The two backends state MIN/MAX differently
        (native vs lowered), so they hand the elastic engine different row sets; any repair
        choice not fixed by a stated rule falls out differently and a user moving hosts is
        told to edit a different line of their own query."""
        sql = self._COMPOSED_DATA_SQL
        by_backend = {}
        for name, cli in (("highs", decidb_cli_highs), ("gurobi", decidb_cli_gurobi)):
            rows = _rows(_diagnose(cli, sql))
            by_backend[name] = {
                "edits": _clause_edits(rows),
                "achievable_objective": _attrs(rows, "model", "NULL").get(
                    "achievable_objective"),
            }
        assert_backends_agree(by_backend, "composed MIN/MAX over a data column")


@pytest.mark.query_diagnostics
class TestEqualityBoundConflict:
    """Two per-row equality bounds on one variable must intersect (and conflict if
    contradictory), never resolve last-writer-wins to a wrong solution."""

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    @pytest.mark.parametrize("clause", ["x = 5 AND x = 10", "x = 10 AND x = 5"])
    def test_contradictory_equalities_are_infeasible(self, request, cli_fixture, clause):
        cli = request.getfixturevalue(cli_fixture)
        sql = f"SELECT id,x FROM (VALUES (1)) t(id) DECIDE x(REAL) SUCH THAT {clause} MAXIMIZE SUM(x)"
        result = _diagnose(cli, sql)
        rows = _rows(result)
        assert {r["state"] for r in rows} == {"infeasible"}
        _apply_reported_fix(cli, sql, rows)

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    @pytest.mark.parametrize("clause,expected", [("x = 5 AND x = 5", 5.0), ("x = -3", -3.0)])
    def test_consistent_equality_still_solves(self, request, cli_fixture, clause, expected):
        """Regression guard: the intersect must not break a consistent (or explicitly
        negative) equality — both still solve to their value, no false infeasible."""
        cli = request.getfixturevalue(cli_fixture)
        sql = f"SELECT x FROM (VALUES (1)) t(id) DECIDE x(REAL) SUCH THAT {clause} MAXIMIZE SUM(x)"
        out = cli.execute_script(".mode csv\n" + sql + ";\n").stdout
        rows = list(csv.DictReader(io.StringIO(out)))
        assert len(rows) == 1 and float(rows[0]["x"]) == pytest.approx(expected)


@pytest.mark.query_diagnostics
class TestDegenerateCoefficientFreeRow:
    """A constraint that keeps no decision term at all (`SUM(0 * x) <= -1`, or
    `x - x <= -1` where the terms cancel) is still a user clause with a bound, so it
    is diagnosed like any other: named, and loosened to the nearest bound 0 can meet.

    Such a row used to be discarded at build time, taking the whole model with it —
    diagnosis then ran against a model that was never populated and aborted fatally.
    The row is now kept coefficient-free, and no backend is asked to load it.
    """

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    @pytest.mark.parametrize("clause,subject,fixed", [
        ("SUM(0*x) <= -1", "SUM(0 * x) <= -1", "SUM(0 * x) <= 0"),
        ("x - x <= -1", "x - x <= -1", "x - x <= 0"),
        ("0*x >= 1", "0 * x >= 1", "0 * x >= 0"),
    ])
    def test_degenerate_row_is_named_and_loosened(
        self, request, cli_fixture, clause, subject, fixed
    ):
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x FROM (VALUES (1), (2)) t(id) "
            f"DECIDE x(INT) SUCH THAT x <= 5 AND {clause} MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql)

        assert "INTERNAL Error" not in result.stderr, result.stderr
        rows = _rows(result)
        assert {r["state"] for r in rows} == {"infeasible"}
        edit = _attrs(rows, "clause", subject)
        assert edit["edit_kind"] == "loosen"
        # The left side is 0 whatever x does, so the least change is to move the
        # bound to 0 — the one value that admits it.
        assert edit["suggested_change"] == fixed
        _apply_reported_fix(cli, sql, rows, {subject: clause})

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_degenerate_row_does_not_mask_a_second_conflict(self, request, cli_fixture):
        """The degenerate row is repaired independently: a genuine conflict elsewhere
        in the same query is still found and reported alongside it."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x FROM (VALUES (1), (2)) t(id) "
            "DECIDE x(INT) SUCH THAT x <= 5 AND SUM(x) >= 100 AND SUM(0*x) <= -1 "
            "MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql)

        rows = _rows(result)
        subjects = {e["subject"] for e in _clause_edits(rows)}
        assert "SUM(0 * x) <= -1" in subjects
        assert subjects - {"SUM(0 * x) <= -1"}, (
            f"the second conflict was not reported: {subjects}"
        )
        _apply_reported_fix(cli, sql, rows, {"SUM(0 * x) <= -1": "SUM(0*x) <= -1"})


@pytest.mark.query_diagnostics
class TestUnreachableBound:
    """A bound no assignment can reach is named, and no edit is offered for it.

    `x >= inf` is infeasible on its own — no other clause is implicated, and no
    finite loosening closes the gap. The elastic engine cannot express that: it
    wires slack into the left side (`Ax - s <= b`), so `b` is never touched. Left
    to the solve it either saturated the slack at the internal 1e30 sentinel and
    handed back the user's own text as the "fix", or returned elastic-infeasible
    and named nothing at all. The verdict is now reached before the elastic model
    exists, so every shape names its clause and reports `unreachable_bound`
    instead of a suggested change.

    The cases that need an infinite bound to survive into the model run on Gurobi
    only: HiGHS pairs a one-sided row bound with its own 1e30 infinity sentinel, so an
    unreachable bound arrives as an inverted `lower > upper` pair it rejects at model
    load. It never reaches a solve, so there is nothing to diagnose; DeciDB refuses the
    query with a named SQL error instead, covered in `test_infinite_bounds.py`. The two
    regression cases below that do not need an infinite bound in the model cover both
    backends.
    """

    _ROWS = "SELECT id, x FROM (VALUES (1), (2), (3)) t(id) "
    _INF_BACKENDS = ["decidb_cli_gurobi"]

    @pytest.mark.parametrize("cli_fixture", _INF_BACKENDS)
    @pytest.mark.parametrize("clause,subject", [
        # A bare per-row bound: the shape that used to quote 1e+30 back as the amount.
        ("x >= 1e1000::DOUBLE", "x >= inf"),
        # An aggregate bound: the shape that used to name nothing at all.
        ("SUM(x) >= 1e1000::DOUBLE", "SUM(x) >= inf"),
        # Hard MIN/MAX, both directions. These rows are stamped USER_MECHANISM (the
        # clause fans into Big-M rows), so they are found by tracing the row to a user
        # clause rather than by asking whether it is elastically editable.
        ("MIN(x) <= -1e1000::DOUBLE", "MIN(x) <= -inf"),
        ("MAX(x) >= 1e1000::DOUBLE", "MAX(x) >= inf"),
    ])
    def test_unreachable_bound_names_its_clause_and_offers_no_edit(
        self, request, cli_fixture, clause, subject
    ):
        cli = request.getfixturevalue(cli_fixture)
        sql = self._ROWS + f"DECIDE x(INT) SUCH THAT x >= 0 AND x <= 6 AND {clause} MAXIMIZE SUM(x)"
        result = _diagnose(cli, sql)

        rows = _rows(result)
        assert {r["state"] for r in rows} == {"infeasible"}
        # The clause is named, and named as the user wrote it.
        assert _attrs(rows, "clause", subject) == {"unreachable_bound": "true"}
        # No edit is offered: there is no finite bound to suggest, and the old behavior
        # (suggesting the user's own text back) is what made this misleading.
        assert not [r for r in rows if r["attribute"] in
                    ("edit_kind", "suggested_change", "amount")], rows
        # The generic "a fixed part of the query" fallback is wrong here — the conflict
        # is in a SUCH THAT clause — and must no longer be reached.
        assert not [r for r in rows if r["attribute"] == "elastic_infeasible"], rows

    @pytest.mark.parametrize("cli_fixture", _INF_BACKENDS)
    def test_grouped_unreachable_bound_names_the_failing_group(self, request, cli_fixture):
        """A per-group failure has to say WHICH group failed, not just which clause.

        `MIN(x) <= MAX(cap) PER g` is out of reach in group 0 only (cap -inf) and
        satisfiable in group 1. The clause alone sends the user looking through every
        group. The label was being dropped: a hard MIN/MAX whose bound no row can reach
        is re-emitted as ordinary per-row rows, and that emission site stamped the group
        key but passed an empty group label."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "WITH data AS ("
            "SELECT 0 AS g, -1e1000::DOUBLE AS cap UNION ALL "
            "SELECT 0, -1e1000::DOUBLE UNION ALL "
            "SELECT 1, 3.0 UNION ALL "
            "SELECT 1, 1.0) "
            "SELECT g, x FROM data DECIDE x(INT) "
            "SUCH THAT x >= 0 AND x <= 6 AND MIN(x) <= MAX(cap) PER g "
            "MAXIMIZE SUM(x)"
        )
        rows = _rows(_diagnose(cli, sql))
        assert {r["state"] for r in rows} == {"infeasible"}
        # The clause reads as the user wrote it, and the group is its own attribute
        # rather than a suffix baked into the clause text.
        assert _attrs(rows, "clause", "MIN(x) <= -inf PER g") == {
            "unreachable_bound": "true",
            "group": "0",
        }
        # Group 1 is satisfiable and must not be implicated.
        assert not [r for r in rows if r["value"] == "1" and r["attribute"] == "group"], rows

    @pytest.mark.parametrize("cli_fixture", _INF_BACKENDS)
    def test_one_clause_over_many_rows_is_reported_once(self, request, cli_fixture):
        """The clause fans into one model row per relation row, all rendering the same
        text. The user wrote one clause and must read one finding."""
        cli = request.getfixturevalue(cli_fixture)
        sql = self._ROWS + "DECIDE x(INT) SUCH THAT x <= 6 AND x >= 1e1000::DOUBLE MAXIMIZE SUM(x)"
        rows = _rows(_diagnose(cli, sql))
        assert [r["attribute"] for r in rows].count("unreachable_bound") == 1

    @pytest.mark.parametrize("cli_fixture", _INF_BACKENDS)
    def test_every_unreachable_clause_is_named(self, request, cli_fixture):
        """Two out-of-reach bounds are two independent findings; reporting only the
        first would send the user back for a second failure after fixing it."""
        cli = request.getfixturevalue(cli_fixture)
        sql = self._ROWS + (
            "DECIDE x(INT) SUCH THAT x <= 6 AND x >= 1e1000::DOUBLE "
            "AND SUM(x) >= 1e1000::DOUBLE MAXIMIZE SUM(x)"
        )
        rows = _rows(_diagnose(cli, sql))
        named = {r["subject"] for r in rows if r["attribute"] == "unreachable_bound"}
        assert named == {"x >= inf", "SUM(x) >= inf"}

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_a_vacuous_infinity_is_not_a_finding(self, request, cli_fixture):
        """`x <= +inf` points the other way: it constrains nothing and can never be why
        a solve failed. The scan must not confuse "infinite" with "out of reach", or an
        unrelated conflict would be reported against a clause that admits everything."""
        cli = request.getfixturevalue(cli_fixture)
        sql = self._ROWS + (
            "DECIDE x(INT) SUCH THAT x <= 1e1000::DOUBLE AND x >= 0 "
            "AND SUM(x) >= 100 AND SUM(x) <= 5 MAXIMIZE SUM(x)"
        )
        rows = _rows(_diagnose(cli, sql))
        assert not [r for r in rows if r["attribute"] == "unreachable_bound"], rows
        # The real conflict is still diagnosed as an ordinary loosen edit.
        assert [r for r in rows if r["attribute"] == "edit_kind"], rows

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_a_finite_unreachable_bound_still_gets_an_edit(self, request, cli_fixture):
        """Guard on the short-circuit's scope: only an infinite bound skips the elastic
        solve. A bound that is merely far out of reach is still repairable by loosening,
        and must keep its suggested change."""
        cli = request.getfixturevalue(cli_fixture)
        sql = self._ROWS + (
            "DECIDE x(INT) SUCH THAT x >= 0 AND x <= 6 AND SUM(x) >= 1000000000 "
            "MAXIMIZE SUM(x)"
        )
        rows = _rows(_diagnose(cli, sql))
        assert not [r for r in rows if r["attribute"] == "unreachable_bound"], rows
        # Which clause the engine picks to loosen is its own policy; what matters here
        # is that a repairable conflict still produces a repair.
        edits = _clause_edits(rows)
        assert edits and all(e["edit_kind"] == "loosen" for e in edits), rows
        assert all(e.get("suggested_change") for e in edits), rows


@pytest.mark.query_diagnostics
class TestNativeConstructDiagnosis:
    """Diagnosis over a construct the backend states itself.

    Gurobi can be told `aux = MAX(t1, t2)` or `aux = |t|` directly, so those clauses
    reach the model as a general (or, for `<>`, an indicator) constraint instead of
    Big-M rows. Every one of those queries is diagnosable — the user's own clause
    still has a row — but until this class none of them was covered, and the gap hid
    a real defect.

    **The defect.** The elastic engine folds the rows of one clause into a single
    shared slack, grouping them by `repair_group_id`. Absorbed bounds (`y <= 2`, which
    is stored as a column box, not a row) are re-emitted as slackable rows at diagnosis
    time under freshly minted ids, and those ids started at the count of linear
    constraint specs. But a GLOBAL constraint is numbered `constraints.size() + its own
    index`, from the same base — so the two ranges overlap. When the count of linear
    specs happened to exceed the ids actually in use the overlap was empty and nothing
    showed; stating a construct natively removes linear specs without removing ids, and
    the first absorbed bound landed on top of the first global constraint.

    The two then shared one slack, which is a slack spanning two unrelated clauses.
    The engine could satisfy `SUM(y) >= 100` by spending that slack on the bound rows,
    then report the edit against the aggregate: `loosen SUM(y) >= 100 to >= 81.6`, on a
    query where `y <= 2` over four rows caps `SUM(y)` at 8. Applying the suggested edit
    left the query infeasible.

    So these tests are written the way that defect would have been caught: the reported
    edit is applied and the query re-run, and the two arms are compared against each
    other. Neither reads a hand-computed number.
    """

    _ROWS = "SELECT g, x, y FROM (VALUES (0), (0), (1), (1)) t(g) "
    _NATIVE_BACKENDS = ["decidb_cli_gurobi"]

    # Each shape pairs a construct over `x` with a conflict that has nothing to do with
    # it: `y <= 2` over four rows caps `SUM(y)` at 8, so `SUM(y) >= 100` cannot hold and
    # the only repair is loosening the bound. The construct is there to put a general or
    # indicator constraint in the model — it is satisfiable throughout, and a diagnosis
    # that implicates it, or that spends the bound's slack and bills the aggregate, is
    # wrong.
    _SHAPES = [
        ("max", "x(REAL), y(REAL)", "MAX(x) >= 5"),
        ("max_per", "x(REAL), y(REAL)", "MAX(x) >= 5 PER g"),
        ("min", "x(REAL), y(REAL)", "MIN(x) >= 5"),
        ("abs", "x(REAL), y(REAL)", "ABS(x - 3) >= 2"),
        ("not_equal", "x(INT), y(REAL)", "x <> 3"),
    ]

    @classmethod
    def _sql(cls, decls, construct, x_bound=""):
        return (
            cls._ROWS + f"DECIDE {decls} SUCH THAT y <= 2{x_bound} AND {construct} "
            "AND SUM(y) >= 100 MINIMIZE SUM(y)"
        )

    @pytest.mark.parametrize("cli_fixture", _NATIVE_BACKENDS)
    @pytest.mark.parametrize("name, decls, construct", _SHAPES,
                             ids=[s[0] for s in _SHAPES])
    def test_repair_over_a_native_construct_actually_repairs(
        self, request, cli_fixture, name, decls, construct
    ):
        """The whole promise, on the shape that broke it: apply the reported edit and
        the query must solve. `x` is left unbounded above, which is the class that only
        reaches a model at all because the construct is stated natively — a Big-M
        lowering refuses it for want of a finite bound.
        """
        cli = request.getfixturevalue(cli_fixture)
        sql = self._sql(decls, construct)
        result = _diagnose(cli, sql)
        rows = _rows(result)

        assert {r["state"] for r in rows} == {"infeasible"}, rows
        # The bound is the only repairable clause; billing the aggregate was the defect.
        edits = _clause_edits(rows)
        assert [e["subject"] for e in edits] == ["y <= 2"], rows

        fixed_sql = _apply_reported_fix(cli, sql, rows)
        # Oracle: the objective the diagnosis promised is the one the repaired query
        # actually reaches. Re-solved, never computed here.
        reported = _attrs(rows, "model", "NULL")["achievable_objective"]
        repaired = list(csv.DictReader(io.StringIO(
            cli.execute_script(".mode csv\n" + fixed_sql + ";\n").stdout)))
        assert float(reported) == pytest.approx(
            sum(float(r["y"]) for r in repaired))

    @pytest.mark.parametrize("cli_fixture", _NATIVE_BACKENDS)
    @pytest.mark.parametrize("name, decls, construct", _SHAPES,
                             ids=[s[0] for s in _SHAPES])
    def test_native_and_lowered_report_the_same_repair(
        self, request, cli_fixture, name, decls, construct
    ):
        """The A/B. Which arm ran is a fact about the host's solver, so it must not
        change a word of what the user is told. `x` is bounded here only so the lowered
        arm has a finite Big-M and both arms exist to compare.
        """
        cli = request.getfixturevalue(cli_fixture)
        sql = self._sql(decls, construct, x_bound=" AND x <= 100")
        script = f".mode csv\nDIAGNOSE {sql};\n"

        def diagnosis(runner):
            proc = runner.execute_script(script)
            return proc.stdout + proc.stderr

        native = diagnosis(cli)
        lowered = diagnosis(cli.with_env({"DECIDB_NATIVE_CONSTRUCTS": "off"}))
        assert "infeasible" in native.lower(), native
        assert native == lowered, (
            f"the two arms disagree on {construct!r}:\n{native}\n---\n{lowered}"
        )

    @pytest.mark.parametrize("cli_fixture", _NATIVE_BACKENDS)
    def test_a_per_clause_reports_one_edit_not_one_per_group(self, request, cli_fixture):
        """One line of SQL is one edit, however many groups it covers.

        A natively-stated `MAX(e) >= K PER g` emits one bound row per group, and those
        rows are the same user literal — loosening `K` moves every group at once. Left
        unlabelled they did not fold, so this query reported two edits against the same
        clause text, `>= 11` and `>= 13`, with nothing to say which group either
        belonged to. Only the loosest repaired anything: applying `>= 13` left the query
        infeasible, so the diagnosis handed back an edit that made no progress.
        """
        cli = request.getfixturevalue(cli_fixture)
        sql = self._MINMAX_SQL
        result = _diagnose(cli, sql)
        rows = _rows(result)

        edits = _clause_edits(rows)
        assert len(edits) == 1, f"one clause, one edit:\n{rows}"
        assert edits[0]["subject"] == "MAX(x * 0.5 + c) >= 25 PER g", rows
        # A shared literal, not a per-row data offset — the user has a number to retype.
        assert edits[0]["edit_source"] == "source_literal", rows

        fixed_sql = _apply_reported_fix(cli, sql, rows)
        reported = _attrs(rows, "model", "NULL")["achievable_objective"]
        repaired = list(csv.DictReader(io.StringIO(
            cli.execute_script(".mode csv\n" + fixed_sql + ";\n").stdout)))
        assert float(reported) == pytest.approx(
            sum(float(r["x"]) for r in repaired))

    #: `x` carries a fractional weight inside the MAX on purpose. Both this clause and
    #: the column bound `x <= 9` can repair the query, and with a weight of 1 they cost
    #: the SAME edit (14 either way) — a tie the achievable-objective rule then settles
    #: in favour of the bound, which is correct but tests nothing about MIN/MAX. At 0.5
    #: the column bound has to travel twice as far (37 against the clause's 18.5), so
    #: the clause is the cheapest repair outright and naming it means what it says.
    #: The tie itself is covered by `test_a_tie_goes_to_the_repair_worth_more`.
    _MINMAX_SQL = (
        "SELECT g, x FROM (VALUES (0,1),(0,2),(1,3),(1,4)) t(g,c) "
        "DECIDE x(REAL) SUCH THAT x >= 0 AND x <= 9 AND MAX(x * 0.5 + c) >= 25 PER g "
        "MAXIMIZE SUM(x)"
    )

    def test_hard_minmax_names_the_users_clause_on_every_backend(
        self, decidb_cli_gurobi, decidb_cli_highs
    ):
        """A hard MIN/MAX clause is repaired by loosening the clause, everywhere.

        Gurobi states the MAX itself and keeps a row for it; HiGHS lowers it to a Big-M
        family. Those are different models, and the diagnosis used to follow: Gurobi
        loosened `MAX(x + c) >= 25`, HiGHS loosened the unrelated `x <= 9` because the
        lowered rows were rigid and the column bound was the only knob it had. Both
        repairs worked, but the same SQL got two different answers depending on which
        solver the host happened to have, and only one of them named the line the user
        wrote. The lowered rows now carry the clause too, so all three arms agree.
        """
        script = f".mode csv\nDIAGNOSE {self._MINMAX_SQL};\n"
        arms = {
            "gurobi (native)": decidb_cli_gurobi,
            "gurobi (lowered)": decidb_cli_gurobi.with_env(
                {"DECIDB_NATIVE_CONSTRUCTS": "off"}),
            "highs": decidb_cli_highs,
        }
        seen = {}
        for label, runner in arms.items():
            proc = runner.execute_script(script)
            seen[label] = proc.stdout + proc.stderr
            rows = _rows(proc)
            edits = _clause_edits(rows)
            assert len(edits) == 1, f"{label}: one clause, one edit:\n{rows}"
            assert edits[0]["subject"] == "MAX(x * 0.5 + c) >= 25 PER g", f"{label}:\n{rows}"
            _apply_reported_fix(runner, self._MINMAX_SQL, rows)

        distinct = set(seen.values())
        assert len(distinct) == 1, (
            "the arms disagree:\n" + "\n---\n".join(f"{k}:\n{v}" for k, v in seen.items())
        )

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_a_constant_on_the_left_is_not_subtracted_from_the_quoted_bound(
        self, request, cli_fixture
    ):
        """The clause is quoted as written, constant and all.

        `SUM(x + c) >= 100` is emitted as `SUM(x) >= 100 - SUM(c)`, because a constant
        LHS term folds into the RHS. The reported label still renders the LHS the user
        wrote — `SUM(x + c)` — so quoting the folded bound against it produced
        `SUM(x + c) >= 90`, a clause that appears nowhere in the query and whose
        suggested edit could not be pasted back over anything.
        """
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT g, x FROM (VALUES (0,1),(0,2),(1,3),(1,4)) t(g,c) "
            "DECIDE x(REAL) SUCH THAT x <= 9 AND SUM(x + c) >= 100 AND SUM(x + c) <= 5 "
            "MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql)
        rows = _rows(result)
        subjects = {e["subject"] for e in _clause_edits(rows)}
        assert subjects == {"SUM(x + c) >= 100", "SUM(x + c) <= 5"}, rows
        # Every quoted clause is a substring of the query the user ran, which is what
        # makes the suggestion pasteable.
        for subject in subjects:
            assert subject in sql, f"{subject!r} is not in the query:\n{sql}"
        _apply_reported_fix(cli, sql, rows)


@pytest.mark.query_diagnostics
class TestRepairsTheModelCanReach:
    """A repair the model cannot represent is a repair the diagnosis cannot offer.

    Every Big-M and every derived column ceiling in the linearizer is sized from the
    decision box as the query states it. That is right for the solve and wrong for the
    diagnosis that follows an infeasible one, because the elastic engine repairs by
    WIDENING a bound — and a ceiling baked in at the old width makes the widened repair
    unrepresentable. The engine then reports whatever else it can still see.

    B3 is the worked example. `ABS(x) >= 5` over `x` in [-1, 1] has two repairs of
    identical size: widen the box to `x <= 5` (worth 30) or weaken the ABS to
    `ABS(x) >= 1` (worth 6). Gurobi states ABS natively and bakes in nothing, so it saw
    both and took the better one; HiGHS lowers ABS to a Big-M envelope sized at 1, so
    the first repair was invisible to it and it reported the second. Same SQL, same
    tie-break rule, advice worth five times as much on one host than the other.

    The rule these tests hold: a clause that demands more of an auxiliary than the box
    can supply sizes that auxiliary itself. It fires only when the demand exceeds the
    box, which is only when the clause cannot be met as written — so no query that
    solves has its Big-M loosened.
    """

    #: B3 verbatim. `contrib` is projected so the achievable objective can be oracled
    #: against a real re-solve of the repaired query rather than a hand-computed number.
    _ABS_SQL = (
        "SELECT id, x * 6 AS contrib FROM (VALUES (1)) t(id) "
        "DECIDE x(REAL) SUCH THAT x >= -1 AND x <= 1 AND ABS(x) >= 5 "
        "MAXIMIZE SUM(x * 6)"
    )

    def test_abs_advice_does_not_depend_on_the_backend(
        self, decidb_cli_gurobi, decidb_cli_highs
    ):
        """Invariant 2 in its observable form, for ABS — `assert_backends_agree`'s
        intended caller. The lowered arm is exercised on Gurobi too, so the check is of
        the two FORMULATIONS and not of two solvers that happen to differ."""
        by_backend = {}
        arms = {
            "gurobi (native)": decidb_cli_gurobi,
            "gurobi (lowered)": decidb_cli_gurobi.with_env(
                {"DECIDB_NATIVE_CONSTRUCTS": "off"}),
            "highs": decidb_cli_highs,
        }
        for label, runner in arms.items():
            rows = _rows(_diagnose(runner, self._ABS_SQL))
            by_backend[label] = {
                "edits": _clause_edits(rows),
                "achievable_objective": _attrs(rows, "model", "NULL").get(
                    "achievable_objective"),
            }
        assert_backends_agree(by_backend, "ABS over a boxed decision")

    #: Each case carries a WITNESS repair: a single edit, verified by a real solve, that
    #: restores feasibility on its own. It is not the expected answer — it is a floor. A
    #: diagnosis is allowed to offer a different repair, but not one that pays less than a
    #: repair anyone could have found by hand, because "here is a smaller edit that gets
    #: you further" is the whole product.
    #:
    #: The five cases of the retained-constants table. Three rows, `x` boxed to [0, 5],
    #: and a clause whose repair needs that box to move. The first two are controls with
    #: no Big-M and no envelope at all; the last three each go through a construct whose
    #: constant is derived from the box — a lowered `<>`, an L0 `norm`, a McCormick
    #: product — and so each is a way for the repair to be capped at the box it was
    #: supposed to widen. `contrib` is projected so the promised payoff can be oracled
    #: against a real re-solve instead of a hand-computed number.
    _WIDENING_CASES = {
        "control, one box width": (
            "SELECT id, x AS contrib FROM (VALUES (1),(2),(3)) t(id) "
            "DECIDE x(INT) SUCH THAT x >= 0 AND x <= 5 AND SUM(x) >= 30 "
            "MAXIMIZE SUM(x)", {}, ("x <= 5", "x <= 10"),
        ),
        "control, three box widths": (
            "SELECT id, x AS contrib FROM (VALUES (1),(2),(3)) t(id) "
            "DECIDE x(INT) SUCH THAT x >= 0 AND x <= 5 AND SUM(x) >= 60 "
            "MAXIMIZE SUM(x)", {}, ("x <= 5", "x <= 20"),
        ),
        "through a <> disjunction": (
            "SELECT id, x AS contrib FROM (VALUES (1),(2),(3)) t(id) "
            "DECIDE x(INT) SUCH THAT x >= 0 AND x <= 5 AND x <> 3 AND SUM(x) >= 60 "
            "MAXIMIZE SUM(x)", {}, ("x <= 5", "x <= 20"),
        ),
        "through an L0 norm": (
            "SELECT id, x AS contrib FROM (VALUES (1),(2),(3)) t(id) "
            "DECIDE x(INT) SUCH THAT x >= 0 AND x <= 5 AND norm(x, 0) <= 2 "
            "AND SUM(x) >= 30 MAXIMIZE SUM(x)",
            {"NORM(x, 0) <= 2": "norm(x, 0) <= 2"}, ("x <= 5", "x <= 15"),
        ),
        "through a McCormick envelope": (
            "SELECT id, c * b * x AS contrib FROM (VALUES (1,1.0),(2,1.0),(3,1.0)) t(id,c) "
            "DECIDE b(BOOL), x(INT) SUCH THAT x >= 0 AND x <= 5 AND SUM(b * x) >= 30 "
            "MAXIMIZE SUM(c * b * x)", {}, ("x <= 5", "x <= 10"),
        ),
    }

    @pytest.mark.parametrize("case", sorted(_WIDENING_CASES))
    def test_widening_advice_does_not_depend_on_the_backend(
        self, request, case, decidb_cli_gurobi, decidb_cli_highs
    ):
        """The sharpest available oracle for a stale constant: whether the two hosts
        agree. A constant baked at the unrepaired box caps the repair, and the two
        backends do not bake the same ones — Gurobi states `<>` natively and derives no
        Big-M for it, HiGHS lowers it and derives one — so a capped repair shows up as
        the same query being told two different things. Deriving the constants against
        the box the repair actually searches is what makes the two answers one."""
        sql, _, _ = self._WIDENING_CASES[case]
        arms = {
            "gurobi (native)": decidb_cli_gurobi,
            "gurobi (lowered)": decidb_cli_gurobi.with_env(
                {"DECIDB_NATIVE_CONSTRUCTS": "off"}),
            "highs": decidb_cli_highs,
        }
        by_backend = {}
        for label, runner in arms.items():
            rows = _rows(_diagnose(runner, sql))
            by_backend[label] = {
                "edits": _clause_edits(rows),
                "achievable_objective": _attrs(rows, "model", "NULL").get(
                    "achievable_objective"),
            }
        assert_backends_agree(by_backend, f"a repair that widens the box — {case}")

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    @pytest.mark.parametrize("case", sorted(_WIDENING_CASES))
    def test_the_widened_payoff_survives_a_re_solve(self, request, case, cli_fixture):
        """The promised payoff has to be one the repaired query actually delivers.

        This is what a stale constant gets wrong quietly: the repair it offers is valid
        (applying it does make the query solve), so only the number is off — the engine
        reports what the capped model could reach rather than what the widened one can.
        Re-solving the edited query is the only check that catches that, and it needs no
        hand-computed answer."""
        sql, subject_to_sql, _ = self._WIDENING_CASES[case]
        cli = request.getfixturevalue(cli_fixture)
        rows = _rows(_diagnose(cli, sql))
        reported = _attrs(rows, "model", "NULL")["achievable_objective"]
        fixed_sql = _apply_reported_fix(cli, sql, rows, subject_to_sql)
        solved = list(csv.DictReader(io.StringIO(
            cli.execute_script(".mode csv\n" + fixed_sql + ";\n").stdout)))
        assert float(reported) == pytest.approx(
            sum(float(r["contrib"]) for r in solved)), (
            f"promised {reported} for:\n{fixed_sql}\n{solved}")

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    @pytest.mark.parametrize("case", sorted(_WIDENING_CASES))
    def test_the_repair_is_not_beaten_by_a_witness(self, request, case, cli_fixture):
        """A repair capped at the box it was meant to widen is still a VALID repair —
        apply it and the query solves — so nothing about the edited query catches it.
        What catches it is a second repair that anyone could write down, solved for real:
        if that one reaches further, the diagnosis left value on the table and reported a
        payoff smaller than the query can actually deliver.

        This is the only check here that covers the McCormick row. Both backends lower a
        product the same way, so both were wrong by the same amount and agreed with each
        other while doing it."""
        sql, _, (before, after) = self._WIDENING_CASES[case]
        cli = request.getfixturevalue(cli_fixture)

        assert before in sql, f"witness edit {before!r} is not in:\n{sql}"
        witness_sql = sql.replace(before, after, 1)
        witness = cli.execute_script(".mode csv\n" + witness_sql + ";\n")
        witness_rows = list(csv.DictReader(io.StringIO(witness.stdout)))
        assert witness_rows, (
            f"the witness repair does not solve, so it cannot bound anything:\n"
            f"{witness_sql}\n{witness.stderr}")
        witness_objective = sum(float(r["contrib"]) for r in witness_rows)

        rows = _rows(_diagnose(cli, sql))
        reported = float(_attrs(rows, "model", "NULL")["achievable_objective"])
        assert reported >= witness_objective - 1e-6 * max(1.0, abs(witness_objective)), (
            f"the diagnosis promises {reported}, but `{before}` → `{after}` alone reaches "
            f"{witness_objective}:\n{sql}\n{rows}")

    #: One large-coefficient row beside a small repairable one. The repair is `x <= 7`,
    #: an edit of 2, while the model's own scale is ~1e8 — so any noise floor taken from
    #: the model rather than from the repair erases it.
    _SMALL_EDIT_SQL = (
        "SELECT id, x AS contrib FROM (VALUES (1,1000000.0),(2,1000000.0),(3,1000000.0)) t(id,big) "
        "DECIDE x(INT) SUCH THAT x >= 0 AND x <= 5 AND SUM(big * x) <= 100000000 "
        "AND SUM(x) >= 20 MAXIMIZE SUM(x)"
    )

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_a_small_edit_survives_beside_a_large_coefficient_row(self, request, cli_fixture):
        """Widening the box costs conditioning, so the readback has to tell a backend's
        rounding noise from a real edit. It must do that relative to the REPAIR and never
        relative to the model: here the repair is two units and the model's scale is a
        hundred million, and a floor derived from the latter reports "no loosening
        restores feasibility" about a query that is one `x <= 7` away from solving."""
        cli = request.getfixturevalue(cli_fixture)
        rows = _rows(_diagnose(cli, self._SMALL_EDIT_SQL))
        edits = _clause_edits(rows)
        assert edits, f"the repair was filtered away as noise:\n{rows}"
        reported = float(_attrs(rows, "model", "NULL")["achievable_objective"])
        fixed_sql = _apply_reported_fix(cli, self._SMALL_EDIT_SQL, rows)
        solved = list(csv.DictReader(io.StringIO(
            cli.execute_script(".mode csv\n" + fixed_sql + ";\n").stdout)))
        assert reported == pytest.approx(sum(float(r["contrib"]) for r in solved))

    #: Two independent conflicts in one query, four orders of magnitude apart. Both halves
    #: have to be reported: applying one without the other leaves the query infeasible.
    _MIXED_SCALE_SQL = (
        "SELECT id, x, y FROM (VALUES (1)) t(id) DECIDE x(REAL), y(REAL) "
        "SUCH THAT x <= 0.001 AND x >= 0.002 AND y <= 1 AND y >= 20000 "
        "MAXIMIZE SUM(x + y)"
    )

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_a_repair_spanning_four_orders_of_magnitude_keeps_both_halves(
        self, request, cli_fixture
    ):
        """The noise floor that separates a real edit from a backend's rounding has to be
        absolute. Measured against the largest edit in the same repair, the `x <= 0.001`
        half of this one is five orders of magnitude down and gets read as noise — and the
        `y` edit alone does not restore feasibility, so the user is handed a repair that
        does not work. `_apply_reported_fix` re-solves, which is what catches it."""
        cli = request.getfixturevalue(cli_fixture)
        rows = _rows(_diagnose(cli, self._MIXED_SCALE_SQL))
        subjects = sorted(e["subject"] for e in _clause_edits(rows))
        assert subjects == ["x <= 0.001", "y <= 1"], rows
        _apply_reported_fix(cli, self._MIXED_SCALE_SQL, rows)

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_the_reported_repair_is_the_one_worth_more(self, request, cli_fixture):
        """Of the two equally small repairs, the reported one is the better repair —
        and its stated payoff survives a real re-solve of the edited query."""
        cli = request.getfixturevalue(cli_fixture)
        rows = _rows(_diagnose(cli, self._ABS_SQL))
        edits = _clause_edits(rows)
        assert len(edits) == 1, f"one clause, one edit:\n{rows}"
        assert edits[0]["subject"] == "x <= 1", rows
        assert edits[0]["suggested_change"] == "x <= 5", rows

        reported = _attrs(rows, "model", "NULL")["achievable_objective"]
        fixed_sql = _apply_reported_fix(cli, self._ABS_SQL, rows)
        solved = list(csv.DictReader(io.StringIO(
            cli.execute_script(".mode csv\n" + fixed_sql + ";\n").stdout)))
        assert float(reported) == pytest.approx(
            sum(float(r["contrib"]) for r in solved))

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_the_abs_clause_wins_when_it_is_the_smaller_edit(self, request, cli_fixture):
        """The rule is not "always prefer the bound".

        Weighting `x` inside the ABS breaks the symmetry that made the two repairs tie:
        reaching 5 through `ABS(x * 0.5)` needs the box to travel to 10 (an edit of 9),
        while the clause itself only has to come down to 0.5 (an edit of 4.5). The
        clause is now the smallest edit, so it is the one reported — no tie-break is
        even consulted."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, x * 6 AS contrib FROM (VALUES (1)) t(id) "
            "DECIDE x(REAL) SUCH THAT x >= -1 AND x <= 1 AND ABS(x * 0.5) >= 5 "
            "MAXIMIZE SUM(x * 6)"
        )
        rows = _rows(_diagnose(cli, sql))
        edits = _clause_edits(rows)
        assert len(edits) == 1, f"one clause, one edit:\n{rows}"
        assert edits[0]["subject"] == "ABS(x * 0.5) >= 5", rows
        assert float(edits[0]["amount"]) == pytest.approx(4.5), rows
        _apply_reported_fix(cli, sql, rows)

    def test_a_minmax_tie_goes_to_the_repair_worth_more(
        self, decidb_cli_gurobi, decidb_cli_highs
    ):
        """The same blind spot on MIN/MAX, where it never showed as a disagreement.

        `MAX(x + c) >= 25 PER g` over `x` in [0, 9] boxes the extremum column at the
        family's own reach and sizes the closing Big-M off its span, so widening
        `x <= 9` was unrepresentable on EVERY arm — native included, because the
        extremum column is boxed the same way whichever formulation pins it. All three
        agreed, and all three advised loosening the MAX for a query worth 36 when
        `x <= 23` was the same size of edit and worth 92."""
        sql = (
            "SELECT g, x FROM (VALUES (0,1),(0,2),(1,3),(1,4)) t(g,c) "
            "DECIDE x(REAL) SUCH THAT x >= 0 AND x <= 9 AND MAX(x + c) >= 25 PER g "
            "MAXIMIZE SUM(x)"
        )
        # `force` earns its place here: MIN/MAX is native only where the Big-M is
        # underivable, so this clause takes the LOWERING on the default arm and the
        # native path is otherwise never reached. It caps differently — a general
        # constraint states `z = MAX(t..)` as an equality, so the extremum column has to
        # hold whatever the members reach rather than just the bound — and it kept
        # reporting an objective of 89 after the lowered arms already read 92.
        arms = {
            "gurobi (lowered)": decidb_cli_gurobi.with_env(
                {"DECIDB_NATIVE_CONSTRUCTS": "off"}),
            "gurobi (native)": decidb_cli_gurobi.with_env(
                {"DECIDB_NATIVE_CONSTRUCTS": "force"}),
            "highs": decidb_cli_highs,
        }
        for label, runner in arms.items():
            rows = _rows(_diagnose(runner, sql))
            edits = _clause_edits(rows)
            assert len(edits) == 1, f"{label}: one clause, one edit:\n{rows}"
            assert edits[0]["subject"] == "x <= 9", f"{label}:\n{rows}"

            reported = _attrs(rows, "model", "NULL")["achievable_objective"]
            fixed_sql = _apply_reported_fix(runner, sql, rows)
            solved = list(csv.DictReader(io.StringIO(
                runner.execute_script(".mode csv\n" + fixed_sql + ";\n").stdout)))
            assert float(reported) == pytest.approx(
                sum(float(r["x"]) for r in solved)), label
