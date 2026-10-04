"""DeciQL declarations, the edges (syntax_reference §2; deck p6-p11): companion to
`test_deciql_declarations.py`, which pins the core surface.

This file pins what the core file leaves open: the spellings of one key (case,
spacing, parentheses, qualification, column order), the `D.x` alias's limits, reads
through a table's PRIMARY KEY / UNIQUE columns, bounds that scale into the objective or
combine a negative constant with a column, the SEMI and TEXT edges (a constant range
against a smaller cap, a negative range, exact-case comparison in every form), the
serialization round-trip of keyed bounds and domains, and the refusals that name the
fix for every wrong domain word, bound and key.

Every correctness test builds the same model independently in the oracle (a keyed
decision is one variable shared by its rows; a SEMI domain is a binary switch; a TEXT
domain is one indicator per value) and compares the objective, plus the decision vector
where the optimum is unique. Each docstring names the rule and the wrong answer a
plausible bug would give.
"""

import json

import pytest

from solver.types import ObjSense, SolverStatus, VarType

_INT, _BIN, _REAL = VarType.INTEGER, VarType.BINARY, VarType.CONTINUOUS
_MAX, _MIN = ObjSense.MAXIMIZE, ObjSense.MINIMIZE

# (id, grp, cap): the two 'a' rows carry different caps, so a keyed and a per-row
# decision answer `x <= cap` differently (3, 3, 2 against 3, 7, 2).
_T = "(VALUES (1, 'a', 3), (2, 'a', 7), (3, 'b', 2)) t(id, grp, cap)"
_CAP = {1: 3, 2: 7, 3: 2}
_PK_DEPOTS = """
    CREATE TEMP TABLE d(depot VARCHAR PRIMARY KEY, cost INT); INSERT INTO d VALUES ('a', 10), ('b', 7);
    CREATE TEMP TABLE r(id INT, depot VARCHAR, cap INT); INSERT INTO r VALUES (1, 'a', 6), (2, 'b', 3), (3, 'b', 3);
"""


def _rows(decidb_cli, sql, *cols):
    rows, names = decidb_cli.execute(sql)
    idx = [names.index(c) for c in cols]
    return sorted(tuple(r[i] for i in idx) for r in rows)


def _solve(oracle, variables, rows, objective, sense=_MAX):
    """One oracle model: variables {name: (type, lb, ub)}, rows [(coeffs, op, rhs)]
    and a linear objective; returns the OPTIMAL result."""
    oracle.create_model("declaration_edges")
    for v, (kind, lb, ub) in variables.items():
        oracle.add_variable(v, kind, lb=float(lb), ub=None if ub is None else float(ub))
    for coeffs, op, rhs in rows:
        oracle.add_constraint(coeffs, op, float(rhs))
    oracle.set_objective(objective, sense)
    result = oracle.solve()
    assert result.status == SolverStatus.OPTIMAL
    return result


def _boxes(oracle, boxes, sense=_MAX, weights=None, kind=_INT):
    """Independent decisions in boxes {name: (lb, ub)} under a weighted sum. A keyed
    decision's weight is the number of rows reading it: `SUM(x)` counts per row."""
    return _solve(oracle, {v: (kind, lb, ub) for v, (lb, ub) in boxes.items()}, [],
                  {v: float((weights or {}).get(v, 1)) for v in boxes}, sense)


def _keyed(oracle, row_keys, caps):
    """`PER <key>: x(INT)` under a per-row `x <= cap` and `MAXIMIZE SUM(x)`: one
    variable per distinct key (`row_keys`: row id -> key), one `x_key <= cap` row per
    input row, and a key's weight is the number of rows reading it."""
    keys = sorted(set(row_keys.values()), key=str)
    rows = [({f"x_{row_keys[i]}": 1.0}, "<=", c) for i, c in caps.items()]
    weights = {f"x_{k}": float(list(row_keys.values()).count(k)) for k in keys}
    return _solve(oracle, {f"x_{k}": (_INT, 0, None) for k in keys}, rows, weights)


def _check(decidb_cli, sql, cols, want, result=None, value=lambda got: sum(r[-1] for r in got)):
    """The query's rows must be `want` and `value(rows)` the oracle's optimum."""
    got = _rows(decidb_cli, sql, *cols)
    assert got == want, sql
    if result is not None:
        assert value(got) == pytest.approx(result.objective_value), sql
    return got


def _semi(oracle, ranges, kind, weights, sense, total=None, extra=()):
    """`s in {0} ∪ [lo, hi]` per key through a binary switch (`ranges`: key -> (lo, hi)),
    an optional `(op, rhs)` row over the sum of the keys, further rows over the `s_<key>`
    variables, and a weighted objective."""
    variables, rows = {}, list(extra)
    for key, (lo, hi) in ranges.items():
        variables.update({f"s_{key}": (kind, min(lo, 0), max(hi, 0)), f"on_{key}": (_BIN, 0, 1)})
        rows += [({f"s_{key}": 1.0, f"on_{key}": -float(hi)}, "<=", 0), ({f"s_{key}": 1.0, f"on_{key}": -float(lo)}, ">=", 0)]
    if total:
        rows.append(({f"s_{k}": 1.0 for k in ranges}, total[0], total[1]))
    return _solve(oracle, variables, rows, {f"s_{k}": float(w) for k, w in weights.items()}, sense)


# ---------------------------------------------------------------------------
# One key, many spellings
# ---------------------------------------------------------------------------

