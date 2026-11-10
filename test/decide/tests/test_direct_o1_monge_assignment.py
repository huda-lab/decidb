"""O1 (Monge assignment), checked three ways: the independent oracle, the solver path, the direct path.

Every case is one DECIDE query over a cross product of two temporary tables whose position columns are declared NOT NULL
(the rule reads no other), `per l` and `per r` each selecting one pair. The oracle builds its own model from the raw
rows: one binary variable for each pair row, one equality for each distinct left row and each distinct right row (a
class is a whole row, NULL compared as a value). DeciDB runs the query with
`decide_direct_solve='off'` (the solver) and `'require'` (direct solve; a query the rule does not apply to fails). The
case passes when all three agree: both runs return an assignment that picks one pair for every class on each side and
reaches the oracle's objective (the pairs may differ on ties), or all three say infeasible.

Rows with identical values are one class, so `duplicate_*` cases have more rows than classes, and a pair row is one of
several copies of a pair. The checker therefore reads classes from the result rows, not from row positions.

What a result comparison cannot see (which plan ran, EXPLAIN, prepared plans, DIAGNOSE, the all-rows read) stays in
`test_direct_rule_conformance.py`.

Note: `oracle_solver` caches each result under the test node id plus a hash of the test function's source. The case
table and the model builder live outside that source, so after editing a case or `_oracle_model`, remove its entries
from `results/oracle_cache.json`.
"""

import math
from collections import Counter

import pytest

from decidb_cli import DeciDBCliError
from solver.types import ObjSense, VarType

from ._direct_oracle_common import _allow_direct_path  # noqa: F401  (autouse fixture)
from ._direct_oracle_common import agree_with_oracle, outcome, run, sql_literal

NAN = float("nan")

PER_SIDE = "per l: SUM(x) by (l) = 1 AND per r: SUM(x) by (r) = 1"
ABS = "ABS(l.lpos - r.rpos)"
SQUARED = "POWER(l.lpos - r.rpos, 2)"
PRODUCT = "l.lpos * r.rpos"

# Three rows out of order on each side; every objective below has one optimum on them.
LEFT = [(1, 10.0), (2, 1.0), (3, 4.0)]
RIGHT = [(7, 6.0), (8, 11.0), (9, 2.0)]


def _case(objective, cost, sense="MINIMIZE", left=LEFT, right=RIGHT, **extra):
    """`cost(a, b)` is the written coefficient of a pair at left position `a` and right position `b`."""
    return dict(objective=objective, cost=cost, sense=sense, left=left, right=right, **extra)


def _abs(a, b):
    return abs(a - b)


def _squared(a, b):
    return (a - b) ** 2


def _product(a, b):
    return a * b


