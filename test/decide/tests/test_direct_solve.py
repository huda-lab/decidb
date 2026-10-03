"""First S1 direct-solve path: admission, result contract, and solver fallback."""

import itertools
import re
import math

import pytest


@pytest.fixture(autouse=True)
def _allow_direct_path(monkeypatch):
    # The suite also runs with a process-wide forced HiGHS backend. This file
    # opts into the direct path; forced-backend cases use their own CLI wrapper.
    monkeypatch.delenv("DECIDB_FORCE_SOLVER", raising=False)


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


def _optimum(scores, capacity, sense):
    feasible = (
        sum(score * x for score, x in zip(scores, assignment))
        for assignment in itertools.product((0, 1), repeat=len(scores))
        if sum(assignment) <= capacity
    )
    return max(feasible) if sense == "MAXIMIZE" else min(feasible)


def _interval_query(scores, constraint, sense="MAXIMIZE"):
    values = ", ".join(f"({i}, CAST('{score}' AS DOUBLE))" for i, score in enumerate(scores))
    return f"""
        SELECT id, score, x FROM (
            FROM (VALUES {values}) t(id, score)
            DECIDE x(BOOL) SUCH THAT {constraint}
            {sense} SUM(score * x)
        ) q ORDER BY id
    """


def _interval_optimum(scores, lower, upper, sense):
    feasible = (
        sum(score * x for score, x in zip(scores, assignment))
        for assignment in itertools.product((0, 1), repeat=len(scores))
        if lower <= sum(assignment) <= upper
    )
    return max(feasible) if sense == "MAXIMIZE" else min(feasible)


@pytest.mark.correctness
@pytest.mark.parametrize(
    "scores,capacity,sense",
    [
        ((2.0, -3.0, 5.0, 0.0), 0, "MAXIMIZE"),
        ((2.0, -3.0, 5.0, 0.0), 1, "MAXIMIZE"),
        ((2.0, -3.0, 5.0, 0.0), 10, "MAXIMIZE"),
        ((2.0, -3.0, 5.0, 0.0), 1, "MINIMIZE"),
        ((2.0, -3.0, 5.0, 0.0), 10, "MINIMIZE"),
        ((4.0, 4.0, 1.0, -1.0), 1, "MAXIMIZE"),
        ((-0.0, 5e-324, -5e-324), 1, "MAXIMIZE"),
    ],
)
def test_s1_matches_independent_enumeration(decidb_cli, scores, capacity, sense):
    rows, columns = _run(decidb_cli, _source_query(scores, capacity, sense))
    assert columns == ["id", "score", "x"]
    assert len(rows) == len(scores)
    assert [row[0] for row in rows] == list(range(len(scores)))
    assert all(type(row[2]) is int and row[2] in (0, 1) for row in rows)
    assert sum(row[2] for row in rows) <= capacity
    actual = sum(score * row[2] for score, row in zip(scores, rows))
    expected = _optimum(scores, capacity, sense)
    assert math.isclose(actual, expected, rel_tol=0, abs_tol=1e-14 if expected != 5e-324 else 0)


@pytest.mark.correctness
@pytest.mark.parametrize(
    "scores,constraint,lower,upper,sense",
    [
        ((9.0, 5.0, -1.0), "SUM(x)>=2", 2, 3, "MAXIMIZE"),
        ((-1.0, -2.0, -3.0), "SUM(x)>=2", 2, 3, "MAXIMIZE"),
        ((9.0, 5.0, -1.0), "SUM(x)=3", 3, 3, "MAXIMIZE"),
        ((9.0, 5.0, -1.0), "SUM(x)=0", 0, 0, "MAXIMIZE"),
        ((9.0, 5.0, -1.0), "SUM(x)>=1 AND SUM(x)<=2", 1, 2, "MAXIMIZE"),
        ((9.0, -1.0, -2.0), "SUM(x)>=2 AND SUM(x)<=2", 2, 2, "MAXIMIZE"),
        ((-9.0, -5.0, 1.0), "SUM(x)>=2", 2, 3, "MINIMIZE"),
        ((-9.0, -5.0, 1.0), "SUM(x)=3", 3, 3, "MINIMIZE"),
        ((9.0, 0.0, -1.0), "SUM(x)>=2", 2, 3, "MAXIMIZE"),
        ((9.0, 5.0, -1.0), "2<=SUM(x)", 2, 3, "MAXIMIZE"),
    ],
)
def test_s1_global_intervals_match_independent_enumeration(
    decidb_cli, scores, constraint, lower, upper, sense
):
    sql = _interval_query(scores, constraint, sense)
    rows, columns = _run(decidb_cli, sql)
    assert columns == ["id", "score", "x"]
    assert len(rows) == len(scores)
    assert [row[0] for row in rows] == list(range(len(scores)))
    assert all(type(row[2]) is int and row[2] in (0, 1) for row in rows)
    assert lower <= sum(row[2] for row in rows) <= upper
    actual = sum(row[1] * row[2] for row in rows)
    assert actual == _interval_optimum(scores, lower, upper, sense)
    solver, _ = _run(decidb_cli, sql, mode="off")
    assert lower <= sum(row[2] for row in solver) <= upper
    assert sum(row[1] * row[2] for row in solver) == actual


@pytest.mark.correctness
@pytest.mark.parametrize(
    "constraint,lower,upper",
    [
        ("SUM(x)<=1.5", 0, 1),
        ("SUM(x)<2", 0, 1),
        ("SUM(x)>=1.5", 2, 3),
        ("SUM(x)>1", 2, 3),
        ("SUM(x)>=-0.5", 0, 3),
        ("SUM(x)>-1", 0, 3),
        ("SUM(x)>=0.5 AND SUM(x)<=2.5", 1, 2),
        ("SUM(x)>1 AND SUM(x)<3", 2, 2),
        ("SUM(x)<=1+1", 0, 2),
        ("SUM(x)<=abs(-1)", 0, 1),
        ("SUM(x)>=1+1", 2, 3),
        ("SUM(x)>1.0/2", 1, 3),
        ("SUM(x)=COALESCE(NULL::INTEGER,2)", 2, 2),
        ("SUM(x)>=1 AND SUM(x)>=2 AND SUM(x)<=3 AND SUM(x)<=2", 2, 2),
        ("SUM(x)=2 AND SUM(x)<=3 AND SUM(x)>=1", 2, 2),
        ("SUM(x)>0.5 AND SUM(x)>=2 AND SUM(x)<3", 2, 2),
        ("SUM(1*x)<=1", 0, 1),
        ("SUM(x*CAST(1 AS DOUBLE))>=2", 2, 3),
        ("SUM((1+0)*x)>=1 AND SUM(x)<=2", 1, 2),
    ],
)
def test_s1_finite_bounds_normalize_to_inclusive_counts(
    decidb_cli, decidb_cli_highs, decidb_cli_gurobi, constraint, lower, upper
):
    sql = _interval_query((9.0, 5.0, -1.0), constraint)
    direct, _ = _run(decidb_cli, sql)
    expected = _interval_optimum((9.0, 5.0, -1.0), lower, upper, "MAXIMIZE")
    solver_rows = [_run(backend, sql, mode="off")[0] for backend in (decidb_cli_highs, decidb_cli_gurobi)]
    for rows in (direct, *solver_rows):
        assert lower <= sum(row[2] for row in rows) <= upper
        assert sum(row[1] * row[2] for row in rows) == expected


@pytest.mark.correctness
@pytest.mark.parametrize("sense,offset", [("MAXIMIZE", 10), ("MINIMIZE", -2)])
def test_s1_constant_objective_offset_keeps_the_optimum(
    decidb_cli, decidb_cli_highs, decidb_cli_gurobi, sense, offset
):
    scores = (9.0, 5.0, -1.0)
    sql = f"""
        SELECT id,score,x FROM (
            FROM (VALUES (1,9.0),(2,5.0),(3,-1.0)) t(id,score)
            DECIDE x(BOOL) SUCH THAT SUM(x)<=1
            {sense} SUM(score*x){offset:+d}
        ) q ORDER BY id
    """
    expected = _optimum(scores, 1, sense) + offset
    for backend, mode in ((decidb_cli, "require"), (decidb_cli_highs, "off"), (decidb_cli_gurobi, "off")):
        rows, _ = _run(backend, sql, mode=mode)
        assert sum(row[2] for row in rows) <= 1
        assert sum(row[1] * row[2] for row in rows) + offset == expected


@pytest.mark.correctness
@pytest.mark.parametrize(
    "sense,objective,coefficients",
    [
        ("MAXIMIZE", "SUM(a*x)+SUM(b*x)", (10.0, 12.0, 4.0)),
        ("MAXIMIZE", "SUM(a*x)-SUM(b*x)", (8.0, -2.0, -6.0)),
        ("MINIMIZE", "SUM(a*x)-SUM(b*x)", (8.0, -2.0, -6.0)),
        ("MAXIMIZE", "SUM(a*x)+SUM(b*x)-SUM(x)", (9.0, 11.0, 3.0)),
        ("MAXIMIZE", "-SUM(a*x)", (-9.0, -5.0, 1.0)),
        ("MINIMIZE", "-SUM(a*x)", (-9.0, -5.0, 1.0)),
    ],
)
def test_s1_additive_linear_score_matches_solvers_and_enumeration(
    decidb_cli, decidb_cli_highs, decidb_cli_gurobi, sense, objective, coefficients
):
    sql = f"""
        SELECT id,a,b,x FROM (
            FROM (VALUES (1,9.0::DOUBLE,1.0::DOUBLE),
                         (2,5.0::DOUBLE,7.0::DOUBLE),
                         (3,-1.0::DOUBLE,5.0::DOUBLE)) t(id,a,b)
            DECIDE x(BOOL) SUCH THAT SUM(x)<=1
            {sense} {objective}
        ) q ORDER BY id
    """
    expected = _optimum(coefficients, 1, sense)
    for backend, mode in ((decidb_cli, "require"), (decidb_cli_highs, "off"), (decidb_cli_gurobi, "off")):
        rows, _ = _run(backend, sql, mode=mode)
        assert sum(row[3] for row in rows) <= 1
        assert sum(coefficient * row[3] for coefficient, row in zip(coefficients, rows)) == expected


@pytest.mark.correctness
@pytest.mark.parametrize("bad", ["NULL::DOUBLE", "'NaN'::DOUBLE", "1e308::DOUBLE"])
def test_s1_additive_score_validates_every_row_under_limit(
    decidb_cli, decidb_cli_highs, decidb_cli_gurobi, bad
):
    # The last value overflows only after the two finite objective terms are added.
    second_term = "1e308::DOUBLE" if bad == "1e308::DOUBLE" else "2.0::DOUBLE"
    sql = f"""
        SELECT id FROM (
            FROM (VALUES (1,9.0::DOUBLE,1.0::DOUBLE),
                         (2,8.0::DOUBLE,1.0::DOUBLE),
                         (3,{bad},{second_term})) t(id,a,b)
            DECIDE x(BOOL) SUCH THAT SUM(x)<=1
            MAXIMIZE SUM(a*x)+SUM(b*x)
        ) q LIMIT 1
    """
    for backend, mode in ((decidb_cli, "require"), (decidb_cli_highs, "off"), (decidb_cli_gurobi, "off")):
        result = _raw(backend, sql, mode=mode)
        assert "Invalid Input Error" in result.stderr, result.stderr


