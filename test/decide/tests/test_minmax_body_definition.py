"""A MIN/MAX over a squared or multiplied body: the body is defined by a helper decision.

Every extremum DeciDB builds reads its members as linear. Stage 05 therefore names a
non-linear body first (``DefineNonlinearReducerBodies``): one helper decision ``t`` per
instance of the body's scope (per row once it reads a data column) and one quadratic row per
instance defining it by the body ``q``, on the side the clause pushes the
reducer (pushed down ``t >= q``, up ``t <= q``, neither ``t = q``). Every reader of one
reducer (copies in a clause, other clauses, the objective) shares one helper, defined on
every side a reader pushes it. The helper has no box,
so only a backend that states MIN/MAX natively (Gurobi) takes it; any other is refused.

Each correctness case is compared with an independent Gurobi model that states the
extremum exactly (``_oracle_extremum``), so a case cannot pass on the side the clause
does not push. The direction cases read the member rows off the model dump, because
``t = q`` answers every query correctly and only the dump tells the sides apart.

Covers:
  - test_defined_constraint_matches_oracle: the constraint shapes, one per branch
  - test_defined_objective_matches_oracle: the flat, composed and two-level objectives
  - test_member_row_follows_the_push: the side of each member row
  - test_readers_of_one_reducer_share_one_helper: two clauses, a clause and the objective
  - test_helper_is_defined_on_the_rows_the_reducer_reads: the clause `when` and the reducer filter
  - test_explain_states_the_side_and_hides_the_row: the account line, one per side
  - test_cheap_lone_extremum_is_not_defined: `max(q) <= K` keeps one row per member
  - test_defined_body_refused_without_native_minmax: the refusal, one per message branch
  - test_body_helper_takes_the_scope_the_body_reads: one helper per instance of the body's scope
"""

import os
import re
import tempfile
from collections import defaultdict

import pytest

from decidb_cli import DecidBCliError
from solver.types import ObjSense, VarType
from comparison.compare import compare_solutions
from ._brute_force import judge_rows

_TWO_ROWS = "(VALUES (1), (2)) v(id)"
_TWO_ROWS_DATA = [(1,), (2,)]


def _oracle_extremum(oracle, name, members, is_max, lo, hi):
    """``z`` = MAX (or MIN) of ``members`` exactly, and its name.

    A member is ``(linear, quadratic, constant)``. Gurobi's indicator constraint takes a
    linear body only, so each member is first held by a column ``q_i`` through one
    quadratic equality. ``z`` bounds every member on the envelope side and equals the
    member a binary selects, so the oracle is exact in both directions whatever the
    clause pushes.
    """
    z = f"{name}_z"
    oracle.add_variable(z, VarType.CONTINUOUS, lb=lo, ub=hi)
    selectors = []
    for i, (linear, quadratic, constant) in enumerate(members):
        q = f"{name}_q{i}"
        oracle.add_variable(q, VarType.CONTINUOUS, lb=lo, ub=hi)
        definition = dict(linear)
        definition[q] = definition.get(q, 0.0) - 1.0
        oracle.add_quadratic_constraint(definition, quadratic, "=", -constant, name=f"{name}_def{i}")
        oracle.add_constraint({z: 1.0, q: -1.0}, ">=" if is_max else "<=", 0.0, name=f"{name}_env{i}")
        y = f"{name}_y{i}"
        oracle.add_variable(y, VarType.BINARY)
        oracle.add_indicator_constraint(y, 1, {z: 1.0, q: -1.0}, "=", 0.0, name=f"{name}_sel{i}")
        selectors.append(y)
    oracle.add_constraint({y: 1.0 for y in selectors}, ">=", 1.0, name=f"{name}_one")
    return z


def _square(var, target=0.0):
    """``power(var - target, 2)`` as an oracle member."""
    return ({var: -2.0 * target}, {(var, var): 1.0}, target * target)


def _x_values(rows, cols):
    xi = cols.index("x")
    return [float(r[xi]) for r in rows]


# ---------------------------------------------------------------------------
# Constraints
# ---------------------------------------------------------------------------
#
# Two INT rows, `0 <= x <= 7`. Each case states its clause for DeciDB, the same clause
# over the oracle's columns, and the clause evaluated on DeciDB's answer.


