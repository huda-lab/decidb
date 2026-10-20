"""A1 (separable box-constrained problem), checked three ways: the independent oracle, the solver path, the direct path.

Every case is one DECIDE query over rows whose decisions do not interact. The oracle builds its own model from the
raw rows, from the bounds and objective coefficients each case states in Python (never from DeciDB's reading of the
SQL). DeciDB then runs the query with `decide_direct_solve='off'` (the solver) and `'require'` (direct solve; a query
the rule does not apply to fails instead of quietly using the solver). The case passes when all three agree:

- the oracle finds the problem feasible: both DeciDB runs return an assignment inside every row's box, whole-numbered
  for INT and BOOL, that reaches the oracle's objective (rows may differ on ties);
- the oracle finds it infeasible or unbounded: both DeciDB runs raise that error.

The solver has one behavior the oracle cannot derive from the SQL, so each bound states it: a bare unconditional
constant lower bound (`x >= -3`) replaces the variable's default lower bound of 0, while every other lower bound
(a column, a WHEN, a coefficient on x) only intersects with it. `Bound.replaces_default` pins that, and the cases that
use it are the ones that would notice if the two paths ever disagreed about it.

A bare strict bound against a fractional literal on a whole-numbered decision (`x > 0.5`, `x < 2.5`) rounds to the first
whole value inside the bound: `x > 0.5` gives 1 and `x < 2.5` gives 2. The solver once moved the bound by a whole unit
before rounding and got 2 and 1; both paths now share one function for the shift (`DecideStrictBound`). Those cases are
checked against the oracle on the direct path and on the solver path.

What a result comparison cannot see (which plan ran, EXPLAIN, prepared plans, DIAGNOSE, pruning) stays in
`test_direct_rule_conformance.py`.

Note: `oracle_solver` caches each result under the test node id plus a hash of the test function's source. The case
tables live outside that source, so editing a case does not invalidate the cache; a stale value fails loudly with an
objective mismatch. After changing a case or the model builder, remove its entries from `results/oracle_cache.json`.
"""

import itertools
import math
import random
from dataclasses import dataclass
from fractions import Fraction

import pytest

from decidb_cli import DecidBCliError
from solver.types import ObjSense, VarType

from ._direct_differential import error_class
from ._direct_oracle_common import _allow_direct_path  # noqa: F401  (autouse fixture)
from ._direct_oracle_common import agree_with_oracle, outcome as _run_all, run as _run, values_clause


INF = math.inf
BIG = 1e30  # the oracle reads a bound this large as none

# ---------------------------------------------------------------------------
# Data. Every value is a dyadic rational, so a product or a quotient of two of them is exact in a double.
# ---------------------------------------------------------------------------

COLUMNS = (
    ("id", "INTEGER"),
    ("t", "DOUBLE"),  # a target to move toward
    ("cap", "DOUBLE"),  # a per-row upper bound
    ("lo", "DOUBLE"),  # a per-row lower bound
    ("w", "DOUBLE"),  # a per-row coefficient, with positive, negative and zero values
    ("f", "BOOLEAN"),  # WHEN membership
    ("s", "DOUBLE"),  # a per-row score
)
NAMES = [name for name, _ in COLUMNS]


def _row(id, t, cap, lo, w, f, s):
    return dict(id=id, t=t, cap=cap, lo=lo, w=w, f=f, s=s)


ROWS = (
    _row(1, 8.0, 12.0, 2.0, 1.0, True, 2.0),
    _row(2, 8.0, 5.0, -3.0, -1.0, False, -3.0),
    _row(3, -3.0, 10.0, 4.0, 2.0, True, 4.0),
    _row(4, 2.5, 7.5, 0.0, 0.0, False, 0.0),
    _row(5, 6.25, 4.0, 1.5, 0.5, True, -0.5),
)


def _literal(value, sql_type):
    if value is None:
        return "NULL"
    if sql_type == "BOOLEAN":
        return "true" if value else "false"
    if sql_type == "INTEGER":
        return str(value)
    if math.isinf(value):
        return "'Infinity'::DOUBLE" if value > 0 else "'-Infinity'::DOUBLE"
    if math.isnan(value):
        return "'NaN'::DOUBLE"
    return f"{value!r}::DOUBLE"


def _values(rows):
    return values_clause(rows, NAMES, dict(COLUMNS), _literal)


# ---------------------------------------------------------------------------
# Bounds, objectives and cases
# ---------------------------------------------------------------------------


def _interval(a, op, rhs, domain):
    """The interval a clause `a*x op rhs` allows x, derived here in Python and not read from DeciDB."""
    whole = domain != "REAL"
    strict = op in ("<", ">")
    if a == 0:
        holds = {"<=": 0 <= rhs, "<": 0 < rhs, ">=": 0 >= rhs, ">": 0 > rhs, "=": 0 == rhs}[op]
        return (-INF, INF) if holds else (INF, -INF)
    value = rhs / a
    if op == "=":
        return (value, value)
    upper_side = (op in ("<=", "<") and a > 0) or (op in (">=", ">") and a < 0)
    if upper_side:
        if strict and whole and not math.isinf(value):
            value = math.ceil(value) - 1
        return (-INF, value)
    if strict and whole and not math.isinf(value):
        value = math.floor(value) + 1
    return (value, INF)


@dataclass(frozen=True)
class Bound:
    #: The clause as written.
    sql: str
    #: (row, domain) -> (lower, upper) this clause puts on the decision on that row.
    box: object
    decision: str = "x"
    #: The solver folds this clause into the variable, replacing its default lower bound of 0.
    replaces_default: bool = False


def clause(sql, op, rhs, a=lambda row: 1.0, when=None, replaces=False, decision="x"):
    def box(row, domain):
        if when is not None and not when(row):
            return (-INF, INF)
        return _interval(a(row), op, rhs(row), domain)

    return Bound(sql, box, decision, replaces)


def const(value):
    return lambda row: value


def col(name):
    return lambda row: row[name]


@dataclass(frozen=True)
class Objective:
    sql: str
    #: True, False, or None when there is no objective.
    maximize: object
    #: row -> {decision: (a, b)}: the objective function's per-row `a*x^2 + b*x`, before the sense is applied.
    coefficients: object


NO_OBJECTIVE = Objective("", None, lambda row: {})


def objective(sql, maximize, fn):
    return Objective(sql, maximize, fn)


def _masked(row, a, b, mask):
    return (a, b) if mask is None or mask(row) else (0.0, 0.0)


OBJECTIVES = {
    "lin_min": objective("MINIMIZE SUM(s*x)", False, lambda r: {"x": (0.0, r["s"])}),
    "lin_max": objective("MAXIMIZE SUM(s*x)", True, lambda r: {"x": (0.0, r["s"])}),
    "lin_const": objective("MINIMIZE SUM(s*x + 5)", False, lambda r: {"x": (0.0, r["s"])}),
    "lin_sums": objective("MAXIMIZE SUM(s*x) - SUM(w*x)", True, lambda r: {"x": (0.0, r["s"] - r["w"])}),
    "lin_scaled": objective("MINIMIZE 4*SUM(s*x)/2", False, lambda r: {"x": (0.0, 2.0 * r["s"])}),
    "lin_when": objective(
        "MAXIMIZE SUM(s*x) WHEN (f)", True, lambda r: {"x": _masked(r, 0.0, r["s"], lambda q: q["f"])}
    ),
    "sq": objective("MINIMIZE SUM(POWER(x - t, 2))", False, lambda r: {"x": (1.0, -2.0 * r["t"])}),
    "sq_max": objective("MAXIMIZE SUM(POWER(x - t, 2))", True, lambda r: {"x": (1.0, -2.0 * r["t"])}),
    "sq_neg_max": objective("MAXIMIZE -SUM(POWER(x - t, 2))", True, lambda r: {"x": (-1.0, 2.0 * r["t"])}),
    "sq_neg_min": objective("MINIMIZE SUM(-POWER(x - t, 2))", False, lambda r: {"x": (-1.0, 2.0 * r["t"])}),
    "sq_xx": objective("MINIMIZE SUM(x*x - 2*t*x)", False, lambda r: {"x": (1.0, -2.0 * r["t"])}),
    "sq_times": objective("MINIMIZE SUM((x - t)*(x - t))", False, lambda r: {"x": (1.0, -2.0 * r["t"])}),
    "sq_lin": objective("MINIMIZE SUM(POWER(x - t, 2) + s*x)", False, lambda r: {"x": (1.0, -2.0 * r["t"] + r["s"])}),
    "sq_const": objective("MINIMIZE SUM(POWER(x - t, 2) + 5)", False, lambda r: {"x": (1.0, -2.0 * r["t"])}),
    "sq_scaled": objective("MINIMIZE 3*SUM(POWER(x - t, 2))/2", False, lambda r: {"x": (1.5, -3.0 * r["t"])}),
    "sq_inside": objective("MINIMIZE SUM(2*POWER(x - t, 2) - x)", False, lambda r: {"x": (2.0, -4.0 * r["t"] - 1.0)}),
    "sq_sums": objective(
        "MINIMIZE SUM(POWER(x - t, 2)) + SUM(s*x)", False, lambda r: {"x": (1.0, -2.0 * r["t"] + r["s"])}
    ),
    "sq_inner_w": objective(
        "MINIMIZE SUM(POWER(w*x - t, 2))", False, lambda r: {"x": (r["w"] ** 2, -2.0 * r["w"] * r["t"])}
    ),
    "sq_inner_w_max": objective(
        "MAXIMIZE SUM(POWER(w*x - t, 2))", True, lambda r: {"x": (r["w"] ** 2, -2.0 * r["w"] * r["t"])}
    ),
    "sq_when": objective(
        "MINIMIZE SUM(POWER(x - t, 2)) WHEN (f)",
        False,
        lambda r: {"x": _masked(r, 1.0, -2.0 * r["t"], lambda q: q["f"])},
    ),
    "sq_lin_when": objective(
        "MINIMIZE SUM(POWER(x - t, 2) + s*x) WHEN (f)",
        False,
        lambda r: {"x": _masked(r, 1.0, -2.0 * r["t"] + r["s"], lambda q: q["f"])},
    ),
}


