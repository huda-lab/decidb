"""Declarations (DeciQL syntax reference §2, deck p6-p11): scopes, bounds and domains.

`[PER scope:] name(domain) [bounds]` says how many decisions a declarator makes (one
per row, per key value, per relation tuple, or one for the query), where their box
comes from (a constant, a key-determined column, a frame over data) and what values
they take (INT, REAL, BOOL, the SEMI switch domains, a TEXT list).

Every correctness test states the same model independently in gurobipy through
`oracle_solver` and compares the objective; the decision vector is compared where the
optimum is unique. Each docstring names the rule and the wrong answer a plausible bug
would give, so that a test cannot pass on an implementation that ignores the construct.
"""

import pytest

from solver.types import ObjSense, SolverStatus, VarType

_T = "(VALUES (1, 'a', 3), (2, 'a', 7), (3, 'b', 2)) t(id, grp, cap)"
_CAP = {1: 3, 2: 7, 3: 2}
INT, BIN, REAL = VarType.INTEGER, VarType.BINARY, VarType.CONTINUOUS


def _rows(decidb_cli, sql, *cols):
    rows, names = decidb_cli.execute(sql)
    idx = [names.index(c) for c in cols]
    return sorted(tuple(r[i] for i in idx) for r in rows)


def _solve(oracle_solver, name, variables, rows, objective, sense=ObjSense.MAXIMIZE):
    """variables: {name: (VarType, lb, ub)}; rows: [(coeffs, op, rhs)]."""
    oracle_solver.create_model(name)
    for var, (kind, lb, ub) in variables.items():
        oracle_solver.add_variable(var, kind, lb=lb, ub=ub)
    for coeffs, op, rhs in rows:
        oracle_solver.add_constraint(coeffs, op, rhs)
    oracle_solver.set_objective(objective, sense)
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL
    return result


# ---------------------------------------------------------------------------
# Generation scope (§2.1, deck p6-p7)
# ---------------------------------------------------------------------------

@pytest.mark.var_integer
@pytest.mark.correctness
def test_row_scope_is_the_default_and_per_row_spells_it(decidb_cli, oracle_solver):
    """§2.1: `x(INT)` and `PER ROW: x(INT)` make one decision per result row, each
    capped by its own row (objective 12). Reading PER ROW as PER () makes `SUM(x)`
    a refusal (nothing to reduce); reading it as PER grp gives 3, 3, 2 (objective 8)."""
    result = _solve(oracle_solver, "row_scope",
                    {f"x{i}": (INT, 0.0, float(c)) for i, c in _CAP.items()}, [],
                    {f"x{i}": 1.0 for i in _CAP})
    for prefix in ("", "PER ROW: ", "per row: "):
        got = _rows(decidb_cli, f"""
            SELECT id, x FROM {_T} DECIDE {prefix}x(INT) SUCH THAT x <= cap MAXIMIZE SUM(x)
        """, "id", "x")
        assert got == [(1, 3), (2, 7), (3, 2)], prefix
        assert sum(x for _, x in got) == pytest.approx(result.objective_value), prefix


@pytest.mark.var_integer
@pytest.mark.per_clause
@pytest.mark.correctness
def test_query_wide_decision_takes_one_value_for_every_row(decidb_cli, oracle_solver):
    """§2.1: `PER (): x` is one decision for the whole query, so the row-generated
    `x <= cap` caps it by the smallest cap. A per-row x would read 3, 7, 2."""
    got = _rows(decidb_cli, f"""
        SELECT id, x FROM {_T} DECIDE PER (): x(INT) SUCH THAT x <= cap MAXIMIZE x
    """, "id", "x")
    result = _solve(oracle_solver, "query_wide", {"x": (INT, 0.0, None)},
                    [({"x": 1.0}, "<=", float(c)) for c in _CAP.values()], {"x": 1.0})
    assert got == [(1, 2), (2, 2), (3, 2)]
    assert got[0][1] == pytest.approx(result.objective_value)


@pytest.mark.var_integer
@pytest.mark.per_clause
@pytest.mark.correctness
def test_column_key_shares_one_decision_across_its_rows(decidb_cli, oracle_solver):
    """§2.1: `PER grp`, `PER (grp)` and `PER t.grp` are the same key: one decision per
    distinct grp, repeated on its rows, so group a takes min(3, 7). A per-row x would
    give 3, 7, 2 (objective 12 instead of 8)."""
    result = _solve(oracle_solver, "column_key",
                    {"xa": (INT, 0.0, None), "xb": (INT, 0.0, None)},
                    [({"xa": 1.0}, "<=", 3.0), ({"xa": 1.0}, "<=", 7.0), ({"xb": 1.0}, "<=", 2.0)],
                    {"xa": 2.0, "xb": 1.0})
    for key in ("grp", "(grp)", "t.grp"):
        got = _rows(decidb_cli, f"""
            SELECT id, grp, x FROM {_T} DECIDE PER {key}: x(INT)
            SUCH THAT x <= cap MAXIMIZE SUM(x)
        """, "id", "grp", "x")
        assert got == [(1, "a", 3), (2, "a", 3), (3, "b", 2)], key
        assert sum(x for *_, x in got) == pytest.approx(result.objective_value), key


@pytest.mark.var_integer
@pytest.mark.per_clause
@pytest.mark.sql_joins
@pytest.mark.correctness
def test_key_may_combine_columns_of_two_relations(decidb_cli, oracle_solver):
    """Deck p7: `PER D.region, P.pid: stock` keys on columns of two relations; the two
    R1 depots share one stock per product, so their group sum counts it twice and
    d1's weight 3 cannot draw the whole cap onto d1 (objective 30). One decision per
    join row would put the cap on d1 alone for 40; a key on region alone gives 24,
    on pid alone 25."""
    got = _rows(decidb_cli, """
        SELECT depot, pid, w, stock
        FROM (VALUES ('d1', 'R1', 3), ('d2', 'R1', 1), ('d3', 'R2', 1)) D(depot, region, w)
        CROSS JOIN (VALUES ('p1', 4), ('p2', 6)) P(pid, cap)
        DECIDE PER D.region, P.pid: stock(INT)
        SUCH THAT PER D.region, P: SUM(stock) BY (D.region, P.pid) <= cap
        MAXIMIZE SUM(stock * w)
    """, "depot", "pid", "w", "stock")
    result = _solve(oracle_solver, "two_relation_key",
                    {k: (INT, 0.0, None) for k in ("r1p1", "r1p2", "r2p1", "r2p2")},
                    [({"r1p1": 2.0}, "<=", 4.0), ({"r1p2": 2.0}, "<=", 6.0),
                     ({"r2p1": 1.0}, "<=", 4.0), ({"r2p2": 1.0}, "<=", 6.0)],
                    {"r1p1": 4.0, "r1p2": 4.0, "r2p1": 1.0, "r2p2": 1.0})
    assert got == [("d1", "p1", 3, 2), ("d1", "p2", 3, 3), ("d2", "p1", 1, 2), ("d2", "p2", 1, 3),
                   ("d3", "p1", 1, 4), ("d3", "p2", 1, 6)]
    assert sum(w * s for *_, w, s in got) == pytest.approx(result.objective_value)


