"""Diagnostics on real TPC-H data (sf=0.01) — richer than the constructed
2-tuple / range() cases in the other diagnostics suites.

Real joins produce *heterogeneous* per-row / per-group RHS, which is what makes
the interesting diagnostic branches fire naturally: only some rows escape
(row-scoped unbounded), only some PER groups are infeasible (per-group loosen),
per-row data conflicts. These cases were surfaced by a Phase-0 exploration over
`decidb.db` (the TPC-H CLI fixture DB).

Data-dependent expectations (row counts, the failing-group set, the binding
literal) are **computed from the same database** rather than hard-coded, so the
assertions stay valid if the fixture data changes.

Four branches are solid and asserted directly (A row escape, C total escape,
D per-group loosen, F per-row conflict). Four are known gaps, logged in
`context/descriptions/07_issues/` and marked `xfail` here — the test states the
*desired* behavior so the marker can be removed when the gap is closed:

  * B  entity-scoped escape ignores join-partner columns   -> bugs/todo.md
  * D2 headline enumerates every failing group (no cap)     -> code_quality/todo.md
  * E  least-change reports a degenerate "require nothing"  -> bugs/todo.md
  * G  `drop` edit duplicated once per per-row `<>` row      -> code_quality/todo.md

Runs under both backends.
"""

import csv
import io
import re

import pytest


_BACKENDS = ["decidb_cli_highs", "decidb_cli_gurobi"]


# --------------------------------------------------------------------------- #
# helpers
# --------------------------------------------------------------------------- #
def _diagnose(cli, decide_sql, extra_pragmas=""):
    """Run a failing DECIDE under `auto`, then read the diagnostics relation.

    Returns the raw CompletedProcess: stderr carries the headline, stdout the
    CSV of `decide_diagnostics()` (the DECIDE itself errors to stderr, the
    session continues to the SELECT).
    """
    script = (
        ".mode csv\n"
        "PRAGMA diagnose_decide='auto';\n"
        f"{extra_pragmas}"
        f"{decide_sql};\n"
        "SELECT * FROM decide_diagnostics();\n"
    )
    return cli.execute_script(script)


def _rows(result):
    return list(csv.DictReader(io.StringIO(result.stdout)))


def _headline(result):
    """The single diagnosis line on stderr (drops the `Details:` pointer)."""
    for line in result.stderr.splitlines():
        if "DECIDE optimization" in line:
            return line.strip()
    raise AssertionError(f"no diagnosis headline on stderr:\n{result.stderr}")


def _attr(rows, subject_kind, attribute, subject=None):
    for row in rows:
        if (
            row["subject_kind"] == subject_kind
            and row["attribute"] == attribute
            and (subject is None or row["subject"] == subject)
        ):
            return row["value"]
    raise AssertionError(f"missing {subject_kind}/{attribute} ({subject}): {rows}")


def _csv_query(cli, sql):
    """Run a plain SQL query and return a list of dict rows (CSV mode)."""
    result = cli.execute_script(f".mode csv\n{sql};\n")
    return list(csv.DictReader(io.StringIO(result.stdout)))


def _scalar(cli, sql):
    """First column of the first row of `sql`, as a string."""
    rows = _csv_query(cli, sql)
    return next(iter(rows[0].values()))


