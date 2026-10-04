"""The frames matrix (DeciQL spec §4.2, §7.3; syntax_reference §5; deck p42-56, p73-75):
selectors x boundary policies x direction x CYCLIC x WITHIN, under every prefix, over
data, decision and data*decision bodies, beside sibling terms, and the refusals by name.

The oracle navigates on its own: `_navigate` builds each timeline from the data (WITHIN
partition, order key, direction, peers, NULL keys), resolves every selector and range
from the instance's position, wraps under CYCLIC and applies the boundary policy, and
the gurobipy model is written from what it reads. The hand tables kept beside some
cases are asserted against `_navigate` as a readability check. Every relation lists its
rows out of key order, so an engine that walked the input order would read the wrong
neighbour.
"""

import re

import pytest

from solver.types import ObjSense, SolverStatus, VarType


def _rel(cols, *data, key=1):
    """A VALUES relation and the same rows for the oracle, {key: {column: value}},
    keyed by the first `key` columns."""
    lit = lambda v: "NULL" if v is None else repr(v)
    sql = "(VALUES " + ", ".join(f"({', '.join(map(lit, row))})" for row in data) + f") r({cols})"
    names = [c.strip() for c in cols.split(",")]
    return sql, {(row[0] if key == 1 else row[:key]): dict(zip(names, row)) for row in data}


# Caps, the ELSE fills and the boxes are pairwise distinct, so a row that reads its own
# cap, the wrong neighbour or the wrong policy lands on a different number.
_R3, _ROWS3 = _rel("t, cap", (3, 8), (1, 6), (2, 2))
_R4, _ROWS4 = _rel("t, cap", (2, 6), (4, 9), (1, 4), (3, 2))
_T5, _ROWS5 = _rel("t", (4,), (1,), (5,), (3,), (2,))
# Two product timelines of unequal length.
_RP, _ROWSP = _rel("p, t, cap", ("B", 2, 6), ("A", 3, 8), ("A", 1, 5), ("B", 1, 4), ("A", 2, 2), key=2)
_D3, _D4, _DP = (_R3, _ROWS3), (_R4, _ROWS4), (_RP, _ROWSP)
_R5C, _ROWS5C = _rel("t, cap", (4, 9), (2, 6), (5, 7), (1, 4), (3, 2))

# --- The oracle's navigator

_FRAME = re.compile(r"(?:FROM\s+(?P<lo>.+?)\s+TO\s+(?P<hi>.+?)|(?P<at>.+?))(?:\s+EVERY\s+(?P<every>\d+))?"
                    r"(?:\s+ELSE\s+(?P<fill>NULL|-?\d+)|\s+(?P<all>ALL))?", re.I)


def _order(order):
    """`key [ASC | DESC] [CYCLIC] [WITHIN a, b]` -> (key, desc, cyclic, WITHIN columns)."""
    head, *tail = re.split(r"\s+within\s+", order.strip(), maxsplit=1, flags=re.I)
    col = lambda c: c.strip(" ()").split(".")[-1]
    words = [w.upper() for w in head.split()]
    return (col(head.split()[0]), "DESC" in words, "CYCLIC" in words,
            [col(c) for c in tail[0].split(",")] if tail else [])


def _endpoint(sel, i, n):
    """(walk index, 'FIRST' | 'LAST' | None) of a selector read from position i of n."""
    w = sel.upper().split()
    if w[-1] in ("FIRST", "LAST"):
        return (0 if w[-1] == "FIRST" else n - 1), w[-1]
    k = int(w[0]) if len(w) == 2 else 1
    return (i - k if w[-1] == "PREVIOUS" else i + k), None


def _positions(m, i, n):
    if m["at"]:
        return [_endpoint(m["at"], i, n)[0]]
    (a, ka), (b, kb) = _endpoint(m["lo"], i, n), _endpoint(m["hi"], i, n)
    if (ka is None) == (kb is None):                  # a relative pair, or FIRST and LAST
        lo, hi = min(a, b), max(a, b)
    else:                                             # FIRST pins the low end, LAST the high
        rel = a if ka is None else b
        lo, hi = (0, rel) if "FIRST" in (ka, kb) else (rel, n - 1)
    return list(range(lo, hi + 1, int(m["every"] or 1)))


def _navigate(rows, frame, order):
    """{key: None} for a skipped instance, else {key: (row keys read, with repeats,
    constant contributed by ELSE)}. `frame` is the text before the frame's colon."""
    m = _FRAME.fullmatch(frame.strip())
    key, desc, cyclic, within = _order(order)
    fill = None if m["fill"] is None or m["fill"].upper() == "NULL" else int(m["fill"])
    parts = {}
    for k, r in rows.items():
        if r[key] is not None:                        # a NULL order key: on no timeline
            parts.setdefault(tuple(r[c] for c in within), []).append(k)
    out = dict.fromkeys(rows)
    for members in parts.values():                    # a NULL WITHIN key is a partition
        walk = [[k for k in members if rows[k][key] == v]
                for v in sorted({rows[k][key] for k in members}, reverse=desc)]
        for i, peers in enumerate(walk):
            positions = _positions(m, i, len(walk))
            inside = [p for p in positions if cyclic or 0 <= p < len(walk)]
            read = [r for p in inside for r in walk[p % len(walk)]]
            missing = len(positions) - len(inside)
            if m["at"]:
                value = (read, 0) if read else (None if fill is None else ([], fill))
            elif m["all"]:
                value = None if missing else (read, 0)
            elif fill is not None:
                value = (read, fill * missing)
            else:
                value = (read, 0) if read else None
            out.update(dict.fromkeys(peers, value))
    return out


def _landing(rows, frame, order):
    """row -> the single row an AT reads, or None: the form of the hand tables."""
    return {k: v[0][0] if v and v[0] else None for k, v in _navigate(rows, frame, order).items()}


def _data_bounds(rows, frame, order, col="cap"):
    """row -> the frame's value over known data (None: the instance is skipped)."""
    return {k: None if v is None else sum(rows[r][col] for r in v[0]) + v[1]
            for k, v in _navigate(rows, frame, order).items()}


def _reads(rows, frame, order, var="x", weight=None):
    """row -> None, or (coefficients of the decisions the frame reads, ELSE constant)."""
    out = {}
    for k, v in _navigate(rows, frame, order).items():
        if v is not None:
            coeffs = {}
            for r in v[0]:
                coeffs[_vname(r, var)] = coeffs.get(_vname(r, var), 0.0) + (rows[r][weight] if weight else 1.0)
            v = (coeffs, float(v[1]))
        out[k] = v
    return out


def _lin(*terms):
    """Sum (scale, coefficients) pairs into one coefficient dict."""
    out = {}
    for scale, coeffs in terms:
        for n, c in coeffs.items():
            out[n] = out.get(n, 0.0) + scale * c
    return out


# --- Running and checking

def _rows(decidb_cli, sql, *cols):
    rows, names = decidb_cli.execute(sql)
    idx = [names.index(c) for c in cols]
    return sorted((tuple(r[i] for i in idx) for r in rows),
                  key=lambda r: tuple((v is None, 0 if v is None else v) for v in r))


def _vname(key, var="x"):
    parts = key if isinstance(key, tuple) else (key,)
    return var + "_" + "_".join(str(k) for k in parts)


def _q(body, decl="x(INT) BETWEEN 0 AND 9", rel=_R3, cols="t, x"):
    return f"SELECT {cols} FROM {rel} DECIDE {decl} SUCH THAT {body} MAXIMIZE SUM(x)"


def _check_bounds(decidb_cli, oracle_solver, sql, ub, bounds, keys=("t",), var="x"):
    """Oracle for `<coef> * x <= <navigated bound> MAXIMIZE SUM(x)` over INT x in [0, ub]:
    `bounds` maps a row to its bound (a number, or a (coef, rhs) pair), or to None when
    the instance is skipped. Each row is boxed on its own, so the optimum is unique and
    the engine's vector must equal the oracle's, row for row."""
    got = _rows(decidb_cli, sql, *keys, var)
    oracle_solver.create_model("navigated_bounds")
    for key, bound in bounds.items():
        oracle_solver.add_variable(_vname(key, var), VarType.INTEGER, lb=0.0, ub=float(ub))
        if bound is not None:
            coef, rhs = bound if isinstance(bound, tuple) else (1.0, bound)
            oracle_solver.add_constraint({_vname(key, var): float(coef)}, "<=", float(rhs))
    oracle_solver.set_objective({_vname(k, var): 1.0 for k in bounds}, ObjSense.MAXIMIZE)
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL
    engine = {(r[0] if len(keys) == 1 else r[:-1]): int(r[-1]) for r in got}
    assert len(engine) == len(got)
    assert engine == {k: int(round(result.variable_values[_vname(k, var)])) for k in bounds}, got
    assert sum(engine.values()) == pytest.approx(result.objective_value)


def _solve(oracle, model, *extra):
    oracle.create_model("frames")
    model(oracle)
    for coeffs, sense, rhs in extra:
        oracle.add_constraint(coeffs, sense, rhs)
    return oracle.solve()


