"""Easy-direction MIN/MAX under PER, BY, WHEN and IF (syntax_reference §4).

`MAX(e) <= K` and `MIN(e) >= K` are stated per row: every instance bounds every row
its reducer reads. So a row's bound is the tightest K among the instances whose
reducer reads it -- the instances of its own `BY` group, or of the whole input for
`BY ()` -- and the reducer's own `WHEN` picks the rows that are bounded, never the
instances that bound them. Before 2026-09-30 the rewrite dropped the `BY` group (every
row got the tightest K of the whole input), let the reducer's `WHEN` replace the
clause's, and split `MAX(e) = K` into an easy half that forgot the clause's `WHEN`.

Every oracle states the rule itself: one row `x_s <= K_i` for every instance i and
every row s its reducer reads, with no reduction done by hand.
"""

import pytest

from solver.types import ObjSense, SolverStatus, VarType

# id -> (grp, cap, w, lim): lim is a function of grp; cap varies inside each group.
_T = "(VALUES (1, 'a', 3, 1, 3), (2, 'a', 7, 2, 3), (3, 'b', 2, 1, 4), (4, 'b', 5, 3, 4)) t(id, grp, cap, w, lim)"
_ROWS = {1: ("a", 3, 1, 3), 2: ("a", 7, 2, 3), 3: ("b", 2, 1, 4), 4: ("b", 5, 3, 4)}
_GRP, _CAP, _W, _LIM = ({i: r[k] for i, r in _ROWS.items()} for k in range(4))


def _rows(decidb_cli, sql, *cols):
    rows, names = decidb_cli.execute(sql)
    idx = [names.index(c) for c in cols]
    return sorted(tuple(r[i] for i in idx) for r in rows)


def _solve(oracle, name, rows, sense=ObjSense.MAXIMIZE, lb=None):
    """x_s in [lb_s, cap_s] (integer), `rows` a list of (coeffs, sense, rhs); objective
    SUM(x * w) in `sense`."""
    oracle.create_model(name)
    for i in _ROWS:
        oracle.add_variable(f"x{i}", VarType.INTEGER, lb=float((lb or {}).get(i, 0)), ub=float(_CAP[i]))
    for coeffs, op, rhs in rows:
        oracle.add_constraint(coeffs, op, float(rhs))
    oracle.set_objective({f"x{i}": float(_W[i]) for i in _ROWS}, sense)
    result = oracle.solve()
    assert result.status == SolverStatus.OPTIMAL
    return result


def _easy_rows(instances, reads, bound, op="<=", scale=1.0):
    """The easy rule, stated literally: instance i bounds every row its reducer reads."""
    return [({f"x{s}": scale}, op, bound(i)) for i in instances for s in reads(i)]


def _value(got):
    return sum(_W[i] * x for i, x in got)


def _query(body, objective="MAXIMIZE SUM(x * w)"):
    return f"SELECT id, x FROM {_T} DECIDE x(INT) SUCH THAT x <= cap AND {body} {objective}"


# ---------------------------------------------------------------------------
# The bound is the tightest one among the instances of the row's BY group
# ---------------------------------------------------------------------------

@pytest.mark.var_integer
@pytest.mark.per_clause
@pytest.mark.min_max
@pytest.mark.correctness
def test_keyed_max_takes_its_own_groups_bound(decidb_cli, oracle_solver):
    """§4: `PER lim: MAX(x) BY (lim) <= lim` bounds the lim-3 rows by 3 and the lim-4
    rows by 4: x = 3, 3, 2, 4 (value 23). Taking the tightest bound of the whole input
    gives x4 <= 3 (value 20)."""
    instances = sorted(set(_LIM.values()))
    rows = _easy_rows(instances, lambda l: [s for s in _ROWS if _LIM[s] == l], lambda l: l)
    result = _solve(oracle_solver, "keyed_max", rows)
    got = _rows(decidb_cli, _query("PER lim: MAX(x) BY (lim) <= lim"), "id", "x")
    assert got == [(1, 3), (2, 3), (3, 2), (4, 4)]
    assert _value(got) == pytest.approx(result.objective_value) == 23


