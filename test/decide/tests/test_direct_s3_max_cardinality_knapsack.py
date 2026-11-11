"""S3 (maximum-cardinality knapsack), checked three ways: the independent oracle, the solver path, the direct path.

Every case is one DECIDE query over rows that share a budget: a `BOOL` decision `x` on each row, one budget
`SUM(w*x) <= B` (a constant, a column or a reducer of the data on the right, optionally `by (g)`), and the objective
`MAXIMIZE SUM(x)` under a constant factor. The oracle builds a 0-1 model of its own from the raw rows and the budget
each case states in Python (never from DeciDB's reading of the SQL): binary `x`, one constraint per group, and the
objective that counts the selected rows. DeciDB then runs the query with `decide_direct_solve='off'` (the solver) and
`'require'` (direct solve; a query the rule does not apply to fails instead of quietly using the solver). The case
passes when all three agree:

- the oracle finds the problem feasible: both DeciDB runs return an assignment whose budget holds in every group as
  written, and whose count reaches the oracle's (rows may differ on ties);
- the oracle finds it infeasible, or the data is invalid (NaN, infinity): both DeciDB runs raise that error.

Every weight and limit is a dyadic rational, so sums of them are exact in a double and the budget is checked without a
tolerance. A value the oracle cannot hold, a sum within rounding of the budget, goes in the boundary table, where direct
solve is compared with the solver path alone.

What a result comparison cannot see (which plan ran, EXPLAIN, prepared plans, DIAGNOSE, pruning) stays in
`test_direct_rule_conformance.py`.

Note: `oracle_solver` caches each result under the test node id plus a hash of the test function's source. The case
table and the model builder live outside that source, so editing a case does not invalidate the cache; a stale value
fails loudly with an objective mismatch. After changing a case or the model builder, remove its entries from
`results/oracle_cache.json`.
"""

import math
from dataclasses import dataclass

import pytest

from decidb_cli import DeciDBCliError
from solver.types import ObjSense, VarType

from ._direct_differential import error_class
from ._direct_oracle_common import _allow_direct_path  # noqa: F401  (autouse fixture)
from ._direct_oracle_common import agree_with_oracle
from ._direct_oracle_common import outcome as _outcome, run as _run, sql_literal, values_clause
from ._number_range import writes_number_out_of_range

# ---------------------------------------------------------------------------
# Data
# ---------------------------------------------------------------------------

COLUMNS = (
    ("id", "INTEGER"),
    ("g", "VARCHAR"),  # the `by` key
    ("w", "DOUBLE"),  # the weight of the row in the budget, positive, negative or zero
    ("cap", "DOUBLE"),  # a limit read from the data
)
NAMES = [name for name, _ in COLUMNS]
TYPES = dict(COLUMNS)


def _rows(weights, group="a", caps=None):
    """One row per weight, numbered from 1, in one group (or in the groups `group` lists), with the limits `caps`."""
    groups = group if isinstance(group, (list, tuple)) else [group] * len(weights)
    caps = caps if caps is not None else [0.0] * len(weights)
    return tuple(dict(id=i, g=g, w=w, cap=c) for i, (w, g, c) in enumerate(zip(weights, groups, caps), 1))


def _values(rows):
    return values_clause(rows, NAMES, TYPES, sql_literal)


# ---------------------------------------------------------------------------
# Cases
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class Case:
    rows: tuple
    #: The budget as written.
    budget: str
    #: (row, rows of the row's group) -> the limit the row's instance of the budget states, or None for a NULL, which
    #: skips its instance. Stated here in Python and not read from the SQL.
    limit: object
    #: True when the budget is `by (g)`: a row with a NULL key is in no group, so no budget bounds it.
    by: bool = False
    objective: str = "MAXIMIZE SUM(x)"
    #: The constant factor of the objective's count.
    factor: float = 1.0
    #: row -> the row's weight in the budget; a NULL weight is a NULL summand, which SUM skips.
    weight: object = lambda row: 0.0 if row["w"] is None else row["w"]


CASES = {}


def add(name, rows, budget, limit, **kwargs):
    CASES[name] = Case(tuple(rows), budget, limit, **kwargs)


def constant(number):
    return lambda row, group: number


def column(row, group):
    return row["cap"]


# The worked example of definition.md: the prefix 2, 3, 4 uses the budget up exactly.
WORKED = _rows([2.0, 3.0, 4.0, 6.0])
add("worked_example", WORKED, "SUM(w*x) <= 9", constant(9.0))
# The same weights in the opposite order: the first row, the heaviest, is the one the budget leaves out.
WORKED_REVERSED = _rows([6.0, 4.0, 3.0, 2.0])
add("worked_example_reversed", WORKED_REVERSED, "SUM(w*x) <= 9", constant(9.0))

