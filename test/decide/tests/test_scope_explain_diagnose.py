"""What EXPLAIN and DIAGNOSE print for scoped clauses (D27, D28, D32).

EXPLAIN shows a clause as written, its canonical reading when that differs, and what the
optimizer made of it: an auxiliary decision prints as the expression it stands for, and
each rewrite leaves one account line under its clause. Every clause prints lower-case,
`when c per K:` in front and a reducer's `by (G)` after it. DIAGNOSE, in the expanded
slack scope, reports a `by`-grouped clause once per class the group's row stands for: per
value of its `per` key, or per kept row when it has no `per` key (D28, ANR deck p. 16-17
and 110).
"""

import re

import pytest

from . import _diagnose_relation
from ._output_helpers import assert_no_internal_leak
from ._scope_cases import from_clause, table

_BOX = re.compile(r"[│┌┐└┘├┤┬┴┼─\s]")


def _plan(cli, sql: str) -> str:
    """EXPLAIN's DECIDE node with every box glyph and all whitespace removed: the box
    wraps values mid-token, so nothing may depend on where a line breaks. Only the solver
    path plans a DECIDE node, so the plan is taken with direct solve off."""
    out = cli.with_env({"DECIDB_TEST_DIRECT_SOLVE": "off"}).execute_raw(f"EXPLAIN {sql}")
    assert out.returncode == 0 and not out.stderr.strip(), out.stderr
    return _BOX.sub("", out.stdout)


def _ship_query(decide: str, clause: str, objective: str = "MAXIMIZE SUM(ship)") -> str:
    return f"""
        SELECT shipmentID, ship {from_clause()}
        DECIDE {decide}
        SUCH THAT ship <= S.demand AND {clause}
        {objective}"""


# ---------------------------------------------------------------------------
# EXPLAIN: auxiliaries by what they stand for, one account line per rewrite
# ---------------------------------------------------------------------------


@pytest.mark.explain
@pytest.mark.parametrize(
    "decide, clause, account, row, absent",
    [
        pytest.param(
            "ship(INT)",
            "ship <= 0.5 * sum(ship) by (D.region)",
            "sum(ship)by(D.region)becomesahelperdecisionpergroup",
            None,
            "__value_",
            id="helper-per-group",
        ),
        pytest.param(
            "ship(INT)",
            "per row: ship <= 0.2 * sum(ship)",
            "sum(ship)becomesahelperdecision",
            None,
            "__value_",
            id="helper-query-wide",
        ),
        pytest.param(
            "ship(INT)",
            "when S.priority: abs(ship - 20) <= 3",
            "abs(ship-20)becomesanauxiliaryvariable",
            "whenpriority:abs(ship-20)-ship>=-20",
            "__abs_aux",
            id="abs",
        ),
        pytest.param(
            "ship(INT), per D: open(BOOL)",
            "per (): sum(open * ship) by () <= 50",
            "(open*ship)becomesanauxiliaryvariablelinearizedbyMcCormickrows",
            "(open*ship)",
            "__bilinear",
            id="product",
        ),
        pytest.param(
            "ship(INT)",
            "ship IN (0, 5, 10)",
            "shipIN(0,5,10)becomesoneindicatorpervalue",
            "(ship=0)+(ship=5)+(ship=10)=1",
            "__in_ind",
            id="in",
        ),
        pytest.param(
            "ship(INT)",
            "per D.region: max(ship) by (D.region) >= 12",
            "max(ship)by(D.region)becomesanextremumcolumnpinnedbyindicatorrows",
            None,
            "sum(ship)by(D.region)>=12",
            id="hard-max",
        ),
        pytest.param(
            "ship(INT)",
            "per (): sum(ship) + max(ship) <= 40",
            "max(ship)becomesanextremumcolumn",
            None,
            "MAX(",
            id="composed",
        ),
        pytest.param(
            "ship(INT), per D: open(BOOL)",
            "per (): sum(open * abs(ship - 5)) <= 50",
            "(open*abs(ship-5))becomesanauxiliaryvariablelinearizedbyMcCormickrows",
            None,
            "__abs_aux",
            id="stacked-rewrites",
        ),
        pytest.param(
            "ship(INT)",
            "when S.priority: ship IN (0, 5, 10)",
            "whenpriority:shipIN(0,5,10)",
            "whenpriority:(ship=0)+(ship=5)+(ship=10)=1",
            None,
            id="scoped-in-keeps-its-when",
        ),
        pytest.param(
            "ship(INT)",
            "per row: ship <= 0.2 * sum(ship) by ()",
            "ship<=0.2*sum(ship)",
            None,
            "perrow:",
            id="per-row-prints-as-bare",
        ),
        pytest.param(
            "ship(INT)",
            "per D.region: avg(ship) by (D.region) >= 10",
            "avg(ship)by(D.region)becomesasumdividedbyitsrowcount",
            "perD.region:sum(ship)by(D.region)>=10",
            None,
            id="avg",
        ),
        pytest.param(
            "ship(INT)",
            "per D.region: max(ship) by (D.region) <= 12",
            "max(ship)by(D.region)becomesaboundoneachmember",
            None,
            None,
            id="easy-max",
        ),
        pytest.param(
            "ship(INT)",
            "per D.region: max(ship) by (D.region) = 12",
            "max(ship)by(D.region)becomesanextremumcolumnpinnedbyindicatorrows"
            "↳max(ship)by(D.region)becomesaboundoneachmember",
            "perD.region:max(ship)by(D.region)>=12",
            None,
            id="max-equality",
        ),
        pytest.param(
            "ship(INT)",
            "per (): norm(ship - 5, 1) <= 9",
            "norm(ship-5,1)becomesasumofabsolutevalues",
            "per():sum(abs(ship-5))<=9",
            None,
            id="norm-1",
        ),
        pytest.param(
            "ship(INT)",
            "per (): norm(ship - 5, 'inf') <= 9",
            "norm(ship-5,'inf')becomesamaximumofabsolutevalues",
            "per():max(abs(ship-5))<=9",
            None,
            id="norm-inf",
        ),
        pytest.param(
            "ship(INT)",
            "when S.priority: norm(ship, 0) <= 2",
            "norm(ship,0)becomesoneindicatorperrow",
            "whenpriority:M*(ship<>0)-ship>=0.0",
            "1.0*",
            id="l0-bound-is-derived",
        ),
        pytest.param(
            "ship(INT)",
            "per D.region: sum(ship) by (D.region) <> 20",
            "sum(ship)by(D.region)<>20becomestwoconditionalrowswithanindicator",
            None,
            None,
            id="not-equal",
        ),
    ],
)
def test_explain_accounts_for_each_rewrite(decidb_cli, decide, clause, account, row, absent):
    """Each rewrite leaves an account line under the clause it came from, and the rows
    the solver receives name an auxiliary by what it stands for, never by `__name__` --
    an auxiliary made by an earlier rewrite included. A row that repeats the clause
    (the hard MIN/MAX carrier, printed as its reducer) is not shown again. A scoped `IN`
    keeps its front; `per row:` and an empty `by ()`, the defaults, print as a bare clause
    (D32). An automatic L0 link's bound, which stage 06 derives from the box, prints as
    `M`."""
    text = _plan(decidb_cli, _ship_query(decide, clause))
    assert account in text, text
    if row:
        assert row in text, text
    if absent:
        assert absent not in text, text


