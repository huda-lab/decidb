"""Aggregates on the right of a reduced constraint are window columns of the DECIDE source (H-19).

`SUM(x*w) <= SUM(cap)`: the right side is a reducer over data. The DECIDE optimizer replaces it with the same DuckDB
aggregate as a window over the reducer's own `by` groups of the rows the clause's `when` keeps, so the solver reads the
exact value DuckDB computes. The left side here has a coefficient column, so no direct-solve rule applies and the solver
path (`off`) is the one under test; the S1 suite (`test_direct_s1_uniform_matroid.py`) covers the same right sides under
`require`.

Each case builds the oracle model from the raw rows, as the language reads a clause: every kept row bounds its left
side's `by` group by the right side's value over its own `by` group of the kept rows, NULL a group of its own; equal
rows collapse to one. The limit is evaluated in Python, in exact arithmetic. Rows the `when` drops carry an extreme
`cap`, so a window that read them would change the limit.
"""

import math
from fractions import Fraction

import pytest

from decidb_cli import DecidBCliError
from solver.types import ObjSense, VarType

from ._direct_oracle_common import _allow_direct_path  # noqa: F401  (autouse fixture)
from ._direct_oracle_common import agree_with_oracle, outcome, run

BIG = 10**12

# id, g, flag, w, cap, p. Group a: rows 0-3, b: 4-7, c: 8-9 (no flagged row), NULL key: 10-11 (flagged).
# A row `when flag` drops has cap BIG or -BIG.
ROWS = [
    (0, "a", True, 3, 4, 9.0),
    (1, "a", True, 5, 2, 8.0),
    (2, "a", False, 4, BIG, 7.0),
    (3, "a", True, 2, 6, 6.5),
    (4, "b", True, 6, 3, 9.5),
    (5, "b", False, 3, -BIG, 5.0),
    (6, "b", True, 4, 5, 7.5),
    (7, "b", True, 5, 1, 4.0),
    (8, "c", False, 4, BIG, 8.5),
    (9, "c", False, 3, BIG, 3.0),
    (10, None, True, 4, 7, 6.0),
    (11, None, True, 5, 3, 2.0),
]
COLUMNS = "id, g, flag, w, cap, p"

AGGREGATES = {
    "COUNT(*)": lambda caps: len(caps),
    "COUNT(cap)": lambda caps: len(caps),
    "MIN(cap)": lambda caps: min(caps),
    "MAX(cap)": lambda caps: max(caps),
    "SUM(cap)": lambda caps: sum(caps),
    "AVG(cap)": lambda caps: Fraction(sum(caps), len(caps)),
}
# scope -> (the constraint around `{bound}`, the right side's `by` written after it, the clause's keyword arguments to
# _clause_limits)
SCOPES = {
    "global": ("SUM(x * w) <= {bound}", "", {}),
    "by": ("SUM(x * w) by (g) <= {bound}", " by (g)", {"left_key": 1, "right_key": 1}),
    "by_left_only": ("SUM(x * w) by (g) <= {bound}", "", {"left_key": 1}),
    "when": ("when flag: SUM(x * w) <= {bound}", "", {"keeps": lambda row: row[2]}),
    "when_by": (
        "when flag: SUM(x * w) by (g) <= {bound}",
        " by (g)",
        {"keeps": lambda row: row[2], "left_key": 1, "right_key": 1},
    ),
}


def _values(rows):
    types = ["INTEGER", "VARCHAR", "BOOLEAN", "INTEGER", "BIGINT", "DOUBLE"]
    out = []
    for row in rows:
        parts = []
        for value, sql_type in zip(row, types):
            if value is None:
                parts.append(f"NULL::{sql_type}")
            elif isinstance(value, bool):
                parts.append(str(value).lower())
            elif isinstance(value, str):
                parts.append(f"'{value}'")
            else:
                parts.append(f"{value}::{sql_type}")
        out.append("(" + ", ".join(parts) + ")")
    return ", ".join(out)


def _sql(rows, constraint):
    return (
        f"SELECT id, x FROM (FROM (VALUES {_values(rows)}) t({COLUMNS}) "
        f"DECIDE x(BOOL) SUCH THAT {constraint} MAXIMIZE SUM(p*x)) q ORDER BY id"
    )


