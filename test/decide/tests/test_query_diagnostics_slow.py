"""Slow-solve checkpoint report + continuation (S1-S4).

A solve that reaches the time limit without a proven optimum returns
``SolverStatus::TIME_LIMIT``. This is ordinary execution behaviour, not a diagnosis:
it happens with or without the ``DIAGNOSE`` prefix, and no engine runs. The operator
prints a plain-language checkpoint block to stderr — what the solver already found and
how far it can still improve — in one of two shapes:

  * **Path 1 (solution found):** "usable solution (not proven best)" + best objective
    so far + closeness + elapsed/peak-memory.
  * **Path 2 (no solution yet):** "without finding a solution yet" + elapsed/peak-memory.

What happens after the report is fixed by who is there to answer, not by a setting:

  * **At a terminal:** prompt to keep solving on the warm solver (Enter continues a
    fresh chunk, ``s`` stops and takes the incumbent).
  * **Anywhere else** (tests, pipes, benchmarks, the C API): print the report, then
    error — never prompt where no one can answer.
  * **After Ctrl-C:** the user has already said stop, so there is nothing to ask.
    The incumbent is returned with the caveat that it is not proven best, terminal or
    not. With no incumbent, the timeout error fires.

These tests pin the externally visible behavior on both backends. They set a short
``DECIDB_TIME_LIMIT`` and use two hard MILPs:

  * a 15-dimensional knapsack (x=0 always feasible, so an incumbent always exists,
    but optimality cannot be proven within ~1s) drives path 1;
  * a random market-split (feasibility-hard: the solver finds neither a feasible
    point nor a proof of infeasibility within the limit) drives path 2.

The report timing is inherently solver-dependent; the instances are sized with a
wide margin so the asserted branch is stable across machines. The interactive prompt is
driven through a pseudo-terminal (``execute_interactive``) so ``isatty`` is true.
"""

import os
import random
import csv
import io
import signal
import subprocess
import time

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
    return f"SELECT id, x FROM ({inner}) DECIDE x(BOOL) SUCH THAT {cons} MAXIMIZE SUM(p*x)"


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
    return f"SELECT id, x FROM ({inner}) DECIDE x(BOOL) SUCH THAT {cons}"


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
        # Over piped (non-TTY) stdin there is no one to answer the offer, so the
        # statement still surfaces the timeout error after the report.
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


def _timeout_report(cli, sql):
    """Run a DECIDE that will time out and return its combined output.

    A timeout is execution behaviour now, so everything the user learns is in the
    checkpoint report the operator prints — there is no diagnosis relation to read.
    """
    return cli.execute_raw(sql, timeout=30)


def _large_lp_sql():
    """200k-var continuous LP with an objective: big enough that neither backend can
    finish within a ~1ms limit, and — being an LP — neither backend has a proven
    bound at the timeout. Guards against fabricated 'best possible' output (Gurobi's
    1e+100 ObjBound sentinel is finite; HiGHS's mip_dual_bound holds a 0 default)."""
    inner = "SELECT id, ((id%97)+1) AS w FROM range(1,200001) t(id)"
    return (
        f"SELECT id, x FROM ({inner}) DECIDE x(REAL) "
        "SUCH THAT x >= 0 AND x <= 1000 AND SUM(w*x) <= 100000000 MAXIMIZE SUM(x)"
    )


def _large_qp_sql():
    """Same scale with a convex quadratic objective (Gurobi-only test): the QP
    timeout path must be as bound-honest as the LP one."""
    inner = "SELECT id, ((id%97)+1) AS w FROM range(1,200001) t(id)"
    return (
        f"SELECT id, x FROM ({inner}) DECIDE x(REAL) "
        "SUCH THAT x >= 0 AND x <= 1000 AND SUM(w*x) >= 10000000 "
        "MINIMIZE SUM(POWER(x - w, 2))"
    )


_LARGE_LP_SQL = _large_lp_sql()
_LARGE_QP_SQL = _large_qp_sql()


