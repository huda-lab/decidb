"""Differential tests for the norm(expr, p) regularization function.

norm() is desugared at bind time into existing supported forms:
  norm(e, 1)     -> SUM(ABS(e))         L1
  norm(e, 2)     -> SUM(POWER(e, 2))    squared L2 / ridge
  norm(e, 'inf') -> MAX(ABS(e))         L-infinity
  norm(e, 0, M)  -> indicator + Big-M; term becomes SUM(z)   L0 / count

Each correctness test asserts norm() reaches the SAME optimal objective value as
its hand-written equivalent (robust to alternate optima), plus the error paths.
"""

import pytest

from decidb_cli import DecidBCliError

# Sole-norm objective so the achieved norm value *is* the objective (unique at
# the optimum even when the assignment is not). l_orderkey <= 3 keeps it small.
_BASE = """
    SELECT l_orderkey, l_linenumber, l_quantity, new_qty
    FROM lineitem WHERE l_orderkey <= 3
    DECIDE new_qty IS REAL
    SUCH THAT SUM(new_qty) = 100
    MINIMIZE {obj}
"""


def _devs(rows, cols):
    """new_qty - l_quantity for each row."""
    nq, lq = cols.index("new_qty"), cols.index("l_quantity")
    return [float(r[nq]) - float(r[lq]) for r in rows]


@pytest.mark.correctness
def test_norm_l1_matches_sum_abs(decidb_cli):
    r1, c1 = decidb_cli.execute(_BASE.format(obj="norm(new_qty - l_quantity, 1)"))
    r2, c2 = decidb_cli.execute(_BASE.format(obj="SUM(ABS(new_qty - l_quantity))"))
    v1 = sum(abs(d) for d in _devs(r1, c1))
    v2 = sum(abs(d) for d in _devs(r2, c2))
    assert v1 == pytest.approx(v2, abs=1e-4)


@pytest.mark.correctness
def test_norm_l2_matches_sum_power(decidb_cli):
    r1, c1 = decidb_cli.execute(_BASE.format(obj="norm(new_qty - l_quantity, 2)"))
    r2, c2 = decidb_cli.execute(_BASE.format(obj="SUM(POWER(new_qty - l_quantity, 2))"))
    v1 = sum(d * d for d in _devs(r1, c1))
    v2 = sum(d * d for d in _devs(r2, c2))
    assert v1 == pytest.approx(v2, abs=1e-3)


@pytest.mark.correctness
def test_norm_linf_matches_max_abs(decidb_cli):
    r1, c1 = decidb_cli.execute(_BASE.format(obj="norm(new_qty - l_quantity, 'inf')"))
    r2, c2 = decidb_cli.execute(_BASE.format(obj="MAX(ABS(new_qty - l_quantity))"))
    v1 = max(abs(d) for d in _devs(r1, c1))
    v2 = max(abs(d) for d in _devs(r2, c2))
    assert v1 == pytest.approx(v2, abs=1e-4)


@pytest.mark.correctness
def test_norm_l0_matches_handrolled(decidb_cli):
    """norm(e, 0, M) (auto indicator) == hand-rolled z + ABS(e)<=M*z + SUM(z)."""
    r1, c1 = decidb_cli.execute(_BASE.format(obj="norm(new_qty - l_quantity, 0, 100)"))
    hand = """
        SELECT l_orderkey, l_linenumber, l_quantity, new_qty
        FROM lineitem WHERE l_orderkey <= 3
        DECIDE new_qty IS REAL, z IS BOOLEAN
        SUCH THAT SUM(new_qty) = 100 AND ABS(new_qty - l_quantity) <= 100 * z
        MINIMIZE SUM(z)
    """
    r2, c2 = decidb_cli.execute(hand)
    n1 = sum(1 for d in _devs(r1, c1) if abs(d) > 1e-6)
    n2 = sum(1 for d in _devs(r2, c2) if abs(d) > 1e-6)
    assert n1 == n2


@pytest.mark.correctness
def test_norm_l0_count_constraint(decidb_cli):
    """norm(e, 0, M) <= K caps the number of changed rows at K."""
    # Target just above the baseline sum so it is reachable by changing one row,
    # keeping the <= 2 cap feasible (a target far from baseline would need many).
    base_rows, _ = decidb_cli.execute(
        "SELECT SUM(l_quantity) AS s FROM lineitem WHERE l_orderkey <= 3")
    target = float(base_rows[0][0]) + 5
    sql = f"""
        SELECT l_orderkey, l_linenumber, l_quantity, new_qty
        FROM lineitem WHERE l_orderkey <= 3
        DECIDE new_qty IS REAL
        SUCH THAT SUM(new_qty) = {target} AND norm(new_qty - l_quantity, 0, 1000) <= 2
        MINIMIZE SUM(ABS(new_qty - l_quantity))
    """
    rows, cols = decidb_cli.execute(sql)
    changed = sum(1 for d in _devs(rows, cols) if abs(d) > 1e-6)
    assert changed <= 2


