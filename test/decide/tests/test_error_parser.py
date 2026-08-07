"""Parser-level syntax error tests.

These mirror the error cases from test/decidb/test_parser.test, ensuring that
DECIDE syntax errors are caught at parse time with clear error messages.
"""

import pytest


@pytest.mark.error_parser
@pytest.mark.error
class TestParserErrors:
    """DECIDE parser should reject malformed syntax."""

    def test_missing_such_that(self, decidb_cli):
        """DECIDE x(INT) MAXIMIZE ... without SUCH THAT."""
        decidb_cli.assert_error("""
                SELECT l_quantity FROM lineitem
                DECIDE x(INT) MAXIMIZE SUM(x*l_quantity) LIMIT 1
            """, match=r"DECIDE requires a SUCH THAT clause")

    def test_missing_decide_variable(self, decidb_cli):
        """DECIDE without a variable name."""
        decidb_cli.assert_error("""
                SELECT l_quantity FROM lineitem
                DECIDE SUCH THAT x IS BINARY
                MAXIMIZE SUM(x*l_quantity) LIMIT 1
            """, match=r"syntax error.*SUCH")

    def test_missing_objective_expression(self, decidb_cli):
        """MAXIMIZE with no expression before LIMIT."""
        decidb_cli.assert_error("""
                SELECT l_quantity FROM lineitem
                DECIDE x(INT) SUCH THAT x IS BINARY
                MAXIMIZE LIMIT 1
            """, match=r"syntax error")

    def test_unknown_variable_type(self, decidb_cli):
        """CONTINUOUS is not a recognized variable type (REAL is the spelling)."""
        decidb_cli.assert_error("""
                SELECT l_quantity FROM lineitem
                DECIDE x(CONTINUOUS) SUCH THAT x <= 1
                MAXIMIZE SUM(x*l_quantity) LIMIT 1
            """, match=r"syntax error.*CONTINUOUS")

    def test_comma_separated_constraints_rejected(self, decidb_cli):
        """Top-level SUCH THAT constraints must be separated with AND."""
        decidb_cli.assert_error("""
                SELECT l_quantity FROM lineitem
                DECIDE x(BOOL)
                SUCH THAT x <= 1, SUM(x) <= 10
                MAXIMIZE SUM(x) LIMIT 1
            """, match=r"(?i)comma-separated SUCH THAT constraints are not supported")
