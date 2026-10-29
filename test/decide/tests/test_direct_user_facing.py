"""What a SQL user sees of direct solve besides the answer: the default mode, error wording, memory use, and policy.

The answers themselves are checked in `test_direct_s1_uniform_matroid.py` and what every rule owes the user in
the conformance suite, `test_direct_rule_conformance.py`.
"""

import re
import math
import pytest

from ._direct_oracle_common import _allow_direct_path  # noqa: F401  (autouse fixture)
from ._direct_oracle_common import outcome


def _source_query(scores, capacity, sense="MAXIMIZE"):
    values = ", ".join(f"({i}, CAST('{score}' AS DOUBLE))" for i, score in enumerate(scores))
    return f"""
        SELECT id, score, x FROM (
            FROM (VALUES {values}) t(id, score)
            DECIDE x(BOOL) SUCH THAT SUM(x) <= {capacity}
            {sense} SUM(score * x)
        ) q ORDER BY id
    """


def _run(cli, sql, mode="require"):
    return cli.execute(f"SET decide_direct_solve='{mode}'; {sql}")


def _raw(cli, sql, mode="require", expect_large_numbers=False):
    return cli.execute_raw(f"SET decide_direct_solve='{mode}'; {sql}", expect_large_numbers=expect_large_numbers)


def _has_decide_operator(plan):
    # The operator's box title. A plain substring test would also match the DECIDE: prefix of the error text that
    # a direct plan carries.
    return re.search(r"│\s+DECIDE(_DIAGNOSE)?\s+│", plan) is not None


@pytest.mark.correctness
def test_s1_direct_reads_bounds_exactly_near_an_integer(decidb_cli):
    # Decided 2026-10-05 (S1-09): direct solve reads a bound exactly and does not copy the solver's feasibility
    # tolerance, which differs between backends. This pins that choice so a change is deliberate. The solver would
    # select 2 rows here (it accepts 1.9999999 as 2); direct selects 1.
    sql = """
        SELECT id, x FROM (
            FROM (VALUES (1, 9.0), (2, 5.0), (3, 8.0)) t(id, score)
            DECIDE x(BOOL) SUCH THAT SUM(x) <= 1.9999999 MAXIMIZE SUM(score * x)
        ) q ORDER BY id
    """
    assert [row[1] for row in _run(decidb_cli, sql)[0]] == [1, 0, 0]


@pytest.mark.correctness
def test_direct_solve_is_on_by_default(decidb_cli):
    # No SET anywhere in this test: it exercises what a user gets out of the box, so it opts out of the
    # suite-wide DECIDB_TEST_DIRECT_SOLVE override.
    decidb_cli = decidb_cli.with_env({"DECIDB_TEST_DIRECT_SOLVE": ""})
    default = decidb_cli.execute_raw("SELECT current_setting('decide_direct_solve')")
    assert "auto" in default.stdout, default.stderr

    s1 = _source_query((2.0, 9.0, -1.0), 1)
    plan = decidb_cli.execute_raw(f"EXPLAIN {s1}").stdout
    assert (
        "Direct solve rule" in plan
        and "S1_UNIFORM_MATROID" in plan
        and "WINDOW" in plan
        and not _has_decide_operator(plan)
    )
    rows, _ = decidb_cli.execute(s1)
    assert sum(row[2] for row in rows) == 1 and sum(row[1] * row[2] for row in rows) == 9.0

    # A shape no rule applies to keeps the solver plan and prints nothing about direct solve.
    weighted = """
        SELECT id, x FROM (
            FROM (VALUES (1, 2.0, 3), (2, 9.0, 2)) t(id, score, weight)
            DECIDE x(BOOL) SUCH THAT SUM(weight * x) <= 3
            MAXIMIZE SUM(score * x)
        ) q
    """
    plan = decidb_cli.execute_raw(f"EXPLAIN {weighted}").stdout
    assert _has_decide_operator(plan) and "Direct solve" not in plan
    rows, _ = decidb_cli.execute(weighted)
    assert sorted(rows) == [(1, 0), (2, 1)]

    # `off` still reaches the solver for a shape direct solve would take.
    off = _raw(decidb_cli, f"EXPLAIN {s1}", mode="off").stdout
    assert _has_decide_operator(off) and "Direct solve" not in off


@pytest.mark.correctness
@pytest.mark.parametrize("score", ["'NaN'::DOUBLE", "'Infinity'::DOUBLE"])
def test_invalid_score_error_matches_solver_wording(decidb_cli, score):
    # This message is a fixed string, so direct solve keeps the solver's. The solver also reports the row number; the
    # direct plan has no row to report.
    sql = f"""
        SELECT id, x FROM (
            FROM (VALUES (1, 2.0), (2, {score})) t(id, score)
            DECIDE x(BOOL) SUCH THAT SUM(x) <= 1 MAXIMIZE SUM(score * x)
        ) q
    """
    solver = _raw(decidb_cli, sql, mode="off").stderr
    direct = _raw(decidb_cli, sql, mode="require").stderr
    assert "Error" in direct, direct
    assert solver == direct