def _clause_limits(
    rows, value, keeps=lambda row: True, left_key=None, right_key=None, left=lambda row: True, right=lambda row: True
):
    """(member ids, limit) per distinct constraint of a clause `[when keeps:] SUM([when left:] x * w) [by left_key] <=
    value([when right:] ...) [by right_key]`, over the rows `keeps` keeps. A key is a column index; None is `by ()`."""
    kept = [row for row in rows if keeps(row)]
    limits = []
    for row in kept:
        members = [k[0] for k in kept if (left_key is None or k[left_key] == row[left_key]) and left(k)]
        reads = [k for k in kept if (right_key is None or k[right_key] == row[right_key]) and right(k)]
        limit = (members, value(reads))
        if limit not in limits:
            limits.append(limit)
    return limits


def _caps(aggregate):
    return lambda members: AGGREGATES[aggregate]([row[4] for row in members])


def _scoped(scope, bound):
    constraint, right_by, clause = SCOPES[scope]
    return constraint.format(bound=bound + right_by), clause


def _agree(cli, oracle_solver, rows, constraint, limits):
    oracle_solver.create_model("rhs_aggregate_window")
    names = {row[0]: f"x_{row[0]}" for row in rows}
    for name in names.values():
        oracle_solver.add_variable(name, VarType.BINARY)
    weight = {row[0]: row[3] for row in rows}
    constraints = []
    for members, limit in limits:
        coefficients = {names[i]: float(weight[i]) for i in members}
        constraints.append((members, limit))
        oracle_solver.add_constraint(coefficients, "<=", float(limit))
    oracle_solver.set_objective({names[row[0]]: row[5] for row in rows}, ObjSense.MAXIMIZE)
    oracle = oracle_solver.solve()

    def check_assignment(result, columns, mode):
        assert columns == ["id", "x"], mode
        chosen = {row[0]: row[1] for row in result}
        assert set(chosen) == set(names), mode
        for members, limit in constraints:
            used = sum(weight[i] * chosen[i] for i in members)
            assert used <= limit + 1e-9, (mode, members, used, limit)
        value = sum(row[5] * chosen[row[0]] for row in rows)
        assert math.isclose(value, oracle.objective_value, abs_tol=1e-6), (mode, value, oracle.objective_value)

    agree_with_oracle(cli, _sql(rows, constraint), oracle, check_assignment, modes=("off",))


@pytest.mark.cons_aggregate
@pytest.mark.per_clause
@pytest.mark.correctness
@pytest.mark.parametrize("scope", sorted(SCOPES))
@pytest.mark.parametrize("aggregate", sorted(AGGREGATES))
def test_rhs_aggregate_window_matches_the_oracle(decidb_cli, oracle_solver, aggregate, scope):
    constraint, clause = _scoped(scope, aggregate)
    _agree(decidb_cli, oracle_solver, ROWS, constraint, _clause_limits(ROWS, _caps(aggregate), **clause))


@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize("scope", ["global", "by"])
def test_rhs_count_of_a_text_column_counts_its_values(decidb_cli, oracle_solver, scope):
    """`COUNT(g)` over the VARCHAR key counts its non-NULL values, as SQL's COUNT does; the solver once cast the
    argument to DOUBLE. ROWS holds NULL keys, which COUNT skips."""
    constraint, clause = _scoped(scope, "COUNT(g)")
    counted = lambda reads: sum(1 for row in reads if row[1] is not None)  # noqa: E731
    _agree(decidb_cli, oracle_solver, ROWS, constraint, _clause_limits(ROWS, counted, **clause))


@pytest.mark.cons_aggregate
@pytest.mark.per_clause
@pytest.mark.correctness
@pytest.mark.parametrize("scope", ["when", "when_by"])
def test_rhs_keyed_reducer_reads_each_class_once(decidb_cli, oracle_solver, scope):
    """`sum(per cap: cap)` adds each distinct cap of the kept rows once (the class's first row stands for it), within
    the `by` group when there is one. The flagged caps repeat 3, so a plain SUM would differ."""
    constraint, clause = _scoped(scope, "SUM(per cap: cap)")
    distinct = lambda reads: sum({row[4] for row in reads})  # noqa: E731
    _agree(decidb_cli, oracle_solver, ROWS, constraint, _clause_limits(ROWS, distinct, **clause))


