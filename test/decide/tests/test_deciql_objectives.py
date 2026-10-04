"""Objectives in DeciQL (syntax_reference §6; spec §4.3; deck p62-63).

`MAXIMIZE | MINIMIZE [PER ():] expr [THEN stage ...] | SATISFY`. An objective is
evaluated once, so it reads reducers, `PER ()` decisions and constants; `THEN`
chains lexicographic stages, each optimized among the optima of the ones before
it. Every oracle below solves the same stages explicitly: solve one, hold it at
its optimum as a row (exactly when the stage is integer-valued, within 1e-6
relative when continuous), solve the next.
"""

import re

import pytest

from solver.types import ObjSense, SolverStatus, VarType

# id, group, capacity, unit cost
_T = "(VALUES (1, 'a', 3, 2), (2, 'a', 7, 5), (3, 'b', 2, 1)) t(id, grp, cap, cost)"
_CAP = {1: 3, 2: 7, 3: 2}
_COST = {1: 2, 2: 5, 3: 1}
_GRP = {1: "a", 2: "a", 3: "b"}


def _rows(decidb_cli, sql, *cols):
    rows, names = decidb_cli.execute(sql)
    idx = [names.index(c) for c in cols]
    return sorted(tuple(r[i] for i in idx) for r in rows)


def _lexi(oracle, build, stages, integer=True):
    """Solve `stages` ([(coeffs, sense), ...]) lexicographically over the model
    `build(oracle)` adds. Returns the last result and the list of stage optima."""
    frozen = []
    result = None
    for coeffs, sense in stages:
        oracle.create_model("lexi")
        build(oracle)
        for held, held_sense, value in frozen:
            slack = 0.5 if integer else 1e-6 * abs(value)
            if held_sense == ObjSense.MAXIMIZE:
                oracle.add_constraint(held, ">=", value - slack)
            else:
                oracle.add_constraint(held, "<=", value + slack)
        oracle.set_objective(coeffs, sense)
        result = oracle.solve()
        assert result.status == SolverStatus.OPTIMAL
        frozen.append((coeffs, sense, result.objective_value))
    return result, [f[2] for f in frozen]


def _xs(o, lb=0.0):
    for i, cap in _CAP.items():
        o.add_variable(f"x{i}", VarType.INTEGER, lb=lb, ub=float(cap))


_SUM_X = {f"x{i}": 1.0 for i in _CAP}
_COST_X = {f"x{i}": float(_COST[i]) for i in _CAP}


# ---------------------------------------------------------------------------
# THEN: how a solved stage is held
# ---------------------------------------------------------------------------

@pytest.mark.var_integer
@pytest.mark.obj_maximize
@pytest.mark.correctness
@pytest.mark.parametrize("weight", [10 ** 9, 10 ** 11])
def test_integer_stage_is_held_exactly_whatever_its_magnitude(decidb_cli, oracle_solver, weight):
    """§6: an integer-valued stage (integer decisions, whole coefficients) is held
    exactly at any magnitude. Stage one, `weight * w + SUM(x)`, peaks at
    3 * weight + 12 (w = 3, x = cap); stage two then has one spare unit per row:
    y = 1, 1, 1. A 1e-6 relative hold (3000 units or more here; what the same
    query gets with the weight written `weight + 0.5`) lets stage two trade every
    x for y (x = 0, y = cap + 1); a dropped THEN leaves y = 0."""
    got = _rows(decidb_cli, f"""
        SELECT id, w, x, y FROM {_T}
        DECIDE PER (): w(INT) BETWEEN 0 AND 3, x(INT) <= cap, y(INT) BETWEEN 0 AND 20
        SUCH THAT x + y <= cap + 1
        MAXIMIZE {weight} * w + SUM(x) THEN MAXIMIZE SUM(y)
    """, "id", "w", "x", "y")

    def build(o):
        o.add_variable("w", VarType.INTEGER, lb=0.0, ub=3.0)
        _xs(o)
        for i, cap in _CAP.items():
            o.add_variable(f"y{i}", VarType.INTEGER, lb=0.0, ub=20.0)
            o.add_constraint({f"x{i}": 1.0, f"y{i}": 1.0}, "<=", float(cap + 1))

    stage1 = {"w": float(weight), **_SUM_X}
    stage2 = {f"y{i}": 1.0 for i in _CAP}
    result, optima = _lexi(oracle_solver, build, [(stage1, ObjSense.MAXIMIZE), (stage2, ObjSense.MAXIMIZE)])
    assert optima[0] == pytest.approx(3 * weight + 12, abs=0.5)
    assert result.objective_value == pytest.approx(3.0)
    assert got == [(1, 3, 3, 1), (2, 3, 7, 1), (3, 3, 2, 1)]
    assert sum(y for *_, y in got) == pytest.approx(result.objective_value)