@pytest.mark.var_integer
@pytest.mark.per_clause
@pytest.mark.correctness
def test_column_key_spellings_name_one_key(decidb_cli, oracle_solver):
    """§2.1: case, spacing and parentheses around a qualified name do not change a key
    (the plain `grp`, `(grp)` and `t.grp` spellings are pinned in the core file), so each
    spelling shares one decision per group: 3, 3, 2 (8). A spelling read as per row gives
    3, 7, 2 (12). DISTINCT reads exactly one value per key; per row it reads (a, 3),
    (a, 7), (b, 2)."""
    result = _keyed(oracle_solver, {1: "a", 2: "a", 3: "b"}, _CAP)
    for scope in ("per T.GRP", "Per ( t.grp ) ", "PER (T.Grp)"):
        _check(decidb_cli, f"SELECT id, x FROM {_T} DECIDE {scope}: x(INT) SUCH THAT x <= cap MAXIMIZE SUM(x)",
               ("id", "x"), [(1, 3), (2, 3), (3, 2)], result)
    _check(decidb_cli, f"SELECT DISTINCT grp, x FROM {_T} DECIDE PER grp: x(INT) SUCH THAT x <= cap MAXIMIZE SUM(x)",
           ("grp", "x"), [("a", 3), ("b", 2)], result, value=lambda got: 2 * got[0][1] + got[1][1])


@pytest.mark.var_integer
@pytest.mark.per_clause
@pytest.mark.correctness
def test_query_wide_key_spellings_make_one_decision(decidb_cli, oracle_solver):
    """§2.1, §6: `PER():` and `per ( ) :` spell `PER ()` (pinned in the core file): one
    decision that the bare objective `x` may read, capped by every row's cap (2). Read as
    per row, the objective `x` is refused and `x <= cap` would allow 3, 7, 2."""
    result = _solve(oracle_solver, {"x": (_INT, 0, None)}, [({"x": 1.0}, "<=", c) for c in _CAP.values()], {"x": 1.0})
    for scope in ("PER(): ", "per ( ) : "):
        _check(decidb_cli, f"SELECT id, x FROM {_T} DECIDE {scope}x(INT) SUCH THAT x <= cap MAXIMIZE x",
               ("id", "x"), [(1, 2), (2, 2), (3, 2)], result, value=lambda got: got[0][1])


@pytest.mark.var_integer
@pytest.mark.per_clause
@pytest.mark.correctness
def test_multi_column_key_over_one_relation_in_any_column_order(decidb_cli, oracle_solver):
    """§2.1, deck p7: `PER a, b` keys on the pair, so (p, q) shares one decision over two
    rows while (p, r) and (s, q) are their own: 3, 3, 2, 9 (17). Keying on `a` alone
    would give 2, 2, 2, 9 (15), on `b` alone 3, 3, 2, 3 (11); per row 3, 7, 2, 9 (21).
    Parentheses and column order do not change the key."""
    src = "(VALUES (1, 'p', 'q', 3), (2, 'p', 'q', 7), (3, 'p', 'r', 2), (4, 's', 'q', 9)) t(id, a, b, cap)"
    result = _keyed(oracle_solver, {1: "pq", 2: "pq", 3: "pr", 4: "sq"}, {1: 3, 2: 7, 3: 2, 4: 9})
    for scope in ("PER a, b", "PER (a, b)", "PER t.b, t.a"):
        _check(decidb_cli, f"SELECT id, x FROM {src} DECIDE {scope}: x(INT) SUCH THAT x <= cap MAXIMIZE SUM(x)",
               ("id", "x"), [(1, 3), (2, 3), (3, 2), (4, 9)], result)


@pytest.mark.var_integer
@pytest.mark.per_clause
@pytest.mark.sql_joins
@pytest.mark.correctness
def test_relation_and_column_mixed_key_shares_only_the_full_tuple(decidb_cli, oracle_solver):
    """§2.1-2.2: `PER t, u.k: x(INT) <= t.cap` keys on t's tuple plus u.k, so the two
    (2, m) rows share one decision while (1, m) and (1, n) do not, and t.cap is a legal
    bound (t lies wholly in the key): 5, 2, 3, 3 (13). Per-row decisions would put 8 on
    (2, m, v = 1) (18); `PER t` alone would drag (1, m) down to 2 (10)."""
    src = ("(VALUES (1, 5), (2, 8)) t(id, cap) "
           "JOIN (VALUES (1, 'm', 1), (1, 'n', 1), (2, 'm', 1), (2, 'm', 2)) u(id, k, v) USING (id)")
    result = _solve(oracle_solver, {"x_1m": (_INT, 0, 5), "x_1n": (_INT, 0, 5), "x_2m": (_INT, 0, 8)},
                    [({"x_1n": 1.0}, "<=", 2), ({"x_2m": 1.0}, "<=", 3)],
                    {"x_1m": 1.0, "x_1n": 1.0, "x_2m": 2.0})
    for scope in ("PER t, u.k", "PER (t, u.k)"):
        _check(decidb_cli, f"""
            SELECT t.id, u.k, u.v, x FROM {src} DECIDE {scope}: x(INT) <= t.cap
            SUCH THAT WHEN k = 'n': x <= 2 AND WHEN v = 2: x <= 3 MAXIMIZE SUM(x)
        """, ("id", "k", "v", "x"), [(1, "m", 1, 5), (1, "n", 1, 2), (2, "m", 1, 3), (2, "m", 2, 3)], result)


@pytest.mark.error
@pytest.mark.error_binder
def test_relation_alias_is_only_for_a_one_relation_key(decidb_cli):
    """§2.1: `D.x` is sanctioned only when x's key is exactly the relation D; a
    column-keyed or a per-row decision has no relation alias. (The same two queries
    with `PER D:` / `PER t:` bind and solve.)"""
    decidb_cli.assert_error("""
        SELECT D.id, D.x FROM (VALUES (1, 3), (2, 7)) D(id, cap)
        DECIDE PER D.id: x(INT) SUCH THAT x <= cap MAXIMIZE SUM(x)
    """, match=r'column named "x"')
    decidb_cli.assert_error(f"SELECT id, t.x FROM {_T} DECIDE x(INT) SUCH THAT t.x <= cap MAXIMIZE SUM(t.x)",
                            match=r"DECIDE variables")


