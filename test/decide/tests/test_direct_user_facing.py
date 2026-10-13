"""What a SQL user sees of direct solve besides the answer: the default mode, error wording, memory use, and policy.

The answers themselves are checked in `test_direct_three_way.py` and the per-rule contract in
`test_direct_rule_contract.py`.
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


def _raw(cli, sql, mode="require"):
    return cli.execute_raw(f"SET decide_direct_solve='{mode}'; {sql}")


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
    assert "Direct solve rule" in plan and "S1_CARDINALITY_INTERVAL" in plan and "WINDOW" in plan and not _has_decide_operator(plan)
    rows, _ = decidb_cli.execute(s1)
    assert sum(row[2] for row in rows) == 1 and sum(row[1] * row[2] for row in rows) == 9.0

    # A shape direct solve does not prove keeps the solver plan and prints nothing about direct solve.
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
    assert re.sub(r" at row \d+", "", solver) == direct


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
    assert _raw(decidb_cli, sql, mode="off").stderr == direct
    assert _raw(decidb_cli_highs, sql, mode="off").stderr == direct


@pytest.mark.correctness
def test_null_score_error_quotes_the_score(decidb_cli):
    # A NULL score is reported with the score as the user wrote it. The solver path says the same for this score.
    sql = """
        SELECT id, x FROM (
            FROM (VALUES (1, '2.0'), (2, 'bad')) t(id, s)
            DECIDE x(BOOL) SUCH THAT SUM(x) <= 1 MAXIMIZE SUM(TRY_CAST(s AS DOUBLE) * x)
        ) q
    """
    solver = _raw(decidb_cli, sql, mode="off").stderr
    direct = _raw(decidb_cli, sql, mode="require").stderr
    assert "TRY_CAST(s AS DOUBLE) is NULL. Impute it with COALESCE()" in direct, direct
    assert solver == direct


_NULL_ERROR_CASES = {
    # S1: the score, a pin and a count bound.
    "s1_score_column": (
        "FROM (VALUES (1, 1.0), (2, NULL::DOUBLE)) t(id, score) "
        "DECIDE x(BOOL) SUCH THAT SUM(x) <= 1 MAXIMIZE SUM(score * x)"
    ),
    "s1_score_expression_one_null_column": (
        "FROM (VALUES (1, 1.0, 2.0), (2, 3.0, NULL::DOUBLE)) t(id, a, b) "
        "DECIDE x(BOOL) SUCH THAT SUM(x) <= 1 MAXIMIZE SUM((a + b) * x)"
    ),
    "s1_score_expression_two_null_columns": (
        "FROM (VALUES (1, 1.0, 2.0), (2, NULL::DOUBLE, NULL::DOUBLE)) t(id, a, b) "
        "DECIDE x(BOOL) SUCH THAT SUM(x) <= 1 MAXIMIZE SUM((a + b) * x)"
    ),
    "s1_score_terms_added_like_terms": (
        "FROM (VALUES (1, 1.0, 2.0), (2, NULL::DOUBLE, NULL::DOUBLE)) t(id, a, b) "
        "DECIDE x(BOOL) SUCH THAT SUM(x) <= 1 MAXIMIZE SUM(a * x + b * x)"
    ),
    "s1_score_per_group": (
        "FROM (VALUES (1, 1.0, 1), (2, NULL::DOUBLE, 1)) t(id, score, g) "
        "DECIDE x(BOOL) SUCH THAT SUM(x) <= 1 PER g MAXIMIZE SUM(score * x)"
    ),
    "s1_score_no_null_column": (
        "FROM (VALUES (1, '2.0'), (2, 'bad')) t(id, s) "
        "DECIDE x(BOOL) SUCH THAT SUM(x) <= 1 MAXIMIZE SUM(TRY_CAST(s AS DOUBLE) * x)"
    ),
    "s1_integer_count_bound_column": (
        "FROM (VALUES (1, 1.0, 2), (2, 3.0, NULL::INTEGER)) t(id, score, cap) "
        "DECIDE x(BOOL) SUCH THAT SUM(x) <= cap MAXIMIZE SUM(score * x)"
    ),
    "s1_double_count_bound_column": (
        "FROM (VALUES (1, 1.0, 2.0), (2, 3.0, NULL::DOUBLE)) t(id, score, cap) "
        "DECIDE x(BOOL) SUCH THAT SUM(x) <= cap MAXIMIZE SUM(score * x)"
    ),
    "s1_numeric_pin_column": (
        "FROM (VALUES (1, 1.0, 1), (2, 3.0, NULL::INTEGER)) t(id, score, p) "
        "DECIDE x(BOOL) SUCH THAT x <= p AND SUM(x) <= 1 MAXIMIZE SUM(score * x)"
    ),
    "s1_boolean_pin_column": (
        "FROM (VALUES (1, 1.0, true), (2, 3.0, NULL::BOOLEAN)) t(id, score, p) "
        "DECIDE x(BOOL) SUCH THAT x <= p AND SUM(x) <= 1 MAXIMIZE SUM(score * x)"
    ),
    # A1: the objective, a square, a bound and a constraint coefficient.
    "a1_linear_coefficient": (
        "FROM (VALUES (1, 1.0, 2.0), (2, NULL::DOUBLE, 2.0)) t(id, score, cap) "
        "DECIDE x(REAL) SUCH THAT x BETWEEN 0 AND cap MAXIMIZE SUM(score * x)"
    ),
    "a1_square_target": (
        "FROM (VALUES (1, 1.0, 2.0), (2, NULL::DOUBLE, 2.0)) t(id, target, cap) "
        "DECIDE x(REAL) SUCH THAT x BETWEEN 0 AND cap MINIMIZE SUM(POWER(x - target, 2))"
    ),
    "a1_square_weight": (
        "FROM (VALUES (1, 1.0, 2.0), (2, NULL::DOUBLE, 2.0)) t(id, s, cap) "
        "DECIDE x(REAL) SUCH THAT x BETWEEN 0 AND cap MINIMIZE SUM(POWER(s * x - 1, 2))"
    ),
    "a1_like_terms": (
        "FROM (VALUES (1, 1.0, 2.0), (2, NULL::DOUBLE, NULL::DOUBLE)) t(id, a, b) "
        "DECIDE x(REAL) SUCH THAT x <= 5 MAXIMIZE SUM(a * x - b * x)"
    ),
    "a1_bound_column": (
        "FROM (VALUES (1, 1.0, 2.0), (2, 1.0, NULL::DOUBLE)) t(id, score, capacity) "
        "DECIDE x(REAL) SUCH THAT x BETWEEN 0 AND capacity MAXIMIZE SUM(score * x)"
    ),
    "a1_bound_column_on_an_integer_decision": (
        "FROM (VALUES (1, 1.0, 2), (2, 1.0, NULL::INTEGER)) t(id, score, cap) "
        "DECIDE x(INT) SUCH THAT x <= cap MAXIMIZE SUM(score * x)"
    ),
    "a1_bound_expression": (
        "FROM (VALUES (1, 1.0, 2.0, 1.0), (2, 1.0, NULL::DOUBLE, 1.0)) t(id, score, cap, k) "
        "DECIDE x(REAL) SUCH THAT x <= cap * k MAXIMIZE SUM(score * x)"
    ),
    "a1_constraint_coefficient": (
        "FROM (VALUES (1, 1.0, 1.0), (2, 1.0, NULL::DOUBLE)) t(id, s, w) "
        "DECIDE x(REAL) SUCH THAT w * x <= 5 MAXIMIZE SUM(s * x)"
    ),
}

# A NaN or infinite value: the same text, except that the solver adds the position of the row, which the plan cannot name.
_INVALID_VALUE_CASES = {
    "s1_score_nan": (
        "FROM (VALUES (1, 1.0), (2, 'NaN'::DOUBLE)) t(id, score) "
        "DECIDE x(BOOL) SUCH THAT SUM(x) <= 1 MAXIMIZE SUM(score * x)"
    ),
    "s1_score_infinity": (
        "FROM (VALUES (1, 1.0), (2, 'Infinity'::DOUBLE)) t(id, score) "
        "DECIDE x(BOOL) SUCH THAT SUM(x) <= 1 MAXIMIZE SUM(score * x)"
    ),
    "s1_count_bound_column_nan": (
        "FROM (VALUES (1, 1.0, 2.0), (2, 3.0, 'NaN'::DOUBLE)) t(id, score, cap) "
        "DECIDE x(BOOL) SUCH THAT SUM(x) <= cap MAXIMIZE SUM(score * x)"
    ),
    "s1_pin_column_nan": (
        "FROM (VALUES (1, 1.0, 1.0), (2, 3.0, 'NaN'::DOUBLE)) t(id, score, p) "
        "DECIDE x(BOOL) SUCH THAT x <= p AND SUM(x) <= 1 MAXIMIZE SUM(score * x)"
    ),
    "a1_objective_coefficient_nan": (
        "FROM (VALUES (1, 1.0, 2.0), (2, 'NaN'::DOUBLE, 2.0)) t(id, score, cap) "
        "DECIDE x(REAL) SUCH THAT x BETWEEN 0 AND cap MAXIMIZE SUM(score * x)"
    ),
    "a1_square_weight_infinity": (
        "FROM (VALUES (1, 1.0, 2.0), (2, 'Infinity'::DOUBLE, 2.0)) t(id, s, cap) "
        "DECIDE x(REAL) SUCH THAT x BETWEEN 0 AND cap MINIMIZE SUM(POWER(s * x - 1, 2))"
    ),
    "a1_constraint_coefficient_nan": (
        "FROM (VALUES (1, 1.0, 1.0), (2, 2.0, 'NaN'::DOUBLE)) t(id, s, w) "
        "DECIDE x(REAL) SUCH THAT w * x <= 5 MAXIMIZE SUM(s * x)"
    ),
    "a1_bound_column_nan": (
        "FROM (VALUES (1, 1.0, 2.0), (2, 1.0, 'NaN'::DOUBLE)) t(id, score, capacity) "
        "DECIDE x(REAL) SUCH THAT x BETWEEN 0 AND capacity MAXIMIZE SUM(score * x)"
    ),
}


def _errors(cli, decide):
    sql = f"SELECT id, x FROM ({decide}) q"
    return _raw(cli, sql, mode="require").stderr, _raw(cli, sql, mode="off").stderr


@pytest.mark.correctness
@pytest.mark.parametrize("name", list(_NULL_ERROR_CASES))
def test_null_value_error_is_the_solvers_error_word_for_word(decidb_cli, name):
    # One function in `decide_value_messages.hpp` builds the NULL message for both paths, and the direct plan names the
    # columns that are NULL on the failing row as the solver does. The full text is compared, never a hand-written string.
    direct, solver = _errors(decidb_cli, _NULL_ERROR_CASES[name])
    assert "is NULL" in direct or "are NULL" in direct, direct
    assert direct == solver


# A value that names many columns: the message must name exactly the columns that are NULL on the failing row, however
# many the value has, and the plan stays one that direct solve proves (it is not capped at a few names).
_WIDE_NULL_SUBSETS = {
    "one_null_first": (0,),
    "one_null_last": (-1,),
    "two_nulls": (1, 3),
    "all_null": "all",
    "non_leading_subset": (2, 4, 5),
}


def _wide_null_columns(width, subset):
    return set(range(width)) if subset == "all" else {i % width for i in subset}


def _wide_null_decide(shape, width, subset):
    columns = [f"c{i}" for i in range(width)]
    nulls = _wide_null_columns(width, subset)
    # DOUBLE throughout: a sum of DECIMAL literals can overflow, which direct solve does not take.
    good = ", ".join(["1", "1.0::DOUBLE"] + ["1.0::DOUBLE"] * width)
    bad = ", ".join(["2", "1.0::DOUBLE"] + ["NULL::DOUBLE" if i in nulls else "1.0::DOUBLE" for i in range(width)])
    source = f"FROM (VALUES ({good}), ({bad})) t(id, score, {', '.join(columns)})"
    total = " + ".join(columns)
    if shape == "s1":
        return f"{source} DECIDE x(BOOL) SUCH THAT SUM(x) <= 1 MAXIMIZE SUM(({total}) * x)"
    return f"{source} DECIDE x(REAL) SUCH THAT x <= {total} MAXIMIZE SUM(score * x)"


@pytest.mark.correctness
@pytest.mark.parametrize("shape", ["s1", "a1"])
@pytest.mark.parametrize("width", [7, 8])
@pytest.mark.parametrize("subset", list(_WIDE_NULL_SUBSETS))
def test_null_error_of_a_wide_value_is_the_solvers_error_word_for_word(decidb_cli, shape, width, subset):
    decide = _wide_null_decide(shape, width, _WIDE_NULL_SUBSETS[subset])
    direct, solver = _errors(decidb_cli, decide)
    # `require` raises the NULL error, not a "direct solve could not prove this" error, so the plan was proved.
    assert "is NULL" in direct or "are NULL" in direct, direct
    assert "Direct solve" not in direct and "direct solve" not in direct, direct
    assert direct == solver
    # The text really names the NULL columns, and only them.
    named = {int(i) for i in re.findall(r'"c(\d+)"', direct)}
    assert named == _wide_null_columns(width, _WIDE_NULL_SUBSETS[subset]), direct


@pytest.mark.correctness
@pytest.mark.parametrize("shape", ["s1", "a1"])
def test_wide_value_is_proved_by_direct_solve_and_answers_like_the_solver(decidb_cli, shape):
    decide = _wide_null_decide(shape, 8, ())
    sql = f"SELECT id, x FROM ({decide}) q ORDER BY id"
    plan = _raw(decidb_cli, f"EXPLAIN {sql}", mode="require").stdout
    assert "Direct solve rule" in plan and not _has_decide_operator(plan), plan
    direct, _ = _run(decidb_cli, sql, mode="require")
    solver, _ = _run(decidb_cli, sql, mode="off")
    assert direct == solver


@pytest.mark.correctness
@pytest.mark.parametrize("name", list(_INVALID_VALUE_CASES))
def test_invalid_value_error_is_the_solvers_error_without_the_row_position(decidb_cli, name):
    # The solver adds ` at row N`, the position of the value in its scan; a plan has no such position to report.
    direct, solver = _errors(decidb_cli, _INVALID_VALUE_CASES[name])
    assert "contains invalid value" in direct, direct
    assert " at row " in solver, solver
    assert re.sub(r" at row \d+", "", solver) == direct


@pytest.mark.correctness
@pytest.mark.parametrize("bad", ["'NaN'::DOUBLE", "'Infinity'::DOUBLE"])
def test_a1_invalid_coefficient_names_the_clause_the_user_wrote_it_in(decidb_cli, bad):
    # A bad number is reported with the noun of the clause that holds it, in both modes. `at row N` is left out of the
    # direct message on purpose, so the two are not compared whole.
    def error(constraint_expression, objective_expression, mode):
        sql = f"""
            SELECT id, x FROM (
                FROM (VALUES (1, 1.0, 1.0), (2, 2.0, {bad}), (3, 3.0, 1.0)) t(id, s, w)
                DECIDE x(REAL) SUCH THAT {constraint_expression} <= 5 MAXIMIZE SUM({objective_expression})
            ) q
        """
        return _raw(decidb_cli, sql, mode=mode).stderr

    for mode in ("require", "off"):
        in_constraint = error("w * x", "s * x", mode)
        assert "constraint coefficient contains invalid value" in in_constraint, (mode, in_constraint)
        assert "objective coefficient" not in in_constraint, (mode, in_constraint)
        in_objective = error("x", "w * x", mode)
        assert "objective coefficient contains invalid value" in in_objective, (mode, in_objective)
        assert "constraint coefficient" not in in_objective, (mode, in_objective)


@pytest.mark.correctness
def test_s1_source_bound_error_for_a_column_is_the_solvers_and_for_a_computed_bound_is_direct_solves_own(decidb_cli):
    """A bound that is one column raises the solver's NULL and NaN errors. A computed bound has no single column to name."""

    def error(cap_type, cap, bound="cap"):
        sql = f"""
            SELECT x FROM (
                FROM (VALUES (1,{cap}::{cap_type},9.0)) t(id,cap,score)
                DECIDE x(BOOL) SUCH THAT SUM(x)<={bound} MAXIMIZE SUM(score*x)
            ) q
        """
        return _raw(decidb_cli, sql, mode="require").stderr, _raw(decidb_cli, sql, mode="off").stderr

    for cap_type in ("INTEGER", "DOUBLE"):
        direct, solver = error(cap_type, "NULL")
        assert 'column "cap" is NULL' in direct and "COALESCE(cap, 0)" in direct and "WHERE" in direct, direct
        assert direct == solver
    direct, solver = error("DOUBLE", "'NaN'")
    assert "constraint right-hand side contains invalid value (NaN)" in direct, direct
    assert re.sub(r" at row \d+", "", solver) == direct
    computed, _ = error("VARCHAR", "'bad'", bound="TRY_CAST(cap AS INTEGER)")
    assert "the bound expression" in computed and "COALESCE()" in computed, computed


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

    invalid = setup.replace("(i % 101)::DOUBLE AS score", "CASE WHEN i=4999 THEN NULL ELSE 1.0 END AS score")
    count_only = f"SELECT COUNT(*) FROM ({decide.replace('<=500', '<=0')}) q"
    assert "is NULL" in _raw(decidb_cli, invalid + count_only).stderr

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
    assert "A1_INDEPENDENT_BOUNDS" in plan and "WINDOW" in plan
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
def test_invalid_forced_solver_and_disabled_optimizer_policy(decidb_cli):
    # DIAGNOSE and a valid forced backend are part of every rule's contract (test_direct_rule_contract.py).
    sql = _source_query((2.0, 9.0), 1)
    invalid = decidb_cli.with_env({"DECIDB_FORCE_SOLVER": "unknown_backend"})
    error = _raw(invalid, sql, mode="auto").stderr
    assert "DECIDB_FORCE_SOLVER=unknown_backend" in error

    disabled = _raw(decidb_cli, f"SET disabled_optimizers='decide_optimizer'; {sql}").stderr
    assert "DECIDE optimizer did not run" in disabled