@pytest.mark.var_integer
@pytest.mark.per_clause
@pytest.mark.sql_joins
@pytest.mark.correctness
def test_relation_key_makes_one_decision_per_tuple_despite_fanout(decidb_cli, oracle_solver):
    """§2.1: `PER t` expands to all of t's columns, so the two join rows of t1 carry one
    x. The per-row sum counts it twice (2 x1 + x2 <= 11) and weight 0 on the second
    row cannot split it: a per-row x would give (5, 0, 6), objective 21 not 16."""
    got = _rows(decidb_cli, """
        SELECT t.id, u.k, x FROM (VALUES (1, 5), (2, 8)) t(id, cap)
        JOIN (VALUES (1, 'm', 3), (1, 'n', 0), (2, 'm', 1)) u(id, k, w) USING (id)
        DECIDE PER t: x(INT) SUCH THAT x <= cap AND SUM(x) <= 11 MAXIMIZE SUM(x * w)
    """, "id", "k", "x")
    result = _solve(oracle_solver, "relation_key",
                    {"x1": (INT, 0.0, 5.0), "x2": (INT, 0.0, 8.0)},
                    [({"x1": 2.0, "x2": 1.0}, "<=", 11.0)], {"x1": 3.0, "x2": 1.0})
    assert got == [(1, "m", 5), (1, "n", 5), (2, "m", 1)]
    assert 3 * got[0][2] + got[2][2] == pytest.approx(result.objective_value)


@pytest.mark.var_integer
@pytest.mark.per_clause
@pytest.mark.sql_joins
@pytest.mark.correctness
def test_relation_and_foreign_column_in_one_key(decidb_cli, oracle_solver):
    """§2.1-2.2: `PER t, u.k` mixes a relation and another relation's column: (1, m)
    and (1, n) are two decisions, the two (2, m) rows share one, and `<= t.cap` is
    legal because t is wholly in the key (`PER u.k` alone is refused). The (1, n) row
    has a negative weight so it stays at 0 while (1, m) fills its cap: objective 26.
    Dropping u.k from the key ties (1, m) to (1, n) and gives (0, 0, 6, 6) for 12; a
    per-row x splits the (2, m) rows as 0 and 7 for 27."""
    got = _rows(decidb_cli, """
        SELECT t.id, u.k, x FROM (VALUES (1, 5), (2, 8)) t(id, cap)
        JOIN (VALUES (1, 'm', 4), (1, 'n', -3), (2, 'm', 1), (2, 'm', 1)) u(id, k, w) USING (id)
        DECIDE PER t, u.k: x(INT) <= t.cap SUCH THAT SUM(x) <= 12 MAXIMIZE SUM(x * w)
    """, "id", "k", "x")
    result = _solve(oracle_solver, "relation_column_key",
                    {"a": (INT, 0.0, 5.0), "b": (INT, 0.0, 5.0), "c": (INT, 0.0, 8.0)},
                    [({"a": 1.0, "b": 1.0, "c": 2.0}, "<=", 12.0)],
                    {"a": 4.0, "b": -3.0, "c": 2.0})
    assert got == [(1, "m", 5), (1, "n", 0), (2, "m", 3), (2, "m", 3)]
    weights = {(1, "m"): 4, (1, "n"): -3, (2, "m"): 1}
    assert sum(weights[i, k] * x for i, k, x in got) == pytest.approx(result.objective_value)


@pytest.mark.var_integer
@pytest.mark.per_clause
@pytest.mark.correctness
def test_relation_keyed_decision_is_reachable_as_relation_dot_name(decidb_cli, oracle_solver):
    """§2.1: `PER D: x` may be written `D.x` when the key is exactly one relation; both
    spellings name the same decision: the cap is written on `x`, the objective on
    `D.x`, and both read back the capped value. If `D.x` named a second decision the
    objective would be unbounded. Under a column key (`PER D.id`) the alias is
    refused, so the spelling depends on the declaration's scope."""
    got = _rows(decidb_cli, """
        SELECT D.id, D.x AS dx, x FROM (VALUES (1, 3), (2, 7)) D(id, cap)
        DECIDE PER D: x(INT) SUCH THAT x <= cap MAXIMIZE SUM(D.x)
    """, "id", "dx", "x")
    result = _solve(oracle_solver, "alias", {"x1": (INT, 0.0, 3.0), "x2": (INT, 0.0, 7.0)},
                    [], {"x1": 1.0, "x2": 1.0})
    assert got == [(1, 3, 3), (2, 7, 7)]
    assert sum(x for *_, x in got) == pytest.approx(result.objective_value)
    decidb_cli.assert_error("""
        SELECT D.id, x FROM (VALUES (1, 3), (2, 7)) D(id, cap)
        DECIDE PER D.id: x(INT) SUCH THAT D.x <= cap MAXIMIZE SUM(x)
    """, match=r"DECIDE variables")


@pytest.mark.var_integer
@pytest.mark.per_clause
@pytest.mark.correctness
def test_two_declarators_naming_the_same_columns_share_one_key(decidb_cli, oracle_solver):
    """§2.1: `PER grp: x` and `PER t.grp: y` name the same columns, so one key holds
    both and `PER t.grp: x + y <= 5` reads them together; each is capped by
    min(3, 7) on group a. y weighs 3, so group a takes (2, 3) on both rows and b
    (2, 2): objective 30. A per-row y is refused under `PER t.grp`, and with the PER
    dropped too it takes (x, y) = (0, 3), (0, 5) on group a for 32; a `PER ()` y is
    capped by 2 everywhere for 26. The constraint's `PER t.grp` alone is equivalent
    to its omission here (both rows of a group state the same row)."""
    got = _rows(decidb_cli, f"""
        SELECT id, grp, x, y FROM {_T} DECIDE PER grp: x(INT), PER t.grp: y(INT)
        SUCH THAT x <= cap AND y <= cap AND PER t.grp: x + y <= 5 MAXIMIZE SUM(x + 3 * y)
    """, "id", "grp", "x", "y")
    result = _solve(oracle_solver, "shared_key",
                    {"xa": (INT, 0.0, 3.0), "ya": (INT, 0.0, 3.0),
                     "xb": (INT, 0.0, 2.0), "yb": (INT, 0.0, 2.0)},
                    [({"xa": 1.0, "ya": 1.0}, "<=", 5.0), ({"xb": 1.0, "yb": 1.0}, "<=", 5.0)],
                    {"xa": 2.0, "ya": 6.0, "xb": 1.0, "yb": 3.0})
    assert got == [(1, "a", 2, 3), (2, "a", 2, 3), (3, "b", 2, 2)]
    assert sum(x + 3 * y for *_, x, y in got) == pytest.approx(result.objective_value)