# Signs. Zero weights cost nothing, so they come first (3 rows: 0, 0 and 1; the 3 would make 4).
add("zero_weights", _rows([3.0, 0.0, 0.0, 1.0, 5.0]), "SUM(w*x) <= 3", constant(3.0))
# A negative weight is a credit: -3, -1, 2 and 4 sum to 2, so four rows fit under 4 where the positive weights alone
# fit one.
add("negative_weights", _rows([-3.0, -1.0, 2.0, 4.0, 5.0]), "SUM(w*x) <= 4", constant(4.0))
# The empty prefix (0) and the first (-5) are over -6; the second (-9) and the third (-6) are within it. The largest
# prefix that fits is the third, so the first failures do not end the search.
add("negative_budget_met_by_negative_weights", _rows([-5.0, -4.0, 3.0, 6.0]), "SUM(w*x) <= -6", constant(-6.0))
# Four equal weights: any three of them.
add("tied_weights", _rows([2.0, 2.0, 2.0, 2.0, 5.0]), "SUM(w*x) <= 6", constant(6.0))

# The budget fits every row, and it fits none but the empty prefix.
add("budget_fits_every_row", _rows([0.5, 2.0, 3.5, 4.0]), "SUM(w*x) <= 100", constant(100.0))
add("budget_fits_only_the_empty_prefix", _rows([2.0, 3.0, 4.0]), "SUM(w*x) <= 1", constant(1.0))

# A budget read from the data. The tightest limit among the rows' instances rules (7 here, not 9 or 12), a NULL limit
# skips its instance (the smallest of the others is 7), and a reducer of all the rows is one value for every row.
add("data_budget_column", _rows([2.0, 3.0, 4.0, 6.0], caps=[9.0, 7.0, 12.0, 8.0]), "SUM(w*x) <= cap", column)
add(
    "data_budget_null_limit_skips_its_instance",
    _rows([2.0, 3.0, 4.0, 6.0], caps=[9.0, None, 7.0, 8.0]),
    "SUM(w*x) <= cap",
    column,
)
add(
    "data_budget_reducer_of_all_rows",
    _rows([2.0, 3.0, 4.0, 6.0], caps=[4.0, 6.0, 2.0, 6.0]),
    "SUM(w*x) <= 0.5 * SUM(cap)",
    lambda row, group: 0.5 * sum(member["cap"] for member in group),
)

# Groups. Every group is read on its own: a holds the worked example (9: three rows), b fits two of three (3), c fits
# none (4 under 5 and 5), d is met by a negative weight (-1 under -2 and 1), e fits every row.
GROUPED = _rows(
    [2.0, 3.0, 4.0, 6.0, 1.0, 1.0, 2.0, 5.0, 5.0, -2.0, 1.0, 1.0, 2.0],
    group=["a"] * 4 + ["b"] * 3 + ["c"] * 2 + ["d"] * 2 + ["e"] * 2,
    caps=[9.0] * 4 + [3.0] * 3 + [4.0] * 2 + [-1.0] * 2 + [100.0] * 2,
)
add("by_groups_with_different_budgets", GROUPED, "SUM(w*x) by (g) <= cap", column, by=True)
add("by_groups_with_one_constant_budget", GROUPED, "SUM(w*x) by (g) <= 4", constant(4.0), by=True)
# A reducer over the group: half the group's limits (a: 9, b: 1.5, c: 4, d: -1, e: 50).
add(
    "by_groups_with_a_reducer_budget",
    GROUPED,
    "SUM(w*x) by (g) <= 0.5 * SUM(cap) by (g)",
    lambda row, group: 0.5 * sum(member["cap"] for member in group),
    by=True,
)
# A row whose key is NULL is in no group, so no budget bounds it: both of them are selected whatever they weigh.
NULL_KEY = _rows([2.0, 3.0, 4.0, 100.0, 200.0], group=["a", "a", "a", None, None])
add("null_key_rows_are_outside_every_budget", NULL_KEY, "SUM(w*x) by (g) <= 5", constant(5.0), by=True)