@pytest.mark.var_integer
@pytest.mark.obj_maximize
@pytest.mark.obj_minimize
@pytest.mark.correctness
@pytest.mark.parametrize("stage", ["MAXIMIZE SUM(x) + 100", "MAXIMIZE SUM(x - 100)", "MINIMIZE 100 - SUM(x)"])
def test_a_constant_in_a_stage_does_not_move_its_hold(decidb_cli, oracle_solver, stage):
    """§6: a stage is held at its optimum, and an additive constant (outside the
    reducer or inside it) changes the stage's value, not its optimum: x = cap,
    then y = 1 on every row. A hold whose bound carried the constant would be 100
    or 300 units loose (stage two trades every x for y: x = 0, y = cap + 1) or 100
    units too tight (stage two infeasible)."""
    got = _rows(decidb_cli, f"""
        SELECT id, x, y FROM {_T}
        DECIDE x(INT) <= cap, y(INT) BETWEEN 0 AND 20
        SUCH THAT x + y <= cap + 1
        {stage} THEN MAXIMIZE SUM(y)
    """, "id", "x", "y")

    def build(o):
        _xs(o)
        for i, cap in _CAP.items():
            o.add_variable(f"y{i}", VarType.INTEGER, lb=0.0, ub=20.0)
            o.add_constraint({f"x{i}": 1.0, f"y{i}": 1.0}, "<=", float(cap + 1))

    stages = [(_SUM_X, ObjSense.MAXIMIZE), ({f"y{i}": 1.0 for i in _CAP}, ObjSense.MAXIMIZE)]
    result, optima = _lexi(oracle_solver, build, stages)
    assert optima[0] == pytest.approx(12.0)
    assert got == [(1, 3, 1), (2, 7, 1), (3, 2, 1)]
    assert sum(y for *_, y in got) == pytest.approx(result.objective_value)


@pytest.mark.var_real
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_continuous_stage_is_held_within_relative_slack(decidb_cli, oracle_solver):
    """§6: a continuous stage is held within 1e-6 relative of its optimum. Stage
    one, `1000000 * c + SUM(x)` under x + y <= cap and 2x + y <= 5, peaks at
    1000007 (c = 1, x = 2.5, 2.5, 2), so its hold gives way by 1.000007; stage two
    spends that on the rows where one x buys two y: SUM(y) = 2.000014. An exact
    (or absolute 1e-6) hold would leave SUM(y) at ~0, and no hold at all gives
    SUM(y) = 10 (x = 0, y = 3, 5, 2). The per-row split of the slack is not
    unique, so only the sums are asserted."""
    got = _rows(decidb_cli, f"""
        SELECT id, c, x, y FROM {_T}
        DECIDE PER (): c(REAL) BETWEEN 0 AND 1, x(REAL), y(REAL) BETWEEN 0 AND 20
        SUCH THAT x + y <= cap AND 2 * x + y <= 5
        MAXIMIZE 1000000 * c + SUM(x) THEN MAXIMIZE SUM(y)
    """, "id", "c", "x", "y")

    def build(o):
        o.add_variable("c", VarType.CONTINUOUS, lb=0.0, ub=1.0)
        for i, cap in _CAP.items():
            o.add_variable(f"x{i}", VarType.CONTINUOUS, lb=0.0, ub=float(cap))
            o.add_variable(f"y{i}", VarType.CONTINUOUS, lb=0.0, ub=20.0)
            o.add_constraint({f"x{i}": 1.0, f"y{i}": 1.0}, "<=", float(cap))
            o.add_constraint({f"x{i}": 2.0, f"y{i}": 1.0}, "<=", 5.0)

    stages = [({"c": 1e6, **{f"x{i}": 1.0 for i in _CAP}}, ObjSense.MAXIMIZE),
              ({f"y{i}": 1.0 for i in _CAP}, ObjSense.MAXIMIZE)]
    result, optima = _lexi(oracle_solver, build, stages, integer=False)
    assert optima[0] == pytest.approx(1000007.0)
    assert result.objective_value == pytest.approx(2.000014, rel=1e-4)
    assert all(c == pytest.approx(1.0) for _, c, _, _ in got)
    stage_one = 1e6 * got[0][1] + sum(x for _, _, x, _ in got)
    assert stage_one >= optima[0] * (1 - 1e-6) - 1e-6
    assert sum(y for *_, y in got) == pytest.approx(result.objective_value, rel=1e-4)