@pytest.mark.query_diagnostics
class TestNonMipTimeoutHonesty:
    """A2/E2: on an LP/QP timeout no backend proves a bound, so the report and the
    report must claim nothing — no 'within …% of the best possible' line, and no solver
    infinity
    sentinel (1e+100) or nan leaking into user output. Whether an incumbent exists
    at an LP timeout differs per backend (HiGHS's simplex has a feasible point,
    Gurobi reports none), so only the bound/gap claims are asserted, not the path."""

    def _assert_no_fabricated_bound(self, result):
        combined = (result.stderr + result.stdout).lower()
        assert "time limit" in combined, combined[:800]
        assert "of the best possible" not in combined, combined[:800]
        assert "1e+100" not in combined and "nan" not in combined, combined[:800]

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_lp_timeout_no_fabricated_bound(self, request, cli_fixture):
        cli = _slow_cli(request.getfixturevalue(cli_fixture), 0.001)
        self._assert_no_fabricated_bound(_timeout_report(cli, _LARGE_LP_SQL))

    def test_qp_timeout_no_fabricated_bound_gurobi(self, decidb_cli_gurobi):
        cli = _slow_cli(decidb_cli_gurobi, 0.001)
        self._assert_no_fabricated_bound(_timeout_report(cli, _LARGE_QP_SQL))

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_mip_timeout_still_reports_bound(self, request, cli_fixture):
        """Regression guard: the MIP timeout keeps its real bound + closeness output."""
        cli = _slow_cli(request.getfixturevalue(cli_fixture), 1)
        combined = _timeout_report(cli, _KNAPSACK_SQL)
        low = (combined.stderr + combined.stdout).lower()
        assert "of the best possible" in low, low[:800]
        assert "best objective so far" in low, low[:800]


_STOP_CAVEAT = "best solution found so far"  # stderr caveat on a stop-with-incumbent
_PROMPT = "keep improving it"  # interactive continuation prompt (lowercased)


def _report_count(text: str) -> int:
    """How many checkpoint reports were printed (one per chunk boundary)."""
    return text.lower().count("time limit with a usable solution") + text.lower().count(
        "time limit without finding a solution"
    )