@pytest.mark.var_integer
@pytest.mark.per_clause
@pytest.mark.correctness
def test_declarators_with_different_keys_in_one_clause(decidb_cli, oracle_solver):
    """§2.1: each declarator carries its own scope: `PER grp: x(INT), y(INT), PER (): z`
    leaves y per row (3, 7, 2). Letting the PER prefix cover y too would give y = 3, 3, 2
    and objective 22 instead of 26."""
    got = _rows(decidb_cli, f"""
        SELECT id, grp, x, y, z FROM {_T} DECIDE PER grp: x(INT), y(INT), PER (): z(INT)
        SUCH THAT x <= cap AND y <= cap AND z <= cap AND PER (): z >= 1
        MAXIMIZE SUM(x + y + z)
    """, "id", "grp", "x", "y", "z")
    variables = {"xa": (INT, 0.0, 3.0), "xb": (INT, 0.0, 2.0), "z": (INT, 1.0, 2.0)}
    variables.update({f"y{i}": (INT, 0.0, float(c)) for i, c in _CAP.items()})
    result = _solve(oracle_solver, "mixed_keys", variables, [],
                    {"xa": 2.0, "xb": 1.0, "z": 3.0, "y1": 1.0, "y2": 1.0, "y3": 1.0})
    assert got == [(1, "a", 3, 3, 2), (2, "a", 3, 7, 2), (3, "b", 2, 2, 2)]
    assert sum(x + y + z for *_, x, y, z in got) == pytest.approx(result.objective_value)


@pytest.mark.var_integer
@pytest.mark.per_clause
@pytest.mark.edge_case
@pytest.mark.correctness
def test_null_is_a_key_value_shared_by_the_null_rows(decidb_cli, oracle_solver):
    """§2.1 (GROUP BY semantics): the two NULL-keyed rows share one decision, so it is
    capped by min(7, 2). Giving each NULL row its own decision would read 7 and 2."""
    got = _rows(decidb_cli, """
        SELECT id, grp, x FROM (VALUES (1, 'a', 3), (2, NULL, 7), (3, NULL, 2)) t(id, grp, cap)
        DECIDE PER grp: x(INT) SUCH THAT x <= cap MAXIMIZE SUM(x)
    """, "id", "grp", "x")
    result = _solve(oracle_solver, "null_key", {"xa": (INT, 0.0, 3.0), "xn": (INT, 0.0, None)},
                    [({"xn": 1.0}, "<=", 7.0), ({"xn": 1.0}, "<=", 2.0)], {"xa": 1.0, "xn": 2.0})
    assert got == [(1, "a", 3), (2, None, 2), (3, None, 2)]
    assert sum(x for *_, x in got) == pytest.approx(result.objective_value)


# ---------------------------------------------------------------------------
# Charging a keyed decision once or per row (§4, deck p16-p18)
# ---------------------------------------------------------------------------

@pytest.mark.var_integer
@pytest.mark.per_clause
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_reducer_per_key_charges_a_keyed_decision_once(decidb_cli, oracle_solver):
    """§4: `SUM(PER grp: x)` counts x_a once, `SUM(x * c)` counts it on both of its
    rows (weights 1 + 2 = 3 for group a, 2 for b). In the constraint: x_a + x_b <= 5
    under MAXIMIZE 3 x_a + 2 x_b puts 5 on a (15); dropping the reducer's PER gives
    2 x_a + x_b <= 5 and moves the budget to b (0, 0, 5). In the objective: 3 x_a +
    2 x_b <= 6 under MAXIMIZE x_a + x_b puts 3 on b; dropping the PER maximises
    2 x_a + x_b and answers (2, 2, 0)."""
    data = "(VALUES (1, 'a', 1), (2, 'a', 2), (3, 'b', 2)) t(id, grp, c)"
    once = _rows(decidb_cli, f"""
        SELECT id, x FROM {data} DECIDE PER grp: x(INT)
        SUCH THAT PER (): SUM(PER grp: x) <= 5 MAXIMIZE SUM(x * c)
    """, "id", "x")
    in_objective = _rows(decidb_cli, f"""
        SELECT id, x FROM {data} DECIDE PER grp: x(INT)
        SUCH THAT PER (): SUM(x * c) <= 6 MAXIMIZE SUM(PER grp: x)
    """, "id", "x")
    boxes = {"xa": (INT, 0.0, None), "xb": (INT, 0.0, None)}
    r_once = _solve(oracle_solver, "charge_once_constraint", boxes,
                    [({"xa": 1.0, "xb": 1.0}, "<=", 5.0)], {"xa": 3.0, "xb": 2.0})
    assert once == [(1, 5), (2, 5), (3, 0)]
    assert 3 * once[0][1] + 2 * once[2][1] == pytest.approx(r_once.objective_value)
    r_obj = _solve(oracle_solver, "charge_once_objective", boxes,
                   [({"xa": 3.0, "xb": 2.0}, "<=", 6.0)], {"xa": 1.0, "xb": 1.0})
    assert in_objective == [(1, 0), (2, 0), (3, 3)]
    assert in_objective[0][1] + in_objective[2][1] == pytest.approx(r_obj.objective_value)


