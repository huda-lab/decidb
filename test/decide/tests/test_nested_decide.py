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

See ``01_pipeline/01_parser/done.md`` → DECIDE/WHEN tokenization.

Covers:
  - Nested clause followed by an outer ``WHEN``, oracle verified
  - The same with the inner clause written in the paper's split order
  - Nesting with no outer ``WHEN`` still behaves as before
  - Ordinary ``CASE WHEN`` after a nested clause still lexes as SQL
  - The duplicate-DECIDE guard is not tripped by a nested declaration
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
    when = "\n    AND SUM(x) <= 5 WHEN grp = 'a'" if trailing_when else ""
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
        f"DecidB={_objective(rows)}, Oracle={expected}"


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
        f"DecidB={_objective(rows)}, Oracle={expected}"


@pytest.mark.correctness
@pytest.mark.sql_subquery
def test_nested_decide_without_outer_when(decidb_cli, oracle_solver, perf_tracker):
    """Nesting with no trailing ``WHEN`` worked before the fix and still does."""
    rows, _ = decidb_cli.execute(_outer(_INNER, trailing_when=False))
    assert len(rows) == 2
    expected = _oracle_objective(oracle_solver, trailing_when=False)
    assert abs(_objective(rows) - expected) < 1e-6, \
        f"DecidB={_objective(rows)}, Oracle={expected}"


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
            AND SUM(x) <= 5 WHEN grp = 'a'
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