@dataclass(frozen=True)
class Case:
    bounds: tuple
    objective: Objective
    decisions: tuple = (("x", "REAL"),)
    rows: tuple = ROWS


def case(domain, bounds, objective_name, **kwargs):
    return Case(tuple(bounds), OBJECTIVES.get(objective_name) or NO_OBJECTIVE, (("x", domain),), **kwargs)


def _domains(case_):
    return dict(case_.decisions)


def _box(case_, row, decision):
    """The box the oracle gives one decision on one row."""
    domain = _domains(case_)[decision]
    lower, upper = -INF, INF
    replaced = False
    for bound in case_.bounds:
        if bound.decision != decision:
            continue
        lo, up = bound.box(row, domain)
        lower, upper = max(lower, lo), min(upper, up)
        replaced = replaced or bound.replaces_default
    if domain == "BOOL":
        return (max(lower, 0.0), min(upper, 1.0))
    # A written constant lower bound replaces the default of 0; any other lower bound only intersects with it.
    return (lower if replaced else max(lower, 0.0), upper)


# ---------------------------------------------------------------------------
# Running a case
# ---------------------------------------------------------------------------


def _sql(case_, wrap="plain"):
    declaration = ", ".join(f"{name}({domain})" for name, domain in case_.decisions)
    constraints = " AND ".join(bound.sql for bound in case_.bounds)
    names = ", ".join(name for name, _ in case_.decisions)
    decide = f"DECIDE {declaration} SUCH THAT {constraints} {case_.objective.sql}"
    cols = ", ".join(NAMES)
    source = f"(VALUES {_values(case_.rows)}) t({cols})"
    select = f"SELECT id, {names}"
    without_s = [n for n in NAMES if n != "s"]
    base = f"(VALUES {_values_without(case_.rows, 's')}) t({', '.join(without_s)})"
    scores = f"(VALUES {', '.join('(' + str(r['id']) + ', ' + _literal(r['s'], 'DOUBLE') + ')' for r in case_.rows)}) z(id, s)"
    ids = ", ".join(f"({r['id']})" for r in case_.rows)
    middle = case_.rows[len(case_.rows) // 2]["id"]
    if wrap == "plain":
        return f"{select} FROM (FROM {source} {decide}) q ORDER BY id"
    if wrap == "cte_materialized":
        return f"WITH src AS MATERIALIZED (SELECT * FROM {source}) {select} FROM (FROM src {decide}) q ORDER BY id"
    if wrap == "cte_inlined":
        return f"WITH src AS (SELECT * FROM {source}) {select} FROM (FROM src {decide}) q ORDER BY id"
    if wrap == "wide_source":
        wide = f"SELECT *, repeat('p', 200) AS payload, NULL::VARCHAR AS unused FROM {source}"
        return f"{select} FROM (FROM ({wide}) w {decide}) q ORDER BY id"
    if wrap == "joined_source":
        picked = ", ".join("z.s" if n == "s" else f"t.{n}" for n in NAMES)
        joined = f"SELECT {picked} FROM {base} JOIN {scores} ON z.id = t.id"
        return f"{select} FROM (FROM ({joined}) j {decide}) q ORDER BY id"
    if wrap == "correlated_source":
        picked = ", ".join(f"t.{n}" for n in without_s)
        lookup = f"(SELECT z.s FROM {scores} WHERE z.id = t.id) AS s"
        return f"{select} FROM (FROM (SELECT {picked}, {lookup} FROM {base}) u {decide}) q ORDER BY id"
    if wrap == "parent_join":
        return f"{select} FROM (FROM {source} {decide}) q JOIN (VALUES {ids}) k(id) USING (id) ORDER BY id"
    if wrap == "recombined_filters":
        # A filter pushed into the DECIDE input would solve a smaller problem. Rows are independent here, so the
        # answer is the same either way; the point is that the plan still returns every row once.
        return (
            f"WITH solved AS MATERIALIZED (FROM {source} {decide}) "
            f"{select} FROM solved WHERE id < {middle} UNION ALL "
            f"{select} FROM solved WHERE id >= {middle} ORDER BY id"
        )
    raise ValueError(wrap)


def _values_without(rows, dropped):
    return values_clause(rows, [n for n in NAMES if n != dropped], dict(COLUMNS), _literal)


def _solve_with_oracle(oracle_solver, case_):
    oracle_solver.create_model("direct_a1_separable_box")
    domains = _domains(case_)
    linear, quadratic = {}, {}
    for decision, domain in case_.decisions:
        for index, row in enumerate(case_.rows):
            lower, upper = _box(case_, row, decision)
            name = f"{decision}_{index}"
            kind = VarType.CONTINUOUS if domain == "REAL" else VarType.INTEGER
            oracle_solver.add_variable(name, kind, max(lower, -BIG), min(upper, BIG))
            for key, (a, b) in case_.objective.coefficients(row).items():
                if key == decision:
                    if b:
                        linear[name] = b
                    if a:
                        quadratic[(name, name)] = a
    sense = ObjSense.MAXIMIZE if case_.objective.maximize else ObjSense.MINIMIZE
    if quadratic:
        oracle_solver.set_quadratic_objective(linear, quadratic, sense)
    else:
        oracle_solver.set_objective(linear, sense)
    return oracle_solver.solve()


def _check_assignment(case_, rows, columns, oracle, mode):
    names = [name for name, _ in case_.decisions]
    assert columns == ["id", *names], mode
    assert [row[0] for row in rows] == [row["id"] for row in case_.rows], mode
    total = 0.0
    domains = _domains(case_)
    for result, source in zip(rows, case_.rows):
        for position, decision in enumerate(names, start=1):
            value = result[position]
            domain = domains[decision]
            lower, upper = _box(case_, source, decision)
            if domain == "REAL":
                assert isinstance(value, float), (mode, decision, value)
            else:
                assert isinstance(value, int) and not isinstance(value, bool), (mode, decision, value)
            assert lower - 1e-9 <= value <= upper + 1e-9, (mode, source["id"], decision, value, (lower, upper))
            a, b = case_.objective.coefficients(source).get(decision, (0.0, 0.0))
            total += a * value * value + b * value
    expected = oracle.objective_value
    assert abs(total - expected) <= 1e-4 * max(1.0, abs(expected)), (mode, total, expected)


def _agree(decidb_cli, oracle_solver, case_, wrap="plain", modes=("off", "require")):
    """Runs the case on the oracle and on the chosen DeciDB paths, and checks they agree."""
    sql = _sql(case_, wrap)
    oracle = _solve_with_oracle(oracle_solver, case_)

    def check_assignment(rows, columns, mode):
        _check_assignment(case_, rows, columns, oracle, mode)

    agree_with_oracle(decidb_cli, sql, oracle, check_assignment, modes)


# ---------------------------------------------------------------------------
# The case tables
# ---------------------------------------------------------------------------

T, X = "t", "x"
le, lt, ge, gt, eq = "<=", "<", ">=", ">", "="

A1_CASES = {}


def add(name, domain, bounds, objective_name):
    A1_CASES[name] = case(domain, bounds, objective_name)


UPPER_CAP = clause("x <= cap", le, col("cap"))
LOWER_ZERO_BOX = clause("x BETWEEN 0 AND cap", le, col("cap"))

# --- Constant boxes, REAL ----------------------------------------------------------------------------------------
add(
    "real_box_between",
    "REAL",
    [clause("x >= 0", ge, const(0.0), replaces=True), clause("x <= cap", le, col("cap"))],
    "sq",
)
add("real_default_lower_only_upper", "REAL", [UPPER_CAP], "sq")
add(
    "real_const_both", "REAL", [clause("x >= 1", ge, const(1.0), replaces=True), clause("x <= 6", le, const(6.0))], "sq"
)
add("real_const_equality", "REAL", [clause("x = 2", eq, const(2.0), replaces=True)], "sq")
add("real_const_upper_only", "REAL", [clause("x <= 6", le, const(6.0))], "sq")
add(
    "real_const_negative_lower_replaces_default",
    "REAL",
    [clause("x >= -3", ge, const(-3.0), replaces=True), clause("x <= 9", le, const(9.0))],
    "lin_min",
)
add(
    "real_const_negative_lower_replaces_default_sq",
    "REAL",
    [clause("x >= -3", ge, const(-3.0), replaces=True), clause("x <= cap", le, col("cap"))],
    "sq",
)
add(
    "real_const_negative_between",
    "REAL",
    [clause("x >= -2", ge, const(-2.0), replaces=True), clause("x <= 5", le, const(5.0))],
    "sq_inner_w",
)
add(
    "real_flipped_constant_lower",
    "REAL",
    [clause("-3 <= x", ge, const(-3.0), replaces=True), clause("x <= 9", le, const(9.0))],
    "lin_min",
)
add(
    "real_negative_lower_under_when_intersects",
    "REAL",
    [clause("x >= -3 WHEN (f)", ge, const(-3.0), when=col("f")), clause("x <= 9", le, const(9.0))],
    "lin_min",
)
add(
    "real_column_negative_lower_intersects",
    "REAL",
    [clause("x >= lo - 5", ge, lambda r: r["lo"] - 5.0), clause("x <= 9", le, const(9.0))],
    "lin_min",
)
add(
    "real_arithmetic_constant_intersects",
    "REAL",
    [clause("x >= 0 - 3", ge, const(-3.0)), clause("x <= 9", le, const(9.0))],
    "lin_min",
)
add(
    "real_scaled_lower_intersects",
    "REAL",
    [clause("2*x >= -6", ge, const(-6.0), a=const(2.0)), clause("x <= 9", le, const(9.0))],
    "lin_min",
)
add(
    "real_negated_upper_intersects",
    "REAL",
    [clause("-x <= 3", le, const(3.0), a=const(-1.0)), clause("x <= 9", le, const(9.0))],
    "lin_min",
)
add(
    "real_offset_lower_intersects",
    "REAL",
    [clause("x + 1 >= -2", ge, const(-3.0)), clause("x <= 9", le, const(9.0))],
    "lin_min",
)
add(
    "real_several_lowers",
    "REAL",
    [
        clause("x >= -1", ge, const(-1.0), replaces=True),
        clause("x >= lo", ge, col("lo")),
        clause("x <= cap", le, col("cap")),
    ],
    "sq",
)
add(
    "real_several_uppers",
    "REAL",
    [
        clause("x <= 6", le, const(6.0)),
        clause("x <= cap", le, col("cap")),
        clause("x <= 3 + lo", le, lambda r: 3.0 + r["lo"]),
    ],
    "sq_max",
)

# --- Row-valued bounds, with and without WHEN ----------------------------------------------------------------------
add("real_column_lower_upper", "REAL", [clause("x >= lo", ge, col("lo")), clause("x <= cap", le, col("cap"))], "sq")
add(
    "real_column_equality_when",
    "REAL",
    [clause("x = lo WHEN (f)", eq, col("lo"), when=col("f")), clause("x <= cap", le, col("cap"))],
    "sq",
)
add(
    "real_upper_when",
    "REAL",
    [clause("x <= 1 WHEN (f)", le, const(1.0), when=col("f")), clause("x <= 9", le, const(9.0))],
    "sq_lin",
)
add(
    "real_cap_when",
    "REAL",
    [clause("x <= cap WHEN (f)", le, col("cap"), when=col("f")), clause("x <= 20", le, const(20.0))],
    "sq_max",
)
add(
    "real_lower_when",
    "REAL",
    [clause("x >= lo WHEN (f)", ge, col("lo"), when=col("f")), clause("x <= cap", le, col("cap"))],
    "sq",
)
add(
    "real_two_whens",
    "REAL",
    [
        clause("x >= 2 WHEN (f)", ge, const(2.0), when=col("f"), replaces=False),
        clause("x <= 3 WHEN (id > 3)", le, const(3.0), when=lambda r: r["id"] > 3),
        clause("x <= 9", le, const(9.0)),
    ],
    "lin_min",
)

# --- Coefficient forms -----------------------------------------------------------------------------------------------
add("real_scaled_upper", "REAL", [clause("2*x <= cap", le, col("cap"), a=const(2.0))], "lin_max")
add("real_negated_lower", "REAL", [clause("-x >= -cap", ge, lambda r: -r["cap"], a=const(-1.0))], "lin_max")
add("real_reversed_sides", "REAL", [clause("cap >= x", le, col("cap"))], "lin_max")
add("real_offset_upper", "REAL", [clause("x - 3 <= 0", le, const(3.0))], "lin_max")
add("real_offset_column", "REAL", [clause("x + 1 <= cap", le, lambda r: r["cap"] - 1.0)], "lin_max")
add(
    "real_row_coefficient_upper",
    "REAL",
    [clause("w*x <= cap", le, col("cap"), a=col("w")), clause("x <= 100", le, const(100.0))],
    "sq",
)
add(
    "real_row_coefficient_lower",
    "REAL",
    [clause("w*x >= -cap", ge, lambda r: -r["cap"], a=col("w")), clause("x <= 100", le, const(100.0))],
    "lin_min",
)
add(
    "real_zero_coefficient_holds",
    "REAL",
    [clause("w*x <= 5", le, const(5.0), a=col("w")), clause("x <= 4", le, const(4.0))],
    "lin_max",
)
add(
    "real_zero_constant_coefficient_holds",
    "REAL",
    [clause("0*x <= 5", le, const(5.0), a=const(0.0)), clause("x <= 4", le, const(4.0))],
    "lin_max",
)
add("real_like_terms", "REAL", [clause("2*x + 3*x <= cap", le, col("cap"), a=const(5.0))], "lin_max")
add(
    "real_negative_coefficient_equality",
    "REAL",
    [clause("-2*x = -lo", eq, lambda r: -r["lo"], a=const(-2.0), replaces=False), clause("x <= 100", le, const(100.0))],
    "sq",
)

# --- Infeasible and unbounded ---------------------------------------------------------------------------------------
add(
    "infeasible_constants",
    "REAL",
    [clause("x >= 5", ge, const(5.0), replaces=True), clause("x <= 3", le, const(3.0))],
    "sq",
)
add("infeasible_one_row", "REAL", [clause("x >= lo", ge, col("lo")), clause("x <= 1", le, const(1.0))], "sq")
add(
    "infeasible_zero_coefficient",
    "REAL",
    [clause("0*x >= 5", ge, const(5.0), a=const(0.0)), clause("x <= cap", le, col("cap"))],
    "sq",
)
add(
    "infeasible_column_when",
    "REAL",
    [clause("x >= lo + 20 WHEN (f)", ge, lambda r: r["lo"] + 20.0, when=col("f")), clause("x <= cap", le, col("cap"))],
    "sq",
)
add("unbounded_max_linear", "REAL", [clause("x >= 0", ge, const(0.0), replaces=True)], "lin_max")
add("unbounded_min_negative_score", "REAL", [clause("x >= 0", ge, const(0.0), replaces=True)], "lin_min")
add("unbounded_concave", "REAL", [clause("x >= 0", ge, const(0.0), replaces=True)], "sq_neg_min")
add(
    "bounded_concave",
    "REAL",
    [clause("x >= 0", ge, const(0.0), replaces=True), clause("x <= cap", le, col("cap"))],
    "sq_neg_min",
)
add(
    "bounded_convex_max",
    "REAL",
    [clause("x >= 0", ge, const(0.0), replaces=True), clause("x <= cap", le, col("cap"))],
    "sq_max",
)
add("unbounded_convex_max", "REAL", [clause("x >= 0", ge, const(0.0), replaces=True)], "sq_max")
add("unbounded_one_row", "REAL", [clause("x <= cap WHEN (f)", le, col("cap"), when=col("f"))], "lin_min")
add(
    "optimal_when_the_open_row_is_outside_the_objective",
    "REAL",
    [clause("x <= cap WHEN (f)", le, col("cap"), when=col("f"))],
    "lin_when",
)

# --- Objectives, REAL, over a box ------------------------------------------------------------------------------------
for objective_name in OBJECTIVES:
    add(
        f"real_objective_{objective_name}",
        "REAL",
        [clause("x >= 0", ge, const(0.0), replaces=True), clause("x <= cap", le, col("cap"))],
        objective_name,
    )
add(
    "real_no_objective",
    "REAL",
    [clause("x >= 1", ge, const(1.0), replaces=True), clause("x <= cap", le, col("cap"))],
    "none",
)
add(
    "real_no_objective_negative_lower",
    "REAL",
    [clause("x >= -3", ge, const(-3.0), replaces=True), clause("x <= cap", le, col("cap"))],
    "none",
)
add("real_no_objective_open_above", "REAL", [clause("x >= lo", ge, col("lo"))], "none")
add(
    "real_linear_zero_slope",
    "REAL",
    [clause("x >= 1", ge, const(1.0), replaces=True), clause("x <= cap", le, col("cap"))],
    "lin_when",
)

# --- INT -------------------------------------------------------------------------------------------------------------
INT_BOX = [clause("x >= 0", ge, const(0.0), replaces=True), clause("x <= cap", le, col("cap"))]
for objective_name in OBJECTIVES:
    add(f"int_objective_{objective_name}", "INT", INT_BOX, objective_name)
add("int_fractional_cap", "INT", [clause("x <= cap/3", le, lambda r: r["cap"] / 3.0)], "lin_max")
add(
    "int_fractional_lower",
    "INT",
    [clause("x >= lo/3", ge, lambda r: r["lo"] / 3.0), clause("x <= cap", le, col("cap"))],
    "lin_min",
)
add(
    "int_fractional_constants",
    "INT",
    [clause("x >= 0.5", ge, const(0.5), replaces=True), clause("x <= 6.5", le, const(6.5))],
    "sq",
)
add(
    "int_empty_window",
    "INT",
    [clause("x >= 0.3", ge, const(0.3), replaces=True), clause("x <= 0.7", le, const(0.7))],
    "lin_min",
)
add(
    "int_empty_window_one_row",
    "INT",
    [clause("x >= lo/4", ge, lambda r: r["lo"] / 4.0), clause("x <= lo/4 + 0.25", le, lambda r: r["lo"] / 4.0 + 0.25)],
    "lin_min",
)
add("int_equality_fractional", "INT", [clause("x = cap/2", eq, lambda r: r["cap"] / 2.0)], "lin_min")
add("int_equality_whole", "INT", [clause("x = 2", eq, const(2.0), replaces=True)], "lin_min")
add(
    "int_equality_column_when",
    "INT",
    [clause("x = lo WHEN (f)", eq, col("lo"), when=col("f")), clause("x <= cap", le, col("cap"))],
    "lin_min",
)
add(
    "int_strict_whole_constants",
    "INT",
    [clause("x > 1", gt, const(1.0), replaces=True), clause("x < 7", lt, const(7.0))],
    "sq",
)
add("int_strict_column", "INT", [clause("x < cap", lt, col("cap"))], "lin_max")
add("int_strict_lower_column", "INT", [clause("x > lo", gt, col("lo")), clause("x <= cap", le, col("cap"))], "lin_min")
add(
    "int_strict_when",
    "INT",
    [clause("x < 2.5 WHEN (f)", lt, const(2.5), when=col("f")), clause("x <= 7", le, const(7.0))],
    "lin_max",
)
add("int_strict_affine", "INT", [clause("2*x < cap", lt, col("cap"), a=const(2.0))], "lin_max")
add(
    "int_strict_affine_lower",
    "INT",
    [clause("2*x > lo", gt, col("lo"), a=const(2.0)), clause("x <= cap", le, col("cap"))],
    "lin_min",
)
add(
    "int_row_coefficient_upper",
    "INT",
    [clause("w*x <= cap", le, col("cap"), a=col("w")), clause("x <= 50", le, const(50.0))],
    "lin_max",
)
add("int_row_coefficient_fraction", "INT", [clause("3*x <= cap", le, col("cap"), a=const(3.0))], "lin_max")
add("int_row_coefficient_negative", "INT", [clause("-3*x >= -cap", ge, lambda r: -r["cap"], a=const(-3.0))], "lin_max")
add("int_tie_half", "INT", [clause("x <= 9", le, const(9.0))], "sq_scaled")
add(
    "int_negative_lower",
    "INT",
    [clause("x >= -4", ge, const(-4.0), replaces=True), clause("x <= 4", le, const(4.0))],
    "sq_inner_w",
)
add(
    "int_negative_lower_column_intersects",
    "INT",
    [clause("x >= lo - 6", ge, lambda r: r["lo"] - 6.0), clause("x <= 4", le, const(4.0))],
    "lin_min",
)
add("int_large_bound", "INT", [clause("x <= 1000000000000", le, const(1e12))], "lin_when")
add(
    "int_upper_when_default_open",
    "INT",
    [clause("x <= 3 WHEN (f)", le, const(3.0), when=col("f")), clause("x <= 10", le, const(10.0))],
    "lin_max",
)

# --- BOOL ------------------------------------------------------------------------------------------------------------
BOOL_BOX = [clause("x <= 1", le, const(1.0))]
for objective_name in ("lin_min", "lin_max", "lin_when", "sq", "sq_lin", "sq_inner_w", "sq_neg_max", "none"):
    add(f"bool_objective_{objective_name}", "BOOL", BOOL_BOX, objective_name)
add("bool_fixed_one", "BOOL", [clause("x = 1", eq, const(1.0), replaces=True)], "lin_min")
add("bool_fixed_zero", "BOOL", [clause("x <= 0", le, const(0.0))], "lin_max")
add(
    "bool_fixed_when",
    "BOOL",
    [clause("x = 1 WHEN (f)", eq, const(1.0), when=col("f"), replaces=False), clause("x <= 1", le, const(1.0))],
    "lin_min",
)
add("bool_lower_when_column", "BOOL", [clause("x >= lo WHEN (f)", ge, col("lo"), when=col("f"))], "lin_min")
add("bool_fractional_cap", "BOOL", [clause("x <= cap/10", le, lambda r: r["cap"] / 10.0)], "lin_max")
add("bool_strict_upper", "BOOL", [clause("x < 1", lt, const(1.0))], "lin_max")
add("bool_strict_lower", "BOOL", [clause("x > 0", gt, const(0.0), replaces=True)], "lin_min")
add("bool_scaled", "BOOL", [clause("2*x <= 1", le, const(1.0), a=const(2.0))], "lin_max")
add("bool_row_coefficient", "BOOL", [clause("w*x <= 1", le, const(1.0), a=col("w"))], "lin_max")

# --- Several independent decisions -----------------------------------------------------------------------------------
A1_CASES["two_decisions_mixed"] = Case(
    (
        clause("x >= 0", ge, const(0.0), replaces=True),
        clause("x <= cap", le, col("cap")),
        clause("y >= 0", ge, const(0.0), replaces=True, decision="y"),
        clause("y <= 5", le, const(5.0), decision="y"),
    ),
    Objective(
        "MINIMIZE SUM(POWER(x - t, 2)) + SUM(s*y)",
        False,
        lambda r: {"x": (1.0, -2.0 * r["t"]), "y": (0.0, r["s"])},
    ),
    (("x", "REAL"), ("y", "INT")),
)
A1_CASES["three_decisions_all_domains"] = Case(
    (
        clause("x <= cap", le, col("cap")),
        clause("y <= 4", le, const(4.0), decision="y"),
        clause("z <= 1", le, const(1.0), decision="z"),
    ),
    Objective(
        "MAXIMIZE SUM(s*x) + SUM(w*y) + SUM(t*z)",
        True,
        lambda r: {"x": (0.0, r["s"]), "y": (0.0, r["w"]), "z": (0.0, r["t"])},
    ),
    (("x", "REAL"), ("y", "INT"), ("z", "BOOL")),
)
A1_CASES["decision_unused_in_objective"] = Case(
    (
        clause("x <= cap", le, col("cap")),
        clause("y >= 2", ge, const(2.0), replaces=True, decision="y"),
        clause("y <= 8", le, const(8.0), decision="y"),
    ),
    Objective("MAXIMIZE SUM(s*x)", True, lambda r: {"x": (0.0, r["s"])}),
    (("x", "REAL"), ("y", "INT")),
)
A1_CASES["two_decisions_one_infeasible"] = Case(
    (
        clause("x <= cap", le, col("cap")),
        clause("y >= 3", ge, const(3.0), replaces=True, decision="y"),
        clause("y <= 2", le, const(2.0), decision="y"),
    ),
    Objective("MAXIMIZE SUM(s*x)", True, lambda r: {"x": (0.0, r["s"])}),
    (("x", "REAL"), ("y", "INT")),
)

# A bare strict bound against a fractional literal on a whole-numbered decision (the solver once moved it by a whole unit
# before rounding). Checked against the oracle on the direct path and on the solver path.
SOLVER_OFF_BY_ONE = {
    "int_strict_lower_fraction": case(
        "INT", [clause("x > 0.5", gt, const(0.5), replaces=True), clause("x <= 9", le, const(9.0))], "lin_min"
    ),
    "int_strict_upper_fraction": case(
        "INT", [clause("x >= 0", ge, const(0.0), replaces=True), clause("x < 2.5", lt, const(2.5))], "lin_max"
    ),
    "int_strict_both_fractions": case(
        "INT", [clause("x > 1.5", gt, const(1.5), replaces=True), clause("x < 6.5", lt, const(6.5))], "sq"
    ),
    "int_strict_fraction_quadratic": case(
        "INT", [clause("x > 0.25", gt, const(0.25), replaces=True), clause("x < 3.75", lt, const(3.75))], "sq_scaled"
    ),
    "bool_strict_lower_fraction": case("BOOL", [clause("x > 0.5", gt, const(0.5), replaces=True)], "lin_min"),
    "bool_strict_upper_fraction": case("BOOL", [clause("x < 1.5", lt, const(1.5))], "lin_max"),
}


@pytest.mark.var_real
@pytest.mark.var_integer
@pytest.mark.var_boolean
@pytest.mark.cons_perrow
@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(A1_CASES))
def test_a1_oracle_solver_and_direct_agree(decidb_cli, oracle_solver, name):
    _agree(decidb_cli, oracle_solver, A1_CASES[name])