@pytest.mark.var_integer
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_three_stages_each_settle_the_ties_of_the_one_before(decidb_cli, oracle_solver):
    """§6, deck p63: stage k is optimized among the optima of stages 1..k-1, in the
    order written. Stage one fixes SUM(x) = 5; stage two puts the whole y budget on
    the row of cost 5 (25), leaving room for z on only two rows; stage three takes
    the dearer two (7): x = 2, 1, 2, y = 0, 5, 0, z = 1, 1, 0. Solving stage three
    before stage two gives z = 1, 1, 1 and y = 0, 4, 0; dropping stage three gives
    z = 0; dropping stage two gives y = 0."""
    got = _rows(decidb_cli, f"""
        SELECT id, x, y, z FROM {_T}
        DECIDE x(INT), y(INT), z(INT) BETWEEN 0 AND 1
        SUCH THAT x + y + z <= cap AND PER (): SUM(x) <= 5 AND PER (): SUM(y) <= 5
        MAXIMIZE SUM(x) THEN MAXIMIZE SUM(cost * y) THEN MAXIMIZE SUM(cost * z)
    """, "id", "x", "y", "z")

    def build(o):
        _xs(o)
        for i, cap in _CAP.items():
            o.add_variable(f"y{i}", VarType.INTEGER, lb=0.0, ub=float(cap))
            o.add_variable(f"z{i}", VarType.INTEGER, lb=0.0, ub=1.0)
            o.add_constraint({f"x{i}": 1.0, f"y{i}": 1.0, f"z{i}": 1.0}, "<=", float(cap))
        o.add_constraint(_SUM_X, "<=", 5.0)
        o.add_constraint({f"y{i}": 1.0 for i in _CAP}, "<=", 5.0)

    stages = [(_SUM_X, ObjSense.MAXIMIZE),
              ({f"y{i}": float(_COST[i]) for i in _CAP}, ObjSense.MAXIMIZE),
              ({f"z{i}": float(_COST[i]) for i in _CAP}, ObjSense.MAXIMIZE)]
    result, optima = _lexi(oracle_solver, build, stages)
    assert optima == pytest.approx([5.0, 25.0, 7.0])
    assert got == [(1, 2, 0, 1), (2, 1, 5, 1), (3, 2, 0, 0)]
    assert sum(_COST[i] * z for i, *_, z in got) == pytest.approx(result.objective_value)


@pytest.mark.var_integer
@pytest.mark.obj_maximize
@pytest.mark.obj_minimize
@pytest.mark.avg_rewrite
@pytest.mark.correctness
def test_avg_is_a_linear_later_stage(decidb_cli, oracle_solver):
    """§6: a later stage may be an AVG of decision terms. Stage one weights the two
    'a' rows equally, so it ties x1 + x2 = 8 (x3 = 0); MINIMIZE AVG(cost * x)
    settles the tie on the cheap row: x = 3, 5, 0 (31/3). Dropping the stage or
    flipping it to MAXIMIZE gives 1, 7, 0 (37/3). Over a fixed row set AVG ranks
    assignments exactly as SUM does, so the rows pin that AVG is accepted and
    minimized as a later stage, not its divisor."""
    got = _rows(decidb_cli, """
        SELECT id, x FROM (VALUES (1, 'a', 3, 2, 2), (2, 'a', 7, 5, 2), (3, 'b', 2, 1, 1)) t(id, grp, cap, cost, w)
        DECIDE x(INT)
        SUCH THAT x <= cap AND PER (): SUM(x) <= 8
        MAXIMIZE SUM(w * x) THEN MINIMIZE AVG(cost * x)
    """, "id", "x")
    w = {1: 2, 2: 2, 3: 1}

    def build(o):
        _xs(o)
        o.add_constraint(_SUM_X, "<=", 8.0)

    stages = [({f"x{i}": float(w[i]) for i in _CAP}, ObjSense.MAXIMIZE),
              ({f"x{i}": _COST[i] / 3.0 for i in _CAP}, ObjSense.MINIMIZE)]
    result, optima = _lexi(oracle_solver, build, stages)
    assert optima == pytest.approx([16.0, 31 / 3])
    assert got == [(1, 3), (2, 5), (3, 0)]
    assert sum(_COST[i] * x for i, x in got) / 3 == pytest.approx(result.objective_value)