@pytest.mark.explain
@pytest.mark.quadratic
def test_explain_accounts_for_a_sum_of_squares(decidb_cli_gurobi):
    """`norm(e, 2)` lowers to a sum of squares, a quadratic row only Gurobi takes."""
    text = _plan(decidb_cli_gurobi, _ship_query("ship(INT)", "per (): norm(ship - 5, 2) <= 9"))
    assert "norm(ship-5,2)becomesasumofsquares↳per():sum(power(ship-5,2))<=9" in text, text


@pytest.mark.explain
def test_explain_names_functions_as_the_scope_spellings_write_them(decidb_cli):
    """A function prints lower-case, as the docs write it; the
    written layer and the solver rows agree."""
    text = _plan(decidb_cli, _ship_query("ship(INT)", "when S.priority: abs(ship - 20) <= 3"))
    assert "whenpriority:abs(ship-20)<=3" in text, text
    assert "ABS(" not in text, text


@pytest.mark.explain
def test_explain_two_level_objective_rows_are_written_in_front(decidb_cli):
    """The two-level objective's inner reducer, once per class, prints with its `per` in
    front and its `by`, with no upper-case `PER`; the outer extremum has its account."""
    text = _plan(
        decidb_cli, _ship_query("ship(INT)", "ship >= 1", "MINIMIZE max(per D.region: sum(ship) by (D.region))")
    )
    assert "MINIMIZEmax(perD.region:sum(ship)by(D.region))" in text, text
    assert "↳max(perD.region:sum(ship)by(D.region))becomesanextremumcolumn" in text, text
    assert "↳perD.region:sum(ship)by(D.region)" in text, text
    assert "PER" not in text, text
    # An inner MIN/MAX leaves a SUM carrier per group; the account tells it, no row misreads it.
    text = _plan(
        decidb_cli, _ship_query("ship(INT)", "ship >= 1", "MINIMIZE sum(per D.region: max(ship) by (D.region))")
    )
    assert "↳max(ship)by(D.region)becomesanextremumcolumnpergroup" in text, text
    assert "↳perD.region:sum(ship)" not in text, text