@pytest.mark.correctness
def test_extension_registration_preserves_binder_errors(decidb_cli):
    sql = """
        SELECT x FROM (
            FROM (VALUES (1,2),(2,9)) n(id,score)
            DECIDE x(BOOL) SUCH THAT SUM(n: x)<=1 MAXIMIZE SUM(score*x)
        ) q
    """
    for mode in ("off", "require"):
        result = _raw(decidb_cli, sql, mode=mode)
        assert "Binder Error" in result.stderr
        assert "not a decision of n" in result.stderr


# --- Reading a stored table twice instead of holding wide rows ---------------------------------------------------------
#
# Direct solve checks every input row before it releases one, so its plan holds the rows. A stored column the parent
# reads and the rule never uses is read from a second scan of the table, joined on the row id, so it is not held.

_SECOND_READ_ROWS = 5000

# (decision clause, score of row i, how a bad last row is spelled). Scores are distinct for S1, so its optimum is unique.
_SECOND_READ_RULES = {
    "s1": (
        "DECIDE x(BOOL) SUCH THAT SUM(x) <= 500 MAXIMIZE SUM(score * x)",
        "((i * 37) % 5000)::DOUBLE",
    ),
    "a1": (
        "DECIDE x(REAL) SUCH THAT x BETWEEN 0 AND cap MINIMIZE SUM(POWER(x - score, 2))",
        "(i % 11)::DOUBLE",
    ),
}


