"""N1 (Euclidean ball), checked three ways: the independent oracle, the solver path, the direct path.

Every case is one DECIDE query with one `REAL` decision on each row, in the box [0, inf) or (-inf, inf), under one
`SUM(q * x^2) <= B` per group, with a linear objective or one convex square. The factor q is one constant, or differs
between rows (`SUM(POWER(SQRT(w) * x, 2)) <= B`, q = w), and a factor that differs between rows takes a linear objective
only. The oracle builds its own model from the raw
rows, from the groups, limits and per-row coefficients each case states in Python (never from DeciDB's reading of the
SQL), through `add_quadratic_constraint`. DeciDB then runs the query with `decide_direct_solve='off'` (the solver) and
`'require'` (direct solve; a query the rule does not apply to fails instead of quietly using the solver). The case passes
when all three agree:

- the oracle finds the problem feasible: both DeciDB runs return an assignment inside the box and every ball, that reaches
  the oracle's objective (rows may differ on ties);
- the oracle finds it infeasible or unbounded: both DeciDB runs raise that error.

The tolerances are measured on these cases, not shared with another suite, and one step tighter than each fails or sits
at the edge: Gurobi stays inside a ball to under 1e-8 of its limit and inside the box to under 1e-5, and its objective
is within 1e-6 of the oracle's (with 25 cases failing at 1e-7). Its answers are less exact row by row (3.0005 where the
exact answer is 3), and that moves the objective only to second order, so no row-by-row comparison is made. The direct
path computes the formula, so it stays inside a ball to rounding.

What a result comparison cannot see (which plan ran, EXPLAIN, prepared plans, DIAGNOSE, pruning) stays in
`test_direct_rule_conformance.py`.

Note: `oracle_solver` caches each result under the test node id plus a hash of the test function's source. The case
tables live outside that source, so editing a case does not invalidate the cache; a stale value fails loudly with an
objective mismatch. After changing a case or the model builder, remove its entries from `results/oracle_cache.json`.
"""

import math
import re
import subprocess
from dataclasses import dataclass

import pytest

from solver.types import ObjSense, VarType

from ._direct_oracle_common import _allow_direct_path  # noqa: F401  (autouse fixture)
from ._direct_oracle_common import agree_with_oracle, outcome, run, sql_literal, values_clause, without_gurobi


BIG = 1e30  # the oracle reads a bound this large as none

_BALL_TOLERANCE = 1e-7
_DIRECT_BALL_TOLERANCE = 1e-14
_BOX_TOLERANCE = 1e-5
_OBJECTIVE_TOLERANCE = 5e-6

# ---------------------------------------------------------------------------
# Data
# ---------------------------------------------------------------------------

COLUMNS = (
    ("id", "INTEGER"),
    ("s", "DOUBLE"),  # a per-row score, with positive, negative and zero values
    ("y", "DOUBLE"),  # a per-row target
    ("k", "VARCHAR"),  # a group key
    ("f", "BOOLEAN"),  # `when` membership
    ("l", "DOUBLE"),  # a limit read from the data
    ("w", "DOUBLE"),  # a per-row weight, the factor of the row's square in the limit of the weighted cases
)
NAMES = [name for name, _ in COLUMNS]


def _row(id, s=0.0, y=0.0, k="a", f=True, l=25.0, w=1.0):
    return dict(id=id, s=s, y=y, k=k, f=f, l=l, w=w)


MIXED = (
    _row(1, 3.0, 1.0, "a", True, 25.0),
    _row(2, 4.0, -2.0, "a", False, 36.0),
    _row(3, -1.0, 6.0, "b", True, 9.0),
    _row(4, 0.0, 0.0, "b", False, 16.0),
    _row(5, 5.0, 0.5, "c", True, 4.0),
)
# No row gains from moving up in [0, inf).
NONPOSITIVE = (_row(1, -3.0, -1.0, "a"), _row(2, -1.0, -2.0, "b"), _row(3, 0.0, 0.0, "b"))
ALL_ZERO = (_row(1), _row(2, k="b"), _row(3, k="b"))
# Every group's targets, in [0, inf) and in a free domain, stay inside a limit of 25.
NEAR = (_row(1, y=1.0, k="a"), _row(2, y=-2.0, k="a"), _row(3, y=3.0, k="b"), _row(4, y=0.0, k="b"))
# A NULL key on an improving row, and on one that does not gain.
NULL_KEY_IMPROVING = (_row(1, 3.0, 1.0, "a"), _row(2, 4.0, 2.0, None))
NULL_KEY_STILL = (_row(1, 3.0, 1.0, "a"), _row(2, -4.0, -2.0, None), _row(3, 0.0, 0.0, None))
NULL_KEY_ONLY = (_row(1, 3.0, 1.0, None), _row(2, -4.0, 2.0, None))
NULL_KEY_ONLY_STILL = (_row(1, -3.0, -1.0, None), _row(2, 0.0, 2.0, None))
# A NULL coefficient skips its instance: the row's coefficient is 0.
NULL_SCORE = (_row(1, 3.0, 1.0), _row(2, None, 2.0), _row(3, 4.0, 3.0))
NULL_TARGET = (_row(1, 1.0, 1.0), _row(2, 2.0, None), _row(3, 3.0, 6.0))
# Limits that differ inside a group, with NULLs: the tightest value applies, and a group with none has no ball.
NULL_LIMITS = (
    _row(1, 3.0, 3.0, "a", True, 25.0),
    _row(2, 4.0, 4.0, "a", True, None),
    _row(3, -1.0, 1.0, "b", True, None),
    _row(4, 0.0, 2.0, "b", True, None),
    _row(5, 2.0, 5.0, "c", True, 9.0),
    _row(6, 1.0, 5.0, "c", False, 4.0),
)
NEGATIVE_IN_ONE_GROUP = (_row(1, 3.0, 1.0, "a", l=25.0), _row(2, 4.0, 2.0, "b", l=-1.0))
# A `when` drops the rows that would be unbounded on their own.
WHEN_STILL = (_row(1, 3.0, 1.0, "a", True), _row(2, 4.0, 3.0, "a", True), _row(3, -1.0, 2.0, "b", False))
WHEN_IMPROVING = (_row(1, 3.0, 1.0, "a", True), _row(2, 4.0, 3.0, "a", False))
WHEN_NULL = (_row(1, 3.0, 1.0, "a", True), _row(2, -1.0, 3.0, "a", None))

# A weight on every row: the factor of the row's square in the limit. The worked example of the class definition.
WORKED = (_row(1, 1.0, w=1.0), _row(2, 4.0, w=4.0))
# MIXED with a weight per row, unequal and below and above 1.
WEIGHTED = tuple(dict(row, w=w) for row, w in zip(MIXED, (1.0, 4.0, 0.25, 2.0, 9.0)))
# A row of weight 0 has no square in the sum: it stays at 0 when its direction is 0, and is unbounded otherwise.
WEIGHT_ZERO_STILL = (_row(1, 3.0, w=1.0), _row(2, 0.0, w=0.0), _row(3, 4.0, w=4.0))
WEIGHT_ZERO_IMPROVING = (_row(1, 3.0, w=1.0), _row(2, 2.0, w=0.0), _row(3, 4.0, w=4.0))
# A NULL weight reads as 0, as a NULL coefficient does.
WEIGHT_NULL_STILL = (_row(1, 3.0, w=1.0), _row(2, 0.0, w=None), _row(3, 4.0, w=4.0))
WEIGHT_NULL_IMPROVING = (_row(1, 3.0, w=1.0), _row(2, 2.0, w=None), _row(3, 4.0, w=4.0))
# A weight on a row a `when` drops is not read: a negative one under SQRT raises nothing.
WEIGHT_NEGATIVE_WHEN_DROPS = (
    _row(1, 3.0, k="a", f=True, w=1.0),
    _row(2, 4.0, k="a", f=True, w=4.0),
    _row(3, 5.0, k="b", f=False, w=-4.0),
)
# Weights and keys: a NULL key puts the improving row outside every ball.
WEIGHTED_KEYS = (
    _row(1, 3.0, k="a", w=1.0),
    _row(2, 4.0, k="a", w=4.0),
    _row(3, 2.0, k="b", w=0.25),
    _row(4, 1.0, k="b", w=9.0),
)
WEIGHTED_NULL_KEY = (_row(1, 3.0, k="a", w=1.0), _row(2, 4.0, k="a", w=4.0), _row(3, 2.0, k=None, w=2.0))
# Limits that differ by group, with a weight per row.
WEIGHTED_LIMITS = (
    _row(1, 3.0, k="a", l=9.0, w=1.0),
    _row(2, 4.0, k="a", l=16.0, w=4.0),
    _row(3, 2.0, k="b", l=4.0, w=0.25),
    _row(4, -1.0, k="b", l=25.0, w=9.0),
)


def _values(rows):
    return values_clause(rows, NAMES, dict(COLUMNS), sql_literal)


