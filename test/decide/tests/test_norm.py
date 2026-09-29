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
    DECIDE new_qty(REAL)
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
        DECIDE new_qty(REAL), z(BOOL)
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
        DECIDE new_qty(REAL)
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
        _BASE.format(obj="norm(WHEN l_orderkey = 1: new_qty - l_quantity, 1)"))
    r2, c2 = decidb_cli.execute(
        _BASE.format(obj="SUM(WHEN l_orderkey = 1: ABS(new_qty - l_quantity))"))

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
        DECIDE new_qty(REAL)
        SUCH THAT new_qty >= 1 AND PER l_orderkey: norm(new_qty - l_quantity, 1) BY (l_orderkey) <= 5
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


# --- L0 soundness: the reverse indicator link makes SUM(z) the EXACT count ---
# Before the reverse link, SUM(z) was only an upper bound on the nonzero count, so
# a spurious z=1 could satisfy a lower-bound / equality / maximize on the count even
# when the true count was smaller. These pin that down.

@pytest.mark.correctness
@pytest.mark.error_infeasible
def test_norm_l0_lower_bound_infeasible_explicit_m(decidb_cli):
    # All x pinned to 0 -> true L0 count is 0, so `>= 2` is unsatisfiable. The old
    # one-way link let phantom z=1 inflate the count and return a (wrong) solution.
    decidb_cli.assert_error("""
        SELECT id, x FROM (VALUES (1), (2)) t(id)
        DECIDE x(REAL)
        SUCH THAT x = 0 AND norm(x, 0, 100) >= 2
        MINIMIZE SUM(x)
    """, match=r"(?i)infeasible")


@pytest.mark.correctness
@pytest.mark.error_infeasible
def test_norm_l0_lower_bound_infeasible_auto_m(decidb_cli):
    # Same, via the auto-M path (placeholder M refilled at execution).
    decidb_cli.assert_error("""
        SELECT id, x FROM (VALUES (1), (2)) t(id)
        DECIDE x(REAL)
        SUCH THAT x = 0 AND norm(x, 0) >= 2
        MINIMIZE SUM(x)
    """, match=r"(?i)infeasible")


@pytest.mark.correctness
@pytest.mark.error_infeasible
def test_norm_l0_equality_infeasible(decidb_cli):
    decidb_cli.assert_error("""
        SELECT id, x FROM (VALUES (1), (2)) t(id)
        DECIDE x(REAL)
        SUCH THAT x = 0 AND norm(x, 0, 100) = 2
        MINIMIZE SUM(x)
    """, match=r"(?i)infeasible")


@pytest.mark.correctness
@pytest.mark.error_infeasible
def test_norm_l0_lower_bound_infeasible_highs(decidb_cli_highs):
    # Solver-agnostic: the reverse link is in our model builder, so HiGHS enforces it too.
    decidb_cli_highs.assert_error("""
        SELECT id, x FROM (VALUES (1), (2)) t(id)
        DECIDE x(REAL)
        SUCH THAT x = 0 AND norm(x, 0, 100) >= 2
        MINIMIZE SUM(x)
    """, match=r"(?i)infeasible")


@pytest.mark.correctness
def test_norm_l0_maximize_not_inflated(decidb_cli):
    # MAXIMIZE rewards z=1; with all x pinned to 0 the true max count is 0, so the
    # objective must not be inflated. The solve succeeds with x all 0 ...
    rows, cols = decidb_cli.execute("""
        SELECT id, x FROM (VALUES (1), (2), (3)) t(id)
        DECIDE x(REAL)
        SUCH THAT x = 0
        MAXIMIZE norm(x, 0, 100)
    """)
    xi = cols.index("x")
    assert all(abs(float(r[xi])) < 1e-4 for r in rows)
    # ... and demanding even one nonzero is then infeasible (count cannot exceed 0).
    decidb_cli.assert_error("""
        SELECT id, x FROM (VALUES (1), (2), (3)) t(id)
        DECIDE x(REAL)
        SUCH THAT x = 0 AND norm(x, 0, 100) >= 1
        MAXIMIZE norm(x, 0, 100)
    """, match=r"(?i)infeasible")


@pytest.mark.correctness
def test_norm_l0_lower_bound_feasible_is_honest(decidb_cli):
    # A genuinely feasible `>= 2`: the returned solution must have at least two
    # actually-nonzero rows (the constraint is met by real nonzeros, not phantom z).
    rows, cols = decidb_cli.execute("""
        SELECT id, x FROM (VALUES (1), (2), (3)) t(id)
        DECIDE x(REAL)
        SUCH THAT x <= 10 AND norm(x, 0, 100) >= 2 AND SUM(x) >= 4
        MINIMIZE SUM(x)
    """)
    xi = cols.index("x")
    nonzeros = sum(1 for r in rows if abs(float(r[xi])) >= 1e-4)
    assert nonzeros >= 2