def _second_read_setup(rule, bad_last=False):
    _, score = _SECOND_READ_RULES[rule]
    if bad_last:
        score = f"CASE WHEN i = {_SECOND_READ_ROWS - 1} THEN 'NaN'::DOUBLE ELSE {score} END"
    return f"""
        CREATE TEMP TABLE rr_src AS
        SELECT i AS id, {score} AS score, (5 + i % 7)::DOUBLE AS cap, 'payload-' || i::VARCHAR AS payload
        FROM range({_SECOND_READ_ROWS}) t(i);
        CREATE TEMP TABLE rr_one AS SELECT 1 AS k;
    """


def _second_read_decide(rule, relation="rr_src"):
    return f"FROM {relation} {_SECOND_READ_RULES[rule][0]}"


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
    setup = _second_read_setup(rule)
    decide = _second_read_decide(rule)
    query = f"SELECT id, payload, x FROM ({decide}) q ORDER BY id"

    plan = _raw(decidb_cli, setup + f"EXPLAIN {query}").stdout
    assert "Direct solve rule" in plan
    assert _table_scans(plan) == 2 and "HASH_JOIN" in plan

    # The same plan with the table round-tripped through a plan serializer, the solver, and a source that is held
    # because it sits over a join.
    second_read, _ = _run(decidb_cli.with_verify_serializer(), setup + query)
    solver, _ = _run(decidb_cli, setup + query, mode="off")
    joined = _second_read_decide(rule, "(SELECT s.* FROM rr_src s JOIN rr_one o ON o.k = 1) s")
    held_query = f"SELECT id, payload, x FROM ({joined}) q ORDER BY id"
    held_plan = _raw(decidb_cli, setup + f"EXPLAIN {held_query}").stdout
    assert "Direct solve rule" in held_plan and _table_scans(held_plan) == 1
    held, _ = _run(decidb_cli, setup + held_query)

    assert len(second_read) == _SECOND_READ_ROWS
    assert second_read[0][1] == "payload-0" and second_read[-1][1] == f"payload-{_SECOND_READ_ROWS - 1}"
    _same_answers(second_read, solver)
    _same_answers(held, solver)


