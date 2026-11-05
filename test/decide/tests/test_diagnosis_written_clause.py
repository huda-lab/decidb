"""A diagnosis quotes the clause the user wrote, not the one canonicalization built.

Stage 04 puts decisions left and the bound right, and for most constraints that is
invisible: ``SUM(x) >= 100`` is already canonical, so re-rendering the canonical tree
reproduces what was typed. One shape does not survive it. When BOTH sides carry
decisions the canonicalizer has to merge them, and

    ship BETWEEN 0 AND capacity * open        (the paper's Figure 1, line 7)

becomes ``ship - capacity * open <= 0`` — a clause the query does not contain, with a
literal bound that is not in it either. Diagnosis then reported that form and offered
``ship - capacity * open <= 25``: an edit with nothing to apply it to.

The written spelling is captured before canonicalization and quoted instead. The offset
is valid against either form — moving a term across a comparison does not change what
adding a constant to the bound means — so the repair reads ``ship <= capacity * open +
25`` off machinery that already existed for column bounds.

It stays narrow on purpose. When only one side carries decisions the rewrite is a clean
move and the leftover bound folds into something *better* than what was written
(``(SELECT 7) >= x + 2`` becomes ``x <= 5``, an opaque subquery turned into an editable
number). Quoting the written form there would be a regression, so it does not.
"""

import csv
import io

import pytest

from . import _diagnose_relation

_DEPOTS = "(VALUES ('D1', 12000), ('D2', 8000)) D(depotID, opening_cost)"
_ROUTES = "(VALUES ('T1','D1',500), ('T2','D1',350), ('T3','D2',300)) T(routeID, depotID, capacity)"
# The same routes with a floor read from data. A literal floor would take the repair ahead of a bound over `capacity`,
# so a test of how that bound's repair reads needs a floor in the same tier.
_ROUTES_WITH_NEED = (
    "(VALUES ('T1','D1',500,5000), ('T2','D1',350,5000), ('T3','D2',300,5000)) T(routeID, depotID, capacity, need)"
)


def _diagnostics(cli, sql):
    """Run `DIAGNOSE <sql>`. Returns {clause: {attribute: value}} plus the raw output."""
    result = cli.execute_script(".mode csv\n" + f"DIAGNOSE {sql};\n")
    rows = list(csv.DictReader(io.StringIO(result.stdout)))
    out = {r["clause"]: r for r in rows}
    return out, result.stdout + result.stderr


@pytest.mark.error
@pytest.mark.cons_between
def test_decision_bearing_bound_is_quoted_as_written(decidb_cli):
    """Figure 1's line 7. The clause and its repair both name what the user typed."""
    attrs, message = _diagnostics(
        decidb_cli,
        f"""
        SELECT routeID, ship, open
        FROM {_DEPOTS} JOIN {_ROUTES_WITH_NEED} USING (depotID)
        DECIDE per T: ship(INT), per D: open(BOOL)
        SUCH THAT ship BETWEEN 0 AND capacity * open AND SUM(ship) >= need
        MINIMIZE SUM(ship)
    """,
    )
    # The clause is the BETWEEN the user wrote, not either half of it.
    assert "ship BETWEEN 0 AND capacity * open" in message, message
    # The algebra stage 04 produced must not reach the user at all.
    assert "ship - capacity * open" not in message, message

    clause = attrs.get("ship BETWEEN 0 AND capacity * open")
    assert clause is not None, attrs
    suggestion = clause["suggested_change"]
    assert suggestion.startswith("ship BETWEEN 0 AND capacity * open +"), suggestion
    # A bound with no number in the query to retype is a virtual offset, not a literal.
    assert clause["edit_source"] == "virtual_offset", clause


@pytest.mark.error
@pytest.mark.cons_comparison
def test_written_form_is_used_for_a_plain_comparison_too(decidb_cli):
    """Not BETWEEN-specific — the trigger is a decision in the bound."""
    _, message = _diagnostics(
        decidb_cli,
        f"""
        SELECT routeID, ship, open
        FROM {_DEPOTS} JOIN {_ROUTES_WITH_NEED} USING (depotID)
        DECIDE per T: ship(INT), per D: open(BOOL)
        SUCH THAT ship >= 0 AND ship <= capacity * open AND SUM(ship) >= need
        MINIMIZE SUM(ship)
    """,
    )
    assert "ship <= capacity * open" in message, message
    assert "ship - capacity * open" not in message, message


@pytest.mark.error
@pytest.mark.cons_comparison
def test_a_data_only_bound_is_untouched(decidb_cli):
    """Nothing moved, so there is no written form to prefer — and none is invented."""
    attrs, message = _diagnostics(
        decidb_cli,
        f"""
        SELECT routeID, ship FROM {_ROUTES}
        DECIDE per T: ship(INT)
        SUCH THAT ship BETWEEN 0 AND capacity AND SUM(ship) >= 9000
        MINIMIZE SUM(ship)
    """,
    )
    assert "sum(ship) >= 9000" in message, message
    clause = attrs.get("sum(ship) >= 9000")
    assert clause is not None, attrs
    assert clause["suggested_change"].startswith("sum(ship) >= "), clause


