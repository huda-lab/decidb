"""Tests for the `<>` range collapse.

`LHS <> K` is a disjunction only when `K` sits strictly inside the range the LHS can
actually reach. When the reachable range lies wholly on one side of `K`, one branch is
dead and the surviving branch is a plain inequality — no indicator term, no Big-M, and an
LP relaxation that is tight instead of empty. The classic `SUM(x) <> 0` over decisions
that cannot go negative is exactly this: it is `SUM(x) >= 1`.

These assert on the model dump, because results cannot see the difference: the collapsed
and the disjunctive encodings describe the same feasible set, so every optimum is
unchanged by construction. What changes is the number of rows and whether a Big-M
coefficient appears at all.

The soundness boundary has its own tests at the bottom. The collapse may only read the
*rigid* box — a variable's intrinsic domain — because a bound the user wrote is re-emitted
as a loosenable row during infeasibility diagnosis, and a collapse that baked one in could
not be reverted with it.
"""

import re

import pytest


def _rows(dump: str) -> list[str]:
    """The dumped model's constraint rows, in row order."""
    return re.findall(r"^row \d+: (.*)$", dump, re.M)


def _has_bigm(dump: str) -> bool:
    """Does any row carry a large negative coefficient (a Big-M indicator term)?

    The `<>` disjunction wires -M onto its indicator column; a collapsed row has no
    indicator term at all, so no coefficient below -1.
    """
    for row in _rows(dump):
        for coef in re.findall(r"\d+:(-?[\d.e+]+)", row):
            if float(coef) < -1.0:
                return True
    return False


@pytest.mark.cons_comparison
@pytest.mark.correctness
def test_aggregate_ne_zero_collapses_to_a_single_tight_row(decidb_cli, tmp_path):
    """`SUM(x) <> 0` over BOOL decisions is `SUM(x) >= 1` — the "pick at least one" case.

    A BOOL decision's intrinsic [0, 1] domain is rigid, so the aggregate cannot reach
    below 0 and the `<= -1` branch of the disjunction is dead.
    """
    dump = decidb_cli.dump_model(
        """
            SELECT id, x FROM (VALUES (1), (2), (3), (4)) t(id)
            DECIDE x(BOOL) SUCH THAT SUM(x) <> 0 MINIMIZE SUM(x)
        """,
        tmp_path / "agg_ne_zero.dump")

    rows = _rows(dump)
    assert len(rows) == 1, f"expected one collapsed row, got {len(rows)}:\n{dump}"
    assert "sense=> rhs=1" in rows[0], f"expected `>= 1`, got: {rows[0]}"
    assert not _has_bigm(dump), f"collapsed row should carry no Big-M:\n{dump}"


@pytest.mark.cons_comparison
@pytest.mark.correctness
def test_per_row_ne_zero_collapses(decidb_cli, tmp_path):
    """The per-row path collapses too: `x <> 0` on a BOOL decision is `x >= 1`."""
    dump = decidb_cli.dump_model(
        """
            SELECT id, x FROM (VALUES (1), (2), (3)) t(id)
            DECIDE x(BOOL) SUCH THAT x <> 0 MINIMIZE SUM(x)
        """,
        tmp_path / "perrow_ne_zero.dump")

    rows = _rows(dump)
    assert len(rows) == 3, f"expected one row per data row, got {len(rows)}:\n{dump}"
    assert all("sense=> rhs=1" in r for r in rows), f"expected `>= 1` rows:\n{dump}"
    assert not _has_bigm(dump), f"collapsed rows should carry no Big-M:\n{dump}"


@pytest.mark.cons_comparison
@pytest.mark.correctness
def test_intrinsic_nonnegativity_is_enough_to_collapse(decidb_cli, tmp_path):
    """An INT decision is non-negative by default, and that floor is rigid.

    The query bounds only the upper side, so nothing the user wrote is doing the work
    here — the collapse rests on the intrinsic domain alone.
    """
    dump = decidb_cli.dump_model(
        """
            SELECT id, x FROM (VALUES (1), (2), (3), (4)) t(id)
            DECIDE x(INT) SUCH THAT x <= 5 AND SUM(x) <> 0 MINIMIZE SUM(x)
        """,
        tmp_path / "intrinsic_floor.dump")

    rows = _rows(dump)
    assert len(rows) == 1, f"expected one collapsed row, got {len(rows)}:\n{dump}"
    assert "sense=> rhs=1" in rows[0], f"expected `>= 1`, got: {rows[0]}"


