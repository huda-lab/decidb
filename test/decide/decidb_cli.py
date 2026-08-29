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


#: Prefix of the machine-readable terminal marker DeciDB emits on stderr when
#: ``DECIDB_STATUS_MARKERS`` is set (see the SUBOPTIMAL arm of ``physical_decide.cpp``).
#: The marker exists so tests never have to match user-facing prose, which is reworded
#: freely for voice and has silently broken a suite before.
_STATUS_MARKER_PREFIX = "DECIDB_STATUS:"


def _extract_status(stderr: str) -> str | None:
    """Return the terminal named by a DECIDB_STATUS marker line, if stderr has one."""
    for line in stderr.splitlines():
        if line.startswith(_STATUS_MARKER_PREFIX):
            return line[len(_STATUS_MARKER_PREFIX):].strip()
    return None


#: Environment variable that turns DuckDB's plan round-trip self-check on for every
#: query this wrapper runs. See ``_verify_serializer_args``.
_VERIFY_SERIALIZER_ENV = "DECIDB_VERIFY_SERIALIZER"

_MODEL_DUMP_BEGIN = "=== DECIDB MODEL DUMP ==="
_MODEL_DUMP_END = "=== END MODEL DUMP ==="

def _verify_serializer_args() -> list[str]:
    """CLI arguments that enable ``PRAGMA verify_serializer`` without output.

    With the pragma on, DuckDB serializes each bound plan, deserializes it, and runs
    the *copy* — so a plan field that never reaches the wire shows up as a wrong
    answer in whatever test happens to depend on it. That is the only guard here that
    does not have to be maintained per field: it costs nothing to keep correct,
    because the existing suite is the assertion.

    Delivered as startup commands rather than by prefixing the SQL, so no caller has
    to cope with an extra result set. A temporary output redirect swallows the pragma's
    own output, which would otherwise land in front of every ``execute_raw`` /
    ``execute_script`` result. PTY-driven interactive continuation is the deliberate
    exception; DuckDB's materializing verifier cannot pause to read terminal input.
    """
    return [
        "-cmd", ".output /dev/null",
        "-cmd", "PRAGMA verify_serializer;",
        "-cmd", ".output stdout",
    ]


def _last_complete_model_dump(text: str) -> str:
    """Return one model when DuckDB verification builds the query repeatedly.

    Verification paths may append complete, equivalent models to
    ``DECIDB_DUMP_MODEL``. Model-shape tests inspect one build, not the verifier's
    copies.
    """
    end = text.rfind(_MODEL_DUMP_END)
    if end == -1:
        return text
    begin = text.rfind(_MODEL_DUMP_BEGIN, 0, end)
    if begin == -1:
        return text
    end += len(_MODEL_DUMP_END)
    return text[begin:end] + ("\n" if text.endswith("\n") else "")