# --------------------------------------------------------------------------- #
# solid branches — asserted directly
# --------------------------------------------------------------------------- #
@pytest.mark.query_diagnostics
class TestSolidBranches:
    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_A_unbounded_partial_row_escape_names_real_column(self, request, cli_fixture):
        """Only l_shipmode='AIR' rows are uncapped -> they escape, and the
        characterization names the real column and the exact slice."""
        cli = request.getfixturevalue(cli_fixture)
        n_air = _scalar(
            cli,
            "SELECT count(*) FROM lineitem WHERE l_orderkey <= 300 AND l_shipmode = 'AIR'",
        )
        sql = (
            "SELECT l_orderkey, l_linenumber, buy FROM lineitem WHERE l_orderkey <= 300 "
            "DECIDE buy IS REAL SUCH THAT buy <= 100 WHEN l_shipmode <> 'AIR' "
            "MAXIMIZE SUM(buy * l_extendedprice)"
        )
        rows = _rows(_diagnose(cli, sql))
        assert _attr(rows, "variable", "grows_toward") == "+inf"
        assert (
            _attr(rows, "variable", "affected_rows")
            == f"{n_air} of {n_air} rows where l_shipmode = 'AIR'"
        )

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_C_unbounded_total_escape_at_scale(self, request, cli_fixture):
        """Every row uncapped -> collapses to the total-escape summary, legible
        at ~1000 rows."""
        cli = request.getfixturevalue(cli_fixture)
        n = _scalar(cli, "SELECT count(*) FROM lineitem WHERE l_orderkey <= 1000")
        sql = (
            "SELECT l_orderkey, l_linenumber, buy FROM lineitem WHERE l_orderkey <= 1000 "
            "DECIDE buy IS REAL SUCH THAT buy >= 0 MAXIMIZE SUM(buy * l_extendedprice)"
        )
        rows = _rows(_diagnose(cli, sql))
        assert _attr(rows, "variable", "affected_rows") == f"all {n} rows"

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_D_per_group_loosen_only_failing_groups(self, request, cli_fixture):
        """SUM(x) >= 5 PER l_orderkey: exactly the orders with < 5 line items are
        infeasible. Each failing group gets its own loosen edit, keyed by the
        real order key, with amount = 5 - group_size."""
        cli = request.getfixturevalue(cli_fixture)
        # group_key -> line count, for the orders that cannot reach 5
        sizes = {
            r["l_orderkey"]: int(r["n"])
            for r in _csv_query(
                cli,
                "SELECT l_orderkey, count(*) AS n FROM lineitem WHERE l_orderkey <= 40 "
                "GROUP BY l_orderkey HAVING count(*) < 5",
            )
        }
        total_rows = _scalar(cli, "SELECT count(*) FROM lineitem WHERE l_orderkey <= 40")

        sql = (
            "SELECT l_orderkey, x FROM lineitem WHERE l_orderkey <= 40 "
            "DECIDE x IS BOOLEAN SUCH THAT SUM(x) >= 5 PER l_orderkey MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql)
        rows = _rows(result)

        assert "grouped clause `SUM(x) >= 5 PER l_orderkey`" in _headline(result)

        # the set of groups carrying a loosen edit == the set of undersized orders
        reported = {r["value"] for r in rows if r["attribute"] == "group"}
        assert reported == set(sizes)

        # per-group edit is a loosen with the right amount and suggested floor
        for gkey, size in sizes.items():
            subj = f"SUM(x) >= 5 PER l_orderkey [group: {gkey}]"
            assert _attr(rows, "clause", "edit_kind", subject=subj) == "loosen"
            assert _attr(rows, "clause", "amount", subject=subj) == str(5 - size)
            assert _attr(rows, "clause", "suggested_change", subject=subj) == (
                f"SUM(x) >= {size} PER l_orderkey"
            )

        # once loosened, MAXIMIZE SUM(x) can select every row
        assert _attr(rows, "model", "achievable_objective") == total_rows

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_F_per_row_conflict_summary(self, request, cli_fixture):
        """x BOOLEAN but x >= l_quantity: per-row data RHS with no single literal
        to loosen -> a conflict summary. The headline names a representative
        `x >= K` clause, the relation echoes that same clause with edit_kind
        'conflict', and the conflict count's denominator is the row count."""
        cli = request.getfixturevalue(cli_fixture)
        total = _scalar(cli, "SELECT count(*) FROM lineitem WHERE l_orderkey <= 20")

        sql = (
            "SELECT l_orderkey, x FROM lineitem WHERE l_orderkey <= 20 "
            "DECIDE x IS BOOLEAN SUCH THAT x >= l_quantity MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql)
        rows = _rows(result)

        # the engine chooses the representative literal K itself; take it from the
        # headline rather than predicting it, then assert the relation agrees.
        hm = re.search(r"per-row data in clause `(x >= \d+)`", _headline(result))
        assert hm, _headline(result)
        subj = hm.group(1)
        assert _attr(rows, "clause", "edit_kind", subject=subj) == "conflict"
        cm = re.fullmatch(
            r"conflicts in (\d+) of (\d+) rows",
            _attr(rows, "clause", "conflict", subject=subj),
        )
        assert cm and cm.group(2) == total


# --------------------------------------------------------------------------- #
# known gaps — desired behavior asserted, xfail until the logged issue is fixed
# --------------------------------------------------------------------------- #
@pytest.mark.query_diagnostics
class TestKnownGaps:
    @pytest.mark.xfail(
        reason="bugs/todo.md: entity-scoped escape ignores join-partner columns",
        strict=False,
    )
    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_B_entity_escape_should_name_join_column(self, request, cli_fixture):
        """GERMANY suppliers escape; the characterization should name n_name even
        though it comes from the joined `nation` table. Today it falls back to a
        bare entity count."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT s.s_suppkey, keep FROM supplier s "
            "JOIN nation n ON s.s_nationkey = n.n_nationkey "
            "DECIDE s.keep IS REAL SUCH THAT keep <= 50 WHEN n.n_name <> 'GERMANY' "
            "MAXIMIZE SUM(keep)"
        )
        rows = _rows(_diagnose(cli, sql))
        assert "n_name = 'GERMANY'" in _attr(rows, "variable", "affected_entities")

    @pytest.mark.xfail(
        reason="code_quality/todo.md: headline enumerates every failing PER group (no cap)",
        strict=False,
    )
    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_D2_many_group_headline_should_be_capped(self, request, cli_fixture):
        """With dozens of failing groups the headline should summarize + truncate
        (e.g. '... and N more'), not list every group key inline."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT l_orderkey, x FROM lineitem WHERE l_orderkey <= 400 "
            "DECIDE x IS BOOLEAN SUCH THAT SUM(x) >= 5 PER l_orderkey MAXIMIZE SUM(x)"
        )
        headline = _headline(_diagnose(cli, sql))
        # a capped headline references the overflow instead of enumerating all groups
        assert "more" in headline

    @pytest.mark.xfail(
        reason="bugs/todo.md: least-change reports a degenerate 'require nothing' edit",
        strict=False,
    )
    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_E_loosen_should_not_be_degenerate(self, request, cli_fixture):
        """The tight constraint is the dollar budget; loosening the count floor to
        0 (select nothing, objective 0) is feasible but useless. A good edit keeps
        a nonzero achievable objective."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT l_orderkey, buy FROM lineitem WHERE l_orderkey <= 100 "
            "DECIDE buy IS BOOLEAN "
            "SUCH THAT SUM(buy) >= 30 AND SUM(buy * l_extendedprice) <= 100 "
            "MAXIMIZE SUM(buy)"
        )
        rows = _rows(_diagnose(cli, sql))
        assert int(_attr(rows, "model", "achievable_objective")) > 0

    @pytest.mark.xfail(
        reason="code_quality/todo.md: `drop` edit duplicated once per per-row `<>` row",
        strict=False,
    )
    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_G_drop_edit_should_be_deduplicated(self, request, cli_fixture):
        """A single `x <> 1` clause over an order with several line items should
        yield one `drop` edit, not one per row."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT l_orderkey, x FROM lineitem WHERE l_orderkey = 1 "
            "DECIDE x IS BOOLEAN SUCH THAT x <> 0 AND x <> 1 MINIMIZE SUM(x)"
        )
        rows = _rows(_diagnose(cli, sql))
        drops = [r for r in rows if r["attribute"] == "edit_kind" and r["value"] == "drop"]
        assert len(drops) == 1
