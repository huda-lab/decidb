"""Combinations of the DeciQL surface that the other suites leave untested (§2-§6).

The prefix grid crosses `[WHEN] [PER] [IF]` with row decisions and SUM bodies; this
file fills the pairs around it: keyed, query-wide and mixed-scope bodies under WHEN /
IF; the prefixes over `=`, `<>`, IN, AVG, MIN/MAX, a reducer's own WHEN, a reducer
beside a row term, two reducers and frames; and body kinds under THEN, a nested
MIN/MAX, SATISFY and a filtered objective. Each case is checked against an
independent gurobipy model (guards as indicator constraints, one row per written
instance); table cases also run a mutant with the construct under test removed and
require a different optimum. The comment above each case names the rule and the
answer a broken implementation gives.

Data: t(id, grp, cap, pri) with caps a = 3, 7; b = 2, 5; c = 4 and the priority rows
1 (a) and 3 (b). `x` is INT in [0, 10].
"""

import pytest

from solver.types import ObjSense, SolverStatus, VarType

MAX, MIN = ObjSense.MAXIMIZE, ObjSense.MINIMIZE
_ROWS = {1: ("a", 3, True), 2: ("a", 7, False), 3: ("b", 2, True),
         4: ("b", 5, False), 5: ("c", 4, False)}
_GROUPS = {"a": (1, 2), "b": (3, 4), "c": (5,)}
_T = ("(VALUES (1, 'a', 3, true), (2, 'a', 7, false), (3, 'b', 2, true), "
      "(4, 'b', 5, false), (5, 'c', 4, false)) t(id, grp, cap, pri)")
_DECL = "DECIDE x(INT) BETWEEN 0 AND 10"
_GOPEN = "DECIDE x(INT) BETWEEN 0 AND 10, PER grp: gopen(BOOL)"
_OPEN = "DECIDE x(INT) BETWEEN 0 AND 10, open(BOOL)"


def _rows(decidb_cli, sql, *cols):
    rows, names = decidb_cli.execute(sql)
    idx = [names.index(c) for c in cols]
    return sorted(tuple(r[i] for i in idx) for r in rows)


def _tw(w):
    """The base relation with a weight column `w` (1 unless overridden)."""
    w = {**{i: 1 for i in _ROWS}, **w}
    vals = ", ".join(f"({i}, '{g}', {c}, {str(p).lower()}, {w[i]})" for i, (g, c, p) in _ROWS.items())
    return f"(VALUES {vals}) t(id, grp, cap, pri, w)", w


def _value(got, w):
    """SUM(x * w) over rows whose last column is x."""
    return sum(w[r[0]] * r[-1] for r in got)


def _x(*ids, c=1.0):
    return {f"x_{i}": c for i in ids}


def _row(o, coeffs, sense, rhs):
    o.add_constraint(coeffs, sense, float(rhs))


def _ind(o, switch, coeffs, sense, rhs, on=1):
    o.add_indicator_constraint(switch, on, coeffs, sense, float(rhs))


def _ne(o, coeffs, value, tag):
    """coeffs <> value over integers: one below or one above, a binary picks the side."""
    o.add_variable(f"ne_{tag}", VarType.BINARY)
    _ind(o, f"ne_{tag}", coeffs, "<=", value - 1, on=0)
    _ind(o, f"ne_{tag}", coeffs, ">=", value + 1)


def _one_of(o, var, values):
    """var IN values: one indicator per value, exactly one on."""
    zs = {f"{var}_is_{v}": float(v) for v in values}
    for z in zs:
        o.add_variable(z, VarType.BINARY)
    o.add_constraint({z: 1.0 for z in zs}, "=", 1.0)
    o.add_constraint({var: 1.0, **{z: -v for z, v in zs.items()}}, "=", 0.0)


def _reaches(o, ids, rhs, tag):
    """MAX(x over ids) >= rhs: some row of the group reaches rhs."""
    for i in ids:
        o.add_variable(f"top_{tag}_{i}", VarType.BINARY)
        _ind(o, f"top_{tag}_{i}", _x(i), ">=", rhs)
    o.add_constraint({f"top_{tag}_{i}": 1.0 for i in ids}, ">=", 1.0)


def _model(o, name, x=True, gopen=False, opens=False, ub=None):
    o.create_model(name)
    for i in _ROWS if x else ():
        o.add_variable(f"x_{i}", VarType.INTEGER, lb=0.0, ub=float((ub or {}).get(i, 10)))
    for g in _GROUPS if gopen else ():
        o.add_variable(f"gopen_{g}", VarType.BINARY)
    for i in _ROWS if opens else ():
        o.add_variable(f"open_{i}", VarType.BINARY)


def _solve(o, obj, sense=MAX):
    o.set_objective(obj, sense)
    result = o.solve()
    assert result.status == SolverStatus.OPTIMAL
    return result


def _groups_open(got):
    """{group: switch} from rows whose second column is the group's switch."""
    by_id = {r[0]: r[1] for r in got}
    return {g: bool(by_id[ids[0]]) for g, ids in _GROUPS.items()}


# ---------------------------------------------------------------------------
# WHEN and PER over the body kinds (row decision x)
# ---------------------------------------------------------------------------