@pytest.mark.correctness
@pytest.mark.parametrize("rule", ["s1", "a1"])
def test_second_read_still_raises_on_a_bad_value_in_the_last_row_under_limit(decidb_cli, rule):
    setup = _second_read_setup(rule, bad_last=True)
    decide = _second_read_decide(rule)
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
    setup = _second_read_setup(rule)
    decide = _second_read_decide(rule)

    def scans(sql, expected_rule_hit=True):
        plan = _raw(decidb_cli, setup + f"EXPLAIN {sql}").stdout
        assert ("Direct solve rule" in plan) == expected_rule_hit
        return _table_scans(plan)

    # Nothing the rule does not use is read: no second scan.
    assert scans(f"SELECT COUNT(*), SUM(x) FROM ({decide}) q") == 1
    assert scans(f"SELECT score, x FROM ({decide}) q") == 1
    # A stored column nothing in the rule uses is read twice, and the payload alone is enough.
    assert scans(f"SELECT payload FROM ({decide}) q") == 2
    assert scans(f"SELECT SUM(length(payload)), SUM(x) FROM ({decide}) q") == 2

    # A computed column keeps holding, and so does a column of a volatile or aggregated source.
    volatile = _second_read_decide(rule, "(SELECT *, random() AS r FROM rr_src) s")
    assert scans(f"SELECT payload, r, x FROM ({volatile}) q") == 1
    grouped = _second_read_decide(
        rule, "(SELECT id, MAX(score) AS score, MAX(cap) AS cap, MAX(payload) AS payload FROM rr_src GROUP BY id) s"
    )
    assert scans(f"SELECT payload, x FROM ({grouped}) q") == 1
    computed = _second_read_decide(rule, "(SELECT id, score, cap, payload || '!' AS payload FROM rr_src) s")
    assert scans(f"SELECT payload, x FROM ({computed}) q") == 1

    # A source over a join keeps holding.
    joined = _second_read_decide(rule, "(SELECT s.* FROM rr_src s JOIN rr_one o ON o.k = 1) s")
    assert scans(f"SELECT payload, x FROM ({joined}) q") == 1

    # A filter on the table is read twice too, and the second read returns only the rows the first one kept.
    filtered = _second_read_decide(rule, "(SELECT * FROM rr_src WHERE id % 2 = 0) s")
    filtered_query = f"SELECT id, payload, x FROM ({filtered}) q ORDER BY id"
    assert scans(filtered_query) == 2
    kept, _ = _run(decidb_cli, setup + filtered_query)
    solver, _ = _run(decidb_cli, setup + filtered_query, mode="off")
    assert len(kept) == _SECOND_READ_ROWS // 2
    _same_answers(kept, solver)


