"""Direct solve, checked three ways: the independent oracle, the solver path, the direct path.

Every case is one DECIDE query in the shape of a direct-solve class. The oracle builds its
own model from the raw rows (never from DeciDB's reading of the SQL). DeciDB then runs the
query twice, with `decide_direct_solve='off'` (the solver) and `'require'` (direct solve;
a query the rule does not apply to fails instead of quietly using the solver). The case passes
when all three agree:

- the oracle finds the problem feasible: both DeciDB runs return an assignment that
  satisfies every constraint and reaches the oracle's objective (rows may differ on ties);
- the oracle finds it infeasible: both DeciDB runs raise the infeasible error;
- the problem has no oracle model (bad input): both DeciDB runs raise the same class of error.

`compare_solutions` only compares objective values, so the constraints are also checked
directly: the list that builds the oracle model is evaluated on each DeciDB assignment.

Strict bounds follow the solver path (`ilp_model_builder.cpp`): on an integer-valued left
side, `< K` is `<= ceil(K) - 1` and `> K` is `>= floor(K) + 1`.

A per-group case writes `by (g)` after every reducer of its clause, the sum and the bound's, and keeps a NULL `g` out
of every group with `when g IS NOT NULL` (a NULL key is its own group otherwise, D5). Direct solve reads no `by` key
yet, so on those cases the direct path is a strict xfail (`direct_path`); the solver path is checked as usual.

A new class adds rows to the tables below. What a result comparison cannot see (which plan
ran, EXPLAIN, prepared plans, DIAGNOSE, pruning) stays in `test_direct_rule_conformance.py`.

Note: `oracle_solver` caches each result under the test node id plus a hash of the test
function's source. The case tables and the model builder live outside that source, so editing
them does not invalidate the cache; a stale value fails loudly with an objective mismatch.
After changing a case or `_s1_constraints`, remove its entries from `results/oracle_cache.json`.
"""

import itertools
import math
import re
from fractions import Fraction

import pytest

from comparison.compare import compare_solutions
from decidb_cli import DecidBCliError
from solver.types import ObjSense, VarType

from ._direct_differential import error_class
from ._direct_oracle_common import _allow_direct_path  # noqa: F401  (autouse fixture)
from ._direct_oracle_common import agree_with_oracle, direct_path, outcome as _outcome, run as _run, sql_literal
from ._direct_oracle_common import values_clause


# ---------------------------------------------------------------------------
# Data
# ---------------------------------------------------------------------------

COLUMNS = (
    ("id", "INTEGER"),
    ("g", "VARCHAR"),  # the `by` key
    ("score", "DOUBLE"),
    ("cap", "INTEGER"),  # per-row count bound
    ("cap2", "DOUBLE"),  # a second per-row count bound
    ("geq", "DOUBLE"),  # a count bound that is constant within each group, for equalities
    ("flag", "BOOLEAN"),  # the `when` condition
    ("fixed_value", "BOOLEAN"),  # a nullable `when` condition for a fixed value
    ("lo", "DOUBLE"),  # per-row bounds on x itself
    ("hi", "DOUBLE"),
    ("nlo", "DOUBLE"),  # nullable count bounds, read through COALESCE
    ("nhi", "DOUBLE"),
    ("ncap", "DOUBLE"),
    ("fb", "DOUBLE"),  # fallback for ncap
    ("sm", "TINYINT"),  # a column whose SUM and AVG a count bound may read (exact in a double)
)
NAMES = [name for name, _ in COLUMNS]


def _row(id, g, score, cap, flag, **extra):
    row = dict(
        id=id,
        g=g,
        score=score,
        cap=cap,
        flag=flag,
        fixed_value=False,
        cap2=1.0,
        geq=1.0,
        lo=0.0,
        hi=1.0,
        nlo=None,
        nhi=None,
        ncap=None,
        fb=1.0,
        sm=(id * 3) % 7 - 2,
    )
    row.update(extra)
    return row


# Groups: a = rows 0-2, b = 3-5, c = 6-7 (no row is flagged), NULL key = 8-9.
# Rows 1, 2 and 4 carry per-row bounds on x (lo/hi): row 1 is fixed to 1, row 2 to 0, row 4 to 1.
MAIN = [
    _row(0, "a", 9.0, 2, True, cap2=2.0, nhi=2.0, ncap=1.0, fb=0.5),
    _row(1, "a", 5.5, 1, True, cap2=2.0, lo=0.5, hi=2.0, nlo=1.0, fb=2.0, fixed_value=None),
    _row(2, "a", -1.0, 3, False, lo=0.0, hi=0.5, nlo=0.0, nhi=3.0, ncap=2.0),
    _row(3, "b", 8.0, 2, True, cap2=2.0, geq=2.0, lo=-1.0, hi=5.0, nlo=1.0, nhi=2.5, fixed_value=True),
    _row(4, "b", 4.0, 2, True, geq=2.0, lo=1.0, hi=1.0, nlo=1.0, nhi=2.0, ncap=2.0, fixed_value=None),
    _row(5, "b", 3.5, 1, False, geq=2.0, nhi=2.0, fb=3.0),
    _row(6, "c", -2.5, 2, False, cap2=2.0, nlo=0.0, ncap=3.0),
    _row(7, "c", 0.5, 3, False, cap2=2.0, nhi=2.0, fixed_value=True),
    _row(8, None, 6.0, 1, True, fb=2.0, fixed_value=None),
    _row(9, None, -3.0, 2, False, fb=0.5),
]

# Equal scores, so a plan that ranks with ties (RANK instead of ROW_NUMBER) selects too many rows.
TIES = [
    _row(i, g, score, 2, True)
    for i, (g, score) in enumerate(
        [("a", 5.0), ("a", 5.0), ("a", 5.0), ("b", 5.0), ("b", 0.0), ("b", 0.0), ("c", -2.0), ("c", -2.0)]
    )
]

# MAIN with row 6 flagged, so every group has a flagged row. A per-group sum with `when flag` inside it reads the
# flagged rows of each group, and a group with none is an error (D8): the per-group cases with an inner `when` run over
# these rows, and the error has a case of its own (`test_s1_an_inner_when_that_empties_a_group_is_an_error`).
MAIN_LOCAL = [dict(row, flag=True) if row["id"] == 6 else row for row in MAIN]

DATASETS = {"main": MAIN, "ties": TIES, "main_local": MAIN_LOCAL}

# Objective name -> (SQL, per-row coefficient of x). The oracle evaluates the Python function.
OBJECTIVES = {
    "score": ("SUM(score * x)", lambda r: r["score"]),
    "score_plus_id": ("SUM(score * x) + SUM(id * x)", lambda r: r["score"] + r["id"]),
    "score_minus_cap": ("SUM(score * x) - SUM(cap * x)", lambda r: r["score"] - r["cap"]),
    "negated": ("-SUM(score * x)", lambda r: -r["score"]),
    "scaled": ("2 * SUM(score * x)", lambda r: 2 * r["score"]),
    "divided_by_negative": ("SUM(score * x) / -2", lambda r: -r["score"] / 2),
    "zero_factor": ("0 * SUM(score * x)", lambda r: 0.0),
    "scaled_two_terms": ("2 * SUM(score * x) - 3 * SUM(id * x)", lambda r: 2 * r["score"] - 3 * r["id"]),
    "constant_offset": ("SUM(score * x) + 10", lambda r: r["score"]),
    "product_of_columns": ("SUM(score * id * x)", lambda r: r["score"] * r["id"]),
    "parenthesized_sum": ("SUM((score + id) * x)", lambda r: r["score"] + r["id"]),
}
NEW_OBJECTIVES = [name for name in OBJECTIVES if name not in ("score", "score_plus_id")]

# A count bound that is a column or an expression: SQL text -> its value on a row, given the number of
# rows `n` the bound's clause covers in the row's group. The oracle evaluates the Python function.
BOUND_EXPRS = {
    "cap": lambda r, n: r["cap"],
    "cap2": lambda r, n: r["cap2"],
    "geq": lambda r, n: r["geq"],
    "geq + 0": lambda r, n: r["geq"],
    "COALESCE(nlo, 1.0)": lambda r, n: 1.0 if r["nlo"] is None else r["nlo"],
    "COALESCE(nhi, 2.0)": lambda r, n: 2.0 if r["nhi"] is None else r["nhi"],
    "COALESCE(ncap, fb)": lambda r, n: r["fb"] if r["ncap"] is None else r["ncap"],
    # COUNT(*) is evaluated in exact arithmetic: DuckDB reads `0.57` as a decimal, so 100 * 0.57 is exactly 57.
    "COUNT(*)": lambda r, n: n,
    "COUNT(*) * 1": lambda r, n: n,
    "COUNT(*) / 2": lambda r, n: Fraction(n, 2),
    "COUNT(*) / 3": lambda r, n: Fraction(n, 3),
    "COUNT(*) / 4": lambda r, n: Fraction(n, 4),
    "COUNT(*) // 2": lambda r, n: n // 2,
    "COUNT(*) - 1": lambda r, n: n - 1,
    "COUNT(*) - 20": lambda r, n: n - 20,
    "COUNT(*) * 0.4": lambda r, n: Fraction(n) * Fraction("0.4"),
    "2 * COUNT(*) / 3 + 1": lambda r, n: Fraction(2 * n, 3) + 1,
    "-COUNT(*) + 6": lambda r, n: -n + 6,
    "COUNT(*) / 4 * 3": lambda r, n: Fraction(n, 4) * 3,
}

