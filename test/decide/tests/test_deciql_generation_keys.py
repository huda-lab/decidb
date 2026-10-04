"""Generation keys: the functional-dependency rule, key refinement, NULL keys and the
explicit spellings of the default key.

DeciQL spec (syntax_reference §2.1, §3.1; deck p26-31, p59): every value an instance
reads directly must be one value per instance -- a column in the key, a column of a
relation wholly in the key, a column of a base table whose PRIMARY KEY / UNIQUE lies
in the key, a decision whose own key is determined the same way, a query-wide
decision, a constant, or a reducer whose BY key is determined. Anything else is a
bind-time refusal naming the offender. Every correctness test states the same model
independently in gurobipy through the oracle fixture and compares the objective of
the returned rows with it; the vector literal beside it is a readability aid.
"""

import pytest

from solver.types import ObjSense, SolverStatus, VarType

from ._oracle_helpers import add_ne_indicator

# (id, grp, cap): group a holds two rows with different caps, group b one row.
_T = "(VALUES (1, 'a', 3), (2, 'a', 7), (3, 'b', 2)) t(id, grp, cap)"
_T_ROWS = {1: ("a", 3), 2: ("a", 7), 3: ("b", 2)}

# (id, grp): two NULL-keyed rows beside two rows of group a.
_N = "(VALUES (1, 'a'), (2, NULL), (3, 'a'), (4, NULL)) t(id, grp)"
_N_ROWS = {1: "a", 2: None, 3: "a", 4: None}

# Two base tables: dep carries a schema key on id, sh joins to it. sid -> (dep, dem).
_DEP_SH = ("CREATE TEMP TABLE dep(id INT {key}, cap INT); INSERT INTO dep VALUES (1, 5), (2, 4); "
           "CREATE TEMP TABLE sh(sid INT {sh_key}, dep INT, dem INT); "
           "INSERT INTO sh VALUES (1, 1, 3), (2, 1, 4), (3, 2, 6); ")
_SH = {1: (1, 3), 2: (1, 4), 3: (2, 6)}
_DEP_CAP = {1: 5, 2: 4}

# Composite PRIMARY KEY (a, b) on dep2, sh2 joins on both: sid 1, 2 -> (1, 1) cap 5; sid 3 -> (1, 2) cap 4.
_DEP2_SH2 = ("CREATE TEMP TABLE dep2(a INT, b INT, cap INT, PRIMARY KEY (a, b)); "
             "INSERT INTO dep2 VALUES (1, 1, 5), (1, 2, 4); "
             "CREATE TEMP TABLE sh2(sid INT, a INT, b INT, dem INT); "
             "INSERT INTO sh2 VALUES (1, 1, 1, 3), (2, 1, 1, 4), (3, 1, 2, 6); "
             "SELECT sid, x FROM sh2 JOIN dep2 ON sh2.a = dep2.a AND sh2.b = dep2.b ")

_INT = (VarType.INTEGER, 0.0, None)   # an unbounded INT decision is >= 0 (§2.2)
_X10 = (VarType.INTEGER, 0.0, 10.0)
_X5 = (VarType.INTEGER, 0.0, 5.0)
_BIN = (VarType.BINARY, 0.0, 1.0)


def _rows(decidb_cli, sql, *cols):
    rows, names = decidb_cli.execute(sql)
    idx = [names.index(c) for c in cols]
    return sorted(tuple(r[i] for i in idx) for r in rows)


def _solve(oracle, name, variables, rows, objective, sense=ObjSense.MAXIMIZE):
    """Build and solve the independent model: variables {name: (type, lb, ub)},
    rows [(coeffs, op, rhs)], a linear objective."""
    oracle.create_model(name)
    for var, (kind, lb, ub) in variables.items():
        oracle.add_variable(var, kind, lb=lb, ub=ub)
    for coeffs, op, rhs in rows:
        oracle.add_constraint(coeffs, op, rhs)
    oracle.set_objective(objective, sense)
    result = oracle.solve()
    assert result.status == SolverStatus.OPTIMAL
    return result


# --- The functional-dependency rule: every admissible way a value is determined ---

@pytest.mark.per_clause
@pytest.mark.var_integer
@pytest.mark.correctness
def test_refining_key_determines_its_column_and_a_coarser_decision(decidb_cli, oracle_solver):
    """§3.1: `PER grp, cap` determines `cap` (in the key) and the `PER grp` decision
    (its key is a subset). Group a carries caps 7, 3, 7, so its three instances hold
    y_a to 3 (objective 3 * 3 + 2 = 11); reading one cap per group, the first or the
    last, would give y_a = 7 and 23."""
    got = _rows(decidb_cli, """
        SELECT id, y FROM (VALUES (1, 'a', 7), (2, 'a', 3), (3, 'a', 7), (4, 'b', 2)) t(id, grp, cap)
        DECIDE PER grp: y(INT) SUCH THAT PER grp, cap: y <= cap MAXIMIZE SUM(y)
    """, "id", "y")
    result = _solve(oracle_solver, "refining_key", {"y_a": _INT, "y_b": _INT},
                    [({"y_a": 1.0}, "<=", 7.0), ({"y_a": 1.0}, "<=", 3.0), ({"y_b": 1.0}, "<=", 2.0)],
                    {"y_a": 3.0, "y_b": 1.0})
    assert got == [(1, 3), (2, 3), (3, 3), (4, 2)]
    assert sum(y for _, y in got) == pytest.approx(result.objective_value)


