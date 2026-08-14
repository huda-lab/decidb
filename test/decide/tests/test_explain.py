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

from ._output_helpers import INTERNAL_TAG_RE


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


_BOX_CHARS = "│┌┐└┘├┤┬┴┼─"


def _unbox(out: str) -> str:
    """Replace EXPLAIN's border glyphs with spaces."""
    return "".join(" " if ch in _BOX_CHARS else ch for ch in out)


def _plan_text(out: str) -> str:
    """Readable one-line form of a plan, for assertion messages."""
    return re.sub(r"\s+", " ", _unbox(out))


def _shows(out: str, *fragments: str) -> bool:
    """True when every fragment appears, in order, in the rendered plan.

    Whitespace is removed from both sides before matching. The box wraps a long
    value at a fixed column and splits it *mid-token* — real output contains
    ``DECIMAL(18 ,0)`` and ``> =`` — so no assertion may depend on where the
    line breaks fall.

    Fragments match in order with gaps allowed, which is what lets a test name
    the user's own terms (``sum(``, ``l_quantity``, ``<=``, ``100``) without
    pinning the implicit casts the binder renders between them.

    Ordering also does real work on ``EXPLAIN ANALYZE``, whose output *echoes
    the submitted SQL* above the plan: a bare ``"PER" in out`` there is
    satisfied by the echo and never inspects the rendered node at all. Leading
    with an anchor like ``"Constraints:"`` forces the match past the echo.
    """
    text = re.sub(r"\s+", "", _unbox(out))
    pattern = ".*?".join(re.escape(re.sub(r"\s+", "", f)) for f in fragments)
    return re.search(pattern, text, re.DOTALL) is not None


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
    assert _shows(out, "Variables:", "x"), _plan_text(out)
    assert _shows(out, "Objective:", "MAXIMIZE", "sum(", "l_extendedprice"), _plan_text(out)
    assert _shows(out, "Constraints:", "sum(", "l_quantity", "<=", "100"), _plan_text(out)


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
    assert _shows(out, "Objective:", "MINIMIZE", "sum(", "l_quantity"), _plan_text(out)
    assert _shows(out, "Constraints:", "l_extendedprice", ">=", "5000"), _plan_text(out)


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
    assert _shows(out, "Variables:", "x"), _plan_text(out)
    # Both constraints render: the per-row bound and the aggregate.
    assert _shows(out, "Constraints:", "(x", "<=", "10"), _plan_text(out)
    assert _shows(out, "Constraints:", "ps_supplycost", "<=", "5000"), _plan_text(out)


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
    # Both variables listed under Variables, not merely present somewhere.
    assert _shows(out, "Variables:", "x", "y", "Objective:"), _plan_text(out)
    # All three constraints render, each carrying its own variable and bound.
    assert _shows(out, "Constraints:", "l_quantity", "<=", "50"), _plan_text(out)
    assert _shows(out, "Constraints:", "(y", "<=", "3"), _plan_text(out)
    assert _shows(out, "Constraints:", "sum(y)", "<=", "10"), _plan_text(out)


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
    assert _shows(
        out, "Constraints:", "l_quantity", "<=", "100", "WHEN", "l_returnflag", "'R'"
    ), f"WHEN suffix missing or incomplete in EXPLAIN:\n{_plan_text(out)}"


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
    assert _shows(
        out, "Constraints:", "l_extendedprice", "<=", "5000", "WHEN", "l_discount", ">=", "0.06"
    ), _plan_text(out)


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
    # The WHEN belongs to the first constraint only; the second must render
    # unconditionally rather than inheriting the suffix.
    assert _shows(
        out, "Constraints:", "l_quantity", "<=", "50", "WHEN", "l_returnflag", "'R'"
    ), _plan_text(out)
    assert _shows(out, "Constraints:", "sum(x)", "<=", "20"), _plan_text(out)


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
    assert _shows(
        out, "Constraints:", "sum(x)", "<=", "5", "PER", "s_nationkey"
    ), f"PER suffix missing or incomplete in EXPLAIN:\n{_plan_text(out)}"


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
    assert _shows(
        out, "Constraints:", "ps_supplycost", "<=", "1000", "PER", "ps_partkey"
    ), _plan_text(out)


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
# Constraints render as SQL, never as internal pipeline tags
# ===================================================================

@pytest.mark.explain
def test_explain_constraints_render_sql_not_internal_tags(decidb_cli):
    """Every constraint row shows the constraint, not the tag DECIDE stamped on it.

    DECIDE records a constraint's source clause in its expression *alias*, and
    ``GetName()`` returns the alias whenever one is set — so the Constraints
    section rendered ``__source_clause_0__`` per constraint instead of any SQL.
    The objective was unaffected (it carries no source-clause tag), which is why
    the section-header assertions elsewhere in this file stayed green.
    """
    sql = """
        SELECT l_orderkey, l_linenumber, x
        FROM lineitem WHERE l_orderkey < 100
        DECIDE x(BOOL)
        SUCH THAT SUM(x * l_quantity) <= 100 AND x <= 1
        MAXIMIZE SUM(x * l_extendedprice)
    """
    text = _plan_text(_explain(decidb_cli, sql))

    assert INTERNAL_TAG_RE.search(text) is None, (
        f"internal tag leaked into EXPLAIN: "
        f"{INTERNAL_TAG_RE.search(text).group(0)!r}\n{text}"
    )
    # Both constraints must be recognizable. Matched loosely on purpose: the
    # binder's implicit casts are part of this rendering (`sum((CAST(x AS
    # DECIMAL(18,0)) * l_quantity))`), and their spelling is not what this
    # test pins — only that the user's own terms are what reaches the page.
    assert re.search(r"sum\(.*l_quantity.*\)\s*<=", text), text
    assert "x" in text


