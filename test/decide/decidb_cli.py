"""CLI wrapper for invoking the DecidB executable via subprocess.

Instead of using the Python ``decidb`` package (which forces single-threaded
execution), this module shells out to the native ``build/release/decidb``
binary so that DECIDE queries can leverage all available cores.

Error detection relies on stderr: the DecidB CLI writes error messages to
stderr (exit code is always 0).  Successful JSON results go to stdout,
possibly preceded by solver-license preamble lines (e.g. Gurobi).
"""

from __future__ import annotations

import json
import os
import re
import subprocess
from pathlib import Path


class DecidBCliError(Exception):
    """Raised when the DecidB CLI reports an error via stderr."""

    def __init__(self, message: str) -> None:
        self.message = message
        super().__init__(f"DecidB CLI error: {message}")


class DecidBCli:
    """Stateless wrapper around the DecidB CLI executable.

    Parameters
    ----------
    exe_path : str
        Absolute path to the ``decidb`` binary.
    db_path : str
        Absolute path to the TPC-H database file.
    env : dict[str, str] | None
        Extra environment variables overlaid on ``os.environ`` for every
        subprocess invocation. Used by fixtures (e.g. ``decidb_cli_highs``)
        that pin ``DECIDB_FORCE_SOLVER``. Pass ``None`` to inherit the
        parent environment unchanged.
    """

    def __init__(
        self, exe_path: str, db_path: str, env: dict[str, str] | None = None
    ) -> None:
        self.exe = exe_path
        self.db = db_path
        self.env = env

    def _subprocess_env(self) -> dict[str, str] | None:
        if not self.env:
            return None
        return {**os.environ, **self.env}

    def execute(
        self, sql: str, *, timeout: float = 120
    ) -> tuple[list[tuple], list[str]]:
        """Run a SQL query and return ``(rows, column_names)``.

        Rows are returned as tuples with native Python types (int, float, str)
        parsed from the CLI's JSON output.

        Raises
        ------
        DecidBCliError
            If the CLI writes anything to stderr (error messages).
        """
        proc = subprocess.run(
            [self.exe, self.db, "-readonly", "-json", "-c", sql],
            capture_output=True,
            text=True,
            timeout=timeout,
            env=self._subprocess_env(),
        )

        stderr = proc.stderr.strip()
        # Filter known solver warnings (not errors)
        if stderr:
            error_lines = [
                line for line in stderr.splitlines()
                if not line.startswith("Warning:")
            ]
            if error_lines:
                raise DecidBCliError("\n".join(error_lines))

        stdout = proc.stdout
        # Find the JSON array start — skip any solver preamble on stdout
        bracket = stdout.find("[")
        if bracket == -1:
            return [], []

        try:
            rows_dicts: list[dict] = json.loads(stdout[bracket:])
        except json.JSONDecodeError:
            return [], []

        if not rows_dicts:
            return [], []

        cols = list(rows_dicts[0].keys())
        rows = [tuple(d[c] for c in cols) for d in rows_dicts]
        return rows, cols

    def execute_raw(
        self, sql: str, *, timeout: float = 120
    ) -> subprocess.CompletedProcess:
        """Run a SQL query and return the raw ``CompletedProcess``."""
        return subprocess.run(
            [self.exe, self.db, "-readonly", "-c", sql],
            capture_output=True,
            text=True,
            timeout=timeout,
            env=self._subprocess_env(),
        )

    def execute_script(
        self, sql: str, *, timeout: float = 120
    ) -> subprocess.CompletedProcess:
        """Run a multi-statement script via stdin on a single connection.

        Unlike ``-c`` (which halts the remaining statements once one errors),
        stdin mode continues after an error. This is required for the
        diagnostics flow: a failing DECIDE throws a pointer error, then a
        follow-up ``SELECT * FROM decide_diagnostics()`` reads the diagnosis
        that was stashed per-connection before the throw. Both statements must
        run on the same connection.
        """
        return subprocess.run(
            [self.exe, self.db, "-readonly"],
            input=sql,
            capture_output=True,
            text=True,
            timeout=timeout,
            env=self._subprocess_env(),
        )

    def assert_error(
        self, sql: str, *, match: str | None = None, timeout: float = 120
    ) -> None:
        """Assert that *sql* produces an error on stderr.

        Both stdout and stderr are searched for the *match* pattern so that
        the test works regardless of where the CLI prints diagnostics.
        """
        result = self.execute_raw(sql, timeout=timeout)
        stderr = result.stderr.strip()
        assert stderr, (
            f"Expected error but stderr was empty.\n"
            f"stdout: {result.stdout[:500]}"
        )
        if match:
            combined = result.stderr + result.stdout
            assert re.search(match, combined), (
                f"Error output did not match '{match}'.\n"
                f"stdout: {result.stdout[:500]}\n"
                f"stderr: {result.stderr[:500]}"
            )