@pytest.mark.cons_comparison
@pytest.mark.correctness
def test_interior_k_keeps_the_disjunction(decidb_cli, tmp_path):
    """`SUM(x) <> 2` with SUM(x) reaching 0..4 is genuinely two-sided — keep the Big-M.

    This is the direction that would silently break if the classifier were too eager:
    collapsing an interior K would cut a whole half of the feasible set.
    """
    dump = decidb_cli.dump_model(
        """
            SELECT id, x FROM (VALUES (1), (2), (3), (4)) t(id)
            DECIDE x(BOOL) SUCH THAT SUM(x) <> 2 MINIMIZE SUM(x)
        """,
        tmp_path / "interior_k.dump")

    assert len(_rows(dump)) == 2, f"expected the two-row disjunction:\n{dump}"
    assert _has_bigm(dump), f"an interior K still needs its Big-M:\n{dump}"


@pytest.mark.cons_comparison
@pytest.mark.correctness
def test_user_written_bound_does_not_license_a_collapse(decidb_cli, tmp_path):
    """`x >= 0` written by the user is loosenable, so it may not be baked into the shape.

    Identical in feasible set to the intrinsic-floor query above, and identical in
    optimum — but the bound is now a clause the elastic engine re-emits as a loosenable
    row when diagnosing infeasibility. A collapse reading it would still be enforcing
    `SUM(x) >= 1` in a repaired model where the user's floor had been relaxed away.

    So this must keep the disjunction. If a future change makes absorbed user bounds
    rigid under diagnosis, this test should be revisited rather than deleted.
    """
    dump = decidb_cli.dump_model(
        """
            SELECT id, x FROM (VALUES (1), (2), (3), (4)) t(id)
            DECIDE x(INT) SUCH THAT x >= 0 AND x <= 5 AND SUM(x) <> 0 MINIMIZE SUM(x)
        """,
        tmp_path / "user_written_floor.dump")

    assert len(_rows(dump)) == 2, f"a loosenable floor must not collapse:\n{dump}"
    assert _has_bigm(dump), f"a loosenable floor must not collapse:\n{dump}"


@pytest.mark.cons_comparison
@pytest.mark.correctness
def test_collapsed_and_disjunctive_forms_agree_on_the_optimum(decidb_cli):
    """The two encodings must describe the same feasible set.

    The pair above differ only in whether the floor was written out, which changes the
    encoding but must not change the answer.
    """
    collapsed, _ = decidb_cli.execute("""
        SELECT id, x FROM (VALUES (1), (2), (3), (4)) t(id)
        DECIDE x(INT) SUCH THAT x <= 5 AND SUM(x) <> 0 MINIMIZE SUM(x)
    """)
    disjunctive, _ = decidb_cli.execute("""
        SELECT id, x FROM (VALUES (1), (2), (3), (4)) t(id)
        DECIDE x(INT) SUCH THAT x >= 0 AND x <= 5 AND SUM(x) <> 0 MINIMIZE SUM(x)
    """)
    x_of = lambda rows: sorted(r[-1] for r in rows)
    assert sum(x_of(collapsed)) == sum(x_of(disjunctive)) == 1


