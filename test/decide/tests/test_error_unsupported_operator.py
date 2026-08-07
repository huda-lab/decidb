"""Data-only unsupported operators fold to per-row coefficients; only
variable-bearing uses of an unsupported operator are rejected.

The symbolic translator models a fixed set of arithmetic operators (`+ - * /
^ **`). An operator outside that set (`%`, bitwise ops, …) used inside a DECIDE
SUM body used to be rejected unconditionally — even when its operands were pure
data. But a subexpression that references no DECIDE variable is a per-row
*constant coefficient*: `(id * 7) % 97` is data the moment the row is known.
Those now fold to a data placeholder (see `ToSymbolicRecursive`) and are
evaluated as ordinary data by the physical layer, exactly like `id * 7`.

Contract pinned here:
  - A data-only unsupported operator inside a SUM body succeeds and produces the
    correct optimum (verified against an independent oracle solver).
  - An unsupported operator that *does* touch a DECIDE variable is still rejected
    with a single-line, actionable error — never a C++ stack trace / INTERNAL.
"""

import time

import pytest

from solver.types import VarType, ObjSense
from comparison.compare import compare_solutions


@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_modulo_data_coefficient_matches_oracle(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """`((id * 7) % 97)` is a data-only coefficient. Maximizing over it with a
    cardinality cap must match an oracle fed the same per-row coefficients."""
    sql = """
        SELECT id, x FROM range(1, 5) t(id)
        DECIDE x(BOOL)
        SUCH THAT SUM(x) <= 2
        MAXIMIZE SUM(((id * 7) % 97) * x)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    # Per-row objective coefficient computed independently (model input, not the answer).
    data = duckdb_conn.execute("""
        SELECT CAST(id AS BIGINT), CAST(((id * 7) % 97) AS DOUBLE)
        FROM range(1, 5) t(id)
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("modulo_data_coeff")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)
    # Constraint: SUM(x) <= 2
    oracle_solver.add_constraint(
        {vnames[i]: 1.0 for i in range(len(data))}, "<=", 2.0, name="cap",
    )
    # Objective: MAXIMIZE SUM(((id*7)%97) * x)
    oracle_solver.set_objective(
        {vnames[i]: data[i][1] for i in range(len(data))}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_result, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float((int(row[decidb_cols.index("id")]) * 7) % 97)},
    )

    perf_tracker.record(
        "modulo_data_coefficient", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.correctness
def test_modulo_data_coefficient_in_constraint_runs(decidb_cli):
    """A data-only `%` coefficient in a constraint LHS binds cleanly (no crash).
    Every coefficient `((id*7)%97)` exceeds the RHS of 3, so the constraint
    forces all rows off — a valid, checkable feasibility outcome."""
    rows, cols = decidb_cli.execute("""
        SELECT id, x FROM range(1, 5) t(id)
        DECIDE x(BOOL)
        SUCH THAT SUM(((id * 7) % 97) * x) <= 3
        MAXIMIZE SUM(x)
    """)
    x_idx = cols.index("x")
    id_idx = cols.index("id")
    # The returned assignment must satisfy the stated constraint.
    lhs = sum(((int(r[id_idx]) * 7) % 97) * int(r[x_idx]) for r in rows)
    assert lhs <= 3


@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_mod_function_data_coefficient_matches_oracle(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """`mod(id, 5)` is a data-only coefficient in *function* form (not the `%`
    operator). Same contract as the operator case: it folds to a per-row
    coefficient and the optimum matches an independent oracle.

    Regression pin for the data-only scalar-function fold — before it,
    `ToSymbolicRecursive` raised INTERNAL on named data-only functions."""
    sql = """
        SELECT id, x FROM range(1, 6) t(id)
        DECIDE x(BOOL)
        SUCH THAT SUM(x) <= 2
        MAXIMIZE SUM(mod(id, 5) * x)
    """
    t0 = time.perf_counter()
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(id AS BIGINT), CAST(mod(id, 5) AS DOUBLE)
        FROM range(1, 6) t(id)
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("mod_fn_data_coeff")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)
    oracle_solver.add_constraint(
        {vnames[i]: 1.0 for i in range(len(data))}, "<=", 2.0, name="cap",
    )
    oracle_solver.set_objective(
        {vnames[i]: data[i][1] for i in range(len(data))}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        decidb_result, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(int(row[decidb_cols.index("id")]) % 5)},
    )

    perf_tracker.record(
        "mod_function_data_coefficient", decidb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.correctness
def test_floor_function_data_coefficient_in_constraint(decidb_cli):
    """`floor(v)` (data-only named function) as a constraint coefficient binds
    cleanly and the returned assignment satisfies the stated bound."""
    rows, cols = decidb_cli.execute("""
        SELECT id, v, x FROM (VALUES (1, 10.7), (2, 20.3), (3, 5.9)) t(id, v)
        DECIDE x(BOOL)
        SUCH THAT SUM(floor(v) * x) <= 16
        MAXIMIZE SUM(x)
    """)
    ci = {name: i for i, name in enumerate(cols)}
    import math
    lhs = sum(math.floor(float(r[ci["v"]])) * int(r[ci["x"]]) for r in rows)
    assert lhs <= 16, f"constraint violated: floor-weighted LHS = {lhs}"


@pytest.mark.error
class TestUnsupportedOperatorOverVariableRejection:
    """An unsupported operator that touches a DECIDE variable is still rejected
    with a friendly, single-line error — never a crash."""

    def test_modulo_over_decide_variable_is_rejected(self, decidb_cli):
        """`x % 97` genuinely applies `%` to a decision variable → not foldable."""
        decidb_cli.assert_error("""
            SELECT id, x FROM range(1, 5) t(id)
            DECIDE x(INT)
            SUCH THAT SUM((x % 97)) <= 3
            MAXIMIZE SUM(x)
        """, match=r"(?i)'%'")

    def test_scalar_function_over_decide_variable_is_rejected(self, decidb_cli):
        """A named scalar function wrapping a decision variable (`mod(x, 5)`) is
        non-linear → rejected with a friendly, actionable error (the data-only
        fold must NOT swallow variable-bearing functions)."""
        decidb_cli.assert_error("""
            SELECT id, x FROM range(1, 5) t(id)
            DECIDE x(INT)
            SUCH THAT SUM(mod(x, 5)) <= 3
            MAXIMIZE SUM(x)
        """, match=r"(?i)'mod'.*DECIDE variable")

    def test_variable_bearing_modulo_does_not_emit_stack_trace(self, decidb_cli):
        """The load-bearing symptom: the rejection path stays a friendly error,
        never a C++ trace / INTERNAL Error."""
        sql = """
            SELECT id, x FROM range(1, 5) t(id)
            DECIDE x(INT)
            SUCH THAT SUM(((id * 7) % x)) <= 3
            MAXIMIZE SUM(x)
        """
        result = decidb_cli.execute_raw(sql)
        combined = result.stderr + result.stdout
        forbidden = [
            "INTERNAL Error",
            "Stack Trace",
            "ToSymbolicRecursive",
            "assertion failure",
        ]
        for token in forbidden:
            assert token not in combined, (
                f"Found {token!r} in output — operator rejection regressed to "
                f"the internal-error path.\n"
                f"stdout: {result.stdout[:500]}\n"
                f"stderr: {result.stderr[:500]}"
            )