# ---------------------------------------------------------------------------
# Balls: which rows share a limit, and what the limit is
# ---------------------------------------------------------------------------


def everyone(row):
    return ()


def by_k(row):
    return None if row["k"] is None else (row["k"],)


def where_f(group):
    return lambda row: group(row) if row["f"] else None


def constant(value):
    return lambda members, rows: value


def tightest_of_group(members, rows):
    """The smallest limit of the group's rows; a NULL is no limit."""
    limits = [row["l"] for row in members if row["l"] is not None]
    return min(limits) if limits else None


def tightest_of_all(members, rows):
    limits = [row["l"] for row in rows if row["l"] is not None]
    return min(limits) if limits else None


def sum_of_group(members, rows):
    """The sum of the group's limits; a NULL is skipped, and a group of NULLs has no limit."""
    limits = [row["l"] for row in members if row["l"] is not None]
    return sum(limits) if limits else None


def half_of_all(members, rows):
    limits = [row["l"] for row in rows if row["l"] is not None]
    return 0.5 * sum(limits) if limits else None


def any_key(rows):
    return any(row["k"] is not None for row in rows)


@dataclass(frozen=True)
class Ball:
    sql: str
    #: The factor of the sum of squares: `SUM(q * x^2)`, one number, or a function of the row.
    q: object = 1.0
    #: row -> the key of the group whose sum holds it, or None when no sum holds it.
    group: object = everyone
    #: (rows of the group, all rows) -> the group's limit, or None for no limit.
    limit: object = constant(25.0)
    #: rows -> whether the clause has an instance at all.
    instance: object = lambda rows: True

    def factor(self, row):
        return self.q(row) if callable(self.q) else self.q


BALLS = {
    "b25": Ball("SUM(POWER(x, 2)) <= 25"),
    "b4": Ball("SUM(POWER(x, 2)) <= 4", limit=constant(4.0)),
    "b1": Ball("SUM(POWER(x, 2)) <= 1", limit=constant(1.0)),
    "b0": Ball("SUM(POWER(x, 2)) <= 0", limit=constant(0.0)),
    "negative": Ball("SUM(POWER(x, 2)) <= -1", limit=constant(-1.0)),
    # The spellings of one limit: each rule read the same way.
    "times": Ball("SUM(x * x) <= 25"),
    "stars": Ball("SUM(x ** 2) <= 25"),
    "norm": Ball("norm(x, 2) <= 25"),
    "scaled_square": Ball("SUM(2 * POWER(x, 2)) <= 50", q=2.0, limit=constant(50.0)),
    "inner_slope": Ball("SUM(POWER(2 * x, 2)) <= 100", q=4.0, limit=constant(100.0)),
    "inner_negative_slope": Ball("SUM(POWER(-2 * x, 2)) <= 100", q=4.0, limit=constant(100.0)),
    "literal_on_right": Ball("25 >= SUM(POWER(x, 2))"),
    # Groups.
    "by": Ball("SUM(POWER(x, 2)) by (k) <= 9", group=by_k, limit=constant(9.0)),
    "per": Ball("per k: SUM(POWER(x, 2)) <= 9", limit=constant(9.0), instance=any_key),
    "per_by": Ball("per k: SUM(POWER(x, 2)) by (k) <= 9", group=by_k, limit=constant(9.0)),
    # A `when` on the clause, and inside the sum.
    "when": Ball("when f: SUM(POWER(x, 2)) <= 9", group=where_f(everyone), limit=constant(9.0)),
    "sum_when": Ball("SUM(when f: POWER(x, 2)) <= 9", group=where_f(everyone), limit=constant(9.0)),
    "when_by": Ball("when f: SUM(POWER(x, 2)) by (k) <= 9", group=where_f(by_k), limit=constant(9.0)),
    # Limits read from the data.
    "column": Ball("SUM(POWER(x, 2)) <= l", limit=tightest_of_all),
    "column_scaled": Ball("SUM(4 * POWER(x, 2)) <= l", q=4.0, limit=tightest_of_all),
    "column_by": Ball("SUM(POWER(x, 2)) by (k) <= l", group=by_k, limit=tightest_of_group),
    "column_when": Ball("when f: SUM(POWER(x, 2)) <= l", group=where_f(everyone), limit=tightest_of_group),
    "column_sum_when": Ball("SUM(when f: POWER(x, 2)) <= l", group=where_f(everyone), limit=tightest_of_all),
    "column_expression": Ball("SUM(POWER(x, 2)) <= 25.0 + 0 * id"),
    # A limit that is an aggregate of the group's rows, or of all rows, which the plan computes beside the ball's group.
    "sum_by": Ball("SUM(POWER(x, 2)) by (k) <= SUM(l) by (k)", group=by_k, limit=sum_of_group),
    "half_sum": Ball("SUM(POWER(x, 2)) <= 0.5 * SUM(l)", limit=half_of_all),
    # COUNT(*) is a limit input with no argument.
    "count_all": Ball("SUM(POWER(x, 2)) <= COUNT(*)", limit=lambda members, rows: float(len(rows))),
    # An aggregate over a sum with its own `when` is computed before the rule and read as a column.
    "aggregate_sum_when": Ball(
        "SUM(when f: POWER(x, 2)) <= 0.25 * SUM(l)",
        group=where_f(everyone),
        limit=lambda members, rows: 0.25 * sum(row["l"] for row in rows),
    ),
    # A factor that differs between rows: the slope inside the square is a value of the row, q = c * s^2. A NULL weight
    # makes the slope 0, so its row has no square in the sum.
    "root_b5": Ball("SUM(POWER(SQRT(w) * x, 2)) <= 5", q=lambda r: r["w"] or 0.0, limit=constant(5.0)),
    "root": Ball("SUM(POWER(SQRT(w) * x, 2)) <= 25", q=lambda r: r["w"] or 0.0),
    "root_b0": Ball("SUM(POWER(SQRT(w) * x, 2)) <= 0", q=lambda r: r["w"] or 0.0, limit=constant(0.0)),
    "root_negative": Ball("SUM(POWER(SQRT(w) * x, 2)) <= -1", q=lambda r: r["w"] or 0.0, limit=constant(-1.0)),
    "slope": Ball("SUM(POWER(w * x, 2)) <= 25", q=lambda r: (r["w"] or 0.0) ** 2),
    "slope_scaled": Ball("SUM(3 * POWER(w * x, 2)) <= 25", q=lambda r: 3.0 * (r["w"] or 0.0) ** 2),
    "root_by": Ball(
        "SUM(POWER(SQRT(w) * x, 2)) by (k) <= 9", q=lambda r: r["w"] or 0.0, group=by_k, limit=constant(9.0)
    ),
    "root_per": Ball(
        "per k: SUM(POWER(SQRT(w) * x, 2)) <= 9",
        q=lambda r: r["w"] or 0.0,
        limit=constant(9.0),
        instance=any_key,
    ),
    "root_when": Ball(
        "when f: SUM(POWER(SQRT(w) * x, 2)) <= 9",
        q=lambda r: r["w"] or 0.0,
        group=where_f(everyone),
        limit=constant(9.0),
    ),
    "root_column_by": Ball(
        "SUM(POWER(SQRT(w) * x, 2)) by (k) <= l", q=lambda r: r["w"] or 0.0, group=by_k, limit=tightest_of_group
    ),
}


# ---------------------------------------------------------------------------
# Objectives: the per-row `K*x^2 + b*x`, before the sense is applied
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class Objective:
    sql: str
    maximize: bool = False
    #: row -> (K, b), or None for no objective.
    coefficients: object = None


def _when(row, values):
    return values if row["f"] else (0.0, 0.0)


