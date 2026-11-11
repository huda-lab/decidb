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
# Columns the queries read but the results do not show: `flag` is the `when` of a budget or an objective; `must` and
# `ban` pin a row to 1 and to 0; `lo` and `hi` are the ends a row allows `x`.
HIDDEN = (
    ("flag", "BOOLEAN"),
    ("must", "BOOLEAN"),
    ("ban", "BOOLEAN"),
    ("lo", "DOUBLE"),
    ("hi", "DOUBLE"),
)
SOURCE_NAMES = NAMES + [name for name, _ in HIDDEN]
TYPES = dict(COLUMNS + HIDDEN)
ROW_DEFAULTS = dict(g="a", cap=0.0, flag=True, must=False, ban=False, lo=0.0, hi=1.0)


def _rows(weights, group="a", caps=None, **columns):
    """One row per weight, numbered from 1, in one group (or in the groups `group` lists), with the limits `caps`.

    A keyword names a hidden column and lists its value on every row (`flag=[True, False]`)."""
    groups = group if isinstance(group, (list, tuple)) else [group] * len(weights)
    caps = caps if caps is not None else [0.0] * len(weights)
    rows = []
    for i, (w, g, c) in enumerate(zip(weights, groups, caps), 1):
        hidden = {name: values[i - 1] for name, values in columns.items()}
        rows.append(dict(ROW_DEFAULTS, id=i, g=g, w=w, cap=c, **hidden))
    return tuple(rows)


def _values(rows):
    return values_clause(rows, SOURCE_NAMES, TYPES, sql_literal)


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
    #: row -> whether the budget's `when` keeps the row. A row it drops has no weight and is in no budget.
    kept: object = lambda row: True
    #: row -> whether the objective counts the row (the rows its `when` keeps).
    counted: object = lambda row: True
    #: The per-row clauses beside the budget as written ('' for none), and row -> the values of x the row's clauses
    #: allow, stated here in Python: (0, 1) for a free row, (1,) for a row fixed to 1, (0,) for one fixed to 0.
    fixes: str = ""
    allowed: object = lambda row: (0, 1)
    #: True for a minimum, `SUM(w*x) >= B`, with a minimized count: the fewest items that reach B. Otherwise the budget
    #: is a maximum and the count is maximized.
    at_least: bool = False


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
    dict(ROW_DEFAULTS, id=1, w=2.0),
    dict(ROW_DEFAULTS, id=2, w=3.0),
    dict(ROW_DEFAULTS, id=2, w=3.0),
    dict(ROW_DEFAULTS, id=3, w=4.0),
)
add("duplicate_rows", DUPLICATES, "SUM(w*x) <= 5", constant(5.0))

# A NULL weight is a NULL summand, which SUM skips: the row costs nothing and is selected.
add("null_weight", _rows([2.0, None, 4.0, 6.0]), "SUM(w*x) <= 5", constant(5.0))