@pytest.mark.per_clause
@pytest.mark.sql_joins
@pytest.mark.correctness
def test_relation_wholly_in_the_key_determines_its_columns(decidb_cli, oracle_solver):
    """§2.1/§3.1: `PER t` expands to every column of t, so `t.cap` is one value per
    instance although the join fans t's rows out. Reading the sum globally instead of
    `BY (t)` would hold every x to 5 in total (x_2m = 5, objective 15 instead of 34)."""
    got = _rows(decidb_cli, """
        SELECT t.id, u.k, x FROM (VALUES (1, 5), (2, 8)) t(id, cap)
        JOIN (VALUES (1, 'm', 1), (1, 'n', 2), (2, 'm', 3)) u(id, k, w) USING (id)
        DECIDE x(INT) BETWEEN 0 AND 10
        SUCH THAT PER t: SUM(x) BY (t) <= t.cap MAXIMIZE SUM(w * x)
    """, "id", "k", "x")
    weights = {(1, "m"): 1, (1, "n"): 2, (2, "m"): 3}
    result = _solve(oracle_solver, "relation_key", {"x_1m": _X10, "x_1n": _X10, "x_2m": _X10},
                    [({"x_1m": 1.0, "x_1n": 1.0}, "<=", 5.0), ({"x_2m": 1.0}, "<=", 8.0)],
                    {"x_1m": 1.0, "x_1n": 2.0, "x_2m": 3.0})
    assert got == [(1, "m", 0), (1, "n", 5), (2, "m", 8)]
    assert sum(weights[i, k] * x for i, k, x in got) == pytest.approx(result.objective_value)


@pytest.mark.per_clause
@pytest.mark.sql_joins
@pytest.mark.correctness
@pytest.mark.parametrize("schema_key", ["PRIMARY KEY", "UNIQUE"])
def test_table_key_in_the_key_determines_the_tables_columns(decidb_cli, oracle_solver, schema_key):
    """§3.1: a base table's PRIMARY KEY or UNIQUE column in the key determines its
    other columns, so `PER dep.id` may read `dep.cap`. The same key on an unkeyed
    relation is refused (see the refusal tests); reading the sum globally instead of
    `BY (dep.id)` would hold every x to 4 in total (x_3 = 4, objective 12 not 21)."""
    got = _rows(decidb_cli, _DEP_SH.format(key=schema_key, sh_key="") + """
        SELECT sid, x FROM sh JOIN dep ON sh.dep = dep.id
        DECIDE x(INT) BETWEEN 0 AND dem
        SUCH THAT PER dep.id: SUM(x) BY (dep.id) <= dep.cap MAXIMIZE SUM(sid * x)
    """, "sid", "x")
    result = _solve(oracle_solver, f"table_key_{schema_key}",
                    {f"x_{s}": (VarType.INTEGER, 0.0, float(dem)) for s, (_, dem) in _SH.items()},
                    [({f"x_{s}": 1.0 for s, (d, _) in _SH.items() if d == dep}, "<=", float(cap))
                     for dep, cap in _DEP_CAP.items()],
                    {f"x_{s}": float(s) for s in _SH})
    assert got == [(1, 1), (2, 4), (3, 4)]
    assert sum(s * x for s, x in got) == pytest.approx(result.objective_value)


@pytest.mark.per_clause
@pytest.mark.sql_joins
@pytest.mark.correctness
def test_composite_table_key_wholly_in_the_key_determines_the_columns(decidb_cli, oracle_solver):
    """§3.1: a composite PRIMARY KEY (a, b) admits the table's columns when both of
    its columns are in the key. (1,1) has cap 5 over demands 3 and 4; (1,2) cap 4;
    a global sum would be held to 4 (objective 12 instead of 21)."""
    got = _rows(decidb_cli, _DEP2_SH2 + """
        DECIDE x(INT) BETWEEN 0 AND dem
        SUCH THAT PER dep2.a, dep2.b: SUM(x) BY (dep2.a, dep2.b) <= dep2.cap
        MAXIMIZE SUM(sid * x)
    """, "sid", "x")
    result = _solve(oracle_solver, "composite_key",
                    {"x_1": (VarType.INTEGER, 0.0, 3.0), "x_2": (VarType.INTEGER, 0.0, 4.0),
                     "x_3": (VarType.INTEGER, 0.0, 6.0)},
                    [({"x_1": 1.0, "x_2": 1.0}, "<=", 5.0), ({"x_3": 1.0}, "<=", 4.0)],
                    {"x_1": 1.0, "x_2": 2.0, "x_3": 3.0})
    assert got == [(1, 1), (2, 4), (3, 4)]
    assert sum(s * x for s, x in got) == pytest.approx(result.objective_value)


@pytest.mark.per_clause
@pytest.mark.var_integer
@pytest.mark.correctness
def test_keyed_decision_under_its_own_key(decidb_cli, oracle_solver):
    """§3.1: a `PER grp` decision is one value per `PER grp` instance. The group
    quotas share a budget of 8 counted once per group (`SUM(PER grp: quota)`); a
    per-row count (2 q_a + q_b <= 8) would give objective 20 instead of 22."""
    got = _rows(decidb_cli, """
        SELECT id, x, quota FROM (VALUES (1, 'a', 1), (2, 'a', 2), (3, 'b', 3)) t(id, grp, w)
        DECIDE x(INT) BETWEEN 0 AND 10, PER grp: quota(INT) BETWEEN 0 AND 6
        SUCH THAT PER grp: SUM(x) BY (grp) <= quota AND PER (): SUM(PER grp: quota) <= 8
        MAXIMIZE SUM(w * x)
    """, "id", "x", "quota")
    result = _solve(oracle_solver, "own_key",
                    {"x_1": _X10, "x_2": _X10, "x_3": _X10,
                     "q_a": (VarType.INTEGER, 0.0, 6.0), "q_b": (VarType.INTEGER, 0.0, 6.0)},
                    [({"x_1": 1.0, "x_2": 1.0, "q_a": -1.0}, "<=", 0.0),
                     ({"x_3": 1.0, "q_b": -1.0}, "<=", 0.0),
                     ({"q_a": 1.0, "q_b": 1.0}, "<=", 8.0)],
                    {"x_1": 1.0, "x_2": 2.0, "x_3": 3.0})
    assert got == [(1, 0, 2), (2, 2, 2), (3, 6, 6)]
    assert sum(i * x for i, x, _ in got) == pytest.approx(result.objective_value)  # w == id


