"""Tests for EXPLAIN output of DECIDE queries on TPC-H data.

Covers:
  - EXPLAIN text output: DECIDE node, Variables, Objective, Constraints
  - WHEN clause display in EXPLAIN
  - PER clause display in EXPLAIN
  - WHEN + PER combined
  - EXPLAIN (FORMAT JSON) structure
  - EXPLAIN ANALYZE execution and timing
  - Logical vs physical plan output
"""

import json
import re

import pytest


@pytest.fixture(autouse=True)
def _reset_explain_output(decidb_cli):
    """Ensure explain_output is reset to default before each test."""
    decidb_cli.execute_raw("pragma explain_output='physical_only'")
    yield
    decidb_cli.execute_raw("pragma explain_output='physical_only'")


# ── helpers ──────────────────────────────────────────────────────────

def _explain(decidb_cli, sql: str) -> str:
    result = decidb_cli.execute_raw(f"EXPLAIN {sql}")
    return result.stdout


def _explain_json(decidb_cli, sql: str, *, logical: bool = False) -> str:
    if logical:
        decidb_cli.execute_raw("pragma explain_output='optimized_only'")
    result = decidb_cli.execute_raw(f"EXPLAIN (FORMAT JSON) {sql}")
    return result.stdout


def _explain_analyze(decidb_cli, sql: str) -> str:
    result = decidb_cli.execute_raw(f"EXPLAIN ANALYZE {sql}")
    return result.stdout


# ===================================================================
# Basic EXPLAIN on TPC-H queries
# ===================================================================

@pytest.mark.explain
def test_explain_basic_knapsack(decidb_cli):
    """EXPLAIN on a basic knapsack query shows DECIDE, Variables, Objective, Constraints."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, x
        FROM lineitem WHERE l_orderkey < 100
        DECIDE x(BOOL)
        SUCH THAT SUM(x * l_quantity) <= 100
        MAXIMIZE SUM(x * l_extendedprice)
    """
    out = _explain(decidb_cli, sql)
    assert "DECIDE" in out, f"DECIDE node missing from EXPLAIN:\n{out}"
    assert "Variables" in out
    assert "x" in out
    assert "MAXIMIZE" in out
    assert "Constraints" in out


@pytest.mark.explain
def test_explain_minimize(decidb_cli):
    """EXPLAIN shows MINIMIZE for a minimize objective."""
    sql = """
        SELECT l_orderkey, l_linenumber, x
        FROM lineitem WHERE l_orderkey < 100
        DECIDE x(BOOL)
        SUCH THAT SUM(x * l_extendedprice) >= 5000
        MINIMIZE SUM(x * l_quantity)
    """
    out = _explain(decidb_cli, sql)
    assert "MINIMIZE" in out, f"MINIMIZE missing from EXPLAIN:\n{out}"


@pytest.mark.explain
def test_explain_integer_variable(decidb_cli):
    """EXPLAIN works for INTEGER decision variables."""
    sql = """
        SELECT ps_partkey, ps_availqty, ps_supplycost, x
        FROM partsupp WHERE ps_partkey < 50
        DECIDE x(INT)
        SUCH THAT x <= 10 AND SUM(x * ps_supplycost) <= 5000
        MAXIMIZE SUM(x * ps_availqty)
    """
    out = _explain(decidb_cli, sql)
    assert "DECIDE" in out
    assert "Variables" in out


@pytest.mark.explain
def test_explain_multi_variable(decidb_cli):
    """EXPLAIN with multiple decision variables shows all of them."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity, x, y
        FROM lineitem WHERE l_orderkey < 50
        DECIDE x(BOOL), y(INT)
        SUCH THAT SUM(x * l_quantity) <= 50
            AND y <= 3
            AND SUM(y) <= 10
        MAXIMIZE SUM(x * l_extendedprice + y)
    """
    out = _explain(decidb_cli, sql)
    assert "DECIDE" in out
    assert "x" in out
    assert "y" in out


# ===================================================================
# WHEN clause in EXPLAIN
# ===================================================================

