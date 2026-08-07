"""The `diagnose_decide` setting: auto by default, off to suppress.

`diagnose_decide` is a sticky session setting with two modes: `auto` (the default)
and `off`. Under `auto` a failed solve is diagnosed automatically wherever an engine
exists; `off` suppresses diagnosis and reproduces the plain static solver error.
Today the unbounded and infeasible engines exist, so a fired diagnosis throws the
short pointer error ("Details: SELECT * FROM decide_diagnostics()"). The slow/time-limit
terminal prints a checkpoint report under `auto` (see test_query_diagnostics_slow) and
then still surfaces the static solver error; `off` reproduces the plain error with no
report.

These tests run under both backends. They use `execute_raw` (one `-c` statement)
because every assertion here is observable from the failing statement itself —
the cross-statement diagnostics relation is covered in
test_query_diagnostics_relation.
"""

import pytest


_BACKENDS = ["decidb_cli_highs", "decidb_cli_gurobi"]

# Cleanly UNBOUNDED LP on both backends: a free REAL var maximized with no upper cap.
_UNBOUNDED_SQL = (
    "SELECT id, x FROM (VALUES (1), (2)) t(id) "
    "DECIDE x(REAL) SUCH THAT x >= 0 MAXIMIZE SUM(x)"
)

# Infeasible: SUM of two booleans cannot reach 3.
_INFEASIBLE_SQL = (
    "SELECT id, x FROM (VALUES (1), (2)) t(id) "
    "DECIDE x(BOOL) SUCH THAT SUM(x) >= 3 MAXIMIZE SUM(x)"
)

_POINTER = "details: select * from decide_diagnostics()"


def _combined(result) -> str:
    return (result.stderr + result.stdout).lower()


@pytest.mark.query_diagnostics
class TestDiagnoseDecidePragma:
    @pytest.mark.error
    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_default_auto_fires_diagnosis(self, request, cli_fixture):
        """No pragma set => auto (the default): an unbounded solve is diagnosed and
        throws the short pointer error, not the plain static error."""
        cli = request.getfixturevalue(cli_fixture)
        out = _combined(cli.execute_raw(_UNBOUNDED_SQL))
        assert _POINTER in out

    @pytest.mark.error
    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_off_suppresses_diagnosis(self, request, cli_fixture):
        """`off` reproduces the plain static solver error: no pointer, no relation."""
        cli = request.getfixturevalue(cli_fixture)
        out = _combined(
            cli.execute_raw(f"PRAGMA diagnose_decide='off'; {_UNBOUNDED_SQL}")
        )
        assert "decide optimization is unbounded" in out
        assert _POINTER not in out
        assert "decide_diagnostics" not in out

    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    @pytest.mark.parametrize("mode", ["off", "auto"])
    def test_valid_modes_are_accepted(self, request, cli_fixture, mode):
        """Both valid modes set cleanly (no error) and the session keeps running."""
        cli = request.getfixturevalue(cli_fixture)
        out = _combined(
            cli.execute_raw(f"PRAGMA diagnose_decide='{mode}'; SELECT 1 AS ok;")
        )
        assert "invalid diagnose_decide" not in out
        assert "ok" in out  # the trailing SELECT ran

    @pytest.mark.error
    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    @pytest.mark.parametrize(
        "mode", ["bogus", "none", "unbounded", "infeasible", "slow"]
    )
    def test_invalid_mode_is_rejected(self, request, cli_fixture, mode):
        """A mode outside {off, auto} — including the removed per-state filters —
        fails fast at SET time (set-callback validation)."""
        cli = request.getfixturevalue(cli_fixture)
        out = _combined(cli.execute_raw(f"PRAGMA diagnose_decide='{mode}';"))
        assert "invalid diagnose_decide mode" in out

    @pytest.mark.error
    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_set_form_and_reset_restore_default_auto(self, request, cli_fixture):
        """The extension option also works through SET, and RESET returns to auto."""
        cli = request.getfixturevalue(cli_fixture)

        off = _combined(
            cli.execute_raw(f"SET diagnose_decide='off'; {_UNBOUNDED_SQL}")
        )
        assert "decide optimization is unbounded" in off
        assert _POINTER not in off

        reset = _combined(
            cli.execute_raw(
                f"SET diagnose_decide='off'; RESET diagnose_decide; {_UNBOUNDED_SQL}"
            )
        )
        assert _POINTER in reset

    @pytest.mark.error
    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_auto_fires_on_unbounded(self, request, cli_fixture):
        """Explicit `auto` routes an unbounded solve into diagnosis."""
        cli = request.getfixturevalue(cli_fixture)
        out = _combined(
            cli.execute_raw(f"PRAGMA diagnose_decide='auto'; {_UNBOUNDED_SQL}")
        )
        assert _POINTER in out

    @pytest.mark.error
    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_auto_fires_on_infeasible(self, request, cli_fixture):
        """`auto` now routes an infeasible solve into the elastic engine (I1): the
        error carries the diagnosis pointer and the least-change fix (here, loosening
        the unreachable SUM target). Edit details are asserted in the relation suite."""
        cli = request.getfixturevalue(cli_fixture)
        out = _combined(
            cli.execute_raw(f"PRAGMA diagnose_decide='auto'; {_INFEASIBLE_SQL}")
        )
        assert "decide optimization is infeasible" in out
        assert _POINTER in out

    @pytest.mark.error
    @pytest.mark.parametrize("cli_fixture", _BACKENDS)
    def test_off_suppresses_infeasible_diagnosis(self, request, cli_fixture):
        """`off` reproduces the plain static infeasible error — no diagnosis pointer."""
        cli = request.getfixturevalue(cli_fixture)
        out = _combined(
            cli.execute_raw(f"PRAGMA diagnose_decide='off'; {_INFEASIBLE_SQL}")
        )
        assert "decide optimization is infeasible" in out
        assert _POINTER not in out
