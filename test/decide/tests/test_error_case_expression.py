"""CASE-expression rejection inside DECIDE.

DecidB does not support SQL CASE expressions inside DECIDE constraints or
objectives. Historically `CASE WHEN ... THEN ... ELSE ... END` fell through
the symbolic translator's default arm and surfaced as an InternalException
with a C++ stack trace — making the rejection look like a decidb bug rather
than an unsupported feature.

These tests pin down the friendly-error contract:
  - The user sees a single-line, actionable error pointing to WHEN / PER /
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
        assert "WHEN" in combined and "PER" in combined, (
            "Friendly error must mention both WHEN and PER as alternatives; "
            f"got: {combined[:600]}"
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
        """PER-wrapped constraint with CASE inside still rejected.

        PER constraints take a different code path (`BindPerConstraint`) than
        plain constraints, so this case confirms the PER wrapper does not
        bypass the new arm by binding the inner expression differently.
        """
        sql = """
            SELECT s_suppkey, x FROM supplier WHERE s_suppkey < 10
            DECIDE x(BOOL)
            SUCH THAT SUM(x * CASE WHEN s_nationkey = 1 THEN 1 ELSE 0 END) <= 1
                     PER s_nationkey
            MAXIMIZE SUM(x * s_acctbal)
        """
        decidb_cli.assert_error(sql, match=self._FRIENDLY)