@pytest.mark.explain
@pytest.mark.when_constraint
def test_explain_when_string_filter(decidb_cli):
    """EXPLAIN shows WHEN suffix for conditional constraints on TPC-H data."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity,
               l_returnflag, x
        FROM lineitem WHERE l_orderkey < 100
        DECIDE x(BOOL)
        SUCH THAT SUM(x * l_quantity) <= 100 WHEN l_returnflag = 'R'
        MAXIMIZE SUM(x * l_extendedprice)
    """
    out = _explain(decidb_cli, sql)
    assert "DECIDE" in out
    assert "WHEN" in out, f"WHEN clause missing from EXPLAIN:\n{out}"


@pytest.mark.explain
@pytest.mark.when_constraint
def test_explain_when_numeric_comparison(decidb_cli):
    """EXPLAIN shows WHEN for numeric comparison filters."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_discount, x
        FROM lineitem WHERE l_orderkey < 100
        DECIDE x(BOOL)
        SUCH THAT SUM(x * l_extendedprice) <= 5000 WHEN l_discount >= 0.06
        MAXIMIZE SUM(x * l_extendedprice)
    """
    out = _explain(decidb_cli, sql)
    assert "WHEN" in out


@pytest.mark.explain
@pytest.mark.when_constraint
def test_explain_when_mixed_constraints(decidb_cli):
    """EXPLAIN with one WHEN-filtered + one unconditional constraint."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity,
               l_returnflag, x
        FROM lineitem WHERE l_orderkey < 100
        DECIDE x(BOOL)
        SUCH THAT SUM(x * l_quantity) <= 50 WHEN l_returnflag = 'R'
            AND SUM(x) <= 20
        MAXIMIZE SUM(x * l_extendedprice)
    """
    out = _explain(decidb_cli, sql)
    assert "WHEN" in out
    assert "Constraints" in out


# ===================================================================
# PER clause in EXPLAIN
# ===================================================================

@pytest.mark.explain
@pytest.mark.per_clause
def test_explain_per_basic(decidb_cli):
    """EXPLAIN shows PER suffix for group-partitioned constraints."""
    sql = """
        SELECT s_suppkey, s_acctbal, x FROM supplier
        DECIDE x(BOOL)
        SUCH THAT SUM(x) <= 5 PER s_nationkey
        MAXIMIZE SUM(x * s_acctbal)
    """
    out = _explain(decidb_cli, sql)
    assert "PER" in out, f"PER clause missing from EXPLAIN:\n{out}"


@pytest.mark.explain
@pytest.mark.per_clause
def test_explain_per_integer(decidb_cli):
    """EXPLAIN shows PER for integer variable with weighted constraint."""
    sql = """
        SELECT ps_partkey, ps_availqty, ps_supplycost, x
        FROM partsupp WHERE ps_partkey < 50
        DECIDE x(INT)
        SUCH THAT SUM(x * ps_supplycost) <= 1000 PER ps_partkey
        MAXIMIZE SUM(x * ps_availqty)
    """
    out = _explain(decidb_cli, sql)
    assert "PER" in out


@pytest.mark.explain
@pytest.mark.per_clause
def test_explain_multi_column_per_parenthesized(decidb_cli):
    """EXPLAIN prints multi-column PER in the supported parenthesized syntax."""
    sql = """
        SELECT l_orderkey, l_returnflag, l_linestatus, x
        FROM lineitem WHERE l_orderkey < 100
        DECIDE x(BOOL)
        SUCH THAT SUM(x) <= 3 PER (l_returnflag, l_linestatus)
        MAXIMIZE SUM(x)
    """
    out = _explain_json(decidb_cli, sql)
    assert "PER (l_returnflag, l_linestatus)" in out, out


# ===================================================================
# WHEN + PER combined
# ===================================================================

