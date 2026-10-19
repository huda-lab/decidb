"""Every auxiliary column stage 05 introduces must carry a derived box.

An auxiliary stands for a known expression — the extremum of a MIN/MAX reducer over
rows, over per-group values, or over group sums — so the range it can reach is always
computable at the moment it is created. Leaving one declared `[-1e30, 1e30]` is not a
wrong answer, which is exactly why it went unnoticed: it is a performance cliff. A free
continuous column gives the root simplex no box to start from, so it walks to the answer
one pivot at a time, and the walk lengthens with row count. Measured on the benchmark
suite's `MAXIMIZE MAX` query at 15K rows: 27,293 root simplex iterations over 28.75s
free, against 61 iterations and 0.02s boxed — same model, same node count, same optimum.

The box comes from `AuxRange`, which the same walk produces alongside the Big-M constant,
so the two can never disagree about what the expression reaches. These tests assert on the
model dump because results cannot see any of it: the optimum is identical either way.

**Pinned to the lowering path.** Each case below asserts an exact auxiliary *count*, which
is a fact about the Big-M formulation — a backend that expresses MIN/MAX natively builds a
different, equally valid set of columns. `DECIDB_NATIVE_CONSTRUCTS=off` keeps the count
meaningful; the invariant that survives both paths ("no auxiliary is left free where a range
is derivable") is asserted without a count in
`test_native_auxiliaries_are_boxed_too`.

Covers one query per auxiliary-creation site:
  - test_flat_minmax_objective_auxiliary_is_boxed: flat `z`, both directions
  - test_per_inner_minmax_auxiliaries_are_boxed: per-group `z_g`
  - test_per_outer_minmax_over_group_values_is_boxed: `w` over the `z_g`s
  - test_per_outer_minmax_over_group_sums_is_boxed: `w` over group sums
  - test_composed_minmax_term_auxiliaries_are_boxed: composed `z_k` per term
  - test_unbounded_variable_keeps_whichever_end_it_can: per end, including the one
    legitimate fully-free case
  - test_native_auxiliaries_are_boxed_too: the invariant, on the native formulation
  - test_half_open_range_keeps_its_closed_side: one open end does not forfeit the other
"""

import re

import pytest


def _lowering(cli):
    """The Big-M formulation, whatever the host's solver can express natively.

    Every count below is a property of that formulation. Reading it off whichever
    formulation the host happens to pick would test the host instead.
    """
    return cli.with_env({"DECIDB_NATIVE_CONSTRUCTS": "off"})


def _continuous_columns(dump: str) -> list[tuple[int, float, float]]:
    """`(index, lb, ub)` of every non-integer column in the dumped model.

    Every query here declares its decision variables `INT` or `BOOL`, so a continuous
    column is necessarily an auxiliary the linearizer introduced — no need to know how
    many, or in what order, the formulation chose to emit them.
    """
    out = []
    for m in re.finditer(r"^col (\d+): lb=(\S+) ub=(\S+) int=0 bin=0", dump, re.M):
        out.append((int(m[1]), float(m[2]), float(m[3])))
    return out


def _assert_all_boxed(dump: str, expected_count: int) -> list[tuple[int, float, float]]:
    aux = _continuous_columns(dump)
    assert len(aux) == expected_count, \
        f"expected {expected_count} auxiliary column(s), got {len(aux)}:\n{dump}"
    for idx, lb, ub in aux:
        assert lb > -1e20 and ub < 1e20, \
            f"auxiliary col {idx} left free at [{lb}, {ub}]:\n{dump}"
    return aux


# Two rows, coefficients 3 and 5, with `x` capped at 4: every per-row expression
# `x * c` reaches at most 5 * 4 = 20, so any auxiliary reducing over rows is boxed
# by [0, 20].
_ROWS = "(VALUES (1,3),(2,5)) t(id,c)"

# Three rows in two groups: group 'a' holds coefficients 3 and 5, group 'b' holds 2.
_GROUPED_ROWS = "(VALUES (1,'a',3),(2,'a',5),(3,'b',2)) t(id,g,c)"


@pytest.mark.min_max
@pytest.mark.correctness
@pytest.mark.parametrize("sense,extra", [
    ("MAXIMIZE", "AND SUM(x) <= 6"),   # hard direction: indicators + Big-M
    ("MINIMIZE", "AND SUM(x) >= 3"),   # easy direction: one-sided envelope pin
])
def test_flat_minmax_objective_auxiliary_is_boxed(decidb_cli, tmp_path, sense, extra):
    """The flat `MAXIMIZE/MINIMIZE MAX(expr)` auxiliary — the site the cliff was found on.

    Both directions are checked: the hard one because it is where the cost showed up, the
    easy one because its auxiliary is created by the same call and would regress silently
    (the one-sided pin plus outer pressure still lands on the right answer).
    """
    dump = _lowering(decidb_cli).dump_model(
        f"""
            SELECT id, x FROM {_ROWS}
            DECIDE x(INT) SUCH THAT x <= 4 {extra}
            {sense} MAX(x * c)
        """,
        tmp_path / f"flat_{sense.lower()}.dump")
    assert _assert_all_boxed(dump, 1) == [(2, 0.0, 20.0)], \
        f"flat MIN/MAX auxiliary is not boxed by the per-row range:\n{dump}"