ORACLE_CASES = {
    # Each objective shape and direction, on the same rows.
    "abs_min": _case(f"MINIMIZE SUM({ABS} * x)", _abs),
    "abs_min_scaled": _case(f"MINIMIZE 2.5 * SUM({ABS} * x)", lambda a, b: 2.5 * abs(a - b)),
    "squared_min": _case(f"MINIMIZE SUM({SQUARED} * x)", _squared),
    "squared_min_scaled": _case(f"MINIMIZE SUM(3 * {SQUARED} * x)", lambda a, b: 3 * (a - b) ** 2),
    "squared_zero_factor": _case(f"MINIMIZE 0 * SUM({SQUARED} * x)", lambda a, b: 0.0),
    "product_max": _case(f"MAXIMIZE SUM({PRODUCT} * x)", _product, "MAXIMIZE"),
    "product_min": _case(f"MINIMIZE SUM({PRODUCT} * x)", _product),
    # A negative factor turns the direction round: maximizing -2ab is minimizing ab, and the other way.
    "product_max_negative_factor": _case(f"MAXIMIZE SUM(-2 * {PRODUCT} * x)", lambda a, b: -2 * a * b, "MAXIMIZE"),
    "product_min_negative_factor": _case(f"MINIMIZE SUM(-2 * {PRODUCT} * x)", lambda a, b: -2 * a * b),
    # The positions the other way round.
    "abs_min_swapped": _case("MINIMIZE SUM(ABS(r.rpos - l.lpos) * x)", _abs),
    "squared_min_swapped": _case("MINIMIZE SUM(POWER(r.rpos - l.lpos, 2) * x)", _squared),
    "product_max_swapped": _case("MAXIMIZE SUM(r.rpos * l.lpos * x)", _product, "MAXIMIZE"),
    # Identical rows are one class: two copies on the left, two on the right, and both.
    "duplicate_left": _case(
        f"MINIMIZE SUM({SQUARED} * x)", _squared, left=[(1, 5.0), (1, 5.0), (2, 9.0)], right=[(7, 6.0), (8, 1.0)]
    ),
    "duplicate_right": _case(
        f"MINIMIZE SUM({SQUARED} * x)", _squared, left=[(1, 5.0), (2, 9.0)], right=[(7, 6.0), (7, 6.0), (8, 1.0)]
    ),
    "duplicate_both": _case(
        f"MINIMIZE SUM({SQUARED} * x)",
        _squared,
        left=[(1, 5.0), (1, 5.0), (2, 9.0)],
        right=[(7, 6.0), (8, 1.0), (8, 1.0), (8, 1.0)],
    ),
    # A NULL in a class column is a value of the class, so two NULL-keyed rows are one class.
    "null_key_class": _case(
        f"MINIMIZE SUM({SQUARED} * x)",
        _squared,
        left=[(None, 5.0), (None, 5.0), (1, 2.0)],
        right=[(7, 3.0), (8, 6.0)],
    ),
    # Different classes at the same position: the ranks of tied rows are arbitrary, the cost is not.
    "tied_positions": _case(
        f"MINIMIZE SUM({ABS} * x)", _abs, left=[(1, 3.0), (2, 3.0), (3, 8.0)], right=[(7, 3.0), (8, 5.0), (9, 5.0)]
    ),
    # Ranked by value, not by size: -5 comes before -1, and the product is least when the ranks run opposite.
    "negative_positions": _case(
        f"MINIMIZE SUM({PRODUCT} * x)",
        _product,
        left=[(1, -5.0), (2, -1.0), (3, 2.0)],
        right=[(7, -4.0), (8, -3.0), (9, 6.0)],
    ),
    "integer_positions": _case(
        f"MINIMIZE SUM({ABS} * x)", _abs, left=[(1, 9), (2, 3), (3, 5)], right=[(7, 4), (8, 8), (9, 1)], integer=True
    ),
    # A WHERE on one side leaves that side's remaining rows complete against the other.
    "where_left": _case(
        f"MINIMIZE SUM({SQUARED} * x)",
        _squared,
        left=[(1, 10.0), (2, -3.0), (3, 1.0), (4, 4.0)],
        where="l.lpos > 0",
        keep_left=lambda row: row[1] > 0,
    ),
    "where_right": _case(
        f"MINIMIZE SUM({SQUARED} * x)",
        _squared,
        right=[(7, 6.0), (8, 11.0), (9, 2.0), (10, -8.0)],
        where="r.rpos > 0",
        keep_right=lambda row: row[1] > 0,
    ),
    # No complete assignment: three left classes against two right classes, and three rows against three whose
    # classes are two against three.
    "unequal_sizes": _case(f"MINIMIZE SUM({SQUARED} * x)", _squared, right=RIGHT[:2]),
    "unequal_classes_equal_rows": _case(
        f"MINIMIZE SUM({SQUARED} * x)", _squared, left=[(1, 5.0), (1, 5.0), (2, 9.0)], right=RIGHT
    ),
}


def _tables(case):
    """The statements that create the two sides as temporary tables. A position is declared NOT NULL, as the rule
    requires, unless the case names its side in `nullable`."""
    position = "INTEGER" if case.get("integer") else "DOUBLE"
    statements = []
    for side, alias in (("left", "l"), ("right", "r")):
        null_rule = "" if case.get("nullable") == side else " NOT NULL"
        table = f"o1_{side}"
        statements.append(f"CREATE OR REPLACE TEMP TABLE {table}({alias}id INTEGER, {alias}pos {position}{null_rule});")
        if case[side]:
            types = ("INTEGER", position)
            values = ", ".join(
                "(" + ", ".join(sql_literal(v, t) for v, t in zip(row, types)) + ")" for row in case[side]
            )
            statements.append(f"INSERT INTO {table} VALUES {values};")
    return " ".join(statements)