OBJECTIVES = {
    "none": Objective(""),
    "max": Objective("MAXIMIZE SUM(s * x)", True, lambda r: (0.0, r["s"])),
    "min": Objective("MINIMIZE SUM(s * x)", False, lambda r: (0.0, r["s"])),
    "constant": Objective("MAXIMIZE SUM(s * x + 5)", True, lambda r: (0.0, r["s"])),
    "constant_column": Objective("MAXIMIZE SUM(s * x + l)", True, lambda r: (0.0, r["s"])),
    "factors": Objective("MAXIMIZE 2 * SUM(s * x) / 4", True, lambda r: (0.0, 0.5 * r["s"])),
    "parts": Objective("MAXIMIZE SUM(s * x) - SUM(y * x)", True, lambda r: (0.0, r["s"] - r["y"])),
    "null_score": Objective("MAXIMIZE SUM(s * x)", True, lambda r: (0.0, r["s"] or 0.0)),
    "part_when": Objective("MAXIMIZE SUM(when f: s * x)", True, lambda r: _when(r, (0.0, r["s"]))),
    "objective_when": Objective("MAXIMIZE when f: SUM(s * x)", True, lambda r: _when(r, (0.0, r["s"]))),
    "parts_when": Objective(
        "MAXIMIZE SUM(when f: s * x) + SUM(when NOT f: y * x)",
        True,
        lambda r: (0.0, r["s"] if r["f"] else r["y"]),
    ),
    "square": Objective("MINIMIZE SUM(POWER(x - y, 2))", False, lambda r: (1.0, -2.0 * r["y"])),
    "negated_square": Objective("MAXIMIZE -SUM(POWER(x - y, 2))", True, lambda r: (-1.0, 2.0 * r["y"])),
    "negated_term": Objective("MAXIMIZE SUM(-POWER(x - y, 2))", True, lambda r: (-1.0, 2.0 * r["y"])),
    "square_times": Objective("MINIMIZE SUM(x * x - 2 * y * x)", False, lambda r: (1.0, -2.0 * r["y"])),
    "square_slope": Objective("MINIMIZE SUM(POWER(2 * x - y, 2))", False, lambda r: (4.0, -4.0 * r["y"])),
    "square_factor": Objective("MINIMIZE 3 * SUM(POWER(x - y, 2))", False, lambda r: (3.0, -6.0 * r["y"])),
    "square_divided": Objective("MINIMIZE SUM(POWER(x - y, 2)) / 2", False, lambda r: (0.5, -r["y"])),
    "square_scaled_term": Objective("MINIMIZE SUM(5 * POWER(x - y, 2))", False, lambda r: (5.0, -10.0 * r["y"])),
    "square_constant": Objective("MINIMIZE SUM(POWER(x - y, 2) + 5)", False, lambda r: (1.0, -2.0 * r["y"])),
    "square_linear": Objective("MINIMIZE SUM(POWER(x - y, 2) + s * x)", False, lambda r: (1.0, r["s"] - 2.0 * r["y"])),
    "square_other_part": Objective("MINIMIZE SUM(POWER(x, 2)) + SUM(s * x)", False, lambda r: (1.0, r["s"])),
    "square_when": Objective("MINIMIZE SUM(when f: POWER(x - y, 2))", False, lambda r: _when(r, (1.0, -2.0 * r["y"]))),
    "square_objective_when": Objective(
        "MINIMIZE when f: SUM(POWER(x - y, 2) + s * x)",
        False,
        lambda r: _when(r, (1.0, r["s"] - 2.0 * r["y"])),
    ),
    # A NULL target skips its term, so the row has no square at all.
    "null_target": Objective(
        "MINIMIZE SUM(POWER(x - y, 2))", False, lambda r: (0.0, 0.0) if r["y"] is None else (1.0, -2.0 * r["y"])
    ),
}


# ---------------------------------------------------------------------------
# Cases
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class Case:
    rows: tuple
    ball: str
    objective: str
    free: bool = False
    #: The box as written: `x >= 0`, which is the default.
    spelled_box: bool = False


N1_CASES = {}


def add(name, rows, ball, objective, **kwargs):
    assert name not in N1_CASES, name
    N1_CASES[name] = Case(rows, ball, objective, **kwargs)


# A linear objective. Each case differs from the first in what decides the branch it reaches.
add("mixed_scores", MIXED, "b25", "max")
add("mixed_scores_free", MIXED, "b25", "max", free=True)
add("mixed_scores_box_written", MIXED, "b25", "max", spelled_box=True)
add("minimize_is_the_negated_direction", MIXED, "b25", "min")
add("no_row_gains_stays_at_zero", NONPOSITIVE, "b25", "max")
add("all_scores_zero", ALL_ZERO, "b25", "max")
add("limit_zero", MIXED, "b0", "max")
add("limit_zero_square", MIXED, "b0", "square")
add("limit_negative", MIXED, "negative", "max")
add("no_objective", MIXED, "b25", "none")
add("constant_term", MIXED, "b25", "constant")
add("constant_column_term", MIXED, "b25", "constant_column")
add("constant_factors", MIXED, "b25", "factors")
add("several_parts", MIXED, "b25", "parts")
add("part_when", WHEN_STILL, "b25", "part_when")
add("objective_when", WHEN_STILL, "b25", "objective_when")
add("parts_when", MIXED, "b25", "parts_when")
add("null_when_on_a_part", WHEN_NULL, "b25", "part_when")
add("null_score_skips_its_term", NULL_SCORE, "b25", "null_score")

# A square. Inside and outside the ball, with negative targets, in both domains.
add("projection_inside", NEAR, "b25", "square")
add("projection_outside", MIXED, "b4", "square")
add("projection_outside_free", MIXED, "b4", "square", free=True)
add("projection_negative_targets_stay_at_zero", NONPOSITIVE, "b4", "square")
add("negated_square_maximized", MIXED, "b4", "negated_square")
add("negated_term_maximized", MIXED, "b4", "negated_term")
add("square_written_as_a_product", MIXED, "b4", "square_times")
add("square_with_a_slope", MIXED, "b4", "square_slope")
add("square_with_a_factor", MIXED, "b4", "square_factor")
add("square_divided", MIXED, "b4", "square_divided")
add("square_scaled_term", MIXED, "b4", "square_scaled_term")
add("square_constant", MIXED, "b4", "square_constant")
add("square_plus_linear", MIXED, "b4", "square_linear")
add("square_plus_linear_in_another_part", MIXED, "b4", "square_other_part")
add("square_under_part_when", MIXED, "b4", "square_when")
add("square_under_objective_when", MIXED, "b4", "square_objective_when")
add("null_target_has_no_square", NULL_TARGET, "b4", "null_target")

# The spellings of the limit.
add("spelled_times", MIXED, "times", "max")
add("spelled_stars", MIXED, "stars", "max")
add("spelled_norm", MIXED, "norm", "max")
add("spelled_scaled_square", MIXED, "scaled_square", "max")
add("spelled_inner_slope", MIXED, "inner_slope", "max")
add("spelled_inner_negative_slope", MIXED, "inner_negative_slope", "max")
add("spelled_literal_on_right", MIXED, "literal_on_right", "max")

# Groups.
add("by_key", MIXED, "by", "max")
add("by_key_square", MIXED, "by", "square")
add("per_key", MIXED, "per", "max")
add("per_key_square", MIXED, "per", "square")
add("per_key_by_key", MIXED, "per_by", "max")
add("per_key_by_key_square", MIXED, "per_by", "square")

# NULL keys: a row outside every ball, improving or not.
add("null_key_improving_row_is_unbounded", NULL_KEY_IMPROVING, "by", "max")
add("null_key_still_row_stays_at_zero", NULL_KEY_STILL, "by", "max")
add("null_key_square_keeps_its_target", NULL_KEY_IMPROVING, "by", "square")
add("null_key_square_free", NULL_KEY_STILL, "by", "square", free=True)
add("null_key_no_objective", NULL_KEY_STILL, "by", "none")
add("per_key_null_key_is_still_in_the_sum", NULL_KEY_IMPROVING, "per", "max")
add("per_key_by_key_null_key_is_outside", NULL_KEY_IMPROVING, "per_by", "max")
add("per_key_no_key_has_no_instance", NULL_KEY_ONLY, "per", "max")
add("per_key_no_key_still_rows", NULL_KEY_ONLY_STILL, "per", "max")
add("per_key_no_key_negative_limit_is_no_instance", NULL_KEY_ONLY_STILL, "per", "square")

# A `when` drops rows from the ball.
add("clause_when_still_row", WHEN_STILL, "when", "max")
add("clause_when_improving_row_is_unbounded", WHEN_IMPROVING, "when", "max")
add("clause_when_square_keeps_the_target", MIXED, "when", "square")
add("clause_when_null_keeps_no_row", WHEN_NULL, "when", "max")
add("sum_when_still_row", WHEN_STILL, "sum_when", "max")
add("sum_when_improving_row_is_unbounded", WHEN_IMPROVING, "sum_when", "max")
add("sum_when_square", MIXED, "sum_when", "square")
add("when_and_by", MIXED, "when_by", "square")

# Limits read from the data.
add("column_limit_tightest_value", MIXED, "column", "max")
add("column_limit_scaled", MIXED, "column_scaled", "max")
add("column_limit_per_group", MIXED, "column_by", "max")
add("column_limit_per_group_square", MIXED, "column_by", "square")
add("column_limit_null_in_a_group_is_no_limit", NULL_LIMITS, "column_by", "square")
add("column_limit_null_group_improving_is_unbounded", NULL_LIMITS, "column_by", "max", free=True)
add("column_limit_negative_in_one_group", NEGATIVE_IN_ONE_GROUP, "column_by", "max")
add("column_limit_under_clause_when", MIXED, "column_when", "square")
add("column_limit_under_sum_when", MIXED, "column_sum_when", "square")
add("limit_expression_over_a_column", MIXED, "column_expression", "max")
add("limit_aggregate_under_sum_when", MIXED, "aggregate_sum_when", "square")
add("aggregate_limit_per_group", MIXED, "sum_by", "max")
add("aggregate_limit_of_a_group_of_nulls_is_no_limit", NULL_LIMITS, "sum_by", "square")
add("aggregate_limit_negative_in_one_group", NEGATIVE_IN_ONE_GROUP, "sum_by", "max")
add("aggregate_limit_null_key_square", NULL_KEY_IMPROVING, "sum_by", "square")
add("aggregate_limit_of_all_rows", MIXED, "half_sum", "max")
# A limit input with no argument (COUNT(*)): the plan lays out no argument column, in P1 and in P2.
add("aggregate_limit_count_star_has_no_argument", MIXED, "count_all", "max")

