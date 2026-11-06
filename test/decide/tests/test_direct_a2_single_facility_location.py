"""A2 (single-facility location), checked three ways: the independent oracle, the solver path, the direct path.

Every case is one DECIDE query with a decision `c` shared by many rows (one for the whole query, or one per class of a
key), under constant bounds, minimizing the squared distance to one value per row. The oracle builds its own model from the raw rows, from the interval and the
objective coefficients each case states in Python (never from DeciDB's reading of the SQL). DeciDB then runs the query
with `decide_direct_solve='off'` (the solver) and `'require'` (direct solve; a query the rule does not apply to fails
instead of quietly using the solver). The case passes when all three agree: both DeciDB runs return one value for every
row, inside the interval, that reaches the oracle's objective.

The solver has one behavior the oracle cannot derive from the SQL, so each case states it: a constant lower bound
replaces the variable's default lower bound of 0 (D41), and a row whose value is NULL contributes nothing to the
objective (the solver reads its square as absent).

When the objective keeps no row (its `when` drops every row, or every value is NULL), every value in the interval is
optimal. The oracle accepts any of them; the owner's ruling (2026-11-06) is that the direct path answers the point of the
interval nearest 0, which `test_a2_an_objective_that_keeps_no_row_answers_the_point_nearest_zero` pins exactly.

What a result comparison cannot see (which plan ran, EXPLAIN, prepared plans, DIAGNOSE, pruning) stays in
`test_direct_rule_conformance.py`, and the wording of the errors in `test_direct_user_facing.py`.

Note: `oracle_solver` caches each result under the test node id plus a hash of the test function's source. The case
tables live outside that source, so editing a case does not invalidate the cache; a stale value fails loudly with an
objective mismatch. After changing a case or the model builder, remove its entries from `results/oracle_cache.json`.
"""

import math
import random
from dataclasses import dataclass

import pytest

from decidb_cli import DeciDBCliError
from solver.types import ObjSense, VarType

from ._direct_differential import error_class
from ._direct_oracle_common import _allow_direct_path  # noqa: F401  (autouse fixture)
from ._direct_oracle_common import agree_with_oracle, outcome as _run_all, run as _run, sql_literal, values_clause
from ._number_range import writes_number_out_of_range


INF = math.inf
BIG = 1e30  # the oracle reads a bound this large as none

# ---------------------------------------------------------------------------
# Data. Every value is a dyadic rational, so a sum or a product of a few of them is exact in a double.
# ---------------------------------------------------------------------------

COLUMNS = (
    ("id", "INTEGER"),
    ("v", "DOUBLE"),  # the observed value each row compares the center with
    ("f", "BOOLEAN"),  # `when` membership
)
NAMES = [name for name, _ in COLUMNS]


#: The type of every column a case table uses.
TYPES = dict(COLUMNS, k="VARCHAR", j="INTEGER")


def _row(id, v, f):
    return dict(id=id, v=v, f=f)


def _krow(id, k, j, v, f):
    """A row of a keyed case: `k` and `j` are the key columns."""
    return dict(id=id, k=k, j=j, v=v, f=f)


ROWS = (
    _row(1, 2.0, True),
    _row(2, 4.0, False),
    _row(3, 9.0, True),
    _row(4, -1.5, False),
    _row(5, 6.25, True),
)
# The worked example of the rule's definition.
WORKED_ROWS = (_row(1, 2.0, True), _row(2, 4.0, True), _row(3, 9.0, True))
# Rows whose value is NULL contribute nothing to the objective.
NULL_ROWS = ROWS + (_row(6, None, True), _row(7, None, False))
ALL_NULL_ROWS = (_row(1, None, True), _row(2, None, True), _row(3, None, False))
# A NaN and an infinity on rows a `when` drops: nothing is computed on them.
DROPPED_BAD_ROWS = ROWS + (_row(6, math.nan, False), _row(7, INF, False), _row(8, -INF, False))
ONE_ROW = (_row(1, 7.5, True),)
ZERO_ROWS = (_row(1, 0.0, True), _row(2, 0.0, True))
DUPLICATE_ROWS = (_row(1, 2.0, True), _row(1, 2.0, True), _row(1, 4.0, True))


def _names(rows):
    return list(rows[0])


def _values(rows):
    return values_clause(rows, _names(rows), TYPES, sql_literal)


# ---------------------------------------------------------------------------
# Intervals, objectives and cases
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class Box:
    #: The constraints as written.
    sql: str
    #: The interval they allow `c`, derived here in Python and not read from DeciDB.
    lower: float
    upper: float


@dataclass(frozen=True)
class Objective:
    sql: str
    maximize: bool
    #: row -> (a, b): the row's `a*c^2 + b*c`, before the sense is applied; (0, 0) when the row contributes nothing.
    coefficients: object


def _term(k, w, t_of, mask=None):
    """`k * (w*c + t)^2` on a row with `t = t_of(v)`: `a = k*w^2` and `b = 2*k*w*t`. A row the mask drops, or whose value
    is NULL, contributes nothing, and nothing is computed on it."""

    def coefficients(row):
        if row["v"] is None or (mask is not None and not mask(row)):
            return (0.0, 0.0)
        t = t_of(row["v"])
        return (k * w * w, 2.0 * k * w * t)

    return coefficients


def _minus(v):
    return -v


def _plain(v):
    return v


