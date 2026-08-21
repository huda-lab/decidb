"""A construct the backend expresses itself is not lowered.

DeciDB lowers ABS, MIN/MAX, ``<>`` and ``IN`` into indicator variables and Big-M
rows because a plain LP/MILP matrix is all a solver was assumed to take. Gurobi has
*general constraints* — ``aux = |t|`` stated directly — and a solver that takes one
natively needs no Big-M, so the lowering is work that only loses accuracy.

The choice is made by a **gate** at stage 08, reading the capability table of the
backend picked at plan time. There are exactly two arms and they are equivalent:
whichever runs, the optimum is the same. That equivalence is the standard a
capability flag has to meet before it goes in the table, and
``DECIDB_NATIVE_CONSTRUCTS=off`` is how it is checked — it forces every construct
back down its lowering path on the same machine.

Native is not only faster. A Big-M needs a finite bound on every contributing
variable, and where none exists DeciDB refuses (see ``test_unbounded_bigm``). A
general constraint needs no bound at all, so the capability turns some refused
queries into answered ones. That is the one place the two arms visibly differ, and
it is the payoff.

Covers:
  - test_native_and_lowered_agree: the A/B, over the ABS-maximize shapes
  - test_minmax_native_and_lowered_agree: the A/B, over the MIN/MAX shapes
  - test_native_replaces_rows_with_a_general_constraint: the model really changed
  - test_native_abs_needs_no_bound: the payoff — refused when lowered, answered native
  - test_native_minmax_needs_no_bound: the same payoff on MIN/MAX
  - test_highs_never_takes_the_native_path: capability is per backend, not global
"""

import os
import re
import tempfile

import pytest

from decidb_cli import DecidBCliError

_FIXTURE = "(VALUES (1, 3.0), (2, 7.0), (3, 5.0)) v(id, target)"

# The ABS-maximize subset, deliberately narrow. Constraint-side ABS in the easy
# direction lowers exactly (`aux >= e`, `aux >= -e`), never needs a Big-M, and never
# routes through the gate — A/B-ing it would exercise no new code. These are the
# shapes that DO: a MAXIMIZE objective over ABS, and the hard-direction constraint
# forms, which are the ones that get the sign indicator.
_ABS_MAXIMIZE_SHAPES = [
    ("maximize objective",
     "SUCH THAT x >= 0 AND x <= 10 MAXIMIZE SUM(ABS(x - target))"),
    ("hard constraint >=",
     "SUCH THAT x >= 0 AND x <= 10 AND ABS(x - target) >= 2 MINIMIZE SUM(x)"),
    ("hard constraint =",
     "SUCH THAT x >= 0 AND x <= 10 AND ABS(x - target) = 2 MINIMIZE SUM(x)"),
    ("hard constraint BETWEEN",
     "SUCH THAT x >= 0 AND x <= 10 AND ABS(x - target) BETWEEN 1 AND 4 MAXIMIZE SUM(x)"),
    ("aggregate over ABS",
     "SUCH THAT x >= 0 AND x <= 10 AND MAX(ABS(x - target)) >= 3 MINIMIZE SUM(x)"),
]


def _solve(cli, clause, extra_env=None):
    sql = f"SELECT id, x FROM {_FIXTURE} DECIDE x(REAL) {clause}"
    if extra_env:
        cli = cli.with_env(extra_env)
    rows, cols = cli.execute(sql)
    ci = {c: i for i, c in enumerate(cols)}
    # Compare by objective-relevant aggregate rather than row order, which the two
    # formulations have no reason to agree on.
    return sorted(round(float(r[ci["x"]]), 6) for r in rows)


@pytest.mark.correctness
@pytest.mark.obj_maximize
@pytest.mark.parametrize("name,clause", _ABS_MAXIMIZE_SHAPES, ids=[s[0] for s in _ABS_MAXIMIZE_SHAPES])
def test_native_and_lowered_agree(decidb_cli_gurobi, name, clause):
    """The gate's two arms must reach the same optimum. This is the A/B."""
    native = _solve(decidb_cli_gurobi, clause)
    lowered = _solve(decidb_cli_gurobi, clause, {"DECIDB_NATIVE_CONSTRUCTS": "off"})
    assert native == lowered, f"{name}: native {native} != lowered {lowered}"


