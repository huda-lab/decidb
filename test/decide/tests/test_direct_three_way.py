"""Direct solve, checked three ways: the independent oracle, the solver path, the direct path.

Every case is one DECIDE query in the shape of a direct-solve class. The oracle builds its
own model from the raw rows (never from DeciDB's reading of the SQL). DeciDB then runs the
query twice, with `decide_direct_solve='off'` (the solver) and `'require'` (direct solve;
a query the rule does not prove fails instead of quietly using the solver). The case passes
when all three agree:

- the oracle finds the problem feasible: both DeciDB runs return an assignment that
  satisfies every constraint and reaches the oracle's objective (rows may differ on ties);
- the oracle finds it infeasible: both DeciDB runs raise the infeasible error;
- the problem has no oracle model (bad input): both DeciDB runs raise the same class of error.

`compare_solutions` only compares objective values, so the constraints are also checked
directly: the list that builds the oracle model is evaluated on each DeciDB assignment.

Strict bounds follow the solver path (`ilp_model_builder.cpp`): on an integer-valued left
side, `< K` is `<= ceil(K) - 1` and `> K` is `>= floor(K) + 1`.

A new class adds rows to the tables below. What a result comparison cannot see (which plan
ran, EXPLAIN, prepared plans, DIAGNOSE, pruning) stays in `test_direct_rule_contract.py`.

Note: `oracle_solver` caches each result under the test node id plus a hash of the test
function's source. The case tables and the model builder live outside that source, so editing
them does not invalidate the cache; a stale value fails loudly with an objective mismatch.
After changing a case or `_s1_constraints`, remove its entries from `results/oracle_cache.json`.
"""

import itertools
import math
from fractions import Fraction

import pytest

from comparison.compare import compare_solutions
from decidb_cli import DecidBCliError
from solver.types import ObjSense, VarType

from ._direct_differential import error_class
from ._direct_oracle_common import _allow_direct_path  # noqa: F401  (autouse fixture)
from ._direct_oracle_common import agree_with_oracle, outcome as _outcome, run as _run, values_clause
from ._direct_rule_fixtures import S1_SOLVER_ONLY_ROW_BOUNDS


# ---------------------------------------------------------------------------
# Data
# ---------------------------------------------------------------------------

COLUMNS = (
    ("id", "INTEGER"),
    ("g", "VARCHAR"),  # PER key
    ("score", "DOUBLE"),
    ("cap", "INTEGER"),  # per-row count bound
    ("cap2", "DOUBLE"),  # a second per-row count bound
    ("geq", "DOUBLE"),  # a count bound that is constant within each group, for equalities
    ("flag", "BOOLEAN"),  # WHEN membership
    ("pin", "BOOLEAN"),  # nullable WHEN membership for a pin
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
    row = dict(id=id, g=g, score=score, cap=cap, flag=flag, pin=False, cap2=1.0, geq=1.0, lo=0.0, hi=1.0,
               nlo=None, nhi=None, ncap=None, fb=1.0, sm=(id * 3) % 7 - 2)
    row.update(extra)
    return row


# Groups: a = rows 0-2, b = 3-5, c = 6-7 (no row is in WHEN), NULL key = 8-9.
# Rows 1, 2 and 4 carry per-row bounds on x (lo/hi): row 1 is fixed to 1, row 2 to 0, row 4 to 1.
MAIN = [
    _row(0, "a", 9.0, 2, True, cap2=2.0, nhi=2.0, ncap=1.0, fb=0.5),
    _row(1, "a", 5.5, 1, True, cap2=2.0, lo=0.5, hi=2.0, nlo=1.0, fb=2.0, pin=None),
    _row(2, "a", -1.0, 3, False, lo=0.0, hi=0.5, nlo=0.0, nhi=3.0, ncap=2.0),
    _row(3, "b", 8.0, 2, True, cap2=2.0, geq=2.0, lo=-1.0, hi=5.0, nlo=1.0, nhi=2.5, pin=True),
    _row(4, "b", 4.0, 2, True, geq=2.0, lo=1.0, hi=1.0, nlo=1.0, nhi=2.0, ncap=2.0, pin=None),
    _row(5, "b", 3.5, 1, False, geq=2.0, nhi=2.0, fb=3.0),
    _row(6, "c", -2.5, 2, False, cap2=2.0, nlo=0.0, ncap=3.0),
    _row(7, "c", 0.5, 3, False, cap2=2.0, nhi=2.0, pin=True),
    _row(8, None, 6.0, 1, True, fb=2.0, pin=None),
    _row(9, None, -3.0, 2, False, fb=0.5),
]

# Equal scores, so a plan that ranks with ties (RANK instead of ROW_NUMBER) selects too many rows.
TIES = [
    _row(i, g, score, 2, True)
    for i, (g, score) in enumerate(
        [("a", 5.0), ("a", 5.0), ("a", 5.0), ("b", 5.0), ("b", 0.0), ("b", 0.0), ("c", -2.0), ("c", -2.0)]
    )
]

DATASETS = {"main": MAIN, "ties": TIES}

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
# TINYINT column `sm`): SQL text -> its value given the rows the bound's clause covers in the group (every row of the
# group under an aggregate-local WHEN, the flagged ones under a top WHEN). The variants ending in `+ 0` exist for a top
# WHEN, which a bare aggregate would take for its own.
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
    "SUM(sm) + 100": lambda rows: sum(r["sm"] for r in rows) + 100,
    "AVG(sm) + 2": lambda rows: Fraction(sum(r["sm"] for r in rows), len(rows)) + 2,
}
REDUCER_BOUND_EXPRS = {
    **_REDUCER_BASES,
    **_REDUCER_EXTRAS,
    **{f"{key} + 0": function for key, function in _REDUCER_BASES.items()},
}


def _dataset(case):
    return DATASETS[case.get("data", "main")]


def _literal(value, sql_type):
    if value is None:
        return f"CAST(NULL AS {sql_type})"
    if isinstance(value, bool):
        return str(value).lower()
    return f"CAST({value!r} AS {sql_type})"


def _values(records, names=NAMES):
    return values_clause(records, names, dict(COLUMNS), _literal)


# ---------------------------------------------------------------------------
# S1: choose rows by score under count bounds on one BOOL decision
# ---------------------------------------------------------------------------
#
# A case is a dict with these optional keys:
#   data      "main" (default) or "ties"
#   bounds    [(op, bound[, scope])]   SUM(x) op bound. The bound is a number or a key of BOUND_EXPRS.
#                                      scope overrides the case's `when` for this bound.
#   count_body                         what SUM(...) adds up, "x" by default; other spellings of one x
#   per       True                     scope the count bounds PER g
#   when      "top"                    WHEN flag after the bound: rows outside it are out of the problem
#             "local"                  WHEN flag inside the aggregate: only flagged rows are counted,
#                                      but every row of the group still states a bound
#   pins      [(value, [ids])]         x = value WHEN id = i
#   pinwhen   True                     x = 1 WHEN pin (the column is NULL on some rows: those are not pinned)
#   flagpin_below N                    x = flag WHEN id < N
#   rowbounds [(op, number, ids|None[, lhs])]  lhs op number on the given ids (None: every row, no WHEN)
#   noop      [SQL]                    clauses that restate the BOOL domain and change nothing
#   colpins   True                     x >= lo AND x <= hi, the bounds read from each row
#   objective key of OBJECTIVES (default "score"), or "none" for a query with no objective (every score is 0)
#   sense     "MAXIMIZE" (default) or "MINIMIZE"; unused when there is no objective

