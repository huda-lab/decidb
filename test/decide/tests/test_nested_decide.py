"""A DECIDE query used as a subquery inside another DECIDE clause.

The inner query is an ordinary scalar subquery: it solves first, and its result
becomes a bound in the enclosing clause. What makes it worth its own file is the
lexer. ``WHEN`` is rewritten to a DECIDE-specific token only while
``in_decide_clause`` is set, and that state used to be a single Boolean that the
*inner* clause cleared on its way out — so an outer ``WHEN`` written after the
subquery lexed as ordinary SQL ``WHEN`` and failed to parse. Nesting alone worked,
and an outer ``WHEN`` alone worked; only the combination broke. The state is now
saved and restored per clause (``PGDecidePushLexState`` / ``PGDecidePopLexState``),
so these tests pin the combination rather than either half.

See ``pipeline/01_parser/done.md`` §2, lexer gating.

Covers:
  - Nested clause followed by an outer ``WHEN``, oracle verified
  - The same with the inner clause written in the paper's split order
  - Nesting with no outer ``WHEN`` still behaves as before
  - Ordinary ``CASE WHEN`` after a nested clause still lexes as SQL
  - The duplicate-DECIDE guard is not tripped by a nested declaration
  - Three levels of DECIDE nesting, oracle verified
  - A plain scalar subquery inside a nested clause's constraint and objective

The second group has a different cause -- subquery flattening rather than the
lexer -- documented above that group below.
"""

import pytest

from solver.types import ObjSense, VarType


# The inner problem: two integers capped at 4, maximized. The outer problem reads
# its objective as a budget, and carries a WHEN-gated cap after the subquery.
_INNER = (
    "SELECT SUM(y) FROM (VALUES (1), (2)) u(uid) "
    "DECIDE y(INT) SUCH THAT y >= 0 AND y <= 4 MAXIMIZE SUM(y)"
)
_INNER_SPLIT_ORDER = (
    "SELECT SUM(y) DECIDE y(INT) FROM (VALUES (1), (2)) u(uid) "
    "SUCH THAT y >= 0 AND y <= 4 MAXIMIZE SUM(y)"
)


def _outer(inner: str, trailing_when: bool) -> str:
    when = "\n    AND when grp = 'a': sum(x) <= 5" if trailing_when else ""
    return f"""
        SELECT id, x
        FROM (VALUES (1, 'a'), (2, 'b')) t(id, grp)
        DECIDE x(INT)
        SUCH THAT x >= 0 AND x <= 9
            AND SUM(x) <= ({inner}){when}
        MAXIMIZE SUM(x)
    """


def _objective(rows) -> int:
    """Total of the decision column. The split between the two rows is degenerate
    under this objective, so the total is the only stable thing to assert."""
    return sum(int(row[1]) for row in rows)


def _oracle_objective(oracle_solver, trailing_when: bool) -> float:
    """Solve the same two problems independently, inner first, exactly as the
    nesting does: the inner optimum is the outer's budget."""
    oracle_solver.create_model("nested_inner")
    inner_obj = {}
    for i in (1, 2):
        oracle_solver.add_variable(f"y_{i}", VarType.INTEGER, lb=0.0, ub=4.0)
        inner_obj[f"y_{i}"] = 1.0
    oracle_solver.set_objective(inner_obj, ObjSense.MAXIMIZE)
    budget = oracle_solver.solve().objective_value

    oracle_solver.create_model("nested_outer")
    outer_obj = {}
    for i in (1, 2):
        oracle_solver.add_variable(f"x_{i}", VarType.INTEGER, lb=0.0, ub=9.0)
        outer_obj[f"x_{i}"] = 1.0
    oracle_solver.add_constraint(outer_obj, "<=", budget, name="nested_budget")
    if trailing_when:
        # WHEN grp = 'a' selects row 1 only, so the gated SUM covers x_1 alone.
        oracle_solver.add_constraint({"x_1": 1.0}, "<=", 5.0, name="when_gated_cap")
    oracle_solver.set_objective(outer_obj, ObjSense.MAXIMIZE)
    return oracle_solver.solve().objective_value


@pytest.mark.correctness
@pytest.mark.sql_subquery
@pytest.mark.when
def test_nested_decide_then_outer_when_oracle(decidb_cli, oracle_solver, perf_tracker):
    """The regression: a constraint using ``WHEN`` *after* a nested DECIDE subquery.

    This is the shape that used to raise `syntax error at or near "WHEN"`, because
    the inner clause's reduction disarmed the outer clause's lexer state.
    """
    rows, _ = decidb_cli.execute(_outer(_INNER, trailing_when=True))
    assert len(rows) == 2
    expected = _oracle_objective(oracle_solver, trailing_when=True)
    assert abs(_objective(rows) - expected) < 1e-6, \
        f"DeciDB={_objective(rows)}, Oracle={expected}"