OBJECTIVES = {
    "sq": Objective("MINIMIZE SUM(POWER(c - v, 2))", False, _term(1.0, 1.0, _minus)),
    "sq_reversed": Objective("MINIMIZE SUM(POWER(v - c, 2))", False, _term(1.0, -1.0, _plain)),
    "sq_plus": Objective("MINIMIZE SUM(POWER(c + v, 2))", False, _term(1.0, 1.0, _plain)),
    "sq_times": Objective("MINIMIZE SUM((c - v)*(c - v))", False, _term(1.0, 1.0, _minus)),
    "sq_computed": Objective("MINIMIZE SUM(POWER(c - (v + 1), 2))", False, _term(1.0, 1.0, lambda v: -(v + 1.0))),
    "sq_offset": Objective("MINIMIZE SUM(POWER(c - v, 2)) + 5", False, _term(1.0, 1.0, _minus)),
    "sq_scaled": Objective("MINIMIZE 3*SUM(POWER(c - v, 2))/2", False, _term(1.5, 1.0, _minus)),
    "sq_inside": Objective("MINIMIZE SUM(2*POWER(c - v, 2))", False, _term(2.0, 1.0, _minus)),
    "sq_slope": Objective("MINIMIZE SUM(POWER(2*c - v, 2))", False, _term(1.0, 2.0, _minus)),
    "sq_slope_negative": Objective("MINIMIZE SUM(POWER(v - 2*c, 2))", False, _term(1.0, -2.0, _plain)),
    "sq_max_negated": Objective("MAXIMIZE -SUM(POWER(c - v, 2))", True, _term(-1.0, 1.0, _minus)),
    "sq_max_inside": Objective("MAXIMIZE SUM(-POWER(c - v, 2))", True, _term(-1.0, 1.0, _minus)),
    "sq_min_double_negative": Objective("MINIMIZE -SUM(-POWER(c - v, 2))", False, _term(1.0, 1.0, _minus)),
    "sq_when": Objective("MINIMIZE when f: SUM(POWER(c - v, 2))", False, _term(1.0, 1.0, _minus, lambda r: r["f"])),
    "sq_filter": Objective("MINIMIZE SUM(when f: POWER(c - v, 2))", False, _term(1.0, 1.0, _minus, lambda r: r["f"])),
    "sq_both_whens": Objective(
        "MINIMIZE when v > 0: SUM(when f: POWER(c - v, 2))",
        False,
        _term(1.0, 1.0, _minus, lambda r: r["f"] and r["v"] > 0),
    ),
    "sq_when_none": Objective(
        "MINIMIZE when v > 100: SUM(POWER(c - v, 2))", False, _term(1.0, 1.0, _minus, lambda r: False)
    ),
}

BOXES = {
    "interior": Box("c BETWEEN 0 AND 10", 0.0, 10.0),
    "clamped_up": Box("c BETWEEN 6 AND 8", 6.0, 8.0),
    "clamped_down": Box("c BETWEEN 0 AND 3", 0.0, 3.0),
    "default_lower": Box("c <= 10", 0.0, 10.0),
    "default_lower_clamps": Box("c <= 3", 0.0, 3.0),
    "negative_lower_replaces_default": Box("c BETWEEN -5 AND 5", -5.0, 5.0),
    "negative_interval": Box("c BETWEEN -5 AND -2", -5.0, -2.0),
    "lower_only_clamps": Box("c >= 5", 5.0, INF),
    "lower_only_negative": Box("c >= -2", -2.0, INF),
    "open_lower": Box("c >= -1e30 AND c <= 4", -INF, 4.0),
    "open_both": Box("c >= -1e30 AND c <= 1e30", -INF, INF),
    "point": Box("c = 3", 3.0, 3.0),
    "several_bounds_tighten": Box("c >= 1 AND c >= 2 AND c <= 9 AND c <= 8", 2.0, 8.0),
    "scaled_bound": Box("2*c <= 4 AND c >= 0", 0.0, 2.0),
    "negated_bound": Box("-c >= -3", 0.0, 3.0),
    "offset_bound": Box("c + 1 <= 4", 0.0, 3.0),
    "reversed_bound": Box("10 >= c", 0.0, 10.0),
    "computed_bound": Box("c BETWEEN 0 AND 2 + 3", 0.0, 5.0),
}


@dataclass(frozen=True)
class Case:
    box: Box
    objective: Objective
    rows: tuple = ROWS
    #: The key columns of `DECIDE per k:`; empty for the query-wide `per ():`.
    keys: tuple = ()
    #: The key is the whole relation (`per t:`): a NULL in a column is a value of its own, not a missing class.
    relation: bool = False


def case(box, objective, rows=ROWS, keys=(), relation=False):
    return Case(BOXES[box], OBJECTIVES[objective], rows, keys, relation)


def _classes(case_):
    """key -> the rows of that class, derived here in Python. A row with a NULL in a key column is in no class, unless
    the key is a relation, where NULL is a value."""
    classes = {}
    for row in case_.rows:
        key = tuple(row[name] for name in (_names(case_.rows) if case_.relation else case_.keys))
        if not case_.relation and any(part is None for part in key):
            continue
        classes.setdefault(key, []).append(row)
    return classes


def _totals(case_, rows=None):
    """The objective's `A*c^2 + B*c` summed over the rows (of one class), as the sense reads it (not negated for a
    maximum)."""
    a = b = 0.0
    for row in case_.rows if rows is None else rows:
        ra, rb = case_.objective.coefficients(row)
        a, b = a + ra, b + rb
    return a, b


# ---------------------------------------------------------------------------
# Running a case
# ---------------------------------------------------------------------------


def _decide(case_):
    key = "t" if case_.relation else ", ".join(case_.keys) or "()"
    return f"DECIDE per {key}: c(REAL) SUCH THAT {case_.box.sql} {case_.objective.sql}"