S1_CASES = {
    # --- count bounds, constant ---
    "global_upper_max": {"bounds": [("<=", 2)]},
    "global_lower_min": {"bounds": [(">=", 3)], "sense": "MINIMIZE"},
    "global_fractional_upper": {"bounds": [("<=", 2.5)]},
    "global_upper_at_2_pow_53": {"bounds": [("<=", 9007199254740992)]},
    "rowbound_when_matches_no_row": {"bounds": [("<=", 5)], "rowbounds": [(">=", 2.0, [99])]},
    "global_interval": {"bounds": [(">=", 4), ("<=", 6)]},
    "global_exact": {"bounds": [("=", 3)]},
    "per_interval": {"bounds": [(">=", 1), ("<=", 2)], "per": True},
    "per_exact_min": {"bounds": [("=", 1)], "per": True, "sense": "MINIMIZE"},
    "top_when_upper": {"bounds": [("<=", 1)], "when": "top"},
    "top_when_per_exact": {"bounds": [(">=", 1), ("<=", 1)], "per": True, "when": "top"},
    "count_body_2x_minus_x": {"bounds": [("<=", 2)], "count_body": "2*x - x"},
    "count_body_constant_unit": {"bounds": [("<=", 2)], "count_body": "(1+0)*x"},
    # --- aggregate-local WHEN ---
    "local_when_interval": {"bounds": [(">=", 1), ("<=", 1)], "per": True, "when": "local"},
    "local_when_upper_global": {"bounds": [("<=", 2)], "when": "local"},
    # Row 5 is not in WHEN, but its cap (1) still limits the flagged rows of group b.
    "local_when_source_bound_reads_unflagged_rows": {"bounds": [("<=", "cap")], "per": True, "when": "local"},
    "local_when_source_bound_global": {"bounds": [("<=", "cap")], "when": "local"},
    "local_when_source_lower_bound": {"bounds": [(">=", "cap2")], "per": True, "when": "local", "sense": "MINIMIZE"},
    # Group c has no flagged row, so the equality leaves it free.
    "local_when_equality_skips_group_without_flagged_rows": {"bounds": [("=", "geq")], "per": True, "when": "local"},
    "mixed_top_and_local_when": {"bounds": [("<=", "cap", "top"), ("<=", "cap2", "local")], "per": True},
    # --- source-valued bounds ---
    "source_bound_per": {"bounds": [("<=", "cap")], "per": True},
    "source_bound_per_top_when": {"bounds": [(">=", 1), ("<=", "cap")], "per": True, "when": "top"},
    "source_bound_global_min": {"bounds": [("<=", "cap")], "sense": "MINIMIZE"},
    "source_strict_upper_per": {"bounds": [("<", "cap")], "per": True},
    "source_strict_lower_local_when": {
        "bounds": [(">", "cap2")], "per": True, "when": "local", "sense": "MINIMIZE"
    },
    "source_equality_constant_in_group": {"bounds": [("=", "geq")], "per": True},
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
    "source_bounds_with_pin": {
        "bounds": [(">=", "COALESCE(nlo, 1.0)"), ("<=", "cap")],
        "per": True,
        "pins": [(1, [5])],
    },
    # --- bounds that count rows: COUNT(*) is the rows the clause covers in each group ---
    "count_star_half_global": {"bounds": [("<=", "COUNT(*) / 2")]},
    "count_star_half_per": {"bounds": [("<=", "COUNT(*) / 2")], "per": True},
    "count_star_minus_one_lower_per": {"bounds": [(">=", "COUNT(*) - 1")], "per": True, "sense": "MINIMIZE"},
    "count_star_equality_per": {"bounds": [("=", "COUNT(*) - 1")], "per": True},
    "count_star_interval_global": {"bounds": [(">=", "COUNT(*) / 4"), ("<=", "COUNT(*) / 2")]},
    # Under a top WHEN the count covers only the flagged rows; under a local WHEN it covers every row of the group.
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
        "bounds": [(">=", "COUNT(*) / 3"), ("<=", "COUNT(*) / 2")], "per": True, "sense": "MINIMIZE"
    },
    "count_star_interval_local_when_per": {
        "bounds": [(">=", "COUNT(*) / 2"), ("<=", "COUNT(*) - 1")], "per": True, "when": "local"
    },
    "count_star_with_pins": {
        "bounds": [("<=", "COUNT(*) / 2")], "per": True, "pins": [(1, [2]), (0, [0])]
    },
    "count_star_with_shifted_scores": {
        "bounds": [("<=", "COUNT(*) / 2")], "per": True, "objective": "score_minus_cap", "sense": "MINIMIZE"
    },
    # --- pins and per-row bounds on x ---
    "pins_with_group_bounds": {"bounds": [("<=", 2)], "per": True, "pins": [(1, [4]), (0, [0, 8])]},
    "pins_with_source_bound": {"bounds": [(">=", 1), ("<=", "cap")], "per": True, "pins": [(1, [2]), (0, [1])]},
    "pin_by_nullable_boolean_column": {"bounds": [("<=", 3)], "pinwhen": True},
    "pin_to_boolean_column": {"bounds": [("<=", 5)], "flagpin_below": 5},
    "pin_written_as_unit_body": {"bounds": [("<=", 2)], "rowbounds": [("<=", 0, [1], "1*x")]},
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
    "column_pins_global": {"bounds": [("<=", 3)], "colpins": True},
    "column_pins_global_min": {"bounds": [("<=", 3)], "colpins": True, "sense": "MINIMIZE"},
    "column_pins_per_interval": {"bounds": [(">=", 1), ("<=", 2)], "per": True, "colpins": True},
    # --- tied scores: a plan that ranks with ties selects too many rows ---
    "ties_top_k": {"data": "ties", "bounds": [("<=", 2)]},
    "ties_one_per_group": {"data": "ties", "bounds": [(">=", 1), ("<=", 1)], "per": True},
    "ties_minimize_with_lower_bound": {"data": "ties", "bounds": [(">=", 3), ("<=", 5)], "sense": "MINIMIZE"},
    # --- infeasible: the oracle must also say so, and both DeciDB paths must raise ---
    "infeasible_lower_exceeds_group": {"bounds": [(">=", 4)], "per": True},
    "infeasible_source_bounds_cross": {"bounds": [(">=", "cap2"), ("<=", "cap")], "per": True},
    "infeasible_local_when_source_bounds_cross": {
        "bounds": [(">=", "cap2"), ("<=", "cap")],
        "per": True,
        "when": "local",
    },
    "infeasible_count_star_bound_below_zero": {"bounds": [("<=", "COUNT(*) - 20")]},
    "infeasible_rowbound_fraction_equal": {"bounds": [("<=", 5)], "rowbounds": [("=", 0.5, [1])]},
    "infeasible_contradicting_pins": {"bounds": [("<=", 5)], "pins": [(1, [3]), (0, [3])]},
    "infeasible_pin_against_upper": {"bounds": [("<=", 1)], "pins": [(1, [0, 3])]},
    "infeasible_pins_against_lower": {"bounds": [(">=", 3), ("<=", 5)], "pins": [(0, [0, 1, 2, 3, 4, 5, 6, 7, 8, 9])]},
}

_OPNAMES = {"<=": "le", "<": "lt", ">=": "ge", ">": "gt", "=": "eq", "<>": "ne"}


def _tag(value):
    return str(value).replace("-", "m").replace(".", "p")


# The count limit: every comparison against whole and fractional limits, including limits no count can meet. A
# lower bound is minimized so that it binds.
for _op, _limit in itertools.product(("<=", "<", ">=", ">", "="), (-0.5, 0, 1.5, 2)):
    S1_CASES[f"limit_{_OPNAMES[_op]}_{_tag(_limit)}"] = {
        "bounds": [(_op, _limit)],
        "sense": "MINIMIZE" if _op in (">", ">=") else "MAXIMIZE",
    }

# A per-row bound on x with a WHEN is plain arithmetic on a Boolean: the row is left free, fixed to 0 or 1, or
# infeasible. Without a WHEN the solver reads some spellings as a bound on the variable instead; those stay on the
# solver (see the contract fixture), and the rest must agree with arithmetic.
# Every comparison against a bound that counts rows, per group; a lower bound is minimized so that it binds.
for _op, _bound in itertools.product(("<=", "<", ">=", ">", "="), ("COUNT(*) / 2", "COUNT(*) - 1")):
    S1_CASES[f"count_star_{_OPNAMES[_op]}_{_tag(_bound.replace('COUNT(*)', 'n').replace(' ', ''))}"] = {
        "bounds": [(_op, _bound)],
        "per": True,
        "sense": "MINIMIZE" if _op in (">", ">=") else "MAXIMIZE",
    }

_ROWBOUND_CONSTANTS = ("-1", "-0.5", "0", "0.5", "1", "1.5", "2")
for _op, _constant in itertools.product(_OPNAMES, _ROWBOUND_CONSTANTS):
    S1_CASES[f"x_{_OPNAMES[_op]}_{_tag(_constant)}_when"] = {
        "bounds": [("<=", 3)],
        "rowbounds": [(_op, float(_constant), [3])],
    }
    if (_op, _constant) not in S1_SOLVER_ONLY_ROW_BOUNDS:
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
    "le_whole": [("<=", 2)],
    "lt_fraction": [("<", 2.5)],
    "eq_whole": [("=", 2)],
    "interval": [(">=", 1), ("<=", 2)],
    "impossible_lower": [(">=", 5)],
    "source_lower": [(">=", "cap2")],
    "source_upper": [("<=", "cap")],
    "source_equality": [("=", "geq")],
    "coalesce_interval": [(">=", "COALESCE(nlo, 1.0)"), ("<=", "COALESCE(nhi, 2.0)")],
    "count_star_lower": [(">=", "COUNT(*) / 2")],
    "count_star_upper": [("<=", "COUNT(*) - 1")],
}
_NO_OBJECTIVE_SCOPES = {
    "global": {},
    "per": {"per": True},
    "top_when": {"when": "top"},
    "top_when_per": {"when": "top", "per": True},
    "local_when": {"when": "local"},
    "local_when_per": {"when": "local", "per": True},
}
for _shape, _scope in itertools.product(_NO_OBJECTIVE_BOUNDS, _NO_OBJECTIVE_SCOPES):
    if _shape == "source_equality" and not _NO_OBJECTIVE_SCOPES[_scope].get("per"):
        continue  # the equality bound is constant within a group only, so a global scope sees it vary and raises
    S1_CASES[f"no_objective_{_shape}_{_scope}"] = {
        "bounds": _NO_OBJECTIVE_BOUNDS[_shape],
        "objective": "none",
        **_NO_OBJECTIVE_SCOPES[_scope],
    }