@pytest.mark.correctness
@pytest.mark.sql_subquery
@pytest.mark.when
def test_nested_split_order_inner_then_outer_when(decidb_cli, oracle_solver, perf_tracker):
    """The inner clause in the paper's split order restores the outer state too.

    The split order routes through ``decide_declaration`` rather than
    ``decide_clause``, a different pop site, so it is worth pinning separately.
    """
    rows, _ = decidb_cli.execute(_outer(_INNER_SPLIT_ORDER, trailing_when=True))
    assert len(rows) == 2
    expected = _oracle_objective(oracle_solver, trailing_when=True)
    assert abs(_objective(rows) - expected) < 1e-6, \
        f"DeciDB={_objective(rows)}, Oracle={expected}"


@pytest.mark.correctness
@pytest.mark.sql_subquery
def test_nested_decide_without_outer_when(decidb_cli, oracle_solver, perf_tracker):
    """Nesting with no trailing ``WHEN`` worked before the fix and still does."""
    rows, _ = decidb_cli.execute(_outer(_INNER, trailing_when=False))
    assert len(rows) == 2
    expected = _oracle_objective(oracle_solver, trailing_when=False)
    assert abs(_objective(rows) - expected) < 1e-6, \
        f"DeciDB={_objective(rows)}, Oracle={expected}"


@pytest.mark.sql_subquery
def test_case_when_after_a_nested_clause_lexes_as_sql(decidb_cli, perf_tracker):
    """Restoring the outer state must not leave ``WHEN`` armed once the whole clause
    closes: an ordinary ``CASE WHEN`` in ``ORDER BY`` still has to parse."""
    sql = f"""
        SELECT id, x, CASE WHEN id = 1 THEN 'first' ELSE 'rest' END AS label
        FROM (VALUES (1, 'a'), (2, 'b')) t(id, grp)
        DECIDE x(INT)
        SUCH THAT x >= 0 AND x <= 9
            AND SUM(x) <= ({_INNER})
            AND when grp = 'a': sum(x) <= 5
        MAXIMIZE SUM(x)
        ORDER BY CASE WHEN id = 1 THEN 0 ELSE 1 END
    """
    rows, cols = decidb_cli.execute(sql)
    assert len(rows) == 2
    assert [row[2] for row in rows] == ["first", "rest"]


@pytest.mark.error_parser
@pytest.mark.error
def test_a_real_duplicate_decide_is_still_rejected(decidb_cli):
    """The "DECIDE appears twice" guard reads a flag a nested clause also sets, so
    it is now saved and restored with the rest of the state.

    The false-positive half is covered by the split-order test above, which would
    raise this error if an inner declaration leaked. This pins the other half: a
    genuine duplicate is still caught.
    """
    decidb_cli.assert_error(
        """
            SELECT id, x
            DECIDE x(INT)
            FROM (VALUES (1, 'a'), (2, 'b')) t(id, grp)
            DECIDE x(INT)
            SUCH THAT x >= 0 AND x <= 9
            MAXIMIZE SUM(x)
        """,
        match=r"(?i)DECIDE appears twice",
    )


# --- Subqueries written *inside* a nested DECIDE ---------------------------
#
# A second regression, with a different cause. ``PlanSubqueries`` normally defers
# a subquery it meets while an outer subquery is still being flattened; the
# deferred work is finished later by ``RecursiveDependentJoinPlanner``. Ordinary
# clauses tolerate that because nothing reads their expressions in between. A
# DECIDE clause does not: canonicalization runs in the same ``CreatePlan`` and
# copies the constraint tree, and a still-unplanned subquery cannot be copied.
# Every query below therefore used to fail with
# ``Serialization Error: Cannot copy BoundSubqueryExpression``.
#
# See ``pipeline/03_logical_plan/done.md`` → DECIDE subquery flattening.

_TWO_ROWS = "(VALUES (1), (2)) u(uid)"

# Innermost of three DECIDE levels: two integers capped at 4 → 8.
_LEVEL_3 = (
    f"SELECT SUM(z) FROM {_TWO_ROWS} "
    "DECIDE z(INT) SUCH THAT z >= 0 AND z <= 4 MAXIMIZE SUM(z)"
)
# Middle level: budgeted by level 3, then reduced in its own SELECT list so each
# level reports a different number and a mix-up cannot pass unnoticed.
_LEVEL_2 = (
    f"SELECT SUM(y) - 3 FROM {_TWO_ROWS} "
    f"DECIDE y(INT) SUCH THAT y >= 0 AND SUM(y) <= ({_LEVEL_3}) MAXIMIZE SUM(y)"
)