@pytest.mark.var_integer
@pytest.mark.obj_maximize
@pytest.mark.obj_minimize
@pytest.mark.per_clause
@pytest.mark.min_max
@pytest.mark.correctness
def test_nested_min_first_stage_then_a_linear_second(decidb_cli, oracle_solver):
    """§6: a MIN/MAX first stage may be the nested `MIN(PER k: SUM(e) BY (k))`,
    followed by a linear stage. Max-min of the group totals under SUM(x) <= 8 is 2
    (group b caps at 2); stage two then buys group a's 2 on its cheaper row:
    x = 2, 0, 2 (cost 6). Dropping stage two returns 0, 6, 2 here; MAX in place of
    MIN gives 3, 5, 0; a flat SUM(x) first stage gives 3, 3, 2."""
    got = _rows(decidb_cli, f"""
        SELECT id, x FROM {_T}
        DECIDE x(INT) <= cap
        SUCH THAT PER (): SUM(x) <= 8
        MAXIMIZE MIN(PER grp: SUM(x) BY (grp)) THEN MINIMIZE SUM(cost * x)
    """, "id", "x")

    def build(o):
        _xs(o)
        o.add_variable("m", VarType.CONTINUOUS, lb=-100.0, ub=100.0)
        o.add_constraint(_SUM_X, "<=", 8.0)
        for g in ("a", "b"):
            o.add_constraint({"m": 1.0, **{f"x{i}": -1.0 for i in _CAP if _GRP[i] == g}}, "<=", 0.0)

    stages = [({"m": 1.0}, ObjSense.MAXIMIZE), (_COST_X, ObjSense.MINIMIZE)]
    result, optima = _lexi(oracle_solver, build, stages)
    assert optima == pytest.approx([2.0, 6.0])
    assert got == [(1, 2), (2, 0), (3, 2)]
    assert sum(_COST[i] * x for i, x in got) == pytest.approx(result.objective_value)


# ---------------------------------------------------------------------------
# What a single stage may read
# ---------------------------------------------------------------------------

@pytest.mark.var_integer
@pytest.mark.obj_minimize
@pytest.mark.per_clause
@pytest.mark.min_max
@pytest.mark.correctness
def test_constant_factor_on_a_nested_reducer_scales_the_objective(decidb_cli, oracle_solver):
    """§6 / optimizer: a constant factor on the inner reducer of a nested MIN/MAX
    objective is folded into its body. `MINIMIZE MAX(PER grp: SUM(cost * x) BY
    (grp) / 2)` under SUM(x) >= 8 and x >= 1 settles at x = 3, 3, 2 (group a costs
    21, halved 10.5). A positive factor cannot move the optimum (the query without
    `/ 2` returns the same rows), so that half pins only that the factor is
    accepted there. A sign does move it: `MINIMIZE MAX(PER grp: -2 * SUM(x) BY
    (grp))` is a max-min of the group totals (-2 * 2 = -4), while a lost sign
    minimizes the totals to x = 0 (objective 0). That optimum is not unique, so
    its objective is asserted, not its rows."""
    got = _rows(decidb_cli, f"""
        SELECT id, x FROM {_T}
        DECIDE x(INT)
        SUCH THAT x >= 1 AND x <= cap AND PER (): SUM(x) >= 8
        MINIMIZE MAX(PER grp: SUM(cost * x) BY (grp) / 2)
    """, "id", "x")
    oracle_solver.create_model("halved_max")
    _xs(oracle_solver, lb=1.0)
    oracle_solver.add_variable("m", VarType.CONTINUOUS, lb=-100.0, ub=100.0)
    oracle_solver.add_constraint(_SUM_X, ">=", 8.0)
    for g in ("a", "b"):
        oracle_solver.add_constraint({"m": -1.0, **{f"x{i}": _COST[i] / 2.0 for i in _CAP if _GRP[i] == g}}, "<=", 0.0)
    oracle_solver.set_objective({"m": 1.0}, ObjSense.MINIMIZE)
    halved = oracle_solver.solve()
    assert halved.status == SolverStatus.OPTIMAL
    assert halved.objective_value == pytest.approx(10.5)
    assert got == [(1, 3), (2, 3), (3, 2)]
    assert max(sum(_COST[i] * x for i, x in got if _GRP[i] == g) / 2 for g in ("a", "b")) == pytest.approx(
        halved.objective_value)

    negated = _rows(decidb_cli, f"""
        SELECT id, x FROM {_T}
        DECIDE x(INT) <= cap
        SUCH THAT PER (): SUM(x) <= 8
        MINIMIZE MAX(PER grp: -2 * SUM(x) BY (grp))
    """, "id", "x")
    oracle_solver.create_model("negated_max")
    _xs(oracle_solver)
    oracle_solver.add_variable("m", VarType.CONTINUOUS, lb=-100.0, ub=100.0)
    oracle_solver.add_constraint(_SUM_X, "<=", 8.0)
    for g in ("a", "b"):
        oracle_solver.add_constraint({"m": -1.0, **{f"x{i}": -2.0 for i in _CAP if _GRP[i] == g}}, "<=", 0.0)
    oracle_solver.set_objective({"m": 1.0}, ObjSense.MINIMIZE)
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL
    assert result.objective_value == pytest.approx(-4.0)
    assert all(0 <= x <= _CAP[i] for i, x in negated)
    assert sum(x for _, x in negated) <= 8
    totals = {g: sum(x for i, x in negated if _GRP[i] == g) for g in ("a", "b")}
    assert max(-2 * t for t in totals.values()) == pytest.approx(result.objective_value)


