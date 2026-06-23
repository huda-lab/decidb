"""Unbounded `escaping_instances` — characterize WHICH instances of a variable escape.

When a variable's name fans out into many scope-instances (row-scoped: one column
per result row; entity-scoped: one per entity) and only some escape, the renamed
`escaping_instances` cell of `decide_diagnostics()` describes the escaping set with
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


_BACKENDS = ["decidb_cli_highs", "decidb_cli_gurobi"]


def _diagnose(cli, decide_sql, setup="", extra_pragmas=""):
    """PRAGMA + optional setup + a failing DECIDE + the relation read, one session."""
    script = (
        ".mode csv\n"
        f"{setup}"
        "PRAGMA diagnose_decide='unbounded';\n"
        f"{extra_pragmas}"
        f"{decide_sql};\n"
        "SELECT * FROM decide_diagnostics();\n"
    )
    return cli.execute_script(script)


def _rows(result):
    return list(csv.DictReader(io.StringIO(result.stdout)))


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
        assert len(rows) == 1
        assert rows[0]["variable"] == "buy"
        assert rows[0]["escaping_instances"] == "channel=export (20/20)"

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
        assert rows[0]["escaping_instances"] == "all 100 instances escape"

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
        assert rows[0]["escaping_instances"] == "50 of 100 instances escape"

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
        assert len(rows) == 1
        assert rows[0]["variable"] == "hire"
        assert rows[0]["escaping_instances"] == "dept=A (10/10)"

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
        assert default[0]["escaping_instances"] == "90 of 300 instances escape"

        lowered = _rows(_diagnose(cli, sql, extra_pragmas="PRAGMA diagnose_decide_escape_rate=0.5;\n"))
        assert lowered[0]["escaping_instances"] == "category=P (90/150)"
