"""Time-limit error path test.

Triggers a deliberately pathological MIQP under a 1s Gurobi TimeLimit and
asserts that DecidB surfaces a "exceeded time limit" message rather than
the generic Gurobi-status fallback.

Regression test for a bug in src/decidb/gurobi/gurobi_loader.hpp where the
hand-mirrored Gurobi status constants drifted from upstream values:
GRB_TIME_LIMIT was set to 7 (actually GRB_ITERATION_LIMIT) and
GRB_ITERATION_LIMIT to 8 (actually GRB_NODE_LIMIT). The friendly
TIME_LIMIT branch in gurobi_solver.cpp was unreachable as a result.
"""

from __future__ import annotations

import os
import re
import subprocess

import pytest


@pytest.mark.error
def test_time_limit_surfaces_friendly_error(
    decidb_exe_path, decidb_db_path, _raw_oracle_solver, tmp_path
):
    if _raw_oracle_solver is None:
        pytest.skip("Gurobi not available — time-limit test requires it")

    # Gurobi auto-loads gurobi.env from the working directory at GRBenv
    # creation. Drop one in tmp_path and run decidb there so the limit is
    # enforced without needing a SQL-level Gurobi parameter knob (DecidB
    # does not currently expose one).
    (tmp_path / "gurobi.env").write_text("TimeLimit 1\n")

    # Symmetric MIQP: ~105 INTEGER vars (all interchangeable), equality
    # SUM = 30, quadratic objective. The optimal-set cardinality is roughly
    # C(105, 30) ~ 10^25 — Gurobi cannot prune the symmetric tree within 1s
    # on any reasonable hardware, guaranteeing status 9 (TIME_LIMIT).
    sql = """
        SELECT l_orderkey, l_linenumber, qty
        FROM lineitem
        WHERE l_orderkey < 100
        DECIDE qty IS INTEGER
        SUCH THAT qty <= 10 AND SUM(qty) = 30
        MINIMIZE SUM(POWER(qty - 2, 2))
    """

    result = subprocess.run(
        [decidb_exe_path, decidb_db_path, "-readonly", "-c", sql],
        capture_output=True,
        text=True,
        timeout=30,
        cwd=str(tmp_path),
        env={**os.environ, "DECIDB_FORCE_SOLVER": "gurobi"},
    )

    combined = result.stderr + result.stdout
    assert re.search(r"(?i)exceeded time limit", combined), (
        f"Expected 'exceeded time limit' message but got:\n"
        f"  stdout: {result.stdout[:400]}\n"
        f"  stderr: {result.stderr[:400]}"
    )
