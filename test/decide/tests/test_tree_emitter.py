"""The tree's model emitter (language/guide.md §6.4): under DECIDB_EVALUATOR=tree a query whose every shape has an arm is
evaluated by the tree -- its programs computed over its nodes' entries -- and formulated through flat's code path, and a
query with a shape that has none takes the interim formulator. Each test names the branch it reaches."""

import json
import os
import subprocess

import pytest

ROWS = "(VALUES (1, 'a', 3.0::DOUBLE), (2, 'a', 4.0::DOUBLE), (3, 'b', 5.0::DOUBLE)) t(id, g, c)"
#: Every clause in the per-row shape and no objective: the shapes with an arm.
ROWS_ONLY = f"SELECT id, x FROM {ROWS} DECIDE x(REAL) SUCH THAT x <= t.c AND x + 1 >= 2"
PLAIN = f"SELECT id, x FROM {ROWS} DECIDE x(REAL) SUCH THAT x <= t.c MAXIMIZE sum(x)"
#: A frame read, which has no arm yet.
NO_ARM = f"""SELECT id, x FROM {ROWS} DECIDE x(REAL)
    SUCH THAT x <= t.c AND x <= at(previous else 9: x) over (t.id) MAXIMIZE sum(x)"""


def run(cli, sql, evaluator="tree", **env):
    """The query under `evaluator`, in a process of its own: its stderr is read whole."""
    environment = {**os.environ, **(cli.env or {}), "DECIDB_EVALUATOR": evaluator, **env}
    return subprocess.run(
        [cli.exe, "-c", "SET threads=1; SET decide_direct_solve='off'; " + sql],
        env=environment,
        capture_output=True,
        text=True,
        timeout=120,
    )


def counters(result, name):
    """The values of the DECIDB_PROFILE counter `name` a run printed."""
    values = []
    for line in result.stderr.splitlines():
        if line.startswith("DECIDB_PROFILE: "):
            event = json.loads(line[len("DECIDB_PROFILE: ") :])
            if event.get("event") == "counter" and event.get("name") == name:
                values.append(event["value"])
    return values


def coverage(cli, sql, tmp_path, evaluator="tree"):
    """What DECIDB_TREE_COVERAGE_LOG says of the query: `covered`, or the counter and the reason it has no arm."""
    log = tmp_path / "coverage.log"
    result = run(cli, sql, evaluator, DECIDB_TREE_COVERAGE_LOG=str(log))
    return log.read_text().splitlines(), result


def models(cli, sql, tmp_path):
    """The model flat builds and the one the tree builds."""
    return [
        cli.with_env({"DECIDB_EVALUATOR": evaluator}).dump_model(sql, tmp_path / f"{evaluator}.dump")
        for evaluator in ("flat", "tree")
    ]


def test_a_query_with_an_arm_is_formulated_as_flat_formulates_it(decidb_cli, tmp_path):
    """The covered branch: the tree emits the clauses, so no query takes the interim formulator. Its descent counts
    the rows it read."""
    result = run(decidb_cli, ROWS_ONLY, DECIDB_PROFILE="1")
    assert result.returncode == 0, result.stderr
    assert counters(result, "execution.tree_fallback") == [], result.stderr
    assert counters(result, "execution.descent_rows") == [3], result.stderr
    flat, tree = models(decidb_cli, ROWS_ONLY, tmp_path)
    assert tree == flat


def test_a_query_with_no_arm_takes_the_interim_formulator(decidb_cli):
    """The uncovered branch: a frame read has no arm yet."""
    result = run(decidb_cli, NO_ARM, DECIDB_PROFILE="1")
    assert counters(result, "execution.tree_fallback") == [1], result.stderr
    flat = run(decidb_cli, f"{NO_ARM} ORDER BY id", "flat").stdout
    assert run(decidb_cli, f"{NO_ARM} ORDER BY id").stdout == flat


#: One query per shape with no arm yet, each its query's first: the shape Uncovered names (a closed set that grows only
#: with stage 06), then what the tree computes no value for.
UNCOVERED = {
    "TIGHTEST_BOUND": "x(REAL) SUCH THAT x <= 9 AND sum(x) by (t.g) <= at(previous else 9: t.c) over (t.id within t.g)",
    "EXTREMUM_ROWS": "x(REAL) SUCH THAT x <= 9 AND max(x) by (t.g) >= 2",
    "MEMBER_ROWS": "x(REAL) SUCH THAT x <= 9 AND max(x) by (t.g) <= 2",
    "OBJECTIVE_SQUARES": "x(REAL) SUCH THAT x <= t.c MINIMIZE sum(power(x - t.c, 2))",
    "OBJECTIVE_EXTREMUM": "x(REAL) SUCH THAT x <= t.c MINIMIZE max(x)",
    "OBJECTIVE_COMPOSED": "x(REAL) SUCH THAT x <= t.c MAXIMIZE max(x) + min(x)",
    "COMPOSED": "x(REAL) SUCH THAT x <= 9 AND sum(x) + max(x) <= 10",
    "VALUE_LINKS": "x(REAL) SUCH THAT x <= 9 AND x <= sum(x) by (t.g)",
    "FRAME_READS": "x(REAL) SUCH THAT x <= 9 AND x <= at(previous else 1: x) over (t.id)",
    "PRODUCT_LINKS": "x(REAL), b(BOOL) SUCH THAT x <= 9 AND b * x <= 5",
    # A norm's value is folded from a body of its arguments, which the tree computes no value for yet.
    "a data reducer folds a norm": "x(REAL) SUCH THAT x <= norm(t.c, 1) by (t.g)",
}

#: The shapes that multiply or square decisions in a clause, or multiply them in the objective, which HiGHS refuses
#: before the query reads a row.
GUROBI_UNCOVERED = {
    "PRODUCT_TERMS": "x(REAL), y(REAL) SUCH THAT x <= 9 AND y <= 9 AND t.c * x * y <= 10",
    "SQUARE_TERMS": "x(REAL) SUCH THAT x <= 9 AND power(x - t.c, 2) <= 50",
    "OBJECTIVE_PRODUCTS": "x(REAL), y(REAL) SUCH THAT x <= t.c AND y <= 9 MAXIMIZE sum(x * y)",
}


def _assert_named(cli, shapes, shape, tmp_path):
    lines, result = coverage(cli, f"SELECT id FROM {ROWS} DECIDE {shapes[shape]}", tmp_path)
    assert lines == [f"execution.tree_fallback: {shape}"], result.stderr


@pytest.mark.parametrize("shape", list(UNCOVERED))
def test_a_shape_with_no_arm_is_named(decidb_cli, shape, tmp_path):
    _assert_named(decidb_cli, UNCOVERED, shape, tmp_path)


@pytest.mark.parametrize("shape", list(GUROBI_UNCOVERED))
def test_a_product_or_square_with_no_arm_is_named(decidb_cli_gurobi, shape, tmp_path):
    _assert_named(decidb_cli_gurobi, GUROBI_UNCOVERED, shape, tmp_path)


@pytest.mark.parametrize("evaluator", ["tree", "verify"])
def test_a_covered_query_is_logged_covered_by_its_evaluator(decidb_cli, tmp_path, evaluator):
    lines, result = coverage(decidb_cli, ROWS_ONLY, tmp_path, evaluator)
    assert lines == [f"covered: {evaluator}"], result.stderr


def test_a_null_coefficient_skips_its_row(decidb_cli, tmp_path):
    """A NULL coefficient on a kept row leaves its instance with no value (D67): row 2 has no model row, y's term
    included."""
    sql = """SELECT id, x, y FROM (VALUES (1, 2.0), (2, NULL), (3, 4.0)) t(id, w)
        DECIDE x(REAL), y(REAL) SUCH THAT x <= 10 AND t.w * x + y <= 8"""
    flat, tree = models(decidb_cli, sql, tmp_path)
    assert tree == flat


def test_a_keyed_decision_with_no_class_skips_its_row(decidb_cli, tmp_path):
    """A decision declared `per t.g` has no instance at a row whose g is NULL (D84): the row reads none."""
    sql = """SELECT id, x FROM (VALUES (1, 'a', 3.0), (2, NULL, 4.0), (3, 'b', 5.0)) t(id, g, c)
        DECIDE per t.g: x(INT) SUCH THAT x <= 10 AND x + 0 * t.c <= t.c"""
    flat, tree = models(decidb_cli, sql, tmp_path)
    assert tree == flat


