"""R4 (weighted max-min fair allocation), checked three ways: the independent oracle, the solver path, the direct path.

Every case is one DECIDE query with one `REAL` decision `amount` on each row, in the box [0, cap], under one budget
`SUM(w * amount) <= B` with a constant weight `w > 0`, maximizing the smallest ratio `amount / d` over the rows the
objective reads. The oracle builds its own model from the raw rows, from the capacities, coefficients and weight each
case states in Python (never from DeciDB's reading of the SQL): one variable per row, a free level `t` and one row
`t <= c * amount` for each row the objective reads, maximizing `t`. DeciDB then runs the query with
`decide_direct_solve='off'` (the solver) and `'require'` (direct solve; a query the rule does not apply to fails
instead of quietly using the solver). The case passes when all three agree:

- the oracle finds the problem feasible: both DeciDB runs return an assignment inside the box and the budget whose
  smallest `c * amount` over the rows read is the oracle's level (the amounts are not unique, so no row is compared);
- the oracle finds it infeasible: both DeciDB runs raise that error.

What a result comparison cannot see (which plan ran, EXPLAIN, prepared plans, DIAGNOSE, pruning) stays in
`test_direct_rule_conformance.py`.

Note: `oracle_solver` caches each result under the test node id plus a hash of the test function's source. The case
tables live outside that source, so editing a case does not invalidate the cache; a stale value fails loudly with an
objective mismatch. After changing a case or the model builder, remove its entries from `results/oracle_cache.json`.
"""

from dataclasses import dataclass

import pytest

from solver.types import ObjSense, VarType

from ._direct_oracle_common import _allow_direct_path  # noqa: F401  (autouse fixture)
from ._direct_oracle_common import agree_with_oracle, outcome, run, sql_literal, values_clause


BIG = 1e30  # the oracle reads a bound this large as none

_FEASIBILITY_TOLERANCE = 1e-6
_OBJECTIVE_TOLERANCE = 5e-6

# ---------------------------------------------------------------------------
# Data
# ---------------------------------------------------------------------------

COLUMNS = (
    ("id", "INTEGER"),
    ("d", "DOUBLE"),  # a demand
    ("cap", "DOUBLE"),  # a capacity
    ("f", "BOOLEAN"),  # `when` membership
    ("r", "DOUBLE"),  # a coefficient read as it is: the ratio of a row's share to its amount
)
NAMES = [name for name, _ in COLUMNS]


def _row(id, d=2.0, cap=4.0, f=True, r=1.0):
    return dict(id=id, d=d, cap=cap, f=f, r=r)


# The worked example of the class: demands 2 and 4, capacities 4 and 8.
WORKED = (_row(1, 2.0, 4.0), _row(2, 4.0, 8.0))
# Three levels at once: the budget binds when it is small, the capacity of the second row binds when it is large.
THREE = (_row(1, 2.0, 10.0), _row(2, 4.0, 6.0), _row(3, 1.0, 5.0))
# A zero capacity fixes the level at 0, and so does a demand that is negative (its coefficient is).
ZERO_CAPACITY = (_row(1, 2.0, 0.0), _row(2, 4.0, 8.0))
NEGATIVE_DEMAND = (_row(1, -2.0, 4.0), _row(2, 4.0, 8.0))
# A NULL demand, and a NULL capacity, are no read and no bound.
NULL_DEMAND = (_row(1, 2.0, 4.0), _row(2, None, 5.0), _row(3, 4.0, 8.0))
NULL_CAPACITY = (_row(1, 2.0, None), _row(2, 4.0, 8.0))
NULL_CAPACITY_ALL = (_row(1, 2.0, None), _row(2, 4.0, None))
NEGATIVE_CAPACITY = (_row(1, 2.0, -1.0), _row(2, 4.0, 8.0))
# The MIN reads no row.
NO_READ_NULL = (_row(1, None, 4.0), _row(2, None, 8.0))
NO_READ_WHEN = (_row(1, 2.0, 4.0, False), _row(2, 4.0, 8.0, False))
# A `when` drops the rows: the one that would have fixed the level at 0 (a zero capacity, a bad demand).
WHEN_DROPS_ZERO_CAPACITY = (_row(1, 2.0, 4.0, True), _row(2, 4.0, 0.0, False), _row(3, 4.0, 8.0, True))
WHEN_DROPS_ZERO_DEMAND = (_row(1, 2.0, 4.0, True), _row(2, 0.0, 5.0, False), _row(3, 4.0, 8.0, True))
# A capacity under a `when`: the row it drops has none, so the other binds the level.
WHEN_CAPACITY = (_row(1, 2.0, 1.0, True), _row(2, 4.0, 0.5, False))
# A coefficient read as it is: a mix of signs and a zero.
RATES = (_row(1, r=0.5, cap=6.0), _row(2, r=2.0, cap=3.0), _row(3, r=4.0, cap=2.0))
RATES_NEGATIVE = (_row(1, r=0.5), _row(2, r=-2.0), _row(3, r=4.0))
RATES_ZERO = (_row(1, r=0.5), _row(2, r=0.0), _row(3, r=4.0))