S1_CASES["no_objective_with_pins_per"] = {
    "bounds": [(">=", 1), ("<=", 2)],
    "per": True,
    "pins": [(1, [4]), (0, [0, 8])],
    "objective": "none",
}
S1_CASES["no_objective_pins_force_the_lower_bound_past_its_rows"] = {
    "bounds": [(">=", 2)],
    "per": True,
    "pins": [(0, [0, 1])],
    "objective": "none",
}
S1_CASES["no_objective_column_pins_global"] = {"bounds": [(">=", 2)], "colpins": True, "objective": "none"}
S1_CASES["no_objective_infeasible_pin_against_upper"] = {
    "bounds": [("<=", 1)],
    "pins": [(1, [0, 3])],
    "objective": "none",
}
S1_CASES["no_objective_ties_dataset"] = {"data": "ties", "bounds": [(">=", 3)], "objective": "none"}

# One representative objective shape per sense, over an interval so that both senses bind.
for _name in NEW_OBJECTIVES:
    for _sense in ("MAXIMIZE", "MINIMIZE"):
        S1_CASES[f"objective_{_name}_{_sense.lower()[:3]}"] = {
            "bounds": [(">=", 2), ("<=", 4)],
            "objective": _name,
            "sense": _sense,
        }


def _decide_clause(case):
    default_scope = case.get("when")
    per = " PER g" if case.get("per") else ""
    body = case.get("count_body", "x")
    clauses = []
    for bound in case.get("bounds", ()):
        op, limit = bound[0], bound[1]
        scope = bound[2] if len(bound) > 2 else default_scope
        if scope == "local":
            clauses.append(f"SUM({body}) WHEN flag {op} {limit}{per}")
        elif scope == "top":
            clauses.append(f"SUM({body}) {op} {limit} WHEN flag{per}")
        else:
            clauses.append(f"SUM({body}) {op} {limit}{per}")
    for value, ids in case.get("pins", ()):
        clauses.extend(f"x = {value} WHEN id = {i}" for i in ids)
    if case.get("pinwhen"):
        clauses.append("x = 1 WHEN pin")
    if case.get("flagpin_below") is not None:
        clauses.append(f"x = flag WHEN id < {case['flagpin_below']}")
    for rowbound in case.get("rowbounds", ()):
        op, constant, ids = rowbound[:3]
        lhs = rowbound[3] if len(rowbound) > 3 else "x"
        if ids is None:
            clauses.append(f"{lhs} {op} {constant}")
        else:
            clauses.extend(f"{lhs} {op} {constant} WHEN id = {i}" for i in ids)
    clauses.extend(case.get("noop", ()))
    if case.get("colpins"):
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
            else:
                counted = members
            if not counted:
                continue  # nothing to count: the group is left free
            # A count body `a*x + c` adds `a` per selected row and `c` per row the SUM covers.
            coefficients = {names[i]: float(case.get("body_a", 1)) for i in counted}
            shift = Fraction(str(case.get("body_c", 0))) * len(counted)
            # `COUNT(*)` counts the rows that state the bound: every row of the group under a local WHEN, the
            # flagged ones under a top WHEN.
            group_rows = [rows[j] for j in members]
            limits = {limit_of(limit, rows[i], len(members), group_rows) for i in members}
            sum_op = op
            for rhs in sorted(limits):
                if shift:
                    rhs = float(Fraction(repr(rhs)) - shift)
                sum_op, rhs = _inclusive(op, rhs)
                constraints.append((coefficients, sum_op, rhs))

    for value, ids in case.get("pins", ()):
        for i in (i for i in ids if i < len(rows)):  # an id no row has is not bound
            constraints.append(({names[i]: 1.0}, "=", float(value)))
    if case.get("pinwhen"):
        constraints.extend(({names[i]: 1.0}, "=", 1.0) for i, row in enumerate(rows) if row["pin"] is True)
    if case.get("flagpin_below") is not None:
        constraints.extend(
            ({names[i]: 1.0}, "=", 1.0 if row["flag"] else 0.0)
            for i, row in enumerate(rows)
            if row["id"] < case["flagpin_below"]
        )
    for rowbound in case.get("rowbounds", ()):
        op, constant, ids = rowbound[:3]
        for i in range(len(rows)) if ids is None else (i for i in ids if i < len(rows)):
            constraints.extend(_row_bound(names[i], op, constant))
    if case.get("colpins"):
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
    oracle_solver.create_model("direct_three_way")
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


# A strict bound against a fraction, with no WHEN, is a bound on the variable that the solver shifts to a whole number
# (`x > 0.5` is `x >= 1`, `x < 1.5` is `x <= 1`, `x < 0.5` is `x <= 0`, `x > -0.5` is `x >= 0`). S1 shifts it the
# same way, so the oracle, `off`, `require` and `auto` all give the same answer, and `auto` takes the direct path.
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize("op,constant", [(">", 0.5), ("<", 1.5), ("<", 0.5), (">", -0.5)])
@pytest.mark.parametrize("sense", ["MAXIMIZE", "MINIMIZE"])
@pytest.mark.parametrize("limit", [4, 50])
def test_s1_strict_bound_against_a_fraction_agrees_under_every_mode(
    decidb_cli, oracle_solver, op, constant, sense, limit
):
    # With the limit of 4 the bound `x > 0.5` (every row selected) is infeasible: every mode must say so.
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


# A bound on x with no WHEN that always holds changes nothing: the solver seeds a Boolean's box at [0, 1] and a written
# bound only narrows it, so `x >= -3` leaves both values allowed. S1 reads it that way, so the query is proved by S1
# (`require` would fail on a miss) and gives the oracle's answer.
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize("op,constant", [(">=", "-3"), (">", "-3"), (">=", "-1"), (">=", "-0.5")])
@pytest.mark.parametrize("sense", ["MAXIMIZE", "MINIMIZE"])
def test_s1_proves_a_bound_that_always_holds_in_require_mode(decidb_cli, oracle_solver, op, constant, sense):
    limit = ("<=", 3) if sense == "MAXIMIZE" else (">=", 2)
    case = {"bounds": [limit], "rowbounds": [(op, constant, None)], "sense": sense}
    _agree(decidb_cli, oracle_solver, case)
    sql = _wrapped_sql(case, "plain")
    plan = decidb_cli.execute_raw(f"SET decide_direct_solve='require'; EXPLAIN {sql}").stdout
    assert "S1_UNIFORM_MATROID" in plan, plan
    assert _outcome(decidb_cli, sql, "require") == _outcome(decidb_cli, sql, "off")