def _sql(case, wrap="plain"):
    where = f"WHERE {case['where']}" if "where" in case else ""
    source = f"FROM o1_left l CROSS JOIN o1_right r {where}"
    decide = f"DECIDE x(BOOL) SUCH THAT {PER_SIDE} {case['objective']}"
    select = "SELECT lid, lpos, rid, rpos, x"
    setup = _tables(case)
    if wrap == "plain":
        return f"{setup} {select} FROM ({source} {decide}) q"
    if wrap in ("limit_one", "count"):
        reads = "*" if wrap == "limit_one" else "COUNT(*)"
        return f"{setup} SELECT {reads} FROM ({source} {decide}) q {'LIMIT 1' if wrap == 'limit_one' else ''}"
    if wrap == "parent_join":
        return f"{setup} {select} FROM ({source} {decide}) q JOIN (VALUES (1), (2), (3), (4)) k(lid) USING (lid)"
    if wrap == "recombined_filters":
        # The parent filters the result two complementary ways and unions them. A filter on the DECIDE input would
        # solve a smaller problem on each side and break the assignment, and a filter on a column the DECIDE passes
        # through is pushed there unless the DECIDE is a materialized CTE.
        return (
            f"{setup} WITH solved AS MATERIALIZED ({source} {decide}) "
            f"{select} FROM solved WHERE lid < 2 UNION ALL {select} FROM solved WHERE lid >= 2"
        )
    raise ValueError(wrap)


def _kept(case):
    left = [row for row in case["left"] if case.get("keep_left", lambda row: True)(row)]
    right = [row for row in case["right"] if case.get("keep_right", lambda row: True)(row)]
    return left, right


def _coefficient(case, a, b):
    """The written coefficient of a pair. A NULL position makes it NULL, and a NULL term is left out of the sum."""
    return 0.0 if a is None or b is None else float(case["cost"](a, b))


def _oracle_model(oracle_solver, case):
    """One binary variable for each pair row, one equality for each distinct row of each side."""
    left, right = _kept(case)
    oracle_solver.create_model("direct_o1_monge_assignment")
    pairs = {(i, j): f"x_{i}_{j}" for i in range(len(left)) for j in range(len(right))}
    for name in pairs.values():
        oracle_solver.add_variable(name, VarType.BINARY)
    for distinct in set(left):
        oracle_solver.add_constraint({n: 1.0 for (i, _), n in pairs.items() if left[i] == distinct}, "=", 1.0)
    for distinct in set(right):
        oracle_solver.add_constraint({n: 1.0 for (_, j), n in pairs.items() if right[j] == distinct}, "=", 1.0)
    sense = ObjSense.MAXIMIZE if case["sense"] == "MAXIMIZE" else ObjSense.MINIMIZE
    oracle_solver.set_objective(
        {name: _coefficient(case, left[i][1], right[j][1]) for (i, j), name in pairs.items()}, sense
    )
    return oracle_solver.solve()


def _check_assignment(case, oracle, rows, columns, mode):
    """The rows are the whole pair set, each class has one selected pair, and the cost is the oracle's."""
    left, right = _kept(case)
    assert columns == ["lid", "lpos", "rid", "rpos", "x"], mode
    assert Counter(tuple(row[:4]) for row in rows) == Counter(l + r for l in left for r in right), mode
    assert all(type(row[4]) is int and row[4] in (0, 1) for row in rows), mode
    for classes, part in ((left, slice(0, 2)), (right, slice(2, 4))):
        for distinct in set(classes):
            selected = sum(row[4] for row in rows if tuple(row[part]) == distinct)
            assert selected == 1, (mode, distinct, selected)
    cost = sum(_coefficient(case, row[1], row[3]) * row[4] for row in rows)
    assert cost == pytest.approx(oracle.objective_value, abs=1e-6), (mode, cost, oracle.objective_value)


def _agree(decidb_cli, oracle_solver, name, wrap="plain"):
    case = ORACLE_CASES[name]
    oracle = _oracle_model(oracle_solver, case)
    agree_with_oracle(
        decidb_cli,
        _sql(case, wrap=wrap),
        oracle,
        lambda rows, columns, mode: _check_assignment(case, oracle, rows, columns, mode),
    )


@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(ORACLE_CASES))
def test_o1_monge_oracle_solver_and_direct_agree(decidb_cli, oracle_solver, name):
    _agree(decidb_cli, oracle_solver, name)


@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.sql_subquery
@pytest.mark.correctness
@pytest.mark.parametrize("wrap", ["parent_join", "recombined_filters"])
def test_o1_monge_agrees_inside_a_parent_context(decidb_cli, oracle_solver, wrap):
    _agree(decidb_cli, oracle_solver, "squared_min", wrap)


