"""R4 (weighted max-min fair allocation), checked three ways: the independent oracle, the solver path, the direct path.

Every case is one DECIDE query with one `REAL` decision `amount` on each row, in the box [0, cap], under one budget
`SUM(w * amount) <= B` with a weight `w` of any sign (a constant or read from the data), maximizing the smallest ratio `amount / d` over the rows the
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
    ("w", "DOUBLE"),  # the weight of a row in the budget
    ("k", "VARCHAR"),  # a budget group
    ("l", "DOUBLE"),  # a budget limit read from the data
)
NAMES = [name for name, _ in COLUMNS]


def _row(id, d=2.0, cap=4.0, f=True, r=1.0, w=1.0, k="a", l=9.0):
    return dict(id=id, d=d, cap=cap, f=f, r=r, w=w, k=k, l=l)


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


# Weights of any sign. A NULL weight adds nothing to the budget, as a NULL term does to a SUM.
MIXED_WEIGHTS = (
    _row(1, 2.0, 4.0, w=1.0),
    _row(2, 4.0, 8.0, w=-1.0),
    _row(3, 1.0, 5.0, w=0.5),
    _row(4, 3.0, 6.0, w=0.0),
)
NULL_WEIGHT = (_row(1, 2.0, 4.0, w=None), _row(2, 4.0, 8.0, w=1.0))
# A negative weight with a capacity frees budget, and one with none frees as much as it is asked to.
NEGATIVE_CAPPED = (_row(1, 2.0, 4.0, w=1.0), _row(2, 4.0, 8.0, w=-1.0))
NEGATIVE_UNCAPPED = (_row(1, 2.0, 4.0, w=1.0), _row(2, 4.0, None, w=-1.0))
# The uncapped row is not read, so only the deficit makes it take anything.
NEGATIVE_UNCAPPED_NOT_READ = (_row(1, 2.0, 4.0, w=1.0), _row(2, None, None, w=-1.0))
# The uncapped row is read and its own low end is more than the deficit it must free.
NEGATIVE_UNCAPPED_READ_LOW = (_row(1, 2.0, 4.0, w=1.0), _row(2, 4.0, None, w=-3.0))
NEGATIVE_UNCAPPED_TWO = (_row(1, 2.0, 4.0, w=2.0), _row(2, None, None, w=-1.0), _row(3, None, 1e25, w=-3.0))
# No row bounds the level: a row read with weight 0 and no capacity, or only negative weights with none.
UNBOUNDED_ZERO_WEIGHT = (_row(1, 2.0, None, w=0.0), _row(2, 4.0, 8.0, w=1.0))
UNBOUNDED_ALL_NEGATIVE = (_row(1, 2.0, None, w=-1.0), _row(2, 4.0, None, w=1.0))
NEGATIVE_NOT_ENOUGH = (_row(1, 2.0, 4.0, w=1.0), _row(2, 4.0, 1.0, w=-1.0))


# Groups of the budget. Each has a level of its own, and the objective is the least of them: group `a` has 1.5 and group
# `b` 4.5, so the rows of `b` take more than the objective needs.
TWO_GROUPS = (
    _row(1, 2.0, 10.0, k="a"),
    _row(2, 4.0, 20.0, k="a"),
    _row(3, 1.0, 10.0, k="b"),
    _row(4, 1.0, 10.0, k="b"),
)
# The capacities of group `b` bind its level before its budget does.
GROUP_CAPACITY_BINDS = (
    _row(1, 2.0, 10.0, k="a"),
    _row(2, 4.0, 20.0, k="a"),
    _row(3, 1.0, 2.0, k="b"),
    _row(4, 1.0, 2.0, k="b"),
)
# Group `b` has no finite level: its rows weigh nothing and have no capacity.
GROUP_NO_FINITE_LEVEL = (
    _row(1, 2.0, 10.0, k="a"),
    _row(2, 4.0, 20.0, k="a"),
    _row(3, 1.0, None, w=0.0, k="b"),
    _row(4, 2.0, None, w=0.0, k="b"),
)
# Rows outside every budget: a NULL key, and a `when` that is false. Their capacities bound the objective too.
OUTSIDE_NULL_KEY = (_row(1, 2.0, 10.0, k="a"), _row(2, 1.0, 0.5, k=None), _row(3, 4.0, 20.0, k=None))
OUTSIDE_WHEN = (_row(1, 2.0, 10.0, f=True), _row(2, 1.0, 0.5, f=False), _row(3, 4.0, 20.0, f=False))
OUTSIDE_UNBOUNDED_ROW = (_row(1, 2.0, 10.0, k="a"), _row(2, 1.0, None, k=None))
# A `when` and a key together: the rows it drops are outside every budget, whatever their key.
WHEN_AND_KEYS = (
    _row(1, 2.0, 10.0, f=True, k="a"),
    _row(2, 4.0, 20.0, f=True, k="b"),
    _row(3, 1.0, 3.0, f=False, k="a"),
    _row(4, 1.0, 10.0, f=True, k=None),
)
# No group has a finite level.
ALL_GROUPS_UNBOUNDED = (_row(1, 2.0, None, w=0.0, k="a"), _row(2, 4.0, None, w=0.0, k="b"))
# A negative weight in a group: an uncapped row absorbs what the group's other rows need, and the other group is plain.
GROUP_NEGATIVE_UNCAPPED = (
    _row(1, 2.0, 4.0, w=1.0, k="a"),
    _row(2, None, None, w=-1.0, k="a"),
    _row(3, 2.0, 10.0, w=1.0, k="b"),
)
# A NULL key row with a negative weight is in no budget: it frees nothing and takes the end of no budget.
NULL_KEY_NEGATIVE = (_row(1, 2.0, 4.0, w=1.0, k="a"), _row(2, 4.0, 3.0, w=-1.0, k=None))
# Limits read from the data: the tightest of a group applies, a NULL is no limit and a group of NULLs has no budget.
DATA_LIMITS = (
    _row(1, 2.0, 10.0, k="a", l=9.0),
    _row(2, 4.0, 20.0, k="a", l=12.0),
    _row(3, 1.0, 5.0, k="b", l=None),
    _row(4, 1.0, 6.0, k="b", l=None),
    _row(5, 2.0, 10.0, k="c", l=None),
    _row(6, 4.0, 20.0, k="c", l=6.0),
)
DATA_LIMIT_NEGATIVE_IN_ONE_GROUP = (_row(1, 2.0, 10.0, k="a", l=9.0), _row(2, 1.0, 10.0, k="b", l=-1.0))
# The group of the first row has a NULL limit, so its row takes the objective's level.
DATA_LIMIT_NULL_GROUP = (_row(1, 2.0, 10.0, k="a", l=None), _row(2, 4.0, 20.0, k="b", l=9.0))
# A negative budget that a freed budget makes feasible, with no row the MIN reads: the uncapped row takes what is needed.
NOTHING_READ_FREED = (_row(1, None, 4.0, w=1.0), _row(2, None, None, w=-1.0))


# A decision shared by the rows of a key. Its weight is the sum of the rows' weights, its capacity the least of their
# capacities, and the MIN reads the least of their coefficients.
KEYED_SEVERAL = (
    _row(1, 2.0, 4.0, w=1.0, k="a"),
    _row(2, 4.0, 8.0, w=1.0, k="a"),
    _row(3, 1.0, 10.0, w=2.0, k="b"),
    _row(4, 1.0, 10.0, w=1.0, k="b"),
    _row(5, 3.0, 6.0, w=1.0, k="c"),
)
KEYED_CAPACITIES_DIFFER = (_row(1, 2.0, 6.0, k="a"), _row(2, 2.0, 1.5, k="a"), _row(3, 2.0, 9.0, k="b"))
KEYED_MIXED_WEIGHTS = (
    _row(1, 2.0, 4.0, w=3.0, k="a"),
    _row(2, 2.0, 4.0, w=-1.0, k="a"),
    _row(3, 1.0, 5.0, w=-2.0, k="b"),
    _row(4, 1.0, 5.0, w=1.0, k="b"),
    _row(5, 2.0, 6.0, w=1.0, k="c"),
)
# One row of a key is outside the MIN: it still weighs on the budget, and its capacity still bounds the key.
KEYED_ROW_NOT_READ = (
    _row(1, 2.0, 4.0, w=1.0, k="a"),
    _row(2, None, 3.0, w=2.0, k="a"),
    _row(3, 4.0, 8.0, w=1.0, k="b"),
)
# A row with a NULL key has no decision: its capacity, weight and demand are read nowhere.
KEYED_NULL_KEY = (
    _row(1, 2.0, 4.0, w=1.0, k="a"),
    _row(2, 0.0, -1.0, w=float("nan"), k=None),
    _row(3, 4.0, 8.0, w=1.0, k="b"),
    _row(4, 1.0, 0.5, w=1.0, k=None),
)
KEYED_NEGATIVE_UNCAPPED = (
    _row(1, 2.0, 4.0, w=1.0, k="a"),
    _row(2, 3.0, None, w=-1.0, k="b"),
    _row(3, None, None, w=-1.0, k="b"),
)
KEYED_UNBOUNDED = (_row(1, 2.0, None, w=0.0, k="a"), _row(2, 4.0, None, w=0.0, k="b"))
KEYED_NEGATIVE_CAPACITY = (_row(1, 2.0, 4.0, k="a"), _row(2, 4.0, -1.0, k="a"))


def _values(rows):
    return values_clause(rows, NAMES, dict(COLUMNS), sql_literal)


# ---------------------------------------------------------------------------
# Budget, box and objective: each the SQL and what the oracle reads of it
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class Budget:
    sql: str
    #: row -> the weight of the row in the budget.
    weight: object
    #: (the rows of a group, all rows) -> the group's limit, or None for no limit.
    limit: object
    #: row -> the key of the group whose budget holds the row, or None when no budget holds it.
    group: object = lambda row: ()


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


def _constant(number):
    return lambda row: number


def _limit(number):
    return lambda members, rows: number


def _group_rows(members, rows):
    return members


def _all_rows(members, rows):
    return rows


def _tightest(rows_of):
    """The smallest limit `l` among the rows whose limit the group takes; a NULL is no limit."""

    def limit(members, rows):
        limits = [row["l"] for row in rows_of(members, rows) if row["l"] is not None]
        return min(limits) if limits else None

    return limit


def _by_k(row):
    return None if row["k"] is None else (row["k"],)


def _everyone(row):
    return ()


def _where_f(group):
    return lambda row: group(row) if row["f"] else None


def _column(row):
    return 0.0 if row["w"] is None else row["w"]


BUDGETS = {
    "b9": Budget("SUM(amount) <= 9", _constant(1.0), _limit(9.0)),
    "b100": Budget("SUM(amount) <= 100", _constant(1.0), _limit(100.0)),
    "b0": Budget("SUM(amount) <= 0", _constant(1.0), _limit(0.0)),
    "negative": Budget("SUM(amount) <= -1", _constant(1.0), _limit(-1.0)),
    # The spellings of one budget: each read the same way.
    "weight_2": Budget("SUM(2 * amount) <= 18", _constant(2.0), _limit(18.0)),
    "weight_half": Budget("SUM(amount / 2) <= 4.5", _constant(0.5), _limit(4.5)),
    "like_terms": Budget("SUM(amount + 2 * amount) <= 18", _constant(3.0), _limit(18.0)),
    "literal_on_left": Budget("9 >= SUM(amount)", _constant(1.0), _limit(9.0)),
    # A negative constant weight: every row takes its capacity.
    "weight_negative": Budget("SUM(-amount) <= -2", _constant(-1.0), _limit(-2.0)),
    # A weight read from the data.
    "column": Budget("SUM(w * amount) <= 9", _column, _limit(9.0)),
    "column_b0": Budget("SUM(w * amount) <= 0", _column, _limit(0.0)),
    "column_negative": Budget("SUM(w * amount) <= -3", _column, _limit(-3.0)),
    "column_like_terms": Budget("SUM(w * amount + 2 * w * amount) <= 18", lambda row: 3.0 * _column(row), _limit(18.0)),
    # Groups and a `when`: each group has a budget of its own, and a row outside them has none.
    "by": Budget("SUM(amount) by (k) <= 9", _constant(1.0), _limit(9.0), _by_k),
    "by_column": Budget("SUM(w * amount) by (k) <= 9", _column, _limit(9.0), _by_k),
    "by_data_limit": Budget("SUM(w * amount) by (k) <= l", _column, _tightest(_group_rows), _by_k),
    "data_limit": Budget("SUM(w * amount) <= l", _column, _tightest(_all_rows)),
    "when": Budget("when f: SUM(w * amount) <= 9", _column, _limit(9.0), _where_f(_everyone)),
    "sum_when": Budget("SUM(when f: w * amount) <= 9", _column, _limit(9.0), _where_f(_everyone)),
    "when_by": Budget("when f: SUM(w * amount) by (k) <= 9", _column, _limit(9.0), _where_f(_by_k)),
    "when_data_limit": Budget("when f: SUM(w * amount) <= l", _column, _tightest(_group_rows), _where_f(_everyone)),
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
    #: `DECIDE per k:`: one amount for the rows of a key, none for a row whose key is NULL.
    keyed: bool = False


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
# A weight of any sign. The worked example read from a column, and the sign decides the end of each row's range.
add("worked_example_weight_from_a_column", WORKED, budget="column")
add("weights_of_mixed_signs_and_zero", MIXED_WEIGHTS, budget="column")
add("weights_like_terms_add", MIXED_WEIGHTS, budget="column_like_terms")
add("weight_null_adds_nothing", NULL_WEIGHT, budget="column")
add("weight_negative_constant_takes_every_capacity", THREE, budget="weight_negative")
add("weight_negative_takes_its_capacity_and_frees_budget", NEGATIVE_CAPPED, budget="column_b0")
add("weight_negative_without_capacity_is_read", NEGATIVE_UNCAPPED, budget="column_b0")
add("weight_negative_without_capacity_not_read_absorbs_the_deficit", NEGATIVE_UNCAPPED_NOT_READ, budget="column_b0")
add("weight_negative_without_capacity_low_end_beats_the_deficit", NEGATIVE_UNCAPPED_READ_LOW, budget="column_b0")
add("weight_negative_without_capacity_two_rows_share_the_deficit", NEGATIVE_UNCAPPED_TWO, budget="column_b0")
add("budget_negative_made_feasible_by_a_freed_budget", NEGATIVE_CAPPED, budget="column_negative")
add("budget_negative_not_made_feasible_is_infeasible", NEGATIVE_NOT_ENOUGH, budget="column_negative")
add("level_unbounded_by_a_read_row_of_weight_zero_and_no_capacity", UNBOUNDED_ZERO_WEIGHT, budget="column")
add("level_unbounded_when_a_negative_weight_frees_unlimited_budget", UNBOUNDED_ALL_NEGATIVE, budget="column")
add("budget_negative_with_no_row_read_the_uncapped_row_frees_it", NOTHING_READ_FREED, budget="column_negative")
# A decision shared by the rows of a key.
add("keys_several", KEYED_SEVERAL, budget="column", keyed=True)
add("keys_the_least_capacity_binds", KEYED_CAPACITIES_DIFFER, budget="b100", keyed=True)
add("keys_weights_of_mixed_signs_add", KEYED_MIXED_WEIGHTS, budget="column", keyed=True)
add("keys_a_row_outside_the_min_still_weighs", KEYED_ROW_NOT_READ, budget="column", keyed=True)
add("keys_null_key_row_has_no_decision", KEYED_NULL_KEY, budget="column", keyed=True)
add("keys_negative_weight_without_capacity", KEYED_NEGATIVE_UNCAPPED, budget="column_b0", keyed=True)
add("keys_level_unbounded", KEYED_UNBOUNDED, budget="column", keyed=True)
add("keys_negative_capacity_is_infeasible", KEYED_NEGATIVE_CAPACITY, budget="b9", keyed=True)
add("keys_under_a_budget_per_key", KEYED_SEVERAL, budget="by_column", keyed=True)
add("keys_under_a_budget_per_key_and_a_null_key", KEYED_NULL_KEY, budget="by", keyed=True)
# A budget per group, and a `when` on it.
add("groups_each_have_a_level", TWO_GROUPS, budget="by")
add("groups_a_capacity_binds_one_level", GROUP_CAPACITY_BINDS, budget="by")
add("groups_a_group_with_no_finite_level_takes_the_objective", GROUP_NO_FINITE_LEVEL, budget="by_column")
add("groups_null_key_row_is_outside_every_budget", OUTSIDE_NULL_KEY, budget="by")
add("groups_outside_row_without_capacity_takes_the_objective", OUTSIDE_UNBOUNDED_ROW, budget="by")
add("groups_no_group_has_a_finite_level_is_unbounded", ALL_GROUPS_UNBOUNDED, budget="by_column")
add("groups_negative_uncapped_row_absorbs_its_groups_deficit", GROUP_NEGATIVE_UNCAPPED, budget="by_column")
add("groups_null_key_row_with_a_negative_weight_frees_nothing", NULL_KEY_NEGATIVE, budget="by_column")
add("when_rows_it_drops_are_outside_every_budget", OUTSIDE_WHEN, budget="when")
add("when_inside_the_sum", OUTSIDE_WHEN, budget="sum_when")
add("when_and_a_key", WHEN_AND_KEYS, budget="when_by")
# A limit read from the data.
add("data_limit_one_budget_takes_the_tightest", DATA_LIMITS, budget="data_limit")
add("data_limit_per_group_takes_the_tightest", DATA_LIMITS, budget="by_data_limit")
add("data_limit_null_group_is_no_budget", DATA_LIMIT_NULL_GROUP, budget="by_data_limit")
add("data_limit_negative_in_one_group_is_infeasible", DATA_LIMIT_NEGATIVE_IN_ONE_GROUP, budget="by_data_limit")
add("data_limit_under_a_when", WHEN_AND_KEYS, budget="when_data_limit")


def _sql_body(case_):
    box = BOXES[case_.box]
    budget = BUDGETS[case_.budget]
    objective = OBJECTIVES[case_.objective]
    declaration = "per k: amount(REAL)" if case_.keyed else "amount(REAL)"
    return f"DECIDE {declaration} SUCH THAT {box.sql} AND {budget.sql} {objective.sql}"


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


def _groups(case_):
    """The budgets that exist: {key: (rows, limit)}. A group with no limit has no budget."""
    budget = BUDGETS[case_.budget]
    members = {}
    for row in case_.rows:
        key = budget.group(row)
        if key is not None:
            members.setdefault(key, []).append(row)
    groups = {key: (rows, budget.limit(rows, case_.rows)) for key, rows in members.items()}
    return {key: group for key, group in groups.items() if group[1] is not None}


def _variable(case_, row):
    """The name of the row's decision, or None for a row that has none."""
    if not case_.keyed:
        return f"x_{row['id']}"
    return None if row["k"] is None else f"x_{row['k']}"


