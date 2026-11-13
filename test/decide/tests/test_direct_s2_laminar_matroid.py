"""S2 (laminar matroid), checked three ways: the independent oracle, the solver path, the direct path.

Every case is one DECIDE query over one Boolean decision under upper count quotas whose groups nest (departments inside
divisions inside one global cap). The oracle builds its own model from the raw rows: one constraint per group of every
quota, never the cascade of rankings S2 runs. DeciDB then runs the query with `decide_direct_solve='off'` (the solver)
and `'require'` (direct solve; a query S2 does not apply to fails instead of quietly using the solver). The case
passes when all three agree: both runs return an assignment that satisfies every constraint and reaches the oracle's
objective (rows may differ on ties), or all three say infeasible.

A quota names its `by` key, its limit, and optionally a `when`. A row with a NULL in a key column is in no group of that
quota, and the quota's `when` drops a row from the quota and from every group of it. What the cases cover, one
decision point each:

- the cascade itself: the example of the definition, the case where one ranking per level is wrong (a division of cap 2
  holding a:9, b:8, c:7, where a and b share a department of cap 1), and chains of two, three and four levels;
- what the limits mean: zero, above the group's size, fractional, strict, a negated count body, clauses of one scope
  merged into one level, a limit below zero;
- fixed rows: a row fixed to 1 uses a quota of every level it is in, a row fixed to 0 leaves the ranking, and fixed rows
  beyond a quota are infeasible, at the finest level and at a coarser one;
- the objective: MINIMIZE, negative scores, ties, no objective;
- rows outside a level: a `when` on the finer level only, the same `when` on every level, a `when` inside the sum, a NULL
  key at each level;
- caps read from the data (S2_DATA_CAP_CASES): a source value per row (the group's tightest) or an aggregate of the
  group's rows, at the finer level, the coarser level and every level, beside a constant cap or another data cap of the
  same level, rounded as the constant ones are, NULL (no limit), below zero (infeasible), with fixed rows and under a
  `when`; a NaN cap raises the solver's error before the first row leaves.

Note: `oracle_solver` caches each result under the test node id plus a hash of the test function's source. The case
tables and the model builder live outside that source, so editing them does not invalidate the cache; a stale value
fails loudly with an objective mismatch. After changing a case or `_s2_constraints`, remove its entries from
`results/oracle_cache.json`.
"""

import math
import re

import pytest

from comparison.compare import compare_solutions
from decidb_cli import DeciDBCliError
from solver.types import ObjSense, VarType

from ._direct_differential import error_class
from ._direct_oracle_common import _allow_direct_path  # noqa: F401  (autouse fixture)
from ._direct_oracle_common import agree_with_oracle, outcome as _outcome, run as _run, sql_literal, values_clause
from ._number_range import writes_number_out_of_range

# ---------------------------------------------------------------------------
# Data
# ---------------------------------------------------------------------------

COLUMNS = (
    ("id", "INTEGER"),
    ("div", "VARCHAR"),  # the coarser key
    ("dept", "VARCHAR"),  # the finer key, which only means something inside a division
    ("score", "DOUBLE"),
    ("flag", "BOOLEAN"),  # the `when` condition
)
NAMES = [name for name, _ in COLUMNS]


def _row(id, div, dept, score, flag=True, **extra):
    return dict(id=id, div=div, dept=dept, score=score, flag=flag, **extra)


# Divisions N (rows 0-3 and 11), S (4-6), W (7-9) and none (10); departments inside them. Row 10 has no division and row
# 11 has no department, so each is in some groups and not in others. Row 3 has a negative score.
MAIN = [
    _row(0, "N", "sales", 9.0),
    _row(1, "N", "sales", 7.0),
    _row(2, "N", "research", 8.0, False),
    _row(3, "N", "research", -2.0),
    _row(4, "S", "sales", 6.0),
    _row(5, "S", "sales", 5.0, False),
    _row(6, "S", "research", 4.5),
    _row(7, "W", "research", 4.0),
    _row(8, "W", "research", -1.0, False),
    _row(9, "W", "tools", 3.5),
    _row(10, None, "sales", 7.5),
    _row(11, "N", None, 6.5),
]

# The example of the definition: one candidate per department, one per division, two overall. A and D are selected.
EXAMPLE = [
    _row(0, "North", "Sales", 9.0),
    _row(1, "North", "Sales", 7.0),
    _row(2, "North", "Research", 8.0),
    _row(3, "South", "Sales", 6.0),
    _row(4, "South", "Sales", 5.0),
    _row(5, "West", "Research", 4.0),
]

# A division of cap 2 holding a:9, b:8, c:7, where a and b are in one department of cap 1. One ranking per level over
# all rows gives {a}, since b takes a place in the division that its department then refuses; the answer is {a, c}.
COUNTER = [
    _row(0, "D", "d1", 9.0),
    _row(1, "D", "d1", 8.0),
    _row(2, "D", "d2", 7.0),
]

# Four levels: team inside department inside division inside the query. Scores fall with the id, so every level keeps
# the rows of the lowest ids it can.
DEEP = [_row(i, "N" if i < 8 else "S", f"d{i // 4}", float(20 - i), team=f"t{i // 2}") for i in range(12)]

# No row has a division or a department: only the quota of the whole query has a group.
NULLS = [_row(i, None, None, float(10 - i)) for i in range(6)]

# Equal scores in every group, so a plan that ranks with ties (RANK instead of ROW_NUMBER) keeps too many rows, and a
# tie that one level breaks one way and the next level another must still fit every quota.
TIES = [
    _row(0, "N", "a", 5.0),
    _row(1, "N", "a", 5.0),
    _row(2, "N", "b", 5.0),
    _row(3, "N", "b", 5.0),
    _row(4, "S", "a", 5.0),
    _row(5, "S", "a", 5.0),
    _row(6, "S", "b", 0.0),
    _row(7, "S", "b", -3.0),
]

# Every row scores below zero: nothing improves a maximized objective.
LOSSES = [_row(i, "N" if i < 3 else "S", "a" if i % 2 else "b", -1.0 - i) for i in range(6)]