# The same problem inside a parent query. The first base case meets every context; the others meet one each,
# since a context does not interact with the kind of bound.
CONTEXT_CASES = [("global_upper_max", wrap) for wrap in (
    "cte_materialized", "cte_inlined", "wide_source", "joined_source", "correlated_source", "parent_join",
    "recombined_filters",
)] + [
    ("per_interval", "wide_source"),
    ("local_when_interval", "recombined_filters"),
    ("pins_with_group_bounds", "joined_source"),
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

    oracle_solver.create_model("direct_three_way_second")
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

# Each source scores 1.0 on every row but one bad value on the last of 5,000 rows, so the
# error must raise however little of the result a parent reads (the solver reads every row).
_LATE = (
    "FROM (SELECT i, {score} AS score FROM range(5000) t(i)) s "
    "DECIDE x(BOOL) SUCH THAT SUM(x) <= 0 MAXIMIZE SUM(score*x)"
)


def _problem(values, columns, constraint, tail="", objective="SUM(score*x)"):
    return (
        f"SELECT x FROM (FROM (VALUES {values}) t({columns}) "
        f"DECIDE x(BOOL) SUCH THAT {constraint} MAXIMIZE {objective}) q{tail}"
    )


# name -> (sql, expected error class on the direct path or None, text both messages carry or None).
# Both paths must raise; the message and the choice among several errors need not match.
S1_ERROR_CASES = {
    # --- scores ---
    "null_score": (_problem("(1, 2.0), (2, NULL::DOUBLE)", "id, score", "SUM(x) <= 1"), None, None),
    "nan_score": (_problem("(1, 2.0), (2, 'NaN'::DOUBLE)", "id, score", "SUM(x) <= 1"), None, None),
    "infinite_score": (_problem("(1, 2.0), (2, 'Infinity'::DOUBLE)", "id, score", "SUM(x) <= 1"), None, None),
    "late_null_score_under_limit": (
        "SELECT i FROM (" + _LATE.format(score="CASE WHEN i=4999 THEN NULL ELSE 1.0 END") + ") q LIMIT 1",
        None,
        None,
    ),
    "late_nan_score_under_count": (
        "SELECT COUNT(*) FROM (" + _LATE.format(score="CASE WHEN i=4999 THEN 'NaN'::DOUBLE ELSE 1.0 END") + ") q",
        None,
        None,
    ),
    # The last value overflows only after the two finite objective terms are added.
    "sum_of_terms_overflows": (
        _problem(
            "(1, 9.0::DOUBLE, 1.0::DOUBLE), (2, 8.0::DOUBLE, 1.0::DOUBLE), (3, 1e308::DOUBLE, 1e308::DOUBLE)",
            "id, a, b",
            "SUM(x) <= 1",
            " LIMIT 1",
            "SUM(a*x)+SUM(b*x)",
        ),
        None,
        None,
    ),
    "scaled_score_overflows": (
        _problem("(1, 9.0), (2, 5.0)", "id, score", "SUM(x) <= 2", objective="1e308 * SUM(score*x)"), None, None,
    ),
    # --- an empty aggregate ---
    "empty_top_when": (
        _problem("(1, 2.0, false)", "id, score, flag", "SUM(x) <= 1 WHEN flag"), "empty_aggregate", None,
    ),
    "empty_all_keys_null": (
        _problem("(1, NULL::VARCHAR, 2.0), (2, NULL::VARCHAR, 3.0)", "id, dept, score", "SUM(x) <= 1 PER dept"),
        "empty_aggregate",
        None,
    ),
    "impossible_upper_bound_per_is_infeasible": (
        _problem(
            "(1, NULL::VARCHAR, TRUE, 9.0), (2, 'A', FALSE, 5.0)", "id, dept, flag, score", "SUM(x) <= -1 PER dept"
        ),
        "infeasible",
        None,
    ),
    "impossible_upper_bound_with_when_has_no_rows": (
        _problem(
            "(1, NULL::VARCHAR, TRUE, 9.0), (2, 'A', FALSE, 5.0)",
            "id, dept, flag, score",
            "SUM(x) <= -1 WHEN flag PER dept",
        ),
        "empty_aggregate",
        None,
    ),
    # --- a bad source-valued bound raises even on a row the problem does not read ---
    "null_bound_on_a_row_with_null_key": (
        _problem(
            "(1, NULL::INTEGER, TRUE, NULL::INTEGER, 9.0), (2, 1, TRUE, 1, 2.0)",
            "id, cap, flag, dept, score",
            "SUM(x) <= cap PER dept",
        ),
        "null_bound",
        None,
    ),
    "nan_bound_on_a_row_outside_top_when": (
        _problem(
            "(1, 'NaN'::DOUBLE, FALSE, NULL::INTEGER, 9.0), (2, 1.0::DOUBLE, TRUE, 1, 8.0)",
            "id, cap, flag, dept, score",
            "SUM(x) <= cap WHEN flag PER dept",
        ),
        "null_bound",
        None,
    ),
    "bad_try_cast_bound_on_a_row_outside_local_when": (
        _problem(
            "(1, 'bad'::VARCHAR, FALSE, NULL::INTEGER, 9.0), (2, '1'::VARCHAR, TRUE, 1, 8.0)",
            "id, raw, flag, dept, score",
            "SUM(x) WHEN flag <= TRY_CAST(raw AS DOUBLE) PER dept",
        ),
        "null_bound",
        None,
    ),
    "late_null_bound_under_limit": (
        "SELECT x FROM (FROM (SELECT i AS id, 1.0::DOUBLE AS lo, "
        "CASE WHEN i=4999 THEN NULL ELSE 1.0 END::DOUBLE AS hi, "
        "9.0::DOUBLE AS score FROM range(5000) t(i)) s "
        "DECIDE x(BOOL) SUCH THAT SUM(x) >= lo AND SUM(x) <= hi MAXIMIZE SUM(score*x)) q LIMIT 1",
        "null_bound",
        None,
    ),
    # --- an equality bound has one value per group ---
    "equality_bound_varies_within_a_group": (
        _problem(
            "(1,'A',1.0::DOUBLE,9.0), (2,'A',1.0::DOUBLE,8.0), (3,'B',1.0::DOUBLE,7.0), (4,'B',2.0::DOUBLE,6.0)",
            "id, dept, eqcap, score",
            "SUM(x) = eqcap PER dept",
        ),
        "equality_varies",
        None,
    ),
    # --- an error survives whatever the parent query keeps of the result ---
    "throwing_cast_in_score_under_limit": (
        "SELECT i FROM (FROM (SELECT i, CASE WHEN i=4999 THEN 'bad' ELSE '1' END AS raw FROM range(5000) t(i)) s "
        "DECIDE x(BOOL) SUCH THAT SUM(x) <= 0 MAXIMIZE SUM(CAST(raw AS DOUBLE)*x)) q LIMIT 1",
        None,
        "Could not convert string 'bad' to DOUBLE",
    ),
    "null_score_under_a_filter_on_the_result": (
        "SELECT i FROM (" + _LATE.format(score="CASE WHEN i=4999 THEN NULL ELSE 1.0 END") + ") q WHERE i = 0",
        "null_bound",
        None,
    ),
    "null_score_behind_a_materialized_filter": (
        "WITH solved AS MATERIALIZED (FROM (SELECT i, CASE WHEN i=4999 THEN NULL ELSE i::DOUBLE END AS score, "
        "repeat('p', 200) AS payload FROM range(5000) t(i)) s DECIDE x(BOOL) SUCH THAT SUM(x)<=1 "
        "MAXIMIZE SUM(score*x)) SELECT i, x FROM solved WHERE i=0",
        "null_bound",
        None,
    ),
    "null_score_behind_a_parent_join_filter": (
        "SELECT d.id, d.x FROM (FROM (VALUES (1,100.0),(2,9.0),(3,NULL::DOUBLE)) s(id,score) "
        "DECIDE x(BOOL) SUCH THAT SUM(x)<=1 MAXIMIZE SUM(score*x)) d JOIN (VALUES (2)) keep(id) USING(id)",
        "null_bound",
        None,
    ),
    # --- no objective: the bounds and pins are still read on every row and still decide feasibility ---
    "no_objective_late_null_bound_under_limit": (
        "SELECT x FROM (FROM (SELECT i AS id, 1.0::DOUBLE AS lo, "
        "CASE WHEN i=4999 THEN NULL ELSE 1.0 END::DOUBLE AS hi FROM range(5000) t(i)) s "
        "DECIDE x(BOOL) SUCH THAT SUM(x) >= lo AND SUM(x) <= hi) q LIMIT 1",
        "null_bound",
        None,
    ),
    "no_objective_late_nan_pin_column_under_count": (
        "SELECT COUNT(*) FROM (FROM (SELECT i AS id, "
        "CASE WHEN i=4999 THEN 'NaN'::DOUBLE ELSE 1.0 END AS pin FROM range(5000) t(i)) s "
        "DECIDE x(BOOL) SUCH THAT SUM(x) >= 1 AND x <= pin) q",
        "null_bound",
        None,
    ),
    "no_objective_empty_top_when": (
        "SELECT x FROM (FROM (VALUES (1, false)) t(id, flag) DECIDE x(BOOL) SUCH THAT SUM(x) >= 1 WHEN flag) q",
        "empty_aggregate",
        None,
    ),
    "no_objective_lower_bound_exceeds_the_rows": (
        "SELECT x FROM (FROM (VALUES (1), (2)) t(id) DECIDE x(BOOL) SUCH THAT SUM(x) >= 3) q",
        "infeasible",
        None,
    ),
    "no_objective_late_pin_conflict_survives_limit": (
        "SELECT x FROM (FROM (VALUES (1), (2), (3)) t(id) "
        "DECIDE x(BOOL) SUCH THAT x = 1 WHEN id = 3 AND x = 0 WHEN id = 3 AND SUM(x) >= 1) q LIMIT 1",
        "infeasible",
        None,
    ),
    # --- pins ---
    "late_pin_conflict_survives_limit": (
        _problem(
            "(1, 9.0), (2, 5.0), (3, -1.0)",
            "id, score",
            "x = 1 WHEN id = 3 AND x = 0 WHEN id = 3 AND SUM(x) <= 2",
            " LIMIT 1",
        ),
        "infeasible",
        None,
    ),
    "pin_column_null_outside_when": (
        _problem(
            "(1, 9.0, 1.0::DOUBLE), (2, 5.0, NULL::DOUBLE)", "id, score, pin", "SUM(x) <= 2 AND x <= pin WHEN id = 1"
        ),
        "null_bound",
        'column "pin"',
    ),
    "pin_column_nan": (
        _problem("(1, 9.0, 'NaN'::DOUBLE), (2, 5.0, 1.0::DOUBLE)", "id, score, pin", "SUM(x) <= 2 AND x <= pin"),
        "null_bound",
        None,
    ),
}


@pytest.mark.var_boolean
@pytest.mark.error
@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(S1_ERROR_CASES))
def test_s1_error_cases_fail_on_both_paths(decidb_cli, name):
    sql, expected, needle = S1_ERROR_CASES[name]
    messages = {}
    for mode in ("off", "require"):
        with pytest.raises(DecidBCliError) as error:
            _run(decidb_cli, sql, mode)
        messages[mode] = str(error.value)
    assert "decide_direct_solve=require" not in messages["require"], f"direct solve declined:\n{messages['require']}"
    if expected is not None:
        assert error_class(messages["require"]) == expected, messages
    if needle is not None:
        assert all(needle in message for message in messages.values()), messages


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