@pytest.mark.var_integer
@pytest.mark.per_clause
@pytest.mark.min_max
@pytest.mark.correctness
@pytest.mark.parametrize("body", [
    "PER grp, cap: MAX(x) BY (grp) <= cap",
    "MAX(x) BY (grp) <= cap",
    "2 * MAX(x) BY (grp) <= 2 * cap",
], ids=["finer-key", "row-instances", "scaled"])
def test_bound_varying_inside_the_group_takes_the_groups_tightest(decidb_cli, oracle_solver, body):
    """§4 / §3.1: when the bound varies inside the BY group, every instance of the group
    bounds all of its rows, so group a takes min(3, 7) = 3 and group b min(2, 5) = 2:
    x = 3, 3, 2, 2 (value 17). Each instance bounding only its own row gives 34; the
    tightest bound of the whole input gives 14. The factor on the reducer scales both
    sides alike."""
    rows = _easy_rows(list(_ROWS), lambda r: [s for s in _ROWS if _GRP[s] == _GRP[r]], lambda r: _CAP[r])
    result = _solve(oracle_solver, "group_tightest", rows)
    got = _rows(decidb_cli, _query(body), "id", "x")
    assert got == [(1, 3), (2, 3), (3, 2), (4, 2)]
    assert _value(got) == pytest.approx(result.objective_value) == 17


@pytest.mark.var_integer
@pytest.mark.per_clause
@pytest.mark.min_max
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_min_mirror_takes_the_groups_largest_lower_bound(decidb_cli, oracle_solver):
    """§4: `MIN(x) BY (grp) >= lim - 2` under MINIMIZE lifts group a to 1 and group b
    to 2: x = 1, 1, 2, 2 (value 11). The largest lower bound of the whole input would
    lift every row to 2 (value 14)."""
    rows = _easy_rows(list(_ROWS), lambda r: [s for s in _ROWS if _GRP[s] == _GRP[r]], lambda r: _LIM[r] - 2, ">=")
    result = _solve(oracle_solver, "min_mirror", rows, ObjSense.MINIMIZE)
    got = _rows(decidb_cli, _query("PER grp, lim: MIN(x) BY (grp) >= lim - 2", "MINIMIZE SUM(x * w)"), "id", "x")
    assert got == [(1, 1), (2, 1), (3, 2), (4, 2)]
    assert _value(got) == pytest.approx(result.objective_value) == 11


@pytest.mark.var_integer
@pytest.mark.per_clause
@pytest.mark.min_max
@pytest.mark.correctness
def test_null_by_key_is_one_group(decidb_cli, oracle_solver):
    """§4: NULL is a BY group of its own. With grp NULL on rows 3 and 4, `MAX(x) BY
    (grp) <= cap` bounds both by min(2, 5) = 2: x = 3, 3, 2, 2 (value 17). Treating
    each NULL row as unkeyed would leave x4 = 5 (value 26)."""
    sql = f"""
        SELECT id, x FROM (VALUES (1, 'a', 3, 1), (2, 'a', 7, 2), (3, NULL, 2, 1), (4, NULL, 5, 3)) t(id, grp, cap, w)
        DECIDE x(INT) SUCH THAT x <= cap AND MAX(x) BY (grp) <= cap MAXIMIZE SUM(x * w)
    """
    grp = {1: "a", 2: "a", 3: None, 4: None}
    rows = _easy_rows(list(_ROWS), lambda r: [s for s in _ROWS if grp[s] == grp[r]], lambda r: _CAP[r])
    result = _solve(oracle_solver, "null_by", rows)
    got = _rows(decidb_cli, sql, "id", "x")
    assert got == [(1, 3), (2, 3), (3, 2), (4, 2)]
    assert _value(got) == pytest.approx(result.objective_value) == 17


