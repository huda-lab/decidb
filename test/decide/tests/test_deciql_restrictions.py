"""The DeciQL restrictions the requirements audit asked to be named, and the shapes it
asked to be closed.

Each rejection here is a spec-conformant or near-conformant spelling the engine does
not formulate; the point of the test is that it is refused by name (never a wrong
answer, an internal error or a crash). The correctness tests pin the shapes the
audit found were mis-built before.
"""

import pytest

from solver.types import ObjSense, SolverStatus, VarType

_ROWS = "(VALUES (1, 5, 3), (2, 7, 6), (3, 2, 9)) t(id, cap, w)"


def _rows(decidb_cli, sql, *cols):
    rows, names = decidb_cli.execute(sql)
    idx = [names.index(c) for c in cols]
    return sorted(tuple(r[i] for i in idx) for r in rows)


# ---------------------------------------------------------------------------
# Compound guards: AND / OR over BOOL decisions are linear guards
# ---------------------------------------------------------------------------

@pytest.mark.var_boolean
@pytest.mark.var_integer
@pytest.mark.correctness
def test_and_guard_is_the_sum_of_its_literals(decidb_cli, oracle_solver):
    """`IF o AND NOT p: x <= 1` is `o - p >= 1 ⟹ x <= 1`."""
    got = _rows(decidb_cli, f"""
        SELECT id, o, p, x FROM {_ROWS}
        DECIDE o(BOOL), p(BOOL), x(INT) BETWEEN 0 AND 9
        SUCH THAT IF o AND NOT p: x <= 1
        MAXIMIZE SUM(x) + SUM(o * 3) - SUM(p * 1)
    """, "id", "o", "p", "x")

    oracle_solver.create_model("and_guard")
    obj = {}
    for i in (1, 2, 3):
        oracle_solver.add_variable(f"o_{i}", VarType.BINARY)
        oracle_solver.add_variable(f"p_{i}", VarType.BINARY)
        oracle_solver.add_variable(f"x_{i}", VarType.INTEGER, lb=0.0, ub=9.0)
        oracle_solver.add_variable(f"y_{i}", VarType.BINARY)
        # y = 0 ⟹ o - p <= 0 ;  y = 1 ⟹ x <= 1   (Big-M rows)
        oracle_solver.add_constraint({f"o_{i}": 1.0, f"p_{i}": -1.0, f"y_{i}": -2.0}, "<=", 0.0)
        oracle_solver.add_constraint({f"x_{i}": 1.0, f"y_{i}": 8.0}, "<=", 9.0)
        obj[f"x_{i}"] = 1.0
        obj[f"o_{i}"] = 3.0
        obj[f"p_{i}"] = -1.0
    oracle_solver.set_objective(obj, ObjSense.MAXIMIZE)
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL

    for _, o, p, x in got:
        if o and not p:
            assert int(x) <= 1
    value = sum(int(x) + 3 * int(o) - int(p) for _, o, p, x in got)
    assert value == pytest.approx(result.objective_value)
    # Taking o (worth 3) and paying for p (cost 1) beats losing 8 of x: o = p = 1.
    assert all(o and p and int(x) == 9 for _, o, p, x in got)


@pytest.mark.var_boolean
@pytest.mark.correctness
def test_or_guard_holds_when_any_literal_does(decidb_cli):
    got = _rows(decidb_cli, f"""
        SELECT id, o, p, x FROM {_ROWS}
        DECIDE o(BOOL), p(BOOL), x(INT) BETWEEN 0 AND 9
        SUCH THAT IF o OR p: x <= 1 AND o + p >= 1
        MAXIMIZE SUM(x)
    """, "id", "o", "p", "x")
    assert all(int(x) <= 1 for _, _, _, x in got)


@pytest.mark.error
@pytest.mark.error_binder
def test_mixed_compound_guard_is_named(decidb_cli):
    decidb_cli.assert_error(f"""
        SELECT id, x FROM {_ROWS}
        DECIDE o(BOOL), x(INT) BETWEEN 0 AND 9
        SUCH THAT IF o AND cap > 5: x <= 1
        MAXIMIZE SUM(x)
    """, match=r"reads known data.*WHEN")