@pytest.mark.correctness
@pytest.mark.parametrize(
    "name,bounds,objective",
    [
        ("x", "x >= 0 AND x <= 1e17", "MAXIMIZE SUM(x)"),
        ("x", "x >= -1e17 AND x <= 0", "MINIMIZE SUM(x)"),
        ("x", "x >= 0 AND x <= 9007199254740994", "MAXIMIZE SUM(x)"),
        (
            "quantity_ordered",
            "quantity_ordered >= 0 AND quantity_ordered <= 12345678901234567890",
            "MAXIMIZE SUM(quantity_ordered)",
        ),
        ("x", "x >= 0 AND x <= cap", "MAXIMIZE SUM(x)"),
    ],
)
def test_a1_whole_number_range_error_is_the_solvers_error_word_for_word(
    decidb_cli, decidb_cli_highs, name, bounds, objective
):
    # One template in `decide_numeric_policy.hpp` serves both paths: the variable's name, the answer printed as the solver
    # prints it, and the advice. The full text is compared across `require` and `off` on both backends, never against a
    # hand-written string. The last case reads the bound from a column, so the answer is formatted at run time per row.
    sql = f"""
        SELECT id, {name} FROM (
            FROM (VALUES (1, 10.0), (2, 1e17)) t(id, cap)
            DECIDE {name}(INT) SUCH THAT {bounds} {objective}
        ) q
    """
    direct = _raw(decidb_cli, sql, mode="require").stderr
    assert "beyond the range DeciDB can return as a whole number" in direct, direct
    assert f"DECIDE variable '{name}' solves to " in direct, direct
    # Every bound is 1e6 or more, so the solver warns before it reaches the answer it cannot return; a direct plan
    # builds no model.
    assert _raw(decidb_cli, sql, mode="off", expect_large_numbers=True).stderr == direct
    assert _raw(decidb_cli_highs, sql, mode="off", expect_large_numbers=True).stderr == direct


# A plan prints an expression by its alias, so a pipeline tag left on a value a direct plan computes reaches EXPLAIN
# (`__source_fragment_0__`). The rule driver strips every tag from the operators it installs; the cases are the values
# that carry one: a scalar subquery in a bound, a `when` and an objective coefficient, and a data reducer's bound.
_TAGGED_BOUNDS = {
    "s1_scalar_subquery": "x(BOOL) SUCH THAT SUM(x) <= 1 AND x <= (SELECT max(v) FROM range(2) r(v)) MAXIMIZE SUM(s * x)",
    "a1_scalar_subquery": "x(REAL) SUCH THAT x <= (SELECT max(v) FROM range(2) r(v)) MAXIMIZE SUM(s * x)",
    "a1_data_reducer": "x(REAL) SUCH THAT x <= SUM(c) / 4 MAXIMIZE SUM(s * x)",
    "a1_subquery_in_a_when": (
        "x(REAL) SUCH THAT x <= 5 AND when c > (SELECT max(v) FROM range(2) r(v)): x <= 1 MAXIMIZE SUM(s * x)"
    ),
    "a1_subquery_coefficient": "x(REAL) SUCH THAT x <= 1 MAXIMIZE SUM((SELECT max(v) FROM range(2) r(v)) * s * x)",
}


@pytest.mark.correctness
@pytest.mark.parametrize("shape", sorted(_TAGGED_BOUNDS))
def test_explain_of_a_direct_plan_shows_no_pipeline_tag(decidb_cli, shape):
    sql = "SELECT id, x FROM (VALUES (1, 2.0, 1.0), (2, 3.0, 2.0)) t(id, c, s) " f"DECIDE {_TAGGED_BOUNDS[shape]}"
    plan = _raw(decidb_cli, f"EXPLAIN {sql}").stdout
    assert "Direct solve" in plan and "__" not in plan, plan


def _wide_decide(shape, width):
    columns = [f"c{i}" for i in range(width)]
    # DOUBLE throughout: a sum of DECIMAL literals can overflow, which direct solve does not take.
    row = ", ".join(["1.0::DOUBLE"] * (width + 1))
    source = f"FROM (VALUES (1, {row}), (2, {row})) t(id, score, {', '.join(columns)})"
    total = " + ".join(columns)
    if shape == "s1":
        return f"{source} DECIDE x(BOOL) SUCH THAT SUM(x) <= 1 MAXIMIZE SUM(({total}) * x)"
    return f"{source} DECIDE x(REAL) SUCH THAT x <= {total} MAXIMIZE SUM(score * x)"


@pytest.mark.correctness
@pytest.mark.parametrize("shape", ["s1", "a1"])
def test_wide_value_is_proved_by_direct_solve_and_answers_like_the_solver(decidb_cli, shape):
    decide = _wide_decide(shape, 8)
    sql = f"SELECT id, x FROM ({decide}) q ORDER BY id"
    plan = _raw(decidb_cli, f"EXPLAIN {sql}", mode="require").stdout
    assert "Direct solve rule" in plan and not _has_decide_operator(plan), plan
    direct, _ = _run(decidb_cli, sql, mode="require")
    solver, _ = _run(decidb_cli, sql, mode="off")
    assert direct == solver


@pytest.mark.correctness
@pytest.mark.parametrize("bad", ["'NaN'::DOUBLE", "'Infinity'::DOUBLE"])
def test_a1_invalid_coefficient_names_the_value_the_user_wrote(decidb_cli, bad):
    # A bad number is reported as the value the user wrote and the edit that skips its rows, in both modes.
    def error(constraint_expression, objective_expression, mode):
        sql = f"""
            SELECT id, x FROM (
                FROM (VALUES (1, 1.0, 1.0), (2, 2.0, {bad}), (3, 3.0, 1.0)) t(id, s, w)
                DECIDE x(REAL) SUCH THAT {constraint_expression} <= 5 MAXIMIZE SUM({objective_expression})
            ) q
        """
        return _raw(decidb_cli, sql, mode=mode).stderr

    skip_w = "DECIDE: w is NaN or infinite on some rows. Skip those rows with 'when isfinite(w):'."
    skip_two_w = "DECIDE: 2 * w is NaN or infinite on some rows. Skip those rows with 'when isfinite(2 * w):'."
    for mode in ("require", "off"):
        assert skip_w in error("w * x", "s * x", mode), mode
        assert skip_w in error("x", "w * x", mode), mode
        # The value is named as the solver reads it once the sum's constant factor is applied.
        assert skip_two_w in error("x", "2 * w * x", mode), mode