# MAIN with a cap per row, read from the data. Per department the tightest cap is 1 (N sales), 2 (N research), 1 (S
# sales), 3 (S research), 1 (W research) and 1 (W tools); per division 1, 1 and 1; the whole query 1. The largest are 2, 2,
# 1, 3, 2, 1 per department and 2, 3, 2 per division.
CAPS = [2.0, 1.0, 2.0, 2.0, 1.0, 1.0, 3.0, 2.0, 1.0, 1.0, 5.0, 2.0]
CAPPED = [dict(row, cap=cap) for row, cap in zip(MAIN, CAPS)]

# The same with NULL caps: every cap of S research, W research and W tools is NULL, so those departments, and the
# division W, have no cap from the data, and N research and S sales have one cap each among their rows.
CAPPED_NULLS = [
    dict(row, cap=cap) for row, cap in zip(MAIN, [2.0, None, None, 2.0, 1.0, None, None, None, None, None, 5.0, None])
]

DATASETS = {
    "capped": CAPPED,
    "capped_nulls": CAPPED_NULLS,
    "main": MAIN,
    "example": EXAMPLE,
    "counter": COUNTER,
    "deep": DEEP,
    "ties": TIES,
    "losses": LOSSES,
    "nulls": NULLS,
}

DIV = ("div",)
DEPT = ("div", "dept")
TEAM = ("div", "dept", "team")


def Q(keys, cap, op="<=", when=None, body="x", cap_of=None, per=None):
    """One quota clause: `SUM(body) by (keys) op cap`, under `when` (None, "flag" in front, "local" inside the sum),
    written `per K:` when `per` is K. K lists columns of `keys`: a NULL in K is then a NULL in the key too, so the `per`
    gates no group the key does not, and the solver's rows are the same as without it.

    `cap` is the SQL text of the limit. A limit read from the data also gives `cap_of(members, whole)`, the limit the
    solver reads for one group as Python computes it from the raw rows: `members` are the rows of the group (the rows
    the clause's `when` keeps with no NULL key) and `whole` all rows with the same key, which a reducer reads when the
    `when` stands inside the sum. None means NULL, which skips the group's instance.
    """
    return dict(keys=keys, cap=cap, op=op, when=when, body=body, cap_of=cap_of, per=per)


