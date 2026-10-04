"""The constraint prefix grid: `[WHEN known] [PER scope] [IF unknown] : body`.

DeciQL syntax reference §3 (deck p58-59): the three prefixes are ordered
filter -> generate -> guard. `WHEN` keeps rows of known data before anything else
happens, `PER` says how many instances the body becomes (one per distinct key value,
`PER ()` one for the query, omitted or `PER ROW` one per row), and `IF` turns each
instance into an implication over decisions. A prefix over a `BETWEEN` body governs
both of its comparisons (delta 8).

Every subset of the three prefixes is crossed here with the PER spellings and the body
kinds the reference lists. Each oracle states the instances explicitly, guards as
Big-M rows, so the check is against an independently built model.

Data throughout: t(id, grp, cap, pri) with caps a = 3, 7; b = 2, 5; c = 4 and the
priority rows 1 (a) and 3 (b). `x` is INT in [0, 10]; unconstrained rows sit at 10.
"""

import re

import pytest

from solver.types import ObjSense, SolverStatus, VarType

_ROWS = {1: ("a", 3, True), 2: ("a", 7, False), 3: ("b", 2, True),
         4: ("b", 5, False), 5: ("c", 4, False)}
_GROUPS = {"a": [1, 2], "b": [3, 4], "c": [5]}
_T = ("(VALUES (1, 'a', 3, true), (2, 'a', 7, false), (3, 'b', 2, true), "
      "(4, 'b', 5, false), (5, 'c', 4, false)) t(id, grp, cap, pri)")
_TN = _T.replace("(5, 'c', 4, false))", "(5, 'c', 4, false), (6, NULL, 6, false))")
_S = "(VALUES ('a', 4), ('b', 3), ('c', 9)) s(grp, lim)"
_LIM = {"a": 4, "b": 3, "c": 9}
_DECL = "DECIDE x(INT) BETWEEN 0 AND 10"
_GOPEN = "DECIDE x(INT) BETWEEN 0 AND 10, PER grp: gopen(BOOL)"


def _tw(w):
    """The base relation with a weight column `w` (one weight per id)."""
    vals = ", ".join(f"({i}, '{g}', {c}, {str(p).lower()}, {w[i]})"
                     for i, (g, c, p) in _ROWS.items())
    return f"(VALUES {vals}) t(id, grp, cap, pri, w)"


def _rows(decidb_cli, sql, *cols):
    rows, names = decidb_cli.execute(sql)
    idx = [names.index(c) for c in cols]
    return sorted(tuple(r[i] for i in idx) for r in rows)


def _num_rows(decidb_cli, sql, tmp_path):
    """The number of constraints the solver model states: the instance count after
    reduction. A guarded instance is a Big-M row on HiGHS and an indicator constraint
    on Gurobi, so both are counted and the answer is the same on either backend."""
    dump = decidb_cli.dump_model(sql, tmp_path / "model.dump")
    rows = int(re.search(r"num_rows: (\d+)", dump).group(1))
    indicators = re.search(r"num_indconstrs: (\d+)", dump)
    return rows + (int(indicators.group(1)) if indicators else 0)


def _x_vars(oracle, name, ids=_ROWS, ub=10.0):
    """One INT decision per row; returns the `SUM(x)` objective."""
    oracle.create_model(name)
    for i in ids:
        oracle.add_variable(f"x_{i}", VarType.INTEGER, lb=0.0, ub=ub)
    return {f"x_{i}": 1.0 for i in ids}


def _optimum(oracle, obj, sense=ObjSense.MAXIMIZE):
    oracle.set_objective(obj, sense)
    result = oracle.solve()
    assert result.status == SolverStatus.OPTIMAL
    return result.objective_value


def _compact(text):
    return "".join(ch for ch in text if ch not in "│┌┐└┘─┬┴ \n")


# ---------------------------------------------------------------------------
# No prefix, and PER alone in every spelling
# ---------------------------------------------------------------------------

@pytest.mark.per_clause
@pytest.mark.correctness
def test_omitted_per_and_per_row_generate_one_instance_per_row(decidb_cli, oracle_solver):
    """§3 / deck p16-17: no PER, `PER ROW:` and `per row:` all mean one instance per
    result row. `SUM(x) BY (grp) <= cap` read per row puts a group's sum under each of
    its rows' caps, so the tightest wins: a <= 3, b <= 2, c <= 4 -> 9. Reading one cap
    per group instead is ambiguous (refused), and `PER ROW` read as `PER ()` would
    refuse `cap` as undetermined."""
    body = "SUM(x) BY (grp) <= cap MAXIMIZE SUM(x)"
    bare = _rows(decidb_cli, f"SELECT id, x FROM {_T} {_DECL} SUCH THAT {body}", "id", "x")
    upper = _rows(decidb_cli, f"SELECT id, x FROM {_T} {_DECL} SUCH THAT PER ROW: {body}", "id", "x")
    lower = _rows(decidb_cli, f"SELECT id, x FROM {_T} {_DECL} SUCH THAT per row: {body}", "id", "x")

    obj = _x_vars(oracle_solver, "per_row_default")
    for i, (g, cap, _) in _ROWS.items():
        oracle_solver.add_constraint({f"x_{j}": 1.0 for j in _GROUPS[g]}, "<=", float(cap))
    best = _optimum(oracle_solver, obj)

    assert bare == upper == lower
    assert sum(x for _, x in bare) == pytest.approx(best) == 9


@pytest.mark.per_clause
@pytest.mark.correctness
def test_per_column_spellings_are_one_key(decidb_cli, oracle_solver, tmp_path):
    """§2.1 / delta 15: `PER grp`, `PER (grp)` and `PER t.grp` name the same key, so the
    three queries build the same three instances (sum per group <= 5 -> 15, 3 model
    rows). A spelling read as `PER ()` would refuse the BY key; one read as a fresh
    key would change the instance count."""
    body = "SUM(x) BY (grp) <= 5 MAXIMIZE SUM(x)"
    sqls = [f"SELECT id, x FROM {_T} {_DECL} SUCH THAT PER {k}: {body}"
            for k in ("grp", "(grp)", "t.grp")]
    got = [_rows(decidb_cli, sql, "id", "x") for sql in sqls]

    obj = _x_vars(oracle_solver, "per_grp")
    for ids in _GROUPS.values():
        oracle_solver.add_constraint({f"x_{i}": 1.0 for i in ids}, "<=", 5.0)
    best = _optimum(oracle_solver, obj)

    assert got[0] == got[1] == got[2]
    assert sum(x for _, x in got[0]) == pytest.approx(best) == 15
    assert all(_num_rows(decidb_cli, sql, tmp_path) == 3 for sql in sqls)


@pytest.mark.per_clause
@pytest.mark.correctness
def test_per_two_columns_with_and_without_parentheses(decidb_cli, oracle_solver, tmp_path):
    """§3.1: `PER grp, cap` (and `PER (grp, cap)`) generates one instance per distinct
    (grp, cap) pair, five in all, and since `cap` only refines the reducer's BY (grp)
    key the instances share the group sum and reduce to the tightest bound: a <= 3,
    b <= 2, c <= 4 -> 9 on 3 model rows. Reading the loosest cap instead gives 16."""
    body = "SUM(x) BY (grp) <= cap MAXIMIZE SUM(x)"
    plain = f"SELECT id, x FROM {_T} {_DECL} SUCH THAT PER grp, cap: {body}"
    paren = f"SELECT id, x FROM {_T} {_DECL} SUCH THAT PER (grp, cap): {body}"
    got = _rows(decidb_cli, plain, "id", "x")

    obj = _x_vars(oracle_solver, "per_grp_cap")
    for g, cap, _ in _ROWS.values():
        oracle_solver.add_constraint({f"x_{i}": 1.0 for i in _GROUPS[g]}, "<=", float(cap))
    best = _optimum(oracle_solver, obj)

    assert got == _rows(decidb_cli, paren, "id", "x")
    assert sum(x for _, x in got) == pytest.approx(best) == 9
    assert _num_rows(decidb_cli, plain, tmp_path) == 3