@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_rhs_sum_of_bigint_values_that_cancel_is_exact(decidb_cli, oracle_solver):
    """2^60, 1 and -2^60 add up to 1. The solver used to add in DOUBLE and read 0."""
    rows = [
        (0, "a", True, 1, 2**60, 5.0),
        (1, "a", True, 1, 1, 4.0),
        (2, "a", True, 1, -(2**60), 3.0),
    ]
    limits = [([0, 1, 2], 1)]
    _agree(decidb_cli, oracle_solver, rows, "SUM(x * w) <= SUM(cap)", limits)
    sql = _sql(rows, "SUM(x * w) <= SUM(cap)")
    chosen = [row[1] for row in run(decidb_cli, sql, "off")[0]]
    assert chosen == [1, 0, 0]


@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_rhs_sum_with_arithmetic_keeps_the_aggregate_type(decidb_cli, oracle_solver):
    """`COUNT(*) * k` is evaluated in DuckDB's types: a group of 3 rows against 3 * 2^61 overflows INT64 on both paths."""
    sql = _sql(ROWS, "SUM(x * w) <= COUNT(*) * 4611686018427387904")
    assert outcome(decidb_cli, sql, "off")[0] == "error"
    covered = "when id > 100: SUM(x * w) <= COUNT(*) * 4611686018427387904"
    # No row is kept: the clause has nothing to evaluate, and its `when` keeping no row is the error (D4).
    with pytest.raises(DecidBCliError, match=r"when id > 100 matches no rows\."):
        run(decidb_cli, _sql(ROWS, covered), "off")


@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize(
    "bound, scope, value",
    [
        # AVG divides by the values it reads: group a's flagged caps 4, NULL, 6 average 5, not 10/3.
        ("AVG(cap)", "when_by", lambda caps: Fraction(sum(caps), len(caps))),
        # A norm's value check reads the body the user wrote; its definition skips the NULL as well.
        ("norm(cap, 0)", "when", lambda caps: sum(1 for cap in caps if cap != 0)),
    ],
    ids=["avg", "norm"],
)
def test_rhs_aggregate_skips_a_null_argument_as_sql_does(decidb_cli, oracle_solver, bound, scope, value):
    """A reducer over data reads its argument as the same SQL aggregate does: a NULL is skipped. Rows 1 and 7 are
    flagged rows of groups a and b."""
    rows = [row[:4] + (None if row[0] in (1, 7) else row[4], row[5]) for row in ROWS]
    constraint, clause = _scoped(scope, bound)
    reads = lambda members: value([row[4] for row in members if row[4] is not None])  # noqa: E731
    _agree(decidb_cli, oracle_solver, rows, constraint, _clause_limits(rows, reads, **clause))


@pytest.mark.error
def test_rhs_aggregate_over_only_nulls_is_a_null_bound(decidb_cli):
    """Every cap of group b is NULL, so its MAX is NULL, as in SQL; a NULL bound is an error, which quotes the reducer
    as written rather than calling it a column."""
    rows = [row[:4] + (None if row[1] == "b" else row[4], row[5]) for row in ROWS]
    with pytest.raises(DecidBCliError, match=r"DECIDE: max\(cap\) by \(g\) is NULL\. Impute it with COALESCE\(\)"):
        run(decidb_cli, _sql(rows, "SUM(x * w) by (g) <= MAX(cap) by (g)"), "off")


@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_rhs_aggregate_argument_that_is_null_on_a_dropped_row_is_never_read(decidb_cli, oracle_solver):
    """Row 8 is not flagged, so `when flag` drops it and its NULL is never read."""
    rows = [row[:4] + (None if row[0] == 8 else row[4], row[5]) for row in ROWS]
    constraint, clause = _scoped("when_by", "MAX(cap)")
    _agree(decidb_cli, oracle_solver, rows, constraint, _clause_limits(rows, _caps("MAX(cap)"), **clause))


@pytest.mark.error
def test_rhs_aggregate_argument_that_is_nan_raises_without_a_row_position(decidb_cli):
    sql = (
        "SELECT id, x FROM (FROM (VALUES (0, 3, 1.0, 5.0), (1, 4, 'NaN'::DOUBLE, 4.0)) t(id, w, cap, p) "
        "DECIDE x(BOOL) SUCH THAT SUM(x * w) <= MIN(cap) MAXIMIZE SUM(p*x)) q"
    )
    with pytest.raises(DecidBCliError, match=r"constraint right-hand side aggregate contains invalid value \(NaN\)"):
        run(decidb_cli, sql, "off")