# A constant factor other than 1 with a square: the ball is SUM(x^2) <= B / q, and a target inside it stays.
add("projection_inside_a_scaled_ball", NEAR, "scaled_square", "square")
add("projection_outside_a_scaled_ball", MIXED, "scaled_square", "square")
add("projection_outside_a_scaled_ball_free", MIXED, "scaled_square", "square", free=True)
add("projection_outside_a_sloped_ball", MIXED, "inner_slope", "square")

# A factor per row. The first row of the worked example, and unequal weights in both domains.
add("weighted_worked_example", WORKED, "root_b5", "max")
add("weighted_unequal_weights", WEIGHTED, "root", "max")
add("weighted_unequal_weights_free", WEIGHTED, "root", "max", free=True)
add("weighted_slope_spelling", WEIGHTED, "slope", "max")
add("weighted_slope_with_a_constant_coefficient", WEIGHTED, "slope_scaled", "max")
add("weighted_minimize_is_the_negated_direction", WEIGHTED, "root", "min")
add("weighted_minimize_free", WEIGHTED, "root", "min", free=True)
add("weighted_no_objective", WEIGHTED, "root", "none")
add("weighted_limit_zero", WEIGHTED, "root_b0", "max")
add("weighted_limit_negative", WEIGHTED, "root_negative", "max")
# A row whose weight is 0 or NULL has no square in the sum.
add("weighted_zero_weight_with_zero_direction_stays_at_zero", WEIGHT_ZERO_STILL, "root", "max")
add("weighted_zero_weight_with_a_direction_is_unbounded", WEIGHT_ZERO_IMPROVING, "root", "max")
add("weighted_null_weight_with_zero_direction_stays_at_zero", WEIGHT_NULL_STILL, "root", "max")
add("weighted_null_weight_with_a_direction_is_unbounded", WEIGHT_NULL_IMPROVING, "root", "max")
# Groups, a `when`, a NULL key and a limit from the data.
add("weighted_by_key", WEIGHTED_KEYS, "root_by", "max")
add("weighted_per_key", WEIGHTED_KEYS, "root_per", "max")
add("weighted_clause_when", WEIGHTED, "root_when", "max")
add("weighted_negative_weight_on_a_row_the_when_drops", WEIGHT_NEGATIVE_WHEN_DROPS, "root_when", "max")
add("weighted_null_key_improving_row_is_unbounded", WEIGHTED_NULL_KEY, "root_by", "max")
add("weighted_column_limit_per_group", WEIGHTED_LIMITS, "root_column_by", "max")


def _sql_body(case_):
    ball = BALLS[case_.ball]
    clauses = [ball.sql]
    if case_.free:
        clauses.insert(0, "x >= -1e30")
    if case_.spelled_box:
        clauses.insert(0, "x >= 0")
    return f"DECIDE x(REAL) SUCH THAT {' AND '.join(clauses)} {OBJECTIVES[case_.objective].sql}"


def _source(case_):
    return f"(VALUES {_values(case_.rows)}) t({', '.join(NAMES)})"


def _sql(case_, wrap="plain"):
    body = _sql_body(case_)
    if wrap == "plain":
        return f"SELECT id, x FROM (FROM {_source(case_)} {body}) q ORDER BY id"
    if wrap == "joined_source":
        ids = ", ".join(f"({row['id']})" for row in case_.rows)
        return (
            f"SELECT id, x FROM (FROM {_source(case_)} JOIN (VALUES {ids}) j(jid) ON t.id = j.jid {body}) q ORDER BY id"
        )
    if wrap == "cte":
        return f"WITH c AS (SELECT * FROM {_source(case_)}) SELECT id, x FROM (FROM c {body}) q ORDER BY id"
    assert wrap == "aggregate_above", wrap
    return f"SELECT id, x FROM (SELECT id, x FROM (FROM {_source(case_)} {body}) q) p ORDER BY id"


def _groups(case_):
    """The groups of the ball that exist: {key: (rows, limit)}. A group with no limit is not a ball."""
    ball = BALLS[case_.ball]
    if not ball.instance(case_.rows):
        return {}
    members = {}
    for row in case_.rows:
        key = ball.group(row)
        if key is not None:
            members.setdefault(key, []).append(row)
    groups = {key: (rows, ball.limit(rows, case_.rows)) for key, rows in members.items()}
    return {key: group for key, group in groups.items() if group[1] is not None}


def _solve_with_oracle(oracle_solver, case_):
    oracle_solver.create_model("direct_n1_euclidean_ball")
    names = {}
    for row in case_.rows:
        names[row["id"]] = f"x_{row['id']}"
        oracle_solver.add_variable(names[row["id"]], VarType.CONTINUOUS, -BIG if case_.free else 0.0, BIG)
    ball = BALLS[case_.ball]
    for index, (rows, limit) in enumerate(_groups(case_).values()):
        quadratic = {(names[row["id"]], names[row["id"]]): ball.factor(row) for row in rows if ball.factor(row) != 0.0}
        oracle_solver.add_quadratic_constraint(
            linear={}, quadratic=quadratic, sense="<=", rhs=limit, name=f"ball_{index}"
        )
    objective = OBJECTIVES[case_.objective]
    sense = ObjSense.MAXIMIZE if objective.maximize else ObjSense.MINIMIZE
    if objective.coefficients is None:
        oracle_solver.set_objective({}, sense)
    else:
        coefficients = [(names[row["id"]], objective.coefficients(row)) for row in case_.rows]
        quadratic = {(name, name): k for name, (k, _) in coefficients if k != 0.0}
        linear = {name: b for name, (_, b) in coefficients}
        oracle_solver.set_quadratic_objective(linear=linear, quadratic=quadratic, sense=sense)
    return oracle_solver.solve()


def _check_assignment(case_, rows, columns, oracle, mode):
    assert columns == ["id", "x"], mode
    assert [row[0] for row in rows] == [row["id"] for row in case_.rows], mode
    values = {row[0]: row[1] for row in rows}
    for source in case_.rows:
        value = values[source["id"]]
        assert isinstance(value, float), (mode, source["id"], value)
        assert case_.free or value >= -_BOX_TOLERANCE, (mode, source["id"], value)
    ball = BALLS[case_.ball]
    tolerance = _DIRECT_BALL_TOLERANCE if mode == "require" else _BALL_TOLERANCE
    for key, (members, limit) in _groups(case_).items():
        length = sum(ball.factor(row) * values[row["id"]] ** 2 for row in members)
        assert length <= limit + tolerance * max(1.0, abs(limit)), (mode, key, length, limit)
    objective = OBJECTIVES[case_.objective]
    if objective.coefficients is None:
        return
    total = 0.0
    for source in case_.rows:
        k, b = objective.coefficients(source)
        value = values[source["id"]]
        total += k * value * value + b * value
    expected = oracle.objective_value
    assert abs(total - expected) <= _OBJECTIVE_TOLERANCE * max(1.0, abs(expected)), (mode, total, expected)


def _agree(decidb_cli, oracle_solver, case_, wrap="plain"):
    """Runs the case on the oracle and on both DeciDB paths, and checks they agree."""
    sql = _sql(case_, wrap)
    oracle = _solve_with_oracle(oracle_solver, case_)

    def check_assignment(rows, columns, mode):
        _check_assignment(case_, rows, columns, oracle, mode)

    agree_with_oracle(decidb_cli, sql, oracle, check_assignment)


@pytest.mark.var_real
@pytest.mark.cons_aggregate
@pytest.mark.quadratic
@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(N1_CASES))
def test_n1_oracle_solver_and_direct_agree(decidb_cli, oracle_solver, name):
    _agree(decidb_cli, oracle_solver, N1_CASES[name])


# The same problem inside a parent query: the rule reads through a join and a CTE in its source, and its columns pass
# through a projection above it.
CONTEXT_CASES = [
    ("mixed_scores", "joined_source"),
    ("by_key_square", "joined_source"),
    ("column_limit_per_group", "cte"),
    ("projection_outside", "aggregate_above"),
    ("weighted_unequal_weights", "joined_source"),
]


@pytest.mark.quadratic
@pytest.mark.correctness
@pytest.mark.parametrize("base,wrap", CONTEXT_CASES)
def test_n1_agrees_inside_a_parent_query(decidb_cli, oracle_solver, base, wrap):
    _agree(decidb_cli, oracle_solver, N1_CASES[base], wrap=wrap)