# The objective's constant factor, outside the sum and inside it.
add(
    "constant_factor_outside_the_sum",
    WORKED,
    "SUM(w*x) <= 9",
    constant(9.0),
    objective="MAXIMIZE 2 * SUM(x)",
    factor=2.0,
)
add("constant_factor_inside_the_sum", WORKED, "SUM(w*x) <= 9", constant(9.0), objective="MAXIMIZE SUM(3*x)", factor=3.0)

# A weight written as a constant or as an expression over the row.
add("constant_weight", _rows([9.0] * 6), "SUM(2*x) <= 9", constant(9.0), weight=lambda row: 2.0)
add(
    "expression_weight",
    _rows([2.0, 3.0, 4.0, 6.0]),
    "SUM((w - 1) * x) <= 4",
    constant(4.0),
    weight=lambda row: row["w"] - 1.0,
)

# Duplicate rows (the same id and the same weight) stay separate rows: 2 and one of the two 3s fit.
DUPLICATES = (
    dict(id=1, g="a", w=2.0, cap=0.0),
    dict(id=2, g="a", w=3.0, cap=0.0),
    dict(id=2, g="a", w=3.0, cap=0.0),
    dict(id=3, g="a", w=4.0, cap=0.0),
)
add("duplicate_rows", DUPLICATES, "SUM(w*x) <= 5", constant(5.0))

# A NULL weight is a NULL summand, which SUM skips: the row costs nothing and is selected.
add("null_weight", _rows([2.0, None, 4.0, 6.0]), "SUM(w*x) <= 5", constant(5.0))


# ---------------------------------------------------------------------------
# Running a case
# ---------------------------------------------------------------------------


def _decide(case):
    return f"DECIDE x(BOOL) SUCH THAT {case.budget} {case.objective}"


def _sql(case, wrap="plain"):
    """The case as a query. `plain` selects every source column and `x`, ordered by id."""
    decide = _decide(case)
    columns = ", ".join(NAMES)
    select = f"SELECT {columns}, x"
    if wrap == "plain":
        return f"{select} FROM (FROM (VALUES {_values(case.rows)}) t({columns}) {decide}) q ORDER BY id"
    if wrap == "joined_source":
        # The weights come from a second table through a join, so the source is not a scan.
        others = [name for name in NAMES if name != "w"]
        base = f"(VALUES {values_clause(case.rows, others, TYPES, sql_literal)}) t({', '.join(others)})"
        weights = f"(VALUES {values_clause(case.rows, ['id', 'w'], TYPES, sql_literal)}) z(id, w)"
        picked = ", ".join("z.w" if name == "w" else f"t.{name}" for name in NAMES)
        joined = f"SELECT {picked} FROM {base} JOIN {weights} ON z.id = t.id"
        return f"{select} FROM (FROM ({joined}) j {decide}) q ORDER BY id"
    if wrap == "counted":
        return f"SELECT COUNT(*), SUM(x), SUM(w*x) FROM (FROM (VALUES {_values(case.rows)}) t({columns}) {decide}) q"
    raise ValueError(wrap)


def _group_of(case, row):
    """The key of the row's budget, or None when the row is outside every budget."""
    return row["g"] if case.by and row["g"] is not None else None


def _groups(case):
    """key -> the rows of that group; the key None (outside every budget) is left out."""
    groups = {}
    for row in case.rows:
        key = _group_of(case, row)
        if key is not None or not case.by:
            groups.setdefault(key, []).append(row)
    return groups


def _limits(case, members):
    """The limits the budget states over a group, one per row; a NULL skips its instance."""
    limits = [case.limit(row, members) for row in members]
    return [limit for limit in limits if limit is not None]


def _solve_with_oracle(oracle_solver, case):
    oracle_solver.create_model("direct_s3_max_cardinality_knapsack")
    for index, _ in enumerate(case.rows):
        oracle_solver.add_variable(f"x_{index}", VarType.BINARY)
    positions = {id(row): index for index, row in enumerate(case.rows)}
    for members in _groups(case).values():
        weights = {f"x_{positions[id(row)]}": case.weight(row) for row in members}
        for limit in _limits(case, members):
            oracle_solver.add_constraint(weights, "<=", limit)
    oracle_solver.set_objective({f"x_{index}": case.factor for index in range(len(case.rows))}, ObjSense.MAXIMIZE)
    return oracle_solver.solve()


def _check_budgets(case, selected):
    """Asserts the budget of every group holds as written for the rows `selected` (a list of 0/1 in row order)."""
    spend = {}
    for row, chosen in zip(case.rows, selected):
        key = _group_of(case, row)
        if key is not None or not case.by:
            spend[key] = spend.get(key, 0.0) + case.weight(row) * chosen
    for key, members in _groups(case).items():
        for limit in _limits(case, members):
            assert spend[key] <= limit, (key, spend[key], limit)