def _values(rows):
    return values_clause(rows, NAMES, dict(COLUMNS), sql_literal)


# ---------------------------------------------------------------------------
# Budget, box and objective: each the SQL and what the oracle reads of it
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class Budget:
    sql: str
    weight: float
    limit: float


@dataclass(frozen=True)
class Box:
    #: The SUCH THAT clauses on the decision, without the budget.
    sql: str
    #: row -> the upper end of the decision on that row; None for no end.
    upper: object


@dataclass(frozen=True)
class Objective:
    sql: str
    #: row -> the coefficient `c` of the MIN's term, or None for a row the MIN does not read.
    coefficient: object


def _ratio(row):
    return None if row["d"] is None else 1.0 / row["d"]


def _ratio_when(row):
    return _ratio(row) if row["f"] else None


def _rate(row):
    return row["r"]


def _cap(row):
    return row["cap"]


BUDGETS = {
    "b9": Budget("SUM(amount) <= 9", 1.0, 9.0),
    "b100": Budget("SUM(amount) <= 100", 1.0, 100.0),
    "b0": Budget("SUM(amount) <= 0", 1.0, 0.0),
    "negative": Budget("SUM(amount) <= -1", 1.0, -1.0),
    # The spellings of one budget: each read the same way.
    "weight_2": Budget("SUM(2 * amount) <= 18", 2.0, 18.0),
    "weight_half": Budget("SUM(amount / 2) <= 4.5", 0.5, 4.5),
    "like_terms": Budget("SUM(amount + 2 * amount) <= 18", 3.0, 18.0),
    "literal_on_left": Budget("9 >= SUM(amount)", 1.0, 9.0),
}

BOXES = {
    "column": Box("amount BETWEEN 0 AND cap", _cap),
    "column_bound": Box("amount <= cap", _cap),
    "floor_written": Box("amount >= 0 AND amount <= cap", _cap),
    "constant": Box("amount <= 3", lambda row: 3.0),
    "constants": Box("amount BETWEEN 0 AND 3", lambda row: 3.0),
    "none": Box("amount >= 0", lambda row: None),
    "scaled": Box("2 * amount <= cap", lambda row: None if row["cap"] is None else row["cap"] / 2.0),
    "two_bounds": Box("amount <= cap AND amount <= 3", lambda row: 3.0 if row["cap"] is None else min(row["cap"], 3.0)),
    "when": Box("when f: amount <= cap", lambda row: row["cap"] if row["f"] else None),
}

OBJECTIVES = {
    "ratio": Objective("MAXIMIZE MIN(amount / d)", _ratio),
    "ratio_when": Objective("MAXIMIZE MIN(when f: amount / d)", _ratio_when),
    "objective_when": Objective("MAXIMIZE when f: MIN(amount / d)", _ratio_when),
    "rate": Objective("MAXIMIZE MIN(r * amount)", _rate),
    # A constant outside the MIN moves no answer.
    "offset": Objective("MAXIMIZE MIN(amount / d) + 2", _ratio),
}


@dataclass(frozen=True)
class Case:
    rows: tuple
    budget: str = "b9"
    box: str = "column"
    objective: str = "ratio"


R4_CASES = {}


