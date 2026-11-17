"""A session whose CLI invalidates its database fails the call at once, with the CLI's text, instead of waiting out
the call's timeout for an end marker that never prints."""

import sys
import time

import pytest

import decidb_cli

#: Stands in for the CLI after a query raised an INTERNAL error: it reads the wrapper's commands, prints that
#: error and DuckDB's invalidated-database message on stderr, and then answers nothing, as the real shell does
#: once its database is invalid.
STUB = r"""
import sys, time
sys.stdin.readline()
sys.stderr.write("INTERNAL Error: stub failure\n")
sys.stderr.write("Fatal Error: Failed: database has been invalidated because of a previous fatal error. "
                 "The database must be restarted prior to being used again.\n")
sys.stderr.flush()
time.sleep(60)
"""


def test_invalidated_database_fails_the_call_at_once(tmp_path, monkeypatch):
    stub = tmp_path / "stub_cli.py"
    stub.write_text(STUB)
    monkeypatch.setattr(decidb_cli, "_SESSIONS", {})
    # The test calls `_run_in_session`, which starts no session with reuse off (the coverage run sets it to 0).
    monkeypatch.delenv(decidb_cli._REUSE_ENV, raising=False)
    started = time.monotonic()
    with pytest.raises(RuntimeError, match="INTERNAL Error: stub failure") as raised:
        decidb_cli._run_in_session([sys.executable, str(stub)], "SELECT 1", None, mode="quote", timeout=6)
    assert time.monotonic() - started < 5
    assert "database has been invalidated" in str(raised.value)
    assert not any(session.alive() for session in decidb_cli._SESSIONS.values())