# A case is a dict with these keys:
#   data      key of DATASETS (default "main")
#   quotas    [Q(...)]    the clauses, in the order written
#   fixed     [(value, [ids])]     when id = i: x = value
#   sense     "MAXIMIZE" (default) or "MINIMIZE"; "none" for a query with no objective (every score is 0)
S2_CASES = {
    # --- the cascade ---
    "definition_example": {"data": "example", "quotas": [Q(DEPT, 1), Q(DIV, 1), Q((), 2)]},
    # As the definition writes it: a `per` on the two finer clauses and none on the global one.
    "definition_example_as_written": {
        "data": "example",
        "quotas": [Q(DEPT, 1, per=DEPT), Q(DIV, 1, per=DIV), Q((), 2)],
    },
    "independent_ranks_are_wrong": {"data": "counter", "quotas": [Q(DEPT, 1), Q(DIV, 2)]},
    "independent_ranks_are_wrong_written_coarse_first": {"data": "counter", "quotas": [Q(DIV, 2), Q(DEPT, 1)]},
    "two_levels_department_and_global": {"quotas": [Q(DEPT, 1), Q((), 3)]},
    "two_levels_division_and_global": {"quotas": [Q(DIV, 2), Q((), 4)]},
    "three_levels": {"quotas": [Q(DEPT, 1), Q(DIV, 2), Q((), 4)]},
    "three_levels_global_binds": {"quotas": [Q(DEPT, 2), Q(DIV, 3), Q((), 3)]},
    "three_levels_division_binds": {"quotas": [Q(DEPT, 2), Q(DIV, 1), Q((), 5)]},
    "three_levels_written_coarse_first": {"quotas": [Q((), 4), Q(DIV, 2), Q(DEPT, 1)]},
    "three_levels_written_in_between": {"quotas": [Q(DIV, 2), Q((), 4), Q(DEPT, 1)]},
    "four_levels": {"data": "deep", "quotas": [Q(TEAM, 1), Q(DEPT, 3), Q(DIV, 4), Q((), 6)]},
    "four_levels_every_level_binds": {"data": "deep", "quotas": [Q(TEAM, 2), Q(DEPT, 3), Q(DIV, 5), Q((), 6)]},
    # --- the limits ---
    "zero_cap_finest": {"quotas": [Q(DEPT, 0), Q(DIV, 2), Q((), 4)]},
    "zero_cap_coarser": {"quotas": [Q(DEPT, 1), Q(DIV, 0), Q((), 4)]},
    "zero_cap_global": {"quotas": [Q(DEPT, 1), Q(DIV, 2), Q((), 0)]},
    "cap_above_the_group_finest": {"quotas": [Q(DEPT, 50), Q(DIV, 2), Q((), 4)]},
    "cap_above_the_group_every_level": {"quotas": [Q(DEPT, 50), Q(DIV, 60), Q((), 70)]},
    "cap_above_the_group_global": {"quotas": [Q(DEPT, 1), Q(DIV, 2), Q((), 70)]},
    "fractional_cap": {"quotas": [Q(DEPT, 1.5), Q(DIV, 2.5), Q((), 4.5)]},
    "strict_caps": {"quotas": [Q(DEPT, 2, "<"), Q(DIV, 3, "<"), Q((), 5, "<")]},
    "strict_cap_beside_a_fraction": {"quotas": [Q(DEPT, 1.5, "<"), Q((), 3.5, "<")]},
    "negated_count_body": {"quotas": [Q(DEPT, -1, ">=", body="-x"), Q(DIV, -2, ">", body="-x"), Q((), 4)]},
    "limit_two_pow_53": {"quotas": [Q(DEPT, 1), Q((), 9007199254740992)]},
    "merged_clauses_of_one_scope": {"quotas": [Q(DEPT, 2), Q(DIV, 3), Q(DEPT, 1), Q((), 4), Q((), 3, "<"), Q(DIV, 9)]},
    "merged_clauses_the_looser_first": {"quotas": [Q(DIV, 9), Q((), 5), Q(DEPT, 2), Q(DIV, 1), Q((), 2)]},
    "merged_clauses_one_of_them_below_zero": {"quotas": [Q(DEPT, 1), Q(DIV, 3), Q(DIV, -1), Q((), 9)]},
    "limit_below_zero_finest": {"quotas": [Q(DEPT, -1), Q(DIV, 2), Q((), 4)]},
    "limit_below_zero_coarser": {"quotas": [Q(DEPT, 1), Q(DIV, 2), Q((), -0.5)]},
    "limit_below_zero_strict": {"quotas": [Q(DEPT, 1), Q(DIV, 0, "<"), Q((), 4)]},
    # --- fixed rows ---
    # Row 1 is fixed to 1: it uses the cap of its department (sales of N), its division and the query, so row 0, the
    # better row of the department, is left out.
    "fixed_one_uses_every_level": {"quotas": [Q(DEPT, 1), Q(DIV, 2), Q((), 4)], "fixed": [(1, [1])]},
    # Row 3 is fixed to 1: it uses the only place of its division, so no other row of N is selected.
    "fixed_one_uses_the_division": {"quotas": [Q(DEPT, 1), Q(DIV, 1), Q((), 4)], "fixed": [(1, [3])]},
    "fixed_one_uses_the_global_cap": {"quotas": [Q(DEPT, 1), Q(DIV, 3), Q((), 2)], "fixed": [(1, [9])]},
    "fixed_zero_leaves_the_ranking": {"quotas": [Q(DEPT, 1), Q(DIV, 2), Q((), 4)], "fixed": [(0, [0])]},
    "fixed_zero_and_one": {"quotas": [Q(DEPT, 1), Q(DIV, 2), Q((), 4)], "fixed": [(1, [1, 7]), (0, [0, 4])]},
    "fixed_to_fill_a_cap_exactly": {"quotas": [Q(DEPT, 1), Q(DIV, 2), Q((), 3)], "fixed": [(1, [1, 4, 7])]},
    # A fixed row the key leaves out of a level still uses the levels it is in: row 10 has no division, row 11 no
    # department.
    "fixed_one_with_no_division": {"quotas": [Q(DEPT, 1), Q(DIV, 1), Q((), 2)], "fixed": [(1, [10])]},
    "fixed_one_with_no_department": {"quotas": [Q(DEPT, 1), Q(DIV, 1), Q((), 2)], "fixed": [(1, [11])]},
    "fixed_with_a_negative_score": {"quotas": [Q(DEPT, 1), Q(DIV, 2), Q((), 4)], "fixed": [(1, [3, 8])]},
    "fixed_in_a_zero_cap_group_is_infeasible": {"quotas": [Q(DEPT, 0), Q(DIV, 2), Q((), 4)], "fixed": [(1, [0])]},
    "fixed_beyond_the_finest_cap": {"quotas": [Q(DEPT, 1), Q(DIV, 3), Q((), 5)], "fixed": [(1, [0, 1])]},
    "fixed_beyond_a_coarser_cap": {"quotas": [Q(DEPT, 2), Q(DIV, 2), Q((), 5)], "fixed": [(1, [0, 1, 3])]},
    "fixed_beyond_the_global_cap": {"quotas": [Q(DEPT, 3), Q(DIV, 4), Q((), 2)], "fixed": [(1, [0, 4, 7])]},
    # The last group to fail is the coarsest one: row 11 joins rows 0 and 3 in N only at the division and query levels.
    "fixed_beyond_the_division_through_a_row_with_no_department": {
        "quotas": [Q(DEPT, 2), Q(DIV, 2), Q((), 9)],
        "fixed": [(1, [0, 3, 11])],
    },
    "fixed_to_both_values_is_infeasible": {"quotas": [Q(DEPT, 1), Q(DIV, 2), Q((), 4)], "fixed": [(1, [2]), (0, [2])]},
    # --- the objective ---
    "minimize": {"quotas": [Q(DEPT, 1), Q(DIV, 2), Q((), 4)], "sense": "MINIMIZE"},
    "minimize_every_row_costs_something": {
        "data": "losses",
        "quotas": [Q(DEPT, 1), Q(DIV, 2), Q((), 3)],
        "sense": "MINIMIZE",
    },
    "negative_scores_are_not_selected": {"data": "losses", "quotas": [Q(DEPT, 1), Q(DIV, 2), Q((), 3)]},
    "ties": {"data": "ties", "quotas": [Q(DEPT, 1), Q(DIV, 2), Q((), 3)]},
    "ties_every_level_binds": {"data": "ties", "quotas": [Q(DEPT, 2), Q(DIV, 3), Q((), 4)]},
    "ties_minimize": {"data": "ties", "quotas": [Q(DEPT, 1), Q(DIV, 2), Q((), 3)], "sense": "MINIMIZE"},
    "no_objective": {"quotas": [Q(DEPT, 1), Q(DIV, 2), Q((), 4)], "sense": "none"},
    # --- rows outside a level ---
    # The `when` drops row 2, 3, 5, 8 from the department quota only; the division and the query count them.
    "when_on_the_finer_level": {"quotas": [Q(DEPT, 1, when="flag"), Q(DIV, 2), Q((), 4)]},
    "when_on_two_levels_from_the_finest": {"quotas": [Q(DEPT, 1, when="flag"), Q(DIV, 2, when="flag"), Q((), 4)]},
    "when_on_every_level": {"quotas": [Q(DEPT, 1, when="flag"), Q(DIV, 2, when="flag"), Q((), 2, when="flag")]},
    "when_inside_every_sum": {"quotas": [Q(DEPT, 1, when="local"), Q(DIV, 2, when="local"), Q((), 2, when="local")]},
    "when_on_the_finer_level_of_one_key": {"quotas": [Q(DIV, 1, when="flag"), Q(DIV, 3), Q((), 4)]},
    "when_with_a_fixed_row_it_drops": {"quotas": [Q(DEPT, 1, when="flag"), Q(DIV, 2), Q((), 4)], "fixed": [(1, [2])]},
    "when_with_a_fixed_row_it_keeps": {"quotas": [Q(DEPT, 1, when="flag"), Q(DIV, 2), Q((), 4)], "fixed": [(1, [1])]},
    # Rows 2 and 3 are fixed to 1 and share a department of cap 1; the `when` drops row 2 from that quota, so it holds.
    "when_drops_a_fixed_row_that_would_overfill_a_group": {
        "quotas": [Q(DEPT, 1, when="flag"), Q(DIV, 3), Q((), 9)],
        "fixed": [(1, [2, 3])],
    },
    "null_division_and_department": {"quotas": [Q(DEPT, 1), Q(DIV, 1), Q((), 5)]},
    "null_keys_beside_a_zero_cap": {"quotas": [Q(DEPT, 0), Q(DIV, 0), Q((), 5)]},
    "no_row_in_a_group_of_the_finer_levels": {"data": "nulls", "quotas": [Q(DEPT, 1), Q(DIV, 1), Q((), 2)]},
}

