"""F4 — the `diagnose_decide` consent gate (manual-first).

F4 adds a sticky session setting `diagnose_decide` with modes
none(default)/infeasible/unbounded/slow/auto and *filter* semantics: a mode
produces a diagnosis only when the solve actually lands in that state; otherwise
the query behaves exactly as before F4. This session only the unbounded engine
exists, so a fired diagnosis throws the short pointer error
("Diagnosis ready: SELECT * FROM decide_diagnostics()"); matched-but-unimplemented
states (infeasible/slow) fall through to the static F1 error.

These tests run under both backends. They use `execute_raw` (one `-c` statement)
because every assertion here is observable from the failing statement itself —
the cross-statement diagnostics relation is covered in test_query_diagnostics_f5.
"""

import pytest


_BACKENDS = ["decidb_cli_highs", "decidb_cli_gurobi"]

# Cleanly UNBOUNDED LP on both backends: a free REAL var maximized with no upper cap.
_UNBOUNDED_SQL = (
    "SELECT id, x FROM (VALUES (1), (2)) t(id) "
    "DECIDE x IS REAL SUCH THAT x >= 0 MAXIMIZE SUM(x)"
)

# Infeasible: SUM of two booleans cannot reach 3.
_INFEASIBLE_SQL = (
    "SELECT id, x FROM (VALUES (1), (2)) t(id) "
    "DECIDE x IS BOOLEAN SUCH THAT SUM(x) >= 3 MAXIMIZE SUM(x)"
)

_POINTER = "diagnosis ready (this session): select * from decide_diagnostics()"


def _combined(result) -> str:
    return (result.stderr + result.stdout).lower()


@pytest.mark.query_diagnostics
class TestF4DiagnosePragma:
    @pytest.mark.error
    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_default_none_reproduces_static_error(self, request, cli_fixture):
        """No pragma set => unchanged F1 behavior: static error, no pointer."""
        cli = request.getfixturevalue(cli_fixture)
        out = _combined(cli.execute_raw(_UNBOUNDED_SQL))
        assert "decide optimization is unbounded" in out
        assert _POINTER not in out
        assert "decide_diagnostics" not in out

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    @pytest.mark.parametrize("mode", ["none", "infeasible", "unbounded", "slow", "auto"])
    def test_valid_modes_are_accepted(self, request, cli_fixture, mode):
        """Every valid mode sets cleanly (no error) and the session keeps running."""
        cli = request.getfixturevalue(cli_fixture)
        out = _combined(
            cli.execute_raw(f"PRAGMA diagnose_decide='{mode}'; SELECT 1 AS ok;")
        )
        assert "invalid diagnose_decide" not in out
        assert "ok" in out  # the trailing SELECT ran

    @pytest.mark.error
    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_invalid_mode_is_rejected(self, request, cli_fixture):
        """A typo'd mode fails fast at SET time (set-callback validation)."""
        cli = request.getfixturevalue(cli_fixture)
        out = _combined(cli.execute_raw("PRAGMA diagnose_decide='bogus';"))
        assert "invalid diagnose_decide mode" in out

    @pytest.mark.error
    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    @pytest.mark.parametrize("mode", ["unbounded", "auto"])
    def test_unbounded_and_auto_fire_on_unbounded(self, request, cli_fixture, mode):
        """unbounded / auto both route an unbounded solve into diagnosis."""
        cli = request.getfixturevalue(cli_fixture)
        out = _combined(
            cli.execute_raw(f"PRAGMA diagnose_decide='{mode}'; {_UNBOUNDED_SQL}")
        )
        assert _POINTER in out

    @pytest.mark.error
    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_infeasible_mode_does_not_fire_on_unbounded(self, request, cli_fixture):
        """Filter scoping: mode targets infeasible, solve is unbounded => static error."""
        cli = request.getfixturevalue(cli_fixture)
        out = _combined(
            cli.execute_raw(f"PRAGMA diagnose_decide='infeasible'; {_UNBOUNDED_SQL}")
        )
        assert "decide optimization is unbounded" in out
        assert _POINTER not in out

    @pytest.mark.error
    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_unbounded_mode_does_not_fire_on_infeasible(self, request, cli_fixture):
        """Filter scoping the other way: unbounded mode ignores an infeasible solve.

        (Infeasible has no engine yet, so even a matching mode would fall through;
        here the mode does not even match, so the static infeasible error stands.)
        """
        cli = request.getfixturevalue(cli_fixture)
        out = _combined(
            cli.execute_raw(f"PRAGMA diagnose_decide='unbounded'; {_INFEASIBLE_SQL}")
        )
        assert "decide optimization is infeasible" in out
        assert _POINTER not in out