def _assert_unique(oracle, model, result):
    """No other integer point reaches the optimum: forcing any variable one step off
    its optimal value, either way, loses objective or is infeasible."""
    for name, value in result.variable_values.items():
        for sense, rhs in (("<=", round(value) - 1.0), (">=", round(value) + 1.0)):
            other = _solve(oracle, model, ({name: 1.0}, sense, rhs))
            assert (other.status != SolverStatus.OPTIMAL
                    or other.objective_value != pytest.approx(result.objective_value)), (name, sense)


def _check_vector(decidb_cli, oracle_solver, sql, keys, model, unique=True):
    """Runs `sql`, builds the oracle through `model(oracle)`, compares the objective and,
    when the optimum is unique (which is then proven), the decision vector; returns the
    engine's {key: x}. The engine must return exactly the rows the oracle has `x_*`
    variables for, so a dropped row fails even when its x is 0."""
    got = _rows(decidb_cli, sql, *keys, "x")
    result = _solve(oracle_solver, model)
    assert result.status == SolverStatus.OPTIMAL
    engine = {(r[0] if len(keys) == 1 else r[:-1]): int(r[-1]) for r in got}
    assert len(engine) == len(got)
    assert {_vname(k) for k in engine} == {n for n in result.variable_values if n.startswith("x_")}
    assert sum(engine.values()) == pytest.approx(result.objective_value)
    if unique:
        assert engine == {k: int(round(result.variable_values[_vname(k)])) for k in engine}
        _assert_unique(oracle_solver, model, result)
    return engine


def _int_vars(oracle, keys, ub=9.0):
    for k in keys:
        oracle.add_variable(_vname(k), VarType.INTEGER, lb=0.0, ub=ub)
    oracle.set_objective({_vname(k): 1.0 for k in keys}, ObjSense.MAXIMIZE)


# --- AT: selector x order x policy over a data body

_AT_CASES = [  # (selector, order, hand table: row -> the row its instance reads; None = missing)
    ("FIRST", "t", {1: 1, 2: 1, 3: 1}),
    ("LAST", "t", {1: 3, 2: 3, 3: 3}),
    ("PREVIOUS", "t", {1: None, 2: 1, 3: 2}),
    ("PREVIOUS", "t ASC", {1: None, 2: 1, 3: 2}),
    ("2 PREVIOUS", "t", {1: None, 2: None, 3: 1}),
    ("NEXT", "t", {1: 2, 2: 3, 3: None}),
    ("2 NEXT", "t", {1: 3, 2: None, 3: None}),
    ("FIRST", "t DESC", {1: 3, 2: 3, 3: 3}),
    ("LAST", "t DESC", {1: 1, 2: 1, 3: 1}),
    ("PREVIOUS", "t DESC", {1: 2, 2: 3, 3: None}),
    ("2 PREVIOUS", "t DESC", {1: 3, 2: None, 3: None}),
    ("NEXT", "t DESC", {1: None, 2: 1, 3: 2}),
    ("2 NEXT", "t DESC", {1: None, 2: None, 3: 1}),
    ("FIRST", "t CYCLIC", {1: 1, 2: 1, 3: 1}),
    ("LAST", "t CYCLIC", {1: 3, 2: 3, 3: 3}),
    ("PREVIOUS", "t CYCLIC", {1: 3, 2: 1, 3: 2}),
    ("2 PREVIOUS", "t CYCLIC", {1: 2, 2: 3, 3: 1}),
    ("3 PREVIOUS", "t CYCLIC", {1: 1, 2: 2, 3: 3}),
    ("NEXT", "t CYCLIC", {1: 2, 2: 3, 3: 1}),
    ("2 NEXT", "t CYCLIC", {1: 3, 2: 1, 3: 2}),
    ("4 NEXT", "t CYCLIC", {1: 2, 2: 3, 3: 1}),
    ("FIRST", "t DESC CYCLIC", {1: 3, 2: 3, 3: 3}),
    ("LAST", "t DESC CYCLIC", {1: 1, 2: 1, 3: 1}),
    ("PREVIOUS", "t DESC CYCLIC", {1: 2, 2: 3, 3: 1}),
    ("NEXT", "t DESC CYCLIC", {1: 3, 2: 1, 3: 2}),
    ("2 NEXT", "t DESC CYCLIC", {1: 2, 2: 3, 3: 1}),
]


@pytest.mark.var_integer
@pytest.mark.correctness
@pytest.mark.parametrize("policy", ["", "ELSE 0", "ELSE 5", "ELSE NULL"])
@pytest.mark.parametrize("selector, order, reads", _AT_CASES,
                         ids=[f"{s}|{o}" for s, o, _ in _AT_CASES])
def test_at_selector_reads_the_navigated_row(decidb_cli, oracle_solver, selector, order,
                                             reads, policy):
    """§5 / deck p43: `AT(sel [ELSE v]: cap) OVER (order)` reads cap at the selected
    position (caps t1..t3 = 6, 2, 8; box 9); DESC reverses the walk, CYCLIC wraps (a
    distance of 3 on a ring of 3 lands on the row itself, 4 on the next), a missing
    position reads the ELSE constant or skips the instance. Dropping DESC turns
    PREVIOUS|t DESC [2,8,skip] into [skip,6,2]; dropping CYCLIC turns PREVIOUS|t CYCLIC
    [8,6,2] into [skip,6,2]; dropping ELSE 0 lifts t1 from 0 to 9 under PREVIOUS|t.
    Honest no-ops, pinned as such: the policy on FIRST / LAST and on every CYCLIC case
    (no position is ever missing), CYCLIC on FIRST / LAST, `ELSE NULL` (the default
    spelled out, same answer as no ELSE) and ASC (the default direction)."""
    frame = f"{selector} {policy}".strip()
    assert _landing(_ROWS3, frame, order) == reads
    _check_bounds(decidb_cli, oracle_solver, _q(f"x <= AT({frame}: cap) OVER ({order})"), 9,
                  _data_bounds(_ROWS3, frame, order))


# --- Decision bodies

_CHAIN_CASES = [  # (id, relation, rows, key columns, frame, order, hand table)
    ("previous", _R3, _ROWS3, ("t",), "PREVIOUS", "t", {1: None, 2: 1, 3: 2}),
    ("2-previous", _T5, _ROWS5, ("t",), "2 PREVIOUS", "t", {1: None, 2: None, 3: 1, 4: 2, 5: 3}),
    ("2-next-else", _T5, _ROWS5, ("t",), "2 NEXT ELSE 5", "t", {1: 3, 2: 4, 3: 5, 4: None, 5: None}),
    ("desc-within", _RP, _ROWSP, ("p", "t"), "PREVIOUS", "t DESC WITHIN p",
     {("A", 1): ("A", 2), ("A", 2): ("A", 3), ("A", 3): None, ("B", 1): ("B", 2), ("B", 2): None}),
    ("null-order-key", *_rel("t, cap", (3, 8), (None, 2), (1, 6)), ("t",), "NEXT", "t",
     {1: 3, None: None, 3: None}),
]


@pytest.mark.var_integer
@pytest.mark.correctness
@pytest.mark.parametrize("rel, rows, keys, frame, order, reads", [c[1:] for c in _CHAIN_CASES],
                         ids=[c[0] for c in _CHAIN_CASES])
def test_selector_over_a_decision_chains_the_navigated_rows(decidb_cli, oracle_solver, rel, rows,
                                                            keys, frame, order, reads):
    """§5: `x <= AT(sel: x) OVER (order) - 1` ties each row to the decision of the row
    it navigates to; the free end is the row with no position (or the ELSE constant).
    PREVIOUS gives [9,8,7] where NEXT gives [7,8,9]; 2 NEXT ELSE 5 gives [2,3,3,4,4]
    where dropping the ELSE frees the tail ([7,8,8,9,9]); DESC within A gives [7,8,9]
    where ASC gives [9,8,7]. A NULL order key is on no timeline, so t1's NEXT is t3 and
    the NULL row is unconstrained: (t1, t3, NULL) = (8, 9, 9), where the NULL sorted as
    0 gives (8, 9, 7), as 2 gives (7, 9, 8) and as 9 gives (7, 8, 9)."""
    assert _landing(rows, frame, order) == reads

    def model(oracle):
        _int_vars(oracle, rows)
        for key, read in _reads(rows, frame, order).items():
            if read is not None:
                oracle.add_constraint(_lin((1.0, {_vname(key): 1.0}), (-1.0, read[0])), "<=", read[1] - 1.0)
    _check_vector(decidb_cli, oracle_solver,
                  _q(f"x <= AT({frame}: x) OVER ({order}) - 1", rel=rel, cols=", ".join(keys) + ", x"),
                  keys, model)