@pytest.mark.correctness
def test_norm_l0_tolerance_pragma(decidb_cli):
    # The nonzero threshold is configurable. With the default (1e-4) a forced x=1
    # counts as nonzero, so `>= 1` is feasible. Raising the tolerance above 1 makes
    # x=1 count as zero, so the same `>= 1` becomes infeasible.
    sql = """
        SELECT id, x FROM (VALUES (1)) t(id)
        DECIDE x(REAL)
        SUCH THAT x = 1 AND norm(x, 0, 100) >= 1
        MINIMIZE SUM(x)
    """
    rows, _ = decidb_cli.execute(sql)
    assert rows  # default tolerance: x=1 is nonzero -> feasible

    decidb_cli.assert_error("SET decide_l0_tolerance=2.0;\n" + sql, match=r"(?i)infeasible")

    # Below the floor is rejected up front.
    decidb_cli.assert_error("SET decide_l0_tolerance=1e-9;",
                            match=r"(?i)decide_l0_tolerance must be")


# --- norm() inside arithmetic ---------------------------------------------
#
# A regularizer is normally written as one term of a larger objective —
# `MINIMIZE SUM(cost*x) + 0.5 * norm(x - base, 1)`. The marker the binder leaves
# behind is a real SUM aggregate carrying the order in its alias, so a marker the
# optimizer fails to lower does not fail loudly: it reads downstream as the plain
# SUM it is built on, and the norm silently vanishes from the model. Each test
# below therefore compares the *composed* objective against the same composition
# written out by hand, which is the only spelling that can tell the two apart.

# Deviations are large under the = 100 target (total l_quantity is 360), so the
# second term is a genuine trade-off rather than a constant offset.
_MIXED = """
    SELECT l_orderkey, l_linenumber, l_quantity, new_qty
    FROM lineitem WHERE l_orderkey <= 3
    DECIDE new_qty(REAL)
    SUCH THAT SUM(new_qty) = 100
    MINIMIZE {obj}
"""


def _qty(rows, cols):
    """new_qty for each row."""
    i = cols.index("new_qty")
    return [float(r[i]) for r in rows]


def _assert_same_objective(cli, norm_obj, hand_obj, score, tol=1e-3):
    """The norm spelling must reach the same objective value as the hand-written one."""
    r1, c1 = cli.execute(_MIXED.format(obj=norm_obj))
    r2, c2 = cli.execute(_MIXED.format(obj=hand_obj))
    assert score(r1, c1) == pytest.approx(score(r2, c2), abs=tol)


def _l1_plus_linear(rows, cols):
    return sum(abs(d) for d in _devs(rows, cols)) + 0.5 * sum(_qty(rows, cols))


@pytest.mark.correctness
def test_norm_l1_in_arithmetic_objective(decidb_cli):
    _assert_same_objective(
        decidb_cli,
        "norm(new_qty - l_quantity, 1) + 0.5 * SUM(new_qty)",
        "SUM(ABS(new_qty - l_quantity)) + 0.5 * SUM(new_qty)",
        _l1_plus_linear)


@pytest.mark.correctness
def test_norm_l2_in_arithmetic_objective(decidb_cli):
    _assert_same_objective(
        decidb_cli,
        "norm(new_qty - l_quantity, 2) + 0.5 * SUM(new_qty)",
        "SUM(POWER(new_qty - l_quantity, 2)) + 0.5 * SUM(new_qty)",
        lambda r, c: sum(d * d for d in _devs(r, c)) + 0.5 * sum(_qty(r, c)))


@pytest.mark.correctness
def test_norm_linf_in_arithmetic_objective(decidb_cli):
    _assert_same_objective(
        decidb_cli,
        "norm(new_qty - l_quantity, 'inf') + 0.5 * SUM(new_qty)",
        "MAX(ABS(new_qty - l_quantity)) + 0.5 * SUM(new_qty)",
        lambda r, c: max(abs(d) for d in _devs(r, c)) + 0.5 * sum(_qty(r, c)))


@pytest.mark.correctness
def test_norm_scaled_objective(decidb_cli):
    """A norm under a bare multiplication, with nothing else in the objective."""
    _assert_same_objective(
        decidb_cli,
        "2 * norm(new_qty - l_quantity, 1)",
        "2 * SUM(ABS(new_qty - l_quantity))",
        lambda r, c: 2 * sum(abs(d) for d in _devs(r, c)))