def _check_assignment(case, rows, columns, oracle, mode):
    assert columns == NAMES + ["x"], mode
    assert [row[0] for row in rows] == [row["id"] for row in case.rows], mode
    assert all(type(row[4]) is int and row[4] in (0, 1) for row in rows), (mode, rows)
    selected = [row[4] for row in rows]
    _check_budgets(case, selected)
    assert math.isclose(case.factor * sum(selected), oracle.objective_value, abs_tol=1e-6), (
        mode,
        sum(selected),
        oracle.objective_value,
    )


def _agree(decidb_cli, oracle_solver, case, wrap="plain"):
    """Runs the case on the oracle and on the chosen DeciDB paths, and checks they agree."""
    sql = _sql(case, wrap)
    oracle = _solve_with_oracle(oracle_solver, case)

    def check_assignment(rows, columns, mode):
        _check_assignment(case, rows, columns, oracle, mode)

    agree_with_oracle(decidb_cli, sql, oracle, check_assignment, number_range=writes_number_out_of_range(sql))


@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(CASES))
def test_s3_oracle_solver_and_direct_agree(decidb_cli, oracle_solver, name):
    _agree(decidb_cli, oracle_solver, CASES[name])


# The same problem inside a parent query. A join in the source, which the rule reads through, is checked against the
# oracle. A parent that counts, filters or limits the result still gets the answer of the whole problem: its rows share
# one budget, so a row's answer depends on rows the parent drops.
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.sql_subquery
@pytest.mark.correctness
def test_s3_agrees_with_the_oracle_over_a_joined_source(decidb_cli, oracle_solver):
    _agree(decidb_cli, oracle_solver, CASES["worked_example_reversed"], wrap="joined_source")


@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.sql_subquery
@pytest.mark.correctness
def test_s3_a_parent_count_reads_the_whole_problem(decidb_cli, oracle_solver):
    case = CASES["worked_example_reversed"]
    sql = _sql(case, "counted")
    oracle = _solve_with_oracle(oracle_solver, case)
    for mode in ("off", "require"):
        [(rows, selected, spend)], _ = _run(decidb_cli, sql, mode)
        assert rows == len(case.rows), mode
        assert selected == oracle.objective_value, mode
        assert spend <= 9.0, mode


@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.sql_subquery
@pytest.mark.correctness
def test_s3_a_parent_limit_or_filter_still_solves_the_whole_problem(decidb_cli):
    # The first row is the heaviest and is the one the budget leaves out; a plan that solved only the rows the parent
    # keeps would select it. The unique optimum makes the rows of the two paths equal.
    inner = _sql(CASES["worked_example_reversed"]).replace(" ORDER BY id", "")
    full = _run(decidb_cli, f"{inner} ORDER BY id", "off")[0]
    assert [row[4] for row in full] == [0, 1, 1, 1]
    for outer, expected in (
        (f"SELECT id, x FROM ({inner}) q ORDER BY id LIMIT 1", [(1, 0)]),
        (f"SELECT id, x FROM ({inner}) q WHERE id >= 3 ORDER BY id", [(3, 1), (4, 1)]),
    ):
        for mode in ("off", "require"):
            assert _run(decidb_cli, outer, mode)[0] == expected, (mode, outer)


# Tied weights at scale. The rank and the running total of the tied rows must come from one sort: two sorts of rows that
# tie could order them differently, and the prefix chosen by one would not be the prefix the total of the other fits. DuckDB
# sorts in parallel only over a source that scans in parallel, which a table of more than a couple of row groups
# (122,880 rows each) does; at 200,000 rows and more the order of the ties differs between two plain sorts, at 170,000
# and fewer it does not. A third of the weights are 1.0, a third 2.0 and a third 3.0; the 1.0s use 83,334 of the budget,
# so it ends inside the run of 2.0s, which cannot all be taken.
TIE_ROWS = 250_000
TIE_BUDGET = 150_001


@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_s3_a_budget_that_ends_inside_a_run_of_ties_holds_when_the_sort_runs_in_parallel(decidb_cli):
    sql = (
        f"SET threads=4; CREATE TEMP TABLE s3_ties AS SELECT i AS id, (1.0 + (i % 3))::DOUBLE AS w FROM range({TIE_ROWS}) t(i); "
        f"SELECT SUM(x), SUM(w*x) <= {TIE_BUDGET} FROM (FROM s3_ties "
        f"DECIDE x(BOOL) SUCH THAT SUM(w*x) <= {TIE_BUDGET} MAXIMIZE SUM(x)) q"
    )
    (off_count, off_holds), (require_count, require_holds) = (
        _run(decidb_cli, sql, mode)[0][0] for mode in ("off", "require")
    )
    assert off_holds and require_holds
    assert require_count == off_count