@pytest.mark.min_max
@pytest.mark.per_clause
@pytest.mark.correctness
def test_per_inner_minmax_auxiliaries_are_boxed(decidb_cli, tmp_path):
    """One `z_g` per group, each an extremum over its group's rows.

    Every `z_g` shares the per-row family's box: a group's extremum cannot leave the range
    any single row can reach, whichever rows the group holds.
    """
    dump = _lowering(decidb_cli).dump_model(
        f"""
            SELECT id, x FROM {_GROUPED_ROWS}
            DECIDE x(INT) SUCH THAT x <= 4
            MAXIMIZE SUM(MAX(x * c)) PER g
        """,
        tmp_path / "per_inner.dump")
    aux = _assert_all_boxed(dump, 2)
    assert [(lb, ub) for _, lb, ub in aux] == [(0.0, 20.0), (0.0, 20.0)], \
        f"per-group z_g auxiliaries are not boxed by the per-row range:\n{dump}"


@pytest.mark.min_max
@pytest.mark.per_clause
@pytest.mark.correctness
def test_per_outer_minmax_over_group_values_is_boxed(decidb_cli, tmp_path):
    """`w` reducing over the `z_g`s inherits their box, since each one lives inside it."""
    dump = _lowering(decidb_cli).dump_model(
        f"""
            SELECT id, x FROM {_GROUPED_ROWS}
            DECIDE x(INT) SUCH THAT x <= 4
            MAXIMIZE MAX(MAX(x * c)) PER g
        """,
        tmp_path / "per_outer_values.dump")
    aux = _assert_all_boxed(dump, 3)  # two z_g plus the outer w
    assert [(lb, ub) for _, lb, ub in aux] == [(0.0, 20.0)] * 3, \
        f"outer w over z_g is not boxed by the per-row range:\n{dump}"


@pytest.mark.min_max
@pytest.mark.per_clause
@pytest.mark.correctness
def test_per_outer_minmax_over_group_sums_is_boxed(decidb_cli, tmp_path):
    """`w` reducing over group SUMS needs its own derivation, not the per-row family's.

    A group sum leaves any single row's range the moment the group holds more than one
    row, so this auxiliary is boxed by the actual per-group sums: group 'a' reaches
    3*4 + 5*4 = 32, group 'b' reaches 2*4 = 8, so w lives in [0, 32]. Widening the per-row
    range (20) by the row count (3) would also be correct but nearly twice as loose at 60,
    and the looseness grows with the relation — which is the whole reason to derive it.
    """
    dump = _lowering(decidb_cli).dump_model(
        f"""
            SELECT id, x FROM {_GROUPED_ROWS}
            DECIDE x(INT) SUCH THAT x <= 4
            MAXIMIZE MIN(SUM(x * c)) PER g
        """,
        tmp_path / "per_outer_sums.dump")
    assert _assert_all_boxed(dump, 1) == [(3, 0.0, 32.0)], \
        f"outer w over group sums is not boxed by the group-sum range:\n{dump}"


@pytest.mark.min_max
@pytest.mark.correctness
def test_composed_minmax_term_auxiliaries_are_boxed(decidb_cli, tmp_path):
    """Each composed term's `z_k` is boxed by that term's own reach, not a shared range.

    `MAX(x)` and `MIN(y)` sit in one constraint over different variables with different
    caps, so their auxiliaries must be boxed independently — [0,4] and [0,6] here.
    """
    dump = _lowering(decidb_cli).dump_model(
        f"""
            SELECT id, x, y FROM {_ROWS}
            DECIDE x(INT), y(INT)
            SUCH THAT x <= 4 AND y <= 6 AND MAX(x) + MIN(y) <= 7
            MAXIMIZE SUM(x + y)
        """,
        tmp_path / "composed.dump")
    aux = _assert_all_boxed(dump, 2)
    assert [(lb, ub) for _, lb, ub in aux] == [(0.0, 4.0), (0.0, 6.0)], \
        f"composed z_k auxiliaries are not boxed by their own terms:\n{dump}"