@pytest.mark.correctness
@pytest.mark.parametrize("bad", ["'NaN'::DOUBLE", "'Infinity'::DOUBLE"])
def test_r1_invalid_number_names_the_value_the_user_wrote(decidb_cli, bad):
    # The weight of the budget and the value in the objective are named as written, on both paths, and the two messages
    # are the same.
    def error(weight, value, mode):
        sql = f"""
            SELECT id, x FROM (
                FROM (VALUES (1, 1.0, 1.0), (2, 2.0, {bad}), (3, 3.0, 1.0)) t(id, a, b)
                DECIDE x(REAL) SUCH THAT x <= 5 AND SUM({weight} * x) <= 9 MAXIMIZE SUM({value} * x)
            ) q
        """
        return _raw(decidb_cli, sql, mode=mode).stderr

    skip_b = "DECIDE: b is NaN or infinite on some rows. Skip those rows with 'when isfinite(b):'."
    for mode in ("require", "off"):
        assert skip_b in error("b", "a", mode), mode
        assert skip_b in error("a", "b", mode), mode
    direct = error("b", "a", "require")
    assert error("b", "a", "off") == direct


@pytest.mark.correctness
def test_r1_objective_factor_that_overflows_names_the_objective_like_the_solvers(decidb_cli):
    # Every value is finite: the factor DeciDB applies leaves the DOUBLE range, so the message names the objective.
    sql = """
        SELECT id, x FROM (
            FROM (VALUES (1, 4.0, 2.0, 10.0), (2, 3.0, 3.0, 9.0)) t(id, cap, w, v)
            DECIDE x(REAL) SUCH THAT x <= cap AND SUM(w * x) <= 9 MAXIMIZE 1e308 * SUM(v * x)
        ) q
    """
    direct = _raw(decidb_cli, sql, mode="require").stderr
    solver = _raw(decidb_cli, sql, mode="off").stderr
    assert (
        "DECIDE: MAXIMIZE 1e+308 * sum(v * x) computes a number too large for a DOUBLE. Scale its data down." in direct
    )
    assert solver == direct


@pytest.mark.correctness
@pytest.mark.parametrize(
    "name,rows,decide",
    [
        ("infeasible_budget", "(1, 4.0, 2.0, 10.0), (2, 3.0, 3.0, 9.0)", "x >= 1 AND x <= cap AND SUM(w * x) <= 3"),
        ("infeasible_empty_box", "(1, 4.0, 2.0, 10.0), (2, 3.0, 3.0, 9.0)", "x >= 2 AND x <= 1 AND SUM(w * x) <= 30"),
        ("unbounded_negative_weight", "(1, 4.0, -2.0, 10.0), (2, 3.0, 3.0, 9.0)", "x >= 0 AND SUM(w * x) <= 30"),
        ("unbounded_zero_weight", "(1, 4.0, 0.0, 10.0), (2, 3.0, 3.0, 9.0)", "x >= 0 AND SUM(w * x) <= 30"),
    ],
)
def test_r1_infeasible_and_unbounded_errors_are_the_solvers_error_word_for_word(decidb_cli, name, rows, decide):
    # The two errors have no row position: one function builds each text for both paths. A budget that cannot be met
    # and a box that is empty are both infeasible; a row that gains without end is unbounded.
    sql = (
        f"SELECT id, x FROM (FROM (VALUES {rows}) t(id, cap, w, v) "
        f"DECIDE x(REAL) SUCH THAT {decide} MAXIMIZE SUM(v * x)) q"
    )
    direct = _raw(decidb_cli, sql, mode="require").stderr
    solver = _raw(decidb_cli, sql, mode="off").stderr
    assert ("infeasible" in direct) == name.startswith("infeasible"), direct
    assert ("unbounded" in direct) == name.startswith("unbounded"), direct
    assert direct == solver


def _s1_source_bound_error(decidb_cli, cap_type, cap):
    sql = f"""
        SELECT x FROM (
            FROM (VALUES (1,{cap}::{cap_type},9.0)) t(id,cap,score)
            DECIDE x(BOOL) SUCH THAT SUM(x)<=cap MAXIMIZE SUM(score*x)
        ) q
    """
    return _raw(decidb_cli, sql, mode="require").stderr, _raw(decidb_cli, sql, mode="off").stderr


@pytest.mark.correctness
def test_s1_source_bound_error_for_a_column_is_the_solvers(decidb_cli):
    """A bound that is one column raises the solver's NaN error."""
    direct, solver = _s1_source_bound_error(decidb_cli, "DOUBLE", "'NaN'")
    assert "DECIDE: the bound cap is NaN on some rows. Skip those rows with 'when NOT isnan(cap):'." in direct, direct
    assert solver == direct


@pytest.mark.correctness
@pytest.mark.parametrize("cap_type", ["INTEGER", "DOUBLE"])
def test_s1_null_source_bound_is_no_bound_on_both_paths(decidb_cli, cap_type):
    """A NULL bound skips its instance (B1): S1 applies and answers like the solver."""
    sql = f"""
        SELECT x FROM (
            FROM (VALUES (1,NULL::{cap_type},9.0)) t(id,cap,score)
            DECIDE x(BOOL) SUCH THAT SUM(x)<=cap MAXIMIZE SUM(score*x)
        ) q
    """
    assert _run(decidb_cli, sql, mode="require") == _run(decidb_cli, sql, mode="off")