def _sql(case_, wrap="plain"):
    decide = _decide(case_)
    source = f"(VALUES {_values(case_.rows)}) t({', '.join(_names(case_.rows))})"
    select = "SELECT id, c"
    ids = ", ".join(f"({row['id']})" for row in case_.rows)
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
        values = f"(VALUES {', '.join('(' + str(r['id']) + ', ' + sql_literal(r['v'], 'DOUBLE') + ')' for r in case_.rows)}) z(id, v)"
        flags = f"(VALUES {', '.join('(' + str(r['id']) + ', ' + sql_literal(r['f'], 'BOOLEAN') + ')' for r in case_.rows)}) y(id, f)"
        joined = f"SELECT z.id, z.v, y.f FROM {values} JOIN {flags} ON z.id = y.id"
        return f"{select} FROM (FROM ({joined}) j {decide}) q ORDER BY id"
    if wrap == "parent_join":
        return f"{select} FROM (FROM {source} {decide}) q JOIN (VALUES {ids}) k(id) USING (id) ORDER BY id"
    if wrap == "recombined_filters":
        # A filter pushed into the DECIDE input would solve a smaller problem. Here that changes the shared center, so
        # the plan must return every row once, each with the center of all of them.
        return (
            f"WITH solved AS MATERIALIZED (FROM {source} {decide}) "
            f"{select} FROM solved WHERE id < {middle} UNION ALL "
            f"{select} FROM solved WHERE id >= {middle} ORDER BY id"
        )
    raise ValueError(wrap)


def _solve_with_oracle(oracle_solver, case_):
    oracle_solver.create_model("direct_a2_single_facility_location")
    linear, quadratic = {}, {}
    for index, rows in enumerate(_classes(case_).values()):
        name = f"c{index}"
        oracle_solver.add_variable(name, VarType.CONTINUOUS, max(case_.box.lower, -BIG), min(case_.box.upper, BIG))
        a, b = _totals(case_, rows)
        linear[name] = b
        if a:
            quadratic[(name, name)] = a
    sense = ObjSense.MAXIMIZE if case_.objective.maximize else ObjSense.MINIMIZE
    if quadratic:
        oracle_solver.set_quadratic_objective({name: b for name, b in linear.items() if b}, quadratic, sense)
    else:
        # No row is kept, or every kept value is NULL: the objective is the constant 0.
        oracle_solver.set_objective(linear, sense)
    return oracle_solver.solve()


def _check_assignment(case_, rows, columns, oracle, mode):
    assert columns == ["id", "c"], mode
    assert [row[0] for row in rows] == [row["id"] for row in case_.rows], mode
    classes = _classes(case_)
    in_class = {id(row): key for key, members in classes.items() for row in members}
    centers = {key: set() for key in classes}
    for source_row, row in zip(case_.rows, rows):
        key = in_class.get(id(source_row))
        if key is None:
            # A row in no class has no decision (D5): it shows NULL.
            assert row[1] is None, (mode, "a row with no class shows NULL", row)
            continue
        assert isinstance(row[1], float), (mode, rows)
        centers[key].add(row[1])
    total = 0.0
    for key, members in classes.items():
        assert len(centers[key]) == 1, (mode, "every row of a class carries its one shared center", key, centers[key])
        center = next(iter(centers[key]))
        assert case_.box.lower - 1e-9 <= center <= case_.box.upper + 1e-9, (mode, center, case_.box)
        a, b = _totals(case_, members)
        total += a * center * center + b * center
    expected = oracle.objective_value
    assert abs(total - expected) <= 1e-5 * max(1.0, abs(expected)), (mode, total, expected)


def _agree(decidb_cli, oracle_solver, case_, wrap="plain", modes=("off", "require")):
    """Runs the case on the oracle and on the chosen DeciDB paths, and checks they agree."""
    sql = _sql(case_, wrap)
    oracle = _solve_with_oracle(oracle_solver, case_)

    def check_assignment(rows, columns, mode):
        _check_assignment(case_, rows, columns, oracle, mode)

    large = writes_number_out_of_range(sql, bounded=["c"])
    agree_with_oracle(decidb_cli, sql, oracle, check_assignment, modes, number_range=large)


# ---------------------------------------------------------------------------
# The case tables
# ---------------------------------------------------------------------------

A2_CASES = {}

# Every objective spelling, in an interval that holds the mean for most of them.
for _name in OBJECTIVES:
    if _name != "sq_when_none":
        A2_CASES[f"objective_{_name}"] = case("interior", _name)

# Every interval, under the plain square: the mean of ROWS is 3.95.
for _name in BOXES:
    A2_CASES[f"box_{_name}"] = case(_name, "sq")

# The mean clamped from each side, under objectives that change it.
A2_CASES["clamp_when_up"] = case("clamped_up", "sq_when")  # the mean of f rows is 5.75
A2_CASES["clamp_when_down"] = case("clamped_down", "sq_when")
A2_CASES["clamp_maximized_down"] = case("clamped_down", "sq_max_negated")
A2_CASES["clamp_negative_mean_to_default_lower"] = case("interior", "sq_plus")
A2_CASES["negative_mean_inside_a_negative_interval"] = case("negative_lower_replaces_default", "sq_plus")
A2_CASES["slope_with_clamp"] = case("clamped_down", "sq_slope")

# The objective keeps no row: every value of the interval is optimal.
for _name in BOXES:
    A2_CASES[f"no_row_kept_{_name}"] = case(_name, "sq_when_none")