def _oracle_chain(oracle_solver, levels) -> float:
    """Solve a chain of two-variable maximizations independently, outermost last.

    ``levels`` is one ``(upper_bound, cap, adjust)`` triple per level from the
    inside out. Each level maximizes the sum of two integers bounded by
    ``upper_bound``; ``cap`` is ``None`` for no sum constraint, the string
    ``"budget"`` to reuse the previous level's result, or an explicit number. The
    level then passes ``result + adjust`` outward, mirroring what its SELECT list
    reports.
    """
    carried = None
    for level, (ub, cap, adjust) in enumerate(levels):
        oracle_solver.create_model(f"chain_{level}")
        obj = {}
        for i in (1, 2):
            oracle_solver.add_variable(f"v{level}_{i}", VarType.INTEGER, lb=0.0, ub=ub)
            obj[f"v{level}_{i}"] = 1.0
        if cap is not None:
            bound = carried if cap == "budget" else cap
            oracle_solver.add_constraint(obj, "<=", bound, name=f"cap_{level}")
        oracle_solver.set_objective(obj, ObjSense.MAXIMIZE)
        carried = oracle_solver.solve().objective_value + adjust
    return carried


@pytest.mark.correctness
@pytest.mark.sql_subquery
def test_three_levels_of_decide_nesting(decidb_cli, oracle_solver, perf_tracker):
    """A DECIDE subquery inside a DECIDE subquery. Two levels always planned; the
    third is the level whose subquery was still unplanned at canonicalization."""
    rows, _ = decidb_cli.execute(_outer(_LEVEL_2, trailing_when=False))
    assert len(rows) == 2
    # Level 3 caps at 4 each (8), level 2 carries 8 - 3 = 5, level 1 is budgeted
    # by that with a slack cap of 9 each.
    expected = _oracle_chain(
        oracle_solver,
        [(4.0, None, -3.0), (1e6, "budget", 0.0), (9.0, "budget", 0.0)],
    )
    assert abs(_objective(rows) - expected) < 1e-6, \
        f"DeciDB={_objective(rows)}, Oracle={expected}"


@pytest.mark.correctness
@pytest.mark.sql_subquery
def test_plain_subquery_inside_a_nested_decide_constraint(decidb_cli, oracle_solver, perf_tracker):
    """The same failure without a third DECIDE: an ordinary scalar subquery used as
    a bound inside the *inner* clause is enough to trigger it."""
    inner = (
        f"SELECT SUM(y) FROM {_TWO_ROWS} DECIDE y(INT) SUCH THAT y >= 0 "
        "AND SUM(y) <= (SELECT MAX(cap) FROM (VALUES (7)) c(cap)) MAXIMIZE SUM(y)"
    )
    rows, _ = decidb_cli.execute(_outer(inner, trailing_when=False))
    assert len(rows) == 2
    expected = _oracle_chain(oracle_solver, [(1e6, 7.0, 0.0), (9.0, "budget", 0.0)])
    assert abs(_objective(rows) - expected) < 1e-6, \
        f"DeciDB={_objective(rows)}, Oracle={expected}"


@pytest.mark.correctness
@pytest.mark.sql_subquery
def test_plain_subquery_inside_a_nested_decide_objective(decidb_cli, oracle_solver, perf_tracker):
    """The objective passes through the same canonicalization boundary, so a
    subquery scaling the inner objective has to be flattened there too."""
    inner = (
        f"SELECT SUM(y) FROM {_TWO_ROWS} DECIDE y(INT) SUCH THAT y >= 0 AND y <= 3 "
        "MAXIMIZE SUM(y) * (SELECT MAX(k) FROM (VALUES (2)) s(k))"
    )
    rows, _ = decidb_cli.execute(_outer(inner, trailing_when=False))
    assert len(rows) == 2
    # Scaling a maximization by a positive constant does not move its argmax, so
    # the inner clause still reports 6 and budgets the outer one at 6.
    expected = _oracle_chain(oracle_solver, [(3.0, None, 0.0), (9.0, "budget", 0.0)])
    assert abs(_objective(rows) - expected) < 1e-6, \
        f"DeciDB={_objective(rows)}, Oracle={expected}"