@pytest.mark.explain
def test_explain_accounts_for_the_objectives_rewrites_too(decidb_cli):
    """An ABS, a product or an AVG in the objective gets its account under the objective,
    and the rows a rewrite added for it are printed there too, not as rows no clause owns."""
    text = _plan(decidb_cli, _ship_query("ship(INT)", "ship >= 1", "MINIMIZE when S.priority: sum(abs(ship - 10))"))
    assert (
        "Objective:MINIMIZEwhenpriority:sum(abs(ship-10))↳abs(ship-10)becomesanauxiliaryvariable"
        "↳whenpriority:abs(ship-10)-ship>=-10"
    ) in text, text
    assert "addedbyformulation" not in text, text
    text = _plan(decidb_cli, _ship_query("ship(INT), per D: open(BOOL)", "ship >= 1", "MAXIMIZE sum(open * ship)"))
    assert "↳(open*ship)becomesanauxiliaryvariablelinearizedbyMcCormickrows" in text, text
    text = _plan(decidb_cli, _ship_query("ship(INT)", "ship >= 1", "MAXIMIZE when S.priority: avg(ship)"))
    assert "↳avg(ship)becomesasumdividedbyitsrowcount" in text, text
    text = _plan(
        decidb_cli, _ship_query("ship(INT)", "ship >= 1", "MINIMIZE sum(per D.region: avg(ship) by (D.region))")
    )
    assert "↳avg(ship)by(D.region)becomesasumdividedbyitsrowcount" in text, text


@pytest.mark.error
@pytest.mark.error_binder
def test_an_outer_avg_over_the_classes_is_refused(decidb_cli):
    """language/guide.md §4 admits sum, min and max over the classes; an outer avg would
    be formulated as a sum and is refused instead."""
    result = decidb_cli.execute_raw(
        _ship_query("ship(INT)", "ship >= 1", "MINIMIZE avg(per D.region: sum(ship) by (D.region))")
    )
    assert "avg over the classes of avg(per D.region: sum(...) by (D.region)) is not supported yet" in result.stderr
    assert_no_internal_leak(result, "an outer avg")


@pytest.mark.explain
def test_explain_prints_no_row_that_repeats_a_per_row_clause(decidb_cli):
    """A `per row:` kept in front of a clause that shows no scope word is on no wrapper of
    the tree, so the clause's own solver row reads without it -- and repeats the clause."""
    text = _plan(
        decidb_cli,
        f"""
        SELECT id, x {table('t', T_COLUMNS, T_ROWS)}
        DECIDE x(INT)
        SUCH THAT x <= t.cap AND per row: x >= 3 AND per row: x <= t.cap - 1
        MAXIMIZE SUM(x)""",
    )
    assert "x>=3" in text and "x<=cap-1" in text and "perrow:" not in text, text
    assert "↳" not in text, text


@pytest.mark.explain
@pytest.mark.parametrize("key", ["t.region, t.depot", "t"])
def test_explain_prints_a_two_level_key_as_written(decidb_cli, key):
    """The two-level objective's key prints as written -- not its columns in binding order,
    nor a relation spelled out column by column."""
    text = _plan(
        decidb_cli,
        f"""
        SELECT id, x {table('t', T_COLUMNS, T_ROWS)}
        DECIDE x(INT)
        SUCH THAT x <= t.cap
        MINIMIZE max(per {key}: sum(x) by ({key}))""",
    )
    written = key.replace(" ", "")
    assert f"MINIMIZEmax(per{written}:sum(x)by({written}))" in text, text
    assert f"↳max(per{written}:sum(x)by({written}))becomesanextremumcolumn" in text, text


# ---------------------------------------------------------------------------
# DIAGNOSE: the `group` column holds the per key value, one finding per class
# ---------------------------------------------------------------------------

#: Depots D1 and D2 lie in region E, D3 in W; x <= cap per row.
T_COLUMNS = ("id", "depot", "region", "cap")
T_ROWS = ((1, "D1", "E", 10), (2, "D1", "E", 20), (3, "D2", "E", 30), (4, "D3", "W", 15))


def _diagnose(cli, clause: str, scope: str = "expanded"):
    sql = f"""
        SELECT id, x {table('t', T_COLUMNS, T_ROWS)}
        DECIDE x(INT)
        SUCH THAT x <= t.cap AND {clause}
        MAXIMIZE SUM(x)"""
    result = _diagnose_relation.run(cli, sql, scope=scope)
    assert_no_internal_leak(result, clause)
    rows = _diagnose_relation.rows(result)
    assert rows and all(row["state"] == "infeasible" for row in rows), result.stdout
    return [row for row in rows if row["edit_source"] not in ("achievable_objective",)]


def _findings(rows):
    """(group, row, amount, edit_source, suggested_change) per finding, amounts as ints."""
    return sorted(
        (row["group"], row["row"], int(float(row["amount"])), row["edit_source"], row["suggested_change"])
        for row in rows
    )


