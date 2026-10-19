"""Data-only aggregate RHS in aggregate constraints.

`SUM(x*val) <= SUM(val)` (RHS aggregate over data columns only) is evaluated in
place: the right-hand side reduces each data reducer to one value per group
(EvaluateRhsReducerPerGroup), honouring the reducer's own `when`, the constraint's
`when`/`per`, and relation-qualifier de-duplication. These tests pin (a) parity with
the explicit hoisted form the binder used to produce, (b) parity with the
scalar-bound equivalent, and (c) `when` / `per` / AVG composition, including
per-group RHS values.

See context/descriptions/03_expressivity/sql_functions/done.md.
"""

import pytest

from ._scope_cases import answer, scope_solver  # noqa: F401 -- scope_solver is a fixture
from ._scope_oracle import CONSTANT, Decision, Reduce, Scoped, ScopedModel, ScopedObjective
from solver.types import ObjSense, VarType


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
    DECIDE x(BOOL)
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
    """`per` carries a per-group RHS. Here every group's budget is 10 (SUM(b) over
    2 rows), so each group admits exactly one item: the higher-profit one."""
    # grp 0 → id 2 (profit 2 > 1); grp 1 → id 4 (profit 4 > 3).
    assert _selected(decidb_cli, "per grp: sum(x * weight) by (grp) <= sum(b) by (grp)") == [2, 4]


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
        DECIDE x(BOOL)
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
    """A `when` inside a bound-side reducer is the reducer's own: it scopes only that
    reducer's rows, not the whole constraint. In `SUM(x*weight) <= sum(when w: b)`
    the moved term keeps its filter, so the bound is SUM(b) over w-rows (= 15) while
    the LHS still sums all rows."""
    direct = _selected(decidb_cli, "SUM(x * weight) <= sum(when w: b)")
    hoist = _selected(decidb_cli, "SUM(x * weight) - sum(when w: b) <= 0")
    # 10*SUM(x) <= 15 over all rows ⇒ at most one item ⇒ highest profit (id 4).
    assert direct == [4]
    assert direct == hoist


@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_rhs_minmax_bound(decidb_cli):
    """MIN/MAX on the RHS is evaluated by the reducer evaluator, not the hoist.

    The hoist could only ever move SUM and AVG, because the left side reduces a
    data term by *summing a column* — MIN cannot be obtained by adding things up.
    `b` is 5.0 on every row, so MIN(b) = 5 caps the weight-10 items at zero.
    """
    assert _selected(decidb_cli, "SUM(x * weight) <= MIN(b)") == []
    # MAX over the same column agrees, since b is constant.
    assert _selected(decidb_cli, "SUM(x * weight) <= MAX(b)") == []


@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_rhs_count_is_group_aware(decidb_cli, scope_solver):
    """COUNT(*) counts the constraint's own rows.

    It used to be folded to the operator's total input cardinality, which was a
    wrong answer as soon as `when` or `per` made the constraint's row set smaller.
    Here COUNT(*) = 4, so `SUM(x) <= COUNT(*)` admits every item. `count(*) by (grp)`
    counts each group's two rows: less one, it admits one item per group, where the
    operator's total would admit all four.
    """
    assert _selected(decidb_cli, "SUM(x) <= COUNT(*)") == [1, 2, 3, 4]
    rows = [{"id": 1, "grp": 0, "profit": 1.0}, {"id": 2, "grp": 0, "profit": 2.0},
            {"id": 3, "grp": 1, "profit": 3.0}, {"id": 4, "grp": 1, "profit": 4.0}]
    model = ScopedModel(rows, [Decision("x", var_type=VarType.BINARY)])
    clauses = [Scoped((Reduce("sum", lambda r: {"x": 1}, by=("grp",)),), "<=",
                      (Reduce("sum", lambda r: {CONSTANT: 1}, by=("grp",)), lambda r: {CONSTANT: -1}),
                      per=("grp",))]
    objective = ScopedObjective(ObjSense.MAXIMIZE, (Reduce("sum", lambda r: {"x": r["profit"]}),))
    result = answer(decidb_cli, _DATA.format(cons="per grp: sum(x) by (grp) <= count(*) by (grp) - 1") + " ORDER BY id")
    assert model.judge_scoped(result, clauses, objective) == pytest.approx(
        model.solve_scoped(scope_solver, clauses, objective), abs=1e-4)


@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_rhs_avg_mixed_with_other_terms(decidb_cli):
    """`<= 2 * AVG(b)` solves, and keeps AVG's fractional value.

    This used to be refused. The refusal was never about the arithmetic — the RHS
    evaluator already divided correctly — but about a type: DecideOptimizer rewrote
    every AVG to SUM, which redeclares the node with SUM's integral type while its
    value stays fractional, and this is the one place a reducer's value is handed
    back to a surrounding expression bound against that type. The rewrite now skips
    decision-free aggregates, so the node stays a real AVG and the round trip is
    DOUBLE->DOUBLE.

    A bare `<= AVG(b)` never exercised this: the bind-time hoist moved it left and
    scaled it there. That hoist is gone (the canonicalization refactor), so both forms now go
    through this path.
    """
    # AVG(b) = 5.0, so the bound is 10 and exactly one weight-10 item fits: the
    # highest-profit one.
    assert _selected(decidb_cli, "SUM(x * weight) <= 2 * AVG(b)") == [4]

    # Fractional AVG, which is what actually discriminates: AVG(b) = 1.5, so the
    # bound is 3 and three items fit. Truncating AVG to 1 would give a bound of 2
    # and select only two -- the silently wrong answer the old guard was protecting
    # against.
    rows, cols = decidb_cli.execute("""
        WITH k(id, profit, b) AS (
            VALUES (1, 1.0, 1.0), (2, 2.0, 1.0), (3, 3.0, 1.0), (4, 4.0, 3.0)
        )
        SELECT id, x FROM k
        DECIDE x(BOOL)
        SUCH THAT SUM(x) <= 2 * AVG(b)
        MAXIMIZE SUM(x * profit)
    """)
    xi, ii = cols.index("x"), cols.index("id")
    picked = sorted(int(r[ii]) for r in rows if int(round(float(r[xi]))) == 1)
    assert picked == [2, 3, 4], f"expected 3 items (bound 2*1.5=3), got {picked}"