@pytest.mark.var_boolean
@pytest.mark.correctness
@pytest.mark.parametrize("empty", ["left", "right"])
def test_o1_monge_an_empty_side_gives_an_empty_result_not_an_infeasible_error(decidb_cli, empty):
    # The other side has rows, so the class counts differ; the cross product has no pair rows and the solver answers it.
    case = _case(f"MINIMIZE SUM({SQUARED} * x)", _squared, **{empty: []})
    sql = _sql(case)
    for mode in ("off", "require"):
        assert run(decidb_cli, sql, mode)[0] == [], mode


@pytest.mark.var_boolean
@pytest.mark.correctness
def test_o1_monge_a_filter_across_both_sides_keeps_the_solver(decidb_cli):
    # `l.lpos < r.rpos` makes a join, and the join misses the pair (2, 7): the rule has no complete cross product. The
    # solver still answers it, and `auto` runs the solver.
    case = _case(
        f"MINIMIZE SUM({SQUARED} * x)",
        _squared,
        left=[(1, 1.0), (2, 4.0)],
        right=[(7, 2.0), (8, 6.0)],
        where="l.lpos < r.rpos",
    )
    sql = _sql(case)
    refused = outcome(decidb_cli, sql, "require")
    assert refused == ("error", "direct_not_applied"), refused
    solver = outcome(decidb_cli, sql, "off")
    assert solver[0] == "ok" and len(solver[1]) == 3, solver
    assert outcome(decidb_cli, sql, "auto") == solver


# A position not declared NOT NULL keeps the solver, whether or not a NULL is stored: the solver leaves a NULL term out
# of the sum, which is not what ranking the positions answers. `null_position` is the case where it matters, a
# NULL pair costing nothing. `require` declines; `off` is checked against the oracle, and `auto` answers as `off` does.
NULLABLE_CASES = {
    "nullable_left_column": _case(f"MINIMIZE SUM({SQUARED} * x)", _squared, nullable="left"),
    "nullable_right_column": _case(f"MINIMIZE SUM({SQUARED} * x)", _squared, nullable="right"),
    "null_position": _case(
        f"MINIMIZE SUM({SQUARED} * x)", _squared, left=[(1, 10.0), (2, None), (3, 4.0)], nullable="left"
    ),
}


@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(NULLABLE_CASES))
def test_o1_monge_a_position_not_declared_not_null_keeps_the_solver(decidb_cli, oracle_solver, name):
    case = NULLABLE_CASES[name]
    sql = _sql(case)
    oracle = _oracle_model(oracle_solver, case)
    agree_with_oracle(
        decidb_cli,
        sql,
        oracle,
        lambda rows, columns, mode: _check_assignment(case, oracle, rows, columns, mode),
        modes=("off",),
    )
    assert outcome(decidb_cli, sql, "require") == ("error", "direct_not_applied")
    assert outcome(decidb_cli, sql, "auto") == outcome(decidb_cli, sql, "off")


# NaN is an error on both paths and the same error. (A NULL position is not, and has no row in this table: see
# `NULLABLE_CASES`.)
ERROR_CASES = {
    "nan_left_abs": _case(f"MINIMIZE SUM({ABS} * x)", _abs, left=[(1, NAN), (2, 1.0), (3, 4.0)]),
    "nan_right_squared": _case(f"MINIMIZE SUM({SQUARED} * x)", _squared, right=[(7, 6.0), (8, NAN), (9, 2.0)]),
    "nan_left_product": _case(
        f"MAXIMIZE SUM({PRODUCT} * x)", _product, "MAXIMIZE", left=[(1, 10.0), (2, NAN), (3, 4.0)]
    ),
}


@pytest.mark.var_boolean
@pytest.mark.error
@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(ERROR_CASES))
def test_o1_monge_error_cases_fail_on_both_paths(decidb_cli, name):
    sql = _sql(ERROR_CASES[name])
    assert outcome(decidb_cli, sql, "off") == ("error", "invalid_value")
    assert outcome(decidb_cli, sql, "require") == ("error", "invalid_value")


@pytest.mark.var_boolean
@pytest.mark.error
@pytest.mark.correctness
@pytest.mark.parametrize("wrap", ["limit_one", "count"])
def test_o1_monge_a_bad_position_on_the_last_row_raises_whatever_the_parent_reads(decidb_cli, wrap):
    # 71 x 71 pairs, and the last left row inserted is the only NaN. A parent that reads one row or a count still
    # raises, on `off` and on `require`: the rule reads every pair row before it releases one.
    left = [(i, float(i)) for i in range(70)] + [(70, NAN)]
    right = [(i, float(i)) for i in range(71)]
    sql = _sql(_case(f"MINIMIZE SUM({SQUARED} * x)", _squared, left=left, right=right), wrap)
    for mode in ("off", "require"):
        with pytest.raises(DeciDBCliError, match="is NaN or infinite on some rows"):
            run(decidb_cli, sql, mode)


