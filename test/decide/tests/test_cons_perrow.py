"""Tests for per-row constraints (variable bounds per row).

Covers:
  - q07_row_wise_bounds: x <= 5 per row, plus global SUM constraint
"""

import time

import pytest

from solver.types import VarType, ObjSense, SolverStatus
from comparison.compare import compare_solutions
from ._scope_cases import answer, scope_solver  # noqa: F401 -- scope_solver is a fixture
from ._scope_oracle import CONSTANT, Decision, Reduce, Scoped, ScopedModel, ScopedObjective


@pytest.mark.var_integer
@pytest.mark.cons_perrow
@pytest.mark.cons_mixed
@pytest.mark.cons_multi
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_q07_row_wise_bounds(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Row-wise bounds: x <= 5 for each row, plus SUM(x) <= 100."""
    sql = """
        SELECT ps_partkey, ps_availqty, x
        FROM partsupp
        WHERE ps_partkey < 20
        DECIDE x(INT)
        SUCH THAT x <= 5
          AND SUM(x) <= 100
        MAXIMIZE SUM(x * ps_availqty)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(ps_partkey AS BIGINT),
               CAST(ps_availqty AS DOUBLE)
        FROM partsupp WHERE ps_partkey < 20
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("q07_row_wise")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        # x <= 5 is a per-row upper bound; lb=0 (default INTEGER)
        oracle_solver.add_variable(vn, VarType.INTEGER, lb=0.0, ub=5.0)

    # Global constraint: SUM(x) <= 100
    oracle_solver.add_constraint(
        {vnames[i]: 1.0 for i in range(len(data))},
        "<=", 100.0, name="total_limit",
    )
    # Objective: MAXIMIZE SUM(x * ps_availqty)
    oracle_solver.set_objective(
        {vnames[i]: data[i][1] for i in range(len(data))},
        ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_result, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[decidb_cols.index("ps_availqty")])},
    )

    perf_tracker.record(
        "q07_row_wise_bounds", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 2,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.var_integer
@pytest.mark.cons_perrow
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_per_row_lower_bound(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Per-row lower bound: x >= 1 forces all variables to at least 1."""
    sql = """
        SELECT ps_partkey, ps_suppkey, ps_supplycost, x
        FROM partsupp
        WHERE ps_partkey < 10
        DECIDE x(INT)
        SUCH THAT x >= 1
            AND SUM(x * ps_supplycost) <= 50000
        MAXIMIZE SUM(x)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(ps_partkey AS BIGINT),
               CAST(ps_suppkey AS BIGINT),
               CAST(ps_supplycost AS DOUBLE)
        FROM partsupp WHERE ps_partkey < 10
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("perrow_lb")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.INTEGER, lb=1.0)

    oracle_solver.add_constraint(
        {vnames[i]: data[i][2] for i in range(len(data))},
        "<=", 50000.0, name="budget",
    )
    oracle_solver.set_objective(
        {vnames[i]: 1.0 for i in range(len(data))},
        ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_result, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": 1.0},
    )

    perf_tracker.record(
        "perrow_lb", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 2,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.var_integer
@pytest.mark.cons_perrow
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_reversed_bound_comparisons_are_normalized(decidb_cli, oracle_solver):
    """Equivalent reversed comparisons (`K >= x`, `K <= x`, `K >= SUM(x)`)
    bind as the canonical DECIDE-LHS forms. The bounds must hold per row, and the
    objective must equal the oracle's, which states each comparison in its canonical
    direction (`x <= 5`, `x >= 2`, `SUM(x) <= 6`)."""
    rows, cols = decidb_cli.execute("""
        SELECT id, x
        FROM (VALUES (1), (2)) t(id)
        DECIDE x(INT)
        SUCH THAT 5 >= x
            AND 2 <= x
            AND 6 >= SUM(x)
        MAXIMIZE SUM(x)
    """)
    x_idx = cols.index("x")
    xs = [int(row[x_idx]) for row in rows]
    assert all(2 <= x <= 5 for x in xs)

    oracle_solver.create_model("reversed_bounds")
    vnames = [f"x_{i}" for i in range(len(xs))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.INTEGER, lb=2.0, ub=5.0)
    oracle_solver.add_constraint({vn: 1.0 for vn in vnames}, "<=", 6.0, name="total_limit")
    oracle_solver.set_objective({vn: 1.0 for vn in vnames}, ObjSense.MAXIMIZE)
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL
    assert (
        abs(sum(xs) - result.objective_value) <= 1e-6
    ), f"Objective mismatch: DeciDB={sum(xs)}, Oracle={result.objective_value}"


# --- Per-row LHS with linear transforms (regression tests) ---
# Before Fix A, these silently returned wrong answers because ExtractTerms
# didn't handle `/` and the ilp_model_builder dropped LHS terms without a
# decide variable instead of moving them to RHS. Each case compares decidb
# against an oracle model that states the constraint as written, one row per
# DeciDB row (`coefficient * x_i <= rhs - constant`), plus only the bounds the
# query itself writes and the type's default bound.


@pytest.mark.var_real
@pytest.mark.cons_perrow
@pytest.mark.obj_maximize
@pytest.mark.correctness
# One operator per case: DecideTermSplitter::Split reads `+`, binary `-`, `/` and a product each by its own branch and
# recurses on the operands, so `x / 2 + 1` and `x + 3 - 1` reach only the branches of `x / 2`, `x + 3` and `x - 3`.
# `2 * x + 3` is the one case with the product.
@pytest.mark.parametrize(
    "lhs_sql,rhs_sql,coefficient,constant",
    [
        ("x + 3", "10", 1.0, 3.0),
        ("x - 3", "2", 1.0, -3.0),
        ("x / 2", "1", 0.5, 0.0),
        ("2 * x + 3", "11", 2.0, 3.0),
    ],
    ids=["x + 3-10-7.0", "x - 3-2-5.0", "x / 2-1-2.0", "2 * x + 3-11-4.0"],
)
def test_perrow_linear_lhs_upper_bound(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker, lhs_sql, rhs_sql, coefficient, constant
):
    """Per-row upper-bound constraints with non-trivial linear LHS. The oracle
    states `coefficient * x_i + constant <= rhs` on every row; decidb should match."""
    sql = f"""
        SELECT ps_partkey, ps_availqty, x
        FROM partsupp
        WHERE ps_partkey < 10
        DECIDE x(REAL)
        SUCH THAT {lhs_sql} <= {rhs_sql}
        MAXIMIZE SUM(x * ps_availqty)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(ps_partkey AS BIGINT),
               CAST(ps_availqty AS DOUBLE)
        FROM partsupp WHERE ps_partkey < 10
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model(f"perrow_linear_{lhs_sql.replace(' ', '')}")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.CONTINUOUS, lb=0.0)
        oracle_solver.add_constraint({vn: coefficient}, "<=", float(rhs_sql) - constant)
    oracle_solver.set_objective(
        {vnames[i]: data[i][1] for i in range(len(data))},
        ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_result, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[decidb_cols.index("ps_availqty")])},
    )
    perf_tracker.record(
        f"perrow_linear_{lhs_sql.replace(' ', '')}", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.var_real
@pytest.mark.cons_perrow
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_perrow_unary_minus_lower_bound(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Unary minus on LHS: `-x <= -K` means `x >= K`. The oracle states `-x_i <= -2` on every row."""
    sql = """
        SELECT ps_partkey, ps_supplycost, x
        FROM partsupp WHERE ps_partkey < 10
        DECIDE x(REAL)
        SUCH THAT -x <= -2
            AND x <= 10
        MAXIMIZE SUM(x)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(ps_partkey AS BIGINT),
               CAST(ps_supplycost AS DOUBLE)
        FROM partsupp WHERE ps_partkey < 10
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("perrow_unary_minus")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.CONTINUOUS, lb=0.0, ub=10.0)
        oracle_solver.add_constraint({vn: -1.0}, "<=", -2.0)
    oracle_solver.set_objective(
        {vnames[i]: 1.0 for i in range(len(data))},
        ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_result, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": 1.0},
    )
    perf_tracker.record(
        "perrow_unary_minus", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 2,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.var_real
@pytest.mark.cons_perrow
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_perrow_data_column_in_lhs(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """`x - col <= K` must bound x per row by `K + col[row]`. Before Fix A, the
    data column was silently dropped and every row got x = K."""
    sql = """
        SELECT ps_partkey, ps_availqty, x
        FROM partsupp WHERE ps_partkey < 10
        DECIDE x(REAL)
        SUCH THAT x - ps_availqty <= 1
        MAXIMIZE SUM(x)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(ps_partkey AS BIGINT),
               CAST(ps_availqty AS DOUBLE)
        FROM partsupp WHERE ps_partkey < 10
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("perrow_data_column")
    vnames = [f"x_{i}" for i in range(len(data))]
    # x_i - ps_availqty[i] <= 1, with the row's own number on the left
    for i, vn in enumerate(vnames):
        oracle_solver.add_variable(vn, VarType.CONTINUOUS, lb=0.0)
        oracle_solver.add_constraint({vn: 1.0}, "<=", 1.0 + data[i][1])
    oracle_solver.set_objective(
        {vnames[i]: 1.0 for i in range(len(data))},
        ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_result, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": 1.0},
    )
    perf_tracker.record(
        "perrow_data_column", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


# --- Edge cases around per-row linear LHS (defensive lock-ins) ---
# Each of these was probed manually and produces the right answer; these tests
# pin the behavior so regressions surface at CI time.

@pytest.mark.var_real
@pytest.mark.cons_perrow
@pytest.mark.correctness
def test_perrow_negative_constant_divisor(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """`x / -2 <= 1` is equivalent to `-0.5 * x <= 1`, i.e., `x >= -2`. The
    coefficient-scaling path must produce the correct signed coefficient — a
    naive drop of the sign would silently flip the inequality."""
    sql = """
        SELECT ps_partkey, ps_availqty, x
        FROM partsupp WHERE ps_partkey < 10
        DECIDE x(REAL)
        SUCH THAT x / (-2) <= 1
            AND x <= 10
        MAXIMIZE SUM(x)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(ps_partkey AS BIGINT), CAST(ps_availqty AS DOUBLE)
        FROM partsupp WHERE ps_partkey < 10
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("perrow_neg_divisor")
    vnames = [f"x_{i}" for i in range(len(data))]
    # x / -2 <= 1 is -0.5 * x_i <= 1 on every row; the query writes x <= 10.
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.CONTINUOUS, lb=0.0, ub=10.0)
        oracle_solver.add_constraint({vn: -0.5}, "<=", 1.0)
    oracle_solver.set_objective(
        {vnames[i]: 1.0 for i in range(len(data))}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()
    cmp = compare_solutions(
        decidb_result, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": 1.0},
    )
    perf_tracker.record(
        "perrow_neg_divisor", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 2,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.var_integer
@pytest.mark.cons_perrow
@pytest.mark.correctness
def test_perrow_strict_lt_with_offset(decidb_cli, duckdb_conn, scope_solver):
    """`x + 3 < 10` with integer x: strict-< on integers becomes `<= K-1`. Checks that
    RHS adjustment and strict-inequality handling compose. The scope oracle states the
    clause as written (`<` with the offset on the left) and applies the language's
    strict rule itself."""
    sql = """
        SELECT ps_partkey, ps_suppkey, ps_availqty, x
        FROM partsupp
        WHERE ps_partkey < 10
        DECIDE x(INT)
        SUCH THAT x + 3 < 10
        MAXIMIZE SUM(x * ps_availqty)
        ORDER BY ps_partkey, ps_suppkey
    """
    rows = [
        dict(zip(("ps_partkey", "ps_suppkey", "ps_availqty"), row))
        for row in duckdb_conn.execute(
            "SELECT ps_partkey, ps_suppkey, ps_availqty FROM partsupp WHERE ps_partkey < 10 ORDER BY ps_partkey, ps_suppkey"
        ).fetchall()
    ]
    model = ScopedModel(rows, [Decision("x")])
    clauses = [Scoped((lambda r: {"x": 1, CONSTANT: 3},), "<", (lambda r: {CONSTANT: 10},))]
    objective = ScopedObjective(ObjSense.MAXIMIZE, (Reduce("sum", lambda r: {"x": r["ps_availqty"]}),))
    assert model.judge_scoped(answer(decidb_cli, sql), clauses, objective) == pytest.approx(
        model.solve_scoped(scope_solver, clauses, objective), abs=1e-4
    )


@pytest.mark.var_integer
@pytest.mark.cons_perrow
@pytest.mark.correctness
def test_perrow_integer_with_fractional_coef(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """`x / 3 <= 1` with integer x. The extracted coefficient is 1/3 (fractional)
    but the variable is integer-typed; the solver must floor correctly
    (x <= 3). Guards against the integer-LHS detector downgrading incorrectly
    when constants moved to RHS produce a fractional adjusted coefficient."""
    sql = """
        SELECT ps_partkey, ps_availqty, x
        FROM partsupp WHERE ps_partkey < 10
        DECIDE x(INT)
        SUCH THAT x / 3 <= 1
        MAXIMIZE SUM(x * ps_availqty)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(ps_partkey AS BIGINT), CAST(ps_availqty AS DOUBLE)
        FROM partsupp WHERE ps_partkey < 10
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("perrow_int_fractional_coef")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.INTEGER, lb=0.0)
        oracle_solver.add_constraint({vn: 1.0 / 3.0}, "<=", 1.0)
    oracle_solver.set_objective(
        {vnames[i]: data[i][1] for i in range(len(data))}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()
    cmp = compare_solutions(
        decidb_result, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[decidb_cols.index("ps_availqty")])},
    )
    perf_tracker.record(
        "perrow_int_fractional_coef", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.var_integer
@pytest.mark.cons_perrow
@pytest.mark.when_row
@pytest.mark.correctness
def test_perrow_when_plus_linear_lhs(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """WHEN + linear LHS: `when flag: x + 3 <= 10` constrains only matching rows.
    Non-matching rows fall back to the explicit `x <= 20` bound."""
    sql = """
        SELECT ps_partkey, ps_availqty, x
        FROM partsupp WHERE ps_partkey < 10
        DECIDE x(INT)
        SUCH THAT when (ps_partkey % 2 = 0): x + 3 <= 10
            AND x <= 20
        MAXIMIZE SUM(x * ps_availqty)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(ps_partkey AS BIGINT), CAST(ps_availqty AS DOUBLE)
        FROM partsupp WHERE ps_partkey < 10
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("perrow_when_linear")
    vnames = [f"x_{i}" for i in range(len(data))]
    for i, vn in enumerate(vnames):
        # The query writes x <= 20 for every row, and x + 3 <= 10 on the rows `when` keeps.
        oracle_solver.add_variable(vn, VarType.INTEGER, lb=0.0, ub=20.0)
        if int(data[i][0]) % 2 == 0:
            oracle_solver.add_constraint({vn: 1.0}, "<=", 10.0 - 3.0)
    oracle_solver.set_objective(
        {vnames[i]: data[i][1] for i in range(len(data))}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()
    cmp = compare_solutions(
        decidb_result, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[decidb_cols.index("ps_availqty")])},
    )
    perf_tracker.record(
        "perrow_when_linear", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 2,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.cons_perrow
@pytest.mark.error
def test_perrow_division_by_zero_column_errors_cleanly(decidb_cli):
    """When a coefficient expression evaluates to NaN/Infinity at some row
    (e.g., `x / col` with a zero in the column), the NaN guard must fire
    instead of silently feeding garbage to the solver."""
    decidb_cli.assert_error("""
        WITH data AS (
            SELECT 1 AS id, 2.0 AS w UNION ALL
            SELECT 2, 0.0 UNION ALL
            SELECT 3, 1.0
        )
        SELECT id, x FROM data
        DECIDE x(REAL)
        SUCH THAT x / w <= 1
            AND x <= 5
        MAXIMIZE SUM(x)
    """, match=r"NaN|Infinity|Division by zero")