@pytest.mark.var_integer
@pytest.mark.correctness
@pytest.mark.parametrize("selector, order, want", [
    ("FIRST", "t", {1: 1, 2: 9, 3: 9}),
    ("LAST", "t DESC", {1: 1, 2: 9, 3: 9}),
    ("FIRST", "t DESC CYCLIC", {1: 9, 2: 9, 3: 1}),
])
def test_absolute_selector_over_a_decision_pins_the_endpoint(decidb_cli, oracle_solver, selector,
                                                             order, want):
    """§5: FIRST / LAST is one row for every instance, so `x + AT(FIRST: x) <= 10` is
    x_t + x_e <= 10 for the endpoint e: the unique optimum sacrifices x_e = 1 for 9 on
    the other two (19). Reading the instance's own row gives [5,5,5] (15); dropping DESC
    moves the endpoint (LAST|t pins t3: [9,9,1]; FIRST|t CYCLIC pins t1: [1,9,9]);
    swapping FIRST and LAST moves it too. CYCLIC does not move an absolute endpoint
    (the same answer without it, as pinned)."""
    def model(oracle):
        _int_vars(oracle, _ROWS3)
        for key, read in _reads(_ROWS3, selector, order).items():
            oracle.add_constraint(_lin((1.0, {_vname(key): 1.0}), (1.0, read[0])), "<=", 10.0)
    engine = _check_vector(decidb_cli, oracle_solver, _q(f"x + AT({selector}: x) OVER ({order}) <= 10"),
                           ("t",), model)
    assert engine == want


@pytest.mark.var_integer
@pytest.mark.correctness
def test_cyclic_chain_over_a_decision(decidb_cli, oracle_solver):
    """§5 (CYCLIC wraps): `x <= AT(NEXT: x) OVER (t CYCLIC) - 1` is a strict cycle, so
    the oracle and the engine both find it infeasible (without the wrap t3 is free and
    the chain solves); `x <= AT(PREVIOUS: x) OVER (t CYCLIC)` closes the ring so every x
    is equal and the cap of 2 at t2 binds all three: [2,2,2]. Without the wrap t1 is
    free and the answer is [6,2,2]."""
    def cycle(oracle):
        _int_vars(oracle, _ROWS3)
        for key, (coeffs, _) in _reads(_ROWS3, "NEXT", "t CYCLIC").items():
            oracle.add_constraint(_lin((1.0, {_vname(key): 1.0}), (-1.0, coeffs)), "<=", -1.0)
    assert _solve(oracle_solver, cycle).status == SolverStatus.INFEASIBLE
    decidb_cli.assert_error(_q("x <= AT(NEXT: x) OVER (t CYCLIC) - 1"), match=r"infeasible")

    def ring(oracle):
        for t, r in _ROWS3.items():
            oracle.add_variable(_vname(t), VarType.INTEGER, lb=0.0, ub=float(r["cap"]))
        oracle.set_objective({_vname(t): 1.0 for t in _ROWS3}, ObjSense.MAXIMIZE)
        for key, (coeffs, _) in _reads(_ROWS3, "PREVIOUS", "t CYCLIC").items():
            oracle.add_constraint(_lin((1.0, {_vname(key): 1.0}), (-1.0, coeffs)), "<=", 0.0)
    engine = _check_vector(decidb_cli, oracle_solver,
                           _q("x <= AT(PREVIOUS: x) OVER (t CYCLIC) AND x <= cap"), ("t",), ring)
    assert engine == {1: 2, 2: 2, 3: 2}


@pytest.mark.var_integer
@pytest.mark.correctness
def test_data_times_decision_inside_the_frame_body(decidb_cli, oracle_solver):
    """§5: `x <= AT(PREVIOUS ELSE 0: cap * x) OVER (t) + 1` reads the previous row's
    cap and decision together, and ELSE replaces the whole product: t1 <= 0 + 1, t2 <=
    6*x_1 + 1, t3 <= 2*x_2 + 1, so [1,7,9]. The instance's own cap with the previous x
    gives [1,3,9]; the previous cap with the own x frees t2 ([1,9,9]); dropping the
    ELSE frees t1 ([9,9,9])."""
    def model(oracle):
        _int_vars(oracle, _ROWS3)
        for key, (coeffs, fill) in _reads(_ROWS3, "PREVIOUS ELSE 0", "t", weight="cap").items():
            oracle.add_constraint(_lin((1.0, {_vname(key): 1.0}), (-1.0, coeffs)), "<=", fill + 1.0)
    engine = _check_vector(decidb_cli, oracle_solver,
                           _q("x <= AT(PREVIOUS ELSE 0: cap * x) OVER (t) + 1"), ("t",), model)
    assert engine == {1: 1, 2: 7, 3: 9}


# --- Data frames read per instance: declaration bound, scales, ELSE, EVERY, distance

_DATA_FRAME_CASES = [  # (id, sql, box, frame, rows, coefficient on x, scale on the frame)
    ("declaration-bound",
     f"SELECT t, x FROM {_R3} DECIDE x(INT) <= AT(PREVIOUS ELSE 9: cap) OVER (t) SUCH THAT x >= 0 MAXIMIZE SUM(x)",
     100, "PREVIOUS ELSE 9", _ROWS3, 1, 1),
    ("2*frame", _q("x <= 2 * AT(PREVIOUS ELSE 1: cap) OVER (t)", decl="x(INT) BETWEEN 0 AND 20"),
     20, "PREVIOUS ELSE 1", _ROWS3, 1, 2),
    ("frame/2", _q("x <= AT(PREVIOUS ELSE 4: cap) OVER (t) / 2", decl="x(INT) BETWEEN 0 AND 20"),
     20, "PREVIOUS ELSE 4", _ROWS3, 1, 0.5),
    ("0.5*x", _q("0.5 * x <= AT(PREVIOUS ELSE 1: cap) OVER (t)", decl="x(INT) BETWEEN 0 AND 20"),
     20, "PREVIOUS ELSE 1", _ROWS3, 0.5, 1),
    ("cap*x<=3*frame", _q("cap * x <= 3 * AT(PREVIOUS ELSE 2: cap) OVER (t)", decl="x(INT) BETWEEN 0 AND 20"),
     20, "PREVIOUS ELSE 2", _ROWS3, "cap", 3),
    ("else-once-per-missing-position",
     _q("x <= SUM(FROM 2 PREVIOUS TO PREVIOUS ELSE 7: cap) OVER (t)", decl="x(INT) BETWEEN 0 AND 20"),
     20, "FROM 2 PREVIOUS TO PREVIOUS ELSE 7", _ROWS3, 1, 1),
    ("every-from-lower-endpoint",
     _q("x <= SUM(FROM FIRST TO LAST EVERY 2: cap) OVER (t)", decl="x(INT) BETWEEN 0 AND 20", rel=_R4),
     20, "FROM FIRST TO LAST EVERY 2", _ROWS4, 1, 1),
    ("distance-999", _q("x <= AT(999 PREVIOUS ELSE 1: cap) OVER (t)"), 9, "999 PREVIOUS ELSE 1", _ROWS3, 1, 1),
]


@pytest.mark.var_integer
@pytest.mark.correctness
@pytest.mark.parametrize("sql, ub, frame, rows, coef, scale", [c[1:] for c in _DATA_FRAME_CASES],
                         ids=[c[0] for c in _DATA_FRAME_CASES])
def test_data_frame_bound_is_read_per_instance(decidb_cli, oracle_solver, sql, ub, frame, rows,
                                               coef, scale):
    """§2.2 (a declaration bound may be a frame over data: [9,6,2], and unbounded without
    its ELSE), §5 / delta §7 (a constant scale on a frame and a fractional factor on the
    decision keep the frame per instance: 2*frame is [2,12,4], where reading the frame
    as a sum over every row, 2 * 16, lifts every x to the box 20 and dropping the scale
    gives [1,6,2]; frame/2 is [2,3,1] against [4,6,2] unscaled), spec §7.3 (`ELSE v`
    fills each missing position: t1 reads 7+7, not 7; `EVERY 2` steps from the lower
    endpoint: positions 1 and 3 = 6, from the upper endpoint 15, without EVERY 20;
    distance 999 is accepted and always missing on three rows)."""
    per_row = _data_bounds(rows, frame, "t")
    _check_bounds(decidb_cli, oracle_solver, sql, ub,
                  {t: (rows[t][coef] if isinstance(coef, str) else coef, scale * b) for t, b in per_row.items()})


# --- Ranges