@pytest.mark.diagnose
def test_a_per_key_finer_than_the_by_key_reports_once_per_class(decidb_cli):
    """`per t.depot: sum(x) by (t.region) >= 100`: region E (D1, D2) is 40 short, W (D3)
    85. The group's finding is reported once per depot, labelled with the depot."""
    findings = _findings(_diagnose(decidb_cli, "per t.depot: sum(x) by (t.region) >= 100"))
    change = "per t.depot: sum(x) by (t.region) >= {}"
    assert findings == [
        ("D1", "NULL", 40, "expanded_group", change.format(60)),
        ("D2", "NULL", 40, "expanded_group", change.format(60)),
        ("D3", "NULL", 85, "expanded_group", change.format(15)),
    ]


@pytest.mark.diagnose
def test_a_hard_min_or_max_reports_once_per_class_too(decidb_cli):
    """The hard half of `per t.depot: max(x) by (t.region) >= 50` is an extremum column
    with the clause's own bound on it; that row carries the group's classes like a sum's."""
    findings = _findings(_diagnose(decidb_cli, "per t.depot: max(x) by (t.region) >= 50"))
    change = "per t.depot: max(x) by (t.region) >= {}"
    assert findings == [
        ("D1", "NULL", 20, "expanded_group", change.format(30)),
        ("D2", "NULL", 20, "expanded_group", change.format(30)),
        ("D3", "NULL", 35, "expanded_group", change.format(15)),
    ]


@pytest.mark.diagnose
@pytest.mark.parametrize(
    "clause, expected",
    [
        pytest.param(
            "per t.depot: max(2 * x) by (t.region) <= -10",
            [
                ("D1", "NULL", 10, "expanded_group", "per t.depot: max(2 * x) by (t.region) <= 0"),
                ("D2", "NULL", 10, "expanded_group", "per t.depot: max(2 * x) by (t.region) <= 0"),
                ("D3", "NULL", 10, "expanded_group", "per t.depot: max(2 * x) by (t.region) <= 0"),
            ],
            id="easy-max-per-key",
        ),
        pytest.param(
            "max(2 * x) by (t.region) <= -10",
            [("NULL", str(row), 10, "expanded_row", "max(2 * x) by (t.region) <= 0") for row in range(4)],
            id="easy-max-no-per",
        ),
        pytest.param(
            "per t.region: max(x) by (t.region) <= min(t.cap) by (t.region) - 30",
            [
                (
                    "E",
                    "NULL",
                    20,
                    "expanded_group",
                    "per t.region: max(x) by (t.region) <= min(cap) by (t.region) - 30 + 20",
                ),
                (
                    "W",
                    "NULL",
                    15,
                    "expanded_group",
                    "per t.region: max(x) by (t.region) <= min(cap) by (t.region) - 30 + 15",
                ),
            ],
            id="easy-max-group-bound",
        ),
        pytest.param(
            "per t.depot: sum(x) + max(x) <= -5",
            [
                ("D1", "NULL", 5, "expanded_group", "per t.depot: sum(x) + max(x) <= 0"),
                ("D2", "NULL", 5, "expanded_group", "per t.depot: sum(x) + max(x) <= 0"),
                ("D3", "NULL", 5, "expanded_group", "per t.depot: sum(x) + max(x) <= 0"),
            ],
            id="composed-per-key",
        ),
        pytest.param(
            "when t.id > 0: sum(x) + max(x) <= -5",
            [("NULL", str(row), 5, "expanded_row", "when id > 0: sum(x) + max(x) <= 0") for row in range(4)],
            id="composed-no-per",
        ),
    ],
)
def test_every_grouped_clause_kind_reports_once_per_class(decidb_cli, clause, expected):
    """An easy MIN/MAX (a bound on each member, its group's bound moving once) and a
    composed clause (one row) report like a sum: once per class of the `per` key, or per
    kept row with none. The amounts are each group's own shortfall. (The easy MAX scales
    its body: over a bare decision it is a column bound, D41.)"""
    assert _findings(_diagnose(decidb_cli, clause)) == sorted(expected)
    # The query scope still folds each clause into one edit.
    folded = _findings(_diagnose(decidb_cli, clause, scope="query"))
    assert len(folded) == 1 and folded[0][0] == "NULL" and folded[0][1] == "NULL", folded


@pytest.mark.diagnose
def test_an_infinity_norm_echo_pastes_back(decidb_cli):
    """`norm(e, 'inf')` echoes with its order quoted -- `inf` alone names a column -- so
    the suggested change binds when pasted back."""
    rows = _diagnose(decidb_cli, "x >= 12 AND per (): norm(x - 5, 'inf') <= 3", scope="query")
    changes = {row["clause"]: row["suggested_change"] for row in rows}
    change = changes["per (): norm(x - 5, 'inf') <= 3"]
    assert change.startswith("per (): norm(x - 5, 'inf') <= "), rows
    pasted = decidb_cli.execute_raw(
        f"""
        SELECT id, x {table('t', T_COLUMNS, T_ROWS)}
        DECIDE x(INT)
        SUCH THAT x <= t.cap AND {change}
        MAXIMIZE SUM(x)"""
    )
    assert pasted.returncode == 0 and not pasted.stderr.strip(), pasted.stderr