@pytest.mark.error
@pytest.mark.parametrize("mode", ["off", "require"])
def test_rhs_l0_norm_reads_a_nan_body_as_invalid(decidb_cli, mode):
    """`SIGN(NaN)` is 0, so a NaN row would count as a zero; the value check reads the body the user wrote instead."""
    sql = (
        "SELECT id, x FROM (FROM (VALUES (0, 3, 1.0, 5.0), (1, 4, 'NaN'::DOUBLE, 4.0)) t(id, w, cap, p) "
        "DECIDE x(BOOL) SUCH THAT SUM(x) <= norm(cap, 0) MAXIMIZE SUM(p*x)) q"
    )
    with pytest.raises(DecidBCliError, match=r"right-hand side aggregate contains invalid value \(NaN\)"):
        run(decidb_cli, sql, mode)


@pytest.mark.error
@pytest.mark.parametrize("mode", ["off", "require"])
def test_rhs_value_is_checked_after_the_scope_checks(decidb_cli, mode):
    """`cap` is NULL on a kept row and the reducer keeps no row: the scope check (D8) is reported, not the NULL."""
    sql = (
        "SELECT id, x FROM (FROM (VALUES (0, 3, NULL::DOUBLE, 5.0), (1, 4, 4.0, 4.0)) t(id, w, cap, p) "
        "DECIDE x(BOOL) SUCH THAT SUM(x) <= cap + sum(when id > 5: w) MAXIMIZE SUM(p*x)) q"
    )
    with pytest.raises(DecidBCliError, match=r"DECIDE: sum\(when id > 5: w\) has no rows\."):
        run(decidb_cli, sql, mode)


@pytest.mark.error
def test_rhs_subquery_that_is_null_is_named_alike_on_both_paths(decidb_cli):
    """A query-wide bound is checked with the other bounds, and a NULL subquery is named as the user wrote it."""
    sql = (
        "SELECT id, x FROM (FROM (VALUES (0, 3, 5.0), (1, 4, 4.0)) t(id, w, p) "
        "DECIDE x(BOOL) SUCH THAT SUM(x) <= (SELECT NULL::INTEGER) MAXIMIZE SUM(p*x)) q"
    )
    messages = []
    for mode in ("off", "require"):
        with pytest.raises(DecidBCliError, match=r"the subquery \(SELECT CAST\(NULL AS INTEGER\)\) is NULL") as error:
            run(decidb_cli, sql, mode)
        messages.append(str(error.value))
    assert messages[0] == messages[1]


@pytest.mark.query_diagnostics
@pytest.mark.correctness
def test_diagnose_names_the_clause_with_a_right_hand_aggregate(decidb_cli):
    """The right side is a window column inside the plan, but the clause reads as the user wrote it."""
    sql = f"""DIAGNOSE SELECT id, x FROM (VALUES {_values(ROWS)}) t({COLUMNS})
        DECIDE x(BOOL) SUCH THAT SUM(x * w) by (g) >= SUM(cap) by (g) + 100 MAXIMIZE SUM(p*x)"""
    rows, columns = decidb_cli.execute(sql)
    clauses = {row[columns.index("clause")] for row in rows if row[columns.index("clause")]}
    assert "sum(x * w) by (g) >= sum(cap) by (g) + 100" in clauses, rows


# Right sides whose reducer the window reads in a way no other case does. id -> (aggregate text, its value over the rows
# a group reads, scope).
READER_CASES = {
    # A constant argument reads no source column, yet the window still sums it once per row of the group.
    "constant_argument": ("SUM(2)", lambda members: 2 * len(members), "by"),
    # `norm(cap, 1)` arrives as `SUM(ABS(cap))` with the written-norm tag: the value check reads the written body.
    "written_norm": ("norm(cap, 1)", lambda members: sum(abs(row[4]) for row in members), "when_by"),
    # A norm of order 0 over data is the count of its non-zero rows, `SUM(ABS(SIGN(e)))`: its body is two calls deep.
    "written_l0_norm": ("norm(cap - 3, 0)", lambda members: sum(1 for row in members if row[4] != 3), "when_by"),
    # A Boolean argument: the value check tests it for NULL only, never casting it to DOUBLE for a NaN test.
    "boolean_argument": ("COUNT(flag)", lambda members: len(members), "when_by"),
    # DuckDB drops the ORDER BY of an order-insensitive aggregate at bind time, which the window asserts.
    "order_by_dropped": ("MIN(cap ORDER BY id)", lambda members: min(row[4] for row in members), "when_by"),
}