_OPNAMES = {"<=": "le", "<": "lt", ">=": "ge", ">": "gt"}

# ---------------------------------------------------------------------------
# Caps read from the data: what each reads per group, as the solver does
# ---------------------------------------------------------------------------


def _caps(members):
    return [r["cap"] for r in members if r["cap"] is not None]


def _tightest(members, whole):
    """A per-row cap's tightest value in the group; NULL when every row's is NULL."""
    return min(_caps(members), default=None)


def _largest(members, whole):
    """MAX(cap) over the group's rows; NULL when every cap is NULL."""
    return max(_caps(members), default=None)


def _largest_of_the_key(members, whole):
    """MAX(cap) by the same key, which a `when` inside the sum does not narrow."""
    return max(_caps(whole), default=None)


def _plus(read, amount):
    return lambda members, whole: None if read(members, whole) is None else read(members, whole) + amount


def _half_the_rows(members, whole):
    return len(members) / 2


def _rows_less_one(members, whole):
    return len(members) - 1


def _a_sixth_of_the_caps(members, whole):
    return sum(_caps(members)) / 6


def _the_length_of_the_department(members, whole):
    return len(members[0]["dept"]) - 4


FINE = Q(DEPT, "cap", cap_of=_tightest)
S2_DATA_CAP_CASES = {
    # --- a source value per row: the group's tightest ---
    "per_row_cap_at_the_finer_level": {"data": "capped", "quotas": [FINE, Q(DIV, 2), Q((), 4)]},
    "per_row_cap_at_the_coarser_level": {
        "data": "capped",
        "quotas": [Q(DEPT, 2), Q(DIV, "cap + 1", cap_of=_plus(_tightest, 1)), Q((), 5)],
    },
    "per_row_cap_at_every_level": {
        "data": "capped",
        "quotas": [FINE, Q(DIV, "cap + 1", cap_of=_plus(_tightest, 1)), Q((), "cap + 3", cap_of=_plus(_tightest, 3))],
    },
    "per_row_cap_that_is_one_value_of_the_key": {
        "data": "capped",
        "quotas": [Q(DEPT, "LENGTH(dept) - 4", cap_of=_the_length_of_the_department), Q(DIV, 2), Q((), 4)],
    },
    # --- an aggregate of the group's rows ---
    "aggregate_cap_at_the_finer_level": {
        "data": "capped",
        "quotas": [Q(DEPT, "MAX(cap) by (div, dept)", cap_of=_largest), Q(DIV, 2), Q((), 4)],
    },
    "aggregate_cap_at_the_coarser_level": {
        "data": "capped",
        "quotas": [Q(DEPT, 1), Q(DIV, "COUNT(*) by (div) / 2", cap_of=_half_the_rows), Q((), 5)],
    },
    "aggregate_cap_at_every_level": {
        "data": "capped",
        "quotas": [
            Q(DEPT, "MAX(cap) by (div, dept)", cap_of=_largest),
            Q(DIV, "COUNT(*) by (div) / 2", cap_of=_half_the_rows),
            Q((), "SUM(cap) / 6", cap_of=_a_sixth_of_the_caps),
        ],
    },
    "aggregate_cap_under_a_when": {
        "data": "capped",
        "quotas": [Q(DEPT, "MAX(cap) by (div, dept)", when="flag", cap_of=_largest), Q(DIV, 2), Q((), 4)],
    },
    # A reducer reads every row of its key, though the sum counts only the rows the `when` keeps.
    "aggregate_cap_beside_a_when_inside_the_sum": {
        "data": "capped",
        "quotas": [Q(DEPT, "MAX(cap) by (div, dept)", when="local", cap_of=_largest_of_the_key), Q(DIV, 2), Q((), 4)],
    },
    # --- rounding: a fractional, a strict and a negated cap, as for a constant ---
    "cap_strict_and_fractional": {
        "data": "capped",
        "quotas": [
            Q(DEPT, "cap / 2.0", "<", cap_of=lambda m, w: _tightest(m, w) / 2),
            Q(DIV, "COUNT(*) by (div) / 2", "<", cap_of=_half_the_rows),
            Q((), 4),
        ],
    },
    "cap_with_a_negated_count_body": {
        "data": "capped",
        "quotas": [
            Q(DEPT, "-cap", ">=", body="-x", cap_of=lambda m, w: -_tightest(m, w)),
            Q(DIV, "-COUNT(*) by (div) / 2", ">", body="-x", cap_of=lambda m, w: -_half_the_rows(m, w)),
            Q((), 4),
        ],
    },
    "cap_zero": {"data": "capped", "quotas": [Q(DEPT, "cap - 1", cap_of=_plus(_tightest, -1)), Q(DIV, 3), Q((), 5)]},
    # --- several clauses of one level: the group's room is the least of them ---
    # The constant cap 2 is the least where the data's is 3 (S research) and the data's where it is 1 (N sales).
    "cap_beside_a_constant_cap_of_its_level": {
        "data": "capped",
        "quotas": [Q(DEPT, 2), FINE, Q(DIV, 3), Q((), 6)],
    },
    "two_data_caps_on_one_level": {
        "data": "capped",
        "quotas": [
            FINE,
            Q(DEPT, "COUNT(*) by (div, dept) - 1", cap_of=_rows_less_one),
            Q(DIV, 3),
            Q((), 9),
        ],
    },
    "two_data_caps_and_a_constant_on_one_level": {
        "data": "capped",
        "quotas": [
            Q(DEPT, 1),
            Q(DIV, "cap + 2", cap_of=_plus(_tightest, 2)),
            Q(DIV, "MAX(cap) by (div)", cap_of=_largest),
            Q(DIV, 3),
            Q((), 5),
        ],
    },
    # --- a NULL cap is no limit; the group's tightest skips a NULL ---
    "null_per_row_cap_is_no_limit": {
        "data": "capped_nulls",
        "quotas": [FINE, Q(DIV, "cap", cap_of=_tightest), Q((), "cap", cap_of=_tightest)],
    },
    "null_aggregate_cap_is_no_limit": {
        "data": "capped_nulls",
        "quotas": [
            Q(DEPT, "MAX(cap) by (div, dept)", cap_of=_largest),
            Q(DIV, "MAX(cap) by (div)", cap_of=_largest),
            Q((), "MAX(cap)", cap_of=_largest),
        ],
    },
    "null_cap_beside_a_constant_cap": {"data": "capped_nulls", "quotas": [FINE, Q(DEPT, 1), Q(DIV, 2), Q((), 4)]},
    # --- a cap with fixed rows: a row fixed to 1 takes a place of every level it is in ---
    "cap_with_fixed_rows": {
        "data": "capped",
        "quotas": [FINE, Q(DIV, "cap + 1", cap_of=_plus(_tightest, 1)), Q((), 5)],
        "fixed": [(1, [1, 6]), (0, [0])],
    },
    "cap_with_a_fixed_row_that_fills_it": {
        "data": "capped",
        "quotas": [FINE, Q(DIV, 3), Q((), 5)],
        "fixed": [(1, [3, 4])],
    },
    # --- infeasible: a cap below zero, or fixed rows beyond a cap ---
    "cap_below_zero_at_the_finer_level": {
        "data": "capped",
        "quotas": [Q(DEPT, "cap - 3", cap_of=_plus(_tightest, -3)), Q(DIV, 5), Q((), 9)],
    },
    "cap_below_zero_at_the_coarser_level": {
        "data": "capped",
        "quotas": [Q(DEPT, 2), Q(DIV, "cap - 2", cap_of=_plus(_tightest, -2)), Q((), 9)],
    },
    "aggregate_cap_below_zero": {
        "data": "capped",
        "quotas": [Q(DEPT, 2), Q(DIV, 3), Q((), "COUNT(*) / 2 - 7", cap_of=lambda m, w: len(m) / 2 - 7)],
    },
    "strict_cap_of_zero": {
        "data": "capped",
        "quotas": [Q(DEPT, "cap - 1", "<", cap_of=_plus(_tightest, -1)), Q(DIV, 5), Q((), 9)],
    },
    "fixed_rows_beyond_a_cap_at_the_finer_level": {
        "data": "capped",
        "quotas": [FINE, Q(DIV, 5), Q((), 9)],
        "fixed": [(1, [0, 1])],
    },
    "fixed_rows_beyond_a_cap_at_the_coarser_level": {
        "data": "capped",
        "quotas": [Q(DEPT, 3), Q(DIV, "cap", cap_of=_tightest), Q((), 9)],
        "fixed": [(1, [0, 3])],
    },
}

