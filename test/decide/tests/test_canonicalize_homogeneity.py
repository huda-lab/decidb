"""Aggregate/per-row homogeneity at the canonicalization boundary.

A reducer anywhere in a comparison makes the comparison aggregate-shaped.  A
bare decision term beside it is legal only when that decision is one value for
the whole query.  Row- and entity-scoped decisions must occur inside a reducer;
otherwise no single solver value exists to compare with the reduced number.

These tests also pin error ownership.  Unsupported mixtures are binder/planning
errors with SQL-level guidance, never late physical-extractor errors.
"""

import pytest

from ._scope_cases import answer, scope_solver  # noqa: F401 -- scope_solver is a fixture
from ._scope_oracle import CONSTANT, GLOBAL, Decision, Reduce, Scoped, ScopedModel, ScopedObjective
from solver.types import ObjSense


@pytest.mark.var_integer
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_query_wide_decision_beside_data_reducer(decidb_cli):
    """A scalar decision is row-invariant and may sit beside a data reducer."""
    rows, cols = decidb_cli.execute(
        """
        SELECT id, p, s
        FROM (VALUES (1, 1), (2, 2), (3, 3)) t(id, p)
        DECIDE per (): s(INT)
        SUCH THAT SUM(p) + s <= 10 AND s <= 10
        MAXIMIZE s
    """
    )
    si = cols.index("s")
    assert {int(r[si]) for r in rows} == {4}, rows


@pytest.mark.var_integer
@pytest.mark.cons_aggregate
@pytest.mark.per_clause
@pytest.mark.correctness
def test_query_wide_decision_beside_per_data_reducer(decidb_cli):
    """The same query-wide decision participates in every per class's reduced bound."""
    rows, cols = decidb_cli.execute(
        """
        SELECT id, grp, p, s
        FROM (VALUES (1, 'a', 1), (2, 'a', 2), (3, 'b', 3)) t(id, grp, p)
        DECIDE per (): s(INT)
        SUCH THAT per grp: sum(p) by (grp) + s <= 10 AND s <= 10
        MAXIMIZE s
    """
    )
    si = cols.index("s")
    assert {int(r[si]) for r in rows} == {7}, rows


@pytest.mark.var_integer
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_row_scoped_decision_inside_reducer_remains_legal(decidb_cli):
    """Row scope is legal when the reducer owns the row-to-one collapse."""
    rows, cols = decidb_cli.execute("""
        SELECT id, p, x
        FROM (VALUES (1, 1), (2, 2), (3, 3)) t(id, p)
        DECIDE x(INT)
        SUCH THAT SUM(p * x) <= 6 AND x <= 3
        MAXIMIZE SUM(x)
    """)
    xi = cols.index("x")
    assert sum(int(r[xi]) * int(r[cols.index("p")]) for r in rows) <= 6


@pytest.mark.var_integer
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_row_scoped_decision_beside_data_reducer_answers(decidb_cli, scope_solver):
    """``SUM(p) + x <= 10`` is generated once per row (D30): each row's x is bounded by
    10 less the total of p."""
    rows = [{"id": 1, "p": 1}, {"id": 2, "p": 2}, {"id": 3, "p": 3}]
    model = ScopedModel(rows, [Decision("x")])
    clauses = [
        Scoped((Reduce("sum", lambda r: {CONSTANT: r["p"]}), lambda r: {"x": 1}), "<=", (lambda r: {CONSTANT: 10},))
    ]
    objective = ScopedObjective(ObjSense.MAXIMIZE, (Reduce("sum", lambda r: {"x": 1}),))
    result = answer(
        decidb_cli,
        """
        SELECT id, p, x
        FROM (VALUES (1, 1), (2, 2), (3, 3)) t(id, p)
        DECIDE x(INT)
        SUCH THAT SUM(p) + x <= 10
        MAXIMIZE SUM(x)
        ORDER BY id
    """,
    )
    assert model.judge_scoped(result, clauses, objective) == pytest.approx(
        model.solve_scoped(scope_solver, clauses, objective), abs=1e-4
    )


@pytest.mark.var_integer
@pytest.mark.cons_aggregate
@pytest.mark.sql_joins
@pytest.mark.correctness
def test_keyed_decision_beside_global_reducer_answers(decidb_cli, duckdb_conn, scope_solver):
    """``SUM(c.c_acctbal) + y <= 1e9`` with a decision per nation is generated once per row
    (D30): each row bounds its nation's y."""
    rows = [
        dict(zip(("c_custkey", "n_nationkey", "c_acctbal"), row))
        for row in duckdb_conn.execute(
            """
        SELECT c.c_custkey, n.n_nationkey, c.c_acctbal
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE c.c_custkey <= 20 ORDER BY c.c_custkey"""
        ).fetchall()
    ]
    model = ScopedModel(rows, [Decision("y", ("n_nationkey",))])
    y = lambda r: {"y": 1}  # noqa: E731
    clauses = [
        Scoped(
            (Reduce("sum", lambda r: {CONSTANT: float(r["c_acctbal"])}), y), "<=", (lambda r: {CONSTANT: 1000000000},)
        ),
        Scoped((y,), "<=", (lambda r: {CONSTANT: 10},)),
    ]
    objective = ScopedObjective(ObjSense.MAXIMIZE, (Reduce("sum", y),))
    result = answer(
        decidb_cli,
        """
        SELECT c.c_custkey, n.n_nationkey, y
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE c.c_custkey <= 20
        DECIDE per n: y(INT)
        SUCH THAT SUM(c.c_acctbal) + y <= 1000000000 AND y <= 10
        MAXIMIZE SUM(y)
        ORDER BY c.c_custkey
    """,
    )
    assert model.judge_scoped(result, clauses, objective) == pytest.approx(
        model.solve_scoped(scope_solver, clauses, objective), abs=1e-4
    )


@pytest.mark.var_integer
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_row_varying_coefficient_on_query_wide_decision_beside_reducer_answers(decidb_cli, scope_solver):
    """``SUM(p) + p * s <= 100`` is generated once per row (D30): each row scales the one
    query-wide s by its own p, so the largest p binds."""
    rows = [{"id": 1, "p": 1}, {"id": 2, "p": 2}, {"id": 3, "p": 3}]
    model = ScopedModel(rows, [Decision("s", GLOBAL)])
    clauses = [
        Scoped(
            (Reduce("sum", lambda r: {CONSTANT: r["p"]}), lambda r: {"s": r["p"]}), "<=", (lambda r: {CONSTANT: 100},)
        )
    ]
    objective = ScopedObjective(ObjSense.MAXIMIZE, (lambda r: {"s": 1},))
    result = answer(
        decidb_cli,
        """
        SELECT id, p, s
        FROM (VALUES (1, 1), (2, 2), (3, 3)) t(id, p)
        DECIDE per (): s(INT)
        SUCH THAT SUM(p) + p * s <= 100
        MAXIMIZE s
        ORDER BY id
    """,
    )
    assert model.judge_scoped(result, clauses, objective) == pytest.approx(
        model.solve_scoped(scope_solver, clauses, objective), abs=1e-4
    )