@pytest.mark.error
@pytest.mark.error_binder
def test_a_key_naming_an_unknown_name_or_a_reducer_over_a_query_wide_decision_is_refused(decidb_cli):
    """§2.1, §4: a key names columns or relations of the FROM clause; `SUM(c)` over a
    `PER ()` decision has nothing to reduce (write c)."""
    decidb_cli.assert_error(f"SELECT id, x FROM {_T} DECIDE PER nope: x(INT) SUCH THAT x <= cap MAXIMIZE SUM(x)",
                            match=r"neither a column nor a relation")
    decidb_cli.assert_error(f"SELECT id, c FROM {_T} DECIDE PER (): c(INT) SUCH THAT c <= cap MAXIMIZE SUM(c)",
                            match=r"nothing to aggregate")


# ---------------------------------------------------------------------------
# Reads through a table key
# ---------------------------------------------------------------------------

@pytest.mark.var_integer
@pytest.mark.per_clause
@pytest.mark.sql_joins
@pytest.mark.correctness
def test_decisions_are_determined_through_a_primary_key(decidb_cli, oracle_solver):
    """§3.1 (delta 6): a row decision binds under `PER d.id` when id is d's PRIMARY KEY,
    and a `PER d` decision under `PER d.depot: x <= cost` when depot is d's PRIMARY KEY.
    Over a key, `PER d.id` is the same as one instance per row (3, 7, 2) and `PER
    d.depot` the same as reading cost per row (10, 7, 7), so what discriminates is the
    binding, and the test pins both sides of it: the same two queries with the PRIMARY
    KEY dropped are refused ("does not identify" / "does not determine"). A UNIQUE
    column determines a row like a PRIMARY KEY. Each x takes its own depot's cost;
    applying the tightest cost to every row would give 7, 7, 7."""
    row_sql = """
        CREATE TEMP TABLE d(id INT {key}, grp VARCHAR, cap INT); INSERT INTO d VALUES (1, 'a', 3), (2, 'a', 7), (3, 'b', 2);
        SELECT id, x FROM d DECIDE x(INT) SUCH THAT PER d.id: x <= cap MAXIMIZE SUM(x);
    """
    pk_row = _keyed(oracle_solver, {i: i for i in _CAP}, _CAP)
    for key in ("PRIMARY KEY", "UNIQUE"):
        _check(decidb_cli, row_sql.format(key=key), ("id", "x"), [(1, 3), (2, 7), (3, 2)], pk_row)
    decidb_cli.assert_error(row_sql.format(key=""), match=r"does not identify")
    rel_sql = ("SELECT id, x FROM r JOIN d USING (depot) DECIDE PER d: x(INT) "
               "SUCH THAT PER d.depot: x <= cost MAXIMIZE SUM(x);")
    pk_rel = _keyed(oracle_solver, {1: "a", 2: "b", 3: "b"}, {1: 10, 2: 7, 3: 7})
    _check(decidb_cli, _PK_DEPOTS + rel_sql, ("id", "x"), [(1, 10), (2, 7), (3, 7)], pk_rel)
    decidb_cli.assert_error(_PK_DEPOTS.replace(" PRIMARY KEY", "") + rel_sql, match=r"does not determine")


@pytest.mark.error
@pytest.mark.error_binder
@pytest.mark.per_clause
def test_a_read_the_key_does_not_determine_is_refused_with_its_repair(decidb_cli):
    """§3.1 (delta 6): a keyed decision under `PER ()` or under a key that does not
    determine its own, and a row decision under a column key, are refused with the
    decision-specific repair; a bound column of a base table whose PRIMARY KEY the key
    does not cover is refused with the table-key repair, which a VALUES list never gets."""
    for decl, prefix, topic in (("PER grp: x(INT)", "PER ()", r"whole query"),
                                ("PER grp: x(INT)", "PER cap", r"does not determine"),
                                ("x(INT)", "PER grp", r"does not identify")):
        decidb_cli.assert_error(f"SELECT id, x FROM {_T} DECIDE {decl} SUCH THAT {prefix}: x <= 5 MAXIMIZE SUM(x)",
                                match=topic)
    decidb_cli.assert_error("""
        CREATE TEMP TABLE d(id INT PRIMARY KEY, grp VARCHAR, cap INT); INSERT INTO d VALUES (1, 'a', 3), (2, 'a', 7);
        SELECT id, x FROM d DECIDE PER grp: x(INT) <= cap SUCH THAT x >= 0 MAXIMIZE SUM(x);
    """, match=r"declare a PRIMARY KEY")
    values = decidb_cli.execute_raw(f"SELECT id, x FROM {_T} DECIDE PER grp: x(INT) <= cap SUCH THAT x >= 0 MAXIMIZE SUM(x)")
    assert "not determined" in values.stderr and "PRIMARY KEY" not in values.stderr, values.stderr