@pytest.mark.var_boolean
@pytest.mark.var_integer
@pytest.mark.per_clause
@pytest.mark.sql_joins
@pytest.mark.correctness
def test_opening_cost_charged_once_per_depot_versus_per_row(decidb_cli, oracle_solver):
    """§4 (the depot example): `SUM(PER d: cost * open)` charges a depot once, `SUM(cost
    * open)` on every route row. Depot 2 has two routes, so the two spellings open
    different depots (cost 7 once vs 10 per row); charging both ways alike would
    open the same depot twice. The `PER d` scope of open is pinned by the first
    query: a per-row open makes `SUM(PER d: cost * open)` a refusal (the second
    query alone would not notice, a per-row open gives the same answer there)."""
    setup = """
        CREATE TEMP TABLE d(id INT PRIMARY KEY, cost INT); INSERT INTO d VALUES (1, 10), (2, 7);
        CREATE TEMP TABLE r(rid INT, did INT, cap INT); INSERT INTO r VALUES (1, 1, 6), (2, 2, 3), (3, 2, 3);
    """
    query = """
        SELECT rid, d.id AS did, open, x FROM r JOIN d ON r.did = d.id
        DECIDE PER d: open(BOOL), x(INT) SUCH THAT x <= cap * open AND PER (): SUM(x) >= 6
        MINIMIZE {objective}
    """
    once = _rows(decidb_cli, setup + query.format(objective="SUM(PER d: cost * open)"),
                 "rid", "did", "open", "x")
    per_row = _rows(decidb_cli, setup + query.format(objective="SUM(cost * open)"),
                    "rid", "did", "open", "x")
    variables = {"o1": (BIN, 0.0, None), "o2": (BIN, 0.0, None),
                 "x1": (INT, 0.0, None), "x2": (INT, 0.0, None), "x3": (INT, 0.0, None)}
    rows = [({"x1": 1.0, "o1": -6.0}, "<=", 0.0), ({"x2": 1.0, "o2": -3.0}, "<=", 0.0),
            ({"x3": 1.0, "o2": -3.0}, "<=", 0.0), ({"x1": 1.0, "x2": 1.0, "x3": 1.0}, ">=", 6.0)]
    cost = {1: 10, 2: 7}
    r_once = _solve(oracle_solver, "open_once", variables, rows, {"o1": 10.0, "o2": 7.0},
                    ObjSense.MINIMIZE)
    assert once == [(1, 1, 0, 0), (2, 2, 1, 3), (3, 2, 1, 3)]
    assert sum(cost[d] for d in {d for _, d, o, _ in once if o}) == pytest.approx(r_once.objective_value)
    r_row = _solve(oracle_solver, "open_per_row", variables, rows, {"o1": 10.0, "o2": 14.0},
                   ObjSense.MINIMIZE)
    assert per_row == [(1, 1, 1, 6), (2, 2, 0, 0), (3, 2, 0, 0)]
    assert sum(cost[d] * o for _, d, o, _ in per_row) == pytest.approx(r_row.objective_value)


@pytest.mark.var_boolean
@pytest.mark.var_integer
@pytest.mark.per_clause
@pytest.mark.when
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_filter_generate_guard_over_keyed_decisions(decidb_cli, oracle_solver):
    """§3 (filter -> generate -> guard, deck p58) over a keyed BOOL: `WHEN cap > 2 PER grp
    IF open: SUM(x) BY (grp) <= 4` reads open once per group and sums only the rows
    the WHEN kept. Group a keeps closed (bonus 2 < loss 6), b opens (bonus 8 > loss 1)
    for 24. Ignoring IF gives 20; ignoring WHEN counts row 3 in b's sum and gives 22;
    dropping BY (grp) sums both groups under one guard and gives 17. The `PER grp`
    is equivalent to its omission here (a per-row instance repeats its group's sum
    and its group's open), so the test discriminates on WHEN, IF and BY."""
    got = _rows(decidb_cli, """
        SELECT id, grp, open, x
        FROM (VALUES (1, 'a', 3, 2), (2, 'a', 7, 2), (3, 'b', 2, 8), (4, 'b', 5, 8)) t(id, grp, cap, bonus)
        DECIDE PER grp: open(BOOL), x(INT) <= cap
        SUCH THAT WHEN cap > 2 PER grp IF open: SUM(x) BY (grp) <= 4
        MAXIMIZE SUM(x) + SUM(PER grp, bonus: bonus * open)
    """, "id", "grp", "open", "x")
    oracle_solver.create_model("filter_generate_guard")
    caps = {1: 3, 2: 7, 3: 2, 4: 5}
    for g in ("a", "b"):
        oracle_solver.add_variable(f"o{g}", BIN)
    for i, c in caps.items():
        oracle_solver.add_variable(f"x{i}", INT, lb=0.0, ub=float(c))
    oracle_solver.add_indicator_constraint("oa", 1, {"x1": 1.0, "x2": 1.0}, "<=", 4.0)
    oracle_solver.add_indicator_constraint("ob", 1, {"x4": 1.0}, "<=", 4.0)
    oracle_solver.set_objective({"x1": 1, "x2": 1, "x3": 1, "x4": 1, "oa": 2.0, "ob": 8.0},
                                ObjSense.MAXIMIZE)
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL
    assert got == [(1, "a", 0, 3), (2, "a", 0, 7), (3, "b", 1, 2), (4, "b", 1, 4)]
    value = sum(x for *_, x in got) + 2 * got[0][2] + 8 * got[2][2]
    assert value == pytest.approx(result.objective_value)


# ---------------------------------------------------------------------------
# Declaration bounds (§2.2, deck p9)
# ---------------------------------------------------------------------------

@pytest.mark.var_integer
@pytest.mark.cons_between
@pytest.mark.correctness
def test_constant_between_bound_binds_on_both_sides(decidb_cli, oracle_solver):
    """§2.2: `x(INT) BETWEEN 1 AND 4` is the box [1, 4] on every row: the ceiling binds
    row 2 (cap 7 -> 4) and the floor binds every row when minimising. Ignoring the
    bound would give 7 on row 2 and 0 everywhere in the minimum."""
    sql = f"SELECT id, x FROM {_T} DECIDE x(INT) BETWEEN 1 AND 4 SUCH THAT x <= cap {{obj}} SUM(x)"
    boxes = {f"x{i}": (INT, 1.0, min(4.0, float(c))) for i, c in _CAP.items()}
    obj = {v: 1.0 for v in boxes}
    high = _rows(decidb_cli, sql.format(obj="MAXIMIZE"), "id", "x")
    assert high == [(1, 3), (2, 4), (3, 2)]
    assert sum(x for _, x in high) == pytest.approx(
        _solve(oracle_solver, "between_max", boxes, [], obj).objective_value)
    low = _rows(decidb_cli, sql.format(obj="MINIMIZE"), "id", "x")
    assert low == [(1, 1), (2, 1), (3, 1)]
    assert sum(x for _, x in low) == pytest.approx(
        _solve(oracle_solver, "between_min", boxes, [], obj, ObjSense.MINIMIZE).objective_value)


