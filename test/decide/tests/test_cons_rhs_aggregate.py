"""Data-only aggregate RHS in aggregate constraints.

`SUM(x*val) <= SUM(val)` (RHS aggregate over data columns only) is supported by
hoisting the RHS aggregate into the LHS as a data-only additive term:
`SUM(x*val) - SUM(val) <= 0`. This reuses the existing aggregate-body / WHEN /
PER / AVG machinery, so these tests pin (a) parity with the explicit hoisted
form, (b) parity with the scalar-bound equivalent, and (c) WHEN / PER / AVG
composition, including per-group RHS values.

See context/descriptions/03_expressivity/sql_functions/done.md.
"""

import pytest


# Deterministic knapsack: 4 unit-weight-10 items, unique optimum in every case.
#   b = per-item budget contribution (SUM over active rows = the bound)
_DATA = """
    WITH k(id, grp, weight, profit, b, w) AS (
        VALUES (1, 0, 10.0, 1.0, 5.0, true),
               (2, 0, 10.0, 2.0, 5.0, false),
               (3, 1, 10.0, 3.0, 5.0, true),
               (4, 1, 10.0, 4.0, 5.0, true)
    )
    SELECT id, x FROM k
    DECIDE x IS BOOLEAN
    SUCH THAT {cons}
    MAXIMIZE SUM(x * profit)
"""


def _selected(decidb_cli, cons):
    rows, cols = decidb_cli.execute(_DATA.format(cons=cons))
    xi, ii = cols.index("x"), cols.index("id")
    return sorted(int(r[ii]) for r in rows if int(round(float(r[xi]))) == 1)


@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_rhs_sum_matches_explicit_hoist(decidb_cli):
    """SUM(x*weight) <= SUM(b) is identical to SUM(x*weight) - SUM(b) <= 0."""
    direct = _selected(decidb_cli, "SUM(x * weight) <= SUM(b)")
    hoist = _selected(decidb_cli, "SUM(x * weight) - SUM(b) <= 0")
    assert direct == hoist


@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_rhs_sum_matches_scalar_bound(decidb_cli):
    """SUM(b) = 20 over all four rows, so the RHS aggregate equals `<= 20`:
    budget for two unit items → pick the two highest-profit (ids 3, 4)."""
    assert _selected(decidb_cli, "SUM(x * weight) <= SUM(b)") == [3, 4]
    assert _selected(decidb_cli, "SUM(x * weight) <= 20") == [3, 4]


@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_rhs_sum_plus_scalar(decidb_cli):
    """A scalar term stays on the RHS: SUM(b) + 10 = 30 → three items (2, 3, 4)."""
    assert _selected(decidb_cli, "SUM(x * weight) <= SUM(b) + 10") == [2, 3, 4]


@pytest.mark.cons_aggregate
@pytest.mark.per_clause
@pytest.mark.correctness
def test_rhs_sum_per_group_bound(decidb_cli):
    """PER carries a per-group RHS. Here every group's budget is 10 (SUM(b) over
    2 rows), so each group admits exactly one item: the higher-profit one."""
    # grp 0 → id 2 (profit 2 > 1); grp 1 → id 4 (profit 4 > 3).
    assert _selected(decidb_cli, "SUM(x * weight) <= SUM(b) PER grp") == [2, 4]


@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_rhs_avg_matches_scalar(decidb_cli):
    """AVG on both sides shares the group denominator. AVG(x*profit) <= AVG(cap)
    with cap = 2 everywhere reduces to SUM(x*profit) <= 2*N; here it caps the
    average profit-per-row, matching the explicit hoisted form."""
    sql = """
        WITH k(id, profit, cap) AS (
            VALUES (1, 4.0, 2.0), (2, 4.0, 2.0), (3, 4.0, 2.0), (4, 4.0, 2.0)
        )
        SELECT id, x FROM k
        DECIDE x IS BOOLEAN
        SUCH THAT {cons}
        MAXIMIZE SUM(x)
    """
    direct, dc = decidb_cli.execute(sql.format(cons="AVG(x * profit) <= AVG(cap)"))
    hoist, hc = decidb_cli.execute(sql.format(cons="AVG(x * profit) - AVG(cap) <= 0"))
    n_direct = sum(int(round(float(r[dc.index("x")]))) for r in direct)
    n_hoist = sum(int(round(float(r[hc.index("x")]))) for r in hoist)
    # AVG(4x) <= 2  ⇒  average of 4x over 4 rows <= 2  ⇒  SUM(x) <= 2.
    assert n_direct == 2
    assert n_direct == n_hoist


@pytest.mark.cons_aggregate
@pytest.mark.when_constraint
@pytest.mark.correctness
def test_rhs_aggregate_local_when(decidb_cli):
    """A WHEN trailing a bare RHS aggregate is aggregate-local: it scopes only that
    aggregate's rows, not the whole constraint. `SUM(x*weight) <= SUM(b) WHEN w`
    means `SUM(x*weight) <= (SUM(b) WHEN w)` — the moved term keeps its filter, so
    the bound is SUM(b) over w-rows (= 15) while the LHS still sums all rows."""
    direct = _selected(decidb_cli, "SUM(x * weight) <= SUM(b) WHEN w")
    hoist = _selected(decidb_cli, "SUM(x * weight) - (SUM(b) WHEN w) <= 0")
    # 10*SUM(x) <= 15 over all rows ⇒ at most one item ⇒ highest profit (id 4).
    assert direct == [4]
    assert direct == hoist


@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_rhs_minmax_still_rejected(decidb_cli):
    """MIN/MAX RHS aggregates are not hoisted — a clean error, no stack trace."""
    decidb_cli.assert_error(
        _DATA.format(cons="SUM(x * weight) <= MIN(b)"),
        match=r"can't be reduced to a scalar bound.*MIN/MAX/COUNT",
    )


@pytest.mark.cons_aggregate
@pytest.mark.error
def test_rhs_scalar_times_avg_error_is_not_misleading(decidb_cli):
    """`<= 2 * AVG(b)` isn't hoistable (the hoist only walks additive trees), so it
    errors. The message must be actionable and must NOT surface the internal name
    'sum' — AVG is rewritten to sum/count before the error site, and quoting that
    would name an aggregate the user never wrote."""
    result = decidb_cli.execute_raw(_DATA.format(cons="SUM(x * weight) <= 2 * AVG(b)"))
    combined = result.stderr + result.stdout
    assert "can't be reduced to a scalar bound" in combined, combined[:400]
    assert "unsupported aggregate 'sum'" not in combined, (
        "error still leaks the misleading internal 'sum' name"
    )
    # No internal-error path either.
    for tok in ("INTERNAL Error", "Stack Trace"):
        assert tok not in combined, f"found {tok!r}: {combined[:400]}"