# NULL values, a NaN and infinities on dropped rows, the worked example, one row and duplicate rows.
A2_CASES["null_value_drops_the_square"] = case("interior", "sq", NULL_ROWS)
A2_CASES["null_value_beside_a_when"] = case("interior", "sq_when", NULL_ROWS)
A2_CASES["null_values_clamped"] = case("clamped_up", "sq", NULL_ROWS)
A2_CASES["every_value_null"] = case("interior", "sq", ALL_NULL_ROWS)
A2_CASES["every_value_null_negative_interval"] = case("negative_interval", "sq", ALL_NULL_ROWS)
A2_CASES["bad_values_on_dropped_rows"] = case("interior", "sq_when", DROPPED_BAD_ROWS)
A2_CASES["bad_values_on_rows_a_part_filter_drops"] = case("interior", "sq_filter", DROPPED_BAD_ROWS)
A2_CASES["worked_example"] = case("interior", "sq", WORKED_ROWS)
A2_CASES["one_row"] = case("interior", "sq", ONE_ROW)
A2_CASES["one_row_clamped"] = case("clamped_down", "sq", ONE_ROW)
A2_CASES["duplicate_rows"] = case("interior", "sq", DUPLICATE_ROWS)


@pytest.mark.var_real
@pytest.mark.cons_perrow
@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(A2_CASES))
def test_a2_oracle_solver_and_direct_agree(decidb_cli, oracle_solver, name):
    _agree(decidb_cli, oracle_solver, A2_CASES[name])


@pytest.mark.correctness
def test_a2_the_worked_example_returns_the_mean(decidb_cli):
    # 2, 4 and 9 have the mean 5, and `center BETWEEN 0 AND 10` holds it.
    sql = (
        "SELECT id, center FROM (FROM (VALUES (1, 2.0), (2, 4.0), (3, 9.0)) t(id, value) "
        "DECIDE per (): center(REAL) SUCH THAT center BETWEEN 0 AND 10 "
        "MINIMIZE SUM(POWER(center - value, 2))) q ORDER BY id"
    )
    rows, columns = _run(decidb_cli, sql, "require")
    assert columns == ["id", "center"]
    assert rows == [(1, 5.0), (2, 5.0), (3, 5.0)]


# The interval's point nearest 0, derived here in Python: the owner's ruling for an objective that keeps no row.
NO_ROW_BOXES = {
    "interior": 0.0,
    "clamped_up": 6.0,
    "default_lower_clamps": 0.0,
    "negative_lower_replaces_default": 0.0,
    "negative_interval": -2.0,
    "lower_only_clamps": 5.0,
    "lower_only_negative": 0.0,
    "open_lower": 0.0,
    "open_both": 0.0,
    "point": 3.0,
}


@pytest.mark.correctness
@pytest.mark.parametrize("box", sorted(NO_ROW_BOXES))
@pytest.mark.parametrize("keeps_none", ["when", "null_values"])
def test_a2_an_objective_that_keeps_no_row_answers_the_point_nearest_zero(decidb_cli, box, keeps_none):
    interval = BOXES[box]
    nearest = min(max(0.0, interval.lower), interval.upper)
    assert nearest == NO_ROW_BOXES[box], "the table states the ruling in Python, the interval states the rest"
    case_ = case(box, "sq_when_none") if keeps_none == "when" else case(box, "sq", ALL_NULL_ROWS)
    rows, _ = _run(decidb_cli, _sql(case_), "require")
    assert [row[1] for row in rows] == [nearest] * len(case_.rows)
    # The solver may answer any other point of the interval, and every one is optimal.
    for row in _run(decidb_cli, _sql(case_), "off")[0]:
        assert interval.lower <= row[1] <= interval.upper


# ---------------------------------------------------------------------------
# A keyed decision: `DECIDE per k: c(REAL)` gives one center per class of the key
# ---------------------------------------------------------------------------

KEYED_ROWS = (
    _krow(1, "a", 1, 2.0, True),
    _krow(2, "a", 1, 4.0, False),
    _krow(3, "b", 1, 9.0, True),
    _krow(4, "b", 2, -1.5, False),
    _krow(5, "a", 2, 6.25, True),
    _krow(6, "c", 1, 3.0, True),
)
# A NULL in a key column puts the row in no class (D5); its value is never read. Row 9 has a class by `k` alone.
KEYED_NULL_ROWS = KEYED_ROWS + (
    _krow(7, None, 1, 100.0, True),
    _krow(8, None, 2, -50.0, False),
    _krow(9, "a", None, 40.0, True),
)
# The objective's `when` drops every row of class "d"; every value of class "e" is NULL.
KEYED_NO_KEPT_ROWS = KEYED_ROWS + (_krow(10, "d", 1, 5.0, False), _krow(11, "d", 2, 7.0, False))
KEYED_ALL_NULL_ROWS = KEYED_ROWS + (_krow(10, "e", 1, None, True), _krow(11, "e", 2, None, False))
# A NaN and an infinity on rows the `when` drops, and on rows with no class: nothing is computed on either.
KEYED_DROPPED_BAD_ROWS = KEYED_ROWS + (_krow(12, "a", 1, math.nan, False), _krow(13, "b", 1, -INF, False))
KEYED_NO_CLASS_BAD_ROWS = KEYED_ROWS + (_krow(12, None, 1, math.nan, True), _krow(13, None, 2, INF, True))
ONE_CLASS_ROWS = tuple(_krow(row["id"], "a", 1, row["v"], row["f"]) for row in ROWS)
DUPLICATE_KEYED_ROWS = (_krow(1, "a", 1, 2.0, True), _krow(1, "a", 1, 2.0, True), _krow(2, "b", 1, 4.0, True))
# `per t:` keys on the whole row: each distinct row is a class, and a NULL in it is a value.
RELATION_ROWS = (
    _krow(1, None, 1, 2.0, True),
    _krow(1, None, 1, 2.0, True),
    _krow(2, "a", 1, 4.0, True),
    _krow(3, None, None, 6.0, True),
    _krow(4, None, None, 6.0, False),
)
# Three classes whose means (4.08, 3.75 and 3) fall inside, above and below the interval.
KEYED_SPLIT_BOX = Box("c BETWEEN 3.9 AND 4.1", 3.9, 4.1)