@pytest.mark.var_integer
@pytest.mark.cons_perrow
@pytest.mark.correctness
def test_one_sided_bounds(decidb_cli, oracle_solver):
    """§2.2: `x(INT) <= 2` caps rows whose cap is larger; `x(INT) >= 1` lifts the
    default floor. Without the bound the maximum would be 3 + 7 + 1 and the minimum 0."""
    high = _rows(decidb_cli, """
        SELECT id, x FROM (VALUES (1, 3), (2, 7), (3, 1)) t(id, cap)
        DECIDE x(INT) <= 2 SUCH THAT x <= cap MAXIMIZE SUM(x)
    """, "id", "x")
    assert high == [(1, 2), (2, 2), (3, 1)]
    r_high = _solve(oracle_solver, "upper_only",
                    {f"x{i}": (INT, 0.0, min(2.0, float(c))) for i, c in {1: 3, 2: 7, 3: 1}.items()},
                    [], {"x1": 1.0, "x2": 1.0, "x3": 1.0})
    assert sum(x for _, x in high) == pytest.approx(r_high.objective_value)
    low = _rows(decidb_cli, f"""
        SELECT id, x FROM {_T} DECIDE x(INT) >= 1 SUCH THAT x <= cap MINIMIZE SUM(x)
    """, "id", "x")
    assert low == [(1, 1), (2, 1), (3, 1)]
    r_low = _solve(oracle_solver, "lower_only",
                   {f"x{i}": (INT, 1.0, float(c)) for i, c in _CAP.items()}, [],
                   {"x1": 1.0, "x2": 1.0, "x3": 1.0}, ObjSense.MINIMIZE)
    assert sum(x for _, x in low) == pytest.approx(r_low.objective_value)


@pytest.mark.var_integer
@pytest.mark.correctness
def test_negative_constant_floor_widens_the_box(decidb_cli, oracle_solver):
    """§2.2: the default floor 0 becomes negative only through an explicit negative
    constant: `x >= -5`, `BETWEEN -4 AND 4` and a keyed `PER grp: x >= -2` all reach
    their floor when minimised. Keeping the floor at 0 would give 0 everywhere. (The
    keyed case pins the floor on a keyed declarator; its key is not what it checks,
    a per-row x with the same floor would read the same.)"""
    per_row = {f"x{i}": (1.0, float(c)) for i, c in _CAP.items()}  # var -> (weight, ceiling)
    keyed = {"xa": (2.0, 3.0), "xb": (1.0, 2.0)}  # group a is capped by min(3, 7)
    cases = (("x(INT) >= -5", -5, None, per_row), ("x(INT) BETWEEN -4 AND 4", -4, 4.0, per_row),
             ("PER grp: x(INT) >= -2", -2, None, keyed))
    for decl, floor, ceiling, variables in cases:
        got = _rows(decidb_cli, f"""
            SELECT id, x FROM {_T} DECIDE {decl} SUCH THAT x <= cap MINIMIZE SUM(x)
        """, "id", "x")
        assert got == [(1, floor), (2, floor), (3, floor)], decl
        boxes = {v: (INT, float(floor), cap if ceiling is None else min(cap, ceiling))
                 for v, (_, cap) in variables.items()}
        result = _solve(oracle_solver, f"floor_{floor}", boxes, [],
                        {v: w for v, (w, _) in variables.items()}, ObjSense.MINIMIZE)
        assert sum(x for _, x in got) == pytest.approx(result.objective_value), decl


@pytest.mark.error
@pytest.mark.error_infeasible
def test_upper_bound_below_the_default_floor_is_infeasible(decidb_cli):
    """§2.2: INT is >= 0 unless bounded below, so `x(INT) <= -1` has an empty box."""
    decidb_cli.assert_error(f"""
        SELECT id, x FROM {_T} DECIDE x(INT) <= -1 SUCH THAT x <= cap MAXIMIZE SUM(x)
    """, match=r"infeasible")


@pytest.mark.var_integer
@pytest.mark.per_clause
@pytest.mark.correctness
def test_column_bound_determined_by_the_key(decidb_cli, oracle_solver):
    """§2.2, §3.1: a column bound needs the key to determine it: `PER grp, cap`, `PER t`
    (the whole relation) and `PER id` over a PRIMARY KEY table all read cap as one
    value per decision. Without the bound the maximum is unbounded."""
    queries = [
        f"SELECT id, x FROM {_T} DECIDE PER grp, cap: x(INT) BETWEEN 0 AND cap SUCH THAT x >= 1 MAXIMIZE SUM(x)",
        f"SELECT id, x FROM {_T} DECIDE PER t: x(INT) <= cap SUCH THAT x >= 1 MAXIMIZE SUM(x)",
        """CREATE TEMP TABLE d(id INT PRIMARY KEY, grp VARCHAR, cap INT);
           INSERT INTO d VALUES (1, 'a', 3), (2, 'a', 7), (3, 'b', 2);
           SELECT id, x FROM d DECIDE PER id: x(INT) <= cap SUCH THAT x >= 1 MAXIMIZE SUM(x)""",
    ]
    result = _solve(oracle_solver, "column_bound",
                    {f"x{i}": (INT, 1.0, float(c)) for i, c in _CAP.items()}, [],
                    {"x1": 1.0, "x2": 1.0, "x3": 1.0})
    for sql in queries:
        got = _rows(decidb_cli, sql, "id", "x")
        assert got == [(1, 3), (2, 7), (3, 2)], sql
        assert sum(x for _, x in got) == pytest.approx(result.objective_value), sql


@pytest.mark.error
@pytest.mark.error_binder
@pytest.mark.per_clause
def test_column_bound_not_determined_by_the_key_is_refused(decidb_cli):
    """§3.1: `PER grp: x(INT) <= cap` reads a column grp does not determine, and a
    `PER ()` decision cannot read a column that varies across rows."""
    decidb_cli.assert_error(f"""
        SELECT id, x FROM {_T} DECIDE PER grp: x(INT) <= cap SUCH THAT x >= 1 MAXIMIZE SUM(x)
    """, match=r"not determined")
    decidb_cli.assert_error(f"""
        SELECT id, x FROM {_T} DECIDE PER (): x(INT) <= cap SUCH THAT x >= 1 MAXIMIZE x
    """, match=r"varies across rows")


@pytest.mark.error
@pytest.mark.error_binder
def test_bound_over_a_decision_or_reducer_is_refused(decidb_cli):
    """Deck p9: a declaration bound is a constant or a column; a decision (declared
    before or after) or a reducer belongs in SUCH THAT."""
    decidb_cli.assert_error(f"""
        SELECT id, x, y FROM {_T} DECIDE x(INT) <= y, y(INT) <= 3 SUCH THAT x >= 1 MAXIMIZE SUM(x)
    """, match=r"declaration bound")
    decidb_cli.assert_error(f"""
        SELECT id, x FROM {_T} DECIDE x(INT) <= SUM(cap) SUCH THAT x >= 1 MAXIMIZE SUM(x)
    """, match=r"declaration bound")


