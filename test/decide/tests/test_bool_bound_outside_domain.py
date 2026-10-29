"""A bound on a BOOL decision that lies outside 0 and 1 is Boolean arithmetic.

A BOOL decision only ever takes 0 or 1. A per-row bound with no `when` that lies outside that range does not widen
the variable: one that always holds (`x >= -3`, `x > -3`, `x <= 5`) changes nothing, and one that cannot hold
(`x >= 2`, `x <= -1`, `x < 0`, `x > 1.5`, `x = -1`) makes the problem infeasible, with an error that quotes it. Each
case runs alone and next to an aggregate that would exploit a widened box (`SUM(x) <= -2` is satisfiable by a signed
x, `SUM(x) >= 4` over three rows is satisfiable by x above 1). The independent oracle reads each bound as plain
arithmetic on a binary variable.

Covers:
  - test_bool_bound_outside_domain_matches_oracle: every bound, alone and with each aggregate
  - test_bool_bound_outside_domain_with_when_matches_oracle: the `when` form of the same bounds, as a control
  - test_bool_bound_outside_domain_diagnose_raises_the_same_error: DIAGNOSE raises the same error
  - test_bool_bound_outside_domain_over_empty_source_raises: an empty source raises the same error
"""

import re

import pytest
from decidb_cli import DecidBCliError

from solver.types import ObjSense, SolverStatus, VarType

_ROWS = [(0, 1), (1, 2), (2, 3)]
_SOURCE = "(VALUES (0, 1), (1, 2), (2, 3)) t(id, v)"

# (written bound, oracle sense, oracle rhs). A strict bound on a whole-numbered decision means the first whole value
# past it, so the oracle row for `x > -3` is `x >= -2`.
_BOUNDS = [
    ("x >= -3", ">=", -3.0),
    ("x > -3", ">=", -2.0),
    ("x <= 5", "<=", 5.0),
    ("x >= 2", ">=", 2.0),
    ("x <= -1", "<=", -1.0),
    ("x < 0", "<=", -1.0),
    ("x > 1.5", ">=", 2.0),
    ("x = -1", "=", -1.0),
]

# (written aggregate or None, oracle sense, oracle rhs)
_AGGREGATES = [
    (None, None, None),
    ("SUM(x) <= -2", "<=", -2.0),
    ("SUM(x) >= 4", ">=", 4.0),
    ("SUM(x) >= 1", ">=", 1.0),
]

_INFEASIBLE = r"DECIDE optimization is infeasible\. Prefix the query with DIAGNOSE to see which clause to change\."
#: The bounds that cannot hold on their own: the error quotes the bound rather than the generic infeasible text.
_CANNOT_HOLD = {"x >= 2", "x <= -1", "x < 0", "x > 1.5", "x = -1"}


def _cannot_hold(bound):
    return re.escape(f"DECIDE optimization is infeasible: {bound} cannot hold for a BOOL variable.")


def _solve_oracle(oracle_solver, model_name, bound, aggregate, *, rows=_ROWS):
    oracle_solver.create_model(model_name)
    names = [f"x_{i}" for i in range(len(rows))]
    for name in names:
        oracle_solver.add_variable(name, VarType.BINARY, lb=0.0, ub=1.0)
    _, sense, rhs = bound
    for i, name in enumerate(names):
        oracle_solver.add_constraint({name: 1.0}, sense, rhs, name=f"bound_{i}")
    _, agg_sense, agg_rhs = aggregate
    if agg_sense is not None:
        oracle_solver.add_constraint({name: 1.0 for name in names}, agg_sense, agg_rhs, name="aggregate")
    oracle_solver.set_objective({names[i]: float(rows[i][1]) for i in range(len(rows))}, ObjSense.MINIMIZE)
    return oracle_solver.solve(), names


@pytest.mark.var_boolean
@pytest.mark.cons_perrow
@pytest.mark.cons_mixed
@pytest.mark.error
@pytest.mark.correctness
@pytest.mark.parametrize("aggregate", _AGGREGATES, ids=[a[0] or "alone" for a in _AGGREGATES])
@pytest.mark.parametrize("bound", _BOUNDS, ids=[b[0] for b in _BOUNDS])
def test_bool_bound_outside_domain_matches_oracle(decidb_cli, oracle_solver, bound, aggregate):
    constraints = bound[0] if aggregate[0] is None else f"{bound[0]} AND {aggregate[0]}"
    sql = f"""
        SELECT id, x FROM (
            SELECT * FROM {_SOURCE}
            DECIDE x(BOOL)
            SUCH THAT {constraints}
            MINIMIZE SUM(v * x)
        ) q ORDER BY id
    """
    result, names = _solve_oracle(oracle_solver, "bool_bound_outside_domain", bound, aggregate)
    if result.status == SolverStatus.INFEASIBLE:
        decidb_cli.assert_error(sql, match=_cannot_hold(bound[0]) if bound[0] in _CANNOT_HOLD else _INFEASIBLE)
        return
    assert result.status == SolverStatus.OPTIMAL
    rows, _ = decidb_cli.execute(sql)
    assert [row[0] for row in rows] == [row[0] for row in _ROWS]
    values = [row[1] for row in rows]
    assert all(value in (0, 1) for value in values), values
    assert values == [round(result.variable_values[name]) for name in names], (values, result.variable_values)
    assert sum(_ROWS[i][1] * values[i] for i in range(len(_ROWS))) == pytest.approx(result.objective_value)


