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

_DEPOTS = "(VALUES ('D1', 12000), ('D2', 8000)) D(depotID, opening_cost)"
_ROUTES = "(VALUES ('T1','D1',500), ('T2','D1',350), ('T3','D2',300)) T(routeID, depotID, capacity)"


def _diagnostics(cli, sql):
    """Run the failing DECIDE and the relation read on ONE session, as the diagnosis is
    stashed per-connection. Returns {subject: {attribute: value}} plus the combined output."""
    result = cli.execute_script(
        ".mode csv\n"
        "PRAGMA diagnose_decide='auto';\n"
        f"{sql};\n"
        "SELECT * FROM decide_diagnostics();\n"
    )
    rows = list(csv.DictReader(io.StringIO(result.stdout)))
    out = {}
    for r in rows:
        out.setdefault(r["subject"], {})[r["attribute"]] = r["value"]
    return out, result.stdout + result.stderr


@pytest.mark.error
@pytest.mark.cons_between
def test_decision_bearing_bound_is_quoted_as_written(decidb_cli):
    """Figure 1's line 7. The clause and its repair both name what the user typed."""
    attrs, message = _diagnostics(decidb_cli, f"""
        SELECT routeID, ship, open
        FROM {_DEPOTS} JOIN {_ROUTES} USING (depotID)
        DECIDE T.ship(INT), D.open(BOOL)
        SUCH THAT ship BETWEEN 0 AND capacity * open AND SUM(ship) >= 5000
        MINIMIZE SUM(ship)
    """)
    assert "ship <= capacity * open" in message, message
    # The algebra stage 04 produced must not reach the user at all.
    assert "ship - capacity * open" not in message, message

    clause = attrs.get("ship <= capacity * open")
    assert clause is not None, attrs
    suggestion = clause["suggested_change"]
    assert suggestion.startswith("ship <= capacity * open +"), suggestion
    # A bound with no number in the query to retype is a virtual offset, not a literal.
    assert clause["edit_source"] == "virtual_offset", clause


@pytest.mark.error
@pytest.mark.cons_comparison
def test_written_form_is_used_for_a_plain_comparison_too(decidb_cli):
    """Not BETWEEN-specific — the trigger is a decision in the bound."""
    _, message = _diagnostics(decidb_cli, f"""
        SELECT routeID, ship, open
        FROM {_DEPOTS} JOIN {_ROUTES} USING (depotID)
        DECIDE T.ship(INT), D.open(BOOL)
        SUCH THAT ship >= 0 AND ship <= capacity * open AND SUM(ship) >= 5000
        MINIMIZE SUM(ship)
    """)
    assert "ship <= capacity * open" in message, message
    assert "ship - capacity * open" not in message, message


@pytest.mark.error
@pytest.mark.cons_comparison
def test_a_data_only_bound_is_untouched(decidb_cli):
    """Nothing moved, so there is no written form to prefer — and none is invented."""
    attrs, message = _diagnostics(decidb_cli, f"""
        SELECT routeID, ship FROM {_ROUTES}
        DECIDE T.ship(INT)
        SUCH THAT ship BETWEEN 0 AND capacity AND SUM(ship) >= 9000
        MINIMIZE SUM(ship)
    """)
    assert "SUM(ship) >= 9000" in message, message
    clause = attrs.get("SUM(ship) >= 9000")
    assert clause is not None, attrs
    assert clause["suggested_change"].startswith("SUM(ship) >= "), clause


@pytest.mark.explain
@pytest.mark.cons_between
def test_explain_shows_the_written_clause(decidb_cli):
    """EXPLAIN reads the same registry, so it stops printing the algebra too."""
    plan = decidb_cli.execute_raw(f"""
        EXPLAIN SELECT routeID, ship, open
        FROM {_DEPOTS} JOIN {_ROUTES} USING (depotID)
        DECIDE T.ship(INT), D.open(BOOL)
        SUCH THAT ship BETWEEN 0 AND capacity * open
        MINIMIZE SUM(ship)
    """).stdout
    assert "ship <= capacity * open" in plan, plan
    assert "ship - capacity * open" not in plan, plan