_X_CASES = [
    # §3.3 `=` under WHEN holds from both sides on rows 1, 3 only (w3 = -1 needs the
    # floor). No WHEN pins every row to its cap (17); `=` read as `<=` gives x3 = 0.
    pytest.param("WHEN pri: x = cap", {3: -1}, MAX,
                 lambda o: (_row(o, _x(1), "=", 3), _row(o, _x(3), "=", 2)),
                 [3, 10, 2, 10, 10], "x = cap", id="when-equality"),
    # §3.3 `<>` under WHEN steps rows 1, 3 off 10 (48); unfiltered every row (45).
    pytest.param("WHEN pri: x <> 10", {}, MAX,
                 lambda o: (_ne(o, _x(1), 10, 1), _ne(o, _x(3), 10, 3)),
                 [9, 10, 9, 10, 10], "x <> 10", id="when-not-equal"),
    # §3.3 IN under WHEN is one-hot on rows 1, 3 only; w3 = -1 picks the list's low end
    # (33). Unfiltered every row lands in the list (15).
    pytest.param("WHEN pri: x IN (1, 4)", {3: -1}, MAX,
                 lambda o: (_one_of(o, "x_1", (1, 4)), _one_of(o, "x_3", (1, 4))),
                 [4, 10, 1, 10, 10], "x IN (1, 4)", id="when-in-list"),
    # §4 easy-direction MAX reads the filtered rows' maximum: rows 2, 4, 5 capped (32);
    # unfiltered 20.
    pytest.param("WHEN NOT pri: MAX(x) <= 4", {}, MAX,
                 lambda o: [_row(o, _x(i), "<=", 4) for i in (2, 4, 5)],
                 [10, 4, 10, 4, 4], "MAX(x) <= 4", id="when-easy-max"),
    # §4 AVG divides by the rows WHEN admits (3): x5 = 9 (38). Unfiltered (5 rows) 25;
    # the filtered sum over the unfiltered count 45.
    pytest.param("WHEN NOT pri: AVG(x) <= 3", {5: 2}, MAX,
                 lambda o: _row(o, _x(2, 4, 5, c=1 / 3), "<=", 3),
                 [10, 0, 10, 0, 9], "AVG(x) <= 3", id="when-avg"),
    # §4 a reducer's own WHEN inside the clause's: rows 2, 4, 5, then cap <= 5 keeps 4
    # and 5 (36). Without the local WHEN x2 joins the sum (26).
    pytest.param("WHEN NOT pri: SUM(WHEN cap <= 5: x) <= 3", {5: 2}, MAX,
                 lambda o: _row(o, _x(4, 5), "<=", 3),
                 [10, 10, 10, 0, 3], "WHEN NOT pri: SUM(x) <= 3", id="when-local-when"),
    # §4 matrix row 5 under WHEN: each admitted row is alone in its filtered group, so
    # x + SUM(x) BY (grp) is 2x (38). The unfiltered groups tie x1 to x2, x3 to x4 (22).
    pytest.param("WHEN NOT pri: x + SUM(x) BY (grp) <= 12", {}, MAX,
                 lambda o: [_row(o, _x(i, c=2.0), "<=", 12) for i in (2, 4, 5)],
                 [10, 6, 10, 6, 6], "x + SUM(x) BY (grp) <= 12", id="when-reducer-beside-row-term"),
    # §4 AVG under WHEN PER: a averages rows 1, 2, b is row 4 alone, c row 5 (28).
    # Unfiltered b averages x3 and x4 (21).
    pytest.param("WHEN cap > 2 PER grp: AVG(x) BY (grp) <= 3", {1: 2}, MAX,
                 lambda o: (_row(o, _x(1, 2, c=0.5), "<=", 3), _row(o, _x(4), "<=", 3),
                            _row(o, _x(5), "<=", 3)),
                 [6, 0, 10, 3, 3], "PER grp: AVG(x) BY (grp) <= 3", id="when-per-avg"),
    # §4 hard-direction MAX under WHEN PER: b's only admitted row is 4, so it must reach
    # 6 there although row 3 is cheaper (30); unfiltered b uses row 3 (24). `x <= 12 -
    # cap` keeps row 2 below 6, so a needs the dearer row 1: MAX read as SUM spreads
    # a over x2 = 5, x1 = 1 (25).
    pytest.param("WHEN cap > 2 PER grp: MAX(x) BY (grp) >= 6 AND x <= 12 - cap", {1: 2, 4: 2}, MIN,
                 lambda o: (_reaches(o, (1, 2), 6, "a"), _reaches(o, (4,), 6, "b"),
                            _reaches(o, (5,), 6, "c"),
                            [_row(o, _x(i), "<=", 12 - c) for i, (_, c, _) in _ROWS.items()]),
                 [6, 0, 0, 6, 6], "PER grp: MAX(x) BY (grp) >= 6 AND x <= 12 - cap", id="when-per-hard-max"),
    # §3.3 `<>` under WHEN PER: only a's two admitted rows can reach 20 (59); unfiltered
    # b's x3 + x4 is stepped off 20 too (58).
    pytest.param("WHEN cap > 2 PER grp: SUM(x) BY (grp) <> 20", {2: 2}, MAX,
                 lambda o: (_ne(o, _x(1, 2), 20, "a"), _ne(o, _x(4), 20, "b"), _ne(o, _x(5), 20, "c")),
                 [9, 10, 10, 10, 10], "PER grp: SUM(x) BY (grp) <> 20", id="when-per-not-equal"),
    # §3.3 `=` under WHEN PER: w5 = -1 needs c's floor, x3 is in no instance (18).
    # Unfiltered b pins x3 too (8); `=` read as `<=` lets x5 sit at 0 (22).
    pytest.param("WHEN cap > 2 PER grp: SUM(x) BY (grp) = 4", {1: 2, 5: -1}, MAX,
                 lambda o: (_row(o, _x(1, 2), "=", 4), _row(o, _x(4), "=", 4), _row(o, _x(5), "=", 4)),
                 [4, 0, 10, 4, 4], "PER grp: SUM(x) BY (grp) = 4", id="when-per-equality"),
    # §3.1 refinement over AVG: `PER grp, cap` writes five instances (the oracle states
    # each) that reduce to each group's tightest cap (48 with w = id). The loosest cap
    # per group gives 84, and the tightest cap of all (2) on every group gives 34. The
    # per-row form without the PER states the same five bounds (48, equivalent), so the
    # mutant reads AVG as SUM instead (34).
    pytest.param("PER grp, cap: AVG(x) BY (grp) <= cap", {i: i for i in _ROWS}, MAX,
                 lambda o: [_row(o, _x(*_GROUPS[g], c=1 / len(_GROUPS[g])), "<=", cap)
                            for g, cap, _ in _ROWS.values()],
                 [0, 6, 0, 4, 4], "PER grp, cap: SUM(x) BY (grp) <= cap", id="per-two-columns-avg"),
]


@pytest.mark.when_constraint
@pytest.mark.per_clause
@pytest.mark.correctness
@pytest.mark.parametrize("clause, w, sense, build, expected, mutant", _X_CASES)
def test_prefix_over_each_body_kind(decidb_cli, oracle_solver, clause, w, sense, build, expected, mutant):
    """§3 / §4: WHEN, WHEN PER and PER (a, b) over the body kinds the prefix grid leaves
    out. The case comment names the rule and a broken implementation's answer; the
    mutant must change the optimum."""
    rel, w = _tw(w)
    word = "MAXIMIZE" if sense is MAX else "MINIMIZE"
    sql = lambda c: f"SELECT id, x FROM {rel} {_DECL} SUCH THAT {c} {word} SUM(x * w)"  # noqa: E731
    got = _rows(decidb_cli, sql(clause), "id", "x")

    _model(oracle_solver, "prefix_body")
    build(oracle_solver)
    best = _solve(oracle_solver, {f"x_{i}": float(w[i]) for i in _ROWS}, sense).objective_value

    assert [x for _, x in got] == expected
    assert _value(got, w) == pytest.approx(best)
    if mutant is not None:
        assert _value(_rows(decidb_cli, sql(mutant), "id", "x"), w) != pytest.approx(best)


# ---------------------------------------------------------------------------
# Guarded group instances (PER grp switch gopen, two of three must open)
# ---------------------------------------------------------------------------