def _oracle_max_sq(o, xs):
    return _oracle_extremum(o, "m", [_square(x) for x in xs], True, 0.0, 49.0)


def _oracle_min_sq(o, xs):
    return _oracle_extremum(o, "m", [_square(x) for x in xs], False, 0.0, 49.0)


_CONSTRAINT_CASES = [
    # The relation pushes MAX up: `t <= q`. Before the definition: reported infeasible.
    (
        "push up",
        "max(power(x, 2)) >= 49",
        ObjSense.MINIMIZE,
        lambda o, xs: o.add_constraint({_oracle_max_sq(o, xs): 1.0}, ">=", 49.0),
        lambda v: max(x * x for x in v) >= 49,
    ),
    # A composed term pushed down: `t >= q`. Before the definition: an internal error.
    (
        "push down",
        "sum(x) + max(power(x, 2)) <= 20",
        ObjSense.MAXIMIZE,
        lambda o, xs: o.add_constraint({**{x: 1.0 for x in xs}, _oracle_max_sq(o, xs): 1.0}, "<=", 20.0),
        lambda v: sum(v) + max(x * x for x in v) <= 20,
    ),
    # `=` pushes neither way: `t = q`.
    (
        "no push, equality",
        "sum(x) + max(power(x, 2)) = 30",
        ObjSense.MAXIMIZE,
        lambda o, xs: o.add_constraint({**{x: 1.0 for x in xs}, _oracle_max_sq(o, xs): 1.0}, "=", 30.0),
        lambda v: sum(v) + max(x * x for x in v) == 30,
    ),
    # A factor whose sign the rows decide pushes neither way: `t = q`.
    (
        "no push, factor of unknown sign",
        "(SELECT -1) * max(power(x, 2)) <= -25",
        ObjSense.MINIMIZE,
        lambda o, xs: o.add_constraint({_oracle_max_sq(o, xs): -1.0}, "<=", -25.0),
        lambda v: -max(x * x for x in v) <= -25,
    ),
    # An INT body gets an INT helper, so the strict bound steps to 49.
    (
        "whole-numbered helper",
        "max(power(x, 2)) > 48",
        ObjSense.MINIMIZE,
        lambda o, xs: o.add_constraint({_oracle_max_sq(o, xs): 1.0}, ">=", 49.0),
        lambda v: max(x * x for x in v) > 48,
    ),
    # The hard MIN. Before the definition: (7, 7), which breaks the clause.
    (
        "hard MIN",
        "min(power(x, 2)) <= 4",
        ObjSense.MAXIMIZE,
        lambda o, xs: o.add_constraint({_oracle_min_sq(o, xs): 1.0}, "<=", 4.0),
        lambda v: min(x * x for x in v) <= 4,
    ),
    # A SUM term beside a MIN or MAX is defined too. Before the definition: an internal error.
    (
        "composed SUM term",
        "sum(power(x, 2)) + max(x) <= 30",
        ObjSense.MAXIMIZE,
        lambda o, xs: o.add_quadratic_constraint(
            {_oracle_extremum(o, "m", [({x: 1.0}, {}, 0.0) for x in xs], True, 0.0, 7.0): 1.0},
            {(x, x): 1.0 for x in xs},
            "<=",
            30.0,
        ),
        lambda v: sum(x * x for x in v) + max(v) <= 30,
    ),
    # A body that goes negative: the helper has no floor of 0.
    (
        "negative body",
        "min(power(x, 2) - 10 * x) <= -24",
        ObjSense.MAXIMIZE,
        lambda o, xs: o.add_constraint(
            {_oracle_extremum(o, "m", [({x: -10.0}, {(x, x): 1.0}, 0.0) for x in xs], False, -25.0, 0.0): 1.0},
            "<=",
            -24.0,
        ),
        lambda v: min(x * x - 10 * x for x in v) <= -24,
    ),
    # RewriteAbs copies the MAX into two rows, pushed opposite ways; they share one helper `t = q`. With a
    # one-sided helper per copy: (2, 2), where the MAX is 4.
    (
        "ABS over a MAX",
        "x <= 2 AND abs(max(power(x, 2)) - 4) >= 1",
        ObjSense.MAXIMIZE,
        lambda o, xs: _oracle_abs_at_least(o, xs, _oracle_max_sq(o, xs), 4.0, 1.0),
        lambda v: max(v) <= 2 and abs(max(x * x for x in v) - 4) >= 1,
    ),
    # Two clauses read one helper, pushed both ways.
    (
        "two clauses",
        "max(power(x, 2)) >= 49 AND sum(x) + max(power(x, 2)) <= 60",
        ObjSense.MAXIMIZE,
        lambda o, xs: _two_readers(o, xs, _oracle_max_sq(o, xs)),
        lambda v: max(x * x for x in v) >= 49 and sum(v) + max(x * x for x in v) <= 60,
    ),
    # The clause's `when` keeps row 2 only.
    (
        "clause when",
        "when id = 2: sum(x) + max(power(x, 2)) <= 20",
        ObjSense.MAXIMIZE,
        lambda o, xs: o.add_constraint({xs[1]: 1.0, _oracle_max_sq(o, xs[1:]): 1.0}, "<=", 20.0),
        lambda v: v[1] + v[1] * v[1] <= 20,
    ),
    # The reducer's own filter keeps row 2 for the MAX only.
    (
        "reducer filter",
        "sum(x) + max(when id = 2: power(x, 2)) <= 20",
        ObjSense.MAXIMIZE,
        lambda o, xs: o.add_constraint({**{x: 1.0 for x in xs}, _oracle_max_sq(o, xs[1:]): 1.0}, "<=", 20.0),
        lambda v: sum(v) + v[1] * v[1] <= 20,
    ),
]