@pytest.mark.var_integer
@pytest.mark.obj_maximize
@pytest.mark.obj_minimize
@pytest.mark.obj_complex
@pytest.mark.correctness
def test_arithmetic_over_query_wide_decisions_is_an_objective(decidb_cli, oracle_solver):
    """§6: linear arithmetic over `PER ()` decisions alone, or beside a reducer, is
    one value for the query. `MINIMIZE -(c + 2 * d) + 1` under c + d <= cap (so
    c + d <= 2) picks c = 0, d = 2 (a lost sign gives 0, 0; a lost weight ties and
    returns 2, 0 here); `MAXIMIZE -c` above c >= 1 stops at 1 (a lost sign gives
    2); `MAXIMIZE SUM(x) + 8 * c` opens c although it costs two units of every x
    (14 > 12), so a dropped `8 * c` gives c = 0 and x = cap."""
    got = _rows(decidb_cli, f"""
        SELECT id, c, d FROM {_T}
        DECIDE PER (): c(INT) BETWEEN 0 AND 4, PER (): d(INT) BETWEEN 0 AND 4
        SUCH THAT c + d <= cap MINIMIZE -(c + 2 * d) + 1
    """, "id", "c", "d")
    oracle_solver.create_model("two_scalars")
    oracle_solver.add_variable("c", VarType.INTEGER, lb=0.0, ub=4.0)
    oracle_solver.add_variable("d", VarType.INTEGER, lb=0.0, ub=4.0)
    for cap in _CAP.values():
        oracle_solver.add_constraint({"c": 1.0, "d": 1.0}, "<=", float(cap))
    oracle_solver.set_objective({"c": -1.0, "d": -2.0}, ObjSense.MINIMIZE)
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL
    assert result.objective_value == pytest.approx(-4.0)
    assert got == [(1, 0, 2), (2, 0, 2), (3, 0, 2)]
    assert -(got[0][1] + 2 * got[0][2]) == pytest.approx(result.objective_value)

    negated = _rows(decidb_cli, f"""
        SELECT id, c FROM {_T} DECIDE PER (): c(INT) BETWEEN 0 AND 4
        SUCH THAT c >= 1 AND c <= cap MAXIMIZE -c
    """, "id", "c")
    oracle_solver.create_model("negated_scalar")
    oracle_solver.add_variable("c", VarType.INTEGER, lb=1.0, ub=4.0)
    for cap in _CAP.values():
        oracle_solver.add_constraint({"c": 1.0}, "<=", float(cap))
    oracle_solver.set_objective({"c": -1.0}, ObjSense.MAXIMIZE)
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL
    assert result.objective_value == pytest.approx(-1.0)
    assert negated == [(1, 1), (2, 1), (3, 1)]

    mixed = _rows(decidb_cli, f"""
        SELECT id, x, c FROM {_T}
        DECIDE x(INT), PER (): c(INT) BETWEEN 0 AND 1
        SUCH THAT x <= cap - 2 * c
        MAXIMIZE SUM(x) + 8 * c
    """, "id", "x", "c")
    oracle_solver.create_model("mixed")
    oracle_solver.add_variable("c", VarType.BINARY)
    _xs(oracle_solver)
    for i, cap in _CAP.items():
        oracle_solver.add_constraint({f"x{i}": 1.0, "c": 2.0}, "<=", float(cap))
    oracle_solver.set_objective({"c": 8.0, **_SUM_X}, ObjSense.MAXIMIZE)
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL
    assert result.objective_value == pytest.approx(14.0)
    assert mixed == [(1, 1, 1), (2, 5, 1), (3, 0, 1)]
    assert sum(x for _, x, _ in mixed) + 8 * mixed[0][2] == pytest.approx(result.objective_value)