@pytest.mark.per_clause
@pytest.mark.correctness
def test_per_empty_generates_one_instance_for_the_query(decidb_cli, oracle_solver, tmp_path):
    """§3 / deck p18 and §3.1: `PER (): SUM(x) <= 12` is one network-level row over
    every decision -> 12 on 1 model row, and that instance may only read query-wide
    values: `PER (): SUM(x) <= cap` is refused because `cap` varies across rows,
    while the same body with no prefix reads each row's own cap (the tightest, 2).
    A `PER ()` read as the per-row default would accept `cap` and answer 2."""
    sql = f"SELECT id, x FROM {_T} {_DECL} SUCH THAT PER (): SUM(x) <= 12 MAXIMIZE SUM(x)"
    got = _rows(decidb_cli, sql, "id", "x")
    per_row = _rows(decidb_cli, f"SELECT id, x FROM {_T} {_DECL} SUCH THAT SUM(x) <= cap MAXIMIZE SUM(x)",
                    "id", "x")

    obj = _x_vars(oracle_solver, "per_empty")
    oracle_solver.add_constraint(dict(obj), "<=", 12.0)
    best = _optimum(oracle_solver, obj)

    assert sum(x for _, x in got) == pytest.approx(best) == 12
    assert _num_rows(decidb_cli, sql, tmp_path) == 1
    assert sum(x for _, x in per_row) == 2
    decidb_cli.assert_error(f"SELECT id, x FROM {_T} {_DECL} SUCH THAT PER (): SUM(x) <= cap MAXIMIZE SUM(x)",
                            match=r"varies across rows")


@pytest.mark.per_clause
@pytest.mark.sql_joins
@pytest.mark.correctness
def test_per_relation_expands_to_its_columns(decidb_cli, oracle_solver, tmp_path):
    """§2.1 / deck p7: `PER s` is `PER s.grp, s.lim`, so `s.lim` is one value per
    instance and `PER t.grp, s` names the same three instances: a <= 4, b <= 3,
    c <= 9 -> 16 on 3 model rows. A relation key that did not expand would leave
    `s.lim` undetermined (refused) or read one limit for every group."""
    join = f"{_T} JOIN {_S} ON t.grp = s.grp"
    by_s = f"SELECT t.id, x FROM {join} {_DECL} SUCH THAT PER s: SUM(x) BY (s.grp) <= s.lim MAXIMIZE SUM(x)"
    mixed = f"SELECT t.id, x FROM {join} {_DECL} SUCH THAT PER t.grp, s: SUM(x) BY (t.grp) <= s.lim MAXIMIZE SUM(x)"
    got = _rows(decidb_cli, by_s, "id", "x")

    obj = _x_vars(oracle_solver, "per_relation")
    for g, ids in _GROUPS.items():
        oracle_solver.add_constraint({f"x_{i}": 1.0 for i in ids}, "<=", float(_LIM[g]))
    best = _optimum(oracle_solver, obj)

    assert got == _rows(decidb_cli, mixed, "id", "x")
    assert sum(x for _, x in got) == pytest.approx(best) == 16
    assert _num_rows(decidb_cli, by_s, tmp_path) == 3


@pytest.mark.per_clause
@pytest.mark.sql_joins
@pytest.mark.correctness
def test_using_merged_column_resolves_as_one_key(decidb_cli, oracle_solver):
    """§2.1 / delta 11: a column merged by `JOIN ... USING (grp)` is one column in a
    PER key, a BY key and beside a relation. `PER grp, lim` and `PER grp, s` both give
    a <= 4, b <= 3, c <= 9 -> 16; an unmerged `grp` is refused as ambiguous."""
    join = f"{_T} JOIN {_S} USING (grp)"
    cols = f"SELECT id, x FROM {join} {_DECL} SUCH THAT PER grp, lim: SUM(x) BY (grp) <= lim MAXIMIZE SUM(x)"
    rel = f"SELECT id, x FROM {join} {_DECL} SUCH THAT PER grp, s: SUM(x) BY (grp) <= lim MAXIMIZE SUM(x)"
    got = _rows(decidb_cli, cols, "id", "x")

    obj = _x_vars(oracle_solver, "using_key")
    for g, ids in _GROUPS.items():
        oracle_solver.add_constraint({f"x_{i}": 1.0 for i in ids}, "<=", float(_LIM[g]))
    best = _optimum(oracle_solver, obj)

    assert got == _rows(decidb_cli, rel, "id", "x")
    assert sum(x for _, x in got) == pytest.approx(best) == 16


@pytest.mark.cons_between
@pytest.mark.per_clause
@pytest.mark.correctness
def test_per_prefix_governs_both_between_bounds(decidb_cli, oracle_solver):
    """§3: a prefix over a BETWEEN body governs both comparisons (delta 8). With
    weights -1 on group a and +1 elsewhere, `PER grp: SUM(x) BY (grp) BETWEEN 2 AND 6`
    pins a to its floor and b, c to their ceiling: -2 + 6 + 6 = 10. A dropped floor
    gives 12, a dropped ceiling 28."""
    got = _rows(decidb_cli, f"""
        SELECT id, x FROM {_tw({1: -1, 2: -1, 3: 1, 4: 1, 5: 1})} {_DECL}
        SUCH THAT PER grp: SUM(x) BY (grp) BETWEEN 2 AND 6
        MAXIMIZE SUM(x * w)
    """, "id", "x")

    _x_vars(oracle_solver, "per_between")
    for ids in _GROUPS.values():
        oracle_solver.add_constraint({f"x_{i}": 1.0 for i in ids}, ">=", 2.0)
        oracle_solver.add_constraint({f"x_{i}": 1.0 for i in ids}, "<=", 6.0)
    best = _optimum(oracle_solver, {"x_1": -1.0, "x_2": -1.0, "x_3": 1.0, "x_4": 1.0, "x_5": 1.0})

    by_id = dict(got)
    assert by_id[1] + by_id[2] == 2 and by_id[3] + by_id[4] == 6 and by_id[5] == 6
    assert -by_id[1] - by_id[2] + by_id[3] + by_id[4] + by_id[5] == pytest.approx(best) == 10


@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_reducer_as_a_bound_reads_the_rows_own_group(decidb_cli, oracle_solver):
    """Deck p21 (matrix row 5): `x <= 0.6 * SUM(x) BY (grp)` is one row per tuple
    against its own depot's total. With `x <= cap`: a = 3 + 4, b = 2 + 3 and the lone
    c row can only be 0 (x5 <= 0.6 x5) -> 12. Against the global sum instead every
    cap is reachable: 21."""
    got = _rows(decidb_cli, f"""
        SELECT id, x FROM {_T} {_DECL}
        SUCH THAT x <= 0.6 * SUM(x) BY (grp) AND x <= cap
        MAXIMIZE SUM(x)
    """, "id", "x")

    obj = _x_vars(oracle_solver, "reducer_bound")
    for i, (g, cap, _) in _ROWS.items():
        row = {f"x_{j}": -0.6 for j in _GROUPS[g]}
        row[f"x_{i}"] = row.get(f"x_{i}", 0.0) + 1.0
        oracle_solver.add_constraint(row, "<=", 0.0)
        oracle_solver.add_constraint({f"x_{i}": 1.0}, "<=", float(cap))
    best = _optimum(oracle_solver, obj)

    assert got == [(1, 3), (2, 4), (3, 2), (4, 3), (5, 0)]
    assert sum(x for _, x in got) == pytest.approx(best) == 12