def test_rows_in_no_class_share_one_absent_class(decidb_cli, tmp_path):
    """The second row with a NULL g joins the class the first opened (KeyedMapping::absent_class): x has the
    instances flat gives it, one per class and one for both rows."""
    sql = """SELECT id, x FROM (VALUES (1, 'a', 3.0), (2, NULL, 4.0), (3, NULL, 5.0), (4, 'b', 6.0)) t(id, g, c)
        DECIDE per t.g: x(INT) SUCH THAT x <= 10 AND x + 0 * t.c <= t.c"""
    flat, tree = models(decidb_cli, sql, tmp_path)
    assert tree == flat


#: Each case raises flat's error, which the rank of the tree's errors decides among those it holds.
RAISED = {
    # Clause 0's bound (NaN on row 2) before clause 1's coefficient (NaN on row 1): clauses rank in flat's order.
    "a-clause-before-the-next": (
        "SELECT id, x FROM (VALUES (1, 'NaN'::DOUBLE, 1.0), (2, 1.0, 'NaN'::DOUBLE)) t(id, a, b) "
        "DECIDE x(REAL) SUCH THAT x + 1 <= t.b AND t.a * x <= 9",
        "the bound -1 + t.b is NaN on some rows",
    ),
    # Term 1's NaN on row 10, in the first window of 2,048 rows, before term 0's on row 2,500: a window's every term
    # is read before the next window's.
    "a-window-before-the-next": (
        "SELECT id, x FROM (SELECT i AS id, CASE WHEN i = 2500 THEN 'NaN'::DOUBLE ELSE 1.0 END AS a, "
        "CASE WHEN i = 10 THEN 'NaN'::DOUBLE ELSE 1.0 END AS b FROM range(3000) r(i)) t "
        "DECIDE x(REAL), y(REAL) SUCH THAT t.a * x + t.b * y <= 10",
        "t.b is NaN or infinite on some rows",
    ),
    # Term 1's NaN on row 1,000 before term 0's on row 3,000, although the `when` puts both in the clause's first
    # 2,048 entries: a window is flat's, of the input's rows, which the failing entry's row places.
    "a-window-of-rows-not-of-entries": (
        "SELECT id, x FROM (SELECT i AS id, CASE WHEN i = 3000 THEN 'NaN'::DOUBLE ELSE 1.0 END AS a, "
        "CASE WHEN i = 1000 THEN 'NaN'::DOUBLE ELSE 1.0 END AS b FROM range(5000) r(i)) t "
        "DECIDE x(REAL), y(REAL) SUCH THAT x <= 9 AND when t.id % 2 = 0: t.a * x + t.b * y <= 10",
        "t.b is NaN or infinite on some rows",
    ),
    # Term 0's NaN on row 3 before term 1's on row 2: in a window, the terms are read in their order.
    "a-term-before-the-next": (
        "SELECT id, x FROM (VALUES (1, 1.0, 1.0), (2, 1.0, 'NaN'::DOUBLE), (3, 'NaN'::DOUBLE, 1.0)) t(id, a, b) "
        "DECIDE x(REAL), y(REAL) SUCH THAT t.a * x + t.b * y <= 10",
        "t.a is NaN or infinite on some rows",
    ),
    # Term 1's logarithm of 0 on row 3 before term 0's NaN on row 2: a window computes every term before it reads one.
    "an-execution-before-a-value": (
        "SELECT id, x FROM (VALUES (1, 1.0, 1.0), (2, 'NaN'::DOUBLE, 1.0), (3, 1.0, 0.0)) t(id, a, w) "
        "DECIDE x(REAL), y(REAL) SUCH THAT t.a * x + ln(t.w) * y <= 10",
        "cannot take logarithm of zero",
    ),
    # The coefficient's NaN on row 2 before the bound's on row 1: every term is read before the bound.
    "a-term-before-its-bound": (
        "SELECT id, x FROM (VALUES (1, 1.0, 'NaN'::DOUBLE), (2, 'NaN'::DOUBLE, 1.0)) t(id, a, b) "
        "DECIDE x(REAL) SUCH THAT t.a * x <= t.b",
        "t.a is NaN or infinite on some rows",
    ),
    # The clause's `when` (row 2) before its coefficient (row 1): what routes the rows ranks before what they read.
    "a-when-before-its-terms": (
        "SELECT id, x FROM (VALUES (1, 4.0, 0.0), (2, -1.0, 2.0)) t(id, c, w) "
        "DECIDE x(REAL) SUCH THAT x <= 9 AND when ln(t.c) > 0: ln(t.w) * x <= 100",
        "cannot take logarithm of a negative number",
    ),
    # The first entry that raises, row 2's zero before row 3's negative: a window that raises is read entry by entry.
    "the-first-failing-entry": (
        "SELECT id, x FROM (VALUES (1, 2.0), (2, 0.0), (3, -1.0)) t(id, w) "
        "DECIDE x(REAL) SUCH THAT x <= 9 AND ln(t.w) * x <= 100",
        "cannot take logarithm of zero",
    ),
    # The clause's `when` (row 2,500, in the second window) before a reducer's member test (row 10, in the first): the
    # clause's `when` routes its rows, so it ranks before every filter, whatever the windows (DecideNode::filter_group
    # is a reducer's alone).
    "a-clauses-when-before-a-reducers-filter": (
        "SELECT id, x FROM (SELECT range AS id, range % 2 AS g, CASE WHEN range = 2500 THEN -1.0 ELSE 2.0 END AS a, "
        "CASE WHEN range = 10 THEN -1.0 ELSE 2.0 END AS w FROM range(3000)) t "
        "DECIDE x(REAL), per t.g: y(REAL) SUCH THAT x <= 1 AND y <= 1 AND "
        "when sqrt(t.a) > -1: sum(ln(t.w) * x) by (t.g) + 2 * y <= 5 MAXIMIZE sum(x)",
        "cannot take square root of a negative number",
    ),
    # A coefficient DeciDB computed past a DOUBLE from finite values names its clause.
    "a-computed-coefficient-past-a-double": (
        "SELECT id, x FROM (VALUES (1, 1e308, 1e308)) t(id, a, b) DECIDE x(REAL) SUCH THAT t.a * x + t.b * x <= 5",
        "t.a * x + t.b * x <= 5 computes a number too large for a DOUBLE",
    ),
    # A coefficient DeciDB computed (t.a + t.b) names the value it is computed from that is not finite there.
    "a-computed-coefficients-input": (
        "SELECT id, x FROM (VALUES (1, 2.0, 1.0), (2, 1.0, 'NaN'::DOUBLE)) t(id, a, b) "
        "DECIDE x(REAL) SUCH THAT t.a * x + t.b * x <= 5",
        "t.b is NaN or infinite on some rows",
    ),
}


@pytest.mark.parametrize("case", list(RAISED))
def test_the_tree_raises_flats_error(decidb_cli, case):
    sql, message = RAISED[case]
    flat = run(decidb_cli, sql, "flat")
    tree = run(decidb_cli, sql)
    assert message in flat.stderr, flat.stderr
    assert tree.stderr == flat.stderr


#: Clause 0 holds no value on any row, which flat reports once its clauses are read, unless one of them raised.
NOTHING = (
    "SELECT id, x FROM (VALUES (1, NULL::DOUBLE, 1.0), (2, NULL, {d})) t(id, n, d) "
    "DECIDE x(REAL) SUCH THAT x + 2 <= t.n AND x + 1 <= t.d"
)


@pytest.mark.parametrize("d,warned", [("2.0", True), ("'NaN'::DOUBLE", False)], ids=["no-error", "a-raised-error"])
def test_skipped_instances_are_reported_unless_a_clause_raised(decidb_cli, d, warned):
    result = run(decidb_cli, NOTHING.format(d=d))
    assert ("constrains nothing" in result.stderr) == warned, result.stderr
    assert result.stderr == run(decidb_cli, NOTHING.format(d=d), "flat").stderr


def test_a_folded_bounds_repair_rows_are_emitted_under_diagnose(decidb_cli):
    """`x >= 4` and `x <= 2` are folded into x's box; only the repair model has their rows."""
    sql = """DIAGNOSE SELECT id, x FROM (VALUES (1, 3.0), (2, 4.0)) t(id, c)
        DECIDE x(INT), y(INT) SUCH THAT x >= 4 AND x <= 2 AND x + y <= 20"""
    flat = run(decidb_cli, sql, "flat")
    assert "x >= 4" in flat.stdout, flat.stdout
    assert run(decidb_cli, sql).stdout == flat.stdout