@pytest.mark.correctness
def test_norm_l0_auto_m_matches_explicit(decidb_cli):
    """norm(e, 0) (auto, data-driven M) matches explicit norm(e, 0, M) with a safe M."""
    auto, ca = decidb_cli.execute(_BASE.format(obj="norm(new_qty - l_quantity, 0)"))
    exp, ce = decidb_cli.execute(_BASE.format(obj="norm(new_qty - l_quantity, 0, 1000)"))
    n_auto = sum(1 for d in _devs(auto, ca) if abs(d) > 1e-6)
    n_exp = sum(1 for d in _devs(exp, ce) if abs(d) > 1e-6)
    assert n_auto == n_exp


def test_norm_l0_negative_bound_rejected(decidb_cli):
    with pytest.raises(DecidBCliError, match=r"positive"):
        decidb_cli.execute(_BASE.format(obj="norm(new_qty - l_quantity, 0, -5)"))


def test_norm_unsupported_order(decidb_cli):
    with pytest.raises(DecidBCliError, match=r"[Uu]nsupported norm order|Supported"):
        decidb_cli.execute(_BASE.format(obj="norm(new_qty - l_quantity, 3)"))


# --- compositions ---------------------------------------------------------

@pytest.mark.correctness
def test_norm_with_when_objective(decidb_cli):
    """norm(e,1) WHEN cond  ==  SUM(ABS(e)) WHEN cond (penalize one group only)."""
    r1, c1 = decidb_cli.execute(
        _BASE.format(obj="norm(new_qty - l_quantity, 1) WHEN l_orderkey = 1"))
    r2, c2 = decidb_cli.execute(
        _BASE.format(obj="SUM(ABS(new_qty - l_quantity)) WHEN l_orderkey = 1"))

    def val(rows, cols):
        ok, nq, lq = cols.index("l_orderkey"), cols.index("new_qty"), cols.index("l_quantity")
        return sum(abs(float(r[nq]) - float(r[lq])) for r in rows if int(r[ok]) == 1)

    assert val(r1, c1) == pytest.approx(val(r2, c2), abs=1e-4)


@pytest.mark.correctness
def test_norm_with_per_constraint(decidb_cli):
    """norm(e,1) <= K PER g  ==  SUM(ABS(e)) <= K PER g, and the per-group cap holds."""
    norm_sql = """
        SELECT l_orderkey, l_linenumber, l_quantity, new_qty
        FROM lineitem WHERE l_orderkey <= 3
        DECIDE new_qty IS REAL
        SUCH THAT new_qty >= 1 AND norm(new_qty - l_quantity, 1) <= 5 PER l_orderkey
        MINIMIZE SUM(new_qty)
    """
    plain_sql = norm_sql.replace("norm(new_qty - l_quantity, 1) <= 5",
                                 "SUM(ABS(new_qty - l_quantity)) <= 5")
    r1, c1 = decidb_cli.execute(norm_sql)
    r2, c2 = decidb_cli.execute(plain_sql)
    tot1 = sum(float(r[c1.index("new_qty")]) for r in r1)
    tot2 = sum(float(r[c2.index("new_qty")]) for r in r2)
    assert tot1 == pytest.approx(tot2, abs=1e-4)
    # per-group total deviation must respect the cap of 5
    ok, nq, lq = c1.index("l_orderkey"), c1.index("new_qty"), c1.index("l_quantity")
    by_grp = {}
    for r in r1:
        by_grp[int(r[ok])] = by_grp.get(int(r[ok]), 0.0) + abs(float(r[nq]) - float(r[lq]))
    for v in by_grp.values():
        assert v <= 5 + 1e-4


# --- HiGHS backend (L2 is a QP, L0 is a MILP — confirm both backends agree) ---

@pytest.mark.correctness
def test_norm_on_highs(decidb_cli_highs):
    # L1
    r1, c1 = decidb_cli_highs.execute(_BASE.format(obj="norm(new_qty - l_quantity, 1)"))
    r2, c2 = decidb_cli_highs.execute(_BASE.format(obj="SUM(ABS(new_qty - l_quantity))"))
    assert sum(abs(d) for d in _devs(r1, c1)) == pytest.approx(
        sum(abs(d) for d in _devs(r2, c2)), abs=1e-3)
    # L2 (QP on HiGHS)
    r3, c3 = decidb_cli_highs.execute(_BASE.format(obj="norm(new_qty - l_quantity, 2)"))
    r4, c4 = decidb_cli_highs.execute(_BASE.format(obj="SUM(POWER(new_qty - l_quantity, 2))"))
    assert sum(d * d for d in _devs(r3, c3)) == pytest.approx(
        sum(d * d for d in _devs(r4, c4)), abs=1e-3)
    # L0 (MILP on HiGHS)
    r5, c5 = decidb_cli_highs.execute(_BASE.format(obj="norm(new_qty - l_quantity, 0, 100)"))
    assert r5  # solved and returned rows
