"""The constraint gate no longer cares which side the decision was written on.

the canonicalization refactor deleted the last of the five duplicate shape-deciding sites:
a parsed-level flip in ``DecideConstraintsBinder::BindComparison`` that swapped
``5 >= x`` into ``x <= 5`` because everything downstream demanded the DECIDE
expression on the left. ``DecideCanonicalizer`` makes the same swap on the bound
tree (K1), so the binder's job shrank to a question that has no side in it: does
*either* side bear a decision?

Two things follow, and this file pins both.

**Shapes that open.** A decision may now sit on the bound side. That is the
paper's §3.1 constraint (``demand - sum(ship) <= max_shortfall``), and it also
admits aggregate-against-aggregate, which no single-sided gate could express.
Nothing new happens below the binder: the canonicalizer moves the term left and
the physical layer sees the reduced shape B.3/B.5 already built.

**A row-varying *data* bound now opens too (C1).** `IsAllowedDecisionFreeBoundExpression`
gained a `COLUMN_REF` case, so a bare column is a legal bound of a reduced constraint —
this is what the paper's own running example needs (`SUM(ship) <= stock PER depotID`).
The conjunction `LHS ⊙ rᵢ` over every row is exactly the tightest one (MIN for `<=`, MAX
for `>=`), applied per `PER` group when one is present; `=` still refuses a bound that
genuinely varies (it would be a contradiction, not a tightening), and `<>` keeps every
excluded value instead of collapsing. See `test_reduced_bound_data_column.py` for the
PER and PER+WHEN cases (Figure 1's own two constraints); this file keeps the no-PER,
side-agnostic case.

Every positive case is checked against an independent oracle model. "It no longer
errors" is not the claim; the claim is that the model DecidB builds for a reversed
constraint is the model the written-forward one describes.

Covers:
  - test_aggregate_vs_aggregate: SUM(x*v) <= SUM(y*v)
  - test_scalar_decision_as_reversed_bound: cap >= SUM(x) matches SUM(x) <= cap
  - test_data_term_left_of_reducer: paper §3.1's demand - SUM(x) <= cap
  - test_composed_reducer_as_bound: SUM(x*v) <= MAX(x*w) + K
  - test_reversed_per_row_bound: 5 >= x, no binder flip involved
  - test_correlated_subquery_bound_on_either_side: correlated row bounds retain
    their per-row meaning through a side swap
  - test_row_varying_bound_collapses_to_tightest_on_either_side: C1, the no-PER case
  - test_row_scoped_decision_as_aggregate_bound_rejected: K3, named and actionable
"""

import pytest

from solver.types import VarType, ObjSense, SolverStatus
from ._oracle_helpers import emit_hard_inner_max


@pytest.mark.cons_aggregate
@pytest.mark.var_multi
@pytest.mark.correctness
def test_aggregate_vs_aggregate(decidb_cli, duckdb_conn, oracle_solver):
    """``SUM(x * val) <= SUM(y * val)`` — a reducer on both sides.

    Refused before C.2 because the RHS validator rejected any bound containing a
    decision variable, which is exactly what the right-hand reducer is. There is
    no new machinery behind it: canonicalization subtracts the two into
    ``SUM(x*val) - SUM(y*val) <= 0``, the shape Phase A already made walkable.

    The instance discriminates against the two ways to get this wrong. ``y`` is
    capped at 5 units, so the x-budget is bounded by what y buys; dropping the
    right-hand reducer entirely would leave ``SUM(x*val) <= 0`` and answer 0,
    while losing its sign would leave the x-budget unbounded and answer at the
    ``x <= 8`` ceiling.
    """
    sql = """
        SELECT l_orderkey, l_linenumber, l_quantity, x, y
        FROM lineitem WHERE l_orderkey <= 3
        DECIDE x(INT), y(INT)
        SUCH THAT SUM(x * l_quantity) <= SUM(y * l_quantity)
              AND SUM(y) <= 5 AND x <= 8 AND y <= 8
        MAXIMIZE SUM(x * l_quantity)
    """
    rows, cols = decidb_cli.execute(sql)

    data = duckdb_conn.execute("""
        SELECT CAST(l_quantity AS DOUBLE) FROM lineitem WHERE l_orderkey <= 3
    """).fetchall()
    n = len(data)
    assert n > 1 and len(rows) == n

    oracle_solver.create_model("aggregate_vs_aggregate")
    budget, obj, y_budget = {}, {}, {}
    for i in range(n):
        qty = data[i][0]
        oracle_solver.add_variable(f"x_{i}", VarType.INTEGER, lb=0.0, ub=8.0)
        oracle_solver.add_variable(f"y_{i}", VarType.INTEGER, lb=0.0, ub=8.0)
        budget[f"x_{i}"] = qty
        budget[f"y_{i}"] = -qty
        y_budget[f"y_{i}"] = 1.0
        obj[f"x_{i}"] = qty
    oracle_solver.add_constraint(budget, "<=", 0.0, name="x_le_y")
    oracle_solver.add_constraint(y_budget, "<=", 5.0, name="y_budget")
    oracle_solver.set_objective(obj, ObjSense.MAXIMIZE)
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL

    xi, yi, qi = cols.index("x"), cols.index("y"), cols.index("l_quantity")
    lhs = sum(int(r[xi]) * float(r[qi]) for r in rows)
    rhs = sum(int(r[yi]) * float(r[qi]) for r in rows)
    assert lhs <= rhs + 1e-6, f"constraint violated: {lhs} > {rhs}"
    assert abs(lhs - result.objective_value) < 1e-4, \
        f"Objective mismatch: DecidB={lhs}, Oracle={result.objective_value}"


