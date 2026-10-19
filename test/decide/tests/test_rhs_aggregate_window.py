"""Aggregates on the right of a reduced constraint are window columns of the DECIDE source (H-19).

`SUM(x*w) <= SUM(cap)`: the right side is a reducer over data. The DECIDE optimizer replaces it with the same DuckDB
aggregate as a window over the clause's group (its PER keys and WHEN), so the solver reads the exact value DuckDB
computes. The left side here has a coefficient column, so no direct-solve rule applies and the solver path (`off`) is the
one under test; the S1 suite (`test_direct_s1_uniform_matroid.py`) covers the same right sides under `require`.

Each case builds the oracle model from the raw rows: one constraint per group, whose limit is the aggregate evaluated in
Python over the rows the clause covers, in exact arithmetic. Rows the clause does not cover (WHEN false, or a NULL PER
key) carry an extreme `cap`, so a window that read them would change the limit.
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
# A row the clause does not cover under every scope below has cap BIG or -BIG.
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
    (10, None, True, 4, BIG, 6.0),
    (11, None, True, 5, -BIG, 2.0),
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
# scope -> (suffix, covers a row, groups by key)
SCOPES = {
    "global": ("", lambda row: True, False),
    "per": (" PER g", lambda row: row[1] is not None, True),
    "when": (" WHEN flag", lambda row: row[2], False),
    "when_per": (" WHEN flag PER g", lambda row: row[2] and row[1] is not None, True),
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


def _limits(rows, aggregate, scope):
    """(member ids, limit) per group the clause covers."""
    _, covers, grouped = SCOPES[scope]
    covered = [row for row in rows if covers(row)]
    groups = {}
    for row in covered:
        groups.setdefault(row[1] if grouped else None, []).append(row)
    return [
        ([row[0] for row in members], AGGREGATES[aggregate]([row[4] for row in members])) for members in groups.values()
    ]


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
    suffix = SCOPES[scope][0]
    # A top WHEN placed after a bare aggregate would be read as the aggregate's own WHEN.
    bound = f"{aggregate} + 0" if "WHEN" in suffix else aggregate
    constraint = f"SUM(x * w) <= {bound}{suffix}"
    limits = _limits(ROWS, aggregate, scope)
    _agree(decidb_cli, oracle_solver, ROWS, constraint, limits)


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
    covered = "SUM(x * w) <= COUNT(*) * 4611686018427387904 + 0 WHEN (id > 100)"
    # No row is covered: the clause has nothing to evaluate, and the left side's own empty-aggregate error is raised.
    with pytest.raises(DecidBCliError, match="empty row set"):
        run(decidb_cli, _sql(ROWS, covered), "off")


@pytest.mark.error
@pytest.mark.parametrize("scope", ["global", "when_per"])
def test_rhs_aggregate_argument_that_is_null_raises_on_every_row(decidb_cli, scope):
    """The solver reads the argument on every row: a NULL in a row the clause leaves out still raises, naming it."""
    suffix = SCOPES[scope][0]
    rows = [row[:4] + (None if row[0] == 8 else row[4], row[5]) for row in ROWS]
    bound = "MAX(cap) + 0" if "WHEN" in suffix else "MAX(cap)"
    sql = _sql(rows, f"SUM(x * w) <= {bound}{suffix}")
    with pytest.raises(DecidBCliError, match=r'column "cap" is NULL'):
        run(decidb_cli, sql, "off")


@pytest.mark.error
def test_rhs_aggregate_argument_that_is_nan_raises_without_a_row_position(decidb_cli):
    sql = (
        "SELECT id, x FROM (FROM (VALUES (0, 3, 1.0, 5.0), (1, 4, 'NaN'::DOUBLE, 4.0)) t(id, w, cap, p) "
        "DECIDE x(BOOL) SUCH THAT SUM(x * w) <= MIN(cap) MAXIMIZE SUM(p*x)) q"
    )
    with pytest.raises(DecidBCliError, match=r"constraint right-hand side aggregate contains invalid value \(NaN\)"):
        run(decidb_cli, sql, "off")


@pytest.mark.query_diagnostics
@pytest.mark.correctness
def test_diagnose_names_the_clause_with_a_right_hand_aggregate(decidb_cli):
    """The right side is a window column inside the plan, but the clause reads as the user wrote it."""
    sql = f"""DIAGNOSE SELECT id, x FROM (VALUES {_values(ROWS)}) t({COLUMNS})
        DECIDE x(BOOL) SUCH THAT SUM(x * w) >= SUM(cap) + 100 PER g MAXIMIZE SUM(p*x)"""
    rows, columns = decidb_cli.execute(sql)
    clauses = {row[columns.index("clause")] for row in rows if row[columns.index("clause")]}
    assert "SUM(x * w) >= SUM(cap) + 100 PER g" in clauses, rows