@pytest.mark.cons_aggregate
@pytest.mark.per_clause
@pytest.mark.correctness
@pytest.mark.parametrize("case", sorted(READER_CASES))
def test_rhs_aggregate_readers_match_the_oracle(decidb_cli, oracle_solver, case):
    aggregate, value, scope = READER_CASES[case]
    constraint, clause = _scoped(scope, aggregate)
    _agree(decidb_cli, oracle_solver, ROWS, constraint, _clause_limits(ROWS, value, **clause))


# Rows where every cap is small, so a limit read from the wrong group binds and changes the answer.
PLAIN = [
    (0, "a", True, 3, 4, 9.0),
    (1, "a", True, 5, 2, 8.0),
    (2, "a", False, 4, 1, 7.0),
    (3, "b", True, 2, 3, 6.5),
    (4, "b", True, 6, 2, 9.5),
    (5, "b", False, 3, 1, 5.0),
]


# id -> (constraint, the clauses it holds). Each pair writes the same `SUM(cap)` twice over different groups, so the two
# window columns must stay apart; the case names the difference the rewrite tells them apart by.
SHARED_AGGREGATE_CASES = {
    "key_count": (
        "SUM(x * w) <= SUM(cap) AND SUM(x * w) by (g) <= SUM(cap) by (g)",
        [{}, {"left_key": 1, "right_key": 1}],
    ),
    "clause_when_present": (
        "when flag: SUM(x * w) <= SUM(cap) AND SUM(x * w) <= SUM(cap)",
        [{"keeps": lambda row: row[2]}, {}],
    ),
    "key_column": (
        "SUM(x * w) by (g) <= SUM(cap) by (g) AND SUM(x * w) by (flag) <= SUM(cap) by (flag)",
        [{"left_key": 1, "right_key": 1}, {"left_key": 2, "right_key": 2}],
    ),
    "clause_when_value": (
        "when flag: SUM(x * w) <= SUM(cap) AND when id < 4: SUM(x * w) <= SUM(cap)",
        [{"keeps": lambda row: row[2]}, {"keeps": lambda row: row[0] < 4}],
    ),
}


@pytest.mark.cons_aggregate
@pytest.mark.per_clause
@pytest.mark.correctness
@pytest.mark.parametrize("case", sorted(SHARED_AGGREGATE_CASES))
def test_rhs_same_aggregate_in_two_clauses_reads_each_clauses_group(decidb_cli, oracle_solver, case):
    constraint, clauses = SHARED_AGGREGATE_CASES[case]
    limits = [limit for clause in clauses for limit in _clause_limits(PLAIN, _caps("SUM(cap)"), **clause)]
    _agree(decidb_cli, oracle_solver, PLAIN, constraint, limits)


@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize("aggregate", ["SUM", "MIN", "AVG"])
def test_rhs_aggregate_with_its_own_when_under_a_clause_when_matches_the_oracle(decidb_cli, oracle_solver, aggregate):
    """A reducer's own `when` narrows the rows the clause keeps; its argument is read only where both hold."""
    constraint = f"when id < 5: SUM(x * w) by (g) <= {aggregate}(when flag: cap) by (g)"
    limits = _clause_limits(
        PLAIN,
        _caps(f"{aggregate}(cap)"),
        keeps=lambda row: row[0] < 5,
        left_key=1,
        right_key=1,
        right=lambda row: row[2],
    )
    _agree(decidb_cli, oracle_solver, PLAIN, constraint, limits)


@pytest.mark.error
def test_rhs_aggregate_whose_own_when_empties_a_group_raises_d8(decidb_cli):
    """Group c has no flagged row, so the right side has no rows to read there (D8)."""
    with pytest.raises(DecidBCliError, match=r"sum\(when flag: cap\) has no rows for g = 'c'\."):
        run(decidb_cli, _sql(ROWS, "SUM(x * w) by (g) <= SUM(when flag: cap) by (g)"), "off")


@pytest.mark.error
def test_rhs_aggregate_shapes_the_binder_rejects(decidb_cli):
    # The binder admits no DISTINCT reducer, so a window never has to copy one.
    with pytest.raises(DecidBCliError, match=r"does not support 'sum\(DISTINCT cap\)'"):
        run(decidb_cli, _sql(PLAIN, "SUM(x * w) <= SUM(DISTINCT cap)"), "off")