@pytest.mark.correctness
@pytest.mark.parametrize("rule", ["s1", "a1"])
def test_second_read_moves_only_variable_width_columns(decidb_cli, rule):
    setup = _second_read_setup(rule)
    decide = _second_read_decide(rule)

    def scans(query):
        plan = _raw(decidb_cli, setup + f"EXPLAIN {query}").stdout
        assert "Direct solve rule" in plan
        return _table_scans(plan)

    # A fixed-width stored column the parent reads stays in the single read.
    narrow_query = f"SELECT id, x FROM ({decide}) q ORDER BY id"
    assert scans(narrow_query) == 1
    # A VARCHAR one moves to the second read, and so does one next to a fixed-width column that stays.
    wide_query = f"SELECT payload, x FROM ({decide}) q ORDER BY payload"
    assert scans(wide_query) == 2
    mixed_query = f"SELECT id, payload, x FROM ({decide}) q ORDER BY id"
    assert scans(mixed_query) == 2

    for query in (narrow_query, wide_query, mixed_query):
        direct, _ = _run(decidb_cli, setup + query)
        solver, _ = _run(decidb_cli, setup + query, mode="off")
        assert len(direct) == _SECOND_READ_ROWS
        _same_answers(direct, solver)


@pytest.mark.correctness
def test_a1_plan_that_streams_does_not_read_twice(decidb_cli):
    setup = _second_read_setup("a1")
    # Every row sees the same bounds and coefficients, so the plan holds nothing and a second read would only add a join.
    decide = "FROM rr_src DECIDE x(REAL) SUCH THAT x BETWEEN 0 AND 10 MAXIMIZE SUM(2 * x)"
    query = f"SELECT id, payload, x FROM ({decide}) q ORDER BY id"
    plan = _raw(decidb_cli, setup + f"EXPLAIN {query}").stdout
    assert "A1_INDEPENDENT_BOUNDS" in plan and _table_scans(plan) == 1
    rows, _ = _run(decidb_cli, setup + query)
    solver, _ = _run(decidb_cli, setup + query, mode="off")
    _same_answers(rows, solver)