@pytest.mark.obj_maximize
def test_native_replaces_rows_with_a_general_constraint(decidb_cli_gurobi):
    """Equal answers are not evidence of equal models, so read the model.

    Native trades the two Big-M rows and the binary sign indicator for one general
    constraint plus a free argument column and its equality row. So the native model
    has *more* columns and *fewer* rows — and the general constraints show up in the
    dump, which they must, or a native construct would read as rows disappearing.
    """
    sql = (f"SELECT id, x FROM {_FIXTURE} DECIDE x(REAL) "
           "SUCH THAT x >= 0 AND x <= 10 MAXIMIZE SUM(ABS(x - target))")

    def dump_of(env):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "model.dump")
            decidb_cli_gurobi.with_env({**env, "DECIDB_DUMP_MODEL": path}).execute(sql)
            with open(path) as f:
                return f.read()

    native = dump_of({})
    lowered = dump_of({"DECIDB_NATIVE_CONSTRUCTS": "off"})

    def field(dump, name):
        match = re.search(rf"^{name}: (\d+)$", dump, re.MULTILINE)
        assert match, f"no {name} in dump:\n{dump}"
        return int(match.group(1))

    assert field(lowered, "num_genconstrs") == 0, lowered
    assert field(native, "num_genconstrs") == 3, native
    assert field(native, "num_rows") < field(lowered, "num_rows")
    assert field(native, "num_vars") > field(lowered, "num_vars")
    # Every general constraint must name a real result column and one argument.
    for result, args in re.findall(r"^gen \d+: kind=abs res=(\d+) args=(\S+)$", native, re.MULTILINE):
        assert int(result) < field(native, "num_vars")
        assert len(args.split(",")) == 1


@pytest.mark.correctness
def test_native_abs_needs_no_bound(decidb_cli_gurobi):
    """The payoff: a query the Big-M path cannot formulate at all.

    ``x`` has no upper bound, so no finite M dominates ``|x - target|`` and the
    lowering path refuses. ``aux = |t|`` needs no M, so the native path answers —
    x = 0 satisfies every ``|x - target| >= 2`` and minimizes the sum.
    """
    clause = "SUCH THAT ABS(x - target) >= 2 MINIMIZE SUM(x)"
    assert _solve(decidb_cli_gurobi, clause) == [0.0, 0.0, 0.0]

    with pytest.raises(DecidBCliError) as excinfo:
        _solve(decidb_cli_gurobi, clause, {"DECIDB_NATIVE_CONSTRUCTS": "off"})
    assert "finite bound on 'x'" in excinfo.value.message, excinfo.value.message


@pytest.mark.correctness
def test_highs_never_takes_the_native_path(decidb_cli_highs):
    """A capability belongs to one backend, and the gate reads the one in play.

    HiGHS declares no construct native, so its model must carry no general
    constraint whatever Gurobi on the same host can do.
    """
    sql = (f"SELECT id, x FROM {_FIXTURE} DECIDE x(REAL) "
           "SUCH THAT x >= 0 AND x <= 10 MAXIMIZE SUM(ABS(x - target))")
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "model.dump")
        decidb_cli_highs.with_env({"DECIDB_DUMP_MODEL": path}).execute(sql)
        with open(path) as f:
            dump = f.read()
    assert "num_genconstrs: 0" in dump, dump


# The MIN/MAX shapes that route through the gate. Easy directions (`MAX(e) <= K`,
# `MIN(e) >= K`, `MINIMIZE MAX`, `MAXIMIZE MIN`) never needed a Big-M — the one-sided
# envelope plus outer pressure is exact — so they never reach it and A/B-ing them
# would exercise no new code. These are the hard ones: constraint, objective,
# PER-nested, and composed.
#
# Each entry carries the objective as a function of the returned rows, because that is
# what "the same optimum" means. Comparing the ASSIGNMENT would be wrong: an optimum can
# be attained by more than one assignment, and two formulations have no obligation to
# break a tie the same way — as `composed objective` here actually does.
_GROUPED = "(VALUES (1,'a',3),(2,'a',5),(3,'b',2),(4,'b',7)) t(id, g, c)"


def _sum_x(rows):
    return sum(x for _g, _c, x in rows)


def _sum_xc(rows):
    return sum(c * x for _g, c, x in rows)


def _max_xc(rows):
    return max(c * x for _g, c, x in rows)


def _min_xc(rows):
    return min(c * x for _g, c, x in rows)