@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_scalar_decision_as_reversed_bound(decidb_cli):
    """``cap >= SUM(x)`` and ``SUM(x) <= cap`` are the same constraint.

    This is the pair the deleted flip existed to collapse, now collapsed one stage
    later and on the bound tree. Comparing the two *queries* rather than either
    one against a constant is the point: a canonicalizer that flipped the relation
    without flipping the sign would still solve, and would still look plausible on
    its own.
    """
    common = """
        SELECT c_custkey, x, cap
        FROM customer WHERE c_custkey <= 20
        DECIDE x(INT), scalar cap(INT)
        SUCH THAT {constraint} AND cap <= 12 AND x <= 12
        MAXIMIZE 3 * SUM(x) - 2 * cap
    """
    forward, f_cols = decidb_cli.execute(common.format(constraint="SUM(x) <= cap"))
    reversed_, r_cols = decidb_cli.execute(common.format(constraint="cap >= SUM(x)"))

    assert f_cols == r_cols
    fx, fc = f_cols.index("x"), f_cols.index("cap")
    f_obj = 3 * sum(int(r[fx]) for r in forward) - 2 * int(forward[0][fc])
    r_obj = 3 * sum(int(r[fx]) for r in reversed_) - 2 * int(reversed_[0][fc])
    assert f_obj == r_obj, f"reversed form disagrees: {f_obj} vs {r_obj}"
    # ... and it is the right answer, not merely a consistent one. A unit of cap
    # unlocks a unit of SUM(x) worth 3 and costs 2, so cap is pushed to its own
    # ceiling: cap = 12, SUM(x) = 12, objective 36 - 24.
    assert f_obj == 12, f"expected the interior optimum 12, got {f_obj}"


@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_data_term_left_of_reducer(decidb_cli, duckdb_conn, oracle_solver):
    """The paper's §3.1 shape: ``demand - SUM(x) <= cap`` with a query-wide cap.

    Both halves of C.2 are load-bearing here. The decision-bearing bound (``cap``)
    is what the binder used to refuse; the row-varying data term (``demand``) is
    what canonicalization sends to the *other* side, where B.5's runtime reduction
    collapses the per-tuple family of bounds to the tightest one.

    ``<=`` takes the MIN of the canonical right-hand side, so with the constraint
    rearranged to ``-SUM(x) - cap <= -demand`` the binding row is the one with the
    LARGEST demand. Reading the rule against the user's original ``demand`` instead
    is a sign bug that picks the smallest, and the oracle below is built to the
    per-tuple conjunction rather than to either reduction, so it catches that.
    """
    sql = """
        SELECT l_orderkey, l_linenumber, l_quantity, x, cap
        FROM lineitem WHERE l_orderkey <= 3
        DECIDE x(INT), scalar cap(INT)
        SUCH THAT l_quantity - SUM(x) <= cap AND x <= 4
        MINIMIZE cap
    """
    rows, cols = decidb_cli.execute(sql)

    data = duckdb_conn.execute("""
        SELECT CAST(l_quantity AS DOUBLE) FROM lineitem WHERE l_orderkey <= 3
    """).fetchall()
    n = len(data)
    assert n > 1
    assert len({row[0] for row in data}) > 1, "needs varying demand to discriminate"

    oracle_solver.create_model("data_term_left_of_reducer")
    oracle_solver.add_variable("cap", VarType.CONTINUOUS, lb=0.0, ub=1000.0)
    for i in range(n):
        oracle_solver.add_variable(f"x_{i}", VarType.INTEGER, lb=0.0, ub=4.0)
    # One instance per tuple, exactly as paper 3.2.1 generates them.
    for i in range(n):
        row = {f"x_{j}": -1.0 for j in range(n)}
        row["cap"] = -1.0
        oracle_solver.add_constraint(row, "<=", -data[i][0], name=f"tuple_{i}")
    oracle_solver.set_objective({"cap": 1.0}, ObjSense.MINIMIZE)
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL

    ci, xi = cols.index("cap"), cols.index("x")
    cap_value = float(rows[0][ci])
    assert all(abs(float(r[ci]) - cap_value) < 1e-6 for r in rows), "cap is not query-wide"
    total_x = sum(int(r[xi]) for r in rows)
    for i in range(n):
        assert data[i][0] - total_x <= cap_value + 1e-6, \
            f"tuple {i} violated: {data[i][0]} - {total_x} > {cap_value}"
    assert abs(cap_value - result.objective_value) < 1e-4, \
        f"Objective mismatch: DecidB={cap_value}, Oracle={result.objective_value}"