_G_CASES = [
    # §4 AVG under PER IF: an open a holds x1 + x2 <= 4, b's loss keeps it closed (70).
    # AVG read as SUM caps a at 2 (66); an always-on guard gives 22.
    pytest.param("PER grp IF gopen: AVG(x) BY (grp) <= 2", {2: 2, 3: 3, 4: 3},
                 [("a", _x(1, 2, c=0.5), "<=", 2), ("b", _x(3, 4, c=0.5), "<=", 2), ("c", _x(5), "<=", 2)],
                 "ac", [0, 4, 10, 10, 2], "PER grp IF gopen: SUM(x) BY (grp) <= 2", id="per-if-avg"),
    # §4 two reducers of one decision under a guard: an open group holds at most a
    # quarter of the query total (70). The global sum read as the group's own zeroes
    # every open group (40); an always-on guard zeroes everything.
    pytest.param("PER grp IF gopen: SUM(x) BY (grp) <= 0.25 * SUM(x) BY ()", {2: 2, 3: 2, 4: 2},
                 [(g, {f"x_{i}": (i in ids) - 0.25 for i in _ROWS}, "<=", 0) for g, ids in _GROUPS.items()],
                 "ac", [0, 10, 10, 10, 10], "PER grp IF gopen: SUM(x) BY (grp) <= 0.25 * SUM(x) BY (grp)",
                 id="per-if-group-against-global"),
    # §3.3 `=` under PER IF pins an open group from both sides; w5 = -1 makes c's floor
    # cost 4, still the cheapest (44). `=` read as `<=` lets x5 sit at 0 (48).
    pytest.param("PER grp IF gopen: SUM(x) BY (grp) = 4", {2: 2, 4: 3, 5: -1},
                 [("a", _x(1, 2), "=", 4), ("b", _x(3, 4), "=", 4), ("c", _x(5), "=", 4)],
                 "ac", [0, 4, 10, 10, 4], "PER grp IF gopen: SUM(x) BY (grp) <= 4", id="per-if-equality"),
    # The full prefix over AVG: WHEN leaves b's row 4 alone, so b's average is x4; a
    # (loses 22) and b (24) open, c (32) stays shut, and a's divisor matters (64).
    # Without the WHEN b averages x3 too (60); AVG read as SUM caps a at 2 (60).
    pytest.param("WHEN cap > 2 PER grp IF gopen: AVG(x) BY (grp) <= 2", {2: 2, 4: 3, 5: 4},
                 [("a", _x(1, 2, c=0.5), "<=", 2), ("b", _x(4), "<=", 2), ("c", _x(5), "<=", 2)],
                 "ab", [0, 4, 10, 2, 10], "PER grp IF gopen: AVG(x) BY (grp) <= 2", id="full-prefix-avg"),
    # The full prefix over `=`: b and c pin one row each, and w5 = -1 makes c's floor
    # cost 3 (30). Without the WHEN b pins x3 + x4 (20); `=` read as `<=` lets x5 sit at
    # 0 (33); an always-on guard gives 13.
    pytest.param("WHEN cap > 2 PER grp IF gopen: SUM(x) BY (grp) = 3", {5: -1},
                 [("a", _x(1, 2), "=", 3), ("b", _x(4), "=", 3), ("c", _x(5), "=", 3)],
                 "bc", [10, 10, 10, 3, 3], "PER grp IF gopen: SUM(x) BY (grp) = 3", id="full-prefix-equality"),
    # The full prefix over a reducer's own WHEN: the clause drops row 3 and the reducer
    # row 2, so an open a caps x1 and an open b caps x4 alone; a (loses 9) and b (18)
    # open (53). Without the local WHEN a caps x1 + x2 (43); without the clause's WHEN
    # b caps x3 + x4, so c opens instead (44).
    pytest.param("WHEN cap > 2 PER grp IF gopen: SUM(WHEN cap < 7: x) BY (grp) <= 1", {4: 2, 5: 3},
                 [("a", _x(1), "<=", 1), ("b", _x(4), "<=", 1), ("c", _x(5), "<=", 1)],
                 "ab", [1, 10, 10, 1, 10], "WHEN cap > 2 PER grp IF gopen: SUM(x) BY (grp) <= 1",
                 id="full-prefix-local-when"),
]


@pytest.mark.when_constraint
@pytest.mark.per_clause
@pytest.mark.var_boolean
@pytest.mark.correctness
@pytest.mark.parametrize("clause, w, guarded, opened, expected, mutant", _G_CASES)
def test_guarded_group_instances_over_each_body_kind(decidb_cli, oracle_solver, clause, w, guarded,
                                                     opened, expected, mutant):
    """§3.2 / deck p58: `PER grp IF gopen` (and the full prefix) over AVG, `=`, two
    reducers and a reducer's own WHEN. Two switches must be on; the weights make the
    choice unique. The mutant must change the optimum."""
    rel, w = _tw(w)
    sql = lambda c: (f"SELECT id, gopen, x FROM {rel} {_GOPEN} SUCH THAT {c} "  # noqa: E731
                     f"AND SUM(PER grp: gopen) >= 2 MAXIMIZE SUM(x * w)")
    got = _rows(decidb_cli, sql(clause), "id", "gopen", "x")

    _model(oracle_solver, "guarded_groups", gopen=True)
    for g, coeffs, sense, rhs in guarded:
        _ind(oracle_solver, f"gopen_{g}", coeffs, sense, rhs)
    _row(oracle_solver, {f"gopen_{g}": 1.0 for g in _GROUPS}, ">=", 2)
    best = _solve(oracle_solver, {f"x_{i}": float(w[i]) for i in _ROWS}).objective_value

    assert [x for *_, x in got] == expected
    assert _groups_open(got) == {g: g in opened for g in _GROUPS}
    assert _value(got, w) == pytest.approx(best)
    assert _value(_rows(decidb_cli, sql(mutant), "id", "gopen", "x"), w) != pytest.approx(best)


# ---------------------------------------------------------------------------
# Guarded row instances (row switch open)
# ---------------------------------------------------------------------------

_PREV_CAP = {1: 9, 2: 3, 3: 7, 4: 2, 5: 5}  # AT(PREVIOUS ELSE 9: cap) OVER (id)

_O_CASES = [
    # §3.2 over `=`: an open row is pinned to its cap from both sides. Row 2 (w -1)
    # would sit at 10, so opening it costs 3 and row 3 costs 2: rows 2, 3 open (-5).
    # Always-on gives 10; `=` read as `>=` lets row 2 stay at 10 (-8), read as `<=`
    # lets every open row sit at 0 (-10).
    pytest.param("IF open: x = cap", 2, MIN, {1: 2, 2: -1},
                 [(i, _x(i), "=", c) for i, (_, c, _) in _ROWS.items()],
                 [2, 3], [0, 7, 2, 0, 0], "x = cap", id="if-equality"),
    # §4 matrix row 5 under IF: an open row holds x + its group's sum <= 12; rows 1-4
    # open (26). Always-on caps row 5 too (22).
    pytest.param("IF open: x + SUM(x) BY (grp) <= 12", 4, MAX, {},
                 [(i, {**_x(*_GROUPS[g]), f"x_{i}": 2.0}, "<=", 12) for i, (g, _, _) in _ROWS.items()],
                 [1, 2, 3, 4], [4, 4, 4, 4, 10], "x + SUM(x) BY (grp) <= 12", id="if-reducer-beside-row-term"),
    # §5 data frame under IF: an open row is capped by the previous row's cap (ELSE 9 on
    # row 1); rows 1, 3, 5 lose least (41). Reading the row's own cap gives 36.
    pytest.param("IF open: x <= AT(PREVIOUS ELSE 9: cap) OVER (id)", 3, MAX, {},
                 [(i, _x(i), "<=", _PREV_CAP[i]) for i in _ROWS],
                 [1, 3, 5], [9, 10, 7, 10, 5], "IF open: x <= cap", id="if-data-frame"),
    # §5 decision frame under IF: an open row sits one below the previous one; row 1
    # cannot open (ELSE 0 - 1 < 0), so rows 2-5 step down from x1 = 10 (40). An
    # always-on guard is infeasible, one never imposed gives 50; without the ELSE row
    # 1's instance is skipped, so it opens for free (46); NEXT gives 6..10 (vector).
    pytest.param("IF open: x <= AT(PREVIOUS ELSE 0: x) OVER (id) - 1", 4, MAX, {},
                 [(1, _x(1), "<=", -1)] + [(i, {f"x_{i}": 1.0, f"x_{i - 1}": -1.0}, "<=", -1)
                                           for i in (2, 3, 4, 5)],
                 [2, 3, 4, 5], [10, 9, 8, 7, 6], "IF open: x <= AT(PREVIOUS: x) OVER (id) - 1",
                 id="if-decision-frame"),
    # WHEN IF over a group reducer: rows 1, 3 have no instance and open for free, then
    # two of rows 2, 4, 5 cap their filtered group, which is the row alone (65). The
    # unfiltered groups tie row 2 to row 1 and row 4 to row 3 (45).
    pytest.param("WHEN NOT pri IF open: SUM(x) BY (grp) <= 5", 4, MAX, {4: 2, 5: 3},
                 [(i, _x(i), "<=", 5) for i in (2, 4, 5)],
                 [1, 2, 3, 4], [10, 5, 10, 5, 10], "IF open: SUM(x) BY (grp) <= 5", id="when-if-group-reducer"),
    # §4 empty reducer under WHEN IF (delta 2): row 2's own reducer admits no row (cap 7),
    # so its instance is not imposed and row 2 opens for free; row 4 is the cheap fourth
    # (51). Without the local WHEN row 2 is capped too (42); an error refuses the query.
    pytest.param("WHEN NOT pri IF open: SUM(WHEN cap <= 5: x) BY (grp) <= 1", 4, MAX, {5: 2},
                 [(i, _x(i), "<=", 1) for i in (4, 5)],
                 [1, 2, 3, 4], [10, 10, 10, 1, 10], "WHEN NOT pri IF open: SUM(x) BY (grp) <= 1",
                 id="when-if-empty-local-reducer"),
]