# A refusal of the inner clause used to reach the user as the outer query's error: DuckDB
# retries a failed expression in each outer query, in case it refers to one, and reported
# the retry's *"Referenced column "y" not found"*. Only a missing name is retried now.
@pytest.mark.error_binder
@pytest.mark.error
@pytest.mark.parametrize(
    "inner_clauses,message",
    [
        ("SUCH THAT y <= cap OR y <= 1 MAXIMIZE SUM(y)", "SUCH THAT: OR is not supported"),
        ("SUCH THAT y <= cap MAXIMIZE y > 1", r"MAXIMIZE does not support '\(y > 1\)'"),
    ],
    ids=["constraint", "objective"],
)
def test_a_nested_refusal_reaches_the_user(decidb_cli, inner_clauses, message):
    table = "(VALUES (1, 3), (2, 5)) t(id, cap)"
    decidb_cli.assert_error(
        f"SELECT id, x FROM {table} DECIDE x(INT) SUCH THAT x <= "
        f"(SELECT max(y) FROM (SELECT y FROM {table} DECIDE y(INT) {inner_clauses})) MAXIMIZE SUM(x)",
        match=message,
    )


# A DECIDE solves one problem, so one that reads a column of an outer query (to be
# re-solved per outer row) is refused at bind time. It used to fail with an internal
# error. Each case reaches the outer column a different way: from WHERE, which binds as
# ordinary SQL; from a clause by its bare name, which DuckDB retries as a missing column;
# and qualified, which it retries as a missing table.
@pytest.mark.error_binder
@pytest.mark.error
@pytest.mark.parametrize(
    "inner,name",
    [
        ("FROM t WHERE t.k = g.k DECIDE x(INT) SUCH THAT x <= 1 MAXIMIZE SUM(x)", "k"),
        ("FROM t DECIDE x(INT) SUCH THAT x <= lim MAXIMIZE SUM(x)", "lim"),
        ("FROM t DECIDE x(INT) SUCH THAT x <= 1 AND SUM(x) <= g.lim MAXIMIZE SUM(x)", "lim"),
    ],
    ids=["where", "clause column", "clause table"],
)
def test_a_decide_cannot_read_an_outer_query(decidb_cli, inner, name):
    decidb_cli.assert_error(
        "SELECT g.k, q.x FROM (VALUES (1, 2)) g(k, lim), "
        f"LATERAL ({inner.replace('FROM t', 'FROM (VALUES (1), (2)) t(k)')}) q",
        match=f"DECIDE: '{name}' refers to an outer query, which a DECIDE cannot read; to solve per group",
    )


# A DECIDE in another DECIDE's FROM (or WITH) is a two-stage pipeline: the inner problem
# solves once, and its answer, decisions included, is data for the outer one. The inner
# optimum is unique, so the oracle can hand its values to the second stage.
_STAGE_ROWS = [(1, 3), (2, 5), (3, 7)]
_STAGE_TABLE = "(VALUES (1, 3), (2, 5), (3, 7)) t(id, cap)"
_STAGE_1 = f"SELECT id, y FROM {_STAGE_TABLE} DECIDE y(INT) SUCH THAT y <= cap AND SUM(y) <= 8 MAXIMIZE SUM(y * cap)"


@pytest.mark.correctness
@pytest.mark.sql_subquery
@pytest.mark.parametrize(
    "sql",
    [
        f"SELECT id, x FROM ({_STAGE_1}) s DECIDE x(INT) SUCH THAT x <= y + 1 AND SUM(x) <= 6 MAXIMIZE SUM(x * id)",
        f"WITH s AS ({_STAGE_1}) "
        "SELECT id, x FROM s DECIDE x(INT) SUCH THAT x <= y + 1 AND SUM(x) <= 6 MAXIMIZE SUM(x * id)",
    ],
    ids=["from", "with"],
)
def test_a_decide_over_a_decide_in_from(decidb_cli, oracle_solver, sql):
    oracle_solver.create_model("stage_1")
    for i, cap in _STAGE_ROWS:
        oracle_solver.add_variable(f"y{i}", VarType.INTEGER, lb=0.0, ub=float(cap))
    oracle_solver.add_constraint({f"y{i}": 1.0 for i, _ in _STAGE_ROWS}, "<=", 8.0, name="budget")
    oracle_solver.set_objective({f"y{i}": float(cap) for i, cap in _STAGE_ROWS}, ObjSense.MAXIMIZE)
    stage_1 = oracle_solver.solve().variable_values

    oracle_solver.create_model("stage_2")
    for i, _ in _STAGE_ROWS:
        oracle_solver.add_variable(f"x{i}", VarType.INTEGER, lb=0.0, ub=round(stage_1[f"y{i}"]) + 1.0)
    oracle_solver.add_constraint({f"x{i}": 1.0 for i, _ in _STAGE_ROWS}, "<=", 6.0, name="budget")
    oracle_solver.set_objective({f"x{i}": float(i) for i, _ in _STAGE_ROWS}, ObjSense.MAXIMIZE)
    expected = oracle_solver.solve().objective_value

    rows, cols = decidb_cli.execute(sql)
    ci = {c: i for i, c in enumerate(cols)}
    assert sum(int(r[ci["x"]]) * int(r[ci["id"]]) for r in rows) == pytest.approx(expected)