@pytest.mark.diagnose
def test_a_per_key_equal_to_the_by_key_reports_once_per_group(decidb_cli):
    """`per t.region: ... by (t.region)`: the class is the group."""
    findings = _findings(_diagnose(decidb_cli, "per t.region: sum(x) by (t.region) >= 100"))
    change = "per t.region: sum(x) by (t.region) >= {}"
    assert findings == [
        ("E", "NULL", 40, "expanded_group", change.format(60)),
        ("W", "NULL", 85, "expanded_group", change.format(15)),
    ]


@pytest.mark.diagnose
def test_no_per_key_reports_once_per_kept_row(decidb_cli):
    """`sum(x) by (t.region) >= 100` with no `per` is `per row`: every kept row is a class
    of its own (ANR deck p. 17, 110), so the region's shortfall is reported on each of
    its rows, `row` set and `group` NULL."""
    findings = _findings(_diagnose(decidb_cli, "sum(x) by (t.region) >= 100"))
    change = "sum(x) by (t.region) >= {}"
    assert findings == [
        ("NULL", "0", 40, "expanded_row", change.format(60)),
        ("NULL", "1", 40, "expanded_row", change.format(60)),
        ("NULL", "2", 40, "expanded_row", change.format(60)),
        ("NULL", "3", 85, "expanded_row", change.format(15)),
    ]


@pytest.mark.diagnose
def test_a_when_in_front_limits_the_classes_to_the_kept_rows(decidb_cli):
    """`when t.id <> 2 per t.depot: ...`: row 2 is dropped, D1 still stands for region E
    through row 1; the shortfall counts the kept rows only (E: 10 + 30 = 40 reach)."""
    findings = _findings(_diagnose(decidb_cli, "when t.id <> 2 per t.depot: sum(x) by (t.region) >= 100"))
    change = "when id <> 2 per t.depot: sum(x) by (t.region) >= {}"
    assert findings == [
        ("D1", "NULL", 60, "expanded_group", change.format(40)),
        ("D2", "NULL", 60, "expanded_group", change.format(40)),
        ("D3", "NULL", 85, "expanded_group", change.format(15)),
    ]


@pytest.mark.diagnose
def test_the_query_scope_still_folds_a_clause_into_one_edit(decidb_cli):
    """The default scope reports one clause-level edit with no group, as before."""
    findings = _findings(_diagnose(decidb_cli, "per t.depot: sum(x) by (t.region) >= 100", scope="query"))
    assert len(findings) == 1 and findings[0][0] == "NULL" and findings[0][3] == "source_literal", findings


@pytest.mark.diagnose
def test_per_global_is_one_class(decidb_cli):
    """`per (): sum(x) by () >= 100` has one class and no key value: one finding, no group.
    The echo leaves out `by ()`, which an omitted `by` means."""
    findings = _findings(_diagnose(decidb_cli, "per (): sum(x) by () >= 100"))
    assert findings == [("NULL", "NULL", 25, "source_literal", "per (): sum(x) >= 75")]


@pytest.mark.diagnose
def test_per_global_equality_reports_once(decidb_cli):
    """`per (): max(2 * x) = -10`: its `<=` half bounds every member, and `per ()` is one
    class with no key value, so it reports once with no group or row, as `<= -10` alone
    does."""
    rows = _diagnose(decidb_cli, "per (): max(2 * x) = -10")
    assert [(row["group"], row["row"], int(float(row["amount"])), row["edit_source"]) for row in rows] == [
        ("NULL", "NULL", 10, "source_literal")
    ], rows


@pytest.mark.diagnose
@pytest.mark.parametrize("clause", ["max(x) by (t.region) <= -5", "per (): max(x) = -5"])
def test_an_easy_max_below_the_floor_is_refused_as_written(decidb_cli, clause):
    """An easy `max(x) <= -5` is the column bound `x <= -5` (D41), which a non-negative x
    cannot meet: refused before solving, as `x <= -5` is, naming the clause as written --
    under DIAGNOSE too."""
    sql = f"""
        SELECT id, x {table('t', T_COLUMNS, T_ROWS)}
        DECIDE x(INT) SUCH THAT x <= t.cap AND {clause} MAXIMIZE SUM(x)"""
    message = f"DECIDE optimization is infeasible: {clause} cannot hold because x is non-negative (>= 0)."
    assert message in decidb_cli.execute_raw(sql).stderr
    assert message in decidb_cli.execute_raw(f"DIAGNOSE {sql}").stderr