def _solve_with_oracle(oracle_solver, case_):
    oracle_solver.create_model("direct_r4_max_min_fairness")
    box, budget, objective = BOXES[case_.box], BUDGETS[case_.budget], OBJECTIVES[case_.objective]
    uppers = {}
    for row in case_.rows:
        name = _variable(case_, row)
        if name is not None:
            end = box.upper(row)
            uppers[name] = min(uppers.get(name, BIG), BIG if end is None else end)
    for name, upper in uppers.items():
        oracle_solver.add_variable(name, VarType.CONTINUOUS, 0.0, upper)
    for index, (members, limit) in enumerate(_groups(case_).values()):
        weights = {}
        for row in members:
            name = _variable(case_, row)
            if name is not None:
                weights[name] = weights.get(name, 0.0) + budget.weight(row)
        oracle_solver.add_constraint(weights, "<=", limit, name=f"budget_{index}")
    read = [(row, objective.coefficient(row)) for row in case_.rows if _variable(case_, row) is not None]
    read = [(row, c) for row, c in read if c is not None]
    if read:
        oracle_solver.add_variable("level", VarType.CONTINUOUS, -BIG, BIG)
        for row, c in read:
            oracle_solver.add_constraint(
                {"level": 1.0, _variable(case_, row): -c}, "<=", 0.0, name=f"level_{row['id']}_{_variable(case_, row)}"
            )
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
    chosen = {}
    for source in case_.rows:
        value = values[source["id"]]
        name = _variable(case_, source)
        if name is None:
            assert value is None, (mode, source["id"], value)
            continue
        assert isinstance(value, float), (mode, source["id"], value)
        assert chosen.setdefault(name, value) == value, (mode, source["id"], value, chosen[name])
        upper = box.upper(source)
        assert value >= -_FEASIBILITY_TOLERANCE, (mode, source["id"], value)
        assert upper is None or value <= upper + _FEASIBILITY_TOLERANCE * max(1.0, abs(upper)), (
            mode,
            source["id"],
            value,
            upper,
        )
    for key, (members, limit) in _groups(case_).items():
        spent = sum(
            budget.weight(source) * values[source["id"]] for source in members if values[source["id"]] is not None
        )
        assert spent <= limit + _FEASIBILITY_TOLERANCE * max(1.0, abs(limit)), (mode, key, spent, limit)
    level = [
        c * values[source["id"]]
        for source in case_.rows
        if values[source["id"]] is not None and (c := objective.coefficient(source)) is not None
    ]
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
@pytest.mark.parametrize("weight", ["amount", "w * amount"])
def test_r4_empty_source_returns_no_rows_whatever_the_budget_says(decidb_cli, budget, weight):
    # The checks run on rows, and there are none: a negative budget has no row to make infeasible, nor has the class
    # of an empty source, whose budget B + F is still negative.
    sql = (
        "SELECT id, amount FROM (SELECT 1 AS id, 2.0::DOUBLE AS d, -1.0::DOUBLE AS cap, 1.0::DOUBLE AS w WHERE false) "
        f"DECIDE amount(REAL) SUCH THAT amount <= cap AND SUM({weight}) <= {budget} MAXIMIZE MIN(amount / d)"
    )
    for mode in ("off", "require"):
        assert run(decidb_cli, sql, mode)[0] == []