# A side the plan cannot scan twice is copied once and read from the copy. A filter that calls `random()` is volatile, so
# the side is no repeatable scan, and `random() * 0` keeps every row, so the answer is the one without it. The count is
# the number of copies the plan holds.
KEEP_ALL_LEFT = "l.lpos + random() * 0 > -1000000"
KEEP_ALL_RIGHT = "r.rpos + random() * 0 > -1000000"
COPIED_CASES = {
    "copied_left_side": (_case(f"MINIMIZE SUM({SQUARED} * x)", _squared, where=KEEP_ALL_LEFT), 1),
    "copied_right_side": (_case(f"MINIMIZE SUM({SQUARED} * x)", _squared, where=KEEP_ALL_RIGHT), 1),
    "copied_both_sides": (
        _case(f"MINIMIZE SUM({SQUARED} * x)", _squared, where=f"{KEEP_ALL_LEFT} AND {KEEP_ALL_RIGHT}"),
        2,
    ),
    # The copy holds the duplicate rows too, and a class is still one of them paired.
    "copied_both_sides_with_duplicates": (
        _case(
            f"MINIMIZE SUM({SQUARED} * x)",
            _squared,
            left=[(1, 5.0), (1, 5.0), (2, 9.0)],
            right=[(7, 6.0), (8, 1.0), (8, 1.0), (8, 1.0)],
            where=f"{KEEP_ALL_LEFT} AND {KEEP_ALL_RIGHT}",
        ),
        2,
    ),
}


def _plan_text(decidb_cli, sql):
    """The `require` plan of a query built by `_sql`, as one string."""
    explain = sql.replace("SELECT lid", "EXPLAIN SELECT lid", 1)
    return decidb_cli.execute_raw(f"SET decide_direct_solve='require'; {explain}").stdout


@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(COPIED_CASES))
def test_o1_monge_a_side_that_is_not_a_repeatable_scan_is_copied_and_still_applies(decidb_cli, oracle_solver, name):
    case, copies = COPIED_CASES[name]
    sql = _sql(case)
    oracle = _oracle_model(oracle_solver, case)
    agree_with_oracle(
        decidb_cli, sql, oracle, lambda rows, columns, mode: _check_assignment(case, oracle, rows, columns, mode)
    )
    assert _plan_text(decidb_cli, sql).count("direct_source") == copies


# A NULL is a value of a class on both sides: two rows with a NULL key are one class, and a NULL-keyed row and a keyed
# row at the same position are two.
NULL_KEY_BOTH_SIDES = _case(
    f"MINIMIZE SUM({SQUARED} * x)",
    _squared,
    left=[(None, 5.0), (None, 5.0), (1, 5.0), (2, 9.0)],
    right=[(None, 6.0), (None, 6.0), (8, 6.0), (9, 1.0)],
)


@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_o1_monge_duplicate_rows_with_a_null_key_on_both_sides_are_one_class_each(decidb_cli, oracle_solver):
    case = NULL_KEY_BOTH_SIDES
    oracle = _oracle_model(oracle_solver, case)
    agree_with_oracle(
        decidb_cli,
        _sql(case),
        oracle,
        lambda rows, columns, mode: _check_assignment(case, oracle, rows, columns, mode),
    )


# The key column that is not the position is a DOUBLE, where SQL equality has -0.0 equal to 0.0 and a NaN equal to a
# NaN: the rows of a pair of these are one class, as the solver's grouping has them.
KEYED_CASES = {
    "signed_zero_key_left": _case(
        f"MINIMIZE SUM({SQUARED} * x)",
        _squared,
        left=[(-0.0, 5.0), (0.0, 5.0), (1.0, 9.0)],
        right=[(2.0, 6.0), (3.0, 1.0)],
    ),
    "signed_zero_key_right": _case(
        f"MINIMIZE SUM({SQUARED} * x)",
        _squared,
        left=[(2.0, 6.0), (3.0, 1.0)],
        right=[(-0.0, 5.0), (0.0, 5.0), (1.0, 9.0)],
    ),
    "nan_key_left": _case(
        f"MINIMIZE SUM({SQUARED} * x)",
        _squared,
        left=[(NAN, 5.0), (NAN, 5.0), (1.0, 9.0)],
        right=[(2.0, 6.0), (3.0, 1.0)],
    ),
    "nan_key_both": _case(
        f"MINIMIZE SUM({SQUARED} * x)",
        _squared,
        left=[(NAN, 5.0), (NAN, 5.0), (1.0, 9.0)],
        right=[(NAN, 6.0), (3.0, 1.0), (NAN, 6.0)],
    ),
}