@pytest.mark.diagnose
@pytest.mark.parametrize(
    "clause, expected",
    [
        pytest.param(
            "per t.region: abs(sum(x) by (t.region) - 100) <= 5",
            [
                ("E", "NULL", 35, "expanded_group", "per t.region: abs(sum(x) by (t.region) - 100) <= 40"),
                ("W", "NULL", 80, "expanded_group", "per t.region: abs(sum(x) by (t.region) - 100) <= 85"),
            ],
            id="per-key",
        ),
        pytest.param(
            "per row: x >= 18",
            [("NULL", "0", 8, "expanded_row", "x >= 10"), ("NULL", "3", 3, "expanded_row", "x >= 15")],
            id="per-row",
        ),
    ],
)
def test_a_bound_folded_into_a_column_box_reports_per_class(decidb_cli, clause, expected):
    """The optimizer folds a constant bound on a bare decision into its column box -- the
    ABS pin over a helper, a `per row:` bound -- yet the clause still reports once per
    class: E reaches 60 (|60 - 100| = 40), W 15; rows 0 and 3 fall short of 18."""
    assert _findings(_diagnose(decidb_cli, clause)) == sorted(expected)
    folded = _findings(_diagnose(decidb_cli, clause, scope="query"))
    assert len(folded) == 1 and folded[0][0] == "NULL" and folded[0][1] == "NULL", folded


@pytest.mark.diagnose
def test_a_keyed_decision_bound_reports_per_class(decidb_cli):
    """`per t.region: y >= 0` on `y` declared per region is folded into y's column box too;
    its classes are still the regions, each held below its caps' sum less 100."""
    sql = f"""
        SELECT id, x {table('t', T_COLUMNS, T_ROWS)}
        DECIDE x(INT), per t.region: y(INT)
        SUCH THAT x <= t.cap AND per t.region: y <= sum(t.cap) by (t.region) - 100
              AND per t.region: y >= 0
        MAXIMIZE SUM(x)"""
    result = _diagnose_relation.run(decidb_cli, sql, scope="expanded")
    assert_no_internal_leak(result, "keyed y")
    rows = [row for row in _diagnose_relation.rows(result) if row["edit_source"] != "achievable_objective"]
    assert _findings(rows) == [
        ("E", "NULL", 40, "expanded_group", "per t.region: y >= -40"),
        ("W", "NULL", 85, "expanded_group", "per t.region: y >= -85"),
    ]


@pytest.mark.diagnose
def test_a_kept_row_finding_offsets_the_bound_as_written(decidb_cli):
    """`x >= 0.5 * sum(x) by (t.region) + 20` is canonicalized with its reducer on the
    left, so a row's own bound (20) is not in the query: each kept row's finding offsets
    the bound the user wrote. Region E's rows are each 20 short, W's row 12.5."""
    clause = "x >= 0.5 * sum(x) by (t.region) + 20"
    rows = _diagnose(decidb_cli, clause)
    assert sorted((row["row"], row["clause"], row["suggested_change"], row["edit_source"]) for row in rows) == [
        ("0", clause, clause + " - 20", "expanded_row"),
        ("1", clause, clause + " - 20", "expanded_row"),
        ("2", clause, clause + " - 20", "expanded_row"),
        ("3", clause, clause + " - 12.5", "expanded_row"),
    ]


@pytest.mark.diagnose
@pytest.mark.parametrize(
    "clause, query, expanded",
    [
        pytest.param(
            "per t.depot: sum(x) by (t.region) >= 'inf'::DOUBLE",
            [("D1", "NULL"), ("D2", "NULL"), ("D3", "NULL")],
            [("D1", "NULL"), ("D2", "NULL"), ("D3", "NULL")],
            id="per-key",
        ),
        pytest.param(
            "sum(x) by (t.region) >= 'inf'::DOUBLE",
            [("NULL", "NULL")],
            [("NULL", "0"), ("NULL", "1"), ("NULL", "2"), ("NULL", "3")],
            id="no-per",
        ),
        pytest.param("per (): sum(x) >= 'inf'::DOUBLE", [("NULL", "NULL")], [("NULL", "NULL")], id="per-global"),
    ],
)
def test_an_unreachable_bound_names_its_classes(decidb_cli_gurobi, clause, query, expanded):
    """An unreachable bound names the values of its `per` key in either scope -- never the
    `by` groups (E, W) -- and the kept rows of a clause with no `per` key in the expanded
    scope only. Gurobi only: HiGHS refuses an infinite row bound when the model loads."""
    for scope, expected in (("query", query), ("expanded", expanded)):
        rows = _diagnose(decidb_cli_gurobi, clause, scope=scope)
        assert all(row["edit_source"] == "unreachable_bound" for row in rows), rows
        assert sorted((row["group"], row["row"]) for row in rows) == expected, (scope, rows)


