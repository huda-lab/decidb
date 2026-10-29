"""Time-limit error path tests.

These verify the contract around DECIDB_TIME_LIMIT, the env-var knob that
caps Gurobi solve time:

  - When set to a small value, a pathologically hard MIQP hits the limit
    and the friendly "hit the time limit" message surfaces (rather than
    the generic Gurobi-status fallback or a SIGKILL from the test harness).
  - When unset, the 300s default applies and normal queries finish without
    interference.
  - When set to garbage (negative, zero, non-numeric, empty), the value
    is silently ignored and the 300s default applies — no exception
    bubbles up to the user.

Historically the limit was hard-coded to 300s after Gurobi's auto-load of
gurobi.env, so user-supplied limits were silently clobbered. The env var
is the supported override mechanism.

Also serves as a regression test for the GRB_TIME_LIMIT status-constant
drift previously fixed in src/decidb/gurobi/gurobi_loader.hpp — if those
constants drift again, the friendly-error branch in gurobi_solver.cpp
becomes unreachable and case 1 below would catch it.
"""

from __future__ import annotations

import re

import pytest


# Symmetric MIQP: ~105 INTEGER vars (all interchangeable), equality
# SUM = 30, quadratic objective. The optimal-set cardinality is roughly
# C(105, 30) ~ 10^25 — Gurobi cannot prune the symmetric tree within
# a small time budget on any reasonable hardware, guaranteeing status
# 9 (TIME_LIMIT) when the limit is short.
_PATHOLOGICAL_MIQP_SQL = """
    SELECT l_orderkey, l_linenumber, qty
    FROM lineitem
    WHERE l_orderkey < 100
    DECIDE qty(INT)
    SUCH THAT qty <= 10 AND SUM(qty) = 30
    MINIMIZE SUM(POWER(qty - 2, 2))
"""

# Cheap query that any sane solver finishes in well under a second.
# Used to verify the env-var read path doesn't break normal workloads.
_FAST_QUERY_SQL = """
    SELECT s_suppkey, x FROM supplier
    DECIDE x(BOOL)
    SUCH THAT SUM(x) = 5
    MAXIMIZE SUM(x * s_acctbal)
    LIMIT 10
"""


# Each row's ``IN`` domain is a clique of binaries that all sit in the objective, so HiGHS's MIP setup
# (``HighsCliqueTable::cliquePartition``) spends about a minute querying every pair of the 270,000 binaries.
_CLIQUE_PARTITION_SQL = """
    SELECT i, x
    FROM (SELECT i, (i * 7919) % 1000 + 1 AS w, (i * 104729) % 97 + 1 AS c FROM range(90000) t(i))
    DECIDE x(INT)
    SUCH THAT x IN (0, 1, 3)
        AND SUM(x * c) <= 1000000
    MAXIMIZE SUM(x * w)
"""


def _run_decidb(cli, sql, *, time_limit=None, subprocess_timeout=30):
    """Run ``sql`` on the Gurobi CLI with optional DECIDB_TIME_LIMIT in the env.

    time_limit can be a number (seconds) or a string (e.g. ``"abc"``,
    ``"-5"``, ``""``) so invalid-value cases can pass arbitrary garbage.
    Returns the CompletedProcess.
    """
    if time_limit is not None:
        cli = cli.with_env({"DECIDB_TIME_LIMIT": str(time_limit)})
    return cli.execute_raw(sql, timeout=subprocess_timeout)