@pytest.mark.correctness
@pytest.mark.parametrize("limit", ["-1", "25", "l"])
@pytest.mark.parametrize("objective", ["MAXIMIZE SUM(s*x)", "MINIMIZE SUM(POWER(x - s, 2))", ""])
def test_n1_empty_source_returns_no_rows_whatever_the_limit_says(decidb_cli, limit, objective):
    # The checks run on rows, and there are none: a negative limit has no instance to make infeasible.
    sql = (
        "SELECT id, x FROM (SELECT 1 AS id, 1.0::DOUBLE AS s, 1.0::DOUBLE AS l WHERE false) "
        f"DECIDE x(REAL) SUCH THAT SUM(POWER(x, 2)) <= {limit} {objective}"
    )
    for mode in ("off", "require"):
        assert run(decidb_cli, sql, mode)[0] == []


# ---------------------------------------------------------------------------
# Errors: both paths fail, in the same class. `test_direct_user_facing.py` compares the text word for word.
# ---------------------------------------------------------------------------

_LINEAR = "SUM(POWER(x, 2)) <= 25 MAXIMIZE SUM(s * x)"
_SQUARE = "SUM(POWER(x, 2)) <= 25 MINIMIZE SUM(POWER(x - y, 2))"
_WEIGHTED = "SUM(POWER(SQRT(w) * x, 2)) <= 25 MAXIMIZE SUM(s * x)"

ERROR_CASES = {
    "score_nan": ("invalid_value", "'NaN'", "2.0", "25.0", "'a'", _LINEAR),
    "score_infinite": ("invalid_value", "'Infinity'", "2.0", "25.0", "'a'", _LINEAR),
    "score_minus_infinite": ("invalid_value", "'-Infinity'", "2.0", "25.0", "'a'", _LINEAR),
    "target_nan": ("invalid_value", "1.0", "'NaN'", "25.0", "'a'", _SQUARE),
    "target_infinite": ("invalid_value", "1.0", "'Infinity'", "25.0", "'a'", _SQUARE),
    "constant_term_nan": (
        "invalid_value",
        "1.0",
        "'NaN'",
        "25.0",
        "'a'",
        "SUM(POWER(x, 2)) <= 25 MAXIMIZE SUM(s * x + y)",
    ),
    # A limit of one value is checked by the driver's row check; one that differs between rows is left as its group's
    # tightest value, with the check of what its aggregates read. A sum of limits that is NaN is the group's own check.
    "limit_nan_one_row": ("null_bound", "1.0", "2.0", "'NaN'", "'a'", "SUM(POWER(x, 2)) <= l MAXIMIZE SUM(s * x)"),
    "limit_nan_tightest": (
        "null_bound",
        "1.0",
        "2.0",
        "CASE WHEN i = 1 THEN 'NaN' ELSE '4.0' END",
        "'a'",
        "SUM(POWER(x, 2)) by (k) <= l MAXIMIZE SUM(s * x)",
    ),
    "limit_nan_sum": (
        "null_bound",
        "1.0",
        "2.0",
        "CASE WHEN i = 0 THEN '-Infinity' ELSE 'Infinity' END",
        "'a'",
        "SUM(POWER(x, 2)) <= SUM(l) MAXIMIZE SUM(s * x)",
    ),
    "limit_minus_infinite": (
        "infeasible",
        "1.0",
        "2.0",
        "'-Infinity'",
        "'a'",
        "SUM(POWER(x, 2)) <= l MAXIMIZE SUM(s * x)",
    ),
    "null_key_improving": (
        "unbounded",
        "1.0",
        "2.0",
        "25.0",
        "NULL",
        "SUM(POWER(x, 2)) by (k) <= 25 MAXIMIZE SUM(s * x)",
    ),
    "null_limit_improving": ("unbounded", "1.0", "2.0", "NULL", "'a'", "SUM(POWER(x, 2)) <= l MAXIMIZE SUM(s * x)"),
    # In a free domain a row outside every ball moves whichever sign its score has. The oracle's Gurobi stops on this
    # model with a numerical status rather than a verdict, so it is not an oracle case.
    "null_key_free_domain": (
        "unbounded",
        "-1.0",
        "2.0",
        "25.0",
        "NULL",
        "x >= -1e30 AND SUM(POWER(x, 2)) by (k) <= 25 MAXIMIZE SUM(s * x)",
    ),
    # A weight is read on every row the clause keeps: a negative one under SQRT raises DuckDB's own error, and a NaN one
    # the number check of the slope, naming the expression.
    "weight_negative_under_sqrt": (
        "cannot take square root",
        "2.0",
        "2.0",
        "25.0",
        "'a'",
        _WEIGHTED,
        "CASE WHEN i = 1 THEN '-4.0' ELSE '1.0' END",
    ),
    "weight_nan_under_sqrt": ("invalid_value", "2.0", "2.0", "25.0", "'a'", _WEIGHTED, "'NaN'"),
    "weight_nan_as_the_slope": (
        "invalid_value",
        "2.0",
        "2.0",
        "25.0",
        "'a'",
        "SUM(POWER(w * x, 2)) <= 25 MAXIMIZE SUM(s * x)",
        "'NaN'",
    ),
    "weight_infinite_as_the_slope": (
        "invalid_value",
        "2.0",
        "2.0",
        "25.0",
        "'a'",
        "SUM(POWER(w * x, 2)) <= 25 MAXIMIZE SUM(s * x)",
        "'Infinity'",
    ),
}


def _error_sql(score, target, limit, key, decide, weight="1.0"):
    def typed(value):
        return f"CAST({value} AS DOUBLE)" if value != "NULL" else "CAST(NULL AS DOUBLE)"

    source = (
        f"SELECT i AS id, {typed(score)} AS s, {typed(target)} AS y, {typed(limit)} AS l, {key}::VARCHAR AS k, "
        f"{typed(weight)} AS w FROM range(3) t(i)"
    )
    return f"SELECT id, x FROM ({source}) DECIDE x(REAL) SUCH THAT {decide}"


@pytest.mark.error
@pytest.mark.parametrize("name", sorted(ERROR_CASES))
def test_n1_both_paths_raise_the_same_error(decidb_cli, name):
    expected, *arguments = ERROR_CASES[name]
    sql = _error_sql(*arguments)
    direct = outcome(decidb_cli, sql, "require")
    solver = outcome(decidb_cli, sql, "off")
    assert direct[0] == "error", direct
    assert direct == solver, (direct, solver)
    assert expected in direct[1], direct


@pytest.mark.error
def test_n1_a_factor_past_a_double_raises_the_solvers_error(decidb_cli):
    # The check of q in P3: w = 1e200 squares to 1e400, a number the solver names too. The row's own slope is a finite
    # number, so only the factor q = w^2 is past a DOUBLE.
    sql = (
        "SELECT id, x FROM (FROM (VALUES (1, 3.0::DOUBLE, 1e200::DOUBLE), (2, 4.0::DOUBLE, 1.0::DOUBLE)) t(id, s, w) "
        "DECIDE x(REAL) SUCH THAT SUM(POWER(w * x, 2)) <= 25 MAXIMIZE SUM(s * x)) ORDER BY id"
    )
    direct = decidb_cli.execute_raw(f"SET decide_direct_solve='require'; {sql}").stderr
    solver = decidb_cli.execute_raw(f"SET decide_direct_solve='off'; {sql}").stderr
    assert "SUM(POWER(w * x, 2)) <= 25 computes a number too large for a DOUBLE" in direct, direct
    assert direct == solver, (direct, solver)


# ---------------------------------------------------------------------------
# The numeric boundary: the direct path against the solver alone, since the oracle is the solver's own engine.
# ---------------------------------------------------------------------------


def _two_rows(first, second, decide):
    return (
        f"SELECT x FROM (FROM (VALUES (1, {first}::DOUBLE), (2, {second}::DOUBLE)) t(id, s) "
        f"DECIDE x(REAL) SUCH THAT {decide}) q ORDER BY id"
    )


def _direct_values(cli, sql, number_range=False):
    return [row[0] for row in run(cli, sql, "require", expect_number_range=number_range)[0]]


def _solver_values(cli, sql, number_range=False):
    """The solver's x values from its printed table. Past some sizes it also warns that it could not prove the answer is
    the best, which `execute` raises as an error."""
    raw = cli.execute_raw(f"SET decide_direct_solve='off'; {sql}", expect_number_range=number_range)
    return [float(number) for number in re.findall(r"-?\d+\.\d+(?:e[-+]?\d+)?", raw.stdout)]


# The boundary rows run with the factor on the square that the limit is written with, 1 and then a constant other than 1
# (`SUM(2 * POWER(x, 2))`): the factor q is in each row's point d / q and in its share d * (d / q) of h, so the radius
# of the ball is sqrt(B) whatever q is. A row whose numbers sit on the edge of a DOUBLE at q = 1 sits on another at
# q = 2, so those rows name their own scores for each factor.
FACTORS = pytest.mark.parametrize("factor,q", [("", 1.0), ("2 * ", 2.0)], ids=["q1", "q2"])