def _two_readers(o, xs, z):
    o.add_constraint({z: 1.0}, ">=", 49.0)
    o.add_constraint({**{x: 1.0 for x in xs}, z: 1.0}, "<=", 60.0)


def _oracle_abs_at_least(o, xs, z, center, gap):
    """`abs(z - center) >= gap` over INT `x <= 2`, as one side or the other chosen by a binary."""
    for x in xs:
        o.add_constraint({x: 1.0}, "<=", 2.0)
    b = "abs_side"
    o.add_variable(b, VarType.BINARY)
    big = 100.0
    o.add_constraint({z: 1.0, b: big}, ">=", center + gap)
    o.add_constraint({z: 1.0, b: big}, "<=", center - gap + big)


@pytest.mark.min_max
@pytest.mark.quadratic
@pytest.mark.correctness
@pytest.mark.parametrize(
    "name,clause,sense,oracle_clause,holds", _CONSTRAINT_CASES, ids=[c[0] for c in _CONSTRAINT_CASES]
)
def test_defined_constraint_matches_oracle(decidb_cli_gurobi, oracle_solver, name, clause, sense, oracle_clause, holds):
    sql = (
        f"SELECT id, x FROM {_TWO_ROWS} DECIDE x(INT) SUCH THAT x <= 7 AND {clause} "
        f"{'MAXIMIZE' if sense == ObjSense.MAXIMIZE else 'MINIMIZE'} sum(x)"
    )
    rows, cols = decidb_cli_gurobi.execute(sql)
    assert holds(_x_values(rows, cols)), f"{name}: the answer breaks the clause: {rows}"

    oracle_solver.create_model(f"defined_{name}")
    xs = [f"x_{i}" for i in range(len(_TWO_ROWS_DATA))]
    for x in xs:
        oracle_solver.add_variable(x, VarType.INTEGER, lb=0.0, ub=7.0)
    oracle_clause(oracle_solver, xs)
    oracle_solver.set_objective({x: 1.0 for x in xs}, sense)
    compare_solutions(
        rows,
        cols,
        oracle_solver.solve(),
        _TWO_ROWS_DATA,
        ["x"],
        decidb_objective_fn=lambda rs, cs: sum(_x_values(rs, cs)),
    )


# ---------------------------------------------------------------------------
# Objectives
# ---------------------------------------------------------------------------

_TARGETS_SQL = "(VALUES (1, 'A', 0.0), (2, 'A', 0.0), (3, 'B', 3.0)) v(id, grp, target)"
_TARGETS = [(1, "A", 0.0), (2, "A", 0.0), (3, "B", 3.0)]


