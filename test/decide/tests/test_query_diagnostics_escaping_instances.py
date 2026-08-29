"""Unbounded escape characterization — which rows/entities of a variable escape.

When a variable's name fans out into many scope-instances (row-scoped: one column per
result row; entity-scoped: one per entity) and only some escape, `DIAGNOSE` breaks the
escape out by categorical slice: one finding per (column, value) whose within-group
escape rate clears the threshold, naming the slice in `group`, counting escaping
instances in `amount`, retaining the denominator in `total`, and identifying rows vs
entities in `scope`. Total escape collapses to one finding covering every instance; a
single-instance variable or a scattered escape that no categorical group characterizes
reports the bare count with no slice named.

Cases are constructed so the escaping slice is known by construction, and the count and
slice are asserted directly. Runs under both backends.
"""

import csv
import io

import pytest

from solver.types import ObjSense, SolverStatus, VarType

from . import _diagnose_relation


_BACKENDS = ["decidb_cli_highs", "decidb_cli_gurobi"]


def _diagnose(cli, decide_sql, setup="", extra_pragmas=""):
    """`DIAGNOSE <query>`, with optional setup and tuning pragmas, in one session."""
    return _diagnose_relation.run(cli, decide_sql, setup=setup, pragmas=extra_pragmas)


def _rows(result):
    return _diagnose_relation.rows(result)


def _remedy(rows, variable):
    """The prescribed `suggested_change`, which every finding for a variable shares."""
    changes = {
        r["suggested_change"]
        for r in rows
        if r["clause"] == variable and (r["edit_source"] or "").startswith("runaway_")
    }
    assert len(changes) == 1, f"remedy for {variable} is not single-valued: {changes}"
    return changes.pop()


def _escape(rows, variable):
    """The escape characterization as `(count, total, scope, slice)` per finding.

    `total` is the denominator for the reported count: the slice size when `slice` is
    present, otherwise the variable's total instance count."""
    found = [
        (
            int(float(r["amount"])) if r["amount"] not in ("", "NULL") else None,
            int(r["total"]) if r["total"] not in ("", "NULL") else None,
            "" if r["scope"] in ("", "NULL") else r["scope"],
            "" if r["group"] in ("", "NULL") else r["group"],
        )
        for r in rows
        if r["clause"] == variable and (r["edit_source"] or "").startswith("runaway_")
    ]
    assert found, f"no escape reported for {variable}: {rows}"
    return found


def _assert_oracle_unbounded(oracle_solver, model_name, total_rows, uncapped_rows):
    """Confirm the constructed DECIDE case is actually unbounded."""
    uncapped = set(uncapped_rows)
    oracle_solver.create_model(model_name)
    objective = {}
    for i in range(1, total_rows + 1):
        name = f"buy_{i}"
        oracle_solver.add_variable(name, VarType.CONTINUOUS, lb=0.0)
        if i not in uncapped:
            oracle_solver.add_constraint({name: 1.0}, "<=", 100.0, name=f"cap_{i}")
        objective[name] = float(i)
    oracle_solver.set_objective(objective, ObjSense.MAXIMIZE)
    result = oracle_solver.solve()
    assert result.status == SolverStatus.UNBOUNDED, (
        f"oracle status for {model_name}: {result.status}"
    )


# 100 rows: only the 20 channel='export' rows are uncapped, so exactly that slice
# escapes -> a clean sufficient rule. id/margin are high-cardinality (excluded).
_ROW_PARTIAL = (
    "SELECT id, buy FROM ("
    "SELECT i AS id, CASE WHEN i % 5 = 0 THEN 'export' ELSE 'domestic' END AS channel, "
    "i * 1.0 AS margin FROM range(1, 101) t(i)) "
    "DECIDE buy(REAL) SUCH THAT buy <= 100 WHEN channel = 'domestic' "
    "MAXIMIZE SUM(buy * margin)"
)


