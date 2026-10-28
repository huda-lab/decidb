"""CLI wrapper for invoking the DecidB executable via subprocess.

Instead of using the Python ``decidb`` package (which forces single-threaded
execution), this module shells out to the native ``build/release/decidb``
binary so that DECIDE queries can leverage all available cores.

Error detection relies on stderr: the DecidB CLI writes error messages to
stderr (exit code is always 0).  Successful JSON results go to stdout,
possibly preceded by solver-license preamble lines (e.g. Gurobi).
"""

from __future__ import annotations

import atexit
import json
import os
import re
import selectors
import signal
import subprocess
import tempfile
import time
from pathlib import Path

from tests._output_helpers import assert_no_internal_leak


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

#: Environment variable that sets ``decide_direct_solve`` for every query this wrapper runs
#: (``off``, ``auto`` or ``require``). Direct solve is on by default, so
#: ``DECIDB_TEST_DIRECT_SOLVE=off`` is how the whole suite is run against the solver alone,
#: as ``DECIDB_FORCE_SOLVER`` runs it against one backend. A test that sets the mode itself wins.
_DIRECT_SOLVE_ENV = "DECIDB_TEST_DIRECT_SOLVE"

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


#: The phrase every removed-spelling error carries (RefuseRemovedDecideSpellings).
REMOVED_SPELLING_PHRASE = "is removed"


def assert_no_removed_spelling(result: subprocess.CompletedProcess, sql: str) -> None:
    """Fail the calling test when the query was refused for a removed DECIDE spelling: a
    loose error pattern would otherwise pass on that error by accident. Only the
    removed-syntax suite runs such queries, and it says so."""
    assert REMOVED_SPELLING_PHRASE not in result.stderr, (
        f"the query uses a removed DECIDE spelling (only test_scope_removed_syntax.py may):\n"
        f"{sql[:300]}\nstderr: {result.stderr[:500]}"
    )


def _check(result: subprocess.CompletedProcess, sql: str, removed_spelling: bool) -> subprocess.CompletedProcess:
    """Fail the calling test if the query showed the user an internal error or an internal
    tag: no query, accepted or refused, may do either. Unless ``removed_spelling`` says the
    test is about one, a removed-spelling error fails it too."""
    assert_no_internal_leak(result, f"the query {sql[:300]!r}", sql)
    if not removed_spelling:
        assert_no_removed_spelling(result, sql)
    return result


def _run(argv: list[str], sql: str, *, removed_spelling: bool = False, **kwargs) -> subprocess.CompletedProcess:
    """Run the CLI on ``sql`` in a process of its own (see ``_check``)."""
    return _check(subprocess.run(argv, capture_output=True, text=True, **kwargs), sql, removed_spelling)


#: Set to ``0`` to start a new process for every query, as the wrapper did before sessions.
_REUSE_ENV = "DECIDB_TEST_REUSE_PROCESS"

#: SQL that changes or reads the connection's state, so a later query in the same process
#: could see it: a setting, a variable, a catalog object, a transaction, the last query's
#: diagnosis. Such a query gets a process of its own. A false match only costs a process.
_SESSION_STATE = re.compile(
    r"\b(SET|RESET|PRAGMA|CREATE|ATTACH|DETACH|USE|LOAD|INSTALL|BEGIN|COMMIT|ROLLBACK|ABORT|CALL|EXPORT|IMPORT|COPY|"
    r"DROP|INSERT|UPDATE|DELETE|ALTER|CHECKPOINT|VACUUM|PREPARE|EXECUTE|DEALLOCATE|setvariable|decide_diagnostics)\b",
    re.IGNORECASE,
)

#: A leading ``SET decide_direct_solve='mode';``, how the direct-solve suites pick a path. It is the
#: one setting a session admits: every query of that session sets the same mode (the mode is in the
#: session's key), so none sees another's.
_DIRECT_MODE_PREFIX = re.compile(r"\A\s*SET\s+decide_direct_solve\s*=\s*'(\w+)'\s*;", re.IGNORECASE)

#: The header DuckDB's shell prints in front of every error (``Parser Error:``,
#: ``Invalid Input Error:``). Under ``-c`` the shell exits 1 after one; a session reports the same.
_ERROR_HEADER = re.compile(r"^[A-Za-z ]*Error:", re.MULTILINE)