# ---------------------------------------------------------------------------
# WHEN: the clause's picks the instances, the reducer's picks the bounded rows
# ---------------------------------------------------------------------------

@pytest.mark.var_integer
@pytest.mark.when_constraint
@pytest.mark.min_max
@pytest.mark.correctness
def test_reducer_when_does_not_narrow_the_instances(decidb_cli, oracle_solver):
    """§4: `MAX(WHEN grp = 'a': x) <= cap` has one instance per row, each bounding the
    a-rows by its own cap, so the a-rows take min over every cap, 2: x = 2, 2, 2, 5
    (value 23). Letting the reducer's WHEN also pick the instances bounds them by
    min(3, 7) = 3 (value 26)."""
    rows = _easy_rows(list(_ROWS), lambda r: [s for s in _ROWS if _GRP[s] == "a"], lambda r: _CAP[r])
    result = _solve(oracle_solver, "reducer_when", rows)
    got = _rows(decidb_cli, _query("MAX(WHEN grp = 'a': x) <= cap"), "id", "x")
    assert got == [(1, 2), (2, 2), (3, 2), (4, 5)]
    assert _value(got) == pytest.approx(result.objective_value) == 23


@pytest.mark.var_integer
@pytest.mark.when_constraint
@pytest.mark.per_clause
@pytest.mark.min_max
@pytest.mark.correctness
def test_clause_when_survives_a_reducer_when(decidb_cli, oracle_solver):
    """§3 / §4: in `WHEN id <= 2 PER grp: MAX(WHEN cap > 2: x) BY (grp) <= 1` only
    group a has an instance and its reducer reads rows 1 and 2, so x = 1, 1, 2, 5
    (value 20). When the reducer's WHEN replaced the clause's, row 4 (cap 5) was
    bounded too (value 8)."""
    admitted = [s for s in _ROWS if s <= 2]
    instances = sorted({_GRP[s] for s in admitted})
    rows = _easy_rows(instances, lambda g: [s for s in admitted if _GRP[s] == g and _CAP[s] > 2], lambda g: 1)
    result = _solve(oracle_solver, "clause_when", rows)
    got = _rows(decidb_cli, _query("WHEN id <= 2 PER grp: MAX(WHEN cap > 2: x) BY (grp) <= 1"), "id", "x")
    assert got == [(1, 1), (2, 1), (3, 2), (4, 5)]
    assert _value(got) == pytest.approx(result.objective_value) == 20


@pytest.mark.var_integer
@pytest.mark.when_constraint
@pytest.mark.min_max
@pytest.mark.correctness
def test_a_reducer_that_reads_no_row_bounds_nothing(decidb_cli, oracle_solver):
    """§4: `MAX(WHEN cap > 50: x) BY (grp) <= 0` reads no row, so no instance is
    imposed and every x reaches its cap (value 34). Imposing it as 0 would pin all
    four rows to 0."""
    result = _solve(oracle_solver, "empty_reducer", [])
    got = _rows(decidb_cli, _query("MAX(WHEN cap > 50: x) BY (grp) <= 0"), "id", "x")
    assert got == [(1, 3), (2, 7), (3, 2), (4, 5)]
    assert _value(got) == pytest.approx(result.objective_value) == 34


# ---------------------------------------------------------------------------
# A reducer in the bound
# ---------------------------------------------------------------------------

def _min_cap(g):
    return min(_CAP[s] for s in _ROWS if _GRP[s] == g)


