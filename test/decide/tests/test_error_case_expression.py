"""CASE-expression rejection inside DECIDE.

DeciDB does not support SQL CASE expressions inside DECIDE constraints or
objectives. Historically `CASE WHEN ... THEN ... ELSE ... END` fell through
the symbolic translator's default arm and surfaced as an InternalException
with a C++ stack trace — making the rejection look like a decidb bug rather
than an unsupported feature.

These tests pin down the friendly-error contract:
  - The user sees a single-line, actionable error pointing to `when` / `per` /
    CTE alternatives.
  - The error never includes a stack trace, "INTERNAL Error", or any
    indication of an assertion failure.

The tests exercise every surface where CASE can syntactically appear inside
DECIDE so a future refactor that touches the binder, symbolic layer, or any
aggregate-specific code path will trip if it bypasses the new arm.
"""

import pytest

from ._output_helpers import assert_no_internal_leak


@pytest.mark.error_binder
@pytest.mark.error
class TestCaseExpressionRejection:
    """SQL CASE inside DECIDE should produce a friendly error, never a crash."""

    _FRIENDLY = r"(?i)CASE expressions are not supported"

    def test_case_in_such_that_is_rejected(self, decidb_cli):
        """Searched CASE inside a SUCH THAT constraint."""
        sql = """
            SELECT s_suppkey, x FROM supplier WHERE s_suppkey < 10
            DECIDE x(BOOL)
            SUCH THAT SUM(x * CASE WHEN s_nationkey = 1 THEN 1 ELSE 0 END) >= 1
            MAXIMIZE SUM(x * s_acctbal)
        """
        decidb_cli.assert_error(sql, match=self._FRIENDLY)
        # The friendly message should point users at the supported alternatives.
        result = decidb_cli.execute_raw(sql)
        combined = result.stderr + result.stdout
        assert "when " in combined and " per" in combined, (
            "Friendly error must name the when and per scopes as alternatives; " f"got: {combined[:600]}"
        )

    def test_case_in_maximize_is_rejected(self, decidb_cli):
        """Searched CASE inside a MAXIMIZE objective."""
        sql = """
            SELECT s_suppkey, x FROM supplier WHERE s_suppkey < 10
            DECIDE x(BOOL)
            SUCH THAT SUM(x) <= 3
            MAXIMIZE SUM(x * CASE WHEN s_nationkey = 1 THEN 100 ELSE 1 END)
        """
        decidb_cli.assert_error(sql, match=self._FRIENDLY)

    def test_case_in_minimize_is_rejected(self, decidb_cli):
        """Searched CASE inside a MINIMIZE objective."""
        sql = """
            SELECT s_suppkey, x FROM supplier WHERE s_suppkey < 10
            DECIDE x(BOOL)
            SUCH THAT SUM(x) >= 3
            MINIMIZE SUM(x * CASE WHEN s_nationkey = 1 THEN 100 ELSE 1 END)
        """
        decidb_cli.assert_error(sql, match=self._FRIENDLY)

    def test_simple_case_form_is_rejected(self, decidb_cli):
        """`CASE expr WHEN val THEN ... END` (simple form) also rejected.

        The simple-form CASE binds to the same ExpressionClass as the
        searched-form CASE, so both must hit the new arm; the test guards
        against a future refactor that special-cases one form.
        """
        sql = """
            SELECT s_suppkey, x FROM supplier WHERE s_suppkey < 10
            DECIDE x(BOOL)
            SUCH THAT SUM(x * CASE s_nationkey WHEN 1 THEN 1 ELSE 0 END) >= 1
            MAXIMIZE SUM(x * s_acctbal)
        """
        decidb_cli.assert_error(sql, match=self._FRIENDLY)

    def test_case_inside_sum_aggregate_is_rejected(self, decidb_cli):
        """CASE directly inside SUM (no outer multiplication)."""
        sql = """
            SELECT s_suppkey, x FROM supplier WHERE s_suppkey < 10
            DECIDE x(BOOL)
            SUCH THAT SUM(CASE WHEN s_nationkey = 1 THEN x ELSE 0 END) >= 1
            MAXIMIZE SUM(x * s_acctbal)
        """
        decidb_cli.assert_error(sql, match=self._FRIENDLY)

    @pytest.mark.parametrize("agg", ["AVG", "MIN", "MAX"])
    def test_case_inside_avg_min_max_is_rejected(self, decidb_cli, agg):
        """CASE inside AVG/MIN/MAX aggregates is rejected, same as SUM.

        Each aggregate has its own optimizer rewriting path (AVG → SUM,
        MIN/MAX → Big-M or aux-var linearization). The CASE rejection must
        fire before any of those rewrites, so a future change that adds
        early aggregate-specific handling cannot accidentally bypass it.
        """
        sql = f"""
            SELECT s_suppkey, x FROM supplier WHERE s_suppkey < 10
            DECIDE x(BOOL)
            SUCH THAT SUM(x) <= 3
            MAXIMIZE {agg}(x * CASE WHEN s_nationkey = 1 THEN 1 ELSE 0 END)
        """
        decidb_cli.assert_error(sql, match=self._FRIENDLY)

    def test_case_in_constraint_does_not_emit_stack_trace(self, decidb_cli):
        """The specific UX symptom that motivated this fix: no C++ trace.

        Even if the rejection message regresses to a less helpful wording,
        the absence of a stack trace is the load-bearing user-experience
        contract — a future refactor that re-routes CASE through the
        InternalException path would re-introduce the original symptom.
        """
        sql = """
            SELECT s_suppkey, x FROM supplier WHERE s_suppkey < 10
            DECIDE x(BOOL)
            SUCH THAT SUM(x * CASE WHEN s_nationkey = 1 THEN 1 ELSE 0 END) >= 1
            MAXIMIZE SUM(x * s_acctbal)
        """
        assert_no_internal_leak(decidb_cli.execute_raw(sql), "CASE rejection")

    def test_case_inside_per_grouped_constraint_is_rejected(self, decidb_cli):
        """A `per`-scoped constraint with CASE inside is still rejected.

        A `per g:` clause with `by (g)` on its reducer binds through the scope
        path rather than as a plain constraint, so this case confirms the scope
        does not bypass the new arm by binding the inner expression differently.
        """
        sql = """
            SELECT s_suppkey, x FROM supplier WHERE s_suppkey < 10
            DECIDE x(BOOL)
            SUCH THAT per s_nationkey:
                     SUM(x * CASE WHEN s_nationkey = 1 THEN 1 ELSE 0 END) by (s_nationkey) <= 1
            MAXIMIZE SUM(x * s_acctbal)
        """
        decidb_cli.assert_error(sql, match=self._FRIENDLY)

    def test_case_outside_a_reducer_shares_the_friendly_message(self, decidb_cli):
        """A CASE written outside any reducer used to miss the friendly arm.

        A CASE sitting directly in the constraint once printed
        `(ExpressionClass::CASE)` — a C++ enum name a SQL user cannot act on.
        """
        sql = """
            SELECT s_suppkey, x FROM supplier WHERE s_suppkey < 10
            DECIDE x(INT)
            SUCH THAT x + CASE WHEN s_nationkey = 1 THEN 1 ELSE 2 END <= 5
            MAXIMIZE SUM(x)
        """
        decidb_cli.assert_error(sql, match=self._FRIENDLY)
        result = decidb_cli.execute_raw(sql)
        combined = result.stderr + result.stdout
        assert "ExpressionClass" not in combined, (
            f"Internal enum name leaked to the user: {combined[:600]}"
        )

    def test_case_outside_a_reducer_in_the_objective(self, decidb_cli):
        """A CASE added to the objective's terms, outside any reducer."""
        sql = """
            SELECT s_suppkey, x FROM supplier WHERE s_suppkey < 10
            DECIDE x(BOOL)
            SUCH THAT SUM(x) <= 3
            MAXIMIZE SUM(x * s_acctbal) + CASE WHEN 1 = 1 THEN 1 ELSE 2 END
        """
        decidb_cli.assert_error(sql, match=self._FRIENDLY)
        result = decidb_cli.execute_raw(sql)
        combined = result.stderr + result.stdout
        assert "ExpressionClass" not in combined, (
            f"Internal enum name leaked to the user: {combined[:600]}"
        )