_RANGE_CASES = [  # (range, order, box, relation, hand table: row -> rows read; [] = empty, None = skipped)
    ("FROM FIRST TO PREVIOUS ELSE 5", "t", 9, _D3, {1: [], 2: [1], 3: [1, 2]}),
    ("FROM FIRST TO PREVIOUS", "t", 9, _D3, {1: None, 2: [1], 3: [1, 2]}),
    ("FROM FIRST TO PREVIOUS ALL", "t", 9, _D3, {1: [], 2: [1], 3: [1, 2]}),
    ("FROM FIRST TO PREVIOUS ELSE 5", "t DESC", 20, _D3, {1: [3, 2], 2: [3], 3: []}),
    ("FROM NEXT TO LAST ELSE 5", "t", 20, _D3, {1: [2, 3], 2: [3], 3: []}),
    ("FROM 2 NEXT TO LAST ELSE 5", "t", 9, _D3, {1: [3], 2: [], 3: []}),
    ("FROM FIRST TO NEXT ELSE 5", "t", 30, _D3, {1: [1, 2], 2: [1, 2, 3], 3: [1, 2, 3]}),
    ("FROM PREVIOUS TO NEXT", "t", 20, _D3, {1: [1, 2], 2: [1, 2, 3], 3: [2, 3]}),
    ("FROM NEXT TO PREVIOUS", "t", 20, _D3, {1: [1, 2], 2: [1, 2, 3], 3: [2, 3]}),
    ("FROM PREVIOUS TO NEXT ALL", "t", 20, _D3, {1: None, 2: [1, 2, 3], 3: None}),
    ("FROM PREVIOUS TO NEXT EVERY 2", "t", 20, _D3, {1: [2], 2: [1, 3], 3: [2]}),
    ("FROM LAST TO FIRST", "t", 20, _D3, {1: [1, 2, 3], 2: [1, 2, 3], 3: [1, 2, 3]}),
    ("FROM 2 PREVIOUS TO PREVIOUS ELSE NULL", "t", 20, _D3, {1: None, 2: [1], 3: [1, 2]}),
    ("FROM 5 PREVIOUS TO PREVIOUS", "t CYCLIC", 40, _D3,
     {1: [2, 3, 1, 2, 3], 2: [3, 1, 2, 3, 1], 3: [1, 2, 3, 1, 2]}),
    ("FROM 2 PREVIOUS TO PREVIOUS", "t DESC CYCLIC", 20, _D4, {1: [3, 2], 2: [4, 3], 3: [1, 4], 4: [2, 1]}),
    ("FROM PREVIOUS TO NEXT", "t CYCLIC WITHIN p", 20, _DP,
     {("A", 1): [("A", 3), ("A", 1), ("A", 2)], ("A", 2): [("A", 1), ("A", 2), ("A", 3)],
      ("A", 3): [("A", 2), ("A", 3), ("A", 1)], ("B", 1): [("B", 2), ("B", 1), ("B", 2)],
      ("B", 2): [("B", 1), ("B", 2), ("B", 1)]}),
]


@pytest.mark.var_integer
@pytest.mark.edge_case
@pytest.mark.correctness
@pytest.mark.parametrize("rng, order, ub, data, reads", _RANGE_CASES,
                         ids=[f"{c[0]}|{c[1]}" for c in _RANGE_CASES])
def test_range_sums_exactly_the_selected_positions(decidb_cli, oracle_solver, rng, order, ub, data, reads):
    """§5 / spec §7.3 (caps t1..t3 = 6, 2, 8): an absolute endpoint pins one end of the
    walk, so `FROM FIRST TO PREVIOUS` at the first position (t1 ASC, t3 DESC) and `FROM
    [2] NEXT TO LAST` at the last select no position: no ELSE fill (0, not the 5 one
    missing position would add), ALL imposes the empty sum 0, no ELSE skips the
    instance (9; on three rows this coincides with FROM 2 PREVIOUS TO PREVIOUS, which
    the ELSE 5 and ALL rows tell apart: 0 against 9); a relative endpoint past the
    timeline under an absolute one is a missing position (FIRST TO NEXT at t3 is 16 +
    5, where clipping to the timeline gives 16). FROM LAST TO FIRST is normalised to
    the whole timeline (16; read as an empty range every row is skipped at 20), and so
    is a relative pair; a straddling pair includes the current position ([8,16,10],
    not [2,14,2]); EVERY 2 skips the current row ([2,14,2]); ALL skips an incomplete
    frame ([20,16,20]); ELSE NULL skips only a range with no position at all. Under
    CYCLIC five previous positions on a ring of three are five reads (26/30/24; a set
    gives 16); DESC CYCLIC on the ring of four walks 4,3,2,1 and wraps ([8,11,13,10],
    ASC [11,13,10,8]); a ring of two reads the neighbour twice (B = [16,14]; without
    WITHIN every row reads 25 and the box 20 binds). A policy under CYCLIC is a no-op
    (nothing is missing)."""
    rel, rows = data
    keys = ("p", "t") if "WITHIN" in order else ("t",)
    got = {k: None if v is None else sorted(v[0]) for k, v in _navigate(rows, rng, order).items()}
    assert got == {k: None if v is None else sorted(v) for k, v in reads.items()}
    _check_bounds(decidb_cli, oracle_solver,
                  _q(f"x <= SUM({rng}: cap) OVER ({order})", decl=f"x(INT) BETWEEN 0 AND {ub}", rel=rel,
                     cols=", ".join(keys) + ", x"),
                  ub, _data_bounds(rows, rng, order), keys=keys)


@pytest.mark.var_integer
@pytest.mark.correctness
@pytest.mark.parametrize("policy, want", [
    ("ELSE 0", {1: 3, 2: 4, 3: 6}), ("ELSE 5", {1: 3, 2: 4, 3: 6}), ("ALL", {1: 3, 2: 4, 3: 6}),
    ("", {1: 9, 2: 7, 3: 9}), ("ELSE NULL", {1: 9, 2: 7, 3: 9}),
])
def test_empty_range_over_a_decision(decidb_cli, oracle_solver, policy, want):
    """§5 / delta §7: `2 * x <= SUM(FROM FIRST TO PREVIOUS: x) OVER (t) + 6` at t1
    selects no position: under ELSE v (any v) and ALL it is imposed as 2*x_1 <= 0 + 6,
    with no ELSE (or ELSE NULL) it is skipped; later rows read the prefix sum, so both
    answers are interior ([3,4,6] imposed, [9,7,9] skipped; skipping every instance
    gives [9,9,9]). One ELSE fill for the empty range gives 2*x_1 <= 11 under ELSE 5
    ([5,5,8]); a range that also read the instance's own row gives [6,9,9]. ELSE 0 /
    ELSE 5 / ALL agree by design, as do no ELSE and ELSE NULL."""
    frame = f"FROM FIRST TO PREVIOUS {policy}".strip()

    def model(oracle):
        _int_vars(oracle, _ROWS3)
        for key, read in _reads(_ROWS3, frame, "t").items():
            if read is not None:
                oracle.add_constraint(_lin((2.0, {_vname(key): 1.0}), (-1.0, read[0])), "<=", 6.0 + read[1])
    engine = _check_vector(decidb_cli, oracle_solver, _q(f"2 * x <= SUM({frame}: x) OVER (t) + 6"), ("t",), model)
    assert engine == want


@pytest.mark.var_integer
@pytest.mark.correctness
def test_straddling_range_over_a_decision(decidb_cli, oracle_solver):
    """deck p45: `SUM(FROM PREVIOUS TO NEXT: x) <= 10` is a sliding window of three
    including the current row (two at the ends). The optimum 20 is not unique, so the
    objective and every window are checked. Excluding the current row (the two
    neighbours alone) allows 29; the previous row alone allows 45 (no row binds); a
    window of five allows 10."""
    windows = {k: v[0] for k, v in _navigate(_ROWS5, "FROM PREVIOUS TO NEXT", "t").items()}

    def model(oracle):
        _int_vars(oracle, _ROWS5)
        for members in windows.values():
            oracle.add_constraint({_vname(m): 1.0 for m in members}, "<=", 10.0)
    engine = _check_vector(decidb_cli, oracle_solver,
                           _q("SUM(FROM PREVIOUS TO NEXT: x) OVER (t) <= 10", rel=_T5), ("t",), model,
                           unique=False)
    assert all(sum(engine[m] for m in members) <= 10 for members in windows.values())
    assert sum(engine.values()) == 20


@pytest.mark.var_integer
@pytest.mark.edge_case
@pytest.mark.correctness
def test_ties_on_the_order_key(decidb_cli, oracle_solver):
    """§5: rows with equal keys are peers at one position. A range sums every peer (id
    3 reads 5 + 7 = 12, where reading one peer gives 5 or 7); an AT from a tied position
    reads the next position (ids 1 and 2 both read id 3's cap 1, where ordering peers by
    id makes id 1 read id 2's 7); an AT onto a multi-row position is refused by name."""
    rel, rows = _rel("id, t, cap", (3, 2, 1), (1, 1, 5), (4, 3, 3), (2, 1, 7))
    for frame in ("SUM(FROM PREVIOUS TO PREVIOUS ELSE 9: cap)", "AT(NEXT ELSE 9: cap)"):
        _check_bounds(decidb_cli, oracle_solver,
                      _q(f"x <= {frame} OVER (t)", decl="x(INT) BETWEEN 0 AND 20", rel=rel, cols="id, x"),
                      20, _data_bounds(rows, frame[frame.index("(") + 1:frame.index(":")], "t"), keys=("id",))
    decidb_cli.assert_error(_q("x <= AT(PREVIOUS ELSE 9: cap) OVER (t)", rel=rel, cols="id, x"),
                            match=r"per position")


# --- NULL keys, WHEN, IF, PER ROW, WITHIN shapes