# ---------------------------------------------------------------------------
# WHEN alone
# ---------------------------------------------------------------------------

@pytest.mark.when_perrow
@pytest.mark.cons_between
@pytest.mark.correctness
def test_when_governs_both_bounds_of_a_between_body(decidb_cli, oracle_solver):
    """§3 WHEN row, delta 8: `WHEN pri: x BETWEEN 2 AND 4` generates instances on rows
    1 and 3 only, and both bounds hold there. Weight -1 on row 1 pins it to the floor
    (2), row 3 to the ceiling (4), the rest are free: -2 + 10 + 4 + 10 + 10 = 32. The
    old defect dropped the WHEN (14); a dropped floor gives 34, a dropped ceiling 38."""
    got = _rows(decidb_cli, f"""
        SELECT id, x FROM {_tw({1: -1, 2: 1, 3: 1, 4: 1, 5: 1})} {_DECL}
        SUCH THAT WHEN pri: x BETWEEN 2 AND 4
        MAXIMIZE SUM(x * w)
    """, "id", "x")

    _x_vars(oracle_solver, "when_between")
    for i in (1, 3):
        oracle_solver.add_constraint({f"x_{i}": 1.0}, ">=", 2.0)
        oracle_solver.add_constraint({f"x_{i}": 1.0}, "<=", 4.0)
    best = _optimum(oracle_solver, {"x_1": -1.0, "x_2": 1.0, "x_3": 1.0, "x_4": 1.0, "x_5": 1.0})

    assert got == [(1, 2), (2, 10), (3, 4), (4, 10), (5, 10)]
    assert -2 + 10 + 4 + 10 + 10 == pytest.approx(best)


@pytest.mark.when_constraint
@pytest.mark.correctness
def test_when_filters_the_reducers_input_rows(decidb_cli, oracle_solver):
    """§4 / deck p29 step 1: a reducer reads the WHEN-filtered relation. `WHEN pri:
    SUM(x) BY (grp) <= cap` generates on rows 1 and 3, and each group sum holds only
    the filtered rows: x1 <= 3, x3 <= 2, the rest free -> 35. Summing the unfiltered
    groups gives x1 + x2 <= 3, x3 + x4 <= 2 -> 15."""
    got = _rows(decidb_cli, f"""
        SELECT id, x FROM {_T} {_DECL}
        SUCH THAT WHEN pri: SUM(x) BY (grp) <= cap
        MAXIMIZE SUM(x)
    """, "id", "x")

    obj = _x_vars(oracle_solver, "when_reducer")
    oracle_solver.add_constraint({"x_1": 1.0}, "<=", 3.0)
    oracle_solver.add_constraint({"x_3": 1.0}, "<=", 2.0)
    best = _optimum(oracle_solver, obj)

    assert got == [(1, 3), (2, 10), (3, 2), (4, 10), (5, 10)]
    assert sum(x for _, x in got) == pytest.approx(best)


# ---------------------------------------------------------------------------
# IF alone
# ---------------------------------------------------------------------------

@pytest.mark.var_boolean
@pytest.mark.correctness
def test_if_guards_each_row_instance_on_its_own_decision(decidb_cli, oracle_solver):
    """§3.2: with no PER, `IF open: x <= cap` is one implication per row, and `PER ROW
    IF open:` spells the same. Three rows must open; the solver caps the rows that
    lose least (7, 5, 4 on rows 2, 4, 5) -> 36. A guard read as always-on gives 21,
    one never imposed 50."""
    body = "IF open: x <= cap AND SUM(open) >= 3 MAXIMIZE SUM(x)"
    decl = "DECIDE x(INT) BETWEEN 0 AND 10, open(BOOL)"
    got = _rows(decidb_cli, f"SELECT id, open, x FROM {_T} {decl} SUCH THAT {body}", "id", "open", "x")
    explicit = _rows(decidb_cli, f"SELECT id, open, x FROM {_T} {decl} SUCH THAT PER ROW {body}", "id", "open", "x")

    obj = _x_vars(oracle_solver, "row_guard")
    for i, (_, cap, _) in _ROWS.items():
        oracle_solver.add_variable(f"open_{i}", VarType.BINARY)
        # open = 1 ⟹ x <= cap:  x + (10 - cap) * open <= 10
        oracle_solver.add_constraint({f"x_{i}": 1.0, f"open_{i}": 10.0 - cap}, "<=", 10.0)
    oracle_solver.add_constraint({f"open_{i}": 1.0 for i in _ROWS}, ">=", 3.0)
    best = _optimum(oracle_solver, obj)

    assert got == explicit
    assert got == [(1, False, 10), (2, True, 7), (3, False, 10), (4, True, 5), (5, True, 4)]
    assert sum(x for _, _, x in got) == pytest.approx(best)


# ---------------------------------------------------------------------------
# WHEN + PER
# ---------------------------------------------------------------------------

@pytest.mark.when_constraint
@pytest.mark.per_clause
@pytest.mark.correctness
def test_when_before_per_an_emptied_group_generates_no_instance(decidb_cli, oracle_solver, tmp_path):
    """§3 (filter -> generate), deck p29: `WHEN pri PER grp` generates over the filtered
    rows, so group c, which WHEN empties, has no instance and the a/b sums hold only
    their priority row: x1 <= 5, x3 <= 5 -> 40 on 2 model rows. Generating and
    reducing over the unfiltered rows would keep a c instance (x5 <= 5: 35 on 3
    rows); dropping the WHEN gives 15; `PER grp` read as `PER ()` is refused."""
    sql = f"SELECT id, x FROM {_T} {_DECL} SUCH THAT WHEN pri PER grp: SUM(x) BY (grp) <= 5 MAXIMIZE SUM(x)"
    got = _rows(decidb_cli, sql, "id", "x")

    obj = _x_vars(oracle_solver, "when_per")
    oracle_solver.add_constraint({"x_1": 1.0}, "<=", 5.0)
    oracle_solver.add_constraint({"x_3": 1.0}, "<=", 5.0)
    best = _optimum(oracle_solver, obj)

    assert got == [(1, 5), (2, 10), (3, 5), (4, 10), (5, 10)]
    assert sum(x for _, x in got) == pytest.approx(best)
    assert _num_rows(decidb_cli, sql, tmp_path) == 2


@pytest.mark.when_constraint
@pytest.mark.per_clause
@pytest.mark.correctness
def test_when_per_on_a_keyed_decision_body(decidb_cli, oracle_solver):
    """§3.1: a `PER grp` decision under `PER grp, cap` is determined; `WHEN pri` keeps
    (a, 3) and (b, 2), so y_a <= 3, y_b <= 2 and y_c is free. `SUM(y)` counts y once
    per row: 3 + 3 + 2 + 2 + 10 = 20. Without the filter every pair binds: 14."""
    got = _rows(decidb_cli, f"""
        SELECT id, y FROM {_T} DECIDE PER grp: y(INT) BETWEEN 0 AND 10
        SUCH THAT WHEN pri PER grp, cap: y <= cap
        MAXIMIZE SUM(y)
    """, "id", "y")

    oracle_solver.create_model("when_per_keyed")
    for g, ids in _GROUPS.items():
        oracle_solver.add_variable(f"y_{g}", VarType.INTEGER, lb=0.0, ub=10.0)
    oracle_solver.add_constraint({"y_a": 1.0}, "<=", 3.0)
    oracle_solver.add_constraint({"y_b": 1.0}, "<=", 2.0)
    best = _optimum(oracle_solver, {f"y_{g}": float(len(ids)) for g, ids in _GROUPS.items()})

    assert got == [(1, 3), (2, 3), (3, 2), (4, 2), (5, 10)]
    assert sum(y for _, y in got) == pytest.approx(best)


