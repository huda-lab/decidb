"""WHEN-clause grammar coverage.

The `decide_when_condition` non-terminal in the DecidB parser is a
restricted `c_expr` that excludes unparenthesized `NOT`, comparison
operators (`=`, `<`, `>`, `<=`, `>=`, `<>`), and arithmetic (`+`, `-`).
Wrapping the condition in parentheses forces it through a fuller grammar
production. Objectives also benefit from
`ReassociateObjectiveWhenComparison()` in `decide_symbolic.cpp`, which
patches up the most common comparison pattern even when unparenthesized
— but only on the objective side and only for simple
`comparison-of-aggregate` shapes.

This file pins the resulting empirical asymmetry:
  - Parenthesized forms work everywhere (oracle-verified positives).
  - Unparenthesized comparison works on objective side only (covered as
    a positive in the parenthesized set; documented in this file's
    docstrings as the asymmetry).
  - Unparenthesized `NOT` and arithmetic-comparison fail with shape-
    specific error messages — pinned exactly so a future grammar widen
    OR a constraint-side reassociator addition trips the test.

See `context/descriptions/03_expressivity/when/todo.md` for the full
asymmetry table.
"""

import time

import pytest

from solver.types import VarType, ObjSense
from comparison.compare import compare_solutions


# ---------------------------------------------------------------------------
# Shared helpers
# ---------------------------------------------------------------------------