@pytest.mark.when_perrow
@pytest.mark.var_boolean
@pytest.mark.correctness
@pytest.mark.parametrize("clause, count, sense, w, guarded, opened, expected, mutant", _O_CASES)
def test_guarded_row_instances_over_each_body_kind(decidb_cli, oracle_solver, clause, count, sense, w,
                                                   guarded, opened, expected, mutant):
    """§3.2: a per-row `IF open` over `=`, a reducer beside the row term, frames, and a
    group reducer under WHEN. `count` switches must be on; the mutant must change the
    optimum."""
    rel, w = _tw(w)
    word = "MAXIMIZE" if sense is MAX else "MINIMIZE"
    sql = lambda c: (f"SELECT id, open, x FROM {rel} {_OPEN} SUCH THAT {c} "  # noqa: E731
                     f"AND SUM(open) >= {count} {word} SUM(x * w)")
    got = _rows(decidb_cli, sql(clause), "id", "open", "x")

    _model(oracle_solver, "guarded_rows", opens=True)
    for i, coeffs, s, rhs in guarded:
        _ind(oracle_solver, f"open_{i}", coeffs, s, rhs)
    _row(oracle_solver, {f"open_{i}": 1.0 for i in _ROWS}, ">=", count)
    best = _solve(oracle_solver, {f"x_{i}": float(w[i]) for i in _ROWS}, sense).objective_value

    assert [x for *_, x in got] == expected
    assert [i for i, o, _ in got if o] == opened
    assert _value(got, w) == pytest.approx(best)
    if mutant is not None:
        assert _value(_rows(decidb_cli, sql(mutant), "id", "open", "x"), w) != pytest.approx(best)


# ---------------------------------------------------------------------------
# Keyed and query-wide decisions under WHEN and IF
# ---------------------------------------------------------------------------

_K_CASES = [
    # §3.1 WHEN over a keyed decision: per-row instances on rows 1, 3 read their group's
    # y, so y_c is free (20). Without the WHEN y_c <= 4 (14).
    pytest.param("WHEN pri: y <= cap",
                 lambda o: (_row(o, {"y_a": 1.0}, "<=", 3), _row(o, {"y_b": 1.0}, "<=", 2)),
                 {"a": 3, "b": 2, "c": 10}, None, "y <= cap", id="keyed-when"),
    # §3.2 keyed body, keyed guard, per-row instances: an open group's y is capped by
    # each of its rows (the smaller cap); a and c open (30). Always-on 14; one instance
    # per group reading its larger cap would give y_a = 7.
    pytest.param("IF gopen: y <= cap AND SUM(PER grp: gopen) >= 2",
                 lambda o: ([_ind(o, f"gopen_{g}", {f"y_{g}": 1.0}, "<=", cap) for g, cap, _ in _ROWS.values()],
                            _row(o, {f"gopen_{g}": 1.0 for g in _GROUPS}, ">=", 2)),
                 {"a": 3, "b": 10, "c": 4}, "ac", "y <= cap AND SUM(PER grp: gopen) >= 2", id="keyed-if"),
    # WHEN IF: only rows 2, 4, 5 have instances, so an open a is capped at 7 (38).
    # Without the WHEN row 1's cap of 3 applies (30).
    pytest.param("WHEN NOT pri IF gopen: y <= cap AND SUM(PER grp: gopen) >= 2",
                 lambda o: ([_ind(o, f"gopen_{g}", {f"y_{g}": 1.0}, "<=", cap)
                             for g, cap, pri in _ROWS.values() if not pri],
                            _row(o, {f"gopen_{g}": 1.0 for g in _GROUPS}, ">=", 2)),
                 {"a": 7, "b": 10, "c": 4}, "ac", "IF gopen: y <= cap AND SUM(PER grp: gopen) >= 2",
                 id="keyed-when-if"),
]


@pytest.mark.when_perrow
@pytest.mark.per_clause
@pytest.mark.correctness
@pytest.mark.parametrize("clause, build, expected, opened, mutant", _K_CASES)
def test_keyed_decision_under_when_and_if(decidb_cli, oracle_solver, clause, build, expected, opened, mutant):
    """§3.1 / §3.2: per-row instances reading a `PER grp` decision (and a `PER grp`
    guard) under WHEN, IF and WHEN IF. `SUM(y)` counts y once per row."""
    sql = lambda c: (f"SELECT id, gopen, y FROM {_T} DECIDE PER grp: y(INT) BETWEEN 0 AND 10, "  # noqa: E731
                     f"PER grp: gopen(BOOL) SUCH THAT {c} MAXIMIZE SUM(y)")
    got = _rows(decidb_cli, sql(clause), "id", "gopen", "y")

    _model(oracle_solver, "keyed_when_if", x=False, gopen=True)
    for g in _GROUPS:
        oracle_solver.add_variable(f"y_{g}", VarType.INTEGER, lb=0.0, ub=10.0)
    build(oracle_solver)
    best = _solve(oracle_solver, {f"y_{g}": float(len(ids)) for g, ids in _GROUPS.items()}).objective_value

    assert [y for *_, y in got] == [expected[_ROWS[i][0]] for i in _ROWS]
    if opened is not None:
        assert _groups_open(got) == {g: g in opened for g in _GROUPS}
    assert sum(y for *_, y in got) == pytest.approx(best)
    assert sum(y for *_, y in _rows(decidb_cli, sql(mutant), "id", "gopen", "y")) != pytest.approx(best)