@pytest.mark.when_constraint
@pytest.mark.per_clause
@pytest.mark.correctness
def test_when_per_with_reducers_on_both_sides(decidb_cli, oracle_solver):
    """§4 matrix row 4 / deck p20 under a filter: both reducers of `WHEN NOT pri PER
    grp: SUM(x) BY (grp) <= SUM(cap) BY (grp)` read the filtered rows, so x2 <= 7,
    x4 <= 5, x5 <= 4 and the priority rows are free -> 36. Unfiltered sums give
    x1 + x2 <= 10, x3 + x4 <= 7, x5 <= 4 -> 21."""
    got = _rows(decidb_cli, f"""
        SELECT id, x FROM {_T} {_DECL}
        SUCH THAT WHEN NOT pri PER grp: SUM(x) BY (grp) <= SUM(cap) BY (grp)
        MAXIMIZE SUM(x)
    """, "id", "x")

    obj = _x_vars(oracle_solver, "when_per_both_sides")
    for i in (2, 4, 5):
        oracle_solver.add_constraint({f"x_{i}": 1.0}, "<=", float(_ROWS[i][1]))
    best = _optimum(oracle_solver, obj)

    assert got == [(1, 10), (2, 7), (3, 10), (4, 5), (5, 4)]
    assert sum(x for _, x in got) == pytest.approx(best)


@pytest.mark.when_constraint
@pytest.mark.per_clause
@pytest.mark.cons_between
@pytest.mark.correctness
def test_when_per_prefix_governs_both_between_bounds(decidb_cli, oracle_solver, tmp_path):
    """Delta 8 under WHEN PER: `WHEN pri PER grp: SUM(x) BY (grp) BETWEEN 1 AND 3`
    is two comparisons on each of the two surviving instances (4 model rows). Weight
    -1 on row 1 pins it to the floor and row 3 to the ceiling: -1 + 10 + 3 + 10 + 10
    = 32. The old defect built the BETWEEN over all rows (9); a dropped floor gives
    33, a dropped ceiling 39."""
    sql = f"""
        SELECT id, x FROM {_tw({1: -1, 2: 1, 3: 1, 4: 1, 5: 1})} {_DECL}
        SUCH THAT WHEN pri PER grp: SUM(x) BY (grp) BETWEEN 1 AND 3
        MAXIMIZE SUM(x * w)
    """
    got = _rows(decidb_cli, sql, "id", "x")

    _x_vars(oracle_solver, "when_per_between")
    for i in (1, 3):
        oracle_solver.add_constraint({f"x_{i}": 1.0}, ">=", 1.0)
        oracle_solver.add_constraint({f"x_{i}": 1.0}, "<=", 3.0)
    best = _optimum(oracle_solver, {"x_1": -1.0, "x_2": 1.0, "x_3": 1.0, "x_4": 1.0, "x_5": 1.0})

    assert got == [(1, 1), (2, 10), (3, 3), (4, 10), (5, 10)]
    assert -1 + 10 + 3 + 10 + 10 == pytest.approx(best)
    assert _num_rows(decidb_cli, sql, tmp_path) == 4


@pytest.mark.when_constraint
@pytest.mark.per_clause
@pytest.mark.edge_case
@pytest.mark.correctness
def test_reducer_local_when_inside_a_filtered_generation(decidb_cli, oracle_solver):
    """§4 (reducer-local WHEN) with §4's empty-reducer rule (delta 2): the clause's
    WHEN keeps rows 2, 4, 5; the reducer's own `WHEN cap <= 5` keeps 4 and 5 of those.
    b and c get x4 <= 3, x5 <= 3; a's only reducer reads no row, so its instance is not
    imposed -> 36. Ignoring the clause WHEN gives 19, the local WHEN 29, both 9; an
    error on the empty a reducer would refuse the query."""
    got = _rows(decidb_cli, f"""
        SELECT id, x FROM {_T} {_DECL}
        SUCH THAT WHEN NOT pri PER grp: SUM(WHEN cap <= 5: x) BY (grp) <= 3
        MAXIMIZE SUM(x)
    """, "id", "x")

    obj = _x_vars(oracle_solver, "local_when")
    oracle_solver.add_constraint({"x_4": 1.0}, "<=", 3.0)
    oracle_solver.add_constraint({"x_5": 1.0}, "<=", 3.0)
    best = _optimum(oracle_solver, obj)

    assert got == [(1, 10), (2, 10), (3, 10), (4, 3), (5, 3)]
    assert sum(x for _, x in got) == pytest.approx(best)


# ---------------------------------------------------------------------------
# WHEN + IF
# ---------------------------------------------------------------------------

@pytest.mark.when_perrow
@pytest.mark.var_boolean
@pytest.mark.correctness
def test_when_if_rows_the_filter_drops_open_for_free(decidb_cli, oracle_solver):
    """§3: WHEN runs before IF, so `WHEN NOT pri IF open: x <= cap` has no instance on
    the priority rows and opening them costs nothing. `SUM(open) >= 2` is met by
    opening rows 1 and 3 alone -> 50. Filtering after guarding would cap the opened
    rows (42); a guard read as always-on gives 21."""
    got = _rows(decidb_cli, f"""
        SELECT id, open, x FROM {_T} DECIDE x(INT) BETWEEN 0 AND 10, open(BOOL)
        SUCH THAT WHEN NOT pri IF open: x <= cap AND SUM(open) >= 2
        MAXIMIZE SUM(x)
    """, "id", "open", "x")

    obj = _x_vars(oracle_solver, "when_if")
    for i, (_, cap, pri) in _ROWS.items():
        oracle_solver.add_variable(f"open_{i}", VarType.BINARY)
        if not pri:
            oracle_solver.add_constraint({f"x_{i}": 1.0, f"open_{i}": 10.0 - cap}, "<=", 10.0)
    oracle_solver.add_constraint({f"open_{i}": 1.0 for i in _ROWS}, ">=", 2.0)
    best = _optimum(oracle_solver, obj)

    assert got == [(1, True, 10), (2, False, 10), (3, True, 10), (4, False, 10), (5, False, 10)]
    assert sum(x for _, _, x in got) == pytest.approx(best)


@pytest.mark.when_perrow
@pytest.mark.var_boolean
@pytest.mark.correctness
def test_when_if_with_a_keyed_guard_on_per_row_instances(decidb_cli, oracle_solver):
    """§3.2 / deck p59 (κ -> b): per-row instances may read a `PER grp` guard. `WHEN
    NOT pri IF gopen: x <= cap` caps rows 2, 4, 5 through their group's switch; two
    groups must open, and a (loses 3) and b (loses 5) beat c (loses 6) -> 42 with
    gopen_c = 0. Without the filter a's cost would include row 1 (34); an always-on
    guard gives 21."""
    got = _rows(decidb_cli, f"""
        SELECT id, gopen, x FROM {_T} {_GOPEN}
        SUCH THAT WHEN NOT pri IF gopen: x <= cap AND SUM(PER grp: gopen) >= 2
        MAXIMIZE SUM(x)
    """, "id", "gopen", "x")

    obj = _x_vars(oracle_solver, "when_if_keyed")
    for g in _GROUPS:
        oracle_solver.add_variable(f"gopen_{g}", VarType.BINARY)
    for i, (g, cap, pri) in _ROWS.items():
        if not pri:
            oracle_solver.add_constraint({f"x_{i}": 1.0, f"gopen_{g}": 10.0 - cap}, "<=", 10.0)
    oracle_solver.add_constraint({f"gopen_{g}": 1.0 for g in _GROUPS}, ">=", 2.0)
    best = _optimum(oracle_solver, obj)

    assert got == [(1, True, 10), (2, True, 7), (3, True, 10), (4, True, 5), (5, False, 10)]
    assert sum(x for _, _, x in got) == pytest.approx(best)