@pytest.mark.var_boolean
@pytest.mark.var_integer
@pytest.mark.per_clause
@pytest.mark.sql_joins
@pytest.mark.correctness
def test_opening_cost_once_per_depot_over_values_and_through_a_primary_key_column(decidb_cli, oracle_solver):
    """§4, deck p21: `SUM(PER grp, cost: cost * open)` charges each depot once, so depot b
    (two routes, cost 7) beats a (10) and opens; `SUM(cost * open)` charges b on both
    rows (14), so a opens instead. Keyed on the PRIMARY KEY column alone, `PER d.depot`
    reads cost through the schema and gives the once-per-depot answer; dropping its
    reducer PER opens a, and dropping the PRIMARY KEY refuses the reducer's body (cost
    is no longer determined by d.depot)."""
    depots = "(VALUES (1, 'a', 10, 6), (2, 'b', 7, 3), (3, 'b', 7, 3)) t(id, grp, cost, cap)"
    open_b = [(1, "a", 0, 0), (2, "b", 1, 3), (3, "b", 1, 3)]
    open_a = [(1, "a", 1, 6), (2, "b", 0, 0), (3, "b", 0, 0)]
    sql = """
        {setup} SELECT id, {grp} AS grp, open, x FROM {source}
        DECIDE {key}: open(BOOL), x(INT) SUCH THAT x <= cap * open AND PER (): SUM(x) >= 6 MINIMIZE {reducer};
    """
    for setup, source, key, reducer, cost_b, want in (
        ("", depots, "PER grp, cost", "SUM(PER grp, cost: cost * open)", 7, open_b),
        ("", depots, "PER grp, cost", "SUM(cost * open)", 14, open_a),
        (_PK_DEPOTS, "r JOIN d USING (depot)", "PER d.depot", "SUM(PER d.depot: cost * open)", 7, open_b),
        (_PK_DEPOTS, "r JOIN d USING (depot)", "PER d.depot", "SUM(cost * open)", 14, open_a),
    ):
        variables = {"open_a": (_BIN, 0, 1), "open_b": (_BIN, 0, 1)}
        variables.update({f"x_{i}": (_INT, 0, None) for i in (1, 2, 3)})
        rows = [({"x_1": 1.0, "open_a": -6.0}, "<=", 0), ({"x_2": 1.0, "open_b": -3.0}, "<=", 0),
                ({"x_3": 1.0, "open_b": -3.0}, "<=", 0), ({"x_1": 1.0, "x_2": 1.0, "x_3": 1.0}, ">=", 6)]
        result = _solve(oracle_solver, variables, rows, {"open_a": 10.0, "open_b": float(cost_b)}, _MIN)
        _check(decidb_cli, sql.format(setup=setup, grp="depot" if setup else "grp", source=source, key=key,
                                      reducer=reducer), ("id", "grp", "open", "x"), want, result,
               value=lambda got, c=cost_b: sum({"a": 10, "b": c}[g] for g in {g for _, g, o, _ in got if o}))
    decidb_cli.assert_error(sql.format(setup=_PK_DEPOTS.replace(" PRIMARY KEY", ""), grp="depot",
                                       source="r JOIN d USING (depot)", key="PER d.depot",
                                       reducer="SUM(PER d.depot: cost * open)"), match=r"declare a PRIMARY KEY")


# ---------------------------------------------------------------------------
# Bounds: table keys, clause order, the wire, the objective, self-reference
# ---------------------------------------------------------------------------

@pytest.mark.var_integer
@pytest.mark.per_clause
@pytest.mark.correctness
def test_column_bound_through_unique_split_order_and_the_serializer(decidb_cli, oracle_solver):
    """§2.2, §3.1, deck p9: a column bound is legal under a UNIQUE column the key covers
    (without UNIQUE it is refused as not determined) and in the split clause order
    (without the bound the query is unbounded): each x caps at its own cap (3, 7, 2).
    `BETWEEN -4 AND cap` (a negative constant beside a keyed column) survives the plan's
    serialization round-trip at both ends: under `MAXIMIZE SUM(x * (cap - 4))` rows 1 and
    3 sit on the floor and row 2 on its cap, -4, 7, -4 (33). A floor lost on the wire gives
    0, 7, 0 (21); a cap lost gives -4, 9, -4 (39) under the row `x <= 9`, which, unlike
    `x >= -4`, cannot restate the floor."""
    result = _keyed(oracle_solver, {i: i for i in _CAP}, _CAP)
    unique = ("CREATE TEMP TABLE d(id INT, grp VARCHAR UNIQUE, cap INT); INSERT INTO d VALUES (1, 'a', 3), (2, 'b', 7), (3, 'c', 2);"
              " SELECT id, x FROM d DECIDE PER grp: x(INT) <= cap SUCH THAT x >= 0 MAXIMIZE SUM(x);")
    for sql in (
        unique,
        f"SELECT id, x DECIDE PER grp, cap: x(INT) BETWEEN 0 AND cap FROM {_T} SUCH THAT x >= 0 MAXIMIZE SUM(x)",
    ):
        _check(decidb_cli, sql, ("id", "x"), [(1, 3), (2, 7), (3, 2)], result)
    decidb_cli.assert_error(unique.replace(" UNIQUE", ""), match=r"not determined")
    wire = _solve(oracle_solver, {f"x_{i}": (_INT, -4, c) for i, c in _CAP.items()}, [({f"x_{i}": 1.0}, "<=", 9) for i in _CAP],
                  {f"x_{i}": float(c - 4) for i, c in _CAP.items()})
    _check(decidb_cli.with_verify_serializer(),
           f"SELECT id, x FROM {_T} DECIDE PER grp, cap: x(INT) BETWEEN -4 AND cap SUCH THAT x <= 9 MAXIMIZE SUM(x * (cap - 4))",
           ("id", "x"), [(1, -4), (2, 7), (3, -4)], wire, value=lambda got: sum(x * (_CAP[i] - 4) for i, x in got))


