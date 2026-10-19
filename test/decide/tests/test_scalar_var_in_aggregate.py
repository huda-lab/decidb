"""A query-wide (`scalar`) decision as a term of an aggregate constraint.

Canonical form K3 says every term on the left of an aggregate constraint is either
a reducer **or row-invariant** — and names a scalar-scoped variable as the
row-invariant case. The code did not agree: `SUM(x) - s <= K` was rejected with
"aggregate constraint LHS contains a non-aggregate term: s", even though the
objective path had accepted the same term for as long as it existed.

Two things had to change, and they fail differently:

  - ``ExtractAggregateConstraintTerms`` threw. That is a visible rejection.
  - the model builder's aggregate accumulator fans every term out over the group's
    rows, which for a one-column variable multiplies its coefficient by the row
    count. That is a **silent wrong answer**, so these tests pin the coefficient by
    choosing data where `n*s` and `s` give different optima.

The paper's `max_shortfall` shape (§3.1) is the motivating example: a query-wide
slack variable that absorbs whatever an aggregate constraint cannot meet.
"""

import pytest

from solver.types import VarType, ObjSense, SolverStatus


@pytest.mark.var_multi
@pytest.mark.cons_aggregate
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_scalar_slack_absorbs_aggregate_shortfall(decidb_cli):
    """``SUM(x) - s <= 4`` with ``MINIMIZE s`` — the paper's max_shortfall shape.

    Four rows, each x forced to 3 by a lower bound, so SUM(x) = 12 and s must be at
    least 8. If the accumulator fanned `s` out over the 4 rows the row would read
    ``SUM(x) - 4s <= 4``, giving s = 2 — a different, smaller answer that still
    looks perfectly reasonable.
    """
    rows, cols = decidb_cli.execute("""
        SELECT id, x, s FROM (VALUES (1), (2), (3), (4)) t(id)
        DECIDE x(INT), scalar s(INT)
        SUCH THAT x >= 3 AND x <= 3 AND SUM(x) - s <= 4
        MINIMIZE s
    """)
    ci = {c: i for i, c in enumerate(cols)}
    svals = {int(r[ci["s"]]) for r in rows}
    assert svals == {8}, f"expected s = 8 (SUM(x)=12, 12 - s <= 4), got {svals}"


@pytest.mark.var_multi
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_scalar_var_added_to_aggregate(decidb_cli):
    """``SUM(x) + s <= 10`` with ``s`` forced to 4 — leaves SUM(x) <= 6.

    Fanning `s` out over 3 rows would leave ``SUM(x) <= 10 - 12``, i.e. infeasible
    or zero, so the two readings are far apart.
    """
    rows, cols = decidb_cli.execute("""
        SELECT id, x, s FROM (VALUES (1), (2), (3)) t(id)
        DECIDE x(INT), scalar s(INT)
        SUCH THAT x <= 9 AND s >= 4 AND s <= 4 AND SUM(x) + s <= 10
        MAXIMIZE SUM(x)
    """)
    ci = {c: i for i, c in enumerate(cols)}
    total = sum(int(r[ci["x"]]) for r in rows)
    assert total == 6, f"expected SUM(x) = 6, got {total}"


@pytest.mark.var_multi
@pytest.mark.cons_aggregate
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_scalar_slack_oracle_verified(decidb_cli, oracle_solver, perf_tracker):
    """The same shape with a real trade-off, checked against an independent model.

    ``SUM(x * v) - s <= 20`` and ``MINIMIZE 10*s - SUM(x)``: the solver must weigh
    buying more x against the slack it forces. One solver column for `s`, shared
    across every row.
    """
    import time

    data = [(1, 3.0), (2, 5.0), (3, 7.0)]
    t0 = time.perf_counter()
    rows, cols = decidb_cli.execute("""
        SELECT id, v, x, s FROM (VALUES (1, 3.0), (2, 5.0), (3, 7.0)) t(id, v)
        DECIDE x(INT), scalar s(INT)
        SUCH THAT x <= 4 AND s <= 100 AND SUM(x * v) - s <= 20
        MINIMIZE 10 * s - SUM(x)
    """)
    decidb_time = time.perf_counter() - t0

    t_build = time.perf_counter()
    oracle_solver.create_model("scalar_slack")
    names = [f"x_{i}" for i in range(len(data))]
    for n in names:
        oracle_solver.add_variable(n, VarType.INTEGER, lb=0.0, ub=4.0)
    oracle_solver.add_variable("s", VarType.INTEGER, lb=0.0, ub=100.0)
    row = {names[i]: data[i][1] for i in range(len(data))}
    row["s"] = -1.0
    oracle_solver.add_constraint(row, "<=", 20.0, name="agg_with_slack")
    obj = {n: -1.0 for n in names}
    obj["s"] = 10.0
    oracle_solver.set_objective(obj, ObjSense.MINIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL

    ci = {c: i for i, c in enumerate(cols)}
    sval = int(rows[0][ci["s"]])
    decidb_obj = 10.0 * sval - sum(int(r[ci["x"]]) for r in rows)
    assert abs(decidb_obj - result.objective_value) <= 1e-6, (
        f"Objective mismatch: DecidB={decidb_obj}, Oracle={result.objective_value}"
    )
    # `s` is one decision for the whole query, so every output row repeats it.
    assert len({int(r[ci["s"]]) for r in rows}) == 1

    perf_tracker.record(
        "scalar_slack", decidb_time, build_time, result.solve_time_seconds,
        len(data), len(names) + 1, 1, result.objective_value,
        oracle_solver.solver_name(), comparison_status="optimal",
    )


@pytest.mark.var_multi
@pytest.mark.cons_aggregate
@pytest.mark.per_clause
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_scalar_var_in_per_aggregate(decidb_cli):
    """``SUM(x) + s <= 8 PER g`` — the PER accumulator fans out separately from the
    ungrouped one, so it needs the same rule and would otherwise be wrong only for
    grouped constraints.

    Two groups of two rows each, ``s`` pinned to 2: each group gets SUM(x) <= 6.
    Fanning `s` over a group's 2 rows would give SUM(x) <= 4 per group.
    """
    rows, cols = decidb_cli.execute("""
        SELECT id, g, x, s FROM (
            VALUES (1, 'A'), (2, 'A'), (3, 'B'), (4, 'B')
        ) t(id, g)
        DECIDE x(INT), scalar s(INT)
        SUCH THAT x <= 9 AND s >= 2 AND s <= 2 AND SUM(x) + s <= 8 PER g
        MAXIMIZE SUM(x)
    """)
    ci = {c: i for i, c in enumerate(cols)}
    per_group = {}
    for r in rows:
        per_group.setdefault(r[ci["g"]], []).append(int(r[ci["x"]]))
    assert {g: sum(v) for g, v in per_group.items()} == {"A": 6, "B": 6}, (
        f"expected 6 per group, got {per_group}"
    )
