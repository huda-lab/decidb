"""A Big-M rewrite over an unbounded decision variable is refused, not guessed.

ABS, MIN/MAX and ``<>`` are encoded with an indicator and a constant M that has to
dominate the reachable magnitude of the row. DeciDB derives M from the actual
variable boxes and the evaluated coefficients. When a contributing variable has no
finite bound there is no such constant — and the failure mode of guessing one is the
worst kind: a query with a guessed M that the true range exceeds does not error, it
silently cuts the feasible region and returns a confidently wrong optimum.

DeciDB used to take ``max(M, 1e6)`` in exactly that situation for MIN/MAX and ``<>``,
while the ABS path already refused. The two now agree, on **both** backends: the
alternative — leaving one backend on the 1e6 floor — means the same query answers
correctly on Gurobi and wrongly on HiGHS, with no story for the divergence. There is
no fixed Big-M anywhere in DeciDB any more.

The refusal names a column the user can bound and the edit that bounds it. It fires
only when the bound is genuinely unknowable: a box derived by implied-bound
propagation counts, and so does one derived for an ABS auxiliary, which is why
``MAX(ABS(x - 5)) >= 3`` over a bounded ``x`` still answers.

**It is a property of the LOWERING path, not of the query.** A backend that expresses
the construct natively needs no Big-M and therefore no bound, so it answers — see
``test_native_constructs``. The invariant these tests defend is not "every backend
refuses"; it is "no backend answers *wrongly*". Where both must lower, both refuse, and
identically. That is why the cases below pin ``DECIDB_NATIVE_CONSTRUCTS=off``: the
claim is about the encoding, and running them on whatever the host happens to support
would test the host instead.

Covers:
  - test_hard_minmax_over_unbounded_variable_is_refused: the MIN/MAX path
  - test_not_equal_over_unbounded_variable_is_refused: per-row and aggregate ``<>``
  - test_composed_minmax_over_unbounded_variable_is_refused: the auxiliary-family path
  - test_refusal_is_identical_on_both_backends: the divergence this closes
  - test_derived_bound_still_answers: propagation and ABS boxing are real bounds
  - test_abs_auxiliary_box_is_derived: the ABS auxiliary gets a finite column box
"""

import os
import re
import tempfile

import pytest

from decidb_cli import DecidBCliError

# Three rows is enough for every shape here; the point is the model, not the data.
_ROWS = "(VALUES (1),(2),(3)) t(id)"


@pytest.mark.min_max
@pytest.mark.cons_aggregate
@pytest.mark.error
def test_hard_minmax_over_unbounded_variable_is_refused(decidb_cli):
    """``MAX(x) >= 5`` lowered needs an indicator, and an indicator needs a finite M."""
    lowering = decidb_cli.with_env({"DECIDB_NATIVE_CONSTRUCTS": "off"})
    with pytest.raises(DecidBCliError) as excinfo:
        lowering.execute(f"""
            SELECT id, x FROM {_ROWS}
            DECIDE x(REAL)
            SUCH THAT MAX(x) >= 5
            MINIMIZE SUM(x)
        """)
    message = excinfo.value.message
    assert re.search(r"finite bound on 'x'", message), message
    # The remedy has to be an edit the user can actually type.
    assert "x >= <lower>" in message and "x <= <upper>" in message, message


@pytest.mark.cons_comparison
@pytest.mark.error
@pytest.mark.parametrize("constraint", ["x <> 5", "SUM(x) <> 5"])
def test_not_equal_over_unbounded_variable_is_refused(decidb_cli, constraint):
    """Both ``<>`` spellings are disjunctions, and a disjunction needs a finite M.

    Per-row expands in place; the aggregate form is deferred until the flat columns
    exist. They took the same 1e6 floor and now take the same refusal.
    """
    lowering = decidb_cli.with_env({"DECIDB_NATIVE_CONSTRUCTS": "off"})
    with pytest.raises(DecidBCliError) as excinfo:
        lowering.execute(f"""
            SELECT id, x FROM {_ROWS}
            DECIDE x(INT)
            SUCH THAT {constraint}
            MAXIMIZE SUM(x)
        """)
    assert re.search(r"finite bound on 'x'", excinfo.value.message), excinfo.value.message


@pytest.mark.min_max
@pytest.mark.cons_mixed
@pytest.mark.error
def test_composed_minmax_over_unbounded_variable_is_refused(decidb_cli):
    """A composed reducer pins a global auxiliary, which needs a family-wide M.

    ``x >= -100`` bounds x from below only, which is what keeps implied-bound
    propagation from supplying an upper bound and lets this reach the auxiliary
    Big-M at all.
    """
    lowering = decidb_cli.with_env({"DECIDB_NATIVE_CONSTRUCTS": "off"})
    with pytest.raises(DecidBCliError) as excinfo:
        lowering.execute(f"""
            SELECT id, x FROM {_ROWS}
            DECIDE x(REAL)
            SUCH THAT x >= -100 AND SUM(x) + MAX(x) >= -10
            MINIMIZE SUM(x)
        """)
    assert re.search(r"finite bound on 'x'", excinfo.value.message), excinfo.value.message