@pytest.mark.var_integer
@pytest.mark.obj_maximize
@pytest.mark.obj_minimize
@pytest.mark.correctness
@pytest.mark.parametrize("first,second", [("PER (): ", "PER (): "), ("per ( ) : ", ""), ("", "PER (): "), ("", "")])
def test_explicit_per_empty_prefix_spells_the_default_objective_scope(decidb_cli, oracle_solver, first, second):
    """§6, deck p62-63: an objective is generated once, so `PER ():` on any stage
    only states the scope every stage already has, and the four spellings are one
    query. Being equivalent to its omission by definition, the prefix is
    discriminated by the chain around it: stage one fills SUM(x) = 8 and stage two
    buys it cheapest (x = 3, 3, 2, cost 23). A prefixed stage that was lost would
    give 0, 6, 2 (stage two) or 0, 0, 0 (stage one)."""
    got = _rows(decidb_cli, f"""
        SELECT id, x FROM {_T} DECIDE x(INT) <= cap SUCH THAT PER (): SUM(x) <= 8
        MAXIMIZE {first}SUM(x) THEN MINIMIZE {second}SUM(cost * x)
    """, "id", "x")

    def build(o):
        _xs(o)
        o.add_constraint(_SUM_X, "<=", 8.0)

    result, optima = _lexi(oracle_solver, build, [(_SUM_X, ObjSense.MAXIMIZE), (_COST_X, ObjSense.MINIMIZE)])
    assert optima == pytest.approx([8.0, 23.0])
    assert got == [(1, 3), (2, 3), (3, 2)]
    assert sum(_COST[i] * x for i, x in got) == pytest.approx(result.objective_value)


@pytest.mark.var_integer
@pytest.mark.obj_maximize
@pytest.mark.when_objective
@pytest.mark.correctness
def test_objective_when_reads_only_the_filtered_rows(decidb_cli, oracle_solver):
    """§4 / §6: a reducer's own WHEN filters the rows an objective reads. Under
    SUM(x) <= 8, `SUM(WHEN id > 1: cost * x)` spends 7 on row 2 and the last unit
    on row 3, which the filter keeps, rather than on row 1, which it drops though
    it costs more: x = 0, 7, 1 (36). An ignored filter gives 1, 7, 0 (37). A WHEN
    that keeps no row leaves the objective without a value and is refused (§4:
    only an objective reducer over no rows is an error)."""
    got = _rows(decidb_cli, f"""
        SELECT id, x FROM {_T}
        DECIDE x(INT) <= cap
        SUCH THAT PER (): SUM(x) <= 8
        MAXIMIZE SUM(WHEN id > 1: cost * x)
    """, "id", "x")
    oracle_solver.create_model("objective_when")
    _xs(oracle_solver)
    oracle_solver.add_constraint(_SUM_X, "<=", 8.0)
    oracle_solver.set_objective({f"x{i}": float(_COST[i]) for i in _CAP if i > 1}, ObjSense.MAXIMIZE)
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL
    assert result.objective_value == pytest.approx(36.0)
    assert got == [(1, 0), (2, 7), (3, 1)]
    assert sum(_COST[i] * x for i, x in got if i > 1) == pytest.approx(result.objective_value)

    decidb_cli.assert_error(f"""
        SELECT id, x FROM {_T} DECIDE x(INT) SUCH THAT x <= cap
        MAXIMIZE SUM(WHEN grp = 'z': x)
    """, match=r"empty row set")