# A count bound read from a column through an aggregate (MIN, MAX, COUNT of any numeric column; SUM and AVG of the
# TINYINT column `sm`): SQL text -> its value given the rows the bound's clause keeps in the group (every row of the
# group under a `when` inside the sum, the flagged ones under a `when` in front of the clause).
_REDUCER_BASES = {
    "MIN(cap)": lambda rows: min(r["cap"] for r in rows),
    "MAX(cap)": lambda rows: max(r["cap"] for r in rows),
    "MIN(cap2)": lambda rows: min(r["cap2"] for r in rows),
    "MAX(cap2) / 2": lambda rows: Fraction(str(max(r["cap2"] for r in rows))) / 2,
    "COUNT(cap)": lambda rows: len(rows),
    "COUNT(cap2) / 2": lambda rows: Fraction(len(rows), 2),
    "SUM(sm)": lambda rows: sum(r["sm"] for r in rows),
    "AVG(sm)": lambda rows: Fraction(sum(r["sm"] for r in rows), len(rows)),
    "SUM(sm) / 2 + 1": lambda rows: Fraction(sum(r["sm"] for r in rows), 2) + 1,
    "MAX(cap) - MIN(cap)": lambda rows: max(r["cap"] for r in rows) - min(r["cap"] for r in rows),
    "AVG(sm) * 0.5": lambda rows: Fraction(sum(r["sm"] for r in rows), len(rows)) * Fraction("0.5"),
}
_REDUCER_EXTRAS = {
    "AVG(sm) + 2": lambda rows: Fraction(sum(r["sm"] for r in rows), len(rows)) + 2,
}
# A reducer with a `when` of its own reads the rows of the clause's group that the `when` keeps. A count with a
# condition is a sum of 1 (COUNT takes no `when`, D15).
_REDUCER_OWN_WHEN = {
    "SUM(when cap > 1: 1)": lambda rows: sum(1 for r in rows if r["cap"] > 1),
    "MIN(when cap > 1: cap)": lambda rows: min(r["cap"] for r in rows if r["cap"] > 1),
    "MAX(when flag: cap)": lambda rows: max(r["cap"] for r in rows if r["flag"]),
    "AVG(when cap > 1: sm)": lambda rows: Fraction(
        sum(r["sm"] for r in rows if r["cap"] > 1), sum(1 for r in rows if r["cap"] > 1)
    ),
    "SUM(when id > 4: 1)": lambda rows: sum(1 for r in rows if r["id"] > 4),
    # Row 0 has id 0, so the `when` leaves it out of the count.
    "SUM(when id > 0: 1) / 2": lambda rows: Fraction(sum(1 for r in rows if r["id"] > 0), 2),
    # Two sums that differ only in their `when` are two window columns.
    "SUM(when cap > 1: 1) - SUM(when cap < 3: 1)": lambda rows: sum(1 for r in rows if r["cap"] > 1)
    - sum(1 for r in rows if r["cap"] < 3),
}
REDUCER_BOUND_EXPRS = {**_REDUCER_BASES, **_REDUCER_EXTRAS, **_REDUCER_OWN_WHEN}


def _dataset(case):
    name = case.get("data", "main")
    inner_when = any((bound[2] if len(bound) > 2 else case.get("when")) == "local" for bound in case.get("bounds", ()))
    if name == "main" and case.get("per") and inner_when:
        return MAIN_LOCAL
    return DATASETS[name]


def _values(records, names=NAMES):
    return values_clause(records, names, dict(COLUMNS), sql_literal)


# ---------------------------------------------------------------------------
# S1: choose rows by score under count bounds on one BOOL decision
# ---------------------------------------------------------------------------
#
# A case is a dict with these optional keys:
#   data      "main" (default) or "ties"
#   bounds    [(op, bound[, scope])]   SUM(x) op bound. The bound is a number or a key of BOUND_EXPRS.
#                                      scope overrides the case's `when` for this bound.
#   count_body                         what SUM(...) adds up, "x" by default; other spellings of one x
#   per       True                     count per group: `by (g)` after every reducer, NULL keys left out
#   when      "top"                    `when flag:` in front of the bound: rows outside it are out of the problem
#             "local"                  `when flag` inside the sum: only flagged rows are counted,
#                                      but every row of the group still states a bound
#             "both"                   `when flag:` in front and `when score > 5` inside the sum: the flagged rows
#                                      state the bound, and those with a score above 5 are counted
#   fixed     [(value, [ids])]         when id = i: x = value
#   fixedwhen True                     when fixed_value: x = 1 (the column is NULL on some rows: those are not fixed)
#   flagfixed_below N                  when id < N: x = flag
#   rowbounds [(op, number, ids|None[, lhs])]  lhs op number on the given ids (None: every row, no `when`)
#   noop      [SQL]                    clauses that restate the BOOL domain and change nothing
#   colfixed  True                     x >= lo AND x <= hi, the bounds read from each row
#   objective key of OBJECTIVES (default "score"), or "none" for a query with no objective (every score is 0)
#   sense     "MAXIMIZE" (default) or "MINIMIZE"; unused when there is no objective

S1_CASES = {
    # --- count bounds, constant ---
    "global_upper_max": {"bounds": [("<=", 2)]},
    "global_lower_min": {"bounds": [(">=", 3)], "sense": "MINIMIZE"},
    "global_fractional_upper": {"bounds": [("<=", 2.5)]},
    "global_upper_at_2_pow_53": {"bounds": [("<=", 9007199254740992)]},
    "global_interval": {"bounds": [(">=", 4), ("<=", 6)]},
    "global_exact": {"bounds": [("=", 3)]},
    "per_interval": {"bounds": [(">=", 1), ("<=", 2)], "per": True},
    "per_exact_min": {"bounds": [("=", 1)], "per": True, "sense": "MINIMIZE"},
    "top_when_upper": {"bounds": [("<=", 1)], "when": "top"},
    "top_when_per_exact": {"bounds": [(">=", 1), ("<=", 1)], "per": True, "when": "top"},
    "count_body_2x_minus_x": {"bounds": [("<=", 2)], "count_body": "2*x - x"},
    "count_body_constant_unit": {"bounds": [("<=", 2)], "count_body": "(1+0)*x"},
    # --- a `when` inside the sum ---
    "local_when_interval": {"bounds": [(">=", 1), ("<=", 1)], "per": True, "when": "local"},
    "local_when_upper_global": {"bounds": [("<=", 2)], "when": "local"},
    "both_whens_upper_global": {"bounds": [("<=", 1)], "when": "both"},
    # Row 5 is not flagged, but its cap (1) still limits the flagged rows of group b.
    "local_when_source_bound_reads_unflagged_rows": {"bounds": [("<=", "cap")], "per": True, "when": "local"},
    "local_when_source_bound_global": {"bounds": [("<=", "cap")], "when": "local"},
    "local_when_source_lower_bound": {"bounds": [(">=", "cap2")], "per": True, "when": "local", "sense": "MINIMIZE"},
    "local_when_equality_per": {"bounds": [("=", "geq")], "per": True, "when": "local"},
    "mixed_top_and_local_when": {"bounds": [("<=", "cap", "top"), ("<=", "cap2", "local")], "per": True},
    # --- source-valued bounds ---
    "source_bound_per": {"bounds": [("<=", "cap")], "per": True},
    "source_bound_per_top_when": {"bounds": [(">=", 1), ("<=", "cap")], "per": True, "when": "top"},
    "source_bound_global_min": {"bounds": [("<=", "cap")], "sense": "MINIMIZE"},
    "source_strict_upper_per": {"bounds": [("<", "cap")], "per": True},
    "source_equality_constant_in_group": {"bounds": [("=", "geq")], "per": True},
    # Each equality has its own check that the bound does not vary within a group.
    "two_source_equalities_per": {"bounds": [("=", "geq"), ("=", "geq + 0")], "per": True},
    "coalesce_bound_per": {"bounds": [("<=", "COALESCE(ncap, fb)")], "per": True},
    "coalesce_bound_global": {"bounds": [("<=", "COALESCE(ncap, fb)")], "sense": "MINIMIZE"},
    # --- several source-valued bounds ---
    "coalesce_interval_per": {"bounds": [(">=", "COALESCE(nlo, 1.0)"), ("<=", "COALESCE(nhi, 2.0)")], "per": True},
    "coalesce_interval_top_when": {
        "bounds": [(">=", "COALESCE(nlo, 1.0)"), ("<=", "COALESCE(nhi, 2.0)")],
        "per": True,
        "when": "top",
    },
    "coalesce_interval_local_when": {
        "bounds": [(">=", "COALESCE(nlo, 1.0)"), ("<=", "COALESCE(nhi, 2.0)")],
        "per": True,
        "when": "local",
    },
    "two_upper_source_bounds": {"bounds": [("<=", "cap"), ("<=", "cap2")], "per": True},
    "three_global_source_bounds": {
        "bounds": [(">=", "COALESCE(nlo, 1.0)"), ("<=", "COALESCE(nhi, 2.0)"), ("<=", "cap2")]
    },
    "source_bounds_with_fixed": {
        "bounds": [(">=", "COALESCE(nlo, 1.0)"), ("<=", "cap")],
        "per": True,
        "fixed": [(1, [5])],
    },
    # --- bounds that count rows: COUNT(*) is the rows the clause covers in each group ---
    "count_star_half_global": {"bounds": [("<=", "COUNT(*) / 2")]},
    "count_star_half_per": {"bounds": [("<=", "COUNT(*) / 2")], "per": True},
    "count_star_minus_one_lower_per": {"bounds": [(">=", "COUNT(*) - 1")], "per": True, "sense": "MINIMIZE"},
    "count_star_equality_per": {"bounds": [("=", "COUNT(*) - 1")], "per": True},
    "count_star_interval_global": {"bounds": [(">=", "COUNT(*) / 4"), ("<=", "COUNT(*) / 2")]},
    # Under a `when` in front of the clause the count covers only the flagged rows; under a `when` inside the sum it
    # covers every row of the group.
    "count_star_top_when_per": {"bounds": [("<=", "COUNT(*) / 2")], "per": True, "when": "top"},
    "count_star_local_when_per": {"bounds": [("<=", "COUNT(*) / 2")], "per": True, "when": "local"},
    "count_star_with_source_bound": {"bounds": [("<=", "COUNT(*)"), ("<=", "cap")], "per": True},
    # Told apart from the case above only when the count covers every row of the group (3 rows, 2 flagged).
    "count_star_minus_one_local_when_per": {"bounds": [("<=", "COUNT(*) - 1")], "per": True, "when": "local"},
    "count_star_floor_division_per": {"bounds": [("<=", "COUNT(*) // 2")], "per": True},
    "count_star_decimal_factor_per": {"bounds": [("<=", "COUNT(*) * 0.4")], "per": True},
    "count_star_affine_lower_per": {"bounds": [(">=", "2 * COUNT(*) / 3 + 1")], "per": True, "sense": "MINIMIZE"},
    "count_star_negated_lower_global": {"bounds": [(">=", "-COUNT(*) + 6")], "sense": "MINIMIZE"},
    "count_star_nested_division_per": {"bounds": [("<=", "COUNT(*) / 4 * 3")], "per": True},
    "count_star_third_to_half_per": {
        "bounds": [(">=", "COUNT(*) / 3"), ("<=", "COUNT(*) / 2")],
        "per": True,
        "sense": "MINIMIZE",
    },
    "count_star_interval_local_when_per": {
        "bounds": [(">=", "COUNT(*) / 2"), ("<=", "COUNT(*) - 1")],
        "per": True,
        "when": "local",
    },
    "count_star_with_fixed": {"bounds": [("<=", "COUNT(*) / 2")], "per": True, "fixed": [(1, [2]), (0, [0])]},
    "count_star_with_shifted_scores": {
        "bounds": [("<=", "COUNT(*) / 2")],
        "per": True,
        "objective": "score_minus_cap",
        "sense": "MINIMIZE",
    },
    # --- fixed values and per-row bounds on x ---
    "fixed_with_group_bounds": {"bounds": [("<=", 2)], "per": True, "fixed": [(1, [4]), (0, [0, 8])]},
    # A count bound that always holds beside fixed values: the plan checks only that no row is fixed to both values.
    "fixed_with_a_bound_that_always_holds": {"bounds": [(">=", 0)], "fixed": [(1, [4]), (0, [0, 8])]},
    "fixed_with_source_bound": {"bounds": [(">=", 1), ("<=", "cap")], "per": True, "fixed": [(1, [2]), (0, [1])]},
    "fixed_by_nullable_boolean_column": {"bounds": [("<=", 3)], "fixedwhen": True},
    "fixed_to_boolean_column": {"bounds": [("<=", 5)], "flagfixed_below": 5},
    "fixed_written_as_unit_body": {"bounds": [("<=", 2)], "rowbounds": [("<=", 0, [1], "1*x")]},
    "domain_restatements": {
        "bounds": [("<=", 3)],
        "noop": ["x <= 1", "x >= 0", "x < 2", "x > -1", "x <> 2", "x BETWEEN 0 AND 1"],
    },
    "rowbounds_with_groups": {
        "bounds": [(">=", 1), ("<=", 2)],
        "per": True,
        "rowbounds": [("=", 1, [2]), ("<=", 0.5, [0]), ("<=", 1, None)],
        "sense": "MINIMIZE",
    },
    "column_fixed_global": {"bounds": [("<=", 3)], "colfixed": True},
    "column_fixed_global_min": {"bounds": [("<=", 3)], "colfixed": True, "sense": "MINIMIZE"},
    "column_fixed_per_interval": {"bounds": [(">=", 1), ("<=", 2)], "per": True, "colfixed": True},
    # --- tied scores: a plan that ranks with ties selects too many rows ---
    "ties_top_k": {"data": "ties", "bounds": [("<=", 2)]},
    "ties_one_per_group": {"data": "ties", "bounds": [(">=", 1), ("<=", 1)], "per": True},
    "ties_minimize_with_lower_bound": {"data": "ties", "bounds": [(">=", 3), ("<=", 5)], "sense": "MINIMIZE"},
}