INFEASIBLE_DATA_CAP_CASES = [
    "cap_below_zero_at_the_finer_level",
    "cap_below_zero_at_the_coarser_level",
    "aggregate_cap_below_zero",
    "strict_cap_of_zero",
    "fixed_rows_beyond_a_cap_at_the_finer_level",
    "fixed_rows_beyond_a_cap_at_the_coarser_level",
]


def _dataset(case):
    return DATASETS[case.get("data", "main")]


def _names(case):
    return NAMES + [name for name in ("team", "cap") if name in _dataset(case)[0]]


def _sql_types(case):
    types = dict(COLUMNS)
    types["team"] = "VARCHAR"
    types["cap"] = "DOUBLE"
    return types


def _values(case, records=None):
    names = _names(case)
    return values_clause(records if records is not None else _dataset(case), names, _sql_types(case), sql_literal)


def _clause_sql(quota):
    keys = f" by ({', '.join(quota['keys'])})" if quota["keys"] else ""
    inner = {"local": "when flag: "}.get(quota["when"], "")
    scope = (["when flag"] if quota["when"] == "flag" else []) + (
        [f"per {', '.join(quota['per'])}"] if quota["per"] is not None else []
    )
    prefix = f"{' '.join(scope)}: " if scope else ""
    return f"{prefix}SUM({inner}{quota['body']}){keys} {quota['op']} {quota['cap']}"


def _decide_clause(case):
    clauses = [_clause_sql(quota) for quota in case["quotas"]]
    for value, ids in case.get("fixed", ()):
        clauses.extend(f"when id = {i}: x = {value}" for i in ids)
    sense = case.get("sense", "MAXIMIZE")
    tail = "" if sense == "none" else f" {sense} SUM(score * x)"
    return f"DECIDE x(BOOL) SUCH THAT {' AND '.join(clauses)}{tail}"


def _coefficient(case):
    """The per-row coefficient of x in the objective the case writes."""
    return (lambda r: 0.0) if case.get("sense") == "none" else (lambda r: r["score"])


def _inclusive(op, limit):
    """The solver path's reading of a strict comparison against an integer-valued left side."""
    if op == "<":
        return "<=", math.ceil(limit) - 1.0
    if op == ">":
        return ">=", math.floor(limit) + 1.0
    return op, limit


def _limit_of(quota, rows, members, key):
    """The limit one group of the quota has: the written constant, or what the quota reads from the data."""
    if quota["cap_of"] is None:
        return float(quota["cap"])
    whole = [r for r in rows if tuple(r[k] for k in quota["keys"]) == key]
    return quota["cap_of"]([rows[i] for i in members], whole)


def _s2_constraints(case):
    """The case as plain linear constraints over x_<row index>, one per group of each quota.

    The groups are read from the raw rows: a row is in a quota when its `when` keeps it and no key column is NULL.
    Returns (names, [(coefficients, op, rhs)]).
    """
    rows = _dataset(case)
    names = [f"x_{i}" for i in range(len(rows))]
    constraints = []
    for quota in case["quotas"]:
        keep = (lambda r: r["flag"]) if quota["when"] in ("flag", "local") else (lambda r: True)
        groups = {}
        for index, row in enumerate(rows):
            if not keep(row) or any(row[key] is None for key in quota["keys"]):
                continue
            groups.setdefault(tuple(row[key] for key in quota["keys"]), []).append(index)
        sign = -1.0 if quota["body"] == "-x" else 1.0
        for key, members in groups.items():
            limit = _limit_of(quota, rows, members, key)
            if limit is None:
                continue
            op, rhs = _inclusive(quota["op"], limit)
            constraints.append(({names[i]: sign for i in members}, op, rhs))
    for value, ids in case.get("fixed", ()):
        constraints.extend(({names[i]: 1.0}, "=", float(value)) for i in ids if i < len(rows))
    return names, constraints