@pytest.mark.parametrize("c", ["5.0", "NULL"], ids=["a-bound-on-every-row", "a-null-bound"])
def test_an_admitted_bound_folds_flats_box(decidb_cli, tmp_path, c):
    """`x <= t.c` admits a range of x on each row, which the model builder folds into x's column; a NULL bound leaves
    its row with no value."""
    sql = f"SELECT id, x FROM (VALUES (1, 3.0), (2, {c}), (3, 4.0)) t(id, c) DECIDE x(REAL) SUCH THAT x <= t.c AND x + 1 >= 2"
    flat, tree = models(decidb_cli, sql, tmp_path)
    assert tree == flat


def test_a_null_in_a_later_window_marks_its_own_entry(decidb_cli, tmp_path):
    """Row 2,500's NULL bound is read in the second window of 2,048 entries, which marks the entry at its offset in the
    node, not in the window: row 2,500 has no value, and row 452 keeps its range."""
    sql = """SELECT id, x FROM (SELECT i AS id, CASE WHEN i = 2500 THEN NULL ELSE 5.0 END AS c FROM range(3000) r(i)) t
        DECIDE x(REAL) SUCH THAT x <= t.c AND x + 1 >= 2"""
    flat, tree = models(decidb_cli, sql, tmp_path)
    assert tree == flat


def test_a_folded_bound_with_no_repair_rows_is_no_clause(decidb_cli, tmp_path):
    """A BOOLEAN's bound is its box, with no rows even for a repair model: its root has no clause to emit."""
    sql = """SELECT id, x, b FROM (VALUES (1, 2.0), (2, 5.0)) t(id, c)
        DECIDE x(REAL), b(BOOL) SUCH THAT x + 1 <= t.c AND b <= 1 AND x + b <= 8"""
    flat, tree = models(decidb_cli, sql, tmp_path)
    assert tree == flat


def test_a_covered_query_prints_each_dump_it_is_asked_for(decidb_cli):
    """The bottom-up values are the interim interpreter's, which runs for its dump alone."""
    dumps = {
        "DECIDB_DUMP_TREE": "1",
        "DECIDB_DUMP_BODIES": "1",
        "DECIDB_DUMP_CONTAINERS": "1",
        "DECIDB_DUMP_VALUES": "1",
    }
    result = run(decidb_cli, ROWS_ONLY, **dumps)
    for dump in ("TREE", "BODIES", "CONTAINERS", "VALUES"):
        assert result.stderr.count(f"DECIDB_{dump}_BEGIN") == 1, result.stderr
    assert " x <= t.c\n  row 0 -> " in result.stderr, result.stderr


def test_a_linear_objective_is_formulated_as_flat_formulates_it(decidb_cli, tmp_path):
    """The objective's arm: each term a column over the members of the G it ranges over."""
    result = run(decidb_cli, PLAIN, DECIDB_PROFILE="1")
    assert counters(result, "execution.tree_fallback") == [], result.stderr
    flat, tree = models(decidb_cli, PLAIN, tmp_path)
    assert tree == flat


#: Rows 2 and 3 have p, so the objective keeps them; c is NULL on row 3, and the groups of g hold 2 rows and 1.
OBJECTIVE_ROWS = (
    "(VALUES (1, 'a', 3.0::DOUBLE, false), (2, 'a', 4.0::DOUBLE, true), (3, 'b', NULL, true)) t(id, g, c, p)"
)

#: Each a model flat builds and the tree builds the same, by the branch it reaches.
OBJECTIVES = {
    # y stands outside every reducer: read on each row the objective keeps, at the entry standing for them, and
    # counted once, on the first of them, row 2 (CountTupleTermsOnce).
    "a-term-outside-every-reducer": "when t.p: sum(t.c * x) + y",
    # AVG divides by the rows its reducer reads, which its member test `c IS NOT NULL` keeps: 1/2, not 1/3.
    "an-avg": "avg(t.c * x)",
    # An AVG of a `by` key divides by its own group's members (D81): 1/2 in group a and 1/1 in group b.
    "an-avg-of-a-group": "sum(per t.g: avg(x) by (t.g))",
    # t.c is a term of no decision, which the objective reads and drops: it is absent nowhere
    # (DecideTreeColumns::Absent).
    "a-constant-term": "sum(x + t.c)",
}


@pytest.mark.parametrize("case", list(OBJECTIVES))
def test_an_objective_reads_the_rows_flat_reads(decidb_cli, tmp_path, case):
    sql = f"""SELECT id, x, y FROM {OBJECTIVE_ROWS}
        DECIDE x(REAL), per (): y(REAL) SUCH THAT x <= 9 AND y <= 4 MAXIMIZE {OBJECTIVES[case]}"""
    result = run(decidb_cli, sql, DECIDB_PROFILE="1")
    assert counters(result, "execution.tree_fallback") == [], result.stderr
    flat, tree = models(decidb_cli, sql, tmp_path)
    assert tree == flat


def test_a_qualified_reducer_reads_one_row_per_class(decidb_cli, tmp_path):
    """`sum(per t.g: y)` reads y once per class of t.g, at its first row: rows 1 and 3, the rows flat's qualifier
    keeps."""
    sql = (
        f"SELECT id, x, y FROM {ROWS} DECIDE x(REAL), per t.g: y(REAL) SUCH THAT x + y <= t.c MAXIMIZE sum(per t.g: y)"
    )
    flat, tree = models(decidb_cli, sql, tmp_path)
    assert tree == flat


def test_a_member_test_keeps_its_terms_from_being_read(decidb_cli, tmp_path):
    """Row 2's `t.v IS NOT NULL` fails, so flat reads no term of the reducer there, and its overflowing coefficient
    raises nothing."""
    sql = """SELECT id, x FROM (VALUES (1, 2::BIGINT, 1::BIGINT, 1.0),
        (2, 4611686018427387904::BIGINT, 4::BIGINT, NULL), (3, 3::BIGINT, 2::BIGINT, 2.0)) t(id, a, b, v)
        DECIDE x(REAL) SUCH THAT x <= 9 MAXIMIZE sum(t.a * t.b * x + t.v * x)"""
    flat, tree = models(decidb_cli, sql, tmp_path)
    assert tree == flat


def test_a_member_test_that_raises_raises_flats_error(decidb_cli):
    """The test `ln(t.w) * t.v IS NOT NULL` raises on row 2, which its reducer reads: flat's message."""
    sql = """SELECT id, x FROM (VALUES (1, 2.0, 1.0), (2, 0.0, NULL), (3, 3.0, 2.0)) t(id, w, v)
        DECIDE x(REAL) SUCH THAT x <= 9 MAXIMIZE sum(ln(t.w) * t.v * x)"""
    flat = run(decidb_cli, sql, "flat")
    assert "cannot take logarithm of zero" in flat.stderr, flat.stderr
    assert run(decidb_cli, sql).stderr == flat.stderr


#: Flat reads the objective's `when` over every row, then each term's filter, its reducer's member tests among them,
#: window by window and in term order, then the coefficients. Each case raises flat's error among a member test's and
#: another read: row 1's `w` is -1 and row 2's `a` is 0.
OBJECTIVE_RAISED = {
    # The objective's `when` (row 2) before a member test (row 1): the rows are routed before any filter is read.
    "a-when-before-a-member-test": "when ln(t.a) > -100: sum(ln(t.w) * t.v * x)",
    # Term 0's member test (row 2) before term 1's (row 1): a window reads the filters in term order.
    "a-member-test-before-the-next-terms": "sum(ln(t.a) * t.v * x) + sum(ln(t.w) * t.v * x)",
}


@pytest.mark.parametrize("case", list(OBJECTIVE_RAISED))
def test_a_member_test_raises_in_flats_order(decidb_cli, tmp_path, case):
    sql = f"""SELECT id, x FROM (VALUES (1, 2.0, -1.0, 1.0), (2, 0.0, 2.0, 1.0)) t(id, a, w, v)
        DECIDE x(REAL) SUCH THAT x <= 1 MAXIMIZE {OBJECTIVE_RAISED[case]}"""
    flat = run(decidb_cli, sql, "flat")
    assert "cannot take logarithm of zero" in flat.stderr, flat.stderr
    lines, tree = coverage(decidb_cli, sql, tmp_path)
    assert lines == ["covered: tree"], tree.stderr
    assert tree.stderr == flat.stderr


#: Row 2's g is NULL, so it reads no instance of y, declared per t.g. The objective keeps row 2 alone, so y, outside
#: every reducer, is read on no row, and flat drops its term (D84).
ABSENT = {
    # y's term is read on no entry (DecideTreeColumns::ReadEntries).
    "a-term-with-no-instance": "3.0::DOUBLE",
    # Nor is its coefficient read there, where it is NaN: Numbers reads only the entries the term is read on.
    "a-coefficient-it-never-reads": "'NaN'::DOUBLE",
}