@pytest.mark.cons_aggregate
@pytest.mark.min_max
@pytest.mark.correctness
def test_composed_reducer_as_bound(decidb_cli, duckdb_conn, oracle_solver):
    """``SUM(x * v) <= MAX(x * w) + K`` — a decision-bearing MAX as the bound.

    Phase A made the composed MIN/MAX walker sign-aware, which is what lets the
    migrated ``- MAX(x*w)`` term be classified rather than thrown on; C.2 is what
    lets the shape reach it at all. The reduction direction matters: pushed to the
    right of ``<=`` the MAX is being raised, so it takes the hard (indicator)
    encoding, and the oracle below mirrors that rather than the easy per-row one.
    """
    sql = """
        SELECT l_orderkey, l_linenumber, l_quantity, l_discount, x
        FROM lineitem WHERE l_orderkey <= 3
        DECIDE x(BOOL)
        SUCH THAT SUM(x * l_quantity) <= MAX(x * l_quantity) + 30
        MAXIMIZE SUM(x * l_quantity)
    """
    rows, cols = decidb_cli.execute(sql)

    data = duckdb_conn.execute("""
        SELECT CAST(l_quantity AS DOUBLE) FROM lineitem WHERE l_orderkey <= 3
    """).fetchall()
    n = len(data)
    assert n > 1

    oracle_solver.create_model("composed_reducer_as_bound")
    obj, budget = {}, {}
    row_coeffs = []
    for i in range(n):
        qty = data[i][0]
        oracle_solver.add_variable(f"x_{i}", VarType.BINARY)
        obj[f"x_{i}"] = qty
        budget[f"x_{i}"] = qty
        row_coeffs.append({f"x_{i}": qty})
    z = emit_hard_inner_max(
        oracle_solver, "cmax", row_coeffs, max(r[0] for r in data),
    )
    budget[z] = -1.0
    oracle_solver.add_constraint(budget, "<=", 30.0, name="sum_le_max")
    oracle_solver.set_objective(obj, ObjSense.MAXIMIZE)
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL

    xi, qi = cols.index("x"), cols.index("l_quantity")
    picked = [float(r[qi]) for r in rows if int(r[xi]) == 1]
    total = sum(picked)
    assert total <= (max(picked) if picked else 0.0) + 30.0 + 1e-6, \
        f"constraint violated: SUM={total}"
    assert abs(total - result.objective_value) < 1e-4, \
        f"Objective mismatch: DecidB={total}, Oracle={result.objective_value}"


@pytest.mark.cons_perrow
@pytest.mark.correctness
def test_reversed_per_row_bound(decidb_cli):
    """``5 >= x`` still binds, and now does so without a parsed-level rewrite.

    Kept as a regression pin rather than for its answer: this is the shape the
    deleted flip was originally written for, and it is the one that would fail
    silently (as an unbounded or vacuous row) if the canonicalizer's own mirror
    branch stopped flipping the relation with the sides.
    """
    rows, cols = decidb_cli.execute("""
        SELECT c_custkey, x FROM customer WHERE c_custkey <= 10
        DECIDE x(INT) SUCH THAT 5 >= x MAXIMIZE SUM(x)
    """)
    assert len(rows) > 1
    xi = cols.index("x")
    assert all(int(r[xi]) == 5 for r in rows), rows


