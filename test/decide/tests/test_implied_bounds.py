"""Tests for data-driven implied-bound propagation (DecidePropagateImpliedBounds).

Propagation derives finite upper bounds for otherwise-unbounded variables from
non-negative `<=`/`=` constraints (the knapsack/budget pattern). These tests
guard its soundness boundaries: it must tighten only when the bound is genuinely
implied (never cutting the optimum), and the inferred bound must correctly enable
Big-M / McCormick formulations that previously required an explicit bound.
"""

import re

import pytest

from ._brute_force import judge_rows
from ._scope_cases import answer


@pytest.fixture
def decidb_cli(decidb_cli):
    # These tests inspect the solver's model, which direct solve does not build, so they run on the solver path.
    return decidb_cli.with_env({"DECIDB_TEST_DIRECT_SOLVE": "off"})


def _column_upper_bounds(dump: str) -> list[float]:
    """Upper bounds of the dumped model's columns, in column order."""
    return [float(m) for m in re.findall(r"^col \d+: .*?\bub=(\S+)", dump, re.M)]


@pytest.mark.correctness
def test_repeated_variable_bound_uses_the_combined_coefficient(decidb_cli, tmp_path):
    """`2*ship + 3*ship <= 10` implies ship <= 2, not 10/3.

    Both terms name the same solver column, so the implied bound follows from
    their sum (5), not from either coefficient alone. Reading one term gave
    10/3 = 3.33 — sound, but 1.67x loose, and the looseness propagates into
    every Big-M derived from this column's range.

    Asserted on the model dump because results cannot see this: the emitted row
    always carried the correct combined `0:5`, so the optimum was already right
    and no result-level test could fail. Both orderings are run because the old
    behaviour picked the largest single coefficient, which is order-independent
    and would survive a one-ordering test.
    """
    sql = """
        SELECT id, ship FROM (VALUES (1)) t(id)
        DECIDE ship(INT) SUCH THAT {lhs} <= 10 MAXIMIZE SUM(ship)
    """
    # A distinct dump path per query: DECIDB_DUMP_MODEL appends, so reusing one
    # file would leave the second parse reading both models.
    for i, lhs in enumerate(("2 * ship + 3 * ship", "3 * ship + 2 * ship")):
        dump = decidb_cli.dump_model(
            sql.format(lhs=lhs), tmp_path / f"repeated_{i}.dump")
        assert _column_upper_bounds(dump) == [2.0], \
            f"`{lhs} <= 10` did not combine its coefficients:\n{dump}"


@pytest.mark.correctness
def test_single_term_bounds_are_unchanged(decidb_cli, tmp_path):
    """The combining loop must not disturb the ordinary one-term-per-variable case.

    `SUM(3*ship) <= 10` still implies ship <= 10/3, and a second variable in the same
    constraint is still bounded independently of the first. (A per-row `3*ship <= 10`
    is a bound on one decision, folded before propagation runs.)
    """
    single = decidb_cli.dump_model(
        """
            SELECT id, ship FROM (VALUES (1)) t(id)
            DECIDE ship(INT) SUCH THAT SUM(3 * ship) <= 10 MAXIMIZE SUM(ship)
        """,
        tmp_path / "single.dump",
    )
    assert _column_upper_bounds(single) == pytest.approx([10.0 / 3.0])

    two_vars = decidb_cli.dump_model(
        """
            SELECT id, ship, hold FROM (VALUES (1)) t(id)
            DECIDE ship(INT), hold(INT)
            SUCH THAT 2 * ship + 5 * hold <= 10 MAXIMIZE SUM(ship + hold)
        """,
        tmp_path / "two_vars.dump")
    assert _column_upper_bounds(two_vars) == pytest.approx([5.0, 2.0])


@pytest.mark.correctness
def test_easy_max_over_a_data_scaled_member_caps_each_column_at_what_duckdb_admits(decidb_cli, tmp_path):
    """`MAX(c * x) <= 0.3` bounds each member: with c = 0.1 the row admits 3 (`0.1 * 3` is
    the DECIMAL 0.3), with c = 0.2 it admits 1. The quotient 0.3 / 0.1 is just below 3 in
    doubles, so a column capped by it reads 2.9999999999999996; the bound is the value
    DuckDB admits, and the rows fold away."""
    dump = decidb_cli.dump_model(
        """
            SELECT id, x FROM (VALUES (1, 0.1), (2, 0.2)) t(id, c)
            DECIDE x(INT) SUCH THAT MAX(c * x) <= 0.3 MAXIMIZE SUM(x)
        """,
        tmp_path / "member.dump",
    )
    assert _column_upper_bounds(dump) == [3.0, 1.0], dump
    assert "num_rows: 0" in dump, dump


