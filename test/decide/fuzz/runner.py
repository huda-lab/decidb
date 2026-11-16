"""The shared runner: a generated case, its run on the CLI, the verdict and the report.

A ``Case`` holds a ``Reference``, which is the oracle's reading of the query, and one script per spelling of the query
(a stored table, inline VALUES, the rows in another order). Every spelling must answer as the reference does, under
each direct-solve mode.
"""

from __future__ import annotations

import importlib
import json
import os
import re
import subprocess
from collections import Counter
from concurrent.futures import ProcessPoolExecutor
from dataclasses import dataclass, field
from functools import partial
from pathlib import Path
from typing import Mapping, Sequence

from solver.factory import get_solver
from solver.types import ObjSense, SolverStatus
from tests._anr_oracle import AnrModel, Col, Const, Objective, ScopeError
from tests._output_helpers import assert_no_internal_leak

from . import known

MODULES = ("nests", "frames", "bounds", "order_keys")
DIRECT_MODES = ("off", "auto")

#: The header the shell prints in front of every error.
_ERROR_HEADER = re.compile(r"^[A-Za-z ]*Error:", re.MULTILINE)
_TIMEOUT = 60.0
_TOLERANCE = 1e-5


def decidb_exe() -> str:
    """The CLI under test, found as ``conftest.py`` finds it."""
    return os.environ.get("DECIDB_EXE_PATH") or str(
        Path(__file__).resolve().parents[3] / "build" / "release" / "decidb"
    )


# -- SQL text -----------------------------------------------------------------


@dataclass(frozen=True)
class Text:
    """An expression twice: as the query writes it and as the oracle reads it."""

    sql: str
    node: object

    def __add__(self, other: "Text") -> "Text":
        return Text(f"({self.sql} + {other.sql})", self.node + other.node)

    def __sub__(self, other: "Text") -> "Text":
        return Text(f"({self.sql} - {other.sql})", self.node - other.node)

    def __mul__(self, other: "Text") -> "Text":
        return Text(f"{self.sql} * {other.sql}", self.node * other.node)


def number(value) -> Text:
    return Text(str(value), Const(float(value)))


def column(name: str) -> Text:
    return Text(f"t.{name}", Col(name))


def literal(value) -> str:
    if value is None:
        return "NULL"
    if isinstance(value, bool):
        return "TRUE" if value else "FALSE"
    if isinstance(value, str):
        return "'" + value.replace("'", "''") + "'"
    return repr(value)


def stored(name: str, columns: Sequence[tuple[str, str]], rows: Sequence[Mapping]) -> str:
    """``CREATE TABLE`` and ``INSERT`` for ``rows``; ``columns`` are (name, type) pairs."""
    names = ", ".join(f"{column} {kind}" for column, kind in columns)
    values = ", ".join("(" + ", ".join(literal(row[column]) for column, _ in columns) + ")" for row in rows)
    return f"CREATE TABLE {name}({names});\nINSERT INTO {name} VALUES {values};\n"


def inline(alias: str, columns: Sequence[tuple[str, str]], rows: Sequence[Mapping]) -> str:
    """``rows`` as a VALUES source; every value is cast, so a column of NULLs keeps its type."""
    values = ", ".join(
        "(" + ", ".join(f"CAST({literal(row[column])} AS {kind})" for column, kind in columns) + ")" for row in rows
    )
    return f"(VALUES {values}) {alias}({', '.join(column for column, _ in columns)})"


# -- The reference -------------------------------------------------------------


@dataclass
class Expected:
    """What the query must do: answer at ``objective``, be infeasible, or raise ``phrase``."""

    status: str
    objective: float | None = None
    phrase: str = ""

    def __str__(self) -> str:
        if self.status == "optimal":
            return f"an answer with objective {self.objective:.6g}"
        return f"the error '{self.phrase}'" if self.status == "error" else self.status


