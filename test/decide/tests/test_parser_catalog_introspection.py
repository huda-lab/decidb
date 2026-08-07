"""Regression guard: the DECIDE grammar must not corrupt ordinary SQL parsing.

History: the aggregate-local WHEN feature was first implemented with a
`func_application WHEN ...` rule in the *global* `c_expr` non-terminal. That
polluted every function call — any unregistered/user-defined function name and
every DuckDB default catalog view (`duckdb_tables`, `information_schema.*`,
`pg_catalog.*`, ...) failed to parse with a bogus `syntax error at or near
"then"`. It went unnoticed because no test ever selected a non-DECIDE function
call or a catalog view. The fix confines the DECIDE WHEN to a context-sensitive
`WHEN_DECIDE` token (see context/descriptions/07_issues/bugs/done.md).

These tests pin the parser back to correct behavior: catalog introspection runs,
and an unregistered function call reaches the *binder* (Catalog error) rather
than dying at parse time. A future grammar change that re-pollutes global
parsing will trip these.
"""

import pytest

from decidb_cli import DecidBCliError


@pytest.mark.error_parser
@pytest.mark.error
class TestNoGlobalGrammarPollution:
    """The DECIDE grammar must stay confined; global SQL parsing is untouched."""

    @pytest.mark.parametrize(
        "view_sql",
        [
            "SELECT * FROM duckdb_tables()",
            "SELECT * FROM duckdb_views()",
            "SELECT * FROM information_schema.tables",
            "SELECT * FROM information_schema.columns",
            "SELECT * FROM pg_catalog.pg_class",
            "SELECT * FROM pg_catalog.pg_type",
            "SELECT * FROM sqlite_master",
        ],
    )
    def test_catalog_views_parse_and_run(self, decidb_cli, view_sql):
        """Catalog/default views materialize without a parse error."""
        # Should not raise: these views parse and execute. (LIMIT keeps it cheap.)
        decidb_cli.execute(f"{view_sql} LIMIT 5")

    @pytest.mark.parametrize(
        "call_sql",
        [
            "SELECT not_a_registered_function()",
            "SELECT my_udf(a, b) FROM (VALUES (1, 2)) v(a, b)",
            "SELECT another_unknown_fn(1) FROM (VALUES (1)) v(a)",
        ],
    )
    def test_unregistered_function_reaches_binder_not_parser(
        self, decidb_cli, call_sql
    ):
        """An unknown function name must PARSE (then fail at bind), not error at
        parse time. The bug produced a `syntax error`; the correct behavior is a
        Catalog/binder error proving the call parsed."""
        with pytest.raises(DecidBCliError) as excinfo:
            decidb_cli.execute(call_sql)
        msg = str(excinfo.value).lower()
        assert "syntax error" not in msg, (
            f"Unregistered function call regressed to a parse error: {msg}"
        )

    def test_simple_case_with_function_operand_parses(self, decidb_cli):
        """`CASE func() WHEN val THEN ...` (simple CASE, function operand) — the
        exact shape that exposed the bug in DuckDB's catalog view bodies."""
        rows, _ = decidb_cli.execute(
            "SELECT CASE length('hi') WHEN 2 THEN 'a' ELSE 'b' END AS r"
        )
        assert rows == [("a",)]

    def test_case_when_after_decide_clause_still_parses(self, decidb_cli):
        """The DECIDE-context WHEN flag must be cleared at clause end so a CASE
        WHEN in a trailing GROUP BY / HAVING is not mis-tokenized."""
        # Should not raise a parse error (semantic errors are fine/irrelevant here).
        try:
            decidb_cli.execute(
                "SELECT l_orderkey FROM lineitem "
                "DECIDE x(BOOL) SUCH THAT SUM(x) <= 1 "
                "GROUP BY l_orderkey "
                "HAVING max(l_quantity) > CASE WHEN l_orderkey > 0 THEN 0 ELSE 1 END"
            )
        except DecidBCliError as e:
            assert "syntax error" not in str(e).lower(), (
                f"CASE WHEN after DECIDE clause regressed to a parse error: {e}"
            )