@pytest.mark.cons_comparison
@pytest.mark.correctness
def test_collapsed_ne_is_still_removable_in_diagnosis(decidb_cli):
    """A collapsed `<>` keeps the remove-only repair the disjunctive one gets.

    `x <> 0 AND x <> 1` on a BOOL decision collapses both clauses (`x >= 1` and
    `x <= 0`) and is infeasible, and neither clause is loosenable — so dropping one is
    the only repair, and the engine must be able to actually neutralize a collapsed row
    to find it.

    That takes the range-derived fallback for the removal Big-M, which is normally read
    off the row's own coefficient on the indicator column — a coefficient a collapsed
    row does not have. Without the fallback the removal is offered but inert, and the
    engine reports the worse of the two drops (`x <> 1`, objective 2) instead of the
    reachable best one.
    """
    script = (
        ".mode csv\n"
        "PRAGMA diagnose_decide='auto';\n"
        "SELECT id, x FROM (VALUES (1), (2)) t(id) "
        "DECIDE x(BOOL) SUCH THAT x <> 0 AND x <> 1 MINIMIZE SUM(x);\n"
        "SELECT * FROM decide_diagnostics();\n"
    )
    proc = decidb_cli.execute_script(script)
    text = proc.stdout + proc.stderr

    assert "drop" in text, f"a collapsed `<>` should still be droppable:\n{text}"
    assert "x <> 0" in text, f"expected the reachable repair to be named:\n{text}"
    assert "achievable_objective,0" in text.replace(" ", ""), (
        f"dropping `x <> 0` reaches objective 0, not 2:\n{text}")


@pytest.mark.cons_comparison
@pytest.mark.var_boolean
@pytest.mark.correctness
def test_bool_domain_reaches_the_bigm_derivation(decidb_cli, tmp_path):
    """A BOOL decision's [0,1] ceiling must reach the box every Big-M is derived from.

    The ceiling used to be applied only at model-build time, while `SolverInput`'s
    bounds still said 1e30. Every Big-M derivation reads that box through
    `DecideRowTermRange`, which treats >= 1e20 as unbounded, so a declared BOOL with no
    written upper bound collapsed to the fallback constant: this query emitted
    M = 1000000 where the identical feasible set spelled `x(INT) ... x <= 1` emitted 7.

    Asserted against the INT spelling rather than against a literal, so the test pins
    "the two agree" rather than one particular tight value.
    """
    def bigm(dump: str) -> float:
        """Largest indicator magnitude in the model — the Big-M the disjunction used."""
        magnitudes = [
            abs(float(c))
            for row in _rows(dump)
            for c in re.findall(r"\d+:(-?[\d.e+]+)", row)
            if abs(float(c)) > 1.0
        ]
        assert magnitudes, f"expected a Big-M coefficient:\n{dump}"
        return max(magnitudes)

    as_bool = decidb_cli.dump_model(
        """
            SELECT id, x FROM (VALUES (1), (2), (3), (4)) t(id)
            DECIDE x(BOOL) SUCH THAT SUM(x) <> 2 MINIMIZE SUM(x)
        """,
        tmp_path / "bool_bigm.dump")
    as_int = decidb_cli.dump_model(
        """
            SELECT id, x FROM (VALUES (1), (2), (3), (4)) t(id)
            DECIDE x(INT) SUCH THAT x <= 1 AND SUM(x) <> 2 MINIMIZE SUM(x)
        """,
        tmp_path / "int_bigm.dump")

    assert bigm(as_bool) == bigm(as_int), (
        f"BOOL and INT spellings of one feasible set disagree on M:\n"
        f"BOOL:\n{as_bool}\nINT:\n{as_int}"
    )
    # And it is a tight constant, not the DECIDE_BIGM_FALLBACK.
    assert bigm(as_bool) < 1000.0, f"M looks like the fallback constant:\n{as_bool}"


@pytest.mark.cons_comparison
@pytest.mark.var_boolean
@pytest.mark.correctness
def test_bool_ceiling_licenses_an_unreachable_drop(decidb_cli, tmp_path):
    """With the ceiling in the box, the collapse can see a BOOL's upper side too.

    Four BOOL decisions total at most 4, so `<> 9` excludes nothing and the constraint
    should vanish entirely. While the ceiling was invisible this emitted the two-row
    disjunction against an unreachable bound.
    """
    dump = decidb_cli.dump_model(
        """
            SELECT id, x FROM (VALUES (1), (2), (3), (4)) t(id)
            DECIDE x(BOOL) SUCH THAT SUM(x) <> 9 MINIMIZE SUM(x)
        """,
        tmp_path / "bool_unreachable.dump")
    assert _rows(dump) == [], f"an unreachable `<>` should emit no rows:\n{dump}"