# ---------------------------------------------------------------------------
# Errors: both paths fail, in the same class, with the same text.
# ---------------------------------------------------------------------------

_RATIO = "amount <= cap AND SUM(amount) <= 9 MAXIMIZE MIN(amount / d)"
_RATE = "amount <= cap AND SUM(amount) <= 9 MAXIMIZE MIN(r * amount)"
_WEIGHT = "amount <= cap AND SUM(w * amount) <= 9 MAXIMIZE MIN(amount / d)"
_LIMIT = "amount <= cap AND SUM(w * amount) <= l MAXIMIZE MIN(amount / d)"

# (the class of the error, the text it has, the bad row's d, cap, r and w, the clauses, and its l). Row 0 is ordinary.
ERROR_CASES = {
    "demand_zero": ("invalid_value", "1 / d is NaN or infinite on some rows", "0.0", "4.0", "1.0", "1.0", _RATIO),
    "demand_nan": ("invalid_value", "1 / d is NaN or infinite on some rows", "'NaN'", "4.0", "1.0", "1.0", _RATIO),
    "rate_nan": ("invalid_value", "r is NaN or infinite on some rows", "2.0", "4.0", "'NaN'", "1.0", _RATE),
    "rate_infinite": ("invalid_value", "r is NaN or infinite on some rows", "2.0", "4.0", "'Infinity'", "1.0", _RATE),
    "weight_nan": ("invalid_value", "w is NaN or infinite on some rows", "2.0", "4.0", "1.0", "'NaN'", _WEIGHT),
    "weight_infinite": (
        "invalid_value",
        "w is NaN or infinite on some rows",
        "2.0",
        "4.0",
        "1.0",
        "'-Infinity'",
        _WEIGHT,
    ),
    "limit_nan": ("null_bound", "the bound l is NaN on some rows", "2.0", "4.0", "1.0", "1.0", _LIMIT, "'NaN'"),
    "capacity_nan": ("null_bound", "the bound cap is NaN on some rows", "2.0", "'NaN'", "1.0", "1.0", _RATIO),
    "capacity_minus_infinite": ("infeasible", "infeasible", "2.0", "'-Infinity'", "1.0", "1.0", _RATIO),
}


def _error_sql(demand, capacity, rate, weight, decide, limit="9.0"):
    def typed(value):
        return f"CAST({value} AS DOUBLE)"

    source = (
        f"SELECT i AS id, CASE WHEN i = 0 THEN 2.0 ELSE {typed(demand)} END AS d, "
        f"CASE WHEN i = 0 THEN 4.0 ELSE {typed(capacity)} END AS cap, "
        f"CASE WHEN i = 0 THEN 1.0 ELSE {typed(rate)} END AS r, "
        f"CASE WHEN i = 0 THEN 1.0 ELSE {typed(weight)} END AS w, "
        f"CASE WHEN i = 0 THEN 9.0 ELSE {typed(limit)} END AS l FROM range(2) t(i)"
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
