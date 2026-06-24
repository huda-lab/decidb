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

import os
import re
import subprocess

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
    DECIDE qty IS INTEGER
    SUCH THAT qty <= 10 AND SUM(qty) = 30
    MINIMIZE SUM(POWER(qty - 2, 2))
"""

# Cheap query that any sane solver finishes in well under a second.
# Used to verify the env-var read path doesn't break normal workloads.
_FAST_QUERY_SQL = """
    SELECT s_suppkey, x FROM supplier
    DECIDE x IS BOOLEAN
    SUCH THAT SUM(x) = 5
    MAXIMIZE SUM(x * s_acctbal)
    LIMIT 10
"""


def _run_decidb(decidb_exe_path, decidb_db_path, sql, *, time_limit=None,
                subprocess_timeout=30):
    """Invoke the decidb CLI with optional DECIDB_TIME_LIMIT in the env.

    time_limit can be a number (seconds) or a string (e.g. ``"abc"``,
    ``"-5"``, ``""``) so invalid-value cases can pass arbitrary garbage.
    Returns the CompletedProcess.
    """
    env = {**os.environ, "DECIDB_FORCE_SOLVER": "gurobi"}
    if time_limit is not None:
        env["DECIDB_TIME_LIMIT"] = str(time_limit)
    return subprocess.run(
        [decidb_exe_path, decidb_db_path, "-readonly", "-c", sql],
        capture_output=True,
        text=True,
        timeout=subprocess_timeout,
        env=env,
    )


@pytest.mark.error
class TestTimeLimit:
    """DECIDB_TIME_LIMIT env-var contract."""

    def test_env_var_surfaces_friendly_error(
        self, decidb_exe_path, decidb_db_path, _raw_oracle_solver
    ):
        """Short DECIDB_TIME_LIMIT must surface the friendly TIME_LIMIT message.

        Primary regression test for the bug: hardcoded 300s used to clobber
        any user-supplied limit, so a 1s ceiling could not fire and the
        subprocess SIGKILL'd at 30s with no error text. This case verifies
        both that the env var is plumbed through to Gurobi AND that the
        GRB_TIME_LIMIT branch produces the documented user message.
        """
        if _raw_oracle_solver is None:
            pytest.skip("Gurobi not available — time-limit test requires it")

        result = _run_decidb(
            decidb_exe_path, decidb_db_path, _PATHOLOGICAL_MIQP_SQL,
            time_limit=1, subprocess_timeout=15,
        )
        combined = result.stderr + result.stdout
        assert re.search(r"(?i)hit the time limit", combined), (
            f"Expected 'hit the time limit' message but got:\n"
            f"  stdout: {result.stdout[:400]}\n"
            f"  stderr: {result.stderr[:400]}"
        )

    def test_default_does_not_break_normal_queries(
        self, decidb_exe_path, decidb_db_path, _raw_oracle_solver
    ):
        """No env var → 300s default → cheap query completes successfully.

        Guards against an accidental default change (e.g. someone lowering
        the constant to 1s) or a regression in the env-var read path that
        silently neutralizes the limit.
        """
        if _raw_oracle_solver is None:
            pytest.skip("Gurobi not available — time-limit test requires it")

        result = _run_decidb(
            decidb_exe_path, decidb_db_path, _FAST_QUERY_SQL,
            time_limit=None, subprocess_timeout=10,
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

    @pytest.mark.parametrize("garbage", ["abc", "-5", "0", ""])
    def test_invalid_env_var_falls_back_to_default(
        self, decidb_exe_path, decidb_db_path, _raw_oracle_solver, garbage
    ):
        """Garbage DECIDB_TIME_LIMIT values must silently fall back to default.

        ``std::stod`` throws on non-numeric input; negative/zero values are
        nonsensical as a time budget. The implementation must catch the
        exception and reject non-positive parsed values without leaking
        either failure mode to the user.
        """
        if _raw_oracle_solver is None:
            pytest.skip("Gurobi not available — time-limit test requires it")

        result = _run_decidb(
            decidb_exe_path, decidb_db_path, _FAST_QUERY_SQL,
            time_limit=garbage, subprocess_timeout=10,
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

    def test_sub_second_value_kills_pathological_query(
        self, decidb_exe_path, decidb_db_path, _raw_oracle_solver
    ):
        """DECIDB_TIME_LIMIT=0.5 must actually fire on a hard MIQP.

        Catches an off-by-orders-of-magnitude parsing bug (e.g. someone
        accidentally treating the value as minutes, or as an integer cast
        that truncates 0.5 to 0). The pathological MIQP cannot complete
        within 0.5s, so observing the friendly message proves the value
        was plumbed through faithfully.
        """
        if _raw_oracle_solver is None:
            pytest.skip("Gurobi not available — time-limit test requires it")

        result = _run_decidb(
            decidb_exe_path, decidb_db_path, _PATHOLOGICAL_MIQP_SQL,
            time_limit=0.5, subprocess_timeout=10,
        )
        combined = result.stderr + result.stdout
        assert re.search(r"(?i)hit the time limit", combined), (
            f"DECIDB_TIME_LIMIT=0.5 did not fire on pathological MIQP:\n"
            f"  stdout: {result.stdout[:400]}\n"
            f"  stderr: {result.stderr[:400]}"
        )