def _max_group_sum_xc(rows):
    per_group = {}
    for g, c, x in rows:
        per_group[g] = per_group.get(g, 0.0) + c * x
    return max(per_group.values())


_MINMAX_SHAPES = [
    ("hard constraint MAX >=",
     "SUCH THAT x <= 9 AND MAX(x * c) >= 20 MINIMIZE SUM(x)", _sum_x),
    ("hard constraint MIN <=",
     "SUCH THAT x <= 9 AND MIN(x * c) <= 3 MAXIMIZE SUM(x)", _sum_x),
    ("hard constraint equality",
     "SUCH THAT x <= 9 AND MAX(x) = 5 MINIMIZE SUM(x * c)", _sum_xc),
    ("hard constraint PER",
     "SUCH THAT x <= 9 AND MAX(x) >= 5 PER g MINIMIZE SUM(x * c)", _sum_xc),
    ("hard objective MAXIMIZE MAX",
     "SUCH THAT x <= 4 AND SUM(x) <= 6 MAXIMIZE MAX(x * c)", _max_xc),
    ("hard objective MINIMIZE MIN",
     "SUCH THAT x >= 1 AND x <= 4 AND SUM(x) >= 3 MINIMIZE MIN(x * c)", _min_xc),
    ("hard PER-nested objective",
     "SUCH THAT x <= 4 AND SUM(x) <= 6 MAXIMIZE MAX(MAX(x * c)) PER g", _max_xc),
    ("hard PER outer over group sums",
     "SUCH THAT x <= 4 AND SUM(x) <= 6 MAXIMIZE MAX(SUM(x * c)) PER g", _max_group_sum_xc),
    ("composed constraint",
     "SUCH THAT x <= 9 AND SUM(x) + MAX(x * c) >= 20 MINIMIZE SUM(x)", _sum_x),
    ("composed objective",
     "SUCH THAT x <= 4 AND SUM(x) <= 6 MAXIMIZE SUM(x) + MAX(x * c)",
     lambda rows: _sum_x(rows) + _max_xc(rows)),
]


def _solve_grouped(cli, clause, extra_env=None):
    """`(g, c, x)` per row, which is everything the objective functions above need."""
    sql = f"SELECT id, g, c, x FROM {_GROUPED} DECIDE x(INT) {clause}"
    if extra_env:
        cli = cli.with_env(extra_env)
    rows, cols = cli.execute(sql)
    ci = {c: i for i, c in enumerate(cols)}
    return [(r[ci["g"]], float(r[ci["c"]]), float(r[ci["x"]])) for r in rows]


@pytest.mark.correctness
@pytest.mark.min_max
@pytest.mark.parametrize("name,clause,objective", _MINMAX_SHAPES,
                         ids=[s[0] for s in _MINMAX_SHAPES])
def test_minmax_native_and_lowered_agree(decidb_cli_gurobi, name, clause, objective):
    """`z = MAX(t..)` and the indicator family must reach the same optimum.

    MIN/MAX is the messiest construct — a constraint form, an objective form, a
    PER-nested form and a composed form, each with its own hard branch — so the A/B
    runs over all of them rather than a representative.
    """
    native = objective(_solve_grouped(decidb_cli_gurobi, clause))
    lowered = objective(_solve_grouped(decidb_cli_gurobi, clause,
                                       {"DECIDB_NATIVE_CONSTRUCTS": "off"}))
    assert abs(native - lowered) <= 1e-6, f"{name}: native {native} != lowered {lowered}"


@pytest.mark.correctness
@pytest.mark.min_max
def test_native_minmax_needs_no_bound(decidb_cli_gurobi):
    """The MIN/MAX payoff, matching the ABS one: no Big-M means no bound needed."""
    clause = "SUCH THAT MAX(x) >= 5 MINIMIZE SUM(x * c)"
    rows = _solve_grouped(decidb_cli_gurobi, clause)
    # The cheapest way to reach MAX(x) = 5 is to put it on the smallest coefficient.
    assert _sum_xc(rows) == 10.0, rows

    with pytest.raises(DecidBCliError) as excinfo:
        _solve_grouped(decidb_cli_gurobi, clause, {"DECIDB_NATIVE_CONSTRUCTS": "off"})
    assert "finite bound on 'x'" in excinfo.value.message, excinfo.value.message