@pytest.mark.correctness
def test_s1_additive_score_with_grouped_bounds_and_fixed_rows(decidb_cli, decidb_cli_highs, decidb_cli_gurobi):
    sql = """
        SELECT id, dept, a, b, x FROM (
            FROM (VALUES (1,'A',9.0::DOUBLE,1.0::DOUBLE,TRUE),
                         (2,'A',5.0::DOUBLE,7.0::DOUBLE,FALSE),
                         (3,'B',4.0::DOUBLE,1.0::DOUBLE,FALSE),
                         (4,'B',3.0::DOUBLE,4.0::DOUBLE,FALSE)) t(id,dept,a,b,pinned)
            DECIDE x(BOOL) SUCH THAT x=1 WHEN pinned AND SUM(x)=1 PER dept
            MAXIMIZE SUM(a*x)+SUM(b*x)
        ) q ORDER BY id
    """
    for backend, mode in ((decidb_cli, "require"), (decidb_cli_highs, "off"), (decidb_cli_gurobi, "off")):
        rows, _ = _run(backend, sql, mode=mode)
        assert [row[4] for row in rows] == [1, 0, 0, 1]


@pytest.mark.correctness
@pytest.mark.parametrize(
    "constraint",
    ["SUM(x)<=-0.5", "SUM(x)<0", "SUM(x)=1.5", "SUM(x)>=2.5 AND SUM(x)<=2.75",
     "SUM(x)>=2 AND SUM(x)<=1 AND SUM(x)>=1"],
)
def test_s1_impossible_integral_bounds_guard_nonempty_groups(
    decidb_cli, decidb_cli_highs, decidb_cli_gurobi, constraint
):
    sql = _interval_query((9.0, 5.0, -1.0), constraint)
    assert "DECIDE optimization is infeasible" in _raw(decidb_cli, sql, mode="require").stderr
    for backend in (decidb_cli_highs, decidb_cli_gurobi):
        assert "DECIDE optimization is infeasible" in _raw(backend, sql, mode="off").stderr
    empty = f"""
        SELECT id,x FROM (
            FROM (SELECT 1 AS id, 9.0 AS score WHERE false) t
            DECIDE x(BOOL) SUCH THAT {constraint} MAXIMIZE SUM(score*x)
        ) q
    """
    for mode in ("require", "off"):
        rows, _ = _run(decidb_cli, empty, mode=mode)
        assert rows == []


@pytest.mark.correctness
@pytest.mark.parametrize("scope", ["PER dept", "WHEN active PER dept"])
def test_s1_impossible_group_bound_bypasses_excluded_rows(decidb_cli, scope):
    query = f"""
        SELECT id,x FROM (
            FROM (VALUES (1,NULL::VARCHAR,TRUE,9.0), (2,'A',FALSE,5.0)) t(id,dept,active,score)
            DECIDE x(BOOL) SUCH THAT SUM(x)<=-1 {scope} MAXIMIZE SUM(score*x)
        ) q ORDER BY id
    """
    expected = (
        "DECIDE optimization is infeasible"
        if scope == "PER dept"
        else "DECIDE empty row set for aggregate in constraint"
    )
    for mode in ("require", "off"):
        assert expected in _raw(decidb_cli, query, mode=mode).stderr


@pytest.mark.correctness
@pytest.mark.parametrize(
    "constraint,rows",
    [
        ("SUM(x)<=1 WHEN active", "(1,'A',FALSE,9.0),(2,'B',FALSE,5.0)"),
        ("SUM(x)<=1 PER dept", "(1,NULL::VARCHAR,TRUE,9.0),(2,NULL::VARCHAR,TRUE,5.0)"),
        ("SUM(x)<=1 WHEN active PER dept", "(1,NULL::VARCHAR,TRUE,9.0),(2,'A',FALSE,5.0)"),
    ],
)
def test_s1_scoped_empty_aggregate_matches_solver(decidb_cli, constraint, rows):
    decide = f"""
        FROM (VALUES {rows}) t(id,dept,active,score)
        DECIDE x(BOOL) SUCH THAT {constraint} MAXIMIZE SUM(score*x)
    """
    for parent in (f"SELECT id,x FROM ({decide}) q", f"SELECT x FROM ({decide}) q LIMIT 1"):
        for mode in ("require", "off"):
            assert "DECIDE empty row set for aggregate in constraint" in _raw(decidb_cli, parent, mode=mode).stderr


@pytest.mark.correctness
@pytest.mark.parametrize(
    "constraint,rows",
    [
        ("SUM(x)<=1 WHEN active", "(1,'A',FALSE,9.0),(2,'B',FALSE,NULL::DOUBLE)"),
        ("SUM(x)<=1 PER dept", "(1,NULL::VARCHAR,TRUE,9.0),(2,NULL::VARCHAR,TRUE,NULL::DOUBLE)"),
    ],
)
def test_s1_empty_scope_error_precedes_invalid_score(decidb_cli, constraint, rows):
    sql = f"""
        SELECT id,x FROM (
            FROM (VALUES {rows}) t(id,dept,active,score)
            DECIDE x(BOOL) SUCH THAT {constraint} MAXIMIZE SUM(score*x)
        ) q
    """
    for mode in ("require", "off"):
        assert "DECIDE empty row set for aggregate in constraint" in _raw(decidb_cli, sql, mode=mode).stderr


@pytest.mark.correctness
def test_s1_when_can_bypass_one_group_if_another_is_active(decidb_cli):
    sql = """
        SELECT id,x FROM (
            FROM (VALUES (1,'A',FALSE,9.0),(2,'B',TRUE,5.0)) t(id,dept,active,score)
            DECIDE x(BOOL) SUCH THAT SUM(x)<=1 WHEN active PER dept MAXIMIZE SUM(score*x)
        ) q ORDER BY id
    """
    for mode in ("require", "off"):
        rows, _ = _run(decidb_cli, sql, mode=mode)
        assert rows == [(1, 1), (2, 1)]


@pytest.mark.correctness
def test_s1_lower_infeasibility_and_empty_input(decidb_cli):
    for constraint in ("SUM(x)>=4", "SUM(x)=4", "SUM(x)>=4 AND SUM(x)<=5"):
        sql = _interval_query((9.0, 5.0, -1.0), constraint)
        for mode in ("require", "off"):
            assert "DECIDE optimization is infeasible" in _raw(decidb_cli, sql, mode=mode).stderr
        decide = f"""
            FROM (VALUES (1,9.0),(2,5.0),(3,-1.0)) t(id,score)
            DECIDE x(BOOL) SUCH THAT {constraint} MAXIMIZE SUM(score*x)
        """
        assert "DECIDE optimization is infeasible" in _raw(
            decidb_cli, f"SELECT COUNT(*) FROM ({decide}) q", mode="require"
        ).stderr
        assert "DECIDE optimization is infeasible" in _raw(
            decidb_cli, f"SELECT x FROM ({decide}) q LIMIT 1", mode="require"
        ).stderr

    empty = """
        SELECT id,x FROM (
            FROM (SELECT 1 AS id, 9.0 AS score WHERE false) t
            DECIDE x(BOOL) SUCH THAT SUM(x)>=1 MAXIMIZE SUM(score*x)
        ) q
    """
    for mode in ("require", "off"):
        rows, _ = _run(decidb_cli, empty, mode=mode)
        assert rows == []


_SCOPED_ROWS = (
    (1, "A", "N", True, 9.0),
    (2, "A", "N", True, -1.0),
    (3, "A", "S", False, 8.0),
    (4, "B", "N", True, -4.0),
    (5, "B", "N", False, 6.0),
    (6, None, "N", True, 7.0),
    (7, "B", "S", True, 5.0),
)


def _scoped_query(constraint, sense="MAXIMIZE"):
    values = ", ".join(
        f"({i}, {('NULL::VARCHAR' if dept is None else repr(dept))}, {repr(region)}, "
        f"{'TRUE' if active else 'FALSE'}, {score})"
        for i, dept, region, active, score in _SCOPED_ROWS
    )
    return f"""
        SELECT id, dept, region, active, score, x FROM (
            FROM (VALUES {values}) t(id,dept,region,active,score)
            DECIDE x(BOOL) SUCH THAT {constraint} {sense} SUM(score*x)
        ) q ORDER BY id
    """


def _scoped_optimum(lower, upper, keys, when, sense):
    return _fixed_optimum(lower, upper, keys, when, (), (), sense)


def _fixed_assignment_is_feasible(assignment, lower, upper, keys, when, fixed_one, fixed_zero):
    groups = {}
    for row, x in zip(_SCOPED_ROWS, assignment):
        row_id, dept, region, active, _ = row
        if (row_id in fixed_one and x != 1) or (row_id in fixed_zero and x != 0):
            return False
        if when and not active:
            continue
        values_by_key = {"dept": dept, "region": region}
        key = tuple(values_by_key[name] for name in keys)
        if any(value is None for value in key):
            continue
        groups[key] = groups.get(key, 0) + x
    return all(lower <= count <= upper for count in groups.values())


def _fixed_optimum(lower, upper, keys, when, fixed_one, fixed_zero, sense):
    values = (
        sum(row[4] * x for row, x in zip(_SCOPED_ROWS, assignment))
        for assignment in itertools.product((0, 1), repeat=len(_SCOPED_ROWS))
        if _fixed_assignment_is_feasible(assignment, lower, upper, keys, when, fixed_one, fixed_zero)
    )
    return max(values) if sense == "MAXIMIZE" else min(values)


@pytest.mark.correctness
@pytest.mark.parametrize(
    "constraint,lower,upper,keys,when,fixed_one,fixed_zero,sense",
    [
        ("x=1+0 WHEN id=2 AND SUM(x)<=1", 0, 1, (), False, (2,), (), "MAXIMIZE"),
        ("x<=0 WHEN id=1 AND SUM(x)>=2", 2, 7, (), False, (), (1,), "MAXIMIZE"),
        ("x=1 WHEN id=2 AND x=1 WHEN id=4 AND x=0 WHEN id=1 AND SUM(x)<=1 PER dept",
         0, 1, ("dept",), False, (2, 4), (1,), "MAXIMIZE"),
        ("x=1 WHEN id=2 AND x=0 WHEN id=1 AND SUM(x)>=2 PER dept",
         2, 7, ("dept",), False, (2,), (1,), "MAXIMIZE"),
        ("x=1 WHEN id=5 AND x<=0 WHEN id=1 AND SUM(x)>=1 WHEN active PER dept "
         "AND SUM(x)<=1 WHEN active PER dept",
         1, 1, ("dept",), True, (5,), (1,), "MAXIMIZE"),
        ("x>=1 WHEN id=2 AND x<1 WHEN id=4 AND SUM(x)=2 PER dept",
         2, 2, ("dept",), False, (2,), (4,), "MINIMIZE"),
        ("x=0 WHEN id=6 AND SUM(x)<=1 PER dept",
         0, 1, ("dept",), False, (), (6,), "MAXIMIZE"),
        ("x=1 WHEN id=2 AND SUM(x)>=1 PER dept AND SUM(x)>=2 PER dept "
         "AND SUM(x)<=3 PER dept AND SUM(x)<=2 PER dept",
         2, 2, ("dept",), False, (2,), (), "MAXIMIZE"),
    ],
)
def test_s1_boolean_pins_match_enumeration_and_solvers(
    decidb_cli, decidb_cli_highs, decidb_cli_gurobi,
    constraint, lower, upper, keys, when, fixed_one, fixed_zero, sense,
):
    sql = _scoped_query(constraint, sense)
    direct, _ = _run(decidb_cli.with_verify_serializer(), sql)
    assert [row[0] for row in direct] == [row[0] for row in _SCOPED_ROWS]
    assignment = [row[5] for row in direct]
    assert _fixed_assignment_is_feasible(assignment, lower, upper, keys, when, fixed_one, fixed_zero)
    expected = _fixed_optimum(lower, upper, keys, when, fixed_one, fixed_zero, sense)
    assert sum(row[4] * row[5] for row in direct) == expected
    for backend in (decidb_cli_highs, decidb_cli_gurobi):
        solver, _ = _run(backend, sql, mode="off")
        assert sum(row[4] * row[5] for row in solver) == expected