_Q_CASES = [
    # WHEN over a query-wide body: c is capped by the admitted rows only (4); unfiltered 2.
    pytest.param("WHEN NOT pri: c <= cap",
                 lambda o: [_row(o, {"c": 1.0}, "<=", cap) for _, cap, pri in _ROWS.values() if not pri],
                 4, "c <= cap", id="query-wide-when"),
    # A row guard over a query-wide body: each open row caps the shared c, so the two
    # largest caps open (5). Always-on gives 2.
    pytest.param("IF open: c <= cap AND SUM(open) >= 2",
                 lambda o: ([_ind(o, f"open_{i}", {"c": 1.0}, "<=", cap) for i, (_, cap, _) in _ROWS.items()],
                            _row(o, {f"open_{i}": 1.0 for i in _ROWS}, ">=", 2)),
                 5, "c <= cap AND SUM(open) >= 2", id="query-wide-body-row-guard"),
    # WHEN PER: the priority rows give group sums 3 (a) and 2 (b), c has no instance:
    # 2c <= 2 (1). Unfiltered sums 10, 7, 4 give 2.
    pytest.param("WHEN pri PER grp: 2 * c <= SUM(cap) BY (grp)",
                 lambda o: (_row(o, {"c": 2.0}, "<=", 3), _row(o, {"c": 2.0}, "<=", 2)),
                 1, "PER grp: 2 * c <= SUM(cap) BY (grp)", id="query-wide-when-per"),
    # The full prefix: filtered sums 7, 5, 4 and a, b open (5). Without the WHEN the
    # sums are 10, 7, 4 (7); an always-on guard gives 4.
    pytest.param("WHEN NOT pri PER grp IF gopen: c <= SUM(cap) BY (grp) AND SUM(PER grp: gopen) >= 2",
                 lambda o: ([_ind(o, f"gopen_{g}", {"c": 1.0}, "<=", s) for g, s in (("a", 7), ("b", 5), ("c", 4))],
                            _row(o, {f"gopen_{g}": 1.0 for g in _GROUPS}, ">=", 2)),
                 5, "PER grp IF gopen: c <= SUM(cap) BY (grp) AND SUM(PER grp: gopen) >= 2",
                 id="query-wide-full-prefix"),
]


@pytest.mark.when_constraint
@pytest.mark.per_clause
@pytest.mark.correctness
@pytest.mark.parametrize("clause, build, expected, mutant", _Q_CASES)
def test_query_wide_decision_under_each_prefix(decidb_cli, oracle_solver, clause, build, expected, mutant):
    """§2.1 / §3: a `PER ()` decision read by per-row, keyed and guarded instances;
    `MAXIMIZE c` (§6, arithmetic over query-wide decisions)."""
    decl = "DECIDE PER (): c(INT) BETWEEN 0 AND 10, PER grp: gopen(BOOL), open(BOOL)"
    sql = lambda c: f"SELECT id, c FROM {_T} {decl} SUCH THAT {c} MAXIMIZE c"  # noqa: E731
    got = _rows(decidb_cli, sql(clause), "id", "c")

    _model(oracle_solver, "query_wide", x=False, gopen=True, opens=True)
    oracle_solver.add_variable("c", VarType.INTEGER, lb=0.0, ub=10.0)
    build(oracle_solver)
    best = _solve(oracle_solver, {"c": 1.0}).objective_value

    assert {c for _, c in got} == {expected}
    assert expected == pytest.approx(best)
    assert {c for _, c in _rows(decidb_cli, sql(mutant), "id", "c")} != {expected}


@pytest.mark.var_boolean
@pytest.mark.correctness
def test_query_wide_switch_guards_every_row_instance_at_once(decidb_cli, oracle_solver):
    """§3.2 with a `PER ()` switch: `IF zb: x <= cap AND IF NOT zb: x <= 4` is one pair
    of implications per row on the same switch. zb = 1 is worth 21 (the caps) against
    20 (five 4s), so every row sits at its cap. An always-on guard gives 17; one never
    imposed 50; a switch read per row takes max(cap, 4) on each row (24)."""
    got = _rows(decidb_cli, f"""
        SELECT id, zb, x FROM {_T} DECIDE x(INT) BETWEEN 0 AND 10, PER (): zb(BOOL)
        SUCH THAT IF zb: x <= cap AND IF NOT zb: x <= 4
        MAXIMIZE SUM(x)
    """, "id", "zb", "x")

    _model(oracle_solver, "query_wide_switch")
    oracle_solver.add_variable("zb", VarType.BINARY)
    for i, (_, cap, _) in _ROWS.items():
        _ind(oracle_solver, "zb", _x(i), "<=", cap)
        _ind(oracle_solver, "zb", _x(i), "<=", 4, on=0)
    best = _solve(oracle_solver, _x(*_ROWS)).objective_value

    assert got == [(i, True, cap) for i, (_, cap, _) in _ROWS.items()]
    assert sum(x for *_, x in got) == pytest.approx(best) == 21


@pytest.mark.var_boolean
@pytest.mark.when_constraint
@pytest.mark.correctness
def test_query_wide_switch_over_a_filtered_reducer(decidb_cli, oracle_solver):
    """§3.2 over §4's reducer-local WHEN: `PER () IF zb: SUM(WHEN pri: x) <= 3 AND
    PER () IF NOT zb: SUM(x) <= 25`. With row 3 weighing 2, zb = 1 caps only the
    priority rows (x3 = 3, x1 = 0: 36) and beats zb = 0's budget (35). An always-on
    guard gives 28, one never imposed 60, and a dropped local WHEN turns zb = 1 into
    SUM(x) <= 3 (so zb = 0: 35)."""
    rel, w = _tw({3: 2})
    got = _rows(decidb_cli, f"""
        SELECT id, zb, x FROM {rel} DECIDE x(INT) BETWEEN 0 AND 10, PER (): zb(BOOL)
        SUCH THAT PER () IF zb: SUM(WHEN pri: x) <= 3 AND PER () IF NOT zb: SUM(x) <= 25
        MAXIMIZE SUM(x * w)
    """, "id", "zb", "x")

    _model(oracle_solver, "query_wide_filtered")
    oracle_solver.add_variable("zb", VarType.BINARY)
    _ind(oracle_solver, "zb", _x(1, 3), "<=", 3)
    _ind(oracle_solver, "zb", _x(*_ROWS), "<=", 25, on=0)
    best = _solve(oracle_solver, {f"x_{i}": float(w[i]) for i in _ROWS}).objective_value

    assert got == [(1, True, 0), (2, True, 10), (3, True, 3), (4, True, 10), (5, True, 10)]
    assert _value(got, w) == pytest.approx(best) == 36


# ---------------------------------------------------------------------------
# Several scopes in one body
# ---------------------------------------------------------------------------