def _squares(rows, cols):
    """``(group, power(x - target, 2))`` for each answer row."""
    xi, gi, ti = cols.index("x"), cols.index("grp"), cols.index("target")
    return [(r[gi], (float(r[xi]) - float(r[ti])) ** 2) for r in rows]


def _by_group(pairs):
    groups = defaultdict(list)
    for group, value in pairs:
        groups[group].append(value)
    return groups


def _solve_targets(
    cli, oracle, budget, objective_sql, oracle_objective, true_objective, data=_TARGETS, data_sql=_TARGETS_SQL
):
    """Solve `objective_sql` over REAL `0 <= x <= 10` with the coupling row `sum(x) <relation> <bound>` given as
    `budget`, and the oracle alike."""
    relation, bound = budget
    rows, cols = cli.execute(
        f"SELECT id, grp, target, x FROM {data_sql} DECIDE x(REAL) "
        f"SUCH THAT x <= 10 AND sum(x) {relation} {bound} {objective_sql}"
    )
    oracle.create_model("defined_objective")
    xs = [f"x_{i}" for i in range(len(data))]
    for x in xs:
        oracle.add_variable(x, VarType.CONTINUOUS, lb=0.0, ub=10.0)
    oracle.add_constraint({x: 1.0 for x in xs}, relation, float(bound))
    oracle_objective(oracle, xs, data)
    result = oracle.solve()
    compare_solutions(rows, cols, result, data, ["x"], decidb_objective_fn=true_objective)
    return result


def _group_extrema(oracle, xs, data, is_max):
    zs = []
    for group, members in _by_group((r[1], (x, r[2])) for x, r in zip(xs, data)).items():
        zs.append(_oracle_extremum(oracle, f"g{group}", [_square(x, t) for x, t in members], is_max, 0.0, 100.0))
    return zs


def _sum_of_squares_answer(oracle, xs, data, budget_relation, budget, sense):
    """The answer of the model the definition replaced: the group extremum read as the sum of the squares."""
    oracle.create_model("sum_of_squares")
    for x in xs:
        oracle.add_variable(x, VarType.CONTINUOUS, lb=0.0, ub=10.0)
    oracle.add_constraint({x: 1.0 for x in xs}, budget_relation, budget)
    linear, quadratic, constant = {}, {}, 0.0
    for x, r in zip(xs, data):
        lin, quad, const = _square(x, r[2])
        linear.update(lin)
        quadratic.update(quad)
        constant += const
    oracle.set_quadratic_objective(linear, quadratic, sense, constant)
    result = oracle.solve()
    return [result.variable_values[x] for x in xs]


@pytest.mark.min_max
@pytest.mark.quadratic
@pytest.mark.per_clause
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_maximize_sum_max_power_per_is_not_a_sum_of_squares(decidb_cli_gurobi, oracle_solver):
    """`MAXIMIZE sum(per g: max(power(x - t, 2)) by (g))`: 149. The sum-of-squares model's answer scores 109 here, so
    a formulation that sums the squares fails."""

    def true_objective(rows, cols):
        return sum(max(v) for v in _by_group(_squares(rows, cols)).values())

    def oracle_objective(o, xs, data):
        o.set_objective({z: 1.0 for z in _group_extrema(o, xs, data, True)}, ObjSense.MAXIMIZE)

    result = _solve_targets(
        decidb_cli_gurobi,
        oracle_solver,
        ("<=", 20),
        "MAXIMIZE sum(per grp: max(power(x - target, 2)) by (grp))",
        oracle_objective,
        true_objective,
    )
    xs = [f"x_{i}" for i in range(len(_TARGETS))]
    old = _sum_of_squares_answer(oracle_solver, xs, _TARGETS, "<=", 20.0, ObjSense.MAXIMIZE)
    old_score = true_objective([(r[0], r[1], r[2], x) for r, x in zip(_TARGETS, old)], ["id", "grp", "target", "x"])
    assert abs(old_score - result.objective_value) > 1.0, (old_score, result.objective_value)