def _violations(constraints, values, tolerance=1e-9):
    """The constraints an assignment (name -> value) breaks."""
    broken = []
    for coefficients, op, rhs in constraints:
        lhs = sum(coefficient * values[name] for name, coefficient in coefficients.items())
        holds = {"<=": lhs <= rhs + tolerance, ">=": lhs >= rhs - tolerance, "=": abs(lhs - rhs) <= tolerance}[op]
        if not holds:
            broken.append((sorted(coefficients), op, rhs, lhs))
    return broken


def _solve_with_oracle(oracle_solver, case):
    names, constraints = _s2_constraints(case)
    rows = _dataset(case)
    oracle_solver.create_model("direct_s2_laminar_matroid")
    for name in names:
        oracle_solver.add_variable(name, VarType.BINARY)
    for coefficients, op, rhs in constraints:
        oracle_solver.add_constraint(coefficients, op, rhs)
    coefficient = _coefficient(case)
    sense = ObjSense.MAXIMIZE if case.get("sense", "MAXIMIZE") != "MINIMIZE" else ObjSense.MINIMIZE
    oracle_solver.set_objective({names[i]: float(coefficient(r)) for i, r in enumerate(rows)}, sense)
    return constraints, oracle_solver.solve()


def _wrapped_sql(case, wrap="plain"):
    records = _dataset(case)
    names = _names(case)
    decide = _decide_clause(case)
    source = f"(VALUES {_values(case)}) t({', '.join(names)})"
    select = "SELECT id, div, dept, score, flag, x"
    ids = ", ".join(f"({r['id']})" for r in records)
    middle = records[len(records) // 2]["id"]
    if wrap == "plain":
        return f"{select} FROM (FROM {source} {decide}) q ORDER BY id"
    if wrap == "cte_materialized":
        return f"WITH src AS MATERIALIZED (SELECT * FROM {source}) {select} FROM (FROM src {decide}) q ORDER BY id"
    if wrap == "wide_source":
        # An unused wide column and an unused NULL column ride along; the plan may prune them.
        wide = f"SELECT *, repeat('p', 200) AS payload, NULL::VARCHAR AS unused FROM {source}"
        return f"{select} FROM (FROM ({wide}) w {decide}) q ORDER BY id"
    if wrap == "parent_join":
        return f"{select} FROM (FROM {source} {decide}) q JOIN (VALUES {ids}) k(id) USING (id) ORDER BY id"
    if wrap == "recombined_filters":
        # The parent filters the result two complementary ways and unions them. A filter pushed into the DECIDE input
        # would solve a smaller problem and break the assignment.
        return (
            f"WITH solved AS MATERIALIZED (FROM {source} {decide}) "
            f"{select} FROM solved WHERE id < {middle} UNION ALL "
            f"{select} FROM solved WHERE id >= {middle} ORDER BY id"
        )
    raise ValueError(wrap)


def _agree(decidb_cli, oracle_solver, case, wrap="plain", modes=("off", "require")):
    """Runs the case on the oracle and on each of `modes` (the solver path and the direct path), and checks they agree."""
    sql = _wrapped_sql(case, wrap)
    rows_data = _dataset(case)
    constraints, oracle = _solve_with_oracle(oracle_solver, case)
    by_id = {r["id"]: i for i, r in enumerate(rows_data)}
    coefficient = _coefficient(case)
    columns_out = ["id", "div", "dept", "score", "flag", "x"]

    def check_assignment(rows, columns, mode):
        assert columns == columns_out, mode
        assert [row[0] for row in rows] == [r["id"] for r in rows_data], mode
        assert all(type(row[5]) is int and row[5] in (0, 1) for row in rows), mode
        values = {f"x_{by_id[row[0]]}": row[5] for row in rows}
        assert not _violations(constraints, values), (mode, _violations(constraints, values))
        compare_solutions(
            rows,
            columns,
            oracle,
            [(r["id"], r["div"], r["dept"], float(r["score"]), r["flag"]) for r in rows_data],
            ["x"],
            coeff_fn=lambda row: {"x": float(coefficient(rows_data[by_id[row[0]]]))},
        )

    agree_with_oracle(
        decidb_cli, sql, oracle, check_assignment, modes=modes, number_range=writes_number_out_of_range(sql)
    )


@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(S2_CASES))
def test_s2_oracle_solver_and_direct_agree(decidb_cli, oracle_solver, name):
    _agree(decidb_cli, oracle_solver, S2_CASES[name])


# The cases that stand for each kind of context: the problem inside a parent query.
CONTEXT_CASES = [
    ("independent_ranks_are_wrong", "wide_source"),
    ("three_levels", "cte_materialized"),
    ("three_levels", "wide_source"),
    ("three_levels", "parent_join"),
    ("three_levels", "recombined_filters"),
    ("fixed_one_uses_every_level", "recombined_filters"),
    ("when_on_the_finer_level", "parent_join"),
    ("fixed_beyond_a_coarser_cap", "wide_source"),
]


@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.sql_subquery
@pytest.mark.correctness
@pytest.mark.parametrize("base,wrap", CONTEXT_CASES, ids=[f"{base}-{wrap}" for base, wrap in CONTEXT_CASES])
def test_s2_agrees_inside_a_parent_context(decidb_cli, oracle_solver, base, wrap):
    _agree(decidb_cli, oracle_solver, S2_CASES[base], wrap)


# ---------------------------------------------------------------------------
# What the cascade answers, checked against the answer worked out in the definition and in the plan's own comment
# ---------------------------------------------------------------------------


def _selected(decidb_cli, case):
    rows, _ = _run(decidb_cli, _wrapped_sql(case), "require")
    return {row[0] for row in rows if row[5] == 1}