def add(name, rows, **kwargs):
    assert name not in R4_CASES, name
    R4_CASES[name] = Case(rows, **kwargs)


# The level is the least of what the budget and each capacity allow.
add("worked_example_the_budget_binds", WORKED)
add("capacity_binds_before_the_budget", WORKED, budget="b100")
add("three_rows_budget_binds", THREE)
add("three_rows_a_capacity_binds", THREE, budget="b100")
add("zero_budget_gives_level_zero", WORKED, budget="b0")
add("zero_capacity_gives_level_zero", ZERO_CAPACITY)
add("negative_demand_gives_level_zero", NEGATIVE_DEMAND)
add("negative_budget_is_infeasible", WORKED, budget="negative")
add("negative_capacity_is_infeasible", NEGATIVE_CAPACITY)
# The spellings of the budget.
add("budget_weight_two", WORKED, budget="weight_2")
add("budget_weight_a_half", WORKED, budget="weight_half")
add("budget_like_terms_add", WORKED, budget="like_terms")
add("budget_literal_on_the_left", WORKED, budget="literal_on_left")
# The box: where the capacity comes from.
add("capacity_in_a_bound", WORKED, box="column_bound")
add("capacity_with_the_floor_written", THREE, box="floor_written", budget="b100")
add("capacity_a_constant", THREE, box="constant", budget="b100")
add("capacity_between_constants", THREE, box="constants", budget="b100")
add("no_capacity", THREE, box="none")
add("capacity_scaled_by_the_coefficient_of_the_decision", THREE, box="scaled", budget="b100")
add("capacity_two_bounds_the_least_applies", THREE, box="two_bounds", budget="b100")
add("capacity_under_a_when_binds_on_its_rows", WHEN_CAPACITY, box="when", budget="b100")
add("capacity_null_is_no_bound", NULL_CAPACITY)
add("capacity_null_on_every_row", NULL_CAPACITY_ALL)
# Rows the MIN does not read.
add("null_demand_row_is_not_read", NULL_DEMAND)
add("min_that_reads_no_row_null_demands", NO_READ_NULL)
add("min_that_reads_no_row_when_drops_all", NO_READ_WHEN, objective="ratio_when")
add("min_when_drops_a_row_with_a_zero_capacity", WHEN_DROPS_ZERO_CAPACITY, objective="ratio_when")
add("min_when_drops_a_row_with_a_zero_demand", WHEN_DROPS_ZERO_DEMAND, objective="ratio_when")
add("objective_when_drops_a_row", WHEN_DROPS_ZERO_CAPACITY, objective="objective_when")
# A coefficient read as it is, and a constant outside the MIN.
add("coefficient_from_a_column", RATES, objective="rate")
add("coefficient_negative_gives_level_zero", RATES_NEGATIVE, objective="rate")
add("coefficient_zero_gives_level_zero", RATES_ZERO, objective="rate")
add("offset_outside_the_min", WORKED, objective="offset")


def _sql_body(case_):
    box = BOXES[case_.box]
    budget = BUDGETS[case_.budget]
    objective = OBJECTIVES[case_.objective]
    return f"DECIDE amount(REAL) SUCH THAT {box.sql} AND {budget.sql} {objective.sql}"


def _source(case_):
    return f"(VALUES {_values(case_.rows)}) t({', '.join(NAMES)})"


def _sql(case_, wrap="plain"):
    body = _sql_body(case_)
    if wrap == "plain":
        return f"SELECT id, amount FROM (FROM {_source(case_)} {body}) q ORDER BY id"
    if wrap == "joined_source":
        ids = ", ".join(f"({row['id']})" for row in case_.rows)
        return (
            f"SELECT id, amount FROM (FROM {_source(case_)} JOIN (VALUES {ids}) j(jid) ON t.id = j.jid {body}) q "
            "ORDER BY id"
        )
    if wrap == "cte":
        return f"WITH c AS (SELECT * FROM {_source(case_)}) SELECT id, amount FROM (FROM c {body}) q ORDER BY id"
    assert wrap == "aggregate_above", wrap
    return f"SELECT id, amount FROM (SELECT id, amount FROM (FROM {_source(case_)} {body}) q) p ORDER BY id"


