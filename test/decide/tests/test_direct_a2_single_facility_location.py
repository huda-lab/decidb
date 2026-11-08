"""A2 (single-facility location), checked three ways: the independent oracle, the solver path, the direct path.

Every case is one DECIDE query with a decision `c` shared by many rows (one for the whole query, or one per class of a
key), under bounds that are constants or read from the data, minimizing the squared distance to one value per row. The oracle builds its own model from the raw rows, from the interval and the
objective coefficients each case states in Python (never from DeciDB's reading of the SQL). DeciDB then runs the query
with `decide_direct_solve='off'` (the solver) and `'require'` (direct solve; a query the rule does not apply to fails
instead of quietly using the solver). The case passes when all three agree: both DeciDB runs return one value for every
row, inside the interval, that reaches the oracle's objective.

The solver has one behavior the oracle cannot derive from the SQL, so each case states it: a constant lower bound
replaces the variable's default lower bound of 0 (D41), and a row whose value is NULL contributes nothing to the
objective (the solver reads its square as absent).

Where several points are optimal (an objective that keeps no row, a flat or tied concave class, the middle of an even
count) the oracle accepts any of them. The owner's rulings (2026-11-06) are that the direct path answers the one nearest
0, the lower of two as near. Each oracle test derives that point in Python, from the case and not from the plan, and on
the `require` run compares the answer with it (`_check_ruled_point`).

Each case in a table is named for the branch of the plan it reaches. The branches: the square's turning point or an
end of the interval (the upper end, the lower end, the tie nearer either, a flat class), the lower and the upper median,
the midpoint of the extreme centers, a clamp up, down or none, the point nearest 0 for a class with no kept row, a NULL
value or weight, a row a `when` drops, a row with no class, each way the interval is written, and each spelling of an
objective once for the IR's reading of it.

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
TYPES = dict(COLUMNS, k="VARCHAR", j="INTEGER", lo="DOUBLE", cap="DOUBLE", w="DOUBLE")


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
# Rows whose value is NULL contribute nothing to the objective.
NULL_ROWS = ROWS + (_row(6, None, True), _row(7, None, False))
ALL_NULL_ROWS = (_row(1, None, True), _row(2, None, True), _row(3, None, False))
# A NaN and an infinity on rows a `when` drops: nothing is computed on them.
DROPPED_BAD_ROWS = ROWS + (_row(6, math.nan, False), _row(7, INF, False), _row(8, -INF, False))
ZERO_ROWS = (_row(1, 0.0, True), _row(2, 0.0, True))


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
    #: For a bound read from the data, the interval of a class derived from its rows: `rows -> (lower, upper)`. Without
    #: it every class has [lower, upper].
    interval: object = None


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
    "negative_lower_replaces_default": Box("c BETWEEN -5 AND 5", -5.0, 5.0),
    "negative_interval": Box("c BETWEEN -5 AND -2", -5.0, -2.0),
    "lower_only_clamps": Box("c >= 5", 5.0, INF),
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


def _interval(case_, rows):
    return case_.box.interval(rows) if case_.box.interval else (case_.box.lower, case_.box.upper)


def _totals(case_, rows=None):
    """The objective's `A*c^2 + B*c` summed over the rows (of one class), as the sense reads it (not negated for a
    maximum)."""
    a = b = 0.0
    for row in case_.rows if rows is None else rows:
        ra, rb = case_.objective.coefficients(row)
        a, b = a + ra, b + rb
    return a, b


def _ruled_point(a, b, lower, upper):
    """The point the rulings name for a class that minimizes `a*c^2 + b*c` on [lower, upper], derived by enumeration and
    not by the plan's formula: the turning point clamped into the interval when the objective is convex, else the best
    of the two ends, and among the optimal points the one nearest 0 (the lower of two as near), as the A2 tie rule says.
    An end that is infinite and best is unbounded, which raises and never reaches this."""
    if a > 0:
        return min(max(-b / (2.0 * a), lower), upper)
    if a == 0 and b == 0:
        return min(max(0.0, lower), upper)

    def value(c):
        return b * c if a == 0 else a * c * c + b * c

    best = min(value(lower), value(upper))
    return min((c for c in (lower, upper) if value(c) == best), key=lambda c: (abs(c), c))


def _check_ruled_point(mode, center, ruled, key):
    """The oracle accepts any optimal point and the solver path may answer any of them; the direct path answers the
    ruled one, and prints 0.0 for it, never -0.0."""
    if mode != "require":
        return
    assert center == pytest.approx(ruled, abs=1e-9, rel=1e-9), (mode, key, center, ruled)
    assert center != 0.0 or math.copysign(1.0, center) == 1.0, (mode, key, "-0.0 must print as 0.0")


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
        lower, upper = _interval(case_, rows)
        oracle_solver.add_variable(name, VarType.CONTINUOUS, max(lower, -BIG), min(upper, BIG))
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
        lower, upper = _interval(case_, members)
        assert lower - 1e-9 <= center <= upper + 1e-9, (mode, center, lower, upper)
        a, b = _totals(case_, members)
        total += a * center * center + b * center
        sign = -1.0 if case_.objective.maximize else 1.0
        _check_ruled_point(mode, center, _ruled_point(sign * a, sign * b, lower, upper), key)
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

# Every interval spelling once, under the plain square: the mean of ROWS is 3.95, so the interval decides the branch of
# the clamp (up, down or none) and how the box is read.
for _name in BOXES:
    if _name not in ("default_lower", "negative_lower_replaces_default"):
        A2_CASES[f"box_{_name}"] = case(_name, "sq")

# A target below 0 is clamped up to the default floor of 0, and a written negative lower bound replaces that floor
# (`sq_plus` has the mean -3.95).
A2_CASES["default_floor_clamps_a_negative_target"] = case("default_lower", "sq_plus")
A2_CASES["negative_lower_bound_replaces_the_default_floor"] = case("negative_lower_replaces_default", "sq_plus")

# The objective keeps no row, so every point of the interval is optimal and the answer is the point nearest 0: 0 inside
# the interval, its lower end above 0, its upper end below 0.
for _name in ("interior", "clamped_up", "negative_interval"):
    A2_CASES[f"no_row_kept_{_name}"] = case(_name, "sq_when_none")

# A row dropped by a NULL value alone and beside a `when`, a class of NULL values, and a NaN and infinities on rows a
# `when` drops (the objective's and the part's own).
A2_CASES["null_value_drops_the_square"] = case("interior", "sq", NULL_ROWS)
A2_CASES["null_value_beside_a_when"] = case("interior", "sq_when", NULL_ROWS)
A2_CASES["every_value_null"] = case("interior", "sq", ALL_NULL_ROWS)
A2_CASES["bad_values_on_dropped_rows"] = case("interior", "sq_when", DROPPED_BAD_ROWS)
A2_CASES["bad_values_on_rows_a_part_filter_drops"] = case("interior", "sq_filter", DROPPED_BAD_ROWS)


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
    # A key of one column and of two, each with a row that has no class (a NULL element), so one case holds both.
    "null_key": keyed("interior", "sq", KEYED_NULL_ROWS),
    "null_in_one_column_of_a_multi_column_key": keyed("interior", "sq", KEYED_NULL_ROWS, ("k", "j")),
    # A class with no kept row (the `when` drops every row of "d"; every value of "e" is NULL) beside classes that have
    # theirs: each answers the point of the interval nearest 0.
    "class_with_no_kept_row": keyed("interior", "sq_when", KEYED_NO_KEPT_ROWS),
    "class_with_every_value_null": keyed("interior", "sq", KEYED_ALL_NULL_ROWS),
    "centers_clamped_per_class": keyed(KEYED_SPLIT_BOX, "sq", KEYED_ROWS),
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


KEYED_CONTEXT_CASES = [
    (base, wrap)
    for base in ("null_in_one_column_of_a_multi_column_key",)
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


# ---------------------------------------------------------------------------
# Bounds read from the data: each row's `c <= cap` binds the same decision, so a class's interval is the tightest of
# them (the MAX of the lower ends and the MIN of the upper ends, over the rows each clause keeps)
# ---------------------------------------------------------------------------


def _drow(id, k, v, f, lo, cap):
    """A row of a data-bound case: `lo` and `cap` are the columns the bounds read."""
    return dict(id=id, k=k, v=v, f=f, lo=lo, cap=cap)


def _data_box(sql, lower=None, upper=None, when=None, lifted=None, floor=0.0, ceiling=INF):
    """A box whose ends come from the rows of each class, derived here in Python.

    `lower` and `upper` name the column each bound reads, `when(row)` is the clause's `when`, and `lifted` is the
    constant lower bound under that `when`, which replaces the default floor of 0 when some row of the class keeps it.
    `floor` and `ceiling` are the constant bounds beside them. A NULL bound is skipped and an infinite one is no bound;
    a data lower bound never replaces the default floor.
    """

    def interval(rows):
        kept = [row for row in rows if when is None or when(row)]
        start = lifted if lifted is not None and kept else floor
        low = max([start] + [row[lower] for row in kept if lower and row[lower] is not None])
        high = min([ceiling] + [row[upper] for row in kept if upper and row[upper] is not None])
        return low, high

    return Box(sql, floor, ceiling, interval)


TIGHT_ROWS = (
    _drow(1, "a", 2.0, True, -4.0, 7.0),
    _drow(2, "a", 4.0, False, -1.0, 3.0),
    _drow(3, "b", 9.0, True, 4.5, 8.0),
    _drow(4, "b", -1.5, False, 2.0, None),
    _drow(5, "a", 6.25, True, None, 5.5),
    _drow(6, "c", 3.0, True, 0.5, 12.0),
)
# Class "d" has no row that `f` keeps. A row with no class holds a bound that would empty every interval it reached.
TIGHT_D_ROWS = TIGHT_ROWS + (_drow(7, "d", 5.0, False, 2.0, 9.0),)
TIGHT_NULL_KEY_ROWS = TIGHT_ROWS + (_drow(8, None, 5.0, True, 100.0, -100.0),)
# Class "a" holds a lower bound above its tightest upper bound.
TIGHT_EMPTY_CLASS_ROWS = TIGHT_ROWS + (_drow(9, "a", 1.0, True, 6.0, 20.0),)
# A NaN bound on a row a `when` drops is never read.
TIGHT_DROPPED_NAN_ROWS = TIGHT_ROWS + (_drow(10, "a", 1.0, False, math.nan, math.nan),)

DATA_BOXES = {
    "upper": _data_box("c <= cap", upper="cap"),
    "lower": _data_box("c >= lo", lower="lo"),
    "both": _data_box("c >= lo AND c <= cap", lower="lo", upper="cap"),
    "between": _data_box("c BETWEEN lo AND cap", lower="lo", upper="cap"),
    "upper_under_when": _data_box("when f: c <= cap", upper="cap", when=lambda r: r["f"]),
    "lower_under_when": _data_box("when f: c >= lo", lower="lo", when=lambda r: r["f"]),
    "both_under_when": _data_box("when f: c BETWEEN lo AND cap", lower="lo", upper="cap", when=lambda r: r["f"]),
    # A constant lower bound under a `when` replaces the floor for the whole class when some row of it keeps the bound.
    "lifted_floor": _data_box("when f: c >= -4", when=lambda r: r["f"], lifted=-4.0),
    "lifted_floor_and_data_upper": _data_box(
        "when f: c BETWEEN -4 AND cap", upper="cap", when=lambda r: r["f"], lifted=-4.0
    ),
    # A coefficient read from the data: `(2 + f) * c <= cap` admits `c <= cap / (2 + f)`.
    "data_coefficient": Box(
        "(2 + f::INT) * c <= cap",
        0.0,
        INF,
        lambda rows: (0.0, min([INF] + [r["cap"] / (2 + int(r["f"])) for r in rows if r["cap"] is not None])),
    ),
    # Constant bounds and a data bound tighten the same interval.
    "constant_and_data": _data_box("c >= 1 AND c <= 6 AND c <= cap", upper="cap", floor=1.0, ceiling=6.0),
}


def tight(box, objective, rows, keys=()):
    return Case(DATA_BOXES[box], OBJECTIVES[objective], rows, keys)


DATA_CASES = {}
# Each way a clause is read once, keyed: an upper and a lower end together (as two clauses and as `BETWEEN`), under a
# `when`, a constant lower bound under a `when` beside a data upper bound, a coefficient read from the data, and
# constant bounds beside a data bound. The ungrouped aggregate is reached query-wide: the tightest ends of all the rows
# cross (an empty interval, which raises) or, under a `when`, hold a feasible interval.
for _name in (
    "both",
    "between",
    "both_under_when",
    "lifted_floor_and_data_upper",
    "data_coefficient",
    "constant_and_data",
):
    DATA_CASES[f"keyed_{_name}"] = tight(_name, "sq", TIGHT_D_ROWS, ("k",))
DATA_CASES["query_wide_empty_interval"] = tight("both", "sq", TIGHT_ROWS)
DATA_CASES["query_wide_both_under_when"] = tight("both_under_when", "sq", TIGHT_ROWS)
# A negative optimum meets the default floor of 0, which a data lower bound does not lower and a constant lower bound
# under a `when` does.
DATA_CASES["keyed_lower_keeps_the_floor"] = tight("lower", "sq_plus", TIGHT_D_ROWS, ("k",))
DATA_CASES["query_wide_lifted_floor_is_reached"] = tight("lifted_floor", "sq_plus", TIGHT_ROWS)
DATA_CASES["keyed_lifted_floor_per_class"] = tight("lifted_floor", "sq_plus", TIGHT_D_ROWS, ("k",))
DATA_CASES["keyed_bound_of_a_row_with_no_class_binds_nothing"] = tight("both", "sq", TIGHT_NULL_KEY_ROWS, ("k",))
DATA_CASES["keyed_data_bound_beside_an_objective_when"] = tight("both", "sq_when", TIGHT_D_ROWS, ("k",))
DATA_CASES["keyed_empty_interval_in_one_class"] = tight("both", "sq", TIGHT_EMPTY_CLASS_ROWS, ("k",))
DATA_CASES["query_wide_maximized"] = tight("upper", "sq_max_negated", TIGHT_ROWS)
DATA_CASES["query_wide_nan_bound_on_a_row_a_when_drops"] = tight("upper_under_when", "sq", TIGHT_DROPPED_NAN_ROWS)
DATA_CASES["keyed_nan_bound_on_a_row_a_when_drops"] = tight("both_under_when", "sq", TIGHT_DROPPED_NAN_ROWS, ("k",))
# Each class keeps no row (`v > 100` is never true), so each answers the point of its own interval nearest 0: above 0,
# inside it, or below it. "x" has no row the `when` keeps, so its floor stays 0; "y" and "z" lower it to -4.
DATA_CASES["keyed_no_kept_row_in_data_intervals"] = tight(
    "between",
    "sq_when_none",
    (_drow(1, "x", 5.0, False, 2.0, 9.0), _drow(2, "y", 5.0, True, -1.0, 4.0), _drow(3, "z", 5.0, True, 1.0, 4.0)),
    ("k",),
)
DATA_CASES["keyed_no_kept_row_under_a_lifted_floor"] = tight(
    "lifted_floor_and_data_upper",
    "sq_when_none",
    (_drow(1, "x", 5.0, False, 0.0, 9.0), _drow(2, "y", 5.0, True, 0.0, -2.0), _drow(3, "z", 5.0, True, 0.0, 4.0)),
    ("k",),
)


@pytest.mark.var_real
@pytest.mark.cons_perrow
@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(DATA_CASES))
def test_a2_data_bounds_oracle_solver_and_direct_agree(decidb_cli, oracle_solver, name):
    _agree(decidb_cli, oracle_solver, DATA_CASES[name])


DATA_CONTEXT_CASES = [
    (base, wrap)
    for base in ("keyed_both_under_when",)
    for wrap in ("cte_materialized", "wide_source", "parent_join", "recombined_filters")
]


@pytest.mark.correctness
@pytest.mark.parametrize("base,wrap", DATA_CONTEXT_CASES)
def test_a2_data_bounds_agree_inside_a_parent_context(decidb_cli, oracle_solver, base, wrap):
    _agree(decidb_cli, oracle_solver, DATA_CASES[base], wrap=wrap)


CONTEXTS = (
    "cte_materialized",
    "cte_inlined",
    "wide_source",
    "joined_source",
    "parent_join",
    "recombined_filters",
)
CONTEXT_CASES = [(base, wrap) for base in ("objective_sq_when",) for wrap in CONTEXTS]


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


def _problem(rows, bounds="c BETWEEN 0 AND 10", obj="MINIMIZE SUM(POWER(c - v, 2))", tail="ORDER BY id", key="()"):
    return (
        f"SELECT id, c FROM (SELECT * FROM (VALUES {_values(rows)}) t({', '.join(_names(rows))})) "
        f"DECIDE per {key}: c(REAL) SUCH THAT {bounds} {obj} {tail}"
    )


def _with_bound(index, rows=TIGHT_ROWS, **changes):
    rows = [dict(row) for row in rows]
    rows[index].update(changes)
    return rows


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
    # A NaN bound raises on the rows its clause keeps, whether or not the row has a class; a NULL one is skipped.
    "nan_data_upper_bound": (_problem(_with_bound(1, cap=math.nan), "c <= cap"), "null_bound"),
    "nan_data_lower_bound": (_problem(_with_bound(1, lo=math.nan), "c >= lo"), "null_bound"),
    "nan_data_bound_of_a_keyed_row": (_problem(_with_bound(1, cap=math.nan), "c <= cap", key="k"), "null_bound"),
    "nan_data_bound_of_a_row_with_no_class": (
        _problem(_with_bound(1, k=None, cap=math.nan), "c <= cap", key="k"),
        "null_bound",
    ),
    "nan_data_bound_under_a_when_that_keeps_it": (
        _problem(_with_bound(0, cap=math.nan), "when f: c <= cap"),
        "null_bound",
    ),
    # A bound's coefficient read from the data must be a number too.
    "nan_data_coefficient_of_a_bound": (_problem(_with_bound(1, lo=math.nan), "lo * c <= cap"), "invalid_value"),
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


def _capped(cap_sql, bounds="c <= cap", lo_sql="CAST(NULL AS DOUBLE)"):
    """Three rows whose bound columns are `lo_sql` and `cap_sql`, to be read as the solver reads a data bound."""
    rows = ", ".join(f"({i}, {i + 1.5}, {cap_sql}, {lo_sql})" for i in range(1, 4))
    return (
        f"SELECT id, c FROM (SELECT * FROM (VALUES {rows}) t(id, v, cap, lo)) "
        f"DECIDE per (): c(REAL) SUCH THAT {bounds} MINIMIZE SUM(POWER(c - v, 2))"
    )


BOUNDARY_CASES = {
    # An infinite data bound is no bound on its side, a NULL one is skipped, and the opposite infinity empties the
    # interval, as for a bound written in the query.
    "data_upper_infinite_is_no_bound": _capped("CAST('Infinity' AS DOUBLE)"),
    "data_upper_minus_infinite_is_infeasible": _capped("CAST('-Infinity' AS DOUBLE)"),
    "data_lower_minus_infinite_is_no_bound": _capped("CAST(NULL AS DOUBLE)", "c >= lo", "CAST('-Infinity' AS DOUBLE)"),
    "data_lower_infinite_is_infeasible": _capped("CAST(NULL AS DOUBLE)", "c >= lo", "CAST('Infinity' AS DOUBLE)"),
    "data_upper_every_row_null": _capped("CAST(NULL AS DOUBLE)"),
    "data_upper_1e25_is_no_bound": (_capped("1e25"), False),
    "data_upper_minus_1e25": (_capped("-1e25"), True),
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


# ---------------------------------------------------------------------------
# Data weights on the square: `SUM(w * POWER(c - v, 2))` with `w` read from the data
# ---------------------------------------------------------------------------
# A class's objective is `W*c^2 - 2*c*SUM(w*v) + constant` with `W` the total weight of its rows. Where `W > 0` the
# answer is the weighted mean clamped into the interval. Where `W <= 0` the objective is concave or linear and the
# answer is an end of the interval: the upper one when `a*(L+U) + b < 0`, and on a tie the point nearest 0 (the lower
# end when two ends are as near). An infinite end that is chosen is unbounded. A NULL weight skips its row; a NaN or
# infinite weight raises. The solver path answers these queries on Gurobi only: HiGHS refuses a square whose sign is
# known when the query runs, so a HiGHS run compares that refusal and nothing else.


def _wrow(id, k, v, w):
    return dict(id=id, k=k, v=v, w=w)


def _weighted(slope, t_of, k_of=_plain, mask=None):
    """`k * (slope*c + t)^2` with `k = k_of(w)` read from the row; a row with a NULL value or weight adds nothing."""

    def coefficients(row):
        if row["v"] is None or row["w"] is None or (mask is not None and not mask(row)):
            return (0.0, 0.0)
        k = k_of(row["w"])
        return (k * slope * slope, 2.0 * k * slope * t_of(row["v"]))

    return coefficients


WEIGHTED_OBJECTIVES = {
    "w_sq": Objective("MINIMIZE SUM(w * POWER(c - v, 2))", False, _weighted(1.0, _minus)),
    "w_sq_maximized": Objective("MAXIMIZE SUM(w * POWER(c - v, 2))", True, _weighted(1.0, _minus)),
    "w_sq_part_negated": Objective("MAXIMIZE -SUM(w * POWER(c - v, 2))", True, _weighted(1.0, _minus, _minus)),
    "w_sq_negated_weight": Objective("MINIMIZE SUM(-w * POWER(c - v, 2))", False, _weighted(1.0, _minus, _minus)),
    "w_sq_slope": Objective("MINIMIZE SUM(w * POWER(2*c - v, 2))", False, _weighted(2.0, _minus)),
    "w_sq_computed_weight": Objective(
        "MINIMIZE SUM((w + 1) * POWER(c - v, 2))", False, _weighted(1.0, _minus, lambda w: w + 1.0)
    ),
    "w_sq_scaled": Objective(
        "MINIMIZE 3 * SUM(w * POWER(c - v, 2)) / 2", False, _weighted(1.0, _minus, lambda w: 1.5 * w)
    ),
    "w_sq_when": Objective(
        "MINIMIZE when v > 0: SUM(w * POWER(c - v, 2))", False, _weighted(1.0, _minus, mask=lambda r: r["v"] > 0)
    ),
}

# (value, weight) pairs of one class. The comment says which branch of the answer the class reaches on [0, 10].
W_SETS = {
    "positive": ((2.0, 1.0), (4.0, 2.0), (9.0, 0.5), (-1.5, 1.0)),  # W > 0: the weighted mean
    "negative_far_upper": ((2.0, -1.0), (4.0, -2.0), (3.0, -1.0)),  # W < 0, a*(L+U)+b < 0: the upper end
    "negative_far_lower": ((7.0, -1.0), (9.0, -1.0), (8.0, -2.0)),  # W < 0, a*(L+U)+b > 0: the lower end
    "negative_total": ((2.0, 1.0), (6.0, -3.0), (4.0, 0.5)),  # signs mixed, W < 0
    "positive_total": ((2.0, 3.0), (6.0, -1.0), (4.0, 1.0)),  # signs mixed, W > 0: a mean from negative weights too
    "zero_total_lower": ((2.0, 1.0), (5.0, -1.0)),  # W = 0, b > 0: the lower end
    "zero_total_upper": ((5.0, 1.0), (2.0, -1.0)),  # W = 0, b < 0: the upper end
    "zero_total_flat": ((3.0, 1.0), (3.0, -1.0)),  # W = 0, b = 0: every point is optimal, the one nearest 0
    "null_weights_skip_their_rows": ((2.0, 1.0), (4.0, 2.0), (9.0, 0.5), (3.0, None), (None, None)),
    "every_weight_null": ((2.0, None), (4.0, None)),
    "concave_tie": ((5.0, -1.0),),  # W < 0 and f(L) = f(U) on [0, 10]: the end nearest 0
    "concave_tie_below_zero": ((-3.5, -1.0),),  # f(L) = f(U) on [-5, -2]: the upper end is the nearer
    "concave_tie_as_near": ((0.0, -1.0),),  # f(L) = f(U) on [-5, 5], both as near 0: the lower end
}


def _query_wide_rows(triples):
    return tuple(_wrow(i, "a", v, w) for i, (v, w) in enumerate(triples, start=1))


def _keyed_rows(**classes):
    rows = []
    for label, triples in classes.items():
        for v, w in triples:
            rows.append(_wrow(len(rows) + 1, label, v, w))
    return tuple(rows)


KEYED_W_ROWS = _keyed_rows(
    a=W_SETS["positive"],
    b=W_SETS["negative_far_upper"],
    c=W_SETS["negative_far_lower"],
    d=W_SETS["zero_total_lower"],
    e=W_SETS["zero_total_upper"],
    f=W_SETS["zero_total_flat"],
    g=W_SETS["every_weight_null"],
    h=W_SETS["positive_total"],
    i=W_SETS["negative_total"],
    j=W_SETS["concave_tie"],
    k=W_SETS["concave_tie_below_zero"],
    m=W_SETS["concave_tie_as_near"],
)
# A row with a NULL key has no decision, so its weight is never read.
KEYED_W_NULL_KEY_ROWS = KEYED_W_ROWS + (_wrow(40, None, 100.0, math.nan), _wrow(41, None, 3.0, -2.0))

W_OBJECTIVE = WEIGHTED_OBJECTIVES["w_sq"]


def _weighted_case(box, objective, rows, keys=()):
    return Case(BOXES[box], WEIGHTED_OBJECTIVES[objective], rows, keys)


W_CASES = {}
# One case per branch of the answer, each on the interval where the comment on its set says it is reached.
for _set, _box in (
    ("positive", "interior"),
    ("negative_far_upper", "interior"),
    ("negative_far_lower", "interior"),
    ("zero_total_lower", "interior"),
    ("zero_total_upper", "interior"),
    ("zero_total_flat", "interior"),
    ("concave_tie", "interior"),
    ("concave_tie_below_zero", "negative_interval"),
    ("concave_tie_as_near", "negative_lower_replaces_default"),
    ("null_weights_skip_their_rows", "interior"),
    ("every_weight_null", "interior"),
):
    W_CASES[f"{_set}_in_{_box}"] = _weighted_case(_box, "w_sq", _query_wide_rows(W_SETS[_set]))
# Every other spelling of the weighted objective once, on the class of positive weights.
for _name in WEIGHTED_OBJECTIVES:
    if _name != "w_sq":
        W_CASES[f"positive_under_{_name}"] = _weighted_case("interior", _name, _query_wide_rows(W_SETS["positive"]))
# An interval open on one side: a chosen finite end is the answer, and the improving direction of a linear objective
# decides whether the open end is chosen (the error table has that case).
W_CASES["zero_total_upper_with_an_open_lower_end"] = _weighted_case(
    "open_lower", "w_sq", _query_wide_rows(W_SETS["zero_total_upper"])
)
W_CASES["zero_total_flat_with_both_ends_open"] = _weighted_case(
    "open_both", "w_sq", _query_wide_rows(W_SETS["zero_total_flat"])
)
W_CASES["positive_with_both_ends_open"] = _weighted_case("open_both", "w_sq", _query_wide_rows(W_SETS["positive"]))
W_CASES["negative_far_upper_with_an_open_lower_end"] = _weighted_case(
    "open_lower", "w_sq", _query_wide_rows(W_SETS["negative_far_upper"])
)
# A NaN and an infinite weight on rows a `when` drops are never read.
W_CASES["bad_weights_on_dropped_rows"] = _weighted_case(
    "interior",
    "w_sq_when",
    _query_wide_rows(W_SETS["positive"] + ((-5.0, math.nan), (-3.0, INF))),
)
# One class of each branch of the answer in one keyed query, with rows of no class whose weights are never read.
W_CASES["keyed_null_key_in_interior"] = _weighted_case("interior", "w_sq", KEYED_W_NULL_KEY_ROWS, ("k",))
W_CASES["keyed_with_an_open_lower_end"] = _weighted_case(
    "open_lower",
    "w_sq",
    _keyed_rows(a=W_SETS["positive"], b=W_SETS["zero_total_upper"], c=W_SETS["zero_total_flat"]),
    ("k",),
)

# Weights beside bounds read from the data: each class's interval is the tightest of its rows'.
_WEIGHT_BY_ID = {1: 1.0, 2: -2.0, 3: 1.5, 4: 0.5, 5: -1.0, 6: 2.0, 7: 1.0}
_WEIGHT_NEGATIVE_BY_ID = {1: -1.0, 2: -2.0, 3: -1.5, 4: -0.5, 5: -1.0, 6: -2.0, 7: -1.0}
for _label, _by_id in (("mixed", _WEIGHT_BY_ID), ("negative", _WEIGHT_NEGATIVE_BY_ID)):
    _rows = tuple(dict(row, w=_by_id[row["id"]]) for row in TIGHT_D_ROWS)
    W_CASES[f"{_label}_weights_with_data_bounds_per_class"] = Case(DATA_BOXES["both"], W_OBJECTIVE, _rows, ("k",))
    # Class "a" alone: its interval is [0, 3].
    W_CASES[f"{_label}_weights_with_data_bounds_query_wide"] = Case(
        DATA_BOXES["both"], W_OBJECTIVE, tuple(row for row in _rows if row["k"] == "a")
    )


@pytest.mark.var_real
@pytest.mark.cons_perrow
@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(W_CASES))
def test_a2_weighted_oracle_solver_and_direct_agree(decidb_cli, oracle_solver, name):
    _agree(decidb_cli, oracle_solver, W_CASES[name])


W_CONTEXT_CASES = [
    (base, wrap)
    for base in ("mixed_weights_with_data_bounds_per_class",)
    for wrap in ("cte_materialized", "wide_source", "parent_join", "recombined_filters")
]


@pytest.mark.correctness
@pytest.mark.parametrize("base,wrap", W_CONTEXT_CASES)
def test_a2_weighted_agrees_inside_a_parent_context(decidb_cli, oracle_solver, base, wrap):
    _agree(decidb_cli, oracle_solver, W_CASES[base], wrap=wrap)


@pytest.mark.error
@pytest.mark.correctness
def test_a2_highs_refuses_a_square_whose_weight_is_read_from_the_data(decidb_cli_highs):
    # Recorded, not required of A2: on HiGHS the solver path cannot answer these queries (Gurobi does), so the
    # weighted forms are compared with Gurobi above. The refusal names the cause.
    with pytest.raises(DeciDBCliError, match="sign is known only when the query runs"):
        _run(decidb_cli_highs, _sql(W_CASES["positive_in_interior"]), "off")


# name -> (sql, error class the two paths must share). The two paths must raise, whichever error each chooses.
W_ERROR_CASES = {
    "nan_weight": (
        _problem(_query_wide_rows(W_SETS["positive"] + ((1.0, math.nan),)), obj=W_OBJECTIVE.sql),
        "invalid_value",
    ),
    "infinite_weight": (
        _problem(_query_wide_rows(W_SETS["positive"] + ((1.0, INF),)), obj=W_OBJECTIVE.sql),
        "invalid_value",
    ),
    "nan_weight_in_a_computed_weight": (
        _problem(
            _query_wide_rows(W_SETS["positive"] + ((1.0, math.nan),)),
            obj=WEIGHTED_OBJECTIVES["w_sq_computed_weight"].sql,
        ),
        "invalid_value",
    ),
    "nan_weight_of_a_keyed_row": (
        _problem(KEYED_W_ROWS[:-1] + (_wrow(99, "j", 1.0, math.nan),), obj=W_OBJECTIVE.sql, key="k"),
        "invalid_value",
    ),
    # A concave class whose best end is infinite improves without limit; so does a linear one that improves toward an
    # open end, and a maximized convex one.
    "negative_weights_with_no_upper_end": (
        _problem(_query_wide_rows(W_SETS["negative_far_upper"]), "c >= 0", W_OBJECTIVE.sql),
        "unbounded",
    ),
    "negative_weights_with_no_lower_end": (
        _problem(_query_wide_rows(W_SETS["negative_far_lower"]), "c >= -1e30 AND c <= 4", W_OBJECTIVE.sql),
        "unbounded",
    ),
    "negative_weights_with_both_ends_open": (
        _problem(_query_wide_rows(W_SETS["negative_far_lower"]), "c >= -1e30 AND c <= 1e30", W_OBJECTIVE.sql),
        "unbounded",
    ),
    "zero_total_improving_toward_an_open_end": (
        _problem(_query_wide_rows(W_SETS["zero_total_lower"]), "c >= -1e30 AND c <= 4", W_OBJECTIVE.sql),
        "unbounded",
    ),
    "maximized_positive_weights_with_no_upper_end": (
        _problem(_query_wide_rows(W_SETS["positive"]), "c >= 0", WEIGHTED_OBJECTIVES["w_sq_maximized"].sql),
        "unbounded",
    ),
    "one_unbounded_class": (
        _problem(_keyed_rows(a=W_SETS["positive"], b=W_SETS["negative_far_upper"]), "c >= 0", W_OBJECTIVE.sql, key="k"),
        "unbounded",
    ),
    # An empty interval is infeasible whatever the weights are.
    "negative_weights_in_an_empty_interval": (
        _problem(_query_wide_rows(W_SETS["negative_far_upper"]), "c >= 5 AND c <= 3", W_OBJECTIVE.sql),
        "infeasible",
    ),
}


@pytest.mark.error
@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(W_ERROR_CASES))
def test_a2_weighted_error_cases_fail_on_both_paths(decidb_cli, name):
    sql, expected = W_ERROR_CASES[name]
    with pytest.raises(DeciDBCliError) as solver_error:
        _run(decidb_cli, sql, "off")
    with pytest.raises(DeciDBCliError) as direct_error:
        _run(decidb_cli, sql, "require")
    assert "decide_direct_solve=require" not in str(direct_error.value), "no rule applied to the query"
    assert error_class(str(direct_error.value)) == error_class(str(solver_error.value)) == expected


# A total weight that cancels to 1.1e-16 beside a `b` of 4e300: the turning point -b / (2a) passes a DOUBLE. The problem
# is bounded, so a finite end of the interval is the answer (the solver path refuses it as a constant k*t^2 past a
# DOUBLE, which A2 never reads, as `rules.md` says), and an open end is an answer past a DOUBLE.
CANCELLING_WEIGHT = -0.9999999999999998


@pytest.mark.correctness
@pytest.mark.parametrize("sign", [1.0, -1.0])
def test_a2_a_turning_point_past_a_double_answers_the_finite_end(decidb_cli, sign):
    triples = ((sign * 1e300, 1.0), (-sign * 1e300, CANCELLING_WEIGHT))
    a = sum(w for _, w in triples)
    b = sum(-2.0 * w * v for v, w in triples)
    assert a > 0 and abs(b) < 1e301 and abs(b / (2.0 * a)) == INF, "the turning point overflows"
    expected = 10.0 if b < 0 else 0.0
    case_ = _weighted_case("interior", "w_sq", _query_wide_rows(triples))
    assert [row[1] for row in _run(decidb_cli, _sql(case_), "require")[0]] == [expected, expected]
    with pytest.raises(DeciDBCliError, match="computes a number too large"):
        _run(decidb_cli, _sql(case_), "off")


@pytest.mark.error
@pytest.mark.correctness
@pytest.mark.parametrize("bounds", ["c >= 0", "c >= -1e30 AND c <= 1e30"])
def test_a2_a_turning_point_past_a_double_with_an_open_end_is_too_large(decidb_cli, bounds):
    sql = _problem(
        _query_wide_rows(((1e300, 1.0), (-1e300, CANCELLING_WEIGHT))), bounds, WEIGHTED_OBJECTIVES["w_sq"].sql
    )
    with pytest.raises(DeciDBCliError, match=r"MINIMIZE SUM\(w \* POWER\(c - v, 2\)\) computes a number too large"):
        _run(decidb_cli, sql, "require")


# ---------------------------------------------------------------------------
# The absolute loss `SUM(ABS(c - v))` and the maximum absolute loss `MAX(ABS(c - v))`
# ---------------------------------------------------------------------------
# The oracle builds each class's problem as a linear program over auxiliary variables, from the terms each case states
# in Python (never from DeciDB's reading of the SQL). The absolute loss is minimized at a weighted median of the
# centers `-t/s`: every point between the lower and the upper median is optimal, and the answer is the one nearest 0
# (the A2 tie rule). The maximum absolute loss is minimized at the midpoint of the extreme centers, clamped into the
# interval, per class.


@dataclass(frozen=True)
class AbsObjective:
    sql: str
    #: "sum" for `SUM(ABS(...))`, "max" for `MAX(ABS(...))`.
    kind: str
    #: row -> (weight, slope, t): the row's `weight * |slope*c + t|`; None when the row contributes nothing.
    terms: object


def _abs_terms(weight, slope, t_of, mask=None):
    def terms(row):
        if row["v"] is None or (mask is not None and not mask(row)):
            return None
        return (weight, slope, t_of(row["v"]))

    return terms


ABS_OBJECTIVES = {
    "abs": AbsObjective("MINIMIZE SUM(ABS(c - v))", "sum", _abs_terms(1.0, 1.0, _minus)),
    "abs_reversed": AbsObjective("MINIMIZE SUM(ABS(v - c))", "sum", _abs_terms(1.0, -1.0, _plain)),
    "abs_norm": AbsObjective("MINIMIZE norm(c - v, 1)", "sum", _abs_terms(1.0, 1.0, _minus)),
    "abs_weight": AbsObjective("MINIMIZE SUM(3 * ABS(c - v))", "sum", _abs_terms(3.0, 1.0, _minus)),
    "abs_scaled": AbsObjective("MINIMIZE 3 * SUM(ABS(c - v)) / 2", "sum", _abs_terms(1.5, 1.0, _minus)),
    "abs_slope": AbsObjective("MINIMIZE SUM(ABS(2*c - v))", "sum", _abs_terms(1.0, 2.0, _minus)),
    "abs_slope_negative": AbsObjective("MINIMIZE SUM(ABS(v - 2*c))", "sum", _abs_terms(1.0, -2.0, _plain)),
    "abs_computed": AbsObjective("MINIMIZE SUM(ABS(c - (v + 1)))", "sum", _abs_terms(1.0, 1.0, lambda v: -(v + 1.0))),
    "abs_offset": AbsObjective("MINIMIZE SUM(ABS(c - v)) + 5", "sum", _abs_terms(1.0, 1.0, _minus)),
    "abs_maximized_negated": AbsObjective("MAXIMIZE -SUM(ABS(c - v))", "sum", _abs_terms(1.0, 1.0, _minus)),
    "abs_when": AbsObjective("MINIMIZE when f: SUM(ABS(c - v))", "sum", _abs_terms(1.0, 1.0, _minus, lambda r: r["f"])),
    "abs_filter": AbsObjective(
        "MINIMIZE SUM(when f: ABS(c - v))", "sum", _abs_terms(1.0, 1.0, _minus, lambda r: r["f"])
    ),
    "abs_when_none": AbsObjective(
        "MINIMIZE when v > 100: SUM(ABS(c - v))", "sum", _abs_terms(1.0, 1.0, _minus, lambda r: False)
    ),
    "max": AbsObjective("MINIMIZE MAX(ABS(c - v))", "max", _abs_terms(1.0, 1.0, _minus)),
    "max_reversed": AbsObjective("MINIMIZE MAX(ABS(v - c))", "max", _abs_terms(1.0, -1.0, _plain)),
    "max_norm": AbsObjective("MINIMIZE norm(c - v, 'inf')", "max", _abs_terms(1.0, 1.0, _minus)),
    "max_weight": AbsObjective("MINIMIZE MAX(3 * ABS(c - v))", "max", _abs_terms(3.0, 1.0, _minus)),
    "max_scaled": AbsObjective("MINIMIZE 2 * MAX(ABS(c - v))", "max", _abs_terms(2.0, 1.0, _minus)),
    "max_slope": AbsObjective("MINIMIZE MAX(ABS(2*c - v))", "max", _abs_terms(1.0, 2.0, _minus)),
    "max_offset": AbsObjective("MINIMIZE MAX(ABS(c - v)) + 5", "max", _abs_terms(1.0, 1.0, _minus)),
    "max_when": AbsObjective("MINIMIZE when f: MAX(ABS(c - v))", "max", _abs_terms(1.0, 1.0, _minus, lambda r: r["f"])),
    "max_filter": AbsObjective(
        "MINIMIZE MAX(when f: ABS(c - v))", "max", _abs_terms(1.0, 1.0, _minus, lambda r: r["f"])
    ),
    "max_when_none": AbsObjective(
        "MINIMIZE when v > 100: MAX(ABS(c - v))", "max", _abs_terms(1.0, 1.0, _minus, lambda r: False)
    ),
}


def _abs_terms_of(case_, members):
    return [t for t in (case_.objective.terms(row) for row in members) if t is not None]


def _abs_values(case_, members, center):
    return [w * abs(s * center + t) for w, s, t in _abs_terms_of(case_, members)]


def _solve_abs_with_oracle(oracle_solver, case_):
    """Each class is a variable in its interval; a term is an auxiliary variable above `±(s*c + t)`, summed, or one
    shared auxiliary variable above every term for the maximum."""
    oracle_solver.create_model("direct_a2_absolute")
    objective, shared = {}, None
    if case_.objective.kind == "max":
        oracle_solver.add_variable("z", VarType.CONTINUOUS, 0.0, None)
        objective["z"] = 1.0
        shared = "z"
    for index, rows in enumerate(_classes(case_).values()):
        name = f"c{index}"
        lower, upper = _interval(case_, rows)
        oracle_solver.add_variable(name, VarType.CONTINUOUS, max(lower, -BIG), min(upper, BIG))
        for term_index, (weight, slope, t) in enumerate(_abs_terms_of(case_, rows)):
            if shared is None:
                aux = f"u{index}_{term_index}"
                oracle_solver.add_variable(aux, VarType.CONTINUOUS, 0.0, None)
                objective[aux] = weight
                scale = 1.0
            else:
                aux, scale = shared, weight
            oracle_solver.add_constraint({aux: 1.0, name: -scale * slope}, ">=", scale * t)
            oracle_solver.add_constraint({aux: 1.0, name: scale * slope}, ">=", -scale * t)
    if not objective:
        # No row contributes: the objective is the constant 0.
        oracle_solver.add_variable("unused", VarType.CONTINUOUS, 0.0, 0.0)
        objective["unused"] = 0.0
    oracle_solver.set_objective(objective, ObjSense.MINIMIZE)
    return oracle_solver.solve()


def _abs_objective_at(case_, centers):
    """The objective at one center per class (key -> center): the sum of the terms, or their maximum."""
    values = []
    for key, members in _classes(case_).items():
        values.extend(_abs_values(case_, members, centers[key]))
    if case_.objective.kind == "max":
        return max(values, default=0.0)
    return sum(values)


def _ruled_absolute(case_, members, lower, upper):
    """The point the rulings name for the absolute loss, derived by enumeration: the objective is piecewise linear, so
    its optimal points are a segment whose ends are among the centers and the ends of the interval. Among the
    candidates that reach the least value, the one nearest 0 (the lower of two as near)."""
    candidates = {min(max(0.0, lower), upper), lower, upper}
    for weight, slope, t in _abs_terms_of(case_, members):
        candidates.add(min(max(-t / slope, lower), upper))
    candidates = [c for c in candidates if lower <= c <= upper]
    value = {c: sum(_abs_values(case_, members, c)) for c in candidates}
    best = min(value.values())
    return min((c for c in candidates if value[c] <= best + 1e-9 * max(1.0, abs(best))), key=lambda c: (abs(c), c))


def _ruled_midpoint(case_, members, lower, upper):
    centers = [-t / slope for weight, slope, t in _abs_terms_of(case_, members)]
    if not centers:
        return min(max(0.0, lower), upper)
    return min(max(min(centers) / 2.0 + max(centers) / 2.0, lower), upper)


def _check_abs_assignment(case_, rows, columns, oracle, mode):
    assert columns == ["id", "c"], mode
    assert [row[0] for row in rows] == [row["id"] for row in case_.rows], mode
    classes = _classes(case_)
    in_class = {id(row): key for key, members in classes.items() for row in members}
    centers = {}
    for source_row, row in zip(case_.rows, rows):
        key = in_class.get(id(source_row))
        if key is None:
            assert row[1] is None, (mode, "a row with no class shows NULL", row)
            continue
        assert isinstance(row[1], float), (mode, rows)
        assert centers.setdefault(key, row[1]) == row[1], (mode, "every row of a class carries its one center", key)
    ruled = _ruled_midpoint if case_.objective.kind == "max" else _ruled_absolute
    for key, members in classes.items():
        lower, upper = _interval(case_, members)
        assert lower - 1e-9 <= centers[key] <= upper + 1e-9, (mode, centers[key], lower, upper)
        _check_ruled_point(mode, centers[key], ruled(case_, members, lower, upper), key)
    total = _abs_objective_at(case_, centers)
    expected = oracle.objective_value
    assert abs(total - expected) <= 1e-5 * max(1.0, abs(expected)), (mode, total, expected)


def _agree_abs(decidb_cli, oracle_solver, case_, wrap="plain"):
    sql = _sql(case_, wrap)
    oracle = _solve_abs_with_oracle(oracle_solver, case_)

    def check_assignment(rows, columns, mode):
        _check_abs_assignment(case_, rows, columns, oracle, mode)

    agree_with_oracle(decidb_cli, sql, oracle, check_assignment, ("off", "require"))


def _abs_case(box, objective, rows=ROWS, keys=(), relation=False):
    return Case(box if isinstance(box, Box) else BOXES[box], ABS_OBJECTIVES[objective], rows, keys, relation)


# An odd count (the median is a value), an even count (every point between the middle two is optimal, and the answer is
# the lower median above 0 and the upper one below 0), ties, a flat optimum that holds 0, NULL values and no value at
# all.
EVEN_ROWS = (_row(1, 2.0, True), _row(2, 4.0, False), _row(3, 9.0, True), _row(4, -1.5, False))
TIED_ROWS = (_row(1, 3.0, True), _row(2, 3.0, False), _row(3, 3.0, True), _row(4, 7.0, True), _row(5, 7.0, False))
EVEN_BELOW_ZERO_ROWS = (_row(1, -9.0, True), _row(2, -4.0, False), _row(3, -2.0, True), _row(4, 1.5, False))
FLAT_AROUND_ZERO_ROWS = (_row(1, -3.0, True), _row(2, 5.0, False))

ABS_CASES = {}
# The median of ROWS is 4 and the midpoint of its extreme centers is 3.75; the clamp is up, down or none.
for _objective in ("abs", "max"):
    ABS_CASES[f"{_objective}_odd_in_interior"] = _abs_case("interior", _objective)
    ABS_CASES[f"{_objective}_odd_in_clamped_up"] = _abs_case("clamped_up", _objective)
    ABS_CASES[f"{_objective}_odd_in_clamped_down"] = _abs_case("clamped_down", _objective)
# The answer is the point of the optimal medians nearest 0: the lower median (2, of 2 and 4), the upper median (-2, of
# -4 and -2) and 0 between a negative and a positive median. The interval holds each answer.
ABS_CASES["abs_even_in_interior"] = _abs_case("interior", "abs", EVEN_ROWS)
ABS_CASES["abs_even_below_zero_in_negative_lower_replaces_default"] = _abs_case(
    "negative_lower_replaces_default", "abs", EVEN_BELOW_ZERO_ROWS
)
ABS_CASES["abs_flat_around_zero_in_negative_lower_replaces_default"] = _abs_case(
    "negative_lower_replaces_default", "abs", FLAT_AROUND_ZERO_ROWS
)
ABS_CASES["abs_ties_in_interior"] = _abs_case("interior", "abs", TIED_ROWS)
# A NULL value has no center, and a row with no center weighs nothing.
for _objective in ("abs", "max"):
    ABS_CASES[f"{_objective}_null_values_in_interior"] = _abs_case("interior", _objective, NULL_ROWS)
    # No row has a center: the answer is the point of the interval nearest 0 (0 inside it, its lower end above 0, its
    # upper end below 0).
    ABS_CASES[f"{_objective}_every_value_null_in_interior"] = _abs_case("interior", _objective, ALL_NULL_ROWS)
    for _box in ("interior", "clamped_up", "negative_interval"):
        ABS_CASES[f"{_objective}_when_none_in_{_box}"] = _abs_case(_box, f"{_objective}_when_none")
# Every other spelling of the objective once. The `when`s need the rows that `f` keeps.
for _name in ABS_OBJECTIVES:
    if _name not in ("abs", "max") and not _name.endswith("when_none"):
        ABS_CASES[f"spelling_{_name}"] = _abs_case("interior", _name, ROWS)
# A NaN and an infinity on rows a `when` drops are never read, whether the `when` is the objective's or the
# reducer's own.
for _name in ("abs_when", "max_when", "abs_filter", "max_filter"):
    ABS_CASES[f"{_name}_bad_values_on_dropped_rows"] = _abs_case("interior", _name, DROPPED_BAD_ROWS)

# A decision per class of a key: classes of an odd and an even count, a row with no class, a key of two columns, the
# whole row as the key, a class with no kept row, and classes whose answers fall inside, above and below the interval.
ABS_KEYED_ROWS = KEYED_ROWS + (_krow(7, "c", 2, 8.0, True), _krow(8, "c", 1, -4.0, False))
ABS_KEYED_CASES = {}
for _objective in ("abs", "max"):
    ABS_KEYED_CASES[f"{_objective}_several_keys_in_interior"] = _abs_case(
        "interior", _objective, ABS_KEYED_ROWS, ("k",)
    )
    ABS_KEYED_CASES[f"{_objective}_null_key_in_interior"] = _abs_case("interior", _objective, KEYED_NULL_ROWS, ("k",))
    ABS_KEYED_CASES[f"{_objective}_class_with_every_value_null"] = _abs_case(
        "interior", _objective, KEYED_ALL_NULL_ROWS, ("k",)
    )
    ABS_KEYED_CASES[f"{_objective}_when_class_with_no_kept_row"] = _abs_case(
        "interior", f"{_objective}_when", KEYED_NO_KEPT_ROWS, ("k",)
    )
    ABS_KEYED_CASES[f"{_objective}_answers_clamped_per_class"] = _abs_case(
        KEYED_SPLIT_BOX, _objective, ABS_KEYED_ROWS, ("k",)
    )
ABS_KEYED_CASES["abs_bad_values_on_dropped_rows"] = _abs_case("interior", "abs_when", KEYED_DROPPED_BAD_ROWS, ("k",))

# Bounds read from the data: the ungrouped aggregate query-wide and the grouped one per class, under a `when`.
ABS_DATA_CASES = {}
for _objective in ("abs", "max"):
    ABS_DATA_CASES[f"{_objective}_keyed_both_under_when"] = Case(
        DATA_BOXES["both_under_when"], ABS_OBJECTIVES[_objective], TIGHT_D_ROWS, ("k",)
    )
    ABS_DATA_CASES[f"{_objective}_query_wide_both_under_when"] = Case(
        DATA_BOXES["both_under_when"], ABS_OBJECTIVES[_objective], TIGHT_ROWS
    )


@pytest.mark.var_real
@pytest.mark.cons_perrow
@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(ABS_CASES))
def test_a2_absolute_oracle_solver_and_direct_agree(decidb_cli, oracle_solver, name):
    _agree_abs(decidb_cli, oracle_solver, ABS_CASES[name])


@pytest.mark.var_real
@pytest.mark.cons_perrow
@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(ABS_KEYED_CASES))
def test_a2_absolute_keyed_oracle_solver_and_direct_agree(decidb_cli, oracle_solver, name):
    _agree_abs(decidb_cli, oracle_solver, ABS_KEYED_CASES[name])


@pytest.mark.var_real
@pytest.mark.cons_perrow
@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(ABS_DATA_CASES))
def test_a2_absolute_data_bounds_oracle_solver_and_direct_agree(decidb_cli, oracle_solver, name):
    _agree_abs(decidb_cli, oracle_solver, ABS_DATA_CASES[name])


ABS_CONTEXT_CASES = [
    (base, wrap)
    for base in ("abs_odd_in_clamped_up", "abs_null_key_in_interior")
    for wrap in ("cte_materialized", "wide_source", "parent_join", "recombined_filters")
]


@pytest.mark.correctness
@pytest.mark.parametrize("base,wrap", ABS_CONTEXT_CASES)
def test_a2_absolute_agrees_inside_a_parent_context(decidb_cli, oracle_solver, base, wrap):
    _agree_abs(decidb_cli, oracle_solver, {**ABS_CASES, **ABS_KEYED_CASES}[base], wrap=wrap)


@pytest.mark.error
@pytest.mark.correctness
def test_a2_a_class_weight_past_a_double_raises_too_large(decidb_cli):
    # Each term weighs 1e308, which is a number; the class's total weight does not fit a DOUBLE.
    sql = _problem(ROWS[:3], obj="MINIMIZE SUM(1e308 * ABS(c - v))")
    with pytest.raises(DeciDBCliError, match="MINIMIZE SUM\\(1e308 \\* ABS\\(c - v\\)\\) computes a number too large"):
        _run(decidb_cli, sql, "require")


@pytest.mark.error
@pytest.mark.correctness
@pytest.mark.parametrize("objective", ["MINIMIZE SUM(ABS(c - v - v))", "MINIMIZE MAX(ABS(c - v - v))"])
def test_a2_values_that_sum_past_a_double_raise_too_large_on_both_paths(decidb_cli, objective):
    # Each v is finite; the negated constant of the argument, -v - v, is not. Neither path names a bound or a row.
    sql = _problem(_with(1, v=1e308), obj=objective)
    for mode in ("off", "require"):
        with pytest.raises(DeciDBCliError) as error:
            _run(decidb_cli, sql, mode)
        assert f"DECIDE: {objective} computes a number too large for a DOUBLE. Scale its data down." in str(error.value)


@pytest.mark.var_real
@pytest.mark.correctness
@pytest.mark.parametrize(
    "objective",
    [
        # t = -1e-200 * v is in the normal range: the division holds its precision.
        pytest.param("MINIMIZE SUM(ABS(c - 1e-200 * v))", id="sum-in-range"),
        pytest.param("MINIMIZE MAX(ABS(c - 1e-200 * v))", id="max-in-range"),
    ],
)
def test_a2_centers_that_keep_their_precision_are_silent(decidb_cli, objective):
    assert _run(decidb_cli, _one_row(objective), "require")[0][0][0] == pytest.approx(
        0.3333333333333333e-200, abs=1e-215
    )


@pytest.mark.var_real
@pytest.mark.correctness
@pytest.mark.parametrize(
    "objective",
    [
        # The constant of the form, -3.3e-311, is nonzero and below the normal range, and so is the center it makes.
        pytest.param("MINIMIZE SUM(ABS(c - 1e-310 * v))", id="sum-constant-below-range"),
        pytest.param("MINIMIZE MAX(ABS(c - 1e-310 * v))", id="max-constant-below-range"),
        # The weight 1e300 * 1e-310 of the center is a normal number, but its slope 1e-310 is not.
        pytest.param("MINIMIZE SUM(1e300 * ABS(1e-310 * c - v))", id="sum-slope-below-range"),
        pytest.param("MINIMIZE MAX(ABS(1e-310 * c - v))", id="max-slope-below-range"),
    ],
)
def test_a2_warns_when_a_center_leaves_the_normal_range(decidb_cli, objective):
    _run(decidb_cli, _one_row(objective), "require", expect_number_range=True)


@pytest.mark.correctness
@pytest.mark.parametrize("objective", ["MINIMIZE MAX(ABS(c - v))", "MINIMIZE SUM(ABS(c - v))"])
def test_a2_centers_near_the_double_limit_have_no_overflow(decidb_cli, objective):
    # The sum of the extreme centers passes a DOUBLE, so the midpoint takes each half first; the solver path builds a
    # model from these numbers and warns that its answer may be inaccurate, so only direct solve is compared, with the
    # midpoint (or, for three values, the median) derived here.
    values = (1.7e308, 1.6e308, 1.65e308)
    rows = ", ".join(f"({i}, {v!r})" for i, v in enumerate(values, start=1))
    sql = (
        f"SELECT id, c FROM (FROM (VALUES {rows}) t(id, v) DECIDE per (): c(REAL) "
        f"SUCH THAT c >= -1e30 AND c <= 1e30 {objective}) q ORDER BY id"
    )
    expected = min(values) / 2.0 + max(values) / 2.0 if "MAX" in objective else sorted(values)[1]
    assert expected == pytest.approx(1.65e308, rel=1e-12)
    answers = [row[1] for row in _run(decidb_cli, sql, "require")[0]]
    assert answers == [pytest.approx(expected, rel=1e-12)] * 3


# name -> (sql, error class of direct solve, error class of the solver). Both raise, and a NaN or infinite value reads
# as the value on both ("v is NaN or infinite on some rows"), checked where the `ABS` term is read.
ABS_ERROR_CASES = {
    "nan_value": (_problem(_with(1, v=math.nan), obj="MINIMIZE SUM(ABS(c - v))"), "invalid_value", "invalid_value"),
    "infinite_value": (_problem(_with(1, v=INF), obj="MINIMIZE SUM(ABS(c - v))"), "invalid_value", "invalid_value"),
    "nan_value_in_a_computed_value": (
        _problem(_with(1, v=math.nan), obj="MINIMIZE SUM(ABS(c - 2*v))"),
        "invalid_value",
        "invalid_value",
    ),
    "nan_value_under_a_maximum": (
        _problem(_with(1, v=math.nan), obj="MINIMIZE MAX(ABS(c - v))"),
        "invalid_value",
        "invalid_value",
    ),
    "infinite_value_under_a_maximum": (
        _problem(_with(1, v=INF), obj="MINIMIZE MAX(ABS(c - v))"),
        "invalid_value",
        "invalid_value",
    ),
    "nan_value_of_a_keyed_row": (
        _problem(KEYED_ROWS[:1] + (_krow(9, "a", 1, math.nan, True),), obj="MINIMIZE SUM(ABS(c - v))", key="k"),
        "invalid_value",
        "invalid_value",
    ),
    "empty_interval": (
        _problem(ROWS, "c >= 5 AND c <= 3", "MINIMIZE SUM(ABS(c - v))"),
        "infeasible",
        "infeasible",
    ),
    "empty_interval_under_a_maximum": (
        _problem(ROWS, "c >= 5 AND c <= 3", "MINIMIZE MAX(ABS(c - v))"),
        "infeasible",
        "infeasible",
    ),
    "nan_data_bound": (
        _problem(_with_bound(1, cap=math.nan), "c <= cap", "MINIMIZE SUM(ABS(c - v))"),
        "null_bound",
        "null_bound",
    ),
    "nan_data_bound_under_a_maximum": (
        _problem(_with_bound(1, cap=math.nan), "c <= cap", "MINIMIZE MAX(ABS(c - v))"),
        "null_bound",
        "null_bound",
    ),
}


@pytest.mark.error
@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(ABS_ERROR_CASES))
def test_a2_absolute_error_cases_fail_on_both_paths(decidb_cli, name):
    sql, direct_class, solver_class = ABS_ERROR_CASES[name]
    with pytest.raises(DeciDBCliError) as solver_error:
        _run(decidb_cli, sql, "off")
    with pytest.raises(DeciDBCliError) as direct_error:
        _run(decidb_cli, sql, "require")
    assert "decide_direct_solve=require" not in str(direct_error.value), "no rule applied to the query"
    assert error_class(str(direct_error.value)) == direct_class
    assert error_class(str(solver_error.value)) == solver_class


@pytest.mark.error
@pytest.mark.correctness
def test_a2_a_bad_value_on_the_last_row_raises_under_an_absolute_loss(decidb_cli):
    for objective in ("SUM(ABS(c - v))", "MAX(ABS(c - v))"):
        inner = (
            "SELECT i, c FROM (SELECT i, CASE WHEN i = 4999 THEN 'NaN'::DOUBLE ELSE 1.0 END AS v FROM range(5000) r(i)) "
            f"DECIDE per (): c(REAL) SUCH THAT c BETWEEN 0 AND 10 MINIMIZE {objective}"
        )
        for outer in (f"SELECT COUNT(*) FROM ({inner}) q", f"SELECT i FROM ({inner}) q WHERE i = 0"):
            with pytest.raises(DeciDBCliError, match=r"v is NaN or infinite on some rows"):
                _run(decidb_cli, outer, "require")