@pytest.mark.var_integer
@pytest.mark.var_real
@pytest.mark.correctness
def test_negative_constant_floor_on_real_and_query_wide_decisions(decidb_cli, oracle_solver):
    """§2.2: REAL is `>= 0` unless bounded below, so `BETWEEN -1.5 AND 2.5` minimizes to
    -1.5 and maximizes to the fractional ceiling where the cap is above it (2.5, 2.5,
    2.0); `PER (): x(INT) >= -2` minimizes the one decision to -2 under the bare
    objective `x`. A floor stuck at 0 (the bound dropped) gives 0 in both minima and a
    lost ceiling 3, 7, 2; `between` reads the same in lower case."""
    sql = f"SELECT id, x FROM {_T} DECIDE x(REAL) between -1.5 and 2.5 SUCH THAT x <= cap {{sense}} SUM(x)"
    for sense, name, want in ((_MIN, "MINIMIZE", [(1, -1.5), (2, -1.5), (3, -1.5)]),
                              (_MAX, "MAXIMIZE", [(1, 2.5), (2, 2.5), (3, 2.0)])):
        real = _solve(oracle_solver, {f"x_{i}": (_REAL, -1.5, 2.5) for i in _CAP},
                      [({f"x_{i}": 1.0}, "<=", c) for i, c in _CAP.items()], {f"x_{i}": 1.0 for i in _CAP}, sense)
        _check(decidb_cli, sql.format(sense=name), ("id", "x"), [(i, pytest.approx(x)) for i, x in want], real)
    wide = _solve(oracle_solver, {"x": (_INT, -2, None)}, [({"x": 1.0}, "<=", c) for c in _CAP.values()], {"x": 1.0}, _MIN)
    _check(decidb_cli, f"SELECT id, x FROM {_T} DECIDE PER (): x(INT) >= -2 SUCH THAT x <= cap MINIMIZE x",
           ("id", "x"), [(1, -2), (2, -2), (3, -2)], wide, value=lambda got: got[0][1])


@pytest.mark.var_integer
@pytest.mark.per_clause
@pytest.mark.correctness
def test_query_wide_bound_scales_into_the_objective(decidb_cli, oracle_solver):
    """§2.2, §6 (delta 10): `PER (): x(INT) <= 2` bounds one decision and `MAXIMIZE 3 * x
    + y` is linear arithmetic over two query-wide decisions that share `2 * x + y <= 6`.
    The scale makes x worth its share of the budget up to its bound: x = 2, y = 2 (8).
    Dropping the scale (`x + y`) gives x = 0, y = 6; dropping the bound gives x = 3,
    y = 0 (9)."""
    result = _solve(oracle_solver, {"x": (_INT, 0, 2), "y": (_INT, 0, None)}, [({"x": 2.0, "y": 1.0}, "<=", 6)],
                    {"x": 3.0, "y": 1.0})
    _check(decidb_cli, f"""
        SELECT id, x, y FROM {_T} DECIDE PER (): x(INT) <= 2, PER (): y(INT)
        SUCH THAT PER (): 2 * x + y <= 6 MAXIMIZE 3 * x + y
    """, ("id", "x", "y"), [(1, 2, 2), (2, 2, 2), (3, 2, 2)], result, value=lambda got: 3 * got[0][1] + got[0][2])


@pytest.mark.error
@pytest.mark.error_binder
def test_bound_may_not_read_the_declarator_itself_or_an_aggregate_inside_between(decidb_cli):
    """Deck p9, spec §4.1: a bound is a constant or a key-determined column; the
    declarator's own name, a decision declared earlier, or an aggregate as a BETWEEN
    endpoint are refused as declaration bounds."""
    for decl in ("x(INT) BETWEEN 0 AND x, y(INT)", "y(INT) <= 3, x(INT) <= y", "x(INT) BETWEEN 0 AND MAX(cap), y(INT)"):
        decidb_cli.assert_error(f"SELECT id, x, y FROM {_T} DECIDE {decl} SUCH THAT x >= 1 MAXIMIZE SUM(x)",
                                match=r"declaration bound")


# ---------------------------------------------------------------------------
# Domains: BOOL, SEMIINT, SEMIREAL, TEXT
# ---------------------------------------------------------------------------

@pytest.mark.var_boolean
@pytest.mark.var_integer
@pytest.mark.correctness
def test_bool_negative_floor_minimizes_to_zero_and_product_caps_stay_binary(decidb_cli, oracle_solver):
    """§2.2 (delta 4): `o >= -1` beside a BOOL (declaration plus row, row alone, or a `PER
    ()` BOOL under `MINIMIZE 3 * o`) minimizes to 0; an INT in the same spot reaches -1
    in all three (a constant `SUCH THAT` floor widens an INT's box like a declared one).
    Under `BETWEEN -3 AND 3` the product `cap * o` still caps x at cap: 3, 7, 2 with
    o = 1 (12 - 3 = 9); an INT o would take 3 and x = 9, 21, 6 (36 - 9 = 27)."""
    per_row = _solve(oracle_solver, {f"o_{i}": (_BIN, 0, 1) for i in _CAP}, [({f"o_{i}": 1.0}, ">=", -1) for i in _CAP],
                     {f"o_{i}": 1.0 for i in _CAP}, _MIN)
    for decl in ("o(BOOL) >= -1", "o(BOOL)"):
        _check(decidb_cli, f"SELECT id, o FROM {_T} DECIDE {decl} SUCH THAT o >= -1 MINIMIZE SUM(o)",
               ("id", "o"), [(1, 0), (2, 0), (3, 0)], per_row)
    one = _boxes(oracle_solver, {"o": (0, 1)}, _MIN, weights={"o": 3}, kind=_BIN)
    _check(decidb_cli, f"SELECT id, o FROM {_T} DECIDE PER (): o(BOOL) >= -1 SUCH THAT o >= -1 MINIMIZE 3 * o",
           ("id", "o"), [(1, 0), (2, 0), (3, 0)], one, value=lambda got: 3 * got[0][1])
    variables = {f"o_{i}": (_BIN, 0, 1) for i in _CAP}
    variables.update({f"x_{i}": (_INT, 0, None) for i in _CAP})
    objective = {f"x_{i}": 1.0 for i in _CAP}
    objective.update({f"o_{i}": -1.0 for i in _CAP})
    product = _solve(oracle_solver, variables, [({f"x_{i}": 1.0, f"o_{i}": -float(c)}, "<=", 0) for i, c in _CAP.items()],
                     objective)
    _check(decidb_cli, f"""
        SELECT id, o, x FROM {_T} DECIDE o(BOOL) BETWEEN -3 AND 3, x(INT)
        SUCH THAT x <= cap * o AND o >= -2 MAXIMIZE SUM(x) - SUM(o)
    """, ("id", "o", "x"), [(1, 1, 3), (2, 1, 7), (3, 1, 2)], product, value=lambda got: sum(x - o for _, o, x in got))