def _solve_with_oracle(oracle_solver, case_):
    oracle_solver.create_model("direct_r4_max_min_fairness")
    box, budget, objective = BOXES[case_.box], BUDGETS[case_.budget], OBJECTIVES[case_.objective]
    names = {}
    for row in case_.rows:
        names[row["id"]] = f"x_{row['id']}"
        upper = box.upper(row)
        oracle_solver.add_variable(names[row["id"]], VarType.CONTINUOUS, 0.0, BIG if upper is None else upper)
    oracle_solver.add_constraint(
        {names[row["id"]]: budget.weight for row in case_.rows}, "<=", budget.limit, name="budget"
    )
    read = [(row, objective.coefficient(row)) for row in case_.rows]
    read = [(row, c) for row, c in read if c is not None]
    if read:
        oracle_solver.add_variable("level", VarType.CONTINUOUS, -BIG, BIG)
        for row, c in read:
            oracle_solver.add_constraint({"level": 1.0, names[row["id"]]: -c}, "<=", 0.0, name=f"level_{row['id']}")
        oracle_solver.set_objective({"level": 1.0}, ObjSense.MAXIMIZE)
    else:
        # A MIN over no row leaves no objective, and any feasible assignment is optimal.
        oracle_solver.set_objective({}, ObjSense.MAXIMIZE)
    return oracle_solver.solve()


def _check_assignment(case_, rows, columns, oracle, mode):
    assert columns == ["id", "amount"], mode
    assert [row[0] for row in rows] == [row["id"] for row in case_.rows], mode
    values = {row[0]: row[1] for row in rows}
    box, budget, objective = BOXES[case_.box], BUDGETS[case_.budget], OBJECTIVES[case_.objective]
    for source in case_.rows:
        value = values[source["id"]]
        assert isinstance(value, float), (mode, source["id"], value)
        upper = box.upper(source)
        assert value >= -_FEASIBILITY_TOLERANCE, (mode, source["id"], value)
        assert upper is None or value <= upper + _FEASIBILITY_TOLERANCE * max(1.0, abs(upper)), (
            mode,
            source["id"],
            value,
            upper,
        )
    spent = budget.weight * sum(values[source["id"]] for source in case_.rows)
    assert spent <= budget.limit + _FEASIBILITY_TOLERANCE * max(1.0, abs(budget.limit)), (mode, spent, budget.limit)
    shares = [objective.coefficient(source) for source in case_.rows]
    level = [c * values[source["id"]] for source, c in zip(case_.rows, shares) if c is not None]
    if not level:
        return
    expected = oracle.objective_value
    assert abs(min(level) - expected) <= _OBJECTIVE_TOLERANCE * max(1.0, abs(expected)), (mode, min(level), expected)


def _agree(decidb_cli, oracle_solver, case_, wrap="plain"):
    """Runs the case on the oracle and on both DeciDB paths, and checks they agree."""
    sql = _sql(case_, wrap)
    oracle = _solve_with_oracle(oracle_solver, case_)

    def check_assignment(rows, columns, mode):
        _check_assignment(case_, rows, columns, oracle, mode)

    agree_with_oracle(decidb_cli, sql, oracle, check_assignment)


@pytest.mark.var_real
@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(R4_CASES))
def test_r4_oracle_solver_and_direct_agree(decidb_cli, oracle_solver, name):
    _agree(decidb_cli, oracle_solver, R4_CASES[name])


# The same problem inside a parent query: the rule reads through a join and a CTE in its source, and its columns pass
# through a projection above it.
CONTEXT_CASES = [
    ("worked_example_the_budget_binds", "joined_source"),
    ("three_rows_a_capacity_binds", "cte"),
    ("null_demand_row_is_not_read", "aggregate_above"),
]


@pytest.mark.correctness
@pytest.mark.parametrize("base,wrap", CONTEXT_CASES)
def test_r4_agrees_inside_a_parent_query(decidb_cli, oracle_solver, base, wrap):
    _agree(decidb_cli, oracle_solver, R4_CASES[base], wrap=wrap)