@pytest.mark.diagnose
def test_an_absorbed_bound_shares_no_slack_with_a_composed_clause(decidb_cli):
    """`per row: w <= 5` is folded into w's column box and reaches the repair model as its
    own rows, numbered among the clauses; the composed clause's row is numbered after all
    of them. Moving the `<>` clauses into indicator rows used to land the composed row on
    the bound's id, and the bound -- no part of the conflict -- was named in its place."""
    sql = """
        SELECT k, x, y, z FROM (VALUES (1), (2)) t(k)
        DECIDE x(INT), y(INT), z(INT), w(INT)
        SUCH THAT x <= 10 AND y <= 10 AND z <= 10 AND x <> 3 AND y <> 3 AND z <> 3
              AND SUM(z) >= 10 AND SUM(z) >= 9 AND per row: w <= 5 AND MAX(y) + SUM(z) <= 4
        MAXIMIZE SUM(x)"""
    result = _diagnose_relation.run(decidb_cli, sql)
    assert_no_internal_leak(result, "composed and absorbed")
    rows = [row for row in _diagnose_relation.rows(result) if row["edit_source"] != "achievable_objective"]
    assert sorted((row["clause"], row["suggested_change"], int(float(row["amount"]))) for row in rows) == [
        ("max(y) + sum(z) <= 4", "max(y) + sum(z) <= 9", 5),
        ("sum(z) >= 10", "sum(z) >= 9", 1),
    ], rows


@pytest.mark.diagnose
@pytest.mark.parametrize("clause", ["min(x) <= 12", "per (): min(x) <= 12"])
def test_a_min_bound_over_a_keyed_decision_is_diagnosed(decidb_cli, clause):
    """`x >= 15` floors every keyed `x`, so `min(x) <= 12` must rise to 15; it once went undiagnosed."""
    sql = f"""
        SELECT id, x {table('t', T_COLUMNS, T_ROWS)}
        DECIDE per t.depot: x(INT) SUCH THAT x >= 15 AND x <= 20 AND {clause}
        MAXIMIZE SUM(x)"""
    result = _diagnose_relation.run(decidb_cli, sql)
    assert_no_internal_leak(result, clause)
    rows = [row for row in _diagnose_relation.rows(result) if row["edit_source"] != "achievable_objective"]
    assert [(row["clause"], row["suggested_change"]) for row in rows] == [(clause, clause.replace("12", "15"))], rows


@pytest.mark.diagnose
def test_a_dropped_ne_leaves_the_composed_clause_its_own_slack(decidb_cli):
    """The five `x <> -k` hold for every x >= 0, so formulation drops them without a row;
    the composed clause's repair id used to land on `per row: x >= 18`'s then, and the bound
    was reported alone with a repair that left the composed clause broken."""
    rows = _diagnose(
        decidb_cli,
        "x <> -1 AND x <> -2 AND x <> -3 AND x <> -4 AND x <> -5 AND "
        "per (): sum(x) + max(x) <= -5 AND per row: x >= 18",
        scope="query",
    )
    assert sorted((row["clause"], row["suggested_change"], int(float(row["amount"]))) for row in rows) == [
        ("per (): sum(x) + max(x) <= -5", "per (): sum(x) + max(x) <= 0", 5),
        ("x >= 18", "x >= 0", 18),
    ], rows


@pytest.mark.diagnose
@pytest.mark.parametrize(
    "clause, suggestions",
    [
        pytest.param("x > sum(x) by (t.region) + 3", ["x > sum(x) by (t.region) + 3 - 4"] * 4, id="strict"),
        pytest.param(
            "x = 0.5 * sum(x) by (t.region) + 20",
            ["x = 0.5 * sum(x) by (t.region) + 20 - 20"] * 3 + ["x = 0.5 * sum(x) by (t.region) + 20 - 12.5"],
            id="equal",
        ),
    ],
)
def test_an_offset_bound_keeps_its_operator_and_moves_the_right_way(decidb_cli, clause, suggestions):
    """An offset over the written bound echoes the clause as written: a strict `>` stays
    strict -- its row was stepped onto the lattice, and the written bound moves by the same
    amount -- and an `=` moves by its signed slack, down here, since `0.5 * sum + 20` must
    fall to meet each row. The query scope's edit pastes back and solves."""
    rows = _diagnose(decidb_cli, clause)
    assert [row["clause"] for row in rows] == [clause] * len(suggestions), rows
    assert sorted(row["suggested_change"] for row in rows) == sorted(suggestions), rows
    folded = _diagnose(decidb_cli, clause, scope="query")
    assert [(row["clause"], row["suggested_change"]) for row in folded] == [(clause, suggestions[0])], folded
    pasted = decidb_cli.execute_raw(
        f"""
        SELECT id, x {table('t', T_COLUMNS, T_ROWS)}
        DECIDE x(INT)
        SUCH THAT x <= t.cap AND {suggestions[0]}
        MAXIMIZE SUM(x)"""
    )
    assert pasted.returncode == 0 and not pasted.stderr.strip(), pasted.stderr