@pytest.mark.per_clause
@pytest.mark.var_boolean
@pytest.mark.sql_joins
@pytest.mark.correctness
def test_keyed_decision_under_a_key_covering_its_relations_table_key(decidb_cli, oracle_solver):
    """§3.1 (delta 6): a `PER D` decision is determined by `PER D.depotID` when
    depotID is D's PRIMARY KEY, in the guard and in the reducer's `PER`. Fixed-charge
    cover of 12 units: D1 alone reaches exactly 12 at cost 10 (D2 + D3 would cost
    20). Charging the opening cost per shipment row instead of once per depot would
    price D1 at 30 and D2 + D3 at 28, opening the wrong pair."""
    got = _rows(decidb_cli, """
        CREATE TEMP TABLE Depot(depotID VARCHAR PRIMARY KEY, cost INT);
        INSERT INTO Depot VALUES ('D1', 10), ('D2', 8), ('D3', 12);
        CREATE TEMP TABLE Shipment(sid VARCHAR PRIMARY KEY, depotID VARCHAR, demand INT);
        INSERT INTO Shipment VALUES ('S1', 'D1', 5), ('S2', 'D1', 3), ('S3', 'D1', 4),
                                    ('S4', 'D2', 6), ('S5', 'D2', 2), ('S6', 'D3', 7);
        SELECT S.sid, ship, open FROM Shipment S JOIN Depot D ON S.depotID = D.depotID
        DECIDE ship(INT) BETWEEN 0 AND S.demand, PER D: open(BOOL)
        SUCH THAT PER D.depotID IF NOT open: SUM(ship) BY (D.depotID) <= 0
              AND PER (): SUM(ship) >= 12
        MINIMIZE SUM(PER D.depotID: D.cost * open)
    """, "sid", "ship", "open")
    ships = {"S1": ("D1", 5), "S2": ("D1", 3), "S3": ("D1", 4),
             "S4": ("D2", 6), "S5": ("D2", 2), "S6": ("D3", 7)}
    costs = {"D1": 10, "D2": 8, "D3": 12}
    variables = {f"o_{d}": _BIN for d in costs}
    variables.update({f"s_{s}": (VarType.INTEGER, 0.0, float(dem)) for s, (_, dem) in ships.items()})
    # open_D = 0 ⟹ ship <= 0, per row with its own demand as the bound.
    rows = [({f"s_{s}": 1.0, f"o_{d}": -float(dem)}, "<=", 0.0) for s, (d, dem) in ships.items()]
    rows.append(({f"s_{s}": 1.0 for s in ships}, ">=", 12.0))
    result = _solve(oracle_solver, "pk_covered_decision", variables, rows,
                    {f"o_{d}": float(c) for d, c in costs.items()}, ObjSense.MINIMIZE)
    assert got == [("S1", 5, True), ("S2", 3, True), ("S3", 4, True),
                   ("S4", 0, False), ("S5", 0, False), ("S6", 0, False)]
    opened = {ships[s][0] for s, _, o in got if o}
    assert sum(costs[d] for d in opened) == pytest.approx(result.objective_value)


@pytest.mark.per_clause
@pytest.mark.sql_joins
@pytest.mark.correctness
def test_row_decision_under_a_key_covering_every_relations_table_key(decidb_cli, oracle_solver):
    """§3.1 (delta 6): a per-row decision is one value per instance when the key
    covers a table key of every relation in FROM (`PER dep.id, sh.sid`, both PRIMARY
    KEYs), so each x is held to its depot's cap (x_3 = 4, not its demand 6). Leaving
    sh's key out is refused (see below)."""
    got = _rows(decidb_cli, _DEP_SH.format(key="PRIMARY KEY", sh_key="PRIMARY KEY") + """
        SELECT sid, x FROM sh JOIN dep ON sh.dep = dep.id
        DECIDE x(INT) BETWEEN 0 AND dem
        SUCH THAT PER dep.id, sh.sid: x <= dep.cap MAXIMIZE SUM(sid * x)
    """, "sid", "x")
    result = _solve(oracle_solver, "row_decision_pk",
                    {f"x_{s}": (VarType.INTEGER, 0.0, float(dem)) for s, (_, dem) in _SH.items()},
                    [({f"x_{s}": 1.0}, "<=", float(_DEP_CAP[d])) for s, (d, _) in _SH.items()],
                    {f"x_{s}": float(s) for s in _SH})
    assert got == [(1, 3), (2, 4), (3, 4)]
    assert sum(s * x for s, x in got) == pytest.approx(result.objective_value)


@pytest.mark.per_clause
@pytest.mark.var_integer
@pytest.mark.correctness
def test_query_wide_decision_is_one_value_in_every_scope(decidb_cli, oracle_solver):
    """§3.1: a `PER ()` decision (and a constant) may be read per row, under `PER
    grp` and under `PER ()`. With `lim <= 6` group a is held to 6; dropping that row
    would let lim float and give x = (3, 7, 2), objective 23 instead of 18."""
    got = _rows(decidb_cli, f"""
        SELECT id, x, lim FROM {_T} DECIDE x(INT) BETWEEN 0 AND cap, PER (): lim(INT)
        SUCH THAT PER grp: SUM(x) BY (grp) <= lim AND x <= lim AND PER (): lim <= 6
        MAXIMIZE SUM(id * x)
    """, "id", "x", "lim")
    variables = {f"x_{i}": (VarType.INTEGER, 0.0, float(cap)) for i, (_, cap) in _T_ROWS.items()}
    variables["lim"] = _INT
    rows = [({"x_1": 1.0, "x_2": 1.0, "lim": -1.0}, "<=", 0.0), ({"x_3": 1.0, "lim": -1.0}, "<=", 0.0),
            ({"lim": 1.0}, "<=", 6.0)]
    rows += [({f"x_{i}": 1.0, "lim": -1.0}, "<=", 0.0) for i in _T_ROWS]
    result = _solve(oracle_solver, "query_wide", variables, rows, {f"x_{i}": float(i) for i in _T_ROWS})
    assert got == [(1, 0, 6), (2, 6, 6), (3, 2, 6)]
    assert sum(i * x for i, x, _ in got) == pytest.approx(result.objective_value)