_UNIT_TARGETS_SQL = "(VALUES (1, 'A', 1.0), (2, 'A', 1.0), (3, 'B', 1.0)) v(id, grp, target)"
_UNIT_TARGETS = [(1, "A", 1.0), (2, "A", 1.0), (3, "B", 1.0)]


@pytest.mark.min_max
@pytest.mark.quadratic
@pytest.mark.per_clause
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_minimize_sum_min_power_per_is_not_a_sum_of_squares(decidb_cli_gurobi, oracle_solver):
    """`MINIMIZE sum(per g: min(power(x - t, 2)) by (g))` with `sum(x) >= 15`: 4.5. The sum-of-squares model's answer
    scores 32 here."""

    def true_objective(rows, cols):
        return sum(min(v) for v in _by_group(_squares(rows, cols)).values())

    def oracle_objective(o, xs, data):
        o.set_objective({z: 1.0 for z in _group_extrema(o, xs, data, False)}, ObjSense.MINIMIZE)

    result = _solve_targets(
        decidb_cli_gurobi,
        oracle_solver,
        (">=", 15),
        "MINIMIZE sum(per grp: min(power(x - target, 2)) by (grp))",
        oracle_objective,
        true_objective,
        _UNIT_TARGETS,
        _UNIT_TARGETS_SQL,
    )
    xs = [f"x_{i}" for i in range(len(_UNIT_TARGETS))]
    old = _sum_of_squares_answer(oracle_solver, xs, _UNIT_TARGETS, ">=", 15.0, ObjSense.MINIMIZE)
    old_score = true_objective(
        [(r[0], r[1], r[2], x) for r, x in zip(_UNIT_TARGETS, old)], ["id", "grp", "target", "x"]
    )
    assert abs(old_score - result.objective_value) > 1.0, (old_score, result.objective_value)


def _flat_mixed(o, xs, data):
    z = _oracle_extremum(o, "m", [({x: 10.0}, {(x, x): 1.0}, 0.0) for x in xs], True, 0.0, 200.0)
    o.set_objective({z: 1.0}, ObjSense.MAXIMIZE)


def _composed_scaled(o, xs, data):
    z = _oracle_extremum(o, "m", [_square(x, r[2]) for x, r in zip(xs, data)], True, 0.0, 100.0)
    o.set_objective({z: 2.0}, ObjSense.MINIMIZE)


def _two_level_inner_sum(o, xs, data):
    # w = MAX over groups of each group's sum of squares, stated exactly as the other extrema are.
    groups = _by_group((r[1], (x, r[2])) for x, r in zip(xs, data))
    members = []
    for group_members in groups.values():
        linear, quadratic, constant = {}, {}, 0.0
        for x, t in group_members:
            lin, quad, const = _square(x, t)
            linear.update(lin)
            quadratic.update(quad)
            constant += const
        members.append((linear, quadratic, constant))
    o.set_objective({_oracle_extremum(o, "w", members, True, 0.0, 300.0): 1.0}, ObjSense.MINIMIZE)


def _two_level_inner_max(o, xs, data):
    # The outer MAX over each group's MAX of squares.
    zs = _group_extrema(o, xs, data, True)
    w = _oracle_extremum(o, "w", [({z: 1.0}, {}, 0.0) for z in zs], True, 0.0, 100.0)
    o.set_objective({w: 1.0}, ObjSense.MINIMIZE)


_OBJECTIVE_CASES = [
    # The flat objective over a body with a linear part: `z + sum(x^2)` before the definition.
    (
        "flat, mixed body",
        ("<=", 6),
        "MAXIMIZE max(power(x, 2) + 10 * x)",
        _flat_mixed,
        lambda rs, cs: max(x * x + 10 * x for x in _x_values(rs, cs)),
    ),
    # A factor sends the objective to the composed path.
    (
        "composed",
        (">=", 6),
        "MINIMIZE 2 * max(power(x - target, 2))",
        _composed_scaled,
        lambda rs, cs: 2 * max(v for _, v in _squares(rs, cs)),
    ),
    # The inner SUM of a two-level objective under an outer MAX.
    (
        "two-level, inner SUM",
        (">=", 6),
        "MINIMIZE max(per grp: sum(power(x - target, 2)) by (grp))",
        _two_level_inner_sum,
        lambda rs, cs: max(sum(v) for v in _by_group(_squares(rs, cs)).values()),
    ),
    # The inner MAX of a two-level objective under an outer MAX.
    (
        "two-level, inner MAX",
        (">=", 6),
        "MINIMIZE max(per grp: max(power(x - target, 2)) by (grp))",
        _two_level_inner_max,
        lambda rs, cs: max(v for _, v in _squares(rs, cs)),
    ),
]