def _pin_column_problem(pin, op):
    return (
        f"SELECT id, x FROM (FROM (VALUES (1, 9.0, {pin}), (2, 5.0, {pin})) t(id, score, pin) "
        f"DECIDE x(BOOL) SUCH THAT SUM(x) <= 2 AND x {op} pin MAXIMIZE SUM(score*x)) q ORDER BY id"
    )


# name -> sql. Limits the solver reads as doubles: infinities, fractions, and integers beyond 2^53.
BOUNDARY_CASES = {
    **{
        f"count_limit_{_OPNAMES[op]}_{_tag(cap)}": _bound_column_problem(cap, op)
        for cap, op in [
            ("1.5::DOUBLE", "<="),
            ("1.5::DOUBLE", "="),
            ("-0.5::DOUBLE", "<="),
            ("'-Infinity'::DOUBLE", "<="),
            ("'-Infinity'::DOUBLE", ">="),
            ("'Infinity'::DOUBLE", "<="),
            ("'Infinity'::DOUBLE", ">="),
            ("9007199254740993::BIGINT", "<="),
            ("9007199254740993::BIGINT", ">="),
            ("9223372036854775807::BIGINT", "<="),
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
        f"pin_{_OPNAMES[op]}_{_tag(pin)}": _pin_column_problem(pin, op)
        for pin, op in [("'Infinity'", "<="), ("'-Infinity'", "<="), ("'Infinity'", ">="), ("'-Infinity'", ">="),
                        ("1.5", "<="), ("0.5", ">=")]
    },
}


@pytest.mark.var_boolean
@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(BOUNDARY_CASES))
def test_s1_boundary_values_read_like_the_solver(decidb_cli, name):
    sql = BOUNDARY_CASES[name]
    direct = _outcome(decidb_cli, sql, "require")
    assert direct != ("error", "direct_miss"), f"direct solve declined:\n{sql}"
    assert direct == _outcome(decidb_cli, sql, "off")


