"""Edge-case tests for DECIDE.

Covers boundary conditions that other test files don't exercise:
  - Single-row input (trivial solve)
  - Very loose constraint (all variables selected)
  - Constraint RHS = 0 (forces all decision vars to 0)
  - Negative objective coefficients
  - Zero rows (empty result after WHERE) — returns empty result like SQL
  - Feasibility problem (no MAXIMIZE/MINIMIZE) — xfail, grammar support pending
  - NULL values in coefficient columns — error: NULLs rejected with COALESCE hint
"""

import time

import pytest

from solver.types import VarType, ObjSense
from comparison.compare import compare_solutions


@pytest.mark.edge_case
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_single_row(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Degenerate case: only 1 input row. Trivial knapsack."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity, x
        FROM lineitem
        WHERE l_orderkey = 1 AND l_linenumber = 1
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * l_quantity) <= 100
        MAXIMIZE SUM(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0
    assert len(decidb_result) == 1, f"Expected 1 row, got {len(decidb_result)}"

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_extendedprice AS DOUBLE),
               CAST(l_quantity AS DOUBLE)
        FROM lineitem WHERE l_orderkey = 1 AND l_linenumber = 1
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("single_row")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    oracle_solver.add_constraint(
        {vnames[i]: data[i][3] for i in range(len(data))},
        "<=", 100.0, name="capacity",
    )
    oracle_solver.set_objective(
        {vnames[i]: data[i][2] for i in range(len(data))},
        ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_result, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[decidb_cols.index("l_extendedprice")])},
    )

    perf_tracker.record(
        "single_row", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.edge_case
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_trivial_all_selected(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Constraint so loose every x=1 is feasible. Optimal is all-ones."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity, x
        FROM lineitem
        WHERE l_orderkey < 20
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * l_quantity) <= 999999
        MAXIMIZE SUM(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_extendedprice AS DOUBLE),
               CAST(l_quantity AS DOUBLE)
        FROM lineitem WHERE l_orderkey < 20
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("trivial_all")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    oracle_solver.add_constraint(
        {vnames[i]: data[i][3] for i in range(len(data))},
        "<=", 999999.0, name="capacity",
    )
    oracle_solver.set_objective(
        {vnames[i]: data[i][2] for i in range(len(data))},
        ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_result, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[decidb_cols.index("l_extendedprice")])},
    )
    assert all(v == 1.0 for v in cmp.decidb_vector), "Expected all x=1"

    perf_tracker.record(
        "trivial_all_selected", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.edge_case
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_rhs_zero_forces_all_zero(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """SUM(x) <= 0 forces all boolean variables to 0."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, x
        FROM lineitem
        WHERE l_orderkey < 20
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) <= 0
        MAXIMIZE SUM(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_extendedprice AS DOUBLE)
        FROM lineitem WHERE l_orderkey < 20
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("rhs_zero")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    oracle_solver.add_constraint(
        {vnames[i]: 1.0 for i in range(len(data))},
        "<=", 0.0, name="zero_budget",
    )
    oracle_solver.set_objective(
        {vnames[i]: data[i][2] for i in range(len(data))},
        ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_result, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[decidb_cols.index("l_extendedprice")])},
    )
    assert all(v == 0.0 for v in cmp.decidb_vector), "Expected all x=0"
    assert cmp.decidb_objective == 0.0

    perf_tracker.record(
        "rhs_zero", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.edge_case
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_negative_objective_coefficients(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Negative account balances as coefficients — solver must pick most negative."""
    sql = """
        SELECT c_custkey, c_acctbal, x
        FROM customer
        WHERE c_nationkey <= 3
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) >= 5
        MINIMIZE SUM(x * c_acctbal)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(c_custkey AS BIGINT),
               CAST(c_acctbal AS DOUBLE)
        FROM customer WHERE c_nationkey <= 3
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("neg_coeffs")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    oracle_solver.add_constraint(
        {vnames[i]: 1.0 for i in range(len(data))},
        ">=", 5.0, name="min_count",
    )
    oracle_solver.set_objective(
        {vnames[i]: data[i][1] for i in range(len(data))},
        ObjSense.MINIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_result, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[decidb_cols.index("c_acctbal")])},
    )

    perf_tracker.record(
        "neg_coeffs", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.edge_case
def test_zero_rows_empty_input(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """DECIDE on empty result set should return empty results, like standard SQL."""
    sql = """
        SELECT l_orderkey, l_linenumber, x
        FROM lineitem
        WHERE l_orderkey < 0
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) <= 5
        MAXIMIZE SUM(x * l_extendedprice)
    """
    result, cols = decidb_cli.execute(sql)
    assert len(result) == 0


@pytest.mark.edge_case
@pytest.mark.when_constraint
@pytest.mark.avg_rewrite
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.error_infeasible
def test_avg_constraint_when_filters_all_rows(decidb_cli):
    """AVG(...) WHEN <cond> where no row matches — now rejected pre-solver.
    AVG(∅) is undefined (divide by zero); caught by the empty-aggregate guard."""
    sql = """
        SELECT id, val, flag, x FROM (
            VALUES (1, 10.0, 'A'),
                   (2, 7.0, 'A'),
                   (3, 5.0, 'B'),
                   (4, 12.0, 'B')
        ) t(id, val, flag)
        DECIDE x IS BOOLEAN
        SUCH THAT AVG(x * val) WHEN (flag = 'Z') <= 1
        MAXIMIZE SUM(x * val)
    """
    decidb_cli.assert_error(sql, match=r"empty|WHEN")


@pytest.mark.edge_case
@pytest.mark.per_clause
@pytest.mark.when_constraint
@pytest.mark.min_max
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_maximize_sum_max_per_with_empty_when_group(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """MAXIMIZE SUM(MAX(x*v)) WHEN flag PER grp where one group has no
    WHEN-matching rows — hard-case inner-MAX with an empty group.

    The hard case emits a per-row binary indicator ``y_i`` plus
    ``SUM(y_i) >= 1`` to pin the auxiliary ``z_g`` to a row's value. An
    empty WHEN-bucket in a group would naively produce ``0 >= 1``
    (spurious infeasibility). Per the documented default PER policy
    (empty groups skipped), group B should contribute nothing and the
    objective should reflect only group A.
    """
    from ._oracle_helpers import emit_hard_inner_max

    sql = """
        SELECT id, grp, val, flag, x FROM (
            VALUES (1, 'A', 10.0, true),
                   (2, 'A', 7.0, true),
                   (3, 'A', 4.0, false),
                   (4, 'B', 12.0, false),
                   (5, 'B', 6.0, false)
        ) t(id, grp, val, flag)
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) >= 1 PER grp
        MAXIMIZE SUM(MAX(x * val)) WHEN flag PER grp
    """
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = [
        (1, 'A', 10.0, True),
        (2, 'A', 7.0, True),
        (3, 'A', 4.0, False),
        (4, 'B', 12.0, False),
        (5, 'B', 6.0, False),
    ]
    n = len(data)

    t_build = time.perf_counter()
    oracle_solver.create_model("max_sum_max_per_empty_when_group")
    vnames = [f"x_{i}" for i in range(n)]
    for v in vnames:
        oracle_solver.add_variable(v, VarType.BINARY)

    groups: dict = {}
    for i, row in enumerate(data):
        groups.setdefault(row[1], []).append(i)

    # SUM(x) >= 1 PER grp: applied to every group.
    for g, idxs in groups.items():
        oracle_solver.add_constraint(
            {vnames[i]: 1.0 for i in idxs}, ">=", 1.0, name=f"sum_ge_{g}",
        )

    # Hard-case inner-MAX aux per non-empty WHEN group.
    z_names = []
    for g, idxs in groups.items():
        qualifying = [i for i in idxs if data[i][3]]
        if not qualifying:
            # Empty WHEN-bucket — default PER policy skips this group.
            continue
        z = emit_hard_inner_max(
            oracle_solver,
            name_prefix=f"mx_{g}",
            row_coeffs=[{vnames[i]: data[i][2]} for i in qualifying],
            row_ub=max(data[i][2] for i in qualifying) + 1.0,
        )
        z_names.append(z)
    oracle_solver.set_objective({z: 1.0 for z in z_names}, ObjSense.MAXIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    from solver.types import SolverStatus
    assert result.status == SolverStatus.OPTIMAL, (
        f"Oracle expected OPTIMAL; got {result.status}. "
        f"DecidB returned {len(decidb_rows)} rows."
    )

    def decidb_obj(rs, cs):
        xi = cs.index("x"); vi = cs.index("val"); fi = cs.index("flag"); gi = cs.index("grp")
        per_grp: dict = {}
        for r in rs:
            if not bool(r[fi]):
                continue
            key = r[gi]
            contrib = float(r[vi]) if float(r[xi]) > 0.5 else 0.0
            per_grp.setdefault(key, []).append(contrib)
        return sum(max(vs) for vs in per_grp.values() if vs)

    cmp = compare_solutions(
        decidb_rows, decidb_cols, result, data, ["x"],
        decidb_objective_fn=decidb_obj,
    )
    perf_tracker.record(
        "max_sum_max_per_empty_when_group", decidb_time, build_time,
        result.solve_time_seconds, n, n + len(z_names), len(groups),
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


# ---------------------------------------------------------------------------
# Empty-set aggregate probe matrix: document concrete behavior for every
# combination of {aggregate kind} × {position} × {empty origin}.
#
# Expected contract (from code trace):
#   - Empty aggregate in a CONSTRAINT → constraint is skipped (trivially true).
#     Holds for SUM, AVG, MIN, MAX; for both flat-WHEN-empty and PER-empty.
#   - Empty aggregate in an OBJECTIVE → contributes 0 to a linear/AVG obj,
#     or the empty group is dropped from a nested PER sum. For FLAT MIN/MAX
#     objectives over a completely-empty WHEN bucket, the auxiliary z has no
#     linking constraints and collapses to its bound — documented here.
# ---------------------------------------------------------------------------


@pytest.mark.edge_case
@pytest.mark.min_max
@pytest.mark.when_constraint
@pytest.mark.error_infeasible
def test_min_geq_constraint_when_empty(decidb_cli):
    """MIN(...) >= K WHEN <never> — easy case.

    DecidB rejects empty aggregate sets before reaching the solver. An empty
    MIN/MAX has no well-defined value (MIN(∅) = +∞, MAX(∅) = −∞); per the
    project decision to "reject all cases of an empty set," even the easy
    direction (previously trivially-satisfied) now raises.
    """
    sql = """
        SELECT id, val, x FROM (
            VALUES (1, 10.0), (2, 7.0), (3, 4.0)
        ) t(id, val)
        DECIDE x IS BOOLEAN
        SUCH THAT MIN(x * val) >= 5 WHEN val > 100
        MAXIMIZE SUM(x * val)
    """
    decidb_cli.assert_error(sql, match=_EMPTY_WHEN_ERROR_REGEX)


@pytest.mark.edge_case
@pytest.mark.min_max
@pytest.mark.when_constraint
@pytest.mark.error_infeasible
def test_min_leq_constraint_when_empty(decidb_cli):
    """MIN(...) <= K WHEN <never> — hard case, now rejected pre-solver."""
    sql = """
        SELECT id, val, x FROM (
            VALUES (1, 10.0), (2, 7.0), (3, 4.0)
        ) t(id, val)
        DECIDE x IS BOOLEAN
        SUCH THAT MIN(x * val) <= 3 WHEN val > 100
        MAXIMIZE SUM(x * val)
    """
    decidb_cli.assert_error(sql, match=_EMPTY_WHEN_ERROR_REGEX)


@pytest.mark.edge_case
@pytest.mark.min_max
@pytest.mark.when_constraint
@pytest.mark.error_infeasible
def test_max_leq_constraint_when_empty(decidb_cli):
    """MAX(...) <= K WHEN <never> — easy case, now rejected pre-solver."""
    sql = """
        SELECT id, val, x FROM (
            VALUES (1, 10.0), (2, 7.0), (3, 4.0)
        ) t(id, val)
        DECIDE x IS BOOLEAN
        SUCH THAT MAX(x * val) <= 2 WHEN val > 100
        MAXIMIZE SUM(x * val)
    """
    decidb_cli.assert_error(sql, match=_EMPTY_WHEN_ERROR_REGEX)


@pytest.mark.edge_case
@pytest.mark.min_max
@pytest.mark.when_constraint
@pytest.mark.error_infeasible
def test_max_geq_constraint_when_empty(decidb_cli):
    """MAX(...) >= K WHEN <never> — hard case, now rejected pre-solver."""
    sql = """
        SELECT id, val, x FROM (
            VALUES (1, 10.0), (2, 7.0), (3, 4.0)
        ) t(id, val)
        DECIDE x IS BOOLEAN
        SUCH THAT MAX(x * val) >= 999 WHEN val > 100
        MAXIMIZE SUM(x * val)
    """
    decidb_cli.assert_error(sql, match=_EMPTY_WHEN_ERROR_REGEX)


@pytest.mark.edge_case
@pytest.mark.avg_rewrite
@pytest.mark.per_clause
@pytest.mark.when_constraint
@pytest.mark.correctness
def test_avg_per_constraint_with_empty_group(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """AVG(...) <= K PER grp WHEN <flag> where one group has no matching rows.

    Per-group AVG zeroes coefficients for empty groups (physical_decide.cpp
    :2004), and the per-group constraint should reduce to 0 <= K (trivially
    true) for that group. Group with matches enforces its AVG cap.
    """
    sql = """
        SELECT id, grp, val, flag, x FROM (
            VALUES (1, 'A', 10.0, true),
                   (2, 'A', 20.0, true),
                   (3, 'A', 5.0, true),
                   (4, 'B', 50.0, false),
                   (5, 'B', 100.0, false)
        ) t(id, grp, val, flag)
        DECIDE x IS BOOLEAN
        SUCH THAT AVG(x * val) WHEN flag <= 8 PER grp
        MAXIMIZE SUM(x * val)
    """
    rows, cols = decidb_cli.execute(sql)
    x_idx, grp_idx, val_idx = cols.index("x"), cols.index("grp"), cols.index("val")

    # Group B is empty after WHEN → constraint skipped → both B rows selected.
    b_rows = [r for r in rows if r[grp_idx] == 'B']
    assert all(int(r[x_idx]) == 1 for r in b_rows), (
        f"Group B (empty after WHEN) should have no constraint; expected "
        f"all x=1, got {[r[x_idx] for r in b_rows]}"
    )

    # Group A enforces AVG(x*val) <= 8, i.e. SUM(x*val over flag rows) <= 8 * 3 = 24.
    a_rows = [r for r in rows if r[grp_idx] == 'A']
    a_when_sum = sum(float(r[val_idx]) * int(r[x_idx]) for r in a_rows)
    assert a_when_sum <= 24.0 + 1e-6, (
        f"Group A AVG cap violated: SUM={a_when_sum} > 24"
    )


_EMPTY_WHEN_ERROR_REGEX = r"infeasible|empty|WHEN|MIN|MAX"


@pytest.mark.edge_case
@pytest.mark.min_max
@pytest.mark.when_objective
@pytest.mark.error_infeasible
def test_maximize_min_objective_when_empty(decidb_cli):
    """MAXIMIZE MIN(...) WHEN <never> — easy-case flat MIN over empty.

    Formulation: global z with per-row `z <= expr_i` for WHEN-matching rows.
    Zero rows → no linking constraints → z is unbounded upward, constrained
    only by its declared upper bound. ILP semantics: MIN(∅) = +∞,
    so MAXIMIZE drives z to +∞ → unbounded → infeasibility/error.
    """
    sql = """
        SELECT id, val, x FROM (
            VALUES (1, 10.0), (2, 7.0)
        ) t(id, val)
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) >= 1
        MAXIMIZE MIN(x * val) WHEN val > 100
    """
    decidb_cli.assert_error(sql, match=_EMPTY_WHEN_ERROR_REGEX)


@pytest.mark.edge_case
@pytest.mark.min_max
@pytest.mark.when_objective
@pytest.mark.error_infeasible
def test_minimize_max_objective_when_empty(decidb_cli):
    """MINIMIZE MAX(...) WHEN <never> — easy-case flat MAX over empty.

    Mirror of `test_maximize_min_objective_when_empty`. MAX(∅) = −∞,
    so MINIMIZE drives z to −∞ → unbounded → infeasibility/error.
    """
    sql = """
        SELECT id, val, x FROM (
            VALUES (1, 10.0), (2, 7.0)
        ) t(id, val)
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) >= 1
        MINIMIZE MAX(x * val) WHEN val > 100
    """
    decidb_cli.assert_error(sql, match=_EMPTY_WHEN_ERROR_REGEX)


@pytest.mark.edge_case
@pytest.mark.min_max
@pytest.mark.when_objective
@pytest.mark.error_infeasible
def test_maximize_max_objective_when_empty(decidb_cli):
    """MAXIMIZE MAX(...) WHEN <never> — HARD-case flat MAX over empty.

    Hard-case formulation: global z + per-row binary indicators y_i with
    SUM(y_i) >= 1 pinning z to one row's value. Empty row set means no
    indicator can satisfy SUM(y) >= 1 → the model is infeasible. Today
    DecidB silently returns OPTIMAL because the indicator block is skipped
    when there are zero matching rows.
    """
    sql = """
        SELECT id, val, x FROM (
            VALUES (1, 10.0), (2, 7.0)
        ) t(id, val)
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) >= 1
        MAXIMIZE MAX(x * val) WHEN val > 100
    """
    decidb_cli.assert_error(sql, match=_EMPTY_WHEN_ERROR_REGEX)


@pytest.mark.edge_case
@pytest.mark.when_constraint
@pytest.mark.cons_aggregate
@pytest.mark.error_infeasible
def test_mixed_empty_and_populated_when_terms_constraint(decidb_cli):
    """Mixed aggregate-local WHEN with one empty term — now rejected.

    ``SUM(x*v) WHEN <never> + SUM(x*v) WHEN <sometimes> <= K`` — previously
    the empty term contributed 0 and the populated term still bound. Per the
    project decision to reject every empty aggregate (strictly applied to
    SUM/AVG as well as MIN/MAX), this shape now raises before reaching the
    solver. Users with data-dependent WHEN predicates that might match zero
    rows must guard against that case in their query.
    """
    sql = """
        SELECT id, val, x FROM (
            VALUES (1, 10.0, true),
                   (2, 5.0, true),
                   (3, 7.0, false)
        ) t(id, val, w2)
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * val) WHEN (val > 1000)
                + SUM(x * val) WHEN w2 <= 8
        MAXIMIZE SUM(x * val)
    """
    decidb_cli.assert_error(sql, match=_EMPTY_WHEN_ERROR_REGEX)


@pytest.mark.edge_case
@pytest.mark.when_objective
@pytest.mark.error_infeasible
def test_mixed_empty_and_populated_when_terms_objective(decidb_cli):
    """Mirror of the constraint-side test for an objective — now rejected."""
    sql = """
        SELECT id, val, bonus, x FROM (
            VALUES (1, 10.0, 0.0, false),
                   (2, 1.0, 100.0, true),
                   (3, 3.0, 5.0, false)
        ) t(id, val, bonus, w2)
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) <= 1
        MAXIMIZE SUM(x * val) WHEN (val > 1000)
               + SUM(x * bonus) WHEN w2
    """
    decidb_cli.assert_error(sql, match=_EMPTY_WHEN_ERROR_REGEX)


@pytest.mark.edge_case
@pytest.mark.min_max
@pytest.mark.when_objective
@pytest.mark.error_infeasible
def test_minimize_min_objective_when_empty(decidb_cli):
    """MINIMIZE MIN(...) WHEN <never> — HARD-case flat MIN over empty.

    Mirror of `test_maximize_max_objective_when_empty` for the MIN side.
    Per-row binary indicators y_i with SUM(y_i) >= 1; empty row set means
    no indicator → infeasible. Currently silently returns OPTIMAL.
    """
    sql = """
        SELECT id, val, x FROM (
            VALUES (1, 10.0), (2, 7.0)
        ) t(id, val)
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) >= 1
        MINIMIZE MIN(x * val) WHEN val > 100
    """
    decidb_cli.assert_error(sql, match=_EMPTY_WHEN_ERROR_REGEX)


# ----- Empty-WHEN on the CONSTRAINT side (mirrors the four objective tests above) -----
#
# Same root bug as the objective-side tests: a WHEN filter that matches zero
# rows leaves the MIN/MAX auxiliary `z` (or `z_k` per term in the composed
# path) unpinned. Hard-direction shapes are observably infeasible by math
# (MIN(∅) = +∞, MAX(∅) = −∞); easy-direction composed shapes happen to
# coincide with the math but the SUM term inside the same constraint is
# silently vacated rather than enforced. See
# `context/descriptions/05_testing/min_max/todo.md` and
# `context/descriptions/05_testing/when/todo.md`.


@pytest.mark.edge_case
@pytest.mark.min_max
@pytest.mark.when_constraint
@pytest.mark.cons_aggregate
@pytest.mark.error_infeasible
def test_max_when_empty_constraint_hard(decidb_cli):
    """`(MAX(x*val) WHEN <never>) >= K` — hard direction. MAX(∅) = −∞ < K
    so semantically infeasible. Currently silently OPTIMAL with arbitrary x.
    """
    sql = """
        SELECT id, val, x FROM (
            VALUES (1, 10.0), (2, 7.0)
        ) t(id, val)
        DECIDE x IS BOOLEAN
        SUCH THAT (MAX(x * val) WHEN (val > 100)) >= 5
        MAXIMIZE SUM(x * val)
    """
    decidb_cli.assert_error(sql, match=_EMPTY_WHEN_ERROR_REGEX)


@pytest.mark.edge_case
@pytest.mark.min_max
@pytest.mark.when_constraint
@pytest.mark.cons_aggregate
@pytest.mark.error_infeasible
def test_min_when_empty_constraint_hard(decidb_cli):
    """`(MIN(x*val) WHEN <never>) <= K` — hard direction. MIN(∅) = +∞ > K
    so semantically infeasible. Currently silently OPTIMAL with arbitrary x.
    """
    sql = """
        SELECT id, val, x FROM (
            VALUES (1, 10.0), (2, 7.0)
        ) t(id, val)
        DECIDE x IS BOOLEAN
        SUCH THAT (MIN(x * val) WHEN (val > 100)) <= 5
        MAXIMIZE SUM(x * val)
    """
    decidb_cli.assert_error(sql, match=_EMPTY_WHEN_ERROR_REGEX)


@pytest.mark.edge_case
@pytest.mark.min_max
@pytest.mark.when_constraint
@pytest.mark.cons_aggregate
@pytest.mark.error_infeasible
def test_sum_plus_max_when_empty_silently_vacates_constraint(decidb_cli):
    """`SUM(x*val) + (MAX(x*val) WHEN <never>) <= K` — composed easy
    direction. The doc reports the entire constraint is silently vacated:
    the SUM term should still bind even when the MAX term has empty WHEN.

    On `(VALUES (1, 10.0), (2, 7.0))` with K=5, the SUM term alone is binding
    (SUM=17 > 5 if both x=1, so the constraint should force x_1+x_2 ≤ 0
    if MAX vanishes, or be infeasible per the root-todo's "reject empty"
    directive). Currently DecidB picks x=[1,1] (SUM=17), confirming the
    constraint is a no-op.
    """
    sql = """
        SELECT id, val, x FROM (
            VALUES (1, 10.0), (2, 7.0)
        ) t(id, val)
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * val) + (MAX(x * val) WHEN (val > 100)) <= 5
        MAXIMIZE SUM(x * val)
    """
    decidb_cli.assert_error(sql, match=_EMPTY_WHEN_ERROR_REGEX)


@pytest.mark.edge_case
@pytest.mark.when_constraint
@pytest.mark.cons_aggregate
@pytest.mark.error_infeasible
def test_sum_when_empty_rejected(decidb_cli):
    """`SUM(x*val) <= K WHEN <never>` — SUM(∅) = 0 is mathematically defined
    but the "reject all empty sets" directive catches this too. Surfaces the
    likely WHEN-typo rather than silently making the constraint vacuous."""
    sql = """
        SELECT id, val, x FROM (
            VALUES (1, 10.0), (2, 7.0), (3, 4.0)
        ) t(id, val)
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * val) <= 5 WHEN val > 100
        MAXIMIZE SUM(x * val)
    """
    decidb_cli.assert_error(sql, match=_EMPTY_WHEN_ERROR_REGEX)


@pytest.mark.edge_case
@pytest.mark.avg_rewrite
@pytest.mark.when_constraint
@pytest.mark.cons_aggregate
@pytest.mark.error_infeasible
def test_avg_when_empty_rejected(decidb_cli):
    """`AVG(x*val) <= K WHEN <never>` — AVG(∅) is undefined (divide by zero
    row count); rejected pre-solver."""
    sql = """
        SELECT id, val, x FROM (
            VALUES (1, 10.0), (2, 7.0), (3, 4.0)
        ) t(id, val)
        DECIDE x IS BOOLEAN
        SUCH THAT AVG(x * val) <= 5 WHEN val > 100
        MAXIMIZE SUM(x * val)
    """
    decidb_cli.assert_error(sql, match=_EMPTY_WHEN_ERROR_REGEX)


@pytest.mark.edge_case
@pytest.mark.min_max
@pytest.mark.when_constraint
@pytest.mark.cons_aggregate
@pytest.mark.error_infeasible
def test_composed_easy_min_when_empty_rejected(decidb_cli):
    """`SUM(x*val) + (MIN(x*val) WHEN <never>) >= K` — composed easy-direction
    with an empty MIN term. Pre-fix DecidB returned OPTIMAL x=[1,1] because
    the per-term auxiliary z_k for the MIN floated free (happened to coincide
    with MIN(∅) = +∞ but via the wrong mechanism). Now rejected pre-solver.
    """
    sql = """
        SELECT id, val, x FROM (
            VALUES (1, 10.0), (2, 7.0)
        ) t(id, val)
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * val) + (MIN(x * val) WHEN (val > 100)) >= 100
        MAXIMIZE SUM(x * val)
    """
    decidb_cli.assert_error(sql, match=_EMPTY_WHEN_ERROR_REGEX)


@pytest.mark.edge_case
def test_feasibility_no_objective(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Feasibility problem (no MAXIMIZE/MINIMIZE) — should find any feasible solution."""
    sql = """
        WITH data AS (
            SELECT 1 AS id, 10.0 AS val UNION ALL
            SELECT 2, 20.0 UNION ALL
            SELECT 3, 30.0 UNION ALL
            SELECT 4, 40.0 UNION ALL
            SELECT 5, 50.0
        )
        SELECT id, val, x
        FROM data
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) = 2 AND SUM(x * val) <= 50
    """
    result, cols = decidb_cli.execute(sql)
    assert len(result) == 5

    x_idx = cols.index("x")
    val_idx = cols.index("val")

    total_x = sum(int(row[x_idx]) for row in result)
    assert total_x == 2, f"SUM(x) = {total_x}, expected 2"

    weighted_sum = sum(int(row[x_idx]) * float(row[val_idx]) for row in result)
    assert weighted_sum <= 50 + 1e-4, f"SUM(x*val) = {weighted_sum} > 50"