@pytest.mark.var_integer
@pytest.mark.edge_case
@pytest.mark.correctness
@pytest.mark.parametrize("order", ["t", "t CYCLIC"])
def test_null_order_key_is_on_no_timeline_and_null_within_key_is_a_partition(decidb_cli, oracle_solver,
                                                                             order):
    """§3.3 / spec §6.2 / delta §1: a NULL order key puts the row on no timeline: its
    instance is skipped (9, not the ELSE 0 of a missing position) and t3's previous is
    t1 (6); under CYCLIC t1 and t3 read each other (8, 6). The NULL written as 0 makes
    t1 read 2, written as 2 makes t3 read 2. A NULL WITHIN key is a partition like any
    other: the two NULL-keyed rows form one timeline, so (NULL, 2) reads 6 (CYCLIC:
    (NULL, 1) reads 2), where dropping the NULL-keyed rows (`WHEN p IS NOT NULL`) leaves
    both at 9 and treating each NULL as distinct gives (NULL, 2) 0 (CYCLIC: each reads
    itself, 6 and 2); the lone A row on a ring reads itself (8)."""
    rel, rows = _rel("t, cap", (3, 8), (None, 2), (1, 6))
    _check_bounds(decidb_cli, oracle_solver, _q(f"x <= AT(PREVIOUS ELSE 0: cap) OVER ({order})", rel=rel),
                  9, _data_bounds(rows, "PREVIOUS ELSE 0", order))
    rel, rows = _rel("p, t, cap", ("A", 1, 8), (None, 2, 2), (None, 1, 6), key=2)
    _check_bounds(decidb_cli, oracle_solver,
                  _q(f"x <= AT(PREVIOUS ELSE 0: cap) OVER ({order} WITHIN p)", rel=rel, cols="p, t, x"),
                  9, _data_bounds(rows, "PREVIOUS ELSE 0", f"{order} WITHIN p"), keys=("p", "t"))


@pytest.mark.var_integer
@pytest.mark.when_constraint
@pytest.mark.correctness
@pytest.mark.parametrize("order", ["t", "t DESC", "t CYCLIC", "t DESC CYCLIC"])
@pytest.mark.parametrize("agg, frame", [("AT", "PREVIOUS ELSE 0"), ("SUM", "FROM 2 PREVIOUS TO PREVIOUS ELSE 5")],
                         ids=["at", "range"])
@pytest.mark.parametrize("prefix", ["WHEN t <> 2:", "WHEN t <> 2 PER ROW:"])
def test_when_removes_the_period_from_the_timeline(decidb_cli, oracle_solver, prefix, agg, frame, order):
    """spec §7.3 (the timeline is built over the WHEN-filtered rows; caps t1..t5 = 4, 6,
    2, 9, 7, box 20): with t2 filtered out the timeline is 1,3,4,5 and t2 has no
    instance (20). AT(PREVIOUS ELSE 0) reads [0,-,4,2,9] ascending, [2,-,9,7,0]
    descending, [7,-,4,2,9] cyclic, [2,-,9,7,4] descending cyclic; dropping the WHEN
    makes t3 read t2's 6 and bounds t2. The range FROM 2 PREVIOUS TO PREVIOUS ELSE 5
    reads [10,-,9,6,11] ascending (unfiltered: t3 10, t4 8), [11,-,16,12,10]
    descending, [16,-,11,6,11] cyclic and [11,-,16,11,6] descending cyclic, so DESC and
    CYCLIC each move a bound; either ELSE is a no-op under CYCLIC (nothing is missing).
    `PER ROW` is the explicit default (the same answer without it, as pinned)."""
    rows = {t: r for t, r in _ROWS5C.items() if r["t"] != 2}
    bounds = {**_data_bounds(rows, frame, order), 2: None}
    _check_bounds(decidb_cli, oracle_solver,
                  _q(f"{prefix} x <= {agg}({frame}: cap) OVER ({order})", decl="x(INT) BETWEEN 0 AND 20",
                     rel=_R5C), 20, bounds)


@pytest.mark.var_boolean
@pytest.mark.var_integer
@pytest.mark.correctness
def test_if_guard_over_a_frame_bound(decidb_cli, oracle_solver):
    """§3.2 + §5: `IF o: x <= AT(PREVIOUS ELSE 0: cap)` imposes the navigated bound only
    where the row's switch is on; with two switches forced on, leaving t1 (bound 0)
    unguarded wins: o = [0,1,1], x = [9,6,2] (17). Dropping the IF imposes every bound
    ([0,6,2], 8); guarding the own cap gives [6,9,8] (23); dropping the ELSE leaves t1
    without an instance, so the guard moves to t1 ([9,6,9], 24)."""
    got = _rows(decidb_cli, f"""
        SELECT t, x, o FROM {_R3} DECIDE x(INT) BETWEEN 0 AND 9, o(BOOL)
        SUCH THAT IF o: x <= AT(PREVIOUS ELSE 0: cap) OVER (t) AND SUM(o) >= 2 MAXIMIZE SUM(x)""",
                "t", "x", "o")

    def model(oracle):
        _int_vars(oracle, _ROWS3)
        for t in _ROWS3:
            oracle.add_variable(f"o_{t}", VarType.BINARY)
        for t, bound in _data_bounds(_ROWS3, "PREVIOUS ELSE 0", "t").items():
            oracle.add_indicator_constraint(f"o_{t}", 1, {_vname(t): 1.0}, "<=", float(bound))
        oracle.add_constraint({f"o_{t}": 1.0 for t in _ROWS3}, ">=", 2.0)
    result = _solve(oracle_solver, model)
    assert result.status == SolverStatus.OPTIMAL
    assert got == [(t, round(result.variable_values[_vname(t)]), round(result.variable_values[f"o_{t}"]))
                   for t in sorted(_ROWS3)]
    _assert_unique(oracle_solver, model, result)
    assert got == [(1, 9, 0), (2, 6, 1), (3, 2, 1)]


@pytest.mark.var_integer
@pytest.mark.per_clause
@pytest.mark.correctness
@pytest.mark.parametrize("order", [
    "t WITHIN p", "t WITHIN (p)", "r.t   within   r.p", "t DESC CYCLIC WITHIN p",
], ids=["column", "parenthesized", "lower-case-qualified", "desc-cyclic-within"])
def test_within_partitions_the_timeline(decidb_cli, oracle_solver, order):
    """§5 / deck p52: WITHIN p gives one timeline per product; `(p)`, lower case and
    qualification are the same key. Under DESC CYCLIC inside the partition, PREVIOUS of
    t1 is t2 and PREVIOUS of A's t3 wraps to A's t1, never to B. Dropping WITHIN puts
    A's and B's t1 at one position (an AT onto two rows is refused); dropping DESC gives
    A [8,5,2] for [2,8,5]; dropping CYCLIC gives A [2,8,15] and B [6,15]; dropping the
    ELSE 15 frees every first period to the box 20 (a no-op under CYCLIC, where nothing
    is missing). `PER ROW` is the default spelled out (the same answer without it)."""
    _check_bounds(decidb_cli, oracle_solver,
                  _q(f"PER ROW: x <= AT(PREVIOUS ELSE 15: cap) OVER ({order})",
                     decl="x(INT) BETWEEN 0 AND 20", rel=_RP, cols="p, t, x"),
                  20, _data_bounds(_ROWSP, "PREVIOUS ELSE 15", order), keys=("p", "t"))


_REGIONS = _rel("reg, p, t, cap", ("S", "A", 2, 6), ("N", "A", 2, 2), ("N", "B", 1, 1), ("S", "A", 1, 4),
                ("N", "A", 1, 5), key=3)
_POLICY_JOIN = """CREATE TEMP TABLE pol(p VARCHAR PRIMARY KEY, lim INT); INSERT INTO pol VALUES ('B', 4), ('A', 5);
    CREATE TEMP TABLE dem(p VARCHAR, t INT, d INT);
    INSERT INTO dem VALUES ('B', 2, 4), ('A', 1, 3), ('B', 1, 2), ('A', 2, 1);
    SELECT dem.p, dem.t, x FROM dem JOIN pol ON pol.p = dem.p DECIDE x(INT) BETWEEN 0 AND 20
    SUCH THAT x <= AT(PREVIOUS ELSE 15: d) OVER (t WITHIN pol) MAXIMIZE SUM(x)"""
_POLICY_ROWS = {(p, t): {"p": p, "lim": {"A": 5, "B": 4}[p], "t": t, "d": d}
                for p, t, d in [("B", 2, 4), ("A", 1, 3), ("B", 1, 2), ("A", 2, 1)]}


@pytest.mark.var_integer
@pytest.mark.sql_joins
@pytest.mark.correctness
@pytest.mark.parametrize("sql, rows, order, col, keys", [
    (_q("x <= AT(PREVIOUS ELSE 15: cap) OVER (t WITHIN reg, p)", decl="x(INT) BETWEEN 0 AND 20",
        rel=_REGIONS[0], cols="reg, p, t, x"), _REGIONS[1], "t WITHIN reg, p", "cap", ("reg", "p", "t")),
    (_POLICY_JOIN, _POLICY_ROWS, "t WITHIN p, lim", "d", ("p", "t")),
], ids=["two-columns", "relation"])
def test_within_two_columns_or_a_relation(decidb_cli, oracle_solver, sql, rows, order, col, keys):
    """§5 / §2.1: `WITHIN reg, p` is one timeline per (reg, p) pair: (N,A,2) reads 5 and
    (S,A,2) reads 4, where partitioning on p alone (or reg alone) puts two rows at t1 and
    refuses the AT; `WITHIN pol` over the one-row-per-product policy table expands to
    its columns (p, lim), one timeline per product: B's t2 reads 2, A's t2 reads 3, where
    dropping WITHIN refuses the AT and a row reading its own d gives 4 and 1."""
    _check_bounds(decidb_cli, oracle_solver, sql, 20, _data_bounds(rows, "PREVIOUS ELSE 15", order, col=col),
                  keys=keys)


