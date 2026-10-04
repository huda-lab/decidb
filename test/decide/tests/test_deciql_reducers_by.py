"""Reducers and their `BY` groups (DeciQL spec §4.2, §6.1, §7.1; deck p16-p32).

A reducer `agg([WHEN θ] [PER K]: e) [BY (Γ)]` consumes the Γ-class containing the
generated instance; omitted `BY` is `BY ()`, the whole WHEN-filtered input. The prefix
says how many instances exist, `BY` which rows each reducer reads (deck p16, "the per
Trap"). Every correctness test states the same problem in gurobipy and compares the
objective and, the optimum being unique, the decision vector. Dataset A is
`t(id, grp, cap, w)` = (1,a,3,1), (2,a,7,2), (3,b,2,1), (4,b,5,3) with `x <= cap` and
`MAXIMIZE SUM(x * w)` unless stated: weight 3 sits on row 4 and weight 2 on row 2.
"""

import re

import pytest

from solver.types import ObjSense, SolverStatus, VarType

_A = "(VALUES (1,'a',3,1),(2,'a',7,2),(3,'b',2,1),(4,'b',5,3)) t(id, grp, cap, w)"
_A_ROWS = {1: ("a", 3, 1), 2: ("a", 7, 2), 3: ("b", 2, 1), 4: ("b", 5, 3)}
_GROUP = {"a": (1, 2), "b": (3, 4)}

# Dataset F adds a second key column: t(id, grp, flag, cap), objective SUM(x * id).
_F = ("(VALUES (1,'a',true,3),(2,'a',false,7),(3,'a',true,4),(4,'b',true,2),(5,'b',false,5)) "
      "t(id, grp, flag, cap)")
_F_ROWS = {1: ("a", True, 3), 2: ("a", False, 7), 3: ("a", True, 4), 4: ("b", True, 2), 5: ("b", False, 5)}


def _rows(decidb_cli, sql, *cols):
    rows, names = decidb_cli.execute(sql)
    idx = [names.index(c) for c in cols]
    return sorted(tuple(r[i] for i in idx) for r in rows)


def _xs(got):
    """The decision vector in id order from `(id, x)` rows."""
    return [int(x) for _, x in got]


def _a_value(got):
    return sum(int(x) * _A_ROWS[i][2] for i, x in got)


def _lin(*parts):
    """Sum coefficient dicts, so `S_g - 0.5 * T` is `_lin(_sum('a'), _total(-0.5))`."""
    return {k: sum(p.get(k, 0.0) for p in parts) for part in parts for k in part}


def _sum(group, coef=1.0):
    return {f"x_{i}": coef for i in _GROUP[group]}


def _total(coef=1.0):
    return {f"x_{i}": coef for i in _A_ROWS}


def _a_model(oracle_solver, name):
    """Dataset A's decisions: x_i integer in [0, cap_i]."""
    oracle_solver.create_model(name)
    for i, (_, cap, _) in _A_ROWS.items():
        oracle_solver.add_variable(f"x_{i}", VarType.INTEGER, lb=0.0, ub=float(cap))


def _a_solve(oracle_solver, sense=ObjSense.MAXIMIZE):
    """Objective SUM(x * w) over dataset A; returns the optimal oracle result."""
    oracle_solver.set_objective({f"x_{i}": float(w) for i, (_, _, w) in _A_ROWS.items()}, sense)
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL
    return result


def _oracle_a(oracle_solver, name, rows, sense=ObjSense.MAXIMIZE):
    """Dataset A with linear rows `(coeffs, op, rhs)`."""
    _a_model(oracle_solver, name)
    for coeffs, op, rhs in rows:
        oracle_solver.add_constraint(coeffs, op, float(rhs))
    return _a_solve(oracle_solver, sense)


def _oracle_xs(oracle_result, ids):
    return [round(oracle_result.variable_values[f"x_{i}"]) for i in sorted(ids)]


def _check_a(decidb_cli, sql, oracle_result, vector):
    """`vector` is the unique optimum, so the oracle must land on it as well;
    the query's rows must reproduce the oracle objective and that vector."""
    assert _oracle_xs(oracle_result, _A_ROWS) == vector
    got = _rows(decidb_cli, sql, "id", "x")
    assert _a_value(got) == pytest.approx(oracle_result.objective_value), sql
    assert _xs(got) == vector, sql


# --- Prefix and BY spellings ------------------------------------------------
@pytest.mark.var_integer
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_global_sum_prefix_spellings_agree(decidb_cli, oracle_solver):
    """Deck p16-p18: `SUM(x)`, `: SUM(: x) BY ( )`, `PER (): SUM(x) BY ()` and the
    fully explicit lower-case `per row: sum(per row: x) by ()` all mean the whole-input
    sum. Total <= 10 puts 5 on row 4 and 5 on row 2 (25); a spelling that read `BY ()`
    as a per-group key would allow 34."""
    result = _oracle_a(oracle_solver, "global_sum", [(_total(), "<=", 10)])
    for clause in ("SUM(x) <= 10", ": SUM(: x) BY ( ) <= 10", "PER (): SUM(x) BY () <= 10",
                   "per row: sum(per row: x) by () <= 10"):
        _check_a(decidb_cli, f"""
            SELECT id, x FROM {_A} DECIDE x(INT)
            SUCH THAT x <= cap AND {clause} MAXIMIZE SUM(x * w)
        """, result, [0, 5, 0, 5])


@pytest.mark.var_integer
@pytest.mark.cons_aggregate
@pytest.mark.per_clause
@pytest.mark.correctness
def test_by_key_sums_only_its_own_group(decidb_cli, oracle_solver):
    """Deck p20 (local per, local aggregation): `PER grp: SUM(x) BY (grp) <= 6` caps
    each group at 6: a spends it on row 2 (12), b puts 5 on row 4 and 1 on row 3 (16),
    28 in all; ignoring `BY` caps the total at 6 and gives 17. The no-space lower-case
    form, split order and parenthesised qualified keys `(t.grp)` are the same key."""
    result = _oracle_a(oracle_solver, "local_local",
                       [(_sum("a"), "<=", 6), (_sum("b"), "<=", 6)])
    for sql in (f"SELECT id, x FROM {_A} DECIDE x(INT) SUCH THAT x <= cap "
                "AND PER grp: SUM(x) BY (grp) <= 6 MAXIMIZE SUM(x * w)",
                f"select id, x from {_A} decide x(int) such that x <= cap "
                "and per grp:sum(x)by(grp)<=6 maximize sum(x*w)",
                f"SELECT id, x DECIDE x(INT) FROM {_A} SUCH THAT x <= cap "
                "AND PER (t.grp): SUM(x) BY (t.grp) <= 6 MAXIMIZE SUM(x * w)"):
        _check_a(decidb_cli, sql, result, [0, 6, 1, 5])