def _max_25(factor):
    return f"SUM({factor}POWER(x, 2)) <= 25 MAXIMIZE SUM(s * x)"


@pytest.mark.correctness
@FACTORS
@pytest.mark.parametrize("scale", ["1e153", "1e-150"])
def test_n1_formula_stays_in_range_where_the_squares_do(decidb_cli, scale, factor, q):
    # The sum of squares stays inside the DOUBLE range, so scores this size give the answer of scores 3 and 4 on the
    # ball of radius 5 / sqrt(q). Gurobi returns x = 0 on each (done.md § Differences from the solver), so this is not
    # a comparison with the solver.
    sql = _two_rows(f"3 * {scale}", f"4 * {scale}", _max_25(factor))
    assert _direct_values(decidb_cli, sql) == pytest.approx([3.0 / math.sqrt(q), 4.0 / math.sqrt(q)], rel=1e-12)


@pytest.mark.correctness
@FACTORS
def test_n1_scores_past_the_range_of_their_squares_raise_rather_than_answer_zero(decidb_cli, factor, q):
    # h is past a DOUBLE from finite inputs: the direct path raises, where Gurobi returns x = 0 with a warning.
    sql = _two_rows("3e154", "4e154", _max_25(factor))
    direct = decidb_cli.execute_raw(f"SET decide_direct_solve='require'; {sql}").stderr
    assert "MAXIMIZE SUM(s * x) computes a number too large for a DOUBLE" in direct
    assert _solver_values(decidb_cli, sql, number_range=True) == [0.0, 0.0]


# Scores whose shares d * (d / q) are each in range and whose sum h is past a DOUBLE: 1.2e154 squared is 1.44e308, and
# 1.7e154 times its half is 1.445e308.
_SHARES_IN_RANGE = [pytest.param("", "1.2e154", id="q1"), pytest.param("2 * ", "1.7e154", id="q2")]


@pytest.mark.correctness
@pytest.mark.parametrize("factor,score", _SHARES_IN_RANGE)
def test_n1_a_sum_of_squares_past_a_double_raises_though_each_square_is_in_range(decidb_cli, factor, score):
    # Each share is in range; only h is past a DOUBLE, a check above the window.
    sql = _two_rows(score, score, _max_25(factor))
    direct = decidb_cli.execute_raw(f"SET decide_direct_solve='require'; {sql}").stderr
    assert "MAXIMIZE SUM(s * x) computes a number too large for a DOUBLE" in direct


# 5,000 rows: the first 4,998 are in group `a`, whose h passes; the last two are in group `b`, whose h is past a DOUBLE
# though each square is in range. A vector holds 2,048 rows, so a `LIMIT 1` is met by the first vector, which holds no row
# of `b`.
def _one_group_overflows(factor, score):
    return (
        "FROM (SELECT i, CASE WHEN i < 4998 THEN 'a' ELSE 'b' END AS k, "
        f"(CASE WHEN i < 4998 THEN 1.0 ELSE {score} END)::DOUBLE AS s FROM range(5000) t(i)) "
        f"DECIDE x(REAL) SUCH THAT SUM({factor}POWER(x, 2)) by (k) <= 25 MAXIMIZE SUM(s * x)"
    )


@pytest.mark.correctness
@pytest.mark.parametrize("factor,score", _SHARES_IN_RANGE)
@pytest.mark.parametrize("read", ["SELECT count(*) FROM (", "SELECT x FROM ("])
@pytest.mark.parametrize("tail", ["", " LIMIT 1"])
def test_n1_a_group_whose_h_passes_a_double_raises_though_no_streamed_row_reads_it(
    decidb_cli, read, tail, factor, score
):
    # h is checked when the groups are built (the group's row, above the aggregate), so the group raises whether or not
    # a row of it is among the rows read: the solver reads every row, and raises for the one group of several.
    sql = f"{read}{_one_group_overflows(factor, score)}) q{tail}"
    direct = decidb_cli.execute_raw(f"SET decide_direct_solve='require'; {sql}").stderr
    assert "MAXIMIZE SUM(s * x) computes a number too large for a DOUBLE" in direct


@pytest.mark.correctness
def test_n1_a_target_past_a_double_raises_on_a_row_outside_every_ball(decidb_cli):
    # Nothing squares the target of a row no ball holds, so only the check of the target itself reads it.
    sql = (
        "SELECT x FROM (FROM (VALUES (1, 1e10::DOUBLE, NULL::VARCHAR)) t(id, s, k) DECIDE x(REAL) "
        "SUCH THAT SUM(POWER(x, 2)) by (k) <= 9 MINIMIZE SUM(1e-300 * POWER(x, 2) - s * x)) q"
    )
    direct = decidb_cli.execute_raw(f"SET decide_direct_solve='require'; {sql}").stderr
    assert "computes a number too large for a DOUBLE" in direct


@pytest.mark.correctness
@FACTORS
def test_n1_a_zero_answer_has_no_sign(decidb_cli, factor, q):
    # A free domain keeps the direction as it is, and -0 is what `MINIMIZE` makes of a score of 0. DeciDB's solver path
    # returns 0, and the difference would show in the printed row.
    sql = (
        "SELECT x FROM (FROM (VALUES (1, 0.0::DOUBLE), (2, 1.0::DOUBLE)) t(id, s) DECIDE x(REAL) "
        f"SUCH THAT x >= -1e30 AND SUM({factor}POWER(x, 2)) <= 25 MINIMIZE SUM(s * x)) q ORDER BY id"
    )
    assert math.copysign(1.0, _direct_values(decidb_cli, sql)[0]) == 1.0


def _on_the_ball(values, q, limit):
    """Whether sqrt(q) * |x| is the radius sqrt(limit), to rounding. `hypot` stays in range where the squares do not."""
    return math.sqrt(q) * math.hypot(*values) == pytest.approx(math.sqrt(limit), rel=1e-12)


@pytest.mark.correctness
def test_n1_a_data_limit_over_a_small_factor_has_a_ball_though_b_over_q_passes_a_double(decidb_cli):
    # The oracle's Gurobi reads a right side of 1e20 or more as infinite (status INF_OR_UNBD at B = 1e100), so this
    # case is checked as a property: the maximum of positive scores lies on the ball. B / q = 1e310 is infinite.
    sql = (
        "SELECT x FROM (FROM (VALUES (1, 3.0::DOUBLE, 1e300::DOUBLE), (2, 4.0::DOUBLE, 1e300::DOUBLE)) t(id, s, l) "
        "DECIDE x(REAL) SUCH THAT SUM(1e-10 * POWER(x, 2)) <= l MAXIMIZE SUM(s * x)) q ORDER BY id"
    )
    values = _direct_values(decidb_cli, sql)
    assert _on_the_ball(values, 1e-10, 1e300), values


@pytest.mark.correctness
def test_n1_a_subnormal_h_under_a_huge_limit_stays_inside_the_ball(decidb_cli):
    # Scores 2^-537 and 2^-536 square to 2^-1074 and 2^-1072, so h = 5 * 2^-1074 is subnormal and exact: only the
    # scale is under test. sqrt(B) / sqrt(h) passes a DOUBLE for B = 1e300, but (value / sqrt(h)) * sqrt(B) does not.
    # A property check, for the oracle's reason above. The plan warns that h is below the normal range.
    sql = (
        "SELECT x FROM (FROM (VALUES (1, POWER(2.0::DOUBLE, -537), 1e300::DOUBLE), "
        "(2, POWER(2.0::DOUBLE, -536), 1e300::DOUBLE)) t(id, s, l) "
        "DECIDE x(REAL) SUCH THAT SUM(POWER(x, 2)) <= l MAXIMIZE SUM(s * x)) q ORDER BY id"
    )
    values = _direct_values(decidb_cli, sql, number_range=True)
    assert _on_the_ball(values, 1.0, 1e300), values


def _objective(scores, values):
    return sum(score * value for score, value in zip(scores, values))


@pytest.mark.quadratic
@pytest.mark.correctness
def test_n1_a_huge_limit_over_a_small_factor_is_the_unit_ball_scaled(decidb_cli, oracle_solver):
    # The oracle solves the same scores on the unit ball (B = 1, q = 1). Scaling every x by sqrt(B / q) maps its answer
    # onto the ball of limit 1e300 and factor 1e-10, so the direct objective divided by that radius equals the oracle's.
    # B / q = 1e310 passes a DOUBLE, so the radius is written sqrt(B) / sqrt(q).
    unit = Case((_row(1, 3.0), _row(2, 4.0)), "b1", "max")
    oracle = _solve_with_oracle(oracle_solver, unit)
    sql = (
        "SELECT x FROM (FROM (VALUES (1, 3.0::DOUBLE, 1e300::DOUBLE), (2, 4.0::DOUBLE, 1e300::DOUBLE)) t(id, s, l) "
        "DECIDE x(REAL) SUCH THAT SUM(1e-10 * POWER(x, 2)) <= l MAXIMIZE SUM(s * x)) q ORDER BY id"
    )
    values = _direct_values(decidb_cli, sql)
    radius = math.sqrt(1e300) / math.sqrt(1e-10)
    objective = _objective((3.0, 4.0), values) / radius
    assert abs(objective - oracle.objective_value) <= _OBJECTIVE_TOLERANCE * max(1.0, abs(oracle.objective_value))
    assert _on_the_ball(values, 1e-10, 1e300), values