@pytest.mark.per_clause
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_reducer_by_key_determined_through_the_table_key(decidb_cli, oracle_solver):
    """§3.1 / deck p26: a reducer is admitted when the key determines its BY key;
    `PER d.id` determines `grp` through the PRIMARY KEY. Both a-instances impose the
    same group row (x1 + x2 <= 6); a global sum instead would give 17, not 26."""
    got = _rows(decidb_cli, """
        CREATE TEMP TABLE d(id INT PRIMARY KEY, grp VARCHAR);
        INSERT INTO d VALUES (1, 'a'), (2, 'a'), (3, 'b');
        SELECT id, x FROM d DECIDE x(INT) BETWEEN 0 AND 5
        SUCH THAT PER d.id: SUM(x) BY (grp) <= 6 MAXIMIZE SUM(id * x)
    """, "id", "x")
    result = _solve(oracle_solver, "by_via_pk", {"x_1": _X5, "x_2": _X5, "x_3": _X5},
                    [({"x_1": 1.0, "x_2": 1.0}, "<=", 6.0), ({"x_3": 1.0}, "<=", 6.0)],
                    {"x_1": 1.0, "x_2": 2.0, "x_3": 3.0})
    assert got == [(1, 1), (2, 5), (3, 5)]
    assert sum(i * x for i, x in got) == pytest.approx(result.objective_value)


# --- The functional-dependency rule: every way it fails ---

@pytest.mark.error
@pytest.mark.error_binder
@pytest.mark.per_clause
def test_column_outside_the_key_is_refused(decidb_cli):
    """§3.1: `cap` varies inside group a, so `PER grp` cannot read it."""
    decidb_cli.assert_error(f"""
        SELECT id, x FROM {_T} DECIDE x(INT) BETWEEN 0 AND 5
        SUCH THAT PER grp: SUM(x) BY (grp) <= cap MAXIMIZE SUM(x)
    """, match=r"not determined")


@pytest.mark.error
@pytest.mark.error_binder
@pytest.mark.per_clause
def test_row_decision_under_a_keyed_generation_is_refused(decidb_cli):
    """§3.1: a per-row decision is not one value per group."""
    decidb_cli.assert_error(f"""
        SELECT id, x FROM {_T} DECIDE x(INT) BETWEEN 0 AND 5
        SUCH THAT PER grp: x <= 3 MAXIMIZE SUM(x)
    """, match=r"decision 'x'")


@pytest.mark.error
@pytest.mark.error_binder
@pytest.mark.per_clause
def test_row_decision_under_a_key_missing_one_relations_table_key_is_refused(decidb_cli):
    """§3.1 (delta 6): `PER dep.id` covers dep's key but not sh's, so a per-row
    decision over the join is still several values per instance."""
    decidb_cli.assert_error(_DEP_SH.format(key="PRIMARY KEY", sh_key="PRIMARY KEY") + """
        SELECT sid, x FROM sh JOIN dep ON sh.dep = dep.id DECIDE x(INT) BETWEEN 0 AND dem
        SUCH THAT PER dep.id: x <= dep.cap MAXIMIZE SUM(x)
    """, match=r"decision 'x'")


@pytest.mark.error
@pytest.mark.error_binder
@pytest.mark.per_clause
def test_keyed_decision_under_an_unrelated_key_is_refused(decidb_cli):
    """§3.1: `PER cap` does not determine a `PER grp` decision (cap 3 and 7 are both group a)."""
    decidb_cli.assert_error(f"""
        SELECT id, y FROM {_T} DECIDE PER grp: y(INT)
        SUCH THAT PER cap: y <= 5 MAXIMIZE SUM(y)
    """, match=r"decision 'y'")


@pytest.mark.error
@pytest.mark.error_binder
@pytest.mark.per_clause
def test_keyed_decision_under_the_query_key_is_refused(decidb_cli):
    """§3.1: `PER ()` generates one instance, which cannot read a `PER grp` decision directly."""
    decidb_cli.assert_error(f"""
        SELECT id, y FROM {_T} DECIDE PER grp: y(INT)
        SUCH THAT PER (): y <= 5 MAXIMIZE SUM(y)
    """, match=r"decision 'y'.*PER \(\)")


@pytest.mark.error
@pytest.mark.error_binder
@pytest.mark.per_clause
def test_keyed_guard_under_a_coarser_key_is_refused(decidb_cli):
    """Deck p59 (κ → b): a `PER grp, cap` guard is two values for the `PER grp` instance of group a."""
    decidb_cli.assert_error(f"""
        SELECT id, x FROM {_T} DECIDE x(INT) BETWEEN 0 AND 5, PER grp, cap: open(BOOL)
        SUCH THAT PER grp IF NOT open: SUM(x) BY (grp) <= 0 MAXIMIZE SUM(x)
    """, match=r"decision 'open'")


@pytest.mark.error
@pytest.mark.error_binder
@pytest.mark.per_clause
def test_by_key_outside_the_generation_key_is_refused(decidb_cli):
    """Deck p27 (κ → Γ): `PER grp` does not determine the `BY (id)` group."""
    decidb_cli.assert_error(f"""
        SELECT id, x FROM {_T} DECIDE x(INT) BETWEEN 0 AND 5
        SUCH THAT PER grp: SUM(x) BY (id) <= 5 MAXIMIZE SUM(x)
    """, match=r"BY \(id\)")


@pytest.mark.error
@pytest.mark.error_binder
@pytest.mark.per_clause
def test_query_key_cannot_read_a_row_column(decidb_cli):
    """§3.1: under `PER ()` a column that varies across rows has no single value."""
    decidb_cli.assert_error(f"""
        SELECT id, x FROM {_T} DECIDE x(INT) BETWEEN 0 AND 5
        SUCH THAT PER (): SUM(x) <= cap MAXIMIZE SUM(x)
    """, match=r"varies across rows")


@pytest.mark.error
@pytest.mark.error_binder
@pytest.mark.per_clause
def test_partial_composite_table_key_is_refused(decidb_cli):
    """§3.1: PRIMARY KEY (a, b) with only `a` in the key does not determine `cap`."""
    decidb_cli.assert_error(_DEP2_SH2 + """
        DECIDE x(INT) BETWEEN 0 AND dem
        SUCH THAT PER dep2.a: SUM(x) BY (dep2.a) <= dep2.cap MAXIMIZE SUM(x)
    """, match=r"not determined")