@pytest.mark.cons_perrow
@pytest.mark.cons_subquery
@pytest.mark.correctness
def test_correlated_subquery_bound_on_either_side(decidb_cli):
    """A correlated scalar subquery remains row-varying in both orientations.

    The forward and reversed spellings must build the same per-row caps.  Using
    distinct caps makes accidental promotion to one query-wide value visible.
    """
    common = """
        SELECT id, hi, x
        FROM (VALUES (1, 2), (2, 4)) t(id, hi)
        DECIDE x(INT)
        SUCH THAT {constraint}
        MAXIMIZE SUM(x)
    """
    forward, f_cols = decidb_cli.execute(
        common.format(constraint="x <= (SELECT hi)")
    )
    reversed_, r_cols = decidb_cli.execute(
        common.format(constraint="(SELECT hi) >= x")
    )

    assert f_cols == r_cols
    ii, hi, xi = (f_cols.index(name) for name in ("id", "hi", "x"))
    f_values = [(int(r[ii]), int(r[hi]), int(r[xi])) for r in forward]
    r_values = [(int(r[ii]), int(r[hi]), int(r[xi])) for r in reversed_]
    assert f_values == r_values == [(1, 2, 2), (2, 4, 4)]


@pytest.mark.cons_perrow
@pytest.mark.correctness
def test_row_varying_bound_collapses_to_tightest_on_either_side(
    decidb_cli, oracle_solver
):
    """C1: a bare data column is now a legal bound of a reduced constraint.

    Without PER there is one group — the whole selection — so `SUM(x) <= cap`
    is the conjunction of `SUM(x) <= cap_i` over every row, which is exactly
    `SUM(x) <= MIN(cap_i)`. Verified against an independently built model (three
    redundant per-row bounds, exactly as the conjunction reads), not a
    hand-computed MIN — Gurobi finds the tightest one itself.

    Reversing the comparison (`cap >= SUM(x)`) must build the identical model:
    the bound rule runs on whichever side is the bound, not on `right` by
    position.
    """
    caps = {1: 4, 2: 9, 3: 6}
    fixture = "(VALUES (1, 4), (2, 9), (3, 6)) t(id, cap)"

    for label, constraint in (
        ("forward", "SUM(x) <= cap"), ("reversed", "cap >= SUM(x)")
    ):
        rows, cols = decidb_cli.execute(f"""
            SELECT id, x FROM {fixture}
            DECIDE x(INT)
            SUCH THAT {constraint}
            MAXIMIZE SUM(x)
        """)
        xi = cols.index("x")
        actual_sum = sum(int(r[xi]) for r in rows)

        oracle_solver.create_model(f"row_varying_bound_{label}")
        for i in caps:
            oracle_solver.add_variable(f"x_{i}", VarType.INTEGER, lb=0.0)
        for i in caps:
            oracle_solver.add_constraint(
                {f"x_{j}": 1.0 for j in caps}, "<=", float(caps[i]), name=f"row_{i}_bound"
            )
        oracle_solver.set_objective({f"x_{i}": 1.0 for i in caps}, ObjSense.MAXIMIZE)
        result = oracle_solver.solve()
        assert result.status == SolverStatus.OPTIMAL
        assert actual_sum == pytest.approx(result.objective_value)


@pytest.mark.error
def test_row_scoped_decision_as_aggregate_bound_rejected(decidb_cli):
    """``SUM(x) <= y`` with a row-scoped ``y`` is K3, and is named as such.

    A query-wide decision is a legal term of a reduced constraint because it is
    row-invariant; a row-scoped one is not — there is no single ``y`` for a number
    that has no row. C.2 moved this from a binder rejection to the homogeneity
    validation at the canonicalization boundary.  The ``Binder Error`` prefix
    pins that ownership: physical extraction must not be the first stage to
    discover the unsupported mixture.
    """
    decidb_cli.assert_error("""
        SELECT c_custkey, x, y FROM customer WHERE c_custkey <= 10
        DECIDE x(INT), y(INT)
        SUCH THAT SUM(x) <= y AND y <= 3
        MAXIMIZE SUM(x)
    """, match=r"Binder Error: DECIDE constraint.*row-scoped.*'y'.*outside.*(?:reducer|SUM)")