@pytest.mark.error
@pytest.mark.error_binder
def test_guarded_abs_body_is_named(decidb_cli):
    decidb_cli.assert_error(f"""
        SELECT id, x FROM {_ROWS}
        DECIDE o(BOOL), x(INT) BETWEEN 0 AND 9
        SUCH THAT IF o: ABS(x - 2) <= 1
        MAXIMIZE SUM(x)
    """, match=r"an IF guard is supported on linear constraints only; ABS\(\.\.\.\) cannot be guarded yet")


@pytest.mark.var_integer
@pytest.mark.correctness
def test_guard_on_a_semi_decision_has_its_box(decidb_cli):
    """A SEMIINT declaration fixes [0, hi], so a Big-M guard over it needs no extra bound."""
    got = _rows(decidb_cli, f"""
        SELECT id, o, x FROM {_ROWS}
        DECIDE o(BOOL), x(SEMIINT) BETWEEN 3 AND 9
        SUCH THAT IF o: x <= 4 AND SUM(o) >= 2
        MAXIMIZE SUM(x)
    """, "id", "o", "x")
    for _, o, x in got:
        assert int(x) == 0 or 3 <= int(x) <= 9
        if o:
            assert int(x) <= 4


# ---------------------------------------------------------------------------
# Nested reducers and frames
# ---------------------------------------------------------------------------

@pytest.mark.error
@pytest.mark.error_binder
def test_nested_reducers_in_a_constraint_are_named(decidb_cli):
    decidb_cli.assert_error("""
        SELECT id, d, c, ship FROM (VALUES (1,'D1','c1'),(2,'D1','c1'),(3,'D1','c2'),(4,'D2','c3')) t(id, d, c)
        DECIDE ship(INT) BETWEEN 0 AND 9
        SUCH THAT PER d: SUM(PER c: SUM(ship) BY (c)) BY (d) <= 5
        MAXIMIZE SUM(ship)
    """, match=r"Nested reducers are supported in an objective only")


@pytest.mark.error
@pytest.mark.error_binder
def test_frame_inside_a_reducer_is_named(decidb_cli):
    decidb_cli.assert_error("""
        SELECT t, x FROM (VALUES (1),(2),(3)) r(t) DECIDE x(INT) BETWEEN 0 AND 9
        SUCH THAT SUM(x - AT(PREVIOUS ELSE 0: x) OVER (t)) <= 5 MAXIMIZE SUM(x)
    """, match=r"A frame .* inside a reducer is not available yet")


@pytest.mark.error
@pytest.mark.error_binder
def test_reducer_inside_a_frame_is_named(decidb_cli):
    decidb_cli.assert_error("""
        SELECT t, x FROM (VALUES (1),(2),(3)) r(t) DECIDE x(INT) BETWEEN 0 AND 9
        SUCH THAT x <= AT(PREVIOUS ELSE 0: SUM(x)) OVER (t) MAXIMIZE SUM(x)
    """, match=r"a reducer or another frame inside it is not available yet")


@pytest.mark.error
@pytest.mark.error_binder
def test_frame_else_must_be_a_constant(decidb_cli):
    decidb_cli.assert_error(f"""
        SELECT id, x FROM {_ROWS} DECIDE x(INT) BETWEEN 0 AND 9
        SUCH THAT x <= AT(PREVIOUS ELSE cap: x) OVER (id) MAXIMIZE SUM(x)
    """, match=r"A frame's ELSE value is a constant")


@pytest.mark.error
def test_at_over_a_multi_row_position_is_named(decidb_cli):
    decidb_cli.assert_error("""
        SELECT id, x FROM (VALUES (1, 1, 5), (2, 1, 7), (3, 2, 1)) r(id, t, cap)
        DECIDE x(INT) BETWEEN 0 AND 9
        SUCH THAT x <= AT(PREVIOUS ELSE 9: cap) OVER (t) MAXIMIZE SUM(x)
    """, match=r"AT reads one row per position, but a position of its OVER key holds 2 rows")