@pytest.mark.error
class TestTimeLimit:
    """DECIDB_TIME_LIMIT env-var contract."""

    def test_env_var_surfaces_friendly_error(self, decidb_cli_gurobi):
        """Short DECIDB_TIME_LIMIT must surface the friendly TIME_LIMIT message.

        Primary regression test for the bug: hardcoded 300s used to clobber
        any user-supplied limit, so a 1s ceiling could not fire and the
        subprocess SIGKILL'd at 30s with no error text. This case verifies
        both that the env var is plumbed through to Gurobi AND that the
        GRB_TIME_LIMIT branch produces the documented user message.
        """
        result = _run_decidb(
            decidb_cli_gurobi,
            _PATHOLOGICAL_MIQP_SQL,
            time_limit=1,
            subprocess_timeout=15,
        )
        combined = result.stderr + result.stdout
        assert re.search(r"(?i)hit the time limit", combined), (
            f"Expected 'hit the time limit' message but got:\n"
            f"  stdout: {result.stdout[:400]}\n"
            f"  stderr: {result.stderr[:400]}"
        )

    def test_default_does_not_break_normal_queries(self, decidb_cli_gurobi):
        """No env var → 300s default → cheap query completes successfully.

        Guards against an accidental default change (e.g. someone lowering
        the constant to 1s) or a regression in the env-var read path that
        silently neutralizes the limit.
        """
        result = _run_decidb(
            decidb_cli_gurobi,
            _FAST_QUERY_SQL,
            time_limit=None,
            subprocess_timeout=10,
        )
        combined = result.stderr + result.stdout
        assert not re.search(r"(?i)hit the time limit", combined), (
            f"Cheap query unexpectedly hit a time limit:\n"
            f"  stdout: {result.stdout[:400]}\n"
            f"  stderr: {result.stderr[:400]}"
        )
        # Some output (a result row or solver-license preamble) must be
        # present — empty stdout would indicate the query never ran.
        assert result.stdout.strip(), (
            f"No stdout from cheap query; stderr was:\n{result.stderr[:400]}"
        )

    def test_highs_limit_bounds_clique_partition(self, decidb_cli_highs):
        """HiGHS stops its objective clique partition at DECIDB_TIME_LIMIT.

        Each row's ``IN`` domain is a clique of binaries that all sit in the objective, so HiGHS's MIP setup
        (``HighsCliqueTable::cliquePartition``) queries every pair of the 270,000 binaries before it checks its
        clock again. Without the time-limit check in that loop (third_party patch SOL-1) this solve runs past
        30 s; with it, HiGHS returns TIME_LIMIT about 1 s in and the subprocess timeout is never reached.
        """
        result = _run_decidb(decidb_cli_highs, _CLIQUE_PARTITION_SQL, time_limit=1, subprocess_timeout=15)
        combined = result.stderr + result.stdout
        assert re.search(r"(?i)hit the time limit", combined), (
            f"Expected 'hit the time limit' message but got:\n"
            f"  stdout: {result.stdout[:400]}\n"
            f"  stderr: {result.stderr[:400]}"
        )

    def test_highs_interrupt_reaches_clique_partition(self, decidb_cli_highs):
        """Ctrl-C stops HiGHS inside its objective clique partition, not after it.

        The SOL-1 check in that loop asks HiGHS's MIP limits, which include the interrupt callback, so a Ctrl-C ~3 s
        in stops the solve at once. A clock-only check there would sit out the whole loop (about a minute) under the
        60 s limit, and the subprocess timeout would kill it.
        """
        cli = decidb_cli_highs.with_env({"DECIDB_TIME_LIMIT": "60"})
        result, latency = cli.execute_interrupted(_CLIQUE_PARTITION_SQL, after=3.0, timeout=15)
        assert "stopped at your request" in result.stderr.lower(), result.stderr[:800]
        assert latency < 10.0, f"interrupt took {latency:.2f}s — expected a stop inside the clique partition"

    @pytest.mark.parametrize("garbage", ["abc", "-5", "0", ""])
    def test_invalid_env_var_falls_back_to_default(self, decidb_cli_gurobi, garbage):
        """Garbage DECIDB_TIME_LIMIT values must silently fall back to default.

        ``std::stod`` throws on non-numeric input; negative/zero values are
        nonsensical as a time budget. The implementation must catch the
        exception and reject non-positive parsed values without leaking
        either failure mode to the user.
        """
        result = _run_decidb(
            decidb_cli_gurobi,
            _FAST_QUERY_SQL,
            time_limit=garbage,
            subprocess_timeout=10,
        )
        combined = result.stderr + result.stdout
        assert not re.search(r"(?i)hit the time limit", combined), (
            f"Garbage env-var value {garbage!r} unexpectedly triggered a "
            f"time limit:\n"
            f"  stdout: {result.stdout[:400]}\n"
            f"  stderr: {result.stderr[:400]}"
        )
        assert result.stdout.strip(), (
            f"No stdout from cheap query with env={garbage!r}; stderr was:\n"
            f"{result.stderr[:400]}"
        )

    def test_sub_second_value_kills_pathological_query(self, decidb_cli_gurobi):
        """DECIDB_TIME_LIMIT=0.5 must actually fire on a hard MIQP.

        Catches an off-by-orders-of-magnitude parsing bug (e.g. someone
        accidentally treating the value as minutes, or as an integer cast
        that truncates 0.5 to 0). The pathological MIQP cannot complete
        within 0.5s, so observing the friendly message proves the value
        was plumbed through faithfully.
        """
        result = _run_decidb(
            decidb_cli_gurobi,
            _PATHOLOGICAL_MIQP_SQL,
            time_limit=0.5,
            subprocess_timeout=10,
        )
        combined = result.stderr + result.stdout
        assert re.search(r"(?i)hit the time limit", combined), (
            f"DECIDB_TIME_LIMIT=0.5 did not fire on pathological MIQP:\n"
            f"  stdout: {result.stdout[:400]}\n"
            f"  stderr: {result.stderr[:400]}"
        )