@pytest.mark.query_diagnostics
class TestEscapingInstances:
    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_row_scoped_partial_escape_rule(self, request, cli_fixture):
        """Only one categorical value's rows escape -> a single sufficient rule."""
        cli = request.getfixturevalue(cli_fixture)
        rows = _rows(_diagnose(cli, _ROW_PARTIAL))
        assert _escape(rows, "buy") == [(20, 20, "row", "channel = 'export'")]

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_total_escape_summary(self, request, cli_fixture):
        """Every instance escaping collapses to the total-escape summary."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, buy FROM ("
            "SELECT i AS id, i * 1.0 AS margin FROM range(1, 101) t(i)) "
            "DECIDE buy(REAL) SUCH THAT buy >= 0 MAXIMIZE SUM(buy * margin)"
        )
        rows = _rows(_diagnose(cli, sql))
        # All 100 instances escape, so one finding covers them with no slice to name.
        assert _escape(rows, "buy") == [(100, 100, "row", "")]

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_scattered_escape_falls_back_to_count(self, request, cli_fixture):
        """When no categorical group clears the threshold, report the bare count."""
        cli = request.getfixturevalue(cli_fixture)
        # parity splits 50/50 but the escaping half (id>50) is half of each parity
        # group -> 0.5 rate, below the 0.8 default. id is high-cardinality (excluded).
        sql = (
            "SELECT id, buy FROM ("
            "SELECT i AS id, (i % 2) AS parity, i * 1.0 AS w FROM range(1, 101) t(i)) "
            "DECIDE buy(REAL) SUCH THAT buy <= 100 WHEN id <= 50 MAXIMIZE SUM(buy * w)"
        )
        rows = _rows(_diagnose(cli, sql))
        assert _escape(rows, "buy") == [(50, 100, "row", "")]

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_entity_scoped_partial_escape_rule(self, request, cli_fixture):
        """Entity-scoped vars are characterized by their entity-key columns.

        The CLI fixture runs read-only, so the entity table is an inline subquery
        alias rather than a CREATE TABLE. 30 entities: dept='A' (10) is uncapped and
        escapes; eid is high-cardinality (excluded), so dept is the characterizer."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT e.eid, hire FROM ("
            "SELECT i AS eid, CASE WHEN i <= 10 THEN 'A' ELSE 'B' END AS dept "
            "FROM range(1, 31) t(i)) e "
            "DECIDE e.hire(REAL) SUCH THAT hire <= 50 WHEN dept = 'B' "
            "MAXIMIZE SUM(hire * eid)"
        )
        rows = _rows(_diagnose(cli, sql))
        # Entity-scoped: the count is entities, not the rows they fan out to.
        assert _escape(rows, "hire") == [(10, 10, "entity", "dept = 'A'")]

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_entity_scoped_join_column_escape_rule(self, request, cli_fixture):
        """A joined dimension column can characterize entity-scoped escapes when
        that joined value is constant for each entity."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT e.eid, hire FROM ("
            "SELECT i AS eid, CASE WHEN i <= 10 THEN 1 ELSE 2 END AS rid "
            "FROM range(1, 31) t(i)) e "
            "JOIN (VALUES (1, 'A'), (2, 'B')) d(rid, region) ON e.rid = d.rid "
            "DECIDE e.hire(REAL) SUCH THAT hire <= 50 WHEN region = 'B' "
            "MAXIMIZE SUM(hire)"
        )
        rows = _rows(_diagnose(cli, sql))
        assert _escape(rows, "hire") == [(10, 10, "entity", "region = 'A'")]

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_select_only_aliased_column_is_named(self, request, cli_fixture, oracle_solver):
        """T2: a low-cardinality column referenced only in the outer SELECT (never in the
        DECIDE clause) is still named when the user gave it an explicit `AS` alias — that
        alias is a user-written identifier, back-filled from the child projection. The rule
        it perfectly characterizes is reported rather than suppressed to a bare count.

        Here `zone` splits rows 1-25 ('A') vs 26-100 ('B'); the cap is driven by `id`, so
        exactly the zone='A' rows escape. `zone` never appears in WHEN/objective/decision,
        so it is not harvested from the clause — only its projection alias supplies the
        name. All 25 zone='A' rows escape, so the rule reads `25 of 25 rows where zone='A'`."""
        _assert_oracle_unbounded(
            oracle_solver, "diagnostic_select_only_column", 100, range(1, 26)
        )
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, zone, buy FROM ("
            "SELECT i AS id, CASE WHEN i <= 25 THEN 'A' ELSE 'B' END AS zone, "
            "i * 1.0 AS w FROM range(1, 101) t(i)) "
            "DECIDE buy(REAL) SUCH THAT buy <= 100 WHEN id > 25 "
            "MAXIMIZE SUM(buy * w)"
        )
        rows = _rows(_diagnose(cli, sql))
        assert _escape(rows, "buy") == [(25, 25, "row", "zone = 'A'")]

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_select_only_unaliased_computed_column_stays_suppressed(
        self, request, cli_fixture, oracle_solver
    ):
        """T2 negative: the back-fill only recovers user-written names. A computed
        projection expression with NO `AS` alias carries only a generated name (its
        ToString), which we must never surface — even when it perfectly characterizes the
        escaping slice. So the same `i <= 25` split, left unaliased and pulled through
        `SELECT *`, is suppressed to the bare count rather than printing a machine name.

        Structurally identical to the aliased case (rows 1-25 escape), but the CASE has no
        alias, so the report falls back to `25 of 100 rows`."""
        _assert_oracle_unbounded(
            oracle_solver, "diagnostic_select_only_column", 100, range(1, 26)
        )
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT * FROM ("
            "SELECT i AS id, CASE WHEN i <= 25 THEN 'A' ELSE 'B' END, "
            "i * 1.0 AS w FROM range(1, 101) t(i)) "
            "DECIDE buy(REAL) SUCH THAT buy <= 100 WHEN id > 25 "
            "MAXIMIZE SUM(buy * w)"
        )
        rows = _rows(_diagnose(cli, sql))
        escape = _escape(rows, "buy")
        assert escape == [(25, 100, "row", "")], escape
        # No slice is named at all — a generated name must never surface as one.
        assert all(not slice_ for _, _, _, slice_ in escape), escape

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_escape_rate_pragma_changes_reporting(self, request, cli_fixture):
        """A group at 0.6 rate is below the 0.8 default (count fallback) but is
        reported once the escape-rate threshold is lowered to 0.5."""
        cli = request.getfixturevalue(cli_fixture)
        # 300 rows: category P = first 150; of those, id>60 (90 rows) are uncapped.
        # P escape rate = 90/150 = 0.6. id/w are high-cardinality (excluded).
        sql = (
            "SELECT id, buy FROM ("
            "SELECT i AS id, CASE WHEN i <= 150 THEN 'P' ELSE 'Q' END AS category, "
            "i * 1.0 AS w FROM range(1, 301) t(i)) "
            "DECIDE buy(REAL) SUCH THAT buy <= 100 WHEN (category = 'Q' OR id <= 60) "
            "MAXIMIZE SUM(buy * w)"
        )
        default = _rows(_diagnose(cli, sql))
        assert _escape(default, "buy") == [(90, 300, "row", "")]

        lowered = _rows(_diagnose(cli, sql, extra_pragmas="PRAGMA diagnose_decide_escape_rate=0.5;\n"))
        assert _escape(lowered, "buy") == [(90, 150, "row", "category = 'P'")]

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_categorical_ratio_pragma_changes_reporting(self, request, cli_fixture, oracle_solver):
        """A 25-value column is above the default 10% cap for 100 rows, then
        qualifies once the ratio cap is raised to 25%."""
        _assert_oracle_unbounded(
            oracle_solver, "diagnostic_categorical_ratio", 100, range(1, 5)
        )
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, buy FROM ("
            "SELECT i AS id, "
            "CASE WHEN i <= 4 THEN 'target' "
            "ELSE 'B' || CAST(((i - 5) % 24) AS VARCHAR) END AS bucket, "
            "i * 1.0 AS w FROM range(1, 101) t(i)) "
            "DECIDE buy(REAL) SUCH THAT buy <= 100 WHEN (id > 4 OR bucket = 'never') "
            "MAXIMIZE SUM(buy * w)"
        )
        default = _rows(_diagnose(cli, sql))
        assert _escape(default, "buy") == [(4, 100, "row", "")]

        raised = _rows(
            _diagnose(
                cli,
                sql,
                extra_pragmas="PRAGMA diagnose_decide_categorical_ratio=0.25;\n",
            )
        )
        assert _escape(raised, "buy") == [(4, 4, "row", "bucket = 'target'")]

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_min_categories_pragma_changes_reporting(self, request, cli_fixture, oracle_solver):
        """A 15-value column qualifies under the default floor of 20, then falls
        back to the count once the absolute floor is lowered below its cardinality."""
        _assert_oracle_unbounded(
            oracle_solver, "diagnostic_min_categories", 120, range(1, 9)
        )
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, buy FROM ("
            "SELECT i AS id, "
            "CASE WHEN i <= 8 THEN 'target' "
            "ELSE 'S' || CAST(((i - 9) % 14) AS VARCHAR) END AS segment, "
            "i * 1.0 AS w FROM range(1, 121) t(i)) "
            "DECIDE buy(REAL) SUCH THAT buy <= 100 WHEN (id > 8 OR segment = 'never') "
            "MAXIMIZE SUM(buy * w)"
        )
        default = _rows(_diagnose(cli, sql))
        assert _escape(default, "buy") == [(8, 8, "row", "segment = 'target'")]

        lowered = _rows(
            _diagnose(
                cli,
                sql,
                extra_pragmas="PRAGMA diagnose_decide_min_categories=10;\n",
            )
        )
        assert _escape(lowered, "buy") == [(8, 120, "row", "")]

    @pytest.mark.error
    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    @pytest.mark.parametrize(
        ("pragma", "value", "message"),
        [
            (
                "diagnose_decide_escape_rate",
                "0",
                "diagnose_decide_escape_rate must be in (0, 1]",
            ),
            (
                "diagnose_decide_categorical_ratio",
                "0",
                "diagnose_decide_categorical_ratio must be in (0, 1]",
            ),
            (
                "diagnose_decide_min_categories",
                "0",
                "diagnose_decide_min_categories must be >= 1",
            ),
        ],
    )
    def test_characterization_pragmas_validate_bounds(
        self, request, cli_fixture, pragma, value, message
    ):
        """Invalid unbounded-characterization knobs fail at SET time."""
        cli = request.getfixturevalue(cli_fixture)
        result = cli.execute_raw(f"PRAGMA {pragma}={value};")
        combined = (result.stderr + result.stdout).lower()
        assert message in combined


# The same six rows reached two ways. Rows 4-6 are uncapped and escape; `region` is
# SELECT-only while `id` is named by the DECIDE clause.
_SIX_ROWS = "('A',1,1),('A',2,2),('A',3,3),('B',4,4),('B',5,5),('B',6,6)"
_SAME_ROWS_DECIDE = (
    "SELECT region, buy FROM {source} "
    "DECIDE buy(REAL) SUCH THAT buy <= 100 WHEN id <= 3 "
    "MAXIMIZE SUM(buy * w)"
)


_TABLE_SETUP = (
    "CREATE TEMP TABLE items(region VARCHAR, id INTEGER, w INTEGER);\n"
    f"INSERT INTO items VALUES {_SIX_ROWS};\n"
)


@pytest.mark.query_diagnostics
class TestEscapeColumnNaming:
    """A characterized slice names the column the user wrote, whatever the rows came from."""

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_values_and_table_agree(self, request, cli_fixture):
        """The same rows as a VALUES list and as a table give identical findings.

        The two used to disagree twice over. A derived table's columns reached the
        diagnosis under the binder's positional `col0` / `col1` placeholders, because a
        `t(a, b, c)` alias list is recorded on the BindContext binding and never reaches
        the plan. A real table's columns arrived unnamed and were dropped, because the
        name back-fill only ran for a projection and a scan sits under DECIDE instead.
        Names now come from the BindContext itself, which is what resolved them."""
        cli = request.getfixturevalue(cli_fixture)
        from_values = _rows(
            _diagnose(
                cli,
                _SAME_ROWS_DECIDE.format(
                    source=f"(VALUES {_SIX_ROWS}) t(region, id, w)"
                ),
            )
        )
        from_table = _rows(
            _diagnose(cli, _SAME_ROWS_DECIDE.format(source="items"), setup=_TABLE_SETUP)
        )
        assert _escape(from_values, "buy") == _escape(from_table, "buy")
        assert _remedy(from_values, "buy") == _remedy(from_table, "buy")
        # `region` is only ever in the SELECT list, yet it is the rule that explains the
        # escape and the one that scopes the remedy — it has to survive both routes.
        assert (3, 3, "row", "region = 'B'") in _escape(from_values, "buy")

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_no_positional_column_names(self, request, cli_fixture):
        """No finding may name a `colN` the user never typed."""
        cli = request.getfixturevalue(cli_fixture)
        rows = _rows(
            _diagnose(
                cli,
                _SAME_ROWS_DECIDE.format(
                    source=f"(VALUES {_SIX_ROWS}) t(region, id, w)"
                ),
            )
        )
        named = [slice_ for _, _, _, slice_ in _escape(rows, "buy") if slice_]
        assert named, "expected at least one characterized slice"
        assert not [s for s in named if s.startswith("col")], named

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_column_names_survive_a_prepared_statement(self, request, cli_fixture):
        """Names must survive a prepared statement's replay.

        This is cache/reuse coverage, not serialization coverage: `EXECUTE` reuses the
        cached PHYSICAL plan, and the rebind path re-plans from a parse-tree copy, so no
        logical plan is ever serialized here. Running it twice proves the diagnosis still
        receives its `DecideSourceColumnName` records on the second, replayed execution.

        The serialization round trip is covered in C++, where it can be driven directly:
        `test/common/test_decidb_plan_serialization.cpp`."""
        cli = request.getfixturevalue(cli_fixture)
        decide_sql = _SAME_ROWS_DECIDE.format(source="items")
        script = (
            ".mode csv\n"
            f"{_TABLE_SETUP}"
            f"PREPARE p AS DIAGNOSE {decide_sql};\n"
            "EXECUTE p;\n"
            "EXECUTE p;\n"
        )
        rows = _rows(cli.execute_script(script))
        named = [
            r["group"] for r in rows if r.get("group") not in (None, "", "NULL")
        ]
        assert named, f"prepared replay lost every slice name: {rows}"
        assert "region = 'B'" in named, named


@pytest.mark.query_diagnostics
class TestEquivalentSlicesCollapse:
    """Columns that pick out exactly the same escaping rows state one fact, not many."""

    # 3 rows, 4 columns; the two cap='0' rows escape. `colour` partitions them exactly
    # as `cap` does, and `tag` exactly as `w` does — pure coincidence on an input this
    # narrow. `cap` and `w` are the columns the DECIDE clause names.
    _NARROW = (
        "SELECT tag, colour, buy FROM "
        "(VALUES ('a','red',5,1),('b','blue',0,2),('c','blue',0,3)) t(tag, colour, cap, w) "
        "DECIDE buy(REAL) SUCH THAT buy <= 100 WHEN cap > 0 "
        "MAXIMIZE SUM(buy * w)"
    )

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_narrow_input_collapses_to_query_columns(self, request, cli_fixture):
        """Equivalent slices collapse, and the DECIDE-clause column is the survivor."""
        cli = request.getfixturevalue(cli_fixture)
        found = _escape(_rows(_diagnose(cli, self._NARROW)), "buy")
        assert found == [
            (2, 2, "row", "cap = '0'"),
            (1, 1, "row", "w = '2'"),
            (1, 1, "row", "w = '3'"),
        ]
        # colour/tag describe the very same rows, so they are the coincidences dropped.
        slices = " ".join(slice_ for _, _, _, slice_ in found)
        assert "colour" not in slices and "tag" not in slices

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_distinct_slices_are_not_truncated(self, request, cli_fixture):
        """Collapsing equivalents must not cap the relation: distinct slices all stay.

        100 rows, and the escaping half splits into three different channel values, so
        three findings covering three different row sets must all survive."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, buy FROM ("
            "SELECT i AS id, CASE WHEN i % 10 = 0 THEN 'export' "
            "WHEN i % 10 = 1 THEN 'transit' WHEN i % 10 = 2 THEN 'bonded' "
            "ELSE 'domestic' END AS channel, i * 1.0 AS margin FROM range(1, 101) t(i)) "
            "DECIDE buy(REAL) SUCH THAT buy <= 100 WHEN channel = 'domestic' "
            "MAXIMIZE SUM(buy * margin)"
        )
        found = _escape(_rows(_diagnose(cli, sql)), "buy")
        assert sorted(found) == [
            (10, 10, "row", "channel = 'bonded'"),
            (10, 10, "row", "channel = 'export'"),
            (10, 10, "row", "channel = 'transit'"),
        ]