@pytest.mark.correctness
@pytest.mark.parametrize("bound", ["TRY_CAST(c AS INTEGER)", "b"], ids=["computed", "column"])
def test_s1_null_per_row_bound_is_no_bound_on_both_paths(decidb_cli, bound):
    """A per-row bound that is NULL skips its instance (B1): S1 applies and answers like the solver."""
    sql = f"""
        SELECT id, x FROM (
            FROM (VALUES (0, 'bad', NULL::BOOLEAN, 5.0)) t(id, c, b, p)
            DECIDE x(BOOL) SUCH THAT x <= {bound} AND SUM(x) <= 1 MAXIMIZE SUM(p*x)
        ) q
    """
    assert _run(decidb_cli, sql, mode="require") == _run(decidb_cli, sql, mode="off")


@pytest.mark.correctness
def test_r1_budget_limit_error_is_the_solvers(decidb_cli):
    """A budget limit raises the solver's NaN error. A NULL one skips its instance (the R1 data-budget cases)."""

    def error(lim_type, lim):
        sql = f"""
            SELECT x FROM (
                FROM (VALUES (1,2.0,9.0,{lim}::{lim_type}),(2,3.0,5.0,{lim}::{lim_type})) t(id,w,score,lim)
                DECIDE x(REAL) SUCH THAT x<=5 AND SUM(w*x)<=lim MAXIMIZE SUM(score*x)
            ) q
        """
        return _raw(decidb_cli, sql, mode="require").stderr, _raw(decidb_cli, sql, mode="off").stderr

    for lim_type in ("FLOAT", "DOUBLE"):
        direct, solver = error(lim_type, "'NaN'")
        assert (
            "DECIDE: the bound lim is NaN on some rows. Skip those rows with 'when NOT isnan(lim):'." in direct
        ), direct
        assert solver == direct


@pytest.mark.correctness
def test_unused_wide_output_is_pruned_without_losing_rank_or_bindings(decidb_cli):
    setup = """
        CREATE TEMP TABLE pruning_source AS
        SELECT i, (i % 101)::DOUBLE AS score, repeat('p', 200) AS payload
        FROM range(5000) t(i);
    """
    decide = """
        FROM pruning_source DECIDE x(BOOL)
        SUCH THAT SUM(x)<=500 MAXIMIZE SUM(score*x)
    """
    aggregate = f"SELECT COUNT(*), SUM(x), SUM(score*x) FROM ({decide}) q"
    plan = _raw(decidb_cli, setup + f"EXPLAIN {aggregate}").stdout
    scan = plan.rsplit("SEQ_SCAN", 1)[-1]
    assert "Direct solve rule" in plan and "WINDOW" in plan
    assert "score" in scan and "payload" not in scan

    aliased = """
        SELECT COUNT(*), SUM(x) FROM (
            FROM (SELECT i, score AS profit, payload FROM pruning_source WHERE i>=0) s
            DECIDE x(BOOL) SUCH THAT SUM(x)<=500 MAXIMIZE SUM(profit*x)
        ) q
    """
    alias_plan = _raw(decidb_cli, setup + f"EXPLAIN {aliased}").stdout
    assert "payload" not in alias_plan.rsplit("SEQ_SCAN", 1)[-1]

    rows, _ = _run(decidb_cli.with_verify_serializer(), setup + aggregate)
    expected = sum(sorted((i % 101 for i in range(5000)), reverse=True)[:500])
    assert rows == [(5000, 500, float(expected))]

    full, _ = _run(decidb_cli, setup + f"SELECT i, score, payload, x FROM ({decide}) q WHERE i=0")
    assert full == [(0, 0.0, "p" * 200, 0)]

    invalid = setup.replace("(i % 101)::DOUBLE AS score", "CASE WHEN i=4999 THEN 'NaN'::DOUBLE ELSE 1.0 END AS score")
    count_only = f"SELECT COUNT(*) FROM ({decide.replace('<=500', '<=0')}) q"
    assert "score is NaN or infinite on some rows" in _raw(decidb_cli, invalid + count_only).stderr

    throwing_source = """
        SELECT i, i::DOUBLE AS score,
               CASE WHEN i=4999 THEN error('payload boom') ELSE 'ok' END AS payload
        FROM range(5000) t(i)
    """
    throwing = f"""
        SELECT COUNT(*) FROM (
            FROM ({throwing_source}) s DECIDE x(BOOL)
            SUCH THAT SUM(x)<=1 MAXIMIZE SUM(score*x)
        ) q
    """
    for mode in ("require", "off"):
        assert "payload boom" in _raw(decidb_cli, throwing, mode=mode).stderr


@pytest.mark.correctness
def test_a1_unused_wide_columns_stay_out_of_the_barrier_and_computed_errors_stay_in(decidb_cli):
    setup = """
        CREATE TEMP TABLE a1_pruning_source AS
        SELECT i, (i % 11)::DOUBLE AS score, 20.0 AS cap, repeat('p', 200) AS payload
        FROM range(5000) t(i);
    """
    decide = """
        FROM a1_pruning_source DECIDE x(REAL)
        SUCH THAT x BETWEEN 0 AND cap MINIMIZE SUM(POWER(x - score, 2))
    """
    aggregate = f"SELECT COUNT(*), SUM(x), SUM(POWER(x - score, 2)) FROM ({decide}) q"
    plan = _raw(decidb_cli, setup + f"EXPLAIN {aggregate}").stdout
    scan = plan.rsplit("SEQ_SCAN", 1)[-1]
    assert "A1_SEPARABLE_BOX" in plan and "WINDOW" in plan
    assert "score" in scan and "payload" not in scan

    rows, _ = _run(decidb_cli.with_verify_serializer(), setup + aggregate)
    assert rows == [(5000, float(sum(i % 11 for i in range(5000))), 0.0)]

    full, _ = _run(decidb_cli, setup + f"SELECT i, payload, x FROM ({decide}) q WHERE i=3")
    assert full == [(3, "p" * 200, 3.0)]

    # A computed column that can raise is not pruned: the solver evaluates it on every row.
    throwing_source = """
        SELECT i, i::DOUBLE AS score, 20.0 AS cap,
               CASE WHEN i=4999 THEN error('payload boom') ELSE 'ok' END AS payload
        FROM range(5000) t(i)
    """
    throwing = f"""
        SELECT COUNT(*) FROM (
            FROM ({throwing_source}) s DECIDE x(REAL)
            SUCH THAT x BETWEEN 0 AND cap MINIMIZE SUM(POWER(x - score, 2))
        ) q
    """
    for mode in ("require", "off"):
        assert "payload boom" in _raw(decidb_cli, throwing, mode=mode).stderr