@pytest.mark.correctness
@pytest.mark.parametrize(
    "constraint",
    [
        "x=1 WHEN id<3 AND SUM(x)<=1",
        "x=0 WHEN id<3 AND SUM(x)>=6",
        "x=1 WHEN id=1 AND x=0 WHEN id=1 AND SUM(x)<=1",
    ],
)
def test_s1_boolean_pin_infeasibility_matches_solvers(
    decidb_cli, decidb_cli_highs, decidb_cli_gurobi, constraint
):
    sql = _scoped_query(constraint)
    for backend, mode in ((decidb_cli, "require"), (decidb_cli_highs, "off"), (decidb_cli_gurobi, "off")):
        assert "DECIDE optimization is infeasible" in _raw(backend, sql, mode=mode).stderr


@pytest.mark.correctness
def test_s1_boolean_pin_empty_input_and_score_error_precedence(decidb_cli):
    empty = """
        SELECT x FROM (
            FROM (SELECT 1 AS id, 9.0 AS score WHERE false) t
            DECIDE x(BOOL) SUCH THAT x=1 AND SUM(x)>=2 MAXIMIZE SUM(score*x)
        ) q
    """
    for mode in ("require", "off"):
        assert _run(decidb_cli, empty, mode=mode)[0] == []
    invalid = """
        SELECT x FROM (
            FROM (VALUES (1,NULL::DOUBLE)) t(id,score)
            DECIDE x(BOOL) SUCH THAT x=1 AND x=0 AND SUM(x)<=0 MAXIMIZE SUM(score*x)
        ) q
    """
    for mode in ("require", "off"):
        assert "NULL" in _raw(decidb_cli, invalid, mode=mode).stderr


@pytest.mark.correctness
def test_s1_boolean_pin_null_when_bypasses_row(decidb_cli):
    sql = """
        SELECT id,x FROM (
            FROM (VALUES (1,NULL::BOOLEAN,9.0),(2,TRUE,-1.0)) t(id,pin,score)
            DECIDE x(BOOL) SUCH THAT x=1 WHEN pin AND SUM(x)<=1 MAXIMIZE SUM(score*x)
        ) q ORDER BY id
    """
    for mode in ("require", "off"):
        assert _run(decidb_cli, sql, mode=mode)[0] == [(1, 0), (2, 1)]


@pytest.mark.correctness
def test_s1_boolean_pin_late_infeasibility_survives_outer_limit(decidb_cli):
    decide = """
        FROM (VALUES (1,9.0),(2,5.0),(3,-1.0)) t(id,score)
        DECIDE x(BOOL) SUCH THAT x=1 WHEN id=3 AND x=0 WHEN id=3
            AND SUM(x)<=2 MAXIMIZE SUM(score*x)
    """
    sql = f"SELECT x FROM ({decide}) q LIMIT 1"
    for mode in ("require", "off"):
        assert "DECIDE optimization is infeasible" in _raw(decidb_cli, sql, mode=mode).stderr


@pytest.mark.correctness
@pytest.mark.parametrize(
    "constraint,lower,upper,keys,when,sense",
    [
        ("SUM(x)<=1 PER dept", 0, 1, ("dept",), False, "MAXIMIZE"),
        ("SUM(x)<=1 PER dept", 0, 1, ("dept",), False, "MINIMIZE"),
        ("SUM(x)>=3 PER dept", 3, 7, ("dept",), False, "MAXIMIZE"),
        ("SUM(x)=2 PER dept", 2, 2, ("dept",), False, "MAXIMIZE"),
        ("SUM(x)>=2 WHEN active PER dept AND SUM(x)<=2 WHEN active PER dept", 2, 2,
         ("dept",), True, "MAXIMIZE"),
        ("SUM(x)<=1 WHEN active PER dept", 0, 1, ("dept",), True, "MAXIMIZE"),
        ("SUM(x) WHEN active <=1 PER dept", 0, 1, ("dept",), True, "MAXIMIZE"),
        ("SUM(x) WHEN active >=1 PER dept AND SUM(x) WHEN active <=2 PER dept", 1, 2,
         ("dept",), True, "MAXIMIZE"),
        ("SUM(x)<=1 PER (dept,region)", 0, 1, ("dept", "region"), False, "MAXIMIZE"),
        ("SUM(1*x)<=1 PER dept", 0, 1, ("dept",), False, "MAXIMIZE"),
        ("SUM(x)<=1 WHEN active", 0, 1, (), True, "MAXIMIZE"),
        ("SUM(x) WHEN active <=1", 0, 1, (), True, "MAXIMIZE"),
    ],
)
def test_s1_scoped_cardinality_matches_enumeration(decidb_cli, constraint, lower, upper, keys, when, sense):
    sql = _scoped_query(constraint, sense)
    direct, columns = _run(decidb_cli.with_verify_serializer(), sql)
    assert columns == ["id", "dept", "region", "active", "score", "x"]
    assert len(direct) == len(_SCOPED_ROWS)
    assert [row[0] for row in direct] == [row[0] for row in _SCOPED_ROWS]
    assert all(type(row[5]) is int and row[5] in (0, 1) for row in direct)
    value = sum(row[4] * row[5] for row in direct)
    assert value == _scoped_optimum(lower, upper, keys, when, sense)
    solver, _ = _run(decidb_cli, sql, mode="off")
    assert sum(row[4] * row[5] for row in solver) == value


@pytest.mark.correctness
def test_s1_aggregate_local_when_interval_matches_both_solvers(decidb_cli, decidb_cli_highs, decidb_cli_gurobi):
    sql = _scoped_query("SUM(x) WHEN active >=1 PER dept AND SUM(x) WHEN active <=1 PER dept")
    optimum = _scoped_optimum(1, 1, ("dept",), True, "MAXIMIZE")
    for backend, mode in ((decidb_cli.with_verify_serializer(), "require"),
                          (decidb_cli_highs, "off"), (decidb_cli_gurobi, "off")):
        rows, _ = _run(backend, sql, mode=mode)
        assert [row[0] for row in rows] == [source[0] for source in _SCOPED_ROWS]
        assert sum(row[4] * row[5] for row in rows) == optimum


@pytest.mark.correctness
def test_s1_aggregate_local_when_source_bound_reads_filtered_out_rhs(decidb_cli):
    sql = """
        SELECT id,x FROM (
            FROM (VALUES (1,9.0,TRUE,2.0),(2,8.0,TRUE,2.0),
                         (3,7.0,FALSE,0.0)) t(id,score,active,cap)
            DECIDE x(BOOL) SUCH THAT SUM(x) WHEN active <= cap
                MAXIMIZE SUM(score*x)
        ) q ORDER BY id
    """
    explain = _raw(decidb_cli, f"EXPLAIN {sql}", mode="require").stdout
    assert "aggregate-local" in explain and "RHS reduction" in explain
    for mode in ("require", "off"):
        assert _run(decidb_cli, sql, mode=mode)[0] == [(1, 0), (2, 0), (3, 1)]


@pytest.mark.correctness
@pytest.mark.parametrize(
    "operator,caps,expected",
    [
        ("<=", (2.0, 2.0, 0.5, 1.0, 2.0, 0.0), 22.0),
        (">=", (1.0, 1.0, 2.0, 1.0, 0.0, 0.0), 39.0),
        ("=", (1.0, 1.0, 1.0, 1.0, 1.0, 0.0), 31.0),
    ],
)
def test_s1_aggregate_local_when_source_bound_matches_enumeration_and_solvers(
    decidb_cli, decidb_cli_highs, decidb_cli_gurobi, operator, caps, expected,
):
    base_rows = ((1, "A", True, 9.0), (2, "A", True, 8.0), (3, "A", False, 7.0),
                 (4, "B", True, 6.0), (5, "B", False, 5.0), (6, None, True, 4.0))
    rows = tuple((row_id, dept, active, cap, score)
                 for (row_id, dept, active, score), cap in zip(base_rows, caps))
    values = ",".join(
        f"({row_id},{repr(dept) if dept is not None else 'NULL::VARCHAR'},"
        f"{'TRUE' if active else 'FALSE'},{cap}::DOUBLE,{score})"
        for row_id, dept, active, cap, score in rows
    )
    sql = f"""
        SELECT id,x FROM (
            FROM (VALUES {values}) t(id,dept,active,cap,score)
            DECIDE x(BOOL) SUCH THAT SUM(x) WHEN active {operator} cap PER dept
                MAXIMIZE SUM(score*x)
        ) q ORDER BY id
    """
    compare = {"<=": lambda count, cap: count <= cap,
               ">=": lambda count, cap: count >= cap,
               "=": lambda count, cap: count == cap}[operator]
    feasible_values = []
    for assignment in itertools.product((0, 1), repeat=len(rows)):
        groups = {}
        for (row_id, dept, active, cap, score), x in zip(rows, assignment):
            if dept is not None:
                groups.setdefault(dept, []).append((active, cap, x))
        if all(all(compare(sum(x for active, _, x in members if active), cap)
                   for _, cap, _ in members) for members in groups.values()):
            feasible_values.append(sum(row[4] * x for row, x in zip(rows, assignment)))
    assert max(feasible_values) == expected
    for backend, mode in ((decidb_cli.with_verify_serializer(), "require"),
                          (decidb_cli_highs, "off"), (decidb_cli_gurobi, "off")):
        result, _ = _run(backend, sql, mode=mode)
        assert [row[0] for row in result] == [source[0] for source in rows]
        assert sum(source[4] * row[1] for source, row in zip(rows, result)) == expected


@pytest.mark.correctness
def test_s1_aggregate_local_when_mixed_rhs_membership(decidb_cli):
    sql = """
        SELECT id,x FROM (
            FROM (VALUES (1,'A',9.0,TRUE,1.0,2.0),
                         (2,'A',8.0,TRUE,2.0,2.0),
                         (3,'A',7.0,FALSE,0.0,1.0)) t(id,dept,score,active,top_cap,local_cap)
            DECIDE x(BOOL) SUCH THAT SUM(x)<=top_cap WHEN active PER dept
                AND SUM(x) WHEN active <=local_cap PER dept MAXIMIZE SUM(score*x)
        ) q ORDER BY id
    """
    for mode in ("require", "off"):
        assert _run(decidb_cli, sql, mode=mode)[0] == [(1, 1), (2, 0), (3, 1)]


@pytest.mark.correctness
def test_s1_aggregate_local_when_source_equality_ignores_inactive_only_group(decidb_cli):
    sql = """
        SELECT id,x FROM (
            FROM (VALUES (1,'A',9.0,TRUE,1.0),
                         (2,'A',8.0,TRUE,1.0),
                         (3,'B',7.0,FALSE,1.0),
                         (4,'B',6.0,FALSE,2.0)) t(id,dept,score,active,cap)
            DECIDE x(BOOL) SUCH THAT SUM(x) WHEN active = cap PER dept
                MAXIMIZE SUM(score*x)
        ) q ORDER BY id
    """
    for mode in ("require", "off"):
        assert _run(decidb_cli, sql, mode=mode)[0] == [(1, 1), (2, 0), (3, 1), (4, 1)]