@pytest.mark.error
@pytest.mark.error_binder
def test_inverted_between_is_refused(decidb_cli):
    """§2.2: an empty constant range is a bind error, for INT and SEMIINT alike."""
    for domain in ("INT", "SEMIINT"):
        decidb_cli.assert_error(f"""
            SELECT id, x FROM {_T} DECIDE x({domain}) BETWEEN 5 AND 2 SUCH THAT x <= cap MAXIMIZE SUM(x)
        """, match=r"empty")


@pytest.mark.var_integer
@pytest.mark.per_clause
@pytest.mark.correctness
def test_bounds_on_a_query_wide_decision(decidb_cli, oracle_solver):
    """§2.2: `PER (): x(INT) BETWEEN 1 AND 4` boxes the single decision; with every cap
    above 4 the ceiling, not the data, binds the maximum (4) and the floor the minimum
    (1). Ignoring the bound would give 5 and 0."""
    rows = "(VALUES (1, 5), (2, 7), (3, 6)) t(id, cap)"
    sql = f"SELECT id, x FROM {rows} DECIDE PER (): x(INT) BETWEEN 1 AND 4 SUCH THAT x <= cap {{obj}} x"
    box = {"x": (INT, 1.0, 4.0)}
    caps = [({"x": 1.0}, "<=", c) for c in (5.0, 7.0, 6.0)]
    high = _rows(decidb_cli, sql.format(obj="MAXIMIZE"), "id", "x")
    assert high == [(1, 4), (2, 4), (3, 4)]
    assert high[0][1] == pytest.approx(_solve(oracle_solver, "wide_max", box, caps, {"x": 1.0}).objective_value)
    low = _rows(decidb_cli, sql.format(obj="MINIMIZE"), "id", "x")
    assert low == [(1, 1), (2, 1), (3, 1)]
    assert low[0][1] == pytest.approx(_solve(oracle_solver, "wide_min", box, caps, {"x": 1.0},
                                             ObjSense.MINIMIZE).objective_value)


@pytest.mark.var_integer
@pytest.mark.correctness
def test_frame_over_data_as_a_bound(decidb_cli, oracle_solver):
    """§2.2, §5: `x(INT) <= AT(PREVIOUS ELSE 9: cap) OVER (t WITHIN p)` bounds each row
    by the previous period's cap of its own product (9, 5, 9, 8: objective 31). The
    periods are distinct across products so every mutant binds: reading the row's
    own cap gives 16; dropping WITHIN bounds B3 by A2's cap (2) for 24; DESC reads
    the next period for 21; dropping ELSE leaves the first periods unbounded."""
    got = _rows(decidb_cli, """
        SELECT p, t, x FROM (VALUES ('A', 1, 5), ('A', 2, 2), ('B', 3, 8), ('B', 4, 1)) r(p, t, cap)
        DECIDE x(INT) <= AT(PREVIOUS ELSE 9: cap) OVER (t WITHIN p) SUCH THAT x >= 0 MAXIMIZE SUM(x)
    """, "p", "t", "x")
    # Oracle: each row's ceiling is the cap of the row before it in its own product, else 9.
    data = [("A", 1, 5), ("A", 2, 2), ("B", 3, 8), ("B", 4, 1)]
    boxes = {}
    for p, t, _ in data:
        earlier = [(tt, c) for pp, tt, c in data if pp == p and tt < t]
        boxes[f"{p}{t}"] = (INT, 0.0, float(max(earlier)[1]) if earlier else 9.0)
    result = _solve(oracle_solver, "frame_bound", boxes, [], {v: 1.0 for v in boxes})
    assert got == [("A", 1, 9), ("A", 2, 5), ("B", 3, 9), ("B", 4, 8)]
    assert sum(x for *_, x in got) == pytest.approx(result.objective_value)


# ---------------------------------------------------------------------------
# Domains (§2.2, deck p11)
# ---------------------------------------------------------------------------

@pytest.mark.var_boolean
@pytest.mark.correctness
def test_bool_stays_binary_under_a_negative_or_wide_bound(decidb_cli, oracle_solver):
    """§2.2: a bound beside a BOOL restates {0, 1}; `o(BOOL) BETWEEN -3 AND 3` and a
    `o >= -1` row change nothing. A box widened to [-1, 1] would pick (1, -1, 1) at a
    cost of -2, and to [-3, 3] (1, -3, 3) at -12, instead of opening the cheapest row
    for 2."""
    result = _solve(oracle_solver, "bool_box", {f"o{i}": (BIN, 0.0, None) for i in _CAP},
                    [({"o1": 1.0, "o2": 1.0, "o3": 1.0}, ">=", 1.0)],
                    {f"o{i}": float(c) for i, c in _CAP.items()}, ObjSense.MINIMIZE)
    for decl, row in (("o(BOOL) BETWEEN -3 AND 3", ""), ("o(BOOL) >= -1", "o >= -1 AND ")):
        got = _rows(decidb_cli, f"""
            SELECT id, o FROM {_T} DECIDE {decl} SUCH THAT {row}SUM(o) >= 1 MINIMIZE SUM(cap * o)
        """, "id", "o")
        assert got == [(1, 0), (2, 0), (3, 1)], decl
        assert sum(_CAP[i] * o for i, o in got) == pytest.approx(result.objective_value), decl


@pytest.mark.var_integer
@pytest.mark.per_clause
@pytest.mark.correctness
def test_semiint_keyed_range_is_off_or_within_range(decidb_cli, oracle_solver):
    """§2.2: SEMIINT is 0 or a whole number in [lo, hi], keyed per group through a hidden
    switch. Covering a demand of 4 costs 5 (group a's floor); a plain box [0, hi]
    would answer 4 and a box [lo, hi] could not switch b off and would answer 11. A
    per-row SEMIINT reaches the same objective with (5, 0, 0), so the shared value on
    group a's two rows is what pins the key."""
    got = _rows(decidb_cli, """
        SELECT id, grp, s FROM (VALUES (1, 'a', 5, 9), (2, 'a', 5, 9), (3, 'b', 6, 8)) t(id, grp, lo, hi)
        DECIDE PER grp, lo, hi: s(SEMIINT) BETWEEN lo AND hi
        SUCH THAT PER (): SUM(PER grp, lo, hi: s) >= 4 MINIMIZE SUM(PER grp, lo, hi: s)
    """, "id", "grp", "s")
    variables = {"sa": (INT, 0.0, None), "sb": (INT, 0.0, None), "ona": (BIN, 0.0, None), "onb": (BIN, 0.0, None)}
    rows = [({"sa": 1.0, "ona": -9.0}, "<=", 0.0), ({"sa": 1.0, "ona": -5.0}, ">=", 0.0),
            ({"sb": 1.0, "onb": -8.0}, "<=", 0.0), ({"sb": 1.0, "onb": -6.0}, ">=", 0.0),
            ({"sa": 1.0, "sb": 1.0}, ">=", 4.0)]
    result = _solve(oracle_solver, "semiint", variables, rows, {"sa": 1.0, "sb": 1.0}, ObjSense.MINIMIZE)
    assert got == [(1, "a", 5), (2, "a", 5), (3, "b", 0)]
    assert got[0][2] + got[2][2] == pytest.approx(result.objective_value)