@pytest.mark.correctness
def test_inner_join_passthrough_prunes_stored_payload_but_keeps_computed_errors(decidb_cli):
    setup = """
        CREATE TEMP TABLE join_source AS
        SELECT i, (i % 101)::DOUBLE AS score, repeat('p', 200) AS payload
        FROM range(5000) t(i);
        CREATE TEMP TABLE join_weights AS
        SELECT k, 1.0 + (k % 3)::DOUBLE / 10.0 AS weight FROM range(7) t(k);
    """
    relation = """
        (SELECT s.i, s.score * w.weight AS score, s.payload
         FROM join_source s JOIN join_weights w ON s.i % 7 = w.k) j
    """
    decide = f"""
        FROM {relation} DECIDE x(BOOL)
        SUCH THAT SUM(x)<=500 MAXIMIZE SUM(score*x)
    """
    aggregate = f"SELECT COUNT(*), SUM(x), SUM(score*x) FROM ({decide}) q"
    plan = _raw(decidb_cli, setup + f"EXPLAIN {aggregate}").stdout
    assert "Direct solve rule" in plan and "HASH_JOIN" in plan and "WINDOW" in plan
    assert "payload" not in plan

    rows, _ = _run(decidb_cli.with_verify_serializer(), setup + aggregate)
    scores = [(i % 101) * (1.0 + (i % 7 % 3) / 10.0) for i in range(5000)]
    assert rows[0][:2] == (5000, 500)
    assert math.isclose(rows[0][2], sum(sorted(scores, reverse=True)[:500]), rel_tol=0, abs_tol=1e-7)

    full, _ = _run(decidb_cli, setup + f"SELECT i, score, payload, x FROM ({decide}) q WHERE i=0")
    assert full == [(0, 0.0, "p" * 200, 0)]

    throwing_relation = relation.replace("s.payload", "CASE WHEN s.i=4999 THEN error('joined payload boom') "
                                                  "ELSE s.payload END AS payload")
    throwing = f"""
        SELECT COUNT(*) FROM (
            FROM {throwing_relation} DECIDE x(BOOL)
            SUCH THAT SUM(x)<=1 MAXIMIZE SUM(score*x)
        ) q
    """
    for mode in ("require", "off"):
        assert "joined payload boom" in _raw(decidb_cli, setup + throwing, mode=mode).stderr


@pytest.mark.correctness
def test_left_join_source_keeps_the_payload_in_the_plan_and_answers_like_the_solver(decidb_cli):
    # Only an inner join passes a column through untouched, so the payload of a left join is read even when no parent
    # does, while the inner join of the test above drops it.
    setup = """
        CREATE TEMP TABLE left_join_source AS
        SELECT i, (i % 101)::DOUBLE AS score, repeat('p', 200) AS payload
        FROM range(5000) t(i);
        CREATE TEMP TABLE left_join_weights AS
        SELECT k, 1.0 + (k % 3)::DOUBLE / 10.0 AS weight FROM range(7) t(k);
    """

    def aggregate(join):
        return f"""
            SELECT COUNT(*), SUM(x), SUM(score*x) FROM (
                FROM (SELECT s.i, s.score * w.weight AS score, s.payload
                      FROM left_join_source s {join} JOIN left_join_weights w ON s.i % 7 = w.k) j
                DECIDE x(BOOL) SUCH THAT SUM(x)<=500 MAXIMIZE SUM(score*x)
            ) q
        """

    left_plan = _raw(decidb_cli, setup + f"EXPLAIN {aggregate('LEFT')}").stdout
    assert "Direct solve rule" in left_plan and "HASH_JOIN" in left_plan and "payload" in left_plan
    inner_plan = _raw(decidb_cli, setup + f"EXPLAIN {aggregate('INNER')}").stdout
    assert "Direct solve rule" in inner_plan and "payload" not in inner_plan

    direct, _ = _run(decidb_cli, setup + aggregate("LEFT"))
    solver, _ = _run(decidb_cli, setup + aggregate("LEFT"), mode="off")
    assert direct[0][:2] == solver[0][:2] == (5000, 500)
    assert math.isclose(direct[0][2], solver[0][2], rel_tol=0, abs_tol=1e-7)


@pytest.mark.correctness
def test_a_filter_over_a_computed_column_keeps_the_computed_error(decidb_cli):
    # A filter passes its child's columns through, so whether a column may be dropped is the child's answer: a
    # computed column that can raise is read on every row, as the solver reads it.
    throwing = """
        SELECT COUNT(*) FROM (
            FROM (SELECT i, i::DOUBLE AS score,
                         CASE WHEN i=4999 THEN error('filtered payload boom') ELSE 'ok' END AS payload
                  FROM range(5000) t(i)) s
            WHERE i >= 0
            DECIDE x(BOOL) SUCH THAT SUM(x)<=1 MAXIMIZE SUM(score*x)
        ) q
    """
    for mode in ("require", "off"):
        assert "filtered payload boom" in _raw(decidb_cli, throwing, mode=mode).stderr