@pytest.mark.explain
@pytest.mark.when_constraint
@pytest.mark.per_clause
def test_explain_when_and_per(decidb_cli):
    """EXPLAIN shows both WHEN and PER when used together."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity,
               l_returnflag, x
        FROM lineitem WHERE l_orderkey < 100
        DECIDE x(BOOL)
        SUCH THAT SUM(x * l_quantity) <= 50 PER l_returnflag
            AND SUM(x) <= 30
        MAXIMIZE SUM(x * l_extendedprice) WHEN l_returnflag = 'R'
    """
    out = _explain(decidb_cli, sql)
    assert "PER" in out, f"PER missing from EXPLAIN:\n{out}"
    # The objective carries the only WHEN here; it must render as a postfix
    # suffix. Before the objective/constraint rendering was unified, an
    # objective WHEN leaked out as "(... AND ...)" and no WHEN appeared at all.
    assert "WHEN" in out, f"objective WHEN missing from EXPLAIN:\n{out}"


# ===================================================================
# WHEN on the objective (rendered symmetrically with constraints)
# ===================================================================

@pytest.mark.explain
@pytest.mark.when_constraint
def test_explain_objective_when_postfix(decidb_cli):
    """An objective WHEN renders as a postfix suffix, not the generic
    conjunction form "(<obj> AND <cond>)".

    The objective and constraint EXPLAIN rows share one tagged-expression
    renderer. The single constraint here has no AND/WHEN of its own, so the
    only conditional is on the objective: WHEN must appear and no spurious
    " AND " (from a leaked conjunction) may appear anywhere in the output.
    """
    sql = """
        SELECT l_orderkey, l_extendedprice, l_returnflag, x
        FROM lineitem WHERE l_orderkey < 100
        DECIDE x(BOOL)
        SUCH THAT SUM(x) <= 20
        MAXIMIZE SUM(x * l_extendedprice) WHEN l_returnflag = 'R'
    """
    out = _explain(decidb_cli, sql)
    assert "WHEN" in out, f"objective WHEN missing from EXPLAIN:\n{out}"
    assert " AND " not in out, (
        "objective WHEN leaked as a conjunction '(... AND ...)' instead of a "
        f"postfix ' WHEN ' suffix:\n{out}"
    )


# ===================================================================
# EXPLAIN (FORMAT JSON) on TPC-H
# ===================================================================

@pytest.mark.explain
def test_explain_json_structure(decidb_cli):
    """EXPLAIN (FORMAT JSON) contains expected keys for DECIDE node."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, x
        FROM lineitem WHERE l_orderkey < 100
        DECIDE x(BOOL)
        SUCH THAT SUM(x * l_quantity) <= 100
        MAXIMIZE SUM(x * l_extendedprice)
    """
    out = _explain_json(decidb_cli, sql)
    assert '"DECIDE"' in out or '"name": "DECIDE"' in out or "DECIDE" in out
    assert "Variables" in out
    assert "Objective" in out
    assert "Constraints" in out


@pytest.mark.explain
@pytest.mark.when_constraint
def test_explain_json_when(decidb_cli):
    """JSON EXPLAIN includes WHEN information in constraint display."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity,
               l_returnflag, x
        FROM lineitem WHERE l_orderkey < 100
        DECIDE x(BOOL)
        SUCH THAT SUM(x * l_quantity) <= 100 WHEN l_returnflag = 'R'
        MAXIMIZE SUM(x * l_extendedprice)
    """
    out = _explain_json(decidb_cli, sql)
    assert "WHEN" in out


@pytest.mark.explain
@pytest.mark.per_clause
def test_explain_json_per(decidb_cli):
    """JSON EXPLAIN includes PER information in constraint display."""
    sql = """
        SELECT s_suppkey, s_acctbal, x FROM supplier
        DECIDE x(BOOL)
        SUCH THAT SUM(x) <= 5 PER s_nationkey
        MAXIMIZE SUM(x * s_acctbal)
    """
    out = _explain_json(decidb_cli, sql)
    assert "PER" in out


@pytest.mark.explain
def test_explain_json_logical_plan(decidb_cli):
    """JSON EXPLAIN of logical plan contains DECIDE node."""
    sql = """
        SELECT l_orderkey, x FROM lineitem WHERE l_orderkey < 100
        DECIDE x(BOOL)
        SUCH THAT SUM(x * l_quantity) <= 100
        MAXIMIZE SUM(x * l_extendedprice)
    """
    out = _explain_json(decidb_cli, sql, logical=True)
    assert "DECIDE" in out