@pytest.mark.var_boolean
@pytest.mark.correctness
@pytest.mark.parametrize("limit", ["-1", "9"])
def test_s3_empty_source_returns_no_rows_whatever_the_budget_says(decidb_cli, limit):
    # The solver answers an empty source with no rows, even under a budget no prefix meets (the empty one included).
    sql = (
        f"SELECT id, x FROM (SELECT 1 AS id, 1.0 AS w WHERE false) DECIDE x(BOOL) SUCH THAT SUM(w*x) <= {limit} "
        "MAXIMIZE SUM(x)"
    )
    for mode in ("off", "require"):
        assert _run(decidb_cli, sql, mode)[0] == [], mode


# A weight of exactly 1 or -1 makes the budget a count, which is S1's. S3 declines it, so the rule driver sees one
# applicable rule (two would raise an internal error), and `require` answers with S1.
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize(
    "budget",
    ["SUM(x) <= 2", "SUM(1*x) <= 2", "SUM(0.5*x + 0.5*x) <= 2", "SUM(-x) <= -1"],
    ids=["unit", "folded_unit", "constants_summing_to_one", "negative_unit"],
)
def test_s3_leaves_a_count_to_s1(decidb_cli, budget):
    sql = f"SELECT id, x FROM (FROM (VALUES (1), (2), (3)) t(id) DECIDE x(BOOL) SUCH THAT {budget} MAXIMIZE SUM(x)) q"
    plan = decidb_cli.execute_raw(f"SET decide_direct_solve='require'; EXPLAIN {sql}").stdout
    assert "S1_UNIFORM_MATROID" in plan and "S3_MAX_CARDINALITY_KNAPSACK" not in plan, plan


# ---------------------------------------------------------------------------
# Errors: the oracle has no model, so `off` and `require` must both fail
# ---------------------------------------------------------------------------


def _problem(rows, budget="SUM(w*x) <= 9", columns=("id", "g", "w", "cap")):
    return (
        f"SELECT id, x FROM (FROM (VALUES {rows}) t({', '.join(columns)}) "
        f"DECIDE x(BOOL) SUCH THAT {budget} MAXIMIZE SUM(x)) q"
    )


_SOURCE = "(1, 'a', 2.0::DOUBLE, 9.0::DOUBLE), (2, 'a', {w}::DOUBLE, {cap}::DOUBLE), (3, 'b', 4.0::DOUBLE, 9.0::DOUBLE)"

# name -> (query, the class of error both paths raise). The classes are `_direct_differential.ERROR_CLASSES`.
S3_ERROR_CASES = {
    "nan_weight": (_problem(_SOURCE.format(w="'NaN'", cap="9.0")), "invalid_value"),
    "infinite_weight": (_problem(_SOURCE.format(w="'Infinity'", cap="9.0")), "invalid_value"),
    "negative_infinite_weight": (_problem(_SOURCE.format(w="'-Infinity'", cap="9.0")), "invalid_value"),
    "nan_limit": (_problem(_SOURCE.format(w="3.0", cap="'NaN'"), "SUM(w*x) <= cap"), "null_bound"),
    # No prefix meets a negative budget when every weight is positive; the empty one is over it too.
    "negative_constant_budget_no_prefix_meets": (
        _problem(_SOURCE.format(w="3.0", cap="9.0"), "SUM(w*x) <= -1"),
        "infeasible",
    ),
    # One group's budget is out of reach, and that makes the whole problem infeasible.
    "negative_limit_in_one_group": (
        _problem(_SOURCE.format(w="3.0", cap="9.0"), "SUM(w*x) by (g) <= CASE WHEN g = 'b' THEN -1 ELSE 9 END"),
        "infeasible",
    ),
}


