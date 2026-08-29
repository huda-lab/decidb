"""A construct the backend expresses itself is not lowered.

DeciDB lowers ABS, MIN/MAX, ``<>`` and ``IN`` into indicator variables and Big-M
rows because a plain LP/MILP matrix is all a solver was assumed to take. Gurobi has
*general constraints* — ``aux = |t|`` stated directly — and a solver that takes one
natively needs no Big-M, so the lowering is work that only loses accuracy.

The choice is made at **stage 05**, from the capability table of the backend picked at
plan time, and recorded on the plan; stage 08 reads it and routes. There are exactly
two arms and they are equivalent:
whichever runs, the optimum is the same. That equivalence is the standard a
capability flag has to meet before it goes in the table, and
``DECIDB_NATIVE_CONSTRUCTS`` is how it is checked on one machine: ``off`` forces every
construct down its lowering path, ``force`` states every declared one natively.

Native is not faster — measured, it is slower wherever the lowering is available, since
a general constraint relates columns (so every member expression needs a pinned one)
and states an equality (so the backend expands both directions). What it is, is
*possible* where the lowering is not: a Big-M needs a finite bound on every contributing
variable, and where none exists DeciDB refuses (see ``test_unbounded_bigm``). A general
constraint needs no bound at all.

So for MIN/MAX native is the FALLBACK, taken per clause only where no Big-M exists.
That is why its A/B runs ``force`` against ``off`` rather than the default against
``off`` — the default would take the lowering on these bounded shapes and the test
would compare the lowering with itself.

Covers:
  - test_native_and_lowered_agree: the A/B, over the ABS-maximize shapes
  - test_minmax_native_and_lowered_agree: the A/B, over the MIN/MAX shapes
  - test_native_replaces_rows_with_a_general_constraint: the model really changed
  - test_minmax_prefers_the_lowering_when_a_big_m_exists: the gate, read off the model
  - test_native_abs_needs_no_bound: the payoff — refused when lowered, answered native
  - test_native_minmax_needs_no_bound: the same payoff on MIN/MAX
  - test_not_equal_native_and_lowered_agree: the A/B, over the `<>` shapes
  - test_native_not_equal_needs_no_bound: the same payoff on `<>`
  - test_native_not_equal_is_still_diagnosable: the dial reaches an implied row
  - test_highs_never_takes_the_native_path: capability is per backend, not global

`<>` is stated with **indicator constraints** rather than a general constraint, and
that is a diagnosis decision, not a Gurobi-vocabulary one. A `<>` clause has no row of
its own: the two Big-M disjunction rows *are* the clause, and dropping them is the only
repair infeasible diagnosis can offer for it. A general constraint carries no row, so
going that way would have made every `<>` undiagnosable. An indicator constraint carries
one — so the removal dial wires its binary into the implied row exactly as it does into
a matrix row, and the diagnosis is unchanged.
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
    constraint plus an argument column and its equality row. So the native model has
    *fewer rows*, and the columns trade one-for-one: the binary sign indicator leaves
    and the continuous argument column takes its place. The general constraints show
    up in the dump, which they must, or a native construct would read as rows
    disappearing.

    The column check is by KIND, not by count, and that is the point of it. A count
    cannot tell "the binary was replaced" from "the binary was left behind and a
    column was added next to it" — which is what used to happen, one dead binary per
    data row on the native arm, presolved away by the solver and therefore invisible
    to every test that reads answers.
    """
    sql = (f"SELECT id, x FROM {_FIXTURE} DECIDE x(REAL) "
           "SUCH THAT x >= 0 AND x <= 10 MAXIMIZE SUM(ABS(x - target))")

    def dump_of(env):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "model.dump")
            return decidb_cli_gurobi.with_env(env).dump_model(sql, path)

    native = dump_of({})
    lowered = dump_of({"DECIDB_NATIVE_CONSTRUCTS": "off"})

    def field(dump, name):
        match = re.search(rf"^{name}: (\d+)$", dump, re.MULTILINE)
        assert match, f"no {name} in dump:\n{dump}"
        return int(match.group(1))

    def binary_columns(dump):
        return len(re.findall(r"^col \d+: .* bin=1 ", dump, re.MULTILINE))

    assert field(lowered, "num_genconstrs") == 0, lowered
    assert field(native, "num_genconstrs") == 3, native
    assert field(native, "num_rows") < field(lowered, "num_rows")
    # One binary sign indicator per data row when lowered; none at all when native,
    # because there is no Big-M for one to switch.
    assert binary_columns(lowered) == 3, lowered
    assert binary_columns(native) == 0, native
    # And nothing left over: the argument column takes the indicator's place.
    assert field(native, "num_vars") == field(lowered, "num_vars")
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
    native = objective(_solve_grouped(decidb_cli_gurobi, clause,
                                      {"DECIDB_NATIVE_CONSTRUCTS": "force"}))
    lowered = objective(_solve_grouped(decidb_cli_gurobi, clause,
                                       {"DECIDB_NATIVE_CONSTRUCTS": "off"}))
    assert abs(native - lowered) <= 1e-6, f"{name}: native {native} != lowered {lowered}"