# --- PER prefixes around a frame

@pytest.mark.var_integer
@pytest.mark.per_clause
@pytest.mark.correctness
@pytest.mark.parametrize("prefix, keep, frame, order", [
    ("PER p:", None, "AT(FIRST: cap)", "t WITHIN p"),
    ("PER p:", None, "AT(LAST: cap)", "t WITHIN p"),
    ("PER p:", None, "AT(LAST: cap)", "t DESC WITHIN p"),
    ("PER p:", None, "SUM(FROM FIRST TO LAST EVERY 2: cap)", "t WITHIN p"),
    ("WHEN t <> 1 PER p:", lambda r: r["t"] != 1, "AT(FIRST: cap)", "t WITHIN p"),
    ("WHEN cap > 2 PER p:", lambda r: r["cap"] > 2, "SUM(FROM FIRST TO LAST EVERY 2: cap)",
     "t DESC CYCLIC WITHIN p"),
], ids=["first", "last", "last-desc", "first-to-last-every-2", "when-first", "when-desc-cyclic-every-2"])
def test_keyed_per_with_an_absolute_selector_binds(decidb_cli, oracle_solver, prefix, keep, frame, order):
    """deck p52 / delta §7: under `PER p` an absolute selector needs only the WITHIN
    partition determined, so one instance per product reads its timeline's endpoint:
    FIRST 5/4, LAST 8/6, LAST under DESC 5/4, EVERY 2 from FIRST to LAST 13/4 (every
    position: 15/10); after `WHEN t <> 1` the first position of A is t2 (2/6, where
    dropping the WHEN gives 5/4); after `WHEN cap > 2` A's DESC timeline is t3, t1 and
    EVERY 2 reads t3 alone (8/6, where dropping the WHEN gives 13/6 and dropping DESC
    5/4; CYCLIC is a no-op on a FIRST..LAST range). Dropping WITHIN refuses FIRST (two
    rows at t1) and reads 8/8 for LAST. Dropping `PER p:` is equivalent here (every row
    of p reads the same endpoint), so the per-product bound is what the test pins.
    SUM(y) counts y_p once per row of p."""
    rows = {k: r for k, r in _ROWSP.items() if keep is None or keep(r)}
    per_row = _data_bounds(rows, frame[frame.index("(") + 1:frame.index(":")], order)
    bounds = {p: {b for k, b in per_row.items() if k[0] == p} for p in ("A", "B")}
    assert all(len(b) == 1 for b in bounds.values())      # FIRST / LAST ignore the position
    got = _rows(decidb_cli, f"""
        SELECT p, t, y FROM {_RP} DECIDE PER p: y(INT) BETWEEN 0 AND 20
        SUCH THAT {prefix} y <= {frame} OVER ({order}) MAXIMIZE SUM(y)""", "p", "t", "y")
    oracle_solver.create_model("keyed")
    for p, (bound,) in bounds.items():
        oracle_solver.add_variable(f"y_{p}", VarType.INTEGER, lb=0.0, ub=20.0)
        oracle_solver.add_constraint({f"y_{p}": 1.0}, "<=", float(bound))
    oracle_solver.set_objective({f"y_{p}": float(sum(k[0] == p for k in _ROWSP)) for p in bounds},
                                ObjSense.MAXIMIZE)
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL
    assert [(p, t) for p, t, _ in got] == sorted(_ROWSP)
    assert all(y == int(round(result.variable_values[f"y_{p}"])) for p, _, y in got), got
    assert sum(y for _, _, y in got) == pytest.approx(result.objective_value)


@pytest.mark.var_integer
@pytest.mark.per_clause
@pytest.mark.correctness
def test_per_key_covering_order_and_partition_binds(decidb_cli, oracle_solver):
    """§3.1 / delta §6: `PER p, t` determines both the partition and the order key, so
    PREVIOUS binds for a `PER p, t` decision (A [15,5,2], B [15,4]); and `PER t` over a
    table whose PRIMARY KEY is t admits the per-row decision through the schema's
    functional dependency ([0,6,2]), where the same table without the key is refused.
    Both PER prefixes are equivalent to their omission here ((p, t) and t are unique),
    so what the test pins is that they bind and navigate the same timeline."""
    _check_bounds(decidb_cli, oracle_solver, f"""
        SELECT p, t, y FROM {_RP} DECIDE PER p, t: y(INT) BETWEEN 0 AND 20
        SUCH THAT PER p, t: y <= AT(PREVIOUS ELSE 15: cap) OVER (t WITHIN p) MAXIMIZE SUM(y)""",
                  20, _data_bounds(_ROWSP, "PREVIOUS ELSE 15", "t WITHIN p"), keys=("p", "t"), var="y")
    table = "CREATE TEMP TABLE r(t INT{}, cap INT); INSERT INTO r VALUES (3, 8), (1, 6), (2, 2);"
    per_t = ("SELECT t, x FROM r DECIDE x(INT) BETWEEN 0 AND 9 "
             "SUCH THAT PER t: x <= AT(PREVIOUS ELSE 0: cap) OVER (t) MAXIMIZE SUM(x)")
    _check_bounds(decidb_cli, oracle_solver, table.format(" PRIMARY KEY") + per_t,
                  9, _data_bounds(_ROWS3, "PREVIOUS ELSE 0", "t"))
    decidb_cli.assert_error(table.format("") + per_t, match=r"generated once per row")


@pytest.mark.var_integer
@pytest.mark.per_clause
@pytest.mark.correctness
def test_per_empty_with_an_absolute_selector_is_one_instance(decidb_cli, oracle_solver, tmp_path):
    """§5: FIRST does not depend on the instance's position and `WITHIN ()` is determined
    by `PER ()`, so `PER (): AT(FIRST: x) OVER (t) <= 5` is the single row x_1 <= 5 and
    `PER (): SUM(FROM FIRST TO LAST: x) OVER (t) <= 10` the single row x_1 + x_2 + x_3
    <= 10. Without `PER ()` the optimum is the same (each row repeats the constraint),
    so the model dump pins the instance count: 1 row, where per-row generation builds 3.
    Reading LAST instead bounds t3 ([9,9,5])."""
    first = {t: 5 if v[0] == [t] else None for t, v in _navigate(_ROWS3, "FIRST", "t").items()}
    sql = _q("PER (): AT(FIRST: x) OVER (t) <= 5")
    _check_bounds(decidb_cli, oracle_solver, sql, 9, first)
    whole = _q("PER (): SUM(FROM FIRST TO LAST: x) OVER (t) <= 10")
    for i, s in enumerate((sql, whole)):
        dump = decidb_cli.dump_model(s, tmp_path / f"model{i}.txt")
        assert int(re.search(r"num_rows: (\d+)", dump).group(1)) == 1

    def model(oracle):
        _int_vars(oracle, _ROWS3)
        for read in _reads(_ROWS3, "FROM FIRST TO LAST", "t").values():
            oracle.add_constraint(read[0], "<=", 10.0)
    engine = _check_vector(decidb_cli, oracle_solver, whole, ("t",), model, unique=False)
    assert sum(engine.values()) == 10


# --- Sibling terms: two frames, a frame beside a reducer

_DUE, _DUE_ROWS = _rel("t, due", (3, 20), (1, 30), (2, 10))
_TWO_ORDERS = _q("AT(PREVIOUS ELSE 0: x) OVER (t) + AT(PREVIOUS ELSE 0: x) OVER (due) <= 6", rel=_DUE)


def _two_orders_model(oracle):
    _int_vars(oracle, _DUE_ROWS)
    by_t, by_due = _reads(_DUE_ROWS, "PREVIOUS ELSE 0", "t"), _reads(_DUE_ROWS, "PREVIOUS ELSE 0", "due")
    for key in _DUE_ROWS:
        oracle.add_constraint(_lin((1.0, by_t[key][0]), (1.0, by_due[key][0])), "<=",
                              6.0 - by_t[key][1] - by_due[key][1])


@pytest.mark.var_integer
@pytest.mark.correctness
def test_two_frames_of_one_decision_with_different_orders(decidb_cli, oracle_solver):
    """deck p47 / p56: sibling frames own their OVER. By t the previous rows are -,1,2;
    by due (10:t2, 20:t3, 30:t1) they are t3,-,t2, so the rows are x_3 <= 6, x_1 <= 6,
    2*x_2 <= 6 ([6,3,6]). Merging both onto the t order gives 2*x_1 <= 6, 2*x_2 <= 6
    ([3,3,9]); onto the due order 2*x_3 <= 6, 2*x_2 <= 6 ([9,3,3])."""
    engine = _check_vector(decidb_cli, oracle_solver, _TWO_ORDERS, ("t",), _two_orders_model)
    assert engine == {1: 6, 2: 3, 3: 6}


