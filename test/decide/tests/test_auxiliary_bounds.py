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

Covers one query per auxiliary-creation site:
  - test_flat_minmax_objective_auxiliary_is_boxed: flat `z`, both directions
  - test_per_inner_minmax_auxiliaries_are_boxed: per-group `z_g`
  - test_per_outer_minmax_over_group_values_is_boxed: `w` over the `z_g`s
  - test_per_outer_minmax_over_group_sums_is_boxed: `w` over group sums
  - test_composed_minmax_term_auxiliaries_are_boxed: composed `z_k` per term
  - test_unbounded_variable_leaves_its_auxiliary_free: the one legitimate exception
"""

import re

import pytest


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
    dump = decidb_cli.dump_model(
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
    dump = decidb_cli.dump_model(
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
    dump = decidb_cli.dump_model(
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
    dump = decidb_cli.dump_model(
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
    dump = decidb_cli.dump_model(
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
def test_unbounded_variable_leaves_its_auxiliary_free(decidb_cli, tmp_path):
    """The one case where a free auxiliary is correct: there is no box to derive.

    `x(REAL)` with no upper bound reaches infinity, so `MAX(x * c)` does too and the
    auxiliary must stay free rather than be given a box that cuts off the optimum. The
    guard for this is the same one the Big-M walk already used to fall back to its floor.
    """
    dump = decidb_cli.dump_model(
        f"""
            SELECT id, x FROM {_ROWS}
            DECIDE x(REAL) SUCH THAT SUM(x) >= 2
            MINIMIZE MAX(x * c)
        """,
        tmp_path / "unbounded.dump")
    # Decision columns are continuous here too, so name the auxiliary by position: it is
    # appended after the per-row grid.
    cols = _continuous_columns(dump)
    assert len(cols) == 3, f"expected two x columns plus one auxiliary:\n{dump}"
    _, lb, ub = cols[-1]
    assert lb <= -1e20 and ub >= 1e20, \
        f"auxiliary over an unbounded variable was given a box [{lb}, {ub}]:\n{dump}"