@pytest.mark.var_integer
@pytest.mark.cons_aggregate
@pytest.mark.per_clause
@pytest.mark.correctness
def test_omitted_by_is_the_whole_input_not_the_per_group(decidb_cli, oracle_solver):
    """Deck p16 ("No by means by ()"): `PER grp: SUM(x) <= 6` generates one instance
    per group, each bounding the SAME whole-input sum. Total <= 6: 5 on row 4, 1 on
    row 2 (17). The per trap -- reading the omitted BY as BY (grp) -- gives 28."""
    result = _oracle_a(oracle_solver, "per_trap", [(_total(), "<=", 6)])
    for clause in ("PER grp: SUM(x) <= 6", "PER grp: SUM(x) BY () <= 6"):
        _check_a(decidb_cli, f"""
            SELECT id, x FROM {_A} DECIDE x(INT)
            SUCH THAT x <= cap AND {clause} MAXIMIZE SUM(x * w)
        """, result, [0, 1, 0, 5])


@pytest.mark.var_integer
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_by_relation_groups_each_distinct_tuple(decidb_cli, oracle_solver):
    """Syntax reference §2.1: a relation in a key expands to all its columns, so
    `BY (t)` over four distinct rows makes every row its own group and
    `x <= 0.5 * SUM(x) BY (t)` forces x = 0. Reading the key as `BY ()` would allow
    (3,7,2,5) = 34; as `BY (grp)`, (3,3,2,2) = 17."""
    result = _oracle_a(oracle_solver, "by_relation",
                       [({f"x_{i}": 0.5}, "<=", 0) for i in _A_ROWS])
    for clause in ("x <= 0.5 * SUM(x) BY (t)", "x <= 0.5 * Sum(x) By ( T )"):
        _check_a(decidb_cli, f"""
            SELECT id, x FROM {_A} DECIDE x(INT)
            SUCH THAT x <= cap AND {clause} MAXIMIZE SUM(x * w)
        """, result, [0, 0, 0, 0])


@pytest.mark.var_integer
@pytest.mark.cons_aggregate
@pytest.mark.per_clause
@pytest.mark.correctness
def test_two_column_by_key_in_either_column_order(decidb_cli, oracle_solver):
    """Deck p7: a multi-column key. `PER grp, flag: SUM(x) BY (grp, flag) <= 4` groups
    (a,t)={1,3}, (a,f)={2}, (b,t)={4}, (b,f)={5}; maximizing SUM(x * id) gives
    (0,4,4,2,4) = 48. A key that dropped `flag` would cap groups a and b at 4,
    (0,0,4,0,4) = 32; one that dropped `grp`, 34. `BY (flag, grp)` is the same key."""
    oracle_solver.create_model("two_col_key")
    obj = {}
    for i, (_, _, cap) in _F_ROWS.items():
        oracle_solver.add_variable(f"x_{i}", VarType.INTEGER, lb=0.0, ub=float(cap))
        obj[f"x_{i}"] = float(i)
    oracle_solver.add_constraint({"x_1": 1.0, "x_3": 1.0}, "<=", 4.0)
    for i in (2, 4, 5):
        oracle_solver.add_constraint({f"x_{i}": 1.0}, "<=", 4.0)
    oracle_solver.set_objective(obj, ObjSense.MAXIMIZE)
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL

    assert _oracle_xs(result, _F_ROWS) == [0, 4, 4, 2, 4]

    for key in ("grp, flag", "flag, grp"):
        got = _rows(decidb_cli, f"""
            SELECT id, x FROM {_F} DECIDE x(INT)
            SUCH THAT x <= cap AND PER grp, flag: SUM(x) BY ({key}) <= 4 MAXIMIZE SUM(x * id)
        """, "id", "x")
        assert sum(int(x) * i for i, x in got) == pytest.approx(result.objective_value)
        assert _xs(got) == [0, 4, 4, 2, 4]


# --- Several reducers of one decision in one body ---------------------------
@pytest.mark.var_integer
@pytest.mark.cons_aggregate
@pytest.mark.per_clause
@pytest.mark.correctness
def test_local_and_global_sums_of_one_decision_in_one_body(decidb_cli, oracle_solver):
    """Deck p32: "different reducers in the same constraint may use different groups".
    `PER grp: SUM(x) BY (grp) <= 0.5 * SUM(x) BY ()` says no group holds more than
    half the total, i.e. S_a = S_b: 7 each, (0,7,2,5) = 31. Merging the two terms
    onto the group key (the old defect) yields 0.5 * S_g <= 0 and x = 0 everywhere.
    The reversed sides and a serializer round-trip give the same rows."""
    result = _oracle_a(oracle_solver, "two_keys_ratio", [
        (_lin(_sum("a"), _total(-0.5)), "<=", 0),
        (_lin(_sum("b"), _total(-0.5)), "<=", 0),
    ])
    for cli, clause in ((decidb_cli, "SUM(x) BY (grp) <= 0.5 * SUM(x) BY ()"),
                        (decidb_cli, "0.5 * SUM(x) BY () >= SUM(x) BY (grp)"),
                        (decidb_cli.with_verify_serializer(), "SUM(x) BY (grp) <= 0.5 * SUM(x) BY ()")):
        _check_a(cli, f"""
            SELECT id, x FROM {_A} DECIDE x(INT)
            SUCH THAT x <= cap AND PER grp: {clause} MAXIMIZE SUM(x * w)
        """, result, [0, 7, 2, 5])


