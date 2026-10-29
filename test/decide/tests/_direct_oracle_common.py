"""The scaffold every direct-solve oracle suite shares (`test_direct_s1_uniform_matroid.py`,
`test_direct_a1_separable_box.py`).

A rule's suite supplies its own case table, its own function that builds the oracle model, and its own check of one
DeciDB assignment. This module supplies the rest: the fixture that keeps a forced backend out of the way, running a
query under a `decide_direct_solve` mode, the outcome of a query as a value to compare, the `VALUES` list for a table of
records, `sql_literal` (the one spelling of a constant), and the "oracle, `off`, `require`" comparison itself.

Test functions stay in their own files with their own names: `oracle_solver` caches each result under the test node id
plus a hash of the test function's source, so moving a test or editing its body would re-run the oracle. Only helpers
live here, and the helpers are not part of that hash.
"""

import contextlib
import math

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


# Direct solve declines a reducer's `by` key until S1 and R1 read it (framework todo H-05). Until then the direct path
# of a query with one is expected to fail, strictly, so that H-05 removes the mark.
BY_KEY_PENDING = "direct solve reads no by key yet (framework todo H-05)"


@contextlib.contextmanager
def direct_path(sql, pending=None):
    """Wraps the checks of `sql`'s direct path. While direct solve cannot read the query yet (`pending` names the task;
    a `by` key implies BY_KEY_PENDING) they are a strict xfail: the test is reported xfailed when the block fails and
    fails when it passes. Run the solver path before the block, so its checks stay ordinary assertions."""
    # S1 reads a `by` key; R1 (the REAL decisions) does not yet.
    reason = pending or (BY_KEY_PENDING if " by (" in sql and "(BOOL)" not in sql else None)
    if reason is None:
        yield
        return
    try:
        yield
    except (AssertionError, DecidBCliError, pytest.fail.Exception) as error:
        pytest.xfail(f"{reason}: {str(error)[:200]}")
    pytest.fail(f"[XPASS(strict)] {reason}: the direct path passes; remove the pending mark")


def _path(sql, mode, pending):
    return direct_path(sql, pending) if mode == "require" else contextlib.nullcontext()


def run(cli, sql, mode, expect_large_numbers=False):
    """Runs `sql` with `decide_direct_solve` set to `mode`; returns `(rows, columns)`.

    `expect_large_numbers` declares the large-number warning, as `DecidBCli.execute` takes it. Only a query the solver
    answers builds a model, so `off` warns when the model holds a number of 1e6 or more, `require` never does, and
    `auto` does when the rule declines the query.
    """
    return cli.execute(f"SET decide_direct_solve='{mode}'; {sql}", expect_large_numbers=expect_large_numbers)


def outcome(cli, sql, mode, expect_large_numbers=False):
    """`("ok", rows)` or `("error", error class)`, so two paths can be compared as values."""
    try:
        return "ok", run(cli, sql, mode, expect_large_numbers)[0]
    except DecidBCliError as error:
        return "error", error_class(str(error))


def values_clause(records, names, types, literal):
    """The body of a `VALUES` list: one tuple per record, `names` in order, each value through `literal(value, type)`."""
    return ", ".join("(" + ", ".join(literal(record[n], types[n]) for n in names) + ")" for record in records)


def sql_literal(value, sql_type):
    """The SQL text of one Python value as a `sql_type` constant.

    Every value but a Boolean goes through `CAST`, so a column's type does not depend on which row comes first, and
    the infinities and NaN are spelled as strings, which `repr` cannot write.
    """
    if value is None:
        return f"CAST(NULL AS {sql_type})"
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, float) and math.isinf(value):
        return f"CAST('{'' if value > 0 else '-'}Infinity' AS {sql_type})"
    if isinstance(value, float) and math.isnan(value):
        return f"CAST('NaN' AS {sql_type})"
    return f"CAST({value!r} AS {sql_type})"


def agree_with_oracle(cli, sql, oracle, check_assignment, modes=("off", "require"), pending=None, large_numbers=False):
    """Checks that DeciDB, on each of `modes`, gives the answer the oracle's result says it should.

    `oracle` is the solved oracle model. When it is infeasible or unbounded, every mode must raise that error. Otherwise
    every mode must return an assignment for which `check_assignment(rows, columns, mode)` raises no AssertionError; the
    callback owns what a valid assignment is for the rule (row layout, boxes, constraints, objective value). `pending`
    makes the direct path a strict xfail (`direct_path`). `large_numbers` says the query's model holds a number of 1e6
    or more: the solver path (`off`) warns, and the direct path (`require`) builds no model and prints nothing.
    """
    if oracle.status in (SolverStatus.INFEASIBLE, SolverStatus.UNBOUNDED):
        pattern = r"(?i)infeasible" if oracle.status == SolverStatus.INFEASIBLE else r"(?i)unbounded"
        for mode in modes:
            with _path(sql, mode, pending), pytest.raises(DecidBCliError, match=pattern):
                run(cli, sql, mode, large_numbers and mode == "off")
        return
    assert oracle.status == SolverStatus.OPTIMAL, oracle.status
    for mode in modes:
        with _path(sql, mode, pending):
            rows, columns = run(cli, sql, mode, large_numbers and mode == "off")
            try:
                check_assignment(rows, columns, mode)
            except AssertionError as error:
                raise AssertionError(f"[{mode}] {error}") from None