@pytest.mark.min_max
def test_minmax_prefers_the_lowering_when_a_big_m_exists(decidb_cli_gurobi):
    """Which arm a bounded MIN/MAX clause takes, read off the model.

    Both arms encode the same thing, so where both are available this is a performance
    question — and the lowering wins it. A general constraint relates COLUMNS, so each
    member expression has to be pinned to a fresh one that presolve cannot substitute
    away, and `z = MAX(t..)` is an equality, so the backend expands both directions
    while the lowering emits only the one the clause needs. Measured on this shape at
    30K rows: 3.4s native against 0.09s lowered, and native's cost grew with row count
    while the lowering stayed flat.

    So native is the FALLBACK — `test_native_minmax_needs_no_bound` covers the case it
    exists for, where no Big-M exists at all. Answers are identical either way, which
    is exactly why this has to be checked against the model rather than the result.
    """
    sql = (f"SELECT id, g, c, x FROM {_GROUPED} DECIDE x(INT) "
           "SUCH THAT x <= 9 AND MAX(x * c) >= 20 MINIMIZE SUM(x)")

    def dump_of(env):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "model.dump")
            return decidb_cli_gurobi.with_env(env).dump_model(sql, path)

    # `on` is the shipping policy spelled out. Pinned rather than left unset so the
    # assertion still means the default when the whole suite runs under an ambient
    # setting of the switch.
    default = dump_of({"DECIDB_NATIVE_CONSTRUCTS": "on"})
    forced = dump_of({"DECIDB_NATIVE_CONSTRUCTS": "force"})
    lowered = dump_of({"DECIDB_NATIVE_CONSTRUCTS": "off"})

    def binary_columns(dump):
        return len(re.findall(r"^col \d+: .* bin=1 ", dump, re.MULTILINE))

    # `x` is bounded here, so a Big-M exists and the default takes the lowering.
    assert "num_genconstrs: 0" in default, default
    assert default == lowered, f"default arm is not the lowering:\n{default}\n---\n{lowered}"
    # ...and the fallback is still reachable, on demand and on its own merits.
    assert re.search(r"^gen \d+: kind=max ", forced, re.MULTILINE), forced

    # One indicator per data row on the arm that switches the disjunction, and NONE on
    # the arm that does not. Stage 05 used to allocate a row-scoped binary per data row
    # on both — it could not know which arm a clause would take, because that depends on
    # evaluated data — so the native arm carried a column no row referenced. It no longer
    # allocates one at all: the clause becomes an extremum column plus the user's bound
    # over it, and the binaries a Big-M pinning needs are global-block columns created by
    # the pass that emits the rows reading them.
    assert binary_columns(lowered) == 4, lowered
    assert binary_columns(forced) == 0, forced


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