@pytest.mark.var_integer
@pytest.mark.cons_aggregate
@pytest.mark.per_clause
@pytest.mark.correctness
def test_local_plus_global_sum_added_on_one_side(decidb_cli, oracle_solver):
    """`PER grp: SUM(x) BY (grp) + SUM(x) BY () <= 10` is 2 S_a + S_b <= 10 and
    S_a + 2 S_b <= 10 (with row 1 and row 3 worthless): S_b = 4, S_a = 2, (0,2,0,4)
    = 16. Merged onto one key it would read 2 S_g <= 10 and return (0,5,0,5) = 25,
    which violates the query as written (5 + 10 > 10)."""
    result = _oracle_a(oracle_solver, "two_keys_additive", [
        (_lin(_sum("a"), _total()), "<=", 10),
        (_lin(_sum("b"), _total()), "<=", 10),
    ])
    _check_a(decidb_cli, f"""
        SELECT id, x FROM {_A} DECIDE x(INT)
        SUCH THAT x <= cap AND PER grp: SUM(x) BY (grp) + SUM(x) BY () <= 10 MAXIMIZE SUM(x * w)
    """, result, [0, 2, 0, 4])


def _model_rows(dump):
    """`row N: sense=.. rhs=.. nnz=K | c:v c:v` lines as {col: coef} dicts."""
    rows = []
    for match in re.finditer(r"^row \d+: .*?\|(.*)$", dump, re.M):
        rows.append({int(c): float(v) for c, v in (p.split(":") for p in match.group(1).split())})
    return rows


@pytest.mark.cons_aggregate
@pytest.mark.per_clause
def test_two_by_keys_are_two_terms_in_the_model(decidb_cli, tmp_path):
    """Model shape for the ratio body (no oracle: the previous test solves it): each
    group instance is one row over all four columns, +0.5 on the group's own columns
    (1 - 0.5) and -0.5 on the other group's. A merge onto one key would leave rows
    with two columns only."""
    dump = decidb_cli.dump_model(f"""
        SELECT id, x FROM {_A} DECIDE x(INT)
        SUCH THAT x <= cap AND PER grp: SUM(x) BY (grp) <= 0.5 * SUM(x) BY () MAXIMIZE SUM(x * w)
    """, tmp_path / "two_keys.dump")
    wide = [r for r in _model_rows(dump) if len(r) == 4]
    assert len(wide) == 2, dump
    plus = [frozenset(c for c, v in r.items() if v == pytest.approx(0.5)) for r in wide]
    minus = [frozenset(c for c, v in r.items() if v == pytest.approx(-0.5)) for r in wide]
    assert all(len(p) == 2 and len(m) == 2 for p, m in zip(plus, minus)), dump
    assert plus[0] == minus[1] and plus[1] == minus[0], dump


@pytest.mark.var_integer
@pytest.mark.cons_mixed
@pytest.mark.correctness
def test_per_row_term_beside_two_reducers_one_scaled(decidb_cli, oracle_solver):
    """Deck p21 + p32: a tuple term beside two differently keyed reducers of the same
    decision. `2 * x + SUM(x) BY (grp) <= 0.5 * SUM(x) BY () + 5` per row gives
    (2,2,2,2) = 14; merging the reducers onto the group key gives (0,2,0,2) = 10, onto
    the total (1,1,1,1) = 7, dropping the per-row term (3,7,2,5) = 34 (the old build
    raised INTERNAL). Split order, per-row term alone on the left: (3,5,2,5) = 30."""
    result = _oracle_a(oracle_solver, "row_plus_two_reducers", [
        (_lin({f"x_{i}": 2.0}, _sum(g), _total(-0.5)), "<=", 5)
        for g, ids in _GROUP.items() for i in ids
    ])
    _check_a(decidb_cli, f"""
        SELECT id, x FROM {_A} DECIDE x(INT)
        SUCH THAT x <= cap AND 2 * x + SUM(x) BY (grp) <= 0.5 * SUM(x) BY () + 5
        MAXIMIZE SUM(x * w)
    """, result, [2, 2, 2, 2])
    result = _oracle_a(oracle_solver, "row_left_two_reducers_right", [
        (_lin({f"x_{i}": 2.0}, _total(-1.0), _sum(g, 0.5)), "<=", 0)
        for g, ids in _GROUP.items() for i in ids
    ])
    _check_a(decidb_cli, f"""
        SELECT id, x DECIDE x(INT) FROM {_A}
        SUCH THAT x <= cap AND 2 * x <= SUM(x) BY () - 0.5 * SUM(x) BY (grp)
        MAXIMIZE SUM(x * w)
    """, result, [3, 5, 2, 5])


# --- AVG, MIN, MAX with BY --------------------------------------------------
@pytest.mark.var_integer
@pytest.mark.avg_rewrite
@pytest.mark.per_clause
@pytest.mark.correctness
def test_avg_by_divides_by_the_rows_its_when_admits(decidb_cli, oracle_solver):
    """Syntax reference §4: AVG divides by the counted rows, after the reducer's own
    WHEN. `PER grp: AVG(x) BY (grp) <= 3` is S_g <= 6: (0,6,1,5) = 28 (dividing by
    the whole-input count of 4 would free every row, 34). `AVG(WHEN id <> 1: x) BY
    (grp) <= 3` leaves group a with row 2 alone (x2 <= 3, row 1 free at 3): (3,3,1,5)
    = 25; dividing a by its full count would allow x2 <= 6, (3,6,1,5) = 31."""
    result = _oracle_a(oracle_solver, "avg_by_le",
                       [(_sum("a", 0.5), "<=", 3), (_sum("b", 0.5), "<=", 3)])
    _check_a(decidb_cli, f"""
        SELECT id, x FROM {_A} DECIDE x(INT)
        SUCH THAT x <= cap AND PER grp: AVG(x) BY (grp) <= 3 MAXIMIZE SUM(x * w)
    """, result, [0, 6, 1, 5])
    result = _oracle_a(oracle_solver, "avg_when_by_le",
                       [({"x_2": 1.0}, "<=", 3), ({"x_3": 0.5, "x_4": 0.5}, "<=", 3)])
    _check_a(decidb_cli, f"""
        SELECT id, x FROM {_A} DECIDE x(INT)
        SUCH THAT x <= cap AND PER grp: AVG(WHEN id <> 1: x) BY (grp) <= 3 MAXIMIZE SUM(x * w)
    """, result, [3, 3, 1, 5])