@pytest.mark.when_perrow
@pytest.mark.var_boolean
@pytest.mark.cons_between
@pytest.mark.correctness
def test_when_if_prefix_governs_both_between_bounds(decidb_cli, oracle_solver, tmp_path):
    """Delta 8 under WHEN IF: `WHEN pri IF open: x BETWEEN 2 AND 4` is two guarded
    comparisons on rows 1 and 3 (4 Big-M rows plus the count). Weight -1 on row 1;
    `SUM(open) >= 4` lets one row close, and closing row 3 (worth 6) beats row 1
    (worth 2): x = 2, 10, 10, 10, 10 -> 38. An always-on guard gives 32, a never
    imposed one 40, a dropped WHEN 20, a dropped floor or ceiling 40."""
    sql = f"""
        SELECT id, open, x FROM {_tw({1: -1, 2: 1, 3: 1, 4: 1, 5: 1})}
        DECIDE x(INT) BETWEEN 0 AND 10, open(BOOL)
        SUCH THAT WHEN pri IF open: x BETWEEN 2 AND 4 AND SUM(open) >= 4
        MAXIMIZE SUM(x * w)
    """
    got = _rows(decidb_cli, sql, "id", "open", "x")

    _x_vars(oracle_solver, "when_if_between")
    for i in _ROWS:
        oracle_solver.add_variable(f"open_{i}", VarType.BINARY)
    for i in (1, 3):
        # open = 1 ⟹ x >= 2 ;  open = 1 ⟹ x <= 4
        oracle_solver.add_constraint({f"x_{i}": 1.0, f"open_{i}": -2.0}, ">=", 0.0)
        oracle_solver.add_constraint({f"x_{i}": 1.0, f"open_{i}": 6.0}, "<=", 10.0)
    oracle_solver.add_constraint({f"open_{i}": 1.0 for i in _ROWS}, ">=", 4.0)
    best = _optimum(oracle_solver, {"x_1": -1.0, "x_2": 1.0, "x_3": 1.0, "x_4": 1.0, "x_5": 1.0})

    assert got == [(1, True, 2), (2, True, 10), (3, False, 10), (4, True, 10), (5, True, 10)]
    assert -2 + 10 + 10 + 10 + 10 == pytest.approx(best)
    assert _num_rows(decidb_cli, sql, tmp_path) == 5


# ---------------------------------------------------------------------------
# PER + IF
# ---------------------------------------------------------------------------

@pytest.mark.per_clause
@pytest.mark.var_boolean
@pytest.mark.correctness
def test_per_if_reads_the_guard_once_per_instance(decidb_cli, oracle_solver, tmp_path):
    """§3.2 / deck p58: `PER grp IF gopen: SUM(x) BY (grp) <= 5` is one implication
    per group on the group's own switch (3 guarded rows + the count). Two must open;
    c loses 5 and a or b lose 15 -> 30 with c open. An always-on guard gives 15, a
    never imposed one 50."""
    sql = f"""
        SELECT id, gopen, x FROM {_T} {_GOPEN}
        SUCH THAT PER grp IF gopen: SUM(x) BY (grp) <= 5 AND SUM(PER grp: gopen) >= 2
        MAXIMIZE SUM(x)
    """
    got = _rows(decidb_cli, sql, "id", "gopen", "x")

    obj = _x_vars(oracle_solver, "per_if")
    for g, ids in _GROUPS.items():
        oracle_solver.add_variable(f"gopen_{g}", VarType.BINARY)
        big_m = 10.0 * len(ids) - 5.0
        row = {f"x_{i}": 1.0 for i in ids}
        row[f"gopen_{g}"] = big_m
        oracle_solver.add_constraint(row, "<=", 5.0 + big_m)
    oracle_solver.add_constraint({f"gopen_{g}": 1.0 for g in _GROUPS}, ">=", 2.0)
    best = _optimum(oracle_solver, obj)

    by_id = {i: (o, x) for i, o, x in got}
    assert by_id[5] == (True, 5)
    for g, ids in _GROUPS.items():
        if by_id[ids[0]][0]:
            assert sum(by_id[i][1] for i in ids) <= 5
    assert sum(x for _, _, x in got) == pytest.approx(best) == 30
    assert _num_rows(decidb_cli, sql, tmp_path) == 4


@pytest.mark.per_clause
@pytest.mark.var_boolean
@pytest.mark.correctness
def test_per_empty_if_guards_the_query_wide_instance(decidb_cli, oracle_solver):
    """§3: `PER () IF zb: SUM(x) <= 15` is one implication on the query-wide switch.
    Beside `SUM(x) <= 40 + 10 * zb`, closing the switch is worth more (40 against
    15), so zb = 0 -> 40. An always-on guard gives 15."""
    got = _rows(decidb_cli, f"""
        SELECT id, zb, x FROM {_T} DECIDE x(INT) BETWEEN 0 AND 10, PER (): zb(BOOL)
        SUCH THAT PER () IF zb: SUM(x) <= 15 AND PER (): SUM(x) <= 40 + 10 * zb
        MAXIMIZE SUM(x)
    """, "id", "zb", "x")

    obj = _x_vars(oracle_solver, "per_empty_if")
    oracle_solver.add_variable("zb", VarType.BINARY)
    oracle_solver.add_constraint({**obj, "zb": 35.0}, "<=", 50.0)   # zb = 1 ⟹ sum <= 15
    oracle_solver.add_constraint({**obj, "zb": -10.0}, "<=", 40.0)
    best = _optimum(oracle_solver, obj)

    assert all(not zb for _, zb, _ in got)
    assert sum(x for _, _, x in got) == pytest.approx(best) == 40


@pytest.mark.per_clause
@pytest.mark.var_boolean
@pytest.mark.cons_between
@pytest.mark.correctness
def test_per_if_prefix_governs_both_between_bounds(decidb_cli, oracle_solver):
    """Delta 8 under PER IF: `PER grp IF gopen: SUM(x) BY (grp) BETWEEN 2 AND 4` guards
    a floor and a ceiling per group. With weights -1 on a, opening a costs 2 (floor),
    b 16 and c 6 (ceiling); two must open -> a and c: -2 + 20 + 4 = 22. A dropped
    floor gives 24, a dropped ceiling 30, an always-on guard 6; the old defect
    imposed every closed group's bounds."""
    got = _rows(decidb_cli, f"""
        SELECT id, gopen, x FROM {_tw({1: -1, 2: -1, 3: 1, 4: 1, 5: 1})} {_GOPEN}
        SUCH THAT PER grp IF gopen: SUM(x) BY (grp) BETWEEN 2 AND 4 AND SUM(PER grp: gopen) >= 2
        MAXIMIZE SUM(x * w)
    """, "id", "gopen", "x")

    _x_vars(oracle_solver, "per_if_between")
    for g, ids in _GROUPS.items():
        oracle_solver.add_variable(f"gopen_{g}", VarType.BINARY)
        big_m = 10.0 * len(ids) - 4.0
        oracle_solver.add_constraint({**{f"x_{i}": 1.0 for i in ids}, f"gopen_{g}": -2.0}, ">=", 0.0)
        oracle_solver.add_constraint({**{f"x_{i}": 1.0 for i in ids}, f"gopen_{g}": big_m}, "<=", 4.0 + big_m)
    oracle_solver.add_constraint({f"gopen_{g}": 1.0 for g in _GROUPS}, ">=", 2.0)
    best = _optimum(oracle_solver, {"x_1": -1.0, "x_2": -1.0, "x_3": 1.0, "x_4": 1.0, "x_5": 1.0})

    by_id = {i: (o, x) for i, o, x in got}
    assert [by_id[i][0] for i in (1, 3, 5)] == [True, False, True]
    assert by_id[1][1] + by_id[2][1] == 2 and by_id[3][1] + by_id[4][1] == 20 and by_id[5][1] == 4
    assert -2 + 20 + 4 == pytest.approx(best)