@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_s2_applies_to_the_definitions_example_and_the_counter_example(decidb_cli):
    """The cases below the oracle checks apply S2 itself, not the solver, and give the answers the definition names."""
    for name in ("definition_example", "definition_example_as_written", "independent_ranks_are_wrong"):
        plan = decidb_cli.execute_raw(f"SET decide_direct_solve='require'; EXPLAIN {_wrapped_sql(S2_CASES[name])}")
        assert "S2_LAMINAR_MATROID" in plan.stdout, (name, plan.stdout, plan.stderr)
    # A (id 0) and D (id 3) are selected.
    assert _selected(decidb_cli, S2_CASES["definition_example"]) == {0, 3}
    assert _selected(decidb_cli, S2_CASES["definition_example_as_written"]) == {0, 3}
    # a and c: b is refused by the department, which leaves the division's second place to c.
    assert _selected(decidb_cli, S2_CASES["independent_ranks_are_wrong"]) == {0, 2}


# ---------------------------------------------------------------------------
# A negative remaining cap raises the infeasible error on both paths before a row leaves
# ---------------------------------------------------------------------------

INFEASIBLE_CASES = [
    "limit_below_zero_finest",
    "limit_below_zero_coarser",
    "limit_below_zero_strict",
    "merged_clauses_one_of_them_below_zero",
    "fixed_in_a_zero_cap_group_is_infeasible",
    "fixed_beyond_the_finest_cap",
    "fixed_beyond_a_coarser_cap",
    "fixed_beyond_the_global_cap",
    "fixed_beyond_the_division_through_a_row_with_no_department",
    "fixed_to_both_values_is_infeasible",
]


@pytest.mark.var_boolean
@pytest.mark.error
@pytest.mark.correctness
@pytest.mark.parametrize("name", INFEASIBLE_CASES)
def test_s2_a_negative_remaining_cap_raises_the_same_error_on_both_paths(decidb_cli, name):
    sql = _wrapped_sql(S2_CASES[name])
    errors = {}
    for mode in ("off", "require"):
        with pytest.raises(DeciDBCliError, match=r"(?i)infeasible") as raised:
            _run(decidb_cli, sql, mode)
        errors[mode] = error_class(str(raised.value))
    assert errors["off"] == errors["require"], errors
    # The error comes before the first row leaves: a parent that reads one row, or counts them, meets it too.
    for outer in (f"SELECT id FROM ({sql}) q LIMIT 1", f"SELECT COUNT(*) FROM ({sql}) q"):
        with pytest.raises(DeciDBCliError, match=r"(?i)infeasible"):
            _run(decidb_cli, outer, "require")


# ---------------------------------------------------------------------------
# Empty source, and a source where every row is outside the quotas
# ---------------------------------------------------------------------------


@pytest.mark.var_boolean
@pytest.mark.correctness
@pytest.mark.parametrize(
    "quotas",
    [
        "SUM(x) by (div, dept) <= -1 AND SUM(x) <= 2",
        "SUM(x) by (div, dept) <= 1 AND SUM(x) by (div) <= 0 AND SUM(x) <= 2",
    ],
)
def test_s2_empty_source_returns_no_rows_even_when_a_limit_is_impossible(decidb_cli, quotas):
    sql = f"""
        SELECT id, x FROM (
            FROM (SELECT 1 AS id, 'N' AS div, 'a' AS dept, 2.0 AS score WHERE FALSE) t
            DECIDE x(BOOL) SUCH THAT {quotas} MAXIMIZE SUM(score * x)
        ) q
    """
    for mode in ("off", "require"):
        assert _run(decidb_cli, sql, mode)[0] == [], mode


# ---------------------------------------------------------------------------
# Errors with no oracle model: both DeciDB paths must fail the same way
# ---------------------------------------------------------------------------

_QUOTAS = "SUM(x) by (div, dept) <= 1 AND SUM(x) by (div) <= 2 AND SUM(x) <= 3"


def _problem(values, columns, constraint=_QUOTAS, objective="SUM(score*x)"):
    return (
        f"SELECT x FROM (FROM (VALUES {values}) t({columns}) "
        f"DECIDE x(BOOL) SUCH THAT {constraint} MAXIMIZE {objective}) q"
    )


S2_ERROR_CASES = {
    "a_nan_score": _problem("(1, 'N', 'a', 5.0), (2, 'N', 'b', CAST('NaN' AS DOUBLE))", "id, div, dept, score"),
    "an_infinite_score": _problem(
        "(1, 'N', 'a', 5.0), (2, 'S', 'b', CAST('Infinity' AS DOUBLE))", "id, div, dept, score"
    ),
    "a_nan_fixed_value": _problem(
        "(1, 'N', 'a', 5.0, 1.0), (2, 'N', 'b', 4.0, CAST('NaN' AS DOUBLE))",
        "id, div, dept, score, cap",
        _QUOTAS + " AND x <= cap",
    ),
}


@pytest.mark.var_boolean
@pytest.mark.error
@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(S2_ERROR_CASES))
def test_s2_error_cases_fail_on_both_paths(decidb_cli, name):
    sql = S2_ERROR_CASES[name]
    classes = {}
    for mode in ("off", "require"):
        with pytest.raises(DeciDBCliError) as raised:
            _run(decidb_cli, sql, mode)
        classes[mode] = error_class(str(raised.value))
    assert classes["off"] == classes["require"], (name, classes)


# ---------------------------------------------------------------------------
# The class S2 leaves to the solver: `require` declines it, and `auto` answers as `off` does
# ---------------------------------------------------------------------------

DECLINED = {
    "scopes_that_do_not_nest": "SUM(x) by (div) <= 1 AND SUM(x) by (dept) <= 2",
    "a_lower_limit": "SUM(x) by (div) <= 2 AND SUM(x) >= 1",
    "a_lower_limit_from_the_data": "SUM(x) by (div) <= 2 AND SUM(x) >= flag_cap",
    "a_when_on_the_coarser_scope_only": "SUM(x) by (div, dept) <= 1 AND when flag: SUM(x) by (div) <= 2",
    "another_when_on_each_scope": "when flag: SUM(x) by (div, dept) <= 1 AND when NOT flag: SUM(x) by (div) <= 2",
}