def _minmax_hard(oracle_solver, name, op, bound, sense):
    """One row per group satisfies `x op bound`: an indicator per row, at least one on."""
    _a_model(oracle_solver, name)
    for ids in _GROUP.values():
        for i in ids:
            oracle_solver.add_variable(f"y_{i}", VarType.BINARY)
            oracle_solver.add_indicator_constraint(f"y_{i}", 1, {f"x_{i}": 1.0}, op, float(bound))
        oracle_solver.add_constraint({f"y_{i}": 1.0 for i in ids}, ">=", 1.0)
    return _a_solve(oracle_solver, sense)


@pytest.mark.var_integer
@pytest.mark.min_max
@pytest.mark.per_clause
@pytest.mark.correctness
def test_max_by_in_the_easy_and_the_hard_direction(decidb_cli, oracle_solver):
    """Syntax reference §4: MAX is per-row in the easy direction and one-row-per-group
    in the hard one. `WHEN grp = 'a' PER grp: MAX(x) BY (grp) <= 2` caps group a's
    rows only: (2,2,2,5) = 23; a MAX that read the whole input (ignoring BY), or a
    dropped WHEN, caps every row, (2,2,2,2) = 14. `PER grp: MAX(x) BY (grp) >= 4`
    under MINIMIZE needs one row per group at 4 (hard, an indicator per row): rows 2
    and 4, (0,4,0,4) = 20; a whole-input MAX would lift one row only, (0,4,0,0) = 8,
    and a per-row reading is infeasible (rows 1 and 3 cannot reach 4)."""
    result = _oracle_a(oracle_solver, "max_easy", [({f"x_{i}": 1.0}, "<=", 2) for i in _GROUP["a"]])
    _check_a(decidb_cli, f"""
        SELECT id, x FROM {_A} DECIDE x(INT)
        SUCH THAT x <= cap AND WHEN grp = 'a' PER grp: MAX(x) BY (grp) <= 2 MAXIMIZE SUM(x * w)
    """, result, [2, 2, 2, 5])
    result = _minmax_hard(oracle_solver, "max_hard", ">=", 4, ObjSense.MINIMIZE)
    _check_a(decidb_cli, f"""
        SELECT id, x FROM {_A} DECIDE x(INT)
        SUCH THAT x <= cap AND PER grp: MAX(x) BY (grp) >= 4 MINIMIZE SUM(x * w)
    """, result, [0, 4, 0, 4])


@pytest.mark.var_integer
@pytest.mark.min_max
@pytest.mark.per_clause
@pytest.mark.correctness
def test_min_by_in_the_easy_and_the_hard_direction(decidb_cli, oracle_solver):
    """Syntax reference §4: MIN is per-row in the easy direction and one-row-per-group
    in the hard one. `WHEN grp = 'b' PER grp: MIN(x) BY (grp) >= 1` under MINIMIZE
    lifts group b's rows only: (0,0,1,1) = 4; a MIN over the whole input (ignoring
    BY), or a dropped WHEN, lifts every row, (1,1,1,1) = 7. `PER grp: MIN(x) BY (grp)
    <= 1` under MAXIMIZE holds one row per group at 1 and frees the rest (hard):
    sacrifice the light rows 1 and 3, (1,7,1,5) = 31; a whole-input MIN sacrifices
    row 3 alone, (3,7,1,5) = 33, and a per-row reading caps everything at 1, 7."""
    result = _oracle_a(oracle_solver, "min_easy", [({f"x_{i}": 1.0}, ">=", 1) for i in _GROUP["b"]],
                       ObjSense.MINIMIZE)
    _check_a(decidb_cli, f"""
        SELECT id, x FROM {_A} DECIDE x(INT)
        SUCH THAT x <= cap AND WHEN grp = 'b' PER grp: MIN(x) BY (grp) >= 1 MINIMIZE SUM(x * w)
    """, result, [0, 0, 1, 1])
    result = _minmax_hard(oracle_solver, "min_hard", "<=", 1, ObjSense.MAXIMIZE)
    _check_a(decidb_cli, f"""
        SELECT id, x FROM {_A} DECIDE x(INT)
        SUCH THAT x <= cap AND PER grp: MIN(x) BY (grp) <= 1 MAXIMIZE SUM(x * w)
    """, result, [1, 7, 1, 5])


# --- Reducer-local PER and WHEN PER -----------------------------------------
# Two depots joined to their routes (demand 3, revenue 3 per unit): depot 1 has three
# routes and a fixed cost of 10, depot 2 one route and a fixed cost of 12.
_DEPOTS = ("CREATE TEMP TABLE depot(did INT PRIMARY KEY, fixed INT, cap INT); "
           "INSERT INTO depot VALUES (1, 10, 9), (2, 12, 3); "
           "CREATE TEMP TABLE route(r INT PRIMARY KEY, did INT, dem INT, profit INT); "
           "INSERT INTO route VALUES (1,1,3,3),(2,1,3,3),(3,1,3,3),(4,2,3,3); ")
_ROUTES = {1: 1, 2: 1, 3: 1, 4: 2}  # route -> depot
_FIXED = {1: 10, 2: 12}
_CAP = {1: 9, 2: 3}


def _depot_oracle(oracle_solver, name, charge):
    """Open/ship model; `charge[d]` is the fixed cost the objective pays for depot d."""
    oracle_solver.create_model(name)
    obj = {}
    for d in _FIXED:
        oracle_solver.add_variable(f"open_{d}", VarType.BINARY)
        obj[f"open_{d}"] = -float(charge[d])
    for r in _ROUTES:
        oracle_solver.add_variable(f"ship_{r}", VarType.INTEGER, lb=0.0, ub=3.0)
        obj[f"ship_{r}"] = 3.0
    for d in _FIXED:
        row = {f"ship_{r}": 1.0 for r, rd in _ROUTES.items() if rd == d}
        row[f"open_{d}"] = -float(_CAP[d])
        oracle_solver.add_constraint(row, "<=", 0.0)
    oracle_solver.set_objective(obj, ObjSense.MAXIMIZE)
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL
    return result


def _depot_rows(decidb_cli, objective):
    """`{route: (open, ship)}` under `objective`."""
    got = _rows(decidb_cli, _DEPOTS + f"""
        SELECT R.r, open, ship FROM route R JOIN depot D ON R.did = D.did
        DECIDE PER D: open(BOOL), ship(INT) BETWEEN 0 AND R.dem
        SUCH THAT PER D: SUM(ship) BY (D) <= D.cap * open
        MAXIMIZE {objective}
    """, "r", "open", "ship")
    return {r: (int(o), int(s)) for r, o, s in got}