@pytest.mark.correctness
def test_s1_aggregate_local_when_source_bound_error_order(decidb_cli):
    invalid = """
        SELECT x FROM (
            FROM (VALUES (1,9.0,TRUE,1.0::DOUBLE),
                         (2,8.0,FALSE,NULL::DOUBLE)) t(id,score,active,cap)
            DECIDE x(BOOL) SUCH THAT SUM(x) WHEN active <= cap
                MAXIMIZE SUM(score*x)
        ) q LIMIT 1
    """
    empty = """
        SELECT x FROM (
            FROM (VALUES (1,NULL::DOUBLE,FALSE,NULL::DOUBLE)) t(id,score,active,cap)
            DECIDE x(BOOL) SUCH THAT SUM(x) WHEN active <= cap
                MAXIMIZE SUM(score*x)
        ) q
    """
    for mode in ("require", "off"):
        assert "NULL" in _raw(decidb_cli, invalid, mode=mode).stderr
        assert "empty row set for aggregate" in _raw(decidb_cli, empty, mode=mode).stderr


@pytest.mark.correctness
def test_s1_aggregate_local_when_empty_error_precedes_score(decidb_cli):
    sql = """
        SELECT x FROM (
            FROM (VALUES (1,NULL::DOUBLE,FALSE)) t(id,score,active)
            DECIDE x(BOOL) SUCH THAT SUM(x) WHEN active <= 1
                MAXIMIZE SUM(score*x)
        ) q
    """
    for mode in ("require", "off"):
        assert "empty row set for aggregate" in _raw(decidb_cli, sql, mode=mode).stderr


@pytest.mark.correctness
def test_s1_scoped_bypass_infeasibility_and_scope_mismatch(decidb_cli, decidb_cli_highs, decidb_cli_gurobi):
    sql = _scoped_query("SUM(x)<=1 WHEN active PER dept")
    direct, _ = _run(decidb_cli, sql)
    assert [row[5] for row in direct] == [1, 0, 1, 0, 1, 1, 1]
    for backend in (decidb_cli_highs, decidb_cli_gurobi):
        solver, _ = _run(backend, sql, mode="off")
        assert sum(row[4] * row[5] for row in solver) == sum(row[4] * row[5] for row in direct)

    infeasible = _scoped_query("SUM(x)>=4 PER dept")
    for mode in ("require", "off"):
        assert "DECIDE optimization is infeasible" in _raw(decidb_cli, infeasible, mode=mode).stderr
    assert "DECIDE optimization is infeasible" in _raw(
        decidb_cli, f"SELECT x FROM ({infeasible}) q LIMIT 1"
    ).stderr

    mismatch = _scoped_query("SUM(x)>=1 PER dept AND SUM(x)<=2 PER region")
    assert "constraint_scope" in _raw(decidb_cli, mismatch).stderr
    plan = _raw(decidb_cli, f"EXPLAIN {mismatch}", mode="auto").stdout
    assert _has_decide_operator(plan) and "Direct solve" not in plan


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
@pytest.mark.parametrize(
    "constraint,objective,value",
    [
        ("SUM(2*x - x) <= 2", "MAXIMIZE SUM(p*x)", "p*x"),
        ("SUM((1+0)*x) <= 2 AND 1*x <= 0 WHEN id = 1", "MAXIMIZE SUM(p*x)", "p*x"),
        ("SUM(x) <= 2", "MAXIMIZE SUM(p*x*q)", "p*x*q"),
        ("SUM(x) <= 2", "MINIMIZE SUM(-(p*x))", "-(p*x)"),
        ("SUM(x) <= 2", "MAXIMIZE SUM((p + q) * x)", "(p + q) * x"),
    ],
)
def test_s1_reads_terms_by_meaning(decidb_cli, decidb_cli_highs, decidb_cli_gurobi, constraint, objective, value):
    # S1 reads the shared term split, so a count or score written differently but meaning the same is the same
    # shape: unit contributions add up to one x, and every linear term in x scores.
    sql = f"""
        SELECT SUM({value}) FROM (
            FROM (VALUES (1, 5.0::DOUBLE, 2.0::DOUBLE), (2, -1.0::DOUBLE, 3.0::DOUBLE),
                         (3, 4.0::DOUBLE, 1.5::DOUBLE), (4, 2.0::DOUBLE, 0.5::DOUBLE)) t(id, p, q)
            DECIDE x(BOOL) SUCH THAT {constraint} {objective}
        ) q
    """
    plan = _raw(decidb_cli, f"EXPLAIN {sql}").stdout
    assert "S1_CARDINALITY_INTERVAL" in plan, _raw(decidb_cli, sql).stderr
    (direct,), _ = _run(decidb_cli, sql)
    for solver in (decidb_cli_highs, decidb_cli_gurobi):
        (expected,), _ = _run(solver, sql, mode="off")
        assert direct[0] == pytest.approx(expected[0])


@pytest.mark.correctness
@pytest.mark.parametrize(
    "constraint,objective,value,quadratic",
    [
        # MAX(ABS(x)) <= 2 holds for every Boolean x; read as SUM(x) <= 2 it caps the count.
        ("norm(x,'inf') <= 2", "MAXIMIZE SUM(val*x)", "val*x", False),
        ("SUM(x) <= 2", "MAXIMIZE SUM(val*x) - norm(val*x,1)", "val*x - ABS(val*x)", False),
        ("SUM(x) <= 2", "MAXIMIZE SUM(val*x) - norm(val*x,0)", "val*x - (val*x <> 0)::INT", False),
        # SUM(POWER(val*x, 2)) ranks rows by val^2, not by val.
        ("SUM(x) >= 2", "MINIMIZE norm(val*x,2)", "POWER(val*x, 2)", True),
    ],
)
def test_norm_is_not_read_as_a_sum(decidb_cli, decidb_cli_highs, decidb_cli_gurobi, constraint, objective, value,
                                   quadratic):
    # The binder carries norm(e, p) as a SUM(e) aggregate tagged with its order. Direct solve must refuse it under
    # every order and leave the query to the solver, whose answer the default path then returns.
    sql = f"""
        SELECT SUM({value}) FROM (
            FROM (VALUES (1, -5.0::DOUBLE), (2, 1.0::DOUBLE), (3, 2.0::DOUBLE), (4, 3.0::DOUBLE)) t(id, val)
            DECIDE x(BOOL) SUCH THAT {constraint} {objective}
        ) q
    """
    required = _raw(decidb_cli, sql)
    assert "decide_direct_solve=require:" in required.stderr, required.stderr
    plan = _raw(decidb_cli, f"EXPLAIN {sql}", mode="auto").stdout
    assert _has_decide_operator(plan) and "Direct solve" not in plan
    (default,), _ = _run(decidb_cli, sql, mode="auto")
    solvers = (decidb_cli_gurobi,) if quadratic else (decidb_cli_highs, decidb_cli_gurobi)
    for solver in solvers:
        (expected,), _ = _run(solver, sql, mode="off")
        assert default[0] == pytest.approx(expected[0])


@pytest.mark.correctness
@pytest.mark.parametrize("score", ["NULL::DOUBLE", "'NaN'::DOUBLE", "'Infinity'::DOUBLE"])
def test_invalid_score_error_matches_solver_wording(decidb_cli, score):
    # The direct path names a bad score the way the solver does, so a user sees one message whichever path ran.
    # The solver also reports the row number; the direct plan has no row to report.
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
def test_computed_score_error_does_not_name_a_column(decidb_cli):
    # A score that is not one bare column has no single column to name; the message keeps the solver's generic
    # wording and still points at the fix.
    sql = """
        SELECT id, x FROM (
            FROM (VALUES (1, 2.0, 1.0), (2, NULL::DOUBLE, 1.0)) t(id, a, b)
            DECIDE x(BOOL) SUCH THAT SUM(x) <= 1 MAXIMIZE SUM((a + b) * x)
        ) q
    """
    error = _raw(decidb_cli, sql, mode="require").stderr
    assert "a value used in the optimization is NULL" in error and "COALESCE()" in error, error


@pytest.mark.correctness
def test_path_selection_and_exact_capacity(decidb_cli):
    sql = _source_query((2.0, 9.0), 1)
    hit = _raw(decidb_cli, f"EXPLAIN {sql}").stdout
    assert "Direct solve rule" in hit and "S1_CARDINALITY_INTERVAL" in hit
    assert "WINDOW" in hit and not _has_decide_operator(hit)
    auto_hit = _raw(decidb_cli, f"EXPLAIN {sql}", mode="auto").stdout
    assert "Direct solve rule" in auto_hit
    off = _raw(decidb_cli, f"EXPLAIN {sql}", mode="off").stdout
    assert _has_decide_operator(off) and "Direct solve" not in off

    rows, _ = _run(decidb_cli, _source_query((2.0, 9.0), 9007199254740992))
    assert [row[2] for row in rows] == [1, 1]
    for capacity in ("9007199254740993", "'Infinity'::DOUBLE"):
        error = _raw(decidb_cli, _source_query((2.0, 9.0), capacity)).stderr
        assert "cardinality bound must be a finite consistent foldable numeric expression" in error

    extra = """
        SELECT x FROM (
            FROM (VALUES (1, 2), (2, 9)) t(id, score)
            DECIDE x(BOOL) SUCH THAT SUM(x) <= 1 AND x <= 1
            MAXIMIZE SUM(score * x)
        ) q
    """
    # A miss under auto leaves the solver plan exactly as it was: no direct-solve text in EXPLAIN.
    miss = _raw(decidb_cli, f"EXPLAIN {extra}", mode="auto").stdout
    assert _has_decide_operator(miss) and "Direct solve" not in miss
    required_miss = _raw(decidb_cli, extra).stderr
    assert "constraint_shape" in required_miss and "solver skipped=true" in required_miss


@pytest.mark.correctness
def test_structural_near_misses(decidb_cli):
    cases = [
        "SUM(x) <= 1 MAXIMIZE SUM(score*x) WHEN active",
        "SUM(x) <= 1 MAXIMIZE SUM(random()*x)",
        "SUM(x) <> 1 MAXIMIZE SUM(score*x)",
    ]
    for clause in cases:
        sql = f"""
            SELECT x FROM (
                FROM (VALUES (1, 2.0, TRUE), (2, 9.0, FALSE)) t(id,score,active)
                DECIDE x(BOOL) SUCH THAT {clause}
            ) q
        """
        error = _raw(decidb_cli, sql).stderr
        assert "decide_direct_solve=require:" in error, (clause, error)


@pytest.mark.correctness
@pytest.mark.parametrize(
    "scope,first_row",
    [
        ("WHEN active PER dept", "(1,NULL::DOUBLE,FALSE,1,9.0)"),
        ("PER dept", "(1,NULL::DOUBLE,TRUE,NULL::INTEGER,9.0)"),
    ],
)
def test_s1_source_numeric_bound_validates_null_bypassed_rows(decidb_cli, scope, first_row):
    sql = f"""
        SELECT x FROM (
            FROM (VALUES {first_row},(2,1.0::DOUBLE,TRUE,1,2.0)) t(id,cap,active,dept,score)
            DECIDE x(BOOL) SUCH THAT SUM(x)<=cap {scope} MAXIMIZE SUM(score*x)
        ) q
    """
    for mode in ("require", "off", "auto"):
        assert "NULL" in _raw(decidb_cli, sql, mode=mode).stderr


@pytest.mark.correctness
def test_s1_source_numeric_empty_scope_precedes_bound_error(decidb_cli):
    sql = """
        SELECT x FROM (
            FROM (VALUES (NULL::DOUBLE,FALSE,NULL::DOUBLE)) t(cap,active,score)
            DECIDE x(BOOL) SUCH THAT SUM(x)<=cap WHEN active MAXIMIZE SUM(score*x)
        ) q
    """
    for mode in ("require", "off", "auto"):
        assert "empty row set for aggregate" in _raw(decidb_cli, sql, mode=mode).stderr