@pytest.mark.var_integer
@pytest.mark.correctness
def test_previous_and_next_frames_of_one_decision_in_one_body(decidb_cli, oracle_solver):
    """delta §9: `AT(PREVIOUS ELSE 0: x) <= 0.5 * AT(NEXT ELSE 0: x)` keeps two
    navigations: t2 says x_1 <= 0.5*x_3, t3 says x_2 <= 0, t1 is trivial ([4,0,9]).
    Collapsing the NEXT read onto the PREVIOUS position gives [0,0,9]; dropping the
    right ELSE frees t2 ([4,9,9]). The left ELSE 0 is a no-op here (t1 reads 0 <= ...)."""
    rows = _rel("t", (2,), (3,), (1,))

    def model(oracle):
        _int_vars(oracle, rows[1])
        prev, nxt = _reads(rows[1], "PREVIOUS ELSE 0", "t"), _reads(rows[1], "NEXT ELSE 0", "t")
        for key in rows[1]:
            oracle.add_constraint(_lin((1.0, prev[key][0]), (-0.5, nxt[key][0])), "<=",
                                  0.5 * nxt[key][1] - prev[key][1])
    engine = _check_vector(decidb_cli, oracle_solver,
                           _q("AT(PREVIOUS ELSE 0: x) OVER (t) <= 0.5 * AT(NEXT ELSE 0: x) OVER (t)",
                              rel=rows[0]), ("t",), model)
    assert engine == {1: 4, 2: 0, 3: 9}


@pytest.mark.var_integer
@pytest.mark.cons_mixed
@pytest.mark.correctness
def test_frame_beside_a_reducer_of_the_same_decision(decidb_cli, oracle_solver):
    """delta §9: `AT(PREVIOUS ELSE 8: x) >= 0.5 * SUM(x)` is one row per instance with
    the global sum on the right: 8 >= S/2, x_1 >= S/2, x_2 >= S/2, whose optimum is
    [8,8,0]. Dropping the ELSE gives [9,9,0]; merging the two terms of x (reading
    SUM(x) as the previous row alone) lets [9,9,9] through."""
    rows = _rel("t", (2,), (3,), (1,))

    def model(oracle):
        _int_vars(oracle, rows[1])
        total = {_vname(t): -0.5 for t in rows[1]}
        for key, (coeffs, fill) in _reads(rows[1], "PREVIOUS ELSE 8", "t").items():
            oracle.add_constraint(_lin((1.0, coeffs), (1.0, total)), ">=", -fill)
    engine = _check_vector(decidb_cli, oracle_solver,
                           _q("AT(PREVIOUS ELSE 8: x) OVER (t) >= 0.5 * SUM(x)", rel=rows[0]), ("t",), model)
    assert engine == {1: 8, 2: 8, 3: 0}


@pytest.mark.var_integer
@pytest.mark.cons_mixed
@pytest.mark.correctness
def test_global_reducer_against_a_data_frame_is_per_row(decidb_cli, oracle_solver):
    """§4 matrix #1 with a frame bound: `0.5 * SUM(x) <= AT(PREVIOUS ELSE 1: cap)` is
    one row per instance (S/2 <= 1, S/2 <= 6, S/2 <= 2), so S <= 2. Dropping the ELSE
    gives S = 4; reading the frame as a sum over every row builds S/2 <= 16 and returns
    27; reading the own row's cap gives S = 4."""
    def model(oracle):
        _int_vars(oracle, _ROWS3)
        for bound in _data_bounds(_ROWS3, "PREVIOUS ELSE 1", "t").values():
            oracle.add_constraint({_vname(t): 0.5 for t in _ROWS3}, "<=", float(bound))
    engine = _check_vector(decidb_cli, oracle_solver,
                           _q("0.5 * SUM(x) <= AT(PREVIOUS ELSE 1: cap) OVER (t)"), ("t",), model, unique=False)
    assert sum(engine.values()) == 2


@pytest.mark.var_boolean
@pytest.mark.var_integer
@pytest.mark.per_clause
@pytest.mark.correctness
def test_keyed_bool_read_at_the_previous_row(decidb_cli, oracle_solver):
    """§2.1 + §5: a `PER p` BOOL navigated to from the next period is the product's
    switch; `x <= 10 * AT(PREVIOUS ELSE 1: open)` frees the first period of each product
    (to 10) and ties the later ones to the switch. Opening A (40) beats B (30); dropping
    the ELSE frees the first periods to 20 (60); dropping WITHIN refuses the AT (two
    rows at t1)."""
    got = _rows(decidb_cli, f"""
        SELECT p, t, x, open FROM {_RP} DECIDE x(INT) BETWEEN 0 AND 20, PER p: open(BOOL)
        SUCH THAT x <= 10 * AT(PREVIOUS ELSE 1: open) OVER (t WITHIN p)
              AND SUM(PER p: open) <= 1 MAXIMIZE SUM(x)""", "p", "t", "x", "open")

    def model(oracle):
        for p in ("A", "B"):
            oracle.add_variable(f"open_{p}", VarType.BINARY)
        for key in _ROWSP:
            oracle.add_variable(_vname(key), VarType.INTEGER, lb=0.0, ub=20.0)
        for key, (read, fill) in _navigate(_ROWSP, "PREVIOUS ELSE 1", "t WITHIN p").items():
            oracle.add_constraint(_lin((1.0, {_vname(key): 1.0}), (-10.0, {f"open_{r[0]}": 1.0 for r in read})),
                                  "<=", 10.0 * fill)
        oracle.add_constraint({"open_A": 1.0, "open_B": 1.0}, "<=", 1.0)
        oracle.set_objective({_vname(k): 1.0 for k in _ROWSP}, ObjSense.MAXIMIZE)
    result = _solve(oracle_solver, model)
    assert result.status == SolverStatus.OPTIMAL
    assert got == [(p, t, round(result.variable_values[_vname((p, t))]), round(result.variable_values[f"open_{p}"]))
                   for p, t in sorted(_ROWSP)]
    assert sum(x for _, _, x, _ in got) == pytest.approx(result.objective_value) == 40
    _assert_unique(oracle_solver, model, result)


# --- Serialization and the deck's production plan

@pytest.mark.var_integer
@pytest.mark.correctness
def test_frame_fields_survive_plan_serialization(decidb_cli, oracle_solver):
    """§9: DESC, WITHIN, EVERY, ELSE and the second frame's own OVER all reach the wire.
    `FROM 3 PREVIOUS TO PREVIOUS EVERY 2` reads the 3-previous and the previous position
    only. Under DESC within A the walk is 3,2,1: t1 reads 7 + cap(t2), t2 reads 7 +
    cap(t3), t3 reads 7 + 7 -> A [9,15,14], B [13,14]. A lost EVERY (every position)
    lifts A's t1 to 17; a lost DESC flips A to [14,12,9]; a lost ELSE gives A [2,8,20];
    a lost WITHIN makes A's t1 read t2's peers of both products (15); a lost second
    OVER gives the two-orders query [3,3,9] for [6,3,6]."""
    cli = decidb_cli.with_verify_serializer()
    frame, order = "FROM 3 PREVIOUS TO PREVIOUS EVERY 2 ELSE 7", "t DESC WITHIN p"
    _check_bounds(cli, oracle_solver,
                  _q(f"x <= SUM({frame}: cap) OVER ({order})", decl="x(INT) BETWEEN 0 AND 20", rel=_RP,
                     cols="p, t, x"),
                  20, _data_bounds(_ROWSP, frame, order), keys=("p", "t"))
    engine = _check_vector(cli, oracle_solver, _TWO_ORDERS, ("t",), _two_orders_model)
    assert engine == {1: 6, 2: 3, 3: 6}