def _double(value):
    """A DOUBLE constant; a negative zero is written as text, since `-0.0` is a decimal negated."""
    if value == 0.0 and math.copysign(1.0, value) < 0:
        return "CAST('-0.0' AS DOUBLE)"
    return sql_literal(value, "DOUBLE")


def _keyed_sql(case):
    statements = []
    for side, alias in (("left", "l"), ("right", "r")):
        values = ", ".join("(" + ", ".join(_double(v) for v in row) + ")" for row in case[side])
        statements.append(
            f"CREATE OR REPLACE TEMP TABLE o1_{side}({alias}key DOUBLE, {alias}pos DOUBLE NOT NULL); "
            f"INSERT INTO o1_{side} VALUES {values};"
        )
    decide = f"DECIDE x(BOOL) SUCH THAT {PER_SIDE} {case['objective']}"
    return f"{' '.join(statements)} SELECT lkey, lpos, rkey, rpos, x FROM (FROM o1_left l CROSS JOIN o1_right r {decide}) q"


def _same_value(value):
    """The value as SQL equality sees it: -0.0 is 0.0, and every NaN is one value. A NULL stays None."""
    return None if value is None else "nan" if math.isnan(value) else value + 0.0


def _check_keyed_assignment(case, oracle, rows, columns, mode):
    """`_check_assignment` for a DOUBLE key, whose classes are compared as SQL compares them."""
    left, right = case["left"], case["right"]
    assert columns == ["lkey", "lpos", "rkey", "rpos", "x"], mode
    same = lambda row: tuple(_same_value(v) for v in row)
    assert Counter(same(row[:4]) for row in rows) == Counter(same(l + r) for l in left for r in right), mode
    assert all(type(row[4]) is int and row[4] in (0, 1) for row in rows), mode
    for classes, part in ((left, slice(0, 2)), (right, slice(2, 4))):
        for distinct in {same(row) for row in classes}:
            selected = sum(row[4] for row in rows if same(row[part]) == distinct)
            assert selected == 1, (mode, distinct, selected)
    cost = sum(_coefficient(case, row[1], row[3]) * row[4] for row in rows)
    assert cost == pytest.approx(oracle.objective_value, abs=1e-6), (mode, cost, oracle.objective_value)


@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(KEYED_CASES))
def test_o1_monge_signed_zeros_and_nans_in_a_key_column_are_one_class_as_on_the_solver_path(
    decidb_cli, oracle_solver, name
):
    case = KEYED_CASES[name]
    oracle = _oracle_model(oracle_solver, case)
    agree_with_oracle(
        decidb_cli,
        _keyed_sql(case),
        oracle,
        lambda rows, columns, mode: _check_keyed_assignment(case, oracle, rows, columns, mode),
    )


@pytest.mark.var_boolean
@pytest.mark.error
@pytest.mark.correctness
def test_o1_monge_a_decimal_product_that_overflows_raises_on_both_paths_though_no_check_is_made(decidb_cli):
    # DECIMAL(38, 0) positions are finite however large, so the coefficient has no check of its own to raise. DuckDB's
    # own multiplication overflows on the one pair, and the plan must still compute the coefficient on every pair.
    big = "100000000000000000000000000000"
    sql = (
        "CREATE OR REPLACE TEMP TABLE o1_left(lid INTEGER, lpos DECIMAL(38, 0) NOT NULL); "
        "CREATE OR REPLACE TEMP TABLE o1_right(rid INTEGER, rpos DECIMAL(38, 0) NOT NULL); "
        f"INSERT INTO o1_left VALUES (1, {big}), (2, 1); INSERT INTO o1_right VALUES (7, {big}), (8, 2); "
        f"SELECT lid, rid, x FROM (FROM o1_left l CROSS JOIN o1_right r DECIDE x(BOOL) SUCH THAT {PER_SIDE} "
        f"MAXIMIZE SUM({PRODUCT} * x)) q"
    )
    for mode in ("off", "require"):
        with pytest.raises(DeciDBCliError, match="Overflow in multiplication of DECIMAL"):
            run(decidb_cli, sql, mode)