@pytest.mark.error
@pytest.mark.error_binder
@pytest.mark.per_clause
def test_table_key_does_not_help_a_key_on_another_column(decidb_cli):
    """§3.1: the PRIMARY KEY on `id` says nothing about `PER grp`, which still does not determine `cap`."""
    decidb_cli.assert_error("""
        CREATE TEMP TABLE d(id INT PRIMARY KEY, grp VARCHAR, cap INT);
        INSERT INTO d VALUES (1, 'a', 3), (2, 'a', 7), (3, 'b', 2);
        SELECT id, x FROM d DECIDE x(INT) BETWEEN 0 AND 5
        SUCH THAT PER grp: SUM(x) BY (grp) <= cap MAXIMIZE SUM(x)
    """, match=r"not determined")


@pytest.mark.error
@pytest.mark.error_binder
@pytest.mark.per_clause
def test_table_key_is_invisible_through_a_subquery(decidb_cli):
    """§3.1 admits a column 'of a base table' only: the same key behind a subquery is refused."""
    decidb_cli.assert_error("""
        CREATE TEMP TABLE d(id INT PRIMARY KEY, grp VARCHAR, cap INT);
        INSERT INTO d VALUES (1, 'a', 3), (2, 'a', 7), (3, 'b', 2);
        SELECT id, x FROM (SELECT * FROM d) s DECIDE x(INT) BETWEEN 0 AND 5
        SUCH THAT PER s.id: SUM(x) BY (s.id) <= s.cap MAXIMIZE SUM(x)
    """, match=r"not determined")


@pytest.mark.error
@pytest.mark.error_binder
@pytest.mark.per_clause
def test_reducer_local_key_must_determine_its_body(decidb_cli):
    """§4: the body of `SUM(PER grp: ...)` must be a function of grp; `cap` is not."""
    decidb_cli.assert_error(f"""
        SELECT id, open FROM {_T} DECIDE PER grp: open(BOOL)
        SUCH THAT PER (): SUM(PER grp: open) >= 1 MINIMIZE SUM(PER grp: cap * open)
    """, match=r"not determined")


@pytest.mark.error
@pytest.mark.error_binder
@pytest.mark.per_clause
def test_key_naming_a_decision_is_refused(decidb_cli):
    """§4 (delta 13): a key names columns or relations, never a decision -- in a
    constraint prefix and in a declarator alike."""
    decidb_cli.assert_error(f"""
        SELECT id, x FROM {_T} DECIDE x(INT) BETWEEN 0 AND 5, y(INT) BETWEEN 0 AND 5
        SUCH THAT PER y: x <= 3 MAXIMIZE SUM(x)
    """, match=r"is a decision")
    decidb_cli.assert_error(f"""
        SELECT id, x FROM {_T} DECIDE y(INT) BETWEEN 0 AND 5, PER y: x(INT)
        SUCH THAT x <= 3 MAXIMIZE SUM(x)
    """, match=r"is a decision")


@pytest.mark.error
@pytest.mark.error_binder
@pytest.mark.per_clause
def test_correlated_subquery_under_a_key_is_refused(decidb_cli):
    """§3.1 lists what a keyed instance may read; a correlated subquery is not among
    them (the same subquery in a per-row constraint solves)."""
    decidb_cli.assert_error(f"""
        SELECT id, x FROM {_T} DECIDE x(INT) BETWEEN 0 AND 5
        SUCH THAT PER grp: SUM(x) BY (grp) <=
            (SELECT MAX(cap) FROM {_T.replace('t(', 'u(')} WHERE u.grp = t.grp)
        MAXIMIZE SUM(x)
    """, match=r"correlated subquery")


# --- Refinement: PER grp, cap: SUM(x) BY (grp) <op> cap with a cap varying in a group ---

@pytest.mark.per_clause
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_refinement_keeps_the_tightest_upper_bound(decidb_cli, oracle_solver):
    """§3.1: the (a, 3) and (a, 7) instances share the group sum, so a is held to 3.
    Keeping only the last cap (7) would give x = (2, 5, 2), objective 18 instead of 12."""
    got = _rows(decidb_cli, f"""
        SELECT id, x FROM {_T} DECIDE x(INT) BETWEEN 0 AND 5
        SUCH THAT PER grp, cap: SUM(x) BY (grp) <= cap MAXIMIZE SUM(id * x)
    """, "id", "x")
    result = _solve(oracle_solver, "refine_le", {"x_1": _X5, "x_2": _X5, "x_3": _X5},
                    [({"x_1": 1.0, "x_2": 1.0}, "<=", 3.0), ({"x_1": 1.0, "x_2": 1.0}, "<=", 7.0),
                     ({"x_3": 1.0}, "<=", 2.0)],
                    {"x_1": 1.0, "x_2": 2.0, "x_3": 3.0})
    assert got == [(1, 0), (2, 3), (3, 2)]
    assert sum(i * x for i, x in got) == pytest.approx(result.objective_value)


@pytest.mark.per_clause
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_refinement_keeps_the_tightest_lower_bound(decidb_cli, oracle_solver):
    """§3.1: for `>=` the tightest cap is the largest (7). Keeping only cap 3 would
    give x = (3, 0, 2), objective 9 instead of 15."""
    got = _rows(decidb_cli, f"""
        SELECT id, x FROM {_T} DECIDE x(INT) BETWEEN 0 AND 5
        SUCH THAT PER grp, cap: SUM(x) BY (grp) >= cap MINIMIZE SUM(id * x)
    """, "id", "x")
    result = _solve(oracle_solver, "refine_ge", {"x_1": _X5, "x_2": _X5, "x_3": _X5},
                    [({"x_1": 1.0, "x_2": 1.0}, ">=", 3.0), ({"x_1": 1.0, "x_2": 1.0}, ">=", 7.0),
                     ({"x_3": 1.0}, ">=", 2.0)],
                    {"x_1": 1.0, "x_2": 2.0, "x_3": 3.0}, ObjSense.MINIMIZE)
    assert got == [(1, 5), (2, 2), (3, 2)]
    assert sum(i * x for i, x in got) == pytest.approx(result.objective_value)


