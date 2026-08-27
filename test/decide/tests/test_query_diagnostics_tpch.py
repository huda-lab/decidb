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

Eight branches are asserted directly (A row escape, B entity-scoped join-column
escape, C total escape, D per-group loosen, F per-row conflict, D2 many failing groups,
G deduplicated `drop` edit, and E — scale-normalized slack weights (T1) loosen the tight
budget instead of gutting the count floor to a degenerate "require nothing").

Runs under both backends.
"""

import csv
import io
import re

import pytest

from . import _diagnose_relation


_BACKENDS = ["decidb_cli_highs", "decidb_cli_gurobi"]


# --------------------------------------------------------------------------- #
# helpers
# --------------------------------------------------------------------------- #
def _diagnose(cli, decide_sql, extra_pragmas=""):
    """`DIAGNOSE <query>` — the only thing that starts the engine — as CSV."""
    return _diagnose_relation.run(cli, decide_sql, pragmas=extra_pragmas, timeout=300)


def _flat(result):
    """The findings as the relation returns them, one dict per row."""
    return _diagnose_relation.rows(result)


def _rows(result):
    """The findings, expressed as long-form (subject, attribute, value) facts."""
    return _diagnose_relation.eav_rows(result)


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
            "DECIDE buy(REAL) SUCH THAT buy <= 100 WHEN l_shipmode <> 'AIR' "
            "MAXIMIZE SUM(buy * l_extendedprice)"
        )
        rows = _rows(_diagnose(cli, sql))
        assert _attr(rows, "variable", "grows_toward") == "+inf"
        # The slice is named by its real column, and counted.
        assert _attr(rows, "variable", "escaping_group") == "l_shipmode = 'AIR'"
        assert _attr(rows, "variable", "escaping_instances") == n_air

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_C_unbounded_total_escape_at_scale(self, request, cli_fixture):
        """Every row uncapped -> collapses to the total-escape summary, legible
        at ~1000 rows."""
        cli = request.getfixturevalue(cli_fixture)
        n = _scalar(cli, "SELECT count(*) FROM lineitem WHERE l_orderkey <= 1000")
        sql = (
            "SELECT l_orderkey, l_linenumber, buy FROM lineitem WHERE l_orderkey <= 1000 "
            "DECIDE buy(REAL) SUCH THAT buy >= 0 MAXIMIZE SUM(buy * l_extendedprice)"
        )
        rows = _rows(_diagnose(cli, sql))
        # Every row escapes, so one finding covers them all with no slice to name.
        assert _attr(rows, "variable", "escaping_instances") == n
        assert not [r for r in rows if r["attribute"] == "escaping_group"]

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_D_per_group_query_folds_expanded_breaks_out(self, request, cli_fixture):
        """SUM(x) >= 5 PER l_orderkey: exactly the orders with < 5 line items are infeasible.
        T3 query mode (default): the PER clause is ONE SQL literal, so it folds to a single
        clause-level edit whose amount is the max shortfall across failing groups. Expanded
        mode breaks it out: one expanded_group edit per failing order, amount = 5 - size."""
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
            "DECIDE x(BOOL) SUCH THAT SUM(x) >= 5 PER l_orderkey MAXIMIZE SUM(x)"
        )

        # --- query mode: one folded clause edit, amount = worst shortfall ---
        q = _rows(_diagnose(cli, sql))
        q_edits = [r for r in q if r["attribute"] == "edit_kind" and r["value"] == "loosen"]
        assert len(q_edits) == 1
        subj = "SUM(x) >= 5 PER l_orderkey"
        assert _attr(q, "clause", "edit_source", subject=subj) == "source_literal"
        assert _attr(q, "clause", "offset_scope", subject=subj) == "clause"
        assert int(float(_attr(q, "clause", "amount", subject=subj))) == max(
            5 - s for s in sizes.values())
        assert not [r for r in q if r["attribute"] == "group"]

        # --- expanded mode: one expanded_group edit per failing order ---
        e_result = _diagnose(
            cli, sql,
            extra_pragmas="PRAGMA diagnose_decide_infeasible_slack_scope='expanded';\n",
        )
        e = _rows(e_result)
        reported = {r["value"] for r in e if r["attribute"] == "group"}
        assert reported == set(sizes)
        # The clause reads as written; the group is its own column.
        by_group = {
            f["group"]: f for f in _flat(e_result) if f["edit_source"] == "expanded_group"
        }
        for gkey, size in sizes.items():
            edit = by_group[gkey]
            assert edit["clause"] == "SUM(x) >= 5 PER l_orderkey"
            assert int(float(edit["amount"])) == 5 - size
            assert edit["suggested_change"] == f"SUM(x) >= {size} PER l_orderkey"
        # once loosened, MAXIMIZE SUM(x) can select every row
        assert float(_attr(e, "model", "achievable_objective")) == float(total_rows)

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_F_per_row_data_query_mode_virtual_offset(self, request, cli_fixture):
        """T3 query mode (default): x BOOLEAN but x >= l_quantity is a per-row data RHS with
        no single literal to loosen -> ONE virtual query offset `x >= l_quantity - delta`
        (delta = max overshoot), tagged edit_source='virtual_offset'. The finding names the
        data-backed clause by its column name and carries the offset."""
        cli = request.getfixturevalue(cli_fixture)
        # A BOOLEAN can reach at most 1, so the tightest row (max l_quantity) needs a
        # -(max-1) offset. Take it from the data, not a hand-computed constant.
        expected_delta = _scalar(
            cli, "SELECT max(l_quantity) - 1 FROM lineitem WHERE l_orderkey <= 20"
        )

        sql = (
            "SELECT l_orderkey, x FROM lineitem WHERE l_orderkey <= 20 "
            "DECIDE x(BOOL) SUCH THAT x >= l_quantity MAXIMIZE SUM(x)"
        )
        result = _diagnose(cli, sql)
        rows = _rows(result)

        subj = "x >= l_quantity"
        assert _attr(rows, "clause", "edit_kind", subject=subj) == "loosen"
        assert _attr(rows, "clause", "edit_source", subject=subj) == "virtual_offset"
        amount = _attr(rows, "clause", "amount", subject=subj)
        assert float(amount) == pytest.approx(float(expected_delta))
        # the suggestion names the column and subtracts exactly that offset (>= loosens down)
        assert _attr(rows, "clause", "suggested_change", subject=subj) == f"x >= l_quantity - {amount}"

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_F_per_row_data_expanded_mode_row_profile(self, request, cli_fixture):
        """T3 expanded mode: the same data RHS stays per-row, so every conflicting row is an
        `expanded_row` profile entry rather than one folded virtual offset — a debug view of
        which generated rows are tight."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT l_orderkey, x FROM lineitem WHERE l_orderkey <= 20 "
            "DECIDE x(BOOL) SUCH THAT x >= l_quantity MAXIMIZE SUM(x)"
        )
        result = _diagnose(
            cli, sql,
            extra_pragmas="PRAGMA diagnose_decide_infeasible_slack_scope='expanded';\n",
        )
        rows = _rows(result)

        sources = {r["value"] for r in rows if r["attribute"] == "edit_source"}
        assert sources == {"expanded_row"}
        assert all(
            r["value"] == "row" for r in rows if r["attribute"] == "offset_scope"
        )

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_D2_many_groups_each_get_their_own_row(self, request, cli_fixture):
        """In expanded mode, dozens of failing groups produce one row each. The pre-H
        stderr headline capped its group list at three and pointed elsewhere for the
        rest; a relation does not truncate, so the cap is gone and the user filters or
        aggregates the rows themselves. (Query mode folds to one clause, so the group
        list only arises in expanded mode.)"""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT l_orderkey, x FROM lineitem WHERE l_orderkey <= 400 "
            "DECIDE x(BOOL) SUCH THAT SUM(x) >= 5 PER l_orderkey MAXIMIZE SUM(x)"
        )
        flat = _flat(_diagnose(
            cli, sql,
            extra_pragmas="PRAGMA diagnose_decide_infeasible_slack_scope='expanded';\n",
        ))
        # Every failing group gets its own row — a relation does not truncate, and the
        # user filters or aggregates it themselves.
        groups = {f["group"] for f in flat if f["edit_source"] == "expanded_group"}
        assert len(groups) > 3, flat

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_G_drop_edit_is_deduplicated(self, request, cli_fixture):
        """A single `x <> 1` clause over an order with several line items yields
        one `drop` edit, not one per row."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT l_orderkey, x FROM lineitem WHERE l_orderkey = 1 "
            "DECIDE x(BOOL) SUCH THAT x <> 0 AND x <> 1 MINIMIZE SUM(x)"
        )
        rows = _rows(_diagnose(cli, sql))
        drops = [r for r in rows if r["attribute"] == "edit_kind" and r["value"] == "drop"]
        assert len(drops) == 1

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_B_entity_escape_names_join_column(self, request, cli_fixture):
        """GERMANY suppliers escape; the characterization names n_name even
        though it comes from the joined `nation` table."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT s.s_suppkey, keep FROM supplier s "
            "JOIN nation n ON s.s_nationkey = n.n_nationkey "
            "DECIDE s.keep(REAL) SUCH THAT keep <= 50 WHEN n.n_name <> 'GERMANY' "
            "MAXIMIZE SUM(keep)"
        )
        rows = _rows(
            _diagnose(
                cli,
                sql,
                extra_pragmas="PRAGMA diagnose_decide_categorical_ratio=0.5;\n",
            )
        )
        assert _attr(rows, "variable", "escaping_group") == "n_name = 'GERMANY'"

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_E_loosen_should_not_be_degenerate(self, request, cli_fixture):
        """The tight constraint is the dollar budget; loosening the count floor to
        0 (select nothing, objective 0) is feasible but useless. Scale-normalized
        slack weights (T1) loosen the budget instead, keeping a nonzero achievable
        objective."""
        cli = request.getfixturevalue(cli_fixture)
        sql = (
            "SELECT l_orderkey, buy FROM lineitem WHERE l_orderkey <= 100 "
            "DECIDE buy(BOOL) "
            "SUCH THAT SUM(buy) >= 30 AND SUM(buy * l_extendedprice) <= 100 "
            "MAXIMIZE SUM(buy)"
        )
        rows = _rows(_diagnose(cli, sql))
        assert float(_attr(rows, "model", "achievable_objective")) > 0