@pytest.mark.var_integer
@pytest.mark.correctness
def test_semiint_constant_range_switches_off_where_the_cap_is_below_it(decidb_cli, oracle_solver):
    """§2.2: `s(SEMIINT) BETWEEN 3 AND 9` with `s <= cap`: caps 3 and 7 take 3 and 7, cap 2
    admits only the off value 0 (10). Without the zero branch (`INT BETWEEN 3 AND 9`) the
    query is infeasible; without the floor (`INT <= 9`) row 3 takes 2 (12). The domain
    word reads the same in mixed case."""
    result = _semi(oracle_solver, {i: (3, 9) for i in _CAP}, _INT, {i: 1 for i in _CAP}, _MAX,
                   extra=[({f"s_{i}": 1.0}, "<=", c) for i, c in _CAP.items()])
    for domain in ("SEMIINT", "SemiInt"):
        _check(decidb_cli, f"SELECT id, s FROM {_T} DECIDE s({domain}) BETWEEN 3 AND 9 SUCH THAT s <= cap MAXIMIZE SUM(s)",
               ("id", "s"), [(1, 3), (2, 7), (3, 0)], result)


@pytest.mark.var_integer
@pytest.mark.per_clause
@pytest.mark.correctness
def test_semiint_keyed_column_range_survives_the_serializer(decidb_cli, oracle_solver):
    """§2.2, deck p11: `PER grp, lo, hi: s(SEMIINT) BETWEEN lo AND hi` is 0 or in [lo, hi]
    per key, and the keyed SEMI domain survives the plan's serialization round-trip.
    Needing 2 in total, the cheapest is s_a = 3 (its floor when on): a plain [0, hi] box
    (`INT <= hi`) answers s_b = 2, a hard floor (`INT BETWEEN lo AND hi`) forces 3 + 5 = 8.
    The core file pins the same shape without the round-trip."""
    result = _semi(oracle_solver, {"a": (3, 9), "b": (5, 6)}, _INT, {"a": 1, "b": 1}, _MIN, total=(">=", 2))
    _check(decidb_cli.with_verify_serializer(), """
        SELECT id, grp, s FROM (VALUES (1, 'a', 3, 9), (2, 'a', 3, 9), (3, 'b', 5, 6)) t(id, grp, lo, hi)
        DECIDE PER grp, lo, hi: s(SEMIINT) BETWEEN lo AND hi
        SUCH THAT PER (): SUM(PER grp, lo, hi: s) >= 2 MINIMIZE SUM(PER grp, lo, hi: s)
    """, ("id", "grp", "s"), [(1, "a", 3), (2, "a", 3), (3, "b", 0)], result, value=lambda got: got[0][2] + got[2][2])


@pytest.mark.var_real
@pytest.mark.per_clause
@pytest.mark.correctness
def test_semireal_keyed_range_is_one_value_across_a_generator_hours(decidb_cli, oracle_solver):
    """Deck p11 generator dispatch over a join-shaped input: `PER gen, ...: p(SEMIREAL)
    BETWEEN lo AND hi` is off or within the stable range, one value per generator
    repeated on its two hours. To supply 3 the cheap generator runs at its fractional
    floor 3.5 (cost 3.5); a plain [0, hi] box would run it at 3.0 (cost 3), a hard floor
    would run both (11), SEMIINT would round the floor up to 4, and a per-row p runs
    one hour only (0.0 and 3.5 on g2)."""
    result = _semi(oracle_solver, {"g1": (2.5, 6.0), "g2": (3.5, 9.0)}, _REAL, {"g1": 3, "g2": 1}, _MIN, total=(">=", 3))
    _check(decidb_cli, """
        SELECT gen, hour, p FROM (VALUES ('g1', 2.5, 6.0, 3, 1), ('g1', 2.5, 6.0, 3, 2),
                                         ('g2', 3.5, 9.0, 1, 1), ('g2', 3.5, 9.0, 1, 2)) t(gen, lo, hi, cost, hour)
        DECIDE PER gen, lo, hi, cost: p(SEMIREAL) BETWEEN lo AND hi
        SUCH THAT PER (): SUM(PER gen, lo, hi, cost: p) >= 3 MINIMIZE SUM(PER gen, lo, hi, cost: p * cost)
    """, ("gen", "hour", "p"), [("g1", 1, pytest.approx(0.0)), ("g1", 2, pytest.approx(0.0)),
                                ("g2", 1, pytest.approx(3.5)), ("g2", 2, pytest.approx(3.5))], result,
           value=lambda got: 3 * got[0][2] + got[2][2])