# ---------------------------------------------------------------------------
# WHEN + PER + IF
# ---------------------------------------------------------------------------

_FULL_W = {1: 2, 2: 1, 3: 1, 4: 1, 5: 1}
_FULL_CLAUSE = ("SUCH THAT WHEN pri PER grp IF gopen: SUM(x) BY (grp) <= 5 "
                "AND SUM(PER grp: gopen) >= 2 MAXIMIZE SUM(x * w)")


def _full_prefix_oracle(oracle):
    """`WHEN pri PER grp IF gopen: SUM(x) BY (grp) <= 5`, two switches on, row 1 weighs 2."""
    _x_vars(oracle, "full_reducer")
    for g in _GROUPS:
        oracle.add_variable(f"gopen_{g}", VarType.BINARY)
    for i, g in ((1, "a"), (3, "b")):
        oracle.add_constraint({f"x_{i}": 1.0, f"gopen_{g}": 5.0}, "<=", 10.0)
    oracle.add_constraint({f"gopen_{g}": 1.0 for g in _GROUPS}, ">=", 2.0)
    return _optimum(oracle, {f"x_{i}": float(w) for i, w in _FULL_W.items()})


@pytest.mark.when_constraint
@pytest.mark.per_clause
@pytest.mark.var_boolean
@pytest.mark.correctness
def test_full_prefix_on_a_reducer_body(decidb_cli, oracle_solver, tmp_path):
    """§3 / deck p58-59: `WHEN pri PER grp IF gopen: SUM(x) BY (grp) <= 5` filters to
    rows 1 and 3, generates a and b (c is emptied), and guards each on its switch.
    Two of three switches must be on and row 1 weighs 2, so closing a (worth 10)
    beats closing b (5) or c (nothing): x = 10, 10, 5, 10, 10 -> 55 on 2 guarded
    rows plus the count. An always-on guard gives 45, a never imposed one 60, a
    dropped filter 40 (every group guarded, a closed), a c instance read over the
    unfiltered rows would cap x5 when c is open."""
    sql = f"SELECT id, gopen, x FROM {_tw(_FULL_W)} {_GOPEN} {_FULL_CLAUSE}"
    got = _rows(decidb_cli, sql, "id", "gopen", "x")
    best = _full_prefix_oracle(oracle_solver)

    assert got == [(1, False, 10), (2, False, 10), (3, True, 5), (4, True, 10), (5, True, 10)]
    assert 2 * 10 + 10 + 5 + 10 + 10 == pytest.approx(best) == 55
    assert _num_rows(decidb_cli, sql, tmp_path) == 3


@pytest.mark.when_constraint
@pytest.mark.per_clause
@pytest.mark.var_boolean
@pytest.mark.correctness
def test_full_prefix_on_a_keyed_decision_body(decidb_cli, oracle_solver):
    """§3.1 / §3.2: under `PER grp, cap` both the `PER grp` decision and the `PER grp`
    guard are determined. `WHEN pri` keeps (a, 3) and (b, 2); two switches must be
    on and `SUM(y)` counts y once per row, so closing b (frees 2 * 8 = 16) beats
    closing a (14) or c (no instance): y_a = 3, y_b = y_c = 10 -> 36. An always-on
    guard gives 20, a never imposed one 30, a dropped filter 30 (c capped at 4)."""
    got = _rows(decidb_cli, f"""
        SELECT id, gopen, y FROM {_T} DECIDE PER grp: y(INT) BETWEEN 0 AND 10, PER grp: gopen(BOOL)
        SUCH THAT WHEN pri PER grp, cap IF gopen: y <= cap AND SUM(PER grp: gopen) >= 2
        MAXIMIZE SUM(y)
    """, "id", "gopen", "y")

    oracle_solver.create_model("full_keyed")
    for g in _GROUPS:
        oracle_solver.add_variable(f"y_{g}", VarType.INTEGER, lb=0.0, ub=10.0)
        oracle_solver.add_variable(f"gopen_{g}", VarType.BINARY)
    for g, cap in (("a", 3.0), ("b", 2.0)):
        oracle_solver.add_constraint({f"y_{g}": 1.0, f"gopen_{g}": 10.0 - cap}, "<=", 10.0)
    oracle_solver.add_constraint({f"gopen_{g}": 1.0 for g in _GROUPS}, ">=", 2.0)
    best = _optimum(oracle_solver, {f"y_{g}": float(len(ids)) for g, ids in _GROUPS.items()})

    assert got == [(1, True, 3), (2, True, 3), (3, False, 10), (4, False, 10), (5, True, 10)]
    assert sum(y for _, _, y in got) == pytest.approx(best) == 36


@pytest.mark.when_constraint
@pytest.mark.per_clause
@pytest.mark.var_boolean
@pytest.mark.cons_between
@pytest.mark.correctness
def test_full_prefix_governs_both_between_bounds(decidb_cli, oracle_solver):
    """Delta 8 under the full prefix: `WHEN NOT pri PER grp IF gopen: SUM(x) BY (grp)
    BETWEEN 2 AND 4` generates on rows 2, 4, 5. Weights -1 on row 2 and 2 on row 5:
    opening a costs 2 (the floor), b 6 and c 12 (the ceiling); two must open -> a
    and b: 10 - 2 + 10 + 4 + 20 = 42. An always-on guard gives 30, a never imposed
    one 50, a dropped filter 32 (a's floor then reads x1 + x2), a dropped floor 44,
    a dropped ceiling 50."""
    got = _rows(decidb_cli, f"""
        SELECT id, gopen, x FROM {_tw({1: 1, 2: -1, 3: 1, 4: 1, 5: 2})} {_GOPEN}
        SUCH THAT WHEN NOT pri PER grp IF gopen: SUM(x) BY (grp) BETWEEN 2 AND 4
              AND SUM(PER grp: gopen) >= 2
        MAXIMIZE SUM(x * w)
    """, "id", "gopen", "x")

    _x_vars(oracle_solver, "full_between")
    for g in _GROUPS:
        oracle_solver.add_variable(f"gopen_{g}", VarType.BINARY)
    for i, g in ((2, "a"), (4, "b"), (5, "c")):
        oracle_solver.add_constraint({f"x_{i}": 1.0, f"gopen_{g}": -2.0}, ">=", 0.0)
        oracle_solver.add_constraint({f"x_{i}": 1.0, f"gopen_{g}": 6.0}, "<=", 10.0)
    oracle_solver.add_constraint({f"gopen_{g}": 1.0 for g in _GROUPS}, ">=", 2.0)
    best = _optimum(oracle_solver, {"x_1": 1.0, "x_2": -1.0, "x_3": 1.0, "x_4": 1.0, "x_5": 2.0})

    assert got == [(1, True, 10), (2, True, 2), (3, True, 10), (4, True, 4), (5, False, 10)]
    assert 10 - 2 + 10 + 4 + 2 * 10 == pytest.approx(best) == 42


@pytest.mark.when_constraint
@pytest.mark.per_clause
@pytest.mark.correctness
def test_both_clause_orders_give_identical_rows(decidb_cli, oracle_solver):
    """§1: the split order (declaration between SELECT and FROM) and the single-block
    order parse to one plan. The full-prefix query of the previous test returns the
    same rows either way (55); a clause order that lost a prefix would return 40, 45
    or 60."""
    rel = _tw(_FULL_W)
    single = _rows(decidb_cli, f"SELECT id, gopen, x FROM {rel} {_GOPEN} {_FULL_CLAUSE}", "id", "gopen", "x")
    split = _rows(decidb_cli, f"SELECT id, gopen, x {_GOPEN} FROM {rel} {_FULL_CLAUSE}", "id", "gopen", "x")
    best = _full_prefix_oracle(oracle_solver)

    assert single == split
    assert single == [(1, False, 10), (2, False, 10), (3, True, 5), (4, True, 10), (5, True, 10)]
    assert 2 * 10 + 10 + 5 + 10 + 10 == pytest.approx(best) == 55