_OPNAMES = {"<=": "le", "<": "lt", ">=": "ge", ">": "gt", "=": "eq", "<>": "ne"}


def _tag(value):
    return str(value).replace("-", "m").replace(".", "p")


# The count limit: every comparison against whole and fractional limits, including limits no count can meet, except a
# strict upper bound at 0 and at -0.5. A lower bound is minimized so that it binds.
for _op, _limit in itertools.product(("<=", "<", ">=", ">", "="), (-0.5, 0, 1.5, 2)):
    if _op == "<" and _limit in (-0.5, 0):
        continue
    S1_CASES[f"limit_{_OPNAMES[_op]}_{_tag(_limit)}"] = {
        "bounds": [(_op, _limit)],
        "sense": "MINIMIZE" if _op in (">", ">=") else "MAXIMIZE",
    }

# A per-row bound on x with a `when` is plain arithmetic on a Boolean: the row is left free, fixed to 0 or 1, or
# infeasible. Without a `when` the solver reads some spellings as a bound on the variable instead; those stay on the
# solver (see the conformance fixture), and the rest must agree with arithmetic.
# Every comparison against a bound that counts rows, per group; a lower bound is minimized so that it binds.
for _op, _bound in itertools.product(("<=", "<", ">=", ">", "="), ("COUNT(*) / 2", "COUNT(*) - 1")):
    S1_CASES[f"count_star_{_OPNAMES[_op]}_{_tag(_bound.replace('COUNT(*)', 'n').replace(' ', ''))}"] = {
        "bounds": [(_op, _bound)],
        "per": True,
        "sense": "MINIMIZE" if _op in (">", ">=") else "MAXIMIZE",
    }

# Per comparison: the constants checked with a `when`, and the constants checked without one. Without a `when` the solver
# reads a spelling that cannot hold as a bound on the variable itself, and S1 leaves those on the solver (see the
# conformance fixture), so only spellings S1 admits are here.
_ROWBOUND_CONSTANTS = {
    "<=": (("0", "0.5", "1", "1.5", "2"), ("1", "1.5", "2")),
    "<": (("0", "0.5", "1", "1.5", "2"), ("1", "1.5", "2")),
    ">=": (("-1", "-0.5", "0", "0.5", "1", "1.5", "2"), ("-1", "-0.5", "0")),
    ">": (("-1", "-0.5", "0", "0.5", "1", "1.5"), ("-1", "-0.5")),
    "=": (("0", "1", "2"), ()),
    "<>": (("-1", "-0.5", "0", "0.5", "1", "1.5", "2"), ("-1", "-0.5", "0.5", "1", "1.5", "2")),
}
for _op, (_when_constants, _everywhere_constants) in _ROWBOUND_CONSTANTS.items():
    for _constant in _when_constants:
        S1_CASES[f"x_{_OPNAMES[_op]}_{_tag(_constant)}_when"] = {
            "bounds": [("<=", 3)],
            "rowbounds": [(_op, float(_constant), [3])],
        }
    for _constant in _everywhere_constants:
        S1_CASES[f"x_{_OPNAMES[_op]}_{_tag(_constant)}_everywhere"] = {
            "bounds": [("<=", 5)],
            "rowbounds": [(_op, float(_constant), None)],
        }

# No objective: every score is 0, so the problem is feasibility under the count bounds. Each bound shape meets every
# scope, so a shape that only fails under one scope shows up. The oracle minimizes a zero objective.
_NO_OBJECTIVE_BOUNDS = {
    "ge_whole": [(">=", 2)],
    "ge_fraction": [(">=", 1.5)],
    "gt_fraction": [(">", 1.5)],
    "eq_whole": [("=", 2)],
    "interval": [(">=", 1), ("<=", 2)],
    "impossible_lower": [(">=", 5)],
    "source_lower": [(">=", "cap2")],
    "source_equality": [("=", "geq")],
    "coalesce_interval": [(">=", "COALESCE(nlo, 1.0)"), ("<=", "COALESCE(nhi, 2.0)")],
    "count_star_lower": [(">=", "COUNT(*) / 2")],
}
_NO_OBJECTIVE_SCOPES = {
    "global": {},
    "per": {"per": True},
    "top_when": {"when": "top"},
    "top_when_per": {"when": "top", "per": True},
    "local_when": {"when": "local"},
    "local_when_per": {"when": "local", "per": True},
}
# A shape that meets only some scopes.
_NO_OBJECTIVE_SHAPE_SCOPES = {"impossible_lower": ("global", "top_when", "local_when")}
for _shape, _scope in itertools.product(_NO_OBJECTIVE_BOUNDS, _NO_OBJECTIVE_SCOPES):
    if _scope not in _NO_OBJECTIVE_SHAPE_SCOPES.get(_shape, _NO_OBJECTIVE_SCOPES):
        continue
    if _shape == "source_equality" and not _NO_OBJECTIVE_SCOPES[_scope].get("per"):
        continue  # the equality bound is constant within a group only, so a global scope sees it vary and raises
    S1_CASES[f"no_objective_{_shape}_{_scope}"] = {
        "bounds": _NO_OBJECTIVE_BOUNDS[_shape],
        "objective": "none",
        **_NO_OBJECTIVE_SCOPES[_scope],
    }
S1_CASES["no_objective_with_fixed_per"] = {
    "bounds": [(">=", 1), ("<=", 2)],
    "per": True,
    "fixed": [(1, [4]), (0, [0, 8])],
    "objective": "none",
}
S1_CASES["no_objective_column_fixed_global"] = {"bounds": [(">=", 2)], "colfixed": True, "objective": "none"}
S1_CASES["no_objective_ties_dataset"] = {"data": "ties", "bounds": [(">=", 3)], "objective": "none"}

# One representative objective shape per sense, over an interval so that both senses bind. Two shapes are checked under
# MAXIMIZE only.
_MAXIMIZE_ONLY = ("product_of_columns", "scaled")
for _name in NEW_OBJECTIVES:
    for _sense in ("MAXIMIZE",) if _name in _MAXIMIZE_ONLY else ("MAXIMIZE", "MINIMIZE"):
        S1_CASES[f"objective_{_name}_{_sense.lower()[:3]}"] = {
            "bounds": [(">=", 2), ("<=", 4)],
            "objective": _name,
            "sense": _sense,
        }


_REDUCER_CALL = re.compile(r"\b(COUNT|MIN|MAX|SUM|AVG)\([^()]*\)")


def _by_group(expression):
    """`expression` with `by (g)` after each reducer call, so each reads its row's group."""
    return _REDUCER_CALL.sub(lambda call: f"{call.group(0)} by (g)", expression)


def _decide_clause(case):
    default_scope = case.get("when")
    per = case.get("per")
    body = case.get("count_body", "x")
    clauses = []
    for bound in case.get("bounds", ()):
        op, limit = bound[0], str(bound[1])
        scope = bound[2] if len(bound) > 2 else default_scope
        inner = {"local": "flag", "both": "score > 5"}.get(scope)
        total = f"SUM(when {inner}: {body})" if inner else f"SUM({body})"
        if per:
            total, limit = _by_group(total), _by_group(limit)
        # A NULL key is a group of its own (D5); the cases leave those rows out, as the oracle does.
        kept = (["flag"] if scope in ("top", "both") else []) + (["g IS NOT NULL"] if per else [])
        prefix = f"when {' AND '.join(kept)}: " if kept else ""
        clauses.append(f"{prefix}{total} {op} {limit}")
    for value, ids in case.get("fixed", ()):
        clauses.extend(f"when id = {i}: x = {value}" for i in ids)
    if case.get("fixedwhen"):
        clauses.append("when fixed_value: x = 1")
    if case.get("flagfixed_below") is not None:
        clauses.append(f"when id < {case['flagfixed_below']}: x = flag")
    for rowbound in case.get("rowbounds", ()):
        op, constant, ids = rowbound[:3]
        lhs = rowbound[3] if len(rowbound) > 3 else "x"
        if ids is None:
            clauses.append(f"{lhs} {op} {constant}")
        else:
            clauses.extend(f"when id = {i}: {lhs} {op} {constant}" for i in ids)
    clauses.extend(case.get("noop", ()))
    if case.get("colfixed"):
        clauses.extend(["x >= lo", "x <= hi"])
    objective = case.get("objective", "score")
    tail = "" if objective == "none" else f" {case.get('sense', 'MAXIMIZE')} {OBJECTIVES[objective][0]}"
    return f"DECIDE x(BOOL) SUCH THAT {' AND '.join(clauses)}{tail}"


def _coefficient(case):
    """The per-row coefficient of x in the case's objective; 0 for a query with no objective."""
    objective = case.get("objective", "score")
    return (lambda r: 0.0) if objective == "none" else OBJECTIVES[objective][1]