class _Session:
    """One long-lived CLI process that runs queries one after another (a connection pool of size one).

    Starting the CLI costs ~20 ms, more than most queries in this suite; a test runs two or three
    queries, so starting one process per query was most of the suite's time. Each query is written
    to a file and run with ``.read``, which ends at the file's end, so an unterminated quote in a
    parser-error test cannot swallow what follows. After it, ``.print`` ends stdout and a query
    that raises ends stderr; the wrapper reads both pipes until it sees both markers.
    """

    def __init__(self, argv: list[str], env: dict[str, str]) -> None:
        self.proc = subprocess.Popen(
            argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env
        )
        fd, self.query_file = tempfile.mkstemp(suffix=".sql", prefix="decidb_session_")
        os.close(fd)
        self.count = 0

    def alive(self) -> bool:
        return self.proc.poll() is None

    def close(self) -> None:
        if self.alive():
            self.proc.kill()
            self.proc.wait()
        Path(self.query_file).unlink(missing_ok=True)

    def run(self, sql: str, *, json_mode: bool, timeout: float) -> subprocess.CompletedProcess:
        self.count += 1
        marker = f"__DECIDB_SESSION_END_{self.count}__"
        Path(self.query_file).write_text(sql)
        mode = "json" if json_mode else "duckbox"
        commands = f".mode {mode}\n.read {self.query_file}\n.print {marker}\nSELECT error('{marker}');\n"
        out, err = bytearray(), bytearray()
        end_out, end_err = marker.encode() + b"\n", f"Invalid Input Error: {marker}\n".encode()
        deadline = time.monotonic() + timeout
        try:
            self.proc.stdin.write(commands.encode())
            self.proc.stdin.flush()
            with selectors.DefaultSelector() as pipes:
                pipes.register(self.proc.stdout, selectors.EVENT_READ, out)
                pipes.register(self.proc.stderr, selectors.EVENT_READ, err)
                while not (out.endswith(end_out) and err.endswith(end_err)) and pipes.get_map():
                    left = deadline - time.monotonic()
                    if left <= 0:
                        self.close()
                        raise subprocess.TimeoutExpired(self.proc.args, timeout)
                    for ready, _ in pipes.select(left):
                        chunk = os.read(ready.fd, 1 << 16)
                        if not chunk:
                            pipes.unregister(ready.fileobj)  # the process died: report what it wrote
                        ready.data.extend(chunk)
        except BrokenPipeError:
            pass
        stdout = bytes(out).decode(errors="replace")
        stderr = bytes(err).decode(errors="replace")
        if self.alive():
            stdout, stderr = stdout[: -len(end_out)], stderr[: -len(end_err)]
            returncode = 1 if _ERROR_HEADER.search(stderr) else 0
        else:
            returncode = self.proc.returncode
        return subprocess.CompletedProcess(self.proc.args, returncode, stdout, stderr)


#: This process's sessions (one per pytest-xdist worker), keyed by the CLI arguments and the
#: environment they start with, since DeciDB reads its environment variables when it starts.
#: Least recently used first; past ``_MAX_SESSIONS`` the oldest is closed.
_SESSIONS: dict[tuple, _Session] = {}
_MAX_SESSIONS = 6

#: Variables pytest changes on every test, which DeciDB never reads: left out of a session's key.
_PER_TEST_ENV = ("PYTEST_CURRENT_TEST",)


def _close_sessions() -> None:
    for session in _SESSIONS.values():
        session.close()
    _SESSIONS.clear()


atexit.register(_close_sessions)