@pytest.mark.when_perrow
@pytest.mark.correctness
def test_row_and_keyed_decisions_in_one_filtered_row_body(decidb_cli, oracle_solver):
    """§3.1 under WHEN: `WHEN pri: x + y <= cap` reads each priority row's own x and its
    group's y. y is worth 2 per group, so y_a = 3 and y_b = 2 take the caps from x1 and
    x3, and y_c is untouched: 30 + 2 * 15 = 60. Dropping the WHEN caps every row (25);
    reading y per row would let rows 1 and 2 disagree on y_a."""
    got = _rows(decidb_cli, f"""
        SELECT id, x, y FROM {_T} DECIDE x(INT) BETWEEN 0 AND 10, PER grp: y(INT) BETWEEN 0 AND 10
        SUCH THAT WHEN pri: x + y <= cap
        MAXIMIZE SUM(x) + SUM(PER grp: 2 * y)
    """, "id", "x", "y")

    _model(oracle_solver, "row_and_keyed")
    for g in _GROUPS:
        oracle_solver.add_variable(f"y_{g}", VarType.INTEGER, lb=0.0, ub=10.0)
    _row(oracle_solver, {"x_1": 1.0, "y_a": 1.0}, "<=", 3)
    _row(oracle_solver, {"x_3": 1.0, "y_b": 1.0}, "<=", 2)
    best = _solve(oracle_solver, {**_x(*_ROWS), **{f"y_{g}": 2.0 for g in _GROUPS}}).objective_value

    assert got == [(1, 0, 3), (2, 10, 3), (3, 0, 2), (4, 10, 2), (5, 10, 10)]
    assert 30 + 2 * (3 + 2 + 10) == pytest.approx(best)


@pytest.mark.per_clause
@pytest.mark.correctness
def test_three_scopes_in_one_keyed_body(decidb_cli, oracle_solver):
    """§3.1: under `PER grp` a row decision is reduced, a `PER grp` decision is one value
    per instance and a `PER ()` decision one value for all: `PER grp: SUM(x) BY (grp) +
    y + c <= 12 AND PER (): c >= 3`. c costs a unit in each instance and earns nothing,
    so c = 3; y (worth 2) takes the rest: y = 9, x = 0 -> 54. Counting c once per row of
    the group gives 42, dropping it 66."""
    got = _rows(decidb_cli, f"""
        SELECT id, x, y, c FROM {_T}
        DECIDE x(INT) BETWEEN 0 AND 10, PER grp: y(INT) BETWEEN 0 AND 10, PER (): c(INT) BETWEEN 0 AND 10
        SUCH THAT PER grp: SUM(x) BY (grp) + y + c <= 12 AND PER (): c >= 3
        MAXIMIZE SUM(x) + SUM(PER grp: 2 * y)
    """, "id", "x", "y", "c")

    _model(oracle_solver, "three_scopes")
    oracle_solver.add_variable("c", VarType.INTEGER, lb=3.0, ub=10.0)
    for g, ids in _GROUPS.items():
        oracle_solver.add_variable(f"y_{g}", VarType.INTEGER, lb=0.0, ub=10.0)
        _row(oracle_solver, {**_x(*ids), f"y_{g}": 1.0, "c": 1.0}, "<=", 12)
    best = _solve(oracle_solver, {**_x(*_ROWS), **{f"y_{g}": 2.0 for g in _GROUPS}}).objective_value

    assert got == [(i, 0, 9, 3) for i in _ROWS]
    assert 2 * 9 * 3 == pytest.approx(best)


@pytest.mark.when_constraint
@pytest.mark.per_clause
@pytest.mark.var_boolean
@pytest.mark.correctness
def test_mixed_scope_body_under_the_full_prefix(decidb_cli, oracle_solver):
    """§3 (filter -> generate -> guard) over a body reading a reduced row decision and a
    keyed one: `WHEN NOT pri PER grp, cap IF gopen: SUM(x) BY (grp) + y <= cap` is one
    instance per group on rows 2, 4, 5 (x2 + y_a <= 7, x4 + y_b <= 5, x5 + y_c <= 4).
    y is worth 2; two groups open and a (loses 16) and b (20) beat c (22): 74. An
    always-on guard gives 52, one never imposed 110; without the filter the priority
    rows' caps (3, 2) are the tightest."""
    got = _rows(decidb_cli, f"""
        SELECT id, gopen, x, y FROM {_T}
        DECIDE x(INT) BETWEEN 0 AND 10, PER grp: y(INT) BETWEEN 0 AND 10, PER grp: gopen(BOOL)
        SUCH THAT WHEN NOT pri PER grp, cap IF gopen: SUM(x) BY (grp) + y <= cap
              AND SUM(PER grp: gopen) >= 2
        MAXIMIZE SUM(x) + SUM(PER grp: 2 * y)
    """, "id", "gopen", "x", "y")

    _model(oracle_solver, "mixed_full_prefix", gopen=True)
    for g in _GROUPS:
        oracle_solver.add_variable(f"y_{g}", VarType.INTEGER, lb=0.0, ub=10.0)
    for i, (g, cap, pri) in _ROWS.items():
        if not pri:
            _ind(oracle_solver, f"gopen_{g}", {f"x_{i}": 1.0, f"y_{g}": 1.0}, "<=", cap)
    _row(oracle_solver, {f"gopen_{g}": 1.0 for g in _GROUPS}, ">=", 2)
    best = _solve(oracle_solver, {**_x(*_ROWS), **{f"y_{g}": 2.0 for g in _GROUPS}}).objective_value

    assert got == [(1, True, 10, 7), (2, True, 0, 7), (3, True, 10, 5), (4, True, 0, 5), (5, False, 10, 10)]
    assert 30 + 2 * (7 + 5 + 10) == pytest.approx(best) == 74


@pytest.mark.cons_in
@pytest.mark.per_clause
@pytest.mark.correctness
def test_in_lists_on_keyed_and_query_wide_decisions(decidb_cli, oracle_solver):
    """§3.3: IN on a decision is one-hot, and on a keyed or query-wide decision the
    indicators belong to the decision: `PER grp: y IN (1, 4, 8) AND y <= cap + 1` gives
    y_a = 4, y_b = 1, y_c = 4, and `PER (): c IN (2, 6, 9) AND PER (): c <= 8` gives
    c = 6: 2*4 + 2*1 + 4 + 6 = 20. Ignoring the lists gives 27."""
    got = _rows(decidb_cli, f"""
        SELECT id, y, c FROM {_T}
        DECIDE PER grp: y(INT) BETWEEN 0 AND 10, PER (): c(INT) BETWEEN 0 AND 10
        SUCH THAT PER grp: y IN (1, 4, 8) AND y <= cap + 1 AND PER (): c IN (2, 6, 9) AND PER (): c <= 8
        MAXIMIZE SUM(y) + c
    """, "id", "y", "c")

    _model(oracle_solver, "in_lists", x=False)
    oracle_solver.add_variable("c", VarType.INTEGER, lb=0.0, ub=8.0)
    _one_of(oracle_solver, "c", (2, 6, 9))
    for g in _GROUPS:
        oracle_solver.add_variable(f"y_{g}", VarType.INTEGER, lb=0.0, ub=10.0)
        _one_of(oracle_solver, f"y_{g}", (1, 4, 8))
    for g, cap, _ in _ROWS.values():
        _row(oracle_solver, {f"y_{g}": 1.0}, "<=", cap + 1)
    best = _solve(oracle_solver, {"c": 1.0, **{f"y_{g}": float(len(ids)) for g, ids in _GROUPS.items()}})

    assert got == [(1, 4, 6), (2, 4, 6), (3, 1, 6), (4, 1, 6), (5, 4, 6)]
    assert sum(y for _, y, _ in got) + 6 == pytest.approx(best.objective_value) == 20