@pytest.mark.error
@pytest.mark.per_clause
@pytest.mark.cons_comparison
def test_refinement_of_an_equality_with_two_values_is_a_contradiction(decidb_cli):
    """§3.1: `SUM(x) BY (grp) = cap` cannot hold 3 and 7 at once for group a."""
    decidb_cli.assert_error(f"""
        SELECT id, x FROM {_T} DECIDE x(INT) BETWEEN 0 AND 5
        SUCH THAT PER grp, cap: SUM(x) BY (grp) = cap MAXIMIZE SUM(id * x)
    """, match=r"more than one value")


@pytest.mark.per_clause
@pytest.mark.cons_comparison
@pytest.mark.correctness
def test_refinement_of_an_equality_with_one_value_solves(decidb_cli, oracle_solver):
    """§3.1: when every instance of a group carries the same cap the `=` refinement
    is one row (x1 + x2 = 3), not a contradiction; without it x = (5, 5, 5) = 30."""
    got = _rows(decidb_cli, """
        SELECT id, x FROM (VALUES (1, 'a', 3), (2, 'a', 3), (3, 'b', 2)) t(id, grp, cap)
        DECIDE x(INT) BETWEEN 0 AND 5
        SUCH THAT PER grp, cap: SUM(x) BY (grp) = cap MAXIMIZE SUM(id * x)
    """, "id", "x")
    result = _solve(oracle_solver, "refine_eq", {"x_1": _X5, "x_2": _X5, "x_3": _X5},
                    [({"x_1": 1.0, "x_2": 1.0}, "=", 3.0), ({"x_3": 1.0}, "=", 2.0)],
                    {"x_1": 1.0, "x_2": 2.0, "x_3": 3.0})
    assert got == [(1, 0), (2, 3), (3, 2)]
    assert sum(i * x for i, x in got) == pytest.approx(result.objective_value)


@pytest.mark.per_clause
@pytest.mark.cons_comparison
@pytest.mark.correctness
def test_refinement_excludes_every_value_for_not_equal(decidb_cli, oracle_solver):
    """§3.1: `<>` keeps every cap of the group as its own exclusion: a avoids 3 and
    4, so its best sum is 2. Excluding only 3 would give (2, 2, 1) = 9, only 4 gives
    (1, 2, 1) = 8; the answer 7 pins 'every one'."""
    got = _rows(decidb_cli, """
        SELECT id, x FROM (VALUES (1, 'a', 3), (2, 'a', 4), (3, 'b', 2)) t(id, grp, cap)
        DECIDE x(INT) BETWEEN 0 AND 2
        SUCH THAT PER grp, cap: SUM(x) BY (grp) <> cap MAXIMIZE SUM(id * x)
    """, "id", "x")
    oracle_solver.create_model("refine_ne")
    for i in (1, 2, 3):
        oracle_solver.add_variable(f"x_{i}", VarType.INTEGER, lb=0.0, ub=2.0)
    add_ne_indicator(oracle_solver, {"x_1": 1.0, "x_2": 1.0}, 3.0, "a_ne_3")
    add_ne_indicator(oracle_solver, {"x_1": 1.0, "x_2": 1.0}, 4.0, "a_ne_4")
    add_ne_indicator(oracle_solver, {"x_3": 1.0}, 2.0, "b_ne_2")
    oracle_solver.set_objective({"x_1": 1.0, "x_2": 2.0, "x_3": 3.0}, ObjSense.MAXIMIZE)
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL
    assert got == [(1, 0), (2, 2), (3, 1)]
    assert sum(i * x for i, x in got) == pytest.approx(result.objective_value)


# --- NULL as a key value ---

def _null_group_rows(cap, ids=_N_ROWS):
    """One `SUM(x) BY (grp) <= cap` row per distinct key of _N, NULL being a key."""
    groups = {}
    for i, g in ids.items():
        groups.setdefault(g, {})[f"x_{i}"] = 1.0
    return [(coeffs, "<=", cap) for coeffs in groups.values()]


@pytest.mark.per_clause
@pytest.mark.edge_case
@pytest.mark.correctness
def test_null_keyed_rows_form_one_constraint_instance(decidb_cli, oracle_solver):
    """§2.1 (delta 1): `PER grp` generates an instance for the NULL-keyed rows too,
    as SQL's GROUP BY would. If NULL generated no instance rows 2 and 4 would be free
    at 10 (objective 72 instead of 28); a NULL row alone in its group gives 36."""
    got = _rows(decidb_cli, f"""
        SELECT id, x FROM {_N} DECIDE x(INT) BETWEEN 0 AND 10
        SUCH THAT PER grp: SUM(x) BY (grp) <= 4 MAXIMIZE SUM(id * x)
    """, "id", "x")
    result = _solve(oracle_solver, "null_instance", {f"x_{i}": _X10 for i in _N_ROWS},
                    _null_group_rows(4.0), {f"x_{i}": float(i) for i in _N_ROWS})
    assert got == [(1, 0), (2, 0), (3, 4), (4, 4)]
    assert sum(i * x for i, x in got) == pytest.approx(result.objective_value)


@pytest.mark.per_clause
@pytest.mark.when_constraint
@pytest.mark.correctness
def test_when_not_null_excludes_the_null_keyed_instance(decidb_cli, oracle_solver):
    """§2.1: `WHEN grp IS NOT NULL PER grp` is how the NULL-keyed rows are left out;
    only group a is capped, so rows 2 and 4 take their box of 10 (objective 72;
    ignoring the WHEN gives 28)."""
    got = _rows(decidb_cli, f"""
        SELECT id, x FROM {_N} DECIDE x(INT) BETWEEN 0 AND 10
        SUCH THAT WHEN grp IS NOT NULL PER grp: SUM(x) BY (grp) <= 4 MAXIMIZE SUM(id * x)
    """, "id", "x")
    non_null = {i: g for i, g in _N_ROWS.items() if g is not None}
    result = _solve(oracle_solver, "when_not_null", {f"x_{i}": _X10 for i in _N_ROWS},
                    _null_group_rows(4.0, non_null), {f"x_{i}": float(i) for i in _N_ROWS})
    assert got == [(1, 0), (2, 10), (3, 4), (4, 10)]
    assert sum(i * x for i, x in got) == pytest.approx(result.objective_value)