# `<>` shapes that reach the gate. A clause whose range collapses to a plain inequality
# never had a disjunction to state, so it takes neither arm — `test_ne_range_collapse`
# covers that boundary.
_NOT_EQUAL_SHAPES = [
    ("per-row", "SUCH THAT x <= 9 AND x <> 5 MAXIMIZE SUM(x * c)", _sum_xc),
    ("per-row under WHEN",
     "SUCH THAT x <= 9 AND x <> 5 WHEN g = 'a' MAXIMIZE SUM(x * c)", _sum_xc),
    ("aggregate", "SUCH THAT x <= 9 AND SUM(x) <> 36 MAXIMIZE SUM(x * c)", _sum_xc),
    ("aggregate PER",
     "SUCH THAT x <= 9 AND SUM(x) <> 18 PER g MAXIMIZE SUM(x * c)", _sum_xc),
    ("both spellings at once",
     "SUCH THAT x <= 9 AND x <> 5 AND SUM(x) <> 36 MAXIMIZE SUM(x * c)", _sum_xc),
]


@pytest.mark.correctness
@pytest.mark.cons_comparison
@pytest.mark.parametrize("name,clause,objective", _NOT_EQUAL_SHAPES,
                         ids=[s[0] for s in _NOT_EQUAL_SHAPES])
def test_not_equal_native_and_lowered_agree(decidb_cli_gurobi, name, clause, objective):
    """`z==0 => LHS <= K-1` / `z==1 => LHS >= K+1` must mean the Big-M pair."""
    native = objective(_solve_grouped(decidb_cli_gurobi, clause))
    lowered = objective(_solve_grouped(decidb_cli_gurobi, clause,
                                       {"DECIDB_NATIVE_CONSTRUCTS": "off"}))
    assert abs(native - lowered) <= 1e-6, f"{name}: native {native} != lowered {lowered}"


@pytest.mark.correctness
@pytest.mark.cons_comparison
def test_native_not_equal_needs_no_bound(decidb_cli_gurobi):
    """The `<>` payoff. An implication needs no constant, so it needs no bound.

    `x` has no upper bound, so the Big-M pair has no M and the lowering path refuses.
    """
    clause = "SUCH THAT x <> 5 MINIMIZE SUM(x * c)"
    assert _sum_xc(_solve_grouped(decidb_cli_gurobi, clause)) == 0.0

    with pytest.raises(DecidBCliError) as excinfo:
        _solve_grouped(decidb_cli_gurobi, clause, {"DECIDB_NATIVE_CONSTRUCTS": "off"})
    assert "finite bound on 'x'" in excinfo.value.message, excinfo.value.message


@pytest.mark.correctness
@pytest.mark.cons_comparison
@pytest.mark.query_diagnostics
def test_native_not_equal_is_still_diagnosable(decidb_cli_gurobi):
    """The reason `<>` uses indicator constraints and not a general constraint.

    A `<>` clause has no row of its own — the disjunction *is* the clause — so the only
    repair diagnosis can offer is dropping it, and dropping needs a row to neutralize.
    An indicator constraint has one. This query is infeasible with three remove-only
    `<>` clauses, one of them a live disjunction (`<> 1` sits strictly inside the
    reachable range), so the removal dial must be operating on an implied row; the
    diagnosis has to match the Big-M encoding's exactly.
    """
    script = (
        ".mode csv\n"
        "DIAGNOSE SELECT id, x FROM (VALUES (1), (2)) t(id) DECIDE x(BOOL) "
        "SUCH THAT SUM(x) <> 0 AND SUM(x) <> 2 AND SUM(x) <> 1 MINIMIZE SUM(x);\n"
    )

    def diagnosis(cli):
        proc = cli.execute_script(script)
        return proc.stdout + proc.stderr

    native = diagnosis(decidb_cli_gurobi)
    lowered = diagnosis(decidb_cli_gurobi.with_env({"DECIDB_NATIVE_CONSTRUCTS": "off"}))
    assert "remove_only" in native, f"a native `<>` must stay droppable:\n{native}"
    assert native == lowered, f"native diagnosis differs:\n{native}\n---\n{lowered}"