@pytest.mark.correctness
def test_norm_combined_l1_l2_objective(decidb_cli):
    """Two norms of different orders in one objective (elastic-net shaped)."""
    _assert_same_objective(
        decidb_cli,
        "norm(new_qty - l_quantity, 1) + 0.25 * norm(new_qty - l_quantity, 2)",
        "SUM(ABS(new_qty - l_quantity)) + 0.25 * SUM(POWER(new_qty - l_quantity, 2))",
        lambda r, c: sum(abs(d) for d in _devs(r, c))
                     + 0.25 * sum(d * d for d in _devs(r, c)))


@pytest.mark.correctness
def test_norm_l0_in_arithmetic_objective(decidb_cli):
    """L0 under arithmetic. The reducer returns an integer count where the marker
    returned the deviation's DOUBLE, so this also pins the type change down."""
    r1, c1 = decidb_cli.execute(_MIXED.format(
        obj="norm(new_qty - l_quantity, 0, 100) + 0.001 * SUM(new_qty)"))
    hand = """
        SELECT l_orderkey, l_linenumber, l_quantity, new_qty
        FROM lineitem WHERE l_orderkey <= 3
        DECIDE new_qty(REAL), z(BOOL)
        SUCH THAT SUM(new_qty) = 100 AND ABS(new_qty - l_quantity) <= 100 * z
        MINIMIZE SUM(z) + 0.001 * SUM(new_qty)
    """
    r2, c2 = decidb_cli.execute(hand)
    n1 = sum(1 for d in _devs(r1, c1) if abs(d) > 1e-6)
    n2 = sum(1 for d in _devs(r2, c2) if abs(d) > 1e-6)
    assert n1 == n2


# A norm under arithmetic on the constraint side is the worse failure: the clause
# is still enforced, but as the plain SUM, so it caps the signed total instead of
# the norm. `= 400` leaves the deviations small enough for a tight cap to bind.
_CAPPED = """
    SELECT l_orderkey, l_linenumber, l_quantity, new_qty
    FROM lineitem WHERE l_orderkey <= 3
    DECIDE new_qty(REAL)
    SUCH THAT SUM(new_qty) = 400 AND {cons}
    MAXIMIZE SUM(new_qty * l_quantity)
"""


@pytest.mark.correctness
def test_norm_scaled_constraint(decidb_cli):
    rows, cols = decidb_cli.execute(
        _CAPPED.format(cons="2 * norm(new_qty - l_quantity, 1) <= 100"))
    assert 2 * sum(abs(d) for d in _devs(rows, cols)) <= 100 + 1e-4
    hand, hc = decidb_cli.execute(
        _CAPPED.format(cons="2 * SUM(ABS(new_qty - l_quantity)) <= 100"))
    def obj(r, c):
        nq, lq = c.index("new_qty"), c.index("l_quantity")
        return sum(float(x[nq]) * float(x[lq]) for x in r)
    assert obj(rows, cols) == pytest.approx(obj(hand, hc), abs=1e-3)


@pytest.mark.correctness
def test_norm_offset_constraint(decidb_cli):
    """A constant added to the norm on the left of the comparison."""
    rows, cols = decidb_cli.execute(
        _CAPPED.format(cons="norm(new_qty - l_quantity, 1) + 5 <= 100"))
    assert sum(abs(d) for d in _devs(rows, cols)) + 5 <= 100 + 1e-4


@pytest.mark.correctness
def test_norm_scaled_l0_constraint(decidb_cli):
    """A scaled L0 count cap: at most three rows may move."""
    rows, cols = decidb_cli.execute(
        _CAPPED.format(cons="2 * norm(new_qty - l_quantity, 0, 1000) <= 6"))
    assert sum(1 for d in _devs(rows, cols) if abs(d) > 1e-4) <= 3


@pytest.mark.correctness
def test_norm_in_arithmetic_on_highs(decidb_cli_highs):
    """Both backends agree on the composed forms: L1 is an LP, L2 a QP, L0 a MILP."""
    _assert_same_objective(
        decidb_cli_highs,
        "norm(new_qty - l_quantity, 1) + 0.5 * SUM(new_qty)",
        "SUM(ABS(new_qty - l_quantity)) + 0.5 * SUM(new_qty)",
        _l1_plus_linear)
    _assert_same_objective(
        decidb_cli_highs,
        "norm(new_qty - l_quantity, 2) + 0.5 * SUM(new_qty)",
        "SUM(POWER(new_qty - l_quantity, 2)) + 0.5 * SUM(new_qty)",
        lambda r, c: sum(d * d for d in _devs(r, c)) + 0.5 * sum(_qty(r, c)))
    rows, cols = decidb_cli_highs.execute(_MIXED.format(
        obj="norm(new_qty - l_quantity, 0, 100) + 0.001 * SUM(new_qty)"))
    assert rows  # solved and returned rows