@pytest.mark.parametrize("case", list(ABSENT))
def test_a_term_reading_no_instance_is_dropped(decidb_cli, tmp_path, case):
    sql = f"""SELECT id, x, y FROM (VALUES (1, 'a', 3.0::DOUBLE), (2, NULL, {ABSENT[case]})) t(id, g, c)
        DECIDE x(REAL), per t.g: y(REAL) SUCH THAT x <= 1 AND y <= 2 MAXIMIZE when t.id = 2: sum(x) + t.c * y"""
    lines, result = coverage(decidb_cli, sql, tmp_path)
    assert lines == ["covered: tree"], result.stderr
    flat, tree = models(decidb_cli, sql, tmp_path)
    assert tree == flat


def test_an_objective_produced_twice_raises_flats_error(decidb_cli):
    """t.c, outside every reducer, takes two values over the kept rows: the objective has two instances (D72)."""
    sql = f"""SELECT id, x FROM {ROWS} DECIDE x(REAL), per (): y(REAL)
        SUCH THAT x <= 9 AND y <= 4 MAXIMIZE sum(x) + t.c * y"""
    flat = run(decidb_cli, sql, "flat")
    assert "the objective would be produced more than once" in flat.stderr, flat.stderr
    assert run(decidb_cli, sql).stderr == flat.stderr


#: Rows for the aggregate shape: group a holds rows 1 and 2, group b rows 3, 5 and 6, and row 4's g is NULL, so it is in
#: no group and has no y. Row 1's k is NULL, so it is in no class of t.k, though it is group a's first row; class v
#: holds rows 3 and 6 of group b, with two values of c.
GROUPED_ROWS = (
    "(VALUES (1, 'a', NULL, 3.0::DOUBLE, true), (2, 'a', 'u', 4.0, false), (3, 'b', 'v', 5.0, true), "
    "(4, NULL, 'v', 6.0, true), (5, 'b', 'u', 7.0, false), (6, 'b', 'v', 8.0, false)) t(id, g, k, c, p)"
)

#: Each a clause in the aggregate shape, whose model the tree builds as flat builds it, by the branch it reaches
#: (DecideModelEmitter::Groups).
GROUPED = {
    # A model row per group of the `by` key, each named by its value (ByReducer, Name).
    "a-group-per-key": "sum(x) by (t.g) <= 5",
    # No key: one group of every kept row.
    "one-group": "sum(x) <= 12",
    # The reducer's own `when` keeps rows 1, 3 and 4, so a group emits the rows it reads (a filter of kept rows).
    "a-reducers-own-when": "sum(when t.p: x) by (t.g) <= 5",
    # The second reducer reads no row of group a (c is 3 and 4 there), so group a has no value and is skipped
    # (SkipGroupsWithAnEmptyReducer).
    "a-group-a-reducer-reads-no-row-of": "sum(x) by (t.g) + sum(when t.c > 4.5: x) by (t.g) <= 8",
    # z, beside the reducers, is read on each kept row and counted once per group, on its first row
    # (ReadBesideReducers, CountTupleTermsOncePerGroup).
    "a-term-beside-the-reducers": "sum(x) by (t.g) + z <= 5",
    # Row 1 is in no class of t.k but is group a's first row: its root holds it, so z counts there, as flat counts it
    # (DecideNode::holds_rows_in_no_class).
    "a-row-in-no-class-of-its-per": "per t.k: sum(x) by (t.g) + z <= 5",
    # AVG divides by the rows its group reads (ScaleAvgTerms).
    "an-avg": "avg(x) by (t.g) <= 2",
    # The bound is read on each kept row (DescribeBound).
    "a-bound-on-each-row": "sum(x) by (t.g) <= t.c",
    # The clause's `when` keeps rows 2 to 6.
    "a-clauses-when": "when t.c > 3.5: sum(x) by (t.g) <= 5",
    # A data reducer's value in each group: a hole of the root's layout, which its NaN check reads as well.
    "a-data-reducer": "sum(x) by (t.g) <= max(t.c) by (t.g)",
    # Each variant of a class of t.k once in its group: class v of group b reads 5 and 8, class u 7, so group b's
    # bound is 20 (DecideTreeColumns::FirstOfEachInstance).
    "a-data-reducer-per-class": "x <= 30 AND x <= sum(per t.k: t.c) by (t.g)",
    # Row 4 reaches no group of the data reducer, its g NULL: it reads NULL, so it has no bound.
    "a-data-reducer-a-row-reaches-no-group-of": "x <= sum(t.c) by (t.g)",
}


@pytest.mark.parametrize("case", list(GROUPED))
def test_a_grouped_clause_is_formulated_as_flat_formulates_it(decidb_cli, tmp_path, case):
    sql = f"""SELECT id, x FROM {GROUPED_ROWS} DECIDE x(REAL), per t.g: y(REAL), per (): z(REAL)
        SUCH THAT x <= 9 AND y <= 9 AND z <= 9 AND {GROUPED[case]} MAXIMIZE sum(x) + sum(y) + z"""
    lines, result = coverage(decidb_cli, sql, tmp_path)
    assert lines == ["covered: tree"], result.stderr
    flat, tree = models(decidb_cli, sql, tmp_path)
    assert tree == flat


def test_a_clause_with_no_class_of_its_per_holds_no_instance(decidb_cli, tmp_path):
    """Every k is NULL, so `per t.k` names no class: the clause holds no instance."""
    sql = """SELECT id, x FROM (VALUES (1, 'a', NULL::VARCHAR), (2, 'b', NULL)) t(id, g, k)
        DECIDE x(REAL) SUCH THAT x <= 9 AND per t.k: sum(x) by (t.g) <= 5 MAXIMIZE sum(x)"""
    flat, tree = models(decidb_cli, sql, tmp_path)
    assert tree == flat


def test_an_emptied_group_is_counted_as_flat_counts_it(decidb_cli):
    """No row has c above 100, so each group's reducer reads none: each of the clause's 2 instances has no value (D90),
    which the tree counts as flat does."""
    sql = f"""SELECT id, x FROM {GROUPED_ROWS} DECIDE x(REAL)
        SUCH THAT x <= 9 AND sum(when t.c > 100: x) by (t.g) <= 5 MAXIMIZE sum(x)"""
    flat = run(decidb_cli, sql, "flat")
    assert "each of its 2 instances has no value" in flat.stderr, flat.stderr
    assert run(decidb_cli, sql).stderr == flat.stderr


#: Rows 1 and 2 share a NOCASE class of g, spelled 'a' and 'A', which the reducer's member test reads apart.
NOCASE_ROWS = (
    "(SELECT id, g COLLATE NOCASE AS g, p FROM (VALUES (1, 'a', 5.0), (2, 'A', 4.0), (3, 'b', 3.0)) v(id, g, p)) t"
)

#: A relation-qualified reducer reads each class of its qualifier once, at its first row, however many entries its
#: spellings make of the class (FirstOfEachInstance): in a clause, and in the objective.
QUALIFIED = {
    "in-a-clause": "y <= 10 AND sum(per t.g: y) <= 12 MAXIMIZE sum(t.p * y)",
    "in-the-objective": "y <= 10 MAXIMIZE sum(per t.g: y)",
}


@pytest.mark.parametrize("case", list(QUALIFIED))
def test_a_qualified_reducer_reads_each_class_once(decidb_cli, tmp_path, case):
    sql = f"SELECT id, y FROM {NOCASE_ROWS} DECIDE per t.g: y(INT) SUCH THAT {QUALIFIED[case]}"
    lines, result = coverage(decidb_cli, sql, tmp_path)
    assert lines == ["covered: tree"], result.stderr
    flat, tree = models(decidb_cli, sql, tmp_path)
    assert tree == flat


def test_a_qualified_avg_divides_by_its_classes(decidb_cli, tmp_path):
    """The join repeats t's rows, and `avg(t: x)` reads each of them once in its group: its filter is the
    de-duplication, so AVG divides by the classes it keeps, as flat's qualifier does (ReadInReducer's has_filter)."""
    sql = """SELECT t.id, s.k, x FROM (VALUES (1, 'a', 3.0, true), (2, 'a', 4.0, false), (3, 'b', 5.0, true),
        (4, NULL, 6.0, true)) t(id, g, c, p) JOIN (VALUES (1, 'u'), (1, 'v'), (2, 'u'), (3, 'u'), (3, 'v'), (3, 'w'),
        (4, 'u')) s(id, k) ON t.id = s.id
        DECIDE per t.id: x(REAL) SUCH THAT x <= 9 AND avg(t: x) by (t.g) <= 2 MAXIMIZE sum(t: x)"""
    lines, result = coverage(decidb_cli, sql, tmp_path)
    assert lines == ["covered: tree"], result.stderr
    flat, tree = models(decidb_cli, sql, tmp_path)
    assert tree == flat


