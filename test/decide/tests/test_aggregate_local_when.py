"""Tests for aggregate-local WHEN filters inside DECIDE aggregate expressions.

``SUM(x * v) WHEN cond1 + SUM(x * v) WHEN cond2`` applies each WHEN only to
its own aggregate term. A row matching both contributes to both; a row
matching neither drops out of both. In the oracle we pre-compute a row's
*effective* coefficient as the sum across all mask-gated terms:
  coeff_i = Σ_k (v_i * mask_k(row_i))
and then call ``add_constraint`` / ``set_objective`` with the merged dict.
"""

import time
from collections import defaultdict

import pytest

from solver.types import VarType, ObjSense
from comparison.compare import compare_solutions
from ._oracle_helpers import (
    add_bool_and, add_ne_indicator, group_indices,
)


# ---------------------------------------------------------------------------
# Utility: build one merged coefficient dict from several mask-gated terms
# ---------------------------------------------------------------------------

def _accumulate(coeffs: dict, key: str, val: float) -> None:
    if val == 0.0:
        return
    coeffs[key] = coeffs.get(key, 0.0) + val


def _run_constraint_test(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker,
    *, test_id, decide_sql, data_sql, build_oracle, decidb_obj_fn,
):
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
# Core aggregate-local WHEN tests
# ===========================================================================