@pytest.mark.min_max
@pytest.mark.quadratic
@pytest.mark.correctness
@pytest.mark.parametrize(
    "name,budget,objective,oracle_objective,true_objective", _OBJECTIVE_CASES, ids=[c[0] for c in _OBJECTIVE_CASES]
)
def test_defined_objective_matches_oracle(
    decidb_cli_gurobi, oracle_solver, name, budget, objective, oracle_objective, true_objective
):
    _solve_targets(decidb_cli_gurobi, oracle_solver, budget, objective, oracle_objective, true_objective)


# ---------------------------------------------------------------------------
# Model shape
# ---------------------------------------------------------------------------


def _dump(cli, sql):
    with tempfile.TemporaryDirectory() as tmp:
        return cli.dump_model(sql, os.path.join(tmp, "model.dump"))


def _member_relations(dump):
    """The relation of each defining row, read as `helper REL body`: '<=', '>=' or '='.

    A member row holds one helper column (linear part) and the body's square (quadratic
    part); the solver may have moved either across the relation.
    """
    relations = set()
    for sense, linear, quadratic in re.findall(r"^qrow \d+: sense=(\S) rhs=\S+ \| (.*) \| (.*)$", dump, re.MULTILINE):
        helper = float(linear.split(":")[1])
        square = float(quadratic.split(":")[1])
        assert len(linear.split()) == 1 and helper * square < 0, dump
        flipped = {"<": ">=", ">": "<=", "=": "="}
        relations.add({"<": "<=", ">": ">=", "=": "="}[sense] if helper > 0 else flipped[sense])
    return relations


_PUSH_CASES = [
    ("pushed up", "max(power(x, 2)) >= 49 MINIMIZE sum(x)", "<="),
    ("pushed down", "sum(x) + max(power(x, 2)) <= 20 MAXIMIZE sum(x)", ">="),
    ("pushed neither way", "max(power(x, 2)) = 25 MAXIMIZE sum(x)", "="),
    # A term under a factor of exactly zero is never walked, so nothing records a push for it.
    ("not reached by the polarity walk", "sum(x) + 0 * max(power(x, 2)) <= 5 MAXIMIZE sum(x)", "="),
]


@pytest.mark.min_max
@pytest.mark.quadratic
@pytest.mark.parametrize("name,clause,relation", _PUSH_CASES, ids=[c[0] for c in _PUSH_CASES])
def test_member_row_follows_the_push(decidb_cli_gurobi, name, clause, relation):
    dump = _dump(decidb_cli_gurobi, f"SELECT id, x FROM {_TWO_ROWS} DECIDE x(INT) SUCH THAT x <= 7 AND {clause}")
    assert _member_relations(dump) == {relation}, dump
    # The helper has no floor of 0. (An easy half folds the clause's own bound into its ceiling: `= 25`.)
    assert re.search(r"^col \d+: lb=-1e\+30 ub=\S+ int=1 ", dump, re.MULTILINE), dump


_SHARED_CASES = [
    # One clause pushes the MAX up and the other down: the shared row is `t = q`.
    ("two clauses", "max(power(x, 2)) >= 49 AND sum(x) + max(power(x, 2)) <= 60 MAXIMIZE sum(x)"),
    # The objective pushes it up and the clause down.
    ("a clause and the objective", "sum(x) + max(power(x, 2)) <= 20 MAXIMIZE max(power(x, 2))"),
]


@pytest.mark.min_max
@pytest.mark.quadratic
@pytest.mark.parametrize("name,clause", _SHARED_CASES, ids=[c[0] for c in _SHARED_CASES])
def test_readers_of_one_reducer_share_one_helper(decidb_cli_gurobi, name, clause):
    """One helper per row, defined on both sides, whoever reads it."""
    dump = _dump(decidb_cli_gurobi, f"SELECT id, x FROM {_TWO_ROWS} DECIDE x(INT) SUCH THAT x <= 7 AND {clause}")
    assert re.search(r"^num_qrows: 2$", dump, re.MULTILINE), dump
    assert _member_relations(dump) == {"="}, dump


