"""Does the repair a diagnosis hands the user actually repair the query?

One invariant, swept across scales and clause shapes rather than argued case by case:
an infeasible query under `DIAGNOSE` must come back with either a repair that makes it
solve, or an explicit statement that it could not be analysed. Never a third thing —
not a crash, not silence, and not a repair that leaves the query infeasible.

The sweep exists because hand-picked cases kept missing whole classes. Every failure it
has caught was in the arithmetic *around* the repair rather than in the repair itself: a
noise floor that erased a small edit beside a large-coefficient row, another that erased
the small half of a repair spanning four orders of magnitude, and a rounding step that
reported `x <= 3.33333` for a bound that had to reach `10/3`. All three produced advice
that looked reasonable and did not work.
"""
import csv
import io
import itertools

import pytest

from .test_query_diagnostics_relation import (
    _apply_reported_fix,
    _attrs,
    _clause_edits,
    _diagnose,
    _errors,
    _rows,
)

_BACKENDS = ["decidb_cli_highs", "decidb_cli_gurobi"]

#: Magnitudes a user's own numbers might live at. The point is not any single one — it is
#: that the machinery must not care, and every bug this file caught was a scale coupling.
_SCALES = [0.001, 1, 1000, 1000000]


def _cases():
    """Infeasible queries: `(name -> (sql, rendered-clause -> literal-SQL overrides))`."""
    out = {}
    rows3 = "(VALUES (1),(2),(3)) t(id)"
    for s in _SCALES:
        cap, floor = 5 * s, 30 * s
        out[f"box-vs-floor@{s}"] = (
            f"SELECT id, x AS contrib FROM {rows3} DECIDE x(REAL) "
            f"SUCH THAT x >= 0 AND x <= {cap} AND SUM(x) >= {floor} MAXIMIZE SUM(x)", {})
        out[f"norm@{s}"] = (
            f"SELECT id, x AS contrib FROM {rows3} DECIDE x(REAL) "
            f"SUCH THAT x >= 0 AND x <= {cap} AND norm(x, 0) <= 2 AND SUM(x) >= {floor} "
            f"MAXIMIZE SUM(x)", {"NORM(x, 0) <= 2": "norm(x, 0) <= 2"})
        out[f"bilinear@{s}"] = (
            f"SELECT id, b * x AS contrib FROM {rows3} DECIDE b(BOOL), x(REAL) "
            f"SUCH THAT x >= 0 AND x <= {cap} AND SUM(b * x) >= {floor} "
            f"MAXIMIZE SUM(b * x)", {})
        out[f"abs@{s}"] = (
            f"SELECT id, x AS contrib FROM (VALUES (1)) t(id) DECIDE x(REAL) "
            f"SUCH THAT x >= -{cap} AND x <= {cap} AND ABS(x) >= {floor} "
            f"MAXIMIZE SUM(x)", {})
        out[f"minmax@{s}"] = (
            f"SELECT id, x AS contrib FROM {rows3} DECIDE x(REAL) "
            f"SUCH THAT x >= 0 AND x <= {cap} AND MAX(x) >= {floor} MAXIMIZE SUM(x)", {})
        # One row whose coefficients dwarf every other number in the query. This is what
        # a noise floor taken from the model's scale gets wrong.
        out[f"heavy-row@{s}"] = (
            f"SELECT id, x AS contrib FROM (VALUES (1,{1e6 * s}),(2,{1e6 * s}),"
            f"(3,{1e6 * s})) t(id,w) DECIDE x(REAL) SUCH THAT x >= 0 AND x <= {cap} "
            f"AND SUM(w * x) <= {1e12 * s} AND SUM(x) >= {floor} MAXIMIZE SUM(x)", {})
        if float(s).is_integer():  # `<>` is INT-only by design
            out[f"ne@{s}"] = (
                f"SELECT id, x AS contrib FROM {rows3} DECIDE x(INT) SUCH THAT x >= 0 "
                f"AND x <= {int(cap)} AND x <> {int(cap)} AND SUM(x) >= {int(floor)} "
                f"MAXIMIZE SUM(x)", {})

    # Two unrelated conflicts in one query, at every ordered pair of scales. This is what
    # a noise floor taken from the largest edit in the same repair gets wrong.
    for a, b in itertools.permutations(_SCALES, 2):
        out[f"two-conflicts@{a}/{b}"] = (
            f"SELECT id, x, y FROM (VALUES (1)) t(id) DECIDE x(REAL), y(REAL) "
            f"SUCH THAT x <= {a} AND x >= {2 * a} AND y <= {b} AND y >= {2 * b} "
            f"MAXIMIZE SUM(x + y)", {})

    # Clause shapes, at one scale each. Repairs here land on thirds and other
    # non-terminating decimals, which is what one-directional rounding is for.
    out["equality-conflict"] = (
        f"SELECT id, x AS contrib FROM {rows3} DECIDE x(REAL) "
        f"SUCH THAT x >= 0 AND x <= 5 AND SUM(x) = 40 MAXIMIZE SUM(x)", {})
    out["negative-box"] = (
        f"SELECT id, x AS contrib FROM {rows3} DECIDE x(REAL) "
        f"SUCH THAT x >= -5 AND x <= -1 AND SUM(x) >= 10 MAXIMIZE SUM(x)", {})
    out["two-vars-coupled"] = (
        f"SELECT id, x + y AS contrib FROM {rows3} DECIDE x(REAL), y(REAL) "
        f"SUCH THAT x >= 0 AND x <= 2 AND y >= 0 AND y <= 3 AND SUM(x + y) >= 40 "
        f"MAXIMIZE SUM(x + y)", {})
    out["per-group"] = (
        "SELECT g, x AS contrib FROM (VALUES (1,'a'),(2,'a'),(3,'b')) t(id,g) "
        "DECIDE x(REAL) SUCH THAT x >= 0 AND x <= 5 AND SUM(x) >= 20 PER g "
        "MAXIMIZE SUM(x)", {})
    out["int-box"] = (
        f"SELECT id, x AS contrib FROM {rows3} DECIDE x(INT) "
        f"SUCH THAT x >= 0 AND x <= 5 AND SUM(x) >= 31 MAXIMIZE SUM(x)", {})
    out["bool-pin"] = (
        f"SELECT id, b AS contrib FROM {rows3} DECIDE b(BOOL) "
        f"SUCH THAT b <= 0 AND SUM(b) >= 2 MAXIMIZE SUM(b)", {})
    out["in-domain"] = (
        f"SELECT id, x AS contrib FROM {rows3} DECIDE x(INT) "
        f"SUCH THAT x IN (1, 2) AND SUM(x) >= 20 MAXIMIZE SUM(x)", {})
    out["minmax-objective"] = (
        f"SELECT id, x AS contrib FROM {rows3} DECIDE x(REAL) "
        f"SUCH THAT x >= 0 AND x <= 5 AND SUM(x) >= 40 MAXIMIZE MIN(x)", {})
    out["abs-in-constraint"] = (
        f"SELECT id, x AS contrib FROM {rows3} DECIDE x(REAL) "
        f"SUCH THAT x >= -5 AND x <= 5 AND SUM(ABS(x)) >= 40 MAXIMIZE SUM(x)", {})
    out["when-filtered"] = (
        "SELECT id, x AS contrib FROM (VALUES (1,1),(2,0),(3,1)) t(id,k) "
        "DECIDE x(REAL) SUCH THAT x >= 0 AND x <= 5 AND SUM(x) >= 20 WHEN k = 1 "
        "MAXIMIZE SUM(x)", {})
    out["scalar-scope"] = (
        f"SELECT id, y AS contrib FROM {rows3} DECIDE y(REAL), scalar cap(REAL) "
        f"SUCH THAT y >= 0 AND y <= cap AND cap <= 2 AND SUM(y) >= 30 MAXIMIZE SUM(y)", {})
    return out


