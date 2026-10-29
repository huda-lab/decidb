"""A DuckDB pass that moves bindings below a direct-solve result must not break what a parent reads from it.

The result adapter stands where the DECIDE node stood, and the operators above read their columns from it. Several
DuckDB passes run after direct solve and rewrite the bindings above an operator they change: compressed
materialization inserts a decompressing projection above an ORDER BY, a DISTINCT or a join. Each case below puts that
pass under a DECIDE source, lets the parent read a column the rule uses, a column it does not use, or only the
decision, and compares the direct answer with the solver's.

`require` makes a case fail when the rule stops applying; `auto` is the default mode a user runs. A case also checks
that its pass fired, by running EXPLAIN with that optimizer disabled and expecting a different plan.
"""

import math

import pytest

from ._direct_oracle_common import _allow_direct_path  # noqa: F401  (autouse fixture)
from ._direct_oracle_common import outcome

# Rows of the small source. Scores `(i * 37) % 211` are distinct for i < 211, so the S1 optimum is unique.
_ROWS = 200

# (decision clause, how the rule reads the source). Both rules read `score`; A1 also reads `cap`.
_RULES = {
    "s1": "DECIDE x(BOOL) SUCH THAT SUM(x) <= 20 MAXIMIZE SUM(score * x)",
    "a1": "DECIDE x(REAL) SUCH THAT x BETWEEN 0 AND cap MINIMIZE SUM(POWER(x - score, 2))",
}

# What the parent reads of the DECIDE result: a column the rule uses, a wide column it does not use, and only x.
_READS = {
    "rule_column": "SUM(score)",
    "unused_column": "SUM(length(payload))",
    "decision_only": "SUM(x)",
}

_SMALL_TABLE = f"""
    CREATE TEMP TABLE bb_src AS
    SELECT i AS id, ((i * 37) % 211)::DOUBLE AS score, (5 + i % 7)::DOUBLE AS cap, 'payload-' || i AS payload
    FROM range({_ROWS}) t(i);
"""

# Both sides of the join are over the compressed-materialization build-side threshold (1,048,576 rows), and the
# function filter references both sides, so it cannot move below the join and shrink the estimate.
_BIG_TABLES = """
    CREATE TEMP TABLE bb_big AS
    SELECT i AS k, ((i * 37) % 1000003)::DOUBLE AS score, (5 + i % 7)::DOUBLE AS cap FROM range(1100000) t(i);
    CREATE TEMP TABLE bb_big2 AS SELECT i AS k, 'payload-' || i AS payload FROM range(1100000) t(i);
"""

# source name -> (setup, optimizer whose firing the case checks, query builder from (read, decide clause)).
# The builder returns the whole query; the DECIDE clause follows the source.


def _order_by(read, decide):
    return f"SELECT {read} FROM (SELECT * FROM bb_src ORDER BY id DESC) t {decide}"


def _distinct(read, decide):
    # DISTINCT sits above the select's projection, so it is the source's top operator. A GROUP BY is not: the select's
    # projection covers it, and a DECIDE cannot follow a GROUP BY at the same level.
    return f"SELECT {read} FROM (SELECT DISTINCT * FROM bb_src) t {decide}"


def _join(read, decide):
    # The join is the source itself, with no projection above it.
    return (
        f"SELECT {read} FROM bb_big a JOIN bb_big2 b ON a.k = b.k "
        f"WHERE list_contains([0], (a.k + b.k) % 50000) {decide}"
    )


def _nested(read, decide):
    # A DECIDE over the result of another. The inner source ends in ORDER BY, so the outer rule's own source columns
    # are the inner result adapter's.
    inner = (
        "SELECT * FROM (SELECT * FROM bb_src ORDER BY id DESC) a "
        "DECIDE pick(BOOL) SUCH THAT SUM(pick) <= 100 MAXIMIZE SUM(score * pick)"
    )
    return f"SELECT {read} FROM ({inner}) t {decide}"


_SOURCES = {
    "order_by": (_SMALL_TABLE, "compressed_materialization", _order_by),
    "distinct": (_SMALL_TABLE, "compressed_materialization", _distinct),
    "join": (_BIG_TABLES, "compressed_materialization", _join),
    "nested": (_SMALL_TABLE, "compressed_materialization", _nested),
}


# The largest score of each source's rows that reach the DECIDE. The join keeps the rows whose key is a multiple of 25000
# (its filter), the others hold every `(i * 37) % 211`.
_MAX_SCORE = {
    "join": max((k * 37) % 1000003 for k in range(0, 1100000, 25000)),
    "order_by": 210,
    "distinct": 210,
    "nested": 210,
}


def _solver_model_has_a_large_number(source, rule):
    """A1 minimizes `SUM(POWER(x - score, 2))`, so the linear coefficient of `x` on a row is `-2 * score`; S1
    maximizes `SUM(score * x)`, so it is `score`. The solver (`off`) warns when one reaches 1e6; a direct plan builds
    no model."""
    return (2 if rule == "a1" else 1) * _MAX_SCORE[source] >= 1e6


def _close(left, right):
    assert len(left) == len(right), (left, right)
    for a, b in zip(left, right):
        assert len(a) == len(b), (a, b)
        for u, v in zip(a, b):
            if isinstance(u, float) or isinstance(v, float):
                assert math.isclose(u, v, rel_tol=1e-9, abs_tol=1e-7), (a, b)
            else:
                assert u == v, (a, b)


def _plan(cli, setup, query, mode, disabled=None):
    disable = f"SET disabled_optimizers='{disabled}';" if disabled else ""
    result = cli.execute_raw(f"SET decide_direct_solve='{mode}'; {disable} {setup} EXPLAIN {query}")
    assert not result.stderr.strip(), result.stderr
    return result.stdout


# (source, reads, rules). Each source is a pass that rewrites bindings, so it needs only the reads that reach a binding
# no other read of that source reaches:
#   order_by: all three reads. A column the rule uses, a column it does not use (the rule's plan can drop it), and
#       x alone, whose binding belongs to the DECIDE node and not to its source.
#   distinct: one read; its pass differs from order_by's only in the operator it wraps.
#   join: a column from each side, because compressed materialization packs each side of a join separately.
#   nested: one read per rule. The outer DECIDE reads the inner result adapter's bindings, so the inner rewrite must
#       already be done when the outer rule runs.
_CASES = (
    [("order_by", read, rule) for read in _READS for rule in _RULES]
    + [("distinct", "rule_column", rule) for rule in _RULES]
    + [("join", read, rule) for read in ("rule_column", "unused_column") for rule in _RULES]
    + [("nested", "rule_column", rule) for rule in _RULES]
)


@pytest.mark.correctness
@pytest.mark.parametrize("source,read,rule", _CASES, ids=["-".join(case) for case in _CASES])
def test_a_pass_that_moves_bindings_above_a_source_keeps_the_parents_reads_bound(decidb_cli, source, read, rule):
    setup, optimizer, build = _SOURCES[source]
    query = build(_READS[read], _RULES[rule])

    plan = _plan(decidb_cli, setup, query, "require")
    assert "Direct solve rule" in plan
    without_pass = _plan(decidb_cli, setup, query, "require", disabled=optimizer)
    assert plan != without_pass, f"{optimizer} did not change the plan"

    solver = outcome(decidb_cli, setup + query, "off", _solver_model_has_a_large_number(source, rule))
    assert solver[0] == "ok", solver
    for mode in ("auto", "require"):
        direct = outcome(decidb_cli, setup + query, mode)
        assert direct[0] == "ok", (mode, direct)
        _close(direct[1], solver[1])