@pytest.mark.var_integer
@pytest.mark.cons_perrow
@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(SOLVER_OFF_BY_ONE))
def test_a1_reads_a_fractional_strict_bound_correctly(decidb_cli, oracle_solver, name):
    _agree(decidb_cli, oracle_solver, SOLVER_OFF_BY_ONE[name], modes=("require",))


@pytest.mark.var_integer
@pytest.mark.cons_perrow
@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(SOLVER_OFF_BY_ONE))
def test_a1_solver_off_by_one_on_a_fractional_strict_bound(decidb_cli, oracle_solver, name):
    _agree(decidb_cli, oracle_solver, SOLVER_OFF_BY_ONE[name], modes=("off",))


# A bound that contradicts the variable's built-in domain (an upper bound below the default floor of 0, or a BOOL lower
# bound above 1) is rejected by the solver before it reads any row, on an empty source too. No rule applies to
# such a query, so it reaches the solver and raises the solver's message whatever the table holds.
DECLINED_CASES = {
    "real_upper_below_default": case("REAL", [clause("x <= -1", le, const(-1.0))], "lin_min"),
    "int_upper_below_default": case("INT", [clause("x <= -1", le, const(-1.0))], "lin_min"),
    "bool_upper_below_zero": case("BOOL", [clause("x <= -1", le, const(-1.0))], "lin_min"),
    "bool_lower_above_one": case("BOOL", [clause("x >= 2", ge, const(2.0), replaces=True)], "lin_min"),
}