_CASE = "CASE WHEN s_nationkey = 1 THEN 1 ELSE 2 END"

# The full message, so a refusal that names CASE but drops the alternatives fails too.
_CASE_MESSAGE = (
    "CASE expressions are not supported inside DECIDE constraints or objectives. Write the condition as a "
    "scope in front of the clause (when category = 'A': sum(x) >= 1), split rows into classes with per, or "
    "pre-compute conditional values in a CTE or subquery before the DECIDE clause."
)


@pytest.mark.error_binder
@pytest.mark.error
class TestCaseRefusedEverywhere:
    """A CASE the clause's algebra would read gets the one message, wherever it stands (BUG-42)."""

    # (constraints, objective). Each position is a different way the CASE is reached: the bound of an aggregate
    # clause (which used to be refused by the bound allow-list with another text), then the containers it can sit in.
    _REFUSED = {
        "aggregate bound": (f"SUM(x) <= {_CASE}", "SUM(x)"),
        "aggregate bound, left side": (f"{_CASE} >= SUM(x)", "SUM(x)"),
        "aggregate bound, in arithmetic": (f"SUM(x) <= 1 + {_CASE}", "SUM(x)"),
        "aggregate bound, in a call": (f"SUM(x) <= abs({_CASE})", "SUM(x)"),
        "aggregate bound, in a cast": (f"SUM(x) <= CAST({_CASE} AS INT)", "SUM(x)"),
        "aggregate bound, in a lambda": (
            "SUM(x) <= list_reduce([1, 2], (a, b) -> CASE WHEN a > b THEN a ELSE b END)",
            "SUM(x)",
        ),
        "aggregate bound, in a reducer's FILTER": (f"SUM(x) <= SUM(s_suppkey) FILTER (WHERE {_CASE} = 1)", "SUM(x)"),
        "aggregate bound, in a BETWEEN": (f"SUM(x) BETWEEN 0 AND {_CASE}", "SUM(x)"),
        "aggregate bound, in a later conjunct": (f"SUM(x) <= 5 AND SUM(x) >= {_CASE}", "SUM(x)"),
        "decision reducer's FILTER": (f"SUM(x) FILTER (WHERE {_CASE} = 1) <= 3", "SUM(x)"),
        "per-row bound": (f"x <= {_CASE}", "SUM(x)"),
        "reducer argument": (f"SUM(x * {_CASE}) <= 3", "SUM(x)"),
        "bound of a clause with when": (f"when s_suppkey > 2: SUM(x) <= {_CASE}", "SUM(x)"),
        "reducer argument under by": (f"SUM(x * {_CASE}) by (s_nationkey) <= 3", "SUM(x)"),
        "objective term": ("SUM(x) <= 3", f"SUM(x) + {_CASE}"),
        "objective coefficient": ("SUM(x) <= 3", f"SUM(x * {_CASE})"),
        "objective under when": ("SUM(x) <= 3", f"when s_suppkey > 2: SUM(x) + {_CASE}"),
    }

    @pytest.mark.parametrize("position", list(_REFUSED))
    def test_case_is_refused_with_the_one_message(self, decidb_cli, position):
        constraint, objective = self._REFUSED[position]
        sql = f"""
            SELECT s_suppkey, x FROM supplier WHERE s_suppkey < 10
            DECIDE x(INT)
            SUCH THAT {constraint}
            MAXIMIZE {objective}
        """
        result = decidb_cli.execute_raw(sql)
        assert _CASE_MESSAGE in result.stderr + result.stdout, f"{position}: {result.stderr[:600]}"

    # Positions that hold SQL over the rows, not the clause's algebra: a CASE there is the user's own filter or key.
    _ACCEPTED = {
        "when condition": f"when {_CASE} = 1: SUM(x) <= 3",
        "by key": f"SUM(x) by ({_CASE}) <= 3",
        "scalar subquery bound": f"SUM(x) <= (SELECT {_CASE} FROM supplier WHERE s_suppkey = 1)",
    }

    @pytest.mark.parametrize("position", list(_ACCEPTED))
    def test_case_in_sql_positions_is_still_accepted(self, decidb_cli, position):
        sql = f"""
            SELECT s_suppkey, x FROM supplier WHERE s_suppkey < 10
            DECIDE x(BOOL)
            SUCH THAT {self._ACCEPTED[position]}
            MAXIMIZE SUM(x * s_acctbal)
        """
        rows, _ = decidb_cli.execute(sql)
        assert rows, position