# ===================================================================
# EXPLAIN ANALYZE on TPC-H
# ===================================================================

@pytest.mark.explain
def test_explain_analyze_basic(decidb_cli):
    """EXPLAIN ANALYZE executes the query and shows DECIDE with timing info."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, x
        FROM lineitem WHERE l_orderkey < 100
        DECIDE x(BOOL)
        SUCH THAT SUM(x * l_quantity) <= 100
        MAXIMIZE SUM(x * l_extendedprice)
    """
    out = _explain_analyze(decidb_cli, sql)
    assert "DECIDE" in out, f"DECIDE missing from EXPLAIN ANALYZE:\n{out}"


@pytest.mark.explain
@pytest.mark.when_constraint
def test_explain_analyze_when(decidb_cli):
    """EXPLAIN ANALYZE works with WHEN clause queries."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity,
               l_returnflag, x
        FROM lineitem WHERE l_orderkey < 100
        DECIDE x(BOOL)
        SUCH THAT SUM(x * l_quantity) <= 100 WHEN l_returnflag = 'R'
        MAXIMIZE SUM(x * l_extendedprice)
    """
    out = _explain_analyze(decidb_cli, sql)
    assert "DECIDE" in out


@pytest.mark.explain
@pytest.mark.per_clause
def test_explain_analyze_per(decidb_cli):
    """EXPLAIN ANALYZE works with PER clause queries."""
    sql = """
        SELECT s_suppkey, s_acctbal, x FROM supplier
        DECIDE x(BOOL)
        SUCH THAT SUM(x) <= 5 PER s_nationkey
        MAXIMIZE SUM(x * s_acctbal)
    """
    out = _explain_analyze(decidb_cli, sql)
    assert "DECIDE" in out


@pytest.mark.explain
def test_explain_analyze_multiple_constraints(decidb_cli):
    """EXPLAIN ANALYZE with multiple constraints still produces output."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity,
               l_returnflag, x
        FROM lineitem WHERE l_orderkey < 100
        DECIDE x(BOOL)
        SUCH THAT SUM(x * l_quantity) <= 50 WHEN l_returnflag = 'R'
            AND SUM(x) <= 20
        MAXIMIZE SUM(x * l_extendedprice)
    """
    out = _explain_analyze(decidb_cli, sql)
    assert "DECIDE" in out


# ===================================================================
# Logical plan output
# ===================================================================

@pytest.mark.explain
def test_explain_logical_plan(decidb_cli):
    """Logical plan output (optimized_only) shows DECIDE node with all sections."""
    decidb_cli.execute_raw("pragma explain_output='optimized_only'")
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, x
        FROM lineitem WHERE l_orderkey < 100
        DECIDE x(BOOL)
        SUCH THAT SUM(x * l_quantity) <= 100
        MAXIMIZE SUM(x * l_extendedprice)
    """
    out = _explain(decidb_cli, sql)
    assert "DECIDE" in out
    assert "Variables" in out
    assert "Objective" in out
    assert "Constraints" in out


@pytest.mark.explain
@pytest.mark.when_constraint
def test_explain_logical_when(decidb_cli):
    """Logical plan shows WHEN in constraint display."""
    decidb_cli.execute_raw("pragma explain_output='optimized_only'")
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity,
               l_returnflag, x
        FROM lineitem WHERE l_orderkey < 100
        DECIDE x(BOOL)
        SUCH THAT SUM(x * l_quantity) <= 100 WHEN l_returnflag = 'R'
        MAXIMIZE SUM(x * l_extendedprice)
    """
    out = _explain(decidb_cli, sql)
    assert "WHEN" in out


@pytest.mark.explain
@pytest.mark.per_clause
def test_explain_logical_per(decidb_cli):
    """Logical plan shows PER in constraint display."""
    decidb_cli.execute_raw("pragma explain_output='optimized_only'")
    sql = """
        SELECT s_suppkey, s_acctbal, x FROM supplier
        DECIDE x(BOOL)
        SUCH THAT SUM(x) <= 5 PER s_nationkey
        MAXIMIZE SUM(x * s_acctbal)
    """
    out = _explain(decidb_cli, sql)
    assert "PER" in out