@pytest.mark.var_boolean
@pytest.mark.error
@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(S3_ERROR_CASES))
def test_s3_error_cases_fail_on_both_paths(decidb_cli, name):
    sql, expected = S3_ERROR_CASES[name]
    with pytest.raises(DeciDBCliError) as solver_error:
        _run(decidb_cli, sql, "off")
    assert error_class(str(solver_error.value)) == expected, str(solver_error.value)
    with pytest.raises(DeciDBCliError) as direct_error:
        _run(decidb_cli, sql, "require")
    assert "decide_direct_solve=require" not in str(direct_error.value), "no rule applied to the query"
    assert error_class(str(direct_error.value)) == expected, str(direct_error.value)
    if expected in ("invalid_value", "null_bound"):
        # A NaN or infinite value is worded by one function for both paths.
        assert str(direct_error.value) == str(solver_error.value)


@pytest.mark.var_boolean
@pytest.mark.error
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_s3_constant_weights_that_add_up_past_a_double_raise_the_solvers_error(decidb_cli):
    # Each weight is finite and the like terms make one that is not, which the solver refuses in its own words.
    sql = _problem(_SOURCE.format(w="3.0", cap="9.0"), "SUM(w*x + 1e308*x + 1e308*x) <= 9")
    with pytest.raises(DeciDBCliError) as solver_error:
        _run(decidb_cli, sql, "off")
    assert "computes a number too large for a DOUBLE" in str(solver_error.value)
    with pytest.raises(DeciDBCliError) as direct_error:
        _run(decidb_cli, sql, "require")
    assert "decide_direct_solve=require" not in str(direct_error.value), "no rule applied to the query"
    assert str(direct_error.value) == str(solver_error.value)


@pytest.mark.var_boolean
@pytest.mark.error
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_s3_a_running_total_past_a_double_raises_where_the_solver_answers(decidb_cli):
    # Known difference. The weights are finite and sorted, -1e308 first, and their running total reaches -infinity at the
    # second one, a number the plan computes and cannot compare with the budget; direct solve names the budget, as R1 does
    # when its plan computes a number past a DOUBLE. The solver warns that the number is out of range and solves.
    sql = (
        "SELECT id, x FROM (FROM (VALUES (1, -1e308::DOUBLE), (2, -1e308::DOUBLE), (3, 1e308::DOUBLE), "
        "(4, 1e308::DOUBLE), (5, 1e308::DOUBLE)) t(id, w) DECIDE x(BOOL) SUCH THAT SUM(w*x) <= 0 MAXIMIZE SUM(x)) q"
    )
    assert len(_run(decidb_cli, sql, "off", expect_number_range=True)[0]) == 5
    with pytest.raises(DeciDBCliError) as direct_error:
        _run(decidb_cli, sql, "require")
    assert "decide_direct_solve=require" not in str(direct_error.value), "no rule applied to the query"
    assert "SUM(w*x) <= 0 computes a number too large for a DOUBLE" in str(direct_error.value)


# ---------------------------------------------------------------------------
# Boundary: a prefix total within rounding of the budget, direct solve against the solver path alone
# ---------------------------------------------------------------------------

# Weights 0.1 and 0.2 under a budget of 0.3. In a DOUBLE the two add up to 0.30000000000000004, which is over 0.3. Direct
# solve compares in DOUBLE exactly, as S1 reads its bounds (S1's done.md, Known difference), so it selects one row. The
# solver accepts a violation of about 1e-6 and the two backends differ in what they do with it, so its answer is not
# asserted; it only has to answer.
ROUNDING_PROBLEM = (
    "SELECT id, w, x FROM (FROM (VALUES (1, CAST('0.1' AS DOUBLE)), (2, CAST('0.2' AS DOUBLE))) t(id, w) "
    "DECIDE x(BOOL) SUCH THAT SUM(w*x) <= 0.3 MAXIMIZE SUM(x)) q ORDER BY id"
)


@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_s3_direct_compares_a_prefix_total_with_the_budget_exactly_in_a_double(decidb_cli):
    both_fit = "SELECT SUM(w) <= 0.3 FROM (VALUES (CAST('0.1' AS DOUBLE)), (CAST('0.2' AS DOUBLE))) t(w)"
    assert _run(decidb_cli, both_fit, "off")[0] == [(False,)], "the premise: DuckDB finds the two weights over 0.3"
    rows, _ = _run(decidb_cli, ROUNDING_PROBLEM, "require")
    assert sum(row[2] for row in rows) == 1, rows
    # The rows the direct plan selects satisfy the budget as DuckDB evaluates it on the result.
    held = f"SELECT SUM(w*x) <= 0.3 FROM ({ROUNDING_PROBLEM.replace(' ORDER BY id', '')}) q"
    assert _run(decidb_cli, held, "require")[0] == [(True,)]
    assert _outcome(decidb_cli, ROUNDING_PROBLEM, "off")[0] == "ok"