_SOURCE_BOUND_ROWS = (
    (1, "A", True, 2, 9.0),
    (2, "A", True, 1, 5.0),
    (3, "A", False, 0, -2.0),
    (4, "B", True, 1, 8.0),
    (5, "B", True, 2, -1.0),
    (6, None, True, 3, 7.0),
)


def _source_bound_query(rows, comparison, scope, static_clause="", pin=False, sense="MAXIMIZE"):
    values = ",".join(
        f"({row_id},{repr(dept) if dept is not None else 'NULL::VARCHAR'},"
        f"{'TRUE' if active else 'FALSE'},{cap},{score})"
        for row_id, dept, active, cap, score in rows
    )
    pin_clause = " AND x=1 WHEN id=5" if pin else ""
    return f"""
        SELECT id,x FROM (
            FROM (VALUES {values}) t(id,dept,active,cap,score)
            DECIDE x(BOOL) SUCH THAT SUM(x){comparison}cap {scope}{static_clause}{pin_clause}
            {sense} SUM(score*x)
        ) q ORDER BY id
    """


def _source_bound_optimum(rows, comparison, scope, static_lower, static_upper, pin, sense):
    def feasible(assignment):
        if pin and assignment[4] != 1:
            return False
        groups = {}
        for row, x in zip(rows, assignment):
            _, dept, active, cap, _ = row
            if ("WHEN active" in scope and not active) or ("PER dept" in scope and dept is None):
                continue
            groups.setdefault(dept if "PER dept" in scope else "global", []).append((cap, x))
        for members in groups.values():
            count = sum(x for _, x in members)
            if static_lower is not None and count < static_lower:
                return False
            if static_upper is not None and count > static_upper:
                return False
            for cap, _ in members:
                if not {
                    "<=": count <= cap, "<": count < cap, ">=": count >= cap,
                    ">": count > cap, "=": count == cap,
                }[comparison]:
                    return False
        return True

    values = (
        sum(row[4] * x for row, x in zip(rows, assignment))
        for assignment in itertools.product((0, 1), repeat=len(rows))
        if feasible(assignment)
    )
    return max(values) if sense == "MAXIMIZE" else min(values)


@pytest.mark.correctness
@pytest.mark.parametrize(
    "comparison,scope,static_clause,static_lower,static_upper,pin,sense,caps",
    [
        ("<=", "", "", None, None, False, "MAXIMIZE", None),
        ("<=", "WHEN active PER dept", "", None, None, False, "MAXIMIZE", None),
        ("<", "WHEN active PER dept", "", None, None, False, "MAXIMIZE", None),
        (">=", "WHEN active PER dept", "", None, None, False, "MAXIMIZE", None),
        (">", "WHEN active PER dept", "", None, None, False, "MAXIMIZE", (0, 0, 0, 0, 0, 0)),
        ("<=", "WHEN active PER dept", " AND SUM(x)>=1 WHEN active PER dept", 1, None, False,
         "MAXIMIZE", None),
        (">=", "WHEN active PER dept", " AND SUM(x)<=2 WHEN active PER dept", None, 2, False,
         "MAXIMIZE", None),
        ("<=", "WHEN active PER dept", "", None, None, True, "MAXIMIZE", None),
        (">=", "WHEN active PER dept", "", None, None, True, "MAXIMIZE", None),
        ("<=", "WHEN active PER dept", "", None, None, False, "MINIMIZE", None),
        ("=", "WHEN active PER dept", "", None, None, False, "MAXIMIZE", (1, 1, 1, 1, 1, 1)),
        ("=", "", "", None, None, False, "MAXIMIZE", (1, 1, 1, 1, 1, 1)),
        ("<=", "WHEN active PER dept", "", None, None, False,
         "MAXIMIZE", (1.5, 1.9, 0.0, 2.5, 2.1, 3.0)),
        (">=", "WHEN active PER dept", "", None, None, False,
         "MAXIMIZE", (0.5, 1.5, 0.0, 0.5, 1.5, 3.0)),
    ],
)
def test_s1_source_numeric_bound_matches_enumeration_and_solvers(
    decidb_cli, decidb_cli_highs, decidb_cli_gurobi,
    comparison, scope, static_clause, static_lower, static_upper, pin, sense, caps,
):
    rows = _SOURCE_BOUND_ROWS if caps is None else tuple(
        (row_id, dept, active, cap, score)
        for (row_id, dept, active, _, score), cap in zip(_SOURCE_BOUND_ROWS, caps)
    )
    sql = _source_bound_query(rows, comparison, scope, static_clause, pin, sense)
    optimum = _source_bound_optimum(rows, comparison, scope, static_lower, static_upper, pin, sense)
    for backend, mode in ((decidb_cli.with_verify_serializer(), "require"),
                          (decidb_cli_highs, "off"), (decidb_cli_gurobi, "off")):
        result, columns = _run(backend, sql, mode=mode)
        assert columns == ["id", "x"]
        assert [row[0] for row in result] == [row[0] for row in rows]
        assert all(type(row[1]) is int and row[1] in (0, 1) for row in result)
        assert sum(source[4] * row[1] for source, row in zip(rows, result)) == optimum


@pytest.mark.correctness
@pytest.mark.parametrize(
    "rows,constraint,expected",
    [
        ("(1,-1,9.0),(2,1,8.0)", "SUM(x)<=cap", "infeasible"),
        ("(1,3,9.0),(2,3,8.0)", "SUM(x)>=cap", "infeasible"),
        ("(1,1,9.0),(2,2,8.0)", "SUM(x)=cap", "varies"),
        ("(1,2,9.0),(2,2,8.0)", "SUM(x)>=cap AND SUM(x)<=1", "infeasible"),
    ],
)
def test_s1_source_integer_bound_errors_match_solvers(
    decidb_cli, decidb_cli_highs, decidb_cli_gurobi, rows, constraint, expected
):
    sql = f"""
        SELECT x FROM (
            FROM (VALUES {rows}) t(id,cap,score)
            DECIDE x(BOOL) SUCH THAT {constraint} MAXIMIZE SUM(score*x)
        ) q
    """
    for backend, mode in ((decidb_cli, "require"), (decidb_cli_highs, "off"), (decidb_cli_gurobi, "off")):
        error = _raw(backend, sql, mode=mode).stderr
        phrase = "infeasible" if expected == "infeasible" else "more than one value" if mode == "off" else "varies"
        assert phrase in error


@pytest.mark.correctness
@pytest.mark.parametrize(
    "scope,first_row",
    [
        ("WHEN active PER dept", "(1,NULL::INTEGER,FALSE,1,9.0)"),
        ("PER dept", "(1,NULL::INTEGER,TRUE,NULL::INTEGER,9.0)"),
    ],
)
def test_s1_source_integer_bound_validates_bypassed_rows(decidb_cli, scope, first_row):
    sql = f"""
        SELECT x FROM (
            FROM (VALUES {first_row},(2,1,TRUE,1,2.0)) t(id,cap,active,dept,score)
            DECIDE x(BOOL) SUCH THAT SUM(x)<=cap {scope} MAXIMIZE SUM(score*x)
        ) q
    """
    for mode in ("require", "off"):
        assert "NULL" in _raw(decidb_cli, sql, mode=mode).stderr


@pytest.mark.correctness
def test_s1_null_source_bound_error_names_the_column(decidb_cli):
    """A NULL bound column is reported like the solver path: the column, then COALESCE or WHERE."""

    def error(cap_type, cap, bound="cap", mode="require"):
        sql = f"""
            SELECT x FROM (
                FROM (VALUES (1,{cap}::{cap_type},9.0)) t(id,cap,score)
                DECIDE x(BOOL) SUCH THAT SUM(x)<={bound} MAXIMIZE SUM(score*x)
            ) q
        """
        return _raw(decidb_cli, sql, mode=mode).stderr

    plain = 'DECIDE: column "cap" is NULL. Impute it with COALESCE(cap, 0) or filter those rows out with a WHERE'
    assert plain in error("INTEGER", "NULL", mode="off")
    assert plain in error("INTEGER", "NULL")
    floating = 'DECIDE: column "cap" is NULL or NaN. Impute NULLs with COALESCE(cap, 0) or filter those rows out'
    assert floating in error("DOUBLE", "NULL")
    assert floating in error("DOUBLE", "'NaN'")
    computed = error("VARCHAR", "'bad'", bound="TRY_CAST(cap AS INTEGER)")
    assert "DECIDE: the bound expression is NULL. Impute it with COALESCE(), or filter those rows out" in computed


@pytest.mark.correctness
def test_s1_source_integer_bound_error_order_and_late_validation(decidb_cli):
    late = """
        SELECT x FROM (
            FROM (VALUES (1,1,9.0),(2,1,8.0),(3,NULL::INTEGER,7.0)) t(id,cap,score)
            DECIDE x(BOOL) SUCH THAT SUM(x)<=cap MAXIMIZE SUM(score*x)
        ) q LIMIT 1
    """
    empty = """
        SELECT x FROM (
            FROM (VALUES (NULL::INTEGER,FALSE,NULL::DOUBLE)) t(cap,active,score)
            DECIDE x(BOOL) SUCH THAT SUM(x)<=cap WHEN active MAXIMIZE SUM(score*x)
        ) q
    """
    cap_before_score = """
        SELECT x FROM (
            FROM (VALUES (NULL::INTEGER,NULL::DOUBLE),(1,9.0)) t(cap,score)
            DECIDE x(BOOL) SUCH THAT SUM(x)<=cap MAXIMIZE SUM(score*x)
        ) q
    """
    for mode in ("require", "off"):
        assert "NULL" in _raw(decidb_cli, late, mode=mode).stderr
        assert "empty row set for aggregate" in _raw(decidb_cli, empty, mode=mode).stderr
        error = _raw(decidb_cli, cap_before_score, mode=mode).stderr
        assert "NULL" in error and "coefficient" not in error


@pytest.mark.correctness
@pytest.mark.parametrize(
    "source_type,bound,admitted",
    [
        ("TINYINT", "cap", True),
        ("SMALLINT", "cap", True),
        ("INTEGER", "cap", True),
        ("INTEGER", "CAST(cap AS DOUBLE)", True),
        ("BIGINT", "cap", True),
        ("DOUBLE", "cap", True),
        ("FLOAT", "cap", True),
        ("HUGEINT", "cap", True),
        ("DECIMAL(18,2)", "cap", True),
        ("DOUBLE", "CAST(cap AS INTEGER)", False),
    ],
)
def test_s1_source_bound_type_proof(decidb_cli, source_type, bound, admitted):
    sql = f"""
        SELECT id,x FROM (
            FROM (VALUES (1,1::{source_type},9.0),(2,1::{source_type},8.0)) t(id,cap,score)
            DECIDE x(BOOL) SUCH THAT SUM(x)<={bound} MAXIMIZE SUM(score*x)
        ) q ORDER BY id
    """
    if admitted:
        direct, _ = _run(decidb_cli, sql)
        solver, _ = _run(decidb_cli, sql, mode="off")
        assert direct == solver == [(1, 1), (2, 0)]
    else:
        assert "cardinality bound must be" in _raw(decidb_cli, sql, mode="require").stderr
        solver, _ = _run(decidb_cli, sql, mode="auto")
        assert solver == [(1, 1), (2, 0)]


