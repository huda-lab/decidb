"""The scaffold every direct-solve oracle suite shares (`test_direct_s1_uniform_matroid.py`, `test_direct_a1_separable_box.py`).

A rule's suite supplies its own case table, its own function that builds the oracle model, and its own check of one
DeciDB assignment. This module supplies the rest: the fixture that keeps a forced backend out of the way, running a
query under a `decide_direct_solve` mode, the outcome of a query as a value to compare, the `VALUES` list for a table of
records, and the "oracle, `off`, `require`" comparison itself.

Test functions stay in their own files with their own names: `oracle_solver` caches each result under the test node id
plus a hash of the test function's source, so moving a test or editing its body would re-run the oracle. Only helpers
live here, and the helpers are not part of that hash.
"""

import pytest

from decidb_cli import DecidBCliError
from solver.types import SolverStatus

from ._direct_differential import error_class


@pytest.fixture(autouse=True)
def _allow_direct_path(monkeypatch):
    """The suite also runs with a process-wide forced HiGHS backend, which `require` rejects.

    A suite imports this fixture by name; being autouse, it then applies to every test in that file.
    """
    monkeypatch.delenv("DECIDB_FORCE_SOLVER", raising=False)


def run(cli, sql, mode):
    """Runs `sql` with `decide_direct_solve` set to `mode`; returns `(rows, columns)`."""
    return cli.execute(f"SET decide_direct_solve='{mode}'; {sql}")


def outcome(cli, sql, mode):
    """`("ok", rows)` or `("error", error class)`, so two paths can be compared as values."""
    try:
        return "ok", run(cli, sql, mode)[0]
    except DecidBCliError as error:
        return "error", error_class(str(error))


def values_clause(records, names, types, literal):
    """The body of a `VALUES` list: one tuple per record, `names` in order, each value through `literal(value, type)`."""
    return ", ".join("(" + ", ".join(literal(record[n], types[n]) for n in names) + ")" for record in records)


def agree_with_oracle(cli, sql, oracle, check_assignment, modes=("off", "require")):
    """Checks that DeciDB, on each of `modes`, gives the answer the oracle's result says it should.

    `oracle` is the solved oracle model. When it is infeasible or unbounded, every mode must raise that error. Otherwise
    every mode must return an assignment for which `check_assignment(rows, columns, mode)` raises no AssertionError; the
    callback owns what a valid assignment is for the rule (row layout, boxes, constraints, objective value).
    """
    if oracle.status in (SolverStatus.INFEASIBLE, SolverStatus.UNBOUNDED):
        pattern = r"(?i)infeasible" if oracle.status == SolverStatus.INFEASIBLE else r"(?i)unbounded"
        for mode in modes:
            with pytest.raises(DecidBCliError, match=pattern):
                run(cli, sql, mode)
        return
    assert oracle.status == SolverStatus.OPTIMAL, oracle.status
    for mode in modes:
        rows, columns = run(cli, sql, mode)
        try:
            check_assignment(rows, columns, mode)
        except AssertionError as error:
            raise AssertionError(f"[{mode}] {error}") from None