@pytest.mark.var_real
@pytest.mark.per_clause
@pytest.mark.correctness
def test_semireal_generator_dispatch(decidb_cli, oracle_solver):
    """Deck p11: `PER g: p(SEMIREAL) BETWEEN lo AND hi` over a PRIMARY KEY table: each
    generator is off or inside its stable range. Meeting 7.5 costs 12.5 (g1 at its
    floor, g2 full, g3 off); a box [0, hi] would answer 10.5, [lo, hi] 13.5, and an
    integer switch domain (SEMIINT) 14. `PER g` over a single table is one decision
    per row, so it is equivalent to its omission here; the domain is what is pinned."""
    got = _rows(decidb_cli, """
        CREATE TEMP TABLE g(id INT PRIMARY KEY, lo DOUBLE, hi DOUBLE, cost DOUBLE);
        INSERT INTO g VALUES (1, 2.5, 6, 3), (2, 4, 5, 1), (3, 1, 2, 2);
        SELECT id, p FROM g DECIDE PER g: p(SEMIREAL) BETWEEN lo AND hi
        SUCH THAT SUM(p) >= 7.5 MINIMIZE SUM(p * cost)
    """, "id", "p")
    data = {1: (2.5, 6.0, 3.0), 2: (4.0, 5.0, 1.0), 3: (1.0, 2.0, 2.0)}
    variables, rows = {}, [({f"p{i}": 1.0 for i in data}, ">=", 7.5)]
    for i, (lo, hi, _) in data.items():
        variables[f"p{i}"] = (REAL, 0.0, None)
        variables[f"on{i}"] = (BIN, 0.0, None)
        rows += [({f"p{i}": 1.0, f"on{i}": -hi}, "<=", 0.0), ({f"p{i}": 1.0, f"on{i}": -lo}, ">=", 0.0)]
    result = _solve(oracle_solver, "semireal", variables, rows,
                    {f"p{i}": c for i, (_, _, c) in data.items()}, ObjSense.MINIMIZE)
    assert got == [(1, pytest.approx(2.5)), (2, pytest.approx(5.0)), (3, pytest.approx(0.0))]
    assert sum(p * data[i][2] for i, p in got) == pytest.approx(result.objective_value)


@pytest.mark.error
@pytest.mark.error_binder
def test_semi_domain_needs_both_bounds(decidb_cli):
    """§2.2: SEMIINT / SEMIREAL require `BETWEEN lo AND hi`; one bound is refused."""
    decidb_cli.assert_error(f"""
        SELECT id, s FROM {_T} DECIDE s(SEMIINT) <= 9 SUCH THAT s >= 0 MAXIMIZE SUM(s)
    """, match=r"both bounds")


@pytest.mark.var_integer
@pytest.mark.per_clause
@pytest.mark.correctness
def test_keyed_text_decision_reads_back_one_value_per_key(decidb_cli, oracle_solver):
    """§2.2, deck p58: `PER grp: mode(TEXT IN ['lo', 'hi'])` is one string per group,
    read by every row's `IF mode = ...` guard. Group a's rows disagree (row 1 earns 5
    under 'hi', row 2 earns 4 under 'lo'), so one shared mode picks 'hi' for 5 + 1 and b
    picks 'lo' for 3: objective 9. A mode per row would take 5 + 4 + 3 = 12."""
    caps = {1: (5, 1), 2: (1, 4), 3: (2, 3)}  # id -> (cap under 'hi', cap under 'lo')
    got = _rows(decidb_cli.with_verify_serializer(), """
        SELECT id, grp, mode, x FROM (VALUES (1, 'a', 5, 1), (2, 'a', 1, 4), (3, 'b', 2, 3)) t(id, grp, h, l)
        DECIDE PER grp: mode(TEXT IN ['lo', 'hi']), x(INT) <= 10
        SUCH THAT IF mode = 'hi': x <= h AND IF mode = 'lo': x <= l
        MAXIMIZE SUM(x)
    """, "id", "grp", "mode", "x")
    oracle_solver.create_model("keyed_text")
    for g in ("a", "b"):
        oracle_solver.add_variable(f"hi{g}", BIN)
        oracle_solver.add_variable(f"lo{g}", BIN)
        oracle_solver.add_constraint({f"hi{g}": 1.0, f"lo{g}": 1.0}, "=", 1.0)
    for i, (h, l) in caps.items():
        g = "a" if i < 3 else "b"
        oracle_solver.add_variable(f"x{i}", INT, lb=0.0, ub=10.0)
        oracle_solver.add_indicator_constraint(f"hi{g}", 1, {f"x{i}": 1.0}, "<=", float(h))
        oracle_solver.add_indicator_constraint(f"lo{g}", 1, {f"x{i}": 1.0}, "<=", float(l))
    oracle_solver.set_objective({f"x{i}": 1.0 for i in caps}, ObjSense.MAXIMIZE)
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL
    assert got == [(1, "a", "hi", 5), (2, "a", "hi", 1), (3, "b", "lo", 3)]
    assert sum(x for *_, x in got) == pytest.approx(result.objective_value)


@pytest.mark.edge_case
@pytest.mark.correctness
def test_text_values_are_case_sensitive(decidb_cli, oracle_solver):
    """§2.2: `['a', 'A']` lists two values; `mode <> 'a'` leaves exactly 'A' and
    `mode <> 'A'` exactly 'a'. The oracle is the one-hot pair with the excluded
    indicator pinned to 0; a case-folding list would refuse the declaration or read
    back the same spelling for both."""
    for excluded, kept in (("a", "A"), ("A", "a")):
        got = _rows(decidb_cli, f"""
            SELECT id, mode FROM {_T} DECIDE mode(TEXT IN ['a', 'A']) SUCH THAT mode <> '{excluded}' SATISFY
        """, "id", "mode")
        result = _solve(oracle_solver, f"case_sensitive_text_{kept}",
                        {"is_a": (BIN, 0.0, None), "is_A": (BIN, 0.0, None)},
                        [({"is_a": 1.0, "is_A": 1.0}, "=", 1.0), ({f"is_{excluded}": 1.0}, "<=", 0.0)],
                        {"is_a": 1.0, "is_A": 1.0})
        chosen = [v for v in ("a", "A") if result.variable_values[f"is_{v}"] > 0.5]
        assert chosen == [kept]
        assert got == [(1, kept), (2, kept), (3, kept)]