@pytest.mark.correctness
def test_invalid_forced_solver_and_disabled_optimizer_policy(decidb_cli):
    # DIAGNOSE and a valid forced backend are checked for every rule in the conformance suite
    # (test_direct_rule_conformance.py).
    sql = _source_query((2.0, 9.0), 1)
    invalid = decidb_cli.with_env({"DECIDB_FORCE_SOLVER": "unknown_backend"})
    error = _raw(invalid, sql, mode="auto").stderr
    assert "DECIDB_FORCE_SOLVER=unknown_backend" in error

    # A plan the DECIDE optimizer never saw is refused on every path, whichever way the optimizer was switched off.
    refusal = "DECIDE needs the DECIDE optimizer."
    for mode in ("require", "off"):
        disabled = _raw(decidb_cli, f"SET disabled_optimizers='decide_optimizer'; {sql}", mode=mode).stderr
        assert refusal in disabled, disabled
    assert refusal in _raw(decidb_cli, f"PRAGMA disable_optimizer; {sql}", mode="off").stderr


@pytest.mark.correctness
def test_an_unknown_direct_solve_mode_is_rejected(decidb_cli):
    result = decidb_cli.execute_raw("SET decide_direct_solve='sometimes'")
    assert "decide_direct_solve must be off, auto, or require" in result.stderr, result.stderr


@pytest.mark.correctness
def test_an_empty_forced_solver_name_forces_nothing(decidb_cli):
    # DECIDB_FORCE_SOLVER set to the empty string is no forced backend: direct solve applies under `require`.
    sql = _source_query((2.0, 9.0, -1.0), 1)
    unforced = decidb_cli.with_env({"DECIDB_FORCE_SOLVER": ""})
    assert "S1_UNIFORM_MATROID" in _raw(unforced, f"EXPLAIN {sql}").stdout
    assert _run(unforced, sql)[0] == _run(decidb_cli, sql, mode="off")[0]


@pytest.mark.correctness
def test_an_empty_forced_solver_name_is_no_solver_override_on_the_solver_path_too(decidb_cli):
    # One reader of DECIDB_FORCE_SOLVER (ForcedSolverName): the solver path picks its backend as it does without the
    # variable, where it once refused the empty name as "not a solver DeciDB knows".
    sql = _source_query((2.0, 9.0, -1.0), 1)
    unforced = decidb_cli.with_env({"DECIDB_FORCE_SOLVER": ""})
    result = _raw(unforced, sql, mode="off")
    assert "is not a solver DeciDB knows" not in result.stderr, result.stderr
    assert _run(unforced, sql, mode="off")[0] == _run(decidb_cli, sql, mode="off")[0]


@pytest.mark.correctness
def test_require_reports_the_conflict_before_it_looks_at_the_forced_backend(decidb_cli):
    # The solver is chosen lazily, after the policy: `require` beside a backend no solver answers to is the conflict,
    # not the unknown name, and a query a rule answers never asks for a backend.
    sql = _source_query((2.0, 9.0), 1)
    invalid = decidb_cli.with_env({"DECIDB_FORCE_SOLVER": "unknown_backend"})
    error = _raw(invalid, sql, mode="require").stderr
    assert "conflicts with DECIDB_FORCE_SOLVER" in error and "is not a solver DeciDB knows" not in error, error


@pytest.mark.correctness
@pytest.mark.parametrize("mode", ["require", "off"])
def test_the_optimizer_timer_covers_the_stage_on_both_paths(decidb_cli, mode):
    # A direct query prints the stage's time as a solver query does (DECIDB_BENCH).
    timed = decidb_cli.with_env({"DECIDB_BENCH": "1"})
    assert "DECIDB_BENCH: optimizer_ms=" in _raw(timed, _source_query((2.0, 9.0), 1), mode=mode).stderr


@pytest.mark.correctness
def test_extension_registration_preserves_binder_errors(decidb_cli):
    sql = """
        SELECT x FROM (
            FROM (VALUES (1,2),(2,9)) n(id,score)
            DECIDE x(BOOL) SUCH THAT SUM(m: x)<=1 MAXIMIZE SUM(score*x)
        ) q
    """
    for mode in ("off", "require"):
        result = _raw(decidb_cli, sql, mode=mode)
        assert "Binder Error" in result.stderr
        assert "Relation 'm' is not in the FROM clause" in result.stderr


# --- Reading a stored table twice instead of holding wide rows ---------------------------------------------------------
#
# Direct solve checks every input row before it releases one, so its plan holds the rows. A stored column the parent
# reads and the rule never uses is read from a second scan of the table, joined on the row id, so it is not held.

_LATE_MATERIALIZATION_ROWS = 5000

# (decision clause, score of row i, how a bad last row is spelled). Scores are distinct for S1, so its optimum is unique.
_LATE_MATERIALIZATION_RULES = {
    "s1": (
        "DECIDE x(BOOL) SUCH THAT SUM(x) <= 500 MAXIMIZE SUM(score * x)",
        "((i * 37) % 5000)::DOUBLE",
    ),
    "a1": (
        "DECIDE x(REAL) SUCH THAT x BETWEEN 0 AND cap MINIMIZE SUM(POWER(x - score, 2))",
        "(i % 11)::DOUBLE",
    ),
}