@pytest.mark.query_diagnostics
class TestScopedRemedy:
    """When the rules account for every escaping row, the cap is scoped to those rows."""

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_full_coverage_scopes_the_cap(self, request, cli_fixture):
        """One rule covering every escaper renders as a `WHEN`-scoped conjunct."""
        cli = request.getfixturevalue(cli_fixture)
        rows = _rows(
            _diagnose(cli, _SAME_ROWS_DECIDE.format(source="items"), setup=_TABLE_SETUP)
        )
        assert _remedy(rows, "buy") == "buy <= <cap> WHEN region = 'B'"

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_multiple_rules_render_as_a_disjunction(self, request, cli_fixture):
        """Several covering rules render as the disjunction they form.

        The brackets are load-bearing: DeciQL's `WHEN` takes a bare comparison or a
        parenthesized condition, so an unbracketed `OR` would not parse back."""
        cli = request.getfixturevalue(cli_fixture)
        setup = (
            "CREATE TEMP TABLE regions(region VARCHAR, id INTEGER, w INTEGER);\n"
            "INSERT INTO regions VALUES ('A',1,1),('A',2,2),('B',4,4),('B',5,5),('C',6,6);\n"
        )
        sql = (
            "SELECT region, buy FROM regions "
            "DECIDE buy(REAL) SUCH THAT buy <= 100 WHEN region = 'A' "
            "MAXIMIZE SUM(buy * w)"
        )
        assert (
            _remedy(_rows(_diagnose(cli, sql, setup=setup)), "buy")
            == "buy <= <cap> WHEN (region = 'B' OR region = 'C')"
        )

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_partial_coverage_keeps_the_global_cap(self, request, cli_fixture):
        """An escaper outside every rule keeps the global form.

        100 rows: the 20 'export' rows escape as a clean rule, and row 7 escapes from
        inside a large domestic group no rule characterizes. Scoping to the rules would
        leave row 7 free, so the cap must stay global."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, buy FROM ("
            "SELECT i AS id, CASE WHEN i % 5 = 0 THEN 'export' ELSE 'domestic' END AS channel, "
            "i * 1.0 AS margin FROM range(1, 101) t(i)) "
            "DECIDE buy(REAL) SUCH THAT buy <= 100 WHEN (channel = 'domestic' AND id <> 7) "
            "MAXIMIZE SUM(buy * margin)"
        )
        rows = _rows(_diagnose(cli, sql))
        assert _escape(rows, "buy") == [(20, 20, "row", "channel = 'export'")]
        assert _remedy(rows, "buy") == "buy <= <cap>"

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_scoped_remedy_pasted_back_bounds_the_query(
        self, request, cli_fixture, oracle_solver
    ):
        """The reported remedy, pasted in with a real cap, makes the query solvable —
        and only the escaping rows are capped."""
        cli = request.getfixturevalue(cli_fixture)
        remedy = _remedy(
            _rows(_diagnose(cli, _SAME_ROWS_DECIDE.format(source="items"), setup=_TABLE_SETUP)),
            "buy",
        )
        assert "WHEN" in remedy, remedy

        fixed = (
            "SELECT region, buy FROM items DECIDE buy(REAL) "
            f"SUCH THAT buy <= 100 WHEN id <= 3 AND {remedy.replace('<cap>', '7')} "
            "MAXIMIZE SUM(buy * w)"
        )
        result = cli.execute_script(f".mode csv\n{_TABLE_SETUP}{fixed};\n")
        combined = result.stdout + result.stderr
        assert "unbounded" not in combined.lower(), combined

        # An independent solve of the repaired program: rows 1-3 capped at 100 by the
        # original clause, rows 4-6 at 7 by the pasted remedy.
        oracle_solver.create_model("scoped_remedy_paste_back")
        objective = {}
        for i in range(1, 7):
            name = f"buy_{i}"
            oracle_solver.add_variable(name, VarType.CONTINUOUS, lb=0.0)
            oracle_solver.add_constraint(
                {name: 1.0}, "<=", 100.0 if i <= 3 else 7.0, name=f"cap_{i}"
            )
            objective[name] = float(i)
        oracle_solver.set_objective(objective, ObjSense.MAXIMIZE)
        expected = oracle_solver.solve()
        assert expected.status == SolverStatus.OPTIMAL, expected.status

        values = sorted(
            float(r["buy"]) for r in csv.DictReader(io.StringIO(result.stdout))
        )
        assert values == sorted(
            expected.variable_values[f"buy_{i}"] for i in range(1, 7)
        )