# A bound built from COUNT(*) is evaluated by DuckDB's own arithmetic, so it can overflow. The solver evaluates it once
# per group that has a row the clause covers: a row whose WHEN holds and whose PER key is not NULL. The direct plan must
# raise on exactly those queries. Groups: a = 2 rows (1 flagged), b = 2 (both flagged), c = 4 (none flagged),
# NULL key = 3 (all flagged). With a factor of 3e18, a count of 3 or more overflows INT64.
_OVERFLOW_ROWS = (
    "(1,'a',9.0,true),(2,'a',8.0,false),(3,'b',7.0,true),(4,'b',6.0,true),"
    "(5,'c',5.0,false),(6,'c',4.0,false),(7,'c',3.0,false),(8,'c',2.0,false),"
    "(9,NULL,3.0,true),(10,NULL,2.0,true),(11,NULL,1.0,true)"
)
_BIG = "3000000000000000000"
# name -> (constraint, outcome on both paths)
COUNT_OVERFLOW_CASES = {
    # The NULL-key group counts three flagged rows; the solver never evaluates the bound for it.
    "null_key_group_with_when": (f"SUM(x) <= COUNT(*) * {_BIG} WHEN flag PER g", "ok"),
    # Group c has four rows but none where the local WHEN holds, so the clause covers nothing there.
    "uncovered_group_with_local_when": (f"SUM(x) WHEN flag <= COUNT(*) * {_BIG} PER g", "ok"),
    "uncovered_group_negative_factor": (f"SUM(x) >= COUNT(*) * -{_BIG} WHEN flag PER g", "ok"),
    "infinite_bound": ("SUM(x) <= COUNT(*) * 1e308 WHEN flag PER g", "ok"),
    # Covered groups that do overflow: both paths raise.
    "group_overflow": (f"SUM(x) <= COUNT(*) * {_BIG} PER g", "error"),
    "group_overflow_negative_factor": (f"SUM(x) >= COUNT(*) * -{_BIG} PER g", "error"),
    "global_overflow": (f"SUM(x) <= COUNT(*) * {_BIG}", "error"),
    "global_when_overflow": (f"SUM(x) <= COUNT(*) * {_BIG} WHEN flag", "error"),
    "local_when_overflow": (f"SUM(x) WHEN NOT flag <= COUNT(*) * {_BIG} PER g", "error"),
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
    direct = _outcome(decidb_cli, sql, "require")
    assert direct != ("error", "direct_miss"), f"direct solve declined:\n{sql}"
    assert solver[0] == expected, f"the solver gave {solver}, so this case no longer tests what it says"
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
# S1: a count body with a constant term or a negation, `SUM(x + c)` and `SUM(c - x)`
# ---------------------------------------------------------------------------
#
# `SUM(x + c) <op> B` adds `c` for each row the SUM covers, so it is `SUM(x) <op> B - n*c`, with n the number of rows the
# SUM covers in the group (the clause's WHEN holds, the PER key is not NULL, and an aggregate-local WHEN on the SUM
# holds). `SUM(c - x) <op> B` is `SUM(x) <flipped op> n*c - B`. The oracle never rewrites the comparison: it states
# `a*x_1 + ... + a*x_n <op> B - n*c` over the rows the SUM covers (`body_a` and `body_c` in `_s1_constraints`).

# (a, c) as the user writes them; c is a decimal string so that the oracle reads the same exact value as DuckDB.
OFFSET_BODIES = [(1, "1"), (1, "-1"), (1, "0.5"), (-1, "0"), (-1, "1"), (-1, "0.5"), (-1, "-1")]

# scope name -> (case keys, the rows one group's SUM covers). The bound is chosen so the count threshold is one in a
# group of that size: B = a*1 + n*c.
_OFFSET_SCOPES = {
    "global": ({}, 10),
    "per": ({"per": True}, 3),
    "top_per": ({"per": True, "when": "top"}, 2),
    "local_per": ({"per": True, "when": "local"}, 2),
    "top": ({"when": "top"}, 5),
    "local": ({"when": "local"}, 5),
}


def _offset_body(a, c):
    """The SQL of `a*x + c` for a = 1 or -1."""
    if a > 0:
        return f"x - {c[1:]}" if c.startswith("-") else f"x + {c}"
    return "-x" if c == "0" else f"{c} - x"


def _offset_case(a, c, bounds, scope="per", **extra):
    keys, _ = _OFFSET_SCOPES[scope]
    return {"count_body": _offset_body(a, c), "body_a": a, "body_c": c, "bounds": bounds, **keys, **extra}


def _binds_from_below(op, a):
    """Whether the count must be large for the bound to hold, so that MINIMIZE is the sense in which it binds."""
    return (op in (">", ">=")) != (a < 0)


S1_OFFSET_CASES = {}
_CORE_SCOPES = ("global", "per", "top_per", "local_per")
for (_a, _c), _op, _scope, _delta in itertools.product(OFFSET_BODIES, _OPNAMES, _CORE_SCOPES, (0, 0.5)):
    if _op == "<>":
        continue
    _n = _OFFSET_SCOPES[_scope][1]
    _bound = float(Fraction(_a) + _n * Fraction(_c) + Fraction(str(_delta)))
    S1_OFFSET_CASES[f"offset_{_a:+d}_{_tag(_c)}_{_OPNAMES[_op]}_{_scope}_{_tag(_delta)}"] = _offset_case(
        _a,
        _c,
        [(_op, _bound)],
        _scope,
        sense="MINIMIZE" if _binds_from_below(_op, _a) else "MAXIMIZE",
    )
# The same shapes under a WHEN with no PER (one group of the flagged rows), for a representative set.
for (_a, _c), _op, _scope in itertools.product([(1, "1"), (-1, "1"), (-1, "0.5")], _OPNAMES, ("top", "local")):
    if _op == "<>":
        continue
    _bound = float(Fraction(_a) + _OFFSET_SCOPES[_scope][1] * Fraction(_c))
    S1_OFFSET_CASES[f"offset_{_a:+d}_{_tag(_c)}_{_OPNAMES[_op]}_{_scope}"] = _offset_case(
        _a, _c, [(_op, _bound)], _scope, sense="MINIMIZE" if _binds_from_below(_op, _a) else "MAXIMIZE"
    )

# An interval, a negation alone, and bodies written other ways.
S1_OFFSET_CASES.update(
    {
        "offset_interval_pos_per": _offset_case(1, "1", [(">=", 4), ("<=", 5)], "per"),
        "offset_interval_pos_local_per": _offset_case(1, "0.5", [(">=", 2.5), ("<=", 4)], "local_per"),
        "offset_interval_neg_per": _offset_case(-1, "1", [(">=", 0), ("<=", 1)], "per"),
        "offset_interval_neg_global": _offset_case(-1, "0", [(">=", -6), ("<=", -2)], "global"),
        "offset_interval_mixed_bodies": {
            "count_body": "x + 1",
            "body_a": 1,
            "body_c": "1",
            "bounds": [(">=", 4), ("<=", 5)],
            "per": True,
        },
        "offset_negation_only_global_upper": _offset_case(-1, "0", [("<=", -3)], "global", sense="MINIMIZE"),
        "offset_negation_only_per_lower": _offset_case(-1, "0", [(">=", -1)], "per"),
        "offset_negation_only_strict_per": _offset_case(-1, "0", [(">", -2)], "per"),
        "offset_two_terms_one_x": _offset_case(1, "2", [("<=", 8)], "per", count_body="x + 3 - 1"),
        "offset_unit_weights": _offset_case(1, "1", [("<=", 4)], "per", count_body="2*x - x + 1"),
        "offset_negative_weight_sum": _offset_case(-1, "1", [("<=", 1)], "per", count_body="1 - 2*x + x"),
        "offset_zero_constant": _offset_case(1, "0", [("<=", 1)], "per", count_body="x + 0"),
        "offset_constant_cancels": _offset_case(1, "0", [("<=", 1)], "per", count_body="x + 0.1 + 0.2 - 0.3"),
        # A constant that makes the limit unreachable: every path says infeasible.
        "offset_infeasible_upper": _offset_case(1, "100", [("<=", 5)], "per"),
        "offset_infeasible_negated_upper": _offset_case(-1, "100", [("<=", 5)], "per", sense="MINIMIZE"),
        "offset_infeasible_lower": _offset_case(1, "100", [(">=", 2000)], "global", sense="MINIMIZE"),
        "offset_infeasible_negated_lower": _offset_case(-1, "100", [(">=", 2000)], "global"),
        "offset_infeasible_fractional": _offset_case(1, "0.5", [("<", 1.5)], "per"),
        "offset_infeasible_local_when": _offset_case(1, "100", [("<=", 5)], "local_per"),
        "offset_infeasible_equality": _offset_case(1, "0.5", [("=", 3.25)], "per"),
        # Group c has rows, but the aggregate-local WHEN covers none of them, so n = 0 there and the group states no
        # bound. Read as a bound on an empty sum, `0 >= 2` and `0 >= 1` would fail it.
        "offset_empty_group_local_when_interval": _offset_case(1, "1", [(">=", 3), ("<=", 5)], "local_per"),
        "offset_empty_group_local_when_negated_interval": _offset_case(-1, "1", [(">=", 1), ("<=", 3)], "local_per"),
        "offset_empty_group_local_when_fractional_interval": _offset_case(
            1, "0.5", [(">=", 1.5), ("<=", 2.5)], "local_per"
        ),
        "offset_empty_group_local_when_source_bounds": _offset_case(
            1, "1", [(">=", "cap2"), ("<=", "cap")], "local_per"
        ),
        # The right side is a source column, an expression over columns, COUNT(*) or an expression of it.
        "offset_source_bound_per": _offset_case(1, "1", [("<=", "cap")], "per"),
        "offset_source_bound_negated_per": _offset_case(-1, "1", [("<=", "cap")], "per", sense="MINIMIZE"),
        "offset_source_bound_fractional_per": _offset_case(1, "0.5", [("<=", "cap2")], "per"),
        "offset_source_bound_strict_per": _offset_case(1, "1", [("<", "cap")], "per"),
        "offset_source_bound_global": _offset_case(1, "-1", [("<=", "cap")], "global"),
        "offset_source_bound_lower_per": _offset_case(1, "1", [(">=", "cap")], "per", sense="MINIMIZE"),
        "offset_source_bound_local_when_per": _offset_case(1, "1", [("<=", "cap")], "local_per"),
        "offset_source_bound_local_when_negated": _offset_case(-1, "0.5", [(">=", "cap2")], "local_per"),
        "offset_source_bound_top_when_per": _offset_case(1, "1", [("<=", "cap")], "top_per"),
        "offset_source_equality_per": _offset_case(1, "0.5", [("=", "geq")], "per"),
        "offset_source_equality_local_when_per": _offset_case(-1, "1", [("=", "geq")], "local_per"),
        "offset_coalesce_bound_per": _offset_case(1, "1", [("<=", "COALESCE(ncap, fb)")], "per"),
        "offset_coalesce_interval_local_when": _offset_case(
            1, "1", [(">=", "COALESCE(nlo, 1.0)"), ("<=", "COALESCE(nhi, 2.0)")], "local_per"
        ),
        "offset_source_bounds_two_sides": _offset_case(1, "0.5", [(">=", "cap2"), ("<=", "cap")], "per"),
        "offset_count_star_per": _offset_case(1, "1", [("<=", "COUNT(*)")], "per"),
        "offset_count_star_half_per": _offset_case(1, "0.5", [("<=", "COUNT(*) / 2")], "per"),
        "offset_count_star_negated_per": _offset_case(-1, "1", [("<=", "COUNT(*) - 1")], "per", sense="MINIMIZE"),
        # A bare `COUNT(*) WHEN flag` would give the WHEN to the COUNT alone, so the bound is a product.
        "offset_count_star_top_when_per": _offset_case(1, "1", [("<=", "COUNT(*) * 1")], "top_per"),
        # An aggregate-local WHEN narrows the SUM but not the COUNT(*) on the right: three rows against two.
        "offset_count_star_local_when_per": _offset_case(1, "1", [("<=", "COUNT(*)")], "local_per"),
        "offset_count_star_local_when_fraction": _offset_case(1, "0.5", [("<=", "COUNT(*) * 0.4")], "local_per"),
        "offset_count_star_local_when_negated": _offset_case(-1, "1", [("<=", "COUNT(*) / 2")], "local_per"),
        "offset_count_star_interval_local_when": _offset_case(
            1, "0.5", [(">=", "COUNT(*) / 3"), ("<=", "COUNT(*) - 1")], "local_per"
        ),
        "offset_count_star_global_local_when": _offset_case(1, "1", [("<=", "COUNT(*)")], "local"),
        "offset_count_star_with_source_bound": _offset_case(1, "1", [("<=", "COUNT(*)"), ("<=", "cap")], "per"),
        # Pins and per-row bounds beside the offset bound.
        "offset_with_pins": _offset_case(1, "1", [("<=", 4)], "per", pins=[(1, [4]), (0, [0, 8])]),
        "offset_no_objective": _offset_case(1, "1", [(">=", 4)], "per", objective="none"),
        "offset_no_objective_negated_local": _offset_case(-1, "1", [("<=", 1)], "local_per", objective="none"),
        "offset_ties": _offset_case(1, "1", [("<=", 4)], "per", data="ties"),
    }
)


@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(S1_OFFSET_CASES))
def test_s1_offset_count_body_oracle_solver_and_direct_agree(decidb_cli, oracle_solver, name):
    _agree(decidb_cli, oracle_solver, S1_OFFSET_CASES[name])


OFFSET_CONTEXT_CASES = [
    ("offset_+1_1_le_per_0", "wide_source"),
    ("offset_-1_0p5_ge_local_per_0p5", "recombined_filters"),
    ("offset_source_bound_local_when_per", "joined_source"),
    ("offset_count_star_local_when_per", "cte_materialized"),
    ("offset_negation_only_per_lower", "parent_join"),
]


@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.sql_subquery
@pytest.mark.correctness
@pytest.mark.parametrize(
    "base,wrap", OFFSET_CONTEXT_CASES, ids=[f"{base}-{wrap}" for base, wrap in OFFSET_CONTEXT_CASES]
)
def test_s1_offset_count_body_agrees_inside_a_parent_context(decidb_cli, oracle_solver, base, wrap):
    _agree(decidb_cli, oracle_solver, S1_OFFSET_CASES[base], wrap)