@pytest.mark.explain
def test_explain_prints_the_prefixes_before_the_body(decidb_cli):
    """§8: EXPLAIN lists each clause as written, prefixes first, and the keyed
    declaration with its scope."""
    result = decidb_cli.execute_raw(f"""
        EXPLAIN SELECT id, gopen, x FROM {_T} {_GOPEN}
        SUCH THAT WHEN pri PER grp IF gopen: SUM(x) BY (grp) <= 5 AND SUM(PER grp: gopen) >= 3
        MAXIMIZE SUM(x)
    """)
    compact = _compact(result.stdout + result.stderr)
    assert "WHENpriPERgrpIFgopen:SUM(x)BY(grp)<=5" in compact, result.stdout
    assert "SUM(PERgrp:gopen)>=3" in compact and "gopenPERgrp" in compact


# ---------------------------------------------------------------------------
# NULL keys and filters that admit nothing
# ---------------------------------------------------------------------------

@pytest.mark.per_clause
@pytest.mark.edge_case
@pytest.mark.correctness
def test_null_is_a_key_value_and_when_excludes_it(decidb_cli, oracle_solver, tmp_path):
    """§2.1 / delta 1: NULL is a PER key value like any other, so the NULL-keyed row
    gets its own instance (x6 <= 5 -> 20 on 4 rows); `WHEN grp IS NOT NULL PER grp`
    is how it is excluded (x6 free -> 25 on 3 rows). Skipping the NULL key silently
    gives 25 without the filter."""
    keyed = f"SELECT id, x FROM {_TN} {_DECL} SUCH THAT PER grp: SUM(x) BY (grp) <= 5 MAXIMIZE SUM(x)"
    filtered = keyed.replace("SUCH THAT PER grp", "SUCH THAT WHEN grp IS NOT NULL PER grp")
    with_null = _rows(decidb_cli, keyed, "id", "x")
    without = _rows(decidb_cli, filtered, "id", "x")

    ids = {**_ROWS, 6: (None, 6, False)}
    obj = _x_vars(oracle_solver, "null_key", ids=ids)
    for g in _GROUPS.values():
        oracle_solver.add_constraint({f"x_{i}": 1.0 for i in g}, "<=", 5.0)
    oracle_solver.add_constraint({"x_6": 1.0}, "<=", 5.0)
    best = _optimum(oracle_solver, obj)
    obj = _x_vars(oracle_solver, "null_key_filtered", ids=ids)
    for g in _GROUPS.values():
        oracle_solver.add_constraint({f"x_{i}": 1.0 for i in g}, "<=", 5.0)
    best_filtered = _optimum(oracle_solver, obj)

    assert dict(with_null)[6] == 5 and sum(x for _, x in with_null) == pytest.approx(best) == 20
    assert dict(without)[6] == 10 and sum(x for _, x in without) == pytest.approx(best_filtered) == 25
    assert _num_rows(decidb_cli, keyed, tmp_path) == 4
    assert _num_rows(decidb_cli, filtered, tmp_path) == 3


@pytest.mark.when_constraint
@pytest.mark.per_clause
@pytest.mark.edge_case
@pytest.mark.correctness
def test_when_leaving_only_null_keyed_rows_generates_their_instance(decidb_cli, oracle_solver):
    """Delta 1 with a filter: `WHEN grp IS NULL PER grp` keeps row 6 alone, whose NULL
    key is a value, so one instance x6 <= 5 is generated -> 55. Treating NULL as no
    key gives 60; the old binary raised an empty-row-set error."""
    got = _rows(decidb_cli, f"""
        SELECT id, x FROM {_TN} {_DECL}
        SUCH THAT WHEN grp IS NULL PER grp: SUM(x) BY (grp) <= 5
        MAXIMIZE SUM(x)
    """, "id", "x")

    obj = _x_vars(oracle_solver, "null_only", ids={**_ROWS, 6: (None, 6, False)})
    oracle_solver.add_constraint({"x_6": 1.0}, "<=", 5.0)
    best = _optimum(oracle_solver, obj)

    assert got == [(1, 10), (2, 10), (3, 10), (4, 10), (5, 10), (6, 5)]
    assert sum(x for _, x in got) == pytest.approx(best)


@pytest.mark.when_constraint
@pytest.mark.edge_case
@pytest.mark.correctness
def test_when_that_admits_nothing_imposes_nothing(decidb_cli, oracle_solver, tmp_path):
    """§4 / delta 2: a constraint with no instance at all imposes nothing and is not
    an error, whether the body is a reducer under PER, a per-row bound, a query-wide
    reducer or a guarded row: every x stays at 10 -> 50 on 0 model rows. The old
    binary raised an empty-row-set error for the reducer forms."""
    forms = [
        "WHEN cap > 100 PER grp: SUM(x) BY (grp) <= 5",
        "WHEN cap > 100: x <= 0",
        "WHEN cap > 100 PER (): SUM(x) <= 5",
        "WHEN cap > 100 IF open: x <= 0 AND SUM(open) >= 5",
    ]
    decl = "DECIDE x(INT) BETWEEN 0 AND 10, open(BOOL)"
    best = _optimum(oracle_solver, _x_vars(oracle_solver, "when_nothing"))
    for form in forms:
        sql = f"SELECT id, x FROM {_T} {decl} SUCH THAT {form} MAXIMIZE SUM(x)"
        got = _rows(decidb_cli, sql, "id", "x")
        assert got == [(i, 10) for i in _ROWS], form
        assert sum(x for _, x in got) == pytest.approx(best) == 50
    assert _num_rows(decidb_cli, f"SELECT id, x FROM {_T} {decl} SUCH THAT {forms[0]} MAXIMIZE SUM(x)",
                     tmp_path) == 0


# ---------------------------------------------------------------------------
# Reducer-local prefixes
# ---------------------------------------------------------------------------

@pytest.mark.per_clause
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_reducer_local_per_counts_one_term_per_key(decidb_cli, oracle_solver):
    """§4 / deck p23: `SUM(PER grp: y)` charges each group once, `SUM(y)` once per
    row. Under `<= 6` with a row-weighted objective the first allows y_a = 6 (12), the
    second only 2 y_a + 2 y_b + y_c <= 6 (6). A local PER ignored gives 6 for both."""
    keyed = f"SELECT id, y FROM {_T} DECIDE PER grp: y(INT) BETWEEN 0 AND 10 SUCH THAT PER (): SUM(PER grp: y) <= 6 MAXIMIZE SUM(y)"
    per_row = keyed.replace("SUM(PER grp: y)", "SUM(y)")
    once = _rows(decidb_cli, keyed, "id", "y")
    weighted = _rows(decidb_cli, per_row, "id", "y")

    oracle_solver.create_model("local_per")
    for g in _GROUPS:
        oracle_solver.add_variable(f"y_{g}", VarType.INTEGER, lb=0.0, ub=10.0)
    obj = {f"y_{g}": float(len(ids)) for g, ids in _GROUPS.items()}
    oracle_solver.add_constraint({f"y_{g}": 1.0 for g in _GROUPS}, "<=", 6.0)
    best_once = _optimum(oracle_solver, obj)
    oracle_solver.create_model("local_per_row")
    for g in _GROUPS:
        oracle_solver.add_variable(f"y_{g}", VarType.INTEGER, lb=0.0, ub=10.0)
    oracle_solver.add_constraint(dict(obj), "<=", 6.0)
    best_weighted = _optimum(oracle_solver, obj)

    assert sum(y for _, y in once) == pytest.approx(best_once) == 12
    assert sum(y for _, y in weighted) == pytest.approx(best_weighted) == 6