# A `when` on the budget. The rows it drops (flag false or NULL) have no weight and are in no budget, so they cost
# nothing; the objective counts them (it has no `when`), so they are selected: the weights 2, 3 and 4 fit under 9, and
# the two dropped rows, 100 and 200, are selected beside them.
KEPT = lambda row: bool(row["flag"])  # noqa: E731  (NULL drops the row)
DROPPED = _rows([2.0, 3.0, 4.0, 6.0, 100.0, 200.0], flag=[True, True, True, True, False, None])
add("when_in_front_of_the_budget", DROPPED, "when flag: SUM(w*x) <= 9", constant(9.0), kept=KEPT)
add("when_inside_the_sum", DROPPED, "SUM(when flag: w*x) <= 9", constant(9.0), kept=KEPT)
# The clause's `when` and the sum's own: a row is in the budget when both keep it.
add(
    "when_in_front_and_inside_the_sum",
    _rows([2.0, 3.0, 4.0, 6.0, 1.0], flag=[True, True, True, False, True], must=[True, True, True, True, False]),
    "when flag: SUM(when must: w*x) <= 9",
    constant(9.0),
    kept=lambda row: bool(row["flag"]) and bool(row["must"]),
)
# The objective's `when` is the budget's: a row dropped from both adds nothing and costs nothing, so the count is the
# kept rows' alone (3, not 5).
add(
    "objective_when_equal_to_the_budgets",
    DROPPED,
    "when flag: SUM(w*x) <= 9",
    constant(9.0),
    objective="MAXIMIZE when flag: SUM(x)",
    kept=KEPT,
    counted=KEPT,
)
# A limit read from the data is read on the rows the `when` keeps alone: the dropped row's limit of 0.5 is not one of
# the group's limits (the tightest of the others is 7).
add(
    "when_budget_reads_limits_of_kept_rows_only",
    _rows([2.0, 3.0, 4.0, 6.0, 1.0], caps=[9.0, 7.0, 12.0, 8.0, 0.5], flag=[True, True, True, True, False]),
    "when flag: SUM(w*x) <= cap",
    column,
    kept=KEPT,
)
# Negative weights among the kept rows, and a dropped row too heavy for any budget.
add(
    "when_budget_with_negative_weights",
    _rows([-3.0, -1.0, 2.0, 4.0, 5.0, 1000.0], flag=[True, True, True, True, True, False]),
    "when flag: SUM(w*x) <= 4",
    constant(4.0),
    kept=KEPT,
)
# A `when` and a `by` key: each group's kept rows fit their group's budget, a dropped row is in no group, and a kept
# row with a NULL key is outside every budget too.
WHEN_GROUPS = _rows(
    [2.0, 3.0, 4.0, 6.0, 1.0, 1.0, 2.0, 5.0, 7.0, 8.0],
    group=["a"] * 4 + ["b"] * 3 + ["a", "b", None],
    flag=[True] * 7 + [False, False, True],
)
add(
    "when_with_by_groups_and_equal_objective_when",
    WHEN_GROUPS,
    "when flag: SUM(w*x) by (g) <= 5",
    constant(5.0),
    objective="MAXIMIZE when flag: SUM(x)",
    by=True,
    kept=KEPT,
    counted=KEPT,
)
# Fixed items. A row a clause fixes to 1 is selected and its weight counts toward the budget; a row fixed to 0 leaves
# the prefix; the free rows take the longest prefix that fits beside the rows fixed to 1.
PINNED = lambda row: (1,) if row["must"] else (0,) if row["ban"] else (0, 1)  # noqa: E731
FIX_MUST = "when must: x = 1"
FIX_BAN = "when ban: x = 0"
# The 6 is fixed to 1: the free rows 2, 3 and 4 are left 3 of the budget, and only the 2 fits.
add(
    "fixed_to_one_leaves_the_rest_of_the_budget",
    _rows([2.0, 3.0, 4.0, 6.0], must=[False, False, False, True]),
    "SUM(w*x) <= 9",
    constant(9.0),
    fixes=FIX_MUST,
    allowed=PINNED,
)
# The 2 is fixed to 0: the 3 and the 4 fit, the 6 does not.
add(
    "fixed_to_zero_leaves_the_prefix",
    _rows([2.0, 3.0, 4.0, 6.0], ban=[True, False, False, False]),
    "SUM(w*x) <= 9",
    constant(9.0),
    fixes=FIX_BAN,
    allowed=PINNED,
)
add(
    "fixed_to_one_and_to_zero",
    _rows([2.0, 3.0, 4.0, 6.0, 1.0], must=[False, True, False, False, False], ban=[True, False, False, False, False]),
    "SUM(w*x) <= 9",
    constant(9.0),
    fixes=f"{FIX_MUST} AND {FIX_BAN}",
    allowed=PINNED,
)
# A fixed row of each sign of weight: a negative weight fixed to 1 is a credit (-5 leaves 9 for the free rows), fixed to
# 0 it is lost; a zero weight costs nothing either way.
NEGATIVE_FIXED = [-5.0, 2.0, 3.0, 4.0, 6.0, 0.0]
add(
    "negative_weight_fixed_to_one",
    _rows(NEGATIVE_FIXED, must=[True] + [False] * 5),
    "SUM(w*x) <= 4",
    constant(4.0),
    fixes=FIX_MUST,
    allowed=PINNED,
)
add(
    "negative_weight_fixed_to_zero",
    _rows(NEGATIVE_FIXED, ban=[True] + [False] * 5),
    "SUM(w*x) <= 4",
    constant(4.0),
    fixes=FIX_BAN,
    allowed=PINNED,
)
# The rows fixed to 1 use the budget exactly: 4 + 5 = 9 fits (a comparison with `<=`), and the free row of 5 is the one
# that completes it.
add(
    "fixed_total_and_prefix_total_meet_the_budget",
    _rows([4.0, 5.0, 6.0], must=[True, False, False]),
    "SUM(w*x) <= 9",
    constant(9.0),
    fixes=FIX_MUST,
    allowed=PINNED,
)
# The fixed rows alone are over the budget (10 > 9), and the free row of -3 brings the total back within it: the
# empty prefix is not feasible, the prefix of one row is.
add(
    "fixed_over_the_budget_rescued_by_a_negative_free_weight",
    _rows([10.0, -3.0, 2.0], must=[True, False, False]),
    "SUM(w*x) <= 9",
    constant(9.0),
    fixes=FIX_MUST,
    allowed=PINNED,
)
# Bounds read from the data: lo = 1 fixes a row to 1, hi = 0 fixes it to 0.
add(
    "data_bounds_fix_rows",
    _rows([2.0, 3.0, 4.0, 6.0], lo=[0.0, 1.0, 0.0, 0.0], hi=[1.0, 1.0, 0.0, 1.0]),
    "SUM(w*x) <= 9",
    constant(9.0),
    fixes="x >= lo AND x <= hi",
    allowed=lambda row: (1,) if row["lo"] >= 1 else (0,) if row["hi"] < 1 else (0, 1),
)
# A constant bound on every row: no row is selected whatever the budget allows.
add(
    "constant_bound_fixes_every_row_to_zero",
    _rows([2.0, 3.0, 4.0]),
    "SUM(w*x) <= 9",
    constant(9.0),
    fixes="x <= 0",
    allowed=lambda row: (0,),
)
# Fixed rows in groups, each with its own budget; the groups' fixed rows are counted in their own budget alone.
FIXED_GROUPS = _rows(
    [2.0, 3.0, 4.0, 5.0, 1.0, 2.0, 4.0],
    group=["a"] * 3 + ["b"] * 4,
    caps=[6.0] * 3 + [5.0] * 4,
    must=[False, False, True, False, True, False, False],
    ban=[False, True, False, False, False, False, True],
)
add(
    "fixed_rows_in_by_groups",
    FIXED_GROUPS,
    "SUM(w*x) by (g) <= cap",
    column,
    by=True,
    fixes=f"{FIX_MUST} AND {FIX_BAN}",
    allowed=PINNED,
)
# A fixed row outside every budget (a NULL key, a dropped row) is fixed all the same: the row fixed to 0 is lost to
# the objective, and a row fixed to 1 is selected though the objective's `when` drops it too.
OUTSIDE = _rows(
    [2.0, 3.0, 4.0, 100.0, 200.0, 7.0],
    group=["a", "a", "a", None, None, "a"],
    flag=[True, True, True, True, True, False],
    ban=[False, False, False, True, False, False],
    must=[False, False, False, False, True, True],
)
add(
    "fixed_rows_outside_every_budget_with_the_objective_when",
    OUTSIDE,
    "when flag: SUM(w*x) by (g) <= 5",
    constant(5.0),
    objective="MAXIMIZE when flag: SUM(x)",
    by=True,
    kept=KEPT,
    counted=KEPT,
    fixes=f"{FIX_MUST} AND {FIX_BAN}",
    allowed=PINNED,
)
# A data limit with fixed rows: the group's tightest limit (7) is shared by the fixed row of 3 and the prefix.
add(
    "fixed_row_beside_a_data_budget",
    _rows([2.0, 3.0, 4.0, 6.0], caps=[9.0, 7.0, 12.0, 8.0], must=[False, True, False, False]),
    "SUM(w*x) <= cap",
    column,
    fixes=FIX_MUST,
    allowed=PINNED,
)