@pytest.mark.quadratic
@pytest.mark.correctness
def test_n1_a_subnormal_h_under_a_huge_limit_is_the_unit_ball_scaled(decidb_cli, oracle_solver):
    # The oracle solves scores 1 and 2 on the unit ball. Scores 2^-537 and 2^-536 are those times 2^-537, and their
    # squares sum to the subnormal h = 5 * 2^-1074. The answer on B = 1e300 is the oracle's times sqrt(B), so the direct
    # objective divided by 2^-537 * sqrt(B) equals the oracle's. The plan warns that h is below the normal range.
    unit = Case((_row(1, 1.0), _row(2, 2.0)), "b1", "max")
    oracle = _solve_with_oracle(oracle_solver, unit)
    sql = (
        "SELECT x FROM (FROM (VALUES (1, POWER(2.0::DOUBLE, -537), 1e300::DOUBLE), "
        "(2, POWER(2.0::DOUBLE, -536), 1e300::DOUBLE)) t(id, s, l) "
        "DECIDE x(REAL) SUCH THAT SUM(POWER(x, 2)) <= l MAXIMIZE SUM(s * x)) q ORDER BY id"
    )
    values = _direct_values(decidb_cli, sql, number_range=True)
    score_scale = 2.0**-537
    objective = _objective((score_scale, 2 * score_scale), values) / (score_scale * math.sqrt(1e300))
    assert abs(objective - oracle.objective_value) <= _OBJECTIVE_TOLERANCE * max(1.0, abs(oracle.objective_value))


@pytest.mark.quadratic
@pytest.mark.correctness
def test_n1_a_square_factor_of_1e308_has_the_target_of_the_factor_one_problem(decidb_cli, oracle_solver):
    # Multiplying the objective by a positive constant leaves its minimizer where it was, so the oracle solves the
    # factor-1 problem. Targets stay under 0.89 so that b = -2 * 1e308 * y is finite; 2 * 1e308 passes a DOUBLE.
    rows = (_row(1, y=0.5, k="a"), _row(2, y=0.25, k="b"), _row(3, y=0.75, k="b"), _row(4, y=-0.5, k="c"))
    unit = Case(rows, "b25", "square")
    oracle = _solve_with_oracle(oracle_solver, unit)
    sql = (
        f"SELECT id, x FROM (FROM {_source(unit)} DECIDE x(REAL) SUCH THAT {BALLS['b25'].sql} "
        "MINIMIZE SUM(1e308 * POWER(x - y, 2))) q ORDER BY id"
    )
    result, columns = run(decidb_cli, sql, "require")
    _check_assignment(unit, result, columns, oracle, "require")


# A weight on every row, from 1e-150 to 1e150. Both the oracle's Gurobi and the solver path stop answering at the ends of
# the range (unbounded below 1e-10, x = 0 above 1e50), so the direct answer is checked on the unit problem: rows of weight
# W and 4W (the worked example) are the unit rows on the scale z = SQRT(W) * x. Written as `SQRT(w) * x` the slope is
# SQRT(W) and q = W; written as `w * x` with weights W and 2W the slope scales by W and q = W^2.
_WEIGHT_SCALES = ["1e-150", "1e-100", "1e-50", "1e-10", "1e-3", "1", "1e3", "1e10", "1e50", "1e100", "1e150"]
_WEIGHT_SPELLINGS = {
    # spelling -> (the limit, the second row's weight over the first's, q of the first row, x scales by this over z)
    "root": ("SUM(POWER(SQRT(w) * x, 2)) <= 5", 4.0, lambda weight: weight, lambda weight: math.sqrt(weight)),
    "slope": ("SUM(POWER(w * x, 2)) <= 5", 2.0, lambda weight: weight**2, lambda weight: weight),
}


def _weighted_pair(scale, ratio, limit):
    return (
        f"SELECT x FROM (FROM (VALUES (1, 1.0::DOUBLE, {scale}::DOUBLE), (2, 4.0::DOUBLE, {ratio} * {scale}::DOUBLE)) "
        f"t(id, s, w) DECIDE x(REAL) SUCH THAT {limit} MAXIMIZE SUM(s * x)) q ORDER BY id"
    )


@pytest.mark.quadratic
@pytest.mark.correctness
@pytest.mark.parametrize("spelling", sorted(_WEIGHT_SPELLINGS))
@pytest.mark.parametrize("scale", _WEIGHT_SCALES)
def test_n1_weights_from_1e_150_to_1e150_are_the_unit_problem_scaled(decidb_cli, oracle_solver, spelling, scale):
    limit, ratio, q_of, unscale = _WEIGHT_SPELLINGS[spelling]
    oracle = _solve_with_oracle(oracle_solver, Case(WORKED, "root_b5", "max"))
    values = _direct_values(decidb_cli, _weighted_pair(scale, ratio, limit))
    weight = float(scale)
    # The objective on the scale z = unscale * x is the unit problem's.
    assert _objective((1.0, 4.0), values) * unscale(weight) == pytest.approx(
        oracle.objective_value, rel=_OBJECTIVE_TOLERANCE
    )
    # Both rows sit on the ball: the sum of q * x^2 is the limit, written as a norm so that no square leaves the range.
    q = (q_of(weight), q_of(weight * ratio))
    assert math.hypot(*(math.sqrt(factor) * value for factor, value in zip(q, values))) == pytest.approx(
        math.sqrt(5.0), rel=1e-12
    ), values


@pytest.mark.correctness
@pytest.mark.parametrize(
    "spelling,scale",
    # The weights the solver answers to 1e-3 and without a warning. Past them it stops: `w * x` at 1e3 warns and is 0.3%
    # off, and below 1e-10 and above 1e50 it answers unbounded or x = 0.
    [("root", "1e-3"), ("root", "1"), ("root", "1e3"), ("slope", "1e-3"), ("slope", "1")],
)
def test_n1_weights_the_solver_answers_agree_with_it(decidb_cli, spelling, scale):
    limit, ratio, _, _ = _WEIGHT_SPELLINGS[spelling]
    sql = _weighted_pair(scale, ratio, limit)
    assert _direct_values(decidb_cli, sql) == pytest.approx(_solver_values(decidb_cli, sql), rel=1e-3)


@pytest.mark.correctness
@FACTORS
def test_n1_scores_whose_squares_vanish_stay_at_zero(decidb_cli, factor, q):
    # h underflows to 0 from nonzero scores: the formula's case h = 0 is x = 0, the point the solver also returns. Both
    # paths warn.
    sql = _two_rows("3e-200", "4e-200", _max_25(factor))
    assert _direct_values(decidb_cli, sql, number_range=True) == [0.0, 0.0]
    assert _solver_values(decidb_cli, sql, number_range=True) == [0.0, 0.0]


# The warning a direct plan prints: h below the normal range of a DOUBLE, on a row that reads it. Scores 3 and 4 times the
# scale make h 25 * scale^2, so 1e-153 stays in the normal range and 1e-155 does not.
def _tiny_limit(factor):
    return f"SUM({factor}POWER(x, 2)) <= 1e-320 MINIMIZE SUM(POWER(x - s, 2))"


@pytest.mark.correctness
@FACTORS
@pytest.mark.parametrize("scale,warns", [("1e-153", False), ("1e-155", True)])
def test_n1_warns_when_h_leaves_the_normal_range(decidb_cli, scale, warns, factor, q):
    _direct_values(decidb_cli, _two_rows(f"3 * {scale}", f"4 * {scale}", _max_25(factor)), number_range=warns)


@pytest.mark.correctness
@FACTORS
def test_n1_scores_of_zero_read_no_h_and_are_silent(decidb_cli, factor, q):
    assert _direct_values(decidb_cli, _two_rows("0", "0", _max_25(factor))) == [0.0, 0.0]


@pytest.mark.correctness
@FACTORS
def test_n1_a_target_inside_the_ball_reads_no_scale_and_is_silent(decidb_cli, factor, q):
    sql = _two_rows("3e-162", "4e-162", f"SUM({factor}POWER(x, 2)) <= 25 MINIMIZE SUM(POWER(x - s, 2))")
    assert _direct_values(decidb_cli, sql) == [3e-162, 4e-162]


@pytest.mark.correctness
@FACTORS
def test_n1_a_target_outside_a_tiny_ball_reads_the_scale_and_warns(decidb_cli, factor, q):
    _direct_values(decidb_cli, _two_rows("3e-160", "4e-160", _tiny_limit(factor)), number_range=True)