@pytest.mark.var_integer
@pytest.mark.sql_joins
@pytest.mark.correctness
def test_deck_production_plan_end_to_end(decidb_cli, oracle_solver):
    """deck p73-75: balance from the previous period (opening stock 2), a NEXT ramp that
    is skipped at the last period, FIRST/LAST inventory floors at the product's safety
    stock, cost objective: A produces 5,2,4 (stock 1,2,1), B 3,3,4 (stock 3,0,2), cost
    46, unique. Every construct binds: dropping the ELSE 2 skips the first balance (33),
    dropping the ramp gives 42, reading the ramp at PREVIOUS instead of NEXT 45,
    dropping the FIRST floor 45, the LAST floor 37; dropping WITHIN refuses the AT (two
    rows per period). Swapping FIRST and LAST is equivalent here (one floor for both)."""
    demand = {("A", 1): 6, ("A", 2): 1, ("A", 3): 5, ("B", 1): 2, ("B", 2): 6, ("B", 3): 2}
    policy = {"A": (6, 2, 1, 2, 1), "B": (5, 1, 2, 1, 2)}  # max_rate, max_ramp, hold, run, safety
    order = [("B", 2), ("A", 3), ("A", 1), ("B", 3), ("A", 2), ("B", 1)]
    got = _rows(decidb_cli, f"""
        CREATE TEMP TABLE demand_forecast(product VARCHAR, period INT, demand INT);
        INSERT INTO demand_forecast VALUES {", ".join(f"('{p}', {t}, {demand[p, t]})" for p, t in order)};
        CREATE TEMP TABLE product_policy(product VARCHAR PRIMARY KEY, opening_stock INT,
            safety_stock INT, max_rate INT, warehouse_cap INT, max_ramp INT, hold_cost INT, run_cost INT);
        INSERT INTO product_policy VALUES ('B',2,2,5,20,1,2,1),('A',2,1,6,20,2,1,2);
        SELECT D.product, D.period, produce, inventory
        FROM demand_forecast D JOIN product_policy P ON P.product = D.product
        DECIDE produce(INT) BETWEEN 0 AND max_rate, inventory(INT) BETWEEN 0 AND warehouse_cap
        SUCH THAT inventory = AT(PREVIOUS ELSE 2: inventory) OVER (period WITHIN D.product) + produce - demand
              AND AT(NEXT: produce) OVER (period WITHIN D.product) - produce <= max_ramp
              AND AT(FIRST: inventory) OVER (period WITHIN D.product) >= safety_stock
              AND AT(LAST: inventory) OVER (period WITHIN D.product) >= safety_stock
        MINIMIZE SUM(hold_cost * inventory + run_cost * produce)""",
        "product", "period", "produce", "inventory")
    rows = {k: {"product": k[0], "period": k[1]} for k in demand}
    prev = _reads(rows, "PREVIOUS ELSE 2", "period WITHIN product", var="inv")
    nxt = _reads(rows, "NEXT", "period WITHIN product", var="prod")
    first = _reads(rows, "FIRST", "period WITHIN product", var="inv")
    last = _reads(rows, "LAST", "period WITHIN product", var="inv")

    def model(oracle):
        obj = {}
        for (p, t), d in demand.items():
            rate, ramp, hold, run, _ = policy[p]
            prod, inv = _vname((p, t), "prod"), _vname((p, t), "inv")
            oracle.add_variable(prod, VarType.INTEGER, lb=0.0, ub=float(rate))
            oracle.add_variable(inv, VarType.INTEGER, lb=0.0, ub=20.0)
            obj[prod], obj[inv] = float(run), float(hold)
        for (p, t), d in demand.items():
            prod, inv = _vname((p, t), "prod"), _vname((p, t), "inv")
            coeffs, fill = prev[p, t]
            oracle.add_constraint(_lin((1.0, {inv: 1.0, prod: -1.0}), (-1.0, coeffs)), "=", fill - d)
            if nxt[p, t] is not None:
                oracle.add_constraint(_lin((1.0, nxt[p, t][0]), (-1.0, {prod: 1.0})), "<=", float(policy[p][1]))
            oracle.add_constraint(first[p, t][0], ">=", float(policy[p][4]))
            oracle.add_constraint(last[p, t][0], ">=", float(policy[p][4]))
        oracle.set_objective(obj, ObjSense.MINIMIZE)
    result = _solve(oracle_solver, model)
    assert result.status == SolverStatus.OPTIMAL
    plan = {(p, t): (pr, inv) for p, t, pr, inv in got}
    assert plan == {k: (round(result.variable_values[_vname(k, "prod")]), round(result.variable_values[_vname(k, "inv")]))
                    for k in demand}
    cost = sum(policy[p][3] * pr + policy[p][2] * inv for (p, _), (pr, inv) in plan.items())
    assert cost == pytest.approx(result.objective_value) == 46
    _assert_unique(oracle_solver, model, result)
    assert plan == {("A", 1): (5, 1), ("A", 2): (2, 2), ("A", 3): (4, 1),
                    ("B", 1): (3, 3), ("B", 2): (3, 0), ("B", 3): (4, 2)}


# --- Refusals by name

_XY = "x(INT) BETWEEN 0 AND 9, y(INT) BETWEEN 0 AND 9"
_PER_P = f"SELECT p, t, y FROM {_RP} DECIDE PER p: y(INT) BETWEEN 0 AND 20 SUCH THAT PER p: y <= {{}} MAXIMIZE SUM(y)"
_REFUSALS = [  # (what, sql, topic)
    ("frame in an objective", f"SELECT t, x FROM {_R3} DECIDE x(INT) BETWEEN 0 AND 9 SUCH THAT x <= 9 "
     "MAXIMIZE SUM(x) + AT(PREVIOUS ELSE 0: x) OVER (t)", r"position of a frame"),
    ("absolute frame as the objective", f"SELECT t, x FROM {_R3} DECIDE x(INT) BETWEEN 0 AND 9 SUCH THAT x <= 9 "
     "MAXIMIZE AT(FIRST: x) OVER (t)", r"an objective has none"),
    ("MIN range", _q("MIN(FROM PREVIOUS TO NEXT: x) OVER (t) <= 5"), r"range frame"),
    ("MAX range over data", _q("x <= MAX(FROM PREVIOUS TO NEXT: cap) OVER (t)"), r"range frame"),
    ("AVG range", _q("AVG(FROM PREVIOUS TO NEXT: x) OVER (t) <= 5"), r"range frame"),
    ("reducer in a frame", _q("x <= AT(PREVIOUS ELSE 0: SUM(x)) OVER (t)"), r"frame's body"),
    ("frame in a reducer", _q("PER (): SUM(AT(PREVIOUS: x) OVER (t)) >= 10"), r"inside a reducer"),
    ("ELSE reading a column", _q("x <= AT(PREVIOUS ELSE cap: cap) OVER (t)"), r"ELSE value"),
    ("ABS over a data frame", _q("ABS(AT(PREVIOUS ELSE 0: cap) OVER (t) - 4) >= x"), r"ABS over a frame"),
    ("ABS over a decision frame", _q("ABS(AT(PREVIOUS ELSE 0: x) OVER (t) - x) <= 2"), r"ABS over a frame"),
    ("POWER over a frame", _q("POWER(AT(PREVIOUS ELSE 0: x) OVER (t), 2) <= 16"), r"POWER over a frame"),
    ("frame in WHEN", _q("WHEN AT(PREVIOUS ELSE 0: cap) OVER (t) > 3: x <= 1"), r"cannot filter"),
    ("frame in IF", _q("IF AT(PREVIOUS ELSE 0: x) OVER (t) > 3: x <= 1"), r"cannot guard"),
    ("decision-free frame comparison", _q("AT(PREVIOUS ELSE 9: cap) OVER (t) <= 6 AND x <= 5"), r"decides nothing"),
    ("<> over a frame", _q("AT(PREVIOUS ELSE 5: x) OVER (t) <> 9"), r"not available in that shape"),
    ("distance 0", _q("x <= AT(0 PREVIOUS ELSE 1: cap) OVER (t)"), r"selector distance"),
    ("distance 1000", _q("x <= AT(1000 PREVIOUS ELSE 1: cap) OVER (t)"), r"selector distance"),
    ("EVERY 0", _q("x <= SUM(FROM FIRST TO LAST EVERY 0: cap) OVER (t)"), r"positive step"),
    ("ELSE with ALL", _q("x <= SUM(FROM PREVIOUS TO NEXT ELSE 1 ALL: cap) OVER (t)"), r'at or near "ALL"'),
    ("ALL with ELSE", _q("x <= SUM(FROM PREVIOUS TO NEXT ALL ELSE 1: cap) OVER (t)"), r'at or near "ELSE"'),
    ("frame without OVER", _q("x <= AT(PREVIOUS ELSE 0: cap)"), r"navigates a timeline"),
    ("OVER key is a decision", _q("x <= AT(PREVIOUS: x) OVER (y)", decl=_XY), r"OVER key"),
    ("WITHIN key is a decision", _q("x <= AT(PREVIOUS: cap) OVER (t WITHIN y)", decl=_XY), r"WITHIN partition"),
    ("PER key equal to WITHIN with PREVIOUS", _PER_P.format("AT(PREVIOUS ELSE 0: cap) OVER (t WITHIN p)"),
     r"PREVIOUS / NEXT"),
    ("PER key equal to WITHIN with a FIRST..PREVIOUS range",
     _PER_P.format("SUM(FROM FIRST TO PREVIOUS: cap) OVER (t WITHIN p)"), r"PREVIOUS / NEXT"),
    ("PER () with a relative selector", _q("PER (): SUM(FROM PREVIOUS TO NEXT: x) OVER (t) <= 10"),
     r"position of a frame"),
    ("PER key coarser than WITHIN", "SELECT cat, p, t, y FROM (VALUES ('c', 'A', 1, 5), ('c', 'A', 2, 2), ('c', 'B', 1, 4)) "
     "r(cat, p, t, cap) DECIDE PER cat: y(INT) BETWEEN 0 AND 20 SUCH THAT PER cat: y <= AT(FIRST: cap) OVER (t WITHIN p) "
     "MAXIMIZE SUM(y)", r"partition of a frame"),
]


@pytest.mark.error
@pytest.mark.parametrize("what, sql, topic", _REFUSALS, ids=[r[0] for r in _REFUSALS])
def test_frame_restrictions_are_refused_by_name(decidb_cli, what, sql, topic):
    """§5 (frames appear in constraints only, reduce with SUM only, never inside a
    reducer / WHEN / IF / ABS / POWER, ELSE is a constant, keys are known data, a data
    frame needs a decision), spec §7.3 (distances 1..999, ELSE and ALL exclusive, OVER
    is required), the not-implemented list (`<>` over a frame) and §6.3 (κ -> π; κ ->
    order key for PREVIOUS / NEXT, also as a range endpoint beside FIRST): each is a
    named refusal, never a wrong answer or a crash."""
    decidb_cli.assert_error(sql, match=topic)