@pytest.mark.var_boolean
@pytest.mark.cons_perrow
@pytest.mark.when_row
@pytest.mark.correctness
@pytest.mark.parametrize("bound", _BOUNDS, ids=[b[0] for b in _BOUNDS])
def test_bool_bound_outside_domain_with_when_matches_oracle(decidb_cli, oracle_solver, bound):
    # The same bound under a `when` that holds on every row is plain arithmetic, the control for the unconditional form.
    sql = f"""
        SELECT id, x FROM (
            SELECT * FROM {_SOURCE}
            DECIDE x(BOOL)
            SUCH THAT SUM(x) >= 1 AND when id >= 0: {bound[0]}
            MINIMIZE SUM(v * x)
        ) q ORDER BY id
    """
    result, names = _solve_oracle(oracle_solver, "bool_bound_outside_domain_when", bound, ("SUM(x) >= 1", ">=", 1.0))
    if result.status == SolverStatus.INFEASIBLE:
        decidb_cli.assert_error(sql, match=_INFEASIBLE)
        return
    assert result.status == SolverStatus.OPTIMAL
    rows, _ = decidb_cli.execute(sql)
    values = [row[1] for row in rows]
    assert values == [round(result.variable_values[name]) for name in names], (values, result.variable_values)


@pytest.mark.var_boolean
@pytest.mark.cons_perrow
@pytest.mark.error
def test_bool_bound_outside_domain_diagnose_raises_the_same_error(decidb_cli):
    # A bound against the type's own range is not a clause to loosen: DIAGNOSE raises it as the plain query does.
    sql = f"DIAGNOSE SELECT id, x FROM {_SOURCE} DECIDE x(BOOL) SUCH THAT x >= 2 MINIMIZE SUM(v * x)"
    with pytest.raises(DecidBCliError, match=_cannot_hold("x >= 2")):
        decidb_cli.execute(sql)


@pytest.mark.var_boolean
@pytest.mark.cons_perrow
@pytest.mark.error
@pytest.mark.parametrize("bound", sorted(_CANNOT_HOLD))
def test_bool_bound_outside_domain_over_empty_source_raises(decidb_cli, bound):
    # The solver rejects the bound before it reads a row, so an empty source raises too, with the same text in
    # both modes.
    sql = f"""
        SELECT id, x FROM (
            SELECT * FROM {_SOURCE} WHERE false
            DECIDE x(BOOL) SUCH THAT {bound} MINIMIZE SUM(v * x)
        ) q
    """
    messages = {}
    for mode in ("off", "auto"):
        with pytest.raises(DecidBCliError, match=_cannot_hold(bound)) as error:
            decidb_cli.execute(f"SET decide_direct_solve='{mode}'; {sql}")
        messages[mode] = str(error.value)
    assert messages["off"] == messages["auto"]


@pytest.mark.var_boolean
@pytest.mark.cons_perrow
@pytest.mark.error
@pytest.mark.parametrize("bound", ["x >= (SELECT 2)"])
def test_bool_bound_outside_domain_unfolded_spelling_is_read_per_row(decidb_cli, monkeypatch, bound):
    # A bound is folded into the variable's box by value (D41), so arithmetic no longer keeps it out; a scalar subquery
    # does, being no constant at plan time. S1's direct solve then reads it per row, finds no value of x allowed, and
    # raises the infeasible error the solver raises. `require`
    # proves direct solve applied, because a declined query would raise the "did not apply" error instead. A forced
    # solver backend rejects `require`, so the run that forces one leaves it unset here.
    monkeypatch.delenv("DECIDB_FORCE_SOLVER", raising=False)
    sql = f"""
        SELECT id, x FROM (
            SELECT * FROM {_SOURCE}
            DECIDE x(BOOL) SUCH THAT {bound} AND SUM(x) <= 2 MINIMIZE SUM(v * x)
        ) q
    """
    messages = {}
    for mode in ("off", "require"):
        with pytest.raises(DecidBCliError, match=_INFEASIBLE) as error:
            decidb_cli.execute(f"SET decide_direct_solve='{mode}'; {sql}")
        messages[mode] = str(error.value)
    assert messages["off"] == messages["require"]