@pytest.mark.when
@pytest.mark.when_constraint
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_aggregate_local_when_constraint_independent_masks(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """SUM(x*v) WHEN w1 + SUM(x*v) WHEN w2 <= 6: row c (neither flag) is free."""
    data_sql = """
        SELECT CAST(name AS VARCHAR), CAST(value AS DOUBLE),
               CAST(w1 AS BOOLEAN), CAST(w2 AS BOOLEAN) FROM (
            VALUES ('a', 6, true, false),
                   ('b', 4, false, true),
                   ('c', 10, false, false)
        ) t(name, value, w1, w2)
    """
    decide_sql = """
        SELECT name, value, w1, w2, x FROM (
            VALUES ('a', 6, true, false),
                   ('b', 4, false, true),
                   ('c', 10, false, false)
        ) t(name, value, w1, w2)
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * value) WHEN w1 + SUM(x * value) WHEN w2 <= 6
        MAXIMIZE SUM(x * value)
    """

    def build(oracle, data, cols, rows):
        n = len(data)
        vnames = [f"x_{i}" for i in range(n)]
        for v in vnames:
            oracle.add_variable(v, VarType.BINARY)
        coeffs: dict = {}
        for i, row in enumerate(data):
            mask = (1.0 if row[2] else 0.0) + (1.0 if row[3] else 0.0)
            _accumulate(coeffs, vnames[i], row[1] * mask)
        oracle.add_constraint(coeffs, "<=", 6.0, name="local_when")
        oracle.set_objective(
            {vnames[i]: data[i][1] for i in range(n)}, ObjSense.MAXIMIZE,
        )
        return n, 1

    def decidb_obj(rs, cs):
        xi = cs.index("x"); vi = cs.index("value")
        return sum(float(r[xi]) * float(r[vi]) for r in rs)

    _run_constraint_test(
        decidb_cli, duckdb_conn, oracle_solver, perf_tracker,
        test_id="alw_independent_masks",
        decide_sql=decide_sql, data_sql=data_sql,
        build_oracle=build, decidb_obj_fn=decidb_obj,
    )


@pytest.mark.when
@pytest.mark.when_constraint
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_aggregate_local_when_constraint_parenthesized_condition(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """Parenthesized comparison in WHEN condition."""
    data_sql = """
        SELECT CAST(name AS VARCHAR), CAST(value AS DOUBLE), CAST(tier AS VARCHAR) FROM (
            VALUES ('a', 7, 'high'), ('b', 3, 'low'), ('c', 9, 'none')
        ) t(name, value, tier)
    """
    decide_sql = """
        SELECT name, value, tier, x FROM (
            VALUES ('a', 7, 'high'), ('b', 3, 'low'), ('c', 9, 'none')
        ) t(name, value, tier)
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * value) WHEN (tier = 'high') + SUM(x * value) WHEN (tier = 'low') <= 7
        MAXIMIZE SUM(x * value)
    """

    def build(oracle, data, cols, rows):
        n = len(data)
        vnames = [f"x_{i}" for i in range(n)]
        for v in vnames:
            oracle.add_variable(v, VarType.BINARY)
        coeffs: dict = {}
        for i, row in enumerate(data):
            mask = (1.0 if row[2] == "high" else 0.0) + (1.0 if row[2] == "low" else 0.0)
            _accumulate(coeffs, vnames[i], row[1] * mask)
        oracle.add_constraint(coeffs, "<=", 7.0, name="local_when")
        oracle.set_objective(
            {vnames[i]: data[i][1] for i in range(n)}, ObjSense.MAXIMIZE,
        )
        return n, 1

    def decidb_obj(rs, cs):
        xi = cs.index("x"); vi = cs.index("value")
        return sum(float(r[xi]) * float(r[vi]) for r in rs)

    _run_constraint_test(
        decidb_cli, duckdb_conn, oracle_solver, perf_tracker,
        test_id="alw_paren_cond", decide_sql=decide_sql, data_sql=data_sql,
        build_oracle=build, decidb_obj_fn=decidb_obj,
    )


@pytest.mark.when
@pytest.mark.when_objective
@pytest.mark.correctness
def test_aggregate_local_when_objective_independent_masks(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """Objective: SUM(x*value) WHEN w1 + SUM(x*bonus) WHEN w2."""
    data_sql = """
        SELECT CAST(name AS VARCHAR), CAST(value AS DOUBLE),
               CAST(bonus AS DOUBLE), CAST(w1 AS BOOLEAN), CAST(w2 AS BOOLEAN) FROM (
            VALUES ('a', 10, 0, true, false),
                   ('b', 0, 9, false, true),
                   ('c', 8, 8, false, false)
        ) t(name, value, bonus, w1, w2)
    """
    decide_sql = """
        SELECT name, value, bonus, w1, w2, x FROM (
            VALUES ('a', 10, 0, true, false),
                   ('b', 0, 9, false, true),
                   ('c', 8, 8, false, false)
        ) t(name, value, bonus, w1, w2)
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) <= 2
        MAXIMIZE SUM(x * value) WHEN w1 + SUM(x * bonus) WHEN w2
    """

    def build(oracle, data, cols, rows):
        n = len(data)
        vnames = [f"x_{i}" for i in range(n)]
        for v in vnames:
            oracle.add_variable(v, VarType.BINARY)
        oracle.add_constraint({v: 1.0 for v in vnames}, "<=", 2.0, name="total")
        obj: dict = {}
        for i, row in enumerate(data):
            term = row[1] * (1.0 if row[3] else 0.0) + row[2] * (1.0 if row[4] else 0.0)
            _accumulate(obj, vnames[i], term)
        oracle.set_objective(obj, ObjSense.MAXIMIZE)
        return n, 1

    def decidb_obj(rs, cs):
        xi = cs.index("x"); vi = cs.index("value")
        bi = cs.index("bonus"); w1i = cs.index("w1"); w2i = cs.index("w2")
        total = 0.0
        for r in rs:
            x = float(r[xi])
            total += x * float(r[vi]) * (1.0 if r[w1i] else 0.0)
            total += x * float(r[bi]) * (1.0 if r[w2i] else 0.0)
        return total

    _run_constraint_test(
        decidb_cli, duckdb_conn, oracle_solver, perf_tracker,
        test_id="alw_obj_indep", decide_sql=decide_sql, data_sql=data_sql,
        build_oracle=build, decidb_obj_fn=decidb_obj,
    )


@pytest.mark.when
@pytest.mark.when_constraint
@pytest.mark.correctness
def test_expression_level_when_still_works(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """Whole-expression (non-aggregate-local) WHEN stays available."""
    data_sql = """
        SELECT CAST(name AS VARCHAR), CAST(value AS DOUBLE), CAST(w1 AS BOOLEAN) FROM (
            VALUES ('a', 6, true), ('b', 4, true), ('c', 10, false)
        ) t(name, value, w1)
    """
    decide_sql = """
        SELECT name, value, w1, x FROM (
            VALUES ('a', 6, true), ('b', 4, true), ('c', 10, false)
        ) t(name, value, w1)
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * value) <= 6 WHEN w1
        MAXIMIZE SUM(x * value)
    """

    def build(oracle, data, cols, rows):
        n = len(data)
        vnames = [f"x_{i}" for i in range(n)]
        for v in vnames:
            oracle.add_variable(v, VarType.BINARY)
        coeffs = {vnames[i]: data[i][1] for i in range(n) if data[i][2]}
        oracle.add_constraint(coeffs, "<=", 6.0, name="expr_when")
        oracle.set_objective(
            {vnames[i]: data[i][1] for i in range(n)}, ObjSense.MAXIMIZE,
        )
        return n, 1

    def decidb_obj(rs, cs):
        xi = cs.index("x"); vi = cs.index("value")
        return sum(float(r[xi]) * float(r[vi]) for r in rs)

    _run_constraint_test(
        decidb_cli, duckdb_conn, oracle_solver, perf_tracker,
        test_id="expr_level_when_works",
        decide_sql=decide_sql, data_sql=data_sql,
        build_oracle=build, decidb_obj_fn=decidb_obj,
    )


@pytest.mark.when
@pytest.mark.when_constraint
@pytest.mark.error
@pytest.mark.error_binder
def test_expression_level_when_cannot_mix_with_aggregate_local_when(decidb_cli):
    """Mixed expression-level + aggregate-local WHEN is rejected."""
    decidb_cli.assert_error("""
        SELECT name, value, w1, w2, x FROM (
            VALUES ('a', 6, true, false), ('b', 4, false, true)
        ) t(name, value, w1, w2)
        DECIDE x IS BOOLEAN
        SUCH THAT (SUM(x * value) WHEN w1 + SUM(x * value) WHEN w2 <= 6) WHEN w1
        MAXIMIZE SUM(x * value)
    """, match=r"Cannot combine")


# ---------------------------------------------------------------------------
# A. Composition with other features
# ---------------------------------------------------------------------------

@pytest.mark.when
@pytest.mark.when_constraint
@pytest.mark.avg_rewrite
@pytest.mark.correctness
def test_aggregate_local_when_with_avg_constraint(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """AVG with aggregate-local WHEN: N = number of WHEN-matching rows."""
    data_sql = """
        SELECT CAST(name AS VARCHAR), CAST(value AS DOUBLE), CAST(active AS BOOLEAN) FROM (
            VALUES ('a', 12, true), ('b', 4, true), ('c', 8, false), ('d', 6, true)
        ) t(name, value, active)
    """
    decide_sql = """
        SELECT name, value, active, x FROM (
            VALUES ('a', 12, true), ('b', 4, true), ('c', 8, false), ('d', 6, true)
        ) t(name, value, active)
        DECIDE x IS BOOLEAN
        SUCH THAT AVG(x * value) WHEN active <= 5
        MAXIMIZE SUM(x * value)
    """

    def build(oracle, data, cols, rows):
        n = len(data)
        vnames = [f"x_{i}" for i in range(n)]
        for v in vnames:
            oracle.add_variable(v, VarType.BINARY)
        active_idx = [i for i, r in enumerate(data) if r[2]]
        oracle.add_constraint(
            {vnames[i]: data[i][1] for i in active_idx},
            "<=", 5.0 * len(active_idx), name="avg_active",
        )
        oracle.set_objective(
            {vnames[i]: data[i][1] for i in range(n)}, ObjSense.MAXIMIZE,
        )
        return n, 1

    def decidb_obj(rs, cs):
        xi = cs.index("x"); vi = cs.index("value")
        return sum(float(r[xi]) * float(r[vi]) for r in rs)

    _run_constraint_test(
        decidb_cli, duckdb_conn, oracle_solver, perf_tracker,
        test_id="alw_avg_constraint",
        decide_sql=decide_sql, data_sql=data_sql,
        build_oracle=build, decidb_obj_fn=decidb_obj,
    )


@pytest.mark.when
@pytest.mark.when_constraint
@pytest.mark.per_clause
@pytest.mark.correctness
def test_aggregate_local_when_with_per_constraint(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """Aggregate-local WHEN composes with PER for per-group filtered constraints."""
    data_sql = """
        SELECT CAST(name AS VARCHAR), CAST(value AS DOUBLE),
               CAST(grp AS VARCHAR), CAST(priority AS BOOLEAN) FROM (
            VALUES ('a', 10, 'X', true), ('b', 5, 'X', false),
                   ('c', 8, 'Y', true), ('d', 3, 'Y', false),
                   ('e', 7, 'X', true), ('f', 6, 'Y', true)
        ) t(name, value, grp, priority)
    """
    decide_sql = """
        SELECT name, value, grp, priority, x FROM (
            VALUES ('a', 10, 'X', true), ('b', 5, 'X', false),
                   ('c', 8, 'Y', true), ('d', 3, 'Y', false),
                   ('e', 7, 'X', true), ('f', 6, 'Y', true)
        ) t(name, value, grp, priority)
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * value) WHEN priority <= 12 PER grp
        MAXIMIZE SUM(x * value)
    """

    def build(oracle, data, cols, rows):
        n = len(data)
        vnames = [f"x_{i}" for i in range(n)]
        for v in vnames:
            oracle.add_variable(v, VarType.BINARY)
        for grp, idxs in group_indices(data, lambda r: r[2]).items():
            qualifying = [i for i in idxs if data[i][3]]
            oracle.add_constraint(
                {vnames[i]: data[i][1] for i in qualifying},
                "<=", 12.0, name=f"per_{grp}",
            )
        oracle.set_objective(
            {vnames[i]: data[i][1] for i in range(n)}, ObjSense.MAXIMIZE,
        )
        return n, 2

    def decidb_obj(rs, cs):
        xi = cs.index("x"); vi = cs.index("value")
        return sum(float(r[xi]) * float(r[vi]) for r in rs)

    _run_constraint_test(
        decidb_cli, duckdb_conn, oracle_solver, perf_tracker,
        test_id="alw_per_constraint",
        decide_sql=decide_sql, data_sql=data_sql,
        build_oracle=build, decidb_obj_fn=decidb_obj,
    )


@pytest.mark.when
@pytest.mark.when_constraint
@pytest.mark.avg_rewrite
@pytest.mark.per_clause
@pytest.mark.correctness
def test_aggregate_local_when_with_avg_and_per(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """AVG + WHEN + PER: N_g = per-group count of WHEN-qualifying rows."""
    data_sql = """
        SELECT CAST(name AS VARCHAR), CAST(value AS DOUBLE),
               CAST(grp AS VARCHAR), CAST(active AS BOOLEAN) FROM (
            VALUES ('a', 12, 'G1', true), ('b', 4, 'G1', true),
                   ('c', 3, 'G1', false), ('d', 10, 'G2', true),
                   ('e', 6, 'G2', true), ('f', 2, 'G2', false)
        ) t(name, value, grp, active)
    """
    decide_sql = """
        SELECT name, value, grp, active, x FROM (
            VALUES ('a', 12, 'G1', true), ('b', 4, 'G1', true),
                   ('c', 3, 'G1', false), ('d', 10, 'G2', true),
                   ('e', 6, 'G2', true), ('f', 2, 'G2', false)
        ) t(name, value, grp, active)
        DECIDE x IS BOOLEAN
        SUCH THAT AVG(x * value) WHEN active <= 6 PER grp
        MAXIMIZE SUM(x * value)
    """

    def build(oracle, data, cols, rows):
        n = len(data)
        vnames = [f"x_{i}" for i in range(n)]
        for v in vnames:
            oracle.add_variable(v, VarType.BINARY)
        for grp, idxs in group_indices(data, lambda r: r[2]).items():
            qualifying = [i for i in idxs if data[i][3]]
            if not qualifying:
                continue
            oracle.add_constraint(
                {vnames[i]: data[i][1] for i in qualifying},
                "<=", 6.0 * len(qualifying), name=f"avg_per_{grp}",
            )
        oracle.set_objective(
            {vnames[i]: data[i][1] for i in range(n)}, ObjSense.MAXIMIZE,
        )
        return n, 2

    def decidb_obj(rs, cs):
        xi = cs.index("x"); vi = cs.index("value")
        return sum(float(r[xi]) * float(r[vi]) for r in rs)

    _run_constraint_test(
        decidb_cli, duckdb_conn, oracle_solver, perf_tracker,
        test_id="alw_avg_per",
        decide_sql=decide_sql, data_sql=data_sql,
        build_oracle=build, decidb_obj_fn=decidb_obj,
    )


@pytest.mark.when
@pytest.mark.when_constraint
@pytest.mark.min_max
@pytest.mark.correctness
def test_aggregate_local_when_with_max(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """MAX(x*value) WHEN eligible <= 7 (easy-case MAX strips to per-row)."""
    data_sql = """
        SELECT CAST(name AS VARCHAR), CAST(value AS DOUBLE), CAST(eligible AS BOOLEAN) FROM (
            VALUES ('a', 10, true), ('b', 5, true), ('c', 20, false), ('d', 3, true)
        ) t(name, value, eligible)
    """
    decide_sql = """
        SELECT name, value, eligible, x FROM (
            VALUES ('a', 10, true), ('b', 5, true), ('c', 20, false), ('d', 3, true)
        ) t(name, value, eligible)
        DECIDE x IS BOOLEAN
        SUCH THAT MAX(x * value) WHEN eligible <= 7
        MAXIMIZE SUM(x * value)
    """

    def build(oracle, data, cols, rows):
        n = len(data)
        vnames = [f"x_{i}" for i in range(n)]
        for v in vnames:
            oracle.add_variable(v, VarType.BINARY)
        for i, r in enumerate(data):
            if r[2]:  # eligible
                oracle.add_constraint(
                    {vnames[i]: r[1]}, "<=", 7.0, name=f"max_le_{i}",
                )
        oracle.set_objective(
            {vnames[i]: data[i][1] for i in range(n)}, ObjSense.MAXIMIZE,
        )
        return n, 3

    def decidb_obj(rs, cs):
        xi = cs.index("x"); vi = cs.index("value")
        return sum(float(r[xi]) * float(r[vi]) for r in rs)

    _run_constraint_test(
        decidb_cli, duckdb_conn, oracle_solver, perf_tracker,
        test_id="alw_max", decide_sql=decide_sql, data_sql=data_sql,
        build_oracle=build, decidb_obj_fn=decidb_obj,
    )


@pytest.mark.when
@pytest.mark.when_constraint
@pytest.mark.min_max
@pytest.mark.correctness
def test_aggregate_local_when_with_hard_max(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """MAX(x*value) WHEN active >= 6 (hard-case MAX with aggregate-local WHEN).

    Hard MAX(>=) is disjunctive: at least one WHEN-matching row must satisfy the
    bound. DecidB must emit Big-M indicators *only* for active rows — a bug that
    ignores the WHEN mask would let non-active row c (value=20) trivially satisfy
    the constraint, relaxing the selection and changing the optimum.

    Expected optimum: {a, c} selected, obj = 30. Row a (active, value=10) is the
    only active row that can clear value*x >= 6 with x in {0,1}; rows b (value=5)
    and d (value=3) can't. With x_a forced to 1 and SUM(x) <= 2, the second slot
    goes to the highest-value row — c (value=20, non-active).
    """
    data_sql = """
        SELECT CAST(name AS VARCHAR), CAST(value AS DOUBLE), CAST(active AS BOOLEAN) FROM (
            VALUES ('a', 10, true), ('b', 5, true), ('c', 20, false), ('d', 3, true)
        ) t(name, value, active)
    """
    decide_sql = """
        SELECT name, value, active, x FROM (
            VALUES ('a', 10, true), ('b', 5, true), ('c', 20, false), ('d', 3, true)
        ) t(name, value, active)
        DECIDE x IS BOOLEAN
        SUCH THAT MAX(x * value) WHEN active >= 6
            AND SUM(x) <= 2
        MAXIMIZE SUM(x * value)
    """

    def build(oracle, data, cols, rows):
        n = len(data)
        vnames = [f"x_{i}" for i in range(n)]
        for v in vnames:
            oracle.add_variable(v, VarType.BINARY)
        # Budget: at most 2 selected
        oracle.add_constraint(
            {v: 1.0 for v in vnames}, "<=", 2.0, name="budget",
        )
        # Hard MAX(>=6) filtered by active: need at least one active row where
        # value_i * x_i >= 6. Encode per active row via Gurobi indicators.
        y_names: list[str] = []
        for i, r in enumerate(data):
            if r[2]:  # active
                y = f"y_max_{i}"
                oracle.add_variable(y, VarType.BINARY)
                oracle.add_indicator_constraint(
                    y, 1, {vnames[i]: r[1]}, ">=", 6.0,
                    name=f"max_tight_{i}",
                )
                y_names.append(y)
        oracle.add_constraint(
            {y: 1.0 for y in y_names}, ">=", 1.0, name="max_sel",
        )
        oracle.set_objective(
            {vnames[i]: data[i][1] for i in range(n)}, ObjSense.MAXIMIZE,
        )
        return n + len(y_names), 2 + len(y_names)

    def decidb_obj(rs, cs):
        xi = cs.index("x"); vi = cs.index("value")
        return sum(float(r[xi]) * float(r[vi]) for r in rs)

    _run_constraint_test(
        decidb_cli, duckdb_conn, oracle_solver, perf_tracker,
        test_id="alw_hard_max", decide_sql=decide_sql, data_sql=data_sql,
        build_oracle=build, decidb_obj_fn=decidb_obj,
    )


# ---------------------------------------------------------------------------
# B. Mixed terms
# ---------------------------------------------------------------------------

@pytest.mark.when
@pytest.mark.when_constraint
@pytest.mark.correctness
def test_aggregate_local_when_mixed_filtered_unfiltered_constraint(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """SUM(x*v) WHEN premium + SUM(x) <= 12."""
    data_sql = """
        SELECT CAST(name AS VARCHAR), CAST(value AS DOUBLE), CAST(premium AS BOOLEAN) FROM (
            VALUES ('a', 8, true), ('b', 5, false), ('c', 10, true), ('d', 3, false)
        ) t(name, value, premium)
    """
    decide_sql = """
        SELECT name, value, premium, x FROM (
            VALUES ('a', 8, true), ('b', 5, false), ('c', 10, true), ('d', 3, false)
        ) t(name, value, premium)
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * value) WHEN premium + SUM(x) <= 12
        MAXIMIZE SUM(x * value)
    """

    def build(oracle, data, cols, rows):
        n = len(data)
        vnames = [f"x_{i}" for i in range(n)]
        for v in vnames:
            oracle.add_variable(v, VarType.BINARY)
        coeffs: dict = {}
        for i, r in enumerate(data):
            term = r[1] * (1.0 if r[2] else 0.0) + 1.0
            _accumulate(coeffs, vnames[i], term)
        oracle.add_constraint(coeffs, "<=", 12.0, name="mixed")
        oracle.set_objective(
            {vnames[i]: data[i][1] for i in range(n)}, ObjSense.MAXIMIZE,
        )
        return n, 1

    def decidb_obj(rs, cs):
        xi = cs.index("x"); vi = cs.index("value")
        return sum(float(r[xi]) * float(r[vi]) for r in rs)

    _run_constraint_test(
        decidb_cli, duckdb_conn, oracle_solver, perf_tracker,
        test_id="alw_mixed_constraint",
        decide_sql=decide_sql, data_sql=data_sql,
        build_oracle=build, decidb_obj_fn=decidb_obj,
    )


@pytest.mark.when
@pytest.mark.when_objective
@pytest.mark.correctness
def test_aggregate_local_when_objective_mixed_filtered_unfiltered(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """Objective: SUM(x*value) WHEN vip + SUM(x*bonus)."""
    data_sql = """
        SELECT CAST(name AS VARCHAR), CAST(value AS DOUBLE),
               CAST(bonus AS DOUBLE), CAST(vip AS BOOLEAN) FROM (
            VALUES ('a', 10, 2, true), ('b', 3, 8, false),
                   ('c', 7, 5, true), ('d', 1, 1, false)
        ) t(name, value, bonus, vip)
    """
    decide_sql = """
        SELECT name, value, bonus, vip, x FROM (
            VALUES ('a', 10, 2, true), ('b', 3, 8, false),
                   ('c', 7, 5, true), ('d', 1, 1, false)
        ) t(name, value, bonus, vip)
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) <= 2
        MAXIMIZE SUM(x * value) WHEN vip + SUM(x * bonus)
    """

    def build(oracle, data, cols, rows):
        n = len(data)
        vnames = [f"x_{i}" for i in range(n)]
        for v in vnames:
            oracle.add_variable(v, VarType.BINARY)
        oracle.add_constraint({v: 1.0 for v in vnames}, "<=", 2.0, name="total")
        obj: dict = {}
        for i, r in enumerate(data):
            term = r[1] * (1.0 if r[3] else 0.0) + r[2]
            _accumulate(obj, vnames[i], term)
        oracle.set_objective(obj, ObjSense.MAXIMIZE)
        return n, 1

    def decidb_obj(rs, cs):
        xi = cs.index("x"); vi = cs.index("value")
        bi = cs.index("bonus"); vip_i = cs.index("vip")
        total = 0.0
        for r in rs:
            x = float(r[xi])
            total += x * float(r[vi]) * (1.0 if r[vip_i] else 0.0)
            total += x * float(r[bi])
        return total

    _run_constraint_test(
        decidb_cli, duckdb_conn, oracle_solver, perf_tracker,
        test_id="alw_obj_mixed",
        decide_sql=decide_sql, data_sql=data_sql,
        build_oracle=build, decidb_obj_fn=decidb_obj,
    )


# ---------------------------------------------------------------------------
# C. Edge conditions
# ---------------------------------------------------------------------------

@pytest.mark.when
@pytest.mark.when_constraint
@pytest.mark.edge_case
@pytest.mark.error_infeasible
def test_aggregate_local_when_all_filtered_out(decidb_cli):
    """Mixed aggregate-local WHEN with one term's mask all-false — rejected
    pre-solver per the "reject all empty aggregate sets" rule. Previously the
    empty term was allowed to contribute 0 while the unmasked term still
    bound; the strict rule catches it."""
    decide_sql = """
        SELECT name, value, flag, x FROM (
            VALUES ('a', 10, false), ('b', 5, false), ('c', 8, false)
        ) t(name, value, flag)
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * value) WHEN flag + SUM(x * value) <= 23
        MAXIMIZE SUM(x * value)
    """
    decidb_cli.assert_error(decide_sql, match=r"empty|WHEN")


@pytest.mark.when
@pytest.mark.when_constraint
@pytest.mark.correctness
def test_aggregate_local_when_overlapping_filters(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """Row matching both WHEN conditions contributes to both terms."""
    data_sql = """
        SELECT CAST(name AS VARCHAR), CAST(value AS DOUBLE),
               CAST(cat_a AS BOOLEAN), CAST(cat_b AS BOOLEAN) FROM (
            VALUES ('a', 10, true, true), ('b', 5, true, false),
                   ('c', 8, false, true), ('d', 3, false, false)
        ) t(name, value, cat_a, cat_b)
    """
    decide_sql = """
        SELECT name, value, cat_a, cat_b, x FROM (
            VALUES ('a', 10, true, true), ('b', 5, true, false),
                   ('c', 8, false, true), ('d', 3, false, false)
        ) t(name, value, cat_a, cat_b)
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * value) WHEN cat_a + SUM(x * value) WHEN cat_b <= 20
        MAXIMIZE SUM(x * value)
    """

    def build(oracle, data, cols, rows):
        n = len(data)
        vnames = [f"x_{i}" for i in range(n)]
        for v in vnames:
            oracle.add_variable(v, VarType.BINARY)
        coeffs: dict = {}
        for i, r in enumerate(data):
            term = r[1] * ((1.0 if r[2] else 0.0) + (1.0 if r[3] else 0.0))
            _accumulate(coeffs, vnames[i], term)
        oracle.add_constraint(coeffs, "<=", 20.0, name="overlap")
        oracle.set_objective(
            {vnames[i]: data[i][1] for i in range(n)}, ObjSense.MAXIMIZE,
        )
        return n, 1

    def decidb_obj(rs, cs):
        xi = cs.index("x"); vi = cs.index("value")
        return sum(float(r[xi]) * float(r[vi]) for r in rs)

    _run_constraint_test(
        decidb_cli, duckdb_conn, oracle_solver, perf_tracker,
        test_id="alw_overlap",
        decide_sql=decide_sql, data_sql=data_sql,
        build_oracle=build, decidb_obj_fn=decidb_obj,
    )


@pytest.mark.when
@pytest.mark.when_constraint
@pytest.mark.edge_case
@pytest.mark.correctness
def test_aggregate_local_when_single_aggregate(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """Single aggregate with WHEN — degenerate case equivalent to expression-level."""
    data_sql = """
        SELECT CAST(name AS VARCHAR), CAST(value AS DOUBLE), CAST(active AS BOOLEAN) FROM (
            VALUES ('a', 10, true), ('b', 5, true), ('c', 8, false)
        ) t(name, value, active)
    """
    decide_sql = """
        SELECT name, value, active, x FROM (
            VALUES ('a', 10, true), ('b', 5, true), ('c', 8, false)
        ) t(name, value, active)
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * value) WHEN active <= 10
        MAXIMIZE SUM(x * value)
    """

    def build(oracle, data, cols, rows):
        n = len(data)
        vnames = [f"x_{i}" for i in range(n)]
        for v in vnames:
            oracle.add_variable(v, VarType.BINARY)
        coeffs = {vnames[i]: data[i][1] for i in range(n) if data[i][2]}
        oracle.add_constraint(coeffs, "<=", 10.0, name="single")
        oracle.set_objective(
            {vnames[i]: data[i][1] for i in range(n)}, ObjSense.MAXIMIZE,
        )
        return n, 1

    def decidb_obj(rs, cs):
        xi = cs.index("x"); vi = cs.index("value")
        return sum(float(r[xi]) * float(r[vi]) for r in rs)

    _run_constraint_test(
        decidb_cli, duckdb_conn, oracle_solver, perf_tracker,
        test_id="alw_single_aggregate",
        decide_sql=decide_sql, data_sql=data_sql,
        build_oracle=build, decidb_obj_fn=decidb_obj,
    )


@pytest.mark.when
@pytest.mark.when_constraint
@pytest.mark.correctness
def test_aggregate_local_when_three_terms(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """Three additive aggregate terms with different WHEN conditions."""
    data_sql = """
        SELECT CAST(name AS VARCHAR), CAST(value AS DOUBLE), CAST(cat AS VARCHAR) FROM (
            VALUES ('a', 10, 'X'), ('b', 5, 'Y'), ('c', 8, 'Z'),
                   ('d', 3, 'X'), ('e', 7, 'Y')
        ) t(name, value, cat)
    """
    decide_sql = """
        SELECT name, value, cat, x FROM (
            VALUES ('a', 10, 'X'), ('b', 5, 'Y'), ('c', 8, 'Z'),
                   ('d', 3, 'X'), ('e', 7, 'Y')
        ) t(name, value, cat)
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * value) WHEN (cat = 'X') + SUM(x * value) WHEN (cat = 'Y') + SUM(x * value) WHEN (cat = 'Z') <= 20
        MAXIMIZE SUM(x * value)
    """

    def build(oracle, data, cols, rows):
        n = len(data)
        vnames = [f"x_{i}" for i in range(n)]
        for v in vnames:
            oracle.add_variable(v, VarType.BINARY)
        coeffs: dict = {}
        for i, r in enumerate(data):
            mask = (
                (1.0 if r[2] == "X" else 0.0)
                + (1.0 if r[2] == "Y" else 0.0)
                + (1.0 if r[2] == "Z" else 0.0)
            )
            _accumulate(coeffs, vnames[i], r[1] * mask)
        oracle.add_constraint(coeffs, "<=", 20.0, name="three_terms")
        oracle.set_objective(
            {vnames[i]: data[i][1] for i in range(n)}, ObjSense.MAXIMIZE,
        )
        return n, 1

    def decidb_obj(rs, cs):
        xi = cs.index("x"); vi = cs.index("value")
        return sum(float(r[xi]) * float(r[vi]) for r in rs)

    _run_constraint_test(
        decidb_cli, duckdb_conn, oracle_solver, perf_tracker,
        test_id="alw_three_terms",
        decide_sql=decide_sql, data_sql=data_sql,
        build_oracle=build, decidb_obj_fn=decidb_obj,
    )


# ---------------------------------------------------------------------------
# D. Error cases (DecidB-only; no oracle can help)
# ---------------------------------------------------------------------------

@pytest.mark.when
@pytest.mark.when_constraint
@pytest.mark.error
@pytest.mark.error_binder
def test_aggregate_local_when_decide_var_in_condition_error(decidb_cli):
    decidb_cli.assert_error("""
        SELECT name, value, x FROM (
            VALUES ('a', 10), ('b', 5)
        ) t(name, value)
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * value) WHEN x <= 10
        MAXIMIZE SUM(x * value)
    """, match=r"(?i)DECIDE variables")


@pytest.mark.when
@pytest.mark.when_objective
@pytest.mark.error
@pytest.mark.error_binder
def test_aggregate_local_when_mixed_expression_objective_error(decidb_cli):
    decidb_cli.assert_error("""
        SELECT name, value, w1, w2, x FROM (
            VALUES ('a', 10, true, false), ('b', 5, false, true)
        ) t(name, value, w1, w2)
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) <= 2
        MAXIMIZE (SUM(x * value) WHEN w1 + SUM(x * value) WHEN w2) WHEN w1
    """, match=r"Cannot combine")


@pytest.mark.when
@pytest.mark.when_objective
@pytest.mark.error
@pytest.mark.error_binder
def test_aggregate_local_when_decide_var_in_objective_condition_error(decidb_cli):
    decidb_cli.assert_error("""
        SELECT name, value, x FROM (
            VALUES ('a', 10), ('b', 5)
        ) t(name, value)
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) <= 2
        MAXIMIZE SUM(x * value) WHEN x
    """, match=r"(?i)DECIDE variables")


# ---------------------------------------------------------------------------
# E. Regressions (expression-level WHEN still works)
# ---------------------------------------------------------------------------

@pytest.mark.when
@pytest.mark.when_objective
@pytest.mark.correctness
def test_expression_level_when_objective_still_works(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """Expression-level WHEN on objective."""
    data_sql = """
        SELECT CAST(name AS VARCHAR), CAST(value AS DOUBLE), CAST(vip AS BOOLEAN) FROM (
            VALUES ('a', 10, true), ('b', 5, true), ('c', 8, false)
        ) t(name, value, vip)
    """
    decide_sql = """
        SELECT name, value, vip, x FROM (
            VALUES ('a', 10, true), ('b', 5, true), ('c', 8, false)
        ) t(name, value, vip)
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) <= 2
        MAXIMIZE SUM(x * value) WHEN vip
    """

    def build(oracle, data, cols, rows):
        n = len(data)
        vnames = [f"x_{i}" for i in range(n)]
        for v in vnames:
            oracle.add_variable(v, VarType.BINARY)
        oracle.add_constraint({v: 1.0 for v in vnames}, "<=", 2.0, name="total")
        oracle.set_objective(
            {vnames[i]: (data[i][1] if data[i][2] else 0.0) for i in range(n)},
            ObjSense.MAXIMIZE,
        )
        return n, 1

    def decidb_obj(rs, cs):
        xi = cs.index("x"); vi = cs.index("value"); vip_i = cs.index("vip")
        return sum(
            float(r[xi]) * float(r[vi]) * (1.0 if r[vip_i] else 0.0) for r in rs
        )

    _run_constraint_test(
        decidb_cli, duckdb_conn, oracle_solver, perf_tracker,
        test_id="expr_when_obj",
        decide_sql=decide_sql, data_sql=data_sql,
        build_oracle=build, decidb_obj_fn=decidb_obj,
    )


@pytest.mark.when
@pytest.mark.when_constraint
@pytest.mark.per_clause
@pytest.mark.correctness
def test_expression_level_when_per_still_works(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """Expression-level WHEN + PER."""
    data_sql = """
        SELECT CAST(name AS VARCHAR), CAST(value AS DOUBLE),
               CAST(grp AS VARCHAR), CAST(active AS BOOLEAN) FROM (
            VALUES ('a', 10, 'A', true), ('b', 5, 'A', false),
                   ('c', 8, 'B', true), ('d', 3, 'B', false)
        ) t(name, value, grp, active)
    """
    decide_sql = """
        SELECT name, value, grp, active, x FROM (
            VALUES ('a', 10, 'A', true), ('b', 5, 'A', false),
                   ('c', 8, 'B', true), ('d', 3, 'B', false)
        ) t(name, value, grp, active)
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * value) <= 10 WHEN active PER grp
        MAXIMIZE SUM(x * value)
    """

    def build(oracle, data, cols, rows):
        n = len(data)
        vnames = [f"x_{i}" for i in range(n)]
        for v in vnames:
            oracle.add_variable(v, VarType.BINARY)
        for grp, idxs in group_indices(data, lambda r: r[2]).items():
            qualifying = [i for i in idxs if data[i][3]]
            if not qualifying:
                continue
            oracle.add_constraint(
                {vnames[i]: data[i][1] for i in qualifying},
                "<=", 10.0, name=f"per_{grp}",
            )
        oracle.set_objective(
            {vnames[i]: data[i][1] for i in range(n)}, ObjSense.MAXIMIZE,
        )
        return n, 2

    def decidb_obj(rs, cs):
        xi = cs.index("x"); vi = cs.index("value")
        return sum(float(r[xi]) * float(r[vi]) for r in rs)

    _run_constraint_test(
        decidb_cli, duckdb_conn, oracle_solver, perf_tracker,
        test_id="expr_when_per",
        decide_sql=decide_sql, data_sql=data_sql,
        build_oracle=build, decidb_obj_fn=decidb_obj,
    )


# ---------------------------------------------------------------------------
# F. Compositions and grammar quirks
# ---------------------------------------------------------------------------

@pytest.mark.when
@pytest.mark.when_constraint
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_bilinear_aggregate_local_when_constraint(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """Bilinear b*x with WHEN — AND-linearize the bool product, then mask."""
    data_sql = """
        SELECT CAST(id AS BIGINT), CAST(value AS DOUBLE), CAST(active AS BOOLEAN) FROM (
            VALUES (1, 10, true), (2, 5, true), (3, 8, false), (4, 3, true)
        ) t(id, value, active)
    """
    decide_sql = """
        SELECT id, value, active, b, x FROM (
            VALUES (1, 10, true), (2, 5, true), (3, 8, false), (4, 3, true)
        ) t(id, value, active)
        DECIDE b IS BOOLEAN, x IS BOOLEAN
        SUCH THAT SUM(b * x) WHEN active <= 1
        MAXIMIZE SUM(b * value + x * value)
    """
    t0 = time.perf_counter()
    rows, cols = decidb_cli.execute(decide_sql)
    decidb_time = time.perf_counter() - t0
    data = duckdb_conn.execute(data_sql).fetchall()
    n = len(data)

    t_build = time.perf_counter()
    oracle_solver.create_model("alw_bilinear_constraint")
    for i in range(n):
        oracle_solver.add_variable(f"b_{i}", VarType.BINARY)
        oracle_solver.add_variable(f"x_{i}", VarType.BINARY)
        add_bool_and(oracle_solver, f"b_{i}", f"x_{i}", f"bx_{i}")
    active_idx = [i for i, r in enumerate(data) if r[2]]
    oracle_solver.add_constraint(
        {f"bx_{i}": 1.0 for i in active_idx},
        "<=", 1.0, name="bx_active",
    )
    obj: dict = {}
    for i, r in enumerate(data):
        obj[f"b_{i}"] = r[1]
        obj[f"x_{i}"] = r[1]
    oracle_solver.set_objective(obj, ObjSense.MAXIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    def decidb_obj(rs, cs):
        bi = cs.index("b"); xi = cs.index("x"); vi = cs.index("value")
        return sum(
            (float(r[bi]) + float(r[xi])) * float(r[vi]) for r in rs
        )

    cmp = compare_solutions(
        rows, cols, result, data, ["b", "x"],
        decidb_objective_fn=decidb_obj,
    )
    perf_tracker.record(
        "alw_bilinear_constraint", decidb_time, build_time,
        result.solve_time_seconds, n, 3 * n, 4 * n + 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.when
@pytest.mark.when_objective
@pytest.mark.correctness
def test_bilinear_aggregate_local_when_objective(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """Triple bilinear b*x*value WHEN premium — AND-linearize b∧x then scale by value."""
    data_sql = """
        SELECT CAST(id AS BIGINT), CAST(value AS DOUBLE), CAST(premium AS BOOLEAN) FROM (
            VALUES (1, 10, true), (2, 5, false), (3, 8, true)
        ) t(id, value, premium)
    """
    decide_sql = """
        SELECT id, value, premium, b, x FROM (
            VALUES (1, 10, true), (2, 5, false), (3, 8, true)
        ) t(id, value, premium)
        DECIDE b IS BOOLEAN, x IS BOOLEAN
        SUCH THAT SUM(b) <= 3 AND SUM(x) <= 3
        MAXIMIZE SUM(b * x * value) WHEN premium
    """
    t0 = time.perf_counter()
    rows, cols = decidb_cli.execute(decide_sql)
    decidb_time = time.perf_counter() - t0
    data = duckdb_conn.execute(data_sql).fetchall()
    n = len(data)

    t_build = time.perf_counter()
    oracle_solver.create_model("alw_bilinear_obj")
    for i in range(n):
        oracle_solver.add_variable(f"b_{i}", VarType.BINARY)
        oracle_solver.add_variable(f"x_{i}", VarType.BINARY)
        add_bool_and(oracle_solver, f"b_{i}", f"x_{i}", f"bx_{i}")
    oracle_solver.add_constraint({f"b_{i}": 1.0 for i in range(n)}, "<=", 3.0, name="b_cap")
    oracle_solver.add_constraint({f"x_{i}": 1.0 for i in range(n)}, "<=", 3.0, name="x_cap")
    obj: dict = {}
    for i, r in enumerate(data):
        if r[2]:
            obj[f"bx_{i}"] = r[1]
    oracle_solver.set_objective(obj, ObjSense.MAXIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    def decidb_obj(rs, cs):
        bi = cs.index("b"); xi = cs.index("x")
        vi = cs.index("value"); pi = cs.index("premium")
        return sum(
            float(r[bi]) * float(r[xi]) * float(r[vi])
            * (1.0 if r[pi] else 0.0)
            for r in rs
        )

    cmp = compare_solutions(
        rows, cols, result, data, ["b", "x"],
        decidb_objective_fn=decidb_obj,
    )
    perf_tracker.record(
        "alw_bilinear_obj", decidb_time, build_time, result.solve_time_seconds,
        n, 3 * n, 3 * n + 2,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.when
@pytest.mark.when_constraint
@pytest.mark.cons_comparison
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_ne_aggregate_local_when_constraint(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """SUM(x) WHEN active <> 2 — the active-only count cannot equal 2."""
    data_sql = """
        SELECT CAST(name AS VARCHAR), CAST(value AS DOUBLE), CAST(active AS BOOLEAN) FROM (
            VALUES ('a', 10, true), ('b', 5, true), ('c', 8, false), ('d', 3, true)
        ) t(name, value, active)
    """
    decide_sql = """
        SELECT name, value, active, x FROM (
            VALUES ('a', 10, true), ('b', 5, true), ('c', 8, false), ('d', 3, true)
        ) t(name, value, active)
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) WHEN active <> 2
            AND SUM(x) <= 3
        MAXIMIZE SUM(x * value)
    """

    def build(oracle, data, cols, rows):
        n = len(data)
        vnames = [f"x_{i}" for i in range(n)]
        for v in vnames:
            oracle.add_variable(v, VarType.BINARY)
        active_idx = [i for i, r in enumerate(data) if r[2]]
        add_ne_indicator(
            oracle, {vnames[i]: 1.0 for i in active_idx},
            rhs=2.0, name="ne_active",
        )
        oracle.add_constraint({v: 1.0 for v in vnames}, "<=", 3.0, name="total")
        oracle.set_objective(
            {vnames[i]: data[i][1] for i in range(n)}, ObjSense.MAXIMIZE,
        )
        return n + 1, 4

    def decidb_obj(rs, cs):
        xi = cs.index("x"); vi = cs.index("value")
        return sum(float(r[xi]) * float(r[vi]) for r in rs)

    _run_constraint_test(
        decidb_cli, duckdb_conn, oracle_solver, perf_tracker,
        test_id="alw_ne_constraint",
        decide_sql=decide_sql, data_sql=data_sql,
        build_oracle=build, decidb_obj_fn=decidb_obj,
    )


@pytest.mark.when
@pytest.mark.when_constraint
@pytest.mark.cons_comparison
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_ne_with_per_constraint(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """SUM(x) <> 2 PER dept — each dept's count cannot equal 2."""
    data_sql = """
        SELECT CAST(name AS VARCHAR), CAST(value AS DOUBLE), CAST(dept AS VARCHAR) FROM (
            VALUES ('a', 10, 'eng'), ('b', 5, 'eng'), ('c', 8, 'sales'),
                   ('d', 3, 'sales'), ('e', 7, 'eng')
        ) t(name, value, dept)
    """
    decide_sql = """
        SELECT name, value, dept, x FROM (
            VALUES ('a', 10, 'eng'), ('b', 5, 'eng'), ('c', 8, 'sales'),
                   ('d', 3, 'sales'), ('e', 7, 'eng')
        ) t(name, value, dept)
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) <> 2 PER dept
            AND SUM(x) <= 4
        MAXIMIZE SUM(x * value)
    """

    def build(oracle, data, cols, rows):
        n = len(data)
        vnames = [f"x_{i}" for i in range(n)]
        for v in vnames:
            oracle.add_variable(v, VarType.BINARY)
        for dept, idxs in group_indices(data, lambda r: r[2]).items():
            add_ne_indicator(
                oracle, {vnames[i]: 1.0 for i in idxs},
                rhs=2.0, name=f"ne_{dept}",
            )
        oracle.add_constraint({v: 1.0 for v in vnames}, "<=", 4.0, name="total")
        oracle.set_objective(
            {vnames[i]: data[i][1] for i in range(n)}, ObjSense.MAXIMIZE,
        )
        return n + 2, 6

    def decidb_obj(rs, cs):
        xi = cs.index("x"); vi = cs.index("value")
        return sum(float(r[xi]) * float(r[vi]) for r in rs)

    _run_constraint_test(
        decidb_cli, duckdb_conn, oracle_solver, perf_tracker,
        test_id="alw_ne_per",
        decide_sql=decide_sql, data_sql=data_sql,
        build_oracle=build, decidb_obj_fn=decidb_obj,
    )


@pytest.mark.when
@pytest.mark.when_constraint
@pytest.mark.cons_between
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_between_aggregate_local_when_constraint(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """BETWEEN on aggregate with aggregate-local WHEN."""
    data_sql = """
        SELECT CAST(name AS VARCHAR), CAST(value AS DOUBLE), CAST(active AS BOOLEAN) FROM (
            VALUES ('a', 10, true), ('b', 5, true), ('c', 8, false), ('d', 3, true)
        ) t(name, value, active)
    """
    decide_sql = """
        SELECT name, value, active, x FROM (
            VALUES ('a', 10, true), ('b', 5, true), ('c', 8, false), ('d', 3, true)
        ) t(name, value, active)
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * value) WHEN active BETWEEN 5 AND 13
        MAXIMIZE SUM(x * value)
    """

    def build(oracle, data, cols, rows):
        n = len(data)
        vnames = [f"x_{i}" for i in range(n)]
        for v in vnames:
            oracle.add_variable(v, VarType.BINARY)
        active = {vnames[i]: data[i][1] for i in range(n) if data[i][2]}
        oracle.add_constraint(active, ">=", 5.0, name="between_lo")
        oracle.add_constraint(active, "<=", 13.0, name="between_hi")
        oracle.set_objective(
            {vnames[i]: data[i][1] for i in range(n)}, ObjSense.MAXIMIZE,
        )
        return n, 2

    def decidb_obj(rs, cs):
        xi = cs.index("x"); vi = cs.index("value")
        return sum(float(r[xi]) * float(r[vi]) for r in rs)

    _run_constraint_test(
        decidb_cli, duckdb_conn, oracle_solver, perf_tracker,
        test_id="alw_between",
        decide_sql=decide_sql, data_sql=data_sql,
        build_oracle=build, decidb_obj_fn=decidb_obj,
    )


@pytest.mark.when
@pytest.mark.when_constraint
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_entity_scoped_aggregate_local_when(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """Entity-scoped ``n.keepN`` with aggregate-local WHEN on a row-scoped column."""
    sql = """
        SELECT c.c_custkey, n.n_nationkey, n.n_name, c.c_acctbal, keepN
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0
        DECIDE n.keepN IS BOOLEAN
        SUCH THAT SUM(keepN * c.c_acctbal) WHEN (c.c_acctbal > 5000) <= 50000
        MAXIMIZE SUM(keepN)
    """
    t0 = time.perf_counter()
    rows, cols = decidb_cli.execute(sql)
    decidb_time = time.perf_counter() - t0
    data = duckdb_conn.execute("""
        SELECT CAST(c.c_custkey AS BIGINT),
               CAST(n.n_nationkey AS BIGINT),
               CAST(n.n_name AS VARCHAR),
               CAST(c.c_acctbal AS DOUBLE)
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0
    """).fetchall()
    n = len(data)
    nation_keys = sorted({r[1] for r in data})

    t_build = time.perf_counter()
    oracle_solver.create_model("alw_entity_scoped")
    for nk in nation_keys:
        oracle_solver.add_variable(f"keepN_{nk}", VarType.BINARY)
    coeffs: dict = {}
    for i, r in enumerate(data):
        if r[3] > 5000:
            _accumulate(coeffs, f"keepN_{r[1]}", r[3])
    oracle_solver.add_constraint(coeffs, "<=", 50000.0, name="high_bal")
    # Objective: SUM(keepN) — but in DecidB this sums per row (not per entity).
    # So the oracle objective equals (# rows per entity) * keepN per entity.
    obj: dict = defaultdict(float)
    for r in data:
        obj[f"keepN_{r[1]}"] += 1.0
    oracle_solver.set_objective(dict(obj), ObjSense.MAXIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    # Entity-scoped consistency: same nation ⇒ same keepN.
    ni_col = cols.index("n_nationkey"); keep_col = cols.index("keepN")
    seen: dict = {}
    for r in rows:
        nk, v = r[ni_col], r[keep_col]
        assert nk not in seen or seen[nk] == v, f"Inconsistent keepN for nation {nk}"
        seen[nk] = v

    oracle_obj = result.objective_value
    decidb_obj_val = sum(float(r[keep_col]) for r in rows)
    assert abs(oracle_obj - decidb_obj_val) < 1e-4, (
        f"Objective mismatch: oracle={oracle_obj}, decidb={decidb_obj_val}"
    )

    perf_tracker.record(
        "alw_entity_scoped", decidb_time, build_time, result.solve_time_seconds,
        n, len(nation_keys), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="identical",
        decide_vector=[float(r[keep_col]) for r in rows],
    )


@pytest.mark.when
@pytest.mark.when_constraint
@pytest.mark.error
def test_aggregate_local_when_unparenthesized_comparison_error(decidb_cli):
    decidb_cli.assert_error("""
        SELECT name, value, tier, x FROM (
            VALUES ('a', 7, 'high'), ('b', 3, 'low'), ('c', 9, 'none')
        ) t(name, value, tier)
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * value) WHEN tier = 'high' <= 7
        MAXIMIZE SUM(x * value)
    """)


@pytest.mark.when
@pytest.mark.when_objective
@pytest.mark.correctness
def test_aggregate_local_when_objective_reassociation(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """Objective reassociator converts ``WHEN tier = 'high'`` (unparenthesized)
    into expression-level WHEN(tier = 'high')."""
    data_sql = """
        SELECT CAST(name AS VARCHAR), CAST(value AS DOUBLE), CAST(tier AS VARCHAR) FROM (
            VALUES ('a', 7, 'high'), ('b', 3, 'low'), ('c', 9, 'high')
        ) t(name, value, tier)
    """
    decide_sql = """
        SELECT name, value, tier, x FROM (
            VALUES ('a', 7, 'high'), ('b', 3, 'low'), ('c', 9, 'high')
        ) t(name, value, tier)
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) <= 2
        MAXIMIZE SUM(x * value) WHEN tier = 'high'
    """

    def build(oracle, data, cols, rows):
        n = len(data)
        vnames = [f"x_{i}" for i in range(n)]
        for v in vnames:
            oracle.add_variable(v, VarType.BINARY)
        oracle.add_constraint({v: 1.0 for v in vnames}, "<=", 2.0, name="total")
        oracle.set_objective(
            {vnames[i]: (data[i][1] if data[i][2] == "high" else 0.0) for i in range(n)},
            ObjSense.MAXIMIZE,
        )
        return n, 1

    def decidb_obj(rs, cs):
        xi = cs.index("x"); vi = cs.index("value"); ti = cs.index("tier")
        return sum(
            float(r[xi]) * float(r[vi]) * (1.0 if r[ti] == "high" else 0.0)
            for r in rs
        )

    _run_constraint_test(
        decidb_cli, duckdb_conn, oracle_solver, perf_tracker,
        test_id="alw_obj_reassoc",
        decide_sql=decide_sql, data_sql=data_sql,
        build_oracle=build, decidb_obj_fn=decidb_obj,
    )


# ===========================================================================
# Aggregate-local WHEN combined with arithmetic — peeled in the symbolic
# normalizer so these compositions actually work end-to-end.
# ===========================================================================
#
# `NormalizeComparisonExpr` in decide_symbolic.cpp can't use the SymEngine
# expand/simplify path on a WHEN-bearing LHS — that would flatten the
# aggregate-local WHEN tag and lose the per-aggregate filter. Instead the
# normalizer does a parsed-level rewrite tailored to WHEN-bearing LHSes:
#   1. Folds constant scalars into WHEN-tagged aggregate bodies
#      (`K * (SUM(x) WHEN c)` → `WHEN(SUM(K*x), c)`,
#      `(SUM(x) WHEN c) / K` → `WHEN(SUM(x/K), c)`).
#   2. Decomposes the LHS additively, peels pure-numeric terms into a
#      single offset, and rebuilds the LHS from the structural terms.
#   3. Moves the offset to the RHS as `RHS - offset`.
#
# These tests cover each shape with oracle-verified correctness.

@pytest.mark.when
@pytest.mark.when_constraint
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_when_with_constant_offset_paren_condition(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """`SUM(x) WHEN (w > 1) + 3 <= K` — additive constant peeled to RHS."""
    data_sql = """
        SELECT CAST(id AS BIGINT), CAST(w AS DOUBLE) FROM (
            VALUES (1, 2.0), (2, 0.5), (3, 3.0)
        ) t(id, w)
    """
    decide_sql = """
        SELECT id, w, x FROM (
            VALUES (1, 2.0), (2, 0.5), (3, 3.0)
        ) t(id, w)
        DECIDE x IS REAL
        SUCH THAT SUM(x) WHEN (w > 1) + 3 <= 10
            AND x <= 10
        MAXIMIZE SUM(x)
    """

    def build(oracle, data, cols, rows):
        n = len(data)
        vnames = [f"x_{i}" for i in range(n)]
        for v in vnames:
            oracle.add_variable(v, VarType.CONTINUOUS, lb=0.0, ub=10.0)
        # SUM_{w>1}(x_i) + 3 <= 10  →  SUM_{w>1}(x_i) <= 7
        coeffs = {vnames[i]: 1.0 for i in range(n) if data[i][1] > 1}
        oracle.add_constraint(coeffs, "<=", 7.0, name="when_offset")
        oracle.set_objective(
            {vnames[i]: 1.0 for i in range(n)}, ObjSense.MAXIMIZE,
        )
        return n, 1

    def decidb_obj(rs, cs):
        xi = cs.index("x")
        return sum(float(r[xi]) for r in rs)

    _run_constraint_test(
        decidb_cli, duckdb_conn, oracle_solver, perf_tracker,
        test_id="alw_offset_paren_cond",
        decide_sql=decide_sql, data_sql=data_sql,
        build_oracle=build, decidb_obj_fn=decidb_obj,
    )


@pytest.mark.when
@pytest.mark.when_constraint
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_when_with_scalar_multiplier(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """`2 * (SUM(x) WHEN cond) <= K` — constant scalar folded into the SUM
    body so the extractor sees `WHEN(SUM(2*x), cond)` instead of
    `*(2, BoundAggregate)`."""
    data_sql = """
        SELECT CAST(id AS BIGINT), CAST(w AS BOOLEAN) FROM (
            VALUES (1, true), (2, false), (3, true)
        ) t(id, w)
    """
    decide_sql = """
        SELECT id, w, x FROM (
            VALUES (1, true), (2, false), (3, true)
        ) t(id, w)
        DECIDE x IS REAL
        SUCH THAT 2 * (SUM(x) WHEN w) <= 10
            AND x <= 10
        MAXIMIZE SUM(x)
    """

    def build(oracle, data, cols, rows):
        n = len(data)
        vnames = [f"x_{i}" for i in range(n)]
        for v in vnames:
            oracle.add_variable(v, VarType.CONTINUOUS, lb=0.0, ub=10.0)
        # 2 * SUM_{w}(x_i) <= 10  →  per-row coefficient 2 for w-rows.
        coeffs = {vnames[i]: 2.0 for i in range(n) if data[i][1]}
        oracle.add_constraint(coeffs, "<=", 10.0, name="scalar_mult_when")
        oracle.set_objective(
            {vnames[i]: 1.0 for i in range(n)}, ObjSense.MAXIMIZE,
        )
        return n, 1

    def decidb_obj(rs, cs):
        xi = cs.index("x")
        return sum(float(r[xi]) for r in rs)

    _run_constraint_test(
        decidb_cli, duckdb_conn, oracle_solver, perf_tracker,
        test_id="alw_scalar_mult",
        decide_sql=decide_sql, data_sql=data_sql,
        build_oracle=build, decidb_obj_fn=decidb_obj,
    )


@pytest.mark.when
@pytest.mark.when_constraint
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_when_with_parallel_sum_and_offset(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """`(SUM(x) WHEN w) + (SUM(y) + 3) <= K` — additive walker peels the
    constant offset out of a deeper parenthesized parallel sum, leaving
    `(SUM(x) WHEN w) + SUM(y) <= K - 3` for the extractor."""
    data_sql = """
        SELECT CAST(id AS BIGINT), CAST(w AS BOOLEAN) FROM (
            VALUES (1, true), (2, false), (3, true)
        ) t(id, w)
    """
    decide_sql = """
        SELECT id, w, x, y FROM (
            VALUES (1, true), (2, false), (3, true)
        ) t(id, w)
        DECIDE x IS REAL, y IS REAL
        SUCH THAT (SUM(x) WHEN w) + (SUM(y) + 3) <= 10
            AND x <= 10 AND y <= 10
        MAXIMIZE SUM(x) + SUM(y)
    """

    def build(oracle, data, cols, rows):
        n = len(data)
        xnames = [f"x_{i}" for i in range(n)]
        ynames = [f"y_{i}" for i in range(n)]
        for nm in xnames + ynames:
            oracle.add_variable(nm, VarType.CONTINUOUS, lb=0.0, ub=10.0)
        # SUM_{w}(x_i) + SUM(y_i) + 3 <= 10  →  + SUM <= 7
        coeffs: dict = {}
        for i in range(n):
            if data[i][1]:
                coeffs[xnames[i]] = 1.0
            coeffs[ynames[i]] = 1.0
        oracle.add_constraint(coeffs, "<=", 7.0, name="parallel_when_offset")
        obj = {nm: 1.0 for nm in xnames + ynames}
        oracle.set_objective(obj, ObjSense.MAXIMIZE)
        return 2 * n, 1

    def decidb_obj(rs, cs):
        xi = cs.index("x"); yi = cs.index("y")
        return sum(float(r[xi]) + float(r[yi]) for r in rs)

    _run_constraint_test(
        decidb_cli, duckdb_conn, oracle_solver, perf_tracker,
        test_id="alw_parallel_offset",
        decide_sql=decide_sql, data_sql=data_sql,
        build_oracle=build, decidb_obj_fn=decidb_obj,
    )


@pytest.mark.when
@pytest.mark.error_parser
@pytest.mark.error
def test_when_unparenthesized_condition_misparses(decidb_cli):
    """`(SUM(x) WHEN w > 1) + 3 <= K` — parens in the wrong place. The
    aggregate-local WHEN binds tighter than `>` per POSTFIXOP precedence,
    so `(SUM(x) WHEN w)` reduces first and the outer `>` produces a
    BOOLEAN. The function binder then can't add BOOLEAN + INTEGER. The
    user-facing message at least names the type mismatch; the intended
    form `SUM(x) WHEN (w > 1) + 3 <= K` (parens around the condition)
    is the exercised positive path above."""
    decidb_cli.assert_error("""
        SELECT id, x FROM (VALUES (1, 2.0), (2, 0.5), (3, 3.0)) t(id, w)
        DECIDE x IS REAL
        SUCH THAT (SUM(x) WHEN w > 1) + 3 <= 10
            AND x <= 10
        MAXIMIZE SUM(x)
    """, match=r"\+\(BOOLEAN, INTEGER")


@pytest.mark.when
@pytest.mark.when_objective
@pytest.mark.cons_perrow
@pytest.mark.correctness
def test_when_objective_with_constant_offset(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """`MAXIMIZE (SUM(x) WHEN cond) + 3` — the additive `+3` is peeled from
    the objective body by NormalizeDecideObjective. The constant doesn't
    affect argmax, so decidb's optimal assignment matches an oracle that
    maximizes just the aggregate without the constant."""
    data_sql = """
        SELECT CAST(id AS BIGINT), CAST(w AS DOUBLE) FROM (
            VALUES (1, 2.0), (2, 0.5), (3, 3.0)
        ) t(id, w)
    """
    decide_sql = """
        SELECT id, w, x FROM (
            VALUES (1, 2.0), (2, 0.5), (3, 3.0)
        ) t(id, w)
        DECIDE x IS REAL
        SUCH THAT x <= 10
        MAXIMIZE (SUM(x) WHEN w > 1) + 3
    """

    def build(oracle, data, cols, rows):
        n = len(data)
        vnames = [f"x_{i}" for i in range(n)]
        for v in vnames:
            oracle.add_variable(v, VarType.CONTINUOUS, lb=0.0, ub=10.0)
        # Only w>1 rows contribute to argmax; others are free up to their bound.
        oracle.set_objective(
            {vnames[i]: 1.0 for i in range(n) if data[i][1] > 1},
            ObjSense.MAXIMIZE,
        )
        return n, 0

    def decidb_obj(rs, cs):
        xi = cs.index("x"); wi = cs.index("w")
        # Match oracle: sum of x over w>1 rows (no constant offset).
        return sum(float(r[xi]) for r in rs if float(r[wi]) > 1)

    _run_constraint_test(
        decidb_cli, duckdb_conn, oracle_solver, perf_tracker,
        test_id="alw_obj_constant_offset",
        decide_sql=decide_sql, data_sql=data_sql,
        build_oracle=build, decidb_obj_fn=decidb_obj,
    )


# ===========================================================================
# Regression guards: legitimate WHEN-aggregate compositions still work.
# ===========================================================================

@pytest.mark.when
@pytest.mark.when_constraint
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_outer_when_with_arithmetic_offset_works(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """`SUM(x) + 3 <= K WHEN cond` — outer-WHEN form. Same semantics as
    the now-working aggregate-local form `(SUM(x) WHEN cond) + 3 <= K`
    above; this test pins the equivalence on the outer-WHEN side."""
    data_sql = """
        SELECT CAST(id AS BIGINT), CAST(value AS DOUBLE), CAST(w AS BOOLEAN) FROM (
            VALUES (1, 6.0, true), (2, 4.0, true), (3, 10.0, false)
        ) t(id, value, w)
    """
    decide_sql = """
        SELECT id, value, w, x FROM (
            VALUES (1, 6.0, true), (2, 4.0, true), (3, 10.0, false)
        ) t(id, value, w)
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * value) + 3 <= 10 WHEN w
        MAXIMIZE SUM(x * value)
    """

    def build(oracle, data, cols, rows):
        n = len(data)
        vnames = [f"x_{i}" for i in range(n)]
        for v in vnames:
            oracle.add_variable(v, VarType.BINARY)
        # SUM_{w-true rows}(x_i * value_i) + 3 <= 10  →  SUM <= 7
        coeffs = {vnames[i]: data[i][1] for i in range(n) if data[i][2]}
        oracle.add_constraint(coeffs, "<=", 7.0, name="outer_when_offset")
        oracle.set_objective(
            {vnames[i]: data[i][1] for i in range(n)}, ObjSense.MAXIMIZE,
        )
        return n, 1

    def decidb_obj(rs, cs):
        xi = cs.index("x"); vi = cs.index("value")
        return sum(float(r[xi]) * float(r[vi]) for r in rs)

    _run_constraint_test(
        decidb_cli, duckdb_conn, oracle_solver, perf_tracker,
        test_id="alw_outer_when_offset",
        decide_sql=decide_sql, data_sql=data_sql,
        build_oracle=build, decidb_obj_fn=decidb_obj,
    )


# ===========================================================================
# K * WHEN fold with a per-row data column (not just a numeric constant).
# ===========================================================================
#
# The fold's correctness condition is "the non-WHEN factor contains no
# decide variable" — a data column satisfies this just as well as a
# literal. These tests pin that behavior with non-trivial per-row
# coefficients so a future regression (e.g., tightening the fold to
# require a constant) would fail a correctness check, not just a type
# check.

@pytest.mark.when
@pytest.mark.when_constraint
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_when_with_data_column_scalar_left(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """`col * (SUM(x) WHEN w) <= K` — data column on the left of the `*`.
    The fold rewrites to `WHEN(SUM(col * x), w)`, giving per-row
    coefficient `col[i]` for matching rows."""
    data_sql = """
        SELECT CAST(id AS BIGINT), CAST(col AS DOUBLE), CAST(w AS BOOLEAN) FROM (
            VALUES (1, 2.0, true), (2, 3.0, false), (3, 4.0, true), (4, 1.0, true)
        ) t(id, col, w)
    """
    decide_sql = """
        SELECT id, col, w, x FROM (
            VALUES (1, 2.0, true), (2, 3.0, false), (3, 4.0, true), (4, 1.0, true)
        ) t(id, col, w)
        DECIDE x IS REAL
        SUCH THAT col * (SUM(x) WHEN w) <= 20
            AND x <= 10
        MAXIMIZE SUM(x)
    """

    def build(oracle, data, cols, rows):
        n = len(data)
        vnames = [f"x_{i}" for i in range(n)]
        for v in vnames:
            oracle.add_variable(v, VarType.CONTINUOUS, lb=0.0, ub=10.0)
        # col_i * SUM_{w}(x_i) <= 20  →  per-row coefficient col_i for w-rows.
        coeffs = {vnames[i]: data[i][1] for i in range(n) if data[i][2]}
        oracle.add_constraint(coeffs, "<=", 20.0, name="col_mul_when")
        oracle.set_objective(
            {vnames[i]: 1.0 for i in range(n)}, ObjSense.MAXIMIZE,
        )
        return n, 1

    def decidb_obj(rs, cs):
        xi = cs.index("x")
        return sum(float(r[xi]) for r in rs)

    _run_constraint_test(
        decidb_cli, duckdb_conn, oracle_solver, perf_tracker,
        test_id="alw_col_mul_left",
        decide_sql=decide_sql, data_sql=data_sql,
        build_oracle=build, decidb_obj_fn=decidb_obj,
    )


@pytest.mark.when
@pytest.mark.when_constraint
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_when_with_data_column_scalar_right(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """`(SUM(x) WHEN w) * col <= K` — data column on the right of the `*`.
    The fold accepts either operand order and produces the same rewrite
    as the left-side variant."""
    data_sql = """
        SELECT CAST(id AS BIGINT), CAST(col AS DOUBLE), CAST(w AS BOOLEAN) FROM (
            VALUES (1, 2.0, true), (2, 3.0, false), (3, 4.0, true), (4, 1.0, true)
        ) t(id, col, w)
    """
    decide_sql = """
        SELECT id, col, w, x FROM (
            VALUES (1, 2.0, true), (2, 3.0, false), (3, 4.0, true), (4, 1.0, true)
        ) t(id, col, w)
        DECIDE x IS REAL
        SUCH THAT (SUM(x) WHEN w) * col <= 20
            AND x <= 10
        MAXIMIZE SUM(x)
    """

    def build(oracle, data, cols, rows):
        n = len(data)
        vnames = [f"x_{i}" for i in range(n)]
        for v in vnames:
            oracle.add_variable(v, VarType.CONTINUOUS, lb=0.0, ub=10.0)
        coeffs = {vnames[i]: data[i][1] for i in range(n) if data[i][2]}
        oracle.add_constraint(coeffs, "<=", 20.0, name="when_mul_col")
        oracle.set_objective(
            {vnames[i]: 1.0 for i in range(n)}, ObjSense.MAXIMIZE,
        )
        return n, 1

    def decidb_obj(rs, cs):
        xi = cs.index("x")
        return sum(float(r[xi]) for r in rs)

    _run_constraint_test(
        decidb_cli, duckdb_conn, oracle_solver, perf_tracker,
        test_id="alw_col_mul_right",
        decide_sql=decide_sql, data_sql=data_sql,
        build_oracle=build, decidb_obj_fn=decidb_obj,
    )


@pytest.mark.when
@pytest.mark.when_constraint
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_when_divided_by_data_column(
    decidb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """`(SUM(x) WHEN w) / col <= K` — WHEN-tagged aggregate divided by a
    per-row data column. The fold rewrites to `WHEN(SUM(x / col), w)`,
    giving per-row coefficient `1/col[i]` for matching rows."""
    data_sql = """
        SELECT CAST(id AS BIGINT), CAST(col AS DOUBLE), CAST(w AS BOOLEAN) FROM (
            VALUES (1, 2.0, true), (2, 4.0, false), (3, 1.0, true), (4, 0.5, true)
        ) t(id, col, w)
    """
    decide_sql = """
        SELECT id, col, w, x FROM (
            VALUES (1, 2.0, true), (2, 4.0, false), (3, 1.0, true), (4, 0.5, true)
        ) t(id, col, w)
        DECIDE x IS REAL
        SUCH THAT (SUM(x) WHEN w) / col <= 5
            AND x <= 10
        MAXIMIZE SUM(x)
    """

    def build(oracle, data, cols, rows):
        n = len(data)
        vnames = [f"x_{i}" for i in range(n)]
        for v in vnames:
            oracle.add_variable(v, VarType.CONTINUOUS, lb=0.0, ub=10.0)
        # SUM_{w}(x_i) / col_i <= 5  →  per-row coefficient 1/col_i for w-rows.
        coeffs = {vnames[i]: 1.0 / data[i][1] for i in range(n) if data[i][2]}
        oracle.add_constraint(coeffs, "<=", 5.0, name="when_div_col")
        oracle.set_objective(
            {vnames[i]: 1.0 for i in range(n)}, ObjSense.MAXIMIZE,
        )
        return n, 1

    def decidb_obj(rs, cs):
        xi = cs.index("x")
        return sum(float(r[xi]) for r in rs)

    _run_constraint_test(
        decidb_cli, duckdb_conn, oracle_solver, perf_tracker,
        test_id="alw_when_div_col",
        decide_sql=decide_sql, data_sql=data_sql,
        build_oracle=build, decidb_obj_fn=decidb_obj,
    )