@pytest.mark.correctness
def test_query_wide_step_beside_a_decision_frame(decidb_cli, oracle_solver):
    """§5 with §2.1: `x <= AT(PREVIOUS ELSE 0: x) OVER (id) + c` lets each row rise at
    most c over the previous one, c being one value for every row (x_k <= k * c). c
    costs 5 and is at most 3: c = 3 gives 3, 6, 9, 10, 10 (38 - 15 = 23) against 20
    for c = 2. A frame read as the sum over every row leaves x free (50)."""
    got = _rows(decidb_cli, f"""
        SELECT id, x, c FROM {_T} DECIDE x(INT) BETWEEN 0 AND 10, PER (): c(INT) BETWEEN 0 AND 10
        SUCH THAT x <= AT(PREVIOUS ELSE 0: x) OVER (id) + c AND PER (): c <= 3
        MAXIMIZE SUM(x) - 5 * c
    """, "id", "x", "c")

    _model(oracle_solver, "frame_step")
    oracle_solver.add_variable("c", VarType.INTEGER, lb=0.0, ub=3.0)
    _row(oracle_solver, {"x_1": 1.0, "c": -1.0}, "<=", 0)
    for i in (2, 3, 4, 5):
        _row(oracle_solver, {f"x_{i}": 1.0, f"x_{i - 1}": -1.0, "c": -1.0}, "<=", 0)
    best = _solve(oracle_solver, {**_x(*_ROWS), "c": -5.0}).objective_value

    assert got == [(1, 3, 3), (2, 6, 3), (3, 9, 3), (4, 10, 3), (5, 10, 3)]
    assert 38 - 15 == pytest.approx(best)


# ---------------------------------------------------------------------------
# Body kinds under the objective forms
# ---------------------------------------------------------------------------

@pytest.mark.obj_maximize
@pytest.mark.per_clause
@pytest.mark.correctness
def test_then_settles_the_ties_a_not_equal_body_leaves(decidb_cli, oracle_solver):
    """§6 THEN over §3.3's `<>`: `PER grp: SUM(x) BY (grp) <> 20` forbids a full group,
    so the first stage reaches 19 + 19 + 10 = 48 two ways per two-row group. `THEN
    MINIMIZE SUM(x * id)` keeps the lower id full: x = 10, 9, 10, 9, 10 (144). Dropping
    `<>` gives 50; a second stage solved without the first's optimum gives all zeros;
    ignoring THEN leaves the tie to the solver."""
    got = _rows(decidb_cli, f"""
        SELECT id, x FROM {_T} {_DECL}
        SUCH THAT PER grp: SUM(x) BY (grp) <> 20
        MAXIMIZE SUM(x) THEN MINIMIZE SUM(x * id)
    """, "id", "x")

    def stage(name):
        _model(oracle_solver, name)
        for g, ids in _GROUPS.items():
            _ne(oracle_solver, _x(*ids), 20, g)
    stage("then_ne_first")
    first = _solve(oracle_solver, _x(*_ROWS)).objective_value
    stage("then_ne_second")
    _row(oracle_solver, _x(*_ROWS), "=", first)
    second = _solve(oracle_solver, {f"x_{i}": float(i) for i in _ROWS}, MIN).objective_value

    assert got == [(1, 10), (2, 9), (3, 10), (4, 9), (5, 10)]
    assert sum(x for _, x in got) == pytest.approx(first) == 48
    assert sum(i * x for i, x in got) == pytest.approx(second) == 144


@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_then_after_a_query_wide_first_stage(decidb_cli, oracle_solver):
    """§6: arithmetic over a `PER ()` decision is a stage, so `MAXIMIZE c THEN MAXIMIZE
    SUM(x * id)` first raises the shared cap c to 10 (x <= c, SUM(x) + 2c <= 30) and then
    spends the remaining 10 units on row 5 (50). Solving the stages in the other order
    gives c = 6 (72); ignoring THEN leaves any x with SUM(x) <= 10."""
    got = _rows(decidb_cli, f"""
        SELECT id, x, c FROM {_T} DECIDE x(INT) BETWEEN 0 AND 10, PER (): c(INT) BETWEEN 0 AND 10
        SUCH THAT x <= c AND PER (): SUM(x) + 2 * c <= 30
        MAXIMIZE c THEN MAXIMIZE SUM(x * id)
    """, "id", "x", "c")

    def stage(name):
        _model(oracle_solver, name)
        oracle_solver.add_variable("c", VarType.INTEGER, lb=0.0, ub=10.0)
        for i in _ROWS:
            _row(oracle_solver, {f"x_{i}": 1.0, "c": -1.0}, "<=", 0)
        _row(oracle_solver, {**_x(*_ROWS), "c": 2.0}, "<=", 30)
    stage("then_query_wide_first")
    first = _solve(oracle_solver, {"c": 1.0}).objective_value
    stage("then_query_wide_second")
    _row(oracle_solver, {"c": 1.0}, "=", first)
    second = _solve(oracle_solver, {f"x_{i}": float(i) for i in _ROWS}).objective_value

    assert got == [(1, 0, 10), (2, 0, 10), (3, 0, 10), (4, 0, 10), (5, 10, 10)]
    assert first == pytest.approx(10) and sum(i * x for i, x, _ in got) == pytest.approx(second) == 50


@pytest.mark.obj_minimize
@pytest.mark.var_boolean
@pytest.mark.correctness
def test_nested_max_objective_spreads_over_the_groups_its_guards_open(decidb_cli, oracle_solver):
    """§6 nested `MINIMIZE MAX(PER grp: SUM(x) BY (grp))` over §3.2 guards: `PER grp IF
    NOT gopen: SUM(x) BY (grp) <= 0` empties a shut group, at most two groups open, and
    the rows must supply SUM(x * cap) >= 70. Opening a and b with 6 units each on their
    largest-cap rows (7 * 6 + 5 * 6 = 72) is the unique optimum; a + c needs 7, b + c 8.
    MINIMIZE SUM(x) puts all 10 units on row 2 and a flat MAX(x) caps every row at 5
    (both leave a group total of 10); a guard never imposed, on the wrong polarity or
    without the count spreads over all three groups (5); always-on is infeasible."""
    got = _rows(decidb_cli, f"""
        SELECT id, gopen, x FROM {_T} {_GOPEN}
        SUCH THAT PER grp IF NOT gopen: SUM(x) BY (grp) <= 0 AND SUM(PER grp: gopen) <= 2
              AND PER (): SUM(x * cap) >= 70
        MINIMIZE MAX(PER grp: SUM(x) BY (grp))
    """, "id", "gopen", "x")

    _model(oracle_solver, "nested_max_guard", gopen=True)
    oracle_solver.add_variable("top", VarType.CONTINUOUS, lb=0.0, ub=100.0)
    for g, ids in _GROUPS.items():
        _ind(oracle_solver, f"gopen_{g}", _x(*ids), "<=", 0, on=0)
        _row(oracle_solver, {"top": 1.0, **_x(*ids, c=-1.0)}, ">=", 0)
    _row(oracle_solver, {f"gopen_{g}": 1.0 for g in _GROUPS}, "<=", 2)
    _row(oracle_solver, {f"x_{i}": float(cap) for i, (_, cap, _) in _ROWS.items()}, ">=", 70)
    best = _solve(oracle_solver, {"top": 1.0}, MIN).objective_value

    x = {i: v for i, _, v in got}
    assert got == [(1, True, 0), (2, True, 6), (3, True, 0), (4, True, 6), (5, False, 0)]
    assert max(sum(x[i] for i in ids) for ids in _GROUPS.values()) == pytest.approx(best) == 6