@pytest.mark.error
@pytest.mark.error_binder
def test_duplicate_text_value_is_refused(decidb_cli):
    """§2.2: an exactly repeated TEXT value is an error."""
    decidb_cli.assert_error(f"""
        SELECT id, mode FROM {_T} DECIDE mode(TEXT IN ['a', 'b', 'a']) SUCH THAT mode = 'a' SATISFY
    """, match=r"twice")


@pytest.mark.error
@pytest.mark.error_binder
def test_text_value_outside_the_list_is_refused(decidb_cli):
    """§2.2: comparing a TEXT decision with a value outside its list, by `=` or in an
    `IN` list, is a binder error naming the list."""
    for body in ("mode = 'c'", "mode IN ('a', 'zz')"):
        decidb_cli.assert_error(f"""
            SELECT id, mode FROM {_T} DECIDE mode(TEXT IN ['a', 'b']) SUCH THAT {body} SATISFY
        """, match=r"no value")


# ---------------------------------------------------------------------------
# Parser refusals (§2.1)
# ---------------------------------------------------------------------------

@pytest.mark.error
@pytest.mark.error_parser
def test_trailing_key_spelling_is_refused(decidb_cli):
    """§2.1: the deck's `x(INT) BETWEEN 0 AND cap PER grp` puts the key after the name;
    the parser names the prefix form."""
    decidb_cli.assert_error(f"""
        SELECT id, x FROM {_T} DECIDE x(INT) BETWEEN 0 AND cap PER grp SUCH THAT x >= 0 MAXIMIZE SUM(x)
    """, match=r"before its name")


@pytest.mark.error
@pytest.mark.error_parser
def test_bound_on_a_text_declarator_is_refused(decidb_cli):
    """§2.1: a TEXT decision's domain is its list, so it takes no bound."""
    decidb_cli.assert_error(f"""
        SELECT id, mode FROM {_T} DECIDE mode(TEXT IN ['a', 'b']) <= 5 SUCH THAT mode = 'a' SATISFY
    """, match=r"no BETWEEN")


@pytest.mark.error
@pytest.mark.error_parser
def test_wrong_domain_word_is_refused(decidb_cli):
    """§2.1: a DuckDB type name is not a DECIDE domain; the error lists the six."""
    for word in ("INTEGER", "BOOLEAN", "DOUBLE"):
        decidb_cli.assert_error(f"""
            SELECT id, x FROM {_T} DECIDE x({word}) SUCH THAT x <= cap MAXIMIZE SUM(x)
        """, match=r"not a DECIDE domain")


@pytest.mark.error
@pytest.mark.error_binder
@pytest.mark.per_clause
def test_key_naming_a_decision_is_refused(decidb_cli):
    """§2.1, §4: a key names columns or relations, never a decision, in a declarator
    and in a constraint PER alike."""
    decidb_cli.assert_error(f"""
        SELECT id, x, y FROM {_T} DECIDE y(INT) <= 3, PER y: x(INT) SUCH THAT x <= cap MAXIMIZE SUM(x)
    """, match=r"is a decision")
    decidb_cli.assert_error(f"""
        SELECT id, x FROM {_T} DECIDE open(BOOL), x(INT)
        SUCH THAT PER open: SUM(x) BY (open) <= 5 MAXIMIZE SUM(x)
    """, match=r"is a decision")


# ---------------------------------------------------------------------------
# Hidden switches (§2.2, §8)
# ---------------------------------------------------------------------------

@pytest.mark.var_integer
@pytest.mark.edge_case
@pytest.mark.correctness
def test_select_star_hides_hidden_switches(decidb_cli, oracle_solver):
    """§2.2: the SEMI switch and the TEXT indicators never appear in `SELECT *`, and
    their internal names (`__semi_on_s__`, `__text_mode_0__`) cannot be selected; s = 3
    is the smallest on-range value once 0 is excluded by `s >= 1` (a box [0, 9] would
    answer 1 per row)."""
    rows, names = decidb_cli.execute(f"""
        SELECT * FROM {_T} DECIDE s(SEMIINT) BETWEEN 3 AND 9, mode(TEXT IN ['a', 'b'])
        SUCH THAT s >= 1 AND mode = 'b' MINIMIZE SUM(s)
    """)
    assert names == ["id", "grp", "cap", "s", "mode"]
    assert sorted(rows) == [(1, "a", 3, 3, "b"), (2, "a", 7, 3, "b"), (3, "b", 2, 3, "b")]
    variables, switch_rows = {}, []
    for i in _CAP:
        variables[f"s{i}"] = (INT, 1.0, None)
        variables[f"on{i}"] = (BIN, 0.0, None)
        switch_rows += [({f"s{i}": 1.0, f"on{i}": -9.0}, "<=", 0.0),
                        ({f"s{i}": 1.0, f"on{i}": -3.0}, ">=", 0.0)]
    result = _solve(oracle_solver, "hidden_switch", variables, switch_rows,
                    {f"s{i}": 1.0 for i in _CAP}, ObjSense.MINIMIZE)
    assert sum(r[3] for r in rows) == pytest.approx(result.objective_value)
    decidb_cli.assert_error(f"""
        SELECT id, __semi_on_s__ FROM {_T} DECIDE s(SEMIINT) BETWEEN 3 AND 9 SUCH THAT s >= 1 MINIMIZE SUM(s)
    """, match=r"not found")
    decidb_cli.assert_error(f"""
        SELECT id, __text_mode_0__ FROM {_T} DECIDE mode(TEXT IN ['a', 'b']) SUCH THAT mode = 'b' SATISFY
    """, match=r"not found")


@pytest.mark.explain
def test_explain_lists_each_decision_with_its_scope(decidb_cli):
    """§8: the DECIDE node lists every declared decision with its generation scope (a
    bare name for one per row) and no hidden switch or indicator among them."""
    result = decidb_cli.execute_raw(f"""
        EXPLAIN SELECT id, x FROM {_T}
        DECIDE PER grp: x(INT), s(SEMIINT) BETWEEN 3 AND 9, PER (): c(INT), mode(TEXT IN ['a', 'b']), PER t: r(BOOL)
        SUCH THAT x <= cap AND s >= 1 AND c <= 4 AND mode = 'b' AND r <= 1 MAXIMIZE SUM(x)
    """)
    text = result.stdout + result.stderr
    compact = "".join(ch for ch in text if ch not in "│┌┐└┘─┬┴ \n")
    start, end = compact.index("Variables:"), compact.index("Objective:")
    assert compact[start:end] == "Variables:xPERgrpscPER()moderPERt", text