@dataclass
class Reference:
    """The oracle's reading of a query. ``domains`` asks for every assignment to be tried, which needs no solver;
    without it the model is solved."""

    model: AnrModel
    clauses: list
    objective: Objective
    domains: Mapping | None = None

    def expected(self) -> Expected:
        try:
            if self.domains is None:
                solution = self.model.solve(get_solver(), self.clauses, self.objective)
            else:
                solution = self.model.brute_force(self.clauses, self.objective, self.domains)
        except ScopeError as error:
            return Expected("error", phrase=error.phrase)
        if solution.status == SolverStatus.OPTIMAL:
            return Expected("optimal", objective=solution.objective)
        assert solution.status == SolverStatus.INFEASIBLE, solution.status
        return Expected("infeasible")

    def fault_in(self, answer: list[dict], expected: Expected) -> str | None:
        """What is wrong with ``answer``, read against every generated instance and the optimum."""
        try:
            reached = self.model.judge(answer, self.clauses, self.objective)
        except AssertionError as error:
            return f"an answer that breaks the language: {error}"
        if abs(reached - expected.objective) > _TOLERANCE * max(1.0, abs(expected.objective)):
            worse = (reached < expected.objective) == (self.objective.sense == ObjSense.MAXIMIZE)
            return f"objective {reached:.6g}, {'worse' if worse else 'better'} than the optimum"
        return None


@dataclass
class Case:
    """One generated query: its reference, and a script per spelling, each complete from its first statement."""

    reference: Reference
    scripts: dict[str, str]
    #: Fixed phrases of the refusals the language makes of this case's shape. Any other error is a failure.
    refusals: tuple[str, ...] = ()


# -- A run on the CLI ------------------------------------------------------------


@dataclass
class Outcome:
    """What the CLI did: ``kind`` is answer, infeasible, error, internal, crash or timeout."""

    kind: str
    rows: list = field(default_factory=list)
    text: str = ""

    def __str__(self) -> str:
        return self.kind if self.kind == "answer" else f"{self.kind}: {self.text}"


def execute(script: str, env: Mapping[str, str]) -> Outcome:
    try:
        process = subprocess.run(
            [decidb_exe(), "-batch", "-json"], input=script, capture_output=True, text=True, env=env, timeout=_TIMEOUT
        )
    except subprocess.TimeoutExpired:
        return Outcome("timeout", text=f"no answer in {_TIMEOUT:g} seconds")
    if process.returncode < 0:
        return Outcome("crash", text=f"signal {-process.returncode}\n{process.stderr[-500:]}")
    try:
        assert_no_internal_leak(process, "the generated case", script)
    except AssertionError as leak:
        return Outcome("internal", text=str(leak))
    error = _ERROR_HEADER.search(process.stderr)
    if error:
        text = process.stderr[error.start() :].strip()
        return Outcome("infeasible" if "is infeasible" in text else "error", text=text)
    start = process.stdout.rfind("[{")
    return Outcome("answer", rows=json.loads(process.stdout[start:]) if start >= 0 else [])


def judge(case: Case, expected: Expected, outcome: Outcome) -> tuple[str, str]:
    """The category of ``outcome`` against ``expected`` and a line saying why: "ok" and "refused" pass."""
    if outcome.kind in ("internal", "crash", "timeout"):
        return outcome.kind, outcome.text
    if expected.status == "error" and outcome.text and expected.phrase in outcome.text:
        return "ok", ""
    if outcome.kind == "error":
        if any(phrase in outcome.text for phrase in case.refusals):
            return "refused", outcome.text
        return "false-error", outcome.text
    if outcome.kind == "infeasible":
        return ("ok", "") if expected.status == "infeasible" else ("false-infeasible", outcome.text)
    if expected.status != "optimal":
        return "false-answer", f"rows {outcome.rows}"
    fault = case.reference.fault_in(outcome.rows, expected)
    return ("wrong-answer", f"{fault}; rows {outcome.rows}") if fault else ("ok", "")


# -- A run of a module -------------------------------------------------------------


