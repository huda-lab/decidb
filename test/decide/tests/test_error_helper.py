"""``assert_error_at`` asserts an error's kind and the place it quotes, and nothing else about the message."""

import pytest

# A parser error with a caret under the quoted query line.
CARET_SQL = "CREATE TABLE t(a INT); SELECT a FROM t DECIDE x IS INTEGER SUCH THAT x <= 1;"
# A binder error with a caret, whose first line also quotes a name.
BINDER_SQL = "SELECT zz FROM (SELECT 1 a);"
# A query line long enough that the CLI clips it around the caret.
LONG_SQL = "SELECT " + ", ".join(f"{i}" for i in range(80)) + ", zz FROM (SELECT 1 a);"
# A catalog error that quotes a name and has no caret.
QUOTED_SQL = "CREATE TEMP TABLE t(a INT); CREATE TEMP TABLE t(a INT);"
# An error that neither has a caret nor quotes anything.
BARE_SQL = "SELECT error('boom')"


def test_error_helper_passes_on_kind_and_caret_location(decidb_cli):
    decidb_cli.assert_error_at(CARET_SQL, kind="Parser Error", at="IS INTEGER")
    decidb_cli.assert_error_at(BINDER_SQL, kind="Binder Error", at="zz")


def test_error_helper_reads_the_caret_on_a_clipped_line(decidb_cli):
    decidb_cli.assert_error_at(LONG_SQL, kind="Binder Error", at="zz")


def test_error_helper_passes_on_a_quoted_fragment_without_a_caret(decidb_cli):
    decidb_cli.assert_error_at(QUOTED_SQL, kind="Catalog Error", at="t")


def test_error_helper_passes_on_an_error_that_points_nowhere(decidb_cli):
    decidb_cli.assert_error_at(BARE_SQL, kind="Invalid Input Error")


def test_error_helper_fails_when_the_query_succeeds(decidb_cli):
    with pytest.raises(AssertionError, match="Expected an error"):
        decidb_cli.assert_error_at("SELECT 1", kind="Binder Error")


def test_error_helper_fails_on_a_wrong_kind(decidb_cli):
    with pytest.raises(AssertionError, match="Expected a Parser Error"):
        decidb_cli.assert_error_at(BINDER_SQL, kind="Parser Error", at="zz")


def test_error_helper_fails_on_a_wrong_caret_location(decidb_cli):
    with pytest.raises(AssertionError, match="not inside"):
        decidb_cli.assert_error_at(BINDER_SQL, kind="Binder Error", at="FROM")


def test_error_helper_fails_on_a_wrong_quoted_location(decidb_cli):
    with pytest.raises(AssertionError, match="neither points"):
        decidb_cli.assert_error_at(QUOTED_SQL, kind="Catalog Error", at="u")


def test_error_helper_fails_on_a_missing_location_with_a_caret(decidb_cli):
    with pytest.raises(AssertionError, match="points into the query"):
        decidb_cli.assert_error_at(BINDER_SQL, kind="Binder Error")


def test_error_helper_fails_on_a_missing_location_with_a_quote(decidb_cli):
    with pytest.raises(AssertionError, match="quotes"):
        decidb_cli.assert_error_at(QUOTED_SQL, kind="Catalog Error")


def test_error_helper_fails_on_a_location_the_error_does_not_give(decidb_cli):
    with pytest.raises(AssertionError, match="neither points"):
        decidb_cli.assert_error_at(BARE_SQL, kind="Invalid Input Error", at="boom")


def test_error_helper_tells_two_errors_apart_by_one_word(decidb_cli):
    decidb_cli.assert_error_at(BINDER_SQL, kind="Binder Error", at="zz", word="Referenced")
    with pytest.raises(AssertionError, match="does not contain"):
        decidb_cli.assert_error_at(BINDER_SQL, kind="Binder Error", at="zz", word="Ambiguous")