@pytest.mark.var_boolean
@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(DECLINED))
def test_s2_leaves_what_it_cannot_prove_to_the_solver(decidb_cli, name):
    source = (
        "(VALUES (1, 'N', 'a', 5.0, TRUE, 1), (2, 'N', 'a', 4.0, TRUE, 1), (3, 'N', 'b', 3.0, FALSE, 2), "
        "(4, 'S', 'a', 2.0, TRUE, 1)) t(id, div, dept, score, flag, flag_cap)"
    )
    sql = f"SELECT id, x FROM (FROM {source} DECIDE x(BOOL) SUCH THAT {DECLINED[name]} MAXIMIZE SUM(score*x)) q ORDER BY id"
    with pytest.raises(DeciDBCliError) as declined:
        _run(decidb_cli, sql, "require")
    assert "decide_direct_solve=require" in str(declined.value), declined.value
    assert _outcome(decidb_cli, sql, "auto") == _outcome(decidb_cli, sql, "off")
    assert _outcome(decidb_cli, sql, "off")[0] == "ok"


# ---------------------------------------------------------------------------
# Caps read from the data
# ---------------------------------------------------------------------------


@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(S2_DATA_CAP_CASES))
def test_s2_data_cap_oracle_solver_and_direct_agree(decidb_cli, oracle_solver, name):
    _agree(decidb_cli, oracle_solver, S2_DATA_CAP_CASES[name])


@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize("name", ["per_row_cap_at_every_level", "aggregate_cap_at_every_level"])
def test_s2_data_cap_agrees_inside_a_parent_context(decidb_cli, oracle_solver, name):
    _agree(decidb_cli, oracle_solver, S2_DATA_CAP_CASES[name], "parent_join")


@pytest.mark.var_boolean
@pytest.mark.error
@pytest.mark.correctness
@pytest.mark.parametrize("name", INFEASIBLE_DATA_CAP_CASES)
def test_s2_a_data_cap_that_cannot_be_met_raises_the_same_error_on_both_paths(decidb_cli, name):
    sql = _wrapped_sql(S2_DATA_CAP_CASES[name])
    errors = {}
    for mode in ("off", "require"):
        with pytest.raises(DeciDBCliError, match=r"(?i)infeasible") as raised:
            _run(decidb_cli, sql, mode)
        errors[mode] = error_class(str(raised.value))
    assert errors["off"] == errors["require"], errors
    # The error comes before the first row leaves: a parent that reads one row, or counts them, meets it too.
    for outer in (f"SELECT id FROM ({sql}) q LIMIT 1", f"SELECT COUNT(*) FROM ({sql}) q"):
        with pytest.raises(DeciDBCliError, match=r"(?i)infeasible"):
            _run(decidb_cli, outer, "require")


# A NaN cap raises the solver's error, word for word, on the rows the clause reads. The NaN is on the last row, in the
# last group of every level, so a plan that raised late would let the first rows leave.
_NAN_CAP_ROWS = (
    "(1, 'N', 'a', 9.0, 2.0), (2, 'N', 'a', 8.0, 1.0), (3, 'N', 'b', 7.0, 2.0), (4, 'S', 'a', 6.0, 2.0), "
    "(5, 'S', 'b', 5.0, CAST('NaN' AS DOUBLE))"
)
NAN_CAP_QUOTAS = {
    "per_row_cap_at_the_finer_level": "SUM(x) by (div, dept) <= cap AND SUM(x) by (div) <= 2 AND SUM(x) <= 3",
    "per_row_cap_at_the_coarser_level": "SUM(x) by (div, dept) <= 1 AND SUM(x) by (div) <= cap AND SUM(x) <= 3",
    "per_row_cap_at_the_global_level": "SUM(x) by (div, dept) <= 1 AND SUM(x) by (div) <= 2 AND SUM(x) <= cap",
    "aggregate_cap_at_the_finer_level": (
        "SUM(x) by (div, dept) <= MAX(cap) by (div, dept) AND SUM(x) by (div) <= 2 AND SUM(x) <= 3"
    ),
    "aggregate_cap_at_the_coarser_level": (
        "SUM(x) by (div, dept) <= 1 AND SUM(x) by (div) <= MAX(cap) by (div) AND SUM(x) <= 3"
    ),
    "aggregate_cap_of_a_nan_sum": (
        "SUM(x) by (div, dept) <= 1 AND SUM(x) by (div) <= 2 AND SUM(x) <= SUM(cap) - SUM(cap)"
    ),
}


NAN_INPUT_OF_AN_AGGREGATE = ("aggregate_cap_at_the_finer_level", "aggregate_cap_at_the_coarser_level")


@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.error
@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(NAN_CAP_QUOTAS))
def test_s2_a_nan_cap_raises_the_solver_error_before_the_first_row_leaves(decidb_cli, name):
    rows = (
        _NAN_CAP_ROWS
        if name != "aggregate_cap_of_a_nan_sum"
        else _NAN_CAP_ROWS.replace("CAST('NaN' AS DOUBLE)", "CAST('Infinity' AS DOUBLE)")
    )
    sql = (
        f"SELECT id, x FROM (FROM (VALUES {rows}) t(id, div, dept, score, cap) "
        f"DECIDE x(BOOL) SUCH THAT {NAN_CAP_QUOTAS[name]} MAXIMIZE SUM(score*x)) q ORDER BY id"
    )
    with pytest.raises(DeciDBCliError) as solver_error:
        _run(decidb_cli, sql, "off")
    assert "NaN" in str(solver_error.value), solver_error.value
    for outer in (sql, f"SELECT id FROM ({sql}) q LIMIT 1", f"SELECT COUNT(*) FROM ({sql}) q"):
        with pytest.raises(DeciDBCliError) as direct_error:
            _run(decidb_cli, outer, "require")
        assert "decide_direct_solve=require" not in str(direct_error.value), str(direct_error.value)
        if name in NAN_INPUT_OF_AN_AGGREGATE:
            # Two checks fail: the NaN the reducer reads and the NaN it gives. Error order is not owed, so either
            # message is the solver's; each is its own text, whichever check the thread meets first.
            for error in (solver_error, direct_error):
                assert re.search(
                    r"DECIDE: the bound (cap|MAX\(cap\) by \(div(, dept)?\)) is NaN on some rows", str(error.value)
                )
        else:
            assert str(direct_error.value) == str(solver_error.value)