@pytest.mark.error
@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(DECLINED_CASES))
def test_a1_declines_a_bound_that_contradicts_the_built_in_domain(decidb_cli, oracle_solver, name):
    case_ = DECLINED_CASES[name]
    # The oracle agrees the problem is infeasible; the solver and the default path raise it, and `require` reports
    # that no rule applies to the query.
    _agree(decidb_cli, oracle_solver, case_, modes=("off", "auto"))
    with pytest.raises(DecidBCliError, match="decide_direct_solve=require"):
        _run(decidb_cli, _sql(case_), "require")


@pytest.mark.error
@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(DECLINED_CASES))
def test_a1_declined_domain_bound_raises_the_same_on_an_empty_source(decidb_cli, name):
    case_ = DECLINED_CASES[name]
    declaration = ", ".join(f"{name}({domain})" for name, domain in case_.decisions)
    constraints = " AND ".join(bound.sql for bound in case_.bounds)
    empty = f"(SELECT * FROM (VALUES {_values(ROWS[:1])}) t({', '.join(NAMES)}) WHERE false)"
    sql = f"SELECT id, x FROM (FROM {empty} DECIDE {declaration} SUCH THAT {constraints} {case_.objective.sql}) q"
    messages = {}
    for mode in ("off", "auto"):
        with pytest.raises(DecidBCliError, match=r"(?i)infeasible") as error:
            _run(decidb_cli, sql, mode)
        messages[mode] = str(error.value)
    assert messages["off"] == messages["auto"]
    with pytest.raises(DecidBCliError, match="decide_direct_solve=require"):
        _run(decidb_cli, sql, "require")