def _run_oracle_test(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker,
    *, test_id, decide_sql, data_sql, build_oracle, decidb_obj_fn,
):
    """Boilerplate: execute DecidB, build oracle, compare.

    Mirrors `test_aggregate_local_when._run_constraint_test`. Inlined here
    rather than imported to avoid cross-test-file coupling.
    """
    t0 = time.perf_counter()
    rows, cols = decidb_cli.execute(decide_sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute(data_sql).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model(test_id)
    n_vars, n_constrs = build_oracle(oracle_solver, data, cols, rows)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        rows, cols, result, data, ["x"],
        decidb_objective_fn=decidb_obj_fn,
    )
    perf_tracker.record(
        test_id, decidb_time, build_time, result.solve_time_seconds,
        len(data), n_vars, n_constrs,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


# ===========================================================================
# Positive: parenthesized WHEN conditions on CONSTRAINTS
# ===========================================================================


@pytest.mark.when
@pytest.mark.when_constraint
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_when_paren_not_constraint(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """`WHEN (NOT w)` — parenthesized NOT in a constraint.

    Picks `val` only on rows where `w` is false. With val=[10,5,7] and
    w=[T,F,F], constraint `5 x_1 + 7 x_2 <= 8` admits {x_1, x_2} but not
    both. Maximizer picks x_0=1 (unconstrained) and x_2=1 → obj=17.
    """
    data_sql = """
        SELECT CAST(id AS BIGINT), CAST(val AS DOUBLE), CAST(w AS BOOLEAN)
        FROM (VALUES (1, 10.0, true),
                     (2,  5.0, false),
                     (3,  7.0, false)) t(id, val, w)
    """
    decide_sql = """
        SELECT id, val, w, x FROM (
            VALUES (1, 10.0, true),
                   (2,  5.0, false),
                   (3,  7.0, false)
        ) t(id, val, w)
        DECIDE x(BOOL)
        SUCH THAT SUM(x * val) WHEN (NOT w) <= 8
        MAXIMIZE SUM(x * val)
    """

    def build(oracle, data, cols, rows):
        n = len(data)
        vnames = [f"x_{i}" for i in range(n)]
        for v in vnames:
            oracle.add_variable(v, VarType.BINARY)
        coeffs = {
            vnames[i]: data[i][1] for i in range(n) if not data[i][2]
        }
        oracle.add_constraint(coeffs, "<=", 8.0, name="when_not")
        oracle.set_objective(
            {vnames[i]: data[i][1] for i in range(n)}, ObjSense.MAXIMIZE,
        )
        return n, 1

    def decidb_obj(rs, cs):
        xi = cs.index("x"); vi = cs.index("val")
        return sum(float(r[xi]) * float(r[vi]) for r in rs)

    _run_oracle_test(
        decidb_cli, duckdb_conn, oracle_solver, perf_tracker,
        test_id="when_paren_not_constraint",
        decide_sql=decide_sql, data_sql=data_sql,
        build_oracle=build, decidb_obj_fn=decidb_obj,
    )


@pytest.mark.when
@pytest.mark.when_constraint
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_when_paren_eq_constraint(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """`WHEN (tier = 'high')` — parenthesized comparison in a constraint.

    val=[10,5,7], tier=[high,low,high]. Constraint `10 x_0 + 7 x_2 <= 8`:
    x_0 must be 0 (10>8); x_2 may be 1 (7<=8). x_1 unconstrained →
    optimum picks x_1=1, x_2=1 → obj = 12.
    """
    data_sql = """
        SELECT CAST(id AS BIGINT), CAST(val AS DOUBLE), CAST(tier AS VARCHAR)
        FROM (VALUES (1, 10.0, 'high'),
                     (2,  5.0, 'low'),
                     (3,  7.0, 'high')) t(id, val, tier)
    """
    decide_sql = """
        SELECT id, val, tier, x FROM (
            VALUES (1, 10.0, 'high'),
                   (2,  5.0, 'low'),
                   (3,  7.0, 'high')
        ) t(id, val, tier)
        DECIDE x(BOOL)
        SUCH THAT SUM(x * val) WHEN (tier = 'high') <= 8
        MAXIMIZE SUM(x * val)
    """

    def build(oracle, data, cols, rows):
        n = len(data)
        vnames = [f"x_{i}" for i in range(n)]
        for v in vnames:
            oracle.add_variable(v, VarType.BINARY)
        coeffs = {
            vnames[i]: data[i][1] for i in range(n) if data[i][2] == "high"
        }
        oracle.add_constraint(coeffs, "<=", 8.0, name="when_eq")
        oracle.set_objective(
            {vnames[i]: data[i][1] for i in range(n)}, ObjSense.MAXIMIZE,
        )
        return n, 1

    def decidb_obj(rs, cs):
        xi = cs.index("x"); vi = cs.index("val")
        return sum(float(r[xi]) * float(r[vi]) for r in rs)

    _run_oracle_test(
        decidb_cli, duckdb_conn, oracle_solver, perf_tracker,
        test_id="when_paren_eq_constraint",
        decide_sql=decide_sql, data_sql=data_sql,
        build_oracle=build, decidb_obj_fn=decidb_obj,
    )


@pytest.mark.when
@pytest.mark.when_constraint
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_when_paren_arith_constraint(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """`WHEN (a + b > 5)` — parenthesized arithmetic+comparison in a constraint.

    val=[10,5,7], (a+b)=[6,2,6]. Same shape as the eq test: rows 0,2 enter
    the constraint, row 1 is free. Optimum: x=[0,1,1] → obj=12.
    """
    data_sql = """
        SELECT CAST(id AS BIGINT), CAST(val AS DOUBLE),
               CAST(a AS BIGINT), CAST(b AS BIGINT)
        FROM (VALUES (1, 10.0, 2, 4),
                     (2,  5.0, 1, 1),
                     (3,  7.0, 3, 3)) t(id, val, a, b)
    """
    decide_sql = """
        SELECT id, val, a, b, x FROM (
            VALUES (1, 10.0, 2, 4),
                   (2,  5.0, 1, 1),
                   (3,  7.0, 3, 3)
        ) t(id, val, a, b)
        DECIDE x(BOOL)
        SUCH THAT SUM(x * val) WHEN (a + b > 5) <= 8
        MAXIMIZE SUM(x * val)
    """

    def build(oracle, data, cols, rows):
        n = len(data)
        vnames = [f"x_{i}" for i in range(n)]
        for v in vnames:
            oracle.add_variable(v, VarType.BINARY)
        coeffs = {
            vnames[i]: data[i][1]
            for i in range(n)
            if (data[i][2] + data[i][3]) > 5
        }
        oracle.add_constraint(coeffs, "<=", 8.0, name="when_arith")
        oracle.set_objective(
            {vnames[i]: data[i][1] for i in range(n)}, ObjSense.MAXIMIZE,
        )
        return n, 1

    def decidb_obj(rs, cs):
        xi = cs.index("x"); vi = cs.index("val")
        return sum(float(r[xi]) * float(r[vi]) for r in rs)

    _run_oracle_test(
        decidb_cli, duckdb_conn, oracle_solver, perf_tracker,
        test_id="when_paren_arith_constraint",
        decide_sql=decide_sql, data_sql=data_sql,
        build_oracle=build, decidb_obj_fn=decidb_obj,
    )


# ===========================================================================
# Positive: parenthesized WHEN conditions on OBJECTIVES
# ===========================================================================


@pytest.mark.when
@pytest.mark.when_objective
@pytest.mark.correctness
def test_when_paren_not_objective(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """`WHEN (NOT w)` — parenthesized NOT on the objective.

    SUCH THAT SUM(x) <= 1 forces a single pick. Objective sums val on
    rows where w is false. Optimum: x_2=1 → obj=7 (vs 5 for x_1, 0 for x_0).
    """
    data_sql = """
        SELECT CAST(id AS BIGINT), CAST(val AS DOUBLE), CAST(w AS BOOLEAN)
        FROM (VALUES (1, 10.0, true),
                     (2,  5.0, false),
                     (3,  7.0, false)) t(id, val, w)
    """
    decide_sql = """
        SELECT id, val, w, x FROM (
            VALUES (1, 10.0, true),
                   (2,  5.0, false),
                   (3,  7.0, false)
        ) t(id, val, w)
        DECIDE x(BOOL)
        SUCH THAT SUM(x) <= 1
        MAXIMIZE SUM(x * val) WHEN (NOT w)
    """

    def build(oracle, data, cols, rows):
        n = len(data)
        vnames = [f"x_{i}" for i in range(n)]
        for v in vnames:
            oracle.add_variable(v, VarType.BINARY)
        oracle.add_constraint(
            {v: 1.0 for v in vnames}, "<=", 1.0, name="pick_one",
        )
        obj = {
            vnames[i]: data[i][1] for i in range(n) if not data[i][2]
        }
        oracle.set_objective(obj, ObjSense.MAXIMIZE)
        return n, 1

    def decidb_obj(rs, cs):
        xi = cs.index("x"); vi = cs.index("val"); wi = cs.index("w")
        return sum(
            float(r[xi]) * float(r[vi])
            for r in rs
            if not r[wi]
        )

    _run_oracle_test(
        decidb_cli, duckdb_conn, oracle_solver, perf_tracker,
        test_id="when_paren_not_objective",
        decide_sql=decide_sql, data_sql=data_sql,
        build_oracle=build, decidb_obj_fn=decidb_obj,
    )


@pytest.mark.when
@pytest.mark.when_objective
@pytest.mark.correctness
def test_when_paren_eq_objective(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """`WHEN (tier = 'high')` — parenthesized comparison on the objective.

    Note: the unparenthesized form `WHEN tier = 'high'` *also* works on
    objectives via `ReassociateObjectiveWhenComparison()`. We only cover
    the parenthesized form here because the unparenthesized constraint-
    side rejection (and the asymmetric error message) is pinned in
    `test_constraint_unparen_eq_message_sentinel` below.
    """
    data_sql = """
        SELECT CAST(id AS BIGINT), CAST(val AS DOUBLE), CAST(tier AS VARCHAR)
        FROM (VALUES (1, 10.0, 'high'),
                     (2,  5.0, 'low'),
                     (3,  7.0, 'high')) t(id, val, tier)
    """
    decide_sql = """
        SELECT id, val, tier, x FROM (
            VALUES (1, 10.0, 'high'),
                   (2,  5.0, 'low'),
                   (3,  7.0, 'high')
        ) t(id, val, tier)
        DECIDE x(BOOL)
        SUCH THAT SUM(x) <= 1
        MAXIMIZE SUM(x * val) WHEN (tier = 'high')
    """

    def build(oracle, data, cols, rows):
        n = len(data)
        vnames = [f"x_{i}" for i in range(n)]
        for v in vnames:
            oracle.add_variable(v, VarType.BINARY)
        oracle.add_constraint(
            {v: 1.0 for v in vnames}, "<=", 1.0, name="pick_one",
        )
        obj = {
            vnames[i]: data[i][1] for i in range(n) if data[i][2] == "high"
        }
        oracle.set_objective(obj, ObjSense.MAXIMIZE)
        return n, 1

    def decidb_obj(rs, cs):
        xi = cs.index("x"); vi = cs.index("val"); ti = cs.index("tier")
        return sum(
            float(r[xi]) * float(r[vi])
            for r in rs
            if r[ti] == "high"
        )

    _run_oracle_test(
        decidb_cli, duckdb_conn, oracle_solver, perf_tracker,
        test_id="when_paren_eq_objective",
        decide_sql=decide_sql, data_sql=data_sql,
        build_oracle=build, decidb_obj_fn=decidb_obj,
    )


@pytest.mark.when
@pytest.mark.when_objective
@pytest.mark.correctness
def test_when_paren_arith_objective(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """`WHEN (a + b > 5)` — parenthesized arithmetic+comparison on the objective."""
    data_sql = """
        SELECT CAST(id AS BIGINT), CAST(val AS DOUBLE),
               CAST(a AS BIGINT), CAST(b AS BIGINT)
        FROM (VALUES (1, 10.0, 2, 4),
                     (2,  5.0, 1, 1),
                     (3,  7.0, 3, 3)) t(id, val, a, b)
    """
    decide_sql = """
        SELECT id, val, a, b, x FROM (
            VALUES (1, 10.0, 2, 4),
                   (2,  5.0, 1, 1),
                   (3,  7.0, 3, 3)
        ) t(id, val, a, b)
        DECIDE x(BOOL)
        SUCH THAT SUM(x) <= 1
        MAXIMIZE SUM(x * val) WHEN (a + b > 5)
    """

    def build(oracle, data, cols, rows):
        n = len(data)
        vnames = [f"x_{i}" for i in range(n)]
        for v in vnames:
            oracle.add_variable(v, VarType.BINARY)
        oracle.add_constraint(
            {v: 1.0 for v in vnames}, "<=", 1.0, name="pick_one",
        )
        obj = {
            vnames[i]: data[i][1]
            for i in range(n)
            if (data[i][2] + data[i][3]) > 5
        }
        oracle.set_objective(obj, ObjSense.MAXIMIZE)
        return n, 1

    def decidb_obj(rs, cs):
        xi = cs.index("x"); vi = cs.index("val")
        ai = cs.index("a"); bi = cs.index("b")
        return sum(
            float(r[xi]) * float(r[vi])
            for r in rs
            if (int(r[ai]) + int(r[bi])) > 5
        )

    _run_oracle_test(
        decidb_cli, duckdb_conn, oracle_solver, perf_tracker,
        test_id="when_paren_arith_objective",
        decide_sql=decide_sql, data_sql=data_sql,
        build_oracle=build, decidb_obj_fn=decidb_obj,
    )


# ===========================================================================
# Negative: unparenthesized WHEN conditions — pinned error messages
# ===========================================================================


@pytest.mark.when
@pytest.mark.when_constraint
@pytest.mark.error_parser
@pytest.mark.error
def test_when_unparen_not_constraint_rejects(decidb_cli):
    """`WHEN NOT w` (unparenthesized) on a constraint — parser-level reject."""
    decidb_cli.assert_error(
        """
        SELECT id, val, w, x FROM (
            VALUES (1, 10.0, true),
                   (2,  5.0, false)
        ) t(id, val, w)
        DECIDE x(BOOL)
        SUCH THAT SUM(x * val) WHEN NOT w <= 8
        MAXIMIZE SUM(x * val)
        """,
        match=r'syntax error at or near "NOT"',
    )


@pytest.mark.when
@pytest.mark.when_constraint
@pytest.mark.error_parser
@pytest.mark.error
def test_when_unparen_eq_constraint_rejects(decidb_cli):
    """`WHEN tier = 'high'` (unparenthesized) on a constraint — parser-level reject.

    The `<=` token after the comparison is unparseable inside `c_expr`.
    """
    decidb_cli.assert_error(
        """
        SELECT id, val, tier, x FROM (
            VALUES (1, 10.0, 'high'),
                   (2,  5.0, 'low')
        ) t(id, val, tier)
        DECIDE x(BOOL)
        SUCH THAT SUM(x * val) WHEN tier = 'high' <= 8
        MAXIMIZE SUM(x * val)
        """,
        match=r'syntax error at or near "<="',
    )


@pytest.mark.when
@pytest.mark.when_constraint
@pytest.mark.error_parser
@pytest.mark.error
def test_when_unparen_arith_constraint_rejects(decidb_cli):
    """`WHEN a + b > 5` (unparenthesized arithmetic+comparison) on a constraint."""
    decidb_cli.assert_error(
        """
        SELECT id, val, a, b, x FROM (
            VALUES (1, 10.0, 2, 4),
                   (2,  5.0, 1, 1)
        ) t(id, val, a, b)
        DECIDE x(BOOL)
        SUCH THAT SUM(x * val) WHEN a + b > 5 <= 8
        MAXIMIZE SUM(x * val)
        """,
        match=r'syntax error at or near "<="',
    )


@pytest.mark.when
@pytest.mark.when_objective
@pytest.mark.error_parser
@pytest.mark.error
def test_when_unparen_not_objective_rejects(decidb_cli):
    """`WHEN NOT w` (unparenthesized) on an objective — parser-level reject.

    `ReassociateObjectiveWhenComparison()` only handles the comparison-of-
    aggregate shape, not unary NOT, so the parser bails before the
    reassociator can run.
    """
    decidb_cli.assert_error(
        """
        SELECT id, val, w, x FROM (
            VALUES (1, 10.0, true),
                   (2,  5.0, false)
        ) t(id, val, w)
        DECIDE x(BOOL)
        SUCH THAT SUM(x) <= 1
        MAXIMIZE SUM(x * val) WHEN NOT w
        """,
        match=r'syntax error at or near "NOT"',
    )


@pytest.mark.when
@pytest.mark.when_objective
@pytest.mark.error_binder
@pytest.mark.error
def test_when_unparen_arith_objective_rejects(decidb_cli):
    """`WHEN a + b > 5` (unparenthesized) on an objective — binder-level reject.

    The parser successfully reassociates simple comparison-of-aggregate
    on objectives, but it doesn't cover the arithmetic-then-comparison
    shape `(SUM ... WHEN a) + b > 5`. The binder then rejects the
    resulting comparison expression as a top-level objective component.
    Different error PHASE from the constraint-side parser failure — the
    full asymmetry table is in
    `context/descriptions/03_expressivity/when/todo.md`.
    """
    decidb_cli.assert_error(
        """
        SELECT id, val, a, b, x FROM (
            VALUES (1, 10.0, 2, 4),
                   (2,  5.0, 1, 1)
        ) t(id, val, a, b)
        DECIDE x(BOOL)
        SUCH THAT SUM(x) <= 1
        MAXIMIZE SUM(x * val) WHEN a + b > 5
        """,
        match=r'\[MAXIMIZE\|MINIMIZE\] clause does not support',
    )


# ===========================================================================
# Asymmetric-error sentinel
# ===========================================================================


@pytest.mark.when
@pytest.mark.when_constraint
@pytest.mark.error_parser
@pytest.mark.error
def test_constraint_unparen_eq_message_sentinel(decidb_cli):
    """SENTINEL: pins the actual constraint-side error for unparenthesized
    `WHEN x = y <= K`.

    The error today is parser-level `syntax error at or near "<="`. The
    objective-side equivalent works (reassociator). If/when the
    constraint side gains the same recovery — or the grammar is widened
    to admit unparenthesized comparisons in WHEN — this test will start
    failing.

    DO NOT silently relax this regex. The expected response is to either
    (a) delete this test and convert the constraint case to a positive
    test in this file, or (b) update the docs and re-pin the new error.
    """
    decidb_cli.assert_error(
        """
        SELECT id, val, tier, x FROM (
            VALUES (1, 10.0, 'high'),
                   (2,  5.0, 'low')
        ) t(id, val, tier)
        DECIDE x(BOOL)
        SUCH THAT SUM(x * val) WHEN tier = 'high' <= 10
        MAXIMIZE SUM(x * val)
        """,
        match=r'syntax error at or near "<="',
    )


@pytest.mark.when
@pytest.mark.when_constraint
@pytest.mark.error_parser
@pytest.mark.error
def test_when_unparen_error_carries_paren_hint(decidb_cli):
    """The bare bison syntax error on an unparenthesized WHEN condition is
    augmented with an actionable parenthesization hint (MaybeAppendDecideWhenHint).

    This pins the hint text so it is not silently dropped. If the grammar is
    widened so unparenthesized WHEN conditions parse, delete this test.
    """
    decidb_cli.assert_error(
        """
        SELECT id, val, w, x FROM (
            VALUES (1, 10.0, true),
                   (2,  5.0, false)
        ) t(id, val, w)
        DECIDE x(BOOL)
        SUCH THAT SUM(x * val) WHEN NOT w <= 8
        MAXIMIZE SUM(x * val)
        """,
        match=r"wrap the WHEN condition in parentheses",
    )
