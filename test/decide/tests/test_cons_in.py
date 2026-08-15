"""Tests for IN (...) constraints on decision variables.

``x IN (v1, ..., vK)`` restricts the variable's domain to a discrete set.
DecidB rewrites this at bind time into K binary indicator variables z_k with
``SUM(z_k) = 1`` (cardinality) and ``x = SUM(v_k * z_k)`` (linking). The oracle
mirrors the same construction via ``add_in_domain``.
"""

import time

import pytest

from solver.types import VarType, ObjSense
from comparison.compare import compare_solutions
from ._oracle_helpers import add_in_domain


def _common_lineitem(duckdb_conn, where_clause):
    return duckdb_conn.execute(f"""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_extendedprice AS DOUBLE),
               CAST(l_quantity AS DOUBLE)
        FROM lineitem WHERE {where_clause}
    """).fetchall()


def _build_in_domain_model(oracle, test_id, data, domain, big_M,
                           when_mask=None, explicit_bool=False, lb=0.0):
    """Create vars for each row, apply IN (domain), return var name list.

    `lb` mirrors DecidB's own floor: a decision variable is non-negative unless
    something lowers it, and a domain containing a negative value is what lowers
    it (see the negative-minimum branch of the IN rewrite).
    """
    oracle.create_model(test_id)
    n = len(data)
    vnames = [f"x_{i}" for i in range(n)]
    for i, v in enumerate(vnames):
        if explicit_bool:
            oracle.add_variable(v, VarType.BINARY)
        else:
            oracle.add_variable(v, VarType.INTEGER, lb=lb, ub=big_M)
    if not explicit_bool:
        for i, v in enumerate(vnames):
            if when_mask is None or when_mask[i]:
                add_in_domain(oracle, v, domain, name_prefix=f"in_{i}")
    return vnames