class DecidBCliError(Exception):
    """Raised when the DecidB CLI reports an error via stderr.

    ``status`` carries the machine-readable terminal from a ``DECIDB_STATUS:`` marker
    line when the CLI emitted one (currently only ``"SUBOPTIMAL"``), else ``None``.
    Tests branch on it instead of grepping the user-facing message, whose wording is
    deliberately owned by the user-facing-output principle and changes over time.
    """

    def __init__(self, message: str, status: str | None = None) -> None:
        self.message = message
        self.status = status
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
        self,
        exe_path: str,
        db_path: str,
        env: dict[str, str] | None = None,
        verify_serializer: bool | None = None,
    ) -> None:
        self.exe = exe_path
        self.db = db_path
        self.env = env
        #: ``None`` means "follow the environment"; a bool pins it for this wrapper.
        self.verify_serializer = (
            os.environ.get(_VERIFY_SERIALIZER_ENV) == "1"
            if verify_serializer is None
            else verify_serializer
        )

    def with_env(self, extra: dict[str, str]) -> "DecidBCli":
        """A copy of this wrapper with ``extra`` overlaid on its env.

        Lets a test add a one-off variable (e.g. ``DECIDB_DUMP_MODEL``) to a
        session-scoped fixture without mutating the fixture other tests share.
        """
        return DecidBCli(self.exe, self.db, env={**(self.env or {}), **extra},
                         verify_serializer=self.verify_serializer)

    def with_verify_serializer(self) -> "DecidBCli":
        """A copy of this wrapper that round-trips every bound plan through
        serialization, whatever the environment says.

        Used by tests asserting that a plan field survives the round trip, so they
        hold with the suite-wide switch off.
        """
        return DecidBCli(self.exe, self.db, env=self.env, verify_serializer=True)

    def _argv(self, *args: str) -> list[str]:
        """The CLI invocation, with the round-trip self-check attached when asked for."""
        argv = [self.exe, self.db, "-readonly"]
        if self.verify_serializer:
            argv += _verify_serializer_args()
        return argv + list(args)

    def _subprocess_env(self, *, status_markers: bool = False) -> dict[str, str] | None:
        # `status_markers` asks DeciDB for machine-readable terminal lines. Only
        # ``execute`` sets it: that method classifies stderr itself and strips the
        # markers first, whereas ``execute_raw`` / ``execute_script`` /
        # ``execute_interactive`` hand raw stderr to callers who do their own
        # classification and would read a marker line as an error.
        overlay = dict(self.env or {})
        if status_markers:
            overlay.setdefault("DECIDB_STATUS_MARKERS", "1")
        if not overlay:
            return None
        return {**os.environ, **overlay}

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
            self._argv("-json", "-c", sql),
            capture_output=True,
            text=True,
            timeout=timeout,
            env=self._subprocess_env(status_markers=True),
        )

        stderr = proc.stderr.strip()
        # Filter known solver warnings (not errors), and lift out any DECIDB_STATUS
        # marker so the terminal is available structurally instead of by grepping the
        # user-facing message.
        if stderr:
            status = _extract_status(stderr)
            error_lines = [
                line for line in stderr.splitlines()
                if not line.startswith("Warning:")
                and not line.startswith(_STATUS_MARKER_PREFIX)
            ]
            if error_lines:
                raise DecidBCliError("\n".join(error_lines), status=status)

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

    def dump_model(self, sql: str, path, *, timeout: float = 120) -> str:
        """Run a query with ``DECIDB_DUMP_MODEL`` on and return the dumped model text.

        The solver-neutral dump is the only place where presolve decisions —
        column bounds, row coefficients — are observable from a test. Two queries
        can share an optimum and still build different models, so anything about
        model *shape* has to be asserted here rather than through result rows.
        """
        env = {**os.environ, **(self.env or {}), "DECIDB_DUMP_MODEL": str(path)}
        proc = subprocess.run(
            self._argv("-c", sql),
            capture_output=True,
            text=True,
            timeout=timeout,
            env=env,
        )
        errors = [
            line for line in proc.stderr.splitlines()
            if line and not line.startswith("Warning:")
        ]
        if errors:
            raise DecidBCliError("\n".join(errors))
        if not Path(path).exists():
            raise AssertionError(f"query emitted no model:\n{sql}")
        return _last_complete_model_dump(Path(path).read_text())

    def execute_raw(
        self, sql: str, *, timeout: float = 120
    ) -> subprocess.CompletedProcess:
        """Run a SQL query and return the raw ``CompletedProcess``."""
        return subprocess.run(
            self._argv("-c", sql),
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
        stdin mode continues after an error. Diagnostics no longer require that
        recovery path — ``DIAGNOSE <query>`` returns its relation in one statement —
        but multi-statement setup, tuning pragmas, and lifecycle tests still need one
        connection.
        """
        return subprocess.run(
            self._argv(),
            input=sql,
            capture_output=True,
            text=True,
            timeout=timeout,
            env=self._subprocess_env(),
        )

    def execute_interactive(
        self, sql: str, responses: str, *, timeout: float = 60
    ) -> subprocess.CompletedProcess:
        """Run *sql* with a pseudo-terminal as stdin, feeding *responses*.

        Slow-solve continuation only prompts when stdin is a terminal (``isatty``);
        pytest's captured pipe stdin is not one. Driving the
        child through a PTY makes ``isatty`` true so the interactive continuation
        prompt engages. The SQL is delivered via ``-c`` (so the PTY is free to carry
        only the prompt answers), and *responses* — e.g. ``"s\\n"`` (stop), ``"\\n\\ns\\n"``
        (two continues then stop), or ``"\\x04"`` (EOF) — is written to the PTY master.

        Input is line-buffered by the terminal, so responses can be queued up front:
        each ``getline`` in the operator consumes one line as it reaches each prompt,
        independent of solver timing. Returns a ``CompletedProcess`` (stdout, stderr).
        """
        import pty
        import time

        master_fd, slave_fd = pty.openpty()
        try:
            # DuckDB's verification wrapper materializes a query result and is not
            # compatible with a physical operator pausing that execution to read from
            # the controlling terminal. Other non-interactive tests exercise the same
            # plan fields under the suite-wide serializer guard.
            argv = [self.exe, self.db, "-readonly"]
            proc = subprocess.Popen(
                argv + ["-c", sql],
                stdin=slave_fd,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                env=self._subprocess_env(),
            )
            os.close(slave_fd)  # parent keeps only the master end
            slave_fd = -1
            time.sleep(0.2)  # let the child come up before queueing the answers
            os.write(master_fd, responses.encode())
            stdout, stderr = proc.communicate(timeout=timeout)
        finally:
            os.close(master_fd)
            if slave_fd >= 0:
                os.close(slave_fd)
        return subprocess.CompletedProcess(proc.args, proc.returncode, stdout, stderr)

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