@pytest.mark.explain
@pytest.mark.per_clause
def test_explain_per_key_renders_column_name_not_binding(decidb_cli):
    """A PER key prints its column name, not a binding index.

    Guards the shape of the tag-stripping fix as much as the fix itself: a PER
    key is a column reference whose alias IS its name, so stripping the alias
    outright (rather than keeping what survives) would degrade ``PER grp`` to
    ``PER #[3.1]``.
    """
    sql = """
        SELECT s_suppkey, s_acctbal, x FROM supplier
        DECIDE x(BOOL)
        SUCH THAT SUM(x) <= 5 PER s_nationkey
        MAXIMIZE SUM(x * s_acctbal)
    """
    text = _plan_text(_explain(decidb_cli, sql))

    assert "PER s_nationkey" in text, text
    assert "#[" not in text, f"PER key rendered as a binding index:\n{text}"


@pytest.mark.explain
def test_explain_json_constraints_render_sql_not_internal_tags(decidb_cli):
    """The JSON renderer reads the same walker, so it leaked the same tags."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_returnflag, x
        FROM lineitem WHERE l_orderkey < 100
        DECIDE x(BOOL)
        SUCH THAT SUM(x * l_quantity) <= 100 WHEN l_returnflag = 'R'
        MAXIMIZE SUM(x * l_extendedprice)
    """
    out = _explain_json(decidb_cli, sql)

    assert INTERNAL_TAG_RE.search(out) is None, (
        f"internal tag leaked into JSON EXPLAIN: "
        f"{INTERNAL_TAG_RE.search(out).group(0)!r}\n{out}"
    )
    assert re.search(r"sum\(.*l_quantity.*\)\s*<=", out), out
    assert "WHEN (l_returnflag" in out, out


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
    assert _shows(
        out, "Constraints:", "l_quantity", "<=", "50", "PER", "l_returnflag"
    ), f"PER missing from the constraint row:\n{_plan_text(out)}"
    assert _shows(out, "Constraints:", "sum(x)", "<=", "30"), _plan_text(out)
    # The objective carries the only WHEN here; it must render as a postfix
    # suffix on the Objective row. Before the objective/constraint rendering was
    # unified, an objective WHEN leaked out as "(... AND ...)" and no WHEN
    # appeared at all.
    assert _shows(
        out, "Objective:", "MAXIMIZE", "l_extendedprice", "WHEN", "l_returnflag", "'R'"
    ), f"objective WHEN missing from EXPLAIN:\n{_plan_text(out)}"


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
    assert _shows(
        out, "Objective:", "MAXIMIZE", "l_extendedprice", "WHEN", "l_returnflag", "'R'"
    ), f"objective WHEN missing from EXPLAIN:\n{_plan_text(out)}"
    assert _shows(out, "Constraints:", "sum(x)", "<=", "20"), _plan_text(out)
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
    assert _shows(out, '"Variables"', '"x"'), out
    assert _shows(out, '"Objective"', "MAXIMIZE", "sum(", "l_extendedprice"), out
    assert _shows(out, '"Constraints"', "sum(", "l_quantity", "<=", "100"), out


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
    assert _shows(
        out, '"Constraints"', "l_quantity", "<=", "100", "WHEN", "l_returnflag", "'R'"
    ), out


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
    assert _shows(
        out, '"Constraints"', "sum(x)", "<=", "5", "PER", "s_nationkey"
    ), out


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
    assert _shows(out, '"Constraints"', "sum(", "l_quantity", "<=", "100"), out


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
    assert _shows(out, "Constraints:", "sum(", "l_quantity", "<=", "100"), _plan_text(out)


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
    # Anchored at "Constraints:" so the match cannot be satisfied by the SQL
    # that EXPLAIN ANALYZE echoes above the plan.
    assert _shows(
        out, "Constraints:", "l_quantity", "<=", "100", "WHEN", "l_returnflag", "'R'"
    ), _plan_text(out)


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
    assert _shows(
        out, "Constraints:", "sum(x)", "<=", "5", "PER", "s_nationkey"
    ), _plan_text(out)


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
    assert _shows(
        out, "Constraints:", "l_quantity", "<=", "50", "WHEN", "l_returnflag", "'R'"
    ), _plan_text(out)
    assert _shows(out, "Constraints:", "sum(x)", "<=", "20"), _plan_text(out)


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
    assert _shows(out, "Variables:", "x"), _plan_text(out)
    assert _shows(out, "Objective:", "MAXIMIZE", "sum(", "l_extendedprice"), _plan_text(out)
    assert _shows(out, "Constraints:", "sum(", "l_quantity", "<=", "100"), _plan_text(out)


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
    assert _shows(
        out, "Constraints:", "l_quantity", "<=", "100", "WHEN", "l_returnflag", "'R'"
    ), _plan_text(out)


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
    assert _shows(
        out, "Constraints:", "sum(x)", "<=", "5", "PER", "s_nationkey"
    ), _plan_text(out)