# The DECIDE query is wrapped in a context. Each context returns every row (id, g, score, x),
# so the answer is still checked against the oracle; what changes is the plan around DECIDE.
def _wrapped_sql(case, wrap):
    records = _dataset(case)
    cols = ", ".join(NAMES)
    decide = _decide_clause(case)
    source = f"(VALUES {_values(records)}) t({cols})"
    select = "SELECT id, g, score, x"
    without_score = [n for n in NAMES if n != "score"]
    base = f"(VALUES {_values(records, without_score)}) t({', '.join(without_score)})"
    scores = f"(VALUES {_values(records, ['id', 'score'])}) s(id, score)"
    ids = ", ".join(f"({r['id']})" for r in records)
    middle = records[len(records) // 2]["id"]
    if wrap == "plain":
        return f"{select} FROM (FROM {source} {decide}) q ORDER BY id"
    if wrap == "cte_materialized":
        return f"WITH src AS MATERIALIZED (SELECT * FROM {source}) {select} FROM (FROM src {decide}) q ORDER BY id"
    if wrap == "cte_inlined":
        return f"WITH src AS (SELECT * FROM {source}) {select} FROM (FROM src {decide}) q ORDER BY id"
    if wrap == "wide_source":
        # An unused wide column and an unused NULL column ride along; the plan may prune them.
        wide = f"SELECT *, repeat('p', 200) AS payload, NULL::VARCHAR AS unused FROM {source}"
        return f"{select} FROM (FROM ({wide}) w {decide}) q ORDER BY id"
    if wrap == "joined_source":
        picked = ", ".join("s.score" if n == "score" else f"t.{n}" for n in NAMES)
        joined = f"SELECT {picked} FROM {base} JOIN {scores} ON s.id = t.id"
        return f"{select} FROM (FROM ({joined}) j {decide}) q ORDER BY id"
    if wrap == "correlated_source":
        picked = ", ".join(f"t.{n}" for n in without_score)
        lookup = f"(SELECT s.score FROM {scores} WHERE s.id = t.id) AS score"
        return f"{select} FROM (FROM (SELECT {picked}, {lookup} FROM {base}) u {decide}) q ORDER BY id"
    if wrap == "parent_join":
        return f"{select} FROM (FROM {source} {decide}) q JOIN (VALUES {ids}) k(id) USING (id) ORDER BY id"
    if wrap == "recombined_filters":
        # The parent filters the result two complementary ways and unions them. A filter pushed
        # into the DECIDE input would solve a smaller problem and break the assignment.
        return (
            f"WITH solved AS MATERIALIZED (FROM {source} {decide}) "
            f"{select} FROM solved WHERE id < {middle} UNION ALL "
            f"{select} FROM solved WHERE id >= {middle} ORDER BY id"
        )
    raise ValueError(wrap)


def _inclusive(op, limit):
    """The solver path's reading of a strict comparison against an integer-valued left side."""
    if op == "<":
        return "<=", math.ceil(limit) - 1.0
    if op == ">":
        return ">=", math.floor(limit) + 1.0
    return op, limit


def _row_bound(name, op, constant):
    """`x op constant` on one row, as constraints on the 0/1 variable."""
    op, constant = _inclusive(op, float(constant))
    if op == "<>":
        # x <> c removes c from {0, 1}.
        return {0.0: [({name: 1.0}, ">=", 1.0)], 1.0: [({name: 1.0}, "<=", 0.0)]}.get(constant, [])
    return [({name: 1.0}, op, constant)]


def _s1_constraints(case):
    """The case as plain linear constraints over x_<row index>, built from the raw rows.

    A source-valued bound becomes one constraint per row that states it, so the oracle never
    takes the group MIN or MAX the rule uses. Returns (names, [(coefficients, op, rhs)]).
    """
    rows = _dataset(case)
    names = [f"x_{i}" for i in range(len(rows))]
    constraints = []

    def limit_of(bound, row, covered, group_rows=()):
        if isinstance(bound, (int, float)):
            return float(bound)
        if bound in REDUCER_BOUND_EXPRS:
            return float(REDUCER_BOUND_EXPRS[bound](group_rows))
        return float(BOUND_EXPRS[bound](row, covered))

    for bound in case.get("bounds", ()):
        op, limit = bound[0], bound[1]
        scope = bound[2] if len(bound) > 2 else case.get("when")
        groups = {}
        for index, row in enumerate(rows):
            if case.get("per") and row["g"] is None:
                continue  # a NULL key is out of every group
            groups.setdefault(row["g"] if case.get("per") else None, []).append(index)
        for members in groups.values():
            if scope == "top":
                members = [i for i in members if rows[i]["flag"]]
                counted = members
            elif scope == "local":
                counted = [i for i in members if rows[i]["flag"]]
            elif scope == "both":
                members = [i for i in members if rows[i]["flag"]]
                counted = [i for i in members if rows[i]["score"] > 5]
            else:
                counted = members
            if not counted:
                continue  # nothing to count: the group is left free
            # A count body `a*x` adds `a` per selected row.
            coefficients = {names[i]: float(case.get("body_a", 1)) for i in counted}
            # `COUNT(*)` counts the rows that state the bound: every row of the group under a `when` inside the sum,
            # the flagged ones under a `when` in front of the clause.
            group_rows = [rows[j] for j in members]
            limits = {limit_of(limit, rows[i], len(members), group_rows) for i in members}
            sum_op = op
            for rhs in sorted(limits):
                sum_op, rhs = _inclusive(op, rhs)
                constraints.append((coefficients, sum_op, rhs))

    for value, ids in case.get("fixed", ()):
        for i in (i for i in ids if i < len(rows)):  # an id no row has is not bound
            constraints.append(({names[i]: 1.0}, "=", float(value)))
    if case.get("fixedwhen"):
        constraints.extend(({names[i]: 1.0}, "=", 1.0) for i, row in enumerate(rows) if row["fixed_value"] is True)
    if case.get("flagfixed_below") is not None:
        constraints.extend(
            ({names[i]: 1.0}, "=", 1.0 if row["flag"] else 0.0)
            for i, row in enumerate(rows)
            if row["id"] < case["flagfixed_below"]
        )
    for rowbound in case.get("rowbounds", ()):
        op, constant, ids = rowbound[:3]
        for i in range(len(rows)) if ids is None else (i for i in ids if i < len(rows)):
            constraints.extend(_row_bound(names[i], op, constant))
    if case.get("colfixed"):
        for i, row in enumerate(rows):
            constraints.append(({names[i]: 1.0}, ">=", float(row["lo"])))
            constraints.append(({names[i]: 1.0}, "<=", float(row["hi"])))
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
    names, constraints = _s1_constraints(case)
    rows = _dataset(case)
    oracle_solver.create_model("direct_s1_uniform_matroid")
    for name in names:
        oracle_solver.add_variable(name, VarType.BINARY)
    for coefficients, op, rhs in constraints:
        oracle_solver.add_constraint(coefficients, op, rhs)
    coefficient = _coefficient(case)
    sense = ObjSense.MAXIMIZE if case.get("sense", "MAXIMIZE") == "MAXIMIZE" else ObjSense.MINIMIZE
    oracle_solver.set_objective({names[i]: float(coefficient(r)) for i, r in enumerate(rows)}, sense)
    return constraints, oracle_solver.solve()


def _agree(decidb_cli, oracle_solver, case, wrap="plain"):
    """Runs the case on the oracle, the solver path and the direct path, and checks they agree."""
    sql = _wrapped_sql(case, wrap)
    rows_data = _dataset(case)
    constraints, oracle = _solve_with_oracle(oracle_solver, case)
    by_id = {r["id"]: i for i, r in enumerate(rows_data)}
    coefficient = _coefficient(case)

    def check_assignment(rows, columns, mode):
        assert columns == ["id", "g", "score", "x"], mode
        assert [row[0] for row in rows] == [r["id"] for r in rows_data], mode
        assert all(type(row[3]) is int and row[3] in (0, 1) for row in rows), mode
        values = {f"x_{by_id[row[0]]}": row[3] for row in rows}
        assert not _violations(constraints, values), (mode, _violations(constraints, values))
        compare_solutions(
            rows,
            columns,
            oracle,
            [(r["id"], r["g"], float(r["score"])) for r in rows_data],
            ["x"],
            coeff_fn=lambda row: {"x": float(coefficient(rows_data[by_id[row[0]]]))},
        )

    agree_with_oracle(decidb_cli, sql, oracle, check_assignment)


@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(S1_CASES))
def test_s1_oracle_solver_and_direct_agree(decidb_cli, oracle_solver, name):
    _agree(decidb_cli, oracle_solver, S1_CASES[name])


# A strict bound against a fraction, with no `when`, is a bound on the variable that the solver shifts to a whole number
# (`x > 0.5` is `x >= 1`, `x < 1.5` is `x <= 1`, `x < 0.5` is `x <= 0`, `x > -0.5` is `x >= 0`). S1 shifts it the
# same way, so the oracle, `off`, `require` and `auto` all give the same answer, and `auto` takes the direct path.
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize("op,constant", [("<", 1.5), (">", -0.5)])
@pytest.mark.parametrize("sense", ["MAXIMIZE", "MINIMIZE"])
@pytest.mark.parametrize("limit", [4, 50])
def test_s1_strict_bound_against_a_fraction_agrees_under_every_mode(
    decidb_cli, oracle_solver, op, constant, sense, limit
):
    case = {"bounds": [("<=", limit)], "rowbounds": [(op, constant, None)], "sense": sense}
    _agree(decidb_cli, oracle_solver, case)
    sql = _wrapped_sql(case, "plain")

    def outcome(mode):
        try:
            return "ok", _run(decidb_cli, sql, mode)
        except DecidBCliError as error:
            return "error", error_class(str(error))

    assert outcome("auto") == outcome("require")
    assert outcome("auto")[0] == outcome("off")[0]
    plan = decidb_cli.execute_raw(f"SET decide_direct_solve='auto'; EXPLAIN {sql}").stdout
    assert "S1_UNIFORM_MATROID" in plan, plan


# A bound on x with no `when` that always holds changes nothing: the solver seeds a Boolean's box at [0, 1] and a written
# bound only narrows it, so `x >= -3` leaves both values allowed. S1 reads it that way, so S1 applies to the query
# (`require` would fail if it did not) and gives the oracle's answer.
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize("op,constant", [(">=", "-3"), (">", "-3"), (">=", "-1"), (">=", "-0.5")])
@pytest.mark.parametrize("sense", ["MAXIMIZE", "MINIMIZE"])
def test_s1_applies_when_a_bound_always_holds_in_require_mode(decidb_cli, oracle_solver, op, constant, sense):
    limit = ("<=", 3) if sense == "MAXIMIZE" else (">=", 2)
    case = {"bounds": [limit], "rowbounds": [(op, constant, None)], "sense": sense}
    _agree(decidb_cli, oracle_solver, case)
    sql = _wrapped_sql(case, "plain")
    plan = decidb_cli.execute_raw(f"SET decide_direct_solve='require'; EXPLAIN {sql}").stdout
    assert "S1_UNIFORM_MATROID" in plan, plan
    assert _outcome(decidb_cli, sql, "require") == _outcome(decidb_cli, sql, "off")