# The fewest items that reach a minimum: `SUM(w*x) >= B` with a minimized count. The heaviest rows come first.
FEWEST = "MINIMIZE SUM(x)"
# The worked example of definition.md under `>= 9`: 6 and then 4 reach 10, so D and C are selected.
add("at_least_worked_example", WORKED, "SUM(w*x) >= 9", constant(9.0), objective=FEWEST, at_least=True)
# The weights reach B exactly with the first two.
add(
    "at_least_total_meets_the_minimum_exactly",
    _rows([2.0, 3.0, 4.0]),
    "SUM(w*x) >= 7",
    constant(7.0),
    objective=FEWEST,
    at_least=True,
)
# Every row is needed.
add(
    "at_least_every_row_is_needed",
    _rows([1.5, 2.5, 3.0]),
    "SUM(w*x) >= 7",
    constant(7.0),
    objective=FEWEST,
    at_least=True,
)
# Negative weights: the totals of the heaviest first are 5, 8, -2 and -12, so they rise and then fall. Only the second
# reaches 8; the search stops at the shortest prefix that reaches B, and a later one would not reach it.
add(
    "at_least_prefix_totals_rise_and_then_fall",
    _rows([5.0, -10.0, -10.0, 3.0]),
    "SUM(w*x) >= 8",
    constant(8.0),
    objective=FEWEST,
    at_least=True,
)
add(
    "at_least_negative_and_zero_weights",
    _rows([-3.0, -1.0, 0.0, 2.0, 0.0, 4.0]),
    "SUM(w*x) >= 5",
    constant(5.0),
    objective=FEWEST,
    at_least=True,
)
# A minimum of at most 0 is met by the empty prefix: nothing is selected, whatever the weights.
add("at_least_zero_minimum", _rows([2.0, 3.0]), "SUM(w*x) >= 0", constant(0.0), objective=FEWEST, at_least=True)
add(
    "at_least_negative_minimum_with_negative_weights",
    _rows([-2.0, -3.0]),
    "SUM(w*x) >= -1",
    constant(-1.0),
    objective=FEWEST,
    at_least=True,
)
# A maximized negation of the count is the minimized count.
add(
    "at_least_maximized_negated_count",
    WORKED,
    "SUM(w*x) >= 9",
    constant(9.0),
    objective="MAXIMIZE SUM(-x)",
    at_least=True,
)
# Groups. a needs 9 (6 and 4: two rows), b needs 3 (2 and 1: two), c needs 4 (one row of 5), d is met by the empty
# prefix (0 reaches -1), e needs 2 (one row of 2).
AT_LEAST_GROUPS = _rows(
    [2.0, 3.0, 4.0, 6.0, 1.0, 1.0, 2.0, 5.0, 5.0, -2.0, 1.0, 1.0, 2.0],
    group=["a"] * 4 + ["b"] * 3 + ["c"] * 2 + ["d"] * 2 + ["e"] * 2,
    caps=[9.0] * 4 + [3.0] * 3 + [4.0] * 2 + [-1.0] * 2 + [2.0] * 2,
)
add(
    "at_least_by_groups_with_different_minimums",
    AT_LEAST_GROUPS,
    "SUM(w*x) by (g) >= cap",
    column,
    objective=FEWEST,
    by=True,
    at_least=True,
)
# A row with a NULL key is in no group, so no minimum needs it: it is not selected.
add(
    "at_least_null_key_rows_are_outside_every_minimum",
    NULL_KEY,
    "SUM(w*x) by (g) >= 5",
    constant(5.0),
    objective=FEWEST,
    by=True,
    at_least=True,
)
# A limit read from the data: the group's value is the highest of its rows' limits (9, not 5 or 7).
add(
    "at_least_data_minimum_takes_the_highest_limit",
    _rows([2.0, 3.0, 4.0, 6.0], caps=[5.0, 9.0, 7.0, 6.0]),
    "SUM(w*x) >= cap",
    column,
    objective=FEWEST,
    at_least=True,
)
# A `when` on the budget: a dropped row is in no minimum, and the objective counts it either way, so it is not selected.
add(
    "at_least_when_in_front_of_the_budget",
    DROPPED,
    "when flag: SUM(w*x) >= 9",
    constant(9.0),
    objective=FEWEST,
    kept=KEPT,
    at_least=True,
)
add(
    "at_least_objective_when_equal_to_the_budgets",
    DROPPED,
    "when flag: SUM(w*x) >= 9",
    constant(9.0),
    objective="MINIMIZE when flag: SUM(x)",
    kept=KEPT,
    counted=KEPT,
    at_least=True,
)
# Fixed items beside a minimum: the 4 is fixed to 1 and counts toward the 9, so the 6 reaches it (two items).
add(
    "at_least_fixed_to_one_counts_toward_the_minimum",
    _rows([4.0, 2.0, 3.0, 6.0], must=[True, False, False, False]),
    "SUM(w*x) >= 9",
    constant(9.0),
    objective=FEWEST,
    fixes=FIX_MUST,
    allowed=PINNED,
    at_least=True,
)
# With the 6 fixed to 0 the free rows are 3 and 2: 4 + 3 + 2 = 9 needs both.
add(
    "at_least_fixed_to_zero_leaves_the_prefix",
    _rows([4.0, 2.0, 3.0, 6.0], must=[True, False, False, False], ban=[False, False, False, True]),
    "SUM(w*x) >= 9",
    constant(9.0),
    objective=FEWEST,
    fixes=f"{FIX_MUST} AND {FIX_BAN}",
    allowed=PINNED,
    at_least=True,
)
# The fixed rows alone reach the minimum (10 >= 9): no free row is selected.
add(
    "at_least_fixed_rows_reach_the_minimum_alone",
    _rows([10.0, 2.0, 3.0], must=[True, False, False]),
    "SUM(w*x) >= 9",
    constant(9.0),
    objective=FEWEST,
    fixes=FIX_MUST,
    allowed=PINNED,
    at_least=True,
)
add(
    "at_least_negative_weight_fixed_to_one",
    _rows([-5.0, 2.0, 3.0, 4.0, 6.0], must=[True, False, False, False, False]),
    "SUM(w*x) >= 4",
    constant(4.0),
    objective=FEWEST,
    fixes=FIX_MUST,
    allowed=PINNED,
    at_least=True,
)
add(
    "at_least_fixed_rows_outside_every_budget",
    OUTSIDE,
    "when flag: SUM(w*x) by (g) >= 3",
    constant(3.0),
    objective=FEWEST,
    by=True,
    kept=KEPT,
    fixes=f"{FIX_MUST} AND {FIX_BAN}",
    allowed=PINNED,
    at_least=True,
)