def _depot_value(by_route, charge):
    """The objective the returned rows realise under charging rule `charge`."""
    opened = {_ROUTES[r] for r, (o, _) in by_route.items() if o}
    return sum(3 * s for _, s in by_route.values()) - sum(charge[d] for d in opened)


def _depot_oracle_rows(result):
    """The oracle's optimum in `_depot_rows` shape, `{route: (open, ship)}`."""
    return {r: (round(result.variable_values[f"open_{d}"]), round(result.variable_values[f"ship_{r}"]))
            for r, d in _ROUTES.items()}


@pytest.mark.var_boolean
@pytest.mark.var_integer
@pytest.mark.sql_joins
@pytest.mark.correctness
def test_reducer_per_charges_each_key_once(decidb_cli, oracle_solver):
    """Syntax reference §4: `SUM(PER D: D.fixed * open)` counts one term per depot,
    the per-row `SUM(D.fixed * open)` one per joined route. Charged once, depot 1
    earns 27 - 10 and opens, depot 2 would lose 12 - 9 and stays closed: (1,0) = 17.
    Charged per route, depot 1 pays 30 for 27 and closes too: (0,0) = 0.
    `SUM(PER D.did: ...)` reads the depot through its PRIMARY KEY and agrees."""
    once = _depot_oracle(oracle_solver, "charge_once", _FIXED)
    assert once.objective_value == pytest.approx(27 - 10)
    assert _depot_oracle_rows(once) == {1: (1, 3), 2: (1, 3), 3: (1, 3), 4: (0, 0)}
    for spelling in ("SUM(PER D: D.fixed * open)", "SUM(PER D.did: D.fixed * open)"):
        by_route = _depot_rows(decidb_cli, f"SUM(ship * R.profit) - {spelling}")
        assert _depot_value(by_route, _FIXED) == pytest.approx(once.objective_value)
        assert by_route == _depot_oracle_rows(once)

    per_row_charge = {d: _FIXED[d] * sum(1 for rd in _ROUTES.values() if rd == d) for d in _FIXED}
    per_row = _depot_oracle(oracle_solver, "charge_per_row", per_row_charge)
    assert per_row.objective_value == pytest.approx(0)
    assert _depot_oracle_rows(per_row) == {r: (0, 0) for r in _ROUTES}
    by_route = _depot_rows(decidb_cli, "SUM(ship * R.profit) - SUM(D.fixed * open)")
    assert _depot_value(by_route, per_row_charge) == pytest.approx(per_row.objective_value)
    assert by_route == _depot_oracle_rows(per_row)


@pytest.mark.var_boolean
@pytest.mark.var_integer
@pytest.mark.sql_joins
@pytest.mark.when
@pytest.mark.correctness
def test_reducer_when_per_filters_then_charges_once(decidb_cli, oracle_solver):
    """`SUM(WHEN D.fixed < 11 PER D: D.fixed * open)` filters the reducer's rows
    first: only depot 1 is charged and the uncharged depot 2 opens too, (1,1) = 26.
    Ignoring the WHEN charges depot 2 and closes it (17); ignoring the PER charges
    depot 1 per route and closes it instead, (0,1) = 9. Lower case is the same."""
    charge = {1: 10, 2: 0}
    result = _depot_oracle(oracle_solver, "when_per", charge)
    assert result.objective_value == pytest.approx(36 - 10)
    assert _depot_oracle_rows(result) == {1: (1, 3), 2: (1, 3), 3: (1, 3), 4: (1, 3)}
    for spelling in ("SUM(ship * R.profit) - SUM(WHEN D.fixed < 11 PER D: D.fixed * open)",
                     "sum(ship * R.profit) - sum(when D.fixed < 11 per D: D.fixed * open)"):
        by_route = _depot_rows(decidb_cli, spelling)
        assert _depot_value(by_route, charge) == pytest.approx(result.objective_value)
        assert by_route == _depot_oracle_rows(result)


@pytest.mark.error
@pytest.mark.error_binder
def test_reducer_per_body_must_be_a_function_of_its_key(decidb_cli):
    """Syntax reference §4: under `SUM(PER K: e)` the body must be one value per K.
    A per-row decision under `PER D` and a row column under `PER grp` are refused."""
    decidb_cli.assert_error(_DEPOTS + """
        SELECT R.r, open, ship FROM route R JOIN depot D ON R.did = D.did
        DECIDE PER D: open(BOOL), ship(INT) BETWEEN 0 AND R.dem
        SUCH THAT PER D: SUM(ship) BY (D) <= D.cap * open
        MAXIMIZE SUM(PER D: ship)
    """, match=r"does not identify")
    decidb_cli.assert_error(f"""
        SELECT id, x FROM {_A} DECIDE x(INT)
        SUCH THAT x <= cap AND PER (): SUM(x) <= SUM(PER grp: cap) MAXIMIZE SUM(x * w)
    """, match=r"not determined by the generation key")


# --- Factors on a reducer ---------------------------------------------------
@pytest.mark.var_integer
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_constant_factor_scales_a_reducer(decidb_cli, oracle_solver):
    """`PER grp: 2 * SUM(x) BY (grp) <= 12` is the group cap of 6: (0,6,1,5) = 28.
    Dropping the factor would cap at 12 and free every row: (3,7,2,5) = 34."""
    result = _oracle_a(oracle_solver, "const_factor",
                       [(_sum("a", 2.0), "<=", 12), (_sum("b", 2.0), "<=", 12)])
    _check_a(decidb_cli, f"""
        SELECT id, x FROM {_A} DECIDE x(INT)
        SUCH THAT x <= cap AND PER grp: 2 * SUM(x) BY (grp) <= 12 MAXIMIZE SUM(x * w)
    """, result, [0, 6, 1, 5])