_REDUCER_BOUNDS = [
    # (body, instance -> rows its reducer reads, instance -> bound or None, vector)
    ("MAX(x) <= MIN(cap)",
     lambda r: list(_ROWS), lambda r: min(_CAP.values()), [2, 2, 2, 2]),
    ("MAX(x) BY (grp) <= MIN(cap) BY (grp)",
     lambda r: [s for s in _ROWS if _GRP[s] == _GRP[r]], lambda r: _min_cap(_GRP[r]), [3, 3, 2, 2]),
    ("PER grp: MAX(x) BY (grp) <= MIN(cap) BY (grp)",
     lambda r: [s for s in _ROWS if _GRP[s] == _GRP[r]], lambda r: _min_cap(_GRP[r]), [3, 3, 2, 2]),
    # every row is an instance bounding the a-rows by its own group's smallest cap
    ("MAX(WHEN grp = 'a': x) <= MIN(cap) BY (grp)",
     lambda r: [s for s in _ROWS if _GRP[s] == "a"], lambda r: _min_cap(_GRP[r]), [2, 2, 2, 5]),
    # group b has no cap above 5: its instances have no bound and bound nothing
    ("MAX(x) BY (grp) <= SUM(WHEN cap > 5: cap) BY (grp) - 5",
     lambda r: [s for s in _ROWS if _GRP[s] == _GRP[r]],
     lambda r: 7 - 5 if _GRP[r] == "a" else None, [2, 2, 2, 5]),
    # ... but under BY () group a's instances still bound group b's rows
    ("MAX(x) <= SUM(WHEN cap > 5: cap) BY (grp) - 5",
     lambda r: list(_ROWS), lambda r: 7 - 5 if _GRP[r] == "a" else None, [2, 2, 2, 2]),
]


@pytest.mark.var_integer
@pytest.mark.min_max
@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize("body,reads,bound,vector", _REDUCER_BOUNDS,
                         ids=["global", "by-group", "keyed", "reducer-when", "empty-group", "empty-group-global"])
def test_max_against_a_reducer_in_the_bound(decidb_cli, oracle_solver, body, reads, bound, vector):
    """§4: an easy MAX may be compared with a data reducer. Each instance's bound is
    its own reducer value and bounds every row its MAX reads, so a row takes the
    tightest bound among those instances. A group whose bound reducer reads no row
    has no bound: it bounds nothing, but under `BY ()` the other group's instances
    still reach its rows. These used to be refused as reading rows outside the
    instance; a per-row reading would give each row only its own group's bound."""
    # The easy rule, stated literally: every instance with a bound bounds every row it reads.
    rows = [({f"x{s}": 1.0}, "<=", bound(r)) for r in _ROWS if bound(r) is not None for s in reads(r)]
    result = _solve(oracle_solver, "reducer_bound", rows)
    got = _rows(decidb_cli, _query(body), "id", "x")
    assert [x for _, x in got] == vector
    assert _value(got) == pytest.approx(result.objective_value)


@pytest.mark.var_integer
@pytest.mark.min_max
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_min_against_a_reducer_in_the_bound(decidb_cli, oracle_solver):
    """§4: `MIN(x) BY (grp) >= MIN(cap) BY (grp) - 1` under MINIMIZE lifts group a to
    3 - 1 = 2 and group b to 2 - 1 = 1: x = 2, 2, 1, 1 (value 10). Lifting by the
    whole input's tightest bound would put every row at 2 (value 14)."""
    rows = _easy_rows(list(_ROWS), lambda r: [s for s in _ROWS if _GRP[s] == _GRP[r]],
                      lambda r: _min_cap(_GRP[r]) - 1, ">=")
    result = _solve(oracle_solver, "min_reducer_bound", rows, ObjSense.MINIMIZE)
    got = _rows(decidb_cli, _query("MIN(x) BY (grp) >= MIN(cap) BY (grp) - 1", "MINIMIZE SUM(x * w)"), "id", "x")
    assert got == [(1, 2), (2, 2), (3, 1), (4, 1)]
    assert _value(got) == pytest.approx(result.objective_value) == 10


# ---------------------------------------------------------------------------
# `MAX(e) = K`: the easy half keeps the clause's WHEN and the group
# ---------------------------------------------------------------------------

