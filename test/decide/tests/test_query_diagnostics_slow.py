"""Slow-solve checkpoint report (S1/S2) for query diagnostics.

A solve that reaches the time limit without a proven optimum returns
``SolverStatus::TIME_LIMIT``. Under ``auto`` the operator prints a plain-language
checkpoint block to stderr — what the solver already found and how far it can still
improve — and then (until the S3/S4 continuation work lands) still errors. Two
shapes, split on whether a feasible solution was found:

  * **Path 1 (solution found):** "usable solution (not proven best)" + best objective
    so far + closeness + elapsed/peak-memory.
  * **Path 2 (no solution yet):** "without finding a solution yet" + elapsed/peak-memory.

These tests pin the externally visible report on both backends. They set a short
``DECIDB_TIME_LIMIT`` and use two hard MILPs:

  * a 15-dimensional knapsack (x=0 always feasible, so an incumbent always exists,
    but optimality cannot be proven within ~1s) drives path 1;
  * a random market-split (feasibility-hard: the solver finds neither a feasible
    point nor a proof of infeasibility within the limit) drives path 2.

The report timing is inherently solver-dependent; the instances are sized with a
wide margin so the asserted branch is stable across machines.
"""

import random

import pytest

from decidb_cli import DecidBCli


_BACKENDS = ["decidb_cli_highs", "decidb_cli_gurobi"]


def _slow_cli(base_cli, seconds):
    """A CLI wrapper pinned to the base fixture's solver plus a short time limit."""
    env = dict(base_cli.env or {})
    env["DECIDB_TIME_LIMIT"] = str(seconds)
    return DecidBCli(base_cli.exe, base_cli.db, env=env)


def _knapsack_sql():
    """15-D knapsack: x=0 is feasible (an incumbent always exists), but proving the
    optimum over 15 tight capacity constraints on 400 binaries is not doable in ~1s.
    Coefficients are computed as columns in the inner SELECT (modulo in a DECIDE
    constraint position is unsupported), matching real column-coefficient usage."""
    M, N = 15, 400
    a = lambda k, i: ((i * (7 + 2 * k)) % 97) + 1
    caps = [sum(a(k, i) for i in range(1, N + 1)) // 2 for k in range(M)]
    ccols = ", ".join(f"((id*{7 + 2 * k})%97)+1 AS c{k}" for k in range(M))
    inner = f"SELECT id, {ccols}, ((id*13)%89)+1 AS p FROM range(1,{N + 1}) t(id)"
    cons = " AND ".join(f"SUM(c{k}*x) <= {caps[k]}" for k in range(M))
    return f"SELECT id, x FROM ({inner}) DECIDE x IS BOOLEAN SUCH THAT {cons} MAXIMIZE SUM(p*x)"


def _market_split_sql():
    """Random market-split (Cornuejols-Dawande style): m equalities
    ``SUM(a_k x) = floor(sum(a_k)/2)`` over n binaries. Feasibility-hard — the solver
    reaches the limit without finding a feasible point or proving infeasibility, so
    no incumbent exists (path 2). Seeded for determinism; embedded as a VALUES literal
    because the DB is opened read-only."""
    rng = random.Random(20260702)
    M, N = 6, 60
    A = [[rng.randint(0, 99) for _ in range(N)] for _ in range(M)]
    rhs = [sum(row) // 2 for row in A]
    rows = ", ".join(
        "(" + ",".join([str(j)] + [str(A[k][j]) for k in range(M)]) + ")"
        for j in range(N)
    )
    cols = "id," + ",".join(f"c{k}" for k in range(M))
    inner = f"SELECT * FROM (VALUES {rows}) t({cols})"
    cons = " AND ".join(f"SUM(c{k}*x) = {rhs[k]}" for k in range(M))
    return f"SELECT id, x FROM ({inner}) DECIDE x IS BOOLEAN SUCH THAT {cons}"


_KNAPSACK_SQL = _knapsack_sql()
_MARKET_SPLIT_SQL = _market_split_sql()

# Solver words the user-output rule forbids in the report. "gap" is intentionally
# excluded from the report even though the field is named gap internally.
_FORBIDDEN_JARGON = ["incumbent", "gap", "dual", "ray", "recession", "big-m"]


@pytest.mark.query_diagnostics
class TestSlowCheckpointReport:
    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_path1_incumbent_report(self, request, cli_fixture):
        """Solution-found timeout prints the usable-solution block with closeness."""
        cli = _slow_cli(request.getfixturevalue(cli_fixture), 1)
        result = cli.execute_raw(_KNAPSACK_SQL, timeout=30)
        combined = result.stderr + result.stdout
        low = combined.lower()

        assert "usable solution (not proven best)" in low, combined[:800]
        assert "best objective so far" in low, combined[:800]
        assert "of the best possible" in low, combined[:800]
        assert "elapsed" in low and "peak memory" in low, combined[:800]
        # S4 not built yet: the statement still surfaces the timeout error.
        assert "time limit" in low, combined[:800]
        for word in _FORBIDDEN_JARGON:
            assert word not in low, f"solver jargon {word!r} leaked:\n{combined[:800]}"

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_path2_no_solution_report(self, request, cli_fixture):
        """No-incumbent timeout prints the no-solution block, no objective line."""
        cli = _slow_cli(request.getfixturevalue(cli_fixture), 2)
        result = cli.execute_raw(_MARKET_SPLIT_SQL, timeout=30)
        combined = result.stderr + result.stdout
        low = combined.lower()

        assert "without finding a solution yet" in low, combined[:800]
        assert "elapsed" in low and "peak memory" in low, combined[:800]
        assert "time limit" in low, combined[:800]
        assert "best objective so far" not in low, combined[:800]
        for word in _FORBIDDEN_JARGON:
            assert word not in low, f"solver jargon {word!r} leaked:\n{combined[:800]}"

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_off_suppresses_report(self, request, cli_fixture):
        """`off` reproduces the plain static timeout error — no checkpoint block."""
        cli = _slow_cli(request.getfixturevalue(cli_fixture), 2)
        result = cli.execute_raw(
            "SET diagnose_decide='off'; " + _MARKET_SPLIT_SQL, timeout=30
        )
        combined = result.stderr + result.stdout
        low = combined.lower()

        assert "without finding a solution yet" not in low, combined[:800]
        assert "with a usable solution" not in low, combined[:800]
        # The plain static solver error still fires.
        assert "time limit" in low, combined[:800]