# A `when` inside a per-group sum that leaves a group with no row is an error (D8), where the old `SUM(x) WHEN flag
# PER g` left the group free. Group c of MAIN has no flagged row; the error names it. The same holds for a bound's own
# reducer. Direct solve reads no `by` key yet, so the solver path alone is checked.
@pytest.mark.var_boolean
@pytest.mark.error
@pytest.mark.correctness
@pytest.mark.parametrize(
    "bounds",
    [
        pytest.param([("=", "geq")], id="sum"),
        pytest.param([("<=", "MIN(when flag: cap)", "plain")], id="bound_reducer"),
    ],
)
def test_s1_an_inner_when_that_empties_a_group_is_an_error(decidb_cli, bounds):
    case = {"bounds": bounds, "per": True, "when": "local", "data": "main"}
    sql = (
        f"SELECT id, x FROM (FROM (VALUES {_values(MAIN)}) t({', '.join(NAMES)}) {_decide_clause(case)}) q ORDER BY id"
    )
    with pytest.raises(DecidBCliError, match=r"has no rows for g = 'c'\."):
        _run(decidb_cli, sql, "off")


# The same problem inside a parent query. The first base case meets every context; the others meet one each,
# since a context does not interact with the kind of bound.
CONTEXT_CASES = [
    ("global_upper_max", wrap)
    for wrap in (
        "cte_materialized",
        "cte_inlined",
        "wide_source",
        "joined_source",
        "correlated_source",
        "parent_join",
        "recombined_filters",
    )
] + [
    ("per_interval", "wide_source"),
    ("local_when_interval", "recombined_filters"),
    ("fixed_with_group_bounds", "joined_source"),
    ("no_objective_interval_per", "wide_source"),
    ("no_objective_ge_whole_global", "joined_source"),
    ("no_objective_source_lower_local_when_per", "recombined_filters"),
    ("no_objective_count_star_lower_top_when_per", "parent_join"),
]


@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.sql_subquery
@pytest.mark.correctness
@pytest.mark.parametrize("base,wrap", CONTEXT_CASES, ids=[f"{base}-{wrap}" for base, wrap in CONTEXT_CASES])
def test_s1_agrees_inside_a_parent_context(decidb_cli, oracle_solver, base, wrap):
    _agree(decidb_cli, oracle_solver, S1_CASES[base], wrap)


@pytest.mark.var_boolean
@pytest.mark.correctness
def test_s1_nested_decisions_agree(decidb_cli, oracle_solver):
    """A second DECIDE reads the first one's x. The first problem has a unique optimum (distinct
    scores, top 2), so the oracle's x can feed the second problem's coefficients."""
    case = {"bounds": [("<=", 2)]}
    rows_data = MAIN
    sql = f"""
        SELECT id, g, score, x, y FROM (
            FROM (SELECT id, g, score, x FROM (
                FROM (VALUES {_values(rows_data)}) t({', '.join(NAMES)})
                {_decide_clause(case)}
            ) a) b
            DECIDE y(BOOL) SUCH THAT SUM(y) <= 3 MAXIMIZE SUM((score + x) * y)
        ) c ORDER BY id
    """
    names, constraints = _s1_constraints(case)
    first = _solve_with_oracle(oracle_solver, case)[1]
    chosen = [round(first.variable_values[name]) for name in names]

    oracle_solver.create_model("direct_s1_uniform_matroid_second")
    ys = [f"y_{i}" for i in range(len(rows_data))]
    for name in ys:
        oracle_solver.add_variable(name, VarType.BINARY)
    oracle_solver.add_constraint({name: 1.0 for name in ys}, "<=", 3.0)
    oracle_solver.set_objective(
        {name: float(r["score"] + x) for name, r, x in zip(ys, rows_data, chosen)}, ObjSense.MAXIMIZE
    )
    second = oracle_solver.solve()

    for mode in ("off", "require"):
        rows, columns = _run(decidb_cli, sql, mode)
        assert columns == ["id", "g", "score", "x", "y"], mode
        assert [row[3] for row in rows] == chosen, mode
        assert sum(row[4] for row in rows) <= 3, mode
        compare_solutions(
            rows,
            columns,
            second,
            [(r["id"], r["g"], float(r["score"])) for r in rows_data],
            ["y"],
            coeff_fn=lambda row: {"y": float(row[columns.index("score")] + row[columns.index("x")])},
        )


@pytest.mark.var_boolean
@pytest.mark.correctness
@pytest.mark.parametrize(
    "constraint",
    ["SUM(x) <= 1", "SUM(x) <= -0.5", "SUM(x) = 1.5", "SUM(x) < 0", "x = 1 AND SUM(x) >= 2", "SUM(x) <= COUNT(*) / 2"],
)
def test_s1_empty_source_returns_no_rows_even_when_the_bound_is_impossible(decidb_cli, constraint):
    sql = f"""
        SELECT id, x FROM (
            FROM (SELECT 1 AS id, 2.0 AS score WHERE FALSE) t
            DECIDE x(BOOL) SUCH THAT {constraint} MAXIMIZE SUM(score * x)
        ) q
    """
    for mode in ("off", "require"):
        assert _run(decidb_cli, sql, mode)[0] == [], mode


# ---------------------------------------------------------------------------
# S1 errors with no oracle model: both DeciDB paths must fail the same way
# ---------------------------------------------------------------------------


def _problem(values, columns, constraint, tail="", objective="SUM(score*x)"):
    return (
        f"SELECT x FROM (FROM (VALUES {values}) t({columns}) "
        f"DECIDE x(BOOL) SUCH THAT {constraint} MAXIMIZE {objective}) q{tail}"
    )


# name -> (sql, expected error class on the direct path).
# Both paths must raise; the message and the choice among several errors need not match.
S1_ERROR_CASES = {
    # --- a `when` that keeps no row (D4) ---
    "empty_top_when": (
        _problem("(1, 2.0, false)", "id, score, flag", "when flag: SUM(x) <= 1"),
        "empty_aggregate",
    ),
    # A per-row bound under a `when` that keeps no row is the same error.
    "empty_when_on_a_row_bound": (
        _problem("(1, 2.0), (2, 3.0)", "id, score", "SUM(x) <= 1 AND when id = 99: x >= 1"),
        "empty_aggregate",
    ),
    "empty_all_keys_null": (
        _problem(
            "(1, NULL::VARCHAR, 2.0), (2, NULL::VARCHAR, 3.0)",
            "id, dept, score",
            "when dept IS NOT NULL: SUM(x) by (dept) <= 1",
        ),
        "empty_aggregate",
    ),
    "impossible_upper_bound_per_is_infeasible": (
        _problem(
            "(1, NULL::VARCHAR, TRUE, 9.0), (2, 'A', FALSE, 5.0)",
            "id, dept, flag, score",
            "when dept IS NOT NULL: SUM(x) by (dept) <= -1",
        ),
        "infeasible",
    ),
    # The same over every row, beside a lower bound and a fixed value: the plan counts the fixed rows but keeps no free
    # count, since no count can hold.
    "impossible_upper_bound_beside_a_lower_bound_and_a_fixed_value": (
        _problem("(1, TRUE, 9.0), (2, FALSE, 5.0)", "id, flag, score", "SUM(x) >= 1 AND SUM(x) <= -1 AND x >= flag"),
        "infeasible",
    ),
    "impossible_upper_bound_with_when_has_no_rows": (
        _problem(
            "(1, NULL::VARCHAR, TRUE, 9.0), (2, 'A', FALSE, 5.0)",
            "id, dept, flag, score",
            "when flag AND dept IS NOT NULL: SUM(x) by (dept) <= -1",
        ),
        "empty_aggregate",
    ),
    # The sum's own `when` keeps none of the rows the clause's `when` keeps (D8).
    "inner_when_disjoint_from_the_front_when": (
        _problem("(1, 2.0, true), (2, 3.0, false)", "id, score, flag", "when flag: SUM(when NOT flag: x) <= 1"),
        "empty_aggregate",
    ),
    # --- an equality bound has one value per group ---
    "equality_bound_varies_within_a_group": (
        _problem(
            "(1,'A',1.0::DOUBLE,9.0), (2,'A',1.0::DOUBLE,8.0), (3,'B',1.0::DOUBLE,7.0), (4,'B',2.0::DOUBLE,6.0)",
            "id, dept, eqcap, score",
            "SUM(x) by (dept) = eqcap",
        ),
        "equality_varies",
    ),
    # --- no objective: the bounds and fixed values are still read on every row and still decide feasibility ---
    "no_objective_empty_top_when": (
        "SELECT x FROM (FROM (VALUES (1, false)) t(id, flag) DECIDE x(BOOL) SUCH THAT when flag: SUM(x) >= 1) q",
        "empty_aggregate",
    ),
    # A computed value has no single column to name, so it raises direct solve's own NULL and NaN errors.
    "fixed_computed_value_nan": (
        _problem(
            "(1, 9.0, 'NaN'), (2, 5.0, '1')",
            "id, score, fixed_raw",
            "SUM(x) <= 2 AND x <= TRY_CAST(fixed_raw AS DOUBLE)",
        ),
        "null_bound",
    ),
}


@pytest.mark.var_boolean
@pytest.mark.error
@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(S1_ERROR_CASES))
def test_s1_error_cases_fail_on_both_paths(decidb_cli, name):
    sql, expected = S1_ERROR_CASES[name]
    with pytest.raises(DecidBCliError) as solver_error:
        _run(decidb_cli, sql, "off")
    assert error_class(str(solver_error.value)) == expected, str(solver_error.value)
    with direct_path(sql):
        with pytest.raises(DecidBCliError) as direct_error:
            _run(decidb_cli, sql, "require")
        message = str(direct_error.value)
        assert "decide_direct_solve=require" not in message, f"direct solve declined:\n{message}"
        assert error_class(message) == expected, message


@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_s1_without_an_objective_reads_no_score_column(decidb_cli):
    """With no objective nothing is a score, so a bad value in a column the query never uses raises on neither path."""
    sql = (
        "SELECT id, x FROM (FROM (VALUES (1, 'NaN'::DOUBLE), (2, NULL::DOUBLE), (3, 'Infinity'::DOUBLE)) t(id, score) "
        "DECIDE x(BOOL) SUCH THAT SUM(x) >= 2) q ORDER BY id"
    )
    for mode in ("off", "require"):
        rows, columns = _run(decidb_cli, sql, mode)
        assert columns == ["id", "x"], mode
        assert [row[0] for row in rows] == [1, 2, 3] and sum(row[1] for row in rows) >= 2, mode