def keyed(box, objective, rows, keys=("k",), relation=False):
    return Case(box if isinstance(box, Box) else BOXES[box], OBJECTIVES[objective], rows, keys, relation)


KEYED_CASES = {
    "several_keys": keyed("interior", "sq", KEYED_ROWS),
    "multi_column_key": keyed("interior", "sq", KEYED_ROWS, ("k", "j")),
    "null_key": keyed("interior", "sq", KEYED_NULL_ROWS),
    "null_in_one_column_of_a_multi_column_key": keyed("interior", "sq", KEYED_NULL_ROWS, ("k", "j")),
    "class_with_no_kept_row": keyed("interior", "sq_when", KEYED_NO_KEPT_ROWS),
    "class_with_no_kept_row_negative_interval": keyed("negative_interval", "sq_when", KEYED_NO_KEPT_ROWS),
    "class_with_no_kept_row_clamped_up": keyed("clamped_up", "sq_when", KEYED_NO_KEPT_ROWS),
    "class_with_every_value_null": keyed("interior", "sq", KEYED_ALL_NULL_ROWS),
    "centers_clamped_per_class": keyed(KEYED_SPLIT_BOX, "sq", KEYED_ROWS),
    "maximized": keyed("clamped_down", "sq_max_negated", KEYED_ROWS),
    "slope": keyed("interior", "sq_slope", KEYED_ROWS),
    "one_class": keyed("interior", "sq", ONE_CLASS_ROWS),
    "duplicate_rows": keyed("interior", "sq", DUPLICATE_KEYED_ROWS),
    "bad_values_on_rows_a_when_drops": keyed("interior", "sq_when", KEYED_DROPPED_BAD_ROWS),
    "bad_values_on_rows_with_no_class": keyed("interior", "sq", KEYED_NO_CLASS_BAD_ROWS),
    "relation_key_reads_null_as_a_value": keyed("interior", "sq", RELATION_ROWS, relation=True),
    "relation_key_with_a_when": keyed("interior", "sq_when", RELATION_ROWS, relation=True),
}


@pytest.mark.var_real
@pytest.mark.cons_perrow
@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(KEYED_CASES))
def test_a2_keyed_oracle_solver_and_direct_agree(decidb_cli, oracle_solver, name):
    _agree(decidb_cli, oracle_solver, KEYED_CASES[name])


@pytest.mark.correctness
@pytest.mark.parametrize("box", sorted(NO_ROW_BOXES))
def test_a2_a_class_with_no_kept_row_answers_the_point_nearest_zero(decidb_cli, box):
    # The ruling for the query-wide form holds per class: class "d" has no kept row, and the other classes keep theirs.
    interval = BOXES[box]
    nearest = min(max(0.0, interval.lower), interval.upper)
    assert nearest == NO_ROW_BOXES[box]
    case_ = keyed(box, "sq_when", KEYED_NO_KEPT_ROWS)
    rows, _ = _run(decidb_cli, _sql(case_), "require")
    assert [row[1] for row in rows if row[0] in (10, 11)] == [nearest, nearest]
    for row in _run(decidb_cli, _sql(case_), "off")[0]:
        if row[0] in (10, 11):
            assert interval.lower <= row[1] <= interval.upper


KEYED_CONTEXT_CASES = [
    (base, wrap)
    for base in ("null_key", "class_with_no_kept_row", "multi_column_key")
    for wrap in ("cte_materialized", "cte_inlined", "wide_source", "parent_join", "recombined_filters")
]


@pytest.mark.correctness
@pytest.mark.parametrize("base,wrap", KEYED_CONTEXT_CASES)
def test_a2_keyed_agrees_inside_a_parent_context(decidb_cli, oracle_solver, base, wrap):
    _agree(decidb_cli, oracle_solver, KEYED_CASES[base], wrap=wrap)


@pytest.mark.correctness
def test_a2_keyed_every_row_with_no_class_has_no_instance(decidb_cli):
    # No row has a class, so there is no decision: every row shows NULL, and the empty interval raises nothing because
    # there is no instance for it to empty.
    rows = tuple(_krow(i, None, 1, float(i), True) for i in range(1, 4))
    for bounds in ("c BETWEEN 0 AND 10", "c >= 5 AND c <= 3"):
        sql = (
            f"SELECT id, c FROM (FROM (VALUES {_values(rows)}) t({', '.join(_names(rows))}) "
            f"DECIDE per k: c(REAL) SUCH THAT {bounds} MINIMIZE SUM(POWER(c - v, 2))) q ORDER BY id"
        )
        for mode in ("off", "require"):
            assert _run(decidb_cli, sql, mode)[0] == [(1, None), (2, None), (3, None)], mode


@pytest.mark.correctness
def test_a2_keyed_empty_source_returns_no_rows_whatever_the_bounds_say(decidb_cli):
    for bounds in ("c BETWEEN 0 AND 10", "c >= 5 AND c <= 3"):
        sql = (
            "SELECT id, c FROM (SELECT 1 AS id, 'a' AS k, 1.0 AS v WHERE false) "
            f"DECIDE per k: c(REAL) SUCH THAT {bounds} MINIMIZE SUM(POWER(c - v, 2))"
        )
        for mode in ("off", "require"):
            assert _run(decidb_cli, sql, mode)[0] == []