@pytest.mark.per_clause
@pytest.mark.edge_case
@pytest.mark.correctness
def test_decision_keyed_on_null_is_shared_and_reads_back(decidb_cli, oracle_solver):
    """§2.1: the NULL-keyed rows share one decision, bounded by both their caps
    (min(7, 2) = 2) and returned on both rows. Separate decisions would read 7 and 2."""
    got = _rows(decidb_cli, """
        SELECT id, grp, y FROM (VALUES (1, 'a', 3), (2, NULL, 7), (3, NULL, 2)) t(id, grp, cap)
        DECIDE PER grp: y(INT) SUCH THAT y <= cap MAXIMIZE SUM(y)
    """, "id", "grp", "y")
    result = _solve(oracle_solver, "null_decision", {"y_a": _INT, "y_null": _INT},
                    [({"y_a": 1.0}, "<=", 3.0), ({"y_null": 1.0}, "<=", 7.0), ({"y_null": 1.0}, "<=", 2.0)],
                    {"y_a": 1.0, "y_null": 2.0})
    assert got == [(1, "a", 3), (2, None, 2), (3, None, 2)]
    assert sum(y for _, _, y in got) == pytest.approx(result.objective_value)


@pytest.mark.cons_aggregate
@pytest.mark.edge_case
@pytest.mark.correctness
def test_by_groups_null_keyed_rows_together(decidb_cli, oracle_solver):
    """§4: `BY (grp)` puts the NULL-keyed rows in one group of their own. Each row is
    at most half its group's sum, so x1 = x3 and x2 = x4; a NULL row grouped alone
    would be forced to 0 (x <= 0.5 x), giving (6, 0, 6, 0) = 24 instead of 36."""
    got = _rows(decidb_cli, f"""
        SELECT id, x FROM {_N} DECIDE x(INT) BETWEEN 0 AND 10
        SUCH THAT x <= 0.5 * SUM(x) BY (grp) AND PER (): SUM(x) <= 12 MAXIMIZE SUM(id * x)
    """, "id", "x")
    rows = [({"x_1": 0.5, "x_3": -0.5}, "<=", 0.0), ({"x_3": 0.5, "x_1": -0.5}, "<=", 0.0),
            ({"x_2": 0.5, "x_4": -0.5}, "<=", 0.0), ({"x_4": 0.5, "x_2": -0.5}, "<=", 0.0),
            ({f"x_{i}": 1.0 for i in _N_ROWS}, "<=", 12.0)]
    result = _solve(oracle_solver, "null_by_group", {f"x_{i}": _X10 for i in _N_ROWS}, rows,
                    {f"x_{i}": float(i) for i in _N_ROWS})
    assert got == [(1, 0), (2, 6), (3, 0), (4, 6)]
    assert sum(i * x for i, x in got) == pytest.approx(result.objective_value)


@pytest.mark.per_clause
@pytest.mark.edge_case
@pytest.mark.correctness
def test_absorbed_and_row_bounds_agree_on_a_null_key(decidb_cli, oracle_solver):
    """§2.1: `PER grp: y <= 5` (absorbed into the box) and `PER grp: 2 * y <= 10` (a
    model row) are one constraint and both reach the NULL-keyed decision; skipping
    NULL instances for rows only would leave the second spelling unbounded."""
    absorbed = _rows(decidb_cli, f"""
        SELECT id, y FROM {_T.replace("'a', 7", "NULL, 7")} DECIDE PER grp: y(INT)
        SUCH THAT PER grp: y <= 5 MAXIMIZE SUM(y)
    """, "id", "y")
    row_bound = _rows(decidb_cli, f"""
        SELECT id, y FROM {_T.replace("'a', 7", "NULL, 7")} DECIDE PER grp: y(INT)
        SUCH THAT PER grp: 2 * y <= 10 MAXIMIZE SUM(y)
    """, "id", "y")
    result = _solve(oracle_solver, "null_bound_shapes", {f"y_{k}": _INT for k in ("a", "null", "b")},
                    [({f"y_{k}": 2.0}, "<=", 10.0) for k in ("a", "null", "b")],
                    {f"y_{k}": 1.0 for k in ("a", "null", "b")})
    assert absorbed == row_bound == [(1, 5), (2, 5), (3, 5)]
    assert sum(y for _, y in absorbed) == pytest.approx(result.objective_value)


@pytest.mark.per_clause
@pytest.mark.var_boolean
@pytest.mark.correctness
def test_null_keyed_decision_counts_once_in_a_keyed_reducer(decidb_cli, oracle_solver):
    """§4: `SUM(PER grp: o)` has one term per key, the NULL key included, so at most
    one of o_a and o_NULL may be on (objective 6). Leaving the NULL term out would
    switch both on (10); counting per row (2 o_a + 2 o_NULL <= 1) would switch none on."""
    got = _rows(decidb_cli, f"""
        SELECT id, o FROM {_N} DECIDE PER grp: o(BOOL)
        SUCH THAT PER (): SUM(PER grp: o) <= 1 MAXIMIZE SUM(id * o)
    """, "id", "o")
    result = _solve(oracle_solver, "null_keyed_term", {"o_a": _BIN, "o_null": _BIN},
                    [({"o_a": 1.0, "o_null": 1.0}, "<=", 1.0)], {"o_a": 4.0, "o_null": 6.0})
    assert got == [(1, False), (2, True), (3, False), (4, True)]
    assert sum(i * o for i, o in got) == pytest.approx(result.objective_value)


