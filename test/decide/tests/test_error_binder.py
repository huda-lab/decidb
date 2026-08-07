"""Binder-level semantic error tests.

These mirror the error cases from test/decidb/test_binder.test, updated to
match current DecidB behavior.  Some older error cases (SUM(x+x), strict <)
now succeed and are omitted.
"""

import pytest


@pytest.mark.error_binder
@pytest.mark.error
class TestBinderErrors:
    """DECIDE binder should reject semantically invalid queries."""

    def test_variable_conflicts_with_column(self, decidb_cli):
        """DECIDE variable name clashes with an existing column."""
        decidb_cli.assert_error("""
                SELECT l_quantity FROM lineitem
                DECIDE l_quantity(INT), x(INT)
                SUCH THAT x IN (1,2,3)
                MAXIMIZE SUM(x*l_quantity) LIMIT 1
            """, match=r"conflicts with an existing column")

    def test_duplicate_decide_variables(self, decidb_cli):
        """Same variable name declared twice."""
        decidb_cli.assert_error("""
                SELECT l_quantity FROM lineitem
                DECIDE x(INT), x(INT)
                SUCH THAT x IN (1,2,3)
                MAXIMIZE SUM(x*l_quantity) LIMIT 1
            """, match=r"Duplicate DECIDE variable")

    def test_unknown_variable_in_constraint(self, decidb_cli):
        """Using an undeclared variable in IN expression."""
        decidb_cli.assert_error("""
                SELECT l_quantity FROM lineitem
                DECIDE y(INT)
                SUCH THAT x IN (1,2,3)
                MAXIMIZE SUM(x) LIMIT 1
            """, match=r"does not support IN on")

    def test_is_null_unsupported(self, decidb_cli):
        """IS NULL is not supported in SUCH THAT."""
        decidb_cli.assert_error("""
                SELECT l_quantity FROM lineitem
                DECIDE x(INT)
                SUCH THAT x IS NULL
                MAXIMIZE SUM(x*l_quantity) LIMIT 1
            """, match=r"does not support")

    @pytest.mark.parametrize("agg", ["avg", "min", "max", "sum"])
    def test_data_only_aggregate_coefficient_rejected(self, decidb_cli, agg):
        """A data-only aggregate (`AVG(p)`, `MIN(p)`, … over columns only) used as a
        coefficient on a decision variable is rejected before symbolic normalization.

        Regression: `SUM(avg(p) * x)` used to slip through — the SymEngine round-trip
        distributed the variable into the aggregate (`avg(p)*x` -> `avg(p*x)`), a
        different constraint, and returned a silently wrong optimum. `sum` was already
        rejected (with a cryptic nested-SUM message); now all four reject uniformly.
        """
        decidb_cli.assert_error(
            f"""
                SELECT id, p, x FROM (VALUES (1,10.0),(2,20.0)) t(id, p)
                DECIDE x(BOOL)
                SUCH THAT SUM({agg}(p) * x) <= 3
                MAXIMIZE SUM(x)
            """,
            match=r"aggregate over table columns .* cannot multiply a decision variable",
        )

    def test_data_only_aggregate_coefficient_rejected_in_objective(self, decidb_cli):
        """The same rejection applies in an objective (`ValidateDecideNoNonLinearScalar`
        runs on both constraints and objectives), and the message stays context-neutral
        (no 'move it to the RHS' — an objective has no RHS)."""
        decidb_cli.assert_error("""
                SELECT id, p, x FROM (VALUES (1,10.0),(2,20.0)) t(id, p)
                DECIDE x(BOOL)
                SUCH THAT SUM(x) <= 1
                MAXIMIZE SUM(avg(p) * x)
            """, match=r"aggregate over table columns .* cannot multiply a decision variable")

    def test_data_only_aggregate_rhs_still_allowed(self, decidb_cli):
        """Guard against over-rejecting: a data-only aggregate is fine as a constraint
        RHS (`SUM(x) <= AVG(p)`), and a genuine DECIDE aggregate `AVG(x * p)` is fine —
        only using the data aggregate as a *coefficient* is rejected."""
        rows, _ = decidb_cli.execute("""
            SELECT id, p, x FROM (VALUES (1,10.0),(2,20.0)) t(id, p)
            DECIDE x(BOOL)
            SUCH THAT SUM(x) <= avg(p)
            MAXIMIZE SUM(x)
        """)
        assert rows  # solved, not rejected
        rows2, _ = decidb_cli.execute("""
            SELECT id, p, x FROM (VALUES (1,10.0),(2,20.0)) t(id, p)
            DECIDE x(BOOL)
            SUCH THAT AVG(x * p) >= 5
            MAXIMIZE SUM(x)
        """)
        assert rows2

    def test_sum_with_in_not_allowed(self, decidb_cli):
        """SUM(x) IN (...) is not a valid constraint form.

        Tightened match anchors to the aggregate-specific wording so a
        regression in error-message quality (e.g. falling back to a generic
        "not supported" path) would fail the test.
        """
        decidb_cli.assert_error("""
                SELECT l_quantity FROM lineitem
                DECIDE x(INT)
                SUCH THAT SUM(x) IN (1,2,3)
                MAXIMIZE SUM(x*l_quantity) LIMIT 1
            """, match=r"does not support IN on.*Only simple DECIDE variables are allowed as the IN target")

    def test_non_decide_variable_in_constraint(self, decidb_cli):
        """Using a regular column (not a DECIDE var) as a constrained value."""
        decidb_cli.assert_error("""
                SELECT l_quantity FROM lineitem
                DECIDE x(INT)
                SUCH THAT l_quantity <= 5
                MAXIMIZE SUM(x*l_quantity) LIMIT 1
            """, match=r"must be one of the DECIDE variables")

    def test_non_sum_avg_min_max_function_in_objective(self, decidb_cli):
        """STDDEV(x) is not supported in objective — only SUM, AVG, MIN, or MAX is allowed."""
        decidb_cli.assert_error("""
                SELECT l_quantity FROM lineitem
                DECIDE x(INT)
                SUCH THAT SUM(x) <= 5
                MAXIMIZE STDDEV(x*l_quantity) LIMIT 1
            """, match=r"only SUM, AVG, MIN, or MAX is allowed")

    def test_no_decide_variable_in_sum(self, decidb_cli):
        """SUM over a regular column without DECIDE variable."""
        decidb_cli.assert_error("""
                SELECT l_quantity FROM lineitem
                DECIDE x(INT)
                SUCH THAT SUM(l_quantity) <= 5
                MAXIMIZE SUM(x*l_quantity) LIMIT 1
            """, match=r"must reference at least one DECIDE variable")

    def test_multiple_decide_variables_in_sum(self, decidb_cli):
        """Cubic (x*x*x) in SUM is not supported — must reject."""
        decidb_cli.assert_error("""
                SELECT l_quantity FROM lineitem
                DECIDE x(INT)
                SUCH THAT SUM(x*x*x) <= 5
                MAXIMIZE SUM(x*l_quantity) LIMIT 1
            """, match=r"Triple.*products.*not supported")

    def test_between_non_scalar(self, decidb_cli):
        """SUM BETWEEN with a non-scalar bound.

        BETWEEN desugars into paired `<=` / `>=` constraints; the non-scalar
        check fires on the leg that references the column. Tightened match
        confirms it's the aggregate-RHS scalar check, not some unrelated
        "not a scalar" path elsewhere in the binder.
        """
        decidb_cli.assert_error("""
                SELECT l_quantity FROM lineitem
                DECIDE x(INT)
                SUCH THAT SUM(x) BETWEEN l_quantity AND 1
                MAXIMIZE SUM(x*l_quantity) LIMIT 1
            """, match=r"SUM cannot be compared to an expression that is not a scalar or aggregate without DECIDE variables")

    def test_decide_between_decide_variable(self, decidb_cli):
        """Multi-variable per-row constraints are now supported (e.g. x BETWEEN y AND 1).
        This is needed for ABS linearization which generates constraints like d >= x - c."""
        # This used to be an error; now valid (x >= y AND x <= 1)
        result, cols = decidb_cli.execute("""
                SELECT l_quantity FROM lineitem
                DECIDE x(INT), y(INT)
                SUCH THAT x BETWEEN y AND 1
                MAXIMIZE SUM(x*l_quantity) LIMIT 1
            """)
        assert len(result) > 0

    def test_in_rhs_with_decide_variable(self, decidb_cli):
        """IN domain constraints on DECIDE variables are not yet supported."""
        decidb_cli.assert_error("""
                SELECT l_quantity FROM lineitem
                DECIDE x(INT)
                SUCH THAT x IN (1,2,x)
                MAXIMIZE SUM(x*l_quantity) LIMIT 1
            """, match=r"IN domain constraints on DECIDE variables are not yet supported")

    def test_sum_rhs_non_scalar(self, decidb_cli):
        """SUM comparison RHS must be a scalar."""
        decidb_cli.assert_error("""
                SELECT l_quantity FROM lineitem
                DECIDE x(INT)
                SUCH THAT SUM(x) <= l_quantity
                MAXIMIZE SUM(x*l_quantity) LIMIT 1
            """, match=r"not a scalar")

    def test_decide_variable_rhs_with_decide(self, decidb_cli):
        """Multi-variable per-row constraints are now supported (e.g. x <= y).
        This is needed for ABS linearization which generates constraints like d >= x - c."""
        # This used to be an error; now valid (x - y <= 0)
        # Use small dataset + SUM bound (otherwise MAXIMIZE is unbounded)
        result, cols = decidb_cli.execute("""
                SELECT l_quantity FROM lineitem
                WHERE l_orderkey <= 3
                DECIDE x(INT), y(INT)
                SUCH THAT x <= y AND SUM(y) <= 100
                MAXIMIZE SUM(x*l_quantity)
            """)
        assert len(result) > 0

    def test_sum_equal_non_scalar(self, decidb_cli):
        """SUM = non-scalar expression."""
        decidb_cli.assert_error("""
                SELECT l_quantity FROM lineitem
                DECIDE x(INT)
                SUCH THAT SUM(x) = l_quantity
                MAXIMIZE SUM(x) LIMIT 1
            """, match=r"not a scalar")

    def test_objective_with_addition_succeeds(self, decidb_cli):
        """`MAXIMIZE SUM(...) + 3` is now supported: the constant offset is
        peeled from the objective body (it doesn't affect argmax) and
        preserved on LogicalDecide.objective_constant_offset for any caller
        that later reports the objective value. Previously rejected as a
        "non-aggregate term" by the extractor."""
        rows, cols = decidb_cli.execute("""
                SELECT l_quantity FROM lineitem
                DECIDE x(INT)
                SUCH THAT SUM(x) <= 3
                MAXIMIZE SUM(x*l_quantity)+3 LIMIT 1
            """)
        # Query runs; row count reflects the LIMIT clause.
        assert len(rows) > 0

    def test_objective_bare_column(self, decidb_cli):
        """MAXIMIZE with a bare column (not SUM) is not allowed."""
        decidb_cli.assert_error("""
                SELECT l_quantity FROM lineitem
                DECIDE x(INT)
                SUCH THAT SUM(x) <= 3
                MAXIMIZE l_quantity LIMIT 1
            """, match=r"does not support")

    def test_subquery_rhs_non_scalar(self, decidb_cli):
        """Subquery RHS that references DECIDE variables.

        DecidB surfaces this via the same "not a scalar or aggregate without
        DECIDE variables" binder path; the tightened match anchors to that
        wording so a regression (e.g. a less-informative generic scalar
        check) would fail.
        """
        decidb_cli.assert_error("""
                SELECT l_quantity FROM lineitem
                DECIDE x(INT)
                SUCH THAT SUM(x) <= (SELECT l_quantity+x FROM lineitem)
                MAXIMIZE SUM(x) LIMIT 1
            """, match=r"SUM cannot be compared to an expression that is not a scalar or aggregate without DECIDE variables")

    def test_subquery_rhs_returns_multiple_rows(self, decidb_cli):
        """A non-aggregated subquery RHS that yields more than one row must error.

        Complements `test_subquery_rhs_non_scalar`: that case has a DECIDE
        variable inside the subquery (binder rejection); this case has a
        scalar-expected subquery that returns multiple rows (executor-level
        rejection from DuckDB).
        """
        decidb_cli.assert_error("""
                WITH t AS (SELECT 1 AS l_quantity UNION ALL SELECT 2)
                SELECT l_quantity FROM t
                DECIDE x(INT)
                SUCH THAT SUM(x) <= (SELECT l_quantity FROM t)
                MAXIMIZE SUM(x)
            """, match=r"More than one row returned by a subquery")

    def test_per_on_perrow_constraint_rejection(self, decidb_cli):
        """PER attached to a per-row constraint must be rejected with a clear message.

        PER requires an aggregate constraint — each row already owns its own
        constraint, so partitioning has no meaning. Closes both
        `per/todo.md` and `error_handling/todo.md` gaps.
        """
        decidb_cli.assert_error("""
                WITH t AS (SELECT 1 AS l_quantity, 'A' AS grp
                    UNION ALL SELECT 2, 'B')
                SELECT l_quantity, x FROM t
                DECIDE x(INT)
                SUCH THAT x <= 5 PER grp
                MAXIMIZE SUM(x * l_quantity)
            """, match=r"PER can only be applied to aggregate \(SUM\) constraints")

    def test_aggregate_vs_aggregate_constraint_rejected(self, decidb_cli):
        """Aggregate on both LHS and RHS (`SUM(x*v) <= SUM(y*v)`) must be rejected.

        Per-row `x <= y` is now supported (ABS linearization), but the
        *aggregate-against-aggregate* shape reuses the "not a scalar or
        aggregate without DECIDE variables" binder rejection because the RHS
        is itself an aggregate over DECIDE variables.
        """
        decidb_cli.assert_error("""
                WITH t AS (SELECT 1 AS id, 10 AS val UNION ALL SELECT 2, 20)
                SELECT id, val, x, y FROM t
                DECIDE x(INT), y(INT)
                SUCH THAT SUM(x * val) <= SUM(y * val) AND SUM(y) <= 5
                MAXIMIZE SUM(x * val)
            """, match=r"SUM cannot be compared to an expression that is not a scalar or aggregate without DECIDE variables")

    def test_data_only_minmax_rhs_aggregate_errors_without_internal(self, decidb_cli):
        """Data-only SUM/AVG RHS aggregates are supported (hoisted to the LHS), but
        MIN/MAX RHS aggregates remain unsupported — and must surface a clean
        InvalidInputException, not an InternalException / stack trace."""
        decidb_cli.assert_error("""
                WITH t AS (SELECT 1 AS id, 10 AS val UNION ALL SELECT 2, 20)
                SELECT id, val, x FROM t
                DECIDE x(INT)
                SUCH THAT SUM(x * val) <= MIN(val)
                MAXIMIZE SUM(x * val)
            """, match=r"can't be reduced to a scalar bound.*MIN/MAX/COUNT")

    # --- Unsupported aggregates rejected at DecideBinder::BindAggregate ---

    def test_unsupported_aggregate_rejected(self, decidb_cli):
        """Non-whitelisted aggregates (BIT_AND, STRING_AGG, MEDIAN, ...) are rejected in DECIDE clauses."""
        decidb_cli.assert_error("""
                SELECT l_quantity FROM lineitem
                DECIDE x(INT)
                SUCH THAT BIT_AND(x) >= 0
                MAXIMIZE SUM(x * l_quantity) LIMIT 1
            """, match=r"only SUM, AVG, MIN, MAX, or COUNT is allowed")

    def test_count_over_decide_variable_rejected(self, decidb_cli):
        """COUNT(x) where x is a DECIDE variable is degenerate (always = row count)."""
        decidb_cli.assert_error("""
                SELECT l_quantity FROM lineitem
                DECIDE x(INT)
                SUCH THAT COUNT(x) >= 5
                MAXIMIZE SUM(x * l_quantity) LIMIT 1
            """, match=r"COUNT over a DECIDE variable is degenerate")

    # --- Non-linear scalar functions wrapping a DECIDE variable ---

    @pytest.mark.parametrize("fn", ["sqrt", "exp", "ln", "log", "floor", "ceil", "round", "sin", "cos"])
    def test_nonlinear_scalar_per_row_lhs(self, decidb_cli, fn):
        """f(x) op K as per-row constraint — silently produced wrong answers before
        the bind-time whitelist check. Each non-linear scalar wrapping a DECIDE
        variable should now be rejected with a uniform error."""
        decidb_cli.assert_error(f"""
                SELECT l_quantity FROM lineitem
                DECIDE x(REAL)
                SUCH THAT {fn}(x) <= 2
                MAXIMIZE SUM(x * l_quantity) LIMIT 1
            """, match=r"over a DECIDE variable is not supported")

    @pytest.mark.parametrize("fn", ["sqrt", "exp", "log", "floor"])
    def test_nonlinear_scalar_inside_sum(self, decidb_cli, fn):
        """SUM(f(x)) — crashed the symbolic layer with InternalException before.
        Now a clean BinderException."""
        decidb_cli.assert_error(f"""
                SELECT l_quantity FROM lineitem
                DECIDE x(REAL)
                SUCH THAT x <= 5
                MAXIMIZE SUM({fn}(x)) LIMIT 1
            """, match=r"over a DECIDE variable is not supported")

    def test_nonlinear_scalar_inside_abs(self, decidb_cli):
        """ABS(f(x)) — ABS passes its child through opaquely, so sqrt inside
        ABS used to FATAL the session. The recursive walk catches it."""
        decidb_cli.assert_error("""
                SELECT l_quantity FROM lineitem
                DECIDE x(REAL)
                SUCH THAT x <= 5
                MAXIMIZE SUM(ABS(sqrt(x) - 1)) LIMIT 1
            """, match=r"'sqrt' over a DECIDE variable is not supported")

    # --- POWER with exponent != 2 ---
    # Catches cases that used to crash the symbolic layer with InternalException
    # or silently produce identity/vacuous constraints (POWER(x,1), POWER(x,0)).

    @pytest.mark.parametrize("exp", ["0", "1", "3", "4", "0.5", "2.5", "-1"])
    def test_power_bad_constant_exponent_in_sum(self, decidb_cli, exp):
        """SUM(POWER(x, <non-2>)) — only POWER(expr, 2) is supported."""
        decidb_cli.assert_error(f"""
                SELECT l_quantity FROM lineitem
                DECIDE x(REAL)
                SUCH THAT x <= 5
                MINIMIZE SUM(POWER(x, {exp})) LIMIT 1
            """, match=r"Only POWER\(expr, 2\) is supported|Higher powers are not allowed")

    def test_power_variable_exponent_rejected(self, decidb_cli):
        """POWER(x, x) — exponent is itself a decide var; must be a constant."""
        decidb_cli.assert_error("""
                SELECT l_quantity FROM lineitem
                DECIDE x(REAL)
                SUCH THAT x <= 5
                MINIMIZE SUM(POWER(x, x)) LIMIT 1
            """, match=r"POWER exponent.*must be a constant integer")

    def test_power_column_exponent_rejected(self, decidb_cli):
        """POWER(x, col) — exponent is a table column, not a constant."""
        decidb_cli.assert_error("""
                SELECT l_quantity FROM lineitem
                DECIDE x(REAL)
                SUCH THAT x <= 5
                MINIMIZE SUM(POWER(x, l_quantity)) LIMIT 1
            """, match=r"POWER exponent.*must be a constant integer")

    def test_power_bad_exponent_per_row_constraint(self, decidb_cli):
        """POWER(x, 0.5) <= K as per-row (no SUM) — same clean rejection as inside SUM."""
        decidb_cli.assert_error("""
                SELECT l_quantity FROM lineitem
                DECIDE x(REAL)
                SUCH THAT POWER(x, 0.5) <= 3
                MAXIMIZE SUM(x) LIMIT 1
            """, match=r"Only POWER\(expr, 2\) is supported|Higher powers are not allowed")

    def test_power_over_data_column_allowed(self, decidb_cli):
        """POWER(col, 3) where col is a table column is fine — folds to a
        per-row constant. Only POWER wrapping a DECIDE variable is gated."""
        # Just ensure no BinderException; correctness of POWER-of-data is a
        # separate pre-existing question.
        decidb_cli.execute("""
                SELECT l_quantity, x FROM lineitem WHERE l_orderkey < 10
                DECIDE x(BOOL)
                SUCH THAT SUM(x * POWER(l_quantity, 2)) <= 1000
                MAXIMIZE SUM(x * l_quantity) LIMIT 1
            """)

    def test_power_cast_exponent_rejected(self, decidb_cli):
        """POWER(x, CAST(2 AS INTEGER)) — exponent wrapped in a cast is not a
        bare CONSTANT; rejected with the same 'must be a constant integer'
        message as POWER(x, col). Could be loosened later; defensive for now."""
        decidb_cli.assert_error("""
                SELECT l_quantity FROM lineitem
                DECIDE x(REAL) SUCH THAT x <= 5
                MINIMIZE SUM(POWER(x, CAST(2 AS INTEGER))) LIMIT 1
            """, match=r"POWER exponent.*must be a constant integer")

    def test_power_wrapping_bilinear_rejected(self, decidb_cli):
        """POWER(x * y, 2) — base is bilinear, so total degree is 4. The
        existing quadratic validator catches it; this test pins that it does
        not slip past the new pre-pass."""
        decidb_cli.assert_error("""
                SELECT l_quantity FROM lineitem
                DECIDE x(REAL), y(REAL)
                SUCH THAT x <= 5 AND y <= 5
                MINIMIZE SUM(POWER(x * y, 2)) LIMIT 1
            """, match=r"products of different DECIDE variables|linear in DECIDE variables")

    def test_perrow_null_rhs_rejected(self, decidb_cli):
        """NULL on the RHS of a per-row constraint reaches the binder as a
        CAST expression (DuckDB types NULL as unresolved). The binder rejects
        before we'd subtract an INVALID_INDEX coefficient from NULL."""
        decidb_cli.assert_error("""
                SELECT l_quantity FROM lineitem
                DECIDE x(REAL)
                SUCH THAT x + 3 <= CAST(NULL AS DOUBLE)
                MAXIMIZE SUM(x) LIMIT 1
            """, match=r"does not support|clause")

    # --- WHEN error cases ---

    @pytest.mark.when_constraint
    def test_when_decide_variable_in_condition(self, decidb_cli):
        """WHEN condition must not reference DECIDE variables."""
        decidb_cli.assert_error("""
                SELECT l_quantity FROM lineitem
                DECIDE x(INT)
                SUCH THAT SUM(x * l_quantity) <= 50 WHEN x = 1
                MAXIMIZE SUM(x * l_quantity) LIMIT 1
            """, match=r"WHEN conditions cannot reference DECIDE variables")

    @pytest.mark.when_compound
    def test_when_decide_variable_in_compound_condition(self, decidb_cli):
        """WHEN compound condition must not hide a DECIDE variable."""
        decidb_cli.assert_error("""
                SELECT l_quantity FROM lineitem
                DECIDE x(INT)
                SUCH THAT x <= 1 WHEN (x = 1 AND l_returnflag = 'R')
                MAXIMIZE SUM(x * l_quantity) LIMIT 1
            """, match=r"WHEN conditions cannot reference DECIDE variables")

    # --- Correlated subquery on aggregate RHS error cases ---

    @pytest.mark.cons_subquery
    @pytest.mark.cons_aggregate
    def test_correlated_subquery_aggregate_rhs_non_scalar(self, decidb_cli):
        """Correlated subquery on aggregate constraint RHS produces per-row values — must error.

        SUM(x * supplycost) <= (SELECT p_size ...) decorrelates into a per-row
        column, but an aggregate constraint needs a single scalar RHS.
        """
        decidb_cli.assert_error("""
                SELECT ps_partkey, ps_suppkey, x
                FROM partsupp
                WHERE ps_partkey < 10
                DECIDE x(INT)
                SUCH THAT SUM(x * ps_supplycost)
                          <= (SELECT CAST(p_size AS INTEGER) FROM part
                              WHERE p_partkey = ps_partkey)
                MAXIMIZE SUM(x)
            """, match=r"scalar right-hand side")

    @pytest.mark.cons_subquery
    @pytest.mark.cons_aggregate
    @pytest.mark.per_clause
    def test_correlated_subquery_aggregate_per_rhs_non_scalar(self, decidb_cli):
        """Correlated subquery on aggregate PER constraint RHS — must error.

        Same rejection as the non-PER case, but exercises the PER-path
        validation in ilp_model_builder.
        """
        decidb_cli.assert_error("""
                SELECT ps_partkey, ps_suppkey, x
                FROM partsupp
                WHERE ps_partkey < 20
                DECIDE x(INT)
                SUCH THAT SUM(x) <= (SELECT CAST(p_size AS INTEGER) FROM part
                                     WHERE p_partkey = ps_partkey)
                          PER ps_suppkey
                MAXIMIZE SUM(x)
            """, match=r"scalar right-hand side")

    # --- Division with a DECIDE variable in the divisor ---
    # `x / 2` and `x / data_col` are linear (coefficient scaling); `x / y`
    # where both are DECIDE variables is non-linear and rejected. Before the
    # whitelist tightening, per-row `x / y` was silently accepted with
    # nonsensical results (y=0 in the optimal solution).

    def test_perrow_division_by_decide_var_rejected(self, decidb_cli):
        """`x / y <= K` where both are DECIDE variables — must reject."""
        decidb_cli.assert_error("""
                WITH t AS (SELECT 1 AS id UNION ALL SELECT 2)
                SELECT id, x, y FROM t
                DECIDE x(REAL), y(REAL)
                SUCH THAT x / y <= 5
                    AND x <= 10
                    AND y <= 10
                MAXIMIZE SUM(x)
            """, match=r"Division by a DECIDE variable is not supported")

    def test_sum_division_by_decide_var_rejected(self, decidb_cli):
        """`SUM(x / y) <= K` with both DECIDE variables — must reject cleanly
        instead of crashing in symbolic normalization."""
        decidb_cli.assert_error("""
                WITH t AS (SELECT 1 AS id UNION ALL SELECT 2)
                SELECT id, x, y FROM t
                DECIDE x(REAL), y(REAL)
                SUCH THAT SUM(x / y) <= 5
                    AND x <= 10
                    AND y <= 10
                MAXIMIZE SUM(x)
            """, match=r"Division by a DECIDE variable is not supported")

    def test_objective_division_by_decide_var_rejected(self, decidb_cli):
        """Same rejection from the objective path."""
        decidb_cli.assert_error("""
                WITH t AS (SELECT 1 AS id UNION ALL SELECT 2)
                SELECT id, x, y FROM t
                DECIDE x(REAL), y(REAL)
                SUCH THAT x <= 10 AND y <= 10
                MAXIMIZE SUM(x / y)
            """, match=r"Division by a DECIDE variable is not supported")