@pytest.mark.error
@pytest.mark.correctness
def test_a2_keyed_an_empty_interval_raises_the_solvers_infeasible_error(decidb_cli, decidb_cli_highs):
    sql = (
        f"SELECT id, c FROM (FROM (VALUES {_values(KEYED_ROWS)}) t({', '.join(_names(KEYED_ROWS))}) "
        "DECIDE per k: c(REAL) SUCH THAT c >= 5 AND c <= 3 MINIMIZE SUM(POWER(c - v, 2))) q"
    )
    with pytest.raises(DeciDBCliError, match="(?i)infeasible") as direct:
        _run(decidb_cli, sql, "require")
    for cli in (decidb_cli, decidb_cli_highs):
        with pytest.raises(DeciDBCliError) as solver:
            _run(cli, sql, "off")
        assert str(solver.value) == str(direct.value)


CONTEXTS = (
    "cte_materialized",
    "cte_inlined",
    "wide_source",
    "joined_source",
    "parent_join",
    "recombined_filters",
)
CONTEXT_CASES = [
    (base, wrap)
    for base in ("objective_sq_when", "box_clamped_up", "null_value_drops_the_square", "no_row_kept_negative_interval")
    for wrap in CONTEXTS
]


@pytest.mark.correctness
@pytest.mark.parametrize("base,wrap", CONTEXT_CASES)
def test_a2_agrees_inside_a_parent_context(decidb_cli, oracle_solver, base, wrap):
    _agree(decidb_cli, oracle_solver, A2_CASES[base], wrap=wrap)


@pytest.mark.correctness
@pytest.mark.parametrize("value", ["0.0", "-0.0"])
@pytest.mark.parametrize("objective", ["MINIMIZE SUM(POWER(c - v, 2))", "MINIMIZE when v > 1: SUM(POWER(c - v, 2))"])
def test_a2_an_answer_of_zero_is_positive_zero(decidb_cli, value, objective):
    # A mean of -0.0 and a point of the interval nearest 0 (an objective that keeps no row) both print as 0.0.
    sql = (
        f"SELECT id, c FROM (SELECT * FROM (VALUES (1, {value}::DOUBLE), (2, {value}::DOUBLE)) t(id, v)) "
        f"DECIDE per (): c(REAL) SUCH THAT c BETWEEN -5 AND 5 {objective}"
    )
    rows, _ = _run(decidb_cli, sql, "require")
    assert [row[1] for row in rows] == [0.0, 0.0]
    assert all(math.copysign(1.0, row[1]) == 1.0 for row in rows), rows


@pytest.mark.correctness
def test_a2_a_filter_on_the_result_does_not_shrink_the_problem(decidb_cli):
    # The center is the mean of every row; a parent that keeps two rows still reads the mean of all five.
    case_ = A2_CASES["box_interior"]
    source = f"(VALUES {_values(case_.rows)}) t({', '.join(_names(case_.rows))})"
    filtered = f"SELECT id, c FROM (FROM {source} {_decide(case_)}) q WHERE id <= 2 ORDER BY id"
    full = _run(decidb_cli, _sql(case_), "require")[0]
    for mode in ("off", "require"):
        rows = _run(decidb_cli, filtered, mode)[0]
        assert [row[0] for row in rows] == [1, 2], mode
        assert all(abs(row[1] - full[0][1]) < 1e-9 for row in rows), (mode, rows, full)
    assert abs(full[0][1] - sum(row["v"] for row in case_.rows) / len(case_.rows)) < 1e-9


@pytest.mark.correctness
def test_a2_empty_source_returns_no_rows_whatever_the_bounds_say(decidb_cli):
    for bounds in ("c BETWEEN 0 AND 10", "c >= 0", "c >= -1e30 AND c <= 4"):
        for objective_sql in ("MINIMIZE SUM(POWER(c - v, 2))", "MINIMIZE when v > 0: SUM(POWER(c - v, 2))"):
            sql = (
                "SELECT id, c FROM (SELECT 1 AS id, 1.0 AS v WHERE false) "
                f"DECIDE per (): c(REAL) SUCH THAT {bounds} {objective_sql}"
            )
            for mode in ("off", "require"):
                assert _run(decidb_cli, sql, mode)[0] == []


@pytest.mark.error
@pytest.mark.correctness
@pytest.mark.parametrize("bounds", ["c >= 5 AND c <= 3", "c >= 1.5 AND c <= 1"])
def test_a2_an_empty_interval_raises_the_solvers_infeasible_error(decidb_cli, decidb_cli_highs, bounds):
    # The plan raises on any row. The full text is compared with the solver's on both backends, never with a
    # hand-written string alone.
    sql = (
        f"SELECT id, c FROM (FROM (VALUES {_values(ROWS)}) t({', '.join(_names(ROWS))}) "
        f"DECIDE per (): c(REAL) SUCH THAT {bounds} MINIMIZE SUM(POWER(c - v, 2))) q"
    )
    with pytest.raises(DeciDBCliError, match="(?i)infeasible") as direct:
        _run(decidb_cli, sql, "require")
    assert "decide_direct_solve=require" not in str(direct.value)
    for cli in (decidb_cli, decidb_cli_highs):
        with pytest.raises(DeciDBCliError) as solver:
            _run(cli, sql, "off")
        assert str(solver.value) == str(direct.value)
    # A count or a filter above still reads the problem.
    with pytest.raises(DeciDBCliError, match="(?i)infeasible"):
        _run(decidb_cli, f"SELECT COUNT(*) FROM ({sql}) z", "require")


# ---------------------------------------------------------------------------
# Random problems: random rows, interval and objective, against the oracle
# ---------------------------------------------------------------------------