# ---------------------------------------------------------------------------
# SATISFY
# ---------------------------------------------------------------------------

@pytest.mark.var_integer
@pytest.mark.error_infeasible
@pytest.mark.correctness
def test_satisfy_and_an_omitted_objective_take_any_feasible_assignment(decidb_cli, oracle_solver):
    """§6: `SATISFY`, or no objective, asks for any assignment that meets the
    constraints; the rows are checked against the constraints, not a vector (the
    query without `SUM(x) = 9` returns 1, 1, 1, so the sum check discriminates). An
    infeasible SATISFY is refused as infeasible."""
    oracle_solver.create_model("feasible")
    _xs(oracle_solver, lb=1.0)
    oracle_solver.add_constraint(_SUM_X, "=", 9.0)
    oracle_solver.set_objective({}, ObjSense.MINIMIZE)
    assert oracle_solver.solve().status == SolverStatus.OPTIMAL
    for objective in ("SATISFY", ""):
        got = _rows(decidb_cli, f"""
            SELECT id, x FROM {_T} DECIDE x(INT)
            SUCH THAT x <= cap AND x >= 1 AND PER (): SUM(x) = 9 {objective}
        """, "id", "x")
        assert [i for i, _ in got] == [1, 2, 3]
        assert all(1 <= x <= _CAP[i] for i, x in got), objective
        assert sum(x for _, x in got) == 9, objective

    decidb_cli.assert_error(f"""
        SELECT id, x FROM {_T} DECIDE x(INT)
        SUCH THAT x <= cap AND x >= 1 AND PER (): SUM(x) = 90 SATISFY
    """, match=r"infeasible")


@pytest.mark.explain
def test_explain_renders_satisfy_and_each_stage_on_its_own_line(decidb_cli):
    """§8: EXPLAIN prints `Objective: SATISFY` for a feasibility query, and every
    `THEN` stage on its own line, a first stage that mixes a reducer with a
    `PER ()` decision included."""
    plan = decidb_cli.execute_raw(f"EXPLAIN SELECT id, x FROM {_T} DECIDE x(INT) SUCH THAT x <= cap AND x >= 1 SATISFY")
    assert re.search(r"Objective:\s*SATISFY", plan.stdout), plan.stdout
    plan = decidb_cli.execute_raw(f"""
        EXPLAIN SELECT id, x FROM {_T}
        DECIDE PER (): c(INT) BETWEEN 0 AND 3, x(INT)
        SUCH THAT x <= cap
        MAXIMIZE SUM(x) + 2 * c THEN MINIMIZE SUM(cost * x)
    """)
    lines = [re.sub(r"[│┌┐└┘─┬┴]", "", line).strip() for line in plan.stdout.splitlines()]
    assert "MAXIMIZE SUM(x) + 2 * c" in lines, plan.stdout
    assert "THEN MINIMIZE SUM(cost * x)" in lines, plan.stdout


# ---------------------------------------------------------------------------
# Named refusals
# ---------------------------------------------------------------------------

_BINDER = [pytest.mark.error, pytest.mark.error_binder]

_REFUSALS = [
    pytest.param(
        f"SELECT id, c FROM {_T} DECIDE PER (): c(INT) BETWEEN 0 AND 4 SUCH THAT c <= cap MAXIMIZE SUM(c)",
        r"nothing to aggregate", marks=_BINDER, id="SUM over a PER () decision"),
    pytest.param(
        f"SELECT id, s FROM {_T} DECIDE s(TEXT IN ['a', 'b']) SUCH THAT s = 'a' MAXIMIZE SUM(s)",
        r"no numeric value", marks=_BINDER, id="TEXT decision in an objective"),
    pytest.param(
        f"SELECT id, x FROM {_T} DECIDE x(INT) SUCH THAT x <= cap MAXIMIZE SUM(x) THEN MINIMIZE MAX(x)",
        r"must be linear", marks=_BINDER, id="later stage MIN/MAX"),
    pytest.param(
        f"SELECT id, y FROM {_T} DECIDE PER grp: y(INT) BETWEEN 0 AND 9 SUCH THAT PER t: y <= cap "
        "MAXIMIZE SUM(PER grp: cost * y)",
        r"not determined", marks=_BINDER, id="PER-scoped objective reducer over a column its key does not determine"),
    pytest.param(
        f"SELECT id, x, y FROM {_T} DECIDE x(INT), y(INT) SUCH THAT x <= cap "
        "MAXIMIZE SUM(x) THEN MAXIMIZE SUM(y)",
        r"unbounded", marks=[pytest.mark.error], id="later stage unbounded under the first"),
]