@pytest.mark.diagnose
def test_the_unbounded_remedy_names_a_key_column_as_written(decidb_cli):
    """The cap DIAGNOSE prescribes for an unbounded slice names the column as the clause
    does -- `t.depot`, a key column, already SQL -- so it pastes back and binds."""
    sql = f"""
        SELECT id, x {table('t', T_COLUMNS, T_ROWS)}
        DECIDE x(INT)
        SUCH THAT when t.region = 'E': x <= t.cap AND per t.depot: sum(x) by (t.depot) >= 1
        MAXIMIZE SUM(x)"""
    result = _diagnose_relation.run(decidb_cli, sql)
    assert_no_internal_leak(result, "unbounded remedy")
    assert [(row["state"], row["suggested_change"]) for row in _diagnose_relation.rows(result)] == [
        ("unbounded", "when t.depot = 'D3': x <= <cap>")
    ], result.stdout
    pasted = decidb_cli.execute_raw(sql.replace("MAXIMIZE", "AND when t.depot = 'D3': x <= 7 MAXIMIZE"))
    assert pasted.returncode == 0 and not pasted.stderr.strip(), pasted.stderr


@pytest.mark.diagnose
@pytest.mark.quadratic
@pytest.mark.parametrize(
    "clause, expected",
    [
        pytest.param(
            "per t.depot: sum(power(x - 20, 2)) by (t.region) <= 4",
            [
                ("D1", "NULL", 96, "expanded_group"),
                ("D2", "NULL", 96, "expanded_group"),
                ("D3", "NULL", 21, "expanded_group"),
            ],
            id="per-key",
        ),
        pytest.param(
            "sum(power(x - 20, 2)) by (t.region) <= 4",
            [("NULL", str(row), 96, "expanded_row") for row in range(3)] + [("NULL", "3", 21, "expanded_row")],
            id="no-per",
        ),
        pytest.param(
            "per row: power(x - 20, 2) <= 4",
            [("NULL", "0", 96, "expanded_row"), ("NULL", "3", 21, "expanded_row")],
            id="per-row",
        ),
    ],
)
def test_a_quadratic_clause_reports_per_class(decidb_cli_gurobi, clause, expected):
    """A quadratic row keeps its own slack (I2.d) and, in the expanded scope, reports the
    classes it stands for like any other row: region E's squares come to 100 at best, W's
    to 25 (row 0 alone is 100 short of its 20, row 3 25). Gurobi only: a quadratic row."""
    rows = _diagnose(decidb_cli_gurobi, clause)
    assert sorted(finding[:4] for finding in _findings(rows)) == sorted(expected), rows


@pytest.mark.diagnose
def test_diagnose_echo_of_a_decision_beside_its_reducer_pastes_back(decidb_cli):
    """`per row: x >= 0.5 * sum(x) by () + 1` echoes as the bare clause it means (D32),
    and the suggested change pastes back: a bare clause follows the scope rules (D30)."""
    rows = _diagnose(decidb_cli, "per row: x >= 0.5 * sum(x) by () + 1", scope="query")
    assert rows[0]["clause"] == "x >= 0.5 * sum(x) + 1", rows
    change = rows[0]["suggested_change"]
    assert change.startswith("x >= 0.5 * sum(x) + 1 - "), rows
    pasted = decidb_cli.execute_raw(
        f"""
        SELECT id, x {table('t', T_COLUMNS, T_ROWS)}
        DECIDE x(INT) SUCH THAT x <= t.cap AND {change} MAXIMIZE SUM(x)"""
    )
    assert pasted.returncode == 0 and not pasted.stderr.strip(), pasted.stderr


@pytest.mark.diagnose
def test_diagnose_names_functions_as_the_scope_spellings_write_them(decidb_cli):
    """A clause's echo prints its functions lower-case."""
    rows = _diagnose(decidb_cli, "when t.id = 1: abs(x - 50) <= 3", scope="query")
    assert rows[0]["clause"] == "when id = 1: abs(x - 50) <= 3", rows
    assert rows[0]["suggested_change"].startswith("when id = 1: abs(x - 50) <= "), rows


@pytest.mark.error
@pytest.mark.parametrize(
    "clause, quoted",
    [
        pytest.param("per row: x >= 'inf'::DOUBLE", "`x >= inf`", id="per-row"),
        pytest.param("when t.id > 1: x >= 'inf'::DOUBLE", "`when id > 1: x >= inf`", id="when"),
    ],
)
def test_a_highs_refusal_quotes_the_scope_in_front(decidb_cli_highs, clause, quoted):
    """HiGHS cannot load an infinite row bound, and its refusal quotes the clause as it is
    written: the scope in front, not trailing after it with its colon."""
    out = decidb_cli_highs.execute_raw(
        f"""
        SELECT id, x {table('t', T_COLUMNS, T_ROWS)}
        DECIDE x(INT)
        SUCH THAT x <= t.cap AND {clause}
        MAXIMIZE SUM(x)"""
    )
    assert quoted in out.stderr, out.stderr