@pytest.mark.obj_maximize
@pytest.mark.when_objective
@pytest.mark.var_boolean
@pytest.mark.correctness
def test_filtered_objective_decides_which_guards_open(decidb_cli, oracle_solver):
    """§6 objective WHEN over §3.2 guards: `MAXIMIZE SUM(WHEN NOT pri: x * w)` values
    rows 2, 4, 5 only (weights 1, 2, 3), so an open group (sum <= 3) costs its
    non-priority row: a 7, b 14, c 21 -> a and b open, x = 0, 3, 0, 3, 10 (39). An
    objective reading every row opens a and c instead."""
    rel, w = _tw({4: 2, 5: 3})
    got = _rows(decidb_cli, f"""
        SELECT id, gopen, x FROM {rel} {_GOPEN}
        SUCH THAT PER grp IF gopen: SUM(x) BY (grp) <= 3 AND SUM(PER grp: gopen) >= 2
        MAXIMIZE SUM(WHEN NOT pri: x * w)
    """, "id", "gopen", "x")

    _model(oracle_solver, "objective_when_guard", gopen=True)
    for g, ids in _GROUPS.items():
        _ind(oracle_solver, f"gopen_{g}", _x(*ids), "<=", 3)
    _row(oracle_solver, {f"gopen_{g}": 1.0 for g in _GROUPS}, ">=", 2)
    best = _solve(oracle_solver, {f"x_{i}": float(w[i]) for i, (_, _, pri) in _ROWS.items() if not pri})

    assert got == [(1, True, 0), (2, True, 3), (3, True, 0), (4, True, 3), (5, False, 10)]
    assert 3 * 1 + 3 * 2 + 10 * 3 == pytest.approx(best.objective_value)


@pytest.mark.correctness
def test_satisfy_over_a_frame_chain_with_one_solution(decidb_cli, oracle_solver):
    """§6 SATISFY with a §5 decision frame: `x >= AT(PREVIOUS ELSE 0: x) OVER (id) + 2`
    over x in [0, 10] admits exactly 2, 4, 6, 8, 10 (the ELSE 0 floor starts the
    chain), so the feasible assignment SATISFY returns is that one; the oracle's
    smallest and largest sums coincide at 30. Reading the next row (or DESC) gives the
    reversed chain 10, 8, ..., 2 and CYCLIC is infeasible. A dropped ELSE only relaxes
    row 1 (the chain may then start at 0 or 1), which no SATISFY answer can pin."""
    got = _rows(decidb_cli, f"""
        SELECT id, x FROM {_T} {_DECL}
        SUCH THAT x >= AT(PREVIOUS ELSE 0: x) OVER (id) + 2 SATISFY
    """, "id", "x")

    ends = []
    for sense in (MIN, MAX):
        _model(oracle_solver, f"satisfy_frame_{sense.name}")
        _row(oracle_solver, _x(1), ">=", 2)
        for i in (2, 3, 4, 5):
            _row(oracle_solver, {f"x_{i}": 1.0, f"x_{i - 1}": -1.0}, ">=", 2)
        ends.append(_solve(oracle_solver, _x(*_ROWS), sense).objective_value)

    assert got == [(1, 2), (2, 4), (3, 6), (4, 8), (5, 10)]
    assert ends[0] == pytest.approx(ends[1]) == sum(x for _, x in got) == 30


@pytest.mark.min_max
@pytest.mark.per_clause
@pytest.mark.correctness
def test_satisfy_with_a_refined_hard_max_body(decidb_cli, oracle_solver):
    """§3.1 refinement in the hard direction: `PER grp, cap: MAX(x) BY (grp) >= cap`
    writes five instances that reduce onto each group's largest cap (a >= 7, b >= 5,
    c >= 4), so beside `SUM(x) <= 16` SATISFY must put 7, 5, 4 on one row per group and
    0 elsewhere. The oracle states the five instances and accepts the returned
    assignment. The tightest cap of every instance on every group (7) is infeasible.
    Reading MAX as SUM, or the smallest cap per group (a >= 3), only relaxes the
    problem, which no SATISFY answer can pin, so a second query caps every row at 6:
    a's floor of 7 then has no row to sit on (infeasible), while both relaxed
    readings still solve it."""
    got = _rows(decidb_cli, f"""
        SELECT id, x FROM {_T} {_DECL}
        SUCH THAT PER grp, cap: MAX(x) BY (grp) >= cap AND PER (): SUM(x) <= 16 SATISFY
    """, "id", "x")

    _model(oracle_solver, "satisfy_refined_max")
    for i, (g, cap, _) in _ROWS.items():
        _reaches(oracle_solver, _GROUPS[g], cap, f"i{i}")
    _row(oracle_solver, _x(*_ROWS), "<=", 16)
    for i, x in got:
        _row(oracle_solver, _x(i), "=", x)
    _solve(oracle_solver, {})

    x = dict(got)
    assert [max(x[i] for i in ids) for ids in _GROUPS.values()] == [7, 5, 4]
    assert sum(x.values()) == 16

    decidb_cli.assert_error(f"""
        SELECT id, x FROM {_T} {_DECL}
        SUCH THAT PER grp, cap: MAX(x) BY (grp) >= cap AND x <= 6 SATISFY
    """, match=r"infeasible")
    _model(oracle_solver, "satisfy_refined_max_capped", ub={i: 6 for i in _ROWS})
    for i, (g, cap, _) in _ROWS.items():
        _reaches(oracle_solver, _GROUPS[g], cap, f"i{i}")
    oracle_solver.set_objective({}, MAX)
    assert oracle_solver.solve().status == SolverStatus.INFEASIBLE


# ---------------------------------------------------------------------------
# Combinations the reference refuses by name
# ---------------------------------------------------------------------------

@pytest.mark.error
@pytest.mark.parametrize("clause, topic", [
    ("WHEN NOT pri PER grp IF gopen: SUM(x) BY (grp) <> 5", r"cannot be guarded"),
    ("PER grp IF r >= 1: SUM(x) BY (grp) <= 4", r"not integer-valued"),
    ("WHEN NOT pri PER grp: SUM(x) BY () <> 7", r"outside its own instance"),
    ("WHEN NOT pri: x <> AT(PREVIOUS ELSE 0: x) OVER (id)", r"outside its own instance"),
], ids=["guarded-not-equal", "real-guard-per-key", "not-equal-global-under-per", "not-equal-frame-under-when"])
def test_refused_combinations_are_named(decidb_cli, clause, topic):
    """§3.2: a `<>` body cannot be guarded whatever prefix precedes the guard, and a
    guard over a REAL decision is refused under a keyed PER too. The not-implemented
    list: `<>` over a reducer keyed differently from PER, or beside a frame, is refused
    under a filter as well. Each is named rather than solved with part dropped."""
    decidb_cli.assert_error(
        f"SELECT id, x FROM {_T} DECIDE x(INT) BETWEEN 0 AND 10, PER grp: gopen(BOOL), "
        f"PER grp: r(REAL) BETWEEN 0 AND 5 SUCH THAT {clause} MAXIMIZE SUM(x)",
        match=topic)
