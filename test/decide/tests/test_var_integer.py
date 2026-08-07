"""Tests for the omitted-type DECIDE declaration.

A DECIDE declaration must carry its type in parentheses — ``x(INT)``. The bare
``DECIDE x`` form, which used to default to INTEGER, is rejected.

Covers:
  - simple_test: a declaration with no type is a parser error naming the fix

Positive ``x(INT)`` coverage lives in the constraint/objective suites
(``test_cons_comparison.py``, ``test_cons_in.py``, ``test_implied_bounds.py``,
and others), so it is not duplicated here.
"""

import pytest


@pytest.mark.var_integer
@pytest.mark.error_parser
@pytest.mark.error
def test_simple_test(decidb_cli):
    """Omitted type: ``DECIDE x`` is rejected and the message names the fix.

    This replaces the former default-integer oracle test. A bare ``DECIDE x``
    used to default to an integer variable; now the type is mandatory, so the
    behaviour worth pinning is the rejection and that it names the fix.
    """
    decidb_cli.assert_error("""
            SELECT x, l_orderkey FROM LINEITEM
            WHERE l_orderkey <= 5
            DECIDE x
            SUCH THAT SUM(x * l_extendedprice) <= 10000
            MAXIMIZE SUM(x * l_extendedprice)
        """, match=r'DECIDE variable "x" needs a type')


@pytest.mark.var_integer
@pytest.mark.error_parser
@pytest.mark.error
def test_qualified_missing_type(decidb_cli):
    """Omitted type on a table-scoped declaration is rejected the same way."""
    decidb_cli.assert_error("""
            SELECT x, l_orderkey FROM LINEITEM
            WHERE l_orderkey <= 5
            DECIDE lineitem.x
            SUCH THAT SUM(x * l_extendedprice) <= 10000
            MAXIMIZE SUM(x * l_extendedprice)
        """, match=r'DECIDE variable "lineitem\.x" needs a type')