# A `when` that drops every row leaves no budget: every row is selected.
add(
    "when_that_drops_every_row",
    _rows([2.0, 3.0, 4.0], flag=[False] * 3),
    "when flag: SUM(w*x) <= 1",
    constant(1.0),
    kept=KEPT,
)


# ---------------------------------------------------------------------------
# Running a case
# ---------------------------------------------------------------------------


def _decide(case):
    fixes = f" AND {case.fixes}" if case.fixes else ""
    return f"DECIDE x(BOOL) SUCH THAT {case.budget}{fixes} {case.objective}"


def _sql(case, wrap="plain"):
    """The case as a query. `plain` selects every source column and `x`, ordered by id."""
    decide = _decide(case)
    columns = ", ".join(SOURCE_NAMES)
    select = f"SELECT {', '.join(NAMES)}, x"
    if wrap == "plain":
        return f"{select} FROM (FROM (VALUES {_values(case.rows)}) t({columns}) {decide}) q ORDER BY id"
    if wrap == "joined_source":
        # The weights come from a second table through a join, so the source is not a scan.
        others = [name for name in SOURCE_NAMES if name != "w"]
        base = f"(VALUES {values_clause(case.rows, others, TYPES, sql_literal)}) t({', '.join(others)})"
        weights = f"(VALUES {values_clause(case.rows, ['id', 'w'], TYPES, sql_literal)}) z(id, w)"
        picked = ", ".join("z.w" if name == "w" else f"t.{name}" for name in SOURCE_NAMES)
        joined = f"SELECT {picked} FROM {base} JOIN {weights} ON z.id = t.id"
        return f"{select} FROM (FROM ({joined}) j {decide}) q ORDER BY id"
    if wrap == "counted":
        return f"SELECT COUNT(*), SUM(x), SUM(w*x) FROM (FROM (VALUES {_values(case.rows)}) t({columns}) {decide}) q"
    raise ValueError(wrap)