@pytest.mark.var_integer
@pytest.mark.sql_joins
@pytest.mark.correctness
def test_by_determined_data_factor_deck_example_5(decidb_cli, oracle_solver):
    """Deck p21: `ship <= D.share * SUM(ship) BY (D.d)` -- a per-row instance whose
    factor is one value per reduced group. Depot 1 (share 0.5) forces its two ships
    equal, 3 each; depot 2 (share 0.6) allows S3 <= 1.5 * S4 with S4 <= 2: (3,3,3,2)
    = 11. Ignoring the factor frees every ship to its demand (16); a share of 0.5
    everywhere gives (3,3,2,2) = 10. Admitted through the depot's PRIMARY KEY, or
    through the whole relation `BY (D)` on an unkeyed VALUES join."""
    ships, share = {1: (1, 5), 2: (1, 3), 3: (2, 6), 4: (2, 2)}, {1: 0.5, 2: 0.6}
    oracle_solver.create_model("deck_ex5")
    for s, (_, dem) in ships.items():
        oracle_solver.add_variable(f"ship_{s}", VarType.INTEGER, lb=0.0, ub=float(dem))
    for s, (d, _) in ships.items():
        row = {f"ship_{j}": -share[d] for j, (dj, _) in ships.items() if dj == d}
        row[f"ship_{s}"] = row.get(f"ship_{s}", 0.0) + 1.0
        oracle_solver.add_constraint(row, "<=", 0.0)
    oracle_solver.set_objective({f"ship_{s}": 1.0 for s in ships}, ObjSense.MAXIMIZE)
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL

    keyed = """
        CREATE TEMP TABLE depot(d INT PRIMARY KEY, share DOUBLE); INSERT INTO depot VALUES (1, 0.5), (2, 0.6);
        CREATE TEMP TABLE shp(s INT PRIMARY KEY, d INT, dem INT); INSERT INTO shp VALUES (1,1,5),(2,1,3),(3,2,6),(4,2,2);
        SELECT S.s, ship FROM shp S JOIN depot D ON S.d = D.d
        DECIDE ship(INT) BETWEEN 0 AND S.dem
        SUCH THAT ship <= D.share * SUM(ship) BY (D.d) MAXIMIZE SUM(ship)
    """
    unkeyed = """
        SELECT S.s, ship FROM (VALUES (1,1,5),(2,1,3),(3,2,6),(4,2,2)) S(s, d, dem)
        JOIN (VALUES (1, 0.5), (2, 0.6)) D(d, share) ON S.d = D.d
        DECIDE ship(INT) BETWEEN 0 AND S.dem
        SUCH THAT ship <= SUM(ship) BY (D) * D.share MAXIMIZE SUM(ship)
    """
    assert [round(result.variable_values[f"ship_{s}"]) for s in sorted(ships)] == [3, 3, 3, 2]
    for sql in (keyed, unkeyed):
        got = _rows(decidb_cli, sql, "s", "ship")
        assert sum(int(v) for _, v in got) == pytest.approx(result.objective_value)
        assert _xs(got) == [3, 3, 3, 2]


@pytest.mark.error
@pytest.mark.error_binder
@pytest.mark.correctness
def test_factor_varying_within_the_reduced_group_is_refused(decidb_cli, oracle_solver):
    """Syntax reference §4: a factor on a reducer is one value per reduced group.
    `cap * SUM(x)` varies over the whole input, `D.share * SUM(ship) BY (D.d)` over an
    unkeyed VALUES list is not provably one value per depot; both are refused. The
    divided form `SUM(x) BY (grp) <= 12 / cap` under `PER grp, cap` solves: the
    tightest bound per group (12/7 and 12/5) gives (0,1,0,2) = 8."""
    decidb_cli.assert_error(f"""
        SELECT id, x FROM {_A} DECIDE x(INT)
        SUCH THAT x <= cap AND cap * SUM(x) <= 12 MAXIMIZE SUM(x * w)
    """, match=r"varies across")
    decidb_cli.assert_error("""
        SELECT S.s, ship FROM (VALUES (1,1,5),(2,1,3),(3,2,6),(4,2,2)) S(s, d, dem)
        JOIN (VALUES (1, 0.5), (2, 0.6)) D(d, share) ON S.d = D.d
        DECIDE ship(INT) BETWEEN 0 AND S.dem
        SUCH THAT ship <= D.share * SUM(ship) BY (D.d) MAXIMIZE SUM(ship)
    """, match=r"varies across")
    result = _oracle_a(oracle_solver, "divided_bound",
                       [(_sum("a"), "<=", 12 / 7), (_sum("b"), "<=", 12 / 5)])
    _check_a(decidb_cli, f"""
        SELECT id, x FROM {_A} DECIDE x(INT)
        SUCH THAT x <= cap AND PER grp, cap: SUM(x) BY (grp) <= 12 / cap MAXIMIZE SUM(x * w)
    """, result, [0, 1, 0, 2])


@pytest.mark.error
@pytest.mark.error_binder
def test_by_key_naming_a_decision_is_refused(decidb_cli):
    """Syntax reference §4: a key names columns or relations, never a decision."""
    decidb_cli.assert_error(f"""
        SELECT id, x FROM {_A} DECIDE o(BOOL), x(INT)
        SUCH THAT x <= cap AND SUM(x) BY (o) <= 3 MAXIMIZE SUM(x * w)
    """, match=r"is a decision")


# --- Reducers over no rows --------------------------------------------------
@pytest.mark.var_integer
@pytest.mark.when_constraint
@pytest.mark.edge_case
@pytest.mark.correctness
def test_instance_whose_reducer_reads_no_row_is_not_imposed(decidb_cli, oracle_solver):
    """Syntax reference §4: a reducer over no rows has no value, so the instance is
    skipped. `PER grp: SUM(WHEN grp = 'a': x) BY (grp) >= 1` imposes x1 + x2 >= 1 on
    group a only: (1,0,0,0) = 1 under MINIMIZE; an empty sum read as 0 would make
    group b's `0 >= 1` infeasible, and a dropped reducer WHEN would lift group b too,
    (1,0,1,0) = 2. A clause-level `WHEN grp = 'a'` agrees. The same with AVG:
    `AVG(WHEN grp = 'a': x) BY (grp) >= 1` is x1 + x2 >= 2, (2,0,0,0) = 2."""
    result = _oracle_a(oracle_solver, "empty_skipped", [(_sum("a"), ">=", 1)], ObjSense.MINIMIZE)
    for clause in ("PER grp: SUM(WHEN grp = 'a': x) BY (grp) >= 1",
                   "WHEN grp = 'a' PER grp: SUM(x) BY (grp) >= 1"):
        _check_a(decidb_cli, f"""
            SELECT id, x FROM {_A} DECIDE x(INT)
            SUCH THAT x <= cap AND {clause} MINIMIZE SUM(x * w)
        """, result, [1, 0, 0, 0])
    result = _oracle_a(oracle_solver, "empty_avg_skipped", [(_sum("a", 0.5), ">=", 1)],
                       ObjSense.MINIMIZE)
    _check_a(decidb_cli, f"""
        SELECT id, x FROM {_A} DECIDE x(INT)
        SUCH THAT x <= cap AND PER grp: AVG(WHEN grp = 'a': x) BY (grp) >= 1 MINIMIZE SUM(x * w)
    """, result, [2, 0, 0, 0])