@pytest.mark.per_clause
@pytest.mark.when_constraint
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_reducer_local_when_and_per_together(decidb_cli, oracle_solver):
    """§4 / deck p23: `SUM(WHEN grp <> 'a' PER grp: y)` filters to rows 3, 4, 5, then
    counts one term per group: y_b + y_c <= 6 with y_a free, and `SUM(y)` weighs b
    twice -> y_b = 6: 20 + 12 + 0 = 32. Without the local WHEN a is charged too (12);
    without the local PER the two b rows count twice (2 y_b + y_c <= 6: 26); without
    both 6. The filter must keep a two-row group, or a row-weighted sum would agree."""
    got = _rows(decidb_cli, f"""
        SELECT id, y FROM {_T} DECIDE PER grp: y(INT) BETWEEN 0 AND 10
        SUCH THAT PER (): SUM(WHEN grp <> 'a' PER grp: y) <= 6
        MAXIMIZE SUM(y)
    """, "id", "y")

    oracle_solver.create_model("local_when_per")
    for g in _GROUPS:
        oracle_solver.add_variable(f"y_{g}", VarType.INTEGER, lb=0.0, ub=10.0)
    oracle_solver.add_constraint({"y_b": 1.0, "y_c": 1.0}, "<=", 6.0)
    best = _optimum(oracle_solver, {f"y_{g}": float(len(ids)) for g, ids in _GROUPS.items()})

    assert got == [(1, 10), (2, 10), (3, 6), (4, 6), (5, 0)]
    assert sum(y for _, y in got) == pytest.approx(best) == 32


@pytest.mark.per_clause
@pytest.mark.cons_aggregate
@pytest.mark.edge_case
@pytest.mark.correctness
def test_empty_reducer_beside_a_non_empty_one_contributes_nothing(decidb_cli, oracle_solver):
    """§4 / delta 2 (deck p45): in `PER grp: SUM(WHEN pri: x) BY (grp) + SUM(x) BY
    (grp) <= 5` group c's filtered reducer reads no row and contributes nothing, so its
    instance is x5 <= 5; a and b get 2 x1 + x2 <= 5 and 2 x3 + x4 <= 5 -> 15. Dropping
    the c instance gives 20; an error would refuse the query."""
    got = _rows(decidb_cli, f"""
        SELECT id, x FROM {_T} {_DECL}
        SUCH THAT PER grp: SUM(WHEN pri: x) BY (grp) + SUM(x) BY (grp) <= 5
        MAXIMIZE SUM(x)
    """, "id", "x")

    obj = _x_vars(oracle_solver, "empty_beside")
    oracle_solver.add_constraint({"x_1": 2.0, "x_2": 1.0}, "<=", 5.0)
    oracle_solver.add_constraint({"x_3": 2.0, "x_4": 1.0}, "<=", 5.0)
    oracle_solver.add_constraint({"x_5": 1.0}, "<=", 5.0)
    best = _optimum(oracle_solver, obj)

    assert got == [(1, 0), (2, 5), (3, 0), (4, 5), (5, 5)]
    assert sum(x for _, x in got) == pytest.approx(best)


@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_objective_per_empty_is_the_explicit_default(decidb_cli, oracle_solver):
    """§6 / deck p62: an objective is generated once, so `MAXIMIZE PER (): SUM(x)`
    is the plain objective (each group <= 5 -> 15) and any other key is refused."""
    base = f"SELECT id, x FROM {_T} {_DECL} SUCH THAT PER grp: SUM(x) BY (grp) <= 5"
    keyed = _rows(decidb_cli, f"{base} MAXIMIZE PER (): SUM(x)", "id", "x")

    obj = _x_vars(oracle_solver, "objective_per_empty")
    for ids in _GROUPS.values():
        oracle_solver.add_constraint({f"x_{i}": 1.0 for i in ids}, "<=", 5.0)
    best = _optimum(oracle_solver, obj)

    assert keyed == _rows(decidb_cli, f"{base} MAXIMIZE SUM(x)", "id", "x")
    assert sum(x for _, x in keyed) == pytest.approx(best) == 15
    decidb_cli.assert_error(f"{base} MAXIMIZE PER grp: SUM(x) BY (grp)", match=r"takes no key")


# ---------------------------------------------------------------------------
# Refusals the grammar and binder name
# ---------------------------------------------------------------------------

@pytest.mark.error
@pytest.mark.error_parser
def test_prefixes_out_of_order_are_named(decidb_cli):
    """§3: the order is WHEN, PER, IF; every other order is a parser error naming it."""
    for prefix in ("PER grp WHEN pri", "IF gopen PER grp", "IF gopen WHEN pri",
                   "WHEN pri IF gopen PER grp", "PER grp IF gopen WHEN pri"):
        decidb_cli.assert_error(
            f"SELECT id, x FROM {_T} {_GOPEN} SUCH THAT {prefix}: SUM(x) BY (grp) <= 5 MAXIMIZE SUM(x)",
            match=r"prefixes are written")


@pytest.mark.error
@pytest.mark.error_parser
def test_retired_postfix_spellings_are_named(decidb_cli):
    """§3 / delta 16: the pre-redesign `... PER k`, `... WHEN c` and `SUM(K: e)` are
    parser errors that name the prefix form."""
    decidb_cli.assert_error(
        f"SELECT id, x FROM {_T} {_DECL} SUCH THAT SUM(x) BY (grp) <= 5 PER grp MAXIMIZE SUM(x)",
        match=r"PER is a prefix")
    decidb_cli.assert_error(
        f"SELECT id, x FROM {_T} {_DECL} SUCH THAT x <= cap WHEN pri MAXIMIZE SUM(x)",
        match=r"WHEN is a prefix")
    decidb_cli.assert_error(
        f"SELECT id, x FROM {_T} {_DECL} SUCH THAT SUM(grp: x) <= 5 MAXIMIZE SUM(x)",
        match=r"written with PER")


@pytest.mark.error
@pytest.mark.error_binder
def test_per_key_naming_a_decision_is_refused(decidb_cli):
    """§4 / delta 13: a key names columns or relations, never a decision -- in a
    constraint prefix (keyed or per-row decision) and in a declarator."""
    decidb_cli.assert_error(
        f"SELECT id, x FROM {_T} {_GOPEN} SUCH THAT PER gopen: SUM(x) BY () <= 5 MAXIMIZE SUM(x)",
        match=r"is a decision")
    decidb_cli.assert_error(
        f"SELECT id, x FROM {_T} {_DECL} SUCH THAT PER x: SUM(x) BY () <= 5 MAXIMIZE SUM(x)",
        match=r"is a decision")
    decidb_cli.assert_error(
        f"SELECT id, x FROM {_T} DECIDE open(BOOL), PER open: x(INT) BETWEEN 0 AND 10 "
        f"SUCH THAT x <= 5 MAXIMIZE SUM(x)",
        match=r"is a decision")


@pytest.mark.error
@pytest.mark.error_binder
def test_when_and_if_point_at_each_other(decidb_cli):
    """§3: a decision inside WHEN is redirected to IF, and known data inside IF to WHEN."""
    decidb_cli.assert_error(
        f"SELECT id, x FROM {_T} DECIDE x(INT) BETWEEN 0 AND 10, open(BOOL) "
        f"SUCH THAT WHEN open: x <= cap MAXIMIZE SUM(x)",
        match=r"cannot reference a decision")
    decidb_cli.assert_error(
        f"SELECT id, x FROM {_T} {_DECL} SUCH THAT IF pri: x <= cap MAXIMIZE SUM(x)",
        match=r"must reference a decision")