def _max_equals(oracle, name, groups, bound):
    """MAX over each group equals its bound: every row <= bound, some row >= bound."""
    oracle.create_model(name)
    for i in _ROWS:
        oracle.add_variable(f"x{i}", VarType.INTEGER, lb=0.0, ub=float(_CAP[i]))
    for g, members in groups.items():
        picks = []
        for s in members:
            oracle.add_constraint({f"x{s}": 1.0}, "<=", float(bound(g)))
            oracle.add_variable(f"z{g}_{s}", VarType.BINARY)
            oracle.add_indicator_constraint(f"z{g}_{s}", 1, {f"x{s}": 1.0}, ">=", float(bound(g)))
            picks.append(f"z{g}_{s}")
        oracle.add_constraint({z: 1.0 for z in picks}, ">=", 1.0)
    oracle.set_objective({f"x{i}": float(_W[i]) for i in _ROWS}, ObjSense.MAXIMIZE)
    result = oracle.solve()
    assert result.status == SolverStatus.OPTIMAL
    return result


@pytest.mark.var_integer
@pytest.mark.when_constraint
@pytest.mark.min_max
@pytest.mark.correctness
def test_max_equals_keeps_the_clause_when_on_both_halves(decidb_cli, oracle_solver):
    """§4: `WHEN id <= 2: MAX(x) = 2` holds rows 1 and 2 at a maximum of 2 and leaves
    rows 3 and 4 free: x = 2, 2, 2, 5 (value 23). The split used to bound rows 3 and 4
    as well (value 14)."""
    result = _max_equals(oracle_solver, "max_eq_when", {"c": [1, 2]}, lambda g: 2)
    got = _rows(decidb_cli, _query("WHEN id <= 2: MAX(x) = 2"), "id", "x")
    assert got == [(1, 2), (2, 2), (3, 2), (4, 5)]
    assert _value(got) == pytest.approx(result.objective_value) == 23


@pytest.mark.var_integer
@pytest.mark.per_clause
@pytest.mark.min_max
@pytest.mark.correctness
def test_max_equals_a_group_varying_bound(decidb_cli, oracle_solver):
    """§4: `PER lim: MAX(x) BY (lim) = lim` pins group lim 3 at a maximum of 3 and
    group lim 4 at 4 (x4 = 4, since x3 <= 2): x = 3, 3, 2, 4 (value 23). The easy half
    used to take bound 3 for every row, which made the lim-4 group infeasible."""
    groups = {l: [s for s in _ROWS if _LIM[s] == l] for l in sorted(set(_LIM.values()))}
    result = _max_equals(oracle_solver, "max_eq_group", groups, lambda l: l)
    got = _rows(decidb_cli, _query("PER lim: MAX(x) BY (lim) = lim"), "id", "x")
    assert got == [(1, 3), (2, 3), (3, 2), (4, 4)]
    assert _value(got) == pytest.approx(result.objective_value) == 23


# ---------------------------------------------------------------------------
# The hard direction is untouched; IF needs the instance to be the group
# ---------------------------------------------------------------------------