_SCOPE_CASES = [
    ("clause when", "when id = 2: sum(x) + max(power(x, 2)) <= 20"),
    ("reducer filter", "sum(x) + max(when id = 2: power(x, 2)) <= 20"),
    ("clause when and reducer filter", "when id >= 1: sum(x) + max(when id = 2: power(x, 2)) <= 20"),
]


@pytest.mark.min_max
@pytest.mark.quadratic
@pytest.mark.parametrize("name,clause", _SCOPE_CASES, ids=[c[0] for c in _SCOPE_CASES])
def test_helper_is_defined_on_the_rows_the_reducer_reads(decidb_cli_gurobi, name, clause):
    """Of two rows the MAX reads row 2 only, so one defining row."""
    dump = _dump(
        decidb_cli_gurobi, f"SELECT id, x FROM {_TWO_ROWS} DECIDE x(INT) SUCH THAT x <= 7 AND {clause} MAXIMIZE sum(x)"
    )
    assert re.search(r"^num_qrows: 1$", dump, re.MULTILINE), dump


_EXPLAIN_CASES = [
    ("at most", "max(power(x, 2)) >= 49 MINIMIZE sum(x)"),
    ("at least", "sum(x) + max(power(x, 2)) <= 20 MAXIMIZE sum(x)"),
    ("equal to", "max(power(x, 2)) = 25 MAXIMIZE sum(x)"),
]


@pytest.mark.min_max
@pytest.mark.quadratic
@pytest.mark.parametrize("side,clause", _EXPLAIN_CASES, ids=[c[0] for c in _EXPLAIN_CASES])
def test_explain_states_the_side_and_hides_the_row(decidb_cli_gurobi, side, clause):
    """The account line says how the helper holds the body; the defining row, which restates the body, is left out."""
    out = decidb_cli_gurobi.execute_raw(
        f"EXPLAIN SELECT id, x FROM {_TWO_ROWS} DECIDE x(INT) SUCH THAT x <= 7 AND {clause}"
    ).stdout
    text = re.sub(r"[│┌┐└┘├┤┬┴┼─\s]", "", out)
    assert f"↳power(x,2)becomesahelperdecisionperrow,{side.replace(' ', '')}itsvalue" in text, text
    assert "power(x,2)-power(x,2)" not in text, text


@pytest.mark.min_max
@pytest.mark.quadratic
def test_cheap_lone_extremum_is_not_defined(decidb_cli_gurobi):
    """`max(q) <= K` is one quadratic row per member already, so no helper column is added."""
    dump = _dump(
        decidb_cli_gurobi,
        f"SELECT id, x FROM {_TWO_ROWS} DECIDE x(INT) SUCH THAT x <= 7 AND max(power(x, 2)) <= 16 " "MAXIMIZE sum(x)",
    )
    assert re.search(r"^num_vars: 2$", dump, re.MULTILINE), dump


# ---------------------------------------------------------------------------
# Refusal
# ---------------------------------------------------------------------------

_REFUSAL_CASES = [
    # Gurobi with its native constructs masked: the same refusal HiGHS gets.
    (
        "native MIN/MAX off",
        {"DECIDB_NATIVE_CONSTRUCTS": "off"},
        "x(INT)",
        "x <= 7 AND max(power(x, 2)) >= 49",
        "MINIMIZE sum(x)",
        "'max(power(x, 2))' takes the maximum of a squared or multiplied expression",
    ),
    (
        "MIN on HiGHS",
        {"DECIDB_FORCE_SOLVER": "highs"},
        "x(REAL)",
        "x <= 7",
        "MINIMIZE min(power(x, 2))",
        "'min(power(x, 2))' takes the minimum of a squared or multiplied expression",
    ),
    (
        "SUM beside a MIN or MAX",
        {"DECIDB_FORCE_SOLVER": "highs"},
        "x(INT)",
        "x <= 7 AND sum(power(x, 2)) + max(x) <= 30",
        "MAXIMIZE sum(x)",
        "'sum(power(x, 2))' squares or multiplies decision variables beside a MIN or MAX",
    ),
    (
        "SUM inside a MIN or MAX",
        {"DECIDB_FORCE_SOLVER": "highs"},
        "x(REAL)",
        "x <= 7",
        "MINIMIZE max(per id: sum(power(x, 2)) by (id))",
        "'sum(power(x, 2))' squares or multiplies decision variables inside a MIN or MAX",
    ),
]