@pytest.mark.var_real
@pytest.mark.cons_perrow
@pytest.mark.correctness
@pytest.mark.parametrize("seed", [11, 12, 13, 14])
def test_a2_random_problems_agree_with_the_oracle(decidb_cli, oracle_solver, seed):
    rng = random.Random(seed)
    rows = tuple(_row(i, rng.randrange(-80, 160) / 8.0, rng.random() < 0.6) for i in range(1, 41))
    lower = rng.randrange(-24, 24) / 4.0
    upper = lower + rng.randrange(0, 40) / 4.0
    box = Box(f"c >= {lower} AND c <= {upper}", lower, upper)
    objective = OBJECTIVES[rng.choice(["sq", "sq_when", "sq_slope", "sq_scaled", "sq_max_negated", "sq_both_whens"])]
    _agree(decidb_cli, oracle_solver, Case(box, objective, rows))


# ---------------------------------------------------------------------------
# Errors: both paths must raise
# ---------------------------------------------------------------------------


def _problem(rows, bounds="c BETWEEN 0 AND 10", obj="MINIMIZE SUM(POWER(c - v, 2))", tail="ORDER BY id"):
    return (
        f"SELECT id, c FROM (SELECT * FROM (VALUES {_values(rows)}) t({', '.join(_names(rows))})) "
        f"DECIDE per (): c(REAL) SUCH THAT {bounds} {obj} {tail}"
    )


def _with(index, **changes):
    rows = [dict(row) for row in ROWS]
    rows[index].update(changes)
    return rows


# name -> (sql, error class or message the direct path must raise).
A2_ERROR_CASES = {
    "nan_value": (_problem(_with(1, v=math.nan)), "invalid_value"),
    "infinite_value": (_problem(_with(1, v=INF)), "invalid_value"),
    "negative_infinite_value": (_problem(_with(1, v=-INF)), "invalid_value"),
    "nan_value_in_a_computed_value": (
        _problem(_with(1, v=math.nan), obj="MINIMIZE SUM(POWER(c - 2*v, 2))"),
        "invalid_value",
    ),
    "infinite_value_under_a_scale": (
        _problem(_with(1, v=INF), obj="MINIMIZE 3*SUM(POWER(c - v, 2))"),
        "invalid_value",
    ),
    "nan_value_beside_a_when_that_keeps_it": (
        _problem(_with(1, v=math.nan), obj="MINIMIZE when id > 0: SUM(POWER(c - v, 2))"),
        "invalid_value",
    ),
    "linear_coefficient_too_large": (_problem(_with(1, v=1e308)), "computes a number too large for a DOUBLE"),
    # Only the sum of the squares' coefficients overflows: every value is 0, so the linear coefficient is 0.
    "quadratic_coefficient_too_large": (
        _problem(ZERO_ROWS, obj="MINIMIZE 1e308*SUM(POWER(c - v, 2))"),
        "computes a number too large for a DOUBLE",
    ),
}


@pytest.mark.error
@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(A2_ERROR_CASES))
def test_a2_error_cases_fail_on_both_paths(decidb_cli, name):
    sql, expected = A2_ERROR_CASES[name]
    with pytest.raises(DeciDBCliError) as solver_error:
        _run(decidb_cli, sql, "off")
    with pytest.raises(DeciDBCliError) as direct_error:
        _run(decidb_cli, sql, "require")
    assert "decide_direct_solve=require" not in str(direct_error.value), "no rule applied to the query"
    assert error_class(str(direct_error.value)) == error_class(str(solver_error.value))
    assert expected in str(direct_error.value) or error_class(str(direct_error.value)) == expected


@pytest.mark.error
@pytest.mark.correctness
def test_a2_a_bad_value_on_the_last_row_raises_under_count_and_where(decidb_cli):
    inner = (
        "SELECT i, c FROM (SELECT i, CASE WHEN i = 4999 THEN 'NaN'::DOUBLE ELSE 1.0 END AS v FROM range(5000) r(i)) "
        "DECIDE per (): c(REAL) SUCH THAT c BETWEEN 0 AND 10 MINIMIZE SUM(POWER(c - v, 2))"
    )
    for outer in (f"SELECT COUNT(*) FROM ({inner}) q", f"SELECT i FROM ({inner}) q WHERE i = 0"):
        with pytest.raises(DeciDBCliError, match=r"v is NaN or infinite on some rows"):
            _run(decidb_cli, outer, "require")
    assert _run(decidb_cli, f"SELECT i FROM ({inner}) q LIMIT 0", "require")[0] == []


# ---------------------------------------------------------------------------
# Values the oracle cannot hold or types it does not model: direct solve is compared with the solver path alone
# ---------------------------------------------------------------------------


# A case is its SQL, or the SQL and whether the solver's model holds a number out of range, for a number the SQL text
# does not show as one (a lower bound of 1e30 or an upper bound of -1e25 is a bound, not the absence of one; a value of
# 1e-9 makes a linear coefficient of -2e-9).


def _typed(value_sql, bounds="c BETWEEN 0 AND 10", obj="MINIMIZE SUM(POWER(c - v, 2))"):
    return (
        f"SELECT id, c FROM (SELECT * FROM (VALUES (1, {value_sql}), (2, {value_sql}), (3, {value_sql})) t(id, v)) "
        f"DECIDE per (): c(REAL) SUCH THAT {bounds} {obj}"
    )