@pytest.mark.var_integer
@pytest.mark.per_clause
@pytest.mark.min_max
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_hard_direction_with_a_group_varying_bound(decidb_cli, oracle_solver):
    """§4: `MAX(x) BY (grp) >= lim` under MINIMIZE asks some row of each group to reach
    its bound: x1 = 3 in group a, x4 = 4 in group b (x3 <= 2), value 15. Reading group
    a's bound for group b would ask x4 for only 3 (value 12)."""
    oracle_solver.create_model("hard")
    for i in _ROWS:
        oracle_solver.add_variable(f"x{i}", VarType.INTEGER, lb=0.0, ub=float(_CAP[i]))
    for g in ("a", "b"):
        members = [s for s in _ROWS if _GRP[s] == g]
        bound = _LIM[members[0]]
        for s in members:
            oracle_solver.add_variable(f"z{s}", VarType.BINARY)
            oracle_solver.add_indicator_constraint(f"z{s}", 1, {f"x{s}": 1.0}, ">=", float(bound))
        oracle_solver.add_constraint({f"z{s}": 1.0 for s in members}, ">=", 1.0)
    oracle_solver.set_objective({f"x{i}": float(_W[i]) for i in _ROWS}, ObjSense.MINIMIZE)
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL
    got = _rows(decidb_cli, _query("PER grp, lim: MAX(x) BY (grp) >= lim", "MINIMIZE SUM(x * w)"), "id", "x")
    assert got == [(1, 3), (2, 0), (3, 0), (4, 4)]
    assert _value(got) == pytest.approx(result.objective_value) == 15


@pytest.mark.var_integer
@pytest.mark.var_boolean
@pytest.mark.per_clause
@pytest.mark.min_max
@pytest.mark.correctness
def test_guard_on_a_max_whose_group_is_the_instance(decidb_cli, oracle_solver):
    """§3.2 / §4: `PER grp IF o: MAX(x) BY (grp) <= 1` switches the bound of each whole
    group with that group's own `o`. One group must open; opening b costs x3, x4 <= 1
    (value 21), opening a costs x1, x2 <= 1 (value 20), so b opens. Ignoring the guard
    bounds both groups (value 8)."""
    oracle_solver.create_model("guarded")
    for i in _ROWS:
        oracle_solver.add_variable(f"x{i}", VarType.INTEGER, lb=0.0, ub=float(_CAP[i]))
    for g in ("a", "b"):
        oracle_solver.add_variable(f"o{g}", VarType.BINARY)
        for s in _ROWS:
            if _GRP[s] == g:
                oracle_solver.add_indicator_constraint(f"o{g}", 1, {f"x{s}": 1.0}, "<=", 1.0)
    oracle_solver.add_constraint({"oa": 1.0, "ob": 1.0}, ">=", 1.0)
    oracle_solver.set_objective({f"x{i}": float(_W[i]) for i in _ROWS}, ObjSense.MAXIMIZE)
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL
    rows, names = decidb_cli.execute(f"""
        SELECT id, x, o FROM {_T} DECIDE PER grp: o(BOOL), x(INT)
        SUCH THAT x <= cap AND PER grp IF o: MAX(x) BY (grp) <= 1 AND PER (): SUM(PER grp: o) >= 1
        MAXIMIZE SUM(x * w)
    """)
    got = sorted((r[names.index("id")], r[names.index("x")], r[names.index("o")]) for r in rows)
    assert [(i, x) for i, x, _ in got] == [(1, 3), (2, 7), (3, 1), (4, 1)]
    assert [bool(o) for *_, o in got] == [False, False, True, True]
    assert _value([(i, x) for i, x, _ in got]) == pytest.approx(result.objective_value) == 21


@pytest.mark.error
@pytest.mark.min_max
@pytest.mark.parametrize("body", [
    "IF o: MAX(x) <= 1",
    "PER grp IF o: MAX(x) <= 1",
    "PER grp IF o: MIN(x) BY () >= 1",
], ids=["row-instances", "global-reducer", "min-global"])
def test_guard_whose_instance_is_not_the_group_is_refused(decidb_cli, body):
    """§3.2 / delta list: an IF guard on a MIN/MAX body is a formulation limit, except
    when the instance is the reducer's own group. `IF o: MAX(x) <= 1` would need one
    row's guard to switch the bound on every other row, so it is refused by name
    rather than read per row (it used to bound only the guarded row)."""
    decidb_cli.assert_error(f"""
        SELECT id, x FROM {_T} DECIDE PER grp: o(BOOL), x(INT)
        SUCH THAT x <= cap AND {body} MAXIMIZE SUM(x * w)
    """, match=r"cannot be guarded")