def _run_in_session(
    argv: list[str],
    sql: str,
    env: dict[str, str] | None,
    *,
    json_mode: bool,
    timeout: float,
    removed_spelling: bool = False,
) -> subprocess.CompletedProcess | None:
    """Run ``sql`` in this worker's long-lived process for ``argv`` and ``env``, or return None
    when it must run in a process of its own: it touches session state other than a leading
    direct-solve mode, it is a dot command, or reuse is switched off."""
    prefix = _DIRECT_MODE_PREFIX.match(sql)
    rest = sql[prefix.end() :] if prefix else sql
    if os.environ.get(_REUSE_ENV) == "0" or _SESSION_STATE.search(rest) or rest.lstrip().startswith("."):
        return None
    env = {name: value for name, value in (env if env is not None else os.environ).items() if name not in _PER_TEST_ENV}
    key = (tuple(argv), frozenset(env.items()), prefix and prefix.group(1).lower())
    session = _SESSIONS.pop(key, None)
    if session is None or not session.alive():
        if session is not None:
            session.close()
        session = _Session(argv, env)
    _SESSIONS[key] = session
    while len(_SESSIONS) > _MAX_SESSIONS:
        _SESSIONS.pop(next(iter(_SESSIONS))).close()
    result = session.run(sql, json_mode=json_mode, timeout=timeout)
    if not session.alive():
        session.close()
        del _SESSIONS[key]
    return _check(result, sql, removed_spelling)


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

    def without_verify_serializer(self) -> "DecidBCli":
        """A copy of this wrapper that never round-trips plans, whatever the environment says.

        For a script DuckDB's round trip cannot replay: it re-runs a ``CREATE TEMP MACRO``
        against the read-only database and fails there, with or without DECIDE.
        """
        return DecidBCli(self.exe, self.db, env=self.env, verify_serializer=False)

    def _argv(self, *args: str) -> list[str]:
        """The CLI invocation, with the round-trip self-check attached when asked for."""
        argv = [self.exe, self.db, "-readonly"]
        if self.verify_serializer:
            argv += _verify_serializer_args()
        direct_mode = (self.env or {}).get(_DIRECT_SOLVE_ENV, os.environ.get(_DIRECT_SOLVE_ENV))
        if direct_mode:
            argv += ["-cmd", f"SET decide_direct_solve='{direct_mode}';"]
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
        env = self._subprocess_env(status_markers=True)
        proc = _run_in_session(self._argv(), sql, env, json_mode=True, timeout=timeout) or _run(
            self._argv("-json", "-c", sql), sql, timeout=timeout, env=env
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
        Only the solver path builds a model, so the dump runs with direct solve off.
        It also runs on one thread: a window over a per-group bound hands its rows on in
        an order that varies with the thread schedule, and the dump follows the order of
        the rows the DECIDE operator reads. Row order means nothing to the model, so one
        thread makes two dumps of the same model compare equal (``golden/capture.sh``
        does the same).
        """
        sql = f"SET threads=1; {sql}"
        cli = self.with_env({_DIRECT_SOLVE_ENV: "off"})
        env = {**os.environ, **cli.env, "DECIDB_DUMP_MODEL": str(path)}
        proc = _run(cli._argv("-c", sql), sql, timeout=timeout, env=env)
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
        self, sql: str, *, timeout: float = 120, removed_spelling: bool = False
    ) -> subprocess.CompletedProcess:
        """Run a SQL query and return the raw ``CompletedProcess``. ``removed_spelling``
        says the query is expected to be refused for a removed DECIDE spelling."""
        env = self._subprocess_env()
        return _run_in_session(
            self._argv(), sql, env, json_mode=False, timeout=timeout, removed_spelling=removed_spelling
        ) or _run(self._argv("-c", sql), sql, removed_spelling=removed_spelling, timeout=timeout, env=env)

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
        return _run(self._argv(), sql, input=sql, timeout=timeout, env=self._subprocess_env())

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
        result = subprocess.CompletedProcess(proc.args, proc.returncode, stdout, stderr)
        assert_no_internal_leak(result, f"the query {sql[:300]!r}", sql)
        assert_no_removed_spelling(result, sql)
        return result

    def execute_interrupted(
        self, sql: str, *, after: float, timeout: float = 30
    ) -> tuple[subprocess.CompletedProcess, float]:
        """Run *sql*, send SIGINT *after* seconds, and return ``(result, latency)``, where
        latency is the wall time from the signal to the process exit.

        Like ``execute_interactive``, it skips the serializer self-check: that wrapper
        re-runs the query, so the interrupt would not land on the first solve.
        """
        proc = subprocess.Popen(
            [self.exe, self.db, "-readonly", "-c", sql],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=self._subprocess_env(),
        )
        time.sleep(after)
        signalled = time.time()
        proc.send_signal(signal.SIGINT)
        try:
            stdout, stderr = proc.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            proc.kill()
            stdout, stderr = proc.communicate()
        latency = time.time() - signalled
        result = subprocess.CompletedProcess(proc.args, proc.returncode, stdout, stderr)
        assert_no_internal_leak(result, f"the query {sql[:300]!r}", sql)
        assert_no_removed_spelling(result, sql)
        return result, latency

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