def _late_materialization_setup(rule, bad_last=False):
    _, score = _LATE_MATERIALIZATION_RULES[rule]
    if bad_last:
        score = f"CASE WHEN i = {_LATE_MATERIALIZATION_ROWS - 1} THEN 'NaN'::DOUBLE ELSE {score} END"
    return f"""
        CREATE TEMP TABLE rr_src AS
        SELECT i AS id, {score} AS score, (5 + i % 7)::DOUBLE AS cap, 'payload-' || i::VARCHAR AS payload
        FROM range({_LATE_MATERIALIZATION_ROWS}) t(i);
        CREATE TEMP TABLE rr_one AS SELECT 1 AS k;
    """


def _late_materialization_decide(rule, relation="rr_src"):
    return f"FROM {relation} {_LATE_MATERIALIZATION_RULES[rule][0]}"


def _table_scans(plan, table="rr_src"):
    return len(re.findall(rf"Table: {table}\b", plan))


def _same_answers(left, right):
    assert len(left) == len(right)
    for a, b in zip(left, right):
        assert a[:-1] == b[:-1]
        assert math.isclose(a[-1], b[-1], rel_tol=0, abs_tol=1e-7), (a, b)


@pytest.mark.correctness
@pytest.mark.parametrize("rule", ["s1", "a1"])
def test_second_read_returns_the_solvers_answers_and_the_plan_reads_the_table_twice(decidb_cli, rule):
    setup = _late_materialization_setup(rule)
    decide = _late_materialization_decide(rule)
    query = f"SELECT id, payload, x FROM ({decide}) q ORDER BY id"

    plan = _raw(decidb_cli, setup + f"EXPLAIN {query}").stdout
    assert "Direct solve rule" in plan
    assert _table_scans(plan) == 2 and "HASH_JOIN" in plan

    # The same plan with the table round-tripped through a plan serializer, the solver, and a source that is held
    # because it sits over a join.
    late_materialization, _ = _run(decidb_cli.with_verify_serializer(), setup + query)
    solver, _ = _run(decidb_cli, setup + query, mode="off")
    joined = _late_materialization_decide(rule, "(SELECT s.* FROM rr_src s JOIN rr_one o ON o.k = 1) s")
    held_query = f"SELECT id, payload, x FROM ({joined}) q ORDER BY id"
    held_plan = _raw(decidb_cli, setup + f"EXPLAIN {held_query}").stdout
    assert "Direct solve rule" in held_plan and _table_scans(held_plan) == 1
    held, _ = _run(decidb_cli, setup + held_query)

    assert len(late_materialization) == _LATE_MATERIALIZATION_ROWS
    assert (
        late_materialization[0][1] == "payload-0"
        and late_materialization[-1][1] == f"payload-{_LATE_MATERIALIZATION_ROWS - 1}"
    )
    _same_answers(late_materialization, solver)
    _same_answers(held, solver)


@pytest.mark.correctness
@pytest.mark.parametrize("rule", ["s1", "a1"])
def test_second_read_still_raises_on_a_bad_value_in_the_last_row_under_limit(decidb_cli, rule):
    setup = _late_materialization_setup(rule, bad_last=True)
    decide = _late_materialization_decide(rule)
    query = f"SELECT id, payload, x FROM ({decide}) q LIMIT 1"
    plan = _raw(decidb_cli, setup + f"EXPLAIN {query}").stdout
    assert "Direct solve rule" in plan and _table_scans(plan) == 2

    solver = outcome(decidb_cli.with_verify_serializer(), setup + query, "off")
    direct = outcome(decidb_cli.with_verify_serializer(), setup + query, "require")
    assert solver[0] == "error", solver
    assert direct == solver


@pytest.mark.correctness
@pytest.mark.parametrize("rule", ["s1", "a1"])
def test_second_read_only_for_a_stored_table_and_a_column_the_rule_does_not_use(decidb_cli, rule):
    setup = _late_materialization_setup(rule)
    decide = _late_materialization_decide(rule)

    def scans(sql, expected_rule_applies=True):
        plan = _raw(decidb_cli, setup + f"EXPLAIN {sql}").stdout
        assert ("Direct solve rule" in plan) == expected_rule_applies
        return _table_scans(plan)

    # Nothing the rule does not use is read: no second scan.
    assert scans(f"SELECT COUNT(*), SUM(x) FROM ({decide}) q") == 1
    assert scans(f"SELECT score, x FROM ({decide}) q") == 1
    # A stored column nothing in the rule uses is read twice, and the payload alone is enough.
    assert scans(f"SELECT payload FROM ({decide}) q") == 2
    assert scans(f"SELECT SUM(length(payload)), SUM(x) FROM ({decide}) q") == 2

    # A computed column keeps holding, and so does a column of a volatile or aggregated source.
    volatile = _late_materialization_decide(rule, "(SELECT *, random() AS r FROM rr_src) s")
    assert scans(f"SELECT payload, r, x FROM ({volatile}) q") == 1
    grouped = _late_materialization_decide(
        rule, "(SELECT id, MAX(score) AS score, MAX(cap) AS cap, MAX(payload) AS payload FROM rr_src GROUP BY id) s"
    )
    assert scans(f"SELECT payload, x FROM ({grouped}) q") == 1
    computed = _late_materialization_decide(rule, "(SELECT id, score, cap, payload || '!' AS payload FROM rr_src) s")
    assert scans(f"SELECT payload, x FROM ({computed}) q") == 1

    # A source over a join keeps holding.
    joined = _late_materialization_decide(rule, "(SELECT s.* FROM rr_src s JOIN rr_one o ON o.k = 1) s")
    assert scans(f"SELECT payload, x FROM ({joined}) q") == 1

    # A filter on the table is read twice too, and the late materialization returns only the rows the first one kept.
    filtered = _late_materialization_decide(rule, "(SELECT * FROM rr_src WHERE id % 2 = 0) s")
    filtered_query = f"SELECT id, payload, x FROM ({filtered}) q ORDER BY id"
    assert scans(filtered_query) == 2
    kept, _ = _run(decidb_cli, setup + filtered_query)
    solver, _ = _run(decidb_cli, setup + filtered_query, mode="off")
    assert len(kept) == _LATE_MATERIALIZATION_ROWS // 2
    _same_answers(kept, solver)