def _group_of(case, row):
    """The key of the row's budget, or None when the row is outside every budget."""
    return row["g"] if case.by and row["g"] is not None else None


def _in_a_budget(case, row):
    """Whether a budget covers the row: its `when` keeps it, and its `by` key (if any) is not NULL."""
    return case.kept(row) and (not case.by or row["g"] is not None)


def _groups(case):
    """key -> the rows of that group; the rows outside every budget are left out."""
    groups = {}
    for row in case.rows:
        if _in_a_budget(case, row):
            groups.setdefault(_group_of(case, row), []).append(row)
    return groups


def _limits(case, members):
    """The limits the budget states over a group, one per row; a NULL skips its instance."""
    limits = [case.limit(row, members) for row in members]
    return [limit for limit in limits if limit is not None]


def _solve_with_oracle(oracle_solver, case):
    oracle_solver.create_model("direct_s3_max_cardinality_knapsack")
    for index, row in enumerate(case.rows):
        allowed = case.allowed(row)
        oracle_solver.add_variable(f"x_{index}", VarType.BINARY, lb=min(allowed), ub=max(allowed))
    positions = {id(row): index for index, row in enumerate(case.rows)}
    for members in _groups(case).values():
        weights = {f"x_{positions[id(row)]}": case.weight(row) for row in members}
        for limit in _limits(case, members):
            oracle_solver.add_constraint(weights, ">=" if case.at_least else "<=", limit)
    counts = {f"x_{index}": case.factor for index, row in enumerate(case.rows) if case.counted(row)}
    oracle_solver.set_objective(counts, ObjSense.MINIMIZE if case.at_least else ObjSense.MAXIMIZE)
    return oracle_solver.solve()


