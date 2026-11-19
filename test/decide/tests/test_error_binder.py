"""Binder-level semantic error tests.

These mirror the error cases from test/decidb/test_binder.test, updated to
match current DeciDB behavior.  Some older error cases (SUM(x+x), strict <)
now succeed and are omitted.
"""

import re
from pathlib import Path
from typing import NamedTuple

import pytest

from decidb_cli import DeciDBCliError

from ._output_helpers import assert_no_internal_leak


@pytest.mark.error_binder
@pytest.mark.error
class TestBinderErrors:
    """DECIDE binder should reject semantically invalid queries."""

    def test_variable_conflicts_with_column(self, decidb_cli):
        """DECIDE variable name clashes with an existing column."""
        decidb_cli.assert_error_at(
            """
                SELECT l_quantity FROM lineitem
                DECIDE l_quantity(INT), x(INT)
                SUCH THAT x IN (1,2,3)
                MAXIMIZE SUM(x*l_quantity) LIMIT 1
            """,
            kind="Binder Error",
            at="l_quantity(INT)",
        )

    def test_duplicate_decide_variables(self, decidb_cli):
        """Same variable name declared twice."""
        decidb_cli.assert_error_at(
            """
                SELECT l_quantity FROM lineitem
                DECIDE x(INT), x(INT)
                SUCH THAT x IN (1,2,3)
                MAXIMIZE SUM(x*l_quantity) LIMIT 1
            """,
            kind="Binder Error",
            at="x(INT)",
        )

    def test_unknown_variable_in_constraint(self, decidb_cli):
        """Using an undeclared variable in IN expression."""
        decidb_cli.assert_error_at(
            """
                SELECT l_quantity FROM lineitem
                DECIDE y(INT)
                SUCH THAT x IN (1,2,3)
                MAXIMIZE SUM(x) LIMIT 1
            """,
            kind="Not implemented Error",
            at="x IN (1,2,3)",
        )

    def test_is_null_unsupported(self, decidb_cli):
        """IS NULL is not supported in SUCH THAT."""
        decidb_cli.assert_error(
            """
                SELECT l_quantity FROM lineitem
                DECIDE x(INT)
                SUCH THAT x IS NULL
                MAXIMIZE SUM(x*l_quantity) LIMIT 1
            """,
            match=r"does not support",
        )

    @pytest.mark.parametrize("clause", ["x * avg(p) <= 25", "avg(p) * sum(x) <= 25", "sum(x) * max(p) <= 25"])
    def test_data_only_aggregate_coefficient_rejected(self, decidb_cli, clause):
        """A data-only aggregate (`AVG(p)`, `MAX(p)`, ... over columns only) scaling a
        decision outside every reducer over decisions is rejected: the bound side holds it
        (S10). Inside one it is a value of its path's group, a coefficient
        (test_anr_nested.py)."""
        decidb_cli.assert_error_at(
            f"""
                SELECT id, p, x FROM (VALUES (1,10.0),(2,20.0)) t(id, p)
                DECIDE x(REAL)
                SUCH THAT x <= 1 AND {clause}
                MAXIMIZE SUM(x)
            """,
            kind="Binder Error",
            at=re.search(r"(?:avg|max)\(p\)", clause).group(0),
        )

    def test_data_only_aggregate_rhs_still_allowed(self, decidb_cli):
        """Guard against over-rejecting: a data-only aggregate is fine as a constraint
        RHS (`SUM(x) <= AVG(p)`), and a genuine DECIDE aggregate `AVG(x * p)` is fine —
        only using the data aggregate as a *coefficient* is rejected."""
        rows, _ = decidb_cli.execute(
            """
            SELECT id, p, x FROM (VALUES (1,10.0),(2,20.0)) t(id, p)
            DECIDE x(BOOL)
            SUCH THAT SUM(x) <= avg(p)
            MAXIMIZE SUM(x)
        """
        )
        assert rows  # solved, not rejected
        rows2, _ = decidb_cli.execute(
            """
            SELECT id, p, x FROM (VALUES (1,10.0),(2,20.0)) t(id, p)
            DECIDE x(BOOL)
            SUCH THAT AVG(x * p) >= 5
            MAXIMIZE SUM(x)
        """
        )
        assert rows2

    def test_sum_with_in_not_allowed(self, decidb_cli):
        """SUM(x) IN (...) is not a valid constraint form.

        Tightened match anchors to the aggregate-specific wording so a
        regression in error-message quality (e.g. falling back to a generic
        "not supported" path) would fail the test.
        """
        decidb_cli.assert_error_at(
            """
                SELECT l_quantity FROM lineitem
                DECIDE x(INT)
                SUCH THAT SUM(x) IN (1,2,3)
                MAXIMIZE SUM(x*l_quantity) LIMIT 1
            """,
            kind="Not implemented Error",
            at="SUM(x) IN (1,2,3)",
        )

    def test_non_decide_variable_in_constraint(self, decidb_cli):
        """Using a regular column (not a DECIDE var) as a constrained value."""
        decidb_cli.assert_error_at(
            """
                SELECT l_quantity FROM lineitem
                DECIDE x(INT)
                SUCH THAT l_quantity <= 5
                MAXIMIZE SUM(x*l_quantity) LIMIT 1
            """,
            kind="Binder Error",
            at="l_quantity",
        )

    def test_non_sum_avg_min_max_function_in_objective(self, decidb_cli):
        """STDDEV(x) is not supported in objective — only SUM, AVG, MIN, or MAX is allowed."""
        decidb_cli.assert_error_at(
            """
                SELECT l_quantity FROM lineitem
                DECIDE x(INT)
                SUCH THAT SUM(x) <= 5
                MAXIMIZE STDDEV(x*l_quantity) LIMIT 1
            """,
            kind="Not implemented Error",
            at="STDDEV(x*l_quantity)",
        )

    def test_no_decide_variable_in_sum(self, decidb_cli):
        """SUM over a regular column without DECIDE variable."""
        decidb_cli.assert_error_at(
            """
                SELECT l_quantity FROM lineitem
                DECIDE x(INT)
                SUCH THAT SUM(l_quantity) <= 5
                MAXIMIZE SUM(x*l_quantity) LIMIT 1
            """,
            kind="Binder Error",
            at="SUM(l_quantity)",
        )

    @pytest.mark.parametrize("lhs", ["SUM(l_quantity) + 1", "2 * MAX(l_quantity)", "ABS(SUM(l_quantity) - 3)"])
    def test_no_decide_variable_under_arithmetic(self, decidb_cli, lhs):
        """A reducer under arithmetic reads no decision either: refused by name, where it
        reached term extraction as an internal error."""
        decidb_cli.assert_error_at(
            f"""
                SELECT l_quantity FROM lineitem
                DECIDE x(INT)
                SUCH THAT x <= 1 AND {lhs} <= 500
                MAXIMIZE SUM(x*l_quantity) LIMIT 1
            """,
            kind="Binder Error",
            at=lhs,
        )

    def test_multiple_decide_variables_in_sum(self, decidb_cli):
        """Cubic (x*x*x) in SUM is not supported — must reject."""
        decidb_cli.assert_error_at(
            """
                SELECT l_quantity FROM lineitem
                DECIDE x(INT)
                SUCH THAT SUM(x*x*x) <= 5
                MAXIMIZE SUM(x*l_quantity) LIMIT 1
            """,
            kind="Not implemented Error",
            at="x*x*x",
        )

    def test_between_non_scalar(self, decidb_cli):
        """SUM BETWEEN with a column bound is now supported (C1).

        BETWEEN desugars into paired `<=` / `>=` constraints, and a bare column is
        now a legal decision-free bound on either leg — each collapses to the
        tightest value the per-tuple conjunction implies (MAX for the `>=` leg
        here). See `test_reduced_bound_data_column.py` for the oracle-verified
        proof of that collapse; this pins that BETWEEN composes with it correctly.
        """
        result, cols = decidb_cli.execute(
            """
                SELECT id, x FROM (VALUES (1, 4), (2, 9), (3, 6)) t(id, cap)
                DECIDE x(INT)
                SUCH THAT SUM(x) BETWEEN cap AND 100
                MINIMIZE SUM(x)
            """
        )
        xi = cols.index("x")
        assert sum(int(r[xi]) for r in result) == 9  # MAX(4, 9, 6)

    def test_decide_between_decide_variable(self, decidb_cli):
        """Multi-variable per-row constraints are now supported (e.g. x BETWEEN y AND 1).
        This is needed for ABS linearization which generates constraints like d >= x - c."""
        # This used to be an error; now valid (x >= y AND x <= 1)
        result, cols = decidb_cli.execute(
            """
                SELECT l_quantity FROM lineitem
                DECIDE x(INT), y(INT)
                SUCH THAT x BETWEEN y AND 1
                MAXIMIZE SUM(x*l_quantity) LIMIT 1
            """
        )
        assert len(result) > 0

    def test_in_rhs_with_decide_variable(self, decidb_cli):
        """IN domain constraints on DECIDE variables are not yet supported."""
        decidb_cli.assert_error_at(
            """
                SELECT l_quantity FROM lineitem
                DECIDE x(INT)
                SUCH THAT x IN (1,2,x)
                MAXIMIZE SUM(x*l_quantity) LIMIT 1
            """,
            kind="Binder Error",
            at="x IN (1,2,x)",
        )

    def test_sum_rhs_non_scalar(self, decidb_cli):
        """A bare column RHS is now a legal bound of a reduced constraint (C1).

        Without `by` there is one group, so `SUM(x) <= l_quantity` is the
        conjunction of every row's own bound — `SUM(x) <= MIN(l_quantity)`. See
        `test_canonicalize_side_agnostic.py::test_row_varying_bound_collapses_to_tightest_on_either_side`
        for the oracle-verified proof of the collapse itself.
        """
        result, cols = decidb_cli.execute(
            """
                SELECT id, x FROM (VALUES (1, 4), (2, 9), (3, 6)) t(id, cap)
                DECIDE x(INT)
                SUCH THAT SUM(x) <= cap
                MAXIMIZE SUM(x)
            """
        )
        xi = cols.index("x")
        assert sum(int(r[xi]) for r in result) == 4  # MIN(4, 9, 6)

    def test_decide_variable_rhs_with_decide(self, decidb_cli):
        """Multi-variable per-row constraints are now supported (e.g. x <= y).
        This is needed for ABS linearization which generates constraints like d >= x - c."""
        # This used to be an error; now valid (x - y <= 0)
        # Use small dataset + SUM bound (otherwise MAXIMIZE is unbounded)
        result, cols = decidb_cli.execute(
            """
                SELECT l_quantity FROM lineitem
                WHERE l_orderkey <= 3
                DECIDE x(INT), y(INT)
                SUCH THAT x <= y AND SUM(y) <= 100
                MAXIMIZE SUM(x*l_quantity)
            """
        )
        assert len(result) > 0

    def test_sum_equal_non_scalar(self, decidb_cli):
        """`SUM(x) = l_quantity` (C2): a bare column is a legal `=` bound in general
        (C1), but `l_quantity` genuinely varies across `lineitem`, so the single
        group's `=` constraint contradicts itself and is refused as infeasible."""
        decidb_cli.assert_error_at(
            """
                SELECT l_quantity FROM lineitem
                DECIDE x(INT)
                SUCH THAT SUM(x) = l_quantity
                MAXIMIZE SUM(x) LIMIT 1
            """,
            kind="Invalid Input Error",
            at="l_quantity",
        )

    def test_objective_with_addition_succeeds(self, decidb_cli):
        """`MAXIMIZE SUM(...) + 3` is now supported: the constant offset is
        peeled from the objective body (it doesn't affect argmax) and
        preserved on LogicalDecide.objective_constant_offset for any caller
        that later reports the objective value. Previously rejected as a
        "non-aggregate term" by the extractor."""
        rows, cols = decidb_cli.execute(
            """
                SELECT l_quantity FROM lineitem
                DECIDE x(INT)
                SUCH THAT SUM(x) <= 3
                MAXIMIZE SUM(x*l_quantity)+3 LIMIT 1
            """
        )
        # Query runs; row count reflects the LIMIT clause.
        assert len(rows) > 0

    def test_objective_bare_column(self, decidb_cli):
        """MAXIMIZE with a bare column (not SUM) is not allowed."""
        decidb_cli.assert_error_at(
            """
                SELECT l_quantity FROM lineitem
                DECIDE x(INT)
                SUCH THAT SUM(x) <= 3
                MAXIMIZE l_quantity LIMIT 1
            """,
            kind="Binder Error",
            at="l_quantity",
        )

    def test_subquery_rhs_non_scalar(self, decidb_cli):
        """Subquery RHS that references DECIDE variables.

        DeciDB surfaces this via the same "not a scalar or aggregate without
        DECIDE variables" binder path; the tightened match anchors to that
        wording so a regression (e.g. a less-informative generic scalar
        check) would fail.
        """
        decidb_cli.assert_error_at(
            """
                SELECT l_quantity FROM lineitem
                DECIDE x(INT)
                SUCH THAT SUM(x) <= (SELECT l_quantity+x FROM lineitem)
                MAXIMIZE SUM(x) LIMIT 1
            """,
            kind="Binder Error",
            at="(SELECT l_quantity+x FROM lineitem)",
        )

    def test_subquery_rhs_returns_multiple_rows(self, decidb_cli):
        """A non-aggregated subquery RHS that yields more than one row must error.

        Complements `test_subquery_rhs_non_scalar`: that case has a DECIDE
        variable inside the subquery (binder rejection); this case has a
        scalar-expected subquery that returns multiple rows (executor-level
        rejection from DuckDB).
        """
        decidb_cli.assert_error(
            """
                WITH t AS (SELECT 1 AS l_quantity UNION ALL SELECT 2)
                SELECT l_quantity FROM t
                DECIDE x(INT)
                SUCH THAT SUM(x) <= (SELECT l_quantity FROM t)
                MAXIMIZE SUM(x)
            """,
            match=r"More than one row returned by a subquery",
        )

    # `SUM(x*v) <= SUM(y*v)` used to be rejected here, sharing this file's "not a
    # scalar or aggregate without DECIDE variables" message because the bound was
    # itself a reducer over decision variables. the canonicalization refactor made the gate
    # side-agnostic and the shape solves; it is now oracle-verified in
    # test_canonicalize_side_agnostic.py::test_aggregate_vs_aggregate.

    def test_data_only_minmax_rhs_aggregate_now_supported(self, decidb_cli):
        """MIN/MAX on the RHS used to be refused because the left side could only
        reduce a data term by summing it. The reducer evaluator removes that limit,
        so this now solves: MIN(val) = 10 caps SUM(x*val), giving x = (1, 0)."""
        rows, cols = decidb_cli.execute(
            """
                WITH t AS (SELECT 1 AS id, 10 AS val UNION ALL SELECT 2, 20)
                SELECT id, val, x FROM t
                DECIDE x(INT)
                SUCH THAT SUM(x * val) <= MIN(val)
                MAXIMIZE SUM(x * val)
            """
        )
        xi, vi = cols.index("x"), cols.index("val")
        total = sum(int(round(float(r[xi]))) * float(r[vi]) for r in rows)
        assert total == 10.0, rows

    # --- Unsupported aggregates rejected at DecideBinder::BindAggregate ---

    def test_unsupported_aggregate_rejected(self, decidb_cli):
        """Non-whitelisted aggregates (BIT_AND, STRING_AGG, MEDIAN, ...) are rejected in DECIDE clauses."""
        decidb_cli.assert_error_at(
            """
                SELECT l_quantity FROM lineitem
                DECIDE x(INT)
                SUCH THAT BIT_AND(x) >= 0
                MAXIMIZE SUM(x * l_quantity) LIMIT 1
            """,
            kind="Not implemented Error",
            at="BIT_AND(x)",
        )

    def test_count_over_decide_variable_rejected(self, decidb_cli):
        """COUNT(x) where x is a DECIDE variable is degenerate (always = row count)."""
        decidb_cli.assert_error_at(
            """
                SELECT l_quantity FROM lineitem
                DECIDE x(INT)
                SUCH THAT COUNT(x) >= 5
                MAXIMIZE SUM(x * l_quantity) LIMIT 1
            """,
            kind="Not implemented Error",
            at="COUNT(x)",
        )

    # --- Non-linear scalar functions wrapping a DECIDE variable ---

    # Any function outside `+ - * / abs` and the `__` markers takes the one branch of `CheckVocabulary`, so one name
    # stands for the rest: in a per-row constraint (`CheckConstraints`) here, in an objective below (`CheckObjective`).
    @pytest.mark.parametrize("fn", ["sqrt"])
    def test_nonlinear_scalar_per_row_lhs(self, decidb_cli, fn):
        """f(x) op K as per-row constraint — silently produced wrong answers before
        the bind-time whitelist check. Each non-linear scalar wrapping a DECIDE
        variable should now be rejected with a uniform error."""
        decidb_cli.assert_error_at(
            f"""
                SELECT l_quantity FROM lineitem
                DECIDE x(REAL)
                SUCH THAT {fn}(x) <= 2
                MAXIMIZE SUM(x * l_quantity) LIMIT 1
            """,
            kind="Not implemented Error",
            at=f"{fn}(x)",
        )

    @pytest.mark.parametrize("fn", ["sqrt"])
    def test_nonlinear_scalar_inside_sum(self, decidb_cli, fn):
        """SUM(f(x)) — crashed the symbolic layer with InternalException before.
        Now a clean BinderException."""
        decidb_cli.assert_error_at(
            f"""
                SELECT l_quantity FROM lineitem
                DECIDE x(REAL)
                SUCH THAT x <= 5
                MAXIMIZE SUM({fn}(x)) LIMIT 1
            """,
            kind="Not implemented Error",
            at=f"{fn}(x)",
        )

    def test_nonlinear_scalar_inside_abs(self, decidb_cli):
        """ABS(f(x)) — ABS passes its child through opaquely, so sqrt inside
        ABS used to FATAL the session. The recursive walk catches it."""
        decidb_cli.assert_error_at(
            """
                SELECT l_quantity FROM lineitem
                DECIDE x(REAL)
                SUCH THAT x <= 5
                MAXIMIZE SUM(ABS(sqrt(x) - 1)) LIMIT 1
            """,
            kind="Not implemented Error",
            at="sqrt(x)",
        )

    # --- POWER over a decision ---
    # The shape check judges POWER on the canonical tree: a whole exponent is a product of
    # factors, which the degree rule then judges; any other exponent has no reading in the
    # model. POWER(x, 0) and POWER(x, 1) answer (test_product_normal_form.py).

    # `TryGetDecideWholeExponent` refuses a fraction (`value != floor(value)`: 0.5 stands for 2.5) and a negative
    # (`value < 0.0`: -1) in separate clauses of one condition.
    @pytest.mark.parametrize("exp", ["0.5", "-1"])
    def test_power_bad_constant_exponent_in_sum(self, decidb_cli, exp):
        """SUM(POWER(x, <not a whole number>)) has no reading in the model."""
        decidb_cli.assert_error_at(
            f"""
                SELECT l_quantity FROM lineitem
                DECIDE x(REAL)
                SUCH THAT x <= 5
                MINIMIZE SUM(POWER(x, {exp})) LIMIT 1
            """,
            kind="Not implemented Error",
            at=f"POWER(x, {exp})",
        )

    # 3 is the first whole exponent past degree 2, so the degree rule judges it; every larger one reaches that branch.
    @pytest.mark.parametrize("exp", ["3"])
    def test_power_whole_exponent_above_two_is_a_degree_refusal(self, decidb_cli, exp):
        """SUM(POWER(x, 3)) is x * x * x: the degree rule refuses it, not the exponent rule."""
        decidb_cli.assert_error_at(
            f"""
                SELECT l_quantity FROM lineitem
                DECIDE x(REAL)
                SUCH THAT x <= 5
                MINIMIZE SUM(POWER(x, {exp})) LIMIT 1
            """,
            kind="Not implemented Error",
            at=f"POWER(x, {exp})",
        )

    def test_power_variable_exponent_rejected(self, decidb_cli):
        """POWER(x, x) — exponent is itself a decide var; must be a constant."""
        decidb_cli.assert_error_at(
            """
                SELECT l_quantity FROM lineitem
                DECIDE x(REAL)
                SUCH THAT x <= 5
                MINIMIZE SUM(POWER(x, x)) LIMIT 1
            """,
            kind="Not implemented Error",
            at="POWER(x, x)",
        )

    def test_power_bad_exponent_per_row_constraint(self, decidb_cli):
        """POWER(x, 0.5) <= K as per-row (no SUM) — same clean rejection as inside SUM."""
        decidb_cli.assert_error_at(
            """
                SELECT l_quantity FROM lineitem
                DECIDE x(REAL)
                SUCH THAT POWER(x, 0.5) <= 3
                MAXIMIZE SUM(x) LIMIT 1
            """,
            kind="Not implemented Error",
            at="POWER(x, 0.5)",
        )

    def test_power_over_data_column_allowed(self, decidb_cli):
        """POWER(col, 3) where col is a table column is fine — folds to a
        per-row constant. Only POWER wrapping a DECIDE variable is gated."""
        # Just ensure no BinderException; correctness of POWER-of-data is a
        # separate pre-existing question.
        decidb_cli.execute(
            """
                SELECT l_quantity, x FROM lineitem WHERE l_orderkey < 10
                DECIDE x(BOOL)
                SUCH THAT SUM(x * POWER(l_quantity, 2)) <= 1000
                MAXIMIZE SUM(x * l_quantity) LIMIT 1
            """
        )

    def test_power_wrapping_bilinear_rejected(self, decidb_cli):
        """POWER(x * y, 2) — base is bilinear, so total degree is 4.

        The fault is that squaring a bilinear base reaches degree 4, not the bilinear base,
        which an objective accepts. The degree rule says so directly."""
        decidb_cli.assert_error_at(
            """
                SELECT l_quantity FROM lineitem
                DECIDE x(REAL), y(REAL)
                SUCH THAT x <= 5 AND y <= 5
                MINIMIZE SUM(POWER(x * y, 2)) LIMIT 1
            """,
            kind="Not implemented Error",
            at="POWER(x * y, 2)",
        )

    # `ReadProduct` sums power * degree over the factors in one loop, so a second squared factor adds no branch.
    @pytest.mark.parametrize("expr", ["POWER(x, 2) * y"])
    def test_power_degree_rejected_in_constraint(self, decidb_cli, expr):
        """POWER contributes degree(base) * n, so these reach the degree rule.

        Before the degree function understood POWER, POWER(x, 2) counted as
        degree 1 and these were refused much later by physical extraction with a
        message naming a normalization pass that no longer exists. Pin the
        rejection at the shape check, where it is worded for a user."""
        decidb_cli.assert_error_at(
            f"""
                SELECT l_quantity FROM lineitem
                DECIDE x(REAL), y(REAL)
                SUCH THAT SUM({expr}) <= 10 AND x <= 5 AND y <= 5
                MAXIMIZE SUM(x) LIMIT 1
            """,
            kind="Not implemented Error",
            at=expr,
        )

    def test_power_degree_rejected_in_objective(self, decidb_cli):
        """Same shape in an objective takes the same path and the same error."""
        decidb_cli.assert_error_at(
            """
                SELECT l_quantity FROM lineitem
                DECIDE x(REAL), y(REAL)
                SUCH THAT x <= 5 AND y <= 5
                MINIMIZE SUM(POWER(x, 2) * y) LIMIT 1
            """,
            kind="Not implemented Error",
            at="POWER(x, 2) * y",
        )

    # Four nestings `ReadDecideProduct` flattens to a degree past 2: a power of a product, a product of products, a
    # power of a power, and three factors.
    @pytest.mark.parametrize(
        "constraint",
        [
            "POWER(x * y, 2) <= 5",
            "(x * y) * (x * y) <= 5",
            "POWER(POWER(x, 2), 2) <= 5",
            "x * y * x <= 5",
        ],
    )
    def test_degree_refused_on_a_per_row_constraint(self, decidb_cli, constraint):
        """A per-row constraint is held to the same degree rule as a reducer argument.

        These four used to bind cleanly and be refused at plan time by term extraction,
        because the degree gate ran for `SUM(...)` arguments alone. Degree is one
        judgement with one owner, the shape check, so the spelling must not decide which
        layer refuses."""
        decidb_cli.assert_error_at(
            f"""
                SELECT l_quantity FROM lineitem
                DECIDE x(INT), y(INT)
                SUCH THAT {constraint} AND SUM(x) >= 1
                MAXIMIZE SUM(x) LIMIT 1
            """,
            kind="Not implemented Error",
            at=constraint.split(" <=")[0],
        )

    def test_degree_refusal_points_at_the_offending_term(self, decidb_cli):
        """The refusal carries a source location.

        The shape check judges the canonical tree, so a respelled product keeps the source
        location of the term as written; a plan-time refusal could not point at all."""
        decidb_cli.assert_error(
            """
                SELECT l_quantity FROM lineitem
                DECIDE x(INT), y(INT)
                SUCH THAT x * y * x <= 5 AND SUM(x) >= 1
                MAXIMIZE SUM(x) LIMIT 1
            """,
            match=r"LINE \d+:",
        )

    # Each spelling of degree 2 takes its own route through the walk.
    @pytest.mark.parametrize(
        "constraint",
        [
            "x * y <= 5",  # bilinear: two different decisions
            "POWER(x, 2) <= 5",  # quadratic by squaring
            "x * x <= 5",  # quadratic by self-product
        ],
    )
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
        except DeciDBCliError as e:
            assert (
                "is not supported on this installation" in e.message
            ), f"degree 2 was rejected before reaching a backend: {e.message}"
            return
        assert rows

    def test_power_squared_scaled_by_constant_still_accepted(self, decidb_cli):
        """Guard the other side of the degree change: a constant factor adds no
        degree, so `3 * POWER(x, 2)` stays quadratic and must keep working."""
        rows, _ = decidb_cli.execute(
            """
                SELECT l_quantity FROM lineitem
                DECIDE x(REAL)
                SUCH THAT x <= 5
                MINIMIZE SUM(3 * POWER(x, 2)) LIMIT 1
            """
        )
        assert rows

    # The NULL bound as written, after the decision's side was rebuilt, and on the left.
    @pytest.mark.parametrize(
        "constraint, at",
        [
            # The bound canonicalization left stands where the user wrote it, and the error points at it.
            ("x <= CAST(NULL AS DOUBLE)", "CAST(NULL AS DOUBLE)"),
            # A bound canonicalization rebuilt has no place of its own: the error points at the clause.
            ("x + 3 <= CAST(NULL AS DOUBLE)", "<="),
            ("CAST(NULL AS DOUBLE) >= x + 3", ">="),
        ],
    )
    def test_perrow_null_rhs_rejected(self, decidb_cli, constraint, at):
        """Direct, rebuilt and reversed known-NULL bounds fail during planning."""
        decidb_cli.assert_error_at(
            f"""
                SELECT l_quantity FROM lineitem
                DECIDE x(REAL)
                SUCH THAT {constraint}
                MAXIMIZE SUM(x) LIMIT 1
            """,
            kind="Binder Error",
            at=at,
            word="is NULL",
        )

    # --- WHEN error cases ---

    @pytest.mark.when_constraint
    def test_when_decide_variable_in_condition(self, decidb_cli):
        """WHEN condition must not reference DECIDE variables."""
        decidb_cli.assert_error_at(
            """
                SELECT l_quantity FROM lineitem
                DECIDE x(INT)
                SUCH THAT when x = 1: SUM(x * l_quantity) <= 50
                MAXIMIZE SUM(x * l_quantity) LIMIT 1
            """,
            kind="Binder Error",
            at="x",
        )

    @pytest.mark.when_compound
    def test_when_decide_variable_in_compound_condition(self, decidb_cli):
        """WHEN compound condition must not hide a DECIDE variable."""
        decidb_cli.assert_error_at(
            """
                SELECT l_quantity FROM lineitem
                DECIDE x(INT)
                SUCH THAT when (x = 1 AND l_returnflag = 'R'): x <= 1
                MAXIMIZE SUM(x * l_quantity) LIMIT 1
            """,
            kind="Binder Error",
            at="x",
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
        decidb_cli.assert_error_at(
            """
                SELECT ps_partkey, ps_suppkey, x
                FROM partsupp
                WHERE ps_partkey < 10
                DECIDE x(INT)
                SUCH THAT SUM(x) = (SELECT CAST(p_size AS INTEGER) FROM part
                                    WHERE p_partkey = ps_partkey)
                MAXIMIZE SUM(x)
            """,
            kind="Invalid Input Error",
            at="(SELECT CAST(p_size AS INTEGER) FROM part",
        )

    # --- Division with a DECIDE variable in the divisor ---
    # `x / 2` and `x / data_col` are linear (coefficient scaling); `x / y`
    # where both are DECIDE variables is non-linear and rejected. Before the
    # whitelist tightening, per-row `x / y` was silently accepted with
    # nonsensical results (y=0 in the optimal solution).

    def test_perrow_division_by_decide_var_rejected(self, decidb_cli):
        """`x / y <= K` where both are DECIDE variables — must reject."""
        decidb_cli.assert_error_at(
            """
                WITH t AS (SELECT 1 AS id UNION ALL SELECT 2)
                SELECT id, x, y FROM t
                DECIDE x(REAL), y(REAL)
                SUCH THAT x / y <= 5
                    AND x <= 10
                    AND y <= 10
                MAXIMIZE SUM(x)
            """,
            kind="Not implemented Error",
            at="x / y",
        )

    def test_sum_division_by_decide_var_rejected(self, decidb_cli):
        """`SUM(x / y) <= K` with both DECIDE variables — must reject cleanly
        instead of crashing in symbolic normalization."""
        decidb_cli.assert_error_at(
            """
                WITH t AS (SELECT 1 AS id UNION ALL SELECT 2)
                SELECT id, x, y FROM t
                DECIDE x(REAL), y(REAL)
                SUCH THAT SUM(x / y) <= 5
                    AND x <= 10
                    AND y <= 10
                MAXIMIZE SUM(x)
            """,
            kind="Not implemented Error",
            at="x / y",
        )

    def test_objective_division_by_decide_var_rejected(self, decidb_cli):
        """Same rejection from the objective path."""
        decidb_cli.assert_error_at(
            """
                WITH t AS (SELECT 1 AS id UNION ALL SELECT 2)
                SELECT id, x, y FROM t
                DECIDE x(REAL), y(REAL)
                SUCH THAT x <= 10 AND y <= 10
                MAXIMIZE SUM(x / y)
            """,
            kind="Not implemented Error",
            at="x / y",
        )

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

    # Where the OR hides: at the top, under an AND, nested, between reducers, and under each scope wrapper.
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
        decidb_cli.assert_error_at(
            f"""{TABLES}
            SELECT id, x FROM t DECIDE x(INT) SUCH THAT {clause} MAXIMIZE SUM(x)""",
            kind="Not implemented Error",
            at="OR",
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
        decidb_cli.assert_error_at(
            f"""{TABLES}
            SELECT id, x FROM t DECIDE x(INT) {clauses}""",
            kind="Binder Error",
            at="random()",
        )

    def test_volatile_function_in_the_source_is_data(self, decidb_cli):
        """A value computed once per row before DECIDE is ordinary data."""
        rows = decidb_cli.execute(
            f"""{TABLES}
            SELECT id, x FROM (SELECT *, random() * 0 + 4 AS r FROM t) s
            DECIDE x(INT) SUCH THAT sum(x) <= sum(r) MAXIMIZE SUM(x)"""
        )[0]
        assert sum(int(row[1]) for row in rows) == 16

    # One walk over the whole constraint (`ValidateDecideNoDataAggregateFactor`) tests each `/`, with no case on a
    # scope marker or an aggregate's name (`IsAggregateFunctionName`). The clauses differ in the dividend (a bare
    # decision, a reducer), the divisor (`count(*)` has no argument, a sum nested in arithmetic), a reducer holding
    # data inside a decision's body, and the side of the comparison. The scope spellings run on one clause.
    @pytest.mark.parametrize(
        "spelling,clause",
        [
            pytest.param(spelling, clause, id=f"{clause}-{spelling}")
            for spelling, clause in [
                ("", "x / sum(cap) <= 1"),
                ("", "sum(x) / sum(cap) <= 1"),
                ("per row: ", "sum(x) / sum(cap) <= 1"),
                ("per (): ", "sum(x) / sum(cap) <= 1"),
                ("", "sum(x) / count(*) <= 2"),
                ("", "sum(x * cap) / sum(cap) <= 5"),
                ("", "sum(x) / (1 + sum(cap)) <= 1"),
                ("", "x <= sum(x) / sum(cap)"),
            ]
        ],
    )
    def test_data_only_aggregate_divisor_rejected(self, decidb_cli, spelling, clause):
        """A data-only aggregate stays on the bound side (S10). As a divisor of a
        decision term it is refused like the same aggregate as a multiplier, never
        reaching term extraction ('INTERNAL Error: ... VALUE shape reached term
        extraction')."""
        decidb_cli.assert_error_at(
            f"""{TABLES}
            SELECT id, x FROM t DECIDE x(INT) SUCH THAT x <= cap AND {spelling}{clause} MAXIMIZE SUM(x)""",
            kind="Binder Error",
            at="count(*)" if "count(*)" in clause else "sum(cap)",
        )

    # The same walk, the other message (the divisor holds no data-only aggregate). The clauses differ in the side of
    # the comparison, `count(*)`, a negation over the division, and a divisor that is a subquery; the five spellings
    # run on the first clause.
    @pytest.mark.parametrize(
        "spelling,clause",
        [
            pytest.param(spelling, clause, id=f"{clause}-{spelling}")
            for spelling, clause in [
                (spelling, "(sum(x) + sum(cap)) / 2 <= 20") for spelling in SPELLINGS + ["per g: ", "when id > 0: "]
            ]
            + [
                ("", "20 >= (sum(x) + sum(cap)) / 2"),
                ("", "(sum(x) + count(*)) / 2 <= 5"),
                ("", "-(sum(x) + sum(cap)) / 2 >= -20"),
                ("", "(sum(x) + sum(cap)) / (SELECT 2) <= 20"),
            ]
        ],
    )
    def test_data_only_aggregate_in_a_divided_term_rejected(self, decidb_cli, spelling, clause):
        """A data-only aggregate divided together with a decision term is the scaled term
        the multiplier rule already refuses (`2 * (sum(x) - avg(cap))`); under a scope
        spelling it, too, reached term extraction as an internal error."""
        decidb_cli.assert_error_at(
            f"""{TABLES}
            SELECT id, x FROM t DECIDE x(INT) SUCH THAT x <= cap AND {spelling}{clause} MAXIMIZE SUM(x)""",
            kind="Binder Error",
            at="count(*)" if "count(*)" in clause else "sum(cap)",
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
        decidb_cli.assert_error_at(
            f"""{TABLES}
            SELECT id, x FROM t DECIDE x(INT) SUCH THAT x <= cap
            MAXIMIZE per (): sum(x) + (SELECT k FROM f WHERE f.g = t.g)""",
            kind="Binder Error",
            at="(SELECT k FROM f WHERE f.g = t.g)",
        )

    @pytest.mark.parametrize(
        "term, quoted",
        [
            # The left of an IN and an operator function's operands have nothing around them to delimit them.
            pytest.param("sum(when (t.p AND t.q) IN (false): t.cap)", "(t.p AND t.q) IN (", id="an-and-left-of-an-in"),
            pytest.param(
                "sum(when ((t.p = t.q) || 'x') = 'truex': t.cap)",
                "((t.p = t.q) || 'x')",
                id="a-comparison-under-an-operator",
            ),
        ],
    )
    def test_a_printed_operand_reads_as_written(self, decidb_cli, term, quoted):
        result = decidb_cli.execute_raw(
            f"""SELECT id, x FROM (VALUES (1, 3, true, false), (2, 4, false, true)) t(id, cap, p, q)
            DECIDE x(REAL) SUCH THAT x <= 7 MAXIMIZE sum(x) - {term}"""
        )
        assert quoted in result.stderr, result.stderr

    @pytest.mark.parametrize(
        "term",
        [
            "0.5 * norm(t.cap, 'inf')",
            "norm(t.cap, 'inf') / 2",
            "0.5 * sum(t.cap)",
            "sum(t.cap) by (t.id)",
            "sum((SELECT 1))",
            "norm(CAST(t.cap AS DOUBLE), 2)",
            "sum(CAST(abs(CAST(t.cap AS DOUBLE)) AS INTEGER))",
            "CAST(sum(q := CAST(t.cap AS DOUBLE)) AS INTEGER)",
            'CAST(sum("my q" := CAST(t.cap AS DOUBLE)) AS INTEGER)',
        ],
        ids=[
            "a-norm-under-a-factor",
            "a-norm-under-a-divisor",
            "a-reducer-under-a-factor",
            "a-reducer-with-its-by",
            "a-subquery-as-a-reducers-argument",
            "a-cast-as-a-norms-body",
            "a-cast-under-a-function-under-a-cast",
            "a-written-argument-name-stays",
            "a-quoted-argument-name-stays",
        ],
    )
    def test_a_decision_free_term_is_quoted_as_written(self, decidb_cli, term):
        """Binding casts a factor's operand, and a cast prints its child by its alias, so the bound name held the tag
        `__written_norm_inf__`; the term's own spelling holds no internal name (D40)."""
        decidb_cli.assert_error_at(
            f"SELECT id, x FROM (VALUES (1, 3), (2, 4)) t(id, cap) DECIDE x(REAL) SUCH THAT x <= 7 MAXIMIZE sum(x) - {term}",
            kind="Binder Error",
            at=term,
            word=f"'{term}'",
        )


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
            ("x IS DISTINCT FROM 3", "'x IS DISTINCT FROM 3'"),  # a comparison DECIDE does not read
            ("NOT (x <= 1 OR x >= 3)", "'NOT (x <= 1 OR x >= 3)'"),  # an operator it does not read
            ("1", "'1'"),  # a clause of no kind it reads
        ],
    )
    def test_unsupported_constraint_is_quoted(self, decidb_cli, clause, quoted):
        stderr = self._refusal(decidb_cli, clause)
        assert f"SUCH THAT does not support {quoted}" in stderr, stderr

    @pytest.mark.parametrize(
        "clause,at",
        [
            ("x = (x > 2)", "x > 2"),  # a comparison over a decision as an operand
            ("x >= (x BETWEEN 1 AND 3)", "x BETWEEN 1 AND 3"),  # the same, a BETWEEN
            ("x <= (x > 1 AND cap > 1)", "x > 1 AND cap > 1"),  # the same, an AND
        ],
    )
    def test_a_comparison_used_as_a_number_is_refused_at_it(self, decidb_cli, clause, at):
        decidb_cli.assert_error_at(
            f"{_REFUSAL_ROWS} DECIDE x(INT) SUCH THAT x <= 5 AND {clause} MAXIMIZE SUM(x)",
            kind="Binder Error",
            at=at,
            word=f"'{at}'",
        )

    def test_unsupported_reducer_body_is_quoted(self, decidb_cli):
        # A comparison over data is a 0/1 coefficient (test_data_operands.py); over a
        # decision it is refused.
        decidb_cli.assert_error_at(
            f"{_REFUSAL_ROWS} DECIDE x(INT) SUCH THAT x <= 5 AND SUM(x * (x BETWEEN 1 AND 2)) <= 3 MAXIMIZE SUM(x)",
            kind="Binder Error",
            at="x BETWEEN 1 AND 2",
            word="'x BETWEEN 1 AND 2'",
        )

    # One per message the objective check words: no decision (a constant, a data column), a comparison, `count(*)`.
    @pytest.mark.parametrize(
        "objective,kind,at",
        [
            ("MAXIMIZE 3", "Binder Error", "3"),
            ("MINIMIZE cap", "Binder Error", "cap"),
            ("MAXIMIZE x > 1", "Binder Error", "x > 1"),
            ("MAXIMIZE count(*)", "Not implemented Error", "count(*)"),
        ],
    )
    def test_unsupported_objective_is_quoted(self, decidb_cli, objective, kind, at):
        decidb_cli.assert_error_at(
            f"{_REFUSAL_ROWS} DECIDE x(INT) SUCH THAT x <= 5 AND SUM(x) <= 9 {objective}",
            kind=kind,
            at=at,
            word=f"'{at}'",
        )

    def test_count_star_in_a_constraint_is_quoted_as_written(self, decidb_cli):
        decidb_cli.assert_error_at(
            f"{_REFUSAL_ROWS} DECIDE x(INT) SUCH THAT x <= 5 AND count(*) <= 3 MAXIMIZE SUM(x)",
            kind="Not implemented Error",
            at="count(*)",
            word="'count(*)'",
        )

    def test_reducer_over_a_constant_is_not_a_query_wide_decision(self, decidb_cli):
        """`SUM(1)` has no decision at all; only a body made of query-wide decisions is called one."""
        decidb_cli.assert_error_at(
            f"{_REFUSAL_ROWS} DECIDE x(INT) SUCH THAT x <= 5 AND SUM(1) <= 5 MAXIMIZE SUM(x)",
            kind="Binder Error",
            at="SUM(1)",
        )


_BRANCH_ROWS = (
    "SELECT id, x FROM (VALUES (1, 10.0, 1, 2), (2, 20.0, 1, 3), (3, 5.0, 2, 1), (4, 8.0, 2, 4)) t(id, w, k, c) "
)


class At(NamedTuple):
    """Where a refusal points and of what kind (`assert_error_at`), for a case whose message has no wording to match."""

    kind: str
    at: str


def _branch_refusal(decidb_cli, tail, message, rows=_BRANCH_ROWS):
    if isinstance(message, At):
        decidb_cli.assert_error_at(f"{rows}DECIDE x(INT) {tail}", kind=message.kind, at=message.at)
        return
    result = decidb_cli.execute_raw(f"{rows}DECIDE x(INT) {tail}")
    assert re.search(message, result.stderr), result.stderr
    assert_no_internal_leak(result, "a refused clause")


@pytest.mark.error_binder
@pytest.mark.error
class TestBinderRefusalBranches:
    """One refusal per binder branch that no other refusal reaches; the comment on each case names its branch."""

    @pytest.mark.parametrize(
        "clause, message",
        [
            # A constraint that is a plain function call: DecideConstraintsBinder::BindExpressionInternal's FUNCTION case
            # at the top, and ValidateDecideNoIntegerStepComparisonOnReal's FUNCTION case that is no scope marker.
            ("abs(x)", r"SUCH THAT does not support 'abs\(x\)'"),
            # The same for a CASE at the top.
            ("CASE WHEN c > 1 THEN true ELSE false END", r"SUCH THAT does not support 'CASE"),
            # A subquery that is not scalar, and one that reads a decision (DecideBinder::BindExpressionInternal).
            ("x IN (SELECT 1)", At("Binder Error", "IN (SELECT 1)")),
            ("x <= (SELECT x)", At("Binder Error", "(SELECT x)")),
            # A `when` whose condition, or a reducer's, does not bind (BindScopedConstraint, BindScopedReducer).
            ("when nosuch > 0: x <= 3", r'Referenced column "nosuch" not found'),
            ("SUM(when nosuch > 0: x) <= 3", r'Referenced column "nosuch" not found'),
            ("max(x, 2) <= 2", At("Binder Error", "max(x, 2)")),
            # A COALESCE over a reducer, in the body of a sum's left side (ContainsDecideAggregate's OPERATOR case).
            ("COALESCE(SUM(x), 0) + 1 <= 100", At("Not implemented Error", "COALESCE(SUM(x), 0)")),
        ],
    )
    def test_constraint_shape(self, decidb_cli, clause, message):
        _branch_refusal(decidb_cli, f"SUCH THAT x <= 3 AND {clause} MAXIMIZE SUM(x)", message)

    @pytest.mark.parametrize(
        "bound",
        [
            "sum(w) OVER ()",  # a class IsAllowedDecisionFreeBoundExpression does not list
            "1 + sum(w) OVER ()",  # IsAllowedOperatorChildren
            "abs(sum(w) OVER ())",  # a function's child
            "CAST((sum(w) OVER () = 1) AS INT)",  # a comparison's child, after an allowed first one
            "(EXISTS (SELECT 1))",  # a subquery that is not scalar
            "max(w) FILTER (WHERE EXISTS (SELECT 1))",  # a reducer's filter
            "max(w, 2)",  # a reducer with two arguments
            "max(w, 2) by (k)",  # the same under a `by`
            "abs(w) FILTER (WHERE EXISTS (SELECT 1))",  # a scalar function's filter that is not allowed
        ],
    )
    def test_bound_side(self, decidb_cli, bound):
        _branch_refusal(
            decidb_cli, f"SUCH THAT x <= 3 AND SUM(x) <= {bound} MAXIMIZE SUM(x)", At("Binder Error", bound)
        )

    def test_bound_side_on_the_left(self, decidb_cli):
        """The decision-free side is the left one: IsValidBound is asked of it."""
        _branch_refusal(
            decidb_cli,
            "SUCH THAT x <= 3 AND sum(w) OVER () >= SUM(x) MAXIMIZE SUM(x)",
            At("Binder Error", "sum(w) OVER ()"),
        )

    def test_bound_side_filter_on_a_scalar_function(self, decidb_cli):
        """An allowed filter on a scalar function is left to DuckDB, which refuses it."""
        _branch_refusal(
            decidb_cli,
            "SUCH THAT x <= 3 AND SUM(x) <= abs(w) FILTER (WHERE true) MAXIMIZE SUM(x)",
            r"only applicable to aggregate functions",
        )

    @pytest.mark.parametrize(
        "argument, message",
        [
            ("abs(x, 1)", At("Binder Error", "abs(x, 1)")),
            # A non-scalar subquery, first among a product's factors (the second is then not asked).
            ("EXISTS (SELECT 1) * x", At("Binder Error", "EXISTS (SELECT 1)")),
            ("x * (SELECT x)", At("Binder Error", "(SELECT x)")),
            # A subquery over a per-row decision is not "a query-wide decision": IsRowInvariantExpression reads its body.
            ("2 * (SELECT x)", At("Binder Error", "(SELECT x)")),
            # The same as a call's lone argument, which a call prints with its alias.
            ("(SELECT x)", At("Binder Error", "(SELECT x)")),
        ],
    )
    def test_reducer_argument(self, decidb_cli, argument, message):
        _branch_refusal(decidb_cli, f"SUCH THAT x <= 3 AND SUM({argument}) <= 30 MAXIMIZE SUM(x)", message)

    @pytest.mark.parametrize(
        "constraint, message",
        [
            # A colon form over a function that is no reducer, then a reducer that cannot be qualified, and over a
            # reducer that fails to bind.
            ("abs(t: x) <= 5", At("Binder Error", "abs(t: x)")),
            ("SUM(x) + count(t: w) <= 30", At("Binder Error", "count(t: w)")),
            ("count(t: x) <= 5", At("Not implemented Error", "count(")),
            # The relation of the decisions.
            ("sum(decide_variables: x) <= 5", At("Binder Error", "decide_variables")),
        ],
    )
    def test_colon_form(self, decidb_cli, constraint, message):
        _branch_refusal(decidb_cli, f"SUCH THAT x <= 3 AND {constraint} MAXIMIZE SUM(x)", message)

    @pytest.mark.error_parser
    def test_a_qualified_name_before_a_colon_is_a_parser_error(self, decidb_cli):
        """A reducer's qualifier is one name per relation: a qualified name is refused where it is written."""
        decidb_cli.assert_error_at(
            f"{_BRANCH_ROWS}DECIDE x(INT) SUCH THAT x <= 3 AND sum(t.u: x) <= 5 MAXIMIZE SUM(x)",
            kind="Parser Error",
            at="t.u",
        )

    @pytest.mark.parametrize(
        "key, message",
        [
            # Three names that name no relation (RelationAlias's catalog.schema.alias case), and five that are a
            # column's path past any relation.
            ("per a.b.c: SUM(x) <= 3", At("Binder Error", "a.b.c")),
            ("per t.s.a.b.c: SUM(x) <= 3", At("Binder Error", "t.s.a.b.c")),
            # A `by` key of four names that name no column: it is data, and the catalog is not found.
            ("SUM(x) by (a.b.t.nosuch) <= 3", r'Referenced table "a" not found'),
        ],
    )
    def test_key_names(self, decidb_cli, key, message):
        _branch_refusal(decidb_cli, f"SUCH THAT x <= 3 AND {key} MAXIMIZE SUM(x)", message)

    @pytest.mark.parametrize(
        "objective, message",
        [
            # A COUNT over a decision inside a nest.
            ("sum(per k: count(x) by (k))", At("Not implemented Error", "count(x)")),
            # A nested reducer with no argument: ValidateSumArgumentInternal's one-child guard keeps it from reading
            # children[0], and it reads as a reducer over data.
            ("sum(per k: max() by (k))", At("Binder Error", "sum(per k: max() by (k))")),
            # An aggregate with arguments no function takes.
            ("SUM(w * x, 2)", At("Binder Error", "SUM(w * x, 2)")),
            # MAX(x, n) is DuckDB's top-n aggregate, a list: the objective refuses it as a constraint does.
            ("max(x, 2)", At("Binder Error", "max(x, 2)")),
            # A scope whose body, or condition, does not bind.
            ("when c > 0: sum(nosuch * x)", r'Referenced column "nosuch" not found'),
            ("when nosuch > 0: sum(w * x)", r'Referenced column "nosuch" not found'),
            # An operator or CASE at the top, an operator below it, a product of data, a quotient by a reducer.
            ("COALESCE(SUM(x), 0)", r"does not support 'COALESCE\(SUM\(x\), 0\)'"),
            ("SUM(x) + COALESCE(x, 1)", At("Not implemented Error", "COALESCE(x, 1)")),
            ("CASE WHEN c > 1 THEN 1 ELSE 2 END", At("Binder Error", "CASE WHEN c > 1 THEN 1 ELSE 2 END")),
            ("w * 2", At("Not implemented Error", "w * 2")),
            ("SUM(x * w) / SUM(x)", At("Not implemented Error", "SUM(x * w) / SUM(x)")),
            # `"/"` called by name takes any number of arguments, one here.
            ('"/"(SUM(x))', At("Not implemented Error", '"/"(SUM(x))')),
        ],
    )
    def test_objective_shape(self, decidb_cli, objective, message):
        _branch_refusal(decidb_cli, f"SUCH THAT x <= 3 MINIMIZE {objective}", message)


@pytest.mark.error_binder
@pytest.mark.error
class TestTagNamesRefused:
    """A decision, or a column a clause reads, whose name contains a DECIDE tag is refused (R20) until the tags are
    gone; the refusal names the identifier and nothing of the engine."""

    @pytest.mark.parametrize(
        "name, tail",
        [
            # DecideBinder::QualifyColumnName's refusal, reached from a constraint's bound side
            # (marker_names.sql query 2) and from a reducer's body (query 3).
            ("w__query_wide_value__", 'SUCH THAT "w__query_wide_value__" * SUM(x) <= 2 MAXIMIZE SUM(x * v)'),
            (
                "__definition_function_0__",
                'SUCH THAT x <= 5 AND SUM("__definition_function_0__" * x + v) <= 40 MAXIMIZE SUM(x)',
            ),
        ],
    )
    def test_column_read_by_a_clause(self, decidb_cli, name, tail):
        rows = f'SELECT id, x FROM (VALUES (1, 1.0, 5.0), (2, 2.0, 6.0)) t(id, "{name}", v) '
        decidb_cli.assert_error_at(f"{rows}DECIDE x(INT) {tail}", kind="Binder Error", at=f'"{name}"')

    @pytest.mark.parametrize("name", ["__per_class__", "__abs_aux_0__"])  # a tag, and a helper decision's name prefix
    def test_decision_name(self, decidb_cli, name):
        """DecideDeclarationsBinder's refusal of a declared name."""
        decidb_cli.assert_error_at(
            f'SELECT id, "{name}" FROM (VALUES (1, 5.0)) t(id, v) DECIDE "{name}"(INT) '
            f'SUCH THAT "{name}" <= 3 MAXIMIZE SUM("{name}" * v)',
            kind="Binder Error",
            at=f'"{name}"(INT)',
        )

    def test_key_column(self, decidb_cli):
        """ResolveKeyElement's refusal of a `per` key column; the key is no QualifyColumnName read."""
        decidb_cli.assert_error_at(
            'SELECT id, x FROM (VALUES (1, 1, 5.0), (2, 1, 6.0), (3, 2, 7.0)) t(id, "__frame_0__", v) '
            'DECIDE x(INT) SUCH THAT x <= 3 AND per "__frame_0__": SUM(x) <= 4 MAXIMIZE SUM(x * v)',
            kind="Binder Error",
            at='"__frame_0__"',
        )

    def test_every_tag_constant_is_in_the_list(self):
        """DecideNameContainsTag reads DECIDE_TAGS alone, so a tag constant missing from it is a name left unrefused."""
        header = (Path(__file__).resolve().parents[3] / "src/include/duckdb/common/enums/decide.hpp").read_text()
        constants = re.findall(r'static constexpr const char \*([A-Z_0-9]+) = "__', header)
        listed = re.search(r"DECIDE_TAGS\[\] = \{(.*?)\};", header, re.S).group(1)
        assert set(constants) == set(re.findall(r"[A-Z_0-9]+", listed))