CASES = _cases()


@pytest.mark.query_diagnostics
@pytest.mark.parametrize("cli_fixture", _BACKENDS)
@pytest.mark.parametrize("name", sorted(CASES))
def test_the_reported_repair_repairs_the_query(request, name, cli_fixture):
    """Either a repair that works, or an explicit "could not analyse" — never a third
    thing. A repair that does not restore feasibility is the worst outcome available: it
    reads as a confident answer and wastes the edit the user makes on its say-so."""
    sql, subject_to_sql = CASES[name]
    cli = request.getfixturevalue(cli_fixture)

    assert _errors(cli.execute_script(".mode csv\n" + sql + ";\n")), (
        f"{name}: the sweep needs an infeasible query, but this one solved")

    result = _diagnose(cli, sql)
    assert not _errors(result), f"{name}: DIAGNOSE raised instead of reporting: {result.stderr}"
    rows = _rows(result)
    edits = _clause_edits(rows)
    if not edits:
        # The elastic model is deliberately harder than the query's own, so a backend can
        # fail to load or solve it. Saying so is a valid answer; saying nothing is not.
        assert "undiagnosed" in _attrs(rows, "model", "NULL"), (
            f"{name}: no repair and no explanation:\n{rows}")
        return
    _apply_reported_fix(cli, sql, rows, subject_to_sql)


@pytest.mark.query_diagnostics
@pytest.mark.parametrize("name", sorted(CASES))
def test_both_backends_blame_the_same_clauses(request, name, decidb_cli_gurobi,
                                              decidb_cli_highs):
    """Rule 2 across the whole sweep: a user moving hosts must not be told to edit a
    different line of their own query. A backend that cannot analyse the repair model at
    all is a different thing from one that blames a different clause, and only the second
    is a defect."""
    sql, _ = CASES[name]
    reported = {}
    for label, cli in (("gurobi", decidb_cli_gurobi), ("highs", decidb_cli_highs)):
        rows = _rows(_diagnose(cli, sql))
        reported[label] = sorted(e["subject"] for e in _clause_edits(rows))
    if not reported["gurobi"] or not reported["highs"]:
        return  # one host could not analyse it; covered by the test above
    assert reported["gurobi"] == reported["highs"], f"{name}: {reported}"