@pytest.mark.correctness
@pytest.mark.parametrize("bound,optimum", [("COALESCE(cap,1.0)", 16.0), ("COALESCE(cap,fallback)", 7.0)])
def test_s1_source_expression_bound_matches_enumeration_and_solvers(
    decidb_cli, decidb_cli_highs, decidb_cli_gurobi, bound, optimum,
):
    sql = f"""
        SELECT id,x FROM (
            FROM (VALUES (1,'A',1.0::DOUBLE,1.0::DOUBLE,9.0),
                         (2,'A',NULL::DOUBLE,0.5::DOUBLE,8.0),
                         (3,'B',2.0::DOUBLE,2.0::DOUBLE,7.0),
                         (4,'B',1.0::DOUBLE,1.0::DOUBLE,-2.0)) t(id,dept,cap,fallback,score)
            DECIDE x(BOOL) SUCH THAT SUM(x)<={bound} PER dept MAXIMIZE SUM(score*x)
        ) q ORDER BY id
    """
    for backend, mode in ((decidb_cli.with_verify_serializer(), "require"),
                          (decidb_cli_highs, "off"), (decidb_cli_gurobi, "off")):
        rows, _ = _run(backend, sql, mode=mode)
        assert [row[0] for row in rows] == [1, 2, 3, 4]
        assert sum(score * row[1] for score, row in zip((9.0, 8.0, 7.0, -2.0), rows)) == optimum


@pytest.mark.correctness
@pytest.mark.parametrize("invalid", ["'bad'", "'NaN'"])
def test_s1_try_cast_bound_validates_expression_result_on_bypassed_row(decidb_cli, invalid):
    sql = f"""
        SELECT x FROM (
            FROM (VALUES (1,{invalid}::VARCHAR,FALSE,NULL::INTEGER,9.0),
                         (2,'1'::VARCHAR,TRUE,1,8.0)) t(id,raw_cap,active,dept,score)
            DECIDE x(BOOL) SUCH THAT SUM(x)<=TRY_CAST(raw_cap AS DOUBLE)
                WHEN active PER dept MAXIMIZE SUM(score*x)
        ) q
    """
    for mode in ("require", "off"):
        error = _raw(decidb_cli, sql, mode=mode).stderr
        assert ("NULL" if invalid == "'bad'" else "NaN") in error


@pytest.mark.correctness
def test_s1_multiple_source_expression_bounds_match_solvers(decidb_cli, decidb_cli_highs, decidb_cli_gurobi):
    sql = """
        SELECT id,x FROM (
            FROM (VALUES (1,'A',NULL::DOUBLE,1.0::DOUBLE,9.0),
                         (2,'A',1.0::DOUBLE,NULL::DOUBLE,8.0),
                         (3,'B',1.0::DOUBLE,1.0::DOUBLE,7.0),
                         (4,'B',0.0::DOUBLE,2.0::DOUBLE,-2.0)) t(id,dept,lo,hi,score)
            DECIDE x(BOOL) SUCH THAT SUM(x)>=COALESCE(lo,1.0) PER dept
                AND SUM(x)<=COALESCE(hi,1.0) PER dept MAXIMIZE SUM(score*x)
        ) q ORDER BY id
    """
    for backend, mode in ((decidb_cli.with_verify_serializer(), "require"),
                          (decidb_cli_highs, "off"), (decidb_cli_gurobi, "off")):
        rows, _ = _run(backend, sql, mode=mode)
        assert [row[0] for row in rows] == [1, 2, 3, 4]
        assert sum(score * row[1] for score, row in zip((9.0, 8.0, 7.0, -2.0), rows)) == 16.0


@pytest.mark.correctness
@pytest.mark.parametrize(
    "capacity,comparison,expected",
    [
        ("1.5::DOUBLE", "<=", (1, 0)),
        ("1.5::DOUBLE", "<", (1, 0)),
        ("1.5::DOUBLE", ">=", (1, 1)),
        ("1.5::DOUBLE", ">", (1, 1)),
        ("1.5::DOUBLE", "=", None),
        ("-0.5::DOUBLE", "<=", None),
        ("'-Infinity'::DOUBLE", "<=", None),
        ("'-Infinity'::DOUBLE", ">=", (1, 1)),
        ("'Infinity'::DOUBLE", "<=", (1, 1)),
        ("'Infinity'::DOUBLE", ">=", None),
        ("9007199254740993::BIGINT", "<=", (1, 1)),
        ("9007199254740993::BIGINT", ">=", None),
        ("9223372036854775807::BIGINT", "<=", (1, 1)),
        ("9223372036854775807::BIGINT", ">=", None),
    ],
)
def test_s1_source_numeric_bound_matches_solver_at_boundaries(decidb_cli, capacity, comparison, expected):
    sql = f"""
        SELECT id,x FROM (
            FROM (VALUES (1,{capacity},9.0),(2,{capacity},8.0)) t(id,cap,score)
            DECIDE x(BOOL) SUCH THAT SUM(x){comparison}cap MAXIMIZE SUM(score*x)
        ) q ORDER BY id
    """
    for mode in ("require", "off"):
        if expected is None:
            assert "infeasible" in _raw(decidb_cli, sql, mode=mode).stderr
        else:
            rows, _ = _run(decidb_cli, sql, mode=mode)
            assert tuple(row[1] for row in rows) == expected


@pytest.mark.correctness
def test_s1_source_numeric_equality_uses_solver_double_values(decidb_cli):
    sql = """
        SELECT id,x FROM (
            FROM (VALUES (1,9007199254740992::BIGINT,9.0),
                         (2,9007199254740993::BIGINT,8.0)) t(id,cap,score)
            DECIDE x(BOOL) SUCH THAT SUM(x)=cap MAXIMIZE SUM(score*x)
        ) q ORDER BY id
    """
    for mode in ("require", "off"):
        error = _raw(decidb_cli, sql, mode=mode).stderr
        assert "infeasible" in error and "more than one value" not in error and "varies" not in error


@pytest.mark.correctness
def test_s1_source_numeric_bound_validates_nan_bypassed_rows(decidb_cli):
    sql = """
        SELECT x FROM (
            FROM (VALUES (1,'NaN'::DOUBLE,FALSE,NULL::INTEGER,9.0),
                         (2,1.0::DOUBLE,TRUE,1,8.0)) t(id,cap,active,dept,score)
            DECIDE x(BOOL) SUCH THAT SUM(x)<=cap WHEN active PER dept MAXIMIZE SUM(score*x)
        ) q
    """
    for mode in ("require", "off"):
        assert "NaN" in _raw(decidb_cli, sql, mode=mode).stderr


_MULTI_SOURCE_ROWS = (
    (1, "A", True, 1.0, 2.5, 2.0, 9.0),
    (2, "A", True, 1.5, 2.0, 1.5, 5.0),
    (3, "A", False, 0.0, 0.0, 0.0, -2.0),
    (4, "B", True, 0.5, 1.5, 2.0, 8.0),
    (5, "B", True, 1.0, 2.5, 1.0, -1.0),
    (6, None, True, 3.0, 0.0, 0.0, 7.0),
)


def _multi_source_query(rows, clauses, scope, pin=False):
    values = ",".join(
        f"({row_id},{repr(dept) if dept is not None else 'NULL::VARCHAR'},"
        f"{'TRUE' if active else 'FALSE'},{lo}::DOUBLE,{hi}::DOUBLE,{other}::DOUBLE,{score})"
        for row_id, dept, active, lo, hi, other, score in rows
    )
    bounds = " AND ".join(f"SUM(x){operator}{column} {scope}" for operator, column in clauses)
    pins = " AND x=1 WHEN id=5" if pin else ""
    return f"""
        SELECT id,x FROM (
            FROM (VALUES {values}) t(id,dept,active,lo,hi,other,score)
            DECIDE x(BOOL) SUCH THAT {bounds}{pins} MAXIMIZE SUM(score*x)
        ) q ORDER BY id
    """


def _multi_source_optimum(rows, clauses, scope, pin):
    column = {"lo": 3, "hi": 4, "other": 5}
    compare = {
        "<=": lambda count, cap: count <= cap,
        "<": lambda count, cap: count < cap,
        ">=": lambda count, cap: count >= cap,
        ">": lambda count, cap: count > cap,
        "=": lambda count, cap: count == cap,
    }

    def feasible(assignment):
        if pin and assignment[4] != 1:
            return False
        groups = {}
        for row, x in zip(rows, assignment):
            _, dept, active, *_ = row
            if ("WHEN active" in scope and not active) or ("PER dept" in scope and dept is None):
                continue
            groups.setdefault(dept if "PER dept" in scope else "global", []).append((row, x))
        for members in groups.values():
            count = sum(x for _, x in members)
            for operator, name in clauses:
                if any(not compare[operator](count, row[column[name]]) for row, _ in members):
                    return False
        return True

    return max(
        sum(row[6] * x for row, x in zip(rows, assignment))
        for assignment in itertools.product((0, 1), repeat=len(rows))
        if feasible(assignment)
    )


@pytest.mark.correctness
@pytest.mark.parametrize(
    "clauses,scope,pin",
    [
        (((">=", "lo"), ("<=", "hi")), "WHEN active PER dept", False),
        ((("<=", "hi"), ("<=", "other")), "WHEN active PER dept", False),
        (((">=", "lo"), (">=", "other")), "WHEN active PER dept", False),
        (((">=", "lo"), ("<=", "hi")), "WHEN active PER dept", True),
        ((("<=", "hi"), ("<", "other")), "WHEN active PER dept", False),
        (((">", "lo"), (">=", "other")), "WHEN active PER dept", False),
    ],
)
def test_s1_multiple_source_bounds_match_enumeration_and_solvers(
    decidb_cli, decidb_cli_highs, decidb_cli_gurobi, clauses, scope, pin,
):
    sql = _multi_source_query(_MULTI_SOURCE_ROWS, clauses, scope, pin)
    optimum = _multi_source_optimum(_MULTI_SOURCE_ROWS, clauses, scope, pin)
    for backend, mode in ((decidb_cli.with_verify_serializer(), "require"),
                          (decidb_cli_highs, "off"), (decidb_cli_gurobi, "off")):
        rows, columns = _run(backend, sql, mode=mode)
        assert columns == ["id", "x"]
        assert [row[0] for row in rows] == [source[0] for source in _MULTI_SOURCE_ROWS]
        assert all(type(row[1]) is int and row[1] in (0, 1) for row in rows)
        assert sum(source[6] * row[1] for source, row in zip(_MULTI_SOURCE_ROWS, rows)) == optimum


@pytest.mark.correctness
def test_s1_three_global_source_bounds_match_enumeration_and_solvers(
    decidb_cli, decidb_cli_highs, decidb_cli_gurobi,
):
    rows = (
        (1, None, True, 1.0, 2.5, 2.0, 9.0),
        (2, None, True, 1.0, 2.0, 2.0, 8.0),
        (3, None, True, 1.0, 2.5, 2.0, -1.0),
    )
    clauses = ((">=", "lo"), ("<=", "hi"), ("<=", "other"))
    sql = _multi_source_query(rows, clauses, "")
    optimum = _multi_source_optimum(rows, clauses, "", False)
    for backend, mode in ((decidb_cli.with_verify_serializer(), "require"),
                          (decidb_cli_highs, "off"), (decidb_cli_gurobi, "off")):
        result, _ = _run(backend, sql, mode=mode)
        assert sum(source[6] * row[1] for source, row in zip(rows, result)) == optimum == 17.0


