"""Binder-level semantic error tests.

These mirror the error cases from test/decidb/test_binder.test, updated to
match current DecidB behavior.  Some older error cases (SUM(x+x), strict <)
now succeed and are omitted.
"""

import re

import pytest

from decidb_cli import DecidBCliError

from ._output_helpers import assert_no_internal_leak


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
        """SUM BETWEEN with a column bound is now supported (C1).

        BETWEEN desugars into paired `<=` / `>=` constraints, and a bare column is
        now a legal decision-free bound on either leg — each collapses to the
        tightest value the per-tuple conjunction implies (MAX for the `>=` leg
        here). See `test_reduced_bound_data_column.py` for the oracle-verified
        proof of that collapse; this pins that BETWEEN composes with it correctly.
        """
        result, cols = decidb_cli.execute("""
                SELECT id, x FROM (VALUES (1, 4), (2, 9), (3, 6)) t(id, cap)
                DECIDE x(INT)
                SUCH THAT SUM(x) BETWEEN cap AND 100
                MINIMIZE SUM(x)
            """)
        xi = cols.index("x")
        assert sum(int(r[xi]) for r in result) == 9  # MAX(4, 9, 6)

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
        """A bare column RHS is now a legal bound of a reduced constraint (C1).

        Without `by` there is one group, so `SUM(x) <= l_quantity` is the
        conjunction of every row's own bound — `SUM(x) <= MIN(l_quantity)`. See
        `test_canonicalize_side_agnostic.py::test_row_varying_bound_collapses_to_tightest_on_either_side`
        for the oracle-verified proof of the collapse itself.
        """
        result, cols = decidb_cli.execute("""
                SELECT id, x FROM (VALUES (1, 4), (2, 9), (3, 6)) t(id, cap)
                DECIDE x(INT)
                SUCH THAT SUM(x) <= cap
                MAXIMIZE SUM(x)
            """)
        xi = cols.index("x")
        assert sum(int(r[xi]) for r in result) == 4  # MIN(4, 9, 6)

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
        """`SUM(x) = l_quantity` (C2): a bare column is a legal `=` bound in general
        (C1), but `l_quantity` genuinely varies across `lineitem`, so the single
        group's `=` constraint contradicts itself and is refused as infeasible."""
        decidb_cli.assert_error("""
                SELECT l_quantity FROM lineitem
                DECIDE x(INT)
                SUCH THAT SUM(x) = l_quantity
                MAXIMIZE SUM(x) LIMIT 1
            """, match=r"cannot hold two different values.*infeasible")

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
        decidb_cli.assert_error(
            """
                SELECT l_quantity FROM lineitem
                DECIDE x(INT)
                SUCH THAT SUM(x) <= 3
                MAXIMIZE l_quantity LIMIT 1
            """,
            match=r"MAXIMIZE l_quantity has no decision to optimize",
        )

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

    # `SUM(x*v) <= SUM(y*v)` used to be rejected here, sharing this file's "not a
    # scalar or aggregate without DECIDE variables" message because the bound was
    # itself a reducer over decision variables. the canonicalization refactor made the gate
    # side-agnostic and the shape solves; it is now oracle-verified in
    # test_canonicalize_side_agnostic.py::test_aggregate_vs_aggregate.

    def test_data_only_minmax_rhs_aggregate_now_supported(self, decidb_cli):
        """MIN/MAX on the RHS used to be refused because the left side could only
        reduce a data term by summing it. The reducer evaluator removes that limit,
        so this now solves: MIN(val) = 10 caps SUM(x*val), giving x = (1, 0)."""
        rows, cols = decidb_cli.execute("""
                WITH t AS (SELECT 1 AS id, 10 AS val UNION ALL SELECT 2, 20)
                SELECT id, val, x FROM t
                DECIDE x(INT)
                SUCH THAT SUM(x * val) <= MIN(val)
                MAXIMIZE SUM(x * val)
            """)
        xi, vi = cols.index("x"), cols.index("val")
        total = sum(int(round(float(r[xi]))) * float(r[vi]) for r in rows)
        assert total == 10.0, rows

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
        """POWER(x * y, 2) — base is bilinear, so total degree is 4.

        The refusal is still the binder's; only its wording moved. It used to come from
        the SUM-argument gate, which reported the base as "products of different DECIDE
        variables ... only allowed in objectives and bilinear constraints" — misleading
        here, because bilinear *is* allowed in an objective and the real fault is that
        squaring it reaches degree 4. The degree owner says so directly."""
        decidb_cli.assert_error("""
                SELECT l_quantity FROM lineitem
                DECIDE x(REAL), y(REAL)
                SUCH THAT x <= 5 AND y <= 5
                MINIMIZE SUM(POWER(x * y, 2)) LIMIT 1
            """, match=r"higher-order products.*total degree > 2")

    @pytest.mark.parametrize("expr", [
        "POWER(x, 2) * y",
        "POWER(x, 2) * POWER(y, 2)",
    ])
    def test_power_degree_rejected_in_constraint(self, decidb_cli, expr):
        """POWER contributes degree(base) * n, so these reach the binder gate.

        Before the degree function understood POWER, POWER(x, 2) counted as
        degree 1 and these passed the gate, to be refused much later by physical
        extraction with a message naming a normalization pass that no longer
        exists. Pin the rejection at the binder, where it is worded for a user."""
        decidb_cli.assert_error(f"""
                SELECT l_quantity FROM lineitem
                DECIDE x(REAL), y(REAL)
                SUCH THAT SUM({expr}) <= 10 AND x <= 5 AND y <= 5
                MAXIMIZE SUM(x) LIMIT 1
            """, match=r"Binder Error:.*higher-order products.*total degree > 2")

    def test_power_degree_rejected_in_objective(self, decidb_cli):
        """Same shape in an objective takes the same path and the same error."""
        decidb_cli.assert_error("""
                SELECT l_quantity FROM lineitem
                DECIDE x(REAL), y(REAL)
                SUCH THAT x <= 5 AND y <= 5
                MINIMIZE SUM(POWER(x, 2) * y) LIMIT 1
            """, match=r"Binder Error:.*higher-order products.*total degree > 2")

    @pytest.mark.parametrize("constraint", [
        "POWER(x * y, 2) <= 5",
        "(x * y) * (x * y) <= 5",
        "POWER(POWER(x, 2), 2) <= 5",
        "x * y * x <= 5",
    ])
    def test_degree_refused_on_a_per_row_constraint(self, decidb_cli, constraint):
        """A per-row constraint is held to the same degree rule as a reducer argument.

        These four used to bind cleanly and be refused at plan time by term extraction,
        because the binder's degree gate was reachable only from ValidateSumArgument and
        so ran for `SUM(...)` arguments alone. Degree is one judgement with one owner, so
        the spelling must not decide which layer refuses."""
        decidb_cli.assert_error(f"""
                SELECT l_quantity FROM lineitem
                DECIDE x(INT), y(INT)
                SUCH THAT {constraint} AND SUM(x) >= 1
                MAXIMIZE SUM(x) LIMIT 1
            """, match=r"Binder Error:.*higher-order products.*total degree > 2")

    def test_degree_refusal_points_at_the_offending_term(self, decidb_cli):
        """The refusal carries a source location.

        This is the whole reason the judgement stays at bind time rather than moving to
        the layer that can see the post-rewrite tree: a bound-tree refusal can only point
        a caret if the binder stamped the node, and a plan-time one cannot point at all."""
        decidb_cli.assert_error("""
                SELECT l_quantity FROM lineitem
                DECIDE x(INT), y(INT)
                SUCH THAT x * y * x <= 5 AND SUM(x) >= 1
                MAXIMIZE SUM(x) LIMIT 1
            """, match=r"LINE \d+:")

    @pytest.mark.parametrize("constraint", [
        "x * y <= 5",       # bilinear: two different decisions
        "POWER(x, 2) <= 5", # quadratic by squaring
        "x * x <= 5",       # quadratic by self-product
    ])
    def test_supported_degrees_still_accepted_per_row(self, decidb_cli, constraint):
        """The other side of the gate: moving degree to one owner must not narrow what
        DECIDE accepts. Degree 2 is legal in a per-row constraint in all three spellings,
        and each takes a different route through the walk.

        The claim is the binder's, and the binder is the same on every host, so this
        does not force a backend. What differs is what happens after binding: a host
        without Gurobi cannot load a quadratic constraint, and the plan-time gate
        refuses on those grounds. For this test that refusal counts as acceptance — the
        binder let the constraint through, which is the whole claim. A binder rejection
        looks different and is pinned by the sibling test above: it carries a `LINE n:`
        source position. Three rows are enough: the claim is the binder's, and over all of
        `lineitem` the `x * y` case is a non-convex model the solver needs 40 s for.
        """
        sql = f"""
                SELECT l_quantity FROM (SELECT * FROM lineitem LIMIT 3) lineitem
                DECIDE x(INT), y(INT)
                SUCH THAT {constraint} AND x <= 3 AND y <= 3
                MAXIMIZE SUM(x) LIMIT 1
            """
        try:
            rows, _ = decidb_cli.execute(sql)
        except DecidBCliError as e:
            assert "needs Gurobi" in e.message, \
                f"degree 2 was rejected before reaching a backend: {e.message}"
            return
        assert rows

    def test_power_squared_scaled_by_constant_still_accepted(self, decidb_cli):
        """Guard the other side of the degree change: a constant factor adds no
        degree, so `3 * POWER(x, 2)` stays quadratic and must keep working."""
        rows, _ = decidb_cli.execute("""
                SELECT l_quantity FROM lineitem
                DECIDE x(REAL)
                SUCH THAT x <= 5
                MINIMIZE SUM(3 * POWER(x, 2)) LIMIT 1
            """)
        assert rows

    @pytest.mark.parametrize("constraint", [
        "x <= CAST(NULL AS DOUBLE)",
        "x + 3 <= CAST(NULL AS DOUBLE)",
        "CAST(NULL AS DOUBLE) >= x + 3",
    ])
    def test_perrow_null_rhs_rejected(self, decidb_cli, constraint):
        """Direct, rebuilt and reversed known-NULL bounds fail during planning."""
        decidb_cli.assert_error(f"""
                SELECT l_quantity FROM lineitem
                DECIDE x(REAL)
                SUCH THAT {constraint}
                MAXIMIZE SUM(x) LIMIT 1
            """, match=r"Binder Error: DECIDE constraint bound evaluates to NULL.*COALESCE")

    # --- WHEN error cases ---

    @pytest.mark.when_constraint
    def test_when_decide_variable_in_condition(self, decidb_cli):
        """WHEN condition must not reference DECIDE variables."""
        decidb_cli.assert_error(
            """
                SELECT l_quantity FROM lineitem
                DECIDE x(INT)
                SUCH THAT when x = 1: SUM(x * l_quantity) <= 50
                MAXIMIZE SUM(x * l_quantity) LIMIT 1
            """,
            match=r"x is a decision; when, per and by may only use data",
        )

    @pytest.mark.when_compound
    def test_when_decide_variable_in_compound_condition(self, decidb_cli):
        """WHEN compound condition must not hide a DECIDE variable."""
        decidb_cli.assert_error(
            """
                SELECT l_quantity FROM lineitem
                DECIDE x(INT)
                SUCH THAT when (x = 1 AND l_returnflag = 'R'): x <= 1
                MAXIMIZE SUM(x * l_quantity) LIMIT 1
            """,
            match=r"x is a decision; when, per and by may only use data",
        )

    # --- Correlated subquery on aggregate RHS ---
    # A row-varying RHS on a reduced constraint used to be refused here. It is now
    # reduced to the tightest bound the per-tuple conjunction implies (paper §3.2.1),
    # so the positive cases moved to test_cons_correlated_subquery.py as
    # oracle-verified tests. Only `=` still has no single bound, and that is a
    # data-dependent rejection — it fires when the values really do differ.

    @pytest.mark.cons_subquery
    @pytest.mark.cons_aggregate
    def test_correlated_subquery_aggregate_equality_rhs_rejected(self, decidb_cli):
        """`SUM(x) = <per-row value>` is N contradictory constraints, not a bound.

        `<=`/`>=` collapse to the tightest bound; `=` cannot, so it is refused
        (C2) as a contradiction, with the two conflicting values named.
        """
        decidb_cli.assert_error("""
                SELECT ps_partkey, ps_suppkey, x
                FROM partsupp
                WHERE ps_partkey < 10
                DECIDE x(INT)
                SUCH THAT SUM(x) = (SELECT CAST(p_size AS INTEGER) FROM part
                                    WHERE p_partkey = ps_partkey)
                MAXIMIZE SUM(x)
            """, match=r"takes more than one value here.*cannot hold two different values.*infeasible")

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

    def test_not_equal_still_accepted(self, decidb_cli):
        """`<>` is rewritten into an OR only after binding, so the OR check never sees it."""
        rows, _ = decidb_cli.execute(
            """
                FROM (VALUES (1, 1.0), (2, 2.0)) t(id, s)
                DECIDE x(INT)
                SUCH THAT x BETWEEN 0 AND 3 AND SUM(x) <> 6 AND x <> 3
                MAXIMIZE SUM(s * x)
            """
        )
        assert len(rows) == 2
        assert all(row[-1] != 3 for row in rows)
        assert sum(row[-1] for row in rows) != 6