# A square's turning point warns from one expression under every rule (DirectTurningPoint): a square factor, or a nonzero
# linear coefficient beside it, below the normal range of a DOUBLE. In the ball the target stays inside, so the scale (h)
# is never read: the warning is the target's alone.


@pytest.mark.correctness
@pytest.mark.parametrize(
    "objective,warns",
    [
        # a = 2e-308 is below the normal range and b = -2.3e-308 in it.
        ("MINIMIZE SUM(2e-308 * POWER(x, 2) - 2.3e-308 * x)", True),
        # a = 1 and b = -1e-310.
        ("MINIMIZE SUM(POWER(x, 2) - 1e-310 * x)", True),
        # a = 1e-300 and b = -6.7e-301 are in the normal range.
        ("MINIMIZE SUM(1e-300 * POWER(x - 0.3333333333333333, 2))", False),
        # b = 0 beside a square holds no value to lose.
        ("MINIMIZE SUM(1e-300 * POWER(x, 2))", False),
    ],
)
def test_n1_a_tiny_square_factor_warns(decidb_cli, objective, warns):
    sql = f"SELECT x FROM (FROM (VALUES (1)) t(id) DECIDE x(REAL) SUCH THAT SUM(POWER(x, 2)) <= 100 {objective}) q"
    run(decidb_cli, sql, "require", expect_number_range=warns)


@pytest.mark.correctness
def test_n1_the_target_warning_reads_every_row_though_a_limit_releases_one(decidb_cli):
    # Row 2's linear coefficient -2e-310 is below the normal range; row 1's is not. The warning reads every row before
    # the window releases one, as the checks of a row's values do, so `LIMIT 1` does not hide it.
    sql = (
        "SELECT x FROM (FROM (VALUES (1, 1.0::DOUBLE), (2, 1e-310::DOUBLE)) t(id, s) DECIDE x(REAL) "
        "SUCH THAT SUM(POWER(x, 2)) <= 100 MINIMIZE SUM(POWER(x - s, 2))) q LIMIT 1"
    )
    run(decidb_cli, sql, "require", expect_number_range=True)


@pytest.mark.correctness
def test_n1_a_row_outside_every_ball_reads_no_h_and_is_silent(decidb_cli):
    # h reads 0 and is greater than the limit, but no ball holds the row, so its answer is its own target.
    sql = (
        "SELECT x FROM (FROM (VALUES (1, 1e-170::DOUBLE, false)) t(id, s, f) DECIDE x(REAL) "
        "SUCH THAT when f: SUM(POWER(x, 2)) <= -1 MINIMIZE SUM(POWER(x - s, 2))) q"
    )
    assert _direct_values(decidb_cli, sql) == [1e-170]


@pytest.mark.correctness
def test_n1_warns_once_however_many_rows_read_h(decidb_cli):
    sql = (
        "SELECT count(*) FROM (FROM (SELECT i, 1e-170::DOUBLE AS s FROM range(200000) t(i)) "
        f"DECIDE x(REAL) SUCH THAT {_max_25('')}) q"
    )
    assert run(decidb_cli, sql, "require", expect_number_range=True)[0] == [(200000,)]


@pytest.mark.correctness
def test_n1_warns_again_on_each_query(decidb_exe_path):
    # One process and session, so the second query finds the first's state: QueryBegin must clear it.
    sql = _two_rows("3e-162", "4e-162", _max_25(""))
    script = f"SET decide_direct_solve='require'; {sql}; {sql}; PREPARE p AS {sql}; EXECUTE p; EXECUTE p;"
    done = subprocess.run([decidb_exe_path, "-noheader", "-list", "-c", script], capture_output=True, text=True)
    assert done.stderr.count("a very large or very small number may make this answer inaccurate") == 4, done.stderr


@pytest.mark.correctness
@FACTORS
@pytest.mark.parametrize("limit", ["1e6", "1e18", "1e19"])
def test_n1_large_limit_answers_as_the_solver_does(decidb_cli, limit, factor, q):
    sql = _two_rows("3", "4", f"SUM({factor}POWER(x, 2)) <= {limit} MAXIMIZE SUM(s * x)")
    direct = _direct_values(decidb_cli, sql)
    assert direct == pytest.approx(_solver_values(decidb_cli, sql, float(limit) >= 4.5e9), rel=1e-3)
    assert sum(q * value**2 for value in direct) == pytest.approx(float(limit), rel=1e-12)


@pytest.mark.correctness
@FACTORS
def test_n1_scale_stays_in_range_where_the_limit_over_h_does_not(decidb_cli, factor, q):
    # B / h is 1e319 here: the scale is computed as sqrt(B) / sqrt(h), which is 3e159.
    sql = _two_rows("6e-151", "8e-151", f"SUM({factor}POWER(x, 2)) <= 1e19 MAXIMIZE SUM(s * x)")
    assert sum(q * value**2 for value in _direct_values(decidb_cli, sql)) == pytest.approx(1e19, rel=1e-12)


@pytest.mark.correctness
@FACTORS
def test_n1_data_limit_past_the_solvers_infinity_is_a_number_to_both_paths(decidb_cli, factor, q):
    # A limit read from the data is not folded into an "effective infinity" on either path.
    sql = (
        "SELECT x FROM (FROM (VALUES (1, 3.0::DOUBLE, 1e25::DOUBLE), (2, 4.0::DOUBLE, 1e25::DOUBLE)) t(id, s, l) "
        f"DECIDE x(REAL) SUCH THAT SUM({factor}POWER(x, 2)) <= l MAXIMIZE SUM(s * x)) q ORDER BY id"
    )
    assert _direct_values(decidb_cli, sql) == pytest.approx(_solver_values(decidb_cli, sql, True), rel=1e-3)


_INFINITE_LIMIT = (
    "SELECT x FROM (FROM (VALUES (1, 3.0::DOUBLE, 'Infinity'::DOUBLE), (2, 4.0::DOUBLE, 'Infinity'::DOUBLE)) "
    "t(id, s, l) DECIDE x(REAL) SUCH THAT SUM(POWER(x, 2)) <= l {objective}) q ORDER BY id"
)


@pytest.mark.correctness
def test_n1_infinite_limit_on_a_square_keeps_the_target(decidb_cli):
    sql = _INFINITE_LIMIT.format(objective="MINIMIZE SUM(POWER(x - s, 2))")
    assert _direct_values(decidb_cli, sql) == [3.0, 4.0]
    assert _solver_values(decidb_cli, sql) == pytest.approx([3.0, 4.0], abs=1e-3)


@pytest.mark.error
def test_n1_infinite_limit_with_an_improving_row_is_unbounded(decidb_cli):
    # The solver stops on this model with numerical trouble, a limitation of its own; the problem is unbounded.
    sql = _INFINITE_LIMIT.format(objective="MAXIMIZE SUM(s * x)")
    assert outcome(decidb_cli, sql, "require") == ("error", "unbounded")


# ---------------------------------------------------------------------------
# With no Gurobi
# ---------------------------------------------------------------------------


@pytest.mark.correctness
def test_n1_answers_on_a_machine_with_no_gurobi(decidb_exe_path, tmp_path):
    # The refusal of a quadratic limit is raised when the solver plan is built, after a rule applied and replaced it. The
    # solver's refusal is what `off` still raises, which shows Gurobi really is unavailable to this process.
    cli = without_gurobi(decidb_exe_path, tmp_path)
    query = (
        "SELECT id, x FROM (FROM (VALUES (1, 3.0::DOUBLE), (2, 4.0::DOUBLE)) t(id, s) "
        "DECIDE x(REAL) SUCH THAT SUM(POWER(x, 2)) <= 25 MAXIMIZE SUM(s * x)) ORDER BY id"
    )
    answered = cli(query)
    assert answered.stdout.split() == ["1|3.0", "2|4.0"], answered.stderr
    refused = cli(f"SET decide_direct_solve='off'; {query}")
    assert "which needs Gurobi" in refused.stderr, refused.stderr


@pytest.mark.error
def test_a_refusal_with_no_gurobi_quotes_the_quadratic_clause(decidb_exe_path, tmp_path):
    # An extra clause makes N1 decline, so the solver plan is built and refuses the quadratic limit. The refusal names
    # that limit as written, and not the linear clause beside it.
    query = (
        "SELECT id, x FROM (FROM (VALUES (1, 3.0::DOUBLE), (2, 4.0::DOUBLE)) t(id, s) "
        "DECIDE x(REAL) SUCH THAT SUM(x) <= 50 AND SUM(POWER(x, 2)) <= 25 MAXIMIZE SUM(s * x)) ORDER BY id"
    )
    refused = without_gurobi(decidb_exe_path, tmp_path)(query)
    assert (
        "the SUCH THAT clause `SUM(POWER(x, 2)) <= 25` squares or multiplies decision variables" in refused.stderr
    ), refused.stderr
    assert "SUM(x) <= 50" not in refused.stderr, refused.stderr