# The same problem inside a parent query. One base case meets every context; a context does not interact with the kind
# of bound.
CONTEXTS = (
    "cte_materialized",
    "cte_inlined",
    "wide_source",
    "joined_source",
    "correlated_source",
    "parent_join",
    "recombined_filters",
)
CONTEXT_CASES = [
    (base, wrap)
    for base in ("real_objective_sq_when", "int_objective_sq_inner_w", "bool_objective_sq_lin", "two_decisions_mixed")
    for wrap in CONTEXTS
]


@pytest.mark.correctness
@pytest.mark.parametrize("base,wrap", CONTEXT_CASES)
def test_a1_agrees_inside_a_parent_context(decidb_cli, oracle_solver, base, wrap):
    _agree(decidb_cli, oracle_solver, A1_CASES[base], wrap=wrap)


@pytest.mark.correctness
def test_a1_empty_source_returns_no_rows_whatever_the_bounds_say(decidb_cli):
    for bounds in ("x >= 5 AND x <= 3", "x >= 0"):
        for objective_sql in ("MAXIMIZE SUM(x)", "MINIMIZE SUM(POWER(x - 1, 2))", ""):
            sql = (
                "SELECT id, x FROM (SELECT 1 AS id, 1.0 AS t WHERE false) "
                f"DECIDE x(REAL) SUCH THAT {bounds} {objective_sql}"
            )
            for mode in ("off", "require"):
                assert _run(decidb_cli, sql, mode)[0] == []


@pytest.mark.correctness
def test_a1_binder_rejects_strict_bounds_on_real_before_direct_solve_is_asked(decidb_cli):
    for mode in ("off", "require"):
        with pytest.raises(DecidBCliError, match="Strict inequality"):
            _run(decidb_cli, "SELECT x FROM (SELECT 1 AS id) DECIDE x(REAL) SUCH THAT x < 3 MAXIMIZE SUM(x)", mode)


# ---------------------------------------------------------------------------
# Errors: both paths must raise
# ---------------------------------------------------------------------------


def _problem(
    rows, bounds="x BETWEEN 0 AND cap", obj="MINIMIZE SUM(POWER(x - t, 2))", decl="x(REAL)", tail="ORDER BY id"
):
    return (
        f"SELECT id, x FROM (SELECT * FROM (VALUES {_values(rows)}) v({', '.join(NAMES)})) "
        f"DECIDE {decl} SUCH THAT {bounds} {obj} {tail}"
    )


def _with(index, **changes):
    rows = [dict(row) for row in ROWS]
    rows[index].update(changes)
    return rows


def _late(decide_text, t_sql="1.0", s_sql="1.0", cap_sql="10.0", tail=""):
    """A 5,000-row problem whose last row is built by the given SQL. Rows 0 to 4998 are ordinary."""
    return (
        f"SELECT i, x FROM (SELECT i, {t_sql} AS t, {s_sql} AS s, {cap_sql} AS cap FROM range(5000) r(i)) "
        f"DECIDE {decide_text} {tail}"
    )