# --- Refusals that hold in every spelling ---
# A shape the engine cannot model is refused whether its clause is bare or written
# with a scope in front: never answered as a different constraint, never an
# internal error.

TABLES = """WITH t AS (SELECT * FROM (VALUES (1, 3, 'A'), (2, 5, 'A'), (3, 7, 'B'), (4, 9, 'B'))
                v(id, cap, g)),
                f AS (SELECT * FROM (VALUES ('A', 2), ('B', 4)) v(g, k))"""

SPELLINGS = ["", "per row: ", "per (): "]


def _refusal(decidb_cli, sql, match, context):
    result = decidb_cli.execute_raw(sql)
    assert re.search(match, result.stderr), result.stderr
    assert_no_internal_leak(result, context)


@pytest.mark.error_binder
@pytest.mark.error
class TestRefusalsInEverySpelling:

    @pytest.mark.parametrize(
        "clause",
        [
            "x <= cap OR x <= 1",
            "x <= 40 AND (x >= cap OR x <= 2)",
            "x <= 40 AND (x <= 1 OR (x <= 2 AND x >= 1))",
            "sum(x) <= 4 OR sum(x) >= 10",
            "x <= 40 AND when id > 0: (x >= cap OR x <= 2)",
            "x <= 40 AND per row: (x >= cap OR x <= 2)",
            "x <= 40 AND per (): (sum(x) <= 4 OR sum(x) >= 10)",
        ],
    )
    def test_or_between_constraints_refused(self, decidb_cli, clause):
        """`OR` between constraints used to be read as `AND`: `x <= cap OR x <= 1`
        answered x = 1 on every row. A disjunction needs an indicator the engine does
        not build, so it is refused."""
        _refusal(
            decidb_cli,
            f"""{TABLES}
            SELECT id, x FROM t DECIDE x(INT) SUCH THAT {clause} MAXIMIZE SUM(x)""",
            r"SUCH THAT: OR is not supported\.",
            "OR between constraints",
        )

    def test_or_inside_a_when_condition_still_filters(self, decidb_cli):
        """An `OR` inside a condition is data, not a disjunction of constraints."""
        query = f"""{TABLES}
            SELECT id, x FROM t DECIDE x(INT)
            SUCH THAT x <= cap AND sum(when {{}}: x) <= 4 AND x <= (SELECT max(cap) FROM t WHERE {{}})
                AND when {{}}: x <= 1
            MAXIMIZE SUM(x)"""
        written = query.format(*["g = 'A' OR id = 3"] * 3)
        equivalent = query.format(*["id IN (1, 2, 3)"] * 3)
        assert decidb_cli.execute(written)[0] == decidb_cli.execute(equivalent)[0]

    @pytest.mark.parametrize(
        "clauses,where",
        [
            ("SUCH THAT when random() < 2: sum(x) <= 4 MAXIMIZE SUM(x)", "a constraint"),
            ("SUCH THAT sum(x) <= 4 MAXIMIZE sum(when random() < 2: x)", "the objective"),
            ("SUCH THAT sum(x) by (random() < 2) <= 4 MAXIMIZE SUM(x)", "a by key"),
        ],
    )
    def test_volatile_function_refused(self, decidb_cli, clauses, where):
        """The solver reads a clause more than once (scope checks, rows, a bound-side
        reducer), so `random()` would select different rows each time."""
        _refusal(
            decidb_cli,
            f"""{TABLES}
            SELECT id, x FROM t DECIDE x(INT) {clauses}""",
            rf"DECIDE: random\(\) is not supported in {where}",
            "volatile function",
        )

    def test_volatile_function_in_the_source_is_data(self, decidb_cli):
        """A value computed once per row before DECIDE is ordinary data."""
        rows = decidb_cli.execute(
            f"""{TABLES}
            SELECT id, x FROM (SELECT *, random() * 0 + 4 AS r FROM t) s
            DECIDE x(INT) SUCH THAT sum(x) <= sum(r) MAXIMIZE SUM(x)"""
        )[0]
        assert sum(int(row[1]) for row in rows) == 16

    @pytest.mark.parametrize("spelling", SPELLINGS)
    @pytest.mark.parametrize(
        "clause",
        [
            "x / sum(cap) <= 1",
            "sum(x) / sum(cap) <= 1",
            "sum(x) / avg(cap) <= 1",
            "sum(x) / max(cap) <= 1",
            "sum(x) / min(cap) <= 1",
            "sum(x) / count(*) <= 2",
            "avg(x) / sum(cap) <= 1",
            "max(x) / sum(cap) <= 1",
            "sum(x * cap) / sum(cap) <= 5",
            "sum(x) / (1 + sum(cap)) <= 1",
            "x <= sum(x) / sum(cap)",
        ],
    )
    def test_data_only_aggregate_divisor_rejected(self, decidb_cli, spelling, clause):
        """A data-only aggregate stays on the bound side (S10). As a divisor of a
        decision term it is refused like the same aggregate as a multiplier, never
        reaching term extraction ('INTERNAL Error: ... VALUE shape reached term
        extraction')."""
        _refusal(
            decidb_cli,
            f"""{TABLES}
            SELECT id, x FROM t DECIDE x(INT) SUCH THAT x <= cap AND {spelling}{clause} MAXIMIZE SUM(x)""",
            r"aggregate over table columns .* cannot divide a decision variable",
            "a data-only aggregate divisor",
        )

    @pytest.mark.parametrize("spelling", SPELLINGS + ["per g: ", "when id > 0: "])
    @pytest.mark.parametrize(
        "clause",
        [
            "(sum(x) + sum(cap)) / 2 <= 20",
            "20 >= (sum(x) + sum(cap)) / 2",
            "(sum(x) - avg(cap)) / 2 <= 1",
            "(sum(x) + count(*)) / 2 <= 5",
            "(avg(x) + avg(cap)) / 2 <= 5",
            "(max(x) + sum(cap)) / 2 <= 15",
            "-(sum(x) + sum(cap)) / 2 >= -20",
            "(sum(x) + sum(cap)) / (SELECT 2) <= 20",
        ],
    )
    def test_data_only_aggregate_in_a_divided_term_rejected(self, decidb_cli, spelling, clause):
        """A data-only aggregate divided together with a decision term is the scaled term
        the multiplier rule already refuses (`2 * (sum(x) - avg(cap))`); under a scope
        spelling it, too, reached term extraction as an internal error."""
        _refusal(
            decidb_cli,
            f"""{TABLES}
            SELECT id, x FROM t DECIDE x(INT) SUCH THAT x <= cap AND {spelling}{clause} MAXIMIZE SUM(x)""",
            r"aggregate over table columns .* cannot be divided together with a decision variable",
            "a data-only aggregate in a divided term",
        )

    def test_data_only_aggregate_in_a_divided_term_as_a_scalar_or_moved(self, decidb_cli):
        """The two ways out the message implies give one answer: the aggregate as a scalar
        subquery, or moved to the bound side."""
        query = f"""{TABLES}
            SELECT id, x FROM t DECIDE x(INT) SUCH THAT x <= cap AND per row: {{}} MAXIMIZE SUM(x)"""
        scalar = query.format("(sum(x) + (SELECT sum(cap) FROM t)) / 2 <= 20")
        moved = query.format("sum(x) / 2 <= 20 - sum(cap) / 2")
        assert decidb_cli.execute(scalar)[0] == decidb_cli.execute(moved)[0]

    def test_and_or_inside_a_data_value_bind_as_data(self, decidb_cli):
        """A conjunction inside an operand is a value, not a list of constraints: its
        `OR` is no disjunction of constraints, and a column under it is no constraint."""
        query = f"""{TABLES}, b AS (SELECT *, cap > 4 AS big FROM t)
            SELECT id, x FROM b DECIDE x(INT) SUCH THAT x <= cap AND x <= 1 + CAST({{}} AS INT)
            MAXIMIZE SUM(x)"""
        nested = query.format("((SELECT true) AND ((SELECT true) OR (SELECT false)))")
        assert decidb_cli.execute(nested)[0] == decidb_cli.execute(query.format("true"))[0]
        assert decidb_cli.execute(query.format("(big AND big)"))[0] == decidb_cli.execute(query.format("big"))[0]

    def test_data_only_aggregate_bound_and_scalar_divisor_still_allowed(self, decidb_cli):
        """Guard against over-rejecting: divided on the bound side, or replaced by the
        scalar subquery the message suggests, the aggregate is one number."""
        query = f"""{TABLES}
            SELECT id, x FROM t DECIDE x(INT)
            SUCH THAT x <= cap AND per row: x <= {{}} / 3 AND per row: x / {{}} <= 0.25
            MAXIMIZE SUM(x)"""
        written = query.format("sum(cap)", "(SELECT sum(cap) FROM t)")
        precomputed = query.format(*["(SELECT sum(cap) FROM t)"] * 2)
        assert decidb_cli.execute(written)[0] == decidb_cli.execute(precomputed)[0]

    def test_decision_free_objective_term_named_in_sql(self, decidb_cli):
        """The refusal of a row-varying objective term names the term in SQL, not by
        an internal alias ('__source_fragment_0____row_varying_subquery__'). A subquery
        prints as DuckDB renders it (logged in issues/bugs.md), so only its text is
        pinned."""
        result = decidb_cli.execute_raw(
            f"""{TABLES}
            SELECT id, x FROM t DECIDE x(INT) SUCH THAT x <= cap
            MAXIMIZE per (): sum(x) + (SELECT k FROM f WHERE f.g = t.g)"""
        )
        assert re.search(
            r"'\(SELECT k FROM f WHERE [^']*\)' does not reference a decision", result.stderr
        ), result.stderr
        assert_no_internal_leak(result, "a decision-free objective term")