@pytest.mark.edge_case
@pytest.mark.per_clause
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_feasibility_per(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Feasibility problem (no objective) combined with PER constraints.

    Exercises the FEASIBILITY sense path through PER constraint generation:
    `SUM(x) = 1 PER grp` becomes one equality per group, and the PER
    constraint-builder must work even when the model builder sets all
    objective coefficients to zero (DecideSense::FEASIBILITY).

    Also adds a global aggregate constraint `SUM(x * val) <= 35` that
    rules out picking the heaviest item from each group, so the test
    distinguishes "any per-group selection" from "a per-group selection
    that also respects the global cap".

    Oracle role: build the same feasibility problem in gurobipy and assert
    OPTIMAL — this is independent verification that the constraints are
    satisfiable. Then structurally validate DecidB's chosen feasible
    point (per-group cardinality + global budget). Cannot compare
    variable values directly because feasibility problems admit multiple
    optima and DecidB / Gurobi may pick different ones.
    """
    data_sql = """
        SELECT 1 AS id, 'A' AS grp, 10.0 AS val UNION ALL
        SELECT 2, 'A', 20.0 UNION ALL
        SELECT 3, 'A', 30.0 UNION ALL
        SELECT 4, 'B', 5.0 UNION ALL
        SELECT 5, 'B', 25.0
    """
    sql = f"""
        WITH data AS ({data_sql})
        SELECT id, grp, val, x
        FROM data
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) = 1 PER grp
            AND SUM(x * val) <= 35
    """
    t0 = time.perf_counter()
    rows, cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0
    data = duckdb_conn.execute(f"""
        SELECT CAST(id AS BIGINT), CAST(grp AS VARCHAR), CAST(val AS DOUBLE)
        FROM ({data_sql})
    """).fetchall()
    n = len(data)

    t_build = time.perf_counter()
    oracle_solver.create_model("feasibility_per")
    vnames = [f"x_{i}" for i in range(n)]
    for v in vnames:
        oracle_solver.add_variable(v, VarType.BINARY)
    # Per-group SUM(x) = 1 constraints.
    from collections import defaultdict
    groups: dict = defaultdict(list)
    for i, row in enumerate(data):
        groups[row[1]].append(i)
    for grp, idxs in groups.items():
        oracle_solver.add_constraint(
            {vnames[i]: 1.0 for i in idxs}, "=", 1.0, name=f"pick_one_{grp}",
        )
    # Global cap.
    oracle_solver.add_constraint(
        {vnames[i]: float(data[i][2]) for i in range(n)},
        "<=", 35.0, name="budget",
    )
    # Feasibility: empty objective (zero vector) keeps the sense well-defined.
    oracle_solver.set_objective({}, ObjSense.MINIMIZE)
    build_time = time.perf_counter() - t_build

    from solver.types import SolverStatus
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL, (
        f"Oracle reports infeasible/error for feasibility+PER: status={result.status}. "
        f"DecidB returned {len(rows)} rows, so DecidB thinks it is feasible — divergence."
    )

    # Structural validation of DecidB's chosen point.
    assert len(rows) == n, f"row count mismatch: DecidB returned {len(rows)}, expected {n}"
    x_idx, grp_idx, val_idx = cols.index("x"), cols.index("grp"), cols.index("val")
    by_grp: dict = defaultdict(int)
    for r in rows:
        by_grp[str(r[grp_idx])] += int(r[x_idx])
    for grp, total in by_grp.items():
        assert total == 1, (
            f"Group {grp} SUM(x)={total} but PER constraint requires exactly 1"
        )
    weighted = sum(int(r[x_idx]) * float(r[val_idx]) for r in rows)
    assert weighted <= 35 + 1e-4, (
        f"DecidB violates global budget: SUM(x*val)={weighted} > 35"
    )

    perf_tracker.record(
        "feasibility_per", decidb_time, build_time, result.solve_time_seconds,
        n, n, len(groups) + 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )


@pytest.mark.edge_case
@pytest.mark.error
def test_null_coefficients(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """NULL values in coefficient columns — DecidB rejects with helpful COALESCE hint."""
    decidb_cli.assert_error("""
        WITH data AS (
            SELECT 1 AS id, 10.0 AS weight, 5.0 AS value UNION ALL
            SELECT 2, NULL, 3.0 UNION ALL
            SELECT 3, 8.0, 7.0 UNION ALL
            SELECT 4, 6.0, NULL
        )
        SELECT id, weight, value, x
        FROM data
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * weight) <= 15
        MAXIMIZE SUM(x * value)
    """, match=r"NULL")


@pytest.mark.edge_case
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_all_zero_objective(decidb_cli, oracle_solver, perf_tracker):
    """``MAXIMIZE SUM(x * 0)`` — objective is identically zero.

    With a zero objective, the solver returns any feasible solution. DecidB
    must still report OPTIMAL with objective value 0.0 and must honour the
    feasibility constraint. A status-handling or objective-builder bug
    (e.g., treating the all-zero coefficient vector as "no objective set")
    would surface here.
    """
    sql = """
        SELECT id, val, x FROM (VALUES (1, 0.0), (2, 0.0), (3, 0.0)) t(id, val)
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) >= 1
        MAXIMIZE SUM(x * val)
    """
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    n = 3
    t_build = time.perf_counter()
    oracle_solver.create_model("all_zero_objective")
    vnames = [f"x_{i}" for i in range(n)]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)
    oracle_solver.add_constraint(
        {vn: 1.0 for vn in vnames}, ">=", 1.0, name="floor",
    )
    oracle_solver.set_objective(
        {vn: 0.0 for vn in vnames}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    from solver.types import SolverStatus
    assert result.status == SolverStatus.OPTIMAL
    assert abs(result.objective_value) <= 1e-9, (
        f"Oracle objective should be 0, got {result.objective_value}"
    )

    val_idx = decidb_cols.index("val")
    x_idx = decidb_cols.index("x")
    decidb_obj = sum(int(r[x_idx]) * float(r[val_idx]) for r in decidb_rows)
    assert abs(decidb_obj) <= 1e-9, (
        f"DecidB objective should be 0, got {decidb_obj}"
    )
    # Feasibility: at least one row picked (per SUM(x) >= 1).
    total_x = sum(int(r[x_idx]) for r in decidb_rows)
    assert total_x >= 1, f"SUM(x)={total_x} violates feasibility constraint"

    perf_tracker.record(
        "all_zero_objective", decidb_time, build_time,
        result.solve_time_seconds, n, n, 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )


# ---------------------------------------------------------------------------
# Stress tests from edge_cases/todo.md: many-terms, heterogeneous constraints,
# and large-coefficient numeric stability. All use tiny inline VALUES so the
# oracle stays small; these exercise the symbolic normalizer and matrix
# builder without relying on TPC-H shape.
# ---------------------------------------------------------------------------

@pytest.mark.edge_case
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_many_terms_objective(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """10-column linear combination in both constraint and objective.

    Stresses the symbolic normalizer / expression evaluator on a much wider
    term list than typical TPC-H queries (which use 2-3 columns). Coefficient
    mismatches between DecidB's extraction and the oracle would surface as an
    x-vector or objective divergence.
    """
    data_sql = (
        "SELECT * FROM (VALUES "
        "(1, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0),"
        "(2, 2.0, 1.0, 4.0, 3.0, 6.0, 5.0, 8.0, 7.0, 10.0, 9.0),"
        "(3, 3.0, 3.0, 1.0, 1.0, 2.0, 2.0, 4.0, 4.0, 5.0, 5.0),"
        "(4, 1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 8.5, 9.5, 1.0),"
        "(5, 2.5, 3.5, 1.5, 4.5, 2.0, 3.0, 6.5, 1.5, 4.0, 2.5)"
        ") t(id, c1, c2, c3, c4, c5, c6, c7, c8, c9, c10)"
    )
    sql = f"""
        SELECT id, c1, c2, c3, c4, c5, c6, c7, c8, c9, c10, x FROM ({data_sql})
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * (c1 + c2 + c3 + c4 + c5 + c6 + c7 + c8 + c9 + c10)) <= 40
        MAXIMIZE SUM(x * (c1 + 2*c2 + c3 + 2*c4 + c5 + 2*c6 + c7 + 2*c8 + c9 + 2*c10))
    """
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute(
        f"SELECT id, "
        f"CAST(c1 AS DOUBLE), CAST(c2 AS DOUBLE), CAST(c3 AS DOUBLE), "
        f"CAST(c4 AS DOUBLE), CAST(c5 AS DOUBLE), CAST(c6 AS DOUBLE), "
        f"CAST(c7 AS DOUBLE), CAST(c8 AS DOUBLE), CAST(c9 AS DOUBLE), "
        f"CAST(c10 AS DOUBLE) FROM ({data_sql})"
    ).fetchall()
    n = len(data)

    # Precompute per-row constraint and objective coefficients from the 10 cols.
    cons_coeffs = [sum(data[i][1:11]) for i in range(n)]
    obj_weights = [1, 2, 1, 2, 1, 2, 1, 2, 1, 2]
    obj_coeffs = [
        sum(data[i][1 + k] * w for k, w in enumerate(obj_weights))
        for i in range(n)
    ]

    t_build = time.perf_counter()
    oracle_solver.create_model("many_terms_objective")
    vnames = [f"x_{i}" for i in range(n)]
    for v in vnames:
        oracle_solver.add_variable(v, VarType.BINARY)
    oracle_solver.add_constraint(
        {vnames[i]: cons_coeffs[i] for i in range(n)},
        "<=", 40.0, name="wide_cap",
    )
    oracle_solver.set_objective(
        {vnames[i]: obj_coeffs[i] for i in range(n)}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    def _obj(row):
        # Per-row objective coefficient computed the same way the oracle did.
        cols = decidb_cols
        idx = {c: cols.index(c) for c in
               ("c1", "c2", "c3", "c4", "c5", "c6", "c7", "c8", "c9", "c10")}
        c = [float(row[idx[f"c{k+1}"]]) for k in range(10)]
        return {"x": sum(c[k] * obj_weights[k] for k in range(10))}

    cmp = compare_solutions(
        decidb_rows, decidb_cols, result, data, ["x"],
        coeff_fn=_obj,
    )
    perf_tracker.record(
        "many_terms_objective", decidb_time, build_time,
        result.solve_time_seconds, n, n, 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.edge_case
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.cons_perrow
@pytest.mark.when_constraint
@pytest.mark.per_clause
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_five_plus_heterogeneous_constraints(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """Six mixed constraints in one query: per-row, aggregate, WHEN, PER, NE, BETWEEN.

    Stresses the constraint-matrix builder with heterogeneous shapes in a
    single DECIDE — indexing errors or rewrite ordering bugs that only appear
    when several constraint kinds coexist would surface here.
    """
    data_sql = (
        "SELECT * FROM (VALUES "
        "(1, 3.0, 10.0, 'R', 'A'),"
        "(2, 2.0,  5.0, 'R', 'A'),"
        "(3, 5.0, 15.0, 'N', 'A'),"
        "(4, 4.0, 12.0, 'R', 'B'),"
        "(5, 1.0,  4.0, 'N', 'B'),"
        "(6, 2.0,  8.0, 'N', 'B'),"
        "(7, 3.0,  9.0, 'R', 'C'),"
        "(8, 4.0, 11.0, 'R', 'C')"
        ") t(id, qty, price, flag, category)"
    )
    sql = f"""
        SELECT id, qty, price, flag, category, x FROM ({data_sql})
        DECIDE x IS BOOLEAN
        SUCH THAT x <= 1
            AND SUM(x * qty) <= 12
            AND SUM(x) >= 2 WHEN flag = 'R'
            AND SUM(x) <= 2 PER category
            AND SUM(x) <> 7
            AND SUM(x * price) BETWEEN 10 AND 60
        MAXIMIZE SUM(x * price)
    """
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute(
        f"SELECT id, CAST(qty AS DOUBLE), CAST(price AS DOUBLE), "
        f"CAST(flag AS VARCHAR), CAST(category AS VARCHAR) FROM ({data_sql})"
    ).fetchall()
    n = len(data)

    from ._oracle_helpers import add_ne_indicator, group_indices
    t_build = time.perf_counter()
    oracle_solver.create_model("five_plus_heterogeneous")
    vnames = [f"x_{i}" for i in range(n)]
    for v in vnames:
        oracle_solver.add_variable(v, VarType.BINARY)

    # (1) per-row x <= 1 is baked into the BINARY domain — nothing to add.
    # (2) aggregate quantity cap
    oracle_solver.add_constraint(
        {vnames[i]: data[i][1] for i in range(n)},
        "<=", 12.0, name="qty_cap",
    )
    # (3) WHEN flag='R' lower bound on count
    r_idx = [i for i in range(n) if data[i][3] == "R"]
    oracle_solver.add_constraint(
        {vnames[i]: 1.0 for i in r_idx},
        ">=", 2.0, name="when_R_floor",
    )
    # (4) PER category upper bound on count
    per_cat = group_indices(data, lambda r: r[4])
    for cat, idxs in per_cat.items():
        oracle_solver.add_constraint(
            {vnames[i]: 1.0 for i in idxs},
            "<=", 2.0, name=f"per_cat_{cat}",
        )
    # (5) NE: SUM(x) != 7 via native Gurobi indicator (no Big-M)
    add_ne_indicator(
        oracle_solver, {v: 1.0 for v in vnames}, 7.0, name="sum_ne_7",
    )
    # (6) BETWEEN: SUM(x*price) >= 10 AND <= 60
    price_coeffs = {vnames[i]: data[i][2] for i in range(n)}
    oracle_solver.add_constraint(price_coeffs, ">=", 10.0, name="between_lo")
    oracle_solver.add_constraint(price_coeffs, "<=", 60.0, name="between_hi")

    oracle_solver.set_objective(price_coeffs, ObjSense.MAXIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_rows, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[decidb_cols.index("price")])},
    )
    perf_tracker.record(
        "five_plus_heterogeneous_constraints", decidb_time, build_time,
        result.solve_time_seconds, n, n, 6,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.edge_case
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.cons_comparison
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_large_coefficient_numeric_stability(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """1e9 coefficients combined with `<>` Big-M expansion.

    The NE Big-M rewrite `SUM(x*v) <> K` expands into a disjunction sized by
    the coefficient magnitude. With `v = 1e9`, the Big-M is enormous — a
    numerically unstable formulation would diverge from the oracle or miss
    the optimum. This test pins the oracle on integer-valued SUM (per
    `ilp_model_builder.cpp`'s integer-step rewrite path, `<> 1` is valid
    because the LHS `SUM(x)` is integer-valued).
    """
    data_sql = (
        "SELECT * FROM (VALUES (1, 1000000000.0), (2, 1000000000.0)) t(id, val)"
    )
    sql = f"""
        SELECT id, val, x FROM ({data_sql})
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) <> 1
            AND SUM(x * val) <= 2000000000
        MAXIMIZE SUM(x * val)
    """
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute(
        f"SELECT id, CAST(val AS DOUBLE) FROM ({data_sql})"
    ).fetchall()
    n = len(data)

    from ._oracle_helpers import add_ne_indicator
    t_build = time.perf_counter()
    oracle_solver.create_model("large_coefficient_ne")
    vnames = [f"x_{i}" for i in range(n)]
    for v in vnames:
        oracle_solver.add_variable(v, VarType.BINARY)
    # Native indicator SUM != 1 (no Big-M selection by the oracle)
    add_ne_indicator(
        oracle_solver, {v: 1.0 for v in vnames}, 1.0, name="sum_ne_1",
    )
    oracle_solver.add_constraint(
        {vnames[i]: data[i][1] for i in range(n)},
        "<=", 2_000_000_000.0, name="big_cap",
    )
    oracle_solver.set_objective(
        {vnames[i]: data[i][1] for i in range(n)}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_rows, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[decidb_cols.index("val")])},
        tolerance=1.0,  # 1e9 coefficients → absolute tolerance scaled accordingly
    )
    perf_tracker.record(
        "large_coefficient_numeric_stability", decidb_time, build_time,
        result.solve_time_seconds, n, n, 2,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


# ---------------------------------------------------------------------------
# Dual-solver agreement: same problem through Gurobi and HiGHS
# ---------------------------------------------------------------------------


@pytest.mark.correctness
@pytest.mark.edge_case
def test_gurobi_highs_agree_on_objective(decidb_cli_highs, decidb_cli_gurobi):
    """Run a linear ILP through both backends; objectives must agree.

    Skips if Gurobi isn't linked (``decidb_cli_gurobi`` fixture skips).
    The 3-row cost-knapsack shape mirrors the ``test_bilinear_minimize_objective``
    data table but without the bilinear term — a pure linear IP small enough
    that both solvers hit the same optimum exactly.
    """
    sql = """
        WITH data AS (
            SELECT 1 AS id, 5.0 AS cost UNION ALL
            SELECT 2, 10.0 UNION ALL
            SELECT 3, 3.0
        )
        SELECT id, cost, b FROM data
        DECIDE b IS BOOLEAN
        SUCH THAT SUM(b) >= 2
        MINIMIZE SUM(cost * b)
    """
    highs_rows, highs_cols = decidb_cli_highs.execute(sql)
    gurobi_rows, gurobi_cols = decidb_cli_gurobi.execute(sql)

    h_ci = {name: i for i, name in enumerate(highs_cols)}
    g_ci = {name: i for i, name in enumerate(gurobi_cols)}
    highs_obj = sum(
        float(r[h_ci["cost"]]) * int(r[h_ci["b"]]) for r in highs_rows
    )
    gurobi_obj = sum(
        float(r[g_ci["cost"]]) * int(r[g_ci["b"]]) for r in gurobi_rows
    )
    assert abs(highs_obj - gurobi_obj) <= 1e-4, (
        f"Solver disagreement: HiGHS={highs_obj}, Gurobi={gurobi_obj}"
    )