@pytest.mark.correctness
def test_propagation_multivar_knapsack(decidb_cli):
    """SUM(2x + 3y) <= 12 implies x<=6 and y<=4 (both declared-unbounded). The
    inferred bounds feed the `<>` Big-M; the optimum must be the true ILP optimum.
    max(x + 2y) s.t. 2x+3y<=12, x>=1, y>=1 (from <>0) is 7 (e.g. x=3,y=2)."""
    sql = """
        WITH data AS (SELECT 1 AS id)
        SELECT id, x, y FROM data
        DECIDE x(INT), y(INT)
        SUCH THAT 2 * x + 3 * y <= 12 AND x <> 0 AND y <> 0
        MAXIMIZE SUM(x + 2 * y)
    """
    rows, cols = decidb_cli.execute(sql)
    ci = {n: i for i, n in enumerate(cols)}
    obj = sum(float(r[ci["x"]]) + 2 * float(r[ci["y"]]) for r in rows)
    assert abs(obj - 7.0) <= 1e-6, f"expected 7, got {obj}"


@pytest.mark.correctness
def test_propagation_skips_zero_coefficient_row(decidb_cli):
    """A per-row constraint `x*w <= 10` with w=0 on one row leaves that row's x
    unconstrained, so propagation must NOT derive a shared bound from the w>0 row
    (the `every_instance_bounded` guard). The zero-coef row's x must still reach
    its declared bound. row1: 2x<=10 -> x<=5; row2: unconstrained -> x=100."""
    sql = """
        WITH data AS (SELECT 1 AS id, 2.0 AS w UNION ALL SELECT 2 AS id, 0.0 AS w)
        SELECT id, w, x FROM data
        DECIDE x(INT)
        SUCH THAT x <= 100 AND x * w <= 10
        MAXIMIZE SUM(x)
    """
    rows, cols = decidb_cli.execute(sql)
    ci = {n: i for i, n in enumerate(cols)}
    total = sum(float(r[ci["x"]]) for r in rows)
    assert abs(total - 105.0) <= 1e-6, f"expected 105, got {total}"


@pytest.mark.correctness
def test_propagation_enables_mccormick_bilinear(decidb_cli):
    """Bool x Int bilinear needs a finite bound on the integer factor. With no
    explicit bound but SUM(x) <= 10, propagation infers x<=10, so the McCormick
    envelope is finite and the query solves (previously this raised a 'finite
    upper bound' error). max SUM(b*x) with SUM(x)<=10 is 10 (all b=1)."""
    sql = """
        WITH data AS (SELECT 1 AS id UNION ALL SELECT 2 AS id)
        SELECT id, b, x FROM data
        DECIDE b(BOOL), x(INT)
        SUCH THAT SUM(x) <= 10
        MAXIMIZE SUM(b * x)
    """
    rows, cols = decidb_cli.execute(sql)
    ci = {n: i for i, n in enumerate(cols)}
    obj = sum(float(r[ci["b"]]) * float(r[ci["x"]]) for r in rows)
    assert abs(obj - 10.0) <= 1e-6, f"expected 10, got {obj}"


@pytest.mark.correctness
def test_propagation_skips_negative_coefficient(decidb_cli):
    """A constraint with a negative coefficient (`2x - y <= 4`) must be skipped:
    dropping the `-y` term to derive `x <= K/2` is invalid (it ignores that y
    raises the allowed x). With the negative-coefficient skip, x is only bounded
    by its declared `x <= 100`, so the true optimum stands. max x s.t.
    2x - y <= 4, y <= 10 is x=7 (y=10). If the skip regressed, propagation would
    cap x at 4/2 = 2 and the objective would wrongly drop to 2."""
    sql = """
        WITH data AS (SELECT 1 AS id)
        SELECT id, x, y FROM data
        DECIDE x(INT), y(INT)
        SUCH THAT 2 * x - y <= 4 AND y <= 10 AND x <= 100
        MAXIMIZE SUM(x)
    """
    rows, cols = decidb_cli.execute(sql)
    ci = {n: i for i, n in enumerate(cols)}
    obj = sum(float(r[ci["x"]]) for r in rows)
    assert abs(obj - 7.0) <= 1e-6, f"expected 7, got {obj}"