def test_a_data_check_raises_flats_message_without_side_plans(decidb_cli):
    """Under tree-noside no side plan checks the data reducer's values: its hidden root does, at the check's rank."""
    sql = """SELECT id, x FROM (VALUES (1, 'a', 2.0::DOUBLE), (2, 'a', 'NaN'::DOUBLE)) t(id, g, c)
        DECIDE x(REAL) SUCH THAT x <= max(t.c) by (t.g) MAXIMIZE sum(x)"""
    flat = run(decidb_cli, sql, "flat")
    assert "the bound t.c is NaN on some rows" in flat.stderr, flat.stderr
    assert run(decidb_cli, sql, "tree-noside").stderr == flat.stderr


def test_a_folds_error_raises_flats_message_without_side_plans(decidb_cli):
    """Group a's sum of two DECIMAL(38) maxima overflows as the side plan's sum does: the fold raises its error at the
    data reducer's rank (DecideTreeColumns::Folds)."""
    sql = """SELECT id, x FROM (VALUES (1, 'a', 99999999999999999999999999999999999999::DECIMAL(38, 0)),
        (2, 'a', 99999999999999999999999999999999999999::DECIMAL(38, 0)), (3, 'b', 5::DECIMAL(38, 0))) t(id, g, c)
        DECIDE x(REAL) SUCH THAT x <= 9 AND x <= sum(t.c) by (t.g) / 1e40 MAXIMIZE sum(x)"""
    flat = run(decidb_cli, sql, "flat")
    assert "Overflow in HUGEINT addition" in flat.stderr, flat.stderr
    assert run(decidb_cli, sql, "tree-noside").stderr == flat.stderr


def test_a_data_check_raises_what_its_window_meets_first(decidb_cli):
    """The check of max(ln(t.w)) reads ln only where the clause's `when` keeps the row, as the side plan's does: its
    window raises the `when`'s error (row 2), which a vectorized evaluation meets before row 1's logarithm."""
    sql = """SELECT id, x FROM (VALUES (1, 'a', 2.0::DOUBLE, -1.0), (2, 'a', -3.0, 2.0), (3, 'b', 3.0, 5.0))
        t(id, g, c, w) DECIDE x(REAL) SUCH THAT x <= 9 AND when sqrt(t.c) > -1: x <= max(ln(t.w)) by (t.g)
        MAXIMIZE sum(x)"""
    flat = run(decidb_cli, sql, "flat")
    assert "cannot take square root of a negative number" in flat.stderr, flat.stderr
    assert run(decidb_cli, sql, "tree-noside").stderr == flat.stderr


#: Flat reads a reducer's own `when` in its terms' filter, window by window and term by term, after the routes and
#: before any coefficient: each case raises flat's error, a `when`'s or a member test's, where the descent reads the
#: `when` (DecideNode::filter_group).
FILTER_RAISED = {
    # Term 0's member test (row 2) before term 1's `when` (row 1).
    "a-test-before-the-next-terms-when": (
        "(VALUES (1, 0.0, 2.0, 1.0), (2, 2.0, -1.0, 1.0)) t(id, a, w, v)",
        "sum(ln(t.w) * t.v * x) + sum(when ln(t.a) > -100: x)",
        "logarithm of a negative number",
    ),
    # Term 0's `when` (row 2) before term 1's (row 1).
    "a-when-before-the-next-terms-when": (
        "(VALUES (1, 2.0, -1.0, 1.0), (2, 0.0, 2.0, 1.0)) t(id, a, w, v)",
        "sum(when ln(t.a) > -100: x) + sum(when ln(t.w) > -100: 2 * x)",
        "logarithm of zero",
    ),
    # A reducer's `when` (row 2) before its own member test (row 1): the filter reads the `when` first.
    "a-when-before-its-member-test": (
        "(VALUES (1, 2.0, -1.0, 1.0), (2, 0.0, 2.0, 1.0)) t(id, a, w, v)",
        "sum(when ln(t.a) > -100: ln(t.w) * t.v * x)",
        "logarithm of zero",
    ),
    # A `when` of two conjuncts reads them in their order: the first's error (row 2) before the second's (row 1).
    "a-conjunction-in-its-order": (
        "(VALUES (1, 2.0, -1.0, 1.0), (2, -1.0, 2.0, 1.0)) t(id, a, w, v)",
        "sum(when sqrt(t.a) > -1 AND ln(t.w) > -100: x)",
        "square root of a negative number",
    ),
    # A member test in the first window (row 10) before the `when` in the second (row 2,500).
    "a-window-before-the-next": (
        "(SELECT range AS id, CASE WHEN range = 2500 THEN 0.0 ELSE 2.0 END AS a, "
        "CASE WHEN range = 10 THEN -1.0 ELSE 2.0 END AS w, 1.0 AS v FROM range(3000)) t",
        "sum(when ln(t.a) > -100: ln(t.w) * t.v * x)",
        "logarithm of a negative number",
    ),
}


@pytest.mark.parametrize("case", list(FILTER_RAISED))
def test_a_reducers_when_raises_in_its_filter(decidb_cli, tmp_path, case):
    rows, objective, message = FILTER_RAISED[case]
    sql = f"SELECT id, x FROM {rows} DECIDE x(REAL) SUCH THAT x <= 1 MAXIMIZE {objective}"
    flat = run(decidb_cli, sql, "flat")
    assert message in flat.stderr, flat.stderr
    lines, tree = coverage(decidb_cli, sql, tmp_path)
    assert lines == ["covered: tree"], tree.stderr
    assert tree.stderr == flat.stderr


def test_a_grouped_clauses_reducers_raise_in_term_order(decidb_cli, tmp_path):
    """In a clause, as in the objective: term 0's member test (row 2) before term 1's `when` (row 1)."""
    sql = """SELECT id, x FROM (VALUES (1, 'a', 0.0, 2.0, 1.0), (2, 'a', 2.0, -1.0, 1.0)) t(id, g, a, w, v)
        DECIDE x(REAL) SUCH THAT x <= 1 AND sum(ln(t.w) * t.v * x) by (t.g) + sum(when ln(t.a) > -100: x) by (t.g) <= 5
        MAXIMIZE sum(x)"""
    flat = run(decidb_cli, sql, "flat")
    assert "logarithm of a negative number" in flat.stderr, flat.stderr
    lines, tree = coverage(decidb_cli, sql, tmp_path)
    assert lines == ["covered: tree"], tree.stderr
    assert tree.stderr == flat.stderr


#: Flat groups a clause's kept rows by its `by` key after it computes the terms, so a key's error ranks after theirs:
#: under each evaluator that reads the clause, each case raises flat's error (DecideNode::key_block).
KEY_RAISED = {
    # A reducer's own `when` (row 1) before the key (row 2).
    "a-reducers-when-before-its-by-key": (
        "(VALUES (1, 2.0, -1.0), (2, -1.0, 2.0), (3, 3.0, 5.0)) t(id, c, w)",
        "sum(when ln(t.w) > -100: x) by (sqrt(t.c)) <= 8",
        "logarithm of a negative number",
    ),
    # A coefficient (row 1) before the key (row 2).
    "a-coefficient-before-the-by-key": (
        "(VALUES (1, 2.0, -1.0), (2, -1.0, 2.0), (3, 3.0, 5.0)) t(id, c, w)",
        "sum(ln(t.w) * x) by (sqrt(t.c)) <= 8",
        "logarithm of a negative number",
    ),
    # The key raises on row 2, which still reaches the terms, whose coefficient raises there first.
    "a-coefficient-on-a-row-its-key-raises-on": (
        "(VALUES (1, 2.0), (2, -1.0), (3, 3.0)) t(id, c)",
        "sum(ln(t.c) * x) by (sqrt(t.c)) <= 8",
        "logarithm of a negative number",
    ),
    # A key's elements in their order, the logarithm's (row 2) before the square root's (row 1).
    "a-keys-elements-in-their-order": (
        "(VALUES (1, -1.0, 2.0), (2, 2.0, -1.0), (3, 3.0, 5.0)) t(id, c, w)",
        "sum(x) by (sqrt(t.c), ln(t.w)) <= 8",
        "logarithm of a negative number",
    ),
}