@pytest.mark.parametrize("sql,topic", _REFUSALS)
def test_objective_refusals_are_named(decidb_cli, sql, topic):
    """§4 / §6 / §2.2: each ill-formed objective is refused with a message that
    names the fix: a reducer over a query-wide decision alone, a TEXT decision
    read as a number, a non-linear later stage, a PER-scoped body reading a
    column its key does not determine, a later stage unbounded under the held
    earlier ones. Only a short topic phrase is asserted."""
    decidb_cli.assert_error(sql, match=topic)


@pytest.mark.error
@pytest.mark.quadratic
@pytest.mark.var_real
def test_quadratic_first_stage_then_a_linear_stage_needs_gurobi(decidb_cli_highs):
    """§6 / §9: holding a quadratic first stage for a THEN stage takes a quadratic
    row, which HiGHS does not have; the refusal names the quadratic constraint."""
    decidb_cli_highs.assert_error(f"""
        SELECT id, x FROM {_T} DECIDE x(REAL) BETWEEN 0 AND 10
        SUCH THAT x <= cap
        MINIMIZE SUM(POWER(x - 2, 2)) THEN MAXIMIZE SUM(x)
    """, match=r"quadratic constraint")


# ---------------------------------------------------------------------------
# The outer reducer's WHEN in a nested objective
# ---------------------------------------------------------------------------

# (id, g, pri): group a and b each hold one priority row, group c none.
_PRI = "(VALUES (1, 'a', true), (2, 'a', false), (3, 'b', false), (4, 'b', true), (5, 'c', false)) t(id, g, pri)"
_PRI_ROWS = {1: ("a", True), 2: ("a", False), 3: ("b", False), 4: ("b", True), 5: ("c", False)}


@pytest.mark.var_integer
@pytest.mark.per_clause
@pytest.mark.min_max
@pytest.mark.when_objective
@pytest.mark.obj_minimize
@pytest.mark.correctness
@pytest.mark.parametrize("stage_one", [
    "SUM(WHEN pri PER g: MAX(x) BY (g))",
    "SUM(PER g: MAX(WHEN pri: x) BY (g))",
], ids=["outer-when", "inner-when"])
def test_outer_when_of_a_nested_objective_filters_both_levels(decidb_cli, oracle_solver, stage_one):
    """Deck p29-30 / §6: in `SUM(WHEN pri PER g: MAX(x) BY (g))` each inner group is
    a class of the priority rows, and a key with none (group c) has no term. Stage
    one is then x1 + x4, held at 0, and stage two puts 5 on rows 2, 3 and 5: value
    50. The outer WHEN used to be dropped, so every group's full MAX counted and
    stage one could not reach 0. The filter written inside the inner reducer means
    the same."""
    def build(o):
        for i in _PRI_ROWS:
            o.add_variable(f"x{i}", VarType.INTEGER, lb=0.0, ub=5.0)
        for g in ("a", "b"):
            o.add_variable(f"t{g}", VarType.INTEGER, lb=0.0, ub=5.0)
            for i, (grp, pri) in _PRI_ROWS.items():
                if grp == g and pri:
                    o.add_constraint({f"t{g}": 1.0, f"x{i}": -1.0}, ">=", 0.0)
        o.add_constraint({f"x{i}": 1.0 for i in _PRI_ROWS}, ">=", 6.0)

    stages = [({"ta": 1.0, "tb": 1.0}, ObjSense.MINIMIZE),
              ({f"x{i}": float(i) for i in _PRI_ROWS}, ObjSense.MAXIMIZE)]
    result, optima = _lexi(oracle_solver, build, stages)
    assert optima == pytest.approx([0.0, 50.0])
    got = _rows(decidb_cli, f"""
        SELECT id, x FROM {_PRI} DECIDE x(INT) BETWEEN 0 AND 5
        SUCH THAT PER (): SUM(x) >= 6
        MINIMIZE {stage_one} THEN MAXIMIZE SUM(id * x)
    """, "id", "x")
    assert got == [(1, 0), (2, 5), (3, 5), (4, 0), (5, 5)]
    assert sum(i * x for i, x in got) == pytest.approx(result.objective_value)