# name -> (sql, error class the direct path must raise, or None to accept any).
A1_ERROR_CASES = {
    "null_cap": (_problem(_with(1, cap=None)), "null_bound"),
    "nan_cap": (_problem(_with(1, cap=math.nan)), "null_bound"),
    "null_cap_on_row_a_when_excludes": (
        _problem(_with(1, cap=None), bounds="x <= cap WHEN (f) AND x >= 0"),
        "null_bound",
    ),
    "nan_cap_on_row_a_when_excludes": (
        _problem(_with(1, cap=math.nan), bounds="x <= cap WHEN (f) AND x >= 0"),
        "null_bound",
    ),
    "null_lower_bound": (_problem(_with(2, lo=None), bounds="x >= lo AND x <= cap"), "null_bound"),
    "null_target": (_problem(_with(1, t=None)), "null_bound"),
    "nan_target": (_problem(_with(1, t=math.nan)), "invalid_value"),
    "infinite_target": (_problem(_with(1, t=INF)), "invalid_value"),
    "null_target_on_row_the_objective_when_excludes": (
        _problem(_with(1, t=None), obj="MINIMIZE SUM(POWER(x - t, 2)) WHEN (f)"),
        "null_bound",
    ),
    "nan_score": (_problem(_with(1, s=math.nan), obj="MAXIMIZE SUM(s*x)"), "invalid_value"),
    "null_coefficient": (_problem(_with(0, w=None), bounds="w*x <= cap AND x <= 5"), "null_bound"),
    "infinite_coefficient": (_problem(_with(0, w=INF), bounds="w*x <= cap AND x <= 5"), "invalid_value"),
    "objective_when_matches_no_row": (
        _problem(ROWS, obj="MINIMIZE SUM(POWER(x - t, 2)) WHEN (id > 99)"),
        "empty_aggregate",
    ),
    "objective_part_when_matches_no_row": (
        _problem(ROWS, obj="MINIMIZE SUM(POWER(x - t, 2)) + SUM(s*x) WHEN (id > 99)"),
        "empty_aggregate",
    ),
    "throwing_cast_on_row_a_when_excludes": (
        _problem(_with(1, cap=1000.0), bounds="x <= CAST(cap AS TINYINT) WHEN (f) AND x >= 0"),
        None,
    ),
    "throwing_cast_in_objective": (_problem(_with(1, cap=1000.0), obj="MAXIMIZE SUM(CAST(cap AS TINYINT)*x)"), None),
    "division_by_zero_in_objective": (_problem(ROWS, obj="MAXIMIZE SUM((s/(s-2.0))*x)"), "invalid_value"),
    "overflow_in_bound": (
        _problem(ROWS, decl="x(INT)", bounds="x <= CAST(cap AS BIGINT) * 9223372036854775807"),
        None,
    ),
    "square_weight_overflows": (_problem(_with(1, w=1e200), obj="MINIMIZE SUM(POWER(w*x - 1, 2))"), None),
    "infeasible_constants": (_problem(ROWS, bounds="x >= 5 AND x <= 3"), "infeasible"),
    "infeasible_one_row_in_many": (_problem(_with(2, cap=-1.0)), "infeasible"),
    "infeasible_zero_coefficient": (_problem(ROWS, bounds="0*x >= 5 AND x <= cap"), "infeasible"),
    "infeasible_infinite_lower": (_problem(ROWS, bounds="x >= ('Infinity'::DOUBLE) AND x <= cap"), "infeasible"),
    "infeasible_negative_infinite_cap": (_problem(_with(1, cap=-INF)), "infeasible"),
    "infeasible_int_window": (_problem(ROWS, decl="x(INT)", bounds="x >= 0.3 AND x <= 0.7"), "infeasible"),
    "unbounded_linear_max": (_problem(ROWS, bounds="x >= 0", obj="MAXIMIZE SUM(x)"), "unbounded"),
    "unbounded_linear_min": (_problem(ROWS, bounds="x >= 0", obj="MINIMIZE SUM(s*x)"), "unbounded"),
    "unbounded_concave": (_problem(ROWS, bounds="x >= 0", obj="MINIMIZE SUM(-POWER(x - t, 2))"), "unbounded"),
    "unbounded_convex_max": (_problem(ROWS, bounds="x >= 0", obj="MAXIMIZE SUM(POWER(x - t, 2))"), "unbounded"),
    "unbounded_cap_is_1e20": (_problem(ROWS, bounds="x <= 1e20", obj="MAXIMIZE SUM(x)"), "unbounded"),
    "unbounded_row_cap_is_1e20": (_problem(_with(1, cap=1e20), bounds="x <= cap", obj="MAXIMIZE SUM(x)"), "unbounded"),
    "unbounded_row_cap_infinite": (_problem(_with(1, cap=INF), bounds="x <= cap", obj="MAXIMIZE SUM(x)"), "unbounded"),
    "unbounded_int": (_problem(ROWS, decl="x(INT)", bounds="x >= 0", obj="MAXIMIZE SUM(x)"), "unbounded"),
    "int_beyond_2_53": (
        _problem(ROWS, decl="x(INT)", bounds="x <= 1e17 AND x >= 0", obj="MAXIMIZE SUM(x)"),
        "int_range",
    ),
    "int_just_beyond_2_53": (
        _problem(ROWS, decl="x(INT)", bounds="x <= 9007199254740994 AND x >= 0", obj="MAXIMIZE SUM(x)"),
        "int_range",
    ),
    # Late rows: the error must raise however little of the result is read.
    "late_null_target_under_limit": (
        _late(
            "x(REAL) SUCH THAT x BETWEEN 0 AND cap MINIMIZE SUM(POWER(x - t, 2))",
            t_sql="CASE WHEN i = 4999 THEN NULL ELSE 1.0 END",
            tail="LIMIT 1",
        ),
        "null_bound",
    ),
    "late_infeasible_row_under_limit": (
        _late(
            "x(REAL) SUCH THAT x BETWEEN 0 AND cap MINIMIZE SUM(POWER(x - t, 2))",
            cap_sql="CASE WHEN i = 4999 THEN -1.0 ELSE 10.0 END",
            tail="LIMIT 1",
        ),
        "infeasible",
    ),
    "late_unbounded_row_under_limit": (
        _late(
            "x(REAL) SUCH THAT x <= cap MAXIMIZE SUM(s*x)",
            cap_sql="CASE WHEN i = 4999 THEN 1e30 ELSE 10.0 END",
            tail="LIMIT 1",
        ),
        "unbounded",
    ),
}


@pytest.mark.error
@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(A1_ERROR_CASES))
def test_a1_error_cases_fail_on_both_paths(decidb_cli, name):
    sql, expected = A1_ERROR_CASES[name]
    with pytest.raises(DecidBCliError) as solver_error:
        _run(decidb_cli, sql, "off")
    with pytest.raises(DecidBCliError) as direct_error:
        _run(decidb_cli, sql, "require")
    assert "decide_direct_solve=require" not in str(direct_error.value), "no rule applied to the query"
    if expected is not None:
        assert error_class(str(direct_error.value)) == expected, str(direct_error.value)


@pytest.mark.error
@pytest.mark.correctness
def test_a1_late_bad_row_raises_under_count_and_where(decidb_cli):
    inner = _late(
        "x(REAL) SUCH THAT x BETWEEN 0 AND cap MINIMIZE SUM(POWER(x - t, 2))",
        t_sql="CASE WHEN i = 4999 THEN 'NaN'::DOUBLE ELSE 1.0 END",
    )
    for outer in (f"SELECT COUNT(*) FROM ({inner}) q", f"SELECT i FROM ({inner}) q WHERE i = 0"):
        with pytest.raises(DecidBCliError, match="invalid value"):
            _run(decidb_cli, outer, "require")
    assert _run(decidb_cli, f"SELECT i FROM ({inner}) q LIMIT 0", "require")[0] == []


@pytest.mark.error
@pytest.mark.correctness
def test_a1_an_unused_decision_still_raises(decidb_cli):
    # The solver reads the whole problem even when no column of x is selected.
    sql = f"SELECT id FROM ({_problem(ROWS, bounds='x >= 0', obj='MAXIMIZE SUM(x)', tail='')}) q"
    for mode in ("off", "require"):
        with pytest.raises(DecidBCliError, match="unbounded"):
            _run(decidb_cli, sql, mode)


# ---------------------------------------------------------------------------
# Values the oracle cannot hold: direct solve is compared with the solver path alone
# ---------------------------------------------------------------------------

ONLY_FIRST = "MAXIMIZE SUM(CASE WHEN id = 1 THEN 1.0 ELSE 0.0 END * x)"
ONLY_FIRST_TWO = "MAXIMIZE SUM(CASE WHEN id <= 2 THEN 1.0 ELSE 0.0 END * x)"

BOUNDARY_CASES = {
    "infinite_cap_is_no_bound": _problem(_with(1, cap=INF), bounds="x <= cap AND x >= 0"),
    "infinite_cap_when_excluded": _problem(_with(1, cap=INF), bounds="x <= cap WHEN (f) AND x >= 0 AND x <= 20"),
    "negative_infinite_lower_keeps_default": _problem(_with(1, lo=-INF), bounds="x >= lo AND x <= cap"),
    "negative_infinite_constant_lower_is_no_bound": _problem(ROWS, bounds="x >= ('-Infinity'::DOUBLE) AND x <= cap"),
    "cap_5e19_is_a_bound": _problem(_with(1, cap=5e19), bounds="x <= cap", obj=ONLY_FIRST_TWO),
    "row_lower_1e25": _problem(_with(1, cap=1e25), bounds="x >= cap", obj="MINIMIZE SUM(x)"),
    "constant_upper_5e19": _problem(ROWS, bounds="x <= 5e19", obj=ONLY_FIRST),
    "int_exactly_2_53": _problem(ROWS, decl="x(INT)", bounds="x <= 9007199254740992 AND x >= 0", obj="MAXIMIZE SUM(x)"),
    "int_2_53_minus_1": _problem(ROWS, decl="x(INT)", bounds="x <= 9007199254740991 AND x >= 0", obj="MAXIMIZE SUM(x)"),
    "int_large_whole_bound": _problem(
        ROWS, decl="x(INT)", bounds="x <= 1000000000000000 AND x >= 0", obj="MAXIMIZE SUM(x)"
    ),
    "int_negative_2_53": _problem(
        ROWS, decl="x(INT)", bounds="x >= -9007199254740992 AND x <= 0", obj="MINIMIZE SUM(x)"
    ),
    "real_large_values": _problem(ROWS, bounds="x <= 1e17 AND x >= 0", obj="MAXIMIZE SUM(x)"),
    "real_tiny_quadratic": _problem(ROWS, bounds="x <= 1e6 AND x >= 0", obj="MINIMIZE SUM(1e-9*POWER(x - t, 2))"),
    "decimal_source": (
        "SELECT id, x FROM (SELECT 1 AS id, 2.50::DECIMAL(9,2) AS t, 4.00::DECIMAL(9,2) AS cap) "
        "DECIDE x(REAL) SUCH THAT x BETWEEN 0 AND cap MINIMIZE SUM(POWER(x - t, 2))"
    ),
    "float_source": (
        "SELECT id, x FROM (SELECT 1 AS id, 2.5::FLOAT AS t, 4.0::FLOAT AS cap) "
        "DECIDE x(REAL) SUCH THAT x BETWEEN 0 AND cap MINIMIZE SUM(POWER(x - t, 2))"
    ),
    "tinyint_source": (
        "SELECT id, x FROM (SELECT 1 AS id, 3::TINYINT AS t, 9::TINYINT AS cap) "
        "DECIDE x(INT) SUCH THAT x BETWEEN 0 AND cap MINIMIZE SUM(POWER(x - t, 2))"
    ),
    "hugeint_source": (
        "SELECT id, x FROM (SELECT 1 AS id, 25::HUGEINT AS t, 40::HUGEINT AS cap) "
        "DECIDE x(INT) SUCH THAT x BETWEEN 0 AND cap MINIMIZE SUM(POWER(x - t, 2))"
    ),
    "unsigned_bigint_source": (
        "SELECT id, x FROM (SELECT 1 AS id, 25::UBIGINT AS t, 40::UBIGINT AS cap) "
        "DECIDE x(INT) SUCH THAT x BETWEEN 0 AND cap MINIMIZE SUM(POWER(x - t, 2))"
    ),
    "boolean_column_bound_int": (
        "SELECT id, x FROM (SELECT * FROM (VALUES (1, true, 2.0::DOUBLE), (2, false, 3.0::DOUBLE)) v(id, f, s)) "
        "DECIDE x(INT) SUCH THAT x <= f MAXIMIZE SUM(s*x)"
    ),
    "boolean_column_bound_bool": (
        "SELECT id, x FROM (SELECT * FROM (VALUES (1, true, 2.0::DOUBLE), (2, false, 3.0::DOUBLE)) v(id, f, s)) "
        "DECIDE x(BOOL) SUCH THAT x >= f MINIMIZE SUM(s*x)"
    ),
    "scalar_subquery_bound": (
        "SELECT id, x FROM (SELECT * FROM (VALUES (1, 2.0::DOUBLE), (2, -3.0::DOUBLE)) v(id, s)) "
        "DECIDE x(REAL) SUCH THAT x <= (SELECT 5) AND x >= 0 MAXIMIZE SUM(s*x)"
    ),
    "ties_two_equal_integers": _problem(
        ROWS, decl="x(INT)", bounds="x BETWEEN 0 AND 9", obj="MINIMIZE SUM(POWER(x - 2.5, 2))"
    ),
    "flat_objective_lowest_value": _problem(ROWS, decl="x(INT)", bounds="x BETWEEN 2 AND 6", obj="MAXIMIZE SUM(0*x)"),
}


@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(BOUNDARY_CASES))
def test_a1_boundary_values_read_like_the_solver(decidb_cli, name):
    sql = BOUNDARY_CASES[name]
    direct = _run_all(decidb_cli, sql, "require")
    solver = _run_all(decidb_cli, sql, "off")
    assert (
        direct[0] != "error" or "decide_direct_solve=require" not in direct[1]
    ), "no rule applied to the query"
    assert _all_close(direct, solver), (direct, solver)


def _all_close(left, right):
    if left[0] != right[0]:
        return False
    if left[0] == "error":
        return left[1] == right[1]
    if len(left[1]) != len(right[1]):
        return False
    for a_row, b_row in zip(left[1], right[1]):
        for a, b in zip(a_row, b_row):
            if isinstance(a, float) and isinstance(b, float):
                if abs(a - b) > 1e-9 * max(1.0, abs(a)):
                    return False
            elif a != b:
                return False
    return True


# ---------------------------------------------------------------------------
# The exact reference: random per-row problems against exact arithmetic
# ---------------------------------------------------------------------------


def _exact_best(domain, a, b, lower, upper):
    """The exact optimum of `a*x^2 + b*x` on [lower, upper], with values as Fractions. Returns the smallest argmin."""
    f = lambda x: a * x * x + b * x
    candidates = {lower, upper}
    if a > 0:
        turning = -b / (2 * a)
        if domain == "REAL":
            candidates.add(min(max(turning, lower), upper))
        else:
            for x in (math.floor(turning) - 1, math.floor(turning), math.floor(turning) + 1, math.floor(turning) + 2):
                if lower <= x <= upper:
                    candidates.add(Fraction(x))
    return min(candidates, key=lambda x: (f(x), x))


@pytest.mark.correctness
@pytest.mark.parametrize("domain", ["REAL", "INT", "BOOL"])
@pytest.mark.parametrize("seed", [3, 4])
def test_a1_matches_exact_arithmetic_on_random_rows(decidb_cli, domain, seed):
    rng = random.Random(seed * 1000 + len(domain))
    rows = []
    for index in range(150):
        w = rng.choice([-2.0, -1.0, -0.5, 0.0, 0.5, 1.0, 2.0, 4.0])
        t = rng.randrange(-40, 80) / 4.0
        s = rng.randrange(-12, 12) / 4.0
        # Whole-number bounds for INT keep every row feasible; REAL rows take half steps.
        step = 1.0 if domain == "INT" else 0.5
        lo = rng.randrange(-8, 8) * step
        cap = max(lo, 0.0) + rng.randrange(0, 24) * step
        rows.append(_row(index, t, cap, lo, w, True, s))
    # Per row: minimize w^2 x^2 - 2 w t x + s x over [max(lo, 0), cap], or the BOOL box.
    bounds = "x >= lo AND x <= cap" if domain != "BOOL" else "x <= 1"
    sql = _problem(rows, bounds=bounds, obj="MINIMIZE SUM(POWER(w*x - t, 2) + s*x)", decl=f"x({domain})")
    got, _ = _run(decidb_cli, sql, "require")
    by_id = {row["id"]: row for row in rows}
    for id_, x in got:
        row = by_id[id_]
        a = Fraction(row["w"]) ** 2
        b = -2 * Fraction(row["w"]) * Fraction(row["t"]) + Fraction(row["s"])
        lower, upper = Fraction(max(row["lo"], 0.0)), Fraction(row["cap"])
        if domain == "BOOL":
            lower, upper = Fraction(0), Fraction(1)
        elif domain == "INT":
            lower, upper = Fraction(math.ceil(lower)), Fraction(math.floor(upper))
        if lower > upper:
            pytest.fail(f"row {id_} is infeasible but the query answered")
        best = _exact_best("REAL" if domain == "REAL" else "INT", a, b, lower, upper)
        value = a * Fraction(x) ** 2 + b * Fraction(x)
        optimum = a * best * best + b * best
        assert lower <= Fraction(x) <= upper, (id_, x, lower, upper)
        assert abs(value - optimum) <= Fraction(1, 10**9), (id_, x, float(best), float(value), float(optimum))


# ---------------------------------------------------------------------------
# A problem made only of constants is pipelined; any other problem reads every row before releasing one
# ---------------------------------------------------------------------------

CONSTANT_PROBLEMS = {
    "linear_max": "x BETWEEN 0 AND 10 MAXIMIZE SUM(3*x)",
    "square": "x BETWEEN -2 AND 10 MINIMIZE SUM(POWER(x - 4, 2) + 2)",
    "no_objective": "x BETWEEN 2 AND 5",
    "scaled_square_int": "x BETWEEN 0 AND 9 MINIMIZE 2*SUM(POWER(2*x - 7, 2))/3",
}


def _constant_query(decision, problem, parent="SELECT id, x FROM ({}) q ORDER BY id"):
    inner = f"SELECT * FROM (VALUES (1), (2), (3)) t(id) DECIDE {decision} SUCH THAT {problem}"
    return parent.format(inner)


@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(CONSTANT_PROBLEMS))
@pytest.mark.parametrize("decision", ["x(REAL)", "x(INT)"])
def test_a1_a_constant_problem_is_pipelined_and_matches_the_solver(decidb_cli, name, decision):
    sql = _constant_query(decision, CONSTANT_PROBLEMS[name])
    assert _run(decidb_cli, sql, "require") == _run(decidb_cli, sql, "off")
    plan = decidb_cli.execute_raw(f"SET decide_direct_solve='require'; EXPLAIN {sql}").stdout
    assert "A1_SEPARABLE_BOX" in plan and "WINDOW" not in plan, plan


@pytest.mark.correctness
def test_a1_a_problem_that_reads_a_column_waits_for_every_row(decidb_cli):
    sql = _constant_query("x(REAL)", "x BETWEEN 0 AND id MAXIMIZE SUM(3*x)")
    plan = decidb_cli.execute_raw(f"SET decide_direct_solve='require'; EXPLAIN {sql}").stdout
    assert "A1_SEPARABLE_BOX" in plan and "WINDOW" in plan, plan
    conditional = _constant_query("x(REAL)", "x BETWEEN 0 AND 10 WHEN (id > 1) MAXIMIZE SUM(3*x)")
    plan = decidb_cli.execute_raw(f"SET decide_direct_solve='require'; EXPLAIN {conditional}").stdout
    assert "WINDOW" in plan, plan


@pytest.mark.error
@pytest.mark.correctness
@pytest.mark.parametrize(
    "problem,message",
    [
        ("x >= 5 AND x <= 3 MINIMIZE SUM(x)", "infeasible"),
        ("x >= 0 MAXIMIZE SUM(x)", "unbounded"),
        ("x <= 1e17 AND x >= 0 MAXIMIZE SUM(x)", "beyond the range"),
    ],
)
def test_a1_a_constant_problem_raises_however_little_of_the_result_is_read(decidb_cli, problem, message):
    decision = "x(INT)" if "1e17" in problem else "x(REAL)"
    for parent in (
        "SELECT id FROM ({}) q LIMIT 1",
        "SELECT COUNT(*) FROM ({}) q",
        "SELECT id FROM ({}) q WHERE id = 2",
        "SELECT id, x FROM ({}) q ORDER BY id",
    ):
        with pytest.raises(DecidBCliError, match=message):
            _run(decidb_cli, _constant_query(decision, problem, parent), "require")
    assert _run(decidb_cli, _constant_query(decision, problem, "SELECT id FROM ({}) q LIMIT 0"), "require")[0] == []
    empty = "SELECT id, x FROM (SELECT 1 AS id WHERE false) DECIDE " + decision + " SUCH THAT " + problem
    assert _run(decidb_cli, empty, "require")[0] == []


@pytest.mark.correctness
def test_a1_nested_decisions_agree(decidb_cli):
    # The outer DECIDE reads the inner decision's answer as data, so the inner plan's types and bindings must be exact.
    sql = """
        SELECT id, y FROM (
            SELECT id, x, s, cap FROM (VALUES (1, 2.0, 5.0), (2, -3.0, 4.0), (3, 4.0, 6.0), (4, 0.5, 3.0)) t(id, s, cap)
            DECIDE x(INT) SUCH THAT x BETWEEN 0 AND cap MAXIMIZE SUM(s*x)
        ) DECIDE y(REAL) SUCH THAT y BETWEEN 0 AND x + 1 MAXIMIZE SUM(s*y) ORDER BY id
    """
    assert _run(decidb_cli, sql, "require") == _run(decidb_cli, sql, "off")
    plan = decidb_cli.execute_raw(f"SET decide_direct_solve='require'; EXPLAIN {sql}").stdout
    assert plan.count("A1_SEPARABLE_BOX") >= 2, plan


@pytest.mark.correctness
def test_a1_duplicate_rows_stay_separate_rows(decidb_cli):
    sql = (
        "SELECT id, x FROM (VALUES (1, 5.0), (2, 4.0), (2, 4.0), (3, 6.0)) t(id, cap) "
        "DECIDE x(REAL) SUCH THAT x BETWEEN 0 AND cap MAXIMIZE SUM(x) ORDER BY id"
    )
    rows, _ = _run(decidb_cli, sql, "require")
    assert rows == [(1, 5.0), (2, 4.0), (2, 4.0), (3, 6.0)]
    assert rows == _run(decidb_cli, sql, "off")[0]


# ---------------------------------------------------------------------------
# A lower bound written as "no bound" (1e30 or more in magnitude) is read as no bound
# ---------------------------------------------------------------------------
#
# Both paths fold unconditional bounds into a decision's box with one function (`DecideFoldSingletonRow`). A lower bound that
# was written is a bound even when it is -1e30 or below: it removes the default floor of 0, as -1e29 does, and leaves the
# decision unbounded below. Only a decision with no lower bound written at all keeps the floor.
LOWER_BOUND_AS_NO_BOUND_CASES = {
    f"{domain}_{_tag}_{objective.lower()}": (domain, constraint, objective, expected)
    for domain in ("REAL", "INT")
    for _tag, bound in (("1e30", "-1e30"), ("1e31", "-1e31"), ("1e29", "-1e29"))
    for objective, constraint, expected in (
        # MINIMIZE has nothing to stop it from going down: unbounded.
        ("MINIMIZE", f"x >= {bound} AND x <= 5", ("error", "unbounded")),
        # A second, tighter lower bound decides the answer, as it does without the -1e30.
        ("MINIMIZE", f"x >= {bound} AND x >= 1 AND x <= 5", ("ok", [(1, 1), (2, 1)])),
    )
}
LOWER_BOUND_AS_NO_BOUND_CASES.update(
    {
        # No lower bound written: the floor of 0 holds.
        "REAL_none_written": ("REAL", "x <= 5", "MINIMIZE", ("ok", [(1, 0.0), (2, 0.0)])),
        "INT_none_written": ("INT", "x <= 5", "MINIMIZE", ("ok", [(1, 0), (2, 0)])),
        # A written lower bound of -1e30 with an upper bound below 0 is a valid box, not a contradiction of the floor.
        "REAL_upper_below_zero": ("REAL", "x >= -1e31 AND x <= -1", "MAXIMIZE", ("ok", [(1, -1.0), (2, -1.0)])),
        "INT_upper_below_zero": ("INT", "x >= -1e30 AND x <= -1", "MAXIMIZE", ("ok", [(1, -1), (2, -1)])),
        "REAL_upper_below_zero_minimized": ("REAL", "x >= -1e31 AND x <= -1", "MINIMIZE", ("error", "unbounded")),
        # An ordinary written bound still replaces the floor.
        "REAL_written_below_zero": ("REAL", "x >= -3 AND x <= 5", "MINIMIZE", ("ok", [(1, -3.0), (2, -3.0)])),
    }
)


@pytest.mark.var_real
@pytest.mark.var_integer
@pytest.mark.cons_perrow
@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(LOWER_BOUND_AS_NO_BOUND_CASES))
def test_a1_written_lower_bound_of_no_bound_matches_the_solver(decidb_cli, name):
    domain, constraint, objective, expected = LOWER_BOUND_AS_NO_BOUND_CASES[name]
    sql = (
        f"SELECT id, x FROM (FROM (VALUES (1, 2.0), (2, 3.0)) t(id, s) "
        f"DECIDE x({domain}) SUCH THAT {constraint} {objective} SUM(s * x)) q ORDER BY id"
    )
    for mode in ("off", "require"):
        assert _run_all(decidb_cli, sql, mode) == expected, mode