BOUNDARY_CASES = {
    "decimal_source": (
        "SELECT id, c FROM (SELECT * FROM (VALUES (1, 2.50::DECIMAL(9,2)), (2, 4.25::DECIMAL(9,2))) t(id, v)) "
        "DECIDE per (): c(REAL) SUCH THAT c BETWEEN 0 AND 10 MINIMIZE SUM(POWER(c - v, 2))"
    ),
    "float_source": (
        "SELECT id, c FROM (SELECT * FROM (VALUES (1, 2.5::FLOAT), (2, 4.25::FLOAT)) t(id, v)) "
        "DECIDE per (): c(REAL) SUCH THAT c BETWEEN 0 AND 10 MINIMIZE SUM(POWER(c - v, 2))"
    ),
    "tinyint_source": (
        "SELECT id, c FROM (SELECT * FROM (VALUES (1, 3::TINYINT), (2, 8::TINYINT)) t(id, v)) "
        "DECIDE per (): c(REAL) SUCH THAT c BETWEEN 0 AND 10 MINIMIZE SUM(POWER(c - v, 2))"
    ),
    "bigint_source": (
        "SELECT id, c FROM (SELECT * FROM (VALUES (1, 3000000000::BIGINT), (2, 9000000000::BIGINT)) t(id, v)) "
        "DECIDE per (): c(REAL) SUCH THAT c BETWEEN 0 AND 1e10 MINIMIZE SUM(POWER(c - v, 2))"
    ),
    "hugeint_source": (
        "SELECT id, c FROM (SELECT * FROM (VALUES (1, 25::HUGEINT), (2, 40::HUGEINT)) t(id, v)) "
        "DECIDE per (): c(REAL) SUCH THAT c BETWEEN 0 AND 100 MINIMIZE SUM(POWER(c - v, 2))"
    ),
    "unsigned_bigint_source": (
        "SELECT id, c FROM (SELECT * FROM (VALUES (1, 25::UBIGINT), (2, 40::UBIGINT)) t(id, v)) "
        "DECIDE per (): c(REAL) SUCH THAT c BETWEEN 0 AND 100 MINIMIZE SUM(POWER(c - v, 2))"
    ),
    "boolean_source": (
        "SELECT id, c FROM (SELECT * FROM (VALUES (1, true), (2, false), (3, true)) t(id, v)) "
        "DECIDE per (): c(REAL) SUCH THAT c BETWEEN 0 AND 10 MINIMIZE SUM(POWER(c - v, 2))"
    ),
    "upper_bound_1e25_is_no_bound": _typed("4.0", bounds="c <= 1e25"),
    "upper_bound_1e20_is_no_bound": _typed("4.0", bounds="c <= 1e20"),
    "lower_bound_1e30": (_typed("4.0", bounds="c >= 1e30"), True),
    "lower_bound_1e19": _typed("4.0", bounds="c >= 1e19 AND c <= 1e25"),
    "upper_bound_minus_1e25": (_typed("4.0", bounds="c >= -1e30 AND c <= -1e25"), True),
    "upper_bound_minus_1e19": _typed("4.0", bounds="c >= -1e30 AND c <= -1e19"),
    "huge_values": _typed("1e17", bounds="c BETWEEN 0 AND 1e18"),
    "huge_values_clamped": _typed("1e17"),
    "tiny_values": (_typed("1e-9", bounds="c BETWEEN -1 AND 1"), True),
    "negative_zero_value": _typed("-0.0", bounds="c BETWEEN -5 AND 5"),
    "tiny_factor": _typed("4.0", obj="MINIMIZE SUM(1e-9*POWER(c - v, 2))"),
    "strict_looking_equal_bounds": _typed("4.0", bounds="c >= 3 AND c <= 3"),
    "value_from_an_expression_of_integers": (
        "SELECT id, c FROM (SELECT * FROM (VALUES (1, 3), (2, 8)) t(id, v)) "
        "DECIDE per (): c(REAL) SUCH THAT c BETWEEN 0 AND 10 MINIMIZE SUM(POWER(c - (v * 7 / 2), 2))"
    ),
}


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


@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(BOUNDARY_CASES))
def test_a2_boundary_values_read_like_the_solver(decidb_cli, name):
    sql = BOUNDARY_CASES[name]
    if isinstance(sql, tuple):
        sql, warns = sql
    else:
        warns = writes_number_out_of_range(sql, bounded=["c"])
    direct = _run_all(decidb_cli, sql, "require")
    solver = _run_all(decidb_cli, sql, "off", warns)
    assert direct[0] != "error" or "decide_direct_solve=require" not in direct[1], "no rule applied to the query"
    assert _all_close(direct, solver), (direct, solver)


# ---------------------------------------------------------------------------
# The number-range warning
# ---------------------------------------------------------------------------


def _one_row(objective):
    return f"SELECT c FROM (FROM (VALUES (1, 0.3333333333333333)) t(id, v) DECIDE per (): c(REAL) SUCH THAT c BETWEEN 0 AND 10 {objective}) q"


@pytest.mark.var_real
@pytest.mark.correctness
@pytest.mark.parametrize(
    "objective, unscaled",
    [
        # a = 1e-200 and b = -6.7e-201 are in the normal range: the turning point holds its precision.
        pytest.param("MINIMIZE SUM(1e-200 * POWER(c - v, 2))", "MINIMIZE SUM(POWER(c - v, 2))", id="square-in-range"),
    ],
)
def test_a2_coefficients_that_keep_their_precision_are_silent(decidb_cli, objective, unscaled):
    direct = _run(decidb_cli, _one_row(objective), "require")[0][0][0]
    assert direct == pytest.approx(_run(decidb_cli, _one_row(unscaled), "require")[0][0][0], abs=1e-12)


@pytest.mark.var_real
@pytest.mark.correctness
@pytest.mark.parametrize(
    "objective",
    [
        # a = 1e-310 is below the normal range and b = -6.7e-311 with it.
        pytest.param("MINIMIZE SUM(1e-310 * POWER(c - v, 2))", id="square-below-range"),
        # a = 1 and b = -2e-310.
        pytest.param("MINIMIZE SUM(POWER(c - 1e-310 * v, 2))", id="linear-below-range-beside-a-square"),
    ],
)
def test_a2_warns_when_the_turning_point_leaves_the_normal_range(decidb_cli, objective):
    _run(decidb_cli, _one_row(objective), "require", expect_number_range=True)