@pytest.mark.correctness
def test_r4_the_worked_example_gives_three_and_six(decidb_cli):
    sql = _sql(R4_CASES["worked_example_the_budget_binds"])
    for mode in ("off", "require"):
        rows, _ = run(decidb_cli, sql, mode)
        assert [row[1] for row in rows] == pytest.approx([3.0, 6.0]), mode


@pytest.mark.correctness
@pytest.mark.parametrize("name", ["min_that_reads_no_row_null_demands", "min_that_reads_no_row_when_drops_all"])
def test_r4_a_min_that_reads_no_row_gives_zero_to_every_row(decidb_cli, name):
    # The solver has no objective then and returns 0; so does the plan, whose rows take the end of their range that
    # uses the least budget.
    sql = _sql(R4_CASES[name])
    for mode in ("off", "require"):
        rows, _ = run(decidb_cli, sql, mode)
        assert [row[1] for row in rows] == [0.0, 0.0], mode


@pytest.mark.correctness
@pytest.mark.parametrize("budget", ["-1", "25"])
def test_r4_empty_source_returns_no_rows_whatever_the_budget_says(decidb_cli, budget):
    # The checks run on rows, and there are none: a negative budget has no row to make infeasible.
    sql = (
        "SELECT id, amount FROM (SELECT 1 AS id, 2.0::DOUBLE AS d, -1.0::DOUBLE AS cap WHERE false) "
        f"DECIDE amount(REAL) SUCH THAT amount <= cap AND SUM(amount) <= {budget} MAXIMIZE MIN(amount / d)"
    )
    for mode in ("off", "require"):
        assert run(decidb_cli, sql, mode)[0] == []


# ---------------------------------------------------------------------------
# Errors: both paths fail, in the same class, with the same text.
# ---------------------------------------------------------------------------

_RATIO = "amount <= cap AND SUM(amount) <= 9 MAXIMIZE MIN(amount / d)"
_RATE = "amount <= cap AND SUM(amount) <= 9 MAXIMIZE MIN(r * amount)"

# (the class of the error, the text it has, the bad row's d, cap and r, the clauses). Row 0 is ordinary.
ERROR_CASES = {
    "demand_zero": ("invalid_value", "1 / d is NaN or infinite on some rows", "0.0", "4.0", "1.0", _RATIO),
    "demand_nan": ("invalid_value", "1 / d is NaN or infinite on some rows", "'NaN'", "4.0", "1.0", _RATIO),
    "rate_nan": ("invalid_value", "r is NaN or infinite on some rows", "2.0", "4.0", "'NaN'", _RATE),
    "rate_infinite": ("invalid_value", "r is NaN or infinite on some rows", "2.0", "4.0", "'Infinity'", _RATE),
    "capacity_nan": ("null_bound", "the bound cap is NaN on some rows", "2.0", "'NaN'", "1.0", _RATIO),
    "capacity_minus_infinite": ("infeasible", "infeasible", "2.0", "'-Infinity'", "1.0", _RATIO),
}


def _error_sql(demand, capacity, rate, decide):
    def typed(value):
        return f"CAST({value} AS DOUBLE)"

    source = (
        f"SELECT i AS id, CASE WHEN i = 0 THEN 2.0 ELSE {typed(demand)} END AS d, "
        f"CASE WHEN i = 0 THEN 4.0 ELSE {typed(capacity)} END AS cap, "
        f"CASE WHEN i = 0 THEN 1.0 ELSE {typed(rate)} END AS r FROM range(2) t(i)"
    )
    return f"SELECT id, amount FROM ({source}) DECIDE amount(REAL) SUCH THAT {decide}"


@pytest.mark.error
@pytest.mark.parametrize("name", sorted(ERROR_CASES))
def test_r4_both_paths_raise_the_same_error(decidb_cli, name):
    error_class, text, *arguments = ERROR_CASES[name]
    sql = _error_sql(*arguments)
    direct = outcome(decidb_cli, sql, "require")
    solver = outcome(decidb_cli, sql, "off")
    assert direct[0] == "error", direct
    assert direct == solver == ("error", error_class), (direct, solver)
    for mode in ("require", "off"):
        assert text in decidb_cli.execute_raw(f"SET decide_direct_solve='{mode}'; {sql}").stderr, mode