@pytest.mark.explain
@pytest.mark.cons_between
def test_explain_leads_with_the_written_clause(decidb_cli):
    """EXPLAIN reads the same registry, so the written clause is what it leads with.

    A plan shows the layers a clause passed through, so unlike a diagnosis it does not
    suppress the canonical algebra — it subordinates it. ``ship BETWEEN 0 AND capacity *
    open`` is the clause; its two halves, ``ship >= 0`` and ``ship - capacity * open <= 0``,
    are indented ``≡`` continuations of it. What must never happen is the algebra standing
    alone as though the user wrote it.
    """
    plan = decidb_cli.execute_raw(
        f"""
        EXPLAIN SELECT routeID, ship, open
        FROM {_DEPOTS} JOIN {_ROUTES} USING (depotID)
        DECIDE per T: ship(INT), per D: open(BOOL)
        SUCH THAT ship BETWEEN 0 AND capacity * open
        MINIMIZE SUM(ship)
    """
    ).stdout
    # The box wraps mid-token at a fixed column, so no assertion may depend on where
    # the line breaks fall: drop the box's own edges and the whitespace.
    packed = "".join(plan.replace("│", "").split())
    assert "shipBETWEEN0ANDcapacity*open" in packed, plan
    # The algebra appears, but only ever under the canonical marker.
    assert "ship-capacity*open" in packed, plan
    assert "≡ship-capacity*open" in packed, plan
    # Both halves are listed under the clause, in written order.
    assert packed.index("≡ship>=0") < packed.index("≡ship-capacity*open"), plan
    assert packed.count("ship-capacity*open") == packed.count("≡ship-capacity*open"), plan


# The repair tier follows the right side as written (ReadsRowData), whatever canonicalization leaves of it.
_ONE_ROW = "(VALUES (1, 60)) t(id, cap)"
_MIXED = "SELECT id, x, o FROM {} DECIDE x(INT), o(BOOL) SUCH THAT {} MINIMIZE SUM(x)"


@pytest.mark.error
@pytest.mark.cons_comparison
@pytest.mark.parametrize(
    "bound",
    [
        pytest.param("cap * o", id="column"),
        pytest.param("(SELECT MAX(r.v) FROM (VALUES (1, 60)) r(i, v) WHERE r.i = t.id) * o", id="correlated_subquery"),
    ],
)
def test_a_written_bound_that_reads_data_repairs_after_a_literal(decidb_cli, bound):
    """A bound with data in it gives way to the literal floor, with or without a decision in it."""
    attrs, _ = _diagnostics(decidb_cli, _MIXED.format(_ONE_ROW, f"x <= {bound} AND SUM(x) >= 100"))
    assert "sum(x) >= 100" in attrs, attrs
    assert attrs["sum(x) >= 100"]["edit_source"] == "source_literal", attrs


@pytest.mark.error
@pytest.mark.cons_comparison
@pytest.mark.parametrize(
    "clause",
    [
        pytest.param("2 * x <= 60 * o", id="constant_and_decision"),
        pytest.param("2 * x <= (SELECT 60) * o", id="uncorrelated_subquery"),
        pytest.param("2 * x - cap * o <= 0", id="data_on_the_written_left"),
    ],
)
def test_a_written_bound_with_no_data_repairs_before_a_data_floor(decidb_cli, clause):
    """The factor 2 makes this clause's slack cost twice the floor's, so only the tier can send the repair here."""
    attrs, _ = _diagnostics(decidb_cli, _MIXED.format(_ONE_ROW, f"{clause} AND SUM(x) >= cap + 40"))
    assert clause in attrs, attrs
    assert "sum(x) >= cap + 40" not in attrs, attrs


@pytest.mark.error
@pytest.mark.cons_comparison
def test_a_data_bound_with_a_decision_reports_per_row_when_expanded(decidb_cli):
    """A data bound is a profile over its rows, as `x <= cap` is, not one knob the rows share.

    The factor 2 makes the floor's slack cost twice the bound's, so the repair lands on the bound.
    """
    routes = "(VALUES (1, 60, 500), (2, 10, 500)) t(id, cap, need)"
    result = _diagnose_relation.run(
        decidb_cli, _MIXED.format(routes, "x <= cap * o AND SUM(2 * x) >= need"), scope="expanded"
    )
    rows = [r for r in _diagnose_relation.rows(result) if r["clause"] == "x <= cap * o"]
    assert rows, result.stdout + result.stderr
    assert {r["edit_source"] for r in rows} == {"expanded_row"}, rows
    assert all(r["row"] != "NULL" for r in rows), rows


@pytest.mark.error
@pytest.mark.cons_between
def test_paper_figure_3_keeps_its_repair(decidb_cli):
    """Capacity and demand both read data, so the tier ties and 25 on capacity beats 50 on demand."""
    attrs, _ = _diagnostics(
        decidb_cli,
        """
        SELECT routeID, depotID, regionID, open, ship
        DECIDE per depotID: open(BOOL), per routeID: ship(INT)
        FROM (VALUES ('D1', 800, 12000), ('D2', 500, 8000)) Depots(depotID, stock, opening_cost)
        JOIN (VALUES ('T1', 'D1', 'R1', 500, 4), ('T2', 'D1', 'R2', 250, 6), ('T3', 'D2', 'R2', 300, 3))
            Routes(routeID, depotID, regionID, capacity, unit_cost) USING (depotID)
        JOIN (VALUES ('R1', 450, 'critical'), ('R2', 600, 'critical')) Regions(regionID, demand, priority)
            USING (regionID)
        SUCH THAT ship BETWEEN 0 AND capacity * open
            AND per depotID: sum(ship) by (depotID) <= stock
            AND when priority = 'critical' per regionID: sum(ship) by (regionID) >= demand
        MINIMIZE sum(unit_cost * ship) + sum(per depotID: opening_cost * open)
    """,
    )
    clause = attrs["ship BETWEEN 0 AND capacity * open"]
    assert clause["suggested_change"] == "ship BETWEEN 0 AND capacity * open + 25", attrs
    assert clause["edit_source"] == "virtual_offset", attrs