@pytest.mark.correctness
@pytest.mark.parametrize(
    "clauses,expected",
    [
        ("SUM(x)>=lo AND SUM(x)<=hi", "infeasible"),
        ("SUM(x)=lo AND SUM(x)<=hi", "equality"),
        ("SUM(x)<=hi AND SUM(x)=lo", "NULL"),
    ],
)
def test_s1_multiple_source_bounds_error_order(decidb_cli, clauses, expected):
    if expected == "infeasible":
        values = "(1,2.0::DOUBLE,1.0::DOUBLE,9.0),(2,2.0::DOUBLE,1.0::DOUBLE,8.0)"
    else:
        values = "(1,1.0::DOUBLE,1.0::DOUBLE,9.0),(2,2.0::DOUBLE,NULL::DOUBLE,8.0)"
    sql = f"""
        SELECT x FROM (
            FROM (VALUES {values}) t(id,lo,hi,score)
            DECIDE x(BOOL) SUCH THAT {clauses} MAXIMIZE SUM(score*x)
        ) q
    """
    for mode in ("require", "off"):
        error = _raw(decidb_cli, sql, mode=mode).stderr
        if expected == "equality":
            assert ("varies" if mode == "require" else "more than one value") in error
        else:
            assert expected in error


@pytest.mark.correctness
@pytest.mark.parametrize(
    "clauses,expected",
    [
        ("SUM(x)=eqcap PER dept AND SUM(x)<=hicap PER dept", "equality"),
        ("SUM(x)<=hicap PER dept AND SUM(x)=eqcap PER dept", "NULL"),
    ],
)
def test_s1_multiple_source_bounds_group_error_order(decidb_cli, clauses, expected):
    sql = f"""
        SELECT x FROM (
            FROM (VALUES (1,'A',1.0::DOUBLE,NULL::DOUBLE,9.0),
                         (2,'A',1.0::DOUBLE,1.0::DOUBLE,8.0),
                         (3,'B',1.0::DOUBLE,1.0::DOUBLE,7.0),
                         (4,'B',2.0::DOUBLE,1.0::DOUBLE,6.0)) t(id,dept,eqcap,hicap,score)
            DECIDE x(BOOL) SUCH THAT {clauses} MAXIMIZE SUM(score*x)
        ) q
    """
    for mode in ("require", "off"):
        error = _raw(decidb_cli, sql, mode=mode).stderr
        if expected == "equality":
            assert ("varies" if mode == "require" else "more than one value") in error
        else:
            assert expected in error


@pytest.mark.correctness
def test_s1_multiple_source_bounds_validate_bypassed_and_late_rows(decidb_cli):
    bypassed = """
        SELECT x FROM (
            FROM (VALUES (1,1.0::DOUBLE,NULL::DOUBLE,FALSE,NULL::INTEGER,9.0),
                         (2,1.0::DOUBLE,1.0::DOUBLE,TRUE,1,8.0)) t(id,lo,hi,active,dept,score)
            DECIDE x(BOOL) SUCH THAT SUM(x)>=lo WHEN active PER dept
                AND SUM(x)<=hi WHEN active PER dept MAXIMIZE SUM(score*x)
        ) q
    """
    empty = """
        SELECT x FROM (
            FROM (VALUES (1,NULL::DOUBLE,NULL::DOUBLE,FALSE,9.0)) t(id,lo,hi,active,score)
            DECIDE x(BOOL) SUCH THAT SUM(x)>=lo WHEN active
                AND SUM(x)<=hi WHEN active MAXIMIZE SUM(score*x)
        ) q
    """
    late = """
        SELECT x FROM (
            FROM (SELECT i AS id, 1.0::DOUBLE AS lo,
                         CASE WHEN i=4999 THEN NULL ELSE 1.0 END::DOUBLE AS hi,
                         9.0::DOUBLE AS score FROM range(5000) t(i)) s
            DECIDE x(BOOL) SUCH THAT SUM(x)>=lo AND SUM(x)<=hi MAXIMIZE SUM(score*x)
        ) q LIMIT 1
    """
    for mode in ("require", "off"):
        assert "NULL" in _raw(decidb_cli, bypassed, mode=mode).stderr
        assert "empty row set for aggregate" in _raw(decidb_cli, empty, mode=mode).stderr
        assert "NULL" in _raw(decidb_cli, late, mode=mode).stderr


@pytest.mark.correctness
@pytest.mark.parametrize(
    "declaration,constraint,objective,reason",
    [
        ("x(BOOL), y(BOOL)", "SUM(x)<=1", "MAXIMIZE SUM(p*x)", "variable_shape"),
        ("x(BOOL)", "SUM(x)<=1 AND x<=1", "MAXIMIZE SUM(p*x)", "constraint_shape"),
        ("x(BOOL)", "SUM(x)<=1 AND x<=cap", "MAXIMIZE SUM(p*x)", "constraint_shape"),
        ("x(BOOL)", "SUM(x)<=1 PER id AND SUM(x)>=1 PER cap", "MAXIMIZE SUM(p*x)", "constraint_scope"),
        ("x(BOOL)", "SUM(x)<>1", "MAXIMIZE SUM(p*x)", "constraint_shape"),
        ("x(BOOL)", "SUM(x+1)<=3", "MAXIMIZE SUM(p*x)", "constraint_shape"),
        ("x(BOOL)", "SUM(id*x)<=2", "MAXIMIZE SUM(p*x)", "constraint_shape"),
        ("x(BOOL)", "SUM(x)<=cap+1", "MAXIMIZE SUM(p*x)", "constraint_shape"),
        ("x(BOOL)", "SUM(x)<=1.5+cap", "MAXIMIZE SUM(p*x)", "constraint_shape"),
        ("t.x(BOOL)", "SUM(x)<=1", "MAXIMIZE SUM(p*x)", "variable_shape"),
        ("t.x(BOOL)", "SUM(t: x)<=1", "MAXIMIZE SUM(p*x)", "variable_shape"),
        ("x(BOOL)", "SUM(x)<=random()", "MAXIMIZE SUM(p*x)", "constraint_shape"),
        ("x(BOOL)", "SUM(x)<=1", "MAXIMIZE SUM((p+random())*x)", "coefficient_shape"),
        ("x(BOOL)", "SUM(x)<=1", "MAXIMIZE SUM(p*x)+SUM(CAST(cap::VARCHAR AS DOUBLE)*x)", "coefficient_shape"),
        ("x(BOOL)", "SUM(x)<=1", "MAXIMIZE SUM(MAX(p*x)) PER id", "objective_scope"),
        ("x(BOOL)", "SUM(x)<=1", "MAXIMIZE SUM(p*x) WHEN id=1", "objective_scope"),
        ("x(BOOL)", "SUM(x)<=1", "", "problem_shape"),
        ("x(BOOL)", "SUM(x)<=9007199254740993", "MAXIMIZE SUM(p*x)", "constraint_shape"),
        ("x(BOOL)", "norm(x,'inf')<=1", "MAXIMIZE SUM(p*x)", "constraint_shape"),
        ("x(BOOL)", "norm(x,1)<=1", "MAXIMIZE SUM(p*x)", "constraint_shape"),
        ("x(BOOL)", "SUM(x)<=1", "MAXIMIZE SUM(p*x) - norm(p*x,1)", "objective_shape"),
        ("x(BOOL)", "SUM(x)<=1", "MINIMIZE norm(p*x,2)", "objective_shape"),
    ],
)
def test_baseline_one_condition_away_misses(decidb_cli, declaration, constraint, objective, reason):
    decide = f"""
        FROM (VALUES (1, 9.0::DOUBLE, 1), (2, 10.0::DOUBLE, 1)) t(id,p,cap)
        DECIDE {declaration} SUCH THAT {constraint} {objective}
    """
    sql = f"SELECT id FROM ({decide}) q"
    required = _raw(decidb_cli, sql)
    assert "decide_direct_solve=require:" in required.stderr, required.stderr
    assert reason in required.stderr, required.stderr
    plan = _raw(decidb_cli, f"EXPLAIN {sql}", mode="auto")
    assert _has_decide_operator(plan.stdout) and "Direct solve" not in plan.stdout, plan.stderr


@pytest.mark.correctness
@pytest.mark.parametrize(
    "score_sql,message",
    [
        ("CASE WHEN i=4999 THEN NULL ELSE 1.0 END", 'column "score" is NULL'),
        ("CASE WHEN i=4999 THEN 'NaN'::DOUBLE ELSE 1.0 END", "invalid value (NaN or Infinity)"),
        ("CASE WHEN i=4999 THEN 'Infinity'::DOUBLE ELSE 1.0 END", "invalid value (NaN or Infinity)"),
    ],
)
def test_late_invalid_score_is_read_at_zero_capacity(decidb_cli, score_sql, message):
    decide = f"""
        FROM (SELECT i, {score_sql} AS score FROM range(5000) t(i)) s
        DECIDE x(BOOL) SUCH THAT SUM(x) <= 0 MAXIMIZE SUM(score*x)
    """
    for outer in (
        f"SELECT i FROM ({decide}) q LIMIT 1",
        f"SELECT COUNT(*) FROM ({decide}) q",
        f"SELECT i FROM ({decide}) q WHERE i=0",
    ):
        assert message in _raw(decidb_cli, outer).stderr
    rows, _ = _run(decidb_cli, f"SELECT i FROM ({decide}) q LIMIT 0")
    assert rows == []
    assert message in _raw(decidb_cli, f"SELECT i FROM ({decide}) q ORDER BY i LIMIT 1").stderr


@pytest.mark.correctness
def test_throwing_cast_and_filter_scope(decidb_cli):
    source = """
        SELECT i, CASE WHEN i=4999 THEN 'bad' ELSE '1' END AS raw
        FROM range(5000) t(i)
    """
    decide = f"""
        FROM ({source}) s DECIDE x(BOOL) SUCH THAT SUM(x) <= 0
        MAXIMIZE SUM(CAST(raw AS DOUBLE)*x)
    """
    for outer in (
        f"SELECT i FROM ({decide}) q LIMIT 1",
        f"SELECT COUNT(*) FROM ({decide}) q",
        f"SELECT i FROM ({decide}) q WHERE i=0",
    ):
        error = _raw(decidb_cli, outer).stderr
        assert "Could not convert string 'bad' to DOUBLE" in error

    null_source = """
        SELECT i, CASE WHEN i=4999 THEN NULL ELSE 1.0 END AS score
        FROM range(5000) t(i)
    """
    null_decide = f"FROM ({null_source}) s DECIDE x(BOOL) SUCH THAT SUM(x)<=0 MAXIMIZE SUM(score*x)"
    assert 'column "score" is NULL' in _raw(decidb_cli, f"SELECT i FROM ({null_decide}) q WHERE i=0").stderr
    filtered = f"""
        FROM ({null_source}) s WHERE i<4999
        DECIDE x(BOOL) SUCH THAT SUM(x)<=0 MAXIMIZE SUM(score*x)
    """
    rows, _ = _run(decidb_cli, f"SELECT COUNT(*) AS n FROM ({filtered}) q")
    assert rows == [(4999,)]


