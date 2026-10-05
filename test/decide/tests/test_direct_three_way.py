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
from solver.types import ObjSense, SolverStatus, VarType

from ._direct_differential import error_class
from ._direct_rule_fixtures import S1_SOLVER_ONLY_ROW_BOUNDS


@pytest.fixture(autouse=True)
def _allow_direct_path(monkeypatch):
    # The suite also runs with a process-wide forced HiGHS backend, which `require` rejects.
    monkeypatch.delenv("DECIDB_FORCE_SOLVER", raising=False)


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
)
NAMES = [name for name, _ in COLUMNS]


def _row(id, g, score, cap, flag, **extra):
    row = dict(id=id, g=g, score=score, cap=cap, flag=flag, pin=False, cap2=1.0, geq=1.0, lo=0.0, hi=1.0,
               nlo=None, nhi=None, ncap=None, fb=1.0)
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


def _dataset(case):
    return DATASETS[case.get("data", "main")]


def _literal(value, sql_type):
    if value is None:
        return f"CAST(NULL AS {sql_type})"
    if isinstance(value, bool):
        return str(value).lower()
    return f"CAST({value!r} AS {sql_type})"


def _values(records, names=NAMES):
    types = dict(COLUMNS)
    return ", ".join("(" + ", ".join(_literal(r[n], types[n]) for n in names) + ")" for r in records)


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
#   objective key of OBJECTIVES (default "score")
#   sense     "MAXIMIZE" (default) or "MINIMIZE"

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

_ROWBOUND_CONSTANTS = ("-1", "0", "0.5", "1", "2")
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
    objective_sql = OBJECTIVES[case.get("objective", "score")][0]
    return f"DECIDE x(BOOL) SUCH THAT {' AND '.join(clauses)} {case.get('sense', 'MAXIMIZE')} {objective_sql}"


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

    def limit_of(bound, row, covered):
        if isinstance(bound, (int, float)):
            return float(bound)
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
            coefficients = {names[i]: 1.0 for i in counted}
            # `COUNT(*)` counts the rows that state the bound: every row of the group under a local WHEN, the
            # flagged ones under a top WHEN.
            limits = {limit_of(limit, rows[i], len(members)) for i in members}
            sum_op = op
            for rhs in sorted(limits):
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
    coefficient = OBJECTIVES[case.get("objective", "score")][1]
    sense = ObjSense.MAXIMIZE if case.get("sense", "MAXIMIZE") == "MAXIMIZE" else ObjSense.MINIMIZE
    oracle_solver.set_objective({names[i]: float(coefficient(r)) for i, r in enumerate(rows)}, sense)
    return constraints, oracle_solver.solve()


def _run(cli, sql, mode):
    return cli.execute(f"SET decide_direct_solve='{mode}'; {sql}")


def _agree(decidb_cli, oracle_solver, case, wrap="plain"):
    """Runs the case on the oracle, the solver path and the direct path, and checks they agree."""
    sql = _wrapped_sql(case, wrap)
    rows_data = _dataset(case)
    constraints, oracle = _solve_with_oracle(oracle_solver, case)

    if oracle.status == SolverStatus.INFEASIBLE:
        for mode in ("off", "require"):
            with pytest.raises(DecidBCliError, match=r"(?i)infeasible"):
                _run(decidb_cli, sql, mode)
        return

    by_id = {r["id"]: i for i, r in enumerate(rows_data)}
    coefficient = OBJECTIVES[case.get("objective", "score")][1]
    for mode in ("off", "require"):
        rows, columns = _run(decidb_cli, sql, mode)
        assert columns == ["id", "g", "score", "x"], mode
        assert [row[0] for row in rows] == [r["id"] for r in rows_data], mode
        assert all(type(row[3]) is int and row[3] in (0, 1) for row in rows), mode

        values = {f"x_{by_id[row[0]]}": row[3] for row in rows}
        assert not _violations(constraints, values), (mode, _violations(constraints, values))
        try:
            compare_solutions(
                rows,
                columns,
                oracle,
                [(r["id"], r["g"], float(r["score"])) for r in rows_data],
                ["x"],
                coeff_fn=lambda row: {"x": float(coefficient(rows_data[by_id[row[0]]]))},
            )
        except AssertionError as error:
            raise AssertionError(f"[{mode}] {error}") from None


@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(S1_CASES))
def test_s1_oracle_solver_and_direct_agree(decidb_cli, oracle_solver, name):
    _agree(decidb_cli, oracle_solver, S1_CASES[name])


# The same problem inside a parent query. The first base case meets every context; the others meet one each,
# since a context does not interact with the kind of bound.
CONTEXT_CASES = [("global_upper_max", wrap) for wrap in (
    "cte_materialized", "cte_inlined", "wide_source", "joined_source", "correlated_source", "parent_join",
    "recombined_filters",
)] + [
    ("per_interval", "wide_source"),
    ("local_when_interval", "recombined_filters"),
    ("pins_with_group_bounds", "joined_source"),
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


def _outcome(cli, sql, mode):
    try:
        return "ok", _run(cli, sql, mode)[0]
    except DecidBCliError as error:
        return "error", error_class(str(error))


@pytest.mark.var_boolean
@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(BOUNDARY_CASES))
def test_s1_boundary_values_read_like_the_solver(decidb_cli, name):
    sql = BOUNDARY_CASES[name]
    direct = _outcome(decidb_cli, sql, "require")
    assert direct != ("error", "direct_miss"), f"direct solve declined:\n{sql}"
    assert direct == _outcome(decidb_cli, sql, "off")


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