@pytest.mark.parametrize("evaluator", ["tree", "verify"])
@pytest.mark.parametrize("case", list(KEY_RAISED))
def test_a_by_key_raises_after_the_terms(decidb_cli, tmp_path, case, evaluator):
    rows, clause, message = KEY_RAISED[case]
    sql = f"SELECT id, x FROM {rows} DECIDE x(REAL) SUCH THAT x <= 9 AND {clause} MAXIMIZE sum(x)"
    flat = run(decidb_cli, sql, "flat")
    assert message in flat.stderr, flat.stderr
    lines, result = coverage(decidb_cli, sql, tmp_path, evaluator)
    assert lines == [f"covered: {evaluator}"], result.stderr
    assert result.stderr == flat.stderr


@pytest.mark.parametrize("evaluator", ["tree", "verify"])
def test_a_row_in_no_group_reaches_the_operand(decidb_cli, tmp_path, evaluator):
    """Row 2's g is NULL, so it is in no group of the G: it still reaches the G's operand, which flat computes on every
    row the G reads, so its NaN coefficient raises flat's message (DecideDescent::Visit)."""
    sql = """SELECT id, x FROM (VALUES (1, 'a', 3.0::DOUBLE), (2, NULL, 'NaN'::DOUBLE), (3, 'b', 5.0)) t(id, g, c)
        DECIDE x(REAL) SUCH THAT x <= 9 AND sum(t.c * x) by (t.g) <= 8 MAXIMIZE sum(x)"""
    flat = run(decidb_cli, sql, "flat")
    assert "t.c is NaN or infinite on some rows" in flat.stderr, flat.stderr
    lines, result = coverage(decidb_cli, sql, tmp_path, evaluator)
    assert lines == [f"covered: {evaluator}"], result.stderr
    assert result.stderr == flat.stderr


@pytest.mark.parametrize("evaluator", ["tree", "verify"])
def test_the_objective_reads_no_term_at_a_row_in_no_group(decidb_cli, tmp_path, evaluator):
    """Flat reads the objective's sum of group sums in place (D81), on the rows of each group: row 2's g is NULL, so
    its logarithm of 0 is never computed, and the objective's G does not route the row to its operand."""
    sql = """SELECT id, x FROM (VALUES (1, 'a', 1.0), (2, NULL, 0.0), (3, 'b', 2.0)) t(id, g, w)
        DECIDE x(INT) SUCH THAT x <= 10 AND sum(x) <= 10 MAXIMIZE sum(per t.g: sum(ln(t.w) * x) by (t.g))"""
    lines, result = coverage(decidb_cli, sql, tmp_path, evaluator)
    assert lines == [f"covered: {evaluator}"], result.stderr
    assert result.returncode == 0 and result.stderr == "", result.stderr
    assert result.stdout == run(decidb_cli, sql, "flat").stdout


def test_the_interim_reads_no_value_of_a_g_at_a_row_in_no_group(decidb_cli):
    """A frame read takes the interim formulator. Row 2 is in class u of `per t.k:` but in no group of t.g: it holds no
    entry of the G, so its group's row reads no sum there, and x is free at 9 on it."""
    sql = """SELECT id, x FROM (VALUES (1, 'a', 'u', 1, 3.0), (2, NULL, 'u', 2, 4.0), (3, 'b', 'v', 3, 5.0))
        t(id, g, k, s, c) DECIDE x(REAL) SUCH THAT x <= 9 AND per t.k: sum(x) by (t.g) <= at(previous else 2: t.c)
        over (t.s) MAXIMIZE sum(x) ORDER BY id"""
    result = run(decidb_cli, sql, DECIDB_PROFILE="1")
    assert counters(result, "execution.tree_fallback") == [1], result.stderr
    assert result.stdout == run(decidb_cli, sql, "flat").stdout


def test_the_interim_raises_in_flats_blocks(decidb_cli):
    """A frame read takes the interim formulator, which ranks what it computes in flat's blocks, as the columns do: the
    reducer's member test in its filter and its coefficient among the terms, both after the reducer's own `when` the
    descent reads in the filter. The `when` raises on row 2, the test and the coefficient on row 1."""
    sql = """SELECT id, x FROM (VALUES (1, 'a', -1.0, 2.0), (2, 'a', 2.0, -1.0), (3, 'b', 3.0, 5.0)) t(id, g, c, w)
        DECIDE x(REAL) SUCH THAT x <= at(previous else 9: x) over (t.id) AND x <= 9 AND
        sum(when ln(t.w) > -100: sqrt(t.c) * x) by (t.g) <= 8 MAXIMIZE sum(x)"""
    flat = run(decidb_cli, sql, "flat")
    assert "logarithm of a negative number" in flat.stderr, flat.stderr
    result = run(decidb_cli, sql, DECIDB_PROFILE="1")
    assert counters(result, "execution.tree_fallback") == [1], result.stderr
    assert result.stderr.splitlines()[-1] == flat.stderr.splitlines()[-1]


def test_the_interim_reads_a_grouped_roots_classes_alone(decidb_cli):
    """A frame read takes the interim formulator, whose root of `per t.k:` holds the rows in no class of t.k as well:
    it reads none of them, so with every k NULL the clause holds no instance, and x is free at 9."""
    sql = """SELECT id, x FROM (VALUES (1, 'a', NULL::VARCHAR, 1, 3.0), (2, 'a', NULL, 2, 4.0)) t(id, g, k, s, c)
        DECIDE x(REAL) SUCH THAT x <= 9 AND per t.k: sum(x) by (t.g) <= at(previous: t.c) over (t.s within t.g)
        MAXIMIZE sum(x) ORDER BY id"""
    result = run(decidb_cli, sql, DECIDB_PROFILE="1")
    assert counters(result, "execution.tree_fallback") == [1], result.stderr
    assert result.stdout == run(decidb_cli, sql, "flat").stdout


#: A relation's columns compare NULL as a value: rows 2 and 3 are one group of d, which flat numbers after every other.
RELATION_ROWS = "(VALUES (1, 'a', 2), (2, NULL, 4), (3, NULL, 4), (4, 'b', 1))"


def test_a_null_key_value_is_the_last_group(decidb_cli, tmp_path):
    """The tree's groups are numbered as flat numbers them, NULL after every value, so the model's rows come in flat's
    order."""
    sql = f"""SELECT id, x FROM {RELATION_ROWS} t(id, g, c)
        JOIN (SELECT DISTINCT g AS dg, c AS dc FROM {RELATION_ROWS} s(id, g, c)) d
        ON t.g IS NOT DISTINCT FROM d.dg AND t.c = d.dc
        DECIDE x(REAL) SUCH THAT x <= 9 AND sum(x) by (d) <= t.c MAXIMIZE sum(x)"""
    lines, result = coverage(decidb_cli, sql, tmp_path)
    assert lines == ["covered: tree"], result.stderr
    flat, tree = models(decidb_cli, sql, tmp_path)
    assert tree == flat


def test_a_relations_null_key_class_is_numbered_last(decidb_cli, tmp_path):
    """T's key compares NULL as a value, so the class of T.k NULL is the last class of y's scope, as flat numbers it
    (DecideDescent::Finish): verify finds the tree's classes flat's."""
    sql = """SELECT T.k, y FROM (VALUES (1, NULL), (2, 4.0), (NULL, 5.0)) T(k, v)
        JOIN (VALUES (1, 'u'), (1, 'w'), (2, 'u'), (NULL, 'z')) W(k, c) ON T.k IS NOT DISTINCT FROM W.k
        DECIDE per T: y(REAL) SUCH THAT y <= 9 MAXIMIZE sum(T: y)"""
    _verified(decidb_cli, sql, tmp_path)


def _verified(cli, sql, tmp_path):
    """The query under verify, whose second run is the tree's: it raises if what the two hand stage 06 differs, and
    prints what flat prints otherwise."""
    lines, result = coverage(cli, sql, tmp_path, "verify")
    assert lines == ["covered: verify"], result.stderr
    assert result.stderr == run(cli, sql, "flat").stderr


#: A diagnosis of a clause in the aggregate shape reports the classes each group's row stands for
#: (SolverInput::class_sets), which verify compares, by the branch each case reaches (DecideGroupClassSets): x <= 9
#: cannot meet a sum of 100.
GROUP_CLASSES = {
    # With no `per`, a group's row stands for each of its kept rows.
    "kept-rows": "sum(x) by (t.g) >= 100",
    # One group, which holds every kept row.
    "one-group": "sum(x) >= 100",
    # Under `per t.k:`, for the classes of t.k among its kept rows; row 1 is in none, though group a holds it
    # (DecideModelEmitter::KeyClasses).
    "classes-of-its-per": "per t.k: sum(x) by (t.g) >= 100",
    # Under `per ()`, for the whole clause.
    "the-whole-clause": "per (): sum(x) by (t.g) >= 100",
}