@pytest.mark.correctness
def test_parent_context_and_duplicate_rows(decidb_cli):
    decide = """
        FROM (VALUES (1, 2), (2, 9), (3, 5), (4, 9)) t(id,score)
        DECIDE x(BOOL) SUCH THAT SUM(x)<=1 MAXIMIZE SUM(score*x)
    """
    rows, _ = _run(decidb_cli, f"SELECT id,x FROM ({decide}) q WHERE id=1")
    assert rows == [(1, 0)]
    rows, _ = _run(decidb_cli, f"SELECT COUNT(*) AS n,SUM(x) AS chosen FROM ({decide}) q")
    assert rows == [(4, 1)]
    rows, _ = _run(decidb_cli, f"""
        SELECT q.id,q.x,z.tag FROM ({decide}) q
        JOIN (VALUES (1,'a'),(2,'b')) z(id,tag) USING(id) ORDER BY q.id
    """)
    assert rows[0] == (1, 0, "a")
    assert len(rows) == 2 and rows[1][0] == 2 and rows[1][2] == "b"
    assert rows[1][1] in (0, 1)
    empty = """
        SELECT id,x FROM (
            FROM (SELECT 1 AS id, 2 AS score WHERE FALSE) t
            DECIDE x(BOOL) SUCH THAT SUM(x)<=1 MAXIMIZE SUM(score*x)
        ) q
    """
    rows, _ = _run(decidb_cli, empty)
    assert rows == []

    duplicate = """
        SELECT score,x FROM (
            FROM (VALUES (9.0),(9.0)) t(score)
            DECIDE x(BOOL) SUCH THAT SUM(x)<=1 MAXIMIZE SUM(score*x)
        ) q ORDER BY x
    """
    rows, _ = _run(decidb_cli, duplicate)
    assert rows == [(9.0, 0), (9.0, 1)]

    unrelated_null = """
        SELECT id,payload,x FROM (
            FROM (VALUES (1,NULL,2.0),(2,'ok',9.0)) t(id,payload,score)
            DECIDE x(BOOL) SUCH THAT SUM(x)<=1 MAXIMIZE SUM(score*x)
        ) q ORDER BY id
    """
    rows, _ = _run(decidb_cli, unrelated_null)
    assert rows == [(1, None, 0), (2, "ok", 1)]

    cte = """
        WITH src AS MATERIALIZED (
            SELECT i AS id, i::DOUBLE AS score, repeat('a',100) AS payload
            FROM range(4) t(i)
        )
        SELECT id,x FROM (
            FROM src DECIDE x(BOOL) SUCH THAT SUM(x)<=1 MAXIMIZE SUM(score*x)
        ) q WHERE id=2
    """
    rows, _ = _run(decidb_cli, cte)
    assert rows == [(2, 0)]


@pytest.mark.correctness
def test_materialized_result_filter_keeps_global_input_and_late_guard(decidb_cli):
    sql = """
        WITH solved AS MATERIALIZED (
            FROM (
                SELECT i, i::DOUBLE AS score, repeat('p', 200) AS payload
                FROM range(5000) t(i)
            ) s DECIDE x(BOOL) SUCH THAT SUM(x)<=1 MAXIMIZE SUM(score*x)
        )
        SELECT i, x FROM solved WHERE i=0
    """
    rows, _ = _run(decidb_cli, sql)
    assert rows == [(0, 0)]

    invalid = sql.replace("i::DOUBLE AS score", "CASE WHEN i=4999 THEN NULL ELSE i::DOUBLE END AS score")
    for outer in (invalid, invalid + " LIMIT 1"):
        assert 'column "score" is NULL' in _raw(decidb_cli, outer).stderr


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
    assert 'column "score" is NULL' in _raw(decidb_cli, invalid + count_only).stderr

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
def test_parent_join_filter_does_not_shrink_decide_input(decidb_cli):
    decide = """
        FROM (VALUES (1,100.0),(2,9.0),(3,5.0)) s(id,score)
        DECIDE x(BOOL) SUCH THAT SUM(x)<=1 MAXIMIZE SUM(score*x)
    """
    sql = f"""
        SELECT d.id,d.x FROM ({decide}) d
        JOIN (VALUES (2)) keep(id) USING(id)
    """
    rows, _ = _run(decidb_cli, sql)
    assert rows == [(2, 0)]

    invalid = decide.replace("(3,5.0)", "(3,NULL::DOUBLE)")
    assert 'column "score" is NULL' in _raw(decidb_cli, sql.replace(decide, invalid)).stderr


@pytest.mark.correctness
@pytest.mark.parametrize(
    "scores,capacity,sense",
    [
        ((9.0, 10.0, -1.0), 1, "MAXIMIZE"),
        ((9.0, 10.0, -1.0), 5, "MAXIMIZE"),
        ((-9.0, -10.0, 1.0), 1, "MINIMIZE"),
        ((-9.0, -10.0, 1.0), 0, "MINIMIZE"),
    ],
)
def test_direct_and_forced_backends_agree_on_separated_optima(
    decidb_cli, decidb_cli_highs, decidb_cli_gurobi, scores, capacity, sense
):
    sql = _source_query(scores, capacity, sense)
    direct, columns = _run(decidb_cli, sql)
    assert columns == ["id", "score", "x"]
    direct_value = sum(row[1] * row[2] for row in direct)
    assert direct_value == _optimum(scores, capacity, sense)
    for backend in (decidb_cli_highs, decidb_cli_gurobi):
        solver, solver_columns = _run(backend, sql, mode="off")
        assert solver_columns == columns
        assert len(solver) == len(direct)
        assert [row[0] for row in solver] == [row[0] for row in direct]
        assert all(row[2] in (0, 1) for row in solver)
        assert sum(row[2] for row in solver) <= capacity
        solver_value = sum(row[1] * row[2] for row in solver)
        assert math.isclose(solver_value, direct_value, rel_tol=0, abs_tol=1e-8)


@pytest.mark.correctness
@pytest.mark.parametrize("magnitude", (5e-324, 1e-12, 1e-9, 1e-8, 1e-7, 1e-6))
def test_tiny_score_uses_exact_oracle_and_measured_backend_gap(
    decidb_cli, decidb_cli_highs, decidb_cli_gurobi, magnitude
):
    sql = _source_query((magnitude, 0.0), 1)
    direct, _ = _run(decidb_cli, sql)
    direct_value = sum(row[1] * row[2] for row in direct)
    assert direct_value == magnitude
    assert [row[2] for row in direct] == [1, 0]

    # The baseline and this permanent fixture found a largest backend gap of
    # 1e-7: HiGHS may return zero at that score while both modes succeed.
    for backend in (decidb_cli_highs, decidb_cli_gurobi):
        solver, _ = _run(backend, sql, mode="off")
        assert len(solver) == 2
        assert all(row[2] in (0, 1) for row in solver)
        assert sum(row[2] for row in solver) <= 1
        solver_value = sum(row[1] * row[2] for row in solver)
        assert 0 <= magnitude - solver_value <= 1e-7


@pytest.mark.correctness
def test_serializer_explain_and_prepared_plan(decidb_cli, tmp_path):
    sql = _source_query((2.0, 9.0, -1.0), 1)
    rows, _ = _run(decidb_cli.with_verify_serializer(), sql)
    assert [row[2] for row in rows] == [0, 1, 0]
    logical = _raw(decidb_cli, f"PRAGMA explain_output='optimized_only'; EXPLAIN {sql}").stdout
    physical = _raw(decidb_cli, f"EXPLAIN {sql}").stdout
    profile = _raw(decidb_cli, f"EXPLAIN ANALYZE {sql}").stdout
    for plan in (logical, physical, profile):
        assert "Direct solve rule" in plan and "Direct solve proof" in plan

    dump_path = tmp_path / "direct_model.txt"
    _run(decidb_cli.with_env({"DECIDB_DUMP_MODEL": str(dump_path)}), sql)
    assert not dump_path.exists()

    prepared = _raw(decidb_cli, f"""
        PREPARE direct_s1 AS {sql};
        SET decide_direct_solve='off';
        EXPLAIN EXECUTE direct_s1;
    """).stdout
    assert "Direct solve rule" in prepared and "require" in prepared

    rebound = decidb_cli.execute_raw("""
        CREATE TEMP TABLE direct_life(id INTEGER, score DOUBLE);
        INSERT INTO direct_life VALUES (1,2),(2,9);
        SET decide_direct_solve='require';
        PREPARE direct_life_p AS SELECT id,x FROM (
            FROM direct_life DECIDE x(BOOL)
            SUCH THAT SUM(x)<=1 MAXIMIZE SUM(score*x)
        ) q;
        SET decide_direct_solve='off';
        ALTER TABLE direct_life ADD COLUMN extra INTEGER;
        EXPLAIN EXECUTE direct_life_p;
    """).stdout
    assert _has_decide_operator(rebound) and "Direct solve" not in rebound


@pytest.mark.correctness
def test_forced_solver_bypasses_auto(decidb_cli_highs):
    sql = _source_query((2.0, 9.0, -1.0), 1)
    plan = _raw(decidb_cli_highs, f"EXPLAIN {sql}", mode="auto").stdout
    assert _has_decide_operator(plan) and "Direct solve" not in plan
    rows, _ = _run(decidb_cli_highs, sql, mode="auto")
    assert sum(row[2] for row in rows) <= 1
    assert sum(row[1] * row[2] for row in rows) == 9.0
    assert "conflicts with DECIDB_FORCE_SOLVER" in _raw(decidb_cli_highs, sql).stderr


@pytest.mark.correctness
def test_diagnose_and_invalid_forced_solver_policy(decidb_cli):
    sql = _source_query((2.0, 9.0), 1)
    assert "conflicts with DIAGNOSE" in _raw(decidb_cli, f"DIAGNOSE {sql}").stderr
    diagnose_plan = _raw(decidb_cli, f"EXPLAIN DIAGNOSE {sql}", mode="auto").stdout
    assert "DECIDE_DIAGNOSE" in diagnose_plan and "Direct solve" not in diagnose_plan

    invalid = decidb_cli.with_env({"DECIDB_FORCE_SOLVER": "unknown_backend"})
    error = _raw(invalid, sql, mode="auto").stderr
    assert "DECIDB_FORCE_SOLVER=unknown_backend" in error

    disabled = _raw(decidb_cli, f"SET disabled_optimizers='decide_optimizer'; {sql}").stderr
    assert "DECIDE optimizer did not run" in disabled


@pytest.mark.correctness
def test_nested_direct_decisions(decidb_cli):
    sql = """
        SELECT id,x,y FROM (
            FROM (SELECT id,score,x FROM (
                FROM (VALUES (1,2),(2,9)) t(id,score)
                DECIDE x(BOOL) SUCH THAT SUM(x)<=1 MAXIMIZE SUM(score*x)
            ) a) b
            DECIDE y(BOOL) SUCH THAT SUM(y)<=1 MAXIMIZE SUM((score+x)*y)
        ) c ORDER BY id
    """
    rows, _ = _run(decidb_cli, sql)
    assert rows == [(1, 0, 0), (2, 1, 1)]


@pytest.mark.correctness
def test_correlated_source_and_coefficient(decidb_cli):
    source_subquery = """
        SELECT id,x FROM (
            FROM (
                SELECT t.id,
                    (SELECT s.score FROM (VALUES (1,2.0),(2,9.0)) s(id,score)
                     WHERE s.id=t.id) AS score
                FROM (VALUES (1),(2)) t(id)
            ) u
            DECIDE x(BOOL) SUCH THAT SUM(x)<=1 MAXIMIZE SUM(score*x)
        ) q ORDER BY id
    """
    rows, _ = _run(decidb_cli, source_subquery)
    assert rows == [(1, 0), (2, 1)]

    coefficient_subquery = """
        SELECT id,x FROM (
            FROM (VALUES (1,2.0),(2,9.0)) t(id,score)
            DECIDE x(BOOL) SUCH THAT SUM(x)<=1
            MAXIMIZE SUM(x*(SELECT s.v FROM (VALUES (1,2.0),(2,9.0)) s(k,v)
                            WHERE s.k=t.id))
        ) q ORDER BY id
    """
    rows, _ = _run(decidb_cli, coefficient_subquery)
    assert rows == [(1, 0), (2, 1)]


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