@pytest.mark.var_real
@pytest.mark.var_integer
@pytest.mark.correctness
def test_semi_negative_constant_range_widens_the_box(decidb_cli, oracle_solver):
    """§2.2: a negative constant floor widens a SEMI box: `s(SEMIREAL) BETWEEN -4 AND -1`
    is {0} ∪ [-4, -1], so the minimum is -4.0 on every row (a floor clamped at 0 leaves
    only 0). Under `s <= cap - 5` (-2, 2, -3) the SEMIINT maximum is -2, the off value 0,
    -3 (-5): without the zero branch (`INT BETWEEN -4 AND -1`) row 2 takes -1 (-6), and
    a floor clamped at 0 leaves rows 1 and 3 infeasible."""
    lo = _semi(oracle_solver, {i: (-4, -1) for i in _CAP}, _REAL, {i: 1 for i in _CAP}, _MIN,
               extra=[({f"s_{i}": 1.0}, "<=", 0) for i in _CAP])
    _check(decidb_cli, f"SELECT id, s FROM {_T} DECIDE s(SEMIREAL) BETWEEN -4 AND -1 SUCH THAT s <= 0 MINIMIZE SUM(s)",
           ("id", "s"), [(1, -4.0), (2, -4.0), (3, -4.0)], lo)
    hi = _semi(oracle_solver, {i: (-4, -1) for i in _CAP}, _INT, {i: 1 for i in _CAP}, _MAX,
               extra=[({f"s_{i}": 1.0}, "<=", c - 5) for i, c in _CAP.items()])
    _check(decidb_cli, f"SELECT id, s FROM {_T} DECIDE s(SEMIINT) BETWEEN -4 AND -1 SUCH THAT s <= cap - 5 MAXIMIZE SUM(s)",
           ("id", "s"), [(1, -2), (2, 0), (3, -3)], hi)


@pytest.mark.var_boolean
@pytest.mark.var_integer
@pytest.mark.per_clause
@pytest.mark.correctness
def test_keyed_text_mode_is_shared_by_rows_that_want_different_modes(decidb_cli, oracle_solver):
    """§2.2, deck p58: `PER grp: mode(TEXT IN ['lo', 'hi'])` is one string per key, read
    back on every row of the key. Row 1 (weight -1) would rather be 'lo' and row 2
    (weight 1) 'hi'; sharing one mode, group a picks 'hi' and pays x = 2 on row 1: 7.
    A per-row mode would reach 9. The keyed TEXT domain survives the serialization
    round-trip and DISTINCT reads one mode per key."""
    src = "(VALUES (1, 'a', 3, -1), (2, 'a', 7, 1), (3, 'b', 2, 1)) t(id, grp, cap, w)"
    variables = {f"{v}_{g}": (_BIN, 0, 1) for g in ("a", "b") for v in ("lo", "hi")}
    rows = [({f"lo_{g}": 1.0, f"hi_{g}": 1.0}, "=", 1) for g in ("a", "b")]
    for i, (g, cap) in {1: ("a", 3), 2: ("a", 7), 3: ("b", 2)}.items():
        variables[f"x_{i}"] = (_INT, 0, cap)
        rows.append(({f"x_{i}": 1.0, f"hi_{g}": -float(cap - 1)}, "<=", 1))   # lo ⟹ x <= 1
        rows.append(({f"x_{i}": 1.0, f"hi_{g}": -2.0}, ">=", 0))              # hi ⟹ x >= 2
    result = _solve(oracle_solver, variables, rows, {"x_1": -1.0, "x_2": 1.0, "x_3": 1.0})
    sql = f"""
        SELECT id, grp, mode, x FROM {src} DECIDE PER grp: mode(TEXT IN ['lo', 'hi']), x(INT) <= cap
        SUCH THAT IF mode = 'lo': x <= 1 AND IF mode = 'hi': x >= 2 MAXIMIZE SUM(x * w)
    """
    _check(decidb_cli.with_verify_serializer(), sql, ("id", "grp", "mode", "x"),
           [(1, "a", "hi", 2), (2, "a", "hi", 7), (3, "b", "hi", 2)], result,
           value=lambda got: -got[0][3] + got[1][3] + got[2][3])
    _check(decidb_cli, sql.replace("SELECT id, grp, mode, x", "SELECT DISTINCT grp, mode"), ("grp", "mode"),
           [("a", "hi"), ("b", "hi")])


@pytest.mark.var_boolean
@pytest.mark.correctness
def test_text_values_compare_exactly_in_every_comparison_form(decidb_cli, oracle_solver):
    """§2.2: values compare exactly. `mode = 'A'` over `['a', 'A']` and `mode = 'a'` over
    `['A', 'a']` each leave the second-listed spelling (with no constraint the first is
    returned, so a dropped or case-folded comparison reads back the wrong one);
    `IN ('A')` leaves 'A'; `NOT IN ('a', 'b')` over three values leaves 'c' (the domain
    word reads the same in lower case); and 'A' is outside `['a', 'b']`, so comparing
    with it is refused. The oracle is the one-hot model (one indicator per listed value,
    sum 1, the indicators of values failing the Python string comparison pinned to 0),
    probed value by value: its feasible set must be exactly the value read back."""
    for values, cons, keep, decl in (
        (["a", "A"], "mode = 'A'", lambda v: v == "A", "mode(TEXT IN ['a', 'A'])"),
        (["A", "a"], "mode = 'a'", lambda v: v == "a", "mode(TEXT IN ['A', 'a'])"),
        (["a", "A"], "mode IN ('A')", lambda v: v in ("A",), "mode(TEXT IN ['a', 'A'])"),
        (["a", "b", "c"], "mode NOT IN ('a', 'b')", lambda v: v not in ("a", "b"), "PER (): mode(text in ['a', 'b', 'c'])"),
    ):
        feasible = []
        for probe in values:
            oracle_solver.create_model(f"text_{''.join(values)}_{probe}")
            for v in values:
                oracle_solver.add_variable(f"is_{v}", _BIN, lb=0.0, ub=1.0)
                if not keep(v):
                    oracle_solver.add_constraint({f"is_{v}": 1.0}, "<=", 0.0)
            oracle_solver.add_constraint({f"is_{v}": 1.0 for v in values}, "=", 1.0)
            oracle_solver.set_objective({f"is_{probe}": 1.0}, _MAX)
            result = oracle_solver.solve()
            if result.status == SolverStatus.OPTIMAL and result.objective_value > 0.5:
                feasible.append(probe)
        (want,) = feasible
        _check(decidb_cli, f"SELECT id, mode FROM {_T} DECIDE {decl} SUCH THAT {cons} SATISFY",
               ("id", "mode"), [(1, want), (2, want), (3, want)])
    decidb_cli.assert_error(f"SELECT id, mode FROM {_T} DECIDE mode(TEXT IN ['a', 'b']) SUCH THAT mode = 'A' SATISFY",
                            match=r"has no value")