@pytest.mark.parametrize("case", list(GROUP_CLASSES))
def test_a_diagnosis_reports_the_classes_a_groups_row_stands_for(decidb_cli, tmp_path, case):
    sql = f"""DIAGNOSE SELECT id, x FROM {GROUPED_ROWS} DECIDE x(REAL) SUCH THAT x <= 9 AND {GROUP_CLASSES[case]}
        MAXIMIZE sum(x)"""
    _verified(decidb_cli, sql, tmp_path)


#: Rows for the per-row shape under `per K:` (DecideClauseScope::ONE_ROW_PER_CLASS): class b's rows come before class
#: a's, so flat's numbering by value is not its numbering by row; class a's rows 2 and 4 read one value of c, class b's
#: rows 1 and 3 two; row 5's g is NULL, so it is in no class.
CLASS_ROWS = "(VALUES (1, 'b', 3.0::DOUBLE), (2, 'a', 4.0), (3, 'b', 5.0), (4, 'a', 4.0), (5, NULL, 1.0)) t(id, g, c)"

#: Each a clause with one model row per class or per variant of a class of its `per K:`, which the tree builds, names,
#: counts and reports as flat does (verify compares them, and the classes a diagnosis reports), by the branch it
#: reaches (DecideModelEmitter::Classes).
CLASSES = {
    # One row per class of t.g, at its first row, named by its class (KeyClasses).
    "a-row-per-class": "per t.g: z + y <= 5",
    # One row per value of c in a class: class b's two values make 3 variants of 2 classes, numbered by value, each
    # named by its class and its value (NameVariants) and, under DIAGNOSE, standing for itself; EXPLAIN ANALYZE counts
    # them (RecordVariants).
    "a-row-per-value": "per t.g: z + t.c <= 20",
    # x is per row, so each row is a variant of its own, numbered by its first row (Variants); rows 2 and 4 read one
    # value of c, so each is named by its row too, and by no value of x's instance.
    "a-row-per-decision": "per t.g: x + t.c <= 20",
    # `per ()` has one class with no value, which its variants are named without; under DIAGNOSE they stand for the
    # whole clause.
    "one-class": "per (): z + t.c <= 20",
    # A clause's `when`: its rows do not stand for every row of their class (rows_stand_with_their_class).
    "a-when": "when t.c > 3.5 per t.g: z + y <= 5",
}


@pytest.mark.parametrize("diagnose", ["", "DIAGNOSE "], ids=["solve", "diagnose"])
@pytest.mark.parametrize("case", list(CLASSES))
def test_a_row_per_class_is_formulated_as_flat_formulates_it(decidb_cli, tmp_path, case, diagnose):
    sql = f"""SELECT id, x FROM {CLASS_ROWS} DECIDE x(REAL), per t.g: y(REAL), per (): z(REAL)
        SUCH THAT x <= 9 AND y <= 9 AND z <= 9 AND {CLASSES[case]} MAXIMIZE sum(x) + sum(y) + z"""
    _verified(decidb_cli, diagnose + sql, tmp_path)
    if not diagnose:
        flat, tree = models(decidb_cli, sql, tmp_path)
        assert tree == flat


#: A relation-qualified reducer reads one value per class and value of what its body reads (KeyedScopeInfo::signature):
#: its classes are its operand's variants, numbered as flat numbers them (DecideModelEmitter::SignatureMapping). Row 5's
#: g is NULL, so the reducer reads no value there, which maps it to a class of its own.
SPLIT_CLASSES = {
    # t.c takes two values in class b: 3 values of 2 classes, which EXPLAIN ANALYZE counts. y's instance is a field,
    # so they are numbered by their first rows.
    "by-first-row": "x + y <= t.c MAXIMIZE sum(per t.g: t.c * y)",
    # z is one decision for every row, so they are numbered by value.
    "by-value": "x + z <= t.c MAXIMIZE sum(per t.g: t.c * z)",
}


@pytest.mark.parametrize("case", list(SPLIT_CLASSES))
def test_a_qualified_reducers_classes_are_split_by_its_values(decidb_cli, tmp_path, case):
    sql = f"""SELECT id, x FROM {CLASS_ROWS} DECIDE x(REAL), per t.g: y(REAL), per (): z(REAL)
        SUCH THAT x <= 9 AND y <= 9 AND z <= 9 AND {SPLIT_CLASSES[case]}"""
    _verified(decidb_cli, sql, tmp_path)
    flat, tree = models(decidb_cli, sql, tmp_path)
    assert tree == flat


#: A relation-qualified reducer reads its terms on every row its `when` keeps, and keeps no value of a row in no class
#: of its `per` (ApplyQualifierToFilter): its operand holds such a row in a slot of its own
#: (DecideNode::holds_rows_in_no_class), which the reducer reads nothing of (DecideTreeColumns::FirstOfEachInstance) and
#: no variant holds (DecideModelEmitter::Variants). Row 5's g is NULL, and its c is each case's.
NO_CLASS_ROWS = """(VALUES (1, 'b', 'u', 3.0::DOUBLE), (2, 'a', 'u', 4.0), (3, 'b', 'v', 5.0), (4, 'a', 'u', 4.0),
    (5, NULL, 'z', {c})) t(id, g, h, c)"""

IN_NO_CLASS = {
    # Row 5 stays in the clause's one group.
    "in-its-group": ("1.0", "x <= 9 AND sum(per t.g: t.c * x) <= 20 MAXIMIZE sum(x)"),
    # Row 5 alone makes group z, in which the reducer reads no value, so z has none (D68).
    "a-group-of-its-own": ("1.0", "x <= 9 AND sum(per t.g: x) by (t.h) >= 5 MAXIMIZE sum(x)"),
    # Its coefficient is computed there, so its NaN is refused, in a clause and in the objective.
    "a-nan-in-a-clause": ("'NaN'::DOUBLE", "x <= 9 AND sum(per t.g: t.c * x) <= 20 MAXIMIZE sum(x)"),
    "a-nan-in-the-objective": ("'NaN'::DOUBLE", "x <= 9 MAXIMIZE sum(per t.g: t.c * x)"),
}


@pytest.mark.parametrize("case", list(IN_NO_CLASS))
def test_a_qualified_reducer_reads_its_rows_in_no_class_as_flat_does(decidb_cli, tmp_path, case):
    c, clause = IN_NO_CLASS[case]
    sql = f"SELECT id, x FROM {NO_CLASS_ROWS.format(c=c)} DECIDE x(REAL) SUCH THAT {clause}"
    _verified(decidb_cli, sql, tmp_path)
    assert run(decidb_cli, sql).stderr == run(decidb_cli, sql, "flat").stderr


#: A variant is named by what its class key and its fields read at its first row, as written (DecideBody::names), and
#: a collated column of its class by its least spelling among the clause's rows of the class
#: (DecideDescentGroup::spelling), as flat's reader names it; verify compares the names an expanded diagnosis shows.
NAMED_VARIANTS = {
    # The `by` key t.h is collated: its field is spelled as the variant's first row spells it, 'U', not as it groups.
    "a-by-key-as-written": """SELECT id, y FROM (SELECT id, g, h COLLATE NOCASE AS h, c FROM (VALUES (1, 'a', 'U', 3.0),
        (2, 'a', 'v', 4.0), (3, 'b', 'w', 7.0), (4, 'a', 'u', 2.0)) v(id, g, h, c)) t
        DECIDE per t.g: y(BOOL) SUCH THAT per t.g: y + sum(t.c) by (t.h) <= 4.5 MAXIMIZE sum(y)""",
    # -0.0 and 0.0 are one class: each variant is named by its own first row's g.
    "a-class-at-its-first-row": """SELECT id, y FROM (VALUES (1, -0.0::DOUBLE, 4.0), (2, 0.0::DOUBLE, 1.0),
        (3, 1.0::DOUBLE, 7.0)) t(id, g, c) DECIDE per t.g: y(BOOL) SUCH THAT per t.g: y + t.c <= 0.5""",
    # y's key reads row 1's 'A', which the clause's `when` leaves out: its class is named 'a'.
    "a-collated-class-among-its-rows": """SELECT id, y FROM (SELECT id, g COLLATE NOCASE AS g, c, v FROM (VALUES
        (1, 'A', -1, 1.0), (2, 'a', 1, 2.0), (3, 'a', 1, 3.0), (4, 'b', 1, 1.0)) w(id, g, c, v)) t
        DECIDE per t.g: y(BOOL) SUCH THAT when t.c > 0 per t.g: y + t.v >= 10 MAXIMIZE sum(y)""",
}