@pytest.mark.min_max
@pytest.mark.error
def test_refusal_is_identical_on_both_backends(decidb_cli_highs, decidb_cli_gurobi):
    """The divergence this closes: where both backends lower, both give one answer.

    Leaving HiGHS on the 1e6 floor would mean this query errors on Gurobi and returns
    a quietly wrong optimum on HiGHS — one right answer and one wrong one, with no
    story for the difference. Both now refuse, in the same words.

    Pinned to the lowering path on purpose. A backend that expresses MIN/MAX natively
    answers this query correctly, so the two backends *do* differ once native paths
    exist — but nobody gets a wrong answer, which is the invariant that matters.
    """
    sql = f"""
        SELECT id, x FROM {_ROWS}
        DECIDE x(REAL)
        SUCH THAT MAX(x) >= 5
        MINIMIZE SUM(x)
    """
    lowering = {"DECIDB_NATIVE_CONSTRUCTS": "off"}
    with pytest.raises(DecidBCliError) as highs_error:
        decidb_cli_highs.with_env(lowering).execute(sql)
    with pytest.raises(DecidBCliError) as gurobi_error:
        decidb_cli_gurobi.with_env(lowering).execute(sql)
    assert highs_error.value.message == gurobi_error.value.message


@pytest.mark.min_max
@pytest.mark.correctness
def test_derived_bound_still_answers(decidb_cli):
    """A bound DeciDB can derive is a bound. The refusal must not over-fire.

    Two derivations are exercised. ``SUM(x) <= 10`` with the default ``x >= 0``
    floor gives implied-bound propagation ``x <= 10``. And ``MAX(ABS(x - 5)) >= 3``
    reaches the outer indicator through an ABS auxiliary, whose own column box is
    derived from x's — without that, the outer Big-M would see an unbounded column
    and refuse a query whose bound was there to be computed.
    """
    lowering = decidb_cli.with_env({"DECIDB_NATIVE_CONSTRUCTS": "off"})
    rows, cols = lowering.execute(f"""
        SELECT id, x FROM {_ROWS}
        DECIDE x(REAL)
        SUCH THAT SUM(x) <= 10 AND MAX(x) >= 5
        MINIMIZE SUM(x)
    """)
    ci = {c: i for i, c in enumerate(cols)}
    xs = [float(r[ci["x"]]) for r in rows]
    assert abs(max(xs) - 5.0) <= 1e-4, f"expected max(x)=5, got {xs}"

    rows, cols = lowering.execute(f"""
        SELECT id, x FROM {_ROWS}
        DECIDE x(REAL)
        SUCH THAT x >= 4 AND x <= 20 AND MAX(ABS(x - 5)) >= 3
        MINIMIZE SUM(x)
    """)
    ci = {c: i for i, c in enumerate(cols)}
    xs = [float(r[ci["x"]]) for r in rows]
    assert abs(sum(xs) - 16.0) <= 1e-3, f"expected SUM=16, got {xs}"


@pytest.mark.min_max
@pytest.mark.correctness
def test_abs_auxiliary_box_is_derived(decidb_cli):
    """On the lowering path, every column must carry a finite box.

    Read off the model dump rather than the answer, because a too-wide box does not
    change the optimum here — it changes the Big-M every later rewrite derives from
    it, and (before this) whether the query was refused at all. With ``x in [4, 20]``
    the auxiliary ranges over ``|x - 5| in [0, 15]``, so its upper bound is finite and
    the outer hard-MAX gets a real Big-M instead of a refusal.

    Pinned to the lowering path (``DECIDB_NATIVE_CONSTRUCTS=off``) deliberately: that
    is the path where the box is load-bearing. A backend that expresses ABS natively
    adds a free argument column on purpose — it needs no Big-M, which is exactly the
    capability's payoff — so "no unbounded column anywhere" is a claim about lowering,
    not about the model in general. See test_native_abs_needs_no_bound.
    """
    sql = f"""
        SELECT id, x FROM {_ROWS}
        DECIDE x(REAL)
        SUCH THAT x >= 4 AND x <= 20 AND MAX(ABS(x - 5)) >= 3
        MINIMIZE SUM(x)
    """
    with tempfile.TemporaryDirectory() as tmp:
        dump_path = os.path.join(tmp, "model.dump")
        cli = decidb_cli.with_env(
            {"DECIDB_DUMP_MODEL": dump_path, "DECIDB_NATIVE_CONSTRUCTS": "off"}
        )
        cli.execute(sql)
        with open(dump_path) as f:
            dump = f.read()

    assert "num_genconstrs: 0" in dump, f"expected the lowering path:\n{dump}"
    uppers = [float(m) for m in re.findall(r"^col \d+: lb=\S+ ub=(\S+)", dump, re.MULTILINE)]
    assert uppers, f"no columns in dump:\n{dump}"
    assert all(ub < 1e20 for ub in uppers), (
        f"an unbounded column survived; every box should be derived:\n{dump}"
    )