@pytest.mark.var_integer
@pytest.mark.when_constraint
@pytest.mark.edge_case
@pytest.mark.correctness
def test_empty_reducer_beside_another_contributes_nothing(decidb_cli, oracle_solver):
    """Deck p45 / spec §6.3: a reducer over no rows beside one that reads rows
    contributes nothing. `PER grp: SUM(WHEN grp = 'a': x) BY (grp) + SUM(x) BY (grp)
    >= 3` is 2 S_a >= 3 and S_b >= 3: (2,0,2,1) = 7 under MINIMIZE. Skipping group b's
    whole instance would give (2,0,0,0) = 2; an error would refuse a valid query."""
    result = _oracle_a(oracle_solver, "empty_beside",
                       [(_sum("a", 2.0), ">=", 3), (_sum("b"), ">=", 3)], ObjSense.MINIMIZE)
    _check_a(decidb_cli, f"""
        SELECT id, x FROM {_A} DECIDE x(INT)
        SUCH THAT x <= cap AND PER grp: SUM(WHEN grp = 'a': x) BY (grp) + SUM(x) BY (grp) >= 3
        MINIMIZE SUM(x * w)
    """, result, [2, 0, 2, 1])


@pytest.mark.var_integer
@pytest.mark.when_constraint
@pytest.mark.edge_case
@pytest.mark.correctness
def test_clause_with_no_instance_imposes_nothing(decidb_cli, oracle_solver):
    """Syntax reference §4 (deck p50's NULL policy): a reduced clause none of whose
    instances reads a row -- a reducer WHEN admitting nothing under `PER grp`, per row
    or `PER ()`, a clause WHEN admitting nothing, a defensive `WHEN grp IS NULL PER
    grp` without NULL keys -- imposes nothing and is not an error: under MINIMIZE
    every row stays at 0. The `>= 1` bound tells the outcomes apart: an empty sum
    imposed as `0 >= 1` is infeasible, and a dropped WHEN imposes `SUM(x) >= 1` and
    returns (1,0,0,0) = 1 (or (1,0,1,0) = 2 for the per-group defensive clause). With
    a NULL-keyed row 5 present the defensive clause does generate that one instance,
    x5 >= 1: (0,0,0,0,1) = 1."""
    result = _oracle_a(oracle_solver, "no_instance", [], ObjSense.MINIMIZE)
    for clause in ("PER grp: SUM(WHEN id > 100: x) BY (grp) >= 1",
                   "SUM(WHEN id > 100: x) >= 1",
                   "AVG(WHEN id > 100: x) >= 1",
                   "PER grp: AVG(WHEN id > 100: x) BY (grp) >= 1",
                   "PER (): SUM(WHEN id > 100: x) >= 1",
                   "WHEN id > 100: SUM(x) >= 1",
                   "WHEN grp IS NULL PER grp: SUM(x) BY (grp) >= 1"):
        _check_a(decidb_cli, f"""
            SELECT id, x FROM {_A} DECIDE x(INT)
            SUCH THAT x <= cap AND {clause} MINIMIZE SUM(x * w)
        """, result, [0, 0, 0, 0])

    _a_model(oracle_solver, "null_key_instance")
    oracle_solver.add_variable("x_5", VarType.INTEGER, lb=0.0, ub=4.0)
    oracle_solver.add_constraint({"x_5": 1.0}, ">=", 1.0)
    oracle_solver.set_objective({"x_1": 1.0, "x_2": 2.0, "x_3": 1.0, "x_4": 3.0, "x_5": 1.0},
                                ObjSense.MINIMIZE)
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL
    assert _oracle_xs(result, range(1, 6)) == [0, 0, 0, 0, 1]
    got = _rows(decidb_cli, """
        SELECT id, x FROM (VALUES (1,'a',3,1),(2,'a',7,2),(3,'b',2,1),(4,'b',5,3),(5,NULL,4,1)) t(id, grp, cap, w)
        DECIDE x(INT) SUCH THAT x <= cap AND WHEN grp IS NULL PER grp: SUM(x) BY (grp) >= 1
        MINIMIZE SUM(x * w)
    """, "id", "x")
    assert _a_value(got[:4]) + int(got[4][1]) == pytest.approx(result.objective_value)
    assert _xs(got) == [0, 0, 0, 0, 1]


_EMPTY_BOUNDS = [
    # per-row instances: group b's bound reads no row, so rows 3 and 4 are not bounded
    ("x <= SUM(WHEN cap > 5: cap) BY (grp) - 5",
     [({"x_1": 1.0}, "<=", 2), ({"x_2": 1.0}, "<=", 2)], [2, 2, 2, 5]),
    ("x <= AVG(WHEN cap > 5: cap) BY (grp) - 5",
     [({"x_1": 1.0}, "<=", 2), ({"x_2": 1.0}, "<=", 2)], [2, 2, 2, 5]),
    # keyed instances: group b's instance is not imposed
    ("PER grp: SUM(x) BY (grp) <= MIN(WHEN cap > 5: cap) BY (grp) - 4",
     [(_sum("a"), "<=", 3)], [0, 3, 2, 5]),
    # a finer key coarsened onto grp: (a, 7) and (b, 5) have no bound, (a, 3) and (b, 2) do
    ("PER grp, cap: SUM(x) BY (grp) <= SUM(WHEN cap < 5: cap) BY (grp, cap) + 1",
     [(_sum("a"), "<=", 4), (_sum("b"), "<=", 3)], [0, 4, 0, 3]),
    # one instance for the query, whose bound reads no row
    ("PER (): SUM(x) <= SUM(WHEN cap > 50: cap)", [], [3, 7, 2, 5]),
]


@pytest.mark.var_integer
@pytest.mark.when_constraint
@pytest.mark.edge_case
@pytest.mark.correctness
@pytest.mark.parametrize("clause,rows,vector", _EMPTY_BOUNDS,
                         ids=["per-row-sum", "per-row-avg", "keyed-min", "coarsened-partial", "query-wide"])