@pytest.mark.query_diagnostics
class TestSlowContinuation:
    """S3/S4: the prompt at a terminal, the plain error anywhere else, warm resume,
    and what a stop hands back."""

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_ask_stop_immediately(self, request, cli_fixture):
        """ask at a terminal: `s` stops and returns the incumbent rows + caveat."""
        cli = _slow_cli(request.getfixturevalue(cli_fixture), 1)
        result = cli.execute_interactive(_KNAPSACK_SQL, "s\n", timeout=40)
        err = result.stderr.lower()
        assert _report_count(result.stderr) == 1, result.stderr[:800]
        assert _PROMPT in err, result.stderr[:800]
        assert _STOP_CAVEAT in err, result.stderr[:800]
        # Success: incumbent rows are returned (not a timeout error).
        assert "time limit" not in result.stdout.lower()
        assert result.stdout.strip(), "expected incumbent rows on stdout"

    # NOTE: the multi-continue "two continues then stop, elapsed climbs 1s→2s→3s" case was
    # removed — driving three interactive prompts through the pty is inherently timing-racy
    # (flaky under load, independent of any solver change). The continuation loop is covered
    # deterministically by test_continue_interrupt_breaks and the mid-chunk interrupt test;
    # ask's stop semantics by test_ask_stop_immediately / test_ask_eof_stops.

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_ask_eof_stops(self, request, cli_fixture):
        """ask: EOF (Ctrl-D) on the prompt stops and returns the incumbent."""
        cli = _slow_cli(request.getfixturevalue(cli_fixture), 1)
        result = cli.execute_interactive(_KNAPSACK_SQL, "\x04", timeout=40)
        err = result.stderr.lower()
        assert _report_count(result.stderr) >= 1, result.stderr[:800]
        assert _STOP_CAVEAT in err, result.stderr[:800]
        assert result.stdout.strip(), "expected incumbent rows on stdout"

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_ask_non_tty_behaves_as_error(self, request, cli_fixture):
        """ask over non-terminal stdin never prompts: report, then timeout error."""
        cli = _slow_cli(request.getfixturevalue(cli_fixture), 1)
        # execute_raw pipes stdin (not a TTY); must return promptly (no hang on stdin).
        result = cli.execute_raw(_KNAPSACK_SQL, timeout=30)
        combined = (result.stderr + result.stdout).lower()
        assert "usable solution (not proven best)" in combined, combined[:800]
        assert _PROMPT not in combined, "must not prompt without a terminal"
        assert _STOP_CAVEAT not in combined, "must not deliver incumbent in ask/non-tty"
        assert "time limit" in combined, combined[:800]

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_non_tty_reports_then_errors(self, request, cli_fixture):
        """No terminal: print the report, then error — even when an incumbent exists.
        Nobody is there to say whether to keep going or take it."""
        cli = _slow_cli(request.getfixturevalue(cli_fixture), 1)
        result = cli.execute_raw(_KNAPSACK_SQL, timeout=30)
        combined = (result.stderr + result.stdout).lower()
        assert "usable solution (not proven best)" in combined, combined[:800]
        assert _STOP_CAVEAT not in combined, "a clock timeout never delivers unasked"
        assert "time limit" in combined, combined[:800]

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_interrupt_delivers_the_incumbent_without_a_terminal(
        self, request, cli_fixture
    ):
        """Ctrl-C is not a question. The user has already said stop, so the best-so-far
        comes back with its caveat — no prompt, no terminal needed, no error."""
        # Short chunk so the HiGHS boundary-only path (no thread-safe terminate) still
        # notices the interrupt promptly; Gurobi cuts the solve short mid-search.
        cli = _slow_cli(request.getfixturevalue(cli_fixture), 3)
        proc = subprocess.Popen(
            [cli.exe, cli.db, "-readonly", "-c", _KNAPSACK_SQL],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            env={**os.environ, **(cli.env or {})},
        )
        time.sleep(1.0)
        proc.send_signal(signal.SIGINT)
        out, err = proc.communicate(timeout=40)
        assert _STOP_CAVEAT in err.lower(), err[:1200]
        assert out.strip(), "expected incumbent rows on stdout after interrupt"
        assert proc.returncode == 0, f"expected success, got {proc.returncode}"

    def test_interrupt_is_mid_solve_gurobi(self, decidb_cli_gurobi):
        """Gurobi: Ctrl-C cuts the solve short MID-chunk, not at the next boundary. With a
        6s limit, an interrupt ~1.5s in returns almost immediately (GRBterminate) — far
        short of the ~4.5s a boundary-only stop would wait out. HiGHS is boundary-only (no
        mid-solve interrupt) and is intentionally not asserted."""
        cli = _slow_cli(decidb_cli_gurobi, 6)
        proc = subprocess.Popen(
            [cli.exe, cli.db, "-readonly", "-c", _KNAPSACK_SQL],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            env={**os.environ, **(cli.env or {})},
        )
        time.sleep(1.5)
        t = time.time()
        proc.send_signal(signal.SIGINT)
        out, err = proc.communicate(timeout=30)
        latency = time.time() - t
        assert proc.returncode == 0, err[:800]
        assert out.strip(), "expected incumbent rows after the mid-solve interrupt"
        assert _STOP_CAVEAT in err.lower(), err[:800]
        # Mid-solve terminate returns in well under a second; a boundary-only stop would
        # sit out the remaining ~4.5s of the chunk. 3s cleanly separates the two.
        assert latency < 3.0, f"interrupt took {latency:.2f}s — expected mid-solve stop (<3s)"


_STOP_REQUEST = "stopped at your request"  # interrupt report wording (vs "hit the … time limit")