_REFUSAL_ROWS = "SELECT id, x FROM (VALUES (1, 3), (2, 4)) t(id, cap) "


@pytest.mark.error_binder
@pytest.mark.error
class TestRefusalsSpeakSql:
    """A refusal quotes the clause as the user wrote it and names no engine enum."""

    @staticmethod
    def _refusal(decidb_cli, clauses, objective="MAXIMIZE SUM(x)"):
        result = decidb_cli.execute_raw(f"{_REFUSAL_ROWS} DECIDE x(INT) SUCH THAT x <= 5 AND {clauses} {objective}")
        assert "::" not in result.stderr, result.stderr
        return result.stderr

    @pytest.mark.parametrize(
        "clause,quoted",
        [
            ("x IS DISTINCT FROM 3", "'(x IS DISTINCT FROM 3)'"),  # a comparison DECIDE does not read
            ("NOT (x <= 1 OR x >= 3)", "'(NOT ((x <= 1) OR (x >= 3)))'"),  # an operator it does not read
            ("1", "'1'"),  # a clause of no kind it reads
            ("x = (x > 2)", "'(x > 2)'"),  # a comparison over a decision as an operand
            ("x >= (x BETWEEN 1 AND 3)", "'(x BETWEEN 1 AND 3)'"),  # the same, a BETWEEN
            ("x <= (x > 1 AND cap > 1)", "'((x > 1) AND (cap > 1))'"),  # the same, an AND
        ],
    )
    def test_unsupported_constraint_is_quoted(self, decidb_cli, clause, quoted):
        stderr = self._refusal(decidb_cli, clause)
        assert f"SUCH THAT does not support {quoted}" in stderr, stderr

    def test_unsupported_reducer_body_is_quoted(self, decidb_cli):
        # A comparison over data is a 0/1 coefficient (test_data_operands.py); over a
        # decision it is refused.
        stderr = self._refusal(decidb_cli, "SUM(x * (x BETWEEN 1 AND 2)) <= 3")
        assert "SUM expression does not support '(x BETWEEN 1 AND 2)'" in stderr, stderr

    @pytest.mark.parametrize(
        "objective,message",
        [
            ("MAXIMIZE 3", "MAXIMIZE 3 has no decision to optimize"),
            ("MINIMIZE cap", "MINIMIZE cap has no decision to optimize"),
            ("MAXIMIZE x > 1", "MAXIMIZE does not support '(x > 1)'"),
            ("MAXIMIZE count(*)", "does not support function 'count(*)'"),
        ],
    )
    def test_unsupported_objective_is_quoted(self, decidb_cli, objective, message):
        assert message in self._refusal(decidb_cli, "SUM(x) <= 9", objective)

    def test_count_star_in_a_constraint_is_quoted_as_written(self, decidb_cli):
        stderr = self._refusal(decidb_cli, "count(*) <= 3")
        assert "left-hand side function 'count(*)'" in stderr, stderr

    def test_reducer_over_a_constant_is_not_a_query_wide_decision(self, decidb_cli):
        """`SUM(1)` has no decision at all; only a body made of query-wide decisions is called one."""
        stderr = self._refusal(decidb_cli, "SUM(1) <= 5")
        assert "SUM expression must reference at least one DECIDE variable" in stderr, stderr