def _check_budgets(case, selected):
    """Asserts the budget of every group holds as written for the rows `selected` (a list of 0/1 in row order)."""
    spend = {}
    for row, chosen in zip(case.rows, selected):
        if _in_a_budget(case, row):
            key = _group_of(case, row)
            spend[key] = spend.get(key, 0.0) + case.weight(row) * chosen
    for key, members in _groups(case).items():
        for limit in _limits(case, members):
            assert spend[key] >= limit if case.at_least else spend[key] <= limit, (key, spend[key], limit)


def _check_assignment(case, rows, columns, oracle, mode):
    assert columns == NAMES + ["x"], mode
    assert [row[0] for row in rows] == [row["id"] for row in case.rows], mode
    assert all(type(row[4]) is int and row[4] in (0, 1) for row in rows), (mode, rows)
    selected = [row[4] for row in rows]
    assert all(chosen in case.allowed(row) for row, chosen in zip(case.rows, selected)), (mode, selected)
    _check_budgets(case, selected)
    counted = sum(chosen for row, chosen in zip(case.rows, selected) if case.counted(row))
    assert math.isclose(case.factor * counted, oracle.objective_value, abs_tol=1e-6), (
        mode,
        counted,
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


# The `when` of the budget and of the objective is read through the join's columns too.
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.sql_subquery
@pytest.mark.correctness
@pytest.mark.parametrize("name", ["when_in_front_of_the_budget", "objective_when_equal_to_the_budgets"])
def test_s3_agrees_with_the_oracle_when_a_when_drops_rows_over_a_joined_source(decidb_cli, oracle_solver, name):
    _agree(decidb_cli, oracle_solver, CASES[name], wrap="joined_source")


# Nothing is computed on a row a `when` drops: a NaN weight there raises on neither path, wherever the `when` is and
# whether or not the objective drops the row too. The kept rows (1 and 3, weights 2 and 4) both fit under 9.
@pytest.mark.var_boolean
@pytest.mark.error
@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize(
    "budget, objective, dropped_row_selected",
    [
        ("when flag: SUM(w*x) <= 9", "MAXIMIZE SUM(x)", 1),
        ("SUM(when flag: w*x) <= 9", "MAXIMIZE SUM(x)", 1),
        ("when flag: SUM(w*x) <= 9", "MAXIMIZE when flag: SUM(x)", None),
        ("SUM(when flag: w*x) <= 9", "MAXIMIZE SUM(when flag: x)", None),
    ],
    ids=["when_in_front", "when_inside_the_sum", "objective_when_equal", "objective_part_when_equal"],
)
def test_s3_a_nan_weight_on_a_dropped_row_raises_on_neither_path(decidb_cli, budget, objective, dropped_row_selected):
    rows = "(1, 2.0::DOUBLE, true), (2, 'NaN'::DOUBLE, false), (3, 4.0::DOUBLE, true)"
    sql = (
        f"SELECT id, x FROM (FROM (VALUES {rows}) t(id, w, flag) "
        f"DECIDE x(BOOL) SUCH THAT {budget} {objective}) q ORDER BY id"
    )
    for mode in ("off", "require"):
        answer = dict(_run(decidb_cli, sql, mode)[0])
        assert answer[1] == answer[3] == 1, mode
        if dropped_row_selected is not None:
            assert answer[2] == dropped_row_selected, mode
    # Direct solve drops the row from an objective that drops it too.
    assert dict(_run(decidb_cli, sql, "require")[0])[2] == (dropped_row_selected or 0)


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


def _problem(rows, budget="SUM(w*x) <= 9", columns=("id", "g", "w", "cap"), objective="MAXIMIZE SUM(x)"):
    return (
        f"SELECT id, x FROM (FROM (VALUES {rows}) t({', '.join(columns)}) "
        f"DECIDE x(BOOL) SUCH THAT {budget} {objective}) q"
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


_FIXED_SOURCE = (
    "(1, 2.0::DOUBLE, true, {lo}::DOUBLE), (2, 3.0::DOUBLE, false, 0.0::DOUBLE), (3, 4.0::DOUBLE, true, 0.0::DOUBLE)"
)


def _fixed_problem(lo, budget="SUM(w*x) <= 5", fixes="when must: x = 1"):
    return (
        f"SELECT id, x FROM (FROM (VALUES {_FIXED_SOURCE.format(lo=lo)}) t(id, w, must, lo) "
        f"DECIDE x(BOOL) SUCH THAT {budget} AND {fixes} MAXIMIZE SUM(x)) q"
    )


S3_ERROR_CASES.update(
    {
        # No prefix of the heaviest first reaches the minimum: 3 + 4 + 2 = 9 is under 10.
        "no_prefix_reaches_the_minimum": (
            _problem(_SOURCE.format(w="3.0", cap="9.0"), "SUM(w*x) >= 10", objective="MINIMIZE SUM(x)"),
            "infeasible",
        ),
        # Only negative weights: no prefix, the empty one included, reaches a positive minimum.
        "negative_weights_cannot_reach_a_positive_minimum": (
            _problem(
                "(1, 'a', -2.0::DOUBLE, 9.0::DOUBLE), (2, 'a', -3.0::DOUBLE, 9.0::DOUBLE)",
                "SUM(w*x) >= 1",
                objective="MINIMIZE SUM(x)",
            ),
            "infeasible",
        ),
        # One group's minimum is out of reach (group b has one row of 4 under a minimum of 9).
        "minimum_out_of_reach_in_one_group": (
            _problem(
                _SOURCE.format(w="3.0", cap="9.0"),
                "SUM(w*x) by (g) >= CASE WHEN g = 'b' THEN 9 ELSE 3 END",
                objective="MINIMIZE SUM(x)",
            ),
            "infeasible",
        ),
        "nan_weight_under_a_minimum": (
            _problem(_SOURCE.format(w="'NaN'", cap="9.0"), "SUM(w*x) >= 3", objective="MINIMIZE SUM(x)"),
            "invalid_value",
        ),
        "nan_limit_of_a_minimum": (
            _problem(_SOURCE.format(w="3.0", cap="'NaN'"), "SUM(w*x) >= cap", objective="MINIMIZE SUM(x)"),
            "null_bound",
        ),
        # The rows fixed to 1 weigh 2 and 4, over the budget of 5, and no free weight is negative.
        "fixed_rows_over_the_budget": (_fixed_problem(0.0), "infeasible"),
        # Row 1 allows neither value: fixed to both.
        "row_allows_neither_value": (_fixed_problem(2.0, "SUM(w*x) <= 9", "x >= lo"), "infeasible"),
        "constant_bound_no_bool_meets_under_a_when": (
            _fixed_problem(0.0, "SUM(w*x) <= 9", "when must: x >= 2"),
            "infeasible",
        ),
        "nan_fixed_bound": (_fixed_problem("'NaN'", "SUM(w*x) <= 9", "x >= lo"), "null_bound"),
        # One group's fixed rows are over its budget; the other group is fine.
        "fixed_rows_over_one_groups_budget": (
            _problem(_SOURCE.format(w="3.0", cap="9.0"), "SUM(w*x) by (g) <= 4 AND when g = 'a': x = 1"),
            "infeasible",
        ),
    }
)


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


# The rows fixed to 1 are summed too. Like the running total of the prefix, a sum past a DOUBLE is a number the plan
# computes: direct solve raises and names the budget, where the solver warns and answers. The first case has no free
# row, so only the fixed rows' own total overflows; in the second each total is finite and their sum is not.
@pytest.mark.var_boolean
@pytest.mark.error
@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize(
    "rows",
    ["(1, -1e308::DOUBLE, true), (2, -1e308::DOUBLE, true)", "(1, -1e308::DOUBLE, true), (2, -1e308::DOUBLE, false)"],
    ids=["fixed_total_past_a_double", "fixed_total_and_prefix_total_sum_past_a_double"],
)
def test_s3_fixed_rows_past_a_double_raise_where_the_solver_answers(decidb_cli, rows):
    sql = (
        f"SELECT id, x FROM (FROM (VALUES {rows}) t(id, w, must) "
        "DECIDE x(BOOL) SUCH THAT SUM(w*x) <= 0 AND when must: x = 1 MAXIMIZE SUM(x)) q"
    )
    assert len(_run(decidb_cli, sql, "off", expect_number_range=True)[0]) == 2
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