# ---------------------------------------------------------------------------
# Values the oracle cannot hold: direct solve against the solver path only
# ---------------------------------------------------------------------------


def _bound_column_problem(cap, op):
    return (
        f"SELECT id, x FROM (FROM (VALUES (1, {cap}, 9.0), (2, {cap}, 8.0)) t(id, cap, score) "
        f"DECIDE x(BOOL) SUCH THAT SUM(x) {op} cap MAXIMIZE SUM(score*x)) q ORDER BY id"
    )


def _fixed_column_problem(fixed_value, op):
    return (
        f"SELECT id, x FROM (FROM (VALUES (1, 9.0, {fixed_value}), (2, 5.0, {fixed_value})) t(id, score, fixed_value) "
        f"DECIDE x(BOOL) SUCH THAT SUM(x) <= 2 AND x {op} fixed_value MAXIMIZE SUM(score*x)) q ORDER BY id"
    )


# name -> sql. Limits the solver reads as doubles: fractions, and the types a limit can come from.
BOUNDARY_CASES = {
    **{
        f"count_limit_{_OPNAMES[op]}_{_tag(cap)}": _bound_column_problem(cap, op)
        for cap, op in [
            ("1.5::DOUBLE", "<="),
            ("1.5::DOUBLE", "="),
        ]
    },
    # Each numeric type a bound can be read from converts to a double its own way.
    **{
        f"count_limit_type_{type_name.split('(')[0].lower()}": _bound_column_problem(f"1::{type_name}", "<=")
        for type_name in ("TINYINT", "SMALLINT", "INTEGER", "BIGINT", "DOUBLE", "FLOAT", "HUGEINT", "DECIMAL(18,2)")
    },
    # Two different integers that become the same double: the solver sees one value.
    "count_equality_reads_doubles": (
        "SELECT id, x FROM (FROM (VALUES (1, 9007199254740992::BIGINT, 9.0), (2, 9007199254740993::BIGINT, 8.0)) "
        "t(id, cap, score) DECIDE x(BOOL) SUCH THAT SUM(x) = cap MAXIMIZE SUM(score*x)) q ORDER BY id"
    ),
    # 100 * 0.57 is exactly 57 in the decimal arithmetic DuckDB uses for the literal; in floating point it is
    # 56.99999999999999, so a bound read through a double would select 56.
    **{
        f"count_star_decimal_exact_{_OPNAMES[op]}": (
            f"SELECT SUM(x) FROM (SELECT * FROM (SELECT i AS id, {score} AS score FROM range(100) r(i)) t "
            f"DECIDE x(BOOL) SUCH THAT SUM(x) {op} COUNT(*) * 0.57 MAXIMIZE SUM(score * x)) q"
        )
        for op, score in [("<=", "1.0 + i"), (">=", "-1.0 - i")]
    },
    **{
        f"fixed_{_OPNAMES[op]}_{_tag(fixed_value)}": _fixed_column_problem(fixed_value, op)
        for fixed_value, op in [("1.5", "<="), ("0.5", ">=")]
    },
}


@pytest.mark.var_boolean
@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(BOUNDARY_CASES))
def test_s1_boundary_values_read_like_the_solver(decidb_cli, name):
    sql = BOUNDARY_CASES[name]
    direct = _outcome(decidb_cli, sql, "require")
    assert direct != ("error", "direct_not_applied"), f"direct solve declined:\n{sql}"
    assert direct == _outcome(decidb_cli, sql, "off")


# A bound built from COUNT(*) is evaluated by DuckDB's own arithmetic, so it can overflow. The solver evaluates it once
# per group that has a row the clause keeps: a row whose `when` holds. The direct plan must raise on exactly those
# queries. Groups: a = 2 rows (1 flagged), b = 2 (both flagged), c = 4 (none flagged),
# NULL key = 4 (all flagged). With a factor of 3e18, a count of 4 or more overflows INT64 (3 * 3e18 = 9e18 still fits;
# INT64 holds up to about 9.22e18), so groups c and NULL overflow if their bound is evaluated and a, b never do.
_OVERFLOW_ROWS = (
    "(1,'a',9.0,true),(2,'a',8.0,false),(3,'b',7.0,true),(4,'b',6.0,true),"
    "(5,'c',5.0,false),(6,'c',4.0,false),(7,'c',3.0,false),(8,'c',2.0,false),"
    "(9,NULL,3.0,true),(10,NULL,2.0,true),(11,NULL,1.0,true),(12,NULL,0.5,true)"
)
_BIG = "3000000000000000000"
# name -> (constraint, outcome on both paths)
COUNT_OVERFLOW_CASES = {
    # The NULL-key group counts four flagged rows, so its bound overflows if evaluated; the `when` leaves it out, so the
    # solver never evaluates it and a plan that did would raise here.
    "null_key_group_with_when": (
        f"when flag AND g IS NOT NULL: SUM(x) by (g) <= COUNT(*) by (g) * {_BIG}",
        "ok",
    ),
    "uncovered_group_negative_factor": (
        f"when flag AND g IS NOT NULL: SUM(x) by (g) >= COUNT(*) by (g) * -{_BIG}",
        "ok",
    ),
    "infinite_bound": ("when flag AND g IS NOT NULL: SUM(x) by (g) <= COUNT(*) by (g) * 1e308", "ok"),
    # A covered group that does overflow: both paths raise.
    "global_overflow": (f"SUM(x) <= COUNT(*) * {_BIG}", "error"),
}


@pytest.mark.var_boolean
@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(COUNT_OVERFLOW_CASES))
def test_s1_count_bound_raises_only_where_the_solver_does(decidb_cli, name):
    constraint, expected = COUNT_OVERFLOW_CASES[name]
    sql = (
        f"SELECT id, x FROM (FROM (VALUES {_OVERFLOW_ROWS}) t(id, g, score, flag) "
        f"DECIDE x(BOOL) SUCH THAT {constraint} MAXIMIZE SUM(score*x)) q ORDER BY id"
    )
    solver = _outcome(decidb_cli, sql, "off")
    assert solver[0] == expected, f"the solver gave {solver}, so this case no longer tests what it says"
    with direct_path(sql):
        direct = _outcome(decidb_cli, sql, "require")
        assert direct != ("error", "direct_not_applied"), f"direct solve declined:\n{sql}"
        assert direct == solver


# Each set has one best score; with room for one row the best is the only row worth selecting. The solvers
# disagree with each other near 1e-9, so the reference is the exact finite DOUBLE, not a solver.
TINY_SCORES = [(5e-324, -5e-324, 0.0), (1e-6, 9e-7, 1e-9), (2e-300, 1e-300, 0.0), (1e-12, 5e-13, 0.0)]


@pytest.mark.var_boolean
@pytest.mark.correctness
@pytest.mark.parametrize("scores", TINY_SCORES, ids=[str(s[0]) for s in TINY_SCORES])
@pytest.mark.parametrize("sense", ["MAXIMIZE", "MINIMIZE"])
def test_s1_tiny_scores_pick_the_exact_best(decidb_cli, scores, sense):
    signed = scores if sense == "MAXIMIZE" else tuple(-score for score in scores)
    values = ", ".join(f"({i}, CAST('{score!r}' AS DOUBLE))" for i, score in enumerate(signed))
    sql = f"""
        SELECT x FROM (FROM (VALUES {values}) t(id, score)
            DECIDE x(BOOL) SUCH THAT SUM(x) <= 1 {sense} SUM(score * x)) q ORDER BY id
    """
    best = max(range(3), key=lambda i: signed[i]) if sense == "MAXIMIZE" else min(range(3), key=lambda i: signed[i])
    expected = [1 if i == best else 0 for i in range(3)]
    assert [row[0] for row in _run(decidb_cli, sql, "require")[0]] == expected


# ---------------------------------------------------------------------------
# S1: a count body that is a negation, `SUM(-x)`
# ---------------------------------------------------------------------------
#
# `SUM(-x) <op> B` is `SUM(x) <flipped op> -B`. The oracle never rewrites the comparison: it states `-x_1 - ... - x_n
# <op> B` over the rows the SUM covers (`body_a` in `_s1_constraints`). A constant term in the body (`SUM(x + c)`) stays
# on the solver.

# scope name -> case keys.
_OFFSET_SCOPES = {
    "global": {},
    "per": {"per": True},
    "top_per": {"per": True, "when": "top"},
    "local_per": {"per": True, "when": "local"},
}


def _offset_case(bounds, scope="per", **extra):
    return {"count_body": "-x", "body_a": -1, "bounds": bounds, **_OFFSET_SCOPES[scope], **extra}


def _binds_from_below(op, a):
    """Whether the count must be large for the bound to hold, so that MINIMIZE is the sense in which it binds."""
    return (op in (">", ">=")) != (a < 0)


S1_OFFSET_CASES = {}
_CORE_SCOPES = ("global", "per", "top_per", "local_per")
for _op, _scope, _delta in itertools.product(_OPNAMES, _CORE_SCOPES, (0, 0.5)):
    if _op == "<>":
        continue
    S1_OFFSET_CASES[f"offset_-1_0_{_OPNAMES[_op]}_{_scope}_{_tag(_delta)}"] = _offset_case(
        [(_op, float(-1 + _delta))], _scope, sense="MINIMIZE" if _binds_from_below(_op, -1) else "MAXIMIZE"
    )

# An interval and a negation alone.
S1_OFFSET_CASES.update(
    {
        "offset_interval_neg_global": _offset_case([(">=", -6), ("<=", -2)], "global"),
        "offset_negation_only_global_upper": _offset_case([("<=", -3)], "global", sense="MINIMIZE"),
        "offset_negation_only_per_lower": _offset_case([(">=", -1)], "per"),
        "offset_negation_only_strict_per": _offset_case([(">", -2)], "per"),
    }
)


@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(S1_OFFSET_CASES))
def test_s1_offset_count_body_oracle_solver_and_direct_agree(decidb_cli, oracle_solver, name):
    _agree(decidb_cli, oracle_solver, S1_OFFSET_CASES[name])


OFFSET_CONTEXT_CASES = [("offset_negation_only_per_lower", "parent_join")]


@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.sql_subquery
@pytest.mark.correctness
@pytest.mark.parametrize(
    "base,wrap", OFFSET_CONTEXT_CASES, ids=[f"{base}-{wrap}" for base, wrap in OFFSET_CONTEXT_CASES]
)
def test_s1_offset_count_body_agrees_inside_a_parent_context(decidb_cli, oracle_solver, base, wrap):
    _agree(decidb_cli, oracle_solver, S1_OFFSET_CASES[base], wrap)