@pytest.mark.per_clause
@pytest.mark.when_constraint
@pytest.mark.edge_case
@pytest.mark.correctness
def test_when_admitting_no_row_imposes_nothing(decidb_cli, oracle_solver):
    """§4 (delta 2): a defensive `WHEN grp IS NULL PER grp` on data without NULL keys
    has no instance and is not an error; every x takes its box (30). Imposing the
    row regardless of the WHEN would cap each group at 4 (12)."""
    got = _rows(decidb_cli, """
        SELECT id, x FROM (VALUES (1, 'a'), (2, 'b')) t(id, grp) DECIDE x(INT) BETWEEN 0 AND 10
        SUCH THAT WHEN grp IS NULL PER grp: SUM(x) BY (grp) <= 4 MAXIMIZE SUM(id * x)
    """, "id", "x")
    result = _solve(oracle_solver, "when_empty", {"x_1": _X10, "x_2": _X10}, [], {"x_1": 1.0, "x_2": 2.0})
    assert got == [(1, 10), (2, 10)]
    assert sum(i * x for i, x in got) == pytest.approx(result.objective_value)


# --- Explicit spellings of the default key ---

@pytest.mark.per_clause
@pytest.mark.correctness
def test_per_row_is_the_explicit_spelling_of_the_default(decidb_cli, oracle_solver):
    """§2.1/§3/§4 (delta 14): `PER ROW: x(INT)`, `PER ROW: body` and `SUM(PER ROW: x)`
    are the omitted-PER forms, in either case; taking `row` for a column would refuse,
    and a `PER ()` reading of the declarator would make x one value (x = 2, objective 12)."""
    explicit = _rows(decidb_cli, f"""
        SELECT id, x FROM {_T} DECIDE PER ROW: x(INT)
        SUCH THAT PER ROW: x <= cap AND PER (): SUM(PER ROW: x) <= 8 MAXIMIZE SUM(id * x)
    """, "id", "x")
    lowercase = _rows(decidb_cli, f"""
        SELECT id, x FROM {_T} decide per row: x(INT)
        such that per row: x <= cap and per (): sum(per row: x) <= 8 maximize sum(id * x)
    """, "id", "x")
    default = _rows(decidb_cli, f"""
        SELECT id, x FROM {_T} DECIDE x(INT)
        SUCH THAT x <= cap AND PER (): SUM(x) <= 8 MAXIMIZE SUM(id * x)
    """, "id", "x")
    result = _solve(oracle_solver, "per_row",
                    {f"x_{i}": (VarType.INTEGER, 0.0, float(cap)) for i, (_, cap) in _T_ROWS.items()},
                    [({f"x_{i}": 1.0 for i in _T_ROWS}, "<=", 8.0)], {f"x_{i}": float(i) for i in _T_ROWS})
    assert explicit == lowercase == default == [(1, 0), (2, 6), (3, 2)]
    assert sum(i * x for i, x in explicit) == pytest.approx(result.objective_value)


@pytest.mark.per_clause
@pytest.mark.correctness
def test_parenthesized_key_is_the_same_key(decidb_cli, oracle_solver):
    """§2.1 (delta 15): `PER (grp)` and `PER (grp, cap)` name the same keys as the
    bare spellings, in a declarator and in a constraint prefix. Reading `(grp)` as
    the query key `()` would refuse `SUM(x) BY (grp) <= 4` (grp varies across rows)."""
    parenthesized = _rows(decidb_cli, f"""
        SELECT id, x, y FROM {_T} DECIDE x(INT) BETWEEN 0 AND 5, PER (grp): y(INT)
        SUCH THAT PER (grp): SUM(x) BY (grp) <= 4 AND PER (grp, cap): y <= cap
        MAXIMIZE SUM(id * x) + SUM(y)
    """, "id", "x", "y")
    bare = _rows(decidb_cli, f"""
        SELECT id, x, y FROM {_T} DECIDE x(INT) BETWEEN 0 AND 5, PER grp: y(INT)
        SUCH THAT PER grp: SUM(x) BY (grp) <= 4 AND PER grp, cap: y <= cap
        MAXIMIZE SUM(id * x) + SUM(y)
    """, "id", "x", "y")
    variables = {"x_1": _X5, "x_2": _X5, "x_3": _X5, "y_a": _INT, "y_b": _INT}
    rows = [({"x_1": 1.0, "x_2": 1.0}, "<=", 4.0), ({"x_3": 1.0}, "<=", 4.0),
            ({"y_a": 1.0}, "<=", 3.0), ({"y_a": 1.0}, "<=", 7.0), ({"y_b": 1.0}, "<=", 2.0)]
    result = _solve(oracle_solver, "paren_key", variables, rows,
                    {"x_1": 1.0, "x_2": 2.0, "x_3": 3.0, "y_a": 2.0, "y_b": 1.0})
    assert parenthesized == bare == [(1, 0, 3), (2, 4, 3), (3, 4, 2)]
    assert sum(i * x + y for i, x, y in parenthesized) == pytest.approx(result.objective_value)


@pytest.mark.per_clause
@pytest.mark.edge_case
@pytest.mark.correctness
def test_a_column_named_row_is_keyed_qualified(decidb_cli, oracle_solver):
    """§2.1 (delta 14): `ROW` is the default's spelling, so a column literally named
    row is keyed as `t.row`; group a is held to its smaller cap 3 (objective 8). Taking
    `t.row` for the per-row default would make y per row: (3, 7, 2) = 12."""
    got = _rows(decidb_cli, """
        SELECT id, y FROM (VALUES (1, 'a', 3), (2, 'a', 7), (3, 'b', 2)) t(id, "row", cap)
        DECIDE PER t.row: y(INT) SUCH THAT PER t.row, cap: y <= cap MAXIMIZE SUM(y)
    """, "id", "y")
    result = _solve(oracle_solver, "row_column", {"y_a": _INT, "y_b": _INT},
                    [({"y_a": 1.0}, "<=", 3.0), ({"y_a": 1.0}, "<=", 7.0), ({"y_b": 1.0}, "<=", 2.0)],
                    {"y_a": 2.0, "y_b": 1.0})
    assert got == [(1, 3), (2, 3), (3, 2)]
    assert sum(y for _, y in got) == pytest.approx(result.objective_value)