def test_bound_whose_reducer_reads_no_row_is_not_imposed(decidb_cli, oracle_solver, clause, rows, vector):
    """Syntax reference §4: a data reducer on the bound side reads no row in group b
    (no cap above 5 there), so group b's instances have no bound and are not imposed.
    The bound `... - 5` would be infeasible if an empty SUM were read as 0, and the
    query used to be refused. Under the coarsened key, instance (a, 7) has no bound
    while (a, 3) has 3 + 1 = 4: the group takes 4, not the empty instance's 0 + 1."""
    result = _oracle_a(oracle_solver, "empty_bound", rows)
    _check_a(decidb_cli, f"""
        SELECT id, x FROM {_A} DECIDE x(INT)
        SUCH THAT x <= cap AND {clause} MAXIMIZE SUM(x * w)
    """, result, vector)


@pytest.mark.error
@pytest.mark.when_objective
def test_empty_reducer_in_an_objective_is_an_error(decidb_cli):
    """Syntax reference §4: only a reducer over no rows in an objective is an error."""
    for objective in ("MAXIMIZE SUM(WHEN id > 100: x * w)", "MINIMIZE AVG(WHEN grp = 'z': x)"):
        decidb_cli.assert_error(f"""
            SELECT id, x FROM {_A} DECIDE x(INT) SUCH THAT x <= cap {objective}
        """, match=r"empty")


# --- COUNT(*) BY and norm ---------------------------------------------------
@pytest.mark.var_integer
@pytest.mark.cons_aggregate
@pytest.mark.per_clause
@pytest.mark.correctness
def test_count_star_by_over_data_is_a_bound(decidb_cli, oracle_solver):
    """Syntax reference §4: `COUNT(*) BY (k)` over known data is a bound like any data
    reducer. `PER grp: SUM(x) BY (grp) <= 2 * COUNT(*) BY (grp)` caps each two-row
    group at 4: (0,4,0,4) = 20. A whole-input count (4) would cap at 8 and give
    (1,7,2,5) = 32."""
    result = _oracle_a(oracle_solver, "count_by",
                       [(_sum("a"), "<=", 4), (_sum("b"), "<=", 4)])
    _check_a(decidb_cli, f"""
        SELECT id, x FROM {_A} DECIDE x(INT)
        SUCH THAT x <= cap AND PER grp: SUM(x) BY (grp) <= 2 * COUNT(*) BY (grp) MAXIMIZE SUM(x * w)
    """, result, [0, 4, 0, 4])


@pytest.mark.var_integer
@pytest.mark.cons_aggregate
@pytest.mark.per_clause
@pytest.mark.when_constraint
@pytest.mark.correctness
def test_count_with_its_own_when_counts_zero_over_no_rows(decidb_cli, oracle_solver):
    """Syntax reference §4: a data COUNT takes a WHEN prefix like the other reducers,
    and over no rows it is 0 (SQL's COUNT), not "no value". `PER grp: SUM(x) BY (grp)
    <= 3 * COUNT(WHEN cap > 5: cap) BY (grp)` caps group a (one cap above 5) at 3 and
    group b (none) at 0: (0,3,0,0) = 6. Reading b's empty count as "no value" would
    leave group b free, (0,3,2,5) = 23; ignoring the WHEN caps both groups at 6."""
    result = _oracle_a(oracle_solver, "count_when",
                       [(_sum("a"), "<=", 3), (_sum("b"), "<=", 0)])
    _check_a(decidb_cli, f"""
        SELECT id, x FROM {_A} DECIDE x(INT)
        SUCH THAT x <= cap AND PER grp: SUM(x) BY (grp) <= 3 * COUNT(WHEN cap > 5: cap) BY (grp)
        MAXIMIZE SUM(x * w)
    """, result, [0, 3, 0, 0])


def _l1_rows(oracle_solver, ids, bound):
    """|x_i - 2| summed over `ids` <= bound, through t_i >= x_i - 2 and t_i >= 2 - x_i."""
    for i in ids:
        oracle_solver.add_variable(f"t_{i}", VarType.CONTINUOUS, lb=0.0, ub=10.0)
        oracle_solver.add_constraint({f"t_{i}": 1.0, f"x_{i}": -1.0}, ">=", -2.0)
        oracle_solver.add_constraint({f"t_{i}": 1.0, f"x_{i}": 1.0}, ">=", 2.0)
    oracle_solver.add_constraint({f"t_{i}": 1.0 for i in ids}, "<=", float(bound))


@pytest.mark.var_integer
@pytest.mark.per_clause
@pytest.mark.correctness
def test_norm_takes_a_when_prefix_and_a_by_group(decidb_cli, oracle_solver):
    """Syntax reference §4: `norm(WHEN c: e, 1) BY (k)` is a group-wise L1 term over
    the rows its WHEN admits. `PER grp: norm(x - 2, 1) BY (grp) <= 1` allows one unit
    of deviation per group: (2,3,2,3) = 19 (a whole-input norm allows one unit in
    total, 17). With `WHEN id <> 1` row 1 leaves the norm and takes its cap:
    (3,3,2,3) = 20; ignoring the WHEN gives 19 and ignoring the BY 18."""
    _a_model(oracle_solver, "norm_by")
    for ids in _GROUP.values():
        _l1_rows(oracle_solver, ids, 1)
    result = _a_solve(oracle_solver)
    _check_a(decidb_cli, f"""
        SELECT id, x FROM {_A} DECIDE x(INT)
        SUCH THAT x <= cap AND PER grp: norm(x - 2, 1) BY (grp) <= 1 MAXIMIZE SUM(x * w)
    """, result, [2, 3, 2, 3])

    _a_model(oracle_solver, "norm_when_by")
    _l1_rows(oracle_solver, (2,), 1)
    _l1_rows(oracle_solver, (3, 4), 1)
    result = _a_solve(oracle_solver)
    _check_a(decidb_cli, f"""
        SELECT id, x FROM {_A} DECIDE x(INT)
        SUCH THAT x <= cap AND PER grp: norm(WHEN id <> 1: x - 2, 1) BY (grp) <= 1
        MAXIMIZE SUM(x * w)
    """, result, [3, 3, 2, 3])