@pytest.mark.parametrize("case", list(NAMED_VARIANTS))
def test_a_variant_is_named_as_written_at_its_first_row(decidb_cli, tmp_path, case):
    expanded = "PRAGMA diagnose_decide_infeasible_slack_scope='expanded'; "
    _verified(decidb_cli, f"{expanded}DIAGNOSE {NAMED_VARIANTS[case]}", tmp_path)


def test_a_folded_row_of_one_class_has_no_name(decidb_cli, tmp_path):
    """`2 * z >= 30` is folded into z's box, and only a repair model emits its row: one variant of the one class of
    `per ()`, which has no value to name it by (Classes, under DIAGNOSE alone)."""
    sql = f"""DIAGNOSE SELECT id, x FROM {CLASS_ROWS} DECIDE x(REAL), per (): z(REAL)
        SUCH THAT x <= 9 AND z <= 9 AND per (): 2 * z >= 30 MAXIMIZE sum(x) + z"""
    _verified(decidb_cli, sql, tmp_path)


#: Rows for an auxiliary's absence: row 1's c is NULL; row 4's g is NULL, so y has no instance there.
ABSENT_ROWS = "(VALUES (1, 'a', NULL::DOUBLE), (2, 'a', 3.0), (3, 'b', 5.0), (4, NULL, 2.0)) t(id, g, c)"

#: An auxiliary has no value where what defines it reads none (SolverInput::absent_reads, D88), so a row reading it
#: skips its instance, as flat does; verify compares the absence. By the branch each case reaches
#: (DecideModelEmitter::ReadAbsence).
DEFINED = {
    # Row 1's NULL is in its definition's bound, `a >= x - t.c` read as `a - x >= -t.c`: the clause skips row 1.
    "a-null-bound": "x + abs(x - t.c) <= 20",
    # Row 1's NULL is in its definition's coefficient.
    "a-null-coefficient": "x + abs(t.c * x - 1) <= 20",
    # y has no instance on row 4, which its definition reads.
    "a-decision-with-no-instance": "x + abs(y - 1) <= 20",
    # The outer auxiliary's definition reads the inner one, which has no value on row 1.
    "an-auxiliary-under-another": "x + abs(abs(x - t.c) - 1) <= 20",
}


@pytest.mark.parametrize("case", list(DEFINED))
def test_an_auxiliary_has_no_value_where_its_definition_has_none(decidb_cli, tmp_path, case):
    sql = f"""SELECT id, x FROM {ABSENT_ROWS} DECIDE x(REAL), per t.g: y(REAL)
        SUCH THAT x <= 9 AND y <= 9 AND {DEFINED[case]} MAXIMIZE sum(x) + sum(y)"""
    _verified(decidb_cli, sql, tmp_path)
    flat, tree = models(decidb_cli, sql, tmp_path)
    assert tree == flat


#: Three thousand rows, two windows of 2,048 rows: row 10's d is 0 and row 2,500's c is -1, so ln(t.d) raises in the
#: first window and sqrt(t.c) in the second.
WINDOWS = """(SELECT i, CASE WHEN i = 2500 THEN -1.0 ELSE 1.0 END AS c, CASE WHEN i = 10 THEN 0.0 ELSE 1.0 END AS d
    FROM range(3000) r(i)) t"""

#: An error a definition raises ranks where flat raises it, under tree and under verify, by the branch each case
#: reaches (DecideNode::value_rank, clause_rank).
DEFINITION_ERRORS = {
    # Flat checks a definition's numbers where it evaluates the row among the clauses (clause_rank), after
    # `x <= t.d`'s: row 1's NaN bound raises, not row 2's NaN in |x - t.c|.
    "its-numbers-at-its-place": """(VALUES (1, 1.0, 'NaN'::DOUBLE), (2, 'NaN'::DOUBLE, 1.0)) t(i, c, d)
        DECIDE x(REAL) SUCH THAT x <= t.d AND x + abs(x - t.c) <= 20""",
    # And after the clause before it reads ln(0): the clause's error raises, not the NaN in |t.c * x - 1|.
    "its-numbers-after-an-earlier-clause": """(VALUES (1, 'NaN'::DOUBLE, 0.0), (2, 1.0, 1.0)) t(i, c, d)
        DECIDE x(REAL) SUCH THAT x <= ln(t.d) + 9 AND x + abs(t.c * x - 1) <= 20""",
    # Flat computes a definition first where it resolves the auxiliary's absence, before any clause
    # (DecideTreeColumns::NullEntries): sqrt(-1) raises, though `x <= ln(t.d) + 9` comes first.
    "its-execution-before-every-clause": """(VALUES (1, 1.0, 0.0), (2, -1.0, 1.0)) t(i, c, d)
        DECIDE x(REAL) SUCH THAT x <= ln(t.d) + 9 AND x + abs(x - sqrt(t.c)) <= 20""",
    # It computes a definition's bound and its coefficients together, the bound first: ln(0) on the same row.
    "its-bound-before-its-coefficient": """(VALUES (1, -1.0, 0.0), (2, 1.0, 1.0)) t(i, c, d)
        DECIDE x(REAL) SUCH THAT x <= 9 AND x + abs(sqrt(t.c) * x - ln(t.d)) <= 20""",
    # A window at a time: ln(0) in the first window, though the coefficient comes before the bound in the clause.
    "its-windows-in-order": f"""{WINDOWS}
        DECIDE x(REAL) SUCH THAT x <= 9 AND x + abs(sqrt(t.c) * x - ln(t.d)) <= 20""",
    # Auxiliary after auxiliary (DecideErrorRank::step): the inner |x - sqrt(t.c)|'s, though the outer's ln(t.d) is in
    # the first window.
    "an-auxiliary-before-the-next": f"""{WINDOWS}
        DECIDE x(REAL) SUCH THAT x <= 9 AND x + abs(abs(x - sqrt(t.c)) - ln(t.d)) <= 20""",
}


@pytest.mark.parametrize("case", list(DEFINITION_ERRORS))
def test_a_definitions_error_ranks_where_flat_raises_it(decidb_cli, tmp_path, case):
    sql = f"SELECT i, x FROM {DEFINITION_ERRORS[case]} MAXIMIZE sum(x)"
    flat = run(decidb_cli, sql, "flat").stderr
    assert "Error" in flat
    for evaluator in ("tree", "verify"):
        folder = tmp_path / evaluator
        folder.mkdir()
        lines, result = coverage(decidb_cli, sql, folder, evaluator)
        assert lines == [f"covered: {evaluator}"], result.stderr
        assert result.stderr == flat


def test_the_objective_drops_an_auxiliary_with_no_value(decidb_cli, tmp_path):
    """Row 1's |x - t.c| has no value, so the objective drops its term there (D84; DecideTreeColumns::ReadEntries)."""
    sql = f"SELECT id, x FROM {ABSENT_ROWS} DECIDE x(REAL) SUCH THAT x <= 9 AND x >= 1 MINIMIZE sum(abs(x - t.c))"
    _verified(decidb_cli, sql, tmp_path)
    flat, tree = models(decidb_cli, sql, tmp_path)
    assert tree == flat


def test_a_not_equal_answers_through_its_implied_bounds(decidb_cli, tmp_path):
    """The sum bounds each x, which stage 06 reads into the `<>` it expands (EvaluatedConstraint::ne_clause_idx): the
    tree's rows are flat's, so it answers as flat does."""
    sql = f"SELECT id, x FROM {GROUPED_ROWS} DECIDE x(INT) SUCH THAT x >= 0 AND sum(x) <= 10 AND x <> 3 MAXIMIZE sum(x)"
    lines, result = coverage(decidb_cli, sql, tmp_path)
    assert lines == ["covered: tree"], result.stderr
    flat, tree = models(decidb_cli, sql, tmp_path)
    assert tree == flat
    assert run(decidb_cli, f"{sql} ORDER BY id").stdout == run(decidb_cli, f"{sql} ORDER BY id", "flat").stdout


def test_an_avg_under_not_equal_multiplies_its_bound(decidb_cli, tmp_path):
    """An AVG under `<>` keeps its sums and multiplies its bound by its group's count instead
    (HoistAvgIntoNotEqualBound), as flat's model does."""
    sql = f"""SELECT id, x FROM {GROUPED_ROWS} DECIDE x(INT)
        SUCH THAT x >= 0 AND x <= 5 AND avg(x) by (t.g) <> 3 MAXIMIZE sum(x)"""
    flat, tree = models(decidb_cli, sql, tmp_path)
    assert tree == flat