@pytest.mark.cons_in
@pytest.mark.var_integer
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_in_domain_restriction(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """x IN (0, 1, 3) — restrict integer variable to a sparse domain."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity, x
        FROM lineitem WHERE l_orderkey < 50
        DECIDE x(INT)
        SUCH THAT x IN (0, 1, 3)
            AND SUM(x * l_quantity) <= 200
        MAXIMIZE SUM(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = _common_lineitem(duckdb_conn, "l_orderkey < 50")
    n = len(data)

    t_build = time.perf_counter()
    vnames = _build_in_domain_model(
        oracle_solver, "in_domain_restriction", data, [0, 1, 3], big_M=3.0,
    )
    oracle_solver.add_constraint(
        {vnames[i]: data[i][3] for i in range(n)},
        "<=", 200.0, name="quantity_cap",
    )
    oracle_solver.set_objective(
        {vnames[i]: data[i][2] for i in range(n)}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    x_idx = decidb_cols.index("x")
    for row in decidb_rows:
        assert row[x_idx] in (0, 1, 3), f"x={row[x_idx]} not in allowed domain"

    cmp = compare_solutions(
        decidb_rows, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[decidb_cols.index("l_extendedprice")])},
    )
    perf_tracker.record(
        "in_domain_restriction", decidb_time, build_time, result.solve_time_seconds,
        n, n * 4, 2, result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.cons_in
@pytest.mark.var_boolean
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_in_binary_domain(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """x IN (0, 1) on an implicitly typed variable — equivalent to(BOOL)."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity, x
        FROM lineitem WHERE l_orderkey < 50
        DECIDE x(INT)
        SUCH THAT x IN (0, 1)
            AND SUM(x * l_quantity) <= 100
        MAXIMIZE SUM(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = _common_lineitem(duckdb_conn, "l_orderkey < 50")
    n = len(data)

    t_build = time.perf_counter()
    vnames = _build_in_domain_model(
        oracle_solver, "in_binary_domain", data, [0, 1], big_M=1.0,
    )
    oracle_solver.add_constraint(
        {vnames[i]: data[i][3] for i in range(n)},
        "<=", 100.0, name="quantity_cap",
    )
    oracle_solver.set_objective(
        {vnames[i]: data[i][2] for i in range(n)}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    x_idx = decidb_cols.index("x")
    for row in decidb_rows:
        assert row[x_idx] in (0, 1), f"x={row[x_idx]} not in {{0, 1}}"

    cmp = compare_solutions(
        decidb_rows, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[decidb_cols.index("l_extendedprice")])},
    )
    perf_tracker.record(
        "in_binary_domain", decidb_time, build_time, result.solve_time_seconds,
        n, n * 3, 2, result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.cons_in
@pytest.mark.var_integer
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_in_single_value(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """x IN (3) — single-value IN, equivalent to x = 3."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_quantity, x
        FROM lineitem WHERE l_orderkey < 50
        DECIDE x(INT)
        SUCH THAT x IN (3)
        MINIMIZE SUM(x * l_quantity)
    """
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = _common_lineitem(duckdb_conn, "l_orderkey < 50")
    n = len(data)

    t_build = time.perf_counter()
    vnames = _build_in_domain_model(
        oracle_solver, "in_single_value", data, [3], big_M=3.0,
    )
    oracle_solver.set_objective(
        {vnames[i]: data[i][3] for i in range(n)}, ObjSense.MINIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    x_idx = decidb_cols.index("x")
    for row in decidb_rows:
        assert row[x_idx] == 3, f"x={row[x_idx]}, expected 3"

    cmp = compare_solutions(
        decidb_rows, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[decidb_cols.index("l_quantity")])},
    )
    perf_tracker.record(
        "in_single_value", decidb_time, build_time, result.solve_time_seconds,
        n, n * 2, 0, result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.cons_in
@pytest.mark.var_integer
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_in_minimize_picks_smallest(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """x IN (3, 5, 7) MINIMIZE SUM(x) — solver should pick 3 for all rows."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_quantity, x
        FROM lineitem WHERE l_orderkey < 50
        DECIDE x(INT)
        SUCH THAT x IN (3, 5, 7)
        MINIMIZE SUM(x * l_quantity)
    """
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = _common_lineitem(duckdb_conn, "l_orderkey < 50")
    n = len(data)

    t_build = time.perf_counter()
    vnames = _build_in_domain_model(
        oracle_solver, "in_minimize_picks_smallest", data, [3, 5, 7], big_M=7.0,
    )
    oracle_solver.set_objective(
        {vnames[i]: data[i][3] for i in range(n)}, ObjSense.MINIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    x_idx = decidb_cols.index("x")
    for row in decidb_rows:
        assert row[x_idx] in (3, 5, 7)
        assert row[x_idx] == 3, f"MINIMIZE should pick smallest: x={row[x_idx]}"

    cmp = compare_solutions(
        decidb_rows, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[decidb_cols.index("l_quantity")])},
    )
    perf_tracker.record(
        "in_minimize_picks_smallest", decidb_time, build_time, result.solve_time_seconds,
        n, n * 4, 0, result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.cons_in
@pytest.mark.var_integer
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_in_negative_domain_lowers_floor(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """x IN (-5, 3, 7) MINIMIZE — a negative domain value must stay reachable.

    A decision variable's floor is 0 unless something lowers it. A domain whose
    minimum is negative is what lowers it, so -5 is only selectable if that floor
    actually moves. If it does not, the model is left with x >= 0 and the solver
    settles for 3 — a wrong answer rather than an error, which is why this is
    pinned against the oracle rather than against the model dump.
    """
    sql = """
        SELECT l_orderkey, l_linenumber, l_quantity, x
        FROM lineitem WHERE l_orderkey < 50
        DECIDE x(INT)
        SUCH THAT x IN (-5, 3, 7)
        MINIMIZE SUM(x * l_quantity)
    """
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = _common_lineitem(duckdb_conn, "l_orderkey < 50")
    n = len(data)

    t_build = time.perf_counter()
    vnames = _build_in_domain_model(
        oracle_solver, "in_negative_domain_lowers_floor", data, [-5, 3, 7],
        big_M=7.0, lb=-5.0,
    )
    oracle_solver.set_objective(
        {vnames[i]: data[i][3] for i in range(n)}, ObjSense.MINIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    x_idx = decidb_cols.index("x")
    for row in decidb_rows:
        assert row[x_idx] in (-5, 3, 7), f"x escaped the domain: x={row[x_idx]}"
        assert row[x_idx] == -5, (
            f"MINIMIZE over a non-negative-coefficient objective should pick the "
            f"smallest domain value: x={row[x_idx]}"
        )

    cmp = compare_solutions(
        decidb_rows, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[decidb_cols.index("l_quantity")])},
    )
    perf_tracker.record(
        "in_negative_domain_lowers_floor", decidb_time, build_time, result.solve_time_seconds,
        n, n * 4, 0, result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.cons_in
@pytest.mark.var_integer
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_in_maximize_picks_largest(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """x IN (2, 5) MAXIMIZE SUM(x * extendedprice) with a loose aggregate cap."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity, x
        FROM lineitem WHERE l_orderkey < 50
        DECIDE x(INT)
        SUCH THAT x IN (2, 5)
            AND SUM(x * l_quantity) <= 99999
        MAXIMIZE SUM(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = _common_lineitem(duckdb_conn, "l_orderkey < 50")
    n = len(data)

    t_build = time.perf_counter()
    vnames = _build_in_domain_model(
        oracle_solver, "in_maximize_picks_largest", data, [2, 5], big_M=5.0,
    )
    oracle_solver.add_constraint(
        {vnames[i]: data[i][3] for i in range(n)},
        "<=", 99999.0, name="quantity_cap",
    )
    oracle_solver.set_objective(
        {vnames[i]: data[i][2] for i in range(n)}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    x_idx = decidb_cols.index("x")
    for row in decidb_rows:
        assert row[x_idx] in (2, 5), f"x={row[x_idx]} not in domain"

    cmp = compare_solutions(
        decidb_rows, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[decidb_cols.index("l_extendedprice")])},
    )
    perf_tracker.record(
        "in_maximize_picks_largest", decidb_time, build_time, result.solve_time_seconds,
        n, n * 3, 2, result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.cons_in
@pytest.mark.var_integer
@pytest.mark.when
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_in_with_when(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """x IN (0, 2, 4) WHEN condition — IN restriction gated by a row filter."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity, x
        FROM lineitem WHERE l_orderkey < 50
        DECIDE x(INT)
        SUCH THAT x IN (0, 2, 4) WHEN l_quantity > 20
            AND SUM(x * l_quantity) <= 500
        MAXIMIZE SUM(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = _common_lineitem(duckdb_conn, "l_orderkey < 50")
    n = len(data)
    mask = [row[3] > 20 for row in data]

    t_build = time.perf_counter()
    vnames = _build_in_domain_model(
        oracle_solver, "in_with_when", data, [0, 2, 4], big_M=4.0,
        when_mask=mask,
    )
    oracle_solver.add_constraint(
        {vnames[i]: data[i][3] for i in range(n)},
        "<=", 500.0, name="quantity_cap",
    )
    oracle_solver.set_objective(
        {vnames[i]: data[i][2] for i in range(n)}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    x_idx = decidb_cols.index("x")
    qty_idx = decidb_cols.index("l_quantity")
    for row in decidb_rows:
        if row[qty_idx] > 20:
            assert row[x_idx] in (0, 2, 4), \
                f"x={row[x_idx]} not in domain when l_quantity={row[qty_idx]} > 20"

    cmp = compare_solutions(
        decidb_rows, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[decidb_cols.index("l_extendedprice")])},
    )
    perf_tracker.record(
        "in_with_when", decidb_time, build_time, result.solve_time_seconds,
        n, n * 4, 2, result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.cons_in
@pytest.mark.var_boolean
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_in_boolean_explicit(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """x(BOOL) with x IN (0, 1) — trivially satisfied, no auxiliary vars needed."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity, x
        FROM lineitem WHERE l_orderkey < 50
        DECIDE x(BOOL)
        SUCH THAT x IN (0, 1)
            AND SUM(x) <= 10
        MAXIMIZE SUM(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = _common_lineitem(duckdb_conn, "l_orderkey < 50")
    n = len(data)

    t_build = time.perf_counter()
    vnames = _build_in_domain_model(
        oracle_solver, "in_boolean_explicit", data, [0, 1], big_M=1.0,
        explicit_bool=True,
    )
    oracle_solver.add_constraint(
        {v: 1.0 for v in vnames}, "<=", 10.0, name="count_cap",
    )
    oracle_solver.set_objective(
        {vnames[i]: data[i][2] for i in range(n)}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    x_idx = decidb_cols.index("x")
    for row in decidb_rows:
        assert row[x_idx] in (0, 1), f"x={row[x_idx]} not in {{0, 1}}"

    cmp = compare_solutions(
        decidb_rows, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[decidb_cols.index("l_extendedprice")])},
    )
    perf_tracker.record(
        "in_boolean_explicit", decidb_time, build_time, result.solve_time_seconds,
        n, n, 1, result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.cons_in
@pytest.mark.var_real
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_real_in_oracle(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """x IN (non-integer values) on a REAL variable.

    Regression test for the integer-step rewrite sweep. DecidB rewrites IN into
    binary-indicator cardinality + linking `=` (no discretization of `x`), so
    REAL variables with fractional domain values must solve correctly. Also
    verifies each row's ``x`` is exactly one of the allowed values.
    """
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, x
        FROM lineitem WHERE l_orderkey < 15
        DECIDE x(REAL)
        SUCH THAT x IN (0.5, 1.5, 2.75)
            AND SUM(x) <= 20
        MAXIMIZE SUM(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    decidb_rows, decidb_cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_extendedprice AS DOUBLE)
        FROM lineitem WHERE l_orderkey < 15
    """).fetchall()
    n = len(data)

    t_build = time.perf_counter()
    oracle_solver.create_model("real_in_domain")
    vnames = [f"x_{i}" for i in range(n)]
    for v in vnames:
        oracle_solver.add_variable(v, VarType.CONTINUOUS, lb=0.0, ub=2.75)
    for i, v in enumerate(vnames):
        add_in_domain(oracle_solver, v, [0.5, 1.5, 2.75], name_prefix=f"in_{i}")
    oracle_solver.add_constraint(
        {vnames[i]: 1.0 for i in range(n)}, "<=", 20.0, name="sum_cap",
    )
    oracle_solver.set_objective(
        {vnames[i]: data[i][2] for i in range(n)}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    x_idx = decidb_cols.index("x")
    allowed = {0.5, 1.5, 2.75}
    for row in decidb_rows:
        assert any(abs(float(row[x_idx]) - a) < 1e-6 for a in allowed), (
            f"x={row[x_idx]} not in {allowed}"
        )

    cmp = compare_solutions(
        decidb_rows, decidb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[decidb_cols.index("l_extendedprice")])},
    )
    perf_tracker.record(
        "real_in_oracle", decidb_time, build_time, result.solve_time_seconds,
        n, n * 4, 1 + n, result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )
