"""R1 (continuous knapsack), checked three ways: the independent oracle, the solver path, the direct path.

Every case is one DECIDE query over rows that share one budget: a single `REAL` decision `amount` on each row, per-row
bounds on it, a budget `L <= SUM(w*amount) <= U` (written `<=`, `>=`, `=`, `BETWEEN`, or several comparisons of the same
sum), and a linear objective. The oracle builds its own model from the raw rows, from the
box each case states in Python (never from DeciDB's reading of the SQL). DeciDB then runs the query with
`decide_direct_solve='off'` (the solver) and `'require'` (direct solve; a query the rule does not apply to fails instead
of quietly using the solver). The case passes when all three agree:

- the oracle finds the problem feasible: both DeciDB runs return an assignment inside every row's box, within the
  budget's limits, that reaches the oracle's objective (rows may differ on ties);
- the oracle finds it infeasible or unbounded: both DeciDB runs raise that error.

The solver has one behavior the oracle cannot derive from the SQL, so each case states it: a bare unconditional constant
lower bound (`amount >= 1`) replaces the variable's default lower bound of 0, while every other lower bound (a column, a
`when`) only intersects with it.

What a result comparison cannot see (which plan ran, EXPLAIN, prepared plans, DIAGNOSE, pruning) stays in
`test_direct_rule_conformance.py`.

Note: `oracle_solver` caches each result under the test node id plus a hash of the test function's source. The case
tables live outside that source, so editing a case does not invalidate the cache; a stale value fails loudly with an
objective mismatch. After changing a case or the model builder, remove its entries from `results/oracle_cache.json`.
"""

import json
import math
import re
from dataclasses import dataclass

import pytest

from decidb_cli import DecidBCliError
from solver.types import ObjSense, VarType

from ._direct_differential import error_class
from ._direct_oracle_common import _allow_direct_path  # noqa: F401  (autouse fixture)
from ._direct_oracle_common import BY_KEY_PENDING, READ_THROUGH_PENDING, agree_with_oracle, direct_path
from ._direct_oracle_common import outcome as _run_all, run as _run, sql_literal, values_clause


INF = math.inf
BIG = 1e30  # the oracle reads a bound this large as none

# ---------------------------------------------------------------------------
# Data. Every value is a dyadic rational, so sums and quotients of them are exact in a double.
# ---------------------------------------------------------------------------

COLUMNS = (
    ("id", "INTEGER"),
    ("cap", "DOUBLE"),  # a per-row upper bound
    ("lo", "DOUBLE"),  # a per-row lower bound
    ("w", "DOUBLE"),  # the weight of the row in the budget, positive, negative or zero
    ("v", "DOUBLE"),  # the value of the row in the objective
)
NAMES = [name for name, _ in COLUMNS]


def _row(id, cap, lo, w, v):
    return dict(id=id, cap=cap, lo=lo, w=w, v=v)


# The worked example of definition.md: the benefit per unit of budget is 5, 3 and 2.
WORKED = (_row(1, 4.0, 0.0, 2.0, 10.0), _row(2, 3.0, 0.0, 3.0, 9.0), _row(3, 5.0, 0.0, 2.0, 4.0))

# Every kind of row. Row 2 earns and frees budget; row 3 has no weight; row 4 has a positive lower bound; row 5 loses
# value and uses budget (its price ties with row 2's); row 6 loses value and frees budget.
MIXED = (
    _row(1, 4.0, 0.0, 2.0, 10.0),
    _row(2, 3.0, 0.0, -1.0, 2.0),
    _row(3, 5.0, 0.0, 0.0, 3.0),
    _row(4, 6.0, 1.0, 3.0, 5.0),
    _row(5, 2.0, 0.0, 0.5, -1.0),
    _row(6, 4.0, 0.0, -2.0, -3.0),
)

# Every row has a positive lower bound and a positive weight: the lowest spend is 2*1 + 3*2 + 2*0.5 = 9.
RAISED = (_row(1, 4.0, 1.0, 2.0, 10.0), _row(2, 3.0, 2.0, 3.0, 9.0), _row(3, 5.0, 0.5, 2.0, 4.0))

# Rows with the same price v/w = 1, in different sizes.
EQUAL_PRICES = (
    _row(1, 3.0, 0.0, 1.0, 1.0),
    _row(2, 4.0, 0.0, 2.0, 2.0),
    _row(3, 2.0, 0.0, 0.5, 0.5),
    _row(4, 5.0, 0.0, 1.0, 2.0),
)

# Every row loses value as it grows: a cover buys the least expensive units first.
NEGATIVE_VALUES = (_row(1, 4.0, 0.0, 2.0, -10.0), _row(2, 3.0, 0.0, 3.0, -9.0), _row(3, 5.0, 0.0, 2.0, -4.0))

# For the box `open_floor`: row 1 has no floor and a cap; row 2 has neither a floor nor a cap. Row 1 earns more per
# unit of budget than row 2.
OPEN_PAIR_GAINS = (_row(1, 4.0, 0.0, 1.0, 5.0), _row(2, 1e30, 0.0, 1.0, 2.0))
# The same shape with prices -3 and -1: lowering row 1 and raising row 2 at equal spend gains.
OPEN_PAIR_LOSSES = (_row(1, 4.0, 0.0, 1.0, -3.0), _row(2, 1e30, 0.0, 1.0, -1.0))
# One row with no floor that loses value as it grows.
FLOORLESS_LOSER = (_row(1, 4.0, 0.0, 1.0, -1.0),)
# Rows with equal prices -1 and a negative floor.
EQUAL_PRICE_LOSSES = (_row(1, 3.0, 0.0, 1.0, -1.0), _row(2, 4.0, 0.0, 2.0, -2.0), _row(3, 2.0, 0.0, 0.5, -0.5))

LONE = (_row(1, 9.0, 0.0, 2.0, 3.0),)
LONE_COST = (_row(1, 9.0, 0.0, 2.0, -3.0),)  # for MINIMIZE: the same problem as LONE

# A row that frees budget (weight -1) and a row that spends it. Spending the freed budget is the gain.
PAIR = (_row(1, 4.0, 0.0, -1.0, 1.0), _row(2, 6.0, 0.0, 2.0, 3.0))
PAIR_COST = (_row(1, 4.0, 0.0, -1.0, -1.0), _row(2, 6.0, 0.0, 2.0, -3.0))  # for MINIMIZE

# Three equal rows, equal price, no bound but the budget: any split of the budget is optimal.
TRIPLE = (_row(1, 0.0, 0.0, 1.0, 1.0), _row(2, 0.0, 0.0, 1.0, 1.0), _row(3, 0.0, 0.0, 1.0, 1.0))
TRIPLE_SIZES = (_row(1, 4.0, 0.0, 1.0, 1.0), _row(2, 3.0, 0.0, 2.0, 2.0), _row(3, 8.0, 0.0, 0.5, 0.5))

EMPTY_BOX = (_row(1, 4.0, 0.0, 2.0, 10.0), _row(2, 1.0, 3.0, 3.0, 9.0))  # row 2: lo 3 > cap 1
UNBOUNDED_AND_EMPTY = (_row(1, 4.0, 0.0, -2.0, 10.0), _row(2, 1.0, 3.0, 3.0, 9.0))


def _values(rows):
    return values_clause(rows, NAMES, dict(COLUMNS), sql_literal)


# ---------------------------------------------------------------------------
# Boxes, objectives and cases
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class Box:
    #: The bounds as written.
    sql: str
    #: row -> (lower, upper): the box derived here in Python and not read from DeciDB.
    box: object


def _floor(row):
    return max(row["lo"], 0.0)


BOXES = {
    "between_constants_and_cap": Box("amount BETWEEN 0 AND cap", lambda r: (0.0, r["cap"])),
    "row_lower_and_cap": Box("amount >= lo AND amount <= cap", lambda r: (_floor(r), r["cap"])),
    "cap_only": Box("amount <= cap", lambda r: (0.0, r["cap"])),
    "constant_pair": Box("amount >= 0 AND amount <= 3", lambda r: (0.0, 3.0)),
    # A bare constant lower bound replaces the default floor of 0; the caps are all at least 2.
    "constant_lower": Box("amount >= 1 AND amount <= cap", lambda r: (1.0, r["cap"])),
    "negative_constant_lower": Box("amount >= -2 AND amount <= cap", lambda r: (-2.0, r["cap"])),
    "scaled_cap": Box("amount >= 0 AND 2*amount <= cap", lambda r: (0.0, r["cap"] / 2.0)),
    "cap_under_when": Box(
        "amount >= 0 AND amount <= 4 AND when id > 2: amount <= cap",
        lambda r: (0.0, min(4.0, r["cap"]) if r["id"] > 2 else 4.0),
    ),
    "equality_pin": Box(
        "when id = 4: amount = lo AND amount >= 0 AND amount <= cap",
        lambda r: ((r["lo"], r["lo"]) if r["id"] == 4 else (0.0, r["cap"])),
    ),
    "open_floor": Box("amount >= -1e30 AND amount <= cap", lambda r: (-INF, r["cap"])),
    "uncapped": Box("amount >= 0", lambda r: (0.0, INF)),
    "open_both": Box("amount >= -1e30", lambda r: (-INF, INF)),
}


@dataclass(frozen=True)
class Objective:
    sql: str
    #: True, False, or None for no objective (any assignment is as good as any other).
    maximize: object
    #: row -> the row's value in the objective, before the sense is applied.
    value: object


OBJECTIVES = {
    "max": Objective("MAXIMIZE SUM(v*amount)", True, lambda r: r["v"]),
    "min": Objective("MINIMIZE SUM(v*amount)", False, lambda r: r["v"]),
    "none": Objective("", None, lambda r: 0.0),
    "scaled": Objective("MAXIMIZE 4*SUM(v*amount)/2", True, lambda r: 2.0 * r["v"]),
    "two_parts": Objective("MAXIMIZE SUM(v*amount) - SUM(w*amount)", True, lambda r: r["v"] - r["w"]),
    "min_scaled": Objective("MINIMIZE 2*SUM(v*amount)", False, lambda r: 2.0 * r["v"]),
}

BUDGETS = {
    "plain": ("SUM(w*amount)", lambda r: r["w"]),
    "like_terms": ("SUM(w*amount + 2*amount)", lambda r: r["w"] + 2.0),
    "constant_weight": ("SUM(2*amount)", lambda r: 2.0),
    "unit": ("SUM(amount)", lambda r: 1.0),
}


@dataclass(frozen=True)
class Case:
    rows: tuple
    box: Box
    limit: float
    objective: Objective
    budget: str = "plain"
    #: The budget's right side as written, when it is not the number itself.
    limit_sql: str = None
    #: The whole budget constraint as written, with `{sum}` for the sum, when it is not `{sum} <= limit`; and then the
    #: (lower, upper) limits the oracle holds, stated here and not read from the SQL.
    written: str = None
    limits: tuple = None


def case(rows, box, limit, objective, budget="plain", limit_sql=None, written=None, limits=None):
    return Case(tuple(rows), BOXES[box], limit, OBJECTIVES[objective], budget, limit_sql, written, limits)


def add_directed(name, rows, box, written, limits, objective, budget="plain"):
    """A case whose budget is not `<=`: `written` is the constraint and `limits` the (lower, upper) the oracle holds."""
    R1_CASES[name] = case(rows, box, limits[1], objective, budget, written=written, limits=limits)


def _weight(case_, row):
    # A NULL makes the whole summand NULL, and SUM skips it, as in SQL.
    sql, weight = BUDGETS[case_.budget]
    return 0.0 if re.search(r"\bw\b", sql) and row["w"] is None else weight(row)


def _value(case_, row):
    return 0.0 if row["v"] is None else case_.objective.value(row)


def _box(case_, row):
    """The row's box. A NULL bound skips its instance, which admits every value on that side."""
    lower, upper = case_.box.box(row)
    return (-INF if lower is None else lower, INF if upper is None else upper)


# ---------------------------------------------------------------------------
# Running a case
# ---------------------------------------------------------------------------


def _limits(case_):
    """The (lower, upper) limits of the budget as the oracle holds them."""
    return case_.limits if case_.limits is not None else (-INF, case_.limit)


def _decide(case_):
    total = BUDGETS[case_.budget][0]
    if case_.written is not None:
        budget = case_.written.format(sum=total)
    else:
        limit = case_.limit_sql if case_.limit_sql is not None else repr(case_.limit)
        budget = f"{total} <= {limit}"
    return f"DECIDE amount(REAL) SUCH THAT {case_.box.sql} AND {budget} {case_.objective.sql}"


def _sql(case_, wrap="plain"):
    decide = _decide(case_)
    cols = ", ".join(NAMES)
    source = f"(VALUES {_values(case_.rows)}) t({cols})"
    select = "SELECT id, amount"
    without_v = [n for n in NAMES if n != "v"]
    base = f"(VALUES {values_clause(case_.rows, without_v, dict(COLUMNS), sql_literal)}) t({', '.join(without_v)})"
    values = ", ".join("(" + str(r["id"]) + ", " + sql_literal(r["v"], "DOUBLE") + ")" for r in case_.rows)
    scores = f"(VALUES {values}) z(id, v)"
    if wrap == "plain":
        return f"{select} FROM (FROM {source} {decide}) q ORDER BY id"
    if wrap == "joined_source":
        picked = ", ".join("z.v" if n == "v" else f"t.{n}" for n in NAMES)
        joined = f"SELECT {picked} FROM {base} JOIN {scores} ON z.id = t.id"
        return f"{select} FROM (FROM ({joined}) j {decide}) q ORDER BY id"
    raise ValueError(wrap)


def _solve_with_oracle(oracle_solver, case_):
    oracle_solver.create_model("direct_r1_continuous_knapsack")
    weights, linear = {}, {}
    for index, row in enumerate(case_.rows):
        lower, upper = _box(case_, row)
        name = f"amount_{index}"
        oracle_solver.add_variable(name, VarType.CONTINUOUS, max(lower, -BIG), min(upper, BIG))
        weights[name] = _weight(case_, row)
        linear[name] = _value(case_, row)
    lower, upper = _limits(case_)
    if lower == upper:
        oracle_solver.add_constraint(weights, "=", lower)
    else:
        if upper < INF:
            oracle_solver.add_constraint(weights, "<=", upper)
        if lower > -INF:
            oracle_solver.add_constraint(weights, ">=", lower)
    sense = ObjSense.MINIMIZE if case_.objective.maximize is False else ObjSense.MAXIMIZE
    oracle_solver.set_objective(linear, sense)
    return oracle_solver.solve()


def _check_assignment(case_, rows, columns, oracle, mode):
    assert columns == ["id", "amount"], mode
    assert [row[0] for row in rows] == [row["id"] for row in case_.rows], mode
    spend = total = 0.0
    for result, source in zip(rows, case_.rows):
        value = result[1]
        assert isinstance(value, float), (mode, source["id"], value)
        lower, upper = _box(case_, source)
        assert lower - 1e-9 * max(1.0, abs(lower)) <= value <= upper + 1e-9 * max(1.0, abs(upper)), (
            mode,
            source["id"],
            value,
            (lower, upper),
        )
        spend += _weight(case_, source) * value
        total += _value(case_, source) * value
    lower, upper = _limits(case_)
    assert spend <= upper + 1e-6 * max(1.0, abs(upper)), (mode, spend, upper)
    assert spend >= lower - 1e-6 * max(1.0, abs(lower)), (mode, spend, lower)
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
# The case table
# ---------------------------------------------------------------------------

R1_CASES = {}


def add(name, *args, **kwargs):
    R1_CASES[name] = case(*args, **kwargs)


# The worked example: the budget is used up and one row is partly filled.
add("worked_example", WORKED, "between_constants_and_cap", 14.0, "max")
# A NULL value is dropped where it is read (B1). A NULL weight leaves its row out of the budget, which then fills row 2
# for free; a NULL value leaves it out of the objective.
NULL_WEIGHT = tuple(dict(row, w=None) if row["id"] == 2 else row for row in WORKED)
add("null_weight_leaves_the_row_out_of_the_budget", NULL_WEIGHT, "between_constants_and_cap", 14.0, "max")
add(
    "null_value_leaves_the_row_out_of_the_objective",
    tuple(dict(row, v=None) if row["id"] == 1 else row for row in WORKED),
    "between_constants_and_cap",
    14.0,
    "max",
)
# `w*amount + 2*amount` is NULL as a whole when `w` is: row 2 weighs 0, not 2.
add("null_weight_drops_the_whole_summand", NULL_WEIGHT, "between_constants_and_cap", 14.0, "max", budget="like_terms")
# A NULL cap skips that row's bound: row 3 is held by the budget alone.
add(
    "null_cap_skips_the_row_bound",
    tuple(dict(row, cap=None) if row["id"] == 3 else row for row in WORKED),
    "cap_only",
    14.0,
    "max",
)
add("worked_example_min", WORKED, "between_constants_and_cap", 14.0, "min")

# Weights of every sign, positive lower bounds, ties between rows, under every objective.
for _objective in OBJECTIVES:
    add(f"mixed_binding_{_objective}", MIXED, "row_lower_and_cap", 8.0, _objective)
add("mixed_not_binding_max", MIXED, "row_lower_and_cap", 1000.0, "max")
add("mixed_not_binding_min", MIXED, "row_lower_and_cap", 1000.0, "min")
add("mixed_negative_budget_max", MIXED, "row_lower_and_cap", -5.0, "max")
add("mixed_negative_budget_min", MIXED, "row_lower_and_cap", -5.0, "min")
# The budget equals the lowest spend the boxes allow: the only assignment is every row at its cheapest end.
add("mixed_budget_equals_lowest_spend", MIXED, "row_lower_and_cap", -8.0, "max")
add("mixed_zero_budget_none", MIXED, "row_lower_and_cap", 0.0, "none")

# The budget's own forms.
add("budget_like_terms", MIXED, "row_lower_and_cap", 8.0, "max", budget="like_terms")
add("budget_constant_weight", MIXED, "row_lower_and_cap", 8.0, "max", budget="constant_weight")
add("budget_expression_limit", WORKED, "between_constants_and_cap", 14.0, "max", limit_sql="7*2")
add("budget_decimal_limit", WORKED, "between_constants_and_cap", 14.5, "max", limit_sql="14.5::DECIMAL(5,1)")

# Every spelling of a bound.
for _name in (
    "constant_pair",
    "cap_only",
    "constant_lower",
    "negative_constant_lower",
    "scaled_cap",
    "cap_under_when",
    "equality_pin",
):
    add(f"box_{_name}_max", MIXED, _name, 6.0, "max")
add("box_equality_pin_min", MIXED, "equality_pin", 6.0, "min")
add("box_negative_constant_lower_min", WORKED, "negative_constant_lower", 5.0, "min")

# A positive lower bound is the low end of a positive-weight row: the budget starts spent.
add("raised_floor_binding", RAISED, "row_lower_and_cap", 14.0, "max")
add("raised_floor_not_binding", RAISED, "row_lower_and_cap", 100.0, "max")
add("raised_floor_budget_equals_lowest_spend", RAISED, "row_lower_and_cap", 9.0, "max")
add("raised_floor_infeasible_budget", RAISED, "row_lower_and_cap", 8.5, "max")

# A row with no bound but the budget, alone and in company.
add("lone_free_row_max", LONE, "uncapped", 10.0, "max")
add("lone_free_row_min", LONE_COST, "uncapped", 10.0, "min")
add("lone_free_row_infeasible_budget", LONE, "uncapped", -1.0, "max")
add("lone_free_row_none", LONE, "uncapped", 10.0, "none")
add("lone_free_floorless_row_max", LONE, "open_floor", 10.0, "max")
add("worked_floorless_rows", WORKED, "open_floor", 14.0, "max")

# A row that frees budget and a row that spends it: the pair gains what neither could alone.
add("improving_pair_max", PAIR, "cap_only", 2.0, "max")
add("improving_pair_min", PAIR_COST, "cap_only", 2.0, "min")
add("improving_pair_tight_budget", PAIR, "cap_only", -4.0, "max")

# Rows with the same price: any split of the budget is optimal, and open rooms among them share the remainder.
add("equal_prices_binding", EQUAL_PRICES, "cap_only", 6.0, "max")
add("equal_prices_not_binding", EQUAL_PRICES, "cap_only", 100.0, "max")
add("equal_prices_all_open", TRIPLE, "open_both", 5.0, "max")
add("equal_prices_all_open_sizes", TRIPLE_SIZES, "open_both", 6.0, "max")
add("equal_prices_all_open_none", TRIPLE, "open_both", 5.0, "none")
add("equal_prices_open_floor", TRIPLE_SIZES, "open_floor", 6.0, "max")
# A budget below the anchors' spend, so the rows at the price move down from their anchors: open rooms share what the
# finite rooms leave, and finite rooms fill in turn.
add("equal_prices_all_open_negative_budget", TRIPLE, "open_both", -5.0, "max")
add("equal_prices_open_floor_negative_budget", TRIPLE_SIZES, "open_floor", -6.0, "max")
add("equal_prices_negative_floor_negative_budget", EQUAL_PRICES[:3], "negative_constant_lower", -3.0, "max")
add("equal_prices_negative_floor_partly_cheaper_row", EQUAL_PRICES, "negative_constant_lower", -3.0, "max")

# Infeasible: a row with an empty box; a budget below the lowest spend; infeasible before unbounded.
add("infeasible_empty_box", EMPTY_BOX, "row_lower_and_cap", 100.0, "max")
add("infeasible_empty_box_and_unbounded", UNBOUNDED_AND_EMPTY, "row_lower_and_cap", 100.0, "max")
add("infeasible_budget_below_lowest_spend", WORKED, "constant_lower", 1.0, "max")

# Unbounded: a row that gains without end, with or without the budget in the way.
add("unbounded_negative_weight_row", LONE_COST, "uncapped", 10.0, "max")
add("unbounded_zero_weight_row", (_row(1, 4.0, 0.0, 0.0, 1.0),), "uncapped", 10.0, "max")
add("unbounded_zero_weight_row_min", (_row(1, 4.0, 0.0, 0.0, -1.0),), "uncapped", 10.0, "min")
add("unbounded_zero_weight_floorless_row", (_row(1, 4.0, 0.0, 0.0, -1.0),), "open_floor", 10.0, "max")
add("unbounded_pair_of_open_rows", (_row(1, 0.0, 0.0, -1.0, 1.0), _row(2, 0.0, 0.0, 2.0, 3.0)), "uncapped", 5.0, "max")
add("unbounded_floorless_row_that_loses_value", (_row(1, 4.0, 0.0, 1.0, -1.0),), "open_floor", 10.0, "max")
add("unbounded_one_open_row_among_bounded", WORKED + (_row(4, 1e30, 0.0, -1.0, 1.0),), "cap_only", 14.0, "max")
# No objective cannot be unbounded, however open the rows are.
add("not_unbounded_without_an_objective", (_row(1, 0.0, 0.0, -1.0, 1.0),), "open_both", 10.0, "none")
# An open room up and an open room down at the same price: no gain from moving them together.
add(
    "not_unbounded_equal_open_prices",
    (_row(1, 0.0, 0.0, 1.0, 1.0), _row(2, 0.0, 0.0, 1.0, 1.0)),
    "open_both",
    3.0,
    "max",
)
# Floorless rows whose prices are positive: lowering a row costs value faster than it frees budget, so nothing gains
# without end.
add(
    "bounded_floorless_row_with_positive_price",
    (_row(1, 4.0, 0.0, 2.0, 3.0), _row(2, 3.0, 0.0, 1.0, 2.0)),
    "open_floor",
    10.0,
    "max",
)


# ---------------------------------------------------------------------------
# Budgets written `>=`, `=` and `BETWEEN`. `limits` is the (lower, upper) the oracle holds; the comment names the path
# through the construction that the case reaches and no other case does. "Z" is the zero-price spend, "over" a Z above
# the upper limit, "under" a Z below the lower one, "neither" a Z inside [lower, upper].
# ---------------------------------------------------------------------------

D = add_directed

# `>=`.
# neither, with a lower limit that Z clears.
D("minimum_not_binding_max", WORKED, "between_constants_and_cap", "{sum} >= 5", (5.0, INF), "max")
# neither at the edge: Z equals the lower limit, which is not under it.
D("minimum_equal_to_zero_price_spend_min", WORKED, "between_constants_and_cap", "{sum} >= 0", (0.0, INF), "min")
# neither, with a negative lower limit.
D("minimum_negative_not_binding_min", WORKED, "between_constants_and_cap", "{sum} >= -3", (-3.0, INF), "min")
# under, a partly filled row after one that is full: the items before the price are at their high ends.
D("minimum_binding_min", WORKED, "between_constants_and_cap", "{sum} >= 12", (12.0, INF), "min")
# under, where the limit lands exactly on the end of the first row: a tie of prices at the boundary.
D("minimum_binding_at_a_row_end_min", WORKED, "between_constants_and_cap", "{sum} >= 10", (10.0, INF), "min")
# under at a negative price under MAXIMIZE: every row loses value, and the cover buys the cheapest loss first.
D(
    "minimum_binding_max_at_a_negative_price",
    NEGATIVE_VALUES,
    "between_constants_and_cap",
    "{sum} >= 12",
    (12.0, INF),
    "max",
)
# the minimum-cost cover: MINIMIZE SUM(c*x) with SUM(x) >= d (every weight 1).
D("minimum_cover_min", WORKED, "between_constants_and_cap", "{sum} >= 6", (6.0, INF), "min", budget="unit")
D(
    "minimum_cover_exceeds_cheapest_rows_min",
    WORKED,
    "between_constants_and_cap",
    "{sum} >= 11",
    (11.0, INF),
    "min",
    budget="unit",
)
# under, rows with no objective: all prices tie at zero and the rows fill in turn.
D("minimum_binding_none", WORKED, "between_constants_and_cap", "{sum} >= 14", (14.0, INF), "none")
# under, with weights of every sign and a row with no weight.
# (Z is 23 under MAXIMIZE and -4 under MINIMIZE.)
D("minimum_binding_mixed_max", MIXED, "row_lower_and_cap", "{sum} >= 26", (26.0, INF), "max")
D("minimum_binding_mixed_min", MIXED, "row_lower_and_cap", "{sum} >= 20", (20.0, INF), "min")
for _objective in ("max", "min"):
    D(f"minimum_not_binding_mixed_{_objective}", MIXED, "row_lower_and_cap", "{sum} >= -8", (-8.0, INF), _objective)
# under, a pair where one row frees budget.
D("minimum_improving_pair_binding", PAIR, "cap_only", "{sum} >= 10", (10.0, INF), "max")
# under, positive lower bounds make the anchors' spend the starting point.
D("minimum_raised_floor_min", RAISED, "row_lower_and_cap", "{sum} >= 15", (15.0, INF), "min")
# under, equal prices tie and the rows at the price fill in turn (D above zero) and then spill to the next price.
D("minimum_equal_prices_tie_min", EQUAL_PRICES, "cap_only", "{sum} >= 6", (6.0, INF), "min")
D("minimum_equal_prices_spill_min", EQUAL_PRICES, "cap_only", "{sum} >= 14", (14.0, INF), "min")
# under, the tie is below the anchors' spend and moves down (D below zero): Z is -7 and the limit -3.
D("minimum_ties_move_down_max", EQUAL_PRICE_LOSSES, "negative_constant_lower", "{sum} >= -3", (-3.0, INF), "max")
# under, because a row with no floor and a negative price makes Z minus infinity (czd).
D("minimum_floorless_row_that_loses_value", FLOORLESS_LOSER, "open_floor", "{sum} >= 3", (3.0, INF), "max")
D(
    "minimum_floorless_row_that_loses_value_negative_limit",
    FLOORLESS_LOSER,
    "open_floor",
    "{sum} >= -3",
    (-3.0, INF),
    "max",
)
# under, rows with no floor and no cap tie at a negative price and share the limit evenly (up and down).
D("minimum_open_rows_share_positive_limit_min", TRIPLE, "open_both", "{sum} >= 5", (5.0, INF), "min")
D("minimum_open_rows_share_negative_limit_min", TRIPLE, "open_both", "{sum} >= -5", (-5.0, INF), "min")
# under with an open row after the full ones: the open row is the last to fill and takes what is left (cui before it
# is zero, and the next row after it is not reached).
D(
    "minimum_open_row_takes_the_rest_min",
    (_row(1, 3.0, 0.0, 1.0, 0.0), _row(2, 1e30, 0.0, 1.0, 1.0), _row(3, 5.0, 0.0, 1.0, 2.0)),
    "cap_only",
    "{sum} >= 10",
    (10.0, INF),
    "min",
)
# an open row meets the limit by itself, so no cap on the highest spend makes the problem infeasible.
D("minimum_met_by_an_open_row_min", LONE, "uncapped", "{sum} >= 100", (100.0, INF), "min")

# `=`.
# over: from above, exact fill pulls the rows back in order of value lost per unit of budget.
D("exact_fill_from_above_max", WORKED, "between_constants_and_cap", "{sum} = 14", (14.0, 14.0), "max")
D("exact_fill_from_above_min", NEGATIVE_VALUES, "between_constants_and_cap", "{sum} = 14", (14.0, 14.0), "min")
# under: from below, exact fill pushes the rows up.
D("exact_fill_from_below_min", WORKED, "between_constants_and_cap", "{sum} = 14", (14.0, 14.0), "min")
# under at a negative price: the fill continues past rows whose value is nonpositive.
D("exact_fill_at_a_negative_price_max", NEGATIVE_VALUES, "between_constants_and_cap", "{sum} = 14", (14.0, 14.0), "max")
D(
    "exact_fill_past_losing_rows_max",
    WORKED[:2] + (_row(3, 5.0, 0.0, 2.0, -4.0),),
    "between_constants_and_cap",
    "{sum} = 22",
    (22.0, 22.0),
    "max",
)
# neither: Z is the limit, so the price stays zero.
D("exact_fill_equals_highest_spend_max", WORKED, "between_constants_and_cap", "{sum} = 27", (27.0, 27.0), "max")
D("exact_fill_equals_zero_price_spend_min", WORKED, "between_constants_and_cap", "{sum} = 0", (0.0, 0.0), "min")
# rows with no objective fill in turn from below.
D("exact_fill_none", WORKED, "between_constants_and_cap", "{sum} = 14", (14.0, 14.0), "none")
D("exact_fill_mixed_max", MIXED, "row_lower_and_cap", "{sum} = 10", (10.0, 10.0), "max")
D("exact_fill_mixed_min", MIXED, "row_lower_and_cap", "{sum} = 10", (10.0, 10.0), "min")
D("exact_fill_mixed_negative_max", MIXED, "row_lower_and_cap", "{sum} = -5", (-5.0, -5.0), "max")
D("exact_fill_mixed_negative_min", MIXED, "row_lower_and_cap", "{sum} = -5", (-5.0, -5.0), "min")
D("exact_fill_improving_pair_from_above", PAIR, "cap_only", "{sum} = 2", (2.0, 2.0), "max")
D("exact_fill_improving_pair_from_below", PAIR, "cap_only", "{sum} = 10", (10.0, 10.0), "max")
D("exact_fill_raised_floor_at_lowest_spend", RAISED, "row_lower_and_cap", "{sum} = 9", (9.0, 9.0), "max")
D("exact_fill_raised_floor_between_lowest_and_highest", RAISED, "row_lower_and_cap", "{sum} = 20", (20.0, 20.0), "min")
D("exact_fill_equal_prices_max", EQUAL_PRICES, "cap_only", "{sum} = 6", (6.0, 6.0), "max")
D("exact_fill_equal_prices_min", EQUAL_PRICES, "cap_only", "{sum} = 6", (6.0, 6.0), "min")
# an open row that gains is bounded by an exact fill (it is unbounded under `>=`), and so is a floorless row that loses.
D("exact_fill_open_row_that_gains_max", LONE, "uncapped", "{sum} = 4", (4.0, 4.0), "max")
D("exact_fill_floorless_row_that_loses_value", FLOORLESS_LOSER, "open_floor", "{sum} = 3", (3.0, 3.0), "max")
D("exact_fill_open_rows_share_equally_max", TRIPLE, "open_both", "{sum} = 5", (5.0, 5.0), "max")
D("exact_fill_open_rows_share_equally_negative_max", TRIPLE, "open_both", "{sum} = -5", (-5.0, -5.0), "max")
# two rows with open ends at different prices: no clamp of the open prices to zero under an exact fill.
D("exact_fill_does_not_clamp_open_prices", OPEN_PAIR_GAINS, "open_floor", "{sum} = 10", (10.0, 10.0), "max")
# a looser limit beside `=` changes nothing; the same sum written twice is one budget.
D(
    "exact_fill_with_a_looser_upper_limit",
    WORKED,
    "between_constants_and_cap",
    "{sum} = 14 AND {sum} <= 20",
    (14.0, 14.0),
    "max",
)
D(
    "exact_fill_with_a_looser_lower_limit",
    WORKED,
    "between_constants_and_cap",
    "{sum} >= 3 AND {sum} = 14",
    (14.0, 14.0),
    "min",
)

# `BETWEEN`.
# neither.
D("between_not_binding_max", WORKED, "between_constants_and_cap", "{sum} BETWEEN 5 AND 30", (5.0, 30.0), "max")
# over: binds at the high side only.
D("between_binding_high_max", WORKED, "between_constants_and_cap", "{sum} BETWEEN 5 AND 14", (5.0, 14.0), "max")
D(
    "between_binding_high_min",
    NEGATIVE_VALUES,
    "between_constants_and_cap",
    "{sum} BETWEEN 5 AND 14",
    (5.0, 14.0),
    "min",
)
# under: binds at the low side only.
D("between_binding_low_min", WORKED, "between_constants_and_cap", "{sum} BETWEEN 12 AND 30", (12.0, 30.0), "min")
D(
    "between_binding_low_max",
    NEGATIVE_VALUES,
    "between_constants_and_cap",
    "{sum} BETWEEN 12 AND 30",
    (12.0, 30.0),
    "max",
)
# the same two comparisons written apart (in either order) are one budget.
D(
    "between_written_apart_binding_low",
    WORKED,
    "between_constants_and_cap",
    "{sum} >= 12 AND {sum} <= 30",
    (12.0, 30.0),
    "min",
)
D(
    "between_written_apart_upper_first_binding_high",
    WORKED,
    "between_constants_and_cap",
    "{sum} <= 14 AND {sum} >= 5",
    (5.0, 14.0),
    "max",
)
# an open row is bounded by a BETWEEN (it is unbounded under `>=`).
D("between_open_row_that_gains_max", LONE, "uncapped", "{sum} BETWEEN 4 AND 10", (4.0, 10.0), "max")
# a negative range, and a range of one point.
D("between_negative_range_mixed_max", MIXED, "row_lower_and_cap", "{sum} BETWEEN -6 AND -2", (-6.0, -2.0), "max")
D("between_one_point_min", WORKED, "between_constants_and_cap", "{sum} BETWEEN 14 AND 14", (14.0, 14.0), "min")
D("between_no_objective", WORKED, "between_constants_and_cap", "{sum} BETWEEN 3 AND 14", (3.0, 14.0), "none")

# Several comparisons of one sum: the smallest upper limit and the largest lower limit hold, whichever comes first.
D("upper_limits_smaller_first", WORKED, "between_constants_and_cap", "{sum} <= 14 AND {sum} <= 20", (-INF, 14.0), "max")
D("upper_limits_smaller_last", WORKED, "between_constants_and_cap", "{sum} <= 20 AND {sum} <= 14", (-INF, 14.0), "max")
D("lower_limits_larger_first", WORKED, "between_constants_and_cap", "{sum} >= 12 AND {sum} >= 5", (12.0, INF), "min")
D("lower_limits_larger_last", WORKED, "between_constants_and_cap", "{sum} >= 5 AND {sum} >= 12", (12.0, INF), "min")

# Infeasible: a required spend outside what the boxes can reach, and limits that cross.
for _objective in ("max", "min"):
    D(
        f"infeasible_minimum_above_highest_spend_{_objective}",
        WORKED,
        "between_constants_and_cap",
        "{sum} >= 30",
        (30.0, INF),
        _objective,
    )
    D(
        f"infeasible_exact_fill_above_highest_spend_{_objective}",
        WORKED,
        "between_constants_and_cap",
        "{sum} = 30",
        (30.0, 30.0),
        _objective,
    )
D("infeasible_exact_fill_below_lowest_spend", WORKED, "between_constants_and_cap", "{sum} = -1", (-1.0, -1.0), "max")
D(
    "infeasible_exact_fill_below_lowest_spend_raised_floor",
    RAISED,
    "row_lower_and_cap",
    "{sum} = 8.5",
    (8.5, 8.5),
    "max",
)
D(
    "infeasible_between_above_highest_spend",
    WORKED,
    "between_constants_and_cap",
    "{sum} BETWEEN 28 AND 40",
    (28.0, 40.0),
    "max",
)
D(
    "infeasible_between_below_lowest_spend",
    WORKED,
    "between_constants_and_cap",
    "{sum} BETWEEN -5 AND -1",
    (-5.0, -1.0),
    "max",
)
# the limits cross while the boxes reach both of them.
D("infeasible_between_limits_cross", WORKED, "between_constants_and_cap", "{sum} BETWEEN 10 AND 5", (10.0, 5.0), "max")
D(
    "infeasible_limits_cross_written_apart",
    WORKED,
    "between_constants_and_cap",
    "{sum} >= 10 AND {sum} <= 5",
    (10.0, 5.0),
    "min",
)
D("infeasible_two_exact_fills", WORKED, "between_constants_and_cap", "{sum} = 5 AND {sum} = 6", (6.0, 5.0), "max")
D(
    "infeasible_exact_fill_beside_a_larger_minimum",
    WORKED,
    "between_constants_and_cap",
    "{sum} = 14 AND {sum} >= 20",
    (20.0, 14.0),
    "max",
)
# infeasible before unbounded, under a minimum.
D(
    "infeasible_minimum_empty_box_and_unbounded",
    UNBOUNDED_AND_EMPTY,
    "row_lower_and_cap",
    "{sum} >= 0",
    (0.0, INF),
    "max",
)

# Unbounded under `>=`: an open row that gains moves up for ever (spend above the minimum is allowed) ...
D("unbounded_minimum_open_row_that_gains_max", LONE, "uncapped", "{sum} >= 4", (4.0, INF), "max")
D("unbounded_minimum_open_row_that_gains_min", LONE_COST, "uncapped", "{sum} >= 4", (4.0, INF), "min")
# ... even beside a floorless row with a higher price, because a down-open price counts as at most zero.
D("unbounded_minimum_clamps_open_down_prices_to_zero", OPEN_PAIR_GAINS, "open_floor", "{sum} >= 10", (10.0, INF), "max")
# An open row up and an open row down at prices that make the pair gain, whatever the direction.
for _name, _written, _pair in (
    ("minimum", "{sum} >= 3", (3.0, INF)),
    ("exact_fill", "{sum} = 3", (3.0, 3.0)),
    ("between", "{sum} BETWEEN 1 AND 3", (1.0, 3.0)),
):
    D(f"unbounded_pair_of_open_rows_under_{_name}", OPEN_PAIR_LOSSES, "open_floor", _written, _pair, "max")
# An item with no weight and an open end improves for ever whatever the budget says.
D(
    "unbounded_zero_weight_row_under_exact_fill",
    (_row(1, 4.0, 0.0, 0.0, 1.0),),
    "uncapped",
    "{sum} = 0",
    (0.0, 0.0),
    "max",
)
# Not unbounded without an objective.
D("minimum_open_row_without_objective_none", LONE, "uncapped", "{sum} >= 4", (4.0, INF), "none")


@pytest.mark.var_real
@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(R1_CASES))
def test_r1_oracle_solver_and_direct_agree(decidb_cli, oracle_solver, name):
    _agree(decidb_cli, oracle_solver, R1_CASES[name])


# The same problem inside a parent query: a join in the source, which the rule reads through. The other contexts (CTEs,
# a wide source, a correlated subquery, a join or a pair of filters above the DECIDE) reach no branch of the rule that
# this one and the other tests do not.
CONTEXT_CASES = [("improving_pair_max", "joined_source")]


@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize("base,wrap", CONTEXT_CASES)
def test_r1_agrees_inside_a_parent_context(decidb_cli, oracle_solver, base, wrap):
    _agree(decidb_cli, oracle_solver, R1_CASES[base], wrap=wrap)


@pytest.mark.correctness
def test_r1_empty_source_returns_no_rows_whatever_the_budget_says(decidb_cli):
    for limit in ("-1", "14"):
        for objective_sql in ("MAXIMIZE SUM(v*amount)", "MINIMIZE SUM(v*amount)", ""):
            sql = (
                "SELECT id, amount FROM (SELECT 1 AS id, 1.0 AS w, 1.0 AS v WHERE false) "
                f"DECIDE amount(REAL) SUCH THAT amount >= 0 AND SUM(w*amount) <= {limit} {objective_sql}"
            )
            for mode in ("off", "require"):
                assert _run(decidb_cli, sql, mode)[0] == []


@pytest.mark.correctness
@pytest.mark.parametrize(
    "budget",
    [
        "SUM(w*amount) >= 5",
        "SUM(w*amount) = 5",
        "SUM(w*amount) BETWEEN 2 AND 5",
        "SUM(w*amount) >= 5 AND SUM(w*amount) <= 3",
    ],
)
def test_r1_empty_source_returns_no_rows_under_a_minimum_an_exact_fill_or_a_range(decidb_cli, budget):
    # The solver answers an empty source with no rows whatever the required spend is (even one that nothing reaches, or
    # limits that cross), so the plan must too: its checks run on rows, and there are none.
    sql = (
        "SELECT id, amount FROM (SELECT 1 AS id, 1.0 AS w, 1.0 AS v WHERE false) "
        f"DECIDE amount(REAL) SUCH THAT amount >= 0 AND {budget} MINIMIZE SUM(v*amount)"
    )
    for mode in ("off", "require"):
        assert _run(decidb_cli, sql, mode)[0] == []


@pytest.mark.correctness
def test_r1_binder_rejects_strict_bounds_on_real_before_direct_solve_is_asked(decidb_cli):
    for mode in ("off", "require"):
        with pytest.raises(DecidBCliError, match="Strict inequality"):
            _run(
                decidb_cli,
                "SELECT amount FROM (SELECT 1 AS id, 1.0 AS w) DECIDE amount(REAL) "
                "SUCH THAT amount < 3 AND SUM(w*amount) <= 5 MAXIMIZE SUM(amount)",
                mode,
            )


# ---------------------------------------------------------------------------
# A budget per group and under a `when`
# ---------------------------------------------------------------------------
#
# A budget may be written with `by (g)` after the sum, with `when f:` in front of the clause, or with `when f` inside
# the sum (`SUM(when f: w*amount) <= 6`). A row whose `when` holds belongs to the budget of its `by` group (a NULL key
# is in no group; the cases also write `when g IS NOT NULL:`), and each group has its own budget and its own price. Any other row is outside every budget: it takes its own
# best end (the upper end for a positive value, the lower end for a negative one), as a row with no weight does. The
# oracle holds one constraint per group, over the rows of that group, built here in Python from the raw rows and the
# membership each case states, never from DeciDB's reading of the SQL. A group with no covered row has no constraint.

SCOPED_COLUMNS = (
    ("id", "INTEGER"),
    ("g", "VARCHAR"),  # a `by` key; NULL on some rows
    ("h", "VARCHAR"),  # a second `by` key
    ("f", "BOOLEAN"),  # the `when`
    ("cap", "DOUBLE"),
    ("lo", "DOUBLE"),
    ("w", "DOUBLE"),
    ("v", "DOUBLE"),
)
SCOPED_NAMES = [name for name, _ in SCOPED_COLUMNS]


def _srow(id, g, cap, w, v, f=True, lo=0.0, h="x"):
    return dict(id=id, g=g, h=h, f=f, cap=cap, lo=lo, w=w, v=v)


@dataclass(frozen=True)
class ScopedCase:
    rows: tuple
    #: The budget as written, everything the case adds to the bounds `amount >= lo AND amount <= cap`.
    budget: str
    #: The (lower, upper) limits of the budget of each group, as the oracle holds them.
    limits: tuple
    #: The `by` columns and the `when` the budget has, stated here for the oracle (a NULL key is outside every budget).
    per: tuple = ()
    when: object = None
    objective: str = "max"


SCOPED_CASES = {}


def scoped(name, rows, budget, limits, per=(), when=None, objective="max"):
    SCOPED_CASES[name] = ScopedCase(tuple(rows), budget, limits, per, when, objective)


def _scoped_box(row):
    """The row's box: its floor is at least the default 0 (a column lower bound only intersects with it)."""
    return max(row["lo"], 0.0), row["cap"]


def _group(case_, row):
    """The key of the budget the row belongs to, or None when the row is outside every budget."""
    if case_.when is not None and not case_.when(row):
        return None
    key = tuple(row[name] for name in case_.per)
    return None if any(part is None for part in key) else key


def _scoped_sql(rows, budget, objective_sql="MAXIMIZE SUM(v*amount)"):
    values = values_clause(rows, SCOPED_NAMES, dict(SCOPED_COLUMNS), sql_literal)
    return (
        f"SELECT id, amount FROM (FROM (VALUES {values}) t({', '.join(SCOPED_NAMES)}) "
        f"DECIDE amount(REAL) SUCH THAT amount >= lo AND amount <= cap AND {budget} {objective_sql}) q ORDER BY id"
    )


def _solve_scoped_with_oracle(oracle_solver, case_):
    oracle_solver.create_model("direct_r1_scoped_budget")
    members, linear = {}, {}
    objective = OBJECTIVES[case_.objective]
    for index, row in enumerate(case_.rows):
        lower, upper = _scoped_box(row)
        name = f"amount_{index}"
        oracle_solver.add_variable(name, VarType.CONTINUOUS, max(lower, -BIG), min(upper, BIG))
        linear[name] = objective.value(row)
        key = _group(case_, row)
        if key is not None:
            members.setdefault(key, {})[name] = row["w"]
    lower, upper = case_.limits
    for weights in members.values():
        if lower == upper:
            oracle_solver.add_constraint(weights, "=", lower)
        else:
            if upper < INF:
                oracle_solver.add_constraint(weights, "<=", upper)
            if lower > -INF:
                oracle_solver.add_constraint(weights, ">=", lower)
    sense = ObjSense.MINIMIZE if objective.maximize is False else ObjSense.MAXIMIZE
    oracle_solver.set_objective(linear, sense)
    return oracle_solver.solve()


def _check_scoped_assignment(case_, rows, columns, oracle, mode):
    assert columns == ["id", "amount"], mode
    assert [row[0] for row in rows] == [row["id"] for row in case_.rows], mode
    spend, total = {}, 0.0
    objective = OBJECTIVES[case_.objective]
    for result, source in zip(rows, case_.rows):
        value = result[1]
        assert isinstance(value, float), (mode, source["id"], value)
        lower, upper = _scoped_box(source)
        assert lower - 1e-9 * max(1.0, abs(lower)) <= value <= upper + 1e-9 * max(1.0, abs(upper)), (
            mode,
            source["id"],
            value,
            (lower, upper),
        )
        key = _group(case_, source)
        if key is not None:
            spend[key] = spend.get(key, 0.0) + source["w"] * value
        total += objective.value(source) * value
    lower, upper = case_.limits
    for key, used in spend.items():
        assert lower - 1e-6 * max(1.0, abs(lower)) <= used <= upper + 1e-6 * max(1.0, abs(upper)), (mode, key, used)
    expected = oracle.objective_value
    assert abs(total - expected) <= 1e-4 * max(1.0, abs(expected)), (mode, total, expected)


# Rows of the groups a, b and c, whose budgets bind at different prices. Under `<= 6` and MAXIMIZE, a fills row 1 to 3
# (price 5), b fills row 2 and 1 unit of row 1 (price 2), and c fills its capped row and 2 units of its uncapped row
# (price 1.5; the uncapped row is bounded here because the budget covers it).
THREE_GROUPS = (
    _srow(1, "a", 4.0, 2.0, 10.0),
    _srow(2, "a", 3.0, 3.0, 9.0),
    _srow(3, "a", 5.0, 2.0, 4.0),
    _srow(4, "b", 4.0, 1.0, 2.0),
    _srow(5, "b", 5.0, 1.0, 3.0),
    _srow(6, "b", 3.0, 2.0, 1.0),
    _srow(7, "c", 4.0, 1.0, 4.0),
    _srow(8, "c", 1e30, 1.0, 1.5),
)
# The three covered rows of a budget under a `when`, and rows outside it that would corrupt the budget's sums if they
# joined them: a positive floor (so a positive anchor) and a weight of either sign. Row 4 gains, row 5 loses, row 6 has
# no value and no cap.
COVERED = (_srow(1, "a", 4.0, 2.0, 10.0), _srow(2, "a", 3.0, 3.0, 9.0), _srow(3, "a", 5.0, 2.0, 4.0))
OUTSIDE = (
    _srow(4, "a", 4.0, 2.0, 3.0, f=False, lo=1.0),
    _srow(5, "a", 3.0, 3.0, -2.0, f=False, lo=1.0),
    _srow(6, "a", 1e30, -1.0, 0.0, f=False, lo=1.0),
)

# `by`: each group has its own price (the partitions of the windows).
scoped("per_three_groups_at_different_prices_max", THREE_GROUPS, "SUM(w*amount) by (g) <= 6", (-INF, 6.0), per=("g",))
# `when` on the clause: the covered rows alone share the budget; the rows outside it take their own best ends. The rows
# outside have a positive anchor and a weight, which must stay out of the covered rows' sums.
scoped("when_on_the_clause", COVERED + OUTSIDE, "when f: SUM(w*amount) <= 6", (-INF, 6.0), when=lambda r: r["f"])
# `when` inside the sum (the reducer's own filter): the same membership, read from the other place in the IR.
scoped("when_on_the_aggregate", COVERED + OUTSIDE, "SUM(when f: w*amount) <= 6", (-INF, 6.0), when=lambda r: r["f"])
# `when` in front of the clause and inside the sum: the sum covers the rows both keep. Row 2 is in the clause's `when`
# and out of the sum's; the rows outside the clause's `when` have a cap above 3 too.
scoped(
    "when_in_front_and_inside_the_sum",
    COVERED + OUTSIDE,
    "when f: SUM(when cap > 3: w*amount) <= 4",
    (-INF, 4.0),
    when=lambda r: r["f"] and r["cap"] > 3,
)
# A sum's own `when` of two conditions: the IR takes the NULL tests out of the filter and keeps both conditions.
scoped(
    "when_of_two_conditions_inside_the_sum",
    COVERED + OUTSIDE,
    "SUM(when f AND cap > 3: w*amount) <= 4",
    (-INF, 4.0),
    when=lambda r: r["f"] and r["cap"] > 3,
)
# One budget written as two clauses, one with each spelling of the `when`, is one budget.
scoped(
    "when_written_both_ways_on_two_clauses",
    COVERED + OUTSIDE,
    "when f: SUM(w*amount) <= 6 AND SUM(when f: w*amount) >= 1",
    (1.0, 6.0),
    when=lambda r: r["f"],
)
# `by` with `when`: a group has covered rows and rows outside, and the rows outside stay out of that group's sums.
scoped(
    "per_with_when",
    (
        _srow(1, "a", 4.0, 2.0, 10.0),
        _srow(2, "a", 3.0, 3.0, 9.0),
        _srow(3, "a", 4.0, 2.0, 3.0, f=False, lo=1.0),
        _srow(4, "b", 4.0, 1.0, 2.0),
        _srow(5, "b", 5.0, 1.0, 3.0),
        _srow(6, "b", 3.0, 2.0, 1.0),
        _srow(7, "b", 3.0, 3.0, -2.0, f=False, lo=1.0),
    ),
    "when f: SUM(w*amount) by (g) <= 6",
    (-INF, 6.0),
    per=("g",),
    when=lambda r: r["f"],
)
# Rows with a NULL key, left out by `when g IS NOT NULL`, are outside every budget: row 4 gains and goes to its cap, row 5 loses and goes to its floor,
# row 6 has no value and an uncapped box (it is not unbounded: its value is zero).
scoped(
    "null_key_rows_take_their_best_ends",
    (
        _srow(1, "a", 4.0, 2.0, 10.0),
        _srow(2, "a", 3.0, 3.0, 9.0),
        _srow(3, "a", 5.0, 2.0, 4.0),
        _srow(4, None, 4.0, 2.0, 3.0, lo=1.0),
        _srow(5, None, 3.0, 3.0, -2.0, lo=1.0),
        _srow(6, None, 1e30, -1.0, 0.0, lo=1.0),
    ),
    "when g IS NOT NULL: SUM(w*amount) by (g) <= 6",
    (-INF, 6.0),
    per=("g",),
)
# A group with no covered row has no budget, so a limit that no empty sum meets does not make the problem infeasible:
# `0 <= -1` is not a constraint (group b), and nor is `0 >= 3`.
scoped(
    "uncovered_group_under_a_negative_limit",
    (_srow(1, "a", 4.0, -2.0, 10.0), _srow(2, "a", 3.0, 3.0, 9.0), _srow(3, "b", 5.0, 2.0, 4.0, f=False)),
    "when f: SUM(w*amount) by (g) <= -1",
    (-INF, -1.0),
    per=("g",),
    when=lambda r: r["f"],
)
scoped(
    "uncovered_group_under_a_positive_minimum",
    (_srow(1, "a", 4.0, 2.0, 10.0), _srow(2, "a", 3.0, 3.0, 9.0), _srow(3, "b", 5.0, 2.0, 4.0, f=False)),
    "when f: SUM(w*amount) by (g) >= 3",
    (3.0, INF),
    per=("g",),
    when=lambda r: r["f"],
    objective="min",
)
# A row outside the budget that improves without end makes the problem unbounded, though the same row inside a budget
# (the uncapped row of group c above) is bounded.
scoped(
    "outside_row_uncapped_and_improving",
    (_srow(1, "a", 4.0, 2.0, 10.0), _srow(2, "a", 1e30, 1.0, 2.0, f=False)),
    "when f: SUM(w*amount) <= 6",
    (-INF, 6.0),
    when=lambda r: r["f"],
)
# A minimum, per group: each group buys the cheapest value per unit of weight, at its own price (2, 2.5 and 1.5).
scoped(
    "per_minimum_cover_min",
    (
        _srow(1, "a", 4.0, 2.0, 10.0),
        _srow(2, "a", 3.0, 3.0, 9.0),
        _srow(3, "a", 5.0, 2.0, 4.0),
        _srow(4, "b", 4.0, 1.0, 2.5),
        _srow(5, "b", 5.0, 1.0, 3.0),
        _srow(6, "b", 3.0, 2.0, 1.0),
        _srow(7, "c", 4.0, 1.0, 3.0),
        _srow(8, "c", 1e30, 1.0, 1.5),
    ),
    "SUM(w*amount) by (g) >= 8",
    (8.0, INF),
    per=("g",),
    objective="min",
)
# A range, per group: group a binds at the upper limit, group b at the lower one, group c at neither.
scoped(
    "per_between_binds_each_group_on_its_own_side",
    (
        _srow(1, "a", 4.0, 2.0, 10.0),
        _srow(2, "a", 3.0, 3.0, 9.0),
        _srow(3, "b", 4.0, 1.0, -2.0),
        _srow(4, "b", 5.0, 1.0, -1.0),
        _srow(5, "c", 2.0, 1.0, 1.0),
        _srow(6, "c", 3.0, 1.0, 2.0),
    ),
    "SUM(w*amount) by (g) BETWEEN 3 AND 6",
    (3.0, 6.0),
    per=("g",),
)
# Two `by` keys: the groups are the pairs, which differ from the groups of the first key alone.
scoped(
    "per_two_keys",
    (
        _srow(1, "a", 3.0, 1.0, 5.0, h="x"),
        _srow(2, "a", 3.0, 1.0, 2.0, h="x"),
        _srow(3, "a", 3.0, 1.0, 4.0, h="y"),
        _srow(4, "a", 3.0, 1.0, 1.0, h="y"),
        _srow(5, "b", 3.0, 1.0, 6.0, h="x"),
        _srow(6, "b", 3.0, 1.0, 1.0, h="x"),
    ),
    "SUM(w*amount) by (g, h) <= 4",
    (-INF, 4.0),
    per=("g", "h"),
)


@pytest.mark.var_real
@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(SCOPED_CASES))
def test_r1_scoped_budget_oracle_solver_and_direct_agree(decidb_cli, oracle_solver, name):
    case_ = SCOPED_CASES[name]
    oracle = _solve_scoped_with_oracle(oracle_solver, case_)
    sql = _scoped_sql(case_.rows, case_.budget, OBJECTIVES[case_.objective].sql)

    def check_assignment(rows, columns, mode):
        _check_scoped_assignment(case_, rows, columns, oracle, mode)

    agree_with_oracle(decidb_cli, sql, oracle, check_assignment)


@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize(
    "budget,objective",
    [
        # Two clauses of one budget must cover the same rows: `by` on one clause and not the other ...
        ("SUM(w*amount) by (g) >= 1 AND SUM(w*amount) <= 6", "MAXIMIZE SUM(v*amount)"),
        # ... other keys, or the same keys in another order or number ...
        ("SUM(w*amount) by (g) >= 1 AND SUM(w*amount) by (h) <= 6", "MAXIMIZE SUM(v*amount)"),
        ("SUM(w*amount) by (g, h) >= 1 AND SUM(w*amount) by (h, g) <= 6", "MAXIMIZE SUM(v*amount)"),
        ("SUM(w*amount) by (g, h) >= 1 AND SUM(w*amount) by (g) <= 6", "MAXIMIZE SUM(v*amount)"),
        # ... a `when` on one clause and not the other, or two different ones.
        ("when f: SUM(w*amount) >= 1 AND SUM(w*amount) <= 6", "MAXIMIZE SUM(v*amount)"),
        ("when f: SUM(w*amount) >= 1 AND when id > 4: SUM(w*amount) <= 6", "MAXIMIZE SUM(v*amount)"),
        ("when f: SUM(w*amount) >= 1 AND SUM(when cap > 3: w*amount) <= 6", "MAXIMIZE SUM(v*amount)"),
        # The objective's own `when` is not read.
        ("SUM(w*amount) by (g) <= 6", "MAXIMIZE when f: SUM(v*amount)"),
        ("when f: SUM(w*amount) <= 6", "MAXIMIZE when f: SUM(v*amount)"),
    ],
)
def test_r1_leaves_budgets_that_do_not_share_a_scope_to_the_solver(decidb_cli, budget, objective):
    sql = _scoped_sql(THREE_GROUPS, budget, objective)
    assert _run_all(decidb_cli, sql, "require") == ("error", "direct_not_applied")
    assert _run_all(decidb_cli, sql, "auto") == _run_all(decidb_cli, sql, "off")


# ---------------------------------------------------------------------------
# A budget read from the data
# ---------------------------------------------------------------------------
#
# The limit of the budget is a column, an expression over columns, or an aggregate of the source (`<= 0.5*SUM(cost)`).
# A limit that varies by row means one constraint `SUM(w*x) <op> b_i` for each row i of the group the limit is read
# over (the tuple-by-tuple meaning of the DECIDE definition). The oracle holds exactly those constraints, built here in
# Python from the raw rows, never DeciDB's reduction of them to the group's MIN (for `<=`) or MAX (for `>=`). The
# limit is read over the rows its clause covers in the group, except under a `when` written inside the sum, which
# scopes the sum and not the limit: then it is read over every row of the `by` group.

DATA_COLUMNS = (
    ("id", "INTEGER"),
    ("g", "VARCHAR"),  # the `by` key
    ("f", "BOOLEAN"),  # the `when`
    ("cap", "DOUBLE"),
    ("w", "DOUBLE"),
    ("v", "DOUBLE"),
    ("b", "DOUBLE"),  # a budget limit read from a column
    ("cost", "DOUBLE"),  # summed into an aggregate limit
    ("w2", "DOUBLE"),  # weight and value of the second decision of the cases with two
    ("v2", "DOUBLE"),
)
DATA_NAMES = [name for name, _ in DATA_COLUMNS]


def _drow(id, g, cap, w, v, b, f=True, cost=1.0, w2=1.0, v2=1.0):
    return dict(id=id, g=g, f=f, cap=cap, w=w, v=v, b=b, cost=cost, w2=w2, v2=v2)


# Group a is the worked example with limits 12, 9 and 14 (smallest 9, largest 14); group b has limits 5, 7 and 5. The
# costs add up to 12 in a, 8 in b and 20 in all.
DATA_ROWS = (
    _drow(1, "a", 4.0, 2.0, 10.0, 12.0, cost=4.0),
    _drow(2, "a", 3.0, 3.0, 9.0, 9.0, cost=2.0),
    _drow(3, "a", 5.0, 2.0, 4.0, 14.0, cost=6.0),
    _drow(4, "b", 4.0, 1.0, 2.0, 5.0, cost=2.0),
    _drow(5, "b", 5.0, 1.0, 3.0, 7.0, cost=4.0),
    _drow(6, "b", 3.0, 2.0, 1.0, 5.0, cost=2.0),
)
# The same rows with the limit the same on every row of a group (the only way an equality limit is read).
AGREEING_ROWS = tuple(dict(row, b=9.0 if row["g"] == "a" else 5.0) for row in DATA_ROWS)
# Rows 3 and 6 fail the `when` and carry the smallest limit of their group: a limit read over the covered rows ignores
# them, one read over the whole group does not.
WHEN_ROWS = tuple(
    dict(row, f=row["id"] not in (3, 6), b={3: 2.0, 6: 1.0}.get(row["id"], row["b"])) for row in DATA_ROWS
)
# The second decision of each row, for the cases with two decisions in one budget.
PIECE_ROWS = tuple(
    dict(row, w2=w2, v2=v2)
    for row, w2, v2 in zip(DATA_ROWS, (1.0, 2.0, 1.0, 2.0, 1.0, 1.0), (3.0, 4.0, 2.0, 6.0, 1.0, 5.0))
)
PIECE_WHEN_ROWS = tuple(dict(row, w2=piece["w2"], v2=piece["v2"]) for row, piece in zip(WHEN_ROWS, PIECE_ROWS))


def _limit(row, rows):
    return row["b"]


def _half_the_costs(row, rows):
    return 0.5 * sum(member["cost"] for member in rows)


@dataclass(frozen=True)
class DataBudgetCase:
    rows: tuple
    #: The budget as written, scope included.
    budget: str
    #: (comparison, limit(row, rows of the group the limit is read over)) for each clause, stated here for the oracle.
    clauses: tuple
    #: The `by` columns and the `when` of the budget, stated here for the oracle.
    per: tuple = ()
    when: object = None
    #: The `when` is written inside the sum (`SUM(when f: ...) <= b`).
    when_on_the_sum: bool = False
    objective: str = "max"
    #: Two decisions in the budget, `amount` and `extra`.
    pieces: bool = False


DATA_BUDGET_CASES = {}


def data_budget(name, rows, budget, clauses, **kwargs):
    DATA_BUDGET_CASES[name] = DataBudgetCase(tuple(rows), budget, tuple(clauses), **kwargs)


def _data_per_key(case_, row):
    key = tuple(row[name] for name in case_.per)
    return None if any(part is None for part in key) else key


def _data_group(case_, row):
    """The key of the budget the row belongs to, or None when the row is outside every budget."""
    if case_.when is not None and not case_.when(row):
        return None
    return _data_per_key(case_, row)


def _read_over(case_, key):
    """The rows a limit is read over in the budget `key`."""
    if case_.when_on_the_sum:
        return [row for row in case_.rows if _data_per_key(case_, row) == key]
    return [row for row in case_.rows if _data_group(case_, row) == key]


def _data_sql(case_):
    values = values_clause(case_.rows, DATA_NAMES, dict(DATA_COLUMNS), sql_literal)
    sense = "MAXIMIZE" if case_.objective == "max" else "MINIMIZE"
    if case_.pieces:
        declaration = "amount(REAL), extra(REAL)"
        box = "amount BETWEEN 0 AND cap AND extra BETWEEN 0 AND cap"
        objective, columns = f"{sense} SUM(v*amount + v2*extra)", "id, amount, extra"
    else:
        declaration, box = "amount(REAL)", "amount BETWEEN 0 AND cap"
        objective, columns = f"{sense} SUM(v*amount)", "id, amount"
    return (
        f"SELECT {columns} FROM (FROM (VALUES {values}) t({', '.join(DATA_NAMES)}) "
        f"DECIDE {declaration} SUCH THAT {box} AND {case_.budget} {objective}) q ORDER BY id"
    )


def _data_decisions(case_):
    return ("amount", "extra") if case_.pieces else ("amount",)


def _solve_data_budget_with_oracle(oracle_solver, case_):
    oracle_solver.create_model("direct_r1_data_budget")
    members, linear = {}, {}
    value_of = {"amount": "v", "extra": "v2"}
    weight_of = {"amount": "w", "extra": "w2"}
    for index, row in enumerate(case_.rows):
        key = _data_group(case_, row)
        for decision in _data_decisions(case_):
            name = f"{decision}_{index}"
            oracle_solver.add_variable(name, VarType.CONTINUOUS, 0.0, row["cap"])
            linear[name] = row[value_of[decision]]
            if key is not None:
                members.setdefault(key, {})[name] = row[weight_of[decision]]
    for key, weights in members.items():
        over = _read_over(case_, key)
        for comparison, limit in case_.clauses:
            for row in over:
                bound = limit(row, over)
                # A NULL limit skips its instance.
                if bound is not None:
                    oracle_solver.add_constraint(weights, comparison, bound)
    sense = ObjSense.MAXIMIZE if case_.objective == "max" else ObjSense.MINIMIZE
    oracle_solver.set_objective(linear, sense)
    return oracle_solver.solve()


def _check_data_budget_assignment(case_, rows, columns, oracle, mode):
    assert columns == (["id", "amount", "extra"] if case_.pieces else ["id", "amount"]), mode
    assert [row[0] for row in rows] == [row["id"] for row in case_.rows], mode
    spend, total = {}, 0.0
    for result, source in zip(rows, case_.rows):
        key = _data_group(case_, source)
        for position, (weight, value) in enumerate((("w", "v"), ("w2", "v2")), start=1):
            if position > 1 and not case_.pieces:
                break
            amount = result[position]
            assert isinstance(amount, float), (mode, source["id"], amount)
            assert -1e-9 <= amount <= source["cap"] + 1e-9 * max(1.0, source["cap"]), (mode, source["id"], amount)
            if key is not None:
                spend[key] = spend.get(key, 0.0) + source[weight] * amount
            total += source[value] * amount
    for key, used in spend.items():
        over = _read_over(case_, key)
        for comparison, limit in case_.clauses:
            for row in over:
                bound = limit(row, over)
                if bound is None:
                    continue
                slack = 1e-6 * max(1.0, abs(bound))
                assert comparison == ">=" or used <= bound + slack, (mode, key, used, bound)
                assert comparison == "<=" or used >= bound - slack, (mode, key, used, bound)
    expected = oracle.objective_value
    assert abs(total - expected) <= 1e-4 * max(1.0, abs(expected)), (mode, total, expected)


# A column: the group's smallest limit rules a `<=`, so the whole input shares the smallest of all six (5).
data_budget("column_global", DATA_ROWS, "SUM(w*amount) <= b", [("<=", _limit)])
# A lower limit read from the data over every row: the plan takes the MAX of the limit, on its own and beside a
# constant upper limit.
data_budget("column_lower_global", DATA_ROWS, "SUM(w*amount) >= b", [(">=", _limit)], objective="min")
data_budget(
    "column_lower_and_constant_upper_global",
    DATA_ROWS,
    "SUM(w*amount) >= b AND SUM(w*amount) <= 40",
    [(">=", _limit), ("<=", lambda r, rows: 40.0)],
)
# Per group, with limits that differ inside each group: a rules at 9 and b at 5, at different prices.
data_budget("column_per_group", DATA_ROWS, "SUM(w*amount) by (g) <= b", [("<=", _limit)], per=("g",))
# An expression over columns is read like a column, but its NULL and NaN errors cannot name one.
data_budget(
    "expression_per_group",
    DATA_ROWS,
    "SUM(w*amount) by (g) <= 2*b - 3",
    [("<=", lambda r, rows: 2 * r["b"] - 3)],
    per=("g",),
)
# A NULL limit skips its instance (B1): with both 5s NULL the smallest is 7, and with every limit NULL there is no
# budget at all.
data_budget(
    "null_limit_skips_its_instance",
    [dict(row, b=None) if row["id"] in (4, 6) else row for row in DATA_ROWS],
    "SUM(w*amount) <= b",
    [("<=", _limit)],
)
data_budget(
    "null_limit_on_every_row",
    [dict(row, b=None) for row in DATA_ROWS],
    "SUM(w*amount) <= 2*b - 3",
    [("<=", lambda r, rows: None if r["b"] is None else 2 * r["b"] - 3)],
)
# A lower limit takes the group's largest value (a: 14, b: 7); the minimum cover binds on it.
data_budget(
    "column_lower_per_group", DATA_ROWS, "SUM(w*amount) by (g) >= b", [(">=", _limit)], per=("g",), objective="min"
)
# An equality is read from a limit that agrees inside each group (a: 9, b: 5).
data_budget("column_equality_per_group", AGREEING_ROWS, "SUM(w*amount) by (g) = b", [("=", _limit)], per=("g",))
# An aggregate of the source is a window column holding the group's value: 10 over the whole input, 6 in a and 4 in b.
data_budget("aggregate_global", DATA_ROWS, "SUM(w*amount) <= 0.5*SUM(cost)", [("<=", _half_the_costs)])
data_budget(
    "aggregate_per_group",
    DATA_ROWS,
    "SUM(w*amount) by (g) <= 0.5*SUM(cost) by (g)",
    [("<=", _half_the_costs)],
    per=("g",),
)
# A `when` on the clause: the limit is read over the covered rows only, so the small limits of rows 3 and 6 are ignored.
# Both scopes: the whole input, and per group.
data_budget("clause_when_global", WHEN_ROWS, "when f: SUM(w*amount) <= b", [("<=", _limit)], when=lambda r: r["f"])
data_budget(
    "clause_when_per_group",
    WHEN_ROWS,
    "when f: SUM(w*amount) by (g) <= b",
    [("<=", _limit)],
    per=("g",),
    when=lambda r: r["f"],
)
# A `when` inside the sum: the same rows are in the budget, but the limit is read over the whole group, so the small limits
# of rows 3 and 6 rule (a: 2, b: 1).
data_budget(
    "sum_when_per_group",
    WHEN_ROWS,
    "SUM(when f: w*amount) by (g) <= b",
    [("<=", _limit)],
    per=("g",),
    when=lambda r: r["f"],
    when_on_the_sum=True,
)
# The same on a lower limit: it takes the largest value over the whole group (a: 12, b: 7), the rows left out included,
# and the minimum cover binds on it.
data_budget(
    "sum_when_lower_per_group",
    WHEN_ROWS,
    "SUM(when f: w*amount) by (g) >= b",
    [(">=", _limit)],
    per=("g",),
    when=lambda r: r["f"],
    when_on_the_sum=True,
    objective="min",
)
# Both limits from the data, one an expression: a range of at least cost - 2 (a: 4, b: 2) and at most b (a: 9, b: 5),
# minimized so it binds at the lower end.
data_budget(
    "range_of_an_expression_and_a_column",
    DATA_ROWS,
    "SUM(w*amount) by (g) >= cost - 2 AND SUM(w*amount) by (g) <= b",
    [(">=", lambda r, rows: r["cost"] - 2), ("<=", _limit)],
    per=("g",),
    objective="min",
)
# A constant beside a data limit, on each side. The constant 7 is the smaller upper limit of group a (9) and the larger
# of group b (5); the constant 5 is the smaller lower limit of group a (cost 6) and the larger of group b (cost 4).
data_budget(
    "constant_and_column_upper",
    DATA_ROWS,
    "SUM(w*amount) by (g) <= b AND SUM(w*amount) by (g) <= 7",
    [("<=", _limit), ("<=", lambda r, rows: 7.0)],
    per=("g",),
)
data_budget(
    "constant_and_column_lower",
    DATA_ROWS,
    "SUM(w*amount) by (g) >= 5 AND SUM(w*amount) by (g) >= cost",
    [(">=", lambda r, rows: 5.0), (">=", lambda r, rows: r["cost"])],
    per=("g",),
    objective="min",
)
# Two decisions in one budget: the limits are read on the rows, before the rows are unnested into items.
data_budget(
    "two_decisions_column_per_group",
    PIECE_ROWS,
    "SUM(w*amount + w2*extra) by (g) <= b",
    [("<=", _limit)],
    per=("g",),
    pieces=True,
)
data_budget(
    "two_decisions_clause_when_per_group",
    PIECE_WHEN_ROWS,
    "when f: SUM(w*amount + w2*extra) by (g) <= b",
    [("<=", _limit)],
    per=("g",),
    when=lambda r: r["f"],
    pieces=True,
)


@pytest.mark.var_real
@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(DATA_BUDGET_CASES))
def test_r1_data_budget_oracle_solver_and_direct_agree(decidb_cli, oracle_solver, name):
    case_ = DATA_BUDGET_CASES[name]
    oracle = _solve_data_budget_with_oracle(oracle_solver, case_)

    def check_assignment(rows, columns, mode):
        _check_data_budget_assignment(case_, rows, columns, oracle, mode)

    sql = _data_sql(case_)
    agree_with_oracle(decidb_cli, sql, oracle, check_assignment)


# ---------------------------------------------------------------------------
# Errors: both paths must raise
# ---------------------------------------------------------------------------


def _late(decide_text, w_sql="1.0", v_sql="1.0", cap_sql="10.0", limit="3000", tail=""):
    """A 5,000-row problem whose last row is built by the given SQL. Rows 0 to 4998 are ordinary."""
    return (
        f"SELECT i, amount FROM (SELECT i, {w_sql} AS w, {v_sql} AS v, {cap_sql} AS cap FROM range(5000) r(i)) "
        f"DECIDE amount(REAL) SUCH THAT {decide_text.format(limit=limit)} {tail}"
    )


LATE_DECIDE = "amount >= 0 AND amount <= cap AND SUM(w*amount) <= {limit} MAXIMIZE SUM(v*amount)"
LAST = "CASE WHEN i = 4999 THEN {} ELSE {} END"


def _last_row(late, ordinary):
    return LAST.format(late, ordinary)


# name -> (sql, error class the direct path must raise).
R1_ERROR_CASES = {
    "weight_nan": (_late(LATE_DECIDE, w_sql=_last_row("'NaN'::DOUBLE", "1.0")), "invalid_value"),
    "weight_infinite": (_late(LATE_DECIDE, w_sql=_last_row("'Infinity'::DOUBLE", "1.0")), "invalid_value"),
    "value_nan": (_late(LATE_DECIDE, v_sql=_last_row("'NaN'::DOUBLE", "1.0")), "invalid_value"),
    "value_infinite": (_late(LATE_DECIDE, v_sql=_last_row("'-Infinity'::DOUBLE", "1.0")), "invalid_value"),
    "cap_nan": (_late(LATE_DECIDE, cap_sql=_last_row("'NaN'::DOUBLE", "10.0")), "null_bound"),
    "infeasible_empty_box": (_late(LATE_DECIDE, cap_sql=_last_row("-1.0", "10.0")), "infeasible"),
    "infeasible_budget": (_late(LATE_DECIDE, limit="-1"), "infeasible"),
    "unbounded_zero_weight": (
        _late(LATE_DECIDE, w_sql=_last_row("0.0", "1.0"), cap_sql=_last_row("1e30", "10.0")),
        "unbounded",
    ),
    "unbounded_negative_weight": (
        _late(LATE_DECIDE, w_sql=_last_row("-1.0", "1.0"), cap_sql=_last_row("1e30", "10.0")),
        "unbounded",
    ),
    "unbounded_cap_is_1e20": (
        _late(LATE_DECIDE, w_sql=_last_row("-1.0", "1.0"), cap_sql=_last_row("1e20", "10.0")),
        "unbounded",
    ),
    "unbounded_cap_infinite": (
        _late(LATE_DECIDE, w_sql=_last_row("-1.0", "1.0"), cap_sql=_last_row("'Infinity'::DOUBLE", "10.0")),
        "unbounded",
    ),
    # The factor on the objective overflows the value, though every value is finite.
    "objective_factor_overflows": (
        _late(
            "amount >= 0 AND amount <= cap AND SUM(w*amount) <= {limit} MAXIMIZE 1e308 * SUM(v*amount)", v_sql="10.0"
        ),
        "invalid_value",
    ),
    "infeasible_before_unbounded": (
        _late(
            LATE_DECIDE,
            w_sql=_last_row("-1.0", "1.0"),
            cap_sql="CASE WHEN i = 4999 THEN 1e30 WHEN i = 0 THEN -1.0 ELSE 10.0 END",
        ),
        "infeasible",
    ),
    # A NaN value raises before any verdict, whichever row holds it: the budget is unreachable on every row, yet the bad
    # value on the last row is what the query reports.
    "nan_weight_before_infeasible_budget": (
        _late(LATE_DECIDE, w_sql=_last_row("'NaN'::DOUBLE", "1.0"), limit="-1"),
        "invalid_value",
    ),
    # A NULL value drops its row from the objective and hides nothing: row 0 still improves without end.
    "null_value_beside_unbounded": (
        _late(
            LATE_DECIDE,
            w_sql="CASE WHEN i = 0 THEN -1.0 ELSE 1.0 END",
            v_sql=_last_row("NULL::DOUBLE", "1.0"),
            cap_sql="CASE WHEN i = 0 THEN 1e30 ELSE 10.0 END",
        ),
        "unbounded",
    ),
}


# Budgets that are not `<=`. Every ordinary row holds up to 10 units of weight 1, so the boxes reach spends of 0 to
# 50,000 (the late row can change that).
LATE_BUDGET = "amount >= 0 AND amount <= cap AND {budget} MAXIMIZE SUM(v*amount)"
R1_ERROR_CASES.update(
    {
        # A required spend beyond what the boxes reach: above the highest (cup is zero), below the lowest (cdn is zero).
        "infeasible_minimum_above_highest_spend": (
            _late(LATE_BUDGET.format(budget="SUM(w*amount) >= 60000")),
            "infeasible",
        ),
        "infeasible_exact_fill_above_highest_spend": (
            _late(LATE_BUDGET.format(budget="SUM(w*amount) = 60000")),
            "infeasible",
        ),
        "infeasible_exact_fill_below_lowest_spend": (
            _late(LATE_BUDGET.format(budget="SUM(w*amount) = -1")),
            "infeasible",
        ),
        "infeasible_between_above_highest_spend": (
            _late(LATE_BUDGET.format(budget="SUM(w*amount) BETWEEN 55000 AND 60000")),
            "infeasible",
        ),
        # The limits cross, though the boxes reach both.
        "infeasible_between_limits_cross": (
            _late(LATE_BUDGET.format(budget="SUM(w*amount) BETWEEN 9 AND 3")),
            "infeasible",
        ),
        "infeasible_limits_cross_written_apart": (
            _late(LATE_BUDGET.format(budget="SUM(w*amount) >= 9 AND SUM(w*amount) <= 3")),
            "infeasible",
        ),
        # The last row has no cap: moving up for ever is allowed above a minimum.
        "unbounded_minimum_open_last_row": (
            _late(LATE_BUDGET.format(budget="SUM(w*amount) >= 3000"), cap_sql=_last_row("1e30", "10.0")),
            "unbounded",
        ),
        # A pair of open rows with prices that make moving between them gain, under an exact fill.
        "unbounded_exact_fill_open_pair": (
            _late(
                "amount >= -1e30 AND amount <= cap AND SUM(w*amount) = 3000 MAXIMIZE SUM(v*amount)",
                w_sql="1.0",
                v_sql="CASE WHEN i = 0 THEN 5.0 WHEN i = 4999 THEN 2.0 ELSE 1.0 END",
                cap_sql=_last_row("1e30", "CASE WHEN i = 0 THEN 4.0 ELSE 0.0 END"),
            ),
            "unbounded",
        ),
        # A bad value is an error whichever way the budget is written.
        "weight_nan_exact_fill": (
            _late(LATE_BUDGET.format(budget="SUM(w*amount) = 3000"), w_sql=_last_row("'NaN'::DOUBLE", "1.0")),
            "invalid_value",
        ),
    }
)

# A budget per group and under a `when`. Each case reaches a verdict that only a group, or a row outside every budget, can
# reach.
R1_ERROR_CASES.update(
    {
        # One group's minimum is beyond what its rows reach, though every other group's is met.
        "scoped_one_group_minimum_unreachable": (
            _scoped_sql(THREE_GROUPS[:3] + (_srow(4, "b", 1.0, 1.0, 2.0),), "SUM(w*amount) by (g) >= 7"),
            "infeasible",
        ),
        # One group has a row that gains without end, though the other group is bounded.
        "scoped_one_group_unbounded": (
            _scoped_sql(THREE_GROUPS[:3] + (_srow(4, "b", 1e30, -1.0, 1.0),), "SUM(w*amount) by (g) <= 6"),
            "unbounded",
        ),
        # A row outside every budget still needs a box some value satisfies, and a weight that is a number.
        "scoped_empty_box_outside_the_budget": (
            _scoped_sql(
                (_srow(1, "a", 4.0, 2.0, 10.0), _srow(2, "a", -1.0, 1.0, 9.0, f=False)), "when f: SUM(w*amount) <= 6"
            ),
            "infeasible",
        ),
    }
)


# Group a has two rows outside the `when`, with limits that differ, and one covered row whose limit is the group's; group b
# is covered and agrees.
OUTSIDE_ROWS = tuple(
    dict(row, f=row["id"] not in (2, 3), b={1: 6.0, 2: 3.0, 3: 4.0}.get(row["id"], row["b"])) for row in AGREEING_ROWS
)


# A budget read from the data. Each case reaches a check only a limit from the data has.
def _data_error(rows, budget, objective="max"):
    return _data_sql(DataBudgetCase(tuple(rows), budget, (), objective=objective))


def _with_limit(rows, index, value):
    return [dict(row, b=value) if i == index else row for i, row in enumerate(rows)]


R1_ERROR_CASES.update(
    {
        # An equality limit that is not the same on every row of a group: two different required spends.
        "data_budget_equality_differs_within_a_group": (
            _data_error(DATA_ROWS, "SUM(w*amount) by (g) = b"),
            "equality_varies",
        ),
        # The same without a scope: the one budget reads one value.
        "data_budget_equality_differs_without_a_scope": (
            _data_error(DATA_ROWS, "SUM(w*amount) = b"),
            "equality_varies",
        ),
        # A sum with a `when` of its own reads its limit over every row of the group, the rows it leaves out included.
        "data_budget_equality_differs_over_the_rows_outside_the_sum_when": (
            _data_error(OUTSIDE_ROWS, "SUM(when f: w*amount) by (g) = b"),
            "equality_varies",
        ),
        # A lower limit of a sum with a `when` of its own is the largest over the whole group: row 6 is left out of the
        # budget, yet its limit of 20 is beyond what the covered rows of group b reach.
        "data_budget_lower_over_the_rows_outside_the_sum_when_is_unreachable": (
            _data_error(
                [dict(row, b=20.0) if row["id"] == 6 else row for row in WHEN_ROWS],
                "SUM(when f: w*amount) by (g) >= b",
                "min",
            ),
            "infeasible",
        ),
        # A NaN limit, on the last row, in a column and in an expression.
        "data_budget_column_nan": (
            _data_error(_with_limit(DATA_ROWS, 5, float("nan")), "SUM(w*amount) <= b"),
            "null_bound",
        ),
        "data_budget_expression_nan": (
            _data_error(_with_limit(DATA_ROWS, 5, float("nan")), "SUM(w*amount) by (g) <= 2*b"),
            "null_bound",
        ),
        # One group's lower limit is beyond what its rows reach (b reaches 15), though a's is met.
        "data_budget_lower_unreachable_in_one_group": (
            _data_error(
                [dict(row, b=20.0) if row["g"] == "b" else row for row in DATA_ROWS], "SUM(w*amount) by (g) >= b", "min"
            ),
            "infeasible",
        ),
        # The limits cross in a group (at least 14 and at most 2 in a), though the boxes reach both.
        "data_budget_limits_cross_in_one_group": (
            _data_error(DATA_ROWS, "SUM(w*amount) by (g) >= b AND SUM(w*amount) by (g) <= cost"),
            "infeasible",
        ),
    }
)


@pytest.mark.error
@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(R1_ERROR_CASES))
def test_r1_error_cases_fail_on_both_paths(decidb_cli, name):
    sql, expected = R1_ERROR_CASES[name]
    with pytest.raises(DecidBCliError) as solver_error:
        _run(decidb_cli, sql, "off")
    assert error_class(str(solver_error.value)) == expected, str(solver_error.value)
    with direct_path(sql):
        with pytest.raises(DecidBCliError) as direct_error:
            _run(decidb_cli, sql, "require")
        assert "decide_direct_solve=require" not in str(direct_error.value), "no rule applied to the query"
        assert error_class(str(direct_error.value)) == expected, str(direct_error.value)


@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_r1_an_equality_limit_that_differs_only_among_rows_outside_the_when_is_not_an_error(decidb_cli):
    # The limit of a clause with a `when` is read over the rows it keeps, so the two rows left out, whose limits
    # differ, are not a second required spend.
    sql = _data_error(OUTSIDE_ROWS, "when f: SUM(w*amount) by (g) = b")
    values = {row["id"]: row["v"] for row in OUTSIDE_ROWS}
    solver = sum(values[row[0]] * row[1] for row in _run(decidb_cli, sql, "off")[0])
    with direct_path(sql):
        direct = sum(values[row[0]] * row[1] for row in _run(decidb_cli, sql, "require")[0])
        assert direct == pytest.approx(solver)


@pytest.mark.error
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_r1_late_bad_row_raises_under_count_and_where(decidb_cli):
    inner = _late(LATE_DECIDE, w_sql=_last_row("'NaN'::DOUBLE", "1.0"))
    for outer in (f"SELECT COUNT(*) FROM ({inner}) q", f"SELECT i FROM ({inner}) q WHERE i = 0"):
        with pytest.raises(DecidBCliError, match="invalid value"):
            _run(decidb_cli, outer, "require")
    assert _run(decidb_cli, f"SELECT i FROM ({inner}) q LIMIT 0", "require")[0] == []
    # LIMIT 1 reads the whole problem too: the last row is the unbounded one.
    unbounded = _late(LATE_DECIDE, w_sql=_last_row("-1.0", "1.0"), cap_sql=_last_row("1e30", "10.0"), tail="LIMIT 1")
    with pytest.raises(DecidBCliError, match="unbounded"):
        _run(decidb_cli, unbounded, "require")


@pytest.mark.error
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_r1_an_unused_decision_still_raises(decidb_cli):
    # The solver reads the whole problem even when no column of the decision is selected.
    sql = (
        "SELECT id FROM (SELECT * FROM (VALUES (1, -1.0, 1.0), (2, 1.0, 1.0)) t(id, w, v) "
        "DECIDE amount(REAL) SUCH THAT amount >= 0 AND SUM(w*amount) <= 5 MAXIMIZE SUM(v*amount)) q"
    )
    for mode in ("off", "require"):
        with pytest.raises(DecidBCliError, match="unbounded"):
            _run(decidb_cli, sql, mode)


# ---------------------------------------------------------------------------
# Values the oracle cannot hold: direct solve is compared with the solver path alone
# ---------------------------------------------------------------------------


def _problem(rows, bounds="amount >= lo AND amount <= cap", budget="SUM(w*amount) <= 6", obj="MAXIMIZE SUM(v*amount)"):
    return (
        f"SELECT id, amount FROM (SELECT * FROM (VALUES {_values(rows)}) v({', '.join(NAMES)})) "
        f"DECIDE amount(REAL) SUCH THAT {bounds} AND {budget} {obj} ORDER BY id"
    )


BOUNDARY_CASES = {
    # A column of type BOOLEAN cast to a number is a weight.
    "boolean_column_weight": (
        "SELECT id, amount FROM (SELECT * FROM (VALUES (1, true, 2.0::DOUBLE), (2, false, 3.0::DOUBLE)) v(id, f, s)) "
        "DECIDE amount(REAL) SUCH THAT amount BETWEEN 0 AND 4 AND SUM(f::INTEGER*amount) <= 3 MAXIMIZE SUM(s*amount)"
    ),
}


# A limit read from the data, of any size. The solver reads a number as the number it is, however large, and an infinite
# limit as infinite: an upper limit of +infinity is no bound, a lower limit of +infinity cannot be met. On boxes that all
# have caps this is the same as reading 1e20 and more as infinite, so these cases compare the two on the same rows.
# They stay on the solver path alone because the oracle holds no such values.
def _huge_limit_problem(comparison, limit, objective, bounds="amount BETWEEN 0 AND cap"):
    literal = sql_literal(limit, "DOUBLE")
    rows = ", ".join(
        f"({i}, {cap}, {w}, {v}, {literal})"
        for i, (cap, w, v) in enumerate([(4.0, 2.0, 10.0), (3.0, 3.0, 9.0), (5.0, 2.0, 4.0)], 1)
    )
    return (
        f"SELECT id, amount FROM (VALUES {rows}) t(id, cap, w, v, b) "
        f"DECIDE amount(REAL) SUCH THAT {bounds} AND SUM(w*amount) {comparison} b "
        f"{objective} SUM(v*amount) ORDER BY id"
    )


# An equality to a data limit of 1e19: a number, not "no bound".
BOUNDARY_CASES["data_budget_equal_1e19"] = _huge_limit_problem("=", 1e19, "MINIMIZE")


@pytest.mark.error
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_r1_an_upper_limit_of_minus_infinity_is_infeasible_though_a_row_has_no_floor(decidb_cli):
    # No assignment meets `SUM <= -infinity`, whatever a floorless row would allow. Direct solve says so; the solver
    # path stops with a status error of its own (it cannot tell this from an unbounded problem), so the two are not
    # compared as text.
    sql = _huge_limit_problem("<=", -INF, "MINIMIZE", bounds="amount >= -1e30 AND amount <= cap")
    assert _run_all(decidb_cli, sql, "require") == ("error", "infeasible")
    assert _run_all(decidb_cli, sql, "off")[0] == "error"


@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(BOUNDARY_CASES))
def test_r1_boundary_values_read_like_the_solver(decidb_cli, name):
    sql = BOUNDARY_CASES[name]
    direct = _run_all(decidb_cli, sql, "require")
    solver = _run_all(decidb_cli, sql, "off")
    assert direct[0] != "error" or "decide_direct_solve=require" not in direct[1], "no rule applied to the query"
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


@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize("limit", ["1e20", "'Infinity'::DOUBLE"])
def test_r1_leaves_a_limit_the_solver_reads_as_no_bound_to_the_solver(decidb_cli, limit):
    sql = _problem(WORKED, bounds="amount >= 0 AND amount <= cap", budget=f"SUM(w*amount) <= {limit}")
    require = _run_all(decidb_cli, sql, "require")
    assert require == ("error", "direct_not_applied")
    assert _run_all(decidb_cli, sql, "auto") == _run_all(decidb_cli, sql, "off")


@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize(
    "budget",
    [
        "SUM(w*amount) >= 1e20",
        "SUM(w*amount) >= -1e20",
        "SUM(w*amount) = 1e20",
        "SUM(w*amount) = 'Infinity'::DOUBLE",
        # Every comparison of the budget is checked, not only the first.
        "SUM(w*amount) >= 1 AND SUM(w*amount) <= 1e20",
        "SUM(w*amount) BETWEEN 1 AND 'Infinity'::DOUBLE",
    ],
)
def test_r1_leaves_a_minimum_or_exact_fill_the_solver_reads_as_no_bound_to_the_solver(decidb_cli, budget):
    sql = _problem(WORKED, bounds="amount >= 0 AND amount <= cap", budget=budget)
    assert _run_all(decidb_cli, sql, "require") == ("error", "direct_not_applied")
    assert _run_all(decidb_cli, sql, "auto") == _run_all(decidb_cli, sql, "off")


@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize(
    "budget",
    [
        # Different weights: two budgets with no single price per unit of budget.
        "SUM(w*amount) >= 1 AND SUM(v*amount) <= 20",
        # The same weights with another term, and with another sign on one term: not the same sum.
        "SUM(w*amount) >= 1 AND SUM(w*amount + 2*amount) <= 20",
        "SUM(w*amount + v*amount) >= 1 AND SUM(w*amount - v*amount) <= 40",
    ],
)
def test_r1_leaves_budgets_that_are_not_one_sum_to_the_solver(decidb_cli, budget):
    sql = _problem(WORKED, bounds="amount >= 0 AND amount <= cap", budget=budget)
    assert _run_all(decidb_cli, sql, "require") == ("error", "direct_not_applied")
    assert _run_all(decidb_cli, sql, "auto") == _run_all(decidb_cli, sql, "off")


# ---------------------------------------------------------------------------
# The plan: one sort, and nested problems
# ---------------------------------------------------------------------------


def _count(plan, operator):
    """How many times an operator's box appears in a physical plan."""
    return len(re.findall(rf"│\s+{operator}\s+(?:│|├)", plan))


def _ordered_windows(plan):
    """The WINDOW boxes whose expressions carry an ORDER BY: the sorts."""
    windows = plan.split("│           WINDOW          │")[1:]
    return [body for body in windows if "ORDER BY" in body.split("└")[0]]


# A stored table whose parent reads 20 columns the plan never uses (`pad_0` ... `pad_19`), so a window over source
# columns would be 20 DOUBLEs wider than one over the plan's own.
WIDE_SETUP = (
    "CREATE OR REPLACE TEMP TABLE r1_wide AS SELECT i AS id, 1.0 + (i % 3) AS w, 2.0 + (i % 5) AS v, 3.0 AS cap, "
    "i % 4 AS g, i % 7 > 0 AS f, "
    + ", ".join(f"((i * {k + 1}) % 101)::DOUBLE AS pad_{k}" for k in range(20))
    + " FROM range(2000) t(i);"
)
WIDE_READ = "id, x, " + ", ".join(f"pad_{k}" for k in range(20))


def _wide_query(budget):
    return f"SELECT {WIDE_READ} FROM (FROM r1_wide DECIDE x(REAL) SUCH THAT x BETWEEN 0 AND cap AND {budget} MAXIMIZE SUM(v*x)) q"


def _profiled_windows(decidb_cli, tmp_path, query):
    """Runs the query under the JSON profiler; returns (sorts, rows, bytes per row) for each WINDOW operator."""
    path = tmp_path / "profile.json"
    decidb_cli.execute_raw(
        f"SET decide_direct_solve='require'; {WIDE_SETUP} "
        f"PRAGMA enable_profiling='json'; PRAGMA profiling_output='{path}'; {query}"
    )
    widths = []

    def visit(node):
        if node.get("operator_type") == "WINDOW":
            ordered = "ORDER BY" in str(node["extra_info"])
            widths.append(
                (ordered, node["operator_cardinality"], node["result_set_size"] / node["operator_cardinality"])
            )
        for child in node.get("children", []):
            visit(child)

    visit(json.loads(path.read_text()))
    return widths


def _profiled_unpivots(decidb_cli, tmp_path, query, setup=WIDE_SETUP):
    """Runs the query under the JSON profiler; returns (rows, expressions of the left child) for each CROSS_PRODUCT whose
    right child is a scan of the decision numbers (a COLUMN_DATA_SCAN)."""
    path = tmp_path / "profile_unpivot.json"
    decidb_cli.execute_raw(
        f"SET decide_direct_solve='require'; {setup} "
        f"PRAGMA enable_profiling='json'; PRAGMA profiling_output='{path}'; {query}"
    )
    unpivots = []

    def visit(node):
        if node.get("operator_type") == "CROSS_PRODUCT":
            left, right = node["children"]
            if right["operator_type"] == "COLUMN_DATA_SCAN":
                unpivots.append((node["operator_cardinality"], left["extra_info"]["Projections"]))
        for child in node.get("children", []):
            visit(child)

    visit(json.loads(path.read_text()))
    return unpivots


@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_r1_plan_has_one_sort_and_no_solver_operator(decidb_cli):
    # A VALUES source is not a table scan, so it is materialized once and read through four CTE scans.
    sql = _sql(R1_CASES["mixed_binding_max"]).replace(" ORDER BY id", "")
    plan = decidb_cli.execute_raw(f"SET decide_direct_solve='require'; EXPLAIN {sql}").stdout
    assert "R1_CONTINUOUS_KNAPSACK" in plan and "min/max SUM(v*x)" in plan.replace("\n", ""), plan
    assert not re.search(r"│\s+DECIDE(_DIAGNOSE)?\s+│", plan), plan
    assert "ORDER_BY" not in plan, plan
    assert _count(plan, "CTE_SCAN") == 4 and _count(plan, "COLUMN_DATA_SCAN") == 1, plan
    assert _count(plan, "WINDOW") == 1 and len(_ordered_windows(plan)) == 1, plan


@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_r1_plan_reads_a_stored_table_four_times_and_sorts_narrow_columns(decidb_cli, tmp_path):
    # One budget: the aggregates have no GROUP BY and each stage meets the group's one row in a cross product.
    query = _wide_query("SUM(w*x) <= 2000")
    plan = decidb_cli.execute_raw(f"SET decide_direct_solve='require'; {WIDE_SETUP} EXPLAIN {query}").stdout
    assert "R1_CONTINUOUS_KNAPSACK" in plan and not re.search(r"│\s+DECIDE(_DIAGNOSE)?\s+│", plan), plan
    assert _count(plan, "SEQ_SCAN") == 4 and "CTE" not in plan, plan
    assert "ORDER_BY" not in plan and _count(plan, "WINDOW") == 1 and len(_ordered_windows(plan)) == 1, plan
    assert _count(plan, "UNGROUPED_AGGREGATE") == 3 and _count(plan, "CROSS_PRODUCT") == 3, plan
    assert _count(plan, "HASH_GROUP_BY") == 0 and _count(plan, "HASH_JOIN") == 0, plan
    # The sort carries the price, the rooms and their running sum, never a source column.
    ((ordered, rows, width),) = _profiled_windows(decidb_cli, tmp_path, query)
    assert ordered and rows == 2000 and width <= 5 * 8, (rows, width)


@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.xfail(strict=True, reason=BY_KEY_PENDING)
def test_r1_scoped_plan_groups_with_hash_aggregates_and_joins_on_the_key(decidb_cli, tmp_path):
    # A by key and a when: the group key is (in the budget, g). Each group-level stage is a hash aggregate, and the stages meet
    # the items in hash joins on that key. The empty-aggregate guard is a window over the groups, not over the rows.
    query = _wide_query("when f: SUM(w*x) by (g) <= 500")
    plan = decidb_cli.execute_raw(f"SET decide_direct_solve='require'; {WIDE_SETUP} EXPLAIN {query}").stdout
    assert _count(plan, "SEQ_SCAN") == 4 and "ORDER_BY" not in plan, plan
    assert _count(plan, "HASH_GROUP_BY") == 3 and _count(plan, "HASH_JOIN") == 3, plan
    assert _count(plan, "CROSS_PRODUCT") == 0 and len(_ordered_windows(plan)) == 1, plan
    assert _count(plan, "WINDOW") == 2, plan
    guard, sort = sorted(_profiled_windows(decidb_cli, tmp_path, query))
    assert not guard[0] and guard[1] <= 8, guard  # one row per group
    assert sort[0] and sort[1] == 2000 and sort[2] <= 6 * 8, sort


@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize(
    "budget, groups",
    [
        pytest.param("when f: SUM(w*x) <= 20000", 2, id="when"),
        pytest.param(
            "SUM(w*x) by (g) <= 2000", 4, id="per", marks=pytest.mark.xfail(strict=True, reason=BY_KEY_PENDING)
        ),
    ],
)
def test_r1_hash_joins_build_on_the_group_rows(decidb_cli, tmp_path, budget, groups):
    # The aggregates over the items are estimated as large as the join that feeds them, so an inner join of the items
    # to the group rows used to build its hash table on the items. A keyed join builds on the group rows.
    setup = (
        "CREATE OR REPLACE TEMP TABLE r1_many AS SELECT i AS id, 1.0 + (i % 3) AS w, 2.0 + (i % 5) AS v, 3.0 AS cap, "
        "i % 4 AS g, i % 2 = 0 AS f FROM range(20000) t(i);"
    )
    path = tmp_path / "profile_joins.json"
    query = f"SELECT id, x FROM (FROM r1_many DECIDE x(REAL) SUCH THAT x BETWEEN 0 AND cap AND {budget} MAXIMIZE SUM(v*x)) q"
    decidb_cli.execute_raw(
        f"SET decide_direct_solve='require'; {setup} "
        f"PRAGMA enable_profiling='json'; PRAGMA profiling_output='{path}'; {query}"
    )
    builds = []

    def visit(node):
        if node.get("operator_type") == "HASH_JOIN":
            builds.append(node["children"][1]["operator_cardinality"])
        for child in node.get("children", []):
            visit(child)

    visit(json.loads(path.read_text()))
    assert len(builds) == 3 and max(builds) <= groups, builds


@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_r1_plan_materializes_a_source_that_is_not_a_table_scan(decidb_cli):
    # A join is read once, as a materialized CTE, and each stage reads the copy.
    setup = (
        "CREATE OR REPLACE TEMP TABLE r1_left AS SELECT i AS id, 1.0 + (i % 3) AS w, 2.0 + (i % 5) AS v FROM range(50) t(i); "
        "CREATE OR REPLACE TEMP TABLE r1_right AS SELECT i AS id, 3.0 AS cap FROM range(50) t(i);"
    )
    source = "(SELECT a.id, a.w, a.v, b.cap FROM r1_left a JOIN r1_right b USING (id))"
    sql = f"{setup} EXPLAIN SELECT id, x FROM (FROM {source} DECIDE x(REAL) SUCH THAT x BETWEEN 0 AND cap AND SUM(w*x) <= 60 MAXIMIZE SUM(v*x)) q"
    plan = decidb_cli.execute_raw(f"SET decide_direct_solve='require'; {sql}").stdout
    assert _count(plan, "CTE_SCAN") == 4 and _count(plan, "HASH_JOIN") == 1 and _count(plan, "SEQ_SCAN") == 2, plan
    assert "ORDER_BY" not in plan and len(_ordered_windows(plan)) == 1, plan


@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize("name", ["minimum_binding_mixed_min", "exact_fill_mixed_max", "between_binding_low_min"])
def test_r1_plan_has_one_sort_under_a_minimum_an_exact_fill_and_a_range(decidb_cli, name):
    # The price is found from the same sort under every direction, however the limits are written.
    sql = _sql(R1_CASES[name]).replace(" ORDER BY id", "")
    plan = decidb_cli.execute_raw(f"SET decide_direct_solve='require'; EXPLAIN {sql}").stdout
    assert "R1_CONTINUOUS_KNAPSACK" in plan and not re.search(r"│\s+DECIDE(_DIAGNOSE)?\s+│", plan), plan
    windows = plan.split("│           WINDOW          │")[1:]
    ordered = [body for body in windows if "ORDER BY" in body.split("└")[0]]
    assert "ORDER_BY" not in plan and len(ordered) == 1, plan


# ---------------------------------------------------------------------------
# Sources the plan reads four times
# ---------------------------------------------------------------------------

# A source is re-read when it is a chain of projections and filters over one table scan, and materialized otherwise.
# `FROM r1_wide` stores 2000 rows; each source below keeps some of them in a different way.
STORED_SOURCES = {
    # A comparison on a column: filter pushdown turns it into a filter of the scan itself, which every read must carry.
    "scan_filter": "SELECT * FROM r1_wide WHERE g > 0",
    # A filter on an expression of two columns stays a filter operator above the scan.
    "filter_operator": "SELECT * FROM r1_wide WHERE w * v > g + 3",
    # Both, under a projection that computes the columns the problem reads.
    "projection_over_filters": "SELECT id, w * 2 AS w, v + g AS v, cap, g, f FROM r1_wide WHERE g > 0 AND w * v > 4",
    # Not a chain over one scan: a join and a source with a LIMIT are read once.
    "join": "SELECT a.id, a.w, a.v, b.cap, a.g, a.f FROM r1_wide a JOIN r1_wide b ON a.id = b.id WHERE b.id % 3 > 0",
    "limit": "SELECT * FROM r1_wide ORDER BY id LIMIT 700",
}


@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(STORED_SOURCES))
@pytest.mark.parametrize(
    "budget",
    [
        pytest.param("SUM(w*x) <= 300", id="one_budget"),
        pytest.param(
            "when f: SUM(w*x) by (g) <= 300", id="scoped", marks=pytest.mark.xfail(strict=True, reason=BY_KEY_PENDING)
        ),
    ],
)
def test_r1_sources_over_a_stored_table_agree_with_the_solver(decidb_cli, name, budget):
    # The answers of the direct plan are an optimum of the problem over exactly the rows the source keeps, whichever way
    # the rows are read: the objective and the spend match the solver's (rows may differ on ties).
    sql = (
        f"{WIDE_SETUP} SELECT COUNT(*), SUM(x), SUM(v*x), SUM(w*x) FROM "
        f"(FROM ({STORED_SOURCES[name]}) DECIDE x(REAL) SUCH THAT x BETWEEN 0 AND cap AND {budget} "
        f"MAXIMIZE SUM(v*x)) q"
    )
    direct = _run(decidb_cli, sql, "require")[0][0]
    solver = _run(decidb_cli, sql, "off")[0][0]
    assert direct[0] == solver[0] > 0
    assert direct[2] == pytest.approx(solver[2], rel=1e-9)
    assert direct[3] == pytest.approx(solver[3], rel=1e-9)


@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_r1_a_source_with_a_correlated_subquery_agrees_with_the_solver(decidb_cli):
    # The subquery in the bound makes the source a delim join, whose output order the optimizer may change after the plan
    # is built. The materialized copy must keep the order the reads expect.
    sql = (
        f"{WIDE_SETUP} SELECT COUNT(*), SUM(x), SUM(v*x), SUM(w*x) FROM "
        "(FROM r1_wide a DECIDE x(REAL) SUCH THAT x BETWEEN 0 AND (SELECT AVG(b.cap) + 1 FROM r1_wide b WHERE b.g = a.g) "
        "AND SUM(w*x) <= 300 MAXIMIZE SUM(v*x)) q"
    )
    direct = _run(decidb_cli, sql, "require")[0][0]
    solver = _run(decidb_cli, sql, "off")[0][0]
    assert direct[0] == solver[0] == 2000
    assert direct[2] == pytest.approx(solver[2], rel=1e-9)
    assert direct[3] == pytest.approx(solver[3], rel=1e-9)


@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_r1_a_volatile_source_is_read_once(decidb_cli):
    # random() changes on every evaluation, so the plan must not evaluate it once per stage: the rows it returns are one
    # set of rows, and the answer is an optimum for them. The check is independent of the solver: a greedy fill of the
    # returned rows by v / w.
    sql = (
        "SELECT i, w, v, x FROM (FROM (SELECT i, 1.0 + floor(random() * 4) AS w, 1.0 + floor(random() * 9) AS v, 3.0 AS cap "
        "FROM range(300) t(i)) DECIDE x(REAL) SUCH THAT x BETWEEN 0 AND cap AND SUM(w*x) <= 400 MAXIMIZE SUM(v*x)) q"
    )
    rows = _run(decidb_cli, sql, "require")[0]
    assert len(rows) == 300
    assert all(0 <= x <= 3.0 for _, _, _, x in rows)
    assert sum(w * x for _, w, _, x in rows) <= 400 + 1e-6
    budget, best = 400.0, 0.0
    for _, w, v, _ in sorted(rows, key=lambda row: row[2] / row[1], reverse=True):
        take = min(3.0, budget / w)
        best += v * take
        budget -= w * take
    assert sum(v * x for _, _, v, x in rows) == pytest.approx(best, rel=1e-9)


# Items tied at the price share what the budget leaves them in proportion to their room: every tied item moves the same
# share of its own room. All rows have the price 1 (v = w); the rows' rooms are 3, 8 and 1.
TIED_ROWS = "(1, 3.0, 1.0, 1.0), (2, 4.0, 2.0, 2.0), (3, 2.0, 0.5, 0.5)"


@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize("limit", [3.0, 9.0])
def test_r1_tied_items_share_in_proportion_to_their_room(decidb_cli, limit):
    sql = (
        f"SELECT id, x FROM (FROM (VALUES {TIED_ROWS}) t(id, cap, w, v) "
        f"DECIDE x(REAL) SUCH THAT x BETWEEN 0 AND cap AND SUM(w*x) <= {limit} MAXIMIZE SUM(v*x)) q ORDER BY id"
    )
    direct = _run(decidb_cli, sql, "require")[0]
    solver = _run(decidb_cli, sql, "off")[0]
    caps = {1: 3.0, 2: 4.0, 3: 2.0}
    weights = {1: 1.0, 2: 2.0, 3: 0.5}
    fraction = min(1.0, limit / 12.0)
    for id, x in direct:
        assert x == pytest.approx(fraction * caps[id])
    # The same spend and objective as the solver's, whose split of the tie may differ.
    assert sum(weights[id] * x for id, x in direct) == pytest.approx(sum(weights[id] * x for id, x in solver))


@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_r1_tied_items_with_infinite_rooms_share_what_the_finite_rooms_leave_evenly(decidb_cli):
    # Price 1 for all. Row 1 has a room of 2; rows 2 and 3 have no cap (weights 1 and 2). The budget of 10 fills row 1
    # (2) and leaves 8, which the two open rows share evenly in units of budget: 4 each.
    sql = (
        "SELECT id, x FROM (FROM (VALUES (1, 2.0, 1.0, 1.0), (2, 1e30, 1.0, 1.0), (3, 1e30, 2.0, 2.0)) t(id, cap, w, v) "
        "DECIDE x(REAL) SUCH THAT x BETWEEN 0 AND cap AND SUM(w*x) <= 10 MAXIMIZE SUM(v*x)) q ORDER BY id"
    )
    direct = _run(decidb_cli, sql, "require")[0]
    assert [row[1] for row in direct] == pytest.approx([2.0, 4.0, 2.0])


@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_r1_nested_decisions_agree(decidb_cli):
    # The outer DECIDE reads the inner decision's answer as data, so the inner plan's types and bindings must be exact.
    sql = """
        SELECT id, y FROM (
            SELECT id, x, w, v FROM (VALUES (1, 4.0, 2.0, 10.0), (2, 3.0, 3.0, 9.0), (3, 5.0, 2.0, 4.0)) t(id, cap, w, v)
            DECIDE x(REAL) SUCH THAT x BETWEEN 0 AND cap AND SUM(w*x) <= 14 MAXIMIZE SUM(v*x)
        ) DECIDE y(REAL) SUCH THAT y BETWEEN 0 AND x + 1 AND SUM(w*y) <= 12 MAXIMIZE SUM(v*y) ORDER BY id
    """
    direct, _ = _run(decidb_cli, sql, "require")
    solver, _ = _run(decidb_cli, sql, "off")
    assert [row[0] for row in direct] == [1, 2, 3]
    assert [row[1] for row in direct] == pytest.approx([row[1] for row in solver])
    plan = decidb_cli.execute_raw(f"SET decide_direct_solve='require'; EXPLAIN {sql}").stdout
    assert plan.count("R1_CONTINUOUS_KNAPSACK") >= 2, plan


@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_r1_a_parent_filter_or_limit_still_solves_the_whole_problem(decidb_cli):
    # Rows share one budget, so a row's answer depends on rows the parent drops.
    inner = _sql(R1_CASES["worked_example"]).replace(" ORDER BY id", "")
    for outer, expected in (
        (f"SELECT id, amount FROM ({inner}) q WHERE id <= 2 ORDER BY id", [(1, 4.0), (2, 2.0)]),
        (f"SELECT id, amount FROM ({inner}) q ORDER BY id LIMIT 2", [(1, 4.0), (2, 2.0)]),
        (f"SELECT COUNT(*) FROM ({inner}) q WHERE amount > 0", [(2,)]),
    ):
        assert _run(decidb_cli, outer, "require")[0] == expected, outer
        assert _run(decidb_cli, outer, "off")[0] == expected, outer


DUPLICATE_ROWS = (
    "SELECT id, amount FROM (VALUES (1, 5.0, 1.0, 2.0), (2, 4.0, 1.0, 3.0), (2, 4.0, 1.0, 3.0), (3, 6.0, 2.0, 1.0)) "
    "t(id, cap, w, v) DECIDE amount(REAL) SUCH THAT amount BETWEEN 0 AND cap AND SUM(w*amount) <= 8 "
    "MAXIMIZE SUM(v*amount) ORDER BY id"
)


@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_r1_duplicate_rows_stay_separate_rows(decidb_cli):
    sql = DUPLICATE_ROWS
    rows, _ = _run(decidb_cli, sql, "require")
    assert len(rows) == 4 and [row[0] for row in rows] == [1, 2, 2, 3]
    assert sum(row[1] * w for row, w in zip(rows, (1.0, 1.0, 1.0, 2.0))) <= 8.0 + 1e-9
    solver, _ = _run(decidb_cli, sql, "off")
    values = {1: 2.0, 2: 3.0, 3: 1.0}
    assert sum(values[row[0]] * row[1] for row in rows) == pytest.approx(sum(values[row[0]] * row[1] for row in solver))


# ---------------------------------------------------------------------------
# Several decisions in one budget
# ---------------------------------------------------------------------------
#
# A row may carry several `REAL` decisions (the pieces of a benefit) that share one budget: `SUM(w0*a + w1*b + ...)`.
# Each (row, decision) is an item in the ranking with its own weight, value and box, and each decision is answered in its
# own column. Nothing in the query orders or recombines the pieces of a row, so the pieces rank like rows whatever their
# values do, shrinking or growing. The oracle holds one variable per (row, decision) and builds its model from the raw
# rows and the box each case states in Python, never from DeciDB's reading of the SQL.

PIECE_NAMES = ("a", "b", "c")
PIECE_COLUMNS_OF = {"id": "INTEGER", "g": "VARCHAR"}


def _piece_columns(count):
    names = ["id", "g"]
    for d in range(count):
        names += [f"cap{d}", f"w{d}", f"v{d}"]
    return names


for _d in range(len(PIECE_NAMES)):
    PIECE_COLUMNS_OF.update({f"cap{_d}": "DOUBLE", f"w{_d}": "DOUBLE", f"v{_d}": "DOUBLE"})


def _prow(id, g, *pieces):
    """A row of pieces: each piece is (cap, w, v), the box [0, cap], the weight and the value of one decision."""
    row = dict(id=id, g=g)
    for d, (cap, w, v) in enumerate(pieces):
        row.update({f"cap{d}": cap, f"w{d}": w, f"v{d}": v})
    return row


@dataclass(frozen=True)
class PieceCase:
    rows: tuple
    count: int
    #: The (lower, upper) limits of the budget of each group, as the oracle holds them.
    limits: tuple
    #: The budget as written, with `{sum}` for the sum; and the decisions the sum names (a decision it does not name has
    #: no weight).
    written: str = "{sum} <= {upper}"
    budget_on: tuple = None
    #: True when the budget's sum is `by (g)`.
    per: bool = False
    maximize: bool = True
    #: The decisions the objective names.
    value_on: tuple = None


PIECE_CASES = {}


def pieces(name, rows, limits, **kwargs):
    PIECE_CASES[name] = PieceCase(tuple(rows), (len(rows[0]) - 2) // 3, limits, **kwargs)


def _on(case_, field):
    chosen = getattr(case_, field)
    return tuple(range(case_.count)) if chosen is None else chosen


def _piece_sql(case_):
    names = _piece_columns(case_.count)
    decisions = PIECE_NAMES[: case_.count]
    box = " AND ".join(f"{decisions[d]} BETWEEN 0 AND cap{d}" for d in range(case_.count))
    total = "SUM(" + " + ".join(f"w{d}*{decisions[d]}" for d in _on(case_, "budget_on")) + ")"
    total += " by (g)" if case_.per else ""
    lower, upper = case_.limits
    budget = case_.written.format(sum=total, upper=upper, lower=lower)
    value = "SUM(" + " + ".join(f"v{d}*{decisions[d]}" for d in _on(case_, "value_on")) + ")"
    objective = ("MAXIMIZE " if case_.maximize else "MINIMIZE ") + value
    declared = ", ".join(f"{name}(REAL)" for name in decisions)
    selected = ", ".join(["id", *decisions])
    values = values_clause(case_.rows, names, PIECE_COLUMNS_OF, sql_literal)
    source = f"(VALUES {values}) t({', '.join(names)})"
    decide = f"DECIDE {declared} SUCH THAT {box} AND {budget} {objective}"
    return f"SELECT {selected} FROM (FROM {source} {decide}) q ORDER BY id"


def _piece_key(case_, row):
    return row["g"] if case_.per else ()


def _piece_coefficients(case_, row, field, prefix):
    """The row's coefficient of each decision in one sum, as SQL sums it: a NULL in one piece makes the row's whole
    summand NULL, and SUM skips it."""
    chosen = _on(case_, field)
    if any(row[f"{prefix}{d}"] is None for d in chosen):
        return [0.0] * case_.count
    return [row[f"{prefix}{d}"] if d in chosen else 0.0 for d in range(case_.count)]


def _solve_pieces_with_oracle(oracle_solver, case_):
    oracle_solver.create_model("direct_r1_several_decisions")
    members, linear = {}, {}
    for index, row in enumerate(case_.rows):
        weights = _piece_coefficients(case_, row, "budget_on", "w")
        values = _piece_coefficients(case_, row, "value_on", "v")
        for d in range(case_.count):
            name = f"{PIECE_NAMES[d]}_{index}"
            oracle_solver.add_variable(name, VarType.CONTINUOUS, 0.0, min(row[f"cap{d}"], BIG))
            linear[name] = values[d]
            if d in _on(case_, "budget_on"):
                members.setdefault(_piece_key(case_, row), {})[name] = weights[d]
    lower, upper = case_.limits
    for weights in members.values():
        if lower == upper:
            oracle_solver.add_constraint(weights, "=", lower)
        else:
            if upper < INF:
                oracle_solver.add_constraint(weights, "<=", upper)
            if lower > -INF:
                oracle_solver.add_constraint(weights, ">=", lower)
    oracle_solver.set_objective(linear, ObjSense.MAXIMIZE if case_.maximize else ObjSense.MINIMIZE)
    return oracle_solver.solve()


def _check_pieces(case_, rows, columns, oracle, mode):
    assert columns == ["id", *PIECE_NAMES[: case_.count]], mode
    assert [row[0] for row in rows] == [row["id"] for row in case_.rows], mode
    spend, total = {}, 0.0
    for result, source in zip(rows, case_.rows):
        weights = _piece_coefficients(case_, source, "budget_on", "w")
        values = _piece_coefficients(case_, source, "value_on", "v")
        for d in range(case_.count):
            value = result[1 + d]
            assert isinstance(value, float), (mode, source["id"], d, value)
            cap = source[f"cap{d}"]
            assert -1e-9 <= value <= cap + 1e-9 * max(1.0, abs(cap)), (mode, source["id"], d, value, cap)
            if d in _on(case_, "budget_on"):
                key = _piece_key(case_, source)
                spend[key] = spend.get(key, 0.0) + weights[d] * value
            total += values[d] * value
    lower, upper = case_.limits
    for key, used in spend.items():
        assert lower - 1e-6 * max(1.0, abs(lower)) <= used <= upper + 1e-6 * max(1.0, abs(upper)), (mode, key, used)
    expected = oracle.objective_value
    assert abs(total - expected) <= 1e-4 * max(1.0, abs(expected)), (mode, total, expected)


def _agree_pieces(decidb_cli, oracle_solver, case_):
    oracle = _solve_pieces_with_oracle(oracle_solver, case_)

    def check_assignment(rows, columns, mode):
        _check_pieces(case_, rows, columns, oracle, mode)

    sql = _piece_sql(case_)
    agree_with_oracle(decidb_cli, sql, oracle, check_assignment)


# Each row is a benefit of two pieces that spend the same budget per unit. Shrinking: the first piece of a row earns more
# per unit than the second. Under `<= 7` the pieces rank 8, 6, 5, 4, 3, 1: the price falls inside the first piece of row 2.
SHRINKING = (
    _prow(1, "x", (2.0, 1.0, 8.0), (2.0, 1.0, 4.0)),
    _prow(2, "x", (3.0, 2.0, 10.0), (1.0, 2.0, 2.0)),
    _prow(3, "x", (2.0, 1.0, 6.0), (4.0, 1.0, 3.0)),
)
# Growing: the second piece of a row earns more than its first, so it outranks the first. The oracle still fills the pieces
# in the order of their prices; nothing in the query makes a row's pieces fill in the order they are written.
GROWING = (
    _prow(1, "x", (2.0, 1.0, 4.0), (2.0, 1.0, 8.0)),
    _prow(2, "x", (3.0, 2.0, 2.0), (1.0, 2.0, 10.0)),
    _prow(3, "x", (2.0, 1.0, 3.0), (4.0, 1.0, 6.0)),
)

# Shrinking pieces: the price falls inside a first piece, and the second pieces of the rows above it stay empty.
pieces("two_decisions_shrinking_piece_values", SHRINKING, (-INF, 7.0))
# Growing pieces: a row's second piece fills before its first, which no other case does (the item order within a row is
# the reverse of the decision order).
pieces("two_decisions_growing_piece_values", GROWING, (-INF, 7.0))
# The budget is generous enough for every piece of every row: the budget does not bind, so every decision takes its cap.
pieces("two_decisions_budget_does_not_bind", SHRINKING, (-INF, 100.0))
# Three decisions: the third item of each row (decision index 2) is picked by its own conditional in the group-back, and a
# row has three items in the ranking.
pieces(
    "three_decisions_shrinking_piece_values",
    (
        _prow(1, "x", (2.0, 1.0, 9.0), (2.0, 1.0, 5.0), (2.0, 1.0, 1.0)),
        _prow(2, "x", (1.0, 2.0, 12.0), (3.0, 2.0, 6.0), (1.0, 2.0, 2.0)),
        _prow(3, "x", (3.0, 1.0, 7.0), (1.0, 1.0, 4.0), (4.0, 1.0, 2.0)),
    ),
    (-INF, 9.0),
)
# Three decisions with growing values, so the third piece outranks the other two.
pieces(
    "three_decisions_growing_piece_values",
    (
        _prow(1, "x", (2.0, 1.0, 1.0), (2.0, 1.0, 5.0), (2.0, 1.0, 9.0)),
        _prow(2, "x", (1.0, 2.0, 2.0), (3.0, 2.0, 6.0), (1.0, 2.0, 12.0)),
        _prow(3, "x", (3.0, 1.0, 2.0), (1.0, 1.0, 4.0), (4.0, 1.0, 7.0)),
    ),
    (-INF, 9.0),
)
# Pieces that spend different amounts per unit: the weight is the decision's own, and the price is value over weight.
pieces(
    "two_decisions_with_their_own_weights",
    (
        _prow(1, "x", (2.0, 1.0, 5.0), (2.0, 4.0, 8.0)),
        _prow(2, "x", (3.0, 0.5, 2.0), (1.0, 2.0, 7.0)),
        _prow(3, "x", (2.0, 2.0, 9.0), (3.0, 0.25, 1.0)),
    ),
    (-INF, 6.0),
)
# The budget names the first decision only: the second has no weight, so it takes its own best end (its cap for a positive
# value, 0 for a negative one) without taking part in the ranking.
pieces(
    "two_decisions_second_not_in_the_budget",
    (
        _prow(1, "x", (2.0, 1.0, 8.0), (2.0, 1.0, 4.0)),
        _prow(2, "x", (3.0, 2.0, 10.0), (1.0, 1.0, -2.0)),
        _prow(3, "x", (2.0, 1.0, 6.0), (4.0, 1.0, 3.0)),
    ),
    (-INF, 5.0),
    budget_on=(0,),
)
# The budget names the second decision only: the first has no weight (the weight of decision 0 is the constant 0).
pieces(
    "two_decisions_first_not_in_the_budget",
    (
        _prow(1, "x", (2.0, 1.0, 8.0), (2.0, 1.0, 4.0)),
        _prow(2, "x", (3.0, 2.0, -10.0), (1.0, 1.0, 2.0)),
        _prow(3, "x", (2.0, 1.0, 6.0), (4.0, 1.0, 3.0)),
    ),
    (-INF, 3.0),
    budget_on=(1,),
)
# The objective names the first decision only: the second has the value 0, so under a minimum it only supplies budget.
pieces(
    "two_decisions_second_has_no_value_and_fills_a_minimum",
    (
        _prow(1, "x", (2.0, 1.0, 8.0), (3.0, 1.0, 0.0)),
        _prow(2, "x", (3.0, 2.0, 10.0), (2.0, 1.0, 0.0)),
    ),
    (6.0, INF),
    written="{sum} >= {lower}",
    value_on=(0,),
)
# A decision that frees budget (a negative weight) beside one that spends it, on the same row: freeing is worth it only
# for the spending it lets the other rows do.
pieces(
    "two_decisions_a_negative_weight_beside_a_positive_one",
    (
        _prow(1, "x", (4.0, 2.0, 7.0), (3.0, -1.0, 1.0)),
        _prow(2, "x", (3.0, 3.0, 9.0), (2.0, -2.0, 2.0)),
        _prow(3, "x", (5.0, 2.0, 4.0), (1.0, -1.0, 3.0)),
    ),
    (-INF, 6.0),
)
# MINIMIZE over a minimum: the cheapest pieces first, two decisions per row.
pieces(
    "two_decisions_minimize_a_cover",
    (
        _prow(1, "x", (4.0, 2.0, 6.0), (3.0, 1.0, 3.0)),
        _prow(2, "x", (3.0, 3.0, 5.0), (2.0, 1.0, 4.0)),
        _prow(3, "x", (5.0, 2.0, 2.0), (1.0, 1.0, 5.0)),
    ),
    (8.0, INF),
    written="{sum} >= {lower}",
    maximize=False,
)
# An exact fill over two decisions per row, reached from above: every piece at its high end spends more than the budget.
pieces("two_decisions_exact_fill", SHRINKING, (5.0, 5.0), written="{sum} = {upper}")
# An exact fill reached from below: the pieces at their low ends spend nothing, and the budget needs 9 of them at a loss.
pieces(
    "two_decisions_exact_fill_at_a_negative_price",
    (
        _prow(1, "x", (4.0, 1.0, -3.0), (2.0, 1.0, -1.0)),
        _prow(2, "x", (3.0, 2.0, -8.0), (3.0, 1.0, -2.0)),
    ),
    (9.0, 9.0),
    written="{sum} = {upper}",
)
# `by`: each group has its own budget and price, and a group holds items of several decisions.
pieces(
    "two_decisions_per_group",
    (
        _prow(1, "p", (2.0, 1.0, 8.0), (2.0, 1.0, 4.0)),
        _prow(2, "p", (3.0, 2.0, 10.0), (1.0, 2.0, 2.0)),
        _prow(3, "q", (2.0, 1.0, 6.0), (4.0, 1.0, 3.0)),
        _prow(4, "q", (2.0, 1.0, 5.0), (2.0, 1.0, 1.0)),
    ),
    (-INF, 4.0),
    per=True,
)
# The same prices tie across decisions and rows: any split of the tie is optimal.
pieces(
    "two_decisions_tied_prices",
    (
        _prow(1, "x", (2.0, 1.0, 3.0), (2.0, 2.0, 6.0)),
        _prow(2, "x", (3.0, 0.5, 1.5), (1.0, 1.0, 3.0)),
    ),
    (-INF, 5.0),
)
# A NULL in one piece makes the row's whole summand NULL, as in SQL: row 2 leaves the budget, both decisions, and row
# 1 leaves the objective, both decisions.
pieces(
    "two_decisions_null_weight_drops_the_row_from_the_budget",
    (_prow(1, "x", (2.0, 1.0, 8.0), (2.0, 1.0, 4.0)), _prow(2, "x", (3.0, None, 10.0), (1.0, 2.0, 2.0))),
    (-INF, 3.0),
)
pieces(
    "two_decisions_null_value_drops_the_row_from_the_objective",
    (_prow(1, "x", (2.0, 1.0, 8.0), (2.0, 1.0, None)), _prow(2, "x", (3.0, 2.0, 10.0), (1.0, 2.0, 2.0))),
    (-INF, 3.0),
)
# Errors the oracle finds: an empty box on one decision of one row (cap -1 leaves [0, -1]) is infeasible, whatever the
# other decisions of the row and the other rows do.
pieces(
    "two_decisions_empty_box_on_the_second_decision_of_a_row",
    (_prow(1, "x", (2.0, 1.0, 8.0), (2.0, 1.0, 4.0)), _prow(2, "x", (3.0, 2.0, 10.0), (-1.0, 2.0, 2.0))),
    (-INF, 7.0),
)
# A decision the budget does not name and nothing caps, with a positive value, improves without end.
pieces(
    "two_decisions_uncapped_second_decision_outside_the_budget",
    (_prow(1, "x", (2.0, 1.0, 8.0), (1e30, 1.0, 4.0)), _prow(2, "x", (3.0, 2.0, 10.0), (1.0, 1.0, 2.0))),
    (-INF, 7.0),
    budget_on=(0,),
)
# An uncapped decision inside the budget is bounded by it: the budget fills it, so nothing is unbounded.
pieces(
    "two_decisions_uncapped_second_decision_inside_the_budget",
    (_prow(1, "x", (2.0, 1.0, 8.0), (1e30, 1.0, 4.0)), _prow(2, "x", (3.0, 2.0, 10.0), (1.0, 1.0, 2.0))),
    (-INF, 7.0),
)


@pytest.mark.var_real
@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(PIECE_CASES))
def test_r1_several_decisions_oracle_solver_and_direct_agree(decidb_cli, oracle_solver, name):
    _agree_pieces(decidb_cli, oracle_solver, PIECE_CASES[name])


def _late_pair(budget_on="w*a + w*b", w_sql="1.0", v_sql="1.0", cap_b="10.0", limit="3000"):
    """A 5,000-row problem of two decisions whose last row is built by the given SQL. Rows 0 to 4998 are ordinary."""
    return (
        f"SELECT i, a, b FROM (SELECT i, {w_sql} AS w, {v_sql} AS v, 10.0 AS cap_a, {cap_b} AS cap_b "
        f"FROM range(5000) r(i)) DECIDE a(REAL), b(REAL) SUCH THAT a >= 0 AND a <= cap_a AND b >= 0 AND b <= cap_b "
        f"AND SUM({budget_on}) <= {limit} MAXIMIZE SUM(v*a + v*b)"
    )


# name -> (sql, error class both paths must raise). The bad value is on the second decision, or the problem is made
# bad by the second decision alone, on the last of 5,000 rows.
PIECE_ERROR_CASES = {
    # The empty box is the second decision's (the first decision's box is fine on every row).
    "empty_box_on_the_second_decision": (_late_pair(cap_b=_last_row("-1.0", "10.0")), "infeasible"),
    # The second decision is not in the budget, so nothing bounds it: its item is outside the ranking and improves forever.
    "uncapped_second_decision_outside_the_budget": (
        _late_pair(budget_on="w*a", cap_b=_last_row("1e30", "10.0")),
        "unbounded",
    ),
    # The weight of the second decision is NaN on the last row: its raw value is checked on every row.
    "second_decision_weight_nan": (_late_pair(w_sql=_last_row("'NaN'::DOUBLE", "1.0")), "invalid_value"),
    # The budget cannot be met: no group of items reaches the limit.
    "infeasible_minimum_over_two_decisions": (
        _late_pair().replace("<= 3000", ">= 999999"),
        "infeasible",
    ),
}


@pytest.mark.error
@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(PIECE_ERROR_CASES))
def test_r1_several_decisions_error_cases_fail_on_both_paths(decidb_cli, name):
    sql, expected = PIECE_ERROR_CASES[name]
    with pytest.raises(DecidBCliError) as solver_error:
        _run(decidb_cli, sql, "off")
    with pytest.raises(DecidBCliError) as direct_error:
        _run(decidb_cli, sql, "require")
    assert "decide_direct_solve=require" not in str(direct_error.value), "no rule applied to the query"
    assert error_class(str(solver_error.value)) == expected, str(solver_error.value)
    assert error_class(str(direct_error.value)) == expected, str(direct_error.value)


@pytest.mark.error
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_r1_several_decisions_unread_decisions_still_raise(decidb_cli):
    # A parent that reads no decision (or one) leaves the others unevaluated, but the solver reads the whole problem: the
    # verdict column that rides into the output keeps the check alive. The first row's second decision has an empty box.
    rows = "(1, 2.0, -1.0, 1.0, 5.0, 4.0), (2, 3.0, 1.0, 2.0, 5.0, 4.0)"
    source = f"SELECT * FROM (VALUES {rows}) t(id, cap_a, cap_b, w, v, z)"
    decide = (
        "DECIDE a(REAL), b(REAL) SUCH THAT a BETWEEN 0 AND cap_a AND b BETWEEN 0 AND cap_b "
        "AND SUM(w*a + w*b) <= 4 MAXIMIZE SUM(v*a + v*b)"
    )
    for select in ("SELECT id", "SELECT id, a", "SELECT COUNT(*)"):
        for mode in ("off", "require"):
            with pytest.raises(DecidBCliError, match="infeasible"):
                _run(decidb_cli, f"{select} FROM ({source} {decide}) q", mode)


@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_r1_several_decisions_a_parent_reads_one_decision_and_a_stored_payload(decidb_cli):
    # A stored table with a wide column read by the parent: the payload is read once more by the last stage, not carried
    # through the sort. A parent that reads only one of the two decisions gets that decision's answers.
    setup = (
        "CREATE OR REPLACE TEMP TABLE r1_pieces AS SELECT i AS id, 1.0 + (i % 3) AS w, 2.0 + (i % 5) AS va, "
        "1.0 + (i % 4) AS vb, 3.0 AS cap, repeat('p', 50) || i::VARCHAR AS payload FROM range(40) t(i);"
    )
    decide = (
        "DECIDE a(REAL), b(REAL) SUCH THAT a BETWEEN 0 AND cap AND b BETWEEN 0 AND cap AND SUM(w*a + w*b) <= 60 "
        "MAXIMIZE SUM(va*a + vb*b)"
    )
    for select in ("SELECT id, payload, b FROM", "SELECT id, a FROM", "SELECT SUM(a*va + b*vb) FROM"):
        sql = f"{setup} {select} (FROM r1_pieces {decide}) q ORDER BY 1"
        direct, _ = _run(decidb_cli, sql, "require")
        solver, _ = _run(decidb_cli, sql, "off")
        assert len(direct) == len(solver)
        if "payload" in select:
            assert [row[:2] for row in direct] == [row[:2] for row in solver]
    # The objective value, which every optimal assignment shares.
    sql = f"{setup} SELECT SUM(a*va + b*vb) FROM (FROM r1_pieces {decide}) q"
    assert _run(decidb_cli, sql, "require")[0][0][0] == pytest.approx(_run(decidb_cli, sql, "off")[0][0][0])


def _wide_pair_query(budget):
    return (
        f"SELECT {WIDE_READ}, y FROM (FROM r1_wide DECIDE x(REAL), y(REAL) SUCH THAT x BETWEEN 0 AND cap "
        f"AND y BETWEEN 0 AND cap AND {budget} MAXIMIZE SUM(v*x + pad_0*y)) q"
    )


@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize(
    "limit, scoped",
    [
        pytest.param("<= 2000", False, id="global"),
        pytest.param("<= 500", True, id="scoped", marks=pytest.mark.xfail(strict=True, reason=BY_KEY_PENDING)),
    ],
)
def test_r1_several_decisions_plan_has_one_sort_one_unpivot_and_one_aggregate(decidb_cli, tmp_path, limit, scoped):
    # Two decisions with constant limits run the same aggregate-then-join plan as one: one sort over narrow columns (the
    # price, the rooms and their running sum, never a source column), one unpivot (a cross product with the decision
    # numbers, not an UNNEST) of the narrow prices and rooms only, and no row number and no hash aggregate that groups the items back into rows.
    scope, by = ("when f: ", " by (g)") if scoped else ("", "")
    pair = _wide_pair_query(f"{scope}SUM(w*x + w*y){by} {limit}")
    plan = decidb_cli.execute_raw(f"SET decide_direct_solve='require'; {WIDE_SETUP} EXPLAIN {pair}").stdout
    assert "R1_CONTINUOUS_KNAPSACK" in plan and not re.search(r"│\s+DECIDE(_DIAGNOSE)?\s+│", plan), plan
    assert _count(plan, "SEQ_SCAN") == 4 and "ORDER_BY" not in plan, plan
    assert len(_ordered_windows(plan)) == 1 and _count(plan, "UNNEST") == 0, plan
    assert "ROW_NUMBER" not in plan, plan
    # The operators that group and join are those of one decision: nothing groups items back.
    single = _wide_query(f"{scope}SUM(w*x){by} {limit}")
    single_plan = decidb_cli.execute_raw(f"SET decide_direct_solve='require'; {WIDE_SETUP} EXPLAIN {single}").stdout
    assert _count(single_plan, "UNNEST") == 0 and _count(single_plan, "COLUMN_DATA_SCAN") == 0, single_plan
    for operator in ("HASH_GROUP_BY", "UNGROUPED_AGGREGATE", "HASH_JOIN", "WINDOW"):
        assert _count(plan, operator) == _count(single_plan, operator), (operator, plan)
    # The one cross product the pair adds is the unpivot.
    assert _count(plan, "CROSS_PRODUCT") == _count(single_plan, "CROSS_PRODUCT") + 1, plan
    assert _count(plan, "COLUMN_DATA_SCAN") == 1, plan
    sorts = [window for window in _profiled_windows(decidb_cli, tmp_path, pair) if window[0]]
    assert [(rows, width <= (6 if scoped else 5) * 8) for _, rows, width in sorts] == [(4000, True)], sorts
    # The unpivot reads the group key (with a scope), and the price and room of each decision, nothing else.
    unpivots = _profiled_unpivots(decidb_cli, tmp_path, pair)
    assert [(rows, len(inputs)) for rows, inputs in unpivots] == [(4000, 6 if scoped else 4)], unpivots


@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize("decisions", [1, 2])
@pytest.mark.parametrize(
    "scope",
    [
        pytest.param("", marks=pytest.mark.xfail(strict=True, reason=READ_THROUGH_PENDING)),
        pytest.param(" by (g)", marks=pytest.mark.xfail(strict=True, reason=BY_KEY_PENDING)),
    ],
    ids=["global", "per"],
)
def test_r1_a_limit_from_the_data_runs_the_plan_of_a_constant_limit(decidb_cli, tmp_path, decisions, scope):
    # The group's limits come from the stage that totals the group, so a limit read from the data adds no operator to
    # the plan of a constant one: one sort over narrow columns, and no window over source columns (a window left is
    # over the groups, one row each).
    def query(limit):
        if decisions == 1:
            return _wide_query(f"SUM(w*x){scope} <= {limit}")
        return _wide_pair_query(f"SUM(w*x + w*y){scope} <= {limit}")

    plans = {}
    for name, limit in (("data", "cap * 500"), ("constant", "1500")):
        plans[name] = decidb_cli.execute_raw(
            f"SET decide_direct_solve='require'; {WIDE_SETUP} EXPLAIN {query(limit)}"
        ).stdout
    plan = plans["data"]
    assert "R1_CONTINUOUS_KNAPSACK" in plan and not re.search(r"│\s+DECIDE(_DIAGNOSE)?\s+│", plan), plan
    assert _count(plan, "SEQ_SCAN") == 4 and "ORDER_BY" not in plan and len(_ordered_windows(plan)) == 1, plan
    for operator in (
        "HASH_GROUP_BY",
        "UNGROUPED_AGGREGATE",
        "HASH_JOIN",
        "CROSS_PRODUCT",
        "WINDOW",
        "COLUMN_DATA_SCAN",
    ):
        assert _count(plan, operator) == _count(plans["constant"], operator), (operator, plan)
    windows = _profiled_windows(decidb_cli, tmp_path, query("cap * 500"))
    ((_, rows, width),) = [window for window in windows if window[0]]
    assert rows == 2000 * decisions and width <= 6 * 8, (rows, width)
    assert all(rows <= 8 for ordered, rows, _ in windows if not ordered), windows


@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_r1_tied_items_of_several_decisions_share_in_proportion_to_their_room(decidb_cli):
    # Both decisions of every row have the price 1 (v = w), so the six items tie; their rooms are 3, 6, 8, 4, 1 and 2
    # units of budget. A budget of 12 of the 24 moves every item half of its own room.
    sql = (
        "SELECT id, x, y FROM (FROM (VALUES (1, 3.0, 1.0, 6.0, 1.0), (2, 4.0, 2.0, 2.0, 2.0), (3, 2.0, 0.5, 4.0, 0.5)) "
        "t(id, cap_x, w, cap_y, w2) DECIDE x(REAL), y(REAL) SUCH THAT x BETWEEN 0 AND cap_x AND y BETWEEN 0 AND cap_y "
        "AND SUM(w*x + w2*y) <= 12 MAXIMIZE SUM(w*x + w2*y)) q ORDER BY id"
    )
    direct = _run(decidb_cli, sql, "require")[0]
    assert [row[1] for row in direct] == pytest.approx([1.5, 2.0, 1.0])
    assert [row[2] for row in direct] == pytest.approx([3.0, 1.0, 2.0])