# ---------------------------------------------------------------------------
# S1: a count bound read through an aggregate of a data column, `SUM(x) by (g) <= MIN(cap) by (g)`
# ---------------------------------------------------------------------------
#
# MIN, MAX and COUNT of any numeric column, and SUM and AVG of a TINYINT, SMALLINT, UTINYINT or USMALLINT column, give
# one value per group. The group is the rows the clause keeps (its `when` holds) with the row's key; a `when` inside
# the left sum does not narrow it. The oracle evaluates the aggregate over those rows in Python
# (`REDUCER_BOUND_EXPRS`) and states the bound as a plain linear constraint.

_REDUCER_OPS = ("<=", "<", ">=", ">", "=")
S1_REDUCER_CASES = {}
# Every base meets every comparison in every scope, except the combinations listed here: (base, comparison) -> scopes.
_REDUCER_GRID_LEFT_OUT = {
    ("AVG(sm) * 0.5", "<="): ("top_per",),
    ("AVG(sm) * 0.5", "<"): ("per", "top_per"),
    ("AVG(sm)", "<="): ("top_per",),
    ("AVG(sm)", "<"): ("per", "top_per"),
    ("COUNT(cap)", ">="): ("local_per",),
    ("COUNT(cap)", ">"): _CORE_SCOPES,
    ("MAX(cap)", ">="): ("per", "local_per"),
    ("MAX(cap)", ">"): ("per", "top_per", "local_per"),
    ("MAX(cap) - MIN(cap)", ">"): ("local_per",),
    ("MAX(cap) - MIN(cap)", "<"): ("top_per",),
    ("MIN(cap)", ">"): ("per", "top_per"),
    ("MIN(cap2)", ">"): ("per", "top_per"),
    ("SUM(sm) / 2 + 1", ">"): ("top_per", "local_per"),
    ("SUM(sm)", ">="): ("top_per", "local_per"),
    ("SUM(sm)", ">"): _CORE_SCOPES,
    ("SUM(sm)", "<="): ("top_per",),
    ("SUM(sm)", "<"): ("per", "top_per"),
}
for _base, _op, _scope in itertools.product(_REDUCER_BASES, _REDUCER_OPS, _CORE_SCOPES):
    if _scope in _REDUCER_GRID_LEFT_OUT.get((_base, _op), ()):
        continue
    S1_REDUCER_CASES[f"reducer_{_tag(_base.replace(' ', ''))}_{_OPNAMES[_op]}_{_scope}"] = {
        **_OFFSET_SCOPES[_scope],
        "bounds": [(_op, _base)],
        "sense": "MINIMIZE" if _op in (">", ">=") else "MAXIMIZE",
    }

# Bodies with an offset or a negation against an aggregate bound, intervals, several bounds, fixed values, and no
# objective.
S1_REDUCER_CASES.update(
    {
        "reducer_interval_min_max_per": {"bounds": [(">=", "MIN(cap)"), ("<=", "MAX(cap)")], "per": True},
        "reducer_interval_min_max_local_when": {
            "bounds": [(">=", "MIN(cap)"), ("<=", "MAX(cap)")],
            "per": True,
            "when": "local",
        },
        "reducer_interval_avg_sum_global": {"bounds": [(">=", "AVG(sm)"), ("<=", "SUM(sm) / 2 + 1")]},
        # Group b: the unflagged row has the smallest cap, so a MIN that read only the left sum's flagged rows would
        # differ.
        "reducer_local_when_reads_unflagged_rows": {"bounds": [("<=", "MIN(cap)")], "per": True, "when": "local"},
        "reducer_local_when_max_reads_unflagged_rows": {
            "bounds": [("<=", "MAX(cap)")],
            "per": True,
            "when": "local",
        },
        "reducer_with_source_bound": {"bounds": [("<=", "MIN(cap)"), ("<=", "cap")], "per": True},
        "reducer_with_count_star": {"bounds": [("<=", "MAX(cap)"), ("<=", "COUNT(*) - 1")], "per": True},
        "reducer_with_coalesce_bound": {
            "bounds": [(">=", "COALESCE(nlo, 1.0)"), ("<=", "MAX(cap)")],
            "per": True,
        },
        "reducer_with_fixed": {"bounds": [("<=", "MAX(cap)")], "per": True, "fixed": [(1, [4]), (0, [0, 8])]},
        "reducer_no_objective_lower": {"bounds": [(">=", "MIN(cap)")], "per": True, "objective": "none"},
        "reducer_no_objective_local_when": {
            "bounds": [(">=", "AVG(sm)")],
            "per": True,
            "when": "local",
            "objective": "none",
        },
        "reducer_ties": {"data": "ties", "bounds": [("<=", "MAX(cap)")], "per": True},
        # A reducer with a `when` of its own. Without a key the group is every row (the clause keeps no row less), so
        # the window has no partition and its FILTER is the reducer's `when` alone.
        "reducer_own_when_count_star_global": {"bounds": [(">=", "SUM(when cap > 1: 1)")], "sense": "MINIMIZE"},
        "reducer_own_when_min_global": {"bounds": [("<=", "MIN(when cap > 1: cap)")]},
        # The reducer's `when` scopes only the count on the right; the left side still sums every row. Five rows have
        # id > 4 and seven have a positive score, so the count is what limits the optimum.
        "reducer_own_when_count_star_le_global": {"bounds": [("<=", "SUM(when id > 4: 1)")]},
        "reducer_own_when_count_star_halved_le_global": {"bounds": [("<=", "SUM(when id > 0: 1) / 2")]},
        # Per group, each reducer reads the rows of its group that its own `when` keeps.
        "reducer_own_when_count_star_per": {
            "bounds": [(">=", "SUM(when cap > 1: 1)")],
            "per": True,
            "sense": "MINIMIZE",
        },
        "reducer_own_when_avg_per": {"bounds": [(">=", "AVG(when cap > 1: sm)")], "per": True, "sense": "MINIMIZE"},
        "reducer_own_when_min_per": {"bounds": [("<=", "MIN(when cap > 1: cap)")], "per": True},
        "reducer_own_when_differences_of_two_counts_per": {
            "bounds": [(">=", "SUM(when cap > 1: 1) - SUM(when cap < 3: 1)")],
            "per": True,
            "sense": "MINIMIZE",
        },
        # Both sides have a `when`. The right one scopes the reducer within the group, the left one the rows the sum
        # counts.
        "reducer_own_when_beside_a_left_local_when": {
            "bounds": [("<=", "MIN(when cap > 1: cap)")],
            "per": True,
            "when": "local",
        },
        "reducer_own_when_max_beside_a_left_local_when_global": {
            "bounds": [("<=", "MAX(when flag: cap)")],
            "when": "local",
        },
        # A negated count body against an aggregate bound.
        "reducer_offset_negation_sum_local_when": _offset_case(
            [(">=", "SUM(sm) / 2 + 1")], "local_per", sense="MINIMIZE"
        ),
        # The same over every row: direct solve reads the window's value and negates it (SUM(x) <= COUNT(*) - 6).
        "reducer_offset_negation_count_star_global": _offset_case([(">=", "-COUNT(*) + 6")], "global"),
    }
)


@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(S1_REDUCER_CASES))
def test_s1_reducer_bound_oracle_solver_and_direct_agree(decidb_cli, oracle_solver, name):
    _agree(decidb_cli, oracle_solver, S1_REDUCER_CASES[name])


REDUCER_CONTEXT_CASES = [
    ("reducer_MIN(cap)_le_per", "wide_source"),
    ("reducer_local_when_reads_unflagged_rows", "recombined_filters"),
    ("reducer_AVG(sm)_ge_top_per", "parent_join"),
]


@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.sql_subquery
@pytest.mark.correctness
@pytest.mark.parametrize(
    "base,wrap", REDUCER_CONTEXT_CASES, ids=[f"{base}-{wrap}" for base, wrap in REDUCER_CONTEXT_CASES]
)
def test_s1_reducer_bound_agrees_inside_a_parent_context(decidb_cli, oracle_solver, base, wrap):
    _agree(decidb_cli, oracle_solver, S1_REDUCER_CASES[base], wrap)


# A bad value in a column an aggregate reduces raises on both paths, on every row the aggregate reads.
# name -> (column type, rows as (id, key, flag, value), constraint).
_REDUCER_ROWS = (
    "(1, 'a', true, 9.0, {v1}), (2, 'a', false, 8.0, {v2}), (3, 'b', true, 7.0, {v3}), (4, NULL, true, 6.0, {v4})"
)
S1_REDUCER_ERROR_CASES = {
    # The NaN is on the NULL-key row, a group of its own (D5), which the aggregate reads.
    "nan_float_column": ("FLOAT", ("1.0", "2.0", "3.0", "'NaN'::FLOAT"), "SUM(x) by (g) <= MAX(v) by (g)"),
    # A `when` that keeps no row is an error (D4).
    "empty_group_top_when": ("INTEGER", ("1", "2", "3", "4"), "when id > 100: SUM(x) <= MIN(v)"),
    "empty_group_top_when_per": (
        "SMALLINT",
        ("1", "2", "3", "4"),
        "when id > 100 AND g IS NOT NULL: SUM(x) by (g) <= AVG(v) by (g)",
    ),
}


@pytest.mark.var_boolean
@pytest.mark.error
@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(S1_REDUCER_ERROR_CASES))
def test_s1_reducer_bound_errors_fail_on_both_paths(decidb_cli, name):
    column_type, values, constraint = S1_REDUCER_ERROR_CASES[name]
    typed = tuple(f"CAST({value} AS {column_type})" for value in values)
    rows = _REDUCER_ROWS.format(v1=typed[0], v2=typed[1], v3=typed[2], v4=typed[3])
    sql = (
        f"SELECT id, x FROM (FROM (VALUES {rows}) t(id, g, flag, score, v) "
        f"DECIDE x(BOOL) SUCH THAT {constraint} MAXIMIZE SUM(score*x)) q ORDER BY id"
    )
    with pytest.raises(DecidBCliError) as solver_error:
        _run(decidb_cli, sql, "off")
    with direct_path(sql):
        with pytest.raises(DecidBCliError) as direct_error:
            _run(decidb_cli, sql, "require")
        message = str(direct_error.value)
        assert "decide_direct_solve=require" not in message, f"direct solve declined:\n{message}"
        assert error_class(message) == error_class(str(solver_error.value)), (message, str(solver_error.value))


# A NaN on a row the clause's `when` drops is never read (nothing is computed on a dropped row), so the query solves,
# and both paths give the same rows.
def test_s1_reducer_bound_reads_no_value_on_a_row_the_when_drops(decidb_cli):
    typed = tuple(f"CAST({value} AS DOUBLE)" for value in ("1.0", "'NaN'::DOUBLE", "3.0", "4.0"))
    rows = _REDUCER_ROWS.format(v1=typed[0], v2=typed[1], v3=typed[2], v4=typed[3])
    sql = (
        f"SELECT id, x FROM (FROM (VALUES {rows}) t(id, g, flag, score, v) "
        f"DECIDE x(BOOL) SUCH THAT when flag: SUM(x) <= MIN(v) MAXIMIZE SUM(score*x)) q ORDER BY id"
    )
    direct = _outcome(decidb_cli, sql, "require")
    assert direct[0] == "ok", direct
    assert direct == _outcome(decidb_cli, sql, "off")