@pytest.mark.min_max
@pytest.mark.quadratic
@pytest.mark.error
@pytest.mark.parametrize(
    "name,env,decision,constraints,objective,cause", _REFUSAL_CASES, ids=[c[0] for c in _REFUSAL_CASES]
)
def test_defined_body_refused_without_native_minmax(decidb_cli, name, env, decision, constraints, objective, cause):
    sql = f"SELECT id, x FROM {_TWO_ROWS} DECIDE {decision} SUCH THAT {constraints} {objective}"
    with pytest.raises(DecidBCliError) as error:
        decidb_cli.with_env(env).execute(sql)
    message = str(error.value)
    assert cause in message, message
    assert "which needs Gurobi" in message, message
    assert "replace power(x, 2) with an expression linear in the decision variables" in message, message


# ---------------------------------------------------------------------------
# One helper per instance of the scopes the body reads
# ---------------------------------------------------------------------------

_SCOPE_ROWS = "(VALUES (1, 'a', 2), (2, 'a', 3), (3, 'b', 4)) t(id, k, w)"
_SCOPE_W = (2, 3, 4)


@pytest.mark.min_max
@pytest.mark.quadratic
@pytest.mark.correctness
@pytest.mark.parametrize(
    "declare, clause, objective, domains, holds, value, num_vars, num_qrows",
    [
        pytest.param(
            "x(INT), per (): d(INT)",
            "d <= 3 AND x <= 3 AND SUM(power(d, 2)) + max(x) <= 20",
            "MAXIMIZE SUM(w * d) + SUM(x)",
            {"d": range(4), **{f"x{i}": range(4) for i in range(3)}},
            lambda a: 3 * a["d"] ** 2 + max(a[f"x{i}"] for i in range(3)) <= 20,
            lambda a: sum(_SCOPE_W) * a["d"] + sum(a[f"x{i}"] for i in range(3)),
            # x per row, d, one helper and the maximum's column.
            6,
            1,
            id="body-query-wide-is-one-helper",
        ),
        pytest.param(
            "per k: d(INT)",
            "d <= 3 AND max(power(d, 2)) by (k) >= 4",
            "MINIMIZE SUM(w * d)",
            {f"d{i}": range(4) for i in range(3)},
            lambda a: a["d0"] == a["d1"] and a["d0"] ** 2 >= 4 and a["d2"] ** 2 >= 4,
            lambda a: sum(w * a[f"d{i}"] for i, w in enumerate(_SCOPE_W)),
            # d and one helper per key, and each group's maximum.
            6,
            2,
            id="body-keyed-is-one-helper-per-key",
        ),
    ],
)
def test_body_helper_takes_the_scope_the_body_reads(
    decidb_cli_gurobi, tmp_path, declare, clause, objective, domains, holds, value, num_vars, num_qrows
):
    """A non-linear body's helper is a column defined by the body: one for a query-wide body
    and one per key for a keyed one, with its quadratic row once per instance, while the
    reducer still reads it on every row. Only Gurobi states the MIN/MAX over a helper.
    Judged against every assignment."""
    sql = f"""
        SELECT * FROM {_SCOPE_ROWS}
        DECIDE {declare} SUCH THAT {clause} {objective} ORDER BY id"""
    dump = decidb_cli_gurobi.dump_model(sql, tmp_path / "model.dump")
    assert re.search(rf"^num_vars: {num_vars}$", dump, re.M), dump
    assert re.search(rf"^num_qrows: {num_qrows}$", dump, re.M), dump
    rows, columns = decidb_cli_gurobi.execute(sql)
    judge_rows([dict(zip(columns, row)) for row in rows], domains, holds, value, objective.startswith("MAXIMIZE"))