@dataclass
class Failure:
    module: str
    seed: int
    spelling: str
    category: str
    expected: str
    got: str
    #: The script as it ran, headed by the solver it was forced to, which the CLI reads from the environment.
    script: str
    #: What else the rerun needs: ``--broken NAME``, ``--solver NAME``.
    options: str = ""
    #: The defect ``known.py`` records this failure under; empty for a new one.
    known: str = ""

    def __str__(self) -> str:
        got = "\n".join("-- " + line for line in self.got.splitlines())
        rerun = f"python -m fuzz {self.module} --seed {self.seed} --count 1{self.options}"
        return f"-- {self.category}: {rerun} ({self.spelling})\n-- expected {self.expected}\n{got}\n{self.script}"


@dataclass
class Report:
    counts: Counter = field(default_factory=Counter)
    refusals: Counter = field(default_factory=Counter)
    failures: list[Failure] = field(default_factory=list)
    #: The solver the run solved with, which ``known.py`` records under; None under a broken oracle.
    solver: str | None = None

    def merge(self, other: "Report") -> None:
        self.counts.update(other.counts)
        self.refusals.update(other.refusals)
        self.failures.extend(other.failures)


def load(name: str):
    return importlib.import_module(f"fuzz.{name}")


def session(direct_mode: str, script: str) -> str:
    return f"SET threads=1;\nSET decide_direct_solve='{direct_mode}';\n{script}"


def solver_of(env: Mapping[str, str]) -> str:
    """The solver the CLI solves with under ``env``: the one forced, else Gurobi where the CLI has it, else HiGHS
    (``SelectSolverBackend``), whatever ``--solver`` the run was given."""
    forced = env.get("DECIDB_FORCE_SOLVER")
    if forced:
        return forced
    probe = session("off", "SELECT x FROM (VALUES (1)) t(id) DECIDE x(INT) SUCH THAT x <= 1 MAXIMIZE sum(x);")
    return "gurobi" if execute(probe, {**env, "DECIDB_FORCE_SOLVER": "gurobi"}).kind == "answer" else "highs"


def check_seed(name: str, broken: str | None, env: Mapping[str, str], seed: int) -> Report:
    """Generate the seed's case and judge each spelling of it under each direct-solve mode."""
    module = load(name)
    case = module.generate(seed, module.BROKEN[broken] if broken else module.MODEL)
    solver = env.get("DECIDB_FORCE_SOLVER")
    options = (f" --broken {broken}" if broken else "") + (f" --solver {solver}" if solver else "")
    header = f"-- run with DECIDB_FORCE_SOLVER={solver}\n" if solver else ""
    report = Report()

    def fail(spelling: str, category: str, expected: str, got: str, script: str) -> None:
        report.failures.append(Failure(name, seed, spelling, category, expected, got, header + script, options))

    try:
        expected = case.reference.expected()
    except Exception as error:  # the oracle's own failure is a finding of the seed, not the end of the run
        report.counts["oracle-error"] += 1
        script = session(DIRECT_MODES[0], next(iter(case.scripts.values())))
        fail("the oracle", "oracle-error", "a reading", repr(error), script)
        return report
    for spelling, script in case.scripts.items():
        for direct_mode in DIRECT_MODES:
            complete = session(direct_mode, script)
            category, why = judge(case, expected, execute(complete, env))
            report.counts[category] += 1
            if category == "refused":
                report.refusals[next(phrase for phrase in case.refusals if phrase in why)] += 1
            elif category != "ok":
                fail(f"{spelling}, direct {direct_mode}", category, str(expected), why, complete)
    return report


def run(
    name: str, seeds: Sequence[int], *, broken: str | None = None, solver: str | None = None, jobs: int = 1
) -> Report:
    """Every seed of one module; the report's order follows the seeds, whatever ``jobs`` is. Under the true oracle, a
    failure ``known.py`` records carries its defect."""
    env = dict(os.environ)
    if solver:
        env["DECIDB_FORCE_SOLVER"] = solver
    work = partial(check_seed, name, broken, env)
    report = Report()
    if jobs == 1:
        for seed in seeds:
            report.merge(work(seed))
    else:
        with ProcessPoolExecutor(jobs) as pool:
            for part in pool.map(work, seeds, chunksize=4):
                report.merge(part)
    if not broken:
        report.solver = solver_of(env)
        for failure in report.failures:
            failure.known = known.defect_of(name, failure.seed, report.solver, failure.category)
    return report