def _interrupt_run(cli, script, delay, timeout=30):
    """Launch `script` via -c and send SIGINT after `delay` seconds; return
    (proc, out, err, latency) where latency is wall time from the signal to exit."""
    proc = subprocess.Popen(
        [cli.exe, cli.db, "-readonly", "-c", script],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        env={**os.environ, **(cli.env or {})},
    )
    time.sleep(delay)
    t = time.time()
    proc.send_signal(signal.SIGINT)
    try:
        out, err = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        out, err = proc.communicate()
    return proc, out, err, time.time() - t


@pytest.mark.query_diagnostics
class TestSlowFirstSolveInterrupt:
    """Ctrl-C is a peer trigger into the slow branch, decoupled from the time limit: a
    user interrupt on the first solve cuts it short and hands back the best-so-far —
    instead of aborting the query — even when the wall-clock limit is nowhere near. On Gurobi the stop is attributed to the user (`GRBterminate` →
    `GRB_INTERRUPTED`), so the report says "stopped at your request", not "hit the … time
    limit". HiGHS has no thread-safe terminate, so its first-solve interrupt is boundary-only
    (the documented solver-agnostic fallback) and is reported as a time-limit stop.

    A generous DECIDB_TIME_LIMIT (20s) keeps the first chunk boundary far away, so a fast
    stop after the ~1s signal proves the *first solve itself* was interrupted mid-solve —
    a boundary-only path would have to wait out the remaining ~19s."""

    def test_first_solve_interrupt_returns_incumbent_gurobi(self, decidb_cli_gurobi):
        """Gurobi, continue mode: SIGINT during the first solve returns the incumbent with
        the interrupt wording (not a timeout), fast (mid-solve, not at the far boundary)."""
        cli = _slow_cli(decidb_cli_gurobi, 20)
        proc, out, err, latency = _interrupt_run(
            cli, _KNAPSACK_SQL, delay=1.0
        )
        low = err.lower()
        assert proc.returncode == 0, err[:1000]
        assert out.strip(), "expected incumbent rows after a first-solve interrupt"
        assert _STOP_REQUEST in low, err[:1000]
        assert _STOP_CAVEAT in low, err[:1000]
        # Attributed to the user, not the clock — the whole point of the peer trigger.
        assert "time limit" not in low, err[:1000]
        # Mid-first-solve terminate returns fast; a boundary-only stop would sit out ~19s.
        assert latency < 5.0, f"interrupt took {latency:.2f}s — expected a mid-solve stop"

    def test_first_solve_interrupt_without_an_incumbent_errors_gurobi(
        self, decidb_cli_gurobi
    ):
        """Interrupting a solve that has found nothing yet has no best-so-far to hand
        back, so it reports what it knows and errors."""
        cli = _slow_cli(decidb_cli_gurobi, 20)
        proc, out, err, _ = _interrupt_run(cli, _MARKET_SPLIT_SQL, delay=1.0)
        low = err.lower()
        assert _STOP_REQUEST in low, err[:1000]
        assert _STOP_CAVEAT not in low, "nothing was found, so nothing is delivered"
        assert not out.strip(), "no incumbent means no rows"

    def test_first_solve_interrupt_highs_boundary_only(self, decidb_cli_highs):
        """HiGHS has no thread-safe terminate, so a mid-first-solve SIGINT is *not* seen
        until the chunk boundary: the stop is attributed to the time limit (not the user),
        yet the incumbent is still delivered at the boundary — the solver-agnostic fallback.
        Probed 2026-07-04: a 0.7s signal into a 2s chunk waits out the chunk, then returns
        rows with the "time limit" wording."""
        cli = _slow_cli(decidb_cli_highs, 2)
        proc, out, err, _ = _interrupt_run(
            cli, _KNAPSACK_SQL, delay=0.7
        )
        low = err.lower()
        assert proc.returncode == 0, err[:1000]
        assert out.strip(), "fallback still delivers the incumbent at the boundary"
        assert _STOP_CAVEAT in low, err[:1000]
        # HiGHS cannot attribute the stop to the user → reported as a time-limit stop.
        assert "time limit" in low, err[:1000]
        assert _STOP_REQUEST not in low, err[:1000]