# COUNT reads its argument as SQL's COUNT does: it skips a NULL and counts a NaN, so neither raises. Every score is
# positive, so the best answer fills the limit; DuckDB's own COUNT over the same rows gives the limit.
def test_s1_count_bound_skips_null_and_counts_nan_as_sql_does(decidb_cli):
    typed = tuple(f"CAST({value} AS DOUBLE)" for value in ("1.0", "'NaN'::DOUBLE", "NULL", "4.0"))
    rows = _REDUCER_ROWS.format(v1=typed[0], v2=typed[1], v3=typed[2], v4=typed[3])
    source = f"FROM (VALUES {rows}) t(id, g, flag, score, v)"
    sql = (
        f"SELECT id, x FROM ({source} DECIDE x(BOOL) SUCH THAT SUM(x) <= COUNT(v) - 1 MAXIMIZE SUM(score*x)) q "
        "ORDER BY id"
    )
    counted = _run(decidb_cli, f"SELECT COUNT(v) {source}", "off")[0][0][0]
    solver = _outcome(decidb_cli, sql, "off")
    assert solver[0] == "ok", solver
    assert sum(x for _, x in solver[1]) == counted - 1, (solver, counted)
    with direct_path(sql):
        assert _outcome(decidb_cli, sql, "require") == solver


# Infinity is a value of the aggregate, not an error: no limit above, or a limit no count meets.
_INFINITE_FIRST = ("'Infinity'", "2.0", "3.0")
_INFINITE_SECOND = ("1.0", "'-Infinity'", "3.0")
REDUCER_INFINITY_CASES = {
    f"{aggregate}_{_OPNAMES[op]}_{'_'.join(_tag(v) for v in values)}": (aggregate, op, values)
    for aggregate, op, values in (
        ("MAX(v)", ">=", _INFINITE_SECOND),
        ("MAX(v)", "<=", _INFINITE_SECOND),
        ("MAX(v)", "<", _INFINITE_SECOND),
        ("MIN(v)", ">", _INFINITE_FIRST),
        ("MIN(v)", "<=", _INFINITE_FIRST),
        ("MIN(v)", "<", _INFINITE_FIRST),
    )
}


@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(REDUCER_INFINITY_CASES))
def test_s1_reducer_bound_reads_infinity_like_the_solver(decidb_cli, name):
    aggregate, op, values = REDUCER_INFINITY_CASES[name]
    rows = ", ".join(f"({i}, {10 - i}.0, CAST({v} AS DOUBLE))" for i, v in enumerate(values))
    sql = (
        f"SELECT id, x FROM (FROM (VALUES {rows}) t(id, score, v) "
        f"DECIDE x(BOOL) SUCH THAT SUM(x) {op} {aggregate} MAXIMIZE SUM(score*x)) q ORDER BY id"
    )
    direct = _outcome(decidb_cli, sql, "require")
    assert direct != ("error", "direct_not_applied"), f"direct solve declined:\n{sql}"
    assert direct == _outcome(decidb_cli, sql, "off")


# What stays on the solver. Each declines under `require`, and `auto` gives the answer `off` gives. A
# relation-qualified reducer keeps the solver's fold; every other COUNT, MIN, MAX, SUM and AVG on the right, with or
# without a `when` of its own, is a window column of the source (H-19, H-20) and is exact, so it moved to
# `S1_REDUCER_NOW_APPLIED` below.
S1_REDUCER_DECLINED = {
    "sum_qualified_by_its_relation": ("INTEGER", ("1", "2", "3"), "SUM(t: v)"),
}


@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(S1_REDUCER_DECLINED))
def test_s1_reducer_bound_declines_what_it_cannot_read_exactly(decidb_cli, name):
    column_type, values, reducer = S1_REDUCER_DECLINED[name]
    rows = ", ".join(f"({i}, {9 - i}.0, CAST({value} AS {column_type}))" for i, value in enumerate(values))
    sql = (
        f"SELECT id FROM (FROM (VALUES {rows}) t(id, score, v) "
        f"DECIDE x(BOOL) SUCH THAT SUM(x) <= {reducer} MAXIMIZE SUM(score*x)) q ORDER BY id"
    )
    with pytest.raises(DecidBCliError) as declined:
        _run(decidb_cli, sql, "require")
    assert "decide_direct_solve=require" in str(declined.value), declined.value
    assert _outcome(decidb_cli, sql, "auto") == _outcome(decidb_cli, sql, "off")


# These were declined while the solver added row by row in DOUBLE (S1-16). The right-hand aggregate is now a window
# column of the source, computed by DuckDB in the column's own type, so S1 reads it and the answer is the exact one.
# name -> (column type, values, reducer, the reducer's exact value from the values).
_EXACT = {
    "sum": lambda values: sum(values),
    "avg": lambda values: sum(values) / len(values),
}
S1_REDUCER_NOW_APPLIED = {
    "avg_integer": ("INTEGER", ("1", "2", "3"), "AVG(v)", _EXACT["avg"]),
    "avg_bigint": ("BIGINT", ("1", "2", "3"), "AVG(v)", _EXACT["avg"]),
    "avg_decimal": ("DECIMAL(9,2)", ("1.25", "2.5", "3"), "AVG(v)", _EXACT["avg"]),
    "sum_double": ("DOUBLE", ("0.1", "0.2", "0.3"), "SUM(v)", _EXACT["sum"]),
    "avg_float": ("FLOAT", ("0.1", "0.2", "0.3"), "AVG(v)", _EXACT["avg"]),
    # The solver added in DOUBLE: 2^60 + 1 lost the 1, so it read 0 where the exact sum is 1.
    "sum_bigint_that_cancels": (
        "BIGINT",
        ("1152921504606846976", "1", "-1152921504606846976"),
        "SUM(v)",
        _EXACT["sum"],
    ),
    "min_of_an_expression": ("INTEGER", ("1", "2", "3"), "MIN(v + 1)", lambda values: min(values) + 1),
    # The reducer's own `when` keeps ids 1 and 2 (the id is the position): their values are 2 and 3.
    "min_with_its_own_when": ("INTEGER", ("1", "2", "3"), "MIN(when id > 0: v)", lambda values: min(values[1:])),
    "min_with_its_own_when_in_arithmetic": (
        "INTEGER",
        ("1", "2", "3"),
        "MIN(when id > 0: v) + 1",
        lambda values: min(values[1:]) + 1,
    ),
    "divisor_is_an_aggregate": (
        "INTEGER",
        ("1", "2", "3"),
        "MAX(v) / MIN(v)",
        lambda values: max(values) / min(values),
    ),
}


@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(S1_REDUCER_NOW_APPLIED))
def test_s1_reducer_bound_over_a_window_agrees_with_the_solver_and_oracle(decidb_cli, oracle_solver, name):
    column_type, values, reducer, exact = S1_REDUCER_NOW_APPLIED[name]
    rows = ", ".join(f"({i}, {9 - i}.0, CAST({value} AS {column_type}))" for i, value in enumerate(values))
    sql = (
        f"SELECT id, x FROM (FROM (VALUES {rows}) t(id, score, v) "
        f"DECIDE x(BOOL) SUCH THAT SUM(x) <= {reducer} MAXIMIZE SUM(score*x)) q ORDER BY id"
    )
    limit = min(math.floor(exact([Fraction(value) for value in values])), len(values))
    oracle_solver.create_model("direct_s1_reducer_over_a_window")
    names = [f"x_{i}" for i in range(len(values))]
    for variable in names:
        oracle_solver.add_variable(variable, VarType.BINARY)
    oracle_solver.add_constraint({variable: 1.0 for variable in names}, "<=", float(limit))
    oracle_solver.set_objective({variable: float(9 - i) for i, variable in enumerate(names)}, ObjSense.MAXIMIZE)
    oracle = oracle_solver.solve()

    def check_assignment(result_rows, columns, mode):
        assert [row[0] for row in result_rows] == list(range(len(values))), mode
        assert sum(row[1] for row in result_rows) <= limit, (mode, result_rows, limit)
        assert sum((9 - row[0]) * row[1] for row in result_rows) == oracle.objective_value, (mode, result_rows)

    agree_with_oracle(decidb_cli, sql, oracle, check_assignment)


# The same question for a bound read from a column, or through an aggregate on the right (a window column of the source).
# DuckDB evaluates the arithmetic around it, so it can overflow. The solver evaluates a plain column bound on every row
# the clause keeps, and an aggregate bound once per group that has a kept row. Group a
# and b have cap 2, group c (no flagged row) and the NULL key (flagged) have cap 100, and a factor of 2e17 overflows INT64
# from cap 47 up.
_WINDOW_OVERFLOW_ROWS = (
    "(1,'a',9.0,true,2),(2,'a',8.0,false,2),(3,'b',7.0,true,2),(4,'b',6.0,true,2),"
    "(5,'c',5.0,false,100),(6,'c',4.0,false,100),(7,NULL,3.0,true,100),(8,NULL,2.0,true,100)"
)
_K = "200000000000000000"
# name -> (constraint, outcome on both paths)
WINDOW_OVERFLOW_CASES = {
    # Only groups the `when` leaves out would overflow: both paths leave them alone.
    "uncovered_group_would_overflow_top_when_per": (
        f"when flag AND g IS NOT NULL: SUM(x) by (g) <= MAX(cap) by (g) * {_K}",
        "ok",
    ),
    # A plain column bound is evaluated on every row the clause keeps: here every row.
    "plain_cap_times_huge": (f"SUM(x) <= cap * {_K}", "error"),
    "plain_cap_times_small": ("SUM(x) <= cap * 3", "ok"),
}


@pytest.mark.var_boolean
@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(WINDOW_OVERFLOW_CASES))
def test_s1_bound_over_a_column_raises_only_where_the_solver_does(decidb_cli, name):
    constraint, expected = WINDOW_OVERFLOW_CASES[name]
    sql = (
        f"SELECT id, x FROM (FROM (VALUES {_WINDOW_OVERFLOW_ROWS}) t(id, g, score, flag, cap) "
        f"DECIDE x(BOOL) SUCH THAT {constraint} MAXIMIZE SUM(score*x)) q ORDER BY id"
    )
    solver = _outcome(decidb_cli, sql, "off")
    assert solver[0] == expected, f"the solver gave {solver}, so this case no longer tests what it says"
    with direct_path(sql):
        direct = _outcome(decidb_cli, sql, "require")
        assert direct != ("error", "direct_not_applied"), f"direct solve declined:\n{sql}"
        assert direct == solver