# The offset is read exactly. 100 rows of 0.57 add up to exactly 57 in DuckDB's decimal arithmetic, where a double sum
# of 0.57 is 56.99999999999999: `SUM(x + 0.57) <= 57 + k` must leave room for exactly k rows.
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize("op", ["<=", ">=", "<", ">"])
@pytest.mark.parametrize("body", ["x + 0.57", "0.57 - x"])
def test_s1_offset_is_read_exactly_in_decimal_arithmetic(decidb_cli, op, body):
    score = "1.0 + i" if op in ("<=", "<") else "-1.0 - i"
    for limit in ("57", "58", "56", "57.5"):
        sql = (
            f"SELECT SUM(x) FROM (SELECT * FROM (SELECT i AS id, {score} AS score FROM range(100) r(i)) t "
            f"DECIDE x(BOOL) SUCH THAT SUM({body}) {op} {limit} MAXIMIZE SUM(score * x)) q"
        )
        direct = _outcome(decidb_cli, sql, "require")
        assert direct != ("error", "direct_miss"), f"direct solve declined:\n{sql}"
        # Exact arithmetic over 100 rows: a*count + 57 <op> limit.
        a = 1 if body.startswith("x") else -1
        wanted = [
            k
            for k in range(101)
            if {
                "<=": a * k + 57 <= Fraction(limit),
                "<": a * k + 57 < Fraction(limit),
                ">=": a * k + 57 >= Fraction(limit),
                ">": a * k + 57 > Fraction(limit),
            }[op]
        ]
        if not wanted:
            assert direct[0] == "error", (sql, direct)
        else:
            expected = max(wanted) if score.startswith("1.0") else min(wanted)
            assert direct == ("ok", [(expected,)]), (sql, direct, expected)


# Constant terms the plan cannot read exactly, and bodies that are not one `x`, stay on the solver. Each must say why
# under `require` and give under `auto` the answer `off` gives.
S1_OFFSET_DECLINED = {
    "offset_is_a_column": ("SUM(x + cap) <= 20", "a term added to x in the count must be a finite constant"),
    "offset_is_a_column_negated": ("SUM(cap - x) <= 20", "a term added to x in the count must be a finite constant"),
    "offset_is_null_column": ("SUM(x + ncap) <= 20", "a term added to x in the count must be a finite constant"),
    "offset_is_nan": ("SUM(x + 'NaN'::DOUBLE) <= 20", "a term added to x in the count must be a finite constant"),
    "offset_is_infinite": (
        "SUM(x + 'Infinity'::DOUBLE) <= 20",
        "a term added to x in the count must be a finite constant",
    ),
    "offset_is_too_large": ("SUM(x + 1e15) <= 3e15", "larger than 2097152"),
    "offset_is_just_too_large": ("SUM(x + 2097153) <= 30000000", "larger than 2097152"),
    "offset_beyond_a_double": ("SUM(x + 1e300) <= 1e300", "larger than 2097152"),
    "weight_two": ("SUM(2*x + 1) <= 20", "expected SUM(x) with unit contribution"),
    "weight_cancels": ("SUM(x - x + 1) <= 20", "expected SUM(x) with unit contribution"),
    "weight_is_a_column": ("SUM(cap*x + 1) <= 20", "expected SUM(x) with unit contribution"),
    "constant_bound_beyond_2_pow_53": ("SUM(x + 1) <= 9007199254740993", "cardinality bound must be a finite"),
    "infinite_constant_bound": ("SUM(x + 1) <= 'Infinity'::DOUBLE", "cardinality bound must be a finite"),
}


@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(S1_OFFSET_DECLINED))
def test_s1_offset_count_body_declines_what_it_cannot_read_exactly(decidb_cli, name):
    constraint, reason = S1_OFFSET_DECLINED[name]
    records = [{**row, "ncap": row["ncap"]} for row in MAIN]
    sql = (
        f"SELECT id, x FROM (FROM (VALUES {_values(records)}) t({', '.join(NAMES)}) "
        f"DECIDE x(BOOL) SUCH THAT {constraint} MAXIMIZE SUM(score*x)) q ORDER BY id"
    )
    with pytest.raises(DecidBCliError) as declined:
        _run(decidb_cli, sql, "require")
    assert "decide_direct_solve=require" in str(declined.value) and reason in str(declined.value), declined.value
    assert _outcome(decidb_cli, sql, "auto") == _outcome(decidb_cli, sql, "off")


# A bound column near the BIGINT limit with a nonzero offset: the solver subtracts in DOUBLE and cannot overflow, so
# the shifted bound must not raise a subtraction overflow on the direct path either. Whatever the solver does (an
# answer, or an error), direct solve does the same.
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize(
    "column_type,cap",
    [
        ("BIGINT", "9223372036854775807"),
        ("BIGINT", "-9223372036854775808"),
        ("BIGINT", "9223372036854775800"),
        ("UBIGINT", "18446744073709551615"),
    ],
)
@pytest.mark.parametrize("body", ["x + 1", "x - 1", "1 - x", "-1 - x", "x + 0.5", "0.5 - x"])
@pytest.mark.parametrize("op", ["<=", ">="])
def test_s1_offset_bound_near_the_bigint_limit_matches_the_solver(decidb_cli, column_type, cap, body, op):
    sql = (
        f"SELECT id, x FROM (FROM (VALUES (1, 9.0, CAST({cap} AS {column_type})), "
        f"(2, 8.0, CAST({cap} AS {column_type})), (3, 7.0, CAST({cap} AS {column_type}))) t(id, score, cap) "
        f"DECIDE x(BOOL) SUCH THAT SUM({body}) {op} cap MAXIMIZE SUM(score*x)) q ORDER BY id"
    )
    direct = _outcome(decidb_cli, sql, "require")
    assert direct != ("error", "direct_miss"), f"direct solve declined:\n{sql}"
    assert direct == _outcome(decidb_cli, sql, "off")


# ---------------------------------------------------------------------------
# S1: a count bound read through an aggregate of a data column, `SUM(x) <= MIN(cap) PER g`
# ---------------------------------------------------------------------------
#
# MIN, MAX and COUNT of any numeric column, and SUM and AVG of a TINYINT, SMALLINT, UTINYINT or USMALLINT column, give
# one value per group. The group is the rows the clause covers (its WHEN holds, its PER key is not NULL); an
# aggregate-local WHEN on the left sum does not narrow it. The oracle evaluates the aggregate over those rows in Python
# (`REDUCER_BOUND_EXPRS`) and states the bound as a plain linear constraint.

_REDUCER_OPS = ("<=", "<", ">=", ">", "=")
S1_REDUCER_CASES = {}
for _base, _op, _scope in itertools.product(_REDUCER_BASES, _REDUCER_OPS, _CORE_SCOPES):
    # A bare aggregate before a top WHEN would take the WHEN for its own.
    _bound = f"{_base} + 0" if _scope == "top_per" else _base
    S1_REDUCER_CASES[f"reducer_{_tag(_base.replace(' ', ''))}_{_OPNAMES[_op]}_{_scope}"] = {
        **_OFFSET_SCOPES[_scope][0],
        "bounds": [(_op, _bound)],
        "sense": "MINIMIZE" if _op in (">", ">=") else "MAXIMIZE",
    }