# ---------------------------------------------------------------------------
# Parser refusals that name the fix, in every form
# ---------------------------------------------------------------------------

@pytest.mark.error
@pytest.mark.error_parser
def test_trailing_key_after_a_one_sided_bound_is_refused(decidb_cli):
    """§2.1, OPEN-1: the deck's trailing key (p9, p74) is retired after a one-sided
    bound (either side) as much as after BETWEEN; the refusal says the key goes before
    the name."""
    for bound in (">= 1", "<= 5"):
        decidb_cli.assert_error(f"SELECT id, x FROM {_T} DECIDE x(INT) {bound} PER grp SUCH THAT x <= cap MAXIMIZE SUM(x)",
                                match=r"before its name")


@pytest.mark.error
@pytest.mark.error_parser
def test_between_and_floor_bounds_on_a_text_declarator_are_refused(decidb_cli):
    """§2.1: a TEXT decision's domain is its list, so BETWEEN and `>=` are refused like `<=`."""
    for bound in ("BETWEEN 1 AND 5", ">= 1"):
        decidb_cli.assert_error(f"SELECT id, mode FROM {_T} DECIDE mode(TEXT IN ['a', 'b']) {bound} SUCH THAT mode = 'a' SATISFY",
                                match=r"takes no BETWEEN")


@pytest.mark.error
@pytest.mark.error_parser
def test_every_wrong_domain_word_names_the_six_domains(decidb_cli):
    """§2.1: VARCHAR, FLOAT and BIGINT are DuckDB types, not DECIDE domains, and the
    error names the six; a bare `x` and a `TEXT` without its list get their own hints."""
    for word in ("VARCHAR", "FLOAT", "BIGINT"):
        decidb_cli.assert_error(f"SELECT id, x FROM {_T} DECIDE x({word}) SUCH THAT x <= cap MAXIMIZE SUM(x)",
                                match=r"not a DECIDE domain")
    decidb_cli.assert_error(f"SELECT id, x FROM {_T} DECIDE x SUCH THAT x <= cap MAXIMIZE SUM(x)", match=r"needs a domain")
    decidb_cli.assert_error(f"SELECT id, x FROM {_T} DECIDE x(TEXT) SUCH THAT x = 'a' SATISFY", match=r"lists its values")


# ---------------------------------------------------------------------------
# EXPLAIN: scopes listed, switches hidden, bounds shown as the rows they became
# ---------------------------------------------------------------------------

def _explain_decide(decidb_cli, sql):
    """The DECIDE node's extra_info from `EXPLAIN (FORMAT JSON)`."""
    out = decidb_cli.execute_raw(f"EXPLAIN (FORMAT JSON) {sql}").stdout
    plan = json.loads(out[out.find("["):])

    def walk(nodes):
        for node in nodes:
            if node.get("name") == "DECIDE":
                return node["extra_info"]
            found = walk(node.get("children", []))
            if found:
                return found
        return None

    return walk(plan)


@pytest.mark.explain
def test_explain_json_lists_scopes_and_shows_bounds_as_rows(decidb_cli):
    """§2.2, §8: the DECIDE node's declaration list names each decision with its scope
    (`x PER grp`, `cap2 PER ()`, a bare name for `PER ROW` and the default) and no hidden
    switch; a declaration bound appears as the keyed rows it became (`PER grp: x >= 1`,
    `PER grp: x <= 4`), the SEMI switch only in the rows its declaration became, and the
    TEXT indicators only in the indented rewritten rows beneath a clause written as
    `mode = 'b'`."""
    info = _explain_decide(decidb_cli, f"""
        SELECT id, x FROM {_T}
        DECIDE PER grp: x(INT) BETWEEN 1 AND 4, y(INT), PER (): cap2(INT), s(SEMIINT) BETWEEN 3 AND 9,
               PER grp: mode(TEXT IN ['a', 'b']), PER ROW: r(REAL)
        SUCH THAT x <= cap AND y <= cap AND cap2 <= 4 AND s >= 1 AND mode = 'b' AND r <= 1 MAXIMIZE SUM(x)
    """)
    assert info["Variables"] == ["x PER grp", "y", "cap2 PER ()", "s", "mode PER grp", "r"]
    assert not any("__" in v for v in info["Variables"])
    lines = info["Constraints"]
    assert {"x <= cap", "mode = 'b'", "PER grp: x <= 4", "PER grp: x >= 1"} <= set(lines), lines
    assert any("__semi_on_s__" in line for line in lines), lines
    assert all(line.startswith(" ") for line in lines if "__text_" in line), lines