@pytest.mark.min_max
@pytest.mark.correctness
@pytest.mark.parametrize("name,rows,sense,expected", [
    # `x(REAL)` defaults to a floor of 0 and no ceiling. With positive coefficients the
    # expression inherits that shape exactly, so the floor is derivable and only the
    # ceiling is not.
    ("open above only", "(VALUES (1,3),(2,5)) t(id,c)", "MINIMIZE", (0.0, 1e30)),
    # A negative coefficient swaps which end of `x`'s box feeds which end of the term:
    # the open ceiling on `x` opens the expression's FLOOR, and 0 becomes its ceiling.
    # Blaming the side without respecting sign would get this backwards.
    ("open below only", "(VALUES (1,-3),(2,-5)) t(id,c)", "MAXIMIZE", (-1e30, 0.0)),
    # Both signs in one family: the extremum ranges over a term open above and a term
    # open below, so nothing is derivable and the column is free. This is the case the
    # test was originally written for.
    ("open at both ends", "(VALUES (1,3),(2,-5)) t(id,c)", "MINIMIZE", (-1e30, 1e30)),
], ids=lambda v: v if isinstance(v, str) else "")
def test_unbounded_variable_keeps_whichever_end_it_can(decidb_cli_gurobi, tmp_path, name,
                                                       rows, sense, expected):
    """An underivable end costs that end, and no more.

    "Unbounded" is a property of one end of a range, not of the range. `x(REAL)` with no
    ceiling makes `MAX(x * c)` unbounded above, and that end must stay free rather than
    take a box that could cut off the optimum — but the other end is ordinary arithmetic
    over the user's own bounds, and discarding it buys nothing. A fully free column is
    correct only when nothing at all was derivable, which is the third case here.

    Each case optimizes toward the end that exists — pushing toward the open end is a
    genuinely unbounded query, and testing it would test the refusal rather than the box.
    That is also why this one runs native: reaching the open-below case at all needs
    `MAXIMIZE MAX`, the hard direction, and the lowered arm refuses it for want of a
    finite Big-M before any box is built. The box itself is computed by the same walk on
    both arms, so the arm decides which shapes are expressible, not what is covered.

    The auxiliary is named by its objective coefficient rather than by position: the hard
    direction pins an extra column per row, and only the extremum carries the objective.
    """
    dump = decidb_cli_gurobi.dump_model(
        f"""
            SELECT id, x FROM {rows}
            DECIDE x(REAL) SUCH THAT SUM(x) >= 2
            {sense} MAX(x * c)
        """,
        tmp_path / f"unbounded_{name.replace(' ', '_')}.dump")
    reduced = [(idx, lb, ub) for idx, lb, ub in _continuous_columns(dump)
               if re.search(rf"^col {idx}: .* obj=1$", dump, re.M)]
    assert len(reduced) == 1, f"expected exactly one objective auxiliary:\n{dump}"
    _, lb, ub = reduced[0]
    assert (lb, ub) == expected, \
        f"{name}: auxiliary boxed [{lb}, {ub}], expected {expected}:\n{dump}"


@pytest.mark.min_max
@pytest.mark.correctness
def test_native_auxiliaries_are_boxed_too(decidb_cli, tmp_path):
    """A native formulation builds different columns — but no freer ones.

    The cliff this module is about is a property of a free continuous column, not of
    the Big-M encoding, so it applies just as much to the columns a general constraint
    needs. Asserted without a count: how many columns the native shape uses is its own
    business, and pinning a number here would just re-test the formulation.
    """
    dump = decidb_cli.dump_model(
        f"""
            SELECT id, x FROM {_ROWS}
            DECIDE x(INT) SUCH THAT x <= 4 AND SUM(x) <= 6
            MAXIMIZE MAX(x * c)
        """,
        tmp_path / "native.dump")
    cols = _continuous_columns(dump)
    assert cols, f"no auxiliary columns at all:\n{dump}"
    for idx, lb, ub in cols:
        assert lb > -1e20 and ub < 1e20, \
            f"auxiliary col {idx} left free at [{lb}, {ub}]:\n{dump}"


@pytest.mark.min_max
@pytest.mark.correctness
def test_half_open_range_keeps_its_closed_side(decidb_cli_gurobi, tmp_path):
    """An end that could not be derived does not cost the end that could.

    "Unbounded" is a property of one end, not of a range. `x(INT) SUCH THAT x >= 0`
    with no ceiling reaches arbitrarily high, so a `MAX(x)` auxiliary genuinely has no
    upper bound — but its lower bound is 0, and it was computed on the way to finding
    that out. Discarding it along with the ceiling hands the root simplex a fully free
    column when half a box was available, which is the cliff this whole module exists
    to keep out.

    Only the native arm can be asked this. The lowered arm needs a Big-M over the same
    family, has no finite one here, and refuses the query outright — that refusal is
    `test_native_constructs.test_native_minmax_needs_no_bound`, and it is why an
    unbounded MIN/MAX reaches a model at all.
    """
    dump = decidb_cli_gurobi.dump_model(
        f"""
            SELECT id, x FROM {_ROWS}
            DECIDE x(INT) SUCH THAT MAX(x) >= 3 AND x >= 0
            MINIMIZE SUM(x)
        """,
        tmp_path / "half_open.dump")
    aux = _continuous_columns(dump)
    assert aux, f"no auxiliary columns at all:\n{dump}"
    for idx, lb, ub in aux:
        assert lb == 0.0, \
            f"auxiliary col {idx} threw away its derived floor: [{lb}, {ub}]\n{dump}"
        assert ub >= 1e20, \
            f"auxiliary col {idx} was given a ceiling nothing derived: [{lb}, {ub}]\n{dump}"