# Bodies with an offset or a negation against an aggregate bound, intervals, several bounds, pins, and no objective.
S1_REDUCER_CASES.update(
    {
        "reducer_interval_min_max_per": {"bounds": [(">=", "MIN(cap)"), ("<=", "MAX(cap)")], "per": True},
        "reducer_interval_min_max_local_when": {
            "bounds": [(">=", "MIN(cap)"), ("<=", "MAX(cap)")],
            "per": True,
            "when": "local",
        },
        "reducer_interval_avg_sum_global": {"bounds": [(">=", "AVG(sm)"), ("<=", "SUM(sm) / 2 + 1")]},
        # Group b: the unflagged row has the smallest cap, so a MIN that ignored the left side's WHEN would differ.
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
        "reducer_with_pins": {"bounds": [("<=", "MAX(cap)")], "per": True, "pins": [(1, [4]), (0, [0, 8])]},
        "reducer_no_objective_lower": {"bounds": [(">=", "MIN(cap)")], "per": True, "objective": "none"},
        "reducer_no_objective_local_when": {
            "bounds": [(">=", "AVG(sm)")],
            "per": True,
            "when": "local",
            "objective": "none",
        },
        "reducer_infeasible_lower_above_group": {"bounds": [(">=", "SUM(sm) + 100")], "per": True},
        "reducer_ties": {"data": "ties", "bounds": [("<=", "MAX(cap)")], "per": True},
        # A count body with an offset or a negation against an aggregate bound.
        "reducer_offset_min_per": _offset_case(1, "1", [("<=", "MIN(cap)")], "per"),
        "reducer_offset_max_negated_per": _offset_case(-1, "1", [("<=", "MAX(cap)")], "per", sense="MINIMIZE"),
        "reducer_offset_fractional_avg_local_when": _offset_case(1, "0.5", [("<=", "AVG(sm) + 2")], "local_per"),
        "reducer_offset_count_col_local_when": _offset_case(1, "1", [("<=", "COUNT(cap)")], "local_per"),
        "reducer_offset_negation_sum_local_when": _offset_case(
            -1, "0", [(">=", "SUM(sm) / 2 + 1")], "local_per", sense="MINIMIZE"
        ),
        "reducer_offset_top_when": _offset_case(1, "-1", [("<=", "MAX(cap) + 0")], "top_per"),
        "reducer_offset_global": _offset_case(1, "1", [("<=", "MAX(cap)")], "global"),
        "reducer_offset_interval_per": _offset_case(1, "0.5", [(">=", "MIN(cap2)"), ("<=", "MAX(cap)")], "per"),
        "reducer_offset_negated_interval_local_when": _offset_case(
            -1, "1", [(">=", "MIN(cap)"), ("<=", "MAX(cap)")], "local_per"
        ),
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
    ("reducer_offset_count_col_local_when", "joined_source"),
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


# A bad value in a column an aggregate reduces raises on both paths, on every row, as it does for any bound.
# name -> (column type, rows as (id, key, flag, value), constraint).
_REDUCER_ROWS = (
    "(1, 'a', true, 9.0, {v1}), (2, 'a', false, 8.0, {v2}), (3, 'b', true, 7.0, {v3}), (4, NULL, true, 6.0, {v4})"
)
S1_REDUCER_ERROR_CASES = {
    "null_min": ("INTEGER", ("1", "2", "3", "NULL"), "SUM(x) <= MIN(v)"),
    "null_in_a_row_the_when_excludes": ("INTEGER", ("1", "NULL", "3", "4"), "SUM(x) <= MIN(v) + 0 WHEN flag"),
    "null_in_a_row_with_a_null_key": ("INTEGER", ("1", "2", "3", "NULL"), "SUM(x) <= MAX(v) PER g"),
    "null_count_column": ("INTEGER", ("1", "2", "NULL", "4"), "SUM(x) <= COUNT(v) PER g"),
    "null_sum_column": ("TINYINT", ("1", "2", "NULL", "4"), "SUM(x) <= SUM(v) PER g"),
    "null_avg_column": ("SMALLINT", ("1", "NULL", "3", "4"), "SUM(x) <= AVG(v) PER g"),
    "nan_max": ("DOUBLE", ("1.0", "2.0", "'NaN'::DOUBLE", "4.0"), "SUM(x) <= MAX(v) PER g"),
    "nan_count_column": ("DOUBLE", ("1.0", "'NaN'::DOUBLE", "3.0", "4.0"), "SUM(x) <= COUNT(v)"),
    "nan_in_a_row_the_when_excludes": (
        "DOUBLE",
        ("1.0", "'NaN'::DOUBLE", "3.0", "4.0"),
        "SUM(x) <= MIN(v) + 0 WHEN flag",
    ),
    "nan_float_column": ("FLOAT", ("1.0", "2.0", "3.0", "'NaN'::FLOAT"), "SUM(x) <= MAX(v) PER g"),
    # No row of the clause's group: the aggregate has no value.
    "empty_group_top_when": ("INTEGER", ("1", "2", "3", "4"), "SUM(x) <= MIN(v) + 0 WHEN (id > 100)"),
    "empty_group_top_when_per": ("SMALLINT", ("1", "2", "3", "4"), "SUM(x) <= AVG(v) + 0 WHEN (id > 100) PER g"),
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
    messages = {}
    for mode in ("off", "require"):
        with pytest.raises(DecidBCliError) as error:
            _run(decidb_cli, sql, mode)
        messages[mode] = str(error.value)
    assert "decide_direct_solve=require" not in messages["require"], f"direct solve declined:\n{messages['require']}"
    assert error_class(messages["require"]) == error_class(messages["off"]), messages


# Infinity is a value of the aggregate, not an error: no limit above, or a limit no count meets.
REDUCER_INFINITY_CASES = {
    f"{aggregate}_{_OPNAMES[op]}_{'_'.join(_tag(v) for v in values)}": (aggregate, op, values)
    for aggregate in ("MAX(v)", "MIN(v)")
    for op in ("<=", ">=", "<", ">")
    for values in (("'Infinity'", "2.0", "3.0"), ("1.0", "'-Infinity'", "3.0"), ("'Infinity'", "'-Infinity'", "3.0"))
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
    assert direct != ("error", "direct_miss"), f"direct solve declined:\n{sql}"
    assert direct == _outcome(decidb_cli, sql, "off")


# What stays on the solver. Each says why under `require`, and `auto` gives the answer `off` gives. SUM and AVG over a
# column wider than SMALLINT are not exact in the solver's row-by-row DOUBLE addition (below: 2^60, 1, -2^60 adds up to
# 1 exactly and to 0 in a double), so a window could not match it.
_DECLINED_SUM_REASON = "SUM or AVG of a column that is not TINYINT, SMALLINT, UTINYINT or USMALLINT"
S1_REDUCER_DECLINED = {
    "sum_integer": ("INTEGER", ("1", "2", "3"), "SUM(v)", _DECLINED_SUM_REASON),
    "avg_integer": ("INTEGER", ("1", "2", "3"), "AVG(v)", _DECLINED_SUM_REASON),
    "sum_uinteger": ("UINTEGER", ("1", "2", "3"), "SUM(v)", _DECLINED_SUM_REASON),
    "sum_bigint": ("BIGINT", ("1", "2", "3"), "SUM(v)", _DECLINED_SUM_REASON),
    "avg_bigint": ("BIGINT", ("1", "2", "3"), "AVG(v)", _DECLINED_SUM_REASON),
    "sum_ubigint": ("UBIGINT", ("1", "2", "3"), "SUM(v)", _DECLINED_SUM_REASON),
    "sum_hugeint": ("HUGEINT", ("1", "2", "3"), "SUM(v)", _DECLINED_SUM_REASON),
    "sum_decimal": ("DECIMAL(9,2)", ("1.25", "2.5", "3"), "SUM(v)", _DECLINED_SUM_REASON),
    "avg_decimal": ("DECIMAL(9,2)", ("1.25", "2.5", "3"), "AVG(v)", _DECLINED_SUM_REASON),
    "sum_double": ("DOUBLE", ("0.1", "0.2", "0.3"), "SUM(v)", _DECLINED_SUM_REASON),
    "avg_float": ("FLOAT", ("0.1", "0.2", "0.3"), "AVG(v)", _DECLINED_SUM_REASON),
    # The solver adds in DOUBLE: 2^60 + 1 loses the 1, so the sum is 0 where the exact sum is 1.
    "sum_bigint_that_cancels": (
        "BIGINT",
        ("1152921504606846976", "1", "-1152921504606846976"),
        "SUM(v)",
        _DECLINED_SUM_REASON,
    ),
    "sum_bigint_past_2_pow_53": (
        "BIGINT",
        ("9007199254740992", "9007199254740993", "1"),
        "SUM(v)",
        _DECLINED_SUM_REASON,
    ),
    "min_with_its_own_when": ("INTEGER", ("1", "2", "3"), "MIN(v) WHEN (id > 0)", "a WHEN on the right-hand aggregate"),
    "min_with_its_own_when_in_arithmetic": (
        "INTEGER",
        ("1", "2", "3"),
        "(MIN(v) WHEN (id > 0)) + 1",
        "a WHEN on the right-hand aggregate",
    ),
    "min_of_an_expression": (
        "INTEGER",
        ("1", "2", "3"),
        "MIN(v + 1)",
        "an aggregate over something other than a numeric column",
    ),
    "divisor_is_an_aggregate": (
        "INTEGER",
        ("1", "2", "3"),
        "MAX(v) / MIN(v)",
        "a divisor that is not a nonzero constant",
    ),
    "qualified_reducer": ("INTEGER", ("1", "2", "3"), "MIN(t: v)", "variable_shape"),
}


@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(S1_REDUCER_DECLINED))
def test_s1_reducer_bound_declines_what_it_cannot_read_exactly(decidb_cli, name):
    column_type, values, reducer, reason = S1_REDUCER_DECLINED[name]
    rows = ", ".join(f"({i}, {9 - i}.0, CAST({value} AS {column_type}))" for i, value in enumerate(values))
    declaration = "t.x(BOOL)" if "t:" in reducer else "x(BOOL)"
    sql = (
        f"SELECT id FROM (FROM (VALUES {rows}) t(id, score, v) "
        f"DECIDE {declaration} SUCH THAT SUM(x) <= {reducer} MAXIMIZE SUM(score*x)) q ORDER BY id"
    )
    with pytest.raises(DecidBCliError) as declined:
        _run(decidb_cli, sql, "require")
    assert "decide_direct_solve=require" in str(declined.value) and reason in str(declined.value), declined.value
    assert _outcome(decidb_cli, sql, "auto") == _outcome(decidb_cli, sql, "off")