@pytest.mark.correctness
def test_propagation_skips_when_excluded_constraint(decidb_cli):
    """A when-scoped aggregate `when g = 'A': sum(x) <= 5` only constrains the
    group-A rows; group-B rows carry no such bound. Since the derived bound is
    shared across all of x's rows, propagation must skip this constraint. The
    group-B row's x must still reach its declared bound (100): x_A=5, x_B=100,
    total 105. If the WHEN-excluded skip regressed, x would be capped at 5 and the
    total would wrongly drop to 10."""
    sql = """
        WITH data AS (SELECT 'A' AS g UNION ALL SELECT 'B' AS g)
        SELECT g, x FROM data
        DECIDE x(INT)
        SUCH THAT x <= 100 AND when g = 'A': sum(x) <= 5
        MAXIMIZE SUM(x)
    """
    rows, cols = decidb_cli.execute(sql)
    ci = {n: i for i, n in enumerate(cols)}
    total = sum(float(r[ci["x"]]) for r in rows)
    assert abs(total - 105.0) <= 1e-6, f"expected 105, got {total}"


@pytest.mark.correctness
@pytest.mark.parametrize("cli_fixture", ["decidb_cli_gurobi", "decidb_cli_highs"])
def test_query_wide_decision_beside_a_reducer_is_bounded_by_the_row_it_counts_once_in(request, cli_fixture, tmp_path):
    """`SUM(x) + s <= 8` implies s <= 8. Execution keeps a query-wide decision's coefficient
    on one row of the group (it counts once), so propagation must read the row the model
    emits, where s's column has coefficient 1, not demand a coefficient on every data row.
    Without the bound, HiGHS cannot rewrite `s <> 3` and refuses the query."""
    cli = request.getfixturevalue(cli_fixture).with_env({"DECIDB_TEST_DIRECT_SOLVE": "off"})
    sql = """
        SELECT id, x, s FROM (VALUES (1), (2), (3)) t(id)
        DECIDE x(INT), per (): s(INT)
        SUCH THAT x <= 3 AND SUM(x) + s <= 8 AND s <> 3
        MAXIMIZE SUM(x) + 2 * s ORDER BY id
    """
    # Columns: x on each of the three rows, then s.
    assert _column_upper_bounds(cli.dump_model(sql, tmp_path / "beside.dump"))[:4] == [3.0, 3.0, 3.0, 8.0]
    judge_rows(
        answer(cli, sql),
        {"x0": range(4), "x1": range(4), "x2": range(4), "s": range(12)},
        lambda a: a["x0"] + a["x1"] + a["x2"] + a["s"] <= 8 and a["s"] != 3,
        lambda a: a["x0"] + a["x1"] + a["x2"] + 2 * a["s"],
    )


@pytest.mark.correctness
@pytest.mark.parametrize("cli_fixture", ["decidb_cli_gurobi", "decidb_cli_highs"])
def test_keyed_decision_is_bounded_by_its_coefficient_summed_over_its_rows(request, cli_fixture, tmp_path):
    """`SUM(w * y) <= 12` with `per g: y` emits 3*y_A + 6*y_B <= 12: each instance stands in
    two rows of the group, so its coefficient is their sum, and y_A <= 4, y_B <= 2. The
    shared bound is the looser, 4; read one data row at a time it would be 12 / 1."""
    cli = request.getfixturevalue(cli_fixture).with_env({"DECIDB_TEST_DIRECT_SOLVE": "off"})
    sql = """
        SELECT id, g, y FROM (VALUES (1, 'A', 1), (2, 'A', 2), (3, 'B', 3), (4, 'B', 3)) t(id, g, w)
        DECIDE per g: y(INT)
        SUCH THAT SUM(w * y) <= 12 AND y <> 1
        MAXIMIZE SUM(y) ORDER BY id
    """
    # Columns: y_A, y_B.
    assert _column_upper_bounds(cli.dump_model(sql, tmp_path / "keyed.dump"))[:2] == [4.0, 4.0]
    judge_rows(
        answer(cli, sql),
        {f"y{i}": range(13) for i in range(4)},
        lambda a: a["y0"] == a["y1"]
        and a["y2"] == a["y3"]
        and a["y0"] + 2 * a["y1"] + 3 * a["y2"] + 3 * a["y3"] <= 12
        and all(a[f"y{i}"] != 1 for i in range(4)),
        lambda a: sum(a[f"y{i}"] for i in range(4)),
    )