@pytest.mark.correctness
@pytest.mark.parametrize("rule", ["s1", "a1"])
def test_second_read_moves_only_variable_width_columns(decidb_cli, rule):
    setup = _late_materialization_setup(rule)
    decide = _late_materialization_decide(rule)

    def scans(query):
        plan = _raw(decidb_cli, setup + f"EXPLAIN {query}").stdout
        assert "Direct solve rule" in plan
        return _table_scans(plan)

    # A fixed-width stored column the parent reads stays in the single read.
    narrow_query = f"SELECT id, x FROM ({decide}) q ORDER BY id"
    assert scans(narrow_query) == 1
    # A VARCHAR one moves to the late materialization, and so does one next to a fixed-width column that stays.
    wide_query = f"SELECT payload, x FROM ({decide}) q ORDER BY payload"
    assert scans(wide_query) == 2
    mixed_query = f"SELECT id, payload, x FROM ({decide}) q ORDER BY id"
    assert scans(mixed_query) == 2

    for query in (narrow_query, wide_query, mixed_query):
        direct, _ = _run(decidb_cli, setup + query)
        solver, _ = _run(decidb_cli, setup + query, mode="off")
        assert len(direct) == _LATE_MATERIALIZATION_ROWS
        _same_answers(direct, solver)


@pytest.mark.correctness
def test_a1_pipelined_plan_does_not_read_twice(decidb_cli):
    setup = _late_materialization_setup("a1")
    # Every row sees the same bounds and coefficients, so the plan holds nothing and a late materialization would only add a join.
    decide = "FROM rr_src DECIDE x(REAL) SUCH THAT x BETWEEN 0 AND 10 MAXIMIZE SUM(2 * x)"
    query = f"SELECT id, payload, x FROM ({decide}) q ORDER BY id"
    plan = _raw(decidb_cli, setup + f"EXPLAIN {query}").stdout
    assert "A1_SEPARABLE_BOX" in plan and _table_scans(plan) == 1
    rows, _ = _run(decidb_cli, setup + query)
    solver, _ = _run(decidb_cli, setup + query, mode="off")
    _same_answers(rows, solver)


def _single_read_and_solver_answers(cli, setup, query):
    """The direct plan reads the table once (no late materialization) and answers like the solver."""
    plan = _raw(cli, setup + f"EXPLAIN {query}").stdout
    assert "Direct solve rule" in plan and _table_scans(plan) == 1, plan
    direct, _ = _run(cli, setup + query)
    solver, _ = _run(cli, setup + query, mode="off")
    assert len(direct) > 0
    _same_answers(direct, solver)
    return plan


@pytest.mark.correctness
def test_second_read_is_skipped_when_the_source_already_reads_the_row_id(decidb_cli):
    # A source that selects `rowid` has its own use of the row id, so direct solve leaves its scan alone.
    setup = _late_materialization_setup("s1")
    decide = _late_materialization_decide("s1", "(SELECT rowid AS rid, * FROM rr_src) s")
    query = f"SELECT id, payload, rid, x FROM ({decide}) q ORDER BY id"
    _single_read_and_solver_answers(decidb_cli, setup, query)


@pytest.mark.correctness
@pytest.mark.parametrize(
    "column",
    [
        pytest.param("payload AS payload_copy", id="read_by_two_outputs"),
        pytest.param("payload || '!' AS payload_copy", id="read_by_a_computed_column"),
    ],
)
def test_second_read_is_skipped_when_another_column_reads_the_stored_column(decidb_cli, column):
    # The payload is read by a second output (an alias of it) or by an expression of the source, so it is not only
    # forwarded and stays in the single read.
    setup = _late_materialization_setup("s1")
    decide = _late_materialization_decide("s1", f"(SELECT *, {column} FROM rr_src) s")
    query = f"SELECT id, payload, payload_copy, x FROM ({decide}) q ORDER BY id"
    _single_read_and_solver_answers(decidb_cli, setup, query)


@pytest.mark.correctness
@pytest.mark.parametrize(
    "relation,query_columns",
    [
        # A long IN list in a projection becomes a MARK join under it.
        pytest.param(
            "(SELECT *, id % 100 IN (0, 1, 2, 3, 4, 5, 6, 7) AS flagged FROM rr_src) s",
            "id, payload, flagged, x",
            id="join_under_a_projection",
        ),
        # In a filter it becomes a MARK join under a filter that carries a projection map, which is kept when the
        # parent reads every column.
        pytest.param(
            "(SELECT * FROM rr_src WHERE id % 100 IN (0, 1, 2, 3, 4, 5, 6, 7)) s",
            "*",
            id="join_under_a_filter_with_a_projection_map",
        ),
    ],
)
def test_second_read_is_skipped_when_a_later_pass_adds_a_join_to_the_source(decidb_cli, relation, query_columns):
    setup = _late_materialization_setup("s1")
    decide = _late_materialization_decide("s1", relation)
    query = f"SELECT {query_columns} FROM ({decide}) q ORDER BY id"
    plan = _single_read_and_solver_answers(decidb_cli, setup, query)
    assert "Join Type: MARK" in plan
