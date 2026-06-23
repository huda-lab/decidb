"""Unbounded `escaping_instances` — characterize WHICH instances of a variable escape.

When a variable's name fans out into many scope-instances (row-scoped: one column
per result row; entity-scoped: one per entity) and only some escape, the renamed
`escaping_instances` attribute of `decide_diagnostics()` describes the escaping set with
categorical, sufficient-direction rules — `when c=v, the variable escapes in a of b
instances` (rendered `c=v (a/b)`) for every categorical (column, value) whose
within-group escape rate clears the threshold. Total escape collapses to
`all N instances escape`; a single-instance variable or a scattered escape that no
categorical group characterizes falls back to the bare `a of b instances escape`.

Cases are constructed so the escaping slice is known by construction; the
characterization string is asserted directly (the `diagnosis ready` pointer on
stderr confirms the solve was classified UNBOUNDED). Runs under both backends.
"""

import csv
import io

import pytest

from solver.types import ObjSense, SolverStatus, VarType


_BACKENDS = ["decidb_cli_highs", "decidb_cli_gurobi"]


def _diagnose(cli, decide_sql, setup="", extra_pragmas=""):
    """PRAGMA + optional setup + a failing DECIDE + the relation read, one session."""
    script = (
        ".mode csv\n"
        f"{setup}"
        "PRAGMA diagnose_decide='auto';\n"
        f"{extra_pragmas}"
        f"{decide_sql};\n"
        "SELECT * FROM decide_diagnostics();\n"
    )
    return cli.execute_script(script)


def _rows(result):
    return list(csv.DictReader(io.StringIO(result.stdout)))


def _attr(rows, subject, attribute):
    for row in rows:
        if (
            row["subject_kind"] == "variable"
            and row["subject"] == subject
            and row["attribute"] == attribute
        ):
            return row["value"]
    raise AssertionError(f"missing attribute {attribute} for {subject}: {rows}")


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
    "DECIDE buy IS REAL SUCH THAT buy <= 100 WHEN channel = 'domestic' "
    "MAXIMIZE SUM(buy * margin)"
)


@pytest.mark.query_diagnostics
class TestEscapingInstances:
    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_row_scoped_partial_escape_rule(self, request, cli_fixture):
        """Only one categorical value's rows escape -> a single sufficient rule."""
        cli = request.getfixturevalue(cli_fixture)
        rows = _rows(_diagnose(cli, _ROW_PARTIAL))
        assert _attr(rows, "buy", "escaping_instances") == "channel=export (20/20)"

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_total_escape_summary(self, request, cli_fixture):
        """Every instance escaping collapses to the total-escape summary."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, buy FROM ("
            "SELECT i AS id, i * 1.0 AS margin FROM range(1, 101) t(i)) "
            "DECIDE buy IS REAL SUCH THAT buy >= 0 MAXIMIZE SUM(buy * margin)"
        )
        rows = _rows(_diagnose(cli, sql))
        assert _attr(rows, "buy", "escaping_instances") == "all 100 instances escape"

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_scattered_escape_falls_back_to_count(self, request, cli_fixture):
        """When no categorical group clears the threshold, report the bare count."""
        cli = request.getfixturevalue(cli_fixture)
        # parity splits 50/50 but the escaping half (id>50) is half of each parity
        # group -> 0.5 rate, below the 0.8 default. id is high-cardinality (excluded).
        sql = (
            "SELECT id, buy FROM ("
            "SELECT i AS id, (i % 2) AS parity, i * 1.0 AS w FROM range(1, 101) t(i)) "
            "DECIDE buy IS REAL SUCH THAT buy <= 100 WHEN id <= 50 MAXIMIZE SUM(buy * w)"
        )
        rows = _rows(_diagnose(cli, sql))
        assert _attr(rows, "buy", "escaping_instances") == "50 of 100 instances escape"

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
            "DECIDE e.hire IS REAL SUCH THAT hire <= 50 WHEN dept = 'B' "
            "MAXIMIZE SUM(hire * eid)"
        )
        rows = _rows(_diagnose(cli, sql))
        assert _attr(rows, "hire", "escaping_instances") == "dept=A (10/10)"

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_select_only_column_is_not_named_colN(self, request, cli_fixture, oracle_solver):
        """C6: a low-cardinality column referenced only in the outer SELECT (never in
        the DECIDE clause) has no source name we ever resolved. Even when it perfectly
        characterizes the escaping slice, we suppress the rule rather than printing a
        positional `colN` the user never wrote -> the report falls back to the count.

        Here `zone` splits rows 1-25 ('A') vs 26-100 ('B'); the cap is driven by `id`,
        so exactly the zone='A' rows escape. `zone` is selected but never referenced in
        WHEN/objective/decision, so it carries no harvested name."""
        _assert_oracle_unbounded(
            oracle_solver, "diagnostic_select_only_column", 100, range(1, 26)
        )
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT id, zone, buy FROM ("
            "SELECT i AS id, CASE WHEN i <= 25 THEN 'A' ELSE 'B' END AS zone, "
            "i * 1.0 AS w FROM range(1, 101) t(i)) "
            "DECIDE buy IS REAL SUCH THAT buy <= 100 WHEN id > 25 "
            "MAXIMIZE SUM(buy * w)"
        )
        rows = _rows(_diagnose(cli, sql))
        value = _attr(rows, "buy", "escaping_instances")
        assert value == "25 of 100 instances escape", value
        assert "col" not in value, f"positional colN leaked into report: {value}"

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
            "DECIDE buy IS REAL SUCH THAT buy <= 100 WHEN (category = 'Q' OR id <= 60) "
            "MAXIMIZE SUM(buy * w)"
        )
        default = _rows(_diagnose(cli, sql))
        assert _attr(default, "buy", "escaping_instances") == "90 of 300 instances escape"

        lowered = _rows(_diagnose(cli, sql, extra_pragmas="PRAGMA diagnose_decide_escape_rate=0.5;\n"))
        assert _attr(lowered, "buy", "escaping_instances") == "category=P (90/150)"

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
            "DECIDE buy IS REAL SUCH THAT buy <= 100 WHEN (id > 4 OR bucket = 'never') "
            "MAXIMIZE SUM(buy * w)"
        )
        default = _rows(_diagnose(cli, sql))
        assert _attr(default, "buy", "escaping_instances") == "4 of 100 instances escape"

        raised = _rows(
            _diagnose(
                cli,
                sql,
                extra_pragmas="PRAGMA diagnose_decide_categorical_ratio=0.25;\n",
            )
        )
        assert _attr(raised, "buy", "escaping_instances") == "bucket=target (4/4)"

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
            "DECIDE buy IS REAL SUCH THAT buy <= 100 WHEN (id > 8 OR segment = 'never') "
            "MAXIMIZE SUM(buy * w)"
        )
        default = _rows(_diagnose(cli, sql))
        assert _attr(default, "buy", "escaping_instances") == "segment=target (8/8)"

        lowered = _rows(
            _diagnose(
                cli,
                sql,
                extra_pragmas="PRAGMA diagnose_decide_min_categories=10;\n",
            )
        )
        assert _attr(lowered, "buy", "escaping_instances") == "8 of 120 instances escape"