@pytest.mark.var_integer
@pytest.mark.correctness
def test_data_frame_and_every_against_the_oracle(decidb_cli, oracle_solver):
    """`FROM 4 PREVIOUS TO PREVIOUS EVERY 3` over data, pinned against an explicit model."""
    got = _rows(decidb_cli, """
        SELECT t, x FROM (VALUES (1, 4), (2, 6), (3, 2), (4, 9), (5, 3)) r(t, cap)
        DECIDE x(INT) BETWEEN 0 AND 9
        SUCH THAT x <= SUM(FROM 4 PREVIOUS TO PREVIOUS EVERY 3 ELSE 1: cap) OVER (t)
        MAXIMIZE SUM(x)
    """, "t", "x")
    caps = {1: 4, 2: 6, 3: 2, 4: 9, 5: 3}
    oracle_solver.create_model("every")
    obj = {}
    for t in caps:
        oracle_solver.add_variable(f"x_{t}", VarType.INTEGER, lb=0.0, ub=9.0)
        obj[f"x_{t}"] = 1.0
        positions = [t - 4, t - 1]
        bound = sum(caps[p] if p in caps else 1 for p in positions)
        oracle_solver.add_constraint({f"x_{t}": 1.0}, "<=", float(bound))
    oracle_solver.set_objective(obj, ObjSense.MAXIMIZE)
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL
    assert sum(int(x) for _, x in got) == pytest.approx(result.objective_value)


# ---------------------------------------------------------------------------
# Declarations, keys, objectives
# ---------------------------------------------------------------------------

@pytest.mark.error
@pytest.mark.error_binder
def test_declaration_bound_over_a_decision_is_named(decidb_cli):
    decidb_cli.assert_error(f"""
        SELECT id, x FROM {_ROWS} DECIDE x(INT) <= y, y(INT) BETWEEN 0 AND 5
        SUCH THAT x >= 0 MAXIMIZE SUM(x)
    """, match=r"a declaration bound is a constant or a column; 'y' reads a decision or a reducer")


@pytest.mark.error
@pytest.mark.error_binder
def test_inverted_declaration_bounds_are_named(decidb_cli):
    decidb_cli.assert_error(f"""
        SELECT id, x FROM {_ROWS} DECIDE x(INT) BETWEEN 5 AND 2
        SUCH THAT x >= 0 MAXIMIZE SUM(x)
    """, match=r"BETWEEN 5 AND 2 is empty")


@pytest.mark.error
@pytest.mark.error_binder
def test_by_key_over_a_decision_is_named(decidb_cli):
    decidb_cli.assert_error(f"""
        SELECT id, x FROM {_ROWS} DECIDE o(BOOL), x(INT) BETWEEN 0 AND 9
        SUCH THAT SUM(x) BY (o) <= 3 MAXIMIZE SUM(x)
    """, match=r"'o' is a decision; a key groups known rows")


@pytest.mark.error
@pytest.mark.error_binder
def test_text_decision_in_an_objective_is_named(decidb_cli):
    decidb_cli.assert_error(f"""
        SELECT id, s FROM {_ROWS} DECIDE s(TEXT IN ['a', 'b'])
        SUCH THAT s = 'a' MAXIMIZE SUM(s = 'a')
    """, match=r"A TEXT decision has no numeric value")


@pytest.mark.error
@pytest.mark.error_binder
def test_bare_row_decision_objective_is_named(decidb_cli):
    decidb_cli.assert_error(f"""
        SELECT id, x FROM {_ROWS} DECIDE x(INT) BETWEEN 0 AND 9
        SUCH THAT x >= 0 MAXIMIZE x
    """, match=r"decision 'x' varies across rows, but an objective is generated once")


@pytest.mark.error
@pytest.mark.error_parser
def test_keyed_objective_per_is_named(decidb_cli):
    decidb_cli.assert_error(f"""
        SELECT id, x FROM {_ROWS} DECIDE x(INT) BETWEEN 0 AND 9
        SUCH THAT x >= 0 MAXIMIZE PER id: SUM(x)
    """, match=r"an objective is generated once, so it takes no key")


@pytest.mark.error
@pytest.mark.error_parser
def test_text_without_values_is_named(decidb_cli):
    decidb_cli.assert_error(f"""
        SELECT id, s FROM {_ROWS} DECIDE s(TEXT)
        SUCH THAT s = 'a' SATISFY
    """, match=r"a TEXT decision lists its values")


@pytest.mark.error
def test_unbounded_later_stage_is_named(decidb_cli):
    decidb_cli.assert_error(f"""
        SELECT id, x, y FROM {_ROWS} DECIDE x(INT) BETWEEN 0 AND 9, y(INT)
        SUCH THAT SUM(x) <= 10 MAXIMIZE SUM(x) THEN MAXIMIZE SUM(y)
    """, match=r"THEN stage 2 is unbounded")
